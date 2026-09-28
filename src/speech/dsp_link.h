/* dsp_link.h - the 68000's link to the DSP, and the DAC clock (REFERENCE.md s16.11).
 *
 * In the DTC-01 the 68000 hands the DSP its frames through the SPC: a 32-word input FIFO, a semaphore the DSP raises
 * when it wants the 68000 (bit 0 = error) and the 68000 clears to acknowledge (the DSP reads that on its BIO line),
 * and the DSP's reset line. klsyn posts frames to a queue (dsp_post_frame 0x7b56); the semaphore interrupt
 * (dsp_command_queue_isr 0x12258) sends them to the FIFO one at a time. The DAC takes a sample every 100 us.
 *
 * Here the same parts are C: the SPC, the queue and its interrupt handler, the tone timer, and the DSP program
 * (dsp_synth.c). The DAC clock is the caller: dsp_link_run() makes samples until it has enough or the DSP has
 * nothing to do. Nothing runs by itself; the caller decides when frames are posted and samples are taken.
 *
 * Tags (s16.13): a post may carry a tag (not 0), e.g. an index mark. It rides with the frame's words through the
 * FIFO, and mark() reports it with the number of the first sample the DSP makes after reading that frame: the
 * sample the frame is first heard in. Samples are numbered from 0 at dsp_link_init.
 */
#ifndef DSP_LINK_H
#define DSP_LINK_H

#include "dsp_synth.h"

#define DSP_LINK_POOL 48        /* dsp_frame_pool: (300 * 10 + 63) / 64 + 1 messages, about 300 ms of frames */
#define DSP_LINK_WORDS 26       /* words per message (0x1a) */
#define DSP_LINK_FIFO 32        /* the SPC's input FIFO */
#define DSP_TICK_SAMPLES 100    /* a clock tick (10 ms) in DAC samples */
#define DSP_HELD_MAX 256        /* samples the DSP can make while the 68000 waits on it */
#define DSP_TAGS_MAX 8          /* tags read and not yet reported */

/* dsp_link_t.tone_hook: what the tick hook runs when its count ends (dsp_tone_timeout_hook 0x124a2: the tones to
 * 0 Hz; dsp_tone_done_hook 0x124fe: the item comes back) */
enum { DSP_TONE_OFF, DSP_TONE_TIMEOUT, DSP_TONE_DONE };

/* A queue message (dsp_msg_t): the frame's words and, for a tone command (bit 15), the second word. */
typedef struct dsp_msg {
    struct dsp_msg *next;
    int n;
    uint16_t w[DSP_LINK_WORDS];
    uint16_t tone_arg;          /* the ROM's +0xe */
    int on_ticks, off_ticks;    /* a tone item's tone and silence, in 10 ms ticks (the ROM's dialer: 16 and 6) */
    uint32_t tag;               /* the caller's (0 = none): reported by mark() at the frame's first sample */
} dsp_msg_t;

/* What the link reports (all optional). The test harness uses them to follow the DSP's ports; the library uses
 * returned() for the dialer's wait and mark() for index marks at audio time. */
typedef struct dsp_link_hooks {
    void *ctx;
    void (*poll)(void *ctx, int where, int acked);      /* a BIO poll and what it found */
    void (*word)(void *ctx, uint16_t w);                /* the DSP read a FIFO word */
    void (*signal)(void *ctx, uint16_t v);              /* the DSP raised the semaphore */
    void (*reset)(void *ctx);                           /* the 68000 reset the DSP */
    void (*sample)(void *ctx, uint16_t w);              /* the DSP made a DAC word */
    void (*returned)(void *ctx, const dsp_msg_t *m);    /* a message went back to the pool */
    void (*mark)(void *ctx, uint32_t tag, int64_t sample); /* a tagged frame is first heard in this sample */
    /* tests: the DSP has computed a sample but not yet sent it; a post made here comes during the pass, and a
     * reset it causes kills the sample, as it does in the ROM */
    void (*mid_pass)(void *ctx);
} dsp_link_hooks_t;

typedef struct dsp_link {
    dsp_t dsp;
    dsp_io_t io;
    dsp_link_hooks_t hooks;

    /* the SPC (0x9C000 flags, 0x9C002 FIFO) */
    uint16_t fifo[DSP_LINK_FIFO];
    uint32_t fifo_tag[DSP_LINK_FIFO];   /* the tag of the message whose first word is here */
    int fifo_head, fifo_tail, fifo_count;
    int sem;                    /* the DSP's semaphore (flags bit 7) */
    int err;                    /* its error bit (flags bit 5) */
    int irq_en;                 /* flags bit 6: the semaphore interrupts the 68000 */
    int in_reset;               /* flags bit 0: the DSP is held in reset */

    /* the 68000 side */
    dsp_msg_t pool[DSP_LINK_POOL];
    dsp_msg_t *free_list;
    int nfree;
    dsp_msg_t *head, *tail;     /* dsp_queue 0x82256 */
    dsp_msg_t *speaker;         /* 0x8226c: the last speaker frame, sent again after every reset */
    int tone_active;            /* 0x82270 */
    int tone_hook;              /* the tick hook 0x82272: DSP_TONE_... */
    int tone_ticks;             /* ticks until it runs */
    int tone_off_ticks;         /* the silence after the tone item being played */
    int tick_samples;           /* dsp_link_run: samples since the last tick */
    int in_isr;
    long resets;                /* DSP resets so far */

    /* samples the DSP made while the 68000 waited for its semaphore (tone words), not yet taken */
    int16_t held[DSP_HELD_MAX];
    int held_head, nheld;

    uint32_t tags[DSP_TAGS_MAX];        /* tagged frames the DSP has read, to report at the next sample */
    int ntags;
    int64_t samples;                    /* samples made so far: the number of the next one */
    int yield;                          /* set by a hook: dsp_link_run returns after this sample (then clears it) */
} dsp_link_t;

/* dsp_link_init 0x7aec + dsp_queue_init 0x12226: the pool, the queue, and the DSP reset, waiting for the 68000. */
void dsp_link_init(dsp_link_t *l, const dsp_link_hooks_t *hooks);

/* dsp_post_frame 0x7b56: queue a frame of n words as klsyn gives it (a 0x6000 speaker frame without its checksum,
 * a 0x4000 speech frame without its trailer; both are added here). 0 if the pool is empty: in the ROM klsyn waits
 * there until the DSP has taken a frame. */
int dsp_link_post(dsp_link_t *l, const int16_t *w, int n, uint32_t tag);

/* phone_dial's tone item: w0 = 0x9000 | Hz (high tone), arg = 0x8000 | Hz (low tone). It plays for 16 ticks, then
 * 6 ticks of silence, then comes back (returned()). 0 if the pool is empty. */
int dsp_link_post_tone(dsp_link_t *l, uint16_t w0, uint16_t arg, uint32_t tag);
/* The same with other lengths (the library's TextToSpeechPlayTones), in 10 ms ticks; at least 1 each. */
int dsp_link_post_tone_ticks(dsp_link_t *l, uint16_t w0, uint16_t arg, int on_ticks, int off_ticks, uint32_t tag);
/* The DSP has played the last tone item and has nothing queued: it is in tone mode, making silence, until the next
 * frame (which resets it). */
int dsp_link_tone_done(const dsp_link_t *l);

int dsp_link_room(const dsp_link_t *l);                 /* free messages */
/* Nothing queued, and the DSP makes no sample until the next frame: it waits for the 68000, is idle (three frame
 * times without a frame), or has played its last tone item. */
int dsp_link_quiet(const dsp_link_t *l);

/* The library's Reset (no counterpart in v1.8): drop every queued message, reset the DSP and send it the speaker
 * frame again; the DSP then waits for the next frame. */
void dsp_link_flush(dsp_link_t *l);

/* The DAC clock: up to max samples (PCM: the DAC's 12 bits, as int16_t). Returns how many it made; fewer than max
 * means the DSP is waiting for the 68000 (no frame queued, or idle after three frame times without one), or that a
 * hook set yield (the library: a message came back while klsyn waits for one). */
int dsp_link_run(dsp_link_t *l, int16_t *pcm, int max);

/* The 10 ms clock tick, which times the tones. dsp_link_run() calls it every 100 samples; tests may call it
 * themselves (in the ROM it is the clock interrupt, not tied to the DAC). */
void dsp_link_tick(dsp_link_t *l);

/* One DSP pass (for tests; it does not tick): 1 with a sample in *pcm, 0 without; *starved = the DSP waits for the 68000. */
int dsp_link_step(dsp_link_t *l, int16_t *pcm, int *starved);

#endif
