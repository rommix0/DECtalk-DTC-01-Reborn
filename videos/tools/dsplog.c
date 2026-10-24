/* dsplog - the DSP stage by stage, sample by sample, for the explainer video (videos/PLAN.md, Task 2). Runs text through
 * the speech engine (src/speech, the ROM's code checked word for word) with every library fix off, and logs, for every
 * sample that reaches the DAC, the signal after each stage of the DSP program (src/speech/dsp_synth.c, REFERENCE.md
 * s16.10), plus each phone as it starts and, at the end, the voice in force.
 *
 * usage: dsplog out.tsv "text" ["lead-in"]
 *   The lead-in is spoken first and not logged (make_video.py passes the power-up greeting, as the graph scripts do).
 *
 * Output, one record per line, tab-separated, samples numbered from 0 at the start of the text:
 *   S n pcm lp tilt glot av_lin src noise np cf5 cf4 cf3 cf2 cf1 pf6 pf5 pf4 pf3 pf2 ab nper t0 nopen f1 f2 f3 fnz
 *        pcm: the DAC word (12 bits, as the audio has it); lp: the glottal source after the 4x source's low-pass;
 *        tilt: after the tilt filter; glot: with the breath noise; av_lin: the linear AV; src: the cascade's input
 *        (voicing x AV + noise x AH); noise: the noise generator's output; np ... pf2: each section's output (the
 *        nasal pole, cascade F5-F1, parallel F6-F2); the rest as dsp_t holds them
 *   P n code samples name   a phone starts and lasts this many samples
 *   V v0 ... v27            the voice the text left in force: its 28 [:dv] values (REFERENCE.md s8.1 order)
 *
 * The engine's state is static, so the file is included here: the link's sample hook, called in pass() only for a
 * sample the DAC gets, reads the DSP state that sample left. A section's output is its y[n-1], hist[2 + 2 s].
 */
#include "../../src/speech/engine.c"
#include "tx_rom.h"

static FILE *lf;
static int logging;
static int64_t base;

static void on_sample(void *ctx, uint16_t w)
{
    const dsp_t *d = &E.link.dsp;
    int s;
    (void)ctx;
    if (!logging) return;
    fprintf(lf, "S\t%lld\t%d\t%d\t%d\t%d\t%d\t%d\t%d", (long long)(E.link.samples - 1 - base), (int16_t)(w & 0xFFF0),
            d->lp[1], d->tilt_y, d->glot, d->av_lin, d->hist[2], d->noise);
    for (s = DSP_NP; s <= DSP_PF2; s++) fprintf(lf, "\t%d", d->hist[2 + 2 * s]);
    fprintf(lf, "\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", d->ab, d->nper, d->t0, d->nopen, d->f1, d->f2, d->f3, d->fnz);
}

static void on_phone(void *ctx, int c, int32_t samples, int64_t sample)
{
    (void)ctx;
    if (!logging) return;
    fprintf(lf, "P\t%lld\t%d\t%d\t%s\n", (long long)(sample - base), c, (int)samples,
            c >= 0 && c < 121 && sym_table[c].name ? sym_table[c].name : "?");
}

static void say(const char *text)
{
    static int16_t pcm[4096];
    int k;
    engine_write(text, (int)strlen(text));
    engine_write("\v", 1);                      /* CTRL-K: speak the last clause now */
    for (k = 0; k < 100000; k++)
        if (engine_run(pcm, 4096) < 4096 && engine_quiet()) break;
}

int main(int argc, char **argv)
{
    static int16_t pcm[4096];
    engine_hooks_t h;
    int16_t v[28];
    int i;
    if (argc < 3) {
        fprintf(stderr, "usage: dsplog out.tsv \"text\" [\"lead-in\"]\n");
        return 2;
    }
    lf = fopen(argv[1], "w");
    if (!lf) {
        perror(argv[1]);
        return 1;
    }
    memset(&h, 0, sizeof h);
    h.phone = on_phone;
    if (!engine_init(0, &h)) return 1;
    E.link.hooks.sample = on_sample;
    while (engine_run(pcm, 4096) == 4096) {}   /* whatever the boot queues */
    if (argc > 3) say(argv[3]);
    base = E.link.samples;
    logging = 1;
    say(argv[2]);
    if (engine_voice(-1, v)) {
        fprintf(lf, "V");
        for (i = 0; i < 28; i++) fprintf(lf, "\t%d", v[i]);
        fprintf(lf, "\n");
    }
    fclose(lf);
    engine_shutdown();
    return 0;
}
