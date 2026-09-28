/* pitchlog - the pitch system frame by frame (PITCH_SYSTEM.md). Runs text through the speech engine (src/speech,
 * the ROM's code checked word for word) and logs, for every 6.4 ms speech frame, the F0 and the terms pht0draw
 * sums, plus each F0 command at the frame pht0draw reads it, each phone and each new clause.
 *
 * usage: pitchlog out.tsv "text" ["lead-in"]
 *   The lead-in is spoken first and not logged. make_pitch_graphs.py passes the power-up greeting, so that the
 *   state carried from one clause to the next (baseline, smoother, hat) is the ROM's when the text starts.
 *
 * Output, one record per line, tab-separated:
 *   C frame clause                      a new clause starts (pht0draw's command index went back)
 *   E frame command                     an F0 command read in this frame (f0tar[], PITCH_SYSTEM.md s4.1)
 *   P frame name duration allofeats     phsettar started a phone (duration in frames)
 *   F frame f0_tenths f0 f0baseline tarhat tarimp f0seg f0in phone stress f0mode T0 AV dip smoothed
 *                                       the frame: F0 after the voice (tenths of Hz), F0 before it, the four terms
 *                                       and their sum, the phone and its stress, f0mode, T0 and AV as posted to the
 *                                       DSP, the glottal-stop dip and the smoother's output before the dip
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "ph_frame.h"
#include "tx_text.h"
#include "tx_rom.h"

static FILE *lf;
static int logging, frameno, clause, last_nf0ev = 1 << 30, last_nphone = -1;

static void on_frame(void *ctx, const int16_t *w, int n)
{
    int i, start = last_nf0ev, newcl = 0, d, dip = 0;
    (void)ctx;
    if (!logging || n < 2 || (uint16_t)w[0] != 0x4000) return;      /* speech frames only */
    if (nf0ev < last_nf0ev) {                   /* pht0draw started a new clause */
        newcl = 1;
        start = 0;
        fprintf(lf, "C\t%d\t%d\n", frameno, ++clause);
    }
    for (i = start; i < nf0ev && i < PH_MAXEV; i++)
        fprintf(lf, "E\t%d\t%d\n", frameno, f0tar[i]);
    last_nf0ev = nf0ev;
    if (nphone != last_nphone || newcl) {
        int c = allophons[nphone];
        fprintf(lf, "P\t%d\t%s\t%d\t%d\n", frameno,
                c >= 0 && c < 121 && sym_table[c].name ? sym_table[c].name : "?", allodurs[nphone], allofeats[nphone]);
        last_nphone = nphone;
    }
    d = nframg - tglstp;                        /* the dip pht0draw added (ph_frame.c) */
    if (d < 0) d = -d;
    if (f0mode == 0 && d < 6) dip = d * 100 - 600;
    fprintf(lf, "F\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", frameno, f0_tenths, f0, f0baseline,
            tarhat, tarimp, f0seg, f0in, phcur, allofeats[nphone] & 7, f0mode, w[1], w[9], dip, f0las2 >> 3);
    frameno++;
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
    engine_hooks_t h;
    if (argc < 3) {
        fprintf(stderr, "usage: pitchlog out.tsv \"text\" [\"lead-in\"]\n");
        return 2;
    }
    lf = fopen(argv[1], "w");
    if (!lf) {
        perror(argv[1]);
        return 1;
    }
    memset(&h, 0, sizeof h);
    h.frame = on_frame;
    if (!engine_init(0, &h)) return 1;
    {
        static int16_t pcm[4096];
        while (engine_run(pcm, 4096) == 4096) {}    /* whatever the boot queues */
    }
    if (argc > 3) say(argv[3]);
    logging = 1;
    say(argv[2]);
    fclose(lf);
    engine_shutdown();
    return 0;
}
