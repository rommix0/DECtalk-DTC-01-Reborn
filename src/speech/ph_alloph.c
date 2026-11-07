/* phalloph 0x843e: the clause's phoneme stream (phonemes plus stress and boundary symbols) -> allophones with
 * their structure bits (allofeats), the step before phtiming. Rebuilt from the DTC-01 v1.8 ROM; see
 * REFERENCE.md s15.20. dapi's ph_aloph.c is a much later rewrite (it gets stress and boundaries from a
 * separate sentstruc[] array), so this follows the 68000 code.
 *
 * Input codes (phoneme name table 0x19704): 0-55 phonemes; 56-60 stress symbols (57 ' primary -> stress 5,
 * 58 ` secondary -> 2, 59 " emphatic -> 6; 56 and 60 also give 5 here, although the table prints them as # and -);
 * 61-69 boundaries * space ( ) [ , ! ? . (65 [ = CLSTART, 66 , = the CLEND klclause appends); 0x66/0x67 carry
 * an index marker in the next word. User durations and F0 targets for input phone n arrive in allodurs[n + 5]
 * and f0tar[n + 5] (parse_phoneme_param_stream) and are moved to the output slot here.
 *
 * allofeats[] as written here: 7 stress (1 unstressed, 2 secondary, 5 primary, 6 emphatic), 0x18 syllable
 * position in a word of 2+ syllables (8 first, 0x10 middle, 0x18 last), 0x20 consonant before the word's first
 * vowel, 0x3c0 strength of the boundary that follows (bndval[] below), 0x400 hat rise on this syllable, 0x800
 * hat fall (sentence end), and an inserted silence carries only its boundary value.
 * Firmware v2.0's phalloph is its own function below (phalloph_v20).
 */
#include "ph_frame.h"
#include "ph_rom.h"

int16_t phonemes[PH_MAXALLO_V20];
int16_t nphonemes;

enum {
    PH_SIL = 0, PH_IY = 1, PH_IH = 2, PH_EY = 3, PH_EH = 4, PH_AE = 5, PH_AA = 6, PH_AH = 9, PH_AO = 10,
    PH_OW = 11, PH_UH = 13, PH_UW = 14, PH_RR = 15, PH_AX = 17, PH_IR = 19, PH_ER = 20, PH_AR = 21, PH_OR = 22,
    PH_UR = 23, PH_R = 26, PH_L = 27, PH_RX = 29, PH_LX = 30, PH_DH = 40, PH_S = 41,
    SYM_FIRST = 56, SYM_PRIMARY = 57, SYM_SECONDARY = 58, SYM_EMPHATIC = 59,
    BND_MORPH = 61, BND_PP = 63, BND_VP = 64, BND_CLSTART = 65, BND_COMMA = 66, BND_EXCLAIM = 67,
    BND_QUEST = 68, BND_PERIOD = 69,
    SYM_INDEX1 = 0x66, SYM_INDEX2 = 0x67
};

/* The ROM indexes its arrays without bounds checks. For clauses of more than about 195 symbols the indexes run
 * past 199 into the next RAM variable (allophons -> allodurs -> allofeats -> index_marks -> nallotot ->
 * f0tim/f0tar -> nf0tot, f0_halfsteps, perpause, compause, tm_dpause). ram16() reproduces that mapping for the
 * word variables; the halves of index_marks[] and anything else land in a dummy word. */
static int16_t *ram16(long addr)
{
    static int16_t dummy;
    if (addr >= 0x80f82 && addr < 0x81112) return &allophons[(addr - 0x80f82) / 2];
    if (addr >= 0x81112 && addr < 0x812a2) return &allodurs[(addr - 0x81112) / 2];
    if (addr >= 0x812a2 && addr < 0x81432) return &allofeats[(addr - 0x812a2) / 2];
    if (addr == 0x81752) return &nallotot;
    if (addr >= 0x81754 && addr < 0x81948) return &f0cmd_area[(addr - 0x81754) / 2];
    if (addr == 0x81948) return &nf0tot;
    if (addr == 0x8194a) return &f0_halfsteps;
    if (addr == 0x8194c) return &perpause;
    if (addr == 0x8194e) return &compause;
    if (addr == 0x81950) return &tm_dpause;
    dummy = 0;
    return &dummy;
}
#define APH(i) (*((i) >= 0 && (i) < PH_MAXALLO ? &allophons[i] : ram16(0x80f82L + 2L * (i))))
#define ADUR(i) (*((i) >= 0 && (i) < PH_MAXALLO ? &allodurs[i] : ram16(0x81112L + 2L * (i))))
#define AFEA(i) (*((i) >= 0 && (i) < PH_MAXALLO ? &allofeats[i] : ram16(0x812a2L + 2L * (i))))
#define UF0(i) (*((i) >= 0 && (i) < PH_MAXALLO ? &f0tar[i] : ram16(0x817b8L + 2L * (i))))
/* phonemes[200] is nphonemes (0x81f08) */
#define PHON(i) ((i) < PH_MAXALLO ? phonemes[i] : nphonemes)

int ph_fixes;
ph_extra_mark_t ph_extra_marks[PH_EXTRA_MARKS];
int ph_nextra_marks;
ph_side_mark_t ph_side[PH_SIDE_MAX], ph_cmarks[PH_SIDE_MAX];
int ph_nside, ph_ncmarks;

/* PH_FIX_MARKS: marks go with the next allophone stored, the first phone of their word (or the pause there), and
 * are reported when it starts. The ROM's marker goes with the last allophone stored before it (below): the last
 * phone of the word before, one phone early. */
static uint32_t pending[PH_SIDE_MAX];
static int npending;

static void pend_mark(uint32_t m)
{
    if (npending < PH_SIDE_MAX) pending[npending++] = m;
}

static void place_marks(int16_t nallo)
{
    int16_t at = nallo ? nallo : 1;
    int i;
    for (i = 0; i < npending && at < PH_MAXALLO; i++) {
        if (!index_marks[at]) {
            index_marks[at] = pending[i];
        } else if (ph_nextra_marks < PH_EXTRA_MARKS) {  /* a second mark for this phone: kept too */
            ph_extra_marks[ph_nextra_marks].at = at;
            ph_extra_marks[ph_nextra_marks++].mark = pending[i];
        }
    }
    npending = 0;
}

/* PH_FIX_MARKS: the next input symbol after n that is not an index marker (with its value word) */
static int16_t next_symbol(int16_t n)
{
    int16_t k = (int16_t)(n + 1);
    while (k < nphonemes && (PHON(k) == SYM_INDEX1 || PHON(k) == SYM_INDEX2)) k = (int16_t)(k + 2);
    return PHON(k);
}


/* ======================================================================================================
 * Firmware v2.0's phalloph (0x8966) and its helpers (0x8a72-0x9de8), REFERENCE.md s18.11. v2.0 renumbered the
 * symbols from 59 up (s18.3): 56 #, 57 ' primary, 58 ` secondary, 59 / 60 \ 61 /\ (pitch rise, fall, rise-fall),
 * 62 " emphatic, 63 -, 64-72 boundaries * space ( ) [ , ! ? ., 73 + (the silent phoneme), 0x57/0x58 index
 * markers. The state is kept as the ROM keeps it, in RAM words (v2.0 address in the comments), because the helpers
 * share it and some quirks depend on it: a stress symbol that comes before consonants is moved in phonemes[] to
 * just before its vowel and the loop backs up one symbol to read it again, and the feature history then repeats a
 * symbol.
 * ====================================================================================================== */
int16_t al_cur, al_hat_phase, user_hat;  /* 0x820fc (persists: the last clause's last symbol), 0x820d6, 0x80f6e */
static int16_t v_pound;                 /* 0x820d2: 0x4000 after # until a word ends (then stress symbols are ignored) */
static int16_t v_sylfeat, v_stress, v_nsyl; /* 0x820d8, 0x820da, 0x820dc: as v1.8's sylfeat, stress, nsyl */
static int16_t v_bndstart, v_lastbnd;   /* 0x820e2 (last syllabic), 0x820e4 */
static int16_t v_fall, v_rise, v_prevfall; /* 0x820e6 (hat fall position), 0x820e8 (rise), 0x820ea */
static int16_t v_hatprio, v_nstress;    /* 0x820ec, 0x820ee */
static int16_t v_nallo, v_n, v_prev;    /* 0x820f2, 0x820f4, 0x820fe */
static int16_t v_fea, v_fea_las, v_fea_las2, v_bnd;   /* 0x820f6, 0x820f8, 0x820fa, 0x82102 */

/* v2.0's arrays, with the ROM's reads and writes past their ends (into the next RAM variable), as ram16() above */
static int16_t *ram20(long addr)
{
    static int16_t dummy;
    if (addr >= 0x80f84 && addr < 0x81178) return &allophons[(addr - 0x80f84) / 2];
    if (addr >= 0x81178 && addr < 0x81376) return &allodurs[(addr - 0x81178) / 2];   /* 255 words */
    if (addr >= 0x81376 && addr < 0x8156a) return &allofeats[(addr - 0x81376) / 2];
    if (addr == 0x81952) return &nallotot;
    if (addr >= 0x81954 && addr < 0x81d46) return &f0cmd_area[(addr - 0x81954) / 2];
    if (addr == 0x81d46) return &nf0tot;
    if (addr == 0x81d48) return &f0_halfsteps;
    if (addr == 0x81d4a) return &perpause;
    if (addr == 0x81d4c) return &compause;
    if (addr == 0x81d4e) return &tm_dpause;
    dummy = 0;
    return &dummy;
}
#define APH20(i) (*ram20(0x80f84L + 2L * (i)))
#define ADUR20(i) (*ram20(0x81178L + 2L * (i)))
#define AFEA20(i) (*ram20(0x81376L + 2L * (i)))
#define UF020(i) (*ram20(0x81b48L + 2L * (i)))

/* phonemes[250] is nphonemes; phonemes[-1] (a stress symbol first in the clause) reads RAM the C does not have */
static int16_t *phon20(int i)
{
    static int16_t outside;
    if (i < 0) { outside = 0x7fff; return &outside; }
    return i < PH_MAXALLO_V20 ? &phonemes[i] : &nphonemes;
}

/* bndval by boundary code - 0x40: '+' (9) reads the word after the table, f0_stress_steps[0]; a '(' turned into a
   stress symbol (below) reads 7 words before it, string bytes in the ROM */
static int16_t bndval_v20(int i)
{
    if (i == 9) return f0_stress_steps[0];
    if (i == -7) return 0x202e;
    return bndval[i];
}

/* 0x9560: move the stress symbol al_cur, which stands before consonants, to the vowel of its word. Returns 1 when
   it did (the caller then stops; the symbol is read again), 0 when the word has no vowel. Another stress symbol in
   the word (a primary one, unless this one is secondary) is overwritten instead. */
static int move_stress_v20(int16_t p)
{
    int16_t k, c, s, t;
    while (p < nphonemes - 1 && *phon20(p) > 0x3f) p++;
    if (v_stress != 2)
        for (k = p; *phon20(k) < 0x40; k++)
            if (*phon20(k) == 0x39) { *phon20(k) = al_cur; return 1; }
    for (k = p; *phon20(k) <= 0x3f; k++)
        if (*phon20(k) == 0x3a) { *phon20(k) = al_cur; return 1; }
    for (k = p;; k++) {
        s = *phon20(k);
        if (s > 0x3f) return 0;
        if (featb[s] & 1) break;
    }
    if (s == 0x11 || s == 0x12)                 /* ax ix: the word's last vowel instead */
        for (c = (int16_t)(k + 1); *phon20(c) <= 0x3f; c++)
            if (featb[*phon20(c)] & 1) k = c;
    while (--k >= v_n) {                        /* rotate: the symbol goes just before the vowel */
        t = *phon20(k);
        *phon20(k) = al_cur;
        al_cur = t;
    }
    v_n--;
    v_fea = featb[*phon20(v_n)];
    al_cur = *phon20(v_n);
    return 1;
}

/* 0x8b16: "the" before a vowel, and postvocalic l, r and r-coloured vowels, as v1.8. -1: r merged into the vowel */
static int rcolour_v20(void)
{
    if (phmode_8229c < 4 && (v_fea & 1) && APH20(v_nallo) == PH_AX && APH20(v_nallo - 1) == PH_DH)
        APH20(v_nallo) = PH_IY;
    if (phmode_8229c < 4 && !(featb[*phon20(v_n + 1)] & 0x1000) && (v_fea_las & 4)) {
        int16_t rv = 0;
        if (al_cur == PH_L) al_cur = PH_LX;
        if (al_cur == PH_R) al_cur = PH_RX;
        if (al_cur == PH_RX) {
            if (v_prev == PH_AX) rv = PH_RR;
            else if (v_prev == PH_IY || v_prev == PH_IH) rv = PH_IR;
            else if (v_prev == PH_EY || v_prev == PH_EH || v_prev == PH_AE) rv = PH_ER;
            else if (v_prev == PH_AA || v_prev == PH_AH) rv = PH_AR;
            else if (v_prev == PH_OW || v_prev == PH_AO) rv = PH_OR;
            else if (v_prev == PH_UW || v_prev == PH_UH) rv = PH_UR;
            else if (phmode_8229c == 1) APH20(v_nallo) = PH_AX;
        }
        if (rv) { APH20(v_nallo) = rv; return -1; }
    }
    return 0;
}

/* 0x8d50: store a phone */
static void store_phone_v20(void)
{
    if (v_nallo < PH_MAXALLO_V20 - 1) v_nallo++;
    APH20(v_nallo) = al_cur;
    ADUR20(v_nallo) = ADUR20(v_n + 5);
    if (v_nallo != v_n + 5) ADUR20(v_n + 5) = 0;
    UF020(v_nallo) = UF020(v_n + 5);
    if (v_nallo != v_n + 5) UF020(v_n + 5) = 0;
    if (v_fea & 1) v_sylfeat = 0;
    AFEA20(v_nallo) = (int16_t)(v_pound + v_sylfeat + v_stress);
    if (v_rise == v_nallo) AFEA20(v_rise) |= 0x400;
    if (user_hat && v_fall == v_nallo) AFEA20(v_fall) |= 0x800;
    if (v_fea & 1) {
        if (v_hatprio < 2 && !user_hat) { v_fall = v_nallo; v_hatprio = 1; }
        v_stress = 0;
        v_nsyl++;
        v_bndstart = v_nallo;
        v_bnd = 0;
        v_lastbnd = 0;
    }
}

/* the symbol after this one is not syllabic: move this stress symbol (1: moved, stop here) */
static int not_before_vowel(void)
{
    return !(featb[*phon20(v_n + 1)] & 1);
}

/* 0x8f96: [+]; the stress or / that may start a hat rise */
static int hat_rise_v20(void)
{
    if (al_cur == 0x49) f0_raise = 1;
    if (al_hat_phase == 1 && (((al_cur == 0x39 || al_cur == 0x3e) && !user_hat) ||
                              (user_hat && (al_cur == 0x3b || al_cur == 0x3d)))) {
        if (not_before_vowel()) {
            if (move_stress_v20((int16_t)(v_n + 1)) == 1) return 0;
        } else {
            v_rise = (int16_t)(v_nallo + 1);
            al_hat_phase = 2;
        }
    }
    return 1;
}

/* 0x904c: with the user's pitch symbols, \ or /\ after a rise places the fall */
static int hat_fall_v20(void)
{
    if (user_hat && (al_cur == 0x3c || al_cur == 0x3d) && al_hat_phase == 2) {
        if (not_before_vowel()) {
            if (move_stress_v20((int16_t)(v_n + 1)) == 1) return 0;
        } else {
            v_fall = (int16_t)(v_nallo + 1);
            al_hat_phase = 1;
        }
    }
    return 1;
}

/* 0x90cc: stress symbols */
static int stress_v20(void)
{
    if (al_cur == 0x3e || (al_cur >= 0x39 && al_cur <= 0x3d && v_pound != 0x4000)) {
        if (al_cur == 0x3e) { v_stress = 6; f0_halfsteps = 1; }
        else if (al_cur == 0x3a) v_stress = 2;
        else v_stress = 5;
        if (not_before_vowel()) {
            v_stress = 0;
            move_stress_v20((int16_t)(v_n + 1));
            return 0;
        }
        if (al_cur != 0x3a) v_nstress++;
        if (!user_hat) {
            if (v_stress == 2) {
                if (v_hatprio < 3) { v_fall = (int16_t)(v_nallo + 1); v_hatprio = 2; }
            } else {
                v_fall = (int16_t)(v_nallo + 1);
                v_hatprio = 5;
            }
        }
        /* the stress goes to the onset consonants too, as v1.8 (keeping the bits above 7) */
        if (*phon20(v_n - 1) < NPH && !(v_fea_las & 1)) {
            AFEA20(v_nallo) = (int16_t)(v_stress + (AFEA20(v_nallo) & 0xfff8));
            if (*phon20(v_n - 2) < NPH && (v_fea_las2 & 0x20)) {
                AFEA20(v_nallo - 1) = (int16_t)(v_stress + (AFEA20(v_nallo - 1) & 0xfff8));
                if (*phon20(v_n - 3) == PH_S) AFEA20(v_nallo - 2) = (int16_t)(v_stress + (AFEA20(v_nallo - 2) & 0xfff8));
            }
        }
    }
    return 1;
}

/* 0x94d2: boundaries 64-73 */
static int boundary_v20(void)
{
    int16_t k, s, i, mark;
    if (al_cur > 0x49 || al_cur < 0x40) return 1;
    if (al_cur == 0x42) {                       /* ( before , ! ? or . : a primary stress on the next word */
        k = v_n;
        do s = *phon20(++k); while (s < 0x41);
        if (s > 0x44 && s < 0x49) {
            al_cur = 0x39;
            if (move_stress_v20(v_n) == 1) return 0;
        }
    }
    if (al_cur == 0x42 || al_cur == 0x43) {    /* phrase boundaries by speaking rate */
        if (sprate < 0x79) al_cur = 0x45;
        else if (sprate < 0x8d) al_cur = 0x43;
        v_fea = featb[al_cur];
    }
    v_stress = 1;
    if (al_cur != 0x40) {                       /* a word ends: mark syllable positions */
        if (*phon20(v_n - 1) != 0x38) v_pound = 0;
        v_sylfeat = 0x20;
        if (v_nsyl > 1) {
            mark = 0x18;
            for (i = v_nallo; i >= 0; i--) {
                if (!(featb[APH20(i)] & 1)) continue;
                AFEA20(i) |= mark;
                if (mark == 8) break;
                mark = 0x10;
                if (--v_nsyl < 2) mark = 8;
            }
        }
        v_nsyl = 0;
    }
    v_bnd = bndval_v20(al_cur - 0x40);
    if (v_lastbnd < v_bnd)                      /* back to the last syllabic */
        for (i = v_nallo; i >= v_bndstart; i--) AFEA20(i) = (int16_t)(v_bnd | (AFEA20(i) & 0xfc3f));
    if (al_cur > 0x43 || (v_nstress > 1 && al_cur == 0x43) || (v_nstress > 3 && al_cur == 0x42)) {
        if (v_fall > 0 && (AFEA20(v_fall) & 7) < 5) AFEA20(v_fall) = (int16_t)((AFEA20(v_fall) & 0xfff8) | 5);
        if (al_cur == 0x47 && (AFEA20(v_bndstart) & 7) == 5) {   /* ? : the fall goes back a stressed syllable */
            i = v_bndstart;
            for (;;) {
                v_fall = (int16_t)(i - 1);
                if (v_fall <= 0) break;
                i = v_fall;
                if ((featb[APH20(v_fall)] & 1) && (AFEA20(v_fall) & 7) == 5) break;
            }
            AFEA20(v_bndstart) = (int16_t)((AFEA20(v_bndstart) & 0xfff8) | 2);
        }
        if (v_fall > 0 && !user_hat) {
            AFEA20(v_fall) |= 0x800;
            if (al_hat_phase == 1) AFEA20(v_fall) |= 0x400;
            al_hat_phase = 1;
            if (al_cur < 0x44) {
                v_prevfall = v_fall;
            } else {
                if (v_nstress < 2 && v_prevfall > 0) AFEA20(v_prevfall) &= ~0x800;
                v_prevfall = 0;
            }
        }
        if (al_cur == 0x46) {                   /* ! : primary stresses back from there become emphatic */
            do {
                AFEA20(v_fall) = (int16_t)((AFEA20(v_fall) & 0xfff8) | 6);
                v_fall--;
                if (v_fall < 0 || (AFEA20(v_fall) & 5) != 5) break;
            } while (!(featb[APH20(v_fall)] & 4));
        }
        v_hatprio = 0;
        v_nstress = 0;
    }
    if (al_cur == 0x48 || al_cur == 0x47 || al_cur == 0x46) {
        if (v_fall > 0) AFEA20(v_fall) |= 0x1800;
        user_hat = 0;
    }
    if (v_bnd > 0x100) {                        /* clause-level boundary: insert a silence */
        int16_t old = v_nallo++;
        if (old < PH_MAXALLO_V20) {
            APH20(v_nallo) = PH_SIL;
            AFEA20(v_nallo) = v_bnd;
            v_lastbnd = v_bnd;
            ADUR20(v_nallo) = ADUR20(v_n + 5);
            if (ADUR20(v_nallo) > 0) ADUR20(v_n + 5) = 0;
        }
        v_fall = 0;
    }
    return 1;
}

static void phalloph_v20(void)
{
    v_sylfeat = 0x20;                           /* 0x8a72 */
    v_stress = 1;
    v_nsyl = 0;
    v_pound = 0;
    v_bndstart = 1;
    v_lastbnd = 0;
    v_rise = 0;
    v_fall = 0;
    v_prevfall = 0;
    if (al_cur != 0x45) al_hat_phase = 1;       /* after a clause ending in a comma, a rise may still be placed */
    v_hatprio = 0;
    v_nstress = 0;
    v_nallo = 0;
    al_cur = 0x44;
    v_fea = v_fea_las = v_fea_las2 = featb[0x44];
    f0_halfsteps = 0;
    for (v_n = 0; v_n < nphonemes; v_n++) {
        v_prev = al_cur;
        al_cur = *phon20(v_n);
        v_fea_las2 = v_fea_las;
        v_fea_las = v_fea;
        v_fea = featb[al_cur];
        if (al_cur < NPH) {
            if (rcolour_v20() == -1) {          /* 0x93d0: the r's user duration and F0 target */
                if (ADUR20(v_n + 5) > 0) {
                    ADUR20(v_nallo) = (int16_t)(ADUR20(v_nallo) + ADUR20(v_n + 5));
                    ADUR20(v_n + 5) = 0;
                }
                if (UF020(v_n + 5) > 0) {
                    if (UF020(v_n + 6) == 0) UF020(v_n + 6) = UF020(v_n + 5);
                    UF020(v_n + 5) = 0;
                }
            } else {
                store_phone_v20();
            }
        }
        if (al_cur == 0x38) {                   /* 0x94a6: # */
            v_pound = 0x4000;
            v_stress = 1;
            v_sylfeat = 0x20;
        }
        if (hat_rise_v20() && hat_fall_v20() && stress_v20() && boundary_v20() &&
            (al_cur == 0x57 || al_cur == 0x58)) {   /* 0x8f28: index marker */
            int16_t at;
            v_n++;
            at = v_nallo ? v_nallo : 1;
            if (at < PH_MAXALLO_V20)            /* (past 249 the ROM writes over nallotot/f0tim) */
                index_marks[at] = (uint32_t)((int32_t)al_cur * 0x10000 + (int32_t)*phon20(v_n));
        }
    }
    nallotot = v_nallo;
}

void phalloph(void)
{
    int16_t sylfeat = 0x20;     /* -0x2: 0x20 for consonants until the word's first vowel */
    int16_t stress = 1;         /* -0x4: stress of the current syllable */
    int16_t nsyl = 0;           /* -0x6: syllables in the current word */
    int16_t bndstart = 1;       /* -0xc: last syllabic; a boundary spreads back to here */
    int16_t lastbnd = 0;        /* -0xe: boundary value of the last inserted silence */
    int16_t hatpos = 1;         /* -0x10: syllable that gets the next hat rise */
    int16_t hatprio = 0;        /* -0x12: 1 first syllable, 2 secondary stress, 5 primary stress */
    int16_t nstress = 0;        /* -0x14: stress symbols since the last hat rise */
    int16_t nallo = 0;          /* -0x18 */
    int16_t n;                  /* -0x1a */
    int16_t fea = featb[BND_CLSTART], fea_las = fea, fea_las2 = fea;   /* -0x1c, -0x1e, -0x20 */
    int16_t cur = BND_CLSTART, prev;                                   /* -0x22, -0x24 */
    int16_t bnd, mark, i;       /* -0x28, -0x2a, -0x16 */
    /* -0x8 (always 0, added to an inserted silence's feats), -0xa and -0x26 are written but never read */

    /* PH_FIX_MARKS: the three input symbols before this one, markers skipped (the ROM reads phonemes[n - 1] ...) */
    int16_t b1, b2, b3, h1 = BND_CLSTART, h2 = BND_CLSTART, h3 = BND_CLSTART;
    int fix = (ph_fixes & PH_FIX_MARKS) != 0;

    int cm = 0;                 /* PH_FIX_MARKS: the next of the clause's marks (ph_cmarks) */

    if (ph_v20) { phalloph_v20(); return; }     /* (PH_FIX_MARKS is not in v2.0's path yet) */
    f0_halfsteps = 0;
    if (fix) ph_nextra_marks = npending = 0;
    for (n = 0; n < nphonemes; n++) {
        if (fix) {
            while (cm < ph_ncmarks && ph_cmarks[cm].pos <= n) pend_mark(ph_cmarks[cm++].mark);
            if (phonemes[n] == SYM_INDEX1 || phonemes[n] == SYM_INDEX2) {   /* a marker in the clause: the same */
                int16_t code = phonemes[n++];
                pend_mark((uint32_t)((int32_t)code * 0x10000 + (int32_t)PHON(n)));
                continue;
            }
        }
        b1 = h1;
        b2 = h2;
        b3 = h3;
        h3 = h2;
        h2 = h1;
        h1 = phonemes[n];
        prev = cur;
        cur = phonemes[n];
        fea_las2 = fea_las;
        fea_las = fea;
        fea = featb[cur];

        if (cur < SYM_FIRST) {                                  /* ---- a phone ---- */
            /* "the" before a vowel: dh ax -> dh iy */
            if (phmode_8229c <= 3 && (fea & 1) && APH(nallo) == PH_AX && APH(nallo - 1) == PH_DH)
                APH(nallo) = PH_IY;
            /* postvocalic r and l (unless a stress symbol follows): l -> lx, r -> rx, and rx merges with the vowel
             * before it into an r-colored vowel */
            if (phmode_8229c <= 3 && !(featb[fix ? next_symbol(n) : PHON(n + 1)] & 0x1000) && (fea_las & 4)) {
                int16_t rvowel = 0;
                if (cur == PH_L) cur = PH_LX;
                if (cur == PH_R) cur = PH_RX;
                if (cur == PH_RX) {
                    if (prev == PH_AX) rvowel = PH_RR;
                    else if (prev == PH_IY || prev == PH_IH) rvowel = PH_IR;
                    else if (prev == PH_EY || prev == PH_EH || prev == PH_AE) rvowel = PH_ER;
                    else if (prev == PH_AA || prev == PH_AH) rvowel = PH_AR;
                    else if (prev == PH_OW || prev == PH_AO) rvowel = PH_OR;
                    else if (prev == PH_UW || prev == PH_UH) rvowel = PH_UR;
                    else if (phmode_8229c == 1) APH(nallo) = PH_AX;   /* then rx follows as its own phone */
                }
                if (rvowel) {
                    APH(nallo) = rvowel;
                    if (ADUR(n + 5) > 0) {                      /* the r's user duration goes to the vowel */
                        ADUR(nallo) += ADUR(n + 5);
                        ADUR(n + 5) = 0;
                    }
                    if (UF0(n + 5) > 0) {                       /* its F0 target moves on to the next phone */
                        if (UF0(n + 6) == 0) UF0(n + 6) = UF0(n + 5);
                        UF0(n + 5) = 0;
                    }
                    continue;
                }
            }
            if (nallo < PH_MAXALLO - 1) nallo++;
            APH(nallo) = cur;
            if (npending) place_marks(nallo);          /* PH_FIX_MARKS */
            ADUR(nallo) = ADUR(n + 5);
            if (nallo != n + 5) ADUR(n + 5) = 0;
            UF0(nallo) = UF0(n + 5);
            if (nallo != n + 5) UF0(n + 5) = 0;
            AFEA(nallo) = (int16_t)(stress + sylfeat);
            if (fea & 1) {                                      /* syllabic: the syllable's stress lives here */
                if (hatprio <= 1) { hatpos = nallo; hatprio = 1; }
                AFEA(nallo) = stress;
                sylfeat = 0;
                stress = 0;
                nsyl++;
                bndstart = nallo;
                lastbnd = 0;
            }
        } else if (cur == SYM_INDEX1 || cur == SYM_INDEX2) {    /* ---- index marker ---- */
            int16_t at = nallo ? nallo : 1;
            n++;
            if (at < PH_MAXALLO)                                /* (past 199 the ROM writes over nallotot/f0tim) */
                index_marks[at] = (uint32_t)((int32_t)cur * 0x10000 + (int32_t)PHON(n));
        } else if (cur > BND_PERIOD) {
            /* ignored */
        } else if (cur < BND_MORPH) {                           /* ---- stress symbol ---- */
            stress = 5;
            nstress++;
            if (cur == SYM_EMPHATIC) { stress = 6; f0_halfsteps = 1; }
            if (cur == SYM_SECONDARY) {
                stress = 2;
                if (hatprio <= 2) { hatpos = (int16_t)(nallo + 1); hatprio = 2; }
            } else {
                hatpos = (int16_t)(nallo + 1);
                hatprio = 5;
            }
            /* the symbol stands before the vowel: give the stress to the onset consonants too (one consonant,
             * two if the first is an obstruent, three for s + obstruent + consonant) */
            if ((fix ? b1 : phonemes[n - 1]) < SYM_FIRST && !(fea_las & 1)) {
                AFEA(nallo) = (int16_t)((AFEA(nallo) & 0x38) + stress);
                if ((fix ? b2 : phonemes[n - 2]) < SYM_FIRST && (fea_las2 & 0x20)) {
                    AFEA(nallo - 1) = (int16_t)((AFEA(nallo - 1) & 0x38) + stress);
                    if ((fix ? b3 : phonemes[n - 3]) == PH_S)
                        AFEA(nallo - 2) = (int16_t)((AFEA(nallo - 2) & 0x38) + stress);
                }
            }
        } else {                                                /* ---- boundary 61-69 ---- */
            if (cur == BND_PP || cur == BND_VP) {               /* phrase boundaries by speaking rate */
                if (sprate <= 120) cur = BND_COMMA;
                else if (sprate <= 140) cur = BND_VP;
                fea = featb[cur];
            }
            stress = 1;
            if (cur != BND_MORPH) {                             /* a word ends: mark syllable positions */
                sylfeat = 0x20;
                if (nsyl > 1) {
                    mark = 0x18;
                    for (i = nallo; i >= 0; i--) {
                        if (!(featb[APH(i)] & 1)) continue;
                        AFEA(i) |= mark;
                        if (mark == 8) break;
                        mark = 0x10;
                        if (--nsyl <= 1) mark = 8;
                    }
                }
                nsyl = 0;
            }
            bnd = bndval[cur - BND_MORPH];
            if (bnd > lastbnd)                                  /* back to the last syllabic */
                for (i = nallo; i >= bndstart; i--)
                    AFEA(i) = (int16_t)((AFEA(i) & 0x3f) + bnd);
            /* hat: a rise on the chosen syllable at a clause end, or at a phrase boundary after enough stresses */
            if (cur > BND_VP || (cur == BND_VP && nstress >= 2) || (cur == BND_PP && nstress >= 4)) {
                AFEA(hatpos) |= 0x400;
                if ((AFEA(hatpos) & 7) < 5) AFEA(hatpos) = (int16_t)((AFEA(hatpos) & ~7) | 5);
                if (cur == BND_PERIOD || cur == BND_QUEST || cur == BND_EXCLAIM) AFEA(hatpos) |= 0xc00;
                if (cur == BND_EXCLAIM)                         /* primary stresses back from there: emphatic */
                    while (hatpos >= 0 && (AFEA(hatpos) & 5) == 5) {
                        AFEA(hatpos) = (int16_t)((AFEA(hatpos) & ~7) | 6);
                        hatpos--;
                    }
                hatpos = (int16_t)(nallo + 1);
                hatprio = 0;
                nstress = 0;
            }
            if (bnd > 0x100 && nallo++ < PH_MAXALLO) {          /* clause-level boundary: insert a silence */
                APH(nallo) = PH_SIL;
                if (npending) place_marks(nallo);      /* PH_FIX_MARKS */
                AFEA(nallo) = bnd;
                lastbnd = bnd;
                ADUR(nallo) = ADUR(n + 5);
                if (ADUR(nallo) > 0) ADUR(n + 5) = 0;
            }
        }
    }
    if (fix) {                                  /* marks after the last allophone: with it */
        while (cm < ph_ncmarks) pend_mark(ph_cmarks[cm++].mark);
        if (npending) place_marks(nallo);
    }
    nallotot = nallo;
}
