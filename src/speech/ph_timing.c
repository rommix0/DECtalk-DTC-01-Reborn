/* phtiming 0x8ebe: segment durations (Klatt's duration rules, dapi p_us_tim.c) and, in the same routine, the
 * hat-pattern F0 commands (dapi ph_inton1.c phinton / make_f0_command). Rebuilt from the DTC-01 v1.8 ROM;
 * see REFERENCE.md s15.8 and s15.19. The ROM's rule set is older than dapi's, so the rules below follow
 * the 68000 code, not the dapi source; rule numbers are the ROM's prdurs() checkpoints.
 *
 * featb[] bits (dapi): 1 syllabic, 2 voiced, 4 vowel, 8 sonorant (FSON1), 0x10 sonorant (FSONOR), 0x20 obstruent,
 * 0x40 plosive, 0x80 nasal, 0x100 consonant, 0x200 sonorant consonant (w y r l), 0x800 burst.
 * allofeats[] bits (the ROM's layout): 7 stress (6 = emphasis), 0x18 syllable type (0 = monosyllable,
 * 0x10 = medial, 0x18 = final), 0x20 word-initial consonant, 0x3c0 boundary after (>= 0x100 phrase,
 * >= 0x180 clause), 0x180/0x200 comma/period ends, 0x400/0x800 hat rise/fall [I], 0x1000 do not count [I].
 */
#include "ph_frame.h"
#include "ph_rom.h"
#include "ph_math.h"

int16_t sprate = 180;                   /* 0x82296, .data: 180 words/min at power-up */
int16_t sprate_last, sprat1, sprat2, compause, perpause, f0_halfsteps, cumdur, emphasissw;
int32_t cumdur_long;
int16_t hatstate = 1, hatsize = 200;    /* 0x822a0, 0x822a2, .data */
int16_t hatfall;
int16_t tm_phocur, tm_prcnt, tm_durinh, tm_durmin, tm_deldur, tm_nphon, tm_dpause;

static void log_error(const char *fmt, int a, int b)
{
    if (ph_error_hook) ph_error_hook(fmt, a, b);
}

/* make_f0_command 0xa6e2 (ROM name kl3_push_event): append an F0 command 'delay' frames after the current
 * phone's start, as a time relative to the previous command. At most 50 commands; the 51st overwrites the last
 * slot's time (f0tim[50] = f0tar[0], as in the ROM). */
void make_f0_command(int cmd, int delay)
{
    if (cumdur + delay < 0) delay = -cumdur;
    f0tim[nf0tot] = (int16_t)(delay + cumdur);
    f0tar[nf0tot] = (int16_t)cmd;
    cumdur = (int16_t)-delay;
    if (nf0tot < PH_MAXEV) nf0tot++;
    else log_error("kl3.c: totev > MAXEV (%d)", PH_MAXEV, 0);
}

/* init_timing: speaking rate -> sprat1 (pause scale) and sprat2 (segment scale), Q14 */
static void init_timing(void)
{
    int16_t r;
    if (sprate == sprate_last) return;
    if (sprate < 120) sprate = 120;
    if (sprate > 350) sprate = 350;
    sprate_last = sprate;
    r = sprate < 251 ? sprate : (int16_t)(((sprate - 250) >> 1) + 250);
    sprat1 = r < 180 ? muldiv(0x4000, 300 - r, 120) : muldiv(0x4000, 400 - r, 220);
    sprat2 = r < 181 ? (int16_t)((sprat1 + 0x4000) >> 1) : muldiv(0x4000, 460 - r, 280);
}

void phtiming(void)
{
    int16_t fea_las, fea_cur, fea_nex, ph_las = 0, ph_nex = 0, nstress = 0, durxx;
    int16_t syll, stress, struc, sylltype, bound;
    int16_t factor;                             /* the ROM keeps this in muldiv_a */

    init_timing();
    nf0tot = 0;
    cumdur = 0;
    cumdur_long = 0;
    fea_cur = featb[0];
    tm_nphon = 0;

    while (tm_nphon++ < nallotot) {
        int n = tm_nphon;
        fea_las = fea_cur;
        if (n > 1) ph_las = allophons[n - 1];
        tm_phocur = allophons[n];
        if (n < nallotot) ph_nex = allophons[n + 1];   /* else: keeps the previous phone's value */
        fea_cur = featb[tm_phocur];
        syll = (int16_t)(fea_cur & 1);
        stress = (int16_t)(allofeats[n] & 7);
        struc = allofeats[n];
        sylltype = (int16_t)(allofeats[n] & 0x18);
        bound = (int16_t)(allofeats[n] & 0x3c0);
        fea_nex = featb[ph_nex];

        /* allophones of /t/ and /d/ (flaps, glottalized t) */
        if (phmode_8229c == 0 || phmode_8229c == 2) {
            if (ph_nex == 0x10 || ph_nex == 0x19) {         /* before yu / y: ch, jh */
                if (tm_phocur == 0x2f) tm_phocur = allophons[n] = (allofeats[n + 1] & 7) < 2 ? 0x36 : 0x34;
                if (tm_phocur == 0x30) tm_phocur = allophons[n] = 0x37;
            }
            if (tm_phocur == 0x2f && ((bound >= 0x40 && (fea_nex & 0x400) && (fea_las & 8)) || ph_nex == 0x24))
                tm_phocur = allophons[n] = 0x34;            /* tx */
            if ((tm_phocur == 0x30 || tm_phocur == 0x2f) && stress == 0 && (fea_las & 8) && ph_las != 0x1f &&
                ph_las != 0x23 && ph_las != 0x21 && (ph_las != 0x20 || tm_phocur != 0x30) && (fea_nex & 1)) {
                if (bound < 0x40) {
                    if (ph_nex == 0x11 || ph_nex == 0xf || ph_nex == 1 || ph_nex == 0x12 || ph_nex == 0x22 ||
                        (ph_nex == 0xb && ((allofeats[n + 2] & 0x20) || (featb[allophons[n + 2]] & 0x100) == 0)))
                        allophons[n] = 0x33;                /* dx: flap */
                } else {
                    allophons[n] = bound > 0x80 ? 0x34 : 0x33;
                }
                tm_phocur = allophons[n];
            }
        }

        if (allodurs[n] != 0) {
            durxx = allodurs[n];                /* user-specified duration */
            goto have_dur;
        }
        tm_durinh = inhdr[tm_phocur];
        tm_durmin = mindur[tm_phocur];
        tm_deldur = 0;
        tm_prcnt = 0x80;                        /* 128 = 100 % */

        if (tm_phocur == 0) {                   /* Rule 1: pauses */
            tm_dpause = 10;
            if (n > 1) {
                if (allofeats[n] & 0x180) tm_dpause = (int16_t)(compause + 0x10);
                if (allofeats[n] & 0x200) tm_dpause = (int16_t)(perpause + 0x4b);
            }
            tm_dpause = q14(tm_dpause, sprat1);
            if (tm_dpause < 10) tm_dpause = 10;
            durxx = tm_durinh = tm_durmin = tm_dpause;
            goto have_dur;
        }

        if (bound > 0x17f) {                    /* Rule 2: clause-final lengthening */
            tm_deldur = 6;
            if ((fea_cur & 2) && (fea_cur & 0x20) && (fea_cur & 0x40) == 0) tm_deldur = 4;
            if (tm_phocur == 0x2f || tm_phocur == 0x30) tm_deldur = 4;
            if (nstress == 0 && syll && (allofeats[n] & 0x400)) {
                tm_prcnt = 0xa6;
                tm_deldur = (int16_t)(tm_deldur + 6);
            }
            if (fea_nex & 8) tm_deldur = (int16_t)(tm_deldur - 3);
        }
        if (!syll) {                            /* Rule 3: non-phrase-final segments */
            if ((fea_cur & 8) && stress == 0 && bound > 0xff) tm_deldur = (int16_t)(tm_deldur + 3);
        } else if (bound < 0x100) {
            tm_prcnt = q14(tm_prcnt, 0x2667);
        }
        if (syll) {                             /* Rules 4-5: position in the word */
            if (sylltype == 0 && stress < 2) tm_prcnt = q14(0x2ccd, tm_prcnt);
            else if (sylltype < 9) tm_prcnt = q14(0x2ccd, tm_prcnt);
            else if (bound < 0x80) tm_prcnt = q14(0x3667, tm_prcnt);
            if (sylltype != 0) tm_prcnt = q14(tm_prcnt, 0x3334);
        }
        if (!syll && (struc & 0x20) != 0x20)    /* Rule 6: non-word-initial consonants */
            tm_prcnt = q14(tm_prcnt, 0x3667);

        if (stress < 2) {                       /* Rule 7: unstressed segments */
            if (tm_durmin < tm_durinh) tm_durmin = (int16_t)(tm_durmin >> 1);
            if (!syll) {
                if (tm_phocur > 0x17 && tm_phocur < 0x1c) tm_prcnt = (int16_t)(tm_prcnt >> 1);   /* w y r l */
                else tm_prcnt = q14(tm_prcnt, 0x2ccd);
            } else if (sylltype == 0x10) {
                tm_prcnt = (int16_t)(tm_prcnt >> 1);
            } else {
                tm_prcnt = q14(tm_prcnt, 0x2ccd);
                if (ph_nex == 0x1c && tm_durinh < 0x14) tm_deldur = (int16_t)(tm_deldur + 6);
            }
        } else if (syll && bound < 0x100 && (allofeats[n] & 0x400)) {
            tm_deldur = (int16_t)(tm_deldur + ((allofeats[n] & 0xc00) ? 8 : 4));
        }

        if ((struc & 0x20) == 0x20 || (syll && stress != 6)) emphasissw = 0;   /* Rule 8: emphasis */
        if (stress == 6) emphasissw = 1;
        if (emphasissw == 1) tm_deldur = (int16_t)(tm_deldur + (syll ? 0x12 : 5));

        {                                       /* Rule 9: the consonant after a vowel */
            int16_t posvoc = 0, psonsw = 0;
            factor = 0x4000;
            if (syll || (tm_phocur > 0x1c && tm_phocur < 0x22 && stress == 0 && (fea_nex & 0x20))) {
                if ((allofeats[n + 1] & 7) == 0) {
                    posvoc = ph_nex;
                    if (ph_nex > 0x1c && ph_nex < 0x22 && (featb[allophons[n + 2]] & 0x20) &&
                        (allofeats[n + 2] & 7) == 0) {
                        psonsw = 1;
                        posvoc = allophons[n + 2];
                    }
                    if (tm_phocur > 0x1c && tm_phocur < 0x22) tm_prcnt = (int16_t)((tm_prcnt >> 2) + 0x60);
                    factor = 0x4000;
                    if (posvoc != 0) {
                        if ((featb[posvoc] & 2) == 0) {        /* voiceless */
                            tm_deldur = (int16_t)(tm_deldur - (tm_deldur >> 2));
                            factor = (featb[posvoc] & 0x40) ? 0x2ccd : 0x3667;
                        } else {
                            if (featb[posvoc] & 0x20) factor = 0x4ccd;
                            if ((featb[posvoc] & 0x40) == 0) tm_deldur = (int16_t)(tm_deldur + 4);
                            if (featb[posvoc] & 0x80) factor = 0x3667;
                        }
                    }
                }
                if (bound < 0x100 || psonsw == 1) factor = (int16_t)((factor >> 1) + 0x2000);
                tm_prcnt = q14(factor, tm_prcnt);
            }
        }

        if (syll) {                             /* Rule 10: clusters */
            if (fea_nex & 1) tm_deldur = (int16_t)(tm_deldur + 6);
        } else if (fea_cur & 0x100) {
            if ((fea_nex & 0x100) && bound < 0x100) {
                factor = 0x2ccd;
                if ((fea_cur & 0x80) && (allofeats[n + 1] & 0x20)) factor = 0x6000;
                if (tm_phocur == 0x29 || tm_phocur == 0x27) {
                    if (fea_nex & 0x40) factor = 0x2000;
                    if (ph_nex == 0x2b) {       /* s or th before sh: 2 frames, no more rules */
                        durxx = 2;
                        goto have_dur;
                    }
                }
                tm_prcnt = q14(factor, tm_prcnt);
            }
            if ((fea_las & 0x100) && (allofeats[n - 1] & 0x3c0) < 0x100) {
                factor = 0x2ccd;
                if (fea_cur & 0x40) {
                    if (ph_las == 0x29) factor = 0x2667;
                    if ((fea_las & 0x80) && stress < 2) factor = 0x666;
                    if (fea_las & 0x40) factor = 0x4000;
                }
                tm_prcnt = q14(factor, tm_prcnt);
            }
        }

        /* the duration: min + prcnt/128 of the compressible part, scaled by the rate */
        durxx = q14((int16_t)(((int32_t)tm_prcnt * (tm_durinh - tm_durmin)) >> 7) + tm_durmin, sprat2);
        if (durxx == 0) durxx = 1;
        if ((fea_cur & 0x20) == 0 && ph_las == 0) tm_deldur = (int16_t)(tm_deldur + 5);   /* Rule 11 */
        if (fea_cur & 4) {
            if ((fea_las & 0x400) && (fea_las & 0x80) == 0) tm_deldur = (int16_t)(tm_deldur + 5);
            if ((fea_nex & 0x400) && (fea_nex & 0x80) == 0) tm_deldur = (int16_t)(tm_deldur + 5);
        }
        if ((fea_cur & 8) && (fea_las & 2) == 0 && (fea_las & 0x40)) tm_deldur = (int16_t)(tm_deldur + 3);
        tm_deldur = q14(tm_deldur, sprat2);
        durxx = (int16_t)(tm_deldur + durxx);
        if (durxx > tm_durinh * 2 - 1) durxx = (int16_t)(tm_durinh * 2 - 1);

    have_dur:
        if (durxx < 1) {
            log_error("bug: phoneme %d, durxx (%d) <= 0", tm_phocur, durxx);
            durxx = 1;
        }
        allodurs[n] = durxx;

        if (f0mode == 0) {                      /* hat-pattern F0 commands (dapi phinton) */
            if (stress > 1 && syll) {
                if (stress > 2 && hatstate == 1) {          /* new rise after a fall */
                    make_f0_command(hatsize, (allofeats[n] & 0x400) ? -6 : 0);
                    hatsize = 200;
                    hatstate = 2;
                }
                if (nstress < 4) nstress++;
                {
                    int16_t cmd = (int16_t)(f0_stress_steps[nstress] + f0_stress_steps[5 + stress]);
                    int16_t delay = (allofeats[n] & 0x400) ? -6 : 4;
                    if (f0_halfsteps == 1 && stress < 6) cmd = (int16_t)((cmd >> 1) | 1);
                    if (stress == 6) delay = 0;
                    make_f0_command(cmd, delay);
                }
                if ((allofeats[n] & 0x400) && hatstate == 2) {  /* hat fall */
                    int16_t delay;
                    hatfall = 0x138;
                    delay = (int16_t)((int16_t)(allodurs[n] - 0x19) >> 1);
                    if (delay < 4) delay = 4;
                    if (bound < 0x100) {
                        delay = (int16_t)(allodurs[n] - 8);
                        hatfall = (allofeats[n] & 0xc00) ? 100 : 0x70;
                    } else if (allofeats[n] & 0x100) {
                        hatfall = 0x78;
                    }
                    hatfall = (int16_t)(q12(hatfall, assertiveness) & 0x7ffe);
                    hatsize = (int16_t)(hatfall + hatsize);
                    make_f0_command(-hatsize, delay);
                    if ((allofeats[n] & 0x180) == 0x180 && (allofeats[n] & 0xc00)) {
                        int16_t d = (int16_t)(allodurs[n] - 0xd);
                        delay = (int16_t)((int16_t)(d * 2 + d + 0x19) >> 2);
                        if (delay < allodurs[n] - 0x10) delay = (int16_t)(allodurs[n] - 0x10);
                        make_f0_command((allofeats[n] & 0x200) ? 0xfb : 0x33, delay);
                        make_f0_command(hatfall, delay);
                        hatsize = 200;
                    }
                    hatstate = 1;
                }
            }
            if (stress < 2 && syll && sylltype == 0x18) {  /* unstressed final syllable */
                if ((allofeats[n] & 0x380) == 0x200)
                    make_f0_command((int16_t)(q12(-0xab, assertiveness) | 1), 8);
                if ((allofeats[n] & 0x180) == 0x180)
                    make_f0_command((allofeats[n] & 0x200) ? 0xfb : 0x33, (int16_t)(allodurs[n] - 0xd));
            }
            if (tm_phocur == 0) {               /* pause: reset the hat */
                if (hatsize > 200 && nf0tot > 0) {
                    make_f0_command(hatfall, 0);
                    hatsize = 200;
                }
                if (n != 1) nstress = 1;
                if (allofeats[n] & 0x200) {
                    make_f0_command(0, 0);
                    nstress = 0;
                }
            }
        } else if (f0tar[n] > 0) {              /* user F0: sung note or target for this phone */
            make_f0_command(f0tar[n] + 2000, 0);
            if (n < nf0tot) {
                dt_error_flags |= 8;
                log_error("Too many sung notes", 0, 0);
            }
        }
        cumdur = (int16_t)(allodurs[n] + cumdur);
    }

    /* a clause-final stop gets a short schwa release */
    tm_phocur = allophons[nallotot - 1];
    if (tm_phocur >= 0x2d && tm_phocur <= 0x32) {
        allophons[nallotot + 1] = allophons[nallotot];
        allofeats[nallotot + 1] = allofeats[nallotot];
        allodurs[nallotot + 1] = allodurs[nallotot];
        allophons[nallotot] = 0x11;
        if (allophons[nallotot - 2] < 6 || tm_phocur == 0x2f || tm_phocur == 0x30) allophons[nallotot] = 0x12;
        allodurs[nallotot] = 4;
        allofeats[nallotot] = 0x1000;
        nallotot++;
    }
}
