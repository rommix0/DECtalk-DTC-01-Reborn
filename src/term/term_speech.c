/* dtc01term's speech side on the library (term_speech.h; REFERENCE.md s17.3, s17.14).
 *
 * What the host C finds here, and what it becomes:
 *   the text pipe (cur_stream)       collected, then TextToSpeechSpeak; when the speech is played, dev_putc waits while
 *                                    64 or more characters are queued (v1.8's pipe), so the host task stops reading
 *                                    and XOFF comes as in v1.8 (not into a wave file: g_pipe_size)
 *   0x1A + sem_wait(&sync_sem)       the text before it with TTS_FORCE, then TextToSpeechSync on a helper thread; with
 *                                    stop_pending set (DT_STOP's stop task) TextToSpeechReset, at once
 *   last_index                       the index callback
 *   [:re n] (send_dcs_reply(31, n))  the callback, then the "reply" task
 *   dt_error_flags 0x08 (DSR 25)     GetStatus(STATUS_ERRORS), every tick
 *   dt_log, dt_mode                  SetLog / SetMode, before the next text
 *   the speech side's console        SetConsole, onto the local terminal
 *   DT_DICT, RIS                     AddUserEntry, UnloadUserDictionary
 *   the dialer's tone messages       TextToSpeechPlayTones on a helper thread; the message comes back when heard
 *   speech_init, the board           the ROM's order without dttask and klsyn; the self-test jumper; a fixed heap size
 */
#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "ttsapi.h"
#include "term_dev.h"
#include "term_os.h"
#include "term_phone.h"
#include "term_speech.h"

#define PIPE_SIZE 64            /* v1.8's text pipe (pipe_open's size, 0x40) */
#define HEAP_FREE 17486         /* what DECTST 5 reads out: the emulator's figure (s15.35) */
#define JUMPER_OPEN 0xf5        /* the DUART input port with IP4 high: the self-test runs, the banner is spoken */
#define JUMPER_CLOSED 0xe5

/* ---- the speech side's variables (defined with speech in the firmware) ---- */
int16_t dt_log, dt_mode, last_index, dt_error_flags, stop_pending;
static chardev_t pipe_dev;
static stream_t pipe_wr = { 0, NULL, NULL, STREAM_WRITE, &pipe_dev };   /* unbuffered, as fdopen made it */
stream_t *cur_stream = &pipe_wr;
ksem_t sync_sem;
void *stop_task;
static mbox_t dspq;
mbox_t *dsp_link_queue = &dspq;

static LPTTS_HANDLE_T g_tts;
static int g_quiet, g_wave, g_restart;
static FILE *g_pipe_log;
/* The text pipe holds PIPE_SIZE characters, as v1.8's, when the speech is played. Into a wave file it does not: the
 * file is made far faster than real time, and a held pipe would let the library run dry between two refills, which
 * adds the DSP's pauses and makes the file depend on timing. */
static int g_pipe_size = PIPE_SIZE;
static int g_log_sent, g_mode_sent;     /* what the library has */

/* ---- events from the library's thread and the helper threads, for the main loop ---- */
enum { EV_SYNC_DONE, EV_TONE_DONE, EV_INDEX, EV_REPLY, EV_CONSOLE };
typedef struct event {
    struct event *next;
    int kind, value;
    msg_t *msg;
    int n;
    char text[1];
} event_t;
static term_mutex_t g_evmu;
static event_t *g_ev_head, *g_ev_tail;
static int g_jobs;                      /* helper threads running */

static void post(int kind, int value, msg_t *msg, const char *text, int n)
{
    event_t *e = (event_t *)malloc(sizeof *e + (size_t)(n > 0 ? n : 0));
    if (!e) return;
    e->next = NULL;
    e->kind = kind;
    e->value = value;
    e->msg = msg;
    e->n = n;
    if (n > 0) memcpy(e->text, text, (size_t)n);
    term_mutex_lock(&g_evmu);
    if (g_ev_tail) g_ev_tail->next = e;
    else g_ev_head = e;
    g_ev_tail = e;
    term_mutex_unlock(&g_evmu);
}

static void tts_callback(LONG p1, LONG p2, DWORD inst, UINT msg)
{
    (void)inst;
    if (msg != TTS_MSG_INDEX_MARK) return;
    post(p1 == TTS_INDEX_REPLY ? EV_REPLY : EV_INDEX, (int)p2, NULL, NULL, 0);
}

static VOID console_callback(const char *text, DWORD n, DWORD inst)
{
    (void)inst;
    post(EV_CONSOLE, 0, NULL, text, (int)n);
}

/* ---- helper threads for the calls that wait until the audio is heard ---- */
typedef struct {
    int kind;                           /* EV_SYNC_DONE or EV_TONE_DONE */
    msg_t *msg;
    DWORD high, low;
} job_t;

TERM_THREAD(job_main, arg)
{
    job_t *j = (job_t *)arg;
    if (j->kind == EV_SYNC_DONE) TextToSpeechSync(g_tts);
    else TextToSpeechPlayTones(g_tts, j->high, j->low, 160, 60);
    post(j->kind, 0, j->msg, NULL, 0);
    free(j);
    TERM_THREAD_END;
}

static void start_job(int kind, msg_t *msg, DWORD high, DWORD low)
{
    job_t *j = (job_t *)calloc(1, sizeof *j);
    term_thread_t t;
    if (!j) return;
    j->kind = kind;
    j->msg = msg;
    j->high = high;
    j->low = low;
    g_jobs++;
    if (term_thread_start(&t, job_main, j)) {
        term_thread_detach(t);
    } else {                            /* no thread: do it here (the other tasks wait meanwhile) */
        job_main(j);
    }
}

/* ---- the text pipe ---- */
static char *g_pend;                    /* written, not yet given to the library */
static int g_npend, g_cap, g_force;

static DWORD status(DWORD id)
{
    DWORD i = id, s = 0;
    TextToSpeechGetStatus(g_tts, &i, &s, 1);
    return s;
}

static void send_settings(void)
{
    int log = dt_log & (LOG_TEXT | LOG_PHONEMES | LOG_DEBUG);
    if (log != g_log_sent) {
        TextToSpeechSetLog(g_tts, (DWORD)log);
        g_log_sent = log;
    }
    if (dt_mode != g_mode_sent) {
        TextToSpeechSetMode(g_tts, (DWORD)(uint16_t)dt_mode);
        g_mode_sent = dt_mode;
    }
}

static void flush_text(int force)
{
    send_settings();
    if (!g_npend && !force) return;
    if (g_cap) g_pend[g_npend] = 0;
    TextToSpeechSpeak(g_tts, g_npend ? g_pend : (LPSTR) "", force ? TTS_FORCE : TTS_NORMAL);
    g_npend = 0;
}

void term_speech_flush(void)
{
    flush_text(g_force);
    g_force = 0;
}

static int pipe_room(void *ctx)
{
    (void)ctx;
    return g_npend + (int)status(INPUT_CHARACTER_COUNT) < g_pipe_size;
}

static void pipe_putc(void *ctx, int c)
{
    (void)ctx;
    if (g_pipe_log) {                   /* --log-pipe: every byte a task writes, as the captures' "O P" lines */
        const char *name = kernel_current() ? kernel_task_name(kernel_current()) : NULL;
        fprintf(g_pipe_log, "%s\t%02X\n", name ? name : "-", c & 0xff);
    }
    if (c == 0x1a) {                    /* the sync marker (it also ends a clause): what is before it goes now */
        g_force = 1;
        return;
    }
    if (c == 0) return;
    if (!pipe_room(NULL)) {
        term_speech_flush();
        kernel_wait_until(pipe_room, NULL);
    }
    if (g_npend + 2 > g_cap) {
        int cap = g_cap ? g_cap * 2 : 256;
        char *p = (char *)realloc(g_pend, (size_t)cap);
        if (!p) return;
        g_pend = p;
        g_cap = cap;
    }
    g_pend[g_npend++] = (char)c;
}

static const kdev_ops_t pipe_ops = { pipe_putc, NULL, NULL, NULL };

/* ---- the kernel hooks: DT_SYNC and DT_STOP ---- */
static void on_call(void *ctx, const void *obj)
{
    (void)ctx;
    if (obj != &sync_sem || sync_sem.count) return;
    if (stop_pending) {                 /* DT_STOP: drop what is queued, stop the speech; then the sync is done */
        g_npend = 0;
        g_force = 0;
        TextToSpeechReset(g_tts, FALSE);
        sync_sem.count++;
        return;
    }
    g_force = 1;                        /* Sync ends the clause itself, but the text must reach the library first */
    term_speech_flush();
    start_job(EV_SYNC_DONE, NULL, 0, 0);
}

static const kernel_hooks_t g_hooks = { NULL, on_call, NULL, NULL, NULL };
const kernel_hooks_t *term_speech_hooks(void) { return &g_hooks; }

/* ---- the dialer's tones: a message into the DSP queue comes back when its tone has been heard ---- */
static void dspq_notify(void)
{
    msg_t *m = dspq.head;
    if (!m) return;
    dspq.head = m->next;
    if (!dspq.head) dspq.tail = NULL;
    dspq.count--;
    m->next = NULL;
    term_speech_flush();
    /* the phone line's receiver hears the unit's own dialing (s15.34; phtask_main drops those digits) */
    term_phone_own_tone((int)((uint16_t)m->data[0] & 0xfff), (int)(m->pad0c & 0xfff));
    start_job(EV_TONE_DONE, m, (DWORD)((uint16_t)m->data[0] & 0xfff), (DWORD)(m->pad0c & 0xfff));
}

/* ---- the reply task: klsyn's [:re n] replies (ph_index_reply_hook) ---- */
static int16_t g_replies[64];
static int g_nreplies;
static int have_reply(void *ctx) { (void)ctx; return g_nreplies > 0; }
static void reply_task_main(void)
{
    for (;;) {
        int16_t n;
        kernel_wait_until(have_reply, NULL);
        n = g_replies[0];
        memmove(g_replies, g_replies + 1, (size_t)--g_nreplies * sizeof g_replies[0]);
        send_dcs_reply(31, n);
    }
}

void term_speech_tick(void)
{
    event_t *e;
    term_mutex_lock(&g_evmu);
    e = g_ev_head;
    g_ev_head = g_ev_tail = NULL;
    term_mutex_unlock(&g_evmu);
    while (e) {
        event_t *next = e->next;
        int i;
        switch (e->kind) {
        case EV_SYNC_DONE:
            g_jobs--;
            sem_signal(&sync_sem);
            break;
        case EV_TONE_DONE:
            g_jobs--;
            mbox_put(e->msg->home, e->msg);
            break;
        case EV_REPLY:                  /* a reply mark is also the last index */
            if (g_nreplies < 64) g_replies[g_nreplies++] = (int16_t)e->value;
            last_index = (int16_t)e->value;
            break;
        case EV_INDEX:
            last_index = (int16_t)e->value;
            break;
        case EV_CONSOLE:
            for (i = 0; i < e->n; i++) dev_putc(&console_dev, (unsigned char)e->text[i]);
            break;
        }
        free(e);
        e = next;
    }
    if (status(STATUS_ERRORS) & 0x08) dt_error_flags |= 0x08;  /* DSR 25: a bad phonemic text */
}

/* ---- the boot and the board ---- */
static void nvram_factory(void)
{
    int16_t rec[64];
    int k, j, a;
    memcpy(rec, factory_settings, sizeof rec);
    rec[63] = (int16_t)nvram_checksum(rec, 63);
    for (a = 0, k = 0; k < 64; k++) {
        int16_t w = rec[k];
        for (j = 0; j < 4; j++, a += 2, w >>= 4) nvram[a] = (uint8_t)(w & 0xf);
    }
}

/* 0x30c8: the settings from the NVRAM, then the tasks of the table at 0x12bac (without klsyn and dttask, which are the
 * library's) and the reply task (klsyn's replies) */
void speech_init(void)
{
    settings_reset(3, 0);
    kernel_task("phone", phtask_main, 0);
    kernel_task("host", host_task_main, 0);
    kernel_task("reply", reply_task_main, 50);
    kernel_task("host timeout", host_timeout_task_main, -100);
    stop_task = kernel_task("stop", stop_task_main, 0);
}

uint8_t duart_input_port(void) { return g_quiet ? JUMPER_CLOSED : JUMPER_OPEN; }
int32_t heap_free_total(void) { return HEAP_FREE; }
void profile_start(uint32_t lo, uint32_t hi) { (void)lo; (void)hi; }     /* no profiler: HISTOGRAM shows nothing */
void profile_report(void) {}

/* 0x10e8: TRAP #14, the power-up. The main loop restarts everything; this task waits for it. */
void system_restart(void)
{
    g_restart = 1;
    for (;;) event_wait(1000, NULL, 0);
}

void dict_hash_clear_all(void) { TextToSpeechUnloadUserDictionary(g_tts); }

dict_entry_t *dict_hash_insert_or_delete(const char *name, const char *subst)
{
    static int entry;                   /* any address: the host C only tests for NULL (no room) */
    struct dic_entry e;
    size_t n = strlen(name), m = strlen(subst);
    if (n + m + 2 > sizeof e.text) return NULL;
    memset(&e, 0, sizeof e);
    memcpy(e.text, name, n);
    memcpy(e.text + n + 1, subst, m);
    return TextToSpeechAddUserEntry(g_tts, &e) == MMSYSERR_NOERROR ? (dict_entry_t *)&entry : NULL;
}

int term_speech_start(unsigned device, const char *wave, int quiet, char *err, int errlen)
{
    MMRESULT r;
    term_mutex_init(&g_evmu);
    g_quiet = quiet;
    g_wave = wave != NULL;
    g_pipe_size = wave ? 1 << 30 : PIPE_SIZE;
    r = TextToSpeechStartupEx(&g_tts, device, wave ? DO_NOT_USE_AUDIO_DEVICE : 0, tts_callback, 0);
    if (r != MMSYSERR_NOERROR) {
        snprintf(err, (size_t)errlen, r == MMSYSERR_NODRIVER ? "no audio device (use -w FILE)"
                                                             : "the speech library did not start (error %u)",
                 (unsigned)r);
        return -1;
    }
    if (wave && TextToSpeechOpenWaveOutFile(g_tts, (char *)wave, WAVE_FORMAT_1M16) != MMSYSERR_NOERROR) {
        snprintf(err, (size_t)errlen, "cannot write %s", wave);
        TextToSpeechShutdown(g_tts);
        return -1;
    }
    TextToSpeechSetConsole(g_tts, console_callback, 0);
    g_log_sent = 0;                     /* the library's startup: no log, MODE SQUARE */
    g_mode_sent = TTS_MODE_SQUARE;
    nvram_factory();                    /* no NVRAM file: the factory record at every start (s13 item 15) */
    return 0;
}

void term_speech_boot(void)
{
    kernel_device_init(&pipe_dev, &pipe_ops, NULL);
    mbox_init(&dspq, dspq_notify);
    memset(&sync_sem, 0, sizeof sync_sem);
    g_nreplies = 0;
    g_npend = 0;
    g_force = 0;
    stop_pending = 0;
    kernel_task("main", main_task, 0);
}

int term_speech_restart_wanted(void) { return g_restart; }

/* DECTST 1, TEST POWER: stop the tasks, take the library back as far as it goes (it cannot return to power-up: a
 * voice changed with [:dv] keeps its changes), and boot again. The NVRAM survives, as the X2212 does. */
void term_speech_restart(void)
{
    g_restart = 0;
    kernel_shutdown();                  /* releases the lock */
    TextToSpeechReset(g_tts, TRUE);
    TextToSpeechUnloadUserDictionary(g_tts);
    g_log_sent = 0;
    g_mode_sent = TTS_MODE_SQUARE;
    TextToSpeechSetLog(g_tts, 0);
    TextToSpeechSetMode(g_tts, TTS_MODE_SQUARE);
    kernel_init(&g_hooks);
    kernel_lock();
}

int term_speech_idle(void)
{
    return !g_npend && !g_jobs && !g_ev_head && host_idle == 5 && !status(INPUT_CHARACTER_COUNT) &&
           !status(STATUS_SPEAKING);
}

void term_speech_stop(void)
{
    if (g_wave) TextToSpeechCloseWaveOutFile(g_tts);
    TextToSpeechShutdown(g_tts);
    if (g_pipe_log) fclose(g_pipe_log);
    g_pipe_log = NULL;
}

int term_speech_log_pipe(const char *path)
{
    g_pipe_log = fopen(path, "w");
    return g_pipe_log ? 0 : -1;
}
