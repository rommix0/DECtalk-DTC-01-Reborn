/* Phonetic frame path, rebuilt from the DTC-01 v1.8 68000 ROM (REFERENCE.md s15.17).
 * ROM addresses are given for every routine; dapi file names for the matching later source. */
#include <stddef.h>
#include "ph_frame.h"
#include "ph_rom.h"
#include "ph_math.h"

/* ---- state (RAM addresses in ph_frame.h) ---- */
int16_t allophons[PH_MAXALLO], allodurs[PH_MAXALLO], allofeats[PH_MAXALLO];
uint32_t index_marks[PH_MAXALLO];
int16_t f0cmd_area[PH_MAXEV + PH_MAXALLO];
int16_t nallotot, nf0tot, f0mode, stop_pending, phmode_8229c, dt_error_flags;

voice_t cur_voice;
int16_t malfem, spdeftltoff, assertiveness, f2max, f3max, spdef_packet[SP_WORDS - 1];
int16_t f0basefall, ef_x10, f0_dep_tilt;
int16_t spdef_dirty = 1;                /* 0x8229e, .data: the first clause posts the speaker packet */

int16_t tcum, durfon, nphone;
ph_param_t ph_params[PH_NPARAM];
int16_t dipspec_buf[64];
int16_t parstochip[PH_FRAME_WORDS] = { PH_FRAME_HDR };
int16_t phsettar_started;
int16_t phcur, pholas, phonex, phonex2, pholas2;
int16_t feacur, fealas, feanex, strucur, strucnex, struclas2;
int16_t endtyp_las, begtyp_cur, endtyp_cur, begtyp_nex;
int16_t dur_ratio_q14, ftran_scale, btran_scale, bouval, durtran, stop_save[4];

int16_t nf0ev, f0, nfram, nframb, nframs, nframg, extrad, tglstp, tglstp_next, segdur, segdrg;
int16_t f0las1, f0las2, tarhat, tarimp, f0a2, f0b, f0a1, dtimf0, phonex_t0, f0seg_raw, f0seg, f0basestart, f0in;
int16_t np_drawt0, npg, f0command, delimp, f0baseline, f0endfall, phocur_t0;
int16_t vibsw, vibcum, newnote, delnote, vibcount, vibstep, f0_tenths;

void (*ph_frame_sink)(const int16_t *words, int n);
void (*ph_error_hook)(const char *fmt, int a, int b);
void (*ph_phone_hook)(int phone, int frames);

/* 0x81bda-0x81be6, chosen by sex in phsettar */
static const ph_locus_t (*p_locus)[3];  /* maleloc / femloc */
static const int16_t *p_diph;           /* maldip / femdip */
static const int16_t (*p_tar)[NPH];     /* maltar / femtar */
static const int16_t (*p_amp)[4][6];    /* malamp / femamp */

#define N85PRCNT 0x3667   /* 0.85 in Q14 */
#define N15PRCNT 0x999    /* 0.15 in Q14 */

static void log_error(const char *fmt, int a, int b)
{
    if (ph_error_hook) ph_error_hook(fmt, a, b);
}

/* ======================================================================================================
 * dsp_post_frame 0x7b56 (dapi send_pars): append the checksum (speaker packet, header 0x6000: the sum of all
 * 24 words is 0) or the trailer 0x43D4, and hand the frame on. The ROM posts it to the DSP queue.
 * ====================================================================================================== */
void dsp_post_frame(const int16_t *w, int n)
{
    int16_t out[32], last;
    int i;
    if (w[0] == PH_SPEAKER_HDR) {
        last = PH_SPEAKER_HDR;
        for (i = 1; i < n; i++) last = (int16_t)(last + w[i]);
        last = (int16_t)-last;
    } else {
        last = (int16_t)PH_FRAME_TRAILER;
    }
    for (i = 0; i < n; i++) out[i] = w[i];
    out[n] = last;
    if (ph_frame_sink) ph_frame_sink(out, n + 1);
}

/* ======================================================================================================
 * phclause_draw_frames 0xa782 (dapi ph_claus.c, "6. Phonetic Component"): one pass per 6.4 ms frame.
 * The ROM calls pht0draw after phdraw (dapi: before).
 * ====================================================================================================== */
void phclause_draw_frames(void)
{
    tcum = -1;
    nf0ev = -1;
    durfon = 0;
    nphone = 0;
    for (;;) {
        if (++tcum >= durfon) {
            tcum = (int16_t)(tcum - durfon);
            if (++nphone > nallotot) return;
            phsettar();
            if (ph_phone_hook) ph_phone_hook(phcur, durfon);
        }
        phdraw();
        pht0draw();
        dsp_post_frame(parstochip, PH_FRAME_WORDS);
    }
}

/* ======================================================================================================
 * setloc 0xc420 (dapi ph_setar.c): locus rule for F1-F3 at a consonant-vowel boundary. Sets bouval (value at
 * the boundary) and durtran (transition length) from the locus table.
 * ====================================================================================================== */
static void setloc(int phone, int typ, int typ_other, int param, int target)
{
    int t, set;
    const ph_locus_t *l;
    int16_t locus, pct;
    if (param >= 3 || typ != 4 || typ_other == 4) return;
    t = typ_other == 5 ? 3 : typ_other;
    set = plocu[t - 1][phone];
    if (set == 0) return;
    l = &p_locus[set - 1][param];
    locus = l->locus;
    pct = l->pct_q14;
    if (typ_other != t && param != 0 && (featb[phone] & 0x800) == 0) pct = (int16_t)((pct >> 1) + 0x2000);
    bouval = (int16_t)(q14(pct, target - locus) + locus);
    durtran = l->durtran;
}

/* ======================================================================================================
 * diph_time_scale 0xc570: rescale a diphthong time point (frames) to the phone's actual length. The middle of
 * the phone stretches by dur_ratio_q14; the first and last quarter change less.
 * ====================================================================================================== */
static int16_t diph_time_scale(int16_t t)
{
    int16_t d = inhdr[phcur], half = (int16_t)(d >> 1), quarter = (int16_t)(half >> 1), b;
    int flip = 0;
    if (t > half) { t = (int16_t)(d - t); flip = 1; }
    if (t < quarter) {
        b = dur_ratio_q14 > 0x4000 ? (int16_t)((dur_ratio_q14 >> 2) + 0x3000) : (int16_t)(2 * dur_ratio_q14 - 0x4000);
        if (b < 1) b = 1;
        t = q14(t, b);
    } else {
        if (dur_ratio_q14 > 0x4000) {
            b = (int16_t)(0x5ff7 - (dur_ratio_q14 >> 1));
        } else {
            int16_t a = dur_ratio_q14 < 0x2001 ? 0x2001 : dur_ratio_q14;
            b = (int16_t)(0xC000 - 2 * a);        /* 32-bit in the ROM, stored as a word */
        }
        if (b < 1) b = 1;
        t = (int16_t)(half - q14(half - t, b));
    }
    if (flip) t = (int16_t)(d - t);
    return (int16_t)(q14(t, dur_ratio_q14) + 1);
}

/* coarticulation of F1-F3 targets: 85 % own target, 15 % the neighbour's */
static int16_t coartic(int own, int other)
{
    return (int16_t)(q14(own, N85PRCNT) + q14(other, N15PRCNT));
}

/* ======================================================================================================
 * phsettar 0xaa08 (dapi ph_setar.c): at each phone boundary, set the target and the forward/backward
 * transitions of all 15 tracks, plus the special onset values (bursts, VOT, aspiration).
 * ====================================================================================================== */
void phsettar(void)
{
    int i;
    int16_t vot, v, *dip_out = dipspec_buf;
    int row = 0;                                /* the target row (F1 F2 F3 B1 B2 B3 AV) */
    ph_param_t *np;

    if (phsettar_started < 1) {                 /* first call since power-up */
        phsettar_started++;
        nf0ev = -2;                             /* pht0draw: hard init */
        pholas2 = 0;
        struclas2 = 0;
        pholas = 0;
        ph_params[PF1].tarend = reset_tarend[0];
        ph_params[PF2].tarend = reset_tarend[1];
        ph_params[PF3].tarend = reset_tarend[2];
    } else {
        pholas2 = pholas;
        if (nphone > 1) struclas2 = allofeats[nphone - 2];
        pholas = phcur;
    }
    if (malfem == 1) { p_locus = maleloc; p_diph = maldip; p_tar = maltar; p_amp = malamp; }
    else             { p_locus = femloc;  p_diph = femdip; p_tar = femtar; p_amp = femamp; }

    if (index_marks[nphone] != 0) {
        int k, kept;
        index_mark_reached(index_marks[nphone]);
        index_marks[nphone] = 0;
        for (k = kept = 0; k < ph_nextra_marks; k++) {  /* [DTC01] PH_FIX_MARKS: the phone's other marks */
            if (ph_extra_marks[k].at == nphone) index_mark_reached(ph_extra_marks[k].mark);
            else ph_extra_marks[kept++] = ph_extra_marks[k];
        }
        ph_nextra_marks = kept;
    }
    if (stop_pending > 0) {                     /* DT_STOP: end the clause after this phone */
        stop_save[0] = allophons[nphone + 1];
        stop_save[1] = allodurs[nphone + 1];
        stop_save[2] = allofeats[nphone + 1];
        if (nphone < nallotot) {
            allophons[nphone + 1] = 0;
            allodurs[nphone + 1] = allodurs[nallotot];
            allofeats[nphone + 1] = allofeats[nallotot];
            stop_save[3] = nallotot;
            nallotot = (int16_t)(nphone + 1);
        } else {
            stop_save[3] = nallotot;
        }
    }

    phcur = allophons[nphone];
    strucur = (int16_t)(allofeats[nphone] & 7);
    durfon = allodurs[nphone];
    if (nphone < nallotot) { phonex = allophons[nphone + 1]; strucnex = allofeats[nphone + 1]; }
    else                   { phonex = 0; strucnex = 0; }
    phonex2 = nphone < nallotot - 1 ? allophons[nphone + 2] : 0;
    fealas = featb[pholas];
    feacur = featb[phcur];
    feanex = featb[phonex];
    endtyp_las = endtyp[pholas];
    begtyp_cur = begtyp[phcur];
    endtyp_cur = endtyp[phcur];
    begtyp_nex = begtyp[phonex];

    if ((feacur & 0x20) == 0) {
        dur_ratio_q14 = durfon < inhdr[phcur] * 2 ? muldiv(0x4000, durfon, inhdr[phcur]) : 0x7fff;
        ftran_scale = (int16_t)((dur_ratio_q14 >> 1) + 0x2000);
        btran_scale = (int16_t)((dur_ratio_q14 >> 1) + 0x1704);
    }

    ph_params[PB1].tspesh = 0;
    ph_params[PB2].tspesh = 0;
    for (i = PA2; i <= PAB; i++) ph_params[i].tspesh = 0;

    if (phcur > 0x2c) {                         /* stops and affricates: burst on the parallel amplitudes */
        int16_t burst = stop_burst_dur[phcur - 0x2d];
        if (durfon - burst < 8 && burst > 1) burst--;
        if (feacur & 0x40) {
            if ((feanex & 0x20) || burst < 1) burst = 1;
            if (((feanex & 0x80) || (feanex & 0x40)) && burst_place[phonex] <= burst_place[phcur]) burst = 0;
        }
        for (i = PA2; i <= PAB; i++) {
            ph_params[i].tspesh = (int16_t)(durfon - burst);
            ph_params[i].pspesh = 0;
        }
    }

    ph_params[PAV].pspesh = 0;                  /* voice onset time after a stop */
    ph_params[PAH].pspesh = 0;
    vot = 0;
    if ((phcur != 0 || pholas == 0 || (allofeats[nphone - 1] & 0x1000)) &&
        (fealas & 0x40) && (fealas & 2) == 0 && (feacur & 0x10)) {
        vot = vot_frames[0];
        ph_params[PAH].pspesh = 0x3a;
        if (phmode_8229c == 0) {
            if (strucur < 2) { vot = vot_frames[1]; ph_params[PAH].pspesh = 0x37; }
            if (pholas2 == 0x29 && (struclas2 & 0x3c0) == 0) vot = vot_frames[2];
        }
    }
    if (vot > 0) {
        if ((feacur & 0x200) == 0) {
            if (vot > durfon >> 1 && (allofeats[nphone] & 0x1000) == 0 && phmode_8229c == 0 && strucur >= 2)
                vot = (int16_t)(durfon >> 1);
        } else {
            vot = (int16_t)(vot + vot_frames[3]);
            if (vot > durfon) vot = (int16_t)(durfon - 1);
        }
    }
    ph_params[PAV].tspesh = vot;
    ph_params[PAH].tspesh = vot;

    for (i = 0, np = ph_params; i < PH_NPARAM; i++, np++) {
        int c = partyp[i];
        int f123 = i <= PF3;
        np->tarlas = np->tarend;

        /* ---- target of the next phone (dapi getbegtar) ---- */
        v = 0;
        if (c < 3) {
            if (c == 2) {
                v = burst_amp_set[phonex];
                if (v > 0) {
                    int t = phonex2 == 0 ? endtyp_cur : begtyp[phonex2];
                    int k = t - 1;
                    if (k == 4) k = 2;
                    v = p_amp[v - 1][k][i - PA2];
                }
            } else if (c == 0) {
                if (i == PAV) {
                    v = p_tar[row][phonex];
                    if ((feanex & 0x40) && (feacur & 2) == 0) v = 0;
                } else {
                    v = phonex == 0x1c ? 0x3a : 0;
                }
            }
        } else {
            v = p_tar[row][phonex];
            if (v == -1) v = p_tar[row][phonex2];
            if (v < -1) v = p_diph[-v];
        }
        if (i == PFZ) v = (feanex & 0x80) ? 0x20f : fnz_default;
        np->tarnex = v;

        /* ---- target of this phone ---- */
        if (c < 3) {
            if (c == 0) {
                if (i == PAV) {
                    v = p_tar[row][phcur];
                    if ((feacur & 0x40) && (fealas & 2) == 0) v = 0;
                } else {
                    v = phcur == 0x1c ? 0x3a : 0;
                }
            } else if (c == 2) {
                int ph = (phcur == 0x2f && phonex == 0x1a) ? 0x36 : phcur;
                v = burst_amp_set[ph];
                if (v > 0) {
                    int t = phonex == 0 ? endtyp_las : begtyp_nex;
                    int k = t - 1;
                    int16_t a;
                    if (k == 4) k = 2;
                    a = p_amp[v - 1][k][i - PA2];
                    v = a;
                    if ((phonex == 0 || (strucnex & 0x1000)) && (feacur & 0x40)) {
                        v = (int16_t)(i == PAB ? a - 4 : a - 8);
                        if (v < 0) v = 0;
                    }
                }
            } else if (i == PFZ) {
                v = (feacur & 0x80) ? 0x20f : fnz_default;
            }
        } else {
            v = p_tar[row][phcur];
            if (v == -1) {
                v = p_tar[row][phonex];
                if (v == -1) {
                    v = p_tar[row][phonex2];
                    if (v == -1) v = np->tarlas;
                }
                if (v < -1) v = p_diph[-v];
            }
        }

        if (v < -1) {
            /* diphthong: expand the {value, time} lines into dipspec_buf as {end time, slope x8} pairs */
            int idx = -v, first = 0, cont;
            int16_t from, to = 0, t_from = 0, t_to, dt;
            np->ndip = dip_out;
            from = p_diph[idx];
            if (f123) from = coartic(from, np->tarlas);
            np->tarcur = from;
            do {
                if (!first) {
                    to = from;
                    first = 1;
                    idx++;
                } else {
                    to = p_diph[idx++];
                    if (f123 && np->tarnex > 0) to = coartic(to, np->tarnex);
                }
                t_to = p_diph[idx];
                t_to = t_to == -1 ? durfon : diph_time_scale(t_to);
                dip_out[0] = t_to;
                dt = (int16_t)(t_to - t_from);
                if (dt == 0) {
                    dip_out[1] = 0;
                } else {
                    int16_t delta8 = (int16_t)((to - from) * 8);
                    /* dt is -1 when two rescaled time points step back a frame: divtab[-1] is then the ROM word
                       before the table (divtab_ext in ph_rom.c) */
                    dip_out[1] = dt < 50 ? q14(divtab[dt], delta8) : (int16_t)long_divide(delta8, dt);
                }
                dip_out += 2;
                from = to;
                t_from = t_to;
                cont = p_diph[idx++] != -1;
            } while (cont);
            np->tarend = to;
            np->durlin = *np->ndip++;
            np->deldip = *np->ndip++;
        } else {
            if (f123) v = coartic(v, np->tarnex < 1 ? np->tarlas : (np->tarnex + np->tarlas) >> 1);
            np->tarcur = v;
            np->deldip = 0;
            np->durlin = durfon;
            np->tarend = np->tarcur;
        }
        np->dipcum = 0;
        if (np->tarnex == -1) np->tarnex = np->tarend;
        if (i != PFZ) row++;
        if (f123) np->tarnex = coartic(np->tarnex, np->tarend);

        /* ---- forward transition (smoothing from the last phone) ---- */
        bouval = (int16_t)((np->tarcur + np->tarlas) >> 1);
        durtran = 5;
        if (c == 3) {
            if (feacur & 0x10) {
                if ((feacur & 0x200) == 0) {
                    durtran = 8;
                    if (fealas & 0x200) {
                        bouval = (int16_t)((np->tarlas + bouval) >> 1);
                        durtran = 9;
                        if (pholas == 0x1b && i == PF1) bouval = (int16_t)(bouval + 0x50);
                    }
                } else {
                    durtran = 5;
                    if ((fealas & 0x200) == 0) {
                        bouval = (int16_t)((np->tarcur + bouval) >> 1);
                        durtran = 3;
                    }
                }
            }
            if (phcur == 0) {
                bouval = np->tarlas;
                durtran = durfon;
            } else {
                setloc(pholas, endtyp_las, begtyp_cur, i, np->tarcur);
                setloc(phcur, begtyp_cur, endtyp_las, i, np->tarlas);
                if (feacur & 0x20) { durtran = 5; if (feacur & 0x40) durtran = durfon; }
                if (feacur & 0x80) { durtran = durfon; if (i == PF1) durtran = 0; }
                if (i == PF1 && (fealas & 0x80)) { durtran = 0x10; if (bouval < 0x1a9) bouval = 0x1a9; }
            }
            if ((feacur & 0x20) == 0 && durtran > 0) durtran = (int16_t)(q14(durtran, ftran_scale) + 1);
        } else if (c == 4) {
            durtran = 6;
            if ((feacur & 2) == 0) durtran = 3;
            else if (i == PB1 && (fealas & 2) == 0 && pholas != 0) { durtran = 8; bouval = (int16_t)(bouval + 0x14); }
            if (pholas == 0)      { bouval = (int16_t)(np->tarcur + 50 * (PB3 - i)); durtran = 8; }
            else if (phcur == 0)  { bouval = (int16_t)(np->tarlas + 50 * (PB3 - i)); durtran = 8; }
            if (i == PB1 && (fealas & 0x80)) { durtran = 0x10; bouval = (int16_t)(bouval + 0x46); }
        } else if (c == 2 || c == 0) {
            int16_t s = np->tarcur;
            if (bouval < s - 12) {
                bouval = (int16_t)(s - 12);
                if (i == PAV) {
                    durtran = 2;
                    if (pholas == 0 && ((feacur & 0x200) || strucur > 1)) { durtran = 7; bouval = (int16_t)(s - 20); }
                    if (fealas & 0x20) bouval = (int16_t)(s - 6);
                }
            }
            if ((fealas & 0x80) && np->tarlas < np->tarcur) durtran = 0;
            if (bouval < (int16_t)(np->tarlas - 12)) {
                bouval = (int16_t)(np->tarlas - 15);
                if (phcur == 0) durtran = 11;
                if (i == PAV) durtran = 0;
            }
            if (i == PA4 && (phcur == 0x36 || phcur == 0x37)) {
                durtran = (int16_t)(durfon - 2);
                bouval = (int16_t)(ph_params[PA4].tarcur - 0x28);
            }
        } else if (i == PFZ) {
            durtran = 0;
            if (fealas & 0x80) { bouval = 0x15e; durtran = 0x10; }
        }
        if (durtran > durfon) durtran = durfon;
        if (durtran > 0x14) durtran = 0x14;
        if (bouval < 0) bouval = 0;
        np->ftran = 0;
        if (durtran > 0) {
            np->ftran = (int16_t)((bouval - np->tarcur) * 8);
            if (np->ftran != 0) {
                np->dftran = q14(np->ftran, divtab[durtran]);
                np->ftran = (int16_t)(np->dftran * durtran);
            }
        }

        /* ---- backward transition (smoothing into the next phone) ---- */
        bouval = (int16_t)((np->tarnex + np->tarend) >> 1);
        durtran = 4;
        if (c == 3) {
            if (feacur & 0x10) {
                durtran = 8;
                if ((feacur & 0x200) == 0) {
                    if (feanex & 0x200) {
                        bouval = (int16_t)((np->tarnex + bouval) >> 1);
                        durtran = 9;
                        if (phonex == 0x1b && i == PF1) bouval = (int16_t)(bouval + 0x50);
                    }
                } else {
                    durtran = 6;
                    if ((feanex & 0x200) == 0) {
                        bouval = (int16_t)((np->tarend + bouval) >> 1);
                        durtran = 3;
                    }
                }
            }
            if (phonex == 0) {
                durtran = 0;
            } else {
                setloc(phonex, begtyp_nex, endtyp_cur, i, np->tarend);
                setloc(phcur, endtyp_cur, begtyp_nex, i, np->tarnex);
                if (feacur & 0x20) { durtran = 5; if (feacur & 0x40) durtran = durfon; }
                if (feacur & 0x80) { durtran = durfon; if (i == PF1) durtran = 0; }
                if (i == PF1 && (feanex & 0x80)) { durtran = 0x14; if (bouval < 0x1e0) bouval = 0x1e0; }
            }
            if ((feacur & 0x20) == 0 && durtran > 0) durtran = (int16_t)(q14(durtran, btran_scale) + 1);
        } else if (c == 4) {
            durtran = 6;
            if ((feacur & 2) == 0) durtran = 3;
            else if (i == PB1 && (feanex & 2) == 0) durtran = 8;
            if (phonex == 0)      { bouval = (int16_t)(np->tarend + 50 * (PB3 - i)); durtran = 8; }
            else if (phcur == 0)  { bouval = (int16_t)(np->tarnex + 50 * (PB3 - i)); durtran = 8; }
            if (i == PB1 && (feanex & 0x80)) { durtran = 0x10; bouval = (int16_t)(bouval + 0x46); }
        } else if (c == 2 || c == 0) {
            int take = 0;
            int16_t s = (int16_t)(np->tarnex - 12);
            if (bouval < s) { bouval = s; if (phcur == 0) durtran = 0xb; }
            if (i == PAV && bouval < ph_params[PAV].tarnex && phonex != 0x2a) {
                durtran = 0;                    /* set even when the next test fails */
                take = (feacur & 0x40) || phcur == 0x36;
            }
            if (take) {
                if ((feacur & 2) == 0) bouval = 0;
                else { bouval = 0x14; durtran = 7; }
            } else {
                int16_t floor_ = (int16_t)(np->tarend - 12);
                if ((feacur & 0x80) && np->tarend < np->tarnex) durtran = 0;
                if (phcur > 0x2c) { durtran = 2; if (phcur < 0x36) floor_ = np->tarend; }
                if (bouval < floor_) {
                    bouval = (int16_t)(floor_ - 3);
                    if (i == PAV) {
                        bouval = (int16_t)(floor_ + 4);
                        if (phonex == 0) { bouval = (int16_t)(floor_ - 2); durtran = 0xc; }
                    }
                    if (i == PAH) bouval = ph_params[PAH].tarend;
                }
                if (phonex > 0x2c) durtran = 0;
            }
        } else if (i == PFZ) {
            durtran = 0;
            if (feanex & 0x80) { bouval = 0x15e; durtran = 0x14; }
        }
        if (durtran > 0x14) durtran = 0x14;
        if (durtran > durfon) durtran = durfon;
        np->tbacktr = (int16_t)(durfon - durtran);
        if (bouval < 0) bouval = 0;
        np->btran = 0;
        np->dbtran = 0;
        if (durtran > 0) {
            int16_t d8 = (int16_t)((bouval - np->tarend) * 8);
            if (d8 != 0) np->dbtran = q14(d8, divtab[durtran]);
        }
    }

    if (vot > 0) {                              /* wider B1/B2 during aspiration */
        ph_params[PB1].tspesh = vot;
        ph_params[PB2].tspesh = vot;
        if ((fealas & 2) == 0) {
            ph_params[PB1].pspesh = (int16_t)(ph_params[PB1].tarcur + 0xfa);
            ph_params[PB2].pspesh = (int16_t)(ph_params[PB2].tarcur + 0x50);
        }
    }
}

/* ======================================================================================================
 * phdraw 0xa804 (dapi ph_draw.c, DK 1984): realize one frame from the tracks. Writes F1 ... AB of parstochip.
 * ====================================================================================================== */
void phdraw(void)
{
    int16_t *out = &parstochip[OUT_F1], value;
    ph_param_t *np;
    for (np = &ph_params[PF1]; np <= &ph_params[PB3]; np++, out++) {
        if (np->durlin < tcum) {               /* next straight line of a diphthong */
            np->durlin = *np->ndip++;
            np->deldip = *np->ndip++;
            np->tarcur = (int16_t)((np->dipcum >> 3) + np->tarcur);
            np->dipcum = 0;
        }
        np->dipcum = (int16_t)(np->dipcum + np->deldip);
        value = (int16_t)((np->ftran >> 3) + (np->dipcum >> 3) + np->tarcur);
        if (np->ftran != 0) np->ftran = (int16_t)(np->ftran - np->dftran);
        if (tcum < np->tbacktr) {
            *out = value;
        } else {
            *out = (int16_t)((np->btran >> 3) + value);
            np->btran = (int16_t)(np->btran + np->dbtran);
        }
        if (np->tspesh > 0 && tcum < np->tspesh) *out = np->pspesh;
    }
    if (parstochip[OUT_F2] > f2max) parstochip[OUT_F2] = f2max;
    if (parstochip[OUT_F3] > f3max) parstochip[OUT_F3] = f3max;

    for (; np <= &ph_params[PAB]; np++, out++) {
        value = (int16_t)((np->ftran >> 3) + np->tarcur);
        if (np->ftran != 0) np->ftran = (int16_t)(np->ftran - np->dftran);
        if (tcum < np->tbacktr) {
            *out = value;
        } else {
            *out = (int16_t)((np->btran >> 3) + value);
            np->btran = (int16_t)(np->btran + np->dbtran);
        }
        if (np->tspesh > 0) {
            if (tcum < np->tspesh) {
                *out = np->pspesh;
            } else {
                if (np > &ph_params[PAH] && tcum == np->tspesh + 1 && *out > 9)
                    *out = (int16_t)(*out - 10);             /* second, weaker burst */
                if (np == &ph_params[PAV] && *out > 0x28 && tcum - ph_params[PAV].tspesh < 8)
                    *out = (int16_t)(*out - (ph_params[PAV].tspesh + (8 - tcum)));   /* voicing onset ramp */
            }
        }
    }
}

/* ======================================================================================================
 * pht0draw 0xc722 (dapi ph_drwt01.c): F0 for this frame from the command list (hat pattern steps, impulses,
 * sung notes), the segmental term and the baseline fall; then T0 and the spectral tilt.
 * ====================================================================================================== */
void pht0draw(void)
{
    int16_t f0out = 0, d;

    if (nf0ev < -1) {                           /* hard init */
        f0basestart = f0basefall;
        f0endfall = ef_x10;
        nframb = 0;
        tglstp = -200;
        f0las1 = (int16_t)(f0basefall << 3);
        f0las2 = (int16_t)(f0basefall << 3);
        f0 = f0basefall;
        newnote = f0basefall;
        delnote = 0;
        tarhat = 0;
        tarimp = 0;
        vibcount = 3;
        f0a2 = 0x600;
        f0b = 0x3a00;
        f0a1 = 0x3000;
        nf0ev = -1;
    }
    if (nf0ev < 0) {                            /* new clause */
        dtimf0 = f0tim[0];
        np_drawt0 = 0;
        npg = 0;
        nf0ev = 0;
        nfram = f0mode == 0 ? 4 : 0;
        nframs = 8;
        nframg = 0;
        extrad = 0;
        segdur = 0;
        segdrg = 0;
    }

    while (dtimf0 <= nfram && nf0ev < nf0tot) {  /* F0 commands due now */
        f0command = f0tar[nf0ev];
        nfram = (int16_t)(nfram - dtimf0);
        nf0ev++;
        dtimf0 = f0tim[nf0ev];                  /* f0tim[50] is f0tar[0], as in the ROM */
        if (f0command == 0) {
            nframb = 0;
            tarhat = 0;
        } else if (f0command < 2000) {
            if ((f0command & 1) == 0) {         /* step: hat rise or fall */
                tarhat = (int16_t)(tarhat + f0command);
                if (f0command >= 0) { if (tarimp < 0) tarimp = 0; }
                else if (tarimp > 0) tarimp = 0;
            } else {                            /* impulse (stress), 16 frames */
                tarimp = (int16_t)(f0command * 2);
                delimp = 16;
            }
        } else {                                /* user target: note (< 50) or F0 in Hz */
            f0command = (int16_t)(f0command - 2000);
            if (f0command < 50) {
                if (f0command > 37) {
                    dt_error_flags |= 8;
                    log_error("Sung note %d > %d", f0command, 37);
                    log_error("If intended to be an F0 target, < %d", 50, 0);
                    f0command = 37;
                }
                newnote = notetab[f0command - 1];
                vibsw = 1;
                delnote = (int16_t)((newnote - f0) >> 4);
            } else if (f0command < 512) {
                newnote = (int16_t)(f0command * 10);
                vibsw = 0;
                delnote = (int16_t)long_divide(newnote - f0, durfon);
            } else {
                dt_error_flags |= 8;
                log_error("F0 > %d", 512, 0);
            }
            if (f0 < newnote) delnote++;
            if (newnote < f0) delnote--;
        }
    }

    f0baseline = (int16_t)(f0basestart - nframb);  /* baseline falls 1 per frame down to f0endfall */
    if (f0endfall < f0baseline) nframb++;

    if (extrad + segdur <= nframs && np_drawt0 < nallotot) {   /* next segment for the segmental F0 term */
        nframs = (int16_t)(nframs - segdur);
        np_drawt0++;
        segdur = allodurs[np_drawt0];
        extrad = 0;
        phocur_t0 = allophons[np_drawt0];
        if (np_drawt0 < nallotot) phonex_t0 = allophons[np_drawt0 + 1];
        f0seg_raw = f0segtars[phocur_t0];
        if ((featb[phonex_t0] & 2) == 0 && (featb[phonex_t0] & 0x2000) == 0) extrad = 4;
        if ((featb[phocur_t0] & 0x2000) == 0 && (featb[phocur_t0] & 0x40) && (featb[phocur_t0] & 2) == 0 &&
            (allofeats[np_drawt0] & 6))
            extrad = 0xb;
        if (malfem == 0) extrad = 0;
        if ((allofeats[np_drawt0] & 6) == 0) f0seg_raw = (int16_t)(f0seg_raw >> 2);
        f0seg = f0seg_raw;
    }

    if (nframg < segdrg) {                      /* glottal stop gesture timing */
        if (nframg == 6) tglstp = tglstp_next;
    } else {
        int16_t nx;
        nframg = (int16_t)(nframg - segdrg);
        npg++;
        segdrg = allodurs[npg];
        if (tglstp == 0) tglstp = -200;
        if (tglstp > 0) tglstp = 0;
        tglstp_next = -200;
        nx = allophons[npg + 1];
        if ((featb[nx] & 4) && (allofeats[npg + 1] & 6) && (allofeats[npg + 1] & 0x10) == 0 &&
            (allofeats[npg] & 0x380) && (featb[allophons[npg]] & 0x40) == 0 && nx != 0x10)
            tglstp_next = segdrg;
        if (featb[nx] & 0x2000) tglstp_next = segdrg;
        if (featb[allophons[npg]] & 0x2000) tglstp_next = segdrg;
    }

    if (f0mode == 0) {                          /* rule F0: two-pole smoothing of the summed commands */
        f0in = (int16_t)(tarimp + f0seg + tarhat + f0baseline);
        f0las1 = (int16_t)(q14(f0a1, f0in) + q14(f0b, f0las1));
        f0las2 = (int16_t)(q14(f0a2, f0las1) + q14(f0b, f0las2));
        f0 = (int16_t)(f0las2 >> 3);
        delimp--;
        if (delimp < 1) tarimp = 0;
    } else if (f0mode == 1) {                   /* sung notes / user targets: glide to newnote */
        f0 = (int16_t)(f0 + delnote);
        if (delnote < 0) { if (f0 < newnote) f0 = newnote; }
        else if (newnote < f0) f0 = newnote;
        if (vibsw == 1) {
            vibcum = (int16_t)(vibcum + sung_vibrato_steps[vibstep]);
            f0out = (int16_t)(vibcum + f0);
            if (++vibcount > 5) {
                vibcount = 0;
                if (++vibstep > 3) vibstep = 0;
            }
        } else {
            f0out = f0;
        }
    } else {
        f0out = f0;   /* other modes: the ROM uses an uninitialised local here */
    }

    parstochip[OUT_TLT] = q12((0x4b0 - f0) >> 3, f0_dep_tilt);
    if (parstochip[OUT_TLT] < 0) parstochip[OUT_TLT] = 0;
    parstochip[OUT_TLT] = (int16_t)(parstochip[OUT_TLT] + spdeftltoff);
    if (featb[phcur] & 0x20) parstochip[OUT_TLT] = (int16_t)(parstochip[OUT_TLT] + 18);
    if (parstochip[OUT_TLT] > 28) parstochip[OUT_TLT] = 28;

    if (f0mode == 0) {                          /* dip around a glottal stop */
        d = (int16_t)(nframg - tglstp);
        if (d < 0) d = (int16_t)-d;
        if (d < 6) f0 = (int16_t)(d * 100 - 600 + f0);
    }
    if (f0 > 0x1401) f0 = 0x1401;
    else if (f0 < 500) f0 = 500;

    if (f0mode == 0) {                          /* speaker: scale around 120 Hz, add the floor */
        f0_tenths = (int16_t)(q12(f0 - 0x4b0, spdef_packet[SP_F0SCALEFAC]) + spdef_packet[SP_F0MINIMUM]);
        if (f0_tenths > 0x1401) f0_tenths = 0x1401;
        else if (f0_tenths < 500) f0_tenths = 500;
    } else {
        f0_tenths = f0out;
    }
    parstochip[OUT_T0] = muldiv(400, 1000, f0_tenths);   /* 40000 / F0 */
    nfram++;
    nframs++;
    nframg++;
}

/* ======================================================================================================
 * set_formant_limits 0x7d42 and setspdef 0x81e6 (dapi ph_vset.c): voice record -> speaker state and packet.
 * ====================================================================================================== */
void set_formant_limits(void)
{
    f3max = f3max_by_sex[cur_voice.sex];
    f2max = f2max_by_sex[cur_voice.sex];
    if (cur_voice.f4 != 2500 && cur_voice.f4 < f3max + 300) {
        f3max = (int16_t)(cur_voice.f4 - 300);
        f2max = (int16_t)(cur_voice.f4 - 600);
    }
    if (cur_voice.f5 != 2500 && cur_voice.f5 < cur_voice.f4 + 250) cur_voice.f5 = (int16_t)(cur_voice.f4 + 250);
}

void setspdef(void)
{
    int16_t *p = spdef_packet;
    malfem = cur_voice.sex;
    spdeftltoff = (int16_t)long_divide((int16_t)(cur_voice.sm * 36), 100);
    f0_dep_tilt = (int16_t)(cur_voice.ft * 41);
    assertiveness = (int16_t)(cur_voice.as * 41);
    p[SP_HDR] = PH_SPEAKER_HDR;
    p[SP_FNSCALE] = (int16_t)((200 - cur_voice.hs) * 41);
    p[SP_F4] = cur_voice.f4 == 2500 ? 2500 : (int16_t)(((int32_t)cur_voice.f4 * p[SP_FNSCALE]) >> 12);
    p[SP_B4] = cur_voice.b4;
    if (p[SP_F4] > 0x1356) { p[SP_F4] = 2500; p[SP_B4] = 0x800; }
    p[SP_F5] = cur_voice.f5 == 2500 ? 2500 : (int16_t)(((int32_t)cur_voice.f5 * p[SP_FNSCALE]) >> 12);
    p[SP_B5] = cur_voice.b5;
    if (p[SP_F5] > 0x1356) { p[SP_F5] = 2500; p[SP_B5] = 0x800; }
    p[SP_P4] = cur_voice.p4;
    p[SP_P5] = cur_voice.p5;
    p[SP_F0MINIMUM] = (int16_t)(cur_voice.ap * 10);
    p[SP_T0JIT] = cur_voice.la;
    f0basefall = (int16_t)(cur_voice.bf * 10);
    ef_x10 = (int16_t)(cur_voice.ef * 10);
    p[SP_G1] = cur_voice.g1;
    p[SP_G2] = cur_voice.g2;
    p[SP_G3] = cur_voice.g3;
    p[SP_G4] = cur_voice.g4;
    p[SP_G5] = cur_voice.g5;
    p[SP_NOPEN1] = (int16_t)(cur_voice.ri * 160 + 4000);
    p[SP_NOPEN2] = (int16_t)(cur_voice.nf << 2);
    p[SP_ATURB] = (int16_t)(cur_voice.br + 9);
    p[SP_F0SCALEFAC] = (int16_t)(cur_voice.pr * 41);
    p[SP_GF] = cur_voice.gf;
    p[SP_GN] = cur_voice.gn;
    p[SP_GV] = cur_voice.gv;
    p[SP_GH] = cur_voice.gh;
    spdef_dirty = 1;
}
