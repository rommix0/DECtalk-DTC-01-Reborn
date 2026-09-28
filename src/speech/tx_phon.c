/* Phonemic text (REFERENCE.md s8.3, s15.29): parse_phonemic_text and the phoneme-name parser under it.
 *
 * The clause scanner sends the inside of [...] here, and pronounce_dictionary_word a user-dictionary substitution.
 * The text is a run of phoneme and symbol names (the 2-letter alphabet of sym_table, or with DT_MODE ASKY the
 * 1-character one), each optionally followed by <duration in ms, F0>; pseudo-phonemes such as :ra take a number;
 * the voice names :np ... :nv become :vo and a voice number. Slash-star ... star-slash is a comment, and ":dv" starts
 * the text of a [:dv ...] command, which goes to klsyn one character per word up to ';'. Each symbol goes to
 * clause_putsym. The comment and :dv state live in the caller, so they carry over from one call to the next.
 */
#include <string.h>
#include "tx_text.h"
#include "tx_rom.h"

#define CL_UPPER 0x10                   /* char_class bit */
#define CT_DIGIT 0x04                   /* ctype_tab bit */
#define MODE_ASKY 0x02                  /* dt_mode bit */
#define LOG_ERROR 0x20                  /* dt_log bit */
#define ERR_PHONEMIC 0x08               /* dt_error_flags bit: bad phonemic text */
#define SYM_STRESS1 0x39                /* ' primary stress */
#define SYM_VERB 0x40                   /* ) */
#define SYM_SPACE 0x3e
#define SYM_SEMI 0x3b                   /* the ';' that ends [:dv] text */
#define SYM_RA 0x65                     /* :ra, the speaking rate */
#define SYM_VO 0x64                     /* :vo, a voice number */
#define SYM_NP 0x6b                     /* :np ... :nv, the nine voice names */
#define SYM_NV 0x73

static char lower(char c) { return (char)(c >= 'A' && c <= 'Z' ? c + 0x20 : c); }     /* tolower 0x11e48 */
static int ccls(char c) { return (uint8_t)char_class[(int8_t)c]; }  /* signed index */
static int is_digit(char c) { return ctype_tab[c & 0x7f] & CT_DIGIT; }

/* 0x7c10 (also a static of ph_command.c) */
static const char *skip_blanks(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* log_control_error 0xd1ca: the console, with DT_LOG error on */
static void log_error(const char *fmt, const char *arg)
{
    if (dt_log & LOG_ERROR) kprintf(fmt, arg);
}

/* the decimal numbers of <...> and of a pseudo-phoneme: _ctype digits on (c & 0x7f), each adding its signed
 * value - '0'; the 32-bit total is clamped to 0x7fff */
static const char *read_number(const char *p, uint32_t *v)
{
    uint32_t n = 0;
    while (is_digit(*p)) n = n * 10 + (uint32_t)(int32_t)(*p++ - '0');
    if (n > 0x7fff) n = 0x7fff;
    *v = n;
    return p;
}

/* 0x1100e: the phoneme or symbol name at s; *end = after it. Returns the symbol code (sym_table's code | flags),
 * or -1 for an illegal name. A name is one or two letters: ASCII_CLASS bits 0-1 say whether a letter can stand alone
 * (2), must take a second letter (1: a vowel, which needs a second letter with bit 2) or may (3: then with a second
 * letter with bit 3, or as yu and rr); "h" followed by "x" is not taken as a second letter. ":xx" is three. */
static int16_t lookup_phoneme(const char *s, const char **end)
{
    const char *p = s + 1;
    char c0 = *s, c1;
    char name[4] = {0, 0, 0, 0};        /* the ROM's is on the stack: where it is not set, the message shows old
                                           bytes */
    int f0, f1;
    const tx_sym_t *rec;
    if (c0 == ':') {
        if (!*p || !p[1]) goto fail;
        name[0] = ':';
        name[1] = lower(p[0]);
        name[2] = lower(p[1]);
        name[3] = 0;
        p += 2;
        goto search;
    }
    if (dt_mode & MODE_ASKY) {
        int16_t code;
        if (c0 < 0x20 || c0 > 0x7e) goto fail;
        code = asky_codes[c0 - 0x20];
        if (code == 0xff) goto fail;
        *end = p;
        return code;
    }
    c0 = lower(c0);
    name[0] = c0;
    c1 = lower(*p);
    name[3] = c1;
    f0 = c0 < 0x20 || c0 > 0x7e ? 0 : ph_letter_class[c0 - 0x20];
    f1 = c1 < 0x20 || c1 > 0x7e ? 0 : ph_letter_class[c1 - 0x20];
    switch (f0 & 3) {
    case 0:
        goto fail;
    case 1:
        if (!(f1 & 4) || (c1 == 'h' && lower(p[1]) == 'x')) goto fail;
        goto two;
    case 2:
        goto one;
    default:
        if ((c0 == 'y' && c1 == 'u') || (c0 == 'r' && c1 == 'r')) goto two;
        if ((f1 & 8) && !(c1 == 'h' && lower(p[1]) == 'x')) goto two;
        goto one;
    }
two:
    name[1] = c1;
    p++;
    name[2] = 0;
    goto search;
one:
    name[1] = 0;
search:
    *end = p;
    for (rec = sym_table; rec->name; rec++)
        if (!strcmp(rec->name, name)) return (int16_t)rec->code;
    goto illegal;
fail:
    *end = p;
illegal:
    dt_error_flags |= ERR_PHONEMIC;
    log_error(illegal_phoneme_fmt, name);
    return -1;
}

/* 0x10a9e: one symbol and its values at s; *end = after it. Returns 1 with *code and val[] set, 0 for nothing to
 * send (end of text, or a code beyond :nv), 2 for an illegal name.
 *   phoneme<dur>      -> code | 0x1000, val[0] = dur ms as frames
 *   phoneme<dur,f0>   -> code | 0x2000, val[0] = frames, val[1] = f0
 *   :ra 200 (a 0x4000 symbol)  -> code | 0x1000, val[0] = the number (:ra clamped to 120..350, 0 = 180)
 *   :np ... :nv       -> :vo | 0x1000, val[0] = 0 ... 8 */
static int parse_phoneme(const char *s, const char **end, int16_t *code, int16_t *val)
{
    int result = 0;
    uint32_t v;
    if (*s) {
        const char *e;
        *code = lookup_phoneme(s, &e);
        if (*code == -1) {
            *code = 0;
            *end = e;
            return 2;
        }
        s = e;
        result = 1;
        if (!(*code & 0x4000)) {
            if (*s == '<') {
                s = read_number(skip_blanks(s + 1), &v);
                val[0] = ms_to_frames((int16_t)v);
                s = skip_blanks(s);
                if (*s != ',') {
                    *code |= 0x1000;
                } else {
                    *code |= 0x2000;
                    s = read_number(skip_blanks(s + 1), &v);
                    val[1] = (int16_t)v;
                    s = skip_blanks(s);
                }
                if (*s == '>') {
                    s++;
                } else {
                    log_error(bad_dur_fmt, NULL);
                    dt_error_flags |= ERR_PHONEMIC;
                    while (!strchr(dur_stop, *s)) s++;
                    if (*s == '>') s++;
                }
            }
        } else {
            s = read_number(skip_blanks(s), &v);
            val[0] = (int16_t)v;
            *code &= 0xff;
            if (*code == SYM_RA) {
                if (val[0] == 0) val[0] = 180;
                else if (val[0] < 120) val[0] = 120;
                else if (val[0] > 350) val[0] = 350;
            }
            *code |= 0x1000;
        }
        if (*code >= SYM_NP && *code <= SYM_NV) {
            val[0] = (int16_t)(*code - SYM_NP);
            *code = 0x1000 | SYM_VO;
            *end = s;
            return 1;
        }
        if ((*code & 0xff) <= SYM_NV) {
            *end = s;
            return result;
        }
        dt_error_flags |= ERR_PHONEMIC;
    }
    *end = s;
    return 0;
}

/* [DTC01] TX_FIX_MARKS (tx_text.h): the inside of [ ] if it holds only index marks, ":in n" or ":re n" (read as
 * parse_phoneme reads them: blanks, then a number), with blanks between. Their symbols (code | 0x1000, as
 * parse_phoneme gives them) and values, and how many; -1 for anything else. Nothing is sent or logged. */
int phonemic_marks_only(const char *s, int16_t *sym, int16_t *val, int max)
{
    int n = 0;
    for (;;) {
        const char *e;
        uint32_t v;
        int16_t code;
        char a, b;
        s = skip_blanks(s);
        if (!*s) return n ? n : -1;
        if (n == max || s[0] != ':' || !s[1]) return -1;
        a = lower(s[1]);
        b = lower(s[2]);
        if (!((a == 'i' && b == 'n') || (a == 'r' && b == 'e'))) return -1;
        code = lookup_phoneme(s, &e);
        if (code == -1 || !(code & 0x4000)) return -1;
        s = read_number(skip_blanks(e), &v);
        sym[n] = (int16_t)((code & 0xff) | 0x1000);
        val[n++] = (int16_t)v;
    }
}

/* 0x3dcc: speak phonemic text. flush: at the end, close an open [:dv] command. suffix: the suffix code of a
 * user-dictionary word (pronounce_dictionary_word), which decides whether a ) becomes a word boundary, as there.
 * Returns the last symbol sent by the last step, else 0. */
int parse_phonemic_text(const char *s, int flush, char *in_dv, char *in_comment, int suffix)
{
    int16_t sym = 0, val[2];
    const char *e;
    while (*s) {
        sym = 0;
        if (*in_comment) {
            /* the ROM tests the character after a '*' even when it is the NUL, and then reads on past it */
            while (*s) {
                if (*s++ == '*' && *s++ == '/') {
                    *in_comment = 0;
                    break;
                }
            }
            continue;
        }
        if (*s == '/' && s[1] == '*') {
            *in_comment = 1;
            s += 2;
            continue;
        }
        if (*in_dv) {
            if (*s == ';') {
                clause_putsym(SYM_SEMI, CLAUSE_RAW);
                newclause(0);
                s++;
                *in_dv = 0;
            } else {
                clause_putsym(*s++, CLAUSE_RAW);
            }
            continue;
        }
        if (*s == ':') {
            char c = s[1];
            if ((ccls(c) & CL_UPPER ? c + 0x20 : c) == 'd') {
                c = s[2];
                if ((ccls(c) & CL_UPPER ? c + 0x20 : c) == 'v') {
                    *in_dv = 1;
                    s += 3;
                    newclause(1);
                    continue;
                }
            }
        }
        switch (parse_phoneme(s, &e, &sym, val)) {
        case 0:
            break;
        case 1:
            if ((sym & 0xff) <= 0x45 || (sym & 0xff) >= SYM_VO) {
                if (sym == SYM_STRESS1) {
                    prev_word_stressed = 1;
                } else if (sym == SYM_VERB) {
                    /* as in pronounce_dictionary_word: ) stays only after a stressed word, and not before the
                     * suffixes 2, 6, 7, 8 */
                    if (prev_word_stressed < 0 || suffix == 2 || (suffix >= 6 && suffix <= 8))
                        sym = SYM_SPACE;
                }
                clause_putsym(sym, val);
                break;
            }
            /* fall through: a letter, digit or other code that phonemic text may not send */
        default:
            dt_error_flags |= ERR_PHONEMIC;
            sym = 0;
            break;
        }
        s = e;
    }
    if ((int8_t)flush && *in_dv) {
        clause_putsym(SYM_SEMI, CLAUSE_RAW);
        newclause(0);
        *in_dv = 0;
        sym = 0;
    }
    return sym;
}
