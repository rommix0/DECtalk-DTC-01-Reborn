/* formantlog - the formant system frame by frame (FORMANT_SYSTEM.md). Runs text through the speech engine (src/speech,
 * the ROM's code checked word for word) and logs, for every 6.4 ms speech frame, the 17 parameter words sent to the
 * DSP and the terms phdraw adds for F1-F3; and, for every phone, what phsettar set up for the formant and bandwidth
 * tracks (targets, transitions, diphthong break points).
 *
 * usage: formantlog out.tsv "text" ["lead-in"]
 *   The lead-in is spoken first and not logged (make_formant_graphs.py passes the power-up greeting, so that the
 *   state carried from one clause to the next is the ROM's when the text starts).
 *
 * Output, one record per line, tab-separated:
 *   C frame clause                      a new clause starts
 *   P frame name duration allofeats sex, then 7 track fields for F1 F2 F3 FNZ B1 B2 B3, each
 *         "table,tarlas,tarcur,tarend,tarnex,fbou,fdur,bbou,bdur,breaks"
 *                                       phsettar started a phone (duration in frames). table: the target in the
 *                                       sex's table before coarticulation (-1 resolved, a diphthong's first point);
 *                                       tarlas/tarcur/tarend/tarnex as phsettar left them; fbou/fdur the forward
 *                                       transition (value at the boundary, frames), bbou/bdur the backward one;
 *                                       breaks: the diphthong's line end times in frames, joined by ':' ("-" = none)
 *   F frame T0 F1 F2 F3 FNZ B1 B2 B3 AV AH A2 A3 A4 A5 A6 AB TLT, then for F1 F2 F3 "tar,dip,fwd,bwd,special"
 *                                       the frame's words 1-17 as posted, and phdraw's terms: the target (tarcur),
 *                                       the diphthong line (dipcum/8), the forward and backward transitions (/8) and
 *                                       1 when a special onset value replaced the sum
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "ph_frame.h"
#include "ph_rom.h"
#include "tx_text.h"
#include "tx_rom.h"

static FILE *lf;
static int logging, frameno, clause, last_nf0ev = 1 << 30;
static ph_param_t before[PH_NPARAM];    /* the tracks as phdraw finds them */
static void (*engine_phone_hook)(int phone, int frames);

static const char *name_of(int c)
{
    return c >= 0 && c < 121 && sym_table[c].name ? sym_table[c].name : "?";
}

/* the track's target in the table, before coarticulation (phsettar's lookup) */
static int table_target(int track, int *diph)
{
    static const int row_of[PH_NPARAM] = { 0, 1, 2, -1, 3, 4, 5 };
    const int16_t (*tar)[NPH] = malfem == 1 ? maltar : femtar;
    const int16_t *dip = malfem == 1 ? maldip : femdip;
    int row = row_of[track], v;
    *diph = 0;
    if (row < 0) return ph_params[track].tarcur;
    v = tar[row][phcur];
    if (v == -1) {
        v = tar[row][phonex];
        if (v == -1) {
            v = tar[row][phonex2];
            if (v == -1) return ph_params[track].tarlas;
        }
    }
    if (v < -1) {
        *diph = 1;
        v = dip[-v];
    }
    return v;
}

static void on_phone(int phone, int frames)
{
    int i;
    memcpy(before, ph_params, sizeof before);
    if (logging) {
        fprintf(lf, "P\t%d\t%s\t%d\t%d\t%d", frameno, name_of(phcur), durfon, allofeats[nphone], malfem);
        for (i = 0; i <= PB3; i++) {
            const ph_param_t *p = &ph_params[i];
            int diph, table = table_target(i, &diph), fdur = 0, bdur = durfon - p->tbacktr, bbou;
            if (p->dftran != 0) fdur = p->ftran / p->dftran;
            bbou = p->tarend + (int)(((long)p->dbtran * bdur) >> 3);
            fprintf(lf, "\t%d,%d,%d,%d,%d,%d,%d,%d,%d,", table, p->tarlas, p->tarcur, p->tarend, p->tarnex,
                    p->tarcur + (p->ftran >> 3), fdur, bbou, bdur);
            if (diph) {
                const int16_t *nd = p->ndip;
                int t = p->durlin;
                fprintf(lf, "%d", t);
                while (t < durfon && nd < dipspec_buf + 64) {
                    t = nd[0];
                    nd += 2;
                    fprintf(lf, ":%d", t);
                }
            } else {
                fputc('-', lf);
            }
        }
        fputc('\n', lf);
    }
    if (engine_phone_hook) engine_phone_hook(phone, frames);
}

static void on_frame(void *ctx, const int16_t *w, int n)
{
    int i;
    (void)ctx;
    if (n < 18 || (uint16_t)w[0] != 0x4000) return;        /* speech frames only */
    if (logging) {
        if (nf0ev < last_nf0ev) fprintf(lf, "C\t%d\t%d\n", frameno, ++clause);
        last_nf0ev = nf0ev;
        fprintf(lf, "F\t%d", frameno);
        for (i = 1; i <= 17; i++) fprintf(lf, "\t%d", w[i]);
        for (i = PF1; i <= PF3; i++) {
            const ph_param_t *a = &ph_params[i], *b = &before[i];
            fprintf(lf, "\t%d,%d,%d,%d,%d", a->tarcur, a->dipcum >> 3, b->ftran >> 3,
                    tcum >= b->tbacktr ? b->btran >> 3 : 0, b->tspesh > 0 && tcum < b->tspesh);
        }
        fputc('\n', lf);
        frameno++;
    }
    memcpy(before, ph_params, sizeof before);
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
        fprintf(stderr, "usage: formantlog out.tsv \"text\" [\"lead-in\"]\n");
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
    engine_phone_hook = ph_phone_hook;          /* the engine's own; ours goes in front of it */
    ph_phone_hook = on_phone;
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
