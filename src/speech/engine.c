/* The speech engine: the speech side's tasks on the kernel, the DSP link, the DAC clock (engine.h;
 * REFERENCE.md s17.9).
 *
 * The tasks are the ROM's, with its priorities: klsyn 50, dttask 10 (spawn_system_tasks 0x310a). The ROM's
 * dsp_post_frame 0x7b56 takes a message from the DSP pool and waits there when all 48 are queued; here klsyn waits
 * on `room` the same way, and the link's returned() hook, the ROM's ISR giving a message back, wakes it. Then
 * dsp_link_run returns early (yield) so klsyn posts the next frame at once, as on the 68000, where it would run as
 * soon as the interrupt returned.
 *
 * Index marks (s16.13) and phones: ph_mark_hook and ph_phone_hook number each event; the next post carries the
 * number as its tag, and the link reports the tag at the frame's first sample.
 *
 * The library's other needs are here too, each under the kernel's lock: tones (the phone dialer's tone items, of any
 * length), the user dictionary, the voice records, the flag words.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "ph_frame.h"
#include "ph_rom.h"
#include "tx_text.h"
#include "tx_rom.h"
#include "dsp_link.h"
#include "../kernel/kernel.h"
#include "../kernel/console.h"
#include "../kernel/stream.h"

#define KLSYN_PRIORITY 50
#define DTTASK_PRIORITY 10
#define LOG_ERROR 0x20          /* dt_log: log_error 0xd1ca */
#define NEVENTS 256             /* events between being reached and heard: a mark and a phone per queued frame */
enum { EV_MARK, EV_PHONE };

extern ksem_t sync_sem;         /* ph_task.c: klsyn signals it at each sync marker */

static struct {
    engine_hooks_t hooks;
    dsp_link_t link;
    ksem_t room;                /* klsyn waits here for a free DSP message */
    uint32_t ev[NEVENTS];       /* event number n: ev[n % NEVENTS], a mark or a phone code */
    int32_t ev_len[NEVENTS];    /* a phone's duration in samples */
    uint8_t ev_kind[NEVENTS];
    uint32_t nev;               /* events reached so far */
    uint32_t tag;               /* the event the next post carries (0 = none) */
    uint32_t heard;             /* events up to this number reported */
    long posts;
    int kick;                   /* a message came back while klsyn waited */
} E;

/* ---- the kernel's and the link's hooks ---- */

static void k_call(void *ctx, const void *obj) { (void)ctx; if (E.hooks.call) E.hooks.call(E.hooks.ctx, obj); }
static void k_got(void *ctx, mbox_t *mb, msg_t *m) { (void)ctx; if (E.hooks.got) E.hooks.got(E.hooks.ctx, mb, m); }
static void k_console(void *ctx, int c) { (void)ctx; if (E.hooks.console) E.hooks.console(E.hooks.ctx, c); }
static void k_panic(void *ctx, const char *task, const char *msg)
{
    (void)ctx;
    if (E.hooks.panic) E.hooks.panic(E.hooks.ctx, task, msg);
}

static void l_returned(void *ctx, const dsp_msg_t *m)
{
    (void)ctx;
    if (m->w[0] & 0x8000) E.link.yield = 1;      /* a tone item is done: engine_run may be too */
    if (E.room.waiters) {                       /* klsyn waits in a post: let it make the next one now */
        sem_signal(&E.room);
        E.link.yield = 1;
        E.kick = 1;
    }
}

static void l_mark(void *ctx, uint32_t tag, int64_t sample)
{
    (void)ctx;
    for (; E.heard != tag; E.heard++) {
        int i = (int)((E.heard + 1) % NEVENTS);
        if (E.ev_kind[i] == EV_PHONE) {
            if (E.hooks.phone) E.hooks.phone(E.hooks.ctx, (int)E.ev[i], E.ev_len[i], sample);
        } else if (E.hooks.mark) {
            E.hooks.mark(E.hooks.ctx, E.ev[i], sample);
        } else {
            index_mark_spoken(E.ev[i]);
        }
    }
}

/* ---- the speech side's hooks ---- */

/* dsp_post_frame's end (klsyn): wait for a free message, then post. w has the checksum or trailer; the link adds its
 * own. */
static void sink(const int16_t *w, int n)
{
    uint32_t tag = E.tag;
    E.tag = 0;
    if (E.hooks.frame) E.hooks.frame(E.hooks.ctx, w, n);
    while (!dsp_link_room(&E.link)) sem_wait(&E.room);
    dsp_link_post(&E.link, w, n - 1, tag);
    E.posts++;
}

/* an event goes with the next post */
static void event(int kind, uint32_t value, int32_t len)
{
    int i = (int)(++E.nev % NEVENTS);
    E.ev_kind[i] = (uint8_t)kind;
    E.ev[i] = value;
    E.ev_len[i] = len;
    E.tag = E.nev;
}

/* phsettar reached an index mark */
static void mark_reached(uint32_t mark) { event(EV_MARK, mark, 0); }

/* a phone starts (its duration: frames of 64 samples) */
static void phone_reached(int phone, int frames)
{
    if (E.hooks.phone) event(EV_PHONE, (uint32_t)phone, frames * 64);
}

static void index_reply(int value)
{
    if (E.hooks.index_reply) E.hooks.index_reply(E.hooks.ctx, value);
}

/* console text at printf level (the stdout stream: CR before LF) */
static void console_text(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') dev_putc(&console_dev, '\r');
        dev_putc(&console_dev, (uint8_t)*s);
    }
}

/* log_error 0xd1ca */
static void error_text(const char *fmt, int a, int b)
{
    if (dt_log & LOG_ERROR) kprintf(fmt, a, b);
}

/* ---- the engine ---- */

int engine_init(int fixes, const engine_hooks_t *hooks)
{
    kernel_hooks_t kh;
    dsp_link_hooks_t lh;
    memset(&E, 0, sizeof E);
    if (hooks) E.hooks = *hooks;
    memset(&kh, 0, sizeof kh);
    kh.call = k_call;
    kh.got = k_got;
    kh.console = k_console;
    kh.panic = k_panic;
    kernel_init(&kh);
    memset(&lh, 0, sizeof lh);
    lh.returned = l_returned;
    lh.mark = l_mark;
    dsp_link_init(&E.link, &lh);
    E.link.dsp.fixes = fixes & ~(ENGINE_FIX_MARKS | ENGINE_FIX_SPLIT);
    tx_fixes = fixes & ENGINE_FIX_MARKS ? TX_FIX_MARKS : 0;
    ph_fixes = (fixes & ENGINE_FIX_MARKS ? PH_FIX_MARKS : 0) | (fixes & ENGINE_FIX_SPLIT ? PH_FIX_SPLIT : 0);
    ph_frame_sink = sink;
    ph_mark_hook = mark_reached;
    ph_phone_hook = phone_reached;
    ph_index_reply_hook = index_reply;
    ph_console_hook = console_text;
    ph_error_hook = error_text;
    tx_log_hook = console_text;
    kernel_lock();
    if (!kernel_task("klsyn", klsyn_task_main, KLSYN_PRIORITY) || !kernel_task("dttask", dttask_main, DTTASK_PRIORITY)) {
        kernel_shutdown();
        return 0;
    }
    kernel_run();                               /* both start, and wait: klsyn for a message, dttask for text */
    /* the rest of speech_init 0x30c8, which the main task (priority 0) runs once the tasks wait: Paul, Val = Paul,
     * and a new clause (dttask's first clause, still empty, goes to klsyn as a one-word message) */
    load_voice_definition();
    save_voice_params();
    newclause(0);
    kernel_run();
    kernel_unlock();
    return 1;
}

void engine_write(const char *text, int n)
{
    kernel_lock();
    if (cur_stream) kernel_pipe_write(cur_stream->dev, text, n);
    kernel_unlock();
}

int engine_pending(void)
{
    int n;
    kernel_lock();
    n = cur_stream ? kernel_pipe_count(cur_stream->dev) : 0;
    kernel_unlock();
    return n;
}

int engine_quiet(void)
{
    int q;
    kernel_lock();
    kernel_run();
    q = (!cur_stream || !kernel_pipe_count(cur_stream->dev)) && dsp_link_quiet(&E.link);
    kernel_unlock();
    return q;
}

int engine_run(int16_t *pcm, int max)
{
    int n = 0;
    kernel_lock();
    for (;;) {
        long posts = E.posts;
        kernel_run();
        if (n >= max) break;
        E.kick = 0;
        if (dsp_link_tone_done(&E.link)) break;  /* the DSP makes silence in tone mode until the next frame */
        n += dsp_link_run(&E.link, pcm + n, max - n);
        if (n < max && !E.kick && E.posts == posts) break;     /* the DSP waits, and klsyn has nothing for it */
    }
    kernel_unlock();
    return n;
}

/* The library's Reset. First the ROM's stop task (stop_task_main 0xfa7a): stop_pending up, the sync marker 0x1A
 * into the pipe, wait until klsyn has taken it (dropping the clauses before it, and ending the one it is drawing
 * after the current phone), stop_pending down. Then what v1.8 does not do: the text not yet read is dropped first,
 * and the frames not yet heard, with their marks, after. klsyn may wait for room meanwhile: each flush makes it. */
void engine_flush(void)
{
    int16_t count;
    long guard;
    kernel_lock();
    if (cur_stream) {
        kernel_pipe_clear(cur_stream->dev);
        stop_pending++;
        count = sync_sem.count;
        kernel_pipe_write(cur_stream->dev, "\x1a", 1);
        for (guard = 0; sync_sem.count == count && guard < 100000; guard++) {
            dsp_link_flush(&E.link);
            kernel_run();
        }
        if (sync_sem.count > count) sync_sem.count--;   /* what the stop task's sem_wait takes */
        stop_pending--;
    }
    dsp_link_flush(&E.link);
    E.tag = 0;
    E.heard = E.nev;                            /* the marks of dropped frames are never heard */
    kernel_unlock();
}

int engine_get(int what)
{
    int v = 0;
    kernel_lock();
    switch (what) {
    case ENGINE_MODE: v = dt_mode; break;
    case ENGINE_LOG: v = dt_log; break;
    case ENGINE_LAST_INDEX: v = last_index; break;
    case ENGINE_ERRORS: v = dt_error_flags; dt_error_flags = 0; break;
    case ENGINE_RATE: v = sprate; break;
    case ENGINE_VOICE: v = voice_code - 0x6b; break;
    }
    kernel_unlock();
    return v;
}

void engine_set(int what, int value)
{
    kernel_lock();
    if (what == ENGINE_MODE) dt_mode = (int16_t)value;
    else if (what == ENGINE_LOG) dt_log = (int16_t)value;
    kernel_unlock();
}

void engine_mark_spoken(uint32_t mark)
{
    kernel_lock();
    index_mark_spoken(mark);
    kernel_unlock();
}

int64_t engine_samples(void)
{
    int64_t n;
    kernel_lock();
    n = E.link.samples;
    kernel_unlock();
    return n;
}

/* ---- tones ---- */

int engine_tone(int high, int low, int on_ms, int off_ms)
{
    int ok;
    if (high < 0 || high > 0xfff || low < 0 || low > 0xfff) return 0;
    kernel_lock();
    ok = dsp_link_post_tone_ticks(&E.link, (uint16_t)(0x9000 | high), (uint16_t)(0x8000 | low), (on_ms + 9) / 10,
                                  (off_ms + 9) / 10, 0);
    kernel_unlock();
    return ok;
}

/* ---- the user dictionary ---- */

int engine_dict_set(const char *name, const char *subst)
{
    int ok;
    kernel_lock();
    ok = dict_hash_insert_or_delete(name, subst ? subst : "") != NULL || !subst || !*subst;
    kernel_unlock();
    return ok;
}

/* the entry with exactly this name (as DT_DICT matches to replace or delete) */
static const dict_entry_t *dict_find(const char *name)
{
    const dict_entry_t *e;
    int b;
    for (b = 0; b < DICT_NBUCKET; b++)
        for (e = dict_user_table[b]; e; e = e->next)
            if (!strcmp(e->text, name)) return e;
    return NULL;
}

int engine_dict_get(const char *name, char *subst, int size)
{
    const dict_entry_t *e;
    kernel_lock();
    e = dict_find(name);
    if (e && subst && size > 0) {
        const char *s = e->text + strlen(e->text) + 1;
        strncpy(subst, s, (size_t)size - 1);
        subst[size - 1] = 0;
    }
    kernel_unlock();
    return e != NULL;
}

void engine_dict_clear(void)
{
    kernel_lock();
    dict_hash_clear_all();
    kernel_unlock();
}

static int by_name(const void *a, const void *b)
{
    return strcmp((*(const dict_entry_t *const *)a)->text, (*(const dict_entry_t *const *)b)->text);
}

int engine_dict_list(void (*fn)(void *ctx, const char *name, const char *subst), void *ctx)
{
    const dict_entry_t *e, **all;
    int b, n = 0, i;
    kernel_lock();
    all = (const dict_entry_t **)malloc(((size_t)dict_user_count + 1) * sizeof *all);
    if (all) {
        for (b = 0; b < DICT_NBUCKET; b++)
            for (e = dict_user_table[b]; e && n < dict_user_count; e = e->next) all[n++] = e;
        qsort(all, (size_t)n, sizeof *all, by_name);
        for (i = 0; i < n; i++) fn(ctx, all[i]->text, all[i]->text + strlen(all[i]->text) + 1);
        free(all);
    }
    kernel_unlock();
    return all ? n : -1;
}

/* ---- the built-in dictionary ---- */

#define TRIE_END 0x7f           /* the end-of-word character in the trie (tx_dict.c) */
#define LOG_PHONEME 0x02        /* dt_log: newclause logs each clause (tx_clause.c) */

/* a pronunciation (symbol codes, bit 7 on the last) as phonemic text: the names the DT_LOG phoneme log uses */
static int pron_text(const uint8_t *p, char *out, int size)
{
    int n = 0;
    name_nval = name_nvalues = 0;               /* no values due */
    for (;; p++) {
        const char *s = phoneme_name((int16_t)(*p & 0x7f));
        int k = s ? (int)strlen(s) : 0;
        if (k && n + k < size) {
            memcpy(out + n, s, (size_t)k);
            n += k;
        }
        if (*p & 0x80) break;
    }
    if (size > 0) out[n < size ? n : size - 1] = 0;
    return n;
}

int engine_dict_builtin(const char *word, char *out, int size)
{
    const uint8_t *r;
    int16_t users;
    size_t n = strlen(word);
    if (!n) return 0;
    kernel_lock();
    users = dict_user_count;                    /* lookup_word asks the user dictionary first: not now */
    dict_user_count = 0;
    r = lookup_word(word, word + n - 1);
    dict_user_count = users;
    if (r && out) pron_text(r, out, size);
    kernel_unlock();
    return r != NULL;
}

/* every word of a trie node, depth first, in the trie's (sorted) order */
static int walk(const dict_root_t *root, const int8_t *chars, const int16_t *links, char *word, int len,
                void (*fn)(void *ctx, const char *word, const char *pron), void *ctx)
{
    char pron[256];
    int n = 0, j;
    for (j = 0;; j++) {
        int c = chars[j] & 0x7f, l = links[j];
        if (c == TRIE_END || (l & 0x8000)) {    /* a word ends here (at the end mark, or after this character) */
            int k = len;
            if (c != TRIE_END && k < 79) word[k++] = (char)c;
            word[k] = 0;
            if (root->prons) {
                pron_text(root->prons + (l & 0x7fff), pron, sizeof pron);
                fn(ctx, word, pron);
                n++;
            }
        } else if (len < 79) {
            word[len] = (char)c;
            n += walk(root, chars + j + l, links + j + l, word, len + 1, fn, ctx);
        }
        if (chars[j] & 0x80) break;
    }
    return n;
}

int engine_dict_builtin_list(void (*fn)(void *ctx, const char *word, const char *pron), void *ctx)
{
    char word[82];
    int i, n = 0;
    kernel_lock();
    for (i = 0; i <= dict_last - dict_first; i++) {
        word[0] = (char)(dict_first + i);
        n += walk(&dict_root[i], dict_root[i].chars, dict_root[i].links, word, 1, fn, ctx);
    }
    kernel_unlock();
    return n;
}

/* ---- ConvertToPhonemes: the phoneme log, kept ---- */

static struct { char *text; int n, cap; int16_t log; int silent; } C;

static void convert_text(const char *s)
{
    for (; *s; s++) {
        if (C.n + 1 >= C.cap) {
            int cap = C.cap ? 2 * C.cap : 1024;
            char *b = (char *)realloc(C.text, (size_t)cap);
            if (!b) return;
            C.text = b;
            C.cap = cap;
        }
        C.text[C.n++] = *s == '\n' ? ' ' : *s;
    }
}

void engine_convert_begin(int silent)
{
    kernel_lock();
    C.n = 0;
    C.log = dt_log;
    C.silent = silent;
    dt_log = (int16_t)(dt_log | LOG_PHONEME);
    tx_log_hook = convert_text;
    if (silent) stop_pending++;
    kernel_unlock();
}

int engine_convert_end(char *out, int size)
{
    int n;
    kernel_lock();
    if (C.silent) stop_pending--;
    tx_log_hook = console_text;
    dt_log = C.log;
    while (C.n > 0 && C.text[C.n - 1] == ' ') C.n--;
    n = C.n;
    if (out && size > 0) {
        int k = n < size ? n : size - 1;
        if (k) memcpy(out, C.text, (size_t)k);
        out[k] = 0;
    }
    kernel_unlock();
    return n;
}

/* ---- the voices ---- */

int engine_voice(int which, int16_t v[28])
{
    const int16_t *src;
    int i;
    if (which < -1 || which > 8) return 0;
    kernel_lock();
    src = which < 0 ? &cur_voice.sex : which == 8 ? &val_voice.sex : voice_defs[which];
    for (i = 0; i < 28; i++) v[i] = src[i];
    kernel_unlock();
    return 1;
}

int engine_voice_command(const int16_t v[28], char *out, int size, int part)
{
    int i, n = 0, first = part ? 14 : 0;
    n += snprintf(out + n, (size_t)(size - n), "\x02:dv");
    for (i = first; i < first + 14 && n < size; i++)
        n += snprintf(out + n, (size_t)(size - n), " %s %d", dv_param_table[i].name, v[i]);
    if (n < size) n += snprintf(out + n, (size_t)(size - n), "\x03");
    return n < size ? n : -1;
}

void engine_shutdown(void)
{
    kernel_lock();
    kernel_shutdown();
    ph_frame_sink = NULL;
    ph_mark_hook = NULL;
    ph_index_reply_hook = NULL;
    ph_console_hook = NULL;
    ph_error_hook = NULL;
    tx_log_hook = NULL;
    free(C.text);
    memset(&C, 0, sizeof C);
}
