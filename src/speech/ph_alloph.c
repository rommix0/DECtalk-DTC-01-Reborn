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
 */
#include "ph_frame.h"
#include "ph_rom.h"

int16_t phonemes[PH_MAXALLO];
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
