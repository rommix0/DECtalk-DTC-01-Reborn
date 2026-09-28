/* dtc01term's speech side (REFERENCE.md s17.3, s17.14): what the host C expects from the firmware's speech half and
 * from the board, made of the speech library's public API (ttsapi.h) only.
 *
 * The host C writes the text pipe, syncs, stops, sets DT_LOG/DT_MODE, fills the user dictionary, dials, and reads
 * last_index and the DSR errors; the speech half calls back for [:re n] replies and its console output. Here each of
 * these is a library call or a callback. Library callbacks only queue events; the program's 10 ms loop hands them to
 * the tasks (term_speech_tick), so the kernel's lock is never taken on the library's thread.
 */
#ifndef TERM_SPEECH_H
#define TERM_SPEECH_H
#include "kernel.h"

/* Start the library (the audio device, or a wave file) and put the factory record in the in-memory NVRAM. quiet = the
 * self-test jumper closed (no banner). 0 = started; else the message is in err. */
int term_speech_start(unsigned device, const char *wave, int quiet, char *err, int errlen);
/* The kernel hooks the glue needs (kernel_init's argument). */
const kernel_hooks_t *term_speech_hooks(void);
/* With the lock held, after kernel_init and term_dev_init: the ROM's boot, the main task (which calls speech_init). */
void term_speech_boot(void);
/* With the lock held, every tick before kernel_run: hand the queued events to the tasks, read the DSR errors. */
void term_speech_tick(void);
/* With the lock held, after kernel_run: the text written to the pipe goes to the library. */
void term_speech_flush(void);
/* 1 when DECTST 1 / TEST POWER asked for a restart; term_speech_restart then does it (lock held; it reboots). */
int term_speech_restart_wanted(void);
void term_speech_restart(void);
/* With the lock held: nothing is waiting to be spoken or heard, and no host-line timeout is due (for stdio's end). */
int term_speech_idle(void);
void term_speech_stop(void);
/* A test aid (--log-pipe): a line "<task> TAB <HH>" for every byte a task writes into the text pipe. -1 = the file
 * cannot be opened. */
int term_speech_log_pipe(const char *path);

#endif
