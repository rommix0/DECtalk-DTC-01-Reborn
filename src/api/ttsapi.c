/* ttsapi.c - the speech library, DECtalk.dll / libtts_us.so: its thread and its outputs (REFERENCE.md s17,
 * s17.10).
 *
 * The library's thread drives the speech engine (src/speech/engine.c: dttask and klsyn on the kernel, the DSP link
 * and program). It takes samples from the engine only as fast as the output takes them, 256 at a time (25.6 ms):
 *   - the audio device: as fast as it plays (real time, a little over 200 ms ahead);
 *   - a wave file: as fast as it can;
 *   - memory buffers: as fast as the program adds them;
 *   - nothing (DO_NOT_USE_AUDIO_DEVICE and no file or buffers): as fast as it can, and the samples are dropped.
 *
 * Every sample has a number, counted from startup (the engine's). Events are tied to one: an index mark to the first
 * sample of its frame, the audio's start to its first sample. An event is passed on when the output reaches its
 * sample: when the device has played it, when it is in the file, when it is in a buffer. For index marks that is
 * also when the library acts on them (last_index, the DT_INDEX_REPLY answer: engine_mark_spoken). In memory, as in
 * dapi, the marks go into the buffers (TTS_INDEX_T) instead of messages.
 *
 * Messages go to the callback, the window (Windows' TextToSpeechStartup), or the event queue (TTS_EVENT_QUEUE). They
 * are collected under the handle's lock and sent after it is released, so a callback may call AddBuffer or
 * ReturnBuffer.
 *
 * Locks: the handle's mutex, then the engine's (the kernel's); never the other way round. The engine's hooks run
 * inside engine_run, on the thread that called it, and take no lock.
 *
 * TTS_MANUAL_CLOCK: no thread; TextToSpeechRun does the thread's work in the caller's thread, and Sync and Reset do
 * it too while they wait.
 *
 * Also here: the tones (PlayTones), the console and the log file (what v1.8 prints on its local terminal, as the
 * engine runs), and the phoneme array of memory buffers.
 */
#define BLD_DECTALK_DLL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ttsapi.h"
#include "tts_os.h"
#include "tts_audio.h"
#include "tts_lib.h"
#include "../speech/engine.h"
#include "../speech/dsp_synth.h"
#ifndef _WIN32
#include <unistd.h>
#endif

#define CHUNK 256               /* samples per turn: 25.6 ms */
#define POLL_MS 5               /* how often the thread looks at the device while it plays */
#define MAXPEND 1024            /* events waiting for the output */
#define MAXRUN 64               /* marks in one engine_run (at most one per frame; 256 samples are 4 frames) */
#define RATE 10000
#define MARK_REPLY 0x67         /* the mark's code: DT_INDEX_REPLY, [:re n] */

enum { OUT_NULL, OUT_DEVICE, OUT_FILE, OUT_MEMORY };
enum { STEP_MADE, STEP_OUTPUT, STEP_IDLE };     /* step(): samples made; the output takes none now; nothing to say */
enum { EV_MARK, EV_START };
#define MSG_CONSOLE 0x7fffffffu /* internal: text for the console routine (outmsg_t.ptr, .p1 bytes) */

typedef struct { int64_t sample; int kind; uint32_t mark; } pend_t;
typedef struct { UINT msg; LONG p1, p2; DWORD sample; void *ptr; } outmsg_t;

struct TTS_HANDLE_TAG {
    os_mutex_t mu;
    os_cond_t wake;             /* the thread waits here for work */
    os_cond_t done;             /* Sync and Reset wait here */
    os_thread_t th;
    int thread, quit;
    DWORD options;
    UINT device;
    TTS_CALLBACK_T callback;
    DWORD instance;
#ifdef _WIN32
    HWND hwnd;
    UINT wm_error, wm_index, wm_buffer;
#endif
    MMRESULT last_error;

    /* the console (SetConsole) and the log file (OpenLogFile): v1.8's console text */
    TTS_CONSOLE_T console;
    DWORD console_inst;
    FILE *log;
    DWORD log_flags;            /* SetLog's DT_LOG bits */
    DWORD log_file_flags;       /* OpenLogFile's */
    char *con;                  /* console bytes the engine has written, not yet passed on (the engine's thread) */
    int ncon, con_cap;

    /* the event queue (TTS_EVENT_QUEUE) */
    TTS_EVENT_T *evq;
    int ev_head, ev_count, ev_cap;
#ifdef _WIN32
    HANDLE ev_handle;
#else
    int ev_pipe[2];
    int ev_signalled;
#endif

    /* messages to send once the lock is released */
    outmsg_t *outbox;
    int nout, out_cap;

    /* the output */
    int out;
    int open_error_sent;
    tts_audio_t *audio;
    int64_t audio_base;         /* the sample number of the device's first sample since it was opened */
    FILE *wav;
    DWORD wav_format, wav_bytes;
    DWORD mem_format;
    LPTTS_BUFFER_T *mem_free;   /* buffers added and not yet filled, oldest first */
    int mem_nfree, mem_cap;
    LPTTS_BUFFER_T mem_cur;     /* the one being filled */
    LPTTS_BUFFER_T *mem_full;   /* full ones for ReturnBuffer (64-bit builds, where a LONG cannot carry them) */
    int mem_nfull, mem_fcap;

    int64_t made;               /* samples the engine has made: the next one's number */
    int64_t reached;            /* the output has played, written or delivered every sample before this one */
    pend_t pend[MAXPEND];
    int npend;
    int audible;                /* TTS_AUDIO_PLAY_START sent, TTS_AUDIO_PLAY_STOP not yet */
    int paused;
    unsigned text_gen;          /* text written so far (a count of Speak calls) */
    unsigned idle_gen;          /* the engine has said everything up to this */
    unsigned quiet_gen;         /* ... and the output has passed all of it on */
    unsigned reset_req, reset_done;
    int reset_full;

    /* the engine's marks and phones during one engine_run (only the thread running it touches these) */
    uint32_t rmark[MAXRUN];
    int64_t rsample[MAXRUN];
    int nr;
    int rphone[MAXRUN];
    int32_t rplen[MAXRUN];
    int64_t rpsample[MAXRUN];
    int np;
};

static LPTTS_HANDLE_T g_h;      /* one instance (s17.2) */
static int g_started;           /* the engine has been started in this process: it cannot start again */
static const int g_ptr_in_long = sizeof(void *) <= sizeof(LONG);   /* 32-bit builds: TTS_MSG_BUFFER carries it */

int tts_valid(LPTTS_HANDLE_T h) { return h && h == g_h; }
MMRESULT tts_error(LPTTS_HANDLE_T h, MMRESULT r) { if (tts_valid(h)) h->last_error = r; return r; }
MMRESULT tts_last_error(LPTTS_HANDLE_T h) { return tts_valid(h) ? h->last_error : MMSYSERR_INVALHANDLE; }

/* ---- messages ---- */

/* Queue a message (h->mu held). The event queue takes it at once; the callback and the window get it from flush(). */
static void post(LPTTS_HANDLE_T h, UINT msg, LONG p1, LONG p2, int64_t sample, void *ptr)
{
    if ((h->options & TTS_EVENT_QUEUE) && msg != MSG_CONSOLE) {
        TTS_EVENT_T *e;
        if (h->ev_count == h->ev_cap) {
            int cap = h->ev_cap ? 2 * h->ev_cap : 64, i;
            TTS_EVENT_T *q = (TTS_EVENT_T *)malloc((size_t)cap * sizeof *q);
            if (!q) return;
            for (i = 0; i < h->ev_count; i++) q[i] = h->evq[(h->ev_head + i) % h->ev_cap];
            free(h->evq);
            h->evq = q;
            h->ev_cap = cap;
            h->ev_head = 0;
        }
        e = &h->evq[(h->ev_head + h->ev_count++) % h->ev_cap];
        e->uiMsg = msg;
        e->lParam1 = p1;
        e->lParam2 = p2;
        e->dwSampleNumber = (DWORD)sample;
#ifdef _WIN32
        SetEvent(h->ev_handle);
#else
        if (!h->ev_signalled) {
            char c = 1;
            h->ev_signalled = write(h->ev_pipe[1], &c, 1) == 1;
        }
#endif
        return;
    }
    if (h->nout == h->out_cap) {
        int cap = h->out_cap ? 2 * h->out_cap : 64;
        outmsg_t *o = (outmsg_t *)realloc(h->outbox, (size_t)cap * sizeof *o);
        if (!o) return;
        h->outbox = o;
        h->out_cap = cap;
    }
    h->outbox[h->nout].msg = msg;
    h->outbox[h->nout].p1 = p1;
    h->outbox[h->nout].p2 = p2;
    h->outbox[h->nout].sample = (DWORD)sample;
    h->outbox[h->nout++].ptr = ptr;
}

/* Send the queued messages: called with h->mu held, releases it while it calls out. */
static void flush(LPTTS_HANDLE_T h)
{
    while (h->nout) {
        outmsg_t m = h->outbox[0];
        memmove(h->outbox, h->outbox + 1, (size_t)--h->nout * sizeof m);
        os_unlock(&h->mu);
        if (m.msg == MSG_CONSOLE) {
            if (h->console) h->console((const char *)m.ptr, (DWORD)m.p1, h->console_inst);
            free(m.ptr);
        } else if (h->callback) {
            h->callback(m.p1, m.p2, h->instance, m.msg);
        }
#ifdef _WIN32
        else if (h->hwnd) {
            UINT id = m.msg == TTS_MSG_BUFFER ? h->wm_buffer : m.msg == TTS_MSG_INDEX_MARK ? h->wm_index : h->wm_error;
            PostMessage(h->hwnd, id, (WPARAM)m.p1, m.msg == TTS_MSG_BUFFER ? (LPARAM)m.ptr : (LPARAM)m.p2);
        }
#endif
        os_lock(&h->mu);
    }
}

static void status(LPTTS_HANDLE_T h, LONG code, LONG detail)
{
    post(h, TTS_MSG_STATUS, code, detail, h->made, NULL);
}

/* ---- the engine's hooks (inside engine_run, on the thread running it) ---- */

static void hook_mark(void *ctx, uint32_t mark, int64_t sample)
{
    LPTTS_HANDLE_T h = (LPTTS_HANDLE_T)ctx;
    if (h->nr < MAXRUN) {
        h->rmark[h->nr] = mark;
        h->rsample[h->nr++] = sample;
    }
}

static void hook_phone(void *ctx, int phone, int32_t samples, int64_t sample)
{
    LPTTS_HANDLE_T h = (LPTTS_HANDLE_T)ctx;
    if (h->np < MAXRUN) {
        h->rphone[h->np] = phone;
        h->rplen[h->np] = samples;
        h->rpsample[h->np++] = sample;
    }
}

static void hook_console(void *ctx, int c)
{
    LPTTS_HANDLE_T h = (LPTTS_HANDLE_T)ctx;
    if (h->ncon == h->con_cap) {
        int cap = h->con_cap ? 2 * h->con_cap : 256;
        char *b = (char *)realloc(h->con, (size_t)cap);
        if (!b) return;
        h->con = b;
        h->con_cap = cap;
    }
    h->con[h->ncon++] = (char)c;
}

/* the console text the engine wrote (h->mu held): to the log file (without the CR the firmware puts before each LF)
 * and to the console routine */
static void console_out(LPTTS_HANDLE_T h)
{
    int i;
    if (!h->ncon) return;
    if (h->log) {
        for (i = 0; i < h->ncon; i++)
            if (h->con[i] != '\r') fputc(h->con[i], h->log);
        fflush(h->log);
    }
    if (h->console) {
        char *s = (char *)malloc((size_t)h->ncon + 1);
        if (s) {
            memcpy(s, h->con, (size_t)h->ncon);
            s[h->ncon] = 0;
            post(h, MSG_CONSOLE, h->ncon, 0, h->made, s);
        }
    }
    h->ncon = 0;
}

static void hook_panic(void *ctx, const char *task, const char *msg)
{
    (void)ctx;
    fprintf(stderr, "DECtalk: %s: %s\n", task, msg);
}

/* ---- sample formats ---- */

static int sample_bytes(DWORD format) { return format == WAVE_FORMAT_1M16 ? 2 : 1; }

static uint8_t mulaw(int16_t s)                 /* G.711 */
{
    int sign = s < 0 ? 0x80 : 0, v = s < 0 ? -(int)s : s, exp = 7, mask;
    if (v > 32635) v = 32635;
    v += 0x84;
    for (mask = 0x4000; !(v & mask) && exp > 0; mask >>= 1) exp--;
    return (uint8_t)~(sign | exp << 4 | ((v >> (exp + 3)) & 0x0f));
}

/* n samples into dst in the format; returns the bytes */
static int convert(DWORD format, const int16_t *pcm, int n, unsigned char *dst)
{
    int i;
    if (format == WAVE_FORMAT_1M16) {
        for (i = 0; i < n; i++) {
            dst[2 * i] = (unsigned char)(pcm[i] & 0xff);
            dst[2 * i + 1] = (unsigned char)((uint16_t)pcm[i] >> 8);
        }
        return 2 * n;
    }
    for (i = 0; i < n; i++)
        dst[i] = format == WAVE_FORMAT_1M08 ? (unsigned char)((pcm[i] >> 8) + 128) : mulaw(pcm[i]);
    return n;
}

static int valid_format(DWORD f) { return f == WAVE_FORMAT_1M16 || f == WAVE_FORMAT_1M08 || f == WAVE_FORMAT_08M08; }

/* ---- the outputs ---- */

static int default_output(LPTTS_HANDLE_T h) { return h->options & DO_NOT_USE_AUDIO_DEVICE ? OUT_NULL : OUT_DEVICE; }

static void close_device(LPTTS_HANDLE_T h)
{
    if (h->audio) tts_audio_close(h->audio);
    h->audio = NULL;
}

/* the device, opened when audio comes (and closed when it has gone quiet, unless OWN_AUDIO_DEVICE) */
static int open_device(LPTTS_HANDLE_T h)
{
    unsigned err;
    if (h->audio) return 1;
    if (!(h->audio = tts_audio_open(h->device, &err))) {
        if ((h->options & REPORT_OPEN_ERROR) && !h->open_error_sent) status(h, ERROR_OPENING_WAVE_OUTPUT_DEVICE, (LONG)err);
        h->open_error_sent = 1;
        return 0;
    }
    h->open_error_sent = 0;
    h->audio_base = h->made;
    if (h->paused) tts_audio_pause(h->audio, 1);
    return 1;
}

/* the buffer being filled, taking the next one added if there is none */
static LPTTS_BUFFER_T mem_buffer(LPTTS_HANDLE_T h)
{
    LPTTS_BUFFER_T b;
    if (h->mem_cur || !h->mem_nfree) return h->mem_cur;
    b = h->mem_free[0];
    memmove(h->mem_free, h->mem_free + 1, (size_t)--h->mem_nfree * sizeof b);
    b->dwBufferLength = 0;
    b->dwNumberOfPhonemeChanges = 0;
    b->dwNumberOfIndexMarks = 0;
    return h->mem_cur = b;
}

/* the buffer is full: to the program (TTS_MSG_BUFFER) */
static void mem_deliver(LPTTS_HANDLE_T h)
{
    LPTTS_BUFFER_T b = h->mem_cur;
    if (!b) return;
    h->mem_cur = NULL;
    if (!g_ptr_in_long) {                       /* the message cannot carry it: ReturnBuffer hands it over */
        if (h->mem_nfull == h->mem_fcap) {
            int cap = h->mem_fcap ? 2 * h->mem_fcap : 16;
            LPTTS_BUFFER_T *q = (LPTTS_BUFFER_T *)realloc(h->mem_full, (size_t)cap * sizeof *q);
            if (!q) return;
            h->mem_full = q;
            h->mem_fcap = cap;
        }
        h->mem_full[h->mem_nfull++] = b;
        post(h, TTS_MSG_BUFFER, 0, 0, h->made, b);
    } else {
        post(h, TTS_MSG_BUFFER, (LONG)(intptr_t)b, 0, h->made, b);
    }
}

/* samples the output takes now */
static int out_room(LPTTS_HANDLE_T h)
{
    LPTTS_BUFFER_T b;
    switch (h->out) {
    case OUT_DEVICE:
        return open_device(h) ? tts_audio_room(h->audio) : CHUNK;   /* no device: the samples are dropped */
    case OUT_MEMORY:
        if (!(b = mem_buffer(h))) return 0;
        return (int)((b->dwMaximumBufferLength - b->dwBufferLength) / (DWORD)sample_bytes(h->mem_format));
    default:
        return CHUNK;
    }
}

static void pend(LPTTS_HANDLE_T h, int64_t sample, int kind, uint32_t mark)
{
    if (h->npend == MAXPEND) return;
    h->pend[h->npend].sample = sample;
    h->pend[h->npend].kind = kind;
    h->pend[h->npend++].mark = mark;
}

/* a mark is heard: act on it, and say so */
static void mark_heard(LPTTS_HANDLE_T h, uint32_t mark, int64_t sample, int message)
{
    engine_mark_spoken(mark);
    if (message)
        post(h, TTS_MSG_INDEX_MARK, (mark >> 16) == MARK_REPLY ? TTS_INDEX_REPLY : TTS_INDEX_MARK, (LONG)(mark & 0x7fff),
             sample, NULL);
}

/* n samples (and the marks of this run) to the output */
static void out_write(LPTTS_HANDLE_T h, const int16_t *pcm, int n)
{
    unsigned char bytes[2 * CHUNK];
    int i, k;
    for (i = 0; i < h->nr; i++)
        if (h->out != OUT_MEMORY) pend(h, h->rsample[i], EV_MARK, h->rmark[i]);
    switch (h->out) {
    case OUT_DEVICE:
        if (h->audio && tts_audio_write(h->audio, pcm, n) < n) status(h, ERROR_IN_AUDIO_WRITE, 0);
        break;
    case OUT_FILE:
        k = convert(h->wav_format, pcm, n, bytes);
        if (fwrite(bytes, 1, (size_t)k, h->wav) != (size_t)k) status(h, ERROR_WRITING_FILE, 0);
        h->wav_bytes += (DWORD)k;
        break;
    case OUT_MEMORY: {
        LPTTS_BUFFER_T b = mem_buffer(h);
        int bps = sample_bytes(h->mem_format), m = 0;
        if (!b) break;                              /* cannot happen: n <= out_room */
        k = convert(h->mem_format, pcm, n, bytes);
        memcpy(b->lpData + b->dwBufferLength, bytes, (size_t)k);
        b->dwBufferLength += (DWORD)k;
        for (i = 0; i < h->np; i++) {
            if (b->lpPhonemeArray && b->dwNumberOfPhonemeChanges < b->dwMaximumNumberOfPhonemeChanges) {
                LPTTS_PHONEME_T x = &b->lpPhonemeArray[b->dwNumberOfPhonemeChanges++];
                x->dwPhoneme = (DWORD)h->rphone[i];
                x->dwPhonemeSampleNumber = (DWORD)h->rpsample[i];
                x->dwPhonemeDuration = (DWORD)h->rplen[i];
                x->dwReserved = 0;
            }
            m++;
        }
        for (i = 0; i < h->nr; i++) {
            if (b->lpIndexArray && b->dwNumberOfIndexMarks < b->dwMaximumNumberOfIndexMarks) {
                LPTTS_INDEX_T x = &b->lpIndexArray[b->dwNumberOfIndexMarks++];
                x->dwIndexValue = h->rmark[i] & 0x7fff;
                x->dwIndexSampleNumber = (DWORD)h->rsample[i];
                x->dwReserved = (h->rmark[i] >> 16) == MARK_REPLY ? TTS_INDEX_REPLY : TTS_INDEX_MARK;
            }
            mark_heard(h, h->rmark[i], h->rsample[i], 0);   /* delivered: in the program's memory */
            m++;
        }
        if (b->dwBufferLength + (DWORD)bps > b->dwMaximumBufferLength ||
            (m && b->lpIndexArray && b->dwNumberOfIndexMarks >= b->dwMaximumNumberOfIndexMarks) ||
            (m && b->lpPhonemeArray && b->dwNumberOfPhonemeChanges >= b->dwMaximumNumberOfPhonemeChanges))
            mem_deliver(h);
        break;
    }
    default:
        break;
    }
    h->nr = 0;
    h->np = 0;
    h->made += n;
}

/* How far the output has got; pass on the events it has reached. */
static void progress(LPTTS_HANDLE_T h)
{
    int i, j;
    if (h->out == OUT_DEVICE && h->audio) h->reached = h->audio_base + tts_audio_done(h->audio);
    else h->reached = h->made;
    for (i = j = 0; i < h->npend; i++) {
        pend_t *p = &h->pend[i];
        if (p->sample >= h->reached) {
            h->pend[j++] = *p;
            continue;
        }
        if (p->kind == EV_MARK) mark_heard(h, p->mark, p->sample, 1);
        else post(h, TTS_MSG_STATUS, TTS_AUDIO_PLAY_START, 0, p->sample, NULL);
    }
    h->npend = j;
}

/* Reset (h->mu held; released while the engine flushes) */
static void do_reset(LPTTS_HANDLE_T h)
{
    int full = h->reset_full;
    unsigned req = h->reset_req;
    h->reset_full = 0;
    os_unlock(&h->mu);
    engine_flush();
    if (full) {                                 /* the startup state: SQUARE, Paul at 180 wpm (main_task's text) */
        engine_set(ENGINE_MODE, TTS_MODE_SQUARE);
        engine_write("\x02:np :ra 180\x03", 13);
    }
    os_lock(&h->mu);
    console_out(h);
    h->idle_gen = h->text_gen;                  /* what was queued is gone */
    if (full) h->text_gen++;
    if (h->audio) tts_audio_flush(h->audio);    /* the device drops what it has not played */
    h->npend = 0;                               /* marks not yet heard never will be */
    h->nr = h->np = 0;
    h->reset_done = req;
    os_broadcast(&h->done);
}

/* One turn of the library's thread (or of TextToSpeechRun), h->mu held: at most max samples from the engine, as far
 * as the output takes them, and the events the output has reached. */
static int step(LPTTS_HANDLE_T h, int max)
{
    int16_t pcm[CHUNK];
    int room, n, result = STEP_IDLE;
    unsigned gen;
    if (h->reset_req != h->reset_done) do_reset(h);
    progress(h);
    if (h->idle_gen != h->text_gen) {
        room = out_room(h);
        if (room > max) room = max;
        if (room > CHUNK) room = CHUNK;
        if (room <= 0) {
            result = STEP_OUTPUT;
            if (h->out == OUT_MEMORY) {             /* no buffer: is there anything to put in one? */
                int quiet;
                gen = h->text_gen;
                os_unlock(&h->mu);
                quiet = engine_quiet();
                os_lock(&h->mu);
                console_out(h);
                if (quiet && h->text_gen == gen) h->idle_gen = gen;
            }
        } else {
            gen = h->text_gen;
            h->nr = h->np = 0;
            os_unlock(&h->mu);
            n = engine_run(pcm, room);
            os_lock(&h->mu);
            console_out(h);
            if (n > 0) {
                if (!h->audible && h->out != OUT_MEMORY) pend(h, h->made, EV_START, 0);
                h->audible = 1;
                out_write(h, pcm, n);
                result = STEP_MADE;
            }
            if (n < room && h->text_gen == gen) h->idle_gen = gen;     /* said everything it has */
            progress(h);
        }
    }
    if (h->idle_gen == h->text_gen && h->reached >= h->made && !h->npend) {      /* quiet */
        if (h->audible) {
            if (h->out != OUT_MEMORY) post(h, TTS_MSG_STATUS, TTS_AUDIO_PLAY_STOP, 0, h->made, NULL);
            h->audible = 0;
            if (h->out == OUT_DEVICE && !(h->options & OWN_AUDIO_DEVICE) && !h->paused) close_device(h);
        }
        if (h->quiet_gen != h->idle_gen) {
            h->quiet_gen = h->idle_gen;
            os_broadcast(&h->done);
        }
    }
    return result;
}

static int busy_output(LPTTS_HANDLE_T h) { return h->reached < h->made || h->npend || h->audible; }

static void thread_main(void *arg)
{
    LPTTS_HANDLE_T h = (LPTTS_HANDLE_T)arg;
    os_lock(&h->mu);
    while (!h->quit) {
        int r = step(h, CHUNK);
        flush(h);
        if (h->quit || r == STEP_MADE || h->reset_req != h->reset_done) continue;
        /* nothing made, yet text not said: it came while the engine ran (step saw a newer text_gen), and its wake-up
         * came before this wait, so waiting would lose it (Sync then never returned, seen on Linux) */
        if (r == STEP_IDLE && h->idle_gen != h->text_gen) continue;
        if (r == STEP_OUTPUT) os_wait(&h->wake, &h->mu, h->out == OUT_DEVICE ? POLL_MS : -1);
        else os_wait(&h->wake, &h->mu, h->out == OUT_DEVICE && busy_output(h) ? POLL_MS : -1);
    }
    os_unlock(&h->mu);
}

static void wake(LPTTS_HANDLE_T h) { os_signal(&h->wake); }

/* Wait (h->mu held) until everything written so far has gone through the output. Manual clock: do the work here.
 * 0 when it cannot finish (no buffers to fill, with a manual clock). */
static int wait_quiet(LPTTS_HANDLE_T h)
{
    unsigned target = h->text_gen;
    long idle = 0;
    while ((int)(h->quiet_gen - target) < 0 && !h->quit) {
        if (h->thread) {
            os_wait(&h->done, &h->mu, -1);
            continue;
        }
        if (step(h, CHUNK) == STEP_MADE) idle = 0;
        else if (++idle > 2) return 0;          /* nothing moves: memory output with no buffer */
        flush(h);
    }
    return 1;
}

/* ---- startup and shutdown ---- */

static MMRESULT startup(LPTTS_HANDLE_T *pph, UINT device, DWORD options, TTS_CALLBACK_T cb, DWORD instance
#ifdef _WIN32
                        , HWND hwnd
#endif
)
{
    LPTTS_HANDLE_T h;
    engine_hooks_t hooks;
    unsigned err = MMSYSERR_NOERROR;
    if (!pph) return MMSYSERR_INVALPARAM;
    *pph = NULL;
    if (g_h || g_started) return MMSYSERR_ALLOCATED;   /* one instance per process lifetime (s17.2) */
    if ((options & TTS_MANUAL_CLOCK) && !(options & DO_NOT_USE_AUDIO_DEVICE)) return MMSYSERR_INVALFLAG;
    if (!(h = (LPTTS_HANDLE_T)calloc(1, sizeof *h))) return MMSYSERR_NOMEM;
    h->options = options;
    h->device = device;
    h->callback = cb;
    h->instance = instance;
#ifdef _WIN32
    h->hwnd = hwnd;
    h->wm_error = RegisterWindowMessageA(TTS_ERROR_MESSAGE_NAME);
    h->wm_index = RegisterWindowMessageA(TTS_INDEX_MESSAGE_NAME);
    h->wm_buffer = RegisterWindowMessageA(TTS_BUFFER_MESSAGE_NAME);
#endif
    h->out = default_output(h);
    if (h->out == OUT_DEVICE) {                 /* is there a device? (then closed until audio comes) */
        tts_audio_t *a = tts_audio_open(device, &err);
        if (!a) {
            free(h);
            return err == MMSYSERR_BADDEVICEID ? err : MMSYSERR_NODRIVER;
        }
        if (options & OWN_AUDIO_DEVICE) h->audio = a;
        else tts_audio_close(a);
    }
    if (options & TTS_EVENT_QUEUE) {
#ifdef _WIN32
        if (!(h->ev_handle = CreateEventA(NULL, TRUE, FALSE, NULL))) { close_device(h); free(h); return MMSYSERR_ERROR; }
#else
        if (pipe(h->ev_pipe)) { close_device(h); free(h); return MMSYSERR_ERROR; }
#endif
    }
    os_mutex_init(&h->mu);
    os_cond_init(&h->wake);
    os_cond_init(&h->done);
    memset(&hooks, 0, sizeof hooks);
    hooks.ctx = h;
    hooks.mark = hook_mark;
    hooks.phone = hook_phone;
    hooks.console = hook_console;
    hooks.panic = hook_panic;
    g_started = 1;
    if (!engine_init(DSP_FIX_FIRST_PERIOD | DSP_FIX_ROUND | DSP_FIX_TONE | ENGINE_FIX_MARKS | ENGINE_FIX_SPLIT, &hooks)) {
        close_device(h);
        free(h);
        return MMSYSERR_ERROR;
    }
    g_h = h;
    if (!(options & TTS_MANUAL_CLOCK)) {
        if (!os_thread_start(&h->th, thread_main, h)) {
            engine_shutdown();
            g_h = NULL;
            close_device(h);
            free(h);
            return MMSYSERR_NOMEM;
        }
        h->thread = 1;
    }
    *pph = h;
    return MMSYSERR_NOERROR;
}

#ifdef _WIN32
TTSAPI MMRESULT TextToSpeechStartup(HWND hWnd, LPTTS_HANDLE_T *pphTTS, UINT uiDeviceNumber, DWORD dwDeviceOptions)
{
    return startup(pphTTS, uiDeviceNumber, dwDeviceOptions, NULL, 0, hWnd);
}
TTSAPI MMRESULT TextToSpeechStartupEx(LPTTS_HANDLE_T *pphTTS, UINT uiDeviceNumber, DWORD dwDeviceOptions,
                                      TTS_CALLBACK_T DtCallbackRoutine, LONG lCallbackParameter)
{
    return startup(pphTTS, uiDeviceNumber, dwDeviceOptions, DtCallbackRoutine, (DWORD)lCallbackParameter, NULL);
}
#else
TTSAPI MMRESULT TextToSpeechStartup(LPTTS_HANDLE_T *pphTTS, UINT uiDeviceNumber, DWORD dwDeviceOptions,
                                    TTS_CALLBACK_T DtCallbackRoutine, LONG lCallbackParameter)
{
    return startup(pphTTS, uiDeviceNumber, dwDeviceOptions, DtCallbackRoutine, (DWORD)lCallbackParameter);
}
TTSAPI MMRESULT TextToSpeechStartupEx(LPTTS_HANDLE_T *pphTTS, UINT uiDeviceNumber, DWORD dwDeviceOptions,
                                      TTS_CALLBACK_T DtCallbackRoutine, LONG lCallbackParameter)
{
    return startup(pphTTS, uiDeviceNumber, dwDeviceOptions, DtCallbackRoutine, (DWORD)lCallbackParameter);
}
#endif

static void finish_wav(LPTTS_HANDLE_T h);

/* @2: stops at once (what is not yet heard is dropped). The engine cannot start again in this process. */
TTSAPI MMRESULT TextToSpeechShutdown(LPTTS_HANDLE_T phTTS)
{
    LPTTS_HANDLE_T h = phTTS;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    os_lock(&h->mu);
    h->quit = 1;
    wake(h);
    os_broadcast(&h->done);
    os_unlock(&h->mu);
    if (h->thread) os_thread_join(h->th);
    engine_shutdown();
    close_device(h);
    if (h->wav) finish_wav(h);
    if (h->log) fclose(h->log);
    free(h->con);
    g_h = NULL;
#ifdef _WIN32
    if (h->ev_handle) CloseHandle(h->ev_handle);
#else
    if (h->options & TTS_EVENT_QUEUE) {
        close(h->ev_pipe[0]);
        close(h->ev_pipe[1]);
    }
#endif
    free(h->evq);
    free(h->outbox);
    free(h->mem_free);
    free(h->mem_full);
    os_cond_free(&h->wake);
    os_cond_free(&h->done);
    os_mutex_free(&h->mu);
    free(h);
    return MMSYSERR_NOERROR;
}

/* ---- text ---- */

/* @3: never waits (s17.2): the text goes into the queue, which grows. TTS_FORCE adds a CTRL-K, which ends the
 * clause. How much is still unread: TextToSpeechGetStatus(INPUT_CHARACTER_COUNT). */
TTSAPI MMRESULT TextToSpeechSpeak(LPTTS_HANDLE_T phTTS, LPSTR pszText, DWORD dwFlags)
{
    LPTTS_HANDLE_T h = phTTS;
    size_t n;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    if (!pszText) return tts_error(h, MMSYSERR_INVALPARAM);
    n = strlen(pszText);
    if (n) engine_write(pszText, (int)n);
    if (dwFlags & TTS_FORCE) engine_write("\x0b", 1);
    if (!n && !(dwFlags & TTS_FORCE)) return MMSYSERR_NOERROR;
    os_lock(&h->mu);
    h->text_gen++;
    wake(h);
    os_unlock(&h->mu);
    return MMSYSERR_NOERROR;
}

/* @10: dapi's: returns when everything queued before it has been heard (played, or written to the file or the
 * buffers). It ends the clause (TTS_FORCE) and resumes a paused device, as dapi's does. */
TTSAPI MMRESULT TextToSpeechSync(LPTTS_HANDLE_T phTTS)
{
    LPTTS_HANDLE_T h = phTTS;
    int ok;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    TextToSpeechSpeak(h, "", TTS_FORCE);
    os_lock(&h->mu);
    if (h->paused) {
        h->paused = 0;
        if (h->audio) tts_audio_pause(h->audio, 0);
    }
    ok = wait_quiet(h);
    os_unlock(&h->mu);
    return ok ? MMSYSERR_NOERROR : tts_error(h, MMSYSERR_ERROR);
}

/* @9: DT_STOP and more: the clause being spoken ends, and the text, frames and audio not yet heard are dropped.
 * bReset also returns to the startup state (SQUARE, Paul, 180 wpm; the user dictionary stays) and closes a wave
 * file. */
TTSAPI MMRESULT TextToSpeechReset(LPTTS_HANDLE_T phTTS, BOOL bReset)
{
    LPTTS_HANDLE_T h = phTTS;
    unsigned req;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    os_lock(&h->mu);
    if (bReset) h->reset_full = 1;
    req = ++h->reset_req;
    wake(h);
    if (!h->thread) {
        step(h, 0);
        flush(h);
    }
    while ((int)(h->reset_done - req) < 0 && !h->quit) os_wait(&h->done, &h->mu, -1);
    if (h->audible && h->out != OUT_MEMORY) post(h, TTS_MSG_STATUS, TTS_AUDIO_PLAY_STOP, 0, h->made, NULL);
    h->audible = 0;
    if (bReset && h->out == OUT_FILE) {
        finish_wav(h);
        h->out = default_output(h);
    }
    flush(h);
    os_unlock(&h->mu);
    return MMSYSERR_NOERROR;
}

/* @4, @5: the device only (dapi) */
TTSAPI MMRESULT TextToSpeechPause(LPTTS_HANDLE_T phTTS)
{
    LPTTS_HANDLE_T h = phTTS;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    if (h->options & DO_NOT_USE_AUDIO_DEVICE) return tts_error(h, MMSYSERR_ERROR);
    os_lock(&h->mu);
    h->paused = 1;
    if (h->audio) tts_audio_pause(h->audio, 1);
    os_unlock(&h->mu);
    return MMSYSERR_NOERROR;
}

TTSAPI MMRESULT TextToSpeechResume(LPTTS_HANDLE_T phTTS)
{
    LPTTS_HANDLE_T h = phTTS;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    if (h->options & DO_NOT_USE_AUDIO_DEVICE) return tts_error(h, MMSYSERR_ERROR);
    os_lock(&h->mu);
    h->paused = 0;
    if (h->audio) tts_audio_pause(h->audio, 0);
    wake(h);
    os_unlock(&h->mu);
    return MMSYSERR_NOERROR;
}

/* @8 */
TTSAPI MMRESULT TextToSpeechGetStatus(LPTTS_HANDLE_T phTTS, LPDWORD pdwIdentifier, LPDWORD pdwStatus,
                                      DWORD dwNumberOfStatusValues)
{
    LPTTS_HANDLE_T h = phTTS;
    DWORD i;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    if (!pdwIdentifier || !pdwStatus) return tts_error(h, MMSYSERR_INVALPARAM);
    for (i = 0; i < dwNumberOfStatusValues; i++) {
        switch (pdwIdentifier[i]) {
        case INPUT_CHARACTER_COUNT: pdwStatus[i] = (DWORD)engine_pending(); break;
        case STATUS_SPEAKING:
            os_lock(&h->mu);
            pdwStatus[i] = h->idle_gen != h->text_gen || busy_output(h) || engine_pending() > 0;
            os_unlock(&h->mu);
            break;
        case WAVE_OUT_DEVICE_ID: pdwStatus[i] = h->device; break;
        case STATUS_LAST_INDEX: pdwStatus[i] = (DWORD)engine_get(ENGINE_LAST_INDEX); break;
        case STATUS_ERRORS: pdwStatus[i] = (DWORD)engine_get(ENGINE_ERRORS); break;
        default: return tts_error(h, MMSYSERR_INVALPARAM);
        }
    }
    return MMSYSERR_NOERROR;
}

/* ---- the wave file ---- */

static void put32(unsigned char *p, DWORD v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

static void wav_header(unsigned char *hd, DWORD format, DWORD bytes)
{
    int bps = sample_bytes(format);
    memcpy(hd, "RIFF", 4);
    put32(hd + 4, 36 + bytes);
    memcpy(hd + 8, "WAVEfmt ", 8);
    put32(hd + 16, 16);
    put16(hd + 20, format == WAVE_FORMAT_08M08 ? 7 : 1);   /* mu-law, or PCM */
    put16(hd + 22, 1);
    put32(hd + 24, RATE);
    put32(hd + 28, (DWORD)(RATE * bps));
    put16(hd + 32, (unsigned)bps);
    put16(hd + 34, (unsigned)(8 * bps));
    memcpy(hd + 36, "data", 4);
    put32(hd + 40, bytes);
}

static void finish_wav(LPTTS_HANDLE_T h)
{
    unsigned char hd[44];
    wav_header(hd, h->wav_format, h->wav_bytes);
    fseek(h->wav, 0, SEEK_SET);
    fwrite(hd, 1, sizeof hd, h->wav);
    fclose(h->wav);
    h->wav = NULL;
}

/* @6: after everything queued so far has been heard, the audio goes to the file instead (dapi) */
TTSAPI MMRESULT TextToSpeechOpenWaveOutFile(LPTTS_HANDLE_T phTTS, char *pszFileName, DWORD dwFormat)
{
    LPTTS_HANDLE_T h = phTTS;
    unsigned char hd[44];
    FILE *f;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    if (!pszFileName) return tts_error(h, MMSYSERR_INVALPARAM);
    if (!valid_format(dwFormat)) return tts_error(h, WAVERR_BADFORMAT);
    if (h->out == OUT_FILE || h->out == OUT_MEMORY) return tts_error(h, MMSYSERR_ALLOCATED);
    TextToSpeechSync(h);
    if (!(f = fopen(pszFileName, "wb"))) return tts_error(h, MMSYSERR_ERROR);
    wav_header(hd, dwFormat, 0);
    if (fwrite(hd, 1, sizeof hd, f) != sizeof hd) {
        fclose(f);
        return tts_error(h, MMSYSERR_ERROR);
    }
    os_lock(&h->mu);
    close_device(h);
    h->wav = f;
    h->wav_format = dwFormat;
    h->wav_bytes = 0;
    h->out = OUT_FILE;
    os_unlock(&h->mu);
    return MMSYSERR_NOERROR;
}

/* @7: after everything queued so far is in the file */
TTSAPI MMRESULT TextToSpeechCloseWaveOutFile(LPTTS_HANDLE_T phTTS)
{
    LPTTS_HANDLE_T h = phTTS;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    if (h->out != OUT_FILE) return tts_error(h, MMSYSERR_ERROR);
    TextToSpeechSync(h);
    os_lock(&h->mu);
    if (h->wav) finish_wav(h);
    h->out = default_output(h);
    os_unlock(&h->mu);
    return MMSYSERR_NOERROR;
}

/* ---- memory buffers ---- */

/* @20: after everything queued so far has been heard, the audio goes to the buffers the program adds (dapi) */
TTSAPI MMRESULT TextToSpeechOpenInMemory(LPTTS_HANDLE_T phTTS, DWORD dwFormat)
{
    LPTTS_HANDLE_T h = phTTS;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    if (!valid_format(dwFormat)) return tts_error(h, WAVERR_BADFORMAT);
    if (h->out == OUT_FILE || h->out == OUT_MEMORY) return tts_error(h, MMSYSERR_ALLOCATED);
    TextToSpeechSync(h);
    os_lock(&h->mu);
    close_device(h);
    h->mem_format = dwFormat;
    h->mem_nfree = h->mem_nfull = 0;
    h->mem_cur = NULL;
    h->out = OUT_MEMORY;
    os_unlock(&h->mu);
    return MMSYSERR_NOERROR;
}

/* @21: after everything queued so far is in the buffers; the buffers stay the program's */
TTSAPI MMRESULT TextToSpeechCloseInMemory(LPTTS_HANDLE_T phTTS)
{
    LPTTS_HANDLE_T h = phTTS;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    if (h->out != OUT_MEMORY) return tts_error(h, MMSYSERR_ERROR);
    TextToSpeechSync(h);
    os_lock(&h->mu);
    h->out = default_output(h);
    h->mem_nfree = h->mem_nfull = 0;
    h->mem_cur = NULL;
    os_unlock(&h->mu);
    return MMSYSERR_NOERROR;
}

/* @22: a buffer to fill. The program sets lpData, dwMaximumBufferLength, lpIndexArray and
 * dwMaximumNumberOfIndexMarks (lpPhonemeArray is not filled yet). */
TTSAPI MMRESULT TextToSpeechAddBuffer(LPTTS_HANDLE_T phTTS, LPTTS_BUFFER_T pTTSbuffer)
{
    LPTTS_HANDLE_T h = phTTS;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    if (!pTTSbuffer || !pTTSbuffer->lpData) return tts_error(h, MMSYSERR_INVALPARAM);
    os_lock(&h->mu);
    if (h->out != OUT_MEMORY) {
        os_unlock(&h->mu);
        return tts_error(h, MMSYSERR_ERROR);
    }
    if (h->mem_nfree == h->mem_cap) {
        int cap = h->mem_cap ? 2 * h->mem_cap : 16;
        LPTTS_BUFFER_T *q = (LPTTS_BUFFER_T *)realloc(h->mem_free, (size_t)cap * sizeof *q);
        if (!q) {
            os_unlock(&h->mu);
            return tts_error(h, MMSYSERR_NOMEM);
        }
        h->mem_free = q;
        h->mem_cap = cap;
    }
    h->mem_free[h->mem_nfree++] = pTTSbuffer;
    wake(h);
    os_unlock(&h->mu);
    return MMSYSERR_NOERROR;
}

/* @23: dapi's: the buffer being filled, as it is (NULL if none). [DTC01] In 64-bit builds a full buffer is handed
 * over here first, oldest first, since TTS_MSG_BUFFER cannot carry it. */
TTSAPI MMRESULT TextToSpeechReturnBuffer(LPTTS_HANDLE_T phTTS, LPTTS_BUFFER_T *ppTTSbuffer)
{
    LPTTS_HANDLE_T h = phTTS;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    if (!ppTTSbuffer) return tts_error(h, MMSYSERR_INVALPARAM);
    os_lock(&h->mu);
    if (h->out != OUT_MEMORY) {
        os_unlock(&h->mu);
        return tts_error(h, MMSYSERR_ERROR);
    }
    if (h->mem_nfull) {
        *ppTTSbuffer = h->mem_full[0];
        memmove(h->mem_full, h->mem_full + 1, (size_t)--h->mem_nfull * sizeof *h->mem_full);
    } else {
        *ppTTSbuffer = h->mem_cur;
        h->mem_cur = NULL;
    }
    os_unlock(&h->mu);
    return MMSYSERR_NOERROR;
}

/* ---- [DTC01] the manual clock and the event queue ---- */

/* @108: TTS_MANUAL_CLOCK: up to dwSamples samples, in the caller's thread; stops early when there is nothing to say
 * or nowhere to put it */
TTSAPI MMRESULT TextToSpeechRun(LPTTS_HANDLE_T phTTS, DWORD dwSamples)
{
    LPTTS_HANDLE_T h = phTTS;
    int64_t start;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    if (h->thread) return tts_error(h, MMSYSERR_NOTSUPPORTED);
    os_lock(&h->mu);
    start = h->made;
    while (h->made - start < (int64_t)dwSamples) {
        int left = (int)((int64_t)dwSamples - (h->made - start));
        int r = step(h, left < CHUNK ? left : CHUNK);
        flush(h);
        if (r != STEP_MADE) break;
    }
    os_unlock(&h->mu);
    return MMSYSERR_NOERROR;
}

/* @109 */
TTSAPI MMRESULT TextToSpeechGetEventHandle(LPTTS_HANDLE_T phTTS, TTS_WAITABLE_T *pHandle)
{
    LPTTS_HANDLE_T h = phTTS;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    if (!pHandle) return tts_error(h, MMSYSERR_INVALPARAM);
    if (!(h->options & TTS_EVENT_QUEUE)) return tts_error(h, MMSYSERR_ERROR);
#ifdef _WIN32
    *pHandle = h->ev_handle;
#else
    *pHandle = h->ev_pipe[0];
#endif
    return MMSYSERR_NOERROR;
}

/* @110 */
TTSAPI BOOL TextToSpeechGetEvent(LPTTS_HANDLE_T phTTS, LPTTS_EVENT_T pEvent)
{
    LPTTS_HANDLE_T h = phTTS;
    BOOL got = FALSE;
    if (!tts_valid(h) || !pEvent || !(h->options & TTS_EVENT_QUEUE)) return FALSE;
    os_lock(&h->mu);
    if (h->ev_count) {
        *pEvent = h->evq[h->ev_head];
        h->ev_head = (h->ev_head + 1) % h->ev_cap;
        h->ev_count--;
        got = TRUE;
    }
    if (!h->ev_count) {                         /* empty: the handle stops signalling */
#ifdef _WIN32
        ResetEvent(h->ev_handle);
#else
        char c;
        if (h->ev_signalled && read(h->ev_pipe[0], &c, 1) == 1) h->ev_signalled = 0;
#endif
    }
    os_unlock(&h->mu);
    return got;
}

/* ---- the tones, the console, the log ---- */

/* @105: the two-tone generator (v1.8's DTMF dialer and self-test): dwHigh and dwLow Hz (0-4095, 0 = off) for dwOnMs,
 * then silence for dwOffMs, in 10 ms steps. It follows what is queued (a Sync first), and returns when it has been
 * heard, as Sync does. */
TTSAPI MMRESULT TextToSpeechPlayTones(LPTTS_HANDLE_T phTTS, DWORD dwHigh, DWORD dwLow, DWORD dwOnMs, DWORD dwOffMs)
{
    LPTTS_HANDLE_T h = phTTS;
    MMRESULT r;
    int ok;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    if (dwHigh > 4095 || dwLow > 4095 || dwOnMs > 60000 || dwOffMs > 60000) return tts_error(h, MMSYSERR_INVALPARAM);
    if ((r = TextToSpeechSync(h)) != MMSYSERR_NOERROR) return r;
    if (!engine_tone((int)dwHigh, (int)dwLow, (int)dwOnMs, (int)dwOffMs)) return tts_error(h, MMSYSERR_ERROR);
    os_lock(&h->mu);
    h->text_gen++;                              /* the thread has something to play */
    wake(h);
    ok = wait_quiet(h);
    os_unlock(&h->mu);
    return ok ? MMSYSERR_NOERROR : tts_error(h, MMSYSERR_ERROR);
}

/* @104: v1.8's console text from the speech side ([:dv list], "Illegal voice", the logs of SetLog), as the firmware
 * formats it, from the library's thread as the text is synthesized. NULL: none. */
TTSAPI MMRESULT TextToSpeechSetConsole(LPTTS_HANDLE_T phTTS, TTS_CONSOLE_T ConsoleRoutine, DWORD dwInstanceParameter)
{
    LPTTS_HANDLE_T h = phTTS;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    os_lock(&h->mu);
    h->console = ConsoleRoutine;
    h->console_inst = dwInstanceParameter;
    os_unlock(&h->mu);
    return MMSYSERR_NOERROR;
}

#define LOG_FLAGS (LOG_TEXT | LOG_PHONEMES | LOG_DEBUG)

static MMRESULT log_bits(LPTTS_HANDLE_T h, DWORD flags)
{
    if (flags & LOG_SYLLABLES) return tts_error(h, MMSYSERR_INVALFLAG);    /* dapi's; v1.8 has none */
    if (flags & ~(DWORD)LOG_FLAGS) return tts_error(h, MMSYSERR_INVALFLAG);
    return MMSYSERR_NOERROR;
}

/* @102: DT_LOG: what the speech side logs on the console (SetConsole) */
TTSAPI MMRESULT TextToSpeechSetLog(LPTTS_HANDLE_T phTTS, DWORD dwFlags)
{
    LPTTS_HANDLE_T h = phTTS;
    MMRESULT r;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    if ((r = log_bits(h, dwFlags)) != MMSYSERR_NOERROR) return r;
    os_lock(&h->mu);
    h->log_flags = dwFlags;
    engine_set(ENGINE_LOG, (int)(h->log_flags | h->log_file_flags));
    os_unlock(&h->mu);
    return MMSYSERR_NOERROR;
}

/* @103 */
TTSAPI MMRESULT TextToSpeechGetLog(LPTTS_HANDLE_T phTTS, LPDWORD pdwFlags)
{
    LPTTS_HANDLE_T h = phTTS;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    if (!pdwFlags) return tts_error(h, MMSYSERR_INVALPARAM);
    *pdwFlags = (DWORD)engine_get(ENGINE_LOG);
    return MMSYSERR_NOERROR;
}

/* @24: the console text to a file too (LOG_TEXT: the text as the pipeline reads it; LOG_PHONEMES: each clause's
 * phonemes; [DTC01] LOG_DEBUG), until @25. The firmware's CR before each LF is left out. */
TTSAPI MMRESULT TextToSpeechOpenLogFile(LPTTS_HANDLE_T phTTS, LPSTR pszFileName, DWORD dwFlags)
{
    LPTTS_HANDLE_T h = phTTS;
    MMRESULT r;
    FILE *f;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    if (!pszFileName) return tts_error(h, MMSYSERR_INVALPARAM);
    if ((r = log_bits(h, dwFlags)) != MMSYSERR_NOERROR) return r;
    if (h->log) return tts_error(h, MMSYSERR_ALLOCATED);
    if (!(f = fopen(pszFileName, "w"))) return tts_error(h, MMSYSERR_ERROR);
    os_lock(&h->mu);
    h->log = f;
    h->log_file_flags = dwFlags;
    engine_set(ENGINE_LOG, (int)(h->log_flags | h->log_file_flags));
    os_unlock(&h->mu);
    return MMSYSERR_NOERROR;
}

/* @25 */
TTSAPI MMRESULT TextToSpeechCloseLogFile(LPTTS_HANDLE_T phTTS)
{
    LPTTS_HANDLE_T h = phTTS;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    os_lock(&h->mu);
    if (!h->log) {
        os_unlock(&h->mu);
        return tts_error(h, MMSYSERR_ERROR);
    }
    fclose(h->log);
    h->log = NULL;
    h->log_file_flags = 0;
    engine_set(ENGINE_LOG, (int)h->log_flags);
    os_unlock(&h->mu);
    return MMSYSERR_NOERROR;
}

/* @50: the phonemes of the text, as v1.8's phoneme log writes them (each clause by name, joined by blanks), into
 * szPhonemeBuf; *dwBufSize: its size in, the bytes written with the NUL out (the text is cut to fit). As dapi's, it
 * first waits for what is queued, then speaks the text and waits again; with TTS_SILENT in dwConversionFlags nothing
 * is heard (klsyn drops the clauses: v1.8's DT_STOP flag; [:dv] commands in the text still take effect). The other
 * flags choose dapi's alphabets and are ignored: v1.8 has one. */
TTSAPI MMRESULT TextToSpeechConvertToPhonemes(LPTTS_HANDLE_T phTTS, unsigned char *szPhonemeBuf, DWORD *dwBufSize,
                                              DWORD dwOutPhonemeFlags, unsigned char *szText, DWORD dwInTextFlags,
                                              DWORD dwConversionFlags)
{
    LPTTS_HANDLE_T h = phTTS;
    MMRESULT r;
    int n;
    (void)dwOutPhonemeFlags;
    (void)dwInTextFlags;
    if (!tts_valid(h)) return MMSYSERR_INVALHANDLE;
    if (!szPhonemeBuf || !dwBufSize || !*dwBufSize || !szText) return tts_error(h, MMSYSERR_INVALPARAM);
    if ((r = TextToSpeechSync(h)) != MMSYSERR_NOERROR) return r;
    engine_convert_begin((dwConversionFlags & TTS_SILENT) != 0);
    TextToSpeechSpeak(h, (LPSTR)szText, TTS_FORCE);
    r = TextToSpeechSync(h);
    n = engine_convert_end((char *)szPhonemeBuf, (int)*dwBufSize);
    *dwBufSize = (DWORD)(n < (int)*dwBufSize ? n : (int)*dwBufSize - 1) + 1;
    return r;
}
