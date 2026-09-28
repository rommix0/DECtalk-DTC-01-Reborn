/* The text pipeline (dttask) of the DTC-01 v1.8 firmware, rebuilt in C (REFERENCE.md s15.24).
 *
 * Text in, klsyn work items out: clause scanner -> numbers / words -> dictionary, suffix stripping, letter-to-sound
 * rules -> clause_putsym -> newclause -> klsyn. Rebuilt bottom-up; each routine is checked word for word against the ROM
 * (decomp/test/test_text.c). Every global is a RAM variable of the original; the comment gives its address.
 */
#ifndef TX_TEXT_H
#define TX_TEXT_H
#include <stdint.h>

/* ---- letter-to-sound rules (tx_lts.c; REFERENCE s15.10) ---- */
/* one item of the working list: a letter, phoneme or boundary code and its feature bits (14 bytes in the ROM) */
typedef struct lts_node {
    int16_t code;
    uint32_t feat;
    struct lts_node *next, *prev;
} lts_node_t;

#define LTS_MAXNODE 100                 /* the pool 0x809e2-0x80f59 */
extern lts_node_t lts_pool[LTS_MAXNODE];/* 0x809e2 */
extern int lts_pool_used;               /* 0x8228e as a count: nodes handed out from the pool so far */
extern lts_node_t *lts_free;            /* 0x80f5a: nodes given back */
extern lts_node_t lts_hdr;              /* 0x809d4: pseudo node; .next (0x809da) is the list head */
extern lts_node_t lts_nil;              /* ROM 0x13bc8: the list's end marker */
extern int16_t lts_nnodes;              /* 0x80f5e: nodes in the list */
extern int16_t lts_rule;                /* 0x80f60: rules tried so far */
extern int16_t lts_npos, lts_pos;       /* 0x80f62, 0x80f64: positions to try for this rule, the current one */
/* 0x81d74: pronounce_word subtracts 1 at every word, a primary stress (LTS or dictionary) sets it to 1 and newclause
 * clears it; so during a word it is >= 0 when the previous word of the clause was stressed. The dictionary's ) verb
 * marker is kept only then. */
extern int16_t prev_word_stressed;

int lts_rule_engine(const char *p, const char *end);   /* 0x6a64: 1 = spoken, 0 = cannot */

/* ---- dictionaries (tx_dict.c; REFERENCE s7.2, s15.9, s15.25) ---- */
/* a user-dictionary entry (DT_DICT); the ROM mallocs {next.l, len.b, name, NUL, substitution, NUL} */
typedef struct dict_entry {
    struct dict_entry *next;
    int8_t len;                         /* the name's length */
    char text[1];                       /* the name, NUL, the substitution, NUL */
} dict_entry_t;

#define DICT_NBUCKET 32
extern dict_entry_t *dict_user_table[DICT_NBUCKET]; /* 0x820a0 */
extern int16_t dict_user_count;         /* 0x8209e: entries in the user dictionary */
extern const char *dict_user_text;      /* 0x809d0: the substitution of the last user-dictionary lookup */
extern int32_t dict_hits;               /* 0x81f0e: built-in dictionary hits (nothing reads it) */
#define DICT_USER_HIT ((const uint8_t *)&dict_user_text)   /* lookup_word's result for a user entry */

void dict_hash_clear_all(void);                                         /* 0xfb10 */
const char *dict_hash_lookup(const char *p, const char *last);          /* 0xfb56 */
dict_entry_t *dict_hash_insert_or_delete(const char *name, const char *subst);  /* 0xfc36 */
const uint8_t *lookup_word(const char *p, const char *last);            /* 0x68fc */
const uint8_t *lookup_word_with_suffix_stripping(char *p, char *end, int16_t *suffix);  /* 0x65c8 */
int pronounce_dictionary_word(char *p, char *end);                      /* 0x62d8: 1 = spoken, 0 = not found */

/* ---- words (tx_word.c; REFERENCE s7.1, s15.11, s15.26) ---- */
extern int32_t word_count;              /* 0x81f0a: words so far (nothing reads it) */
/* 0x81d76: -1 at the start of a clause (newclause, . ! ?); the clause's first spoken word sets 1 if it starts with
 * "wh", else 0. A ? after a wh- word ends the clause like a period. */
extern int16_t wh_question;
extern int16_t abbrev_pending;          /* 0x80f66: a lone "Dr." (1) or "St." (2) waits for the next word */
extern char spell_buf[2];               /* 0x8228a: the dictionary key " c" of a spelled character */
extern int16_t dt_mode;                 /* 0x822ce: DT_MODE, set by the host (bit 0 SQUARE, 1 ASKY, 2 MINUS) */

const char *str_match_fold(const char *s, const char *t);                /* 0x4e80 */
void emit_punctuation_symbol(int16_t c);                                /* 0x60aa */
void spell_char(int32_t c);                                             /* 0x621a */
void spell_chars(const char *p, const char *e);                         /* 0x6198 */
void pronounce_word(char *word);                                        /* 0x5a90 */
void pronounce_word_or_abbrev(char *p);                                 /* 0x7192: NULL = the end of the text */

/* ---- tokens and numbers (tx_num.c; REFERENCE s7.3, s15.12, s15.27) ---- */
extern char money_num[82];              /* 0x807d2: a number held back by defer_money */
extern char money_trail[82];            /* 0x80824: the punctuation after it */
extern int8_t money_mode;               /* 0x80876: its number_to_words mode, 0 = none */
extern int8_t currency;                 /* 0x80878: 1 = a lone $, 4 = "dollar" due, 6 = "dollars" due */
extern int32_t num_sign;                /* 0x8087a: the held-back number's sign */
extern int32_t num_group;               /* 0x8087e: the value of the last digit group spoken */
extern int8_t unit_number;              /* 0x80882: the last token was a number: 1 = one, 2 = several */
extern int8_t num_count;                /* 0x80884: number_to_words' number was 1 (1) or more (2) */
extern char out_buf[82];                /* 0x80886: the word out() is building */
extern int8_t out_sep;                  /* 0x808d8: the last out() flag */
extern char tok_split[3 * 82];          /* 0x808da-0x809cf: the token split in three, back to back */
#define tok_lead (tok_split)            /* 0x808da: its opening punctuation */
#define tok_body (tok_split + 82)       /* 0x8092c: the rest */
#define tok_trail (tok_split + 164)     /* 0x8097e: its closing punctuation */
extern char *out_ptr;                   /* 0x82286: the end of out_buf's word */
extern const char *pat_start;           /* 0x80f68: where classify_number's match must start */

void out(const char *s, int32_t flag);                                  /* 0x556e: NULL = the end of the text */
void outn(const char *s, int32_t n, int32_t flag);                      /* 0x5614 */
void token_dispatch(const char *arg);                                   /* 0x4062: NULL or "" = the end of the text */

/* ---- the clause buffer (tx_clause.c; REFERENCE s15.28) ---- */
struct msg;
extern struct msg *clause_msg;          /* 0x807b4: the clause being built, a klsyn_free_pool message */
extern int16_t *clause_wr;              /* 0x807b8: its next free word */
extern int16_t *clause_end;             /* 0x807bc: 199 words in: a symbol there ends the clause */
extern int16_t *clause_soft_end;        /* 0x807c0: 25 words before that: a word boundary there ends the clause */
extern int16_t *clause_last;            /* 0x807c4: the last plain symbol stored */
extern int16_t *clause_mark;            /* 0x807c8: clause_last when the last ) , ! ? . arrived */
extern int16_t clause_run;              /* 0x807cc: symbols stored since then */
extern int16_t clause_bound;            /* 0x807ce: the last * word boundary ( or ) mark */
extern int8_t clause_silence;           /* 0x807d0: the last symbol was a silence (0); another one is dropped */
extern int16_t dt_log;                  /* 0x822ca: DT_LOG, set by the host (0x02 = log phonemes; ph_clause.c) */
extern int16_t name_nval, name_nvalues; /* 0x82120, 0x82122: phoneme_name's values written, and expected */
extern int8_t name_in_frames;           /* 0x82124: the next value is a duration in frames */
extern int8_t name_pseudo;              /* 0x82126: a 0x4000 symbol ("name v " rather than "name<v,v>") */
extern char name_buf[32];               /* 0x82128: phoneme_name's text */
extern void (*tx_log_hook)(const char *text);   /* the DT_LOG console text (printf level; the stream adds CR before LF) */
#define CLAUSE_RAW ((const int16_t *)1) /* clause_putsym: a character of [:...] text, stored as it is */

void newclause(int32_t flag);                           /* 0x3912: 0 = a phoneme clause, 1 = [:...] text */
void clause_putsym(int32_t sym, const int16_t *val);    /* 0x35f8 */
const char *phoneme_name(int16_t c);                    /* 0x10d9c: NULL while values are due */

/* ---- phonemic text (tx_phon.c; REFERENCE s15.29) ---- */
/* 0x3dcc: speak phonemic text ([...] and user-dictionary substitutions); returns the last symbol */
int parse_phonemic_text(const char *s, int flush, char *in_dv, char *in_comment, int suffix);

/* ---- the clause scanner and dttask (tx_scan.c; REFERENCE s15.30) ---- */
extern char readin_buf[82];             /* 0x80732: the word being read */
extern int8_t readin_depth;             /* 0x80784: [ ] nesting */
extern char readin_in_dv;               /* 0x80786: parse_phonemic_text's in_dv */
extern char *readin_hi;                 /* 0x80788: the end of the word so far */
extern char readin_in_comment;          /* 0x8078c: parse_phonemic_text's in_comment */
extern int8_t readin_after_bracket;     /* 0x8078e: the last ] closed phonemic text; a , ! ? . next is a mark */
extern int8_t dttask_eof;               /* 0x81d6a: the 0x1A marker was read */
void dttask_main(void);                 /* 0xf946: the dttask task; clause_readin 0x3182 does the work */

/* ---- [DTC01] departures from the ROM, for the library (0 = the ROM's behaviour, which the tests check) ---- */
/* TX_FIX_MARKS: index marks alone in [ ] ([:in n], [:re n]) leave the speech as it is without them. In the ROM a [
 * ends the run of words (clause_readin calls token_dispatch(NULL)), so the word before a mark is said as a run's
 * last word: a function word gets longer, as at the end of a phrase ("go for [:in 2]it"); REFERENCE s17.13. With
 * the fix a bracket of marks does not end the run, and its marks wait (clause_mark_defer) until the first word that
 * came after them in the text is spoken: the words carry the number of the token they came from (tx_out_origin,
 * tx_word_origin) through the three places that hold words back (money, out()'s word, a lone Dr. or St.). */
extern int tx_fixes;
#define TX_FIX_MARKS 1
extern uint32_t tx_tokens;              /* tokens token_dispatch has taken */
extern uint32_t tx_out_origin;          /* the token the words out() starts now come from */
extern uint32_t tx_word_origin;         /* the token of the word out() hands to pronounce_word_or_abbrev */
void clause_mark_defer(int16_t sym, int16_t val, uint32_t token);  /* a mark for before token `token`'s words */
void clause_marks_upto(uint32_t token); /* put the marks due before the words of this token */
void clause_marks_all(void);            /* put every waiting mark (the run of words ends) */
void clause_marks_clear(void);
/* the inside of [ ] if it holds only index marks: their symbols and values, and how many (at most max); else -1 */
int phonemic_marks_only(const char *s, int16_t *sym, int16_t *val, int max);

/* ---- from the klsyn side (ph_frame.c, ph_clause.c) ---- */
extern int16_t dt_error_flags;          /* 0x81f12: error bits (8 = bad phonemic text) */
int16_t ms_to_frames(int16_t ms);       /* 0xd54c */

/* ---- the console (src/kernel/console.c) and the kernel ---- */
int kprintf(const char *fmt, ...);      /* 0xd248: the console; the ROM's own printf, only %d %c %s */
void panic(const char *msg);            /* 0x1d618: a jump to itself; the unit hangs */

#endif
