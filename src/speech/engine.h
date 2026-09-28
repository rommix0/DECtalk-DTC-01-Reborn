/* The speech side as one engine (REFERENCE.md s17.9): the dttask and klsyn tasks on the kernel
 * (src/kernel/kernel.c), the DSP link and program, and the DAC clock. What the library's thread drives.
 *
 * Text goes in with engine_write, as the host side writes it into the ROM's text pipe; [:...] commands and phonemic
 * text are in-band. engine_run makes the audio: it runs the tasks, then the DAC clock, and back, so klsyn posts a
 * frame whenever the DSP link has room (the ROM's 48-message queue). Everything is in the order the ROM's tasks
 * would do it; only the audio's timing is the library's: no late frames (the tasks always keep up).
 *
 * One engine per process, from power-up: the firmware's state is in globals.
 */
#ifndef ENGINE_H
#define ENGINE_H
#include <stdint.h>
#include "../kernel/rtos.h"

typedef struct {
    void *ctx;
    /* An index mark ([:index mark n], DT_INDEX, DT_INDEX_REPLY; code << 16 | value) is first heard in this sample
     * (numbered from 0 at engine_init). The library calls engine_mark_spoken when that sample is played or delivered.
     * NULL: the engine does it itself, at the sample. */
    void (*mark)(void *ctx, uint32_t mark, int64_t sample);
    /* a phone (its code, as phsettar has it) starts in this sample and lasts this many samples; NULL: not tracked */
    void (*phone)(void *ctx, int phone, int32_t samples, int64_t sample);
    void (*index_reply)(void *ctx, int value);          /* a DT_INDEX_REPLY mark was spoken (the ROM: R2 31, R3 value) */
    void (*console)(void *ctx, int c);                  /* the console: DT_LOG text, [:dv] output; NULL = dropped */
    void (*panic)(void *ctx, const char *task, const char *msg);
    /* tests */
    void (*frame)(void *ctx, const int16_t *w, int n);  /* every frame klsyn posts (dsp_post_frame's words) */
    void (*call)(void *ctx, const void *obj);           /* kernel_hooks_t.call and .got */
    void (*got)(void *ctx, mbox_t *mb, msg_t *m);
} engine_hooks_t;

/* Start the tasks from power-up. fixes: dsp_t.fixes (DSP_FIX_...; 0 = the ROM's DSP), and ENGINE_FIX_MARKS: index
 * marks alone in [ ] leave the speech as without them (tx_text.h, TX_FIX_MARKS; REFERENCE s17.13). 0 if a task cannot
 * start. */
#define ENGINE_FIX_MARKS 0x100
/* ENGINE_FIX_SPLIT: a voice or rate change in the middle of the text does not leave the part before it's durations as
 * user durations for what follows (ph_frame.h, PH_FIX_SPLIT) */
#define ENGINE_FIX_SPLIT 0x200
int engine_init(int fixes, const engine_hooks_t *hooks);
/* Text for the pipe (the host side's cur_stream). Never waits: the pipe grows. */
void engine_write(const char *text, int n);
/* Bytes written and not yet read by dttask. The ROM's pipe holds 64 and then makes the host task wait (and the host
 * line send XOFF); a caller that wants that holds back at a limit of its own. */
int engine_pending(void);
/* Up to max samples (the DAC's 12 bits, as int16_t, 10 kHz). Fewer than max: nothing more to say for now (the text
 * so far is spoken, or waits for the end of its clause). */
int engine_run(int16_t *pcm, int max);
/* Run the tasks (no samples), then 1 if the engine would make no sample now: every byte read, nothing queued for the
 * DSP, the DSP waiting or idle. For an output with no room (memory output without a buffer): nothing to wait for. */
int engine_quiet(void);
/* A mark reported by hooks.mark is heard now: its action (last_index, the DT_INDEX_REPLY answer). */
void engine_mark_spoken(uint32_t mark);
/* The library's Reset: DT_STOP (the clause being drawn ends after its phone, the rest is dropped), and then the text
 * not yet read, the frames not yet heard and their marks are dropped too. Samples already made stay. */
void engine_flush(void);
/* The flag words and values the host side reads and writes in the ROM */
enum { ENGINE_MODE, ENGINE_LOG, ENGINE_LAST_INDEX, ENGINE_ERRORS, ENGINE_RATE, ENGINE_VOICE };
int engine_get(int what);                   /* ENGINE_ERRORS (dt_error_flags) reads and clears; ENGINE_RATE is
                                               sprate, ENGINE_VOICE the voice (0 Paul ... 8 Val) */
void engine_set(int what, int value);       /* ENGINE_MODE (DT_MODE), ENGINE_LOG (DT_LOG) */
/* A tone item for the DSP (the dialer's): high and low in Hz (0-4095, 0 = off), on_ms of tone, then off_ms of
 * silence (10 ms steps). It follows what is queued. 0 when the DSP queue is full. */
int engine_tone(int high, int low, int on_ms, int off_ms);
/* The user dictionary (DT_DICT). set: an empty or NULL subst deletes; 0 = no memory (v1.8's R3 1). get: 1 if the
 * name is there exactly, with its substitution. list: every entry in name order; the count, or -1. */
int engine_dict_set(const char *name, const char *subst);
int engine_dict_get(const char *name, char *subst, int size);
void engine_dict_clear(void);
int engine_dict_list(void (*fn)(void *ctx, const char *name, const char *subst), void *ctx);
/* The built-in dictionary: 1 if the word is in it (exactly, without the user dictionary or suffixes), with its
 * pronunciation as phonemic text (the DT_LOG phoneme log's names). list: every word in trie order; the count. */
int engine_dict_builtin(const char *word, char *out, int size);
int engine_dict_builtin_list(void (*fn)(void *ctx, const char *word, const char *pron), void *ctx);
/* ConvertToPhonemes: between begin and end the phoneme log (DT_LOG 0x02: each clause by name) is kept instead of
 * going to the console. silent: klsyn drops the clauses meanwhile (v1.8's DT_STOP flag), so nothing is spoken; the
 * caller waits in between until the text has been through. end: the text, lines joined by blanks; its length. */
void engine_convert_begin(int silent);
int engine_convert_end(char *out, int size);
/* A voice's 28 [:dv] values (s8.1 order): which 0-7 the built-in ones, 8 Val, -1 the current one. 0 if bad. */
int engine_voice(int which, int16_t v[28]);
/* The [:dv] text that sets them: part 0 the first 14 values, part 1 the other 14 (each within the clause buffer's
 * 200 words), as STX ":dv ..." ETX. The length, or -1. */
int engine_voice_command(const int16_t v[28], char *out, int size, int part);
/* The samples made so far. */
int64_t engine_samples(void);
/* Stop the tasks. */
void engine_shutdown(void);

#endif
