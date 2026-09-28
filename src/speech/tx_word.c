/* The word layer (REFERENCE.md s7.1, s15.11, s15.26): out/outn hand each blank-separated word to
 * pronounce_word_or_abbrev, which holds back a lone "Dr."/"St." until it has seen the next word. pronounce_word then
 * tries the dictionaries on the whole word, strips punctuation, spells acronyms, lower-cases, checks that the word can
 * be pronounced and speaks it part by part (dictionary, else letter-to-sound rules), or spells it. Punctuation after
 * the word goes to emit_punctuation_symbol, which ends the clause on . ! ?.
 *
 * pronounce_word changes the word in place: capitals become small letters when the first lookups fail.
 */
#include <string.h>
#include "tx_text.h"
#include "tx_rom.h"

#define CL_OPEN 0x01                    /* char_class bits */
#define CL_CLOSE 0x02
#define CL_PUNCT (CL_OPEN | CL_CLOSE)
#define CL_VOWEL 0x04
#define CL_LOWER 0x08
#define CL_UPPER 0x10
#define CL_LETTER (CL_LOWER | CL_UPPER)
#define CL_CLAUSE 0x20                  /* ! , . : ; ? */
#define CL_CONS 0x80
#define MODE_SQUARE 0x01                /* dt_mode bits */
#define MODE_MINUS 0x04
#define SYM_SILENCE 0
#define SYM_HYPHEN 0x38                 /* between the parts of a hyphenated word */
#define SYM_PAREN 0x40                  /* for ( [ { in front of a word; the ) of a dictionary pronunciation */
#define SYM_SPACE 0x3e

int32_t word_count;
int16_t wh_question;
int16_t abbrev_pending;
char spell_buf[2] = {' ', '?'};
int16_t dt_mode = 1;                    /* .data: SQUARE at power-up */

static char abbrev_title[2][4] = {"Dr.", "St."};           /* 0x13be6, 0x13bea */
static char abbrev_default[2][7] = {"drive", "street"};    /* 0x13bee, 0x13bf4 */

static int cls(int32_t c) { return (uint8_t)char_class[c]; }
static int ccls(char c) { return cls((int8_t)c); }  /* the ROM's chars are signed */

/* 0x4e80: is s the word t (small letters), ignoring the case of s? A closing punctuation mark after it, or a period
 * and one, may follow. Returns the end of t, or NULL. */
const char *str_match_fold(const char *s, const char *t)
{
    int32_t c;
    for (;; s++) {
        c = (int8_t)*s;
        if (cls(c) & CL_UPPER) c += 0x20;
        if (c != (int8_t)*t++) {
            if (t[-1] || !(cls((int8_t)*s) & CL_PUNCT)) return NULL;
            if (!s[1]) return t;
            if (*s++ != '.' || !(cls((int8_t)*s) & CL_PUNCT) || s[1]) return NULL;
            return t;
        }
        if (!*s) return t;
    }
}

/* 0x5bd0: at the first word of a clause, note whether it starts with "wh" */
static void check_wh_word(const char *s)
{
    if (wh_question < 0) wh_question = (((int8_t)s[0] | 0x20) == 'w' && s[1] == 'h') ? 1 : 0;
}

/* 0x60aa: a punctuation mark after a word. . ! ? end the clause, and a ? after a wh- word ends it like a period. */
void emit_punctuation_symbol(int16_t c)
{
    int end = 0;
    switch (c) {
    case '?':
        if (wh_question > 0) c = '.';
        /* fall through */
    case '.':
    case '!':
        wh_question = -1;
        end = 1;
        break;
    case ')':
    case ']':
    case '}':
        c = ',';
        break;
    }
    if (c >= 0x21 && c <= 0x3f) clause_putsym(punct_syms[c - 0x21], 0);
    else if (cls(c) & CL_CLOSE) clause_putsym(SYM_SILENCE, 0);
    if (end) newclause(0);
}

/* 0x6198: spell p..e-1; a - is "dash" unless MODE MINUS */
void spell_chars(const char *p, const char *e)
{
    const int8_t *s;
    int8_t c;
    while (p < e) {
        c = *p++;
        if (c == '-' && !(dt_mode & MODE_MINUS)) {
            for (s = dash_syms; *s; s++) clause_putsym(*s, 0);
        } else {
            spell_char(c);
        }
    }
}

/* 0x621a: say the name of a character: the dictionary's key " c" (capitals as small letters) */
void spell_char(int32_t c)
{
    spell_buf[1] = (char)((cls(c) & CL_UPPER) ? c + 0x20 : c);
    if (!pronounce_dictionary_word(spell_buf, spell_buf + 2)) {
        kprintf(no_spell_fmt, (int8_t)spell_buf[1], (int8_t)spell_buf[1], (int8_t)spell_buf[1]);
        clause_putsym(SYM_SILENCE, 0);
    }
    clause_putsym(SYM_SPACE, 0);
}

/* 0x5a90: speak one blank-separated word, its punctuation included */
void pronounce_word(char *word)
{
    char *p = word, *e, *q, *wend, seen;
    int8_t c;
    int16_t napos, nvow, ncons;

    word_count++;
    prev_word_stressed--;
    e = wend = word + strlen(word);
    if (pronounce_dictionary_word(p, e)) goto spoken;
    for (; ccls(*p) & CL_OPEN; p++)
        if (*p == '(' || *p == '[' || *p == '{') clause_putsym(SYM_PAREN, 0);
    while (e > p && (ccls(e[-1]) & CL_CLOSE)) e--;
    if (e <= p || (!(dt_mode & MODE_SQUARE) && *p == ')')) goto spell_all;

    if (p > word || e < wend) {         /* punctuation came off: try again, with a final period, as "a" */
        if (*e == '.' && pronounce_dictionary_word(p, e + 1)) goto spoken_period;
        if ((ccls(*e) & CL_CLAUSE) && e - p == 1 && (*p == 'a' || *p == 'A')) {
            spell_char('a');
            goto spoken;
        }
        if (pronounce_dictionary_word(p, e)) goto spoken;
    }

    seen = 0;                           /* an acronym: capitals, each followed by a period */
    for (q = p; q < e; q++) {
        if (*q == '.') {
            if (!((ccls(q[1]) & CL_UPPER) && q > p && q + 2 >= e) && q[2] != '.') goto spell;
            seen = 1;
        } else if (!(ccls(*q) & CL_UPPER)) {
            break;
        }
    }
    if (q >= e && seen) {
        check_wh_word("");
        for (q = p; q < e; q++)
            if (*q != '.') spell_char((int8_t)*q);
        if (*e == '.' && e[1]) e++;
        goto punctuation;
    }

    seen = 0;                           /* lower-case it and look it up again */
    for (q = p; q < e; q++)
        if (ccls(*q) & CL_UPPER) {
            seen = 1;
            *q += 0x20;
        }
    if (seen) {
        if (*e == '.' && pronounce_dictionary_word(p, e + 1)) goto spoken_period;
        if (pronounce_dictionary_word(p, e)) goto spoken;
    }

    /* can it be pronounced? letters in parts split by ' and -, each part with a vowel and a consonant (y counts as
     * both; one vowel alone will do before a ' or -), the last part too unless there was a ' */
    napos = nvow = ncons = 0;
    for (q = p; q < e; q++) {
        c = *q;
        if (c == '\'' || c == '-') {
            if (c == '\'') napos++;
            else if (!((q + 1 < e && ((ccls(q[1]) & CL_VOWEL) || q[1] == 'y')) || (q + 2 < e && (ccls(q[2]) & CL_LETTER))))
                goto spell;
            if (!(ccls(q[1]) & CL_LETTER) || !nvow || (!ncons && nvow > 1)) goto spell;
            ncons = nvow = 0;
        } else if (c == 'y') {
            nvow++;
            ncons++;
        } else if (!(ccls(c) & CL_LETTER)) {
            goto spell;
        } else if (ccls(c) & CL_VOWEL) {
            nvow++;
        } else if (ccls(c) & CL_CONS) {
            ncons++;
        }
    }
    if (!napos && (!nvow || !ncons)) goto spell;

    check_wh_word(p);
    while (p < e) {                     /* part by part: the dictionary, else the rules; one letter is spelled */
        for (q = p; (ccls(*q) & CL_LETTER) || *q == '\''; q++) {}
        if (!pronounce_dictionary_word(p, q)) {
            if (q - p == 1) {
                spell_char((int8_t)*p);
            } else if (!lts_rule_engine(p, q)) {
                word = p;
                goto spell;
            }
        }
        if (*q == '-') {
            clause_putsym(SYM_HYPHEN, 0);
            q++;
        }
        p = q;
    }
    goto punctuation;

spoken_period:
    e++;
spoken:
    check_wh_word(p);
punctuation:                            /* one clause mark at most; "..." is one period; the rest is spelled */
    seen = 0;
    while (*e) {
        c = *e;
        if (c == '.' || c == '!' || c == ',' || c == ':' || c == ';' || c == '?') {
            if (c == '.' && !seen) {
                for (q = e; *q == '.'; q++) {}
                if (!*q) e = q - 1;
            }
            if (seen) {
                word = e;
                goto spell_all;
            }
            seen = 1;
        }
        emit_punctuation_symbol((int8_t)*e++);
    }
    return;

spell_all:
    e = wend;
spell:                                  /* spell word..e-1; a lone clause mark after it stays punctuation */
    spell_chars(word, e);
    if (e < wend) {
        if ((ccls(*e) & CL_CLAUSE) && !e[1]) {
            c = *e;
        } else {
            spell_chars(e, wend);
            c = wend[-1];
        }
        emit_punctuation_symbol(c);
    }
}

/* 0x7192: one word from out/outn, or NULL at the end of their text. A lone "Dr." or "St." waits for the next word:
 * "Doctor"/"Saint" (the dictionary's Dr./St.) before a capitalized word, else "drive"/"street". */
void pronounce_word_or_abbrev(char *p)
{
    static uint32_t abbrev_origin;              /* [DTC01] TX_FIX_MARKS: the held Dr./St.'s token */
    if (!p) {
        if (!abbrev_pending) return;
        clause_marks_upto(abbrev_origin);
        pronounce_word(abbrev_default[abbrev_pending - 1]);
    } else {
        if (abbrev_pending) {
            clause_marks_upto(abbrev_origin);
            pronounce_word((ccls(p[0]) & CL_UPPER) && (ccls(p[1]) & CL_LOWER) ? abbrev_title[abbrev_pending - 1]
                                                                             : abbrev_default[abbrev_pending - 1]);
            abbrev_pending = 0;
        }
        if (str_match_fold(p, "st.")) abbrev_pending = 2;
        else if (str_match_fold(p, "dr.")) abbrev_pending = 1;
        clause_marks_upto(tx_word_origin);      /* before a held Dr./St. too: it is said before any later word */
        if (!abbrev_pending) {
            pronounce_word(p);
            return;
        }
        abbrev_origin = tx_word_origin;
        if (!(ccls(p[3]) & CL_PUNCT)) return;    /* a word of its own: wait for the next one */
        pronounce_word(abbrev_default[abbrev_pending - 1]);
        for (p += 3; *p; p++) emit_punctuation_symbol((int8_t)*p);
    }
    abbrev_pending = 0;
}
