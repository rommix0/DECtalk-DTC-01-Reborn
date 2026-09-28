/* The token layer and the number engine (REFERENCE.md s7.3, s15.12, s15.27).
 *
 * The clause scanner hands every blank-separated token of ordinary text to token_dispatch, numbers or not. It splits
 * off the punctuation around the token, matches the rest against the number patterns (classify_number, a small regular
 * expression engine) and speaks it as a cardinal, ordinal, fraction, year, date, time, scientific number or money; any
 * other token goes to out() as it is. Money is held back one token (defer_money) so that "$1.23 million" can become
 * "one point two three million dollars", and a unit abbreviation after a number ("5 cm.") is spoken in full.
 *
 * The engine speaks by writing words to out()/outn(), which collect characters into a word and hand each finished
 * word to pronounce_word_or_abbrev. out(s, flag): flag 1 = s starts a new word and ends it, 0 = s starts a new word
 * that the next out() continues, -1 = s continues the current word ("twen" + "ty-" + "one" = "twenty-one"; "one" + ","
 * = "one,", a pause).
 */
#include <string.h>
#include "tx_text.h"
#include "tx_rom.h"

#define CL_OPEN 0x01                    /* char_class bits */
#define CL_CLOSE 0x02
#define CL_UPPER 0x10
#define CL_DIGIT 0x40

/* token_dispatch's classes (the second word of each pattern-table entry) */
enum {
    N_WORD = 0,                         /* not a number */
    N_MONEY_ONE_CENTS = 2,              /* $1.23: "one dollar and ..." */
    N_MONEY_CENTS = 3,                  /* $12.34, $.05 */
    N_MONEY_ONE = 4,                    /* $1 */
    N_MONEY_ZERO = 5,                   /* $0, $00 */
    N_MONEY = 6,                        /* $12, $1,234, $1.5 */
    N_YEAR = 7,                         /* 1984: "nineteen eighty-four" */
    N_ZERO = 8,                         /* 0, 000 */
    N_CARDINAL = 9,                     /* 123, 1,234, 12.34, 50% */
    N_FRACTION_ONE = 10,                /* 1/4, 1/100 */
    N_FRACTION = 11,                    /* 3/4, 44/100% */
    N_ORDINAL = 12,                     /* 1st 2nd 3rd 4th 11th */
    N_DIGITS = 13,                      /* 1.2E-4, 1.2.3 */
    N_DATE = 14,                        /* 23-Sep, 23-Sep-83 */
    N_DATE_YEAR = 15,                   /* 23-Sep-1983 */
    N_TIME = 16                         /* 11:04, 11:04:03.01 */
};

char money_num[82];                     /* 0x807d2: a number held back by defer_money */
char money_trail[82];                   /* 0x80824: the punctuation after it */
int8_t money_mode;                      /* 0x80876: its number_to_words mode, 0 = none */
int8_t currency;                        /* 0x80878: 1 = a lone $, 4 = "dollar" due, 6 = "dollars" due */
int32_t num_sign;                       /* 0x8087a: the held-back number's sign (1 +, -1 -) */
int32_t num_group;                      /* 0x8087e: the value of the last digit group spoken */
int8_t unit_number;                     /* 0x80882: the last token was a number, 1 = one, 2 = several (for units) */
int8_t num_count;                       /* 0x80884: number_to_words' number was 1 (1) or more (2) */
char out_buf[82];                       /* 0x80886: the word out() is building */
int8_t out_sep;                         /* 0x808d8: the last out() flag; nonzero = the next word starts afresh */
char tok_split[3 * 82];                 /* 0x808da: tok_lead, tok_body, tok_trail back to back, as in the ROM */
char *out_ptr = out_buf;                /* 0x82286: the end of out_buf's word */
const char *pat_start;                  /* 0x80f68: where classify_number's match must start */
/* [DTC01] TX_FIX_MARKS (tx_text.h): the token each word comes from. The bookkeeping runs always; only the marks use it. */
uint32_t tx_tokens, tx_out_origin, tx_word_origin;
static uint32_t out_word_origin;        /* the token of the word in out_buf */
static uint32_t money_origin;           /* the token of the money held back */

static int cls(int32_t c) { return (uint8_t)char_class[c]; }
static int ccls(char c) { return cls((int8_t)c); }  /* the ROM's chars are signed */
static int is_digit(char c) { return ccls(c) & CL_DIGIT; }
static int32_t fold(char c) { return (ccls(c) & CL_UPPER) ? (int8_t)c + 0x20 : (int8_t)c; }

/* ---- out/outn: words to pronounce_word_or_abbrev ---- */
#define OUT_END (out_buf + 79)          /* 0x808d5 */

static void out_flush(void)
{
    *out_ptr = 0;
    tx_word_origin = out_word_origin;
    pronounce_word_or_abbrev(out_buf);
    out_ptr = out_buf;
}

/* 0x556e: add s to the words being built; flag as described at the top. s = NULL ends the text. */
void out(const char *s, int32_t flag)
{
    int32_t c;
    if (flag >= 0 && out_sep && out_ptr > out_buf) out_flush();
    if (!s) {
        if (out_ptr > out_buf) out_flush();
        pronounce_word_or_abbrev(NULL);
    } else {
        while ((c = (int8_t)*s++) != 0) {
            if (c == ' ') {
                if (out_ptr > out_buf) out_flush();
            } else if (c < 0x20 || c > 0x7e) {
                kprintf(out_illegal_fmt, c);
            } else if (out_ptr >= OUT_END) {
                if (out_ptr > out_buf) out_flush();
                kprintf(out_long_fmt, out_buf);    /* the character is dropped */
            } else {
                if (out_ptr == out_buf) out_word_origin = tx_out_origin;
                *out_ptr++ = (char)c;
            }
        }
    }
    out_sep = (int8_t)flag;
}

/* 0x5614: out() for n characters; only a flag >= 1 starts a new word */
void outn(const char *s, int32_t n, int32_t flag)
{
    int32_t c;
    if (flag >= 1 && out_sep && out_ptr > out_buf) out_flush();
    if (!s) {
        if (out_ptr > out_buf) out_flush();
        pronounce_word_or_abbrev(NULL);
    } else {
        while (--n >= 0) {
            c = (int8_t)*s++;
            if (c == ' ') {
                if (out_ptr > out_buf) out_flush();
            } else if (c < 0x20 || c > 0x7e) {
                kprintf(outn_illegal_fmt, c);
            } else if (out_ptr >= OUT_END) {
                if (out_ptr > out_buf) out_flush();
                kprintf(outn_long_fmt, out_buf);
            } else {
                if (out_ptr == out_buf) out_word_origin = tx_out_origin;
                *out_ptr++ = (char)c;
            }
        }
    }
    out_sep = (int8_t)flag;
}

/* ---- classify_number: the number patterns (pat_number, pat_money) ---- */
enum {
    P_CHAR = 1,                         /* the next byte, capitals folded */
    P_START,                            /* at the start of the token */
    P_END,                              /* at its end */
    P_ANY,                              /* any character */
    P_IN, P_NOT_IN,                     /* a set: a length byte (items + 1), then chars or 0x0f lo hi ranges */
    P_STAR, P_PLUS, P_OPT,              /* a group, up to P_DONE: 0 or more, 1 or more, 0 or 1 times */
    P_LETTER, P_DIGIT, P_ALNUM,
    P_BLANK,                            /* a control character or space, not NUL */
    P_NUM,                              /* a digit, or a comma that groups three digits */
    P_DONE = 0x10,                      /* the end of a group or pattern */
    P_RANGE = 0x0f
};

/* 0x7352: match pattern p at s (the token starts at start); the end of the match, or NULL */
static const char *pattern_match(const char *s, const char *start, const uint8_t *p)
{
    const char *comma = NULL, *save, *r;
    int32_t op, n;
    int8_t c;
    while ((op = (int8_t)*p++) != P_DONE) {
        switch (op) {
        case P_CHAR:
            if (fold(*s++) != (int8_t)*p++) return NULL;
            break;
        case P_START:
            if (s != pat_start) return NULL;
            break;
        case P_END:
            if (*s) return NULL;
            break;
        case P_ANY:
            if (!*s++) return NULL;
            break;
        case P_IN:
        case P_NOT_IN:
            c = (int8_t)fold(*s++);
            n = *p++;
            do {
                if (*p == P_RANGE) {
                    p += 3;
                    n -= 2;
                    if (c >= (int8_t)p[-2] && c <= (int8_t)p[-1]) break;
                } else if (c == (int8_t)*p++) {
                    break;
                }
                n--;
            } while (n > 1);
            if ((op == P_IN) == (n <= 1)) return NULL;
            if (op == P_IN) p += n - 2;     /* the rest of the set */
            break;
        case P_PLUS:
            if (!(s = pattern_match(s, start, p))) return NULL;
            /* fall through */
        case P_STAR:
            save = s;
            while (*s && (r = pattern_match(s, start, p)) != NULL) s = r;
            while (*p++ != P_DONE) {}
            for (;; s--) {                      /* the rest of the pattern, backing off one character at a time */
                if ((r = pattern_match(s, start, p)) != NULL) return r;
                if (s == save) return NULL;
            }
        case P_OPT:
            r = pattern_match(s, start, p);
            while (*p++ != P_DONE) {}
            if (r) s = r;
            break;
        case P_LETTER:
            c = (int8_t)fold(*s++);
            if (c < 'a' || c > 'z') return NULL;
            break;
        case P_DIGIT:
            c = *s++;
            if (c < '0' || c > '9') return NULL;
            break;
        case P_ALNUM:
            c = (int8_t)fold(*s++);
            if (c >= 'a' && c <= 'z') break;
            if (c < '0' || c > '9') return NULL;
            break;
        case P_BLANK:
            c = *s++;
            if (!c || c > ' ') return NULL;
            break;
        case P_NUM:
            c = *s++;
            if (c == ',') {
                if (comma) {
                    if (s - comma != 4) return NULL;
                } else if (s - start > 4 && s[-2] >= '0' && s[-2] <= '9' && s[-3] >= '0' && s[-3] <= '9' &&
                           s[-4] >= '0' && s[-4] <= '9' && s[-5] >= '0' && s[-5] <= '9') {
                    return NULL;                /* four digits before a comma */
                }
                if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9' || s[2] < '0' || s[2] > '9') return NULL;
                if (s[3] > '0' && s[3] < '9') return NULL;     /* a fourth digit, but 0 and 9 pass */
                comma = s;
            } else {
                if (c < '0' || c > '9') return NULL;
                if (comma && s - comma > 3) return NULL;
            }
            break;
        default:
            panic("pattern_match");
            break;
        }
    }
    return s;
}

/* 0x7310: does pattern p match anywhere in s (only at the start if it begins with P_START)? */
static int pattern_find(const char *s, const uint8_t *p)
{
    const char *q;
    pat_start = s;
    for (q = s; *q; q++) {
        if (pattern_match(q, s, p)) return 1;
        if (*p == P_START) return 0;
    }
    return 0;
}

/* 0x40e0: the class of the first pattern in table that s matches; tables are {pattern, class} pairs, NULL-ended */
static int32_t classify_number(const char *s, const num_pattern_t *table)
{
    for (; table->pattern; table++)
        if (pattern_find(s, table->pattern)) return table->cls;
    return 0;
}

/* ---- speaking numbers ---- */
/* 0x4b98: "plus" or "minus" for an explicit sign */
static void speak_number_sign(int32_t sign)
{
    if (sign) out((sign > 0 ? s_plus : s_minus), 1);
}

/* 0x50e8: speak n (1-3) digits as one group. mode: 4, 6, 9 cardinal, 10 fraction "fourth", 11 fraction "fourths",
 * 12 ordinal. flags bit 0: say nothing for 000; flags != 0: no "over" for a denominator 1. Sets num_group. */
static void speak_digit_group(const char *s, int32_t n, int32_t mode, int32_t flags)
{
    int32_t v = 0;
    const char *w;
    if (n < 1 || n > 3) panic("speak_digit_group");
    while (--n >= 0) v = v * 10 + (*s++ - '0');
    num_group = v;
    if (v == 0 && (flags & 1)) return;
    if (v >= 100) {
        out(num_ones[v / 100], 1);
        out(s_hundred, 1);
        v %= 100;
        if (v == 0) goto suffix;
    }
    if (v >= 20) {
        out(num_tens[(v - 20) / 10], 0);
        v %= 10;
        if (v == 0) {
            switch (mode) {
            case 6: case 9: w = s_ty; break;
            case 10: case 12: w = s_tieth; break;
            case 11: w = s_tieths; break;
            default: panic("speak_digit_group"); return;
            }
            out(w, 1);
            return;
        }
        out(s_ty_hyphen, 0);
    }
    if (v > 12) {
        out(num_ones[v], 1);
        goto suffix;
    }
    switch (mode) {
    case 4: case 6: case 9:
        break;
    case 12:
        out(num_ordinals[v], 1);
        return;
    case 10: case 11:
        if (flags || num_group >= 2) {
            if (num_group == 2) {
                out((mode == 10 ? s_half : s_halves), 1);
                return;
            }
            out(num_ordinals[v], 0);
            out((mode == 11 ? s_s : s_empty), 1);
            return;
        }
        out(s_over, 1);                /* 1/1: "one over one" */
        break;
    default:
        panic("speak_digit_group");
        return;
    }
    out(num_ones[v], 1);
    return;
suffix:                                         /* "thirteen" + "th", "hundred" + "ths" */
    if (mode == 10 || mode == 12) out(s_th, -1);
    else if (mode == 11) out(s_ths, -1);
}

/* 0x4ee4: more than nine digits: digit by digit, in threes with a pause between (the last five or fewer singly), or
 * with a pause at each comma if there is one after the first digits */
static const char *speak_digits_individually(const char *s, int32_t mode)
{
    const char *p;
    int32_t n = 0;
    for (p = s; is_digit(*p); p++) n++;
    if (*p == ',') {
        for (p = s;; p++) {
            if (*p == ',') {
                out(s_comma_pause, -1);
            } else {
                if (!is_digit(*p)) return p;
                if (is_digit(p[1]) || p[1] == ',') speak_digit_group(p, 1, 9, 2);
                else speak_digit_group(p, 1, mode, 0);
            }
        }
    }
    p = s;
    for (; n >= 6; n -= 3) {
        speak_digit_group(p++, 1, 9, 2);
        speak_digit_group(p++, 1, 9, 2);
        speak_digit_group(p++, 1, 9, 2);
        out(s_group_pause, -1);
    }
    while (--n > 0) speak_digit_group(p++, 1, 9, 2);
    speak_digit_group(p++, 1, mode, 0);
    return p;
}

/* 0x50a2: speak the digits at s (commas between them allowed) as a whole number; mode 7 = a year. Returns the end. */
static const char *cardinal_number_to_words(const char *s, int32_t mode)
{
    char buf[10], *b = buf, *q;
    const char *p;
    const char *const *scale;           /* into num_scales */
    int32_t n = 0;
    for (p = s; *p; p++) {
        if (is_digit(*p)) {
            if (++n > 9) return speak_digits_individually(s, mode);
            *b++ = *p;
        } else if (*p != ',' || (int8_t)p[1] <= ' ') {
            break;
        }
    }
    *b = 0;
    if (n == 0) return p;
    if (buf[0] == '0') {                        /* a leading zero: digit by digit, 0 = "oh" */
        for (q = buf; *q; q++) out(*q == '0' ? s_oh_leading : num_ones[*q - '0'], 1);
        return p;
    }
    if (n < 4) {
        speak_digit_group(buf, n, mode, 0);
        return p;
    }
    if (mode == 7) {                            /* four digits as a year */
        if (str_match_fold(buf + 1, s_three_zeros)) {
            out(num_ones[buf[0] - '0'], 1);
            out(s_thousand, 1);
            return p;
        }
        speak_digit_group(buf, 2, 9, 2);
        if (buf[2] == '0') {
            if (buf[3] == '0') {
                out(s_hundred_year, 1);
                return p;
            }
            out(s_oh_year, 1);
            speak_digit_group(buf + 3, 1, 9, 0);
            return p;
        }
        speak_digit_group(buf + 2, 2, 9, 0);
        return p;
    }
    scale = num_scales + (n - 1) / 3;
    n = (n - 1) % 3 + 1;
    q = buf;
    speak_digit_group(q, n, 9, 2);
    q += n;
    out(*scale, 1);
    scale--;
    for (; q + 4 < b; q += 3, scale--) {
        speak_digit_group(q, 3, 9, 3);
        if (num_group) out(*scale, 1);
    }
    speak_digit_group(q, 3, mode, 1);
    return p;
}

/* 0x4bd0: speak the number at s in a mode: 2/3 dollars and cents, 4/6 money, 7 year, 9 cardinal, 10/11 fraction
 * denominator, 12 ordinal. A decimal point, and a % after the number, are spoken too. Returns the end. */
static const char *number_to_words(const char *s, int32_t mode)
{
    const char *p, *q;
    num_count = 0;
    switch (mode) {
    case 2: case 3:
        speak_number_sign(num_sign);
        num_sign = 0;
        p = cardinal_number_to_words(s, 9) + 1;
        if (*s != '.') out((mode == 3 ? s_dollars_and : s_dollar_and), 1);
        currency = 0;
        if (*p == '0' && *++p == '0') {
            out(s_no_cents, 1);
            p++;
        } else {
            p = cardinal_number_to_words(p, 9);
            out((num_group == 1 ? s_cent : s_cents), 1);
        }
        break;
    case 9:
        num_count = (s[0] == '1' && !is_digit(s[1]) && s[1] != ',') ? 1 : 2;
        goto cardinal;
    case 7:
        num_count = 2;
        /* fall through */
    case 4: case 6:
    cardinal:
        p = cardinal_number_to_words(s, mode);
        if (*p == '.' && (int8_t)p[1] > ' ' && is_digit(*++p)) {
            out(s_point, 1);
            for (q = p; *q == '0'; q++) {}
            if (is_digit(*q)) {
                for (; is_digit(*p); p++) out(*p == '0' ? s_oh_fraction : num_ones[*p - '0'], 1);
            } else {
                out(s_zero, 1);        /* only zeros after the point */
                p = q;
            }
        }
        break;
    case 10: case 11: case 12:
        p = cardinal_number_to_words(s, mode);
        break;
    default:
        p = s;
        break;
    }
    if (*p == '%') {
        out(s_percent, 1);
        p++;
    }
    return p;
}

/* ---- the token ---- */
/* 0x4114: split a token into its opening punctuation, the rest and its closing punctuation */
static void split_token(const char *s)
{
    char *d = tok_lead;
    size_t n;
    while (ccls(*s) & CL_OPEN) *d++ = *s++;
    *d = 0;
    for (n = strlen(s); n && (ccls(s[n - 1]) & CL_CLOSE); n--) {}
    strcpy(tok_trail, s + n);
    memcpy(tok_body, s, n);
    tok_body[n] = 0;
}

/* 0x4a40, 0x4a6c: speak the token's opening / closing punctuation, glued to the word after / before it */
static void flush_lead(void)
{
    if (tok_lead[0]) {
        out(tok_lead, 0);
        tok_lead[0] = 0;
    }
}

static void flush_trail(void)
{
    if (tok_trail[0]) {
        out(tok_trail, -1);
        tok_trail[0] = 0;
    }
}

/* 0x4a98: hold back an amount of money until the next token has been seen */
static void defer_money(const char *s, int32_t mode)
{
    strcpy(money_num, s);
    money_mode = (int8_t)mode;
    strcpy(money_trail, tok_trail);
    money_origin = tx_out_origin;
}

/* 0x4ace: speak the money held back; with say_unit, then "dollar(s)" and its punctuation */
static void flush_currency_suffix(int32_t say_unit)
{
    uint32_t origin = tx_out_origin;
    tx_out_origin = money_origin;               /* its words come from the money's token */
    if (money_mode) {
        speak_number_sign(num_sign);
        num_sign = 0;
        number_to_words(money_num, money_mode);
        unit_number = 0;
        money_mode = 0;
    }
    if (say_unit) {
        if (currency) {
            out((currency == 6 ? s_dollars : currency == 4 ? s_dollar : s_dollar_sign), 1);
            currency = 0;
        }
        if (money_trail[0]) {
            out(money_trail, -1);
            money_trail[0] = 0;
        }
    }
    tx_out_origin = origin;
}

/* 0x40aa: is s a scale word ("million"), which may follow money? */
static int is_scale_word(const char *s)
{
    const char *const *t;
    for (t = num_scales; *t; t++)
        if (str_match_fold(s, *t)) return 1;
    return 0;
}

/* 0x56c2: a unit abbreviation with a period after a number ("5 cm."): speak it in full, plural after 2 or more */
static int speak_unit_abbrev(const char *s)
{
    const char *const *t;
    const char *w;
    if (tok_trail[0] != '.') return 0;
    for (t = num_units; *t; t++) {
        if (!(w = str_match_fold(s, *t))) continue;
        if (unit_number == 2)
            while (*w++) {}
        out(tok_lead, 0);
        out(w, 1);
        out(tok_trail + 1, -1);
        return 1;
    }
    return 0;
}

/* 0x4062: speak one token of text (NULL or "" = the end of the text) */
void token_dispatch(const char *arg)
{
    int8_t sign = 0, dollar = 0, ndot;
    char buf[5];
    const char *p, *q;
    const char *const *mon;
    int32_t code, c;

    if (!arg || !*arg) {
        flush_currency_suffix(1);
        out(NULL, 0);
        clause_marks_all();                     /* [DTC01] TX_FIX_MARKS: marks after the run's last word */
        return;
    }
    tx_out_origin = tx_tokens++;
    split_token(arg);
    p = tok_body;
    if (*p == '$') {
        flush_currency_suffix(1);
        p++;
        if (!*p && !tok_trail[0]) {             /* a lone $: the amount follows */
            flush_lead();
            currency = 1;
            money_origin = tx_out_origin;
            return;
        }
        dollar = 1;
    }
    if (*p == '+') {
        sign = 1;
        p++;
    } else if (*p == '-') {
        sign = -1;
        p++;
    }
    if (is_digit(*p) || *p == '.') code = classify_number(p, currency || dollar ? pat_money : pat_number);
    else code = N_WORD;

    switch (code) {
    default:
        panic("token_dispatch");
        /* fall through */
    case N_WORD:
        break;

    case N_MONEY_ONE_CENTS: case N_MONEY_CENTS: case N_MONEY_ONE: case N_MONEY:
        flush_lead();
        defer_money(p, code);
        num_sign = sign;
        currency = (code == N_MONEY || code == N_MONEY_CENTS) ? 6 : 4;
        return;
    case N_MONEY_ZERO:
        flush_lead();
        defer_money(s_zero_money, 6);
        num_sign = sign;
        return;

    case N_YEAR: case N_ZERO: case N_CARDINAL: case N_ORDINAL:
        flush_currency_suffix(1);
        flush_lead();
        speak_number_sign(sign);
        if (code == N_ZERO) {
            number_to_words(s_zero_digit, 9);
            unit_number = 2;
        } else {
            number_to_words(p, code);
            unit_number = num_count;
        }
        flush_trail();
        return;
    case N_FRACTION_ONE:
        flush_currency_suffix(1);
        flush_lead();
        speak_number_sign(sign);
        out(s_one, 1);
        number_to_words(p + 2, 10);
        unit_number = 1;
        flush_trail();
        return;
    case N_FRACTION:
        flush_currency_suffix(1);
        flush_lead();
        speak_number_sign(sign);
        number_to_words(number_to_words(p, 9) + 1, 11);
        unit_number = 2;
        flush_trail();
        return;

    case N_DIGITS:
        flush_currency_suffix(1);
        ndot = 0;
        for (q = p;;) {                         /* digits and points, then e and an exponent */
            c = (int8_t)*q++;
            if (c == 0) break;
            if (c == '+' || c == '-') {
                ndot = 0;
            } else if (c == '.') {
                c = (int8_t)*q;
                if (ndot && c > ' ') goto word;
                ndot++;
                if (!is_digit((char)c) || !is_digit(q[-2])) goto word;  /* ".5e5": the last byte of tok_lead */
                continue;
            } else if (c == 'E' || c == 'e') {
                ndot = 1;
                if (*q == '+' || *q == '-') continue;
            } else {
                if (is_digit((char)c)) continue;
                goto word;
            }
            if (*q != '.' && !is_digit(*q)) goto word;
        }
        flush_lead();
        speak_number_sign(sign);
        while (*p) {
            c = (int8_t)*p;
            if (is_digit((char)c) || (c == '.' && is_digit(p[1]))) {
                p = number_to_words(p, 9);
                continue;
            }
            p++;
            if (c == 'e' || c == 'E') {
                out(s_times_ten, 1);
                c = (int8_t)*p;
                if (c == '-') out(s_exp_minus, 1);
                if (c == '+' || c == '-') p++;
                p = number_to_words(p, 12);
                out(s_power, 1);
            } else if (c >= '*' && c <= '/') {
                out(num_punct_words[c - '*'], 1);   /* "times plus comma minus dot over" */
            } else {
                buf[0] = (char)c;
                outn(buf, 1, 1);
            }
        }
        unit_number = 2;
        flush_trail();
        return;

    case N_DATE: case N_DATE_YEAR:
        if (sign || (tok_body[0] == '0' && tok_body[1] == '0')) break;
        while (*p && *p++ != '-') {}
        for (mon = num_months; *mon; mon++) {   /* "janJanuary": a key, then the name */
            const char *m = *mon;
            if (fold(p[0]) != *m++ || fold(p[1]) != *m++ || fold(p[2]) != *m++) continue;
            flush_lead();
            out(m, 1);
            p += 3;
            number_to_words(*arg == '0' ? arg + 1 : arg, 12);  /* the day, from the whole argument */
            if (*p++) {
                out(s_date_comma, -1);
                if (code == N_DATE_YEAR) {
                    number_to_words(p, 7);
                } else {
                    buf[0] = '1';
                    buf[1] = '9';
                    buf[2] = p[0];
                    buf[3] = p[1];
                    buf[4] = 0;
                    number_to_words(buf, 7);
                }
            }
            if (tok_trail[0]) {
                flush_trail();
            } else {
                out(s_date_end, -1);
            }
            unit_number = 0;
            return;
        }
        break;

    case N_TIME:
        flush_lead();
        p = number_to_words(p, 9);
        out(s_time_comma1, -1);
        p = number_to_words(p + 1, 9);
        if (*p++ == ':') {
            out(s_time_comma2, -1);
            number_to_words(p, 9);
        }
        if (tok_trail[0]) {
            flush_trail();
        } else {
            out(s_time_end, -1);
        }
        unit_number = 0;
        return;
    }

word:                                           /* not a number */
    if (currency) {
        if (is_scale_word(tok_body)) {          /* "$1.23 million" */
            if (money_mode) {
                money_mode = 6;
                flush_currency_suffix(0);
                currency = 6;
            }
            flush_lead();
            out(tok_body, 1);
            strcpy(money_trail, tok_trail);
            return;
        }
        flush_currency_suffix(1);
    }
    if (!unit_number || !speak_unit_abbrev(tok_body)) {
        flush_lead();
        out(tok_body, 1);
        flush_trail();
    }
    unit_number = 0;
}
