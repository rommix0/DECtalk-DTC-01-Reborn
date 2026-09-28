/* dsp_synth.h - the DSP (TMS32010) program of the DECtalk DTC-01 v1.8 in C (REFERENCE.md s16, s16.10).
 *
 * The DSP turns the 68000's frames into samples: a speaker frame (24 words, header 0x6000) sets the voice, a speech
 * frame (19 words, header 0x4000) every 64 samples sets the formants and amplitudes, and tone words (bit 15) run the
 * two-tone generator. dsp_step() does what the program does between two samples, and returns the sample it sends
 * to the DAC. It gives the ROM program's samples exactly, including its fixed-point arithmetic.
 */
#ifndef DSP_SYNTH_H
#define DSP_SYNTH_H

#include <stdint.h>

/* Where the program asks whether the 68000 has acknowledged (its BIO line: the semaphore is clear again). */
enum { DSP_AT_READY, DSP_AT_FRAME, DSP_AT_TONE };

/* The program's ports: the 68000's input FIFO and semaphore. */
typedef struct dsp_io {
    void *ctx;
    int (*frame_waiting)(void *ctx, int where);     /* 1: acknowledged, the next frame (or tone word) is there */
    uint16_t (*read_word)(void *ctx);               /* IN PA1: the next word of the input FIFO */
    void (*signal)(void *ctx, uint16_t value);      /* OUT PA0: raise the semaphore; bit 0 = error */
} dsp_io_t;

enum { DSP_WAIT_READY, DSP_SPEECH, DSP_TONE };      /* dsp_t.mode */

/* dsp_t.fixes: departures from the ROM's program, for the library (0, the ROM's behaviour, is what the tests check).
 * Set them after dsp_init(); a reset keeps them. */
enum {
    DSP_FIX_FIRST_PERIOD = 1,   /* start the first glottal period at the first speech frame after a reset: no thump */
    DSP_FIX_ROUND = 2,          /* round the filters' sums instead of truncating them: no DC offset */
    DSP_FIX_TONE = 4            /* tones start at a zero crossing, and a 0 Hz tone is silent (the ROM's holds the cosine's
                                   peak: a DC step of half the scale): no clicks, no DC */
};

/* A two-pole section, y = a x + b y[n-1] + c y[n-2] (Q12); for the nasal zero, y = a x + b x[n-1] + c x[n-2]. */
typedef struct dsp_res { int16_t a, b, c; } dsp_res_t;

/* The resonators, in the order the program runs them: the nasal zero, then the cascade, then the parallel branch. */
enum {
    DSP_NZ, DSP_NP, DSP_CF5, DSP_CF4, DSP_CF3, DSP_CF2, DSP_CF1,
    DSP_PF6, DSP_PF5, DSP_PF4, DSP_PF3, DSP_PF2, DSP_NRES
};

typedef struct dsp {
    const dsp_io_t *io;
    int fixes;                  /* DSP_FIX_... */
    int mode;                   /* DSP_WAIT_READY after a reset, then DSP_SPEECH or DSP_TONE */
    int frame_due;              /* the next step first takes a frame (every 64 samples) */

    /* the speech frame: as sent (F1-F3 then scaled by head size), and its amplitudes made linear */
    int16_t t0, f1, f2, f3, fnz, b1, b2, b3, av, tilt;
    int16_t ah, a2, a3, ab;
    /* the speaker frame */
    int16_t gain_f1, gain_f2, gain_f3;  /* the cascade F1-F3 gains (F4, F5 and the nasal pole's are in res[].a) */
    int16_t hs, gf, gv, gh, br, ri, nf, skew;

    dsp_res_t res[DSP_NRES];
    /* The filters' histories in the program's order: [0] [1] the nasal zero's x[n-2] x[n-1], [2] its input (the
     * source), then y[n-2] y[n-1] of each section from DSP_NP to DSP_PF2. A reset of the state clears a prefix. */
    int16_t hist[3 + 2 * (DSP_NRES - 1)];

    /* the voicing source (parwav's natural source at 4x the output rate) and the noise */
    int16_t nper, nopen, gl_a, gl_b, vwave, glot, av_lin, br_t0, nmod;
    int16_t lp[2];              /* the 4x source's low-pass: y[n-2], y[n-1] */
    int16_t tilt_a, tilt_b, tilt_y;
    uint16_t lfsr;
    int16_t noise, noise_y;

    int16_t nsamp;              /* samples since the last frame time */
    int16_t idle;               /* frame times without a frame */
    int spoke;                  /* a speech frame came since the DSP last went idle */

    int16_t step[2], phase[2], amp[2];  /* tone mode: the two oscillators */
} dsp_t;

void dsp_init(dsp_t *d, const dsp_io_t *io);        /* power-up: state clear, held in reset */
void dsp_reset(dsp_t *d);                            /* the 68000 lets it out of reset: COLD_START */
/* One pass: 1 with the next DAC word in *sample, or 0 when the pass made none (waiting for the 68000, or idle
 * because no frame has come for three frame times). */
int dsp_step(dsp_t *d, uint16_t *sample);

/* The program ROM's tables (dsp_rom.c), in ROM order from word 0x000 to 0x2A6. */
extern const uint16_t dsp_vectors[3];
extern const uint16_t dsp_init_consts[17];
extern const uint16_t dsp_reset_coeffs[13];
extern const int16_t dsp_fnz_a[35], dsp_fnz_b[35], dsp_fnz_c[35];
extern const int16_t dsp_amptable[88];
extern const int16_t dsp_cos_table[229];
extern const int16_t dsp_b0[224];

#endif
