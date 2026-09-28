/* dsp_synth.c - the DSP (TMS32010) program of the DECtalk DTC-01 v1.8 in C (REFERENCE.md s16, s16.10).
 *
 * The program of dsp/dsp_v1.8.lst as plain C. Every value is the program's: each store keeps 16 bits as the chip's
 * does (the int16_t assignments wrap), the right shifts are the high-word stores (SACH), and the filters' sums
 * saturate at 32 bits as the chip's accumulator does in overflow mode, which the program never turns off. The
 * comments give the program's labels and addresses. test_dsp checks it against the ROM sample for sample.
 * Right shifts of negative values are arithmetic (as on every compiler this builds with).
 */
#include <string.h>
#include "dsp_synth.h"

/* What the program writes to the semaphore (OUT PA0). The acknowledge is data RAM 0x07's value, 50: any even word. */
enum { SIG_READY = 0, SIG_ERROR = 1, SIG_ACK = 50 };

/* Where the tables lie in the program ROM (the reads are by ROM address; dsp_rom.c). */
enum { FNZ_A = 0x021, FNZ_B = 0x044, FNZ_C = 0x067, AMPTABLE = 0x08A, COS_TABLE = 0x0E2, B0 = 0x1C7 };

/* dsp_reset_coeffs: loaded at reset. The others are the parallel F6's and the nasal pole's B and C, and five words
 * the program never reads (REFERENCE s16.10). */
#define LFSR_SEED ((uint16_t)dsp_reset_coeffs[0])
#define FNZ_OFS ((int16_t)dsp_reset_coeffs[6])      /* 31: the nasal-zero tables start at 248 Hz */
#define LP_C ((int16_t)dsp_reset_coeffs[9])         /* the 4x source's low-pass */
#define LP_B ((int16_t)dsp_reset_coeffs[10])
#define LP_A 700                                    /* its input gain (an MPYK) */

/* dsp_init_consts, the program's constants (data RAM 0x00-0x10). */
#define ONE 0x1000                                  /* 1.0 in Q12 */
#define THIRD 0x2AAB                                /* 1/3 in Q15 */
#define TILT_OFS 12                                 /* subtracted from the tilt word */
#define TONE_HALF 5000                              /* tone mode: half a period, in Hz x samples */
#define TONE_LOUD 1000                              /* tone mode: tones from 1000 Hz up get amplitude 2 */

/* ---- a word of the program ROM's tables, which lie back to back from word 0x000 ---- */
static int16_t rom(int addr)
{
    static const struct { const void *t; unsigned first, n; } tabs[] = {
        { dsp_vectors, 0x000, 3 }, { dsp_init_consts, 0x003, 17 }, { dsp_reset_coeffs, 0x014, 13 },
        { dsp_fnz_a, FNZ_A, 35 }, { dsp_fnz_b, FNZ_B, 35 }, { dsp_fnz_c, FNZ_C, 35 }, { dsp_amptable, AMPTABLE, 88 },
        { dsp_cos_table, COS_TABLE, 229 }, { dsp_b0, B0, 224 },
    };
    unsigned a = (unsigned)addr & 0xFFF, i;
    for (i = 0; i < sizeof tabs / sizeof tabs[0]; i++)
        if (a >= tabs[i].first && a < tabs[i].first + tabs[i].n)
            return ((const int16_t *)tabs[i].t)[a - tabs[i].first];
    return 0;   /* the program's code, which the 68000's frames never make it read */
}

/* ---- COS_LOOKUP 0x709 (A), 0x721 (B), 0x72A (C): the cosine (Q13) of a frequency in Hz, on a piecewise scale:
 * up to 200 Hz entry 0; then 4 Hz steps to 400, 8 to 800, 16 to 1600, 32 to 3200, 64 above. B and C start the
 * tests further up (the ROM uses B for F3, C for the speaker's frequencies), so they misplace lower frequencies. ---- */
enum { COS_A, COS_B, COS_C };
static int16_t cosine(int f, int entry)
{
    int i;
    if (entry == COS_A && f <= 200) i = 0;
    else if (entry == COS_A && f < 400) i = (f >> 2) - 50;
    else if (entry == COS_A && f < 800) i = f >> 3;
    else if (entry != COS_C && f < 1600) i = (f >> 4) + 50;
    else if (f < 3200) i = (f >> 5) + 100;
    else i = (f >> 6) + 150;
    return rom(COS_TABLE + i);
}

/* A dB word -> linear x gain: amptable[db + 10] x gain >> sh; 0 unless db > -10. */
static int16_t level(int16_t db, int16_t gain, int sh)
{
    int16_t i = (int16_t)(db + 148);
    return i > 138 ? (int16_t)((rom(AMPTABLE + 10 + db) * gain) >> sh) : 0;
}

/* B and C of a resonator from a frequency and a bandwidth term (Q12): B = cos f x (1 - bw), C = 2 bw - 1. */
static void tune(dsp_res_t *r, int16_t f, int16_t bw, int entry)
{
    int16_t k = (int16_t)(ONE - bw);
    r->c = (int16_t)(2 * bw - ONE);
    r->b = (int16_t)((cosine(f, entry) * k) >> 12);
}

/* A normalized to a gain: A = gain x (1 - B - C). (SACH shifts by 0, 1 or 4 only; the program gets the >> 11 by
 * doubling the product in two halves.) */
static void set_gain(dsp_res_t *r, int16_t gain)
{
    int16_t k = (int16_t)(ONE - r->b - r->c);
    r->a = (int16_t)((gain * k) >> 11);
}

/* The filters' sum: the accumulator saturates at 32 bits; then the high word of acc << 4. rnd is 0 (the ROM: the
 * shift truncates) or 0x800 (DSP_FIX_ROUND). */
static int16_t mac3(int32_t p1, int32_t p2, int32_t p3, int rnd)
{
    int64_t s = (int64_t)p1 + p2 + p3 + rnd;
    if (s > INT32_MAX) s = INT32_MAX;
    else if (s < INT32_MIN) s = INT32_MIN;
    return (int16_t)(s >> 12);
}

/* A two-pole section on its history h (h[0] = y[n-2], h[1] = y[n-1]). */
static int16_t resonate(const dsp_res_t *r, int16_t *h, int16_t x, int rnd)
{
    int16_t y = mac3(r->c * h[0], r->b * h[1], r->a * x, rnd);
    h[0] = h[1];
    h[1] = y;
    return y;
}

/* ---- COLD_START 0x2A7: signal ready, clear the state, load the fixed coefficients ---- */
static void cold_start(dsp_t *d)
{
    const dsp_io_t *io = d->io;
    int fixes = d->fixes;
    io->signal(io->ctx, SIG_READY);
    memset(d, 0, sizeof *d);
    d->io = io;
    d->fixes = fixes;
    d->res[DSP_PF6].b = (int16_t)dsp_reset_coeffs[1];
    d->res[DSP_PF6].c = (int16_t)dsp_reset_coeffs[2];
    d->res[DSP_NP].b = (int16_t)dsp_reset_coeffs[3];
    d->res[DSP_NP].c = (int16_t)dsp_reset_coeffs[4];
    d->mode = DSP_WAIT_READY;
}

/* ---- CLEAR_STATE 0x2EE: clear the source low-pass and 24 history words (all but the parallel F2's y[n-1]) ---- */
static void clear_state(dsp_t *d)
{
    d->lp[0] = d->lp[1] = 0;
    memset(d->hist, 0, 24 * sizeof d->hist[0]);
}

static void signal_error(dsp_t *d)
{
    d->io->signal(d->io->ctx, SIG_ERROR);
    clear_state(d);
}

/* ---- the speaker frame (header 0x6000, 24 words), 0x310-0x3D1 ---- */
static void read_speaker_frame(dsp_t *d)
{
    uint16_t w[24], sum = 0;
    int i;
    w[0] = 0x6000;
    for (i = 1; i < 24; i++) w[i] = d->io->read_word(d->io->ctx);
    memset(d->hist, 0, 12 * sizeof d->hist[0]);        /* the nasal zero's, the pole's, F5-F3's, F2's y[n-2] */
    tune(&d->res[DSP_CF4], w[1], w[2], COS_C);          /* f4 b4, f5 b5 */
    tune(&d->res[DSP_CF5], w[3], w[4], COS_C);
    tune(&d->res[DSP_PF4], w[5], 400, COS_C);           /* p4, p5, with fixed bandwidths */
    tune(&d->res[DSP_PF5], w[6], 500, COS_C);
    /* w[7]: the F0 minimum, which only the 68000 uses */
    d->skew = w[8];                                     /* la */
    d->res[DSP_CF5].a = rom(AMPTABLE + (int16_t)w[9]);  /* the cascade gains, dB */
    d->res[DSP_CF4].a = rom(AMPTABLE + (int16_t)w[10]);
    d->gain_f3 = rom(AMPTABLE + (int16_t)w[11]);
    d->gain_f2 = rom(AMPTABLE + (int16_t)w[12]);
    d->gain_f1 = rom(AMPTABLE + (int16_t)w[13]);
    d->ri = w[14];                                      /* the open phase: RI x T0 + NF */
    d->nf = w[15];
    d->br = rom(AMPTABLE + (int16_t)w[16]);             /* breathiness */
    /* w[17]: the F0 scale, which only the 68000 uses */
    d->lfsr = LFSR_SEED;
    d->hs = w[18];                                      /* head size (Q12) */
    d->gf = rom(AMPTABLE + (int16_t)w[19]);
    d->res[DSP_NP].a = rom(AMPTABLE + (int16_t)w[20]);  /* gn */
    d->gv = rom(AMPTABLE + (int16_t)w[21]);
    d->gh = rom(AMPTABLE + (int16_t)w[22]);
    for (i = 0; i < 24; i++) sum = (uint16_t)(sum + w[i]);
    if (sum & 0x7FFF) signal_error(d);                  /* w[23] is the checksum */
    else d->io->signal(d->io->ctx, SIG_ACK);
}

/* ---- the speech frame (header 0x4000 or 0x2000, 19 words), READ_FRAME_19 0x3D3; 0 if its trailer is wrong ---- */
static int read_speech_frame(dsp_t *d)
{
    uint16_t w[18];
    int i, first = d->t0 == 0;
    for (i = 0; i < 18; i++) w[i] = d->io->read_word(d->io->ctx);
    d->t0 = w[0]; d->f1 = w[1]; d->f2 = w[2]; d->f3 = w[3]; d->fnz = w[4];
    d->b1 = w[5]; d->b2 = w[6]; d->b3 = w[7]; d->av = w[8];
    d->ah = level(w[9], d->gh, 12);
    d->a2 = level(w[10], d->gf, 15);
    d->a3 = level(w[11], d->gf, 15);
    d->res[DSP_PF4].a = level(w[12], d->gf, 15);
    d->res[DSP_PF5].a = level(w[13], d->gf, 15);
    d->res[DSP_PF6].a = level(w[14], d->gf, 15);
    d->ab = level(w[15], d->gf, 12);
    d->tilt = (int16_t)(w[16] - TILT_OFS);
    if (w[17] != 0x43D4) return 0;
    /* DSP_FIX_FIRST_PERIOD: the first frame after a reset starts a glottal period at once, as parwav's does (its T0
     * is 0 until the first period). The ROM stores the frame's T0 and waits for it, and meanwhile the cascade keeps
     * the coefficients of the empty frame: 0 Hz, zero bandwidth, poles on the unit circle at DC, which make the
     * first frames' aspiration into a low thump (REFERENCE s16.12). */
    if (first && (d->fixes & DSP_FIX_FIRST_PERIOD)) d->nper = d->t0;
    d->spoke = 1;
    d->io->signal(d->io->ctx, SIG_ACK);

    /* FRAME_COEFFS 0x456: head size scales F1 about 256 Hz, F2 about 512 Hz, F3 about 0 */
    d->f1 = (int16_t)((256 * (ONE - d->hs) + d->f1 * d->hs) >> 12);
    d->f2 = (int16_t)((512 * (ONE - d->hs) + d->f2 * d->hs) >> 12);
    d->f3 = (int16_t)((d->f3 * d->hs) >> 12);
    /* the parallel F2 and F3, with fixed bandwidths */
    tune(&d->res[DSP_PF2], d->f2, 210, COS_A);
    set_gain(&d->res[DSP_PF2], d->a2);
    tune(&d->res[DSP_PF3], d->f3, 280, COS_B);
    set_gain(&d->res[DSP_PF3], d->a3);
    return 1;
}

/* ---- PITCH_SYNC_RESET 0x4DF: a new glottal period (parwav pitch_synch_par_reset) ---- */
static void pitch_sync_reset(dsp_t *d)
{
    int16_t k, i;
    d->vwave = d->glot = d->nper = 0;
    d->av_lin = (int16_t)(d->av + 142) > 0 ? rom(AMPTABLE + 4 + d->av) : 0;
    d->br_t0 = (int16_t)((d->br * (int16_t)(d->t0 * 8)) >> 12);    /* breathiness x T0 */
    d->t0 = (int16_t)(d->t0 + (int16_t)((d->skew * d->t0) >> 12)); /* the alternating skew */
    d->skew = (int16_t)-d->skew;
    d->tilt_a = (int16_t)(1094 * d->tilt);                          /* the tilt filter (kept when tilt < 0) */
    if (d->tilt >= 0) d->tilt_b = (int16_t)(0x7FFF - d->tilt_a);
    d->nmod = d->av_lin > 0 ? d->t0 >> 1 : d->t0;                   /* noise is halved after nmod */

    /* the open phase: RI x T0 + NF, 40..263, shorter than T0; b = B0[nopen - 40], a = (b + 1) nopen / 3 */
    d->nopen = (int16_t)((int16_t)((d->ri * d->t0) >> 15) + d->nf);
    if (d->nopen < 40) d->nopen = 40;
    if (d->nopen > 263) d->nopen = 263;
    if (d->nopen >= d->t0) d->nopen = (int16_t)(d->t0 - 1);
    d->gl_b = rom(B0 + d->nopen - 40);
    k = (int16_t)(d->gl_b + 1);
    if (d->nopen > 95) {
        k = (int16_t)(k * d->nopen);
        d->gl_a = (int16_t)((k * THIRD) >> 15);
    } else {
        d->gl_a = (int16_t)((k * THIRD) >> 15);
        d->gl_a = (int16_t)(d->gl_a * d->nopen);
    }

    /* the cascade F1-F3 and the nasal zero */
    tune(&d->res[DSP_CF1], d->f1, d->b1, COS_A);
    set_gain(&d->res[DSP_CF1], d->gain_f1);
    d->res[DSP_CF1].a = (int16_t)(d->res[DSP_CF1].a * 8);
    tune(&d->res[DSP_CF2], d->f2, d->b2, COS_A);
    set_gain(&d->res[DSP_CF2], d->gain_f2);
    tune(&d->res[DSP_CF3], d->f3, d->b3, COS_B);
    set_gain(&d->res[DSP_CF3], d->gain_f3);
    i = (int16_t)((d->fnz >> 3) - FNZ_OFS);                         /* FNZ in 8 Hz steps from 248 Hz */
    d->res[DSP_NZ].a = rom(FNZ_A + i);
    d->res[DSP_NZ].b = rom(FNZ_B + i);
    d->res[DSP_NZ].c = rom(FNZ_C + i);
}

/* ---- SAMPLE 0x4A5 to 0x679: one output sample of speech ---- */
static int16_t speech_sample(dsp_t *d)
{
    int16_t *h = d->hist, out, x;
    dsp_res_t lowpass;
    int k, s, rnd = d->fixes & DSP_FIX_ROUND ? 0x800 : 0;

    /* the noise generator: 16-bit LFSR, low-passed (noise += its last value / 8), halved after nmod */
    if ((int16_t)d->lfsr > 0) {
        x = (int16_t)(d->lfsr & 0x1FFF);
        d->lfsr = (uint16_t)(d->lfsr << 1);
    } else {
        x = (int16_t)(d->lfsr | 0xE000);
        d->lfsr = (uint16_t)((d->lfsr << 1) ^ 0x0641);
    }
    d->noise = d->noise_y = (int16_t)(x + (d->noise_y >> 3));
    if (d->nmod < d->nper) d->noise >>= 1;

    /* GLOTTAL_4X_LOOP 0x4C2: the natural source at 4x the rate, through a 2-pole low-pass */
    lowpass.a = LP_A; lowpass.b = LP_B; lowpass.c = LP_C;
    for (k = 0; k < 4; k++) {
        if (d->nper <= d->nopen) {                                  /* open phase: a -= b; vwave += a / 16 */
            int64_t v;
            d->gl_a = (int16_t)(d->gl_a - d->gl_b);
            v = (int64_t)d->gl_a * 4096 + (int64_t)d->vwave * 65536;
            d->vwave = (int16_t)((v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v) >> 16);
            d->glot = (int16_t)((d->vwave * d->gv) >> 12);
        } else if (d->nper >= d->t0) {
            pitch_sync_reset(d);
        } else {
            d->glot = 0;
        }
        d->glot = resonate(&lowpass, d->lp, d->glot, rnd);
        d->nper++;
    }

    /* NOISE_AND_MIX 0x5CD: tilt, breath noise in the open phase, then voicing + aspiration into the cascade */
    d->glot = d->tilt_y = (int16_t)((d->tilt_b * d->glot + d->tilt_a * d->tilt_y) >> 15);
    if (d->nper < d->nopen - 2)
        d->glot = (int16_t)((d->glot * 32768 + d->br_t0 * (int16_t)d->lfsr) >> 15);
    x = (int16_t)((d->glot * d->av_lin) >> 12);
    h[2] = (int16_t)((d->ah * d->noise + x * 32768) >> 15);

    /* CASCADE_BRANCH 0x5E8: the nasal zero (FIR), then the nasal pole, F5, F4, F3, F2, F1 */
    out = mac3(d->res[DSP_NZ].c * h[0], d->res[DSP_NZ].b * h[1], d->res[DSP_NZ].a * h[2], rnd);
    h[0] = h[1];
    h[1] = h[2];
    for (s = DSP_NP; s <= DSP_CF1; s++) out = resonate(&d->res[s], &h[1 + 2 * s], out, rnd);
    out = (int16_t)(out * 2);

    /* PARALLEL_BRANCH 0x632: F6, F5, F4, F3, F2 on the noise, with alternating signs, minus the bypass */
    for (s = DSP_PF6; s <= DSP_PF2; s++) out = (int16_t)(resonate(&d->res[s], &h[1 + 2 * s], d->noise, rnd) - out);
    return (int16_t)(out - (int16_t)((d->ab * d->noise) >> 15));
}

/* ---- TONE_MODE 0x698: the tone generator reuses the speech state's words; entering it zeroes the steps and phases,
 * and the amplitudes keep what those words held (nmod, and the parallel F2's y[n-1]) until a tone word sets them ---- */
static void enter_tone_mode(dsp_t *d)
{
    d->step[0] = d->step[1] = d->phase[0] = d->phase[1] = 0;
    d->amp[0] = d->nmod;
    d->amp[1] = d->hist[24];
    d->mode = DSP_TONE;
}

/* TONE_LOOP 0x6A9: a tone word (bit 12: oscillator 1, else 2; the low 12 bits: Hz). A positive word ends tone mode
 * (COLD_START) and returns 0. */
static int tone_word(dsp_t *d, uint16_t w)
{
    int i = (w & 0x1000) ? 0 : 1;
    if ((int16_t)w > 0) { cold_start(d); return 0; }
    d->io->signal(d->io->ctx, SIG_ACK);
    d->phase[i] = (d->fixes & DSP_FIX_TONE) ? TONE_HALF / 2 : 0;   /* DSP_FIX_TONE: a quarter period in, the zero */
    d->step[i] = (int16_t)(w & 0x0FFF);
    d->amp[i] = d->step[i] < TONE_LOUD ? 1 : 2;
    if (!d->step[i] && (d->fixes & DSP_FIX_TONE)) d->amp[i] = 0;
    return 1;
}

/* TONE_SAMPLE 0x6D2: the phase counts Hz and turns over at 5000 (half a period at 10 kHz), where the sign flips. */
static int16_t tone_sample(dsp_t *d)
{
    int i, out = 0;
    for (i = 0; i < 2; i++) {
        int p = d->phase[i] + d->step[i];
        if (p >= TONE_HALF) { p -= TONE_HALF; d->amp[i] = (int16_t)-d->amp[i]; }
        d->phase[i] = (int16_t)p;
    }
    for (i = 0; i < 2; i++) out += cosine(d->phase[i], COS_A) * d->amp[i];
    return (int16_t)out;
}

/* ---- MAIN_LOOP 0x2F9: take the next frame. Returns 1 to go on with speech, 0 when the DSP goes idle (NO_FRAME
 * 0x49E, IDLE_DECAY 0x2E7: three frame times without a frame), 2 when a tone word came. ---- */
static int next_frame(dsp_t *d)
{
    for (;;) {
        uint16_t header;
        if (!d->io->frame_waiting(d->io->ctx, DSP_AT_FRAME)) {
            if (d->idle <= 2) { d->idle++; return 1; }      /* keep the last frame */
            if (d->spoke) { d->spoke = 0; signal_error(d); }
            else clear_state(d);
            return 0;
        }
        d->idle = 0;
        header = d->io->read_word(d->io->ctx);
        if (header & 0x8000) {
            enter_tone_mode(d);
            return tone_word(d, header) ? 2 : 0;
        }
        if (!(header & 0x6000) || (header & 0x1FFF)) signal_error(d);
        else if (header == 0x6000) read_speaker_frame(d);
        else if (read_speech_frame(d)) return 1;
        else signal_error(d);
    }
}

/* ---- the public side ---- */
void dsp_init(dsp_t *d, const dsp_io_t *io)
{
    memset(d, 0, sizeof *d);
    d->io = io;
    d->mode = DSP_WAIT_READY;
}

void dsp_reset(dsp_t *d)
{
    cold_start(d);
}

int dsp_step(dsp_t *d, uint16_t *sample)
{
    if (d->mode == DSP_WAIT_READY) {                               /* WAIT_68K_READY 0x2D9 */
        if (!d->io->frame_waiting(d->io->ctx, DSP_AT_READY)) return 0;
        d->nopen = 200;
        d->mode = DSP_SPEECH;
        d->frame_due = 1;
    }
    if (d->mode == DSP_TONE) {
        if (d->io->frame_waiting(d->io->ctx, DSP_AT_TONE) && !tone_word(d, d->io->read_word(d->io->ctx)))
            return 0;
        *sample = (uint16_t)tone_sample(d);
        return 1;
    }
    if (d->frame_due) {
        int r = next_frame(d);
        if (r == 0) return 0;
        if (r == 2) { *sample = (uint16_t)tone_sample(d); return 1; }
        d->frame_due = 0;
    }
    *sample = (uint16_t)speech_sample(d);
    if (++d->nsamp >= 64) {                                        /* FRAME_TIMEOUT_CHECK 0x2DF */
        d->nsamp = 0;
        d->frame_due = 1;
    }
    return 1;
}
