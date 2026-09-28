/* Phonetic frame path of the DTC-01 v1.8 firmware, rebuilt in C (REFERENCE.md s15.17).
 *
 * Allophones, durations and F0 commands in; one 19-word DSP speech frame per 6.4 ms out. The routines are
 * Dennis Klatt's 1984-86 phonetic component as the ROM has it (dapi ph_claus.c, ph_setar.c, ph_draw.c,
 * ph_drwt01.c, ph_vset.c), with the ROM's own values and quirks kept so the output is word for word the same.
 *
 * Every global below is a RAM variable of the original; the comment gives its address. Integer widths follow
 * the 68000 code: int16_t for words, with 16-bit truncation wherever the ROM stores a word.
 */
#ifndef PH_FRAME_H
#define PH_FRAME_H
#include <stdint.h>

/* ---- one parameter track (dapi PARAMETER without outp), 0x20 bytes in the ROM ---- */
typedef struct {
    int16_t tarcur;   /* current target */
    int16_t durlin;   /* end time of the current diphthong line (frames) */
    int16_t deldip;   /* diphthong slope per frame, x8 */
    int16_t dipcum;   /* accumulated diphthong movement, x8 */
    int16_t ftran;    /* forward transition, x8 */
    int16_t dftran;   /* its decrement per frame */
    int16_t btran;    /* backward transition, x8 */
    int16_t dbtran;   /* its increment per frame */
    int16_t tbacktr;  /* frame where the backward transition starts */
    int16_t tspesh;   /* frames of the special value at phone onset */
    int16_t pspesh;   /* the special value (burst, aspiration, ...) */
    int16_t tarnex;   /* target of the next phone */
    int16_t tarlas;   /* target of the last phone */
    int16_t tarend;   /* end target of this phone */
    int16_t *ndip;    /* next {durlin, deldip} pair in dipspec_buf */
} ph_param_t;

enum { PF1, PF2, PF3, PFZ, PB1, PB2, PB3, PAV, PAH, PA2, PA3, PA4, PA5, PA6, PAB, PH_NPARAM };

/* ---- the 19-word speech frame (parstochip 0x822a4; the trailer is added when posting) ---- */
enum { OUT_HDR, OUT_T0, OUT_F1, OUT_F2, OUT_F3, OUT_FZ, OUT_B1, OUT_B2, OUT_B3, OUT_AV, OUT_AH,
       OUT_A2, OUT_A3, OUT_A4, OUT_A5, OUT_A6, OUT_AB, OUT_TLT, PH_FRAME_WORDS };
#define PH_FRAME_HDR 0x4000
#define PH_FRAME_TRAILER 0x43D4

/* ---- the 24-word speaker packet (spdef_packet 0x81c10: dapi SP_CHIP + f0minimum/f0scalefac) ---- */
enum { SP_HDR, SP_F4, SP_B4, SP_F5, SP_B5, SP_P4, SP_P5, SP_F0MINIMUM, SP_T0JIT, SP_G1, SP_G2, SP_G3, SP_G4,
       SP_G5, SP_NOPEN1, SP_NOPEN2, SP_ATURB, SP_F0SCALEFAC, SP_FNSCALE, SP_GF, SP_GN, SP_GV, SP_GH,
       SP_CHECKSUM, SP_WORDS };
#define PH_SPEAKER_HDR 0x6000

/* ---- voice record, 28 shorts (cur_voice 0x81c44; REFERENCE s8.1) ---- */
typedef struct {
    int16_t sex, sm, as, ap, pr, br, ri, nf, la, hs, f4, b4, f5, b5, p4, p5,
            gf, gh, gv, gn, g1, g2, g3, g4, g5, ft, bf, ef;
} voice_t;

#define PH_MAXALLO 200   /* allophons[] etc. hold 200 entries in the ROM */
#define PH_MAXEV 50      /* F0 command list (make_f0_command) */

/* klsyn work item -> clause (ph_clause.c) */
extern int16_t voice_code, voice_last;  /* 0x82298, 0x8229a: 0x6b + voice index (0x6b = :np ... 0x73 = :nv) */
extern int16_t draw_mode_82294;        /* 0x82294, 's' (.data; never written): klclause draws the frames */
extern int16_t dt_log;                 /* 0x822ca, DT_LOG */
extern voice_t val_voice;              /* 0x81c7c, the :nv voice record */
extern int16_t dv_value;               /* 0x81ccc, the last [:dv] value read (ph_command.c) */
extern void (*ph_console_hook)(const char *text); /* console text of [:dv list] (printf level, "\n") */

/* clause input: klclause -> phalloph */
extern int16_t phonemes[PH_MAXALLO];   /* 0x81d78, the clause as phonemes + stress/boundary symbols (ph_alloph.c) */
extern int16_t nphonemes;              /* 0x81f08 */

/* phalloph -> phtiming -> frame loop (phonemes[n]'s user duration / F0 target arrive in allodurs/f0tar[n + 5]) */
extern int16_t allophons[PH_MAXALLO];   /* 0x80f82 */
extern int16_t allodurs[PH_MAXALLO];    /* 0x81112, frames */
extern int16_t allofeats[PH_MAXALLO];   /* 0x812a2, stress/structure bits */
extern uint32_t index_marks[PH_MAXALLO];/* 0x81432, index marker per phone (0 = none) */
extern int16_t nallotot;                /* 0x81752, last allophone index */
/* 0x81754-0x81947 as one area, because the ROM relies on the layout: f0tim[50] (written when the list is full)
 * is f0tar[0], and f0tar[] doubles as the per-phone list of sung notes / F0 targets (200 entries) that
 * phtiming turns into F0 commands in place. */
extern int16_t f0cmd_area[PH_MAXEV + PH_MAXALLO];
#define f0tim (f0cmd_area)              /* 0x81754, frames since the previous F0 command */
#define f0tar (f0cmd_area + PH_MAXEV)   /* 0x817b8, F0 commands (in: per-phone notes when f0mode = 1) */
extern int16_t nf0tot;                  /* 0x81948, number of F0 commands */
extern int16_t f0mode;                  /* 0x80f6c, 0 = rule F0, 1 = sung notes / user targets */
extern int16_t stop_pending;            /* 0x81bec, DT_STOP seen: truncate the clause */
extern int16_t phmode_8229c;            /* 0x8229c, read by phalloph/phtiming/phsettar; meaning open */
extern int16_t dt_error_flags;          /* 0x81f12, error bits (8 = bad phonemic input) */

/* speaker state (setspdef) */
extern voice_t cur_voice;               /* 0x81c44 */
extern int16_t malfem;                  /* 0x81c06, 1 = male */
extern int16_t spdeftltoff;             /* 0x81c08, sm*36/100 */
extern int16_t assertiveness;           /* 0x81c0a, as*41 */
extern int16_t f2max, f3max;            /* 0x81c0c, 0x81c0e */
extern int16_t spdef_packet[SP_WORDS - 1]; /* 0x81c10, 23 words without the checksum */
extern int16_t f0basefall;              /* 0x81c3e, bf*10 */
extern int16_t ef_x10;                  /* 0x81c40, ef*10 */
extern int16_t f0_dep_tilt;             /* 0x81c42, ft*41 */
extern int16_t spdef_dirty;             /* 0x8229e, a byte in the ROM: 1 = post the speaker packet */

/* frame loop and phsettar */
extern int16_t tcum, durfon, nphone;    /* 0x81bce, 0x81bd0, 0x81bd2 */
extern ph_param_t ph_params[PH_NPARAM]; /* 0x8197e */
extern int16_t dipspec_buf[64];         /* 0x81b80 (39 words in the ROM) */
extern int16_t parstochip[PH_FRAME_WORDS]; /* 0x822a4 */
extern int16_t phsettar_started;        /* 0x81d0c */
extern int16_t phcur, pholas, phonex, phonex2, pholas2; /* 0x81bf8, 0x81ce8, 0x81cea, 0x81cec, 0x81ce6 */
extern int16_t feacur, fealas, feanex;  /* 0x81cf0, 0x81cee, 0x81cf2: featb[] of the phones */
extern int16_t strucur, strucnex, struclas2; /* 0x81cf6, 0x81cf8, 0x81cf4: allofeats[] */
extern int16_t endtyp_las, begtyp_cur, endtyp_cur, begtyp_nex; /* 0x81cfa-0x81d00 */
extern int16_t dur_ratio_q14;           /* 0x81c00, 16384*durfon/inhdr */
extern int16_t ftran_scale, btran_scale;/* 0x81c02, 0x81c04 */
extern int16_t bouval, durtran;         /* 0x81bfa, 0x81bfc */
extern int16_t stop_save[4];            /* 0x81bee-0x81bf4 */

/* pht0draw (dapi names) */
extern int16_t nf0ev;                   /* 0x81bf6, -2 = hard init, -1 = new clause */
extern int16_t f0;                      /* 0x81d1c, tenths of Hz before speaker scaling */
extern int16_t nfram, nframb, nframs, nframg; /* 0x81d1e, 0x81d20, 0x81d22, 0x81d24 */
extern int16_t extrad, tglstp, tglstp_next, segdur, segdrg; /* 0x81d26-0x81d2e */
extern int16_t f0las1, f0las2, tarhat, tarimp; /* 0x81d30-0x81d36 */
extern int16_t f0a2, f0b, f0a1;         /* 0x81d38, 0x81d3a, 0x81d3c (0x600, 0x3a00, 0x3000) */
extern int16_t dtimf0, phonex_t0, f0seg_raw, f0seg, f0basestart, f0in; /* 0x81d3e-0x81d48 */
extern int16_t np_drawt0, npg, f0command, delimp, f0baseline, f0endfall, phocur_t0; /* 0x81d4a-0x81d56 */
extern int16_t vibsw, vibcum, newnote, delnote, vibcount, vibstep; /* 0x81d58-0x81d62 */
extern int16_t f0_tenths;               /* 0x81bd8, speaker-scaled F0 */

/* phtiming (dapi p_us_tim.c names where they exist) */
extern int16_t sprate, sprate_last, sprat1, sprat2; /* 0x82296 (words/min), 0x81954, 0x81956, 0x81958 (Q14) */
extern int16_t compause, perpause;      /* 0x8194e, 0x8194c: extra frames at a comma / period pause */
extern int16_t f0_halfsteps;            /* 0x8194a: 1 = halve the stress F0 commands; set by phalloph [I] */
extern int16_t cumdur;                  /* 0x81cce, frames since the last F0 command (make_f0_command) */
extern int32_t cumdur_long;             /* 0x81cd2, cleared by phtiming, otherwise unused here */
extern int16_t emphasissw;              /* 0x81cd0 */
extern int16_t hatstate, hatsize, hatfall; /* 0x822a0 (1 = at the top of the hat), 0x822a2, 0x8195a */
extern int16_t tm_phocur, tm_prcnt, tm_durinh, tm_durmin, tm_deldur, tm_nphon, tm_dpause;
                                        /* 0x81cd6-0x81ce0, 0x81950: phtiming's working globals */

/* hooks (may be NULL) */
extern void (*ph_frame_sink)(const int16_t *words, int n); /* a finished frame, trailer/checksum included */
extern void (*ph_index_reply_hook)(int value); /* DT_INDEX_REPLY mark spoken: host replies R2 31, R3 value */
/* the library: a mark phsettar reaches goes with the frame about to be posted, and is acted on when that frame is
 * heard (index_mark_spoken); NULL = act on it at once, as the ROM does */
extern void (*ph_mark_hook)(uint32_t mark);
/* the library: a phone starts (phsettar has just run), with its duration in frames; it goes with the next post, as
 * a mark does (the phoneme array of memory buffers). NULL in the ROM checks. */
extern void (*ph_phone_hook)(int phone, int frames);
extern void (*ph_error_hook)(const char *fmt, int a, int b); /* log_control_error */
/* [DTC01] departures from the ROM for the library (0 = the ROM, which the tests check). PH_FIX_MARKS: phalloph
 * skips index markers (0x66/0x67 and their value word) when it looks at the symbols round one. In the ROM a stress
 * symbol right after a marker takes the marker's value for a phone and stresses the phone before it: "are [:in
 * 5]'you" lengthens "are" (REFERENCE s17.13; the text side's half is tx_text.h's TX_FIX_MARKS). */
extern int ph_fixes;
#define PH_FIX_MARKS 1
/* PH_FIX_SPLIT: a :vo or :ra in the middle of a work item speaks the part before it, and phtiming leaves that part's
 * durations in allodurs[]; the ROM reads them as the rest's user durations (ph_clause.c). The fix clears them. */
#define PH_FIX_SPLIT 2
/* PH_FIX_MARKS: marks for a phone that has one already. The ROM keeps one per phone (index_marks[], the last one);
 * the fix keeps the first there and the others here, and phsettar reports them after it, in order. */
#define PH_EXTRA_MARKS 16
typedef struct {
    int16_t at;
    uint32_t mark;
} ph_extra_mark_t;
extern ph_extra_mark_t ph_extra_marks[PH_EXTRA_MARKS];
extern int ph_nextra_marks;
/* PH_FIX_MARKS: the text side sends a clause's marks after its words, not in them (tx_clause.c). klsyn_dispatch
 * takes them (ph_side: the positions in the message), parse_phoneme_param_stream moves them to the positions the
 * words get in phonemes[] (ph_cmarks, for the clause klclause speaks next), and phalloph puts each where a marker
 * at that position would have been. */
#define PH_SIDE_MAX 64
typedef struct {
    int16_t pos;
    uint32_t mark;                      /* code << 16 | value, as index_marks[] */
} ph_side_mark_t;
extern ph_side_mark_t ph_side[PH_SIDE_MAX], ph_cmarks[PH_SIDE_MAX];
extern int ph_nside, ph_ncmarks;

/* the klsyn task (ph_task.c) */
struct msg;
extern int16_t last_index;              /* 0x81d70, the last index marker spoken */
void klsyn_task_main(void);             /* 0x3c20 */
void klsyn_dispatch(struct msg *msg);   /* its loop body: one mailbox message */
void index_mark_reached(uint32_t mark); /* 0xfaaa: code << 16 | value, from phsettar */
void index_mark_spoken(uint32_t mark);  /* its action: last_index, and the reply for a 0x67 mark */

void parse_phoneme_param_stream(int nwords); /* 0x7788: a klsyn work item in phonemes[] -> clauses */
void klclause(int16_t *ph, int16_t *end); /* 0x7a04: one clause -> DSP frames */
const char *parse_bracket_command(const char *p); /* 0x7d1e: [:dv ...] commands */
void print_voice_param_table(int all); /* 0x8072: [:dv list] (0) / listall (1) */
void load_voice_definition(void); /* 0x8150: voice_code -> cur_voice, speaker packet */
void save_voice_params(void);     /* 0x81a8: cur_voice -> val_voice */
int16_t ms_to_frames(int16_t ms); /* 0xd54c */
void phalloph(void);              /* 0x843e: phonemes -> allophones + structure bits */
void phtiming(void);              /* 0x8ebe: durations, then the F0 command list */
void make_f0_command(int cmd, int delay); /* 0xa6e2 (kl3_push_event) */
void phclause_draw_frames(void);  /* 0xa782 */
void phsettar(void);              /* 0xaa08 */
void phdraw(void);                /* 0xa804 */
void pht0draw(void);              /* 0xc722 */
void dsp_post_frame(const int16_t *w, int n); /* 0x7b56 */
void set_formant_limits(void);    /* 0x7d42 */
void setspdef(void);              /* 0x81e6 */

#endif
