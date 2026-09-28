/* The clause scanner and the dttask task (REFERENCE.md s15.30): the top of the text pipeline.
 *
 * dttask reads the text pipe one character at a time. clause_readin collects the characters of a word in readin_buf
 * and hands each word to token_dispatch (ordinary text) or parse_phonemic_text (inside [ ]); a blank ends a word, a
 * line end or the end of the text also ends the clause (token_dispatch(NULL)), and the end of the text posts the last
 * clause too. A 0x1A in the pipe (the DT_SYNC marker) is the end of the text: clause_readin returns, and dttask posts
 * a one-word 0x68 message to klsyn, which signals sync_sem once everything before it has been spoken.
 *
 * Details the ROM has: a backspace moves back over the word, and what is typed then overstrikes it only when its
 * rank (bits 0-1 of the class) is not lower, so a letter wins over punctuation; the characters after the backspace
 * stay (hi is not moved back). A control character other than the ones the scanner acts on is spoken as "control"
 * and its letter (spell_chars), an ESC as "escape", each followed by a comma. With DT_MODE SQUARE, [ and ] (and
 * always 0x02 and 0x03) open and close phonemic text; brackets nest, a ] ends a word boundary, and a , ! ? or . right
 * after the last ] is sent as a mark. A word longer than 79 characters is cut (DT_LOG 0x80 logs it).
 */
#include <stdarg.h>
#include "tx_text.h"
#include "tx_rom.h"
#include "../kernel/rtos.h"
#include "../kernel/console.h"

#define RC_RANK 0x03                    /* readin_class bits */
#define RC_ACTION 0x0c
#define RC_CONTROL 0x04
#define RC_STORE 0x08
#define RC_SPELL 0x0c
#define RC_PUNCT 0x10                   /* , ! ? . */
#define RC_LINE 0x20                    /* LF FF CR */
#define RC_END 0x40                     /* the end of the text (-1), NUL, VT, DEL */
#define RC_BLANK 0x80                   /* space, tab (and the ones above) */
#define SYM_SPACE 0x3e                  /* word boundary */
#define SYM_COMMA 0x42
#define MODE_SQUARE 0x01                /* dt_mode bit */
#define LOG_INPUT 0x01                  /* dt_log bits */
#define LOG_DEBUG 0x80
#define READIN_MAX 0x4f                 /* 0x80781: where a word is cut */
#define KLSYN_SYNC 0x68
#define END_FLAGS 0x30                  /* stream_t flags: end, error */

extern mbox_t klsyn_mbox;               /* 0x807a2 (ph_task.c) */

char readin_buf[82];                    /* 0x80732: the word being read */
int8_t readin_depth;                    /* 0x80784: [ ] nesting */
char readin_in_dv;                      /* 0x80786: parse_phonemic_text's in_dv (never reset here) */
char *readin_hi;                        /* 0x80788: the end of the word so far */
char readin_in_comment;                 /* 0x8078c: parse_phonemic_text's in_comment */
int8_t readin_after_bracket;            /* 0x8078e: the last ] closed phonemic text */
int8_t dttask_eof;                      /* 0x81d6a: dttask_getc met the 0x1A */
stream_t *dttask_in;                    /* 0x81f1a: the text pipe, the reading end */
stream_t *cur_stream;                   /* 0x81f1e: its writing end, where the host side puts the text */
static mbox_t dttask_pool;              /* 0x8208c: 2 messages of 2 words, for the sync message */
/* [DTC01] TX_FIX_MARKS (tx_text.h): a top-level [ has not yet ended the run of words; it does so at the first
 * phonemic text read, or at the ], unless the brackets held only index marks */
static int8_t readin_break;
static int8_t readin_open_prev;         /* the class of the character before that [ */
#define MARKS_IN_BRACKET 8

/* the run break the [ would have made in the ROM, made now */
static void readin_make_break(void)
{
    if (!readin_break) return;
    readin_break = 0;
    token_dispatch(NULL);
}

static int32_t rclass(int32_t c) { return readin_class[c]; }  /* sign-extended; c = -1 too */

/* 0xd1f4 */
static void log_debug(const char *fmt, ...)
{
    va_list ap;
    if (!(dt_log & LOG_DEBUG)) return;
    va_start(ap, fmt);
    vformat_string(fmt, ap);
    va_end(ap);
}

/* 0x3580: hand the word read so far on (flush = parse_phonemic_text's) and start the next */
static char *readin_flush(int8_t flush)
{
    *readin_hi = 0;
    if (readin_hi > readin_buf) {
        if (readin_depth > 0) {
            readin_make_break();
            parse_phonemic_text(readin_buf, flush, &readin_in_dv, &readin_in_comment, 0);
        } else {
            token_dispatch(readin_buf);
        }
    }
    readin_hi = readin_buf;
    return readin_buf;
}

/* 0x3182: read and speak text until its end (getc returns -1) */
void clause_readin(int32_t (*getc)(stream_t *), stream_t *s)
{
    char *p;
    int32_t c, cls = 0;
    int8_t prev;                        /* the previous character's class */

    dttask_eof = 0;
    s->flags &= ~END_FLAGS;
    readin_depth = 0;
    readin_in_comment = 0;
    readin_after_bracket = 0;
    readin_break = 0;
    clause_marks_clear();
    newclause(0);
    readin_hi = p = readin_buf;
    for (;;) {
        prev = (int8_t)cls;
        c = getc(s);
        cls = rclass(c);
        if (readin_after_bracket) {
            readin_after_bracket = 0;
            if ((cls & RC_PUNCT) || c == ',') {
                emit_punctuation_symbol((int16_t)c);
                continue;
            }
        }
        /* , ! ? . then a line end or the end of the text: the clause ends here */
        if (!readin_depth && (cls & (RC_END | RC_BLANK)) && c != ' ' && (prev & RC_PUNCT)) {
            p = readin_flush((cls & RC_END) != 0);
            token_dispatch(NULL);
        }
        if ((cls & (RC_LINE | RC_END)) || (!readin_depth && (cls & RC_BLANK))) {
            if (cls & RC_END) readin_make_break();
            p = readin_flush((cls & RC_END) != 0);
            if (cls & RC_END) {
                if (readin_depth > 0) clause_putsym(SYM_SPACE, NULL);
                token_dispatch(NULL);
                newclause(0);
                readin_depth = 0;
                readin_in_comment = 0;
                if (c == -1) return;
            }
        }
        switch (cls & RC_ACTION) {
        case RC_CONTROL:
            switch (c) {
            case 0x02:
                goto open;
            case 0x03:
                goto close;
            case 0x08:                  /* backspace */
                if (p > readin_buf) {
                    p--;
                    continue;
                }
                goto spell;
            case 0x0e:
            case 0x0f:
                goto spell;
            case '\t':
            case ' ':
                if (readin_depth <= 0) continue;
                c = ' ';                /* a blank inside [ ] belongs to the phonemic text */
                goto store;
            case '[':
                if (dt_mode & MODE_SQUARE) goto open;
                goto store;
            case ']':
                if (dt_mode & MODE_SQUARE) goto close;
                goto store;
            default:
                continue;
            }
        case RC_STORE:
            goto store;
        case RC_SPELL:
            goto spell;
        default:
            continue;
        }
    store:
        if (p >= readin_hi || (rclass((int8_t)*p) & RC_RANK) <= (cls & RC_RANK)) {
            if (p >= readin_buf + READIN_MAX) {
                *p = 0;
                log_debug(readin_long_fmt, readin_buf);
                p = readin_flush(0);
            }
            *p = (char)c;
        }
        p++;
        if (p > readin_hi) readin_hi = p;
        continue;
    open:
        p = readin_flush(0);
        if ((tx_fixes & TX_FIX_MARKS) && readin_depth == 0) {  /* [DTC01]: the break waits for what is inside */
            readin_break = 1;
            readin_open_prev = prev;
        } else {
            readin_break = 0;
            token_dispatch(NULL);
        }
        readin_depth++;
        continue;
    close:
        if (readin_break) {                     /* [DTC01] TX_FIX_MARKS: only index marks inside? */
            int16_t sym[MARKS_IN_BRACKET], val[MARKS_IN_BRACKET];
            int n = -1, i;
            *readin_hi = 0;
            if (readin_depth == 1 && readin_hi > readin_buf && !readin_in_dv && !readin_in_comment)
                n = phonemic_marks_only(readin_buf, sym, val, MARKS_IN_BRACKET);
            if (n > 0) {                        /* as if the brackets were not there: the marks wait for their word */
                for (i = 0; i < n; i++) clause_mark_defer(sym[i], val[i], tx_tokens);
                readin_break = 0;
                readin_depth = 0;
                p = readin_hi = readin_buf;
                cls = readin_open_prev;
                continue;
            }
            readin_make_break();
        }
        p = readin_flush(1);
        clause_putsym(SYM_SPACE, NULL);
        if (readin_depth - 1 <= 0) {    /* subq.b then bgt: -128 - 1 counts as <= 0 as well */
            readin_depth = 0;
            readin_in_comment = 0;
            readin_after_bracket = 1;
        } else {
            readin_depth--;
        }
        continue;
    spell:
        readin_make_break();
        p = readin_flush(1);
        if (c == 0x1b) {
            token_dispatch(readin_escape);
            token_dispatch(NULL);
        } else {
            token_dispatch(readin_control);
            token_dispatch(NULL);
            readin_buf[0] = (char)(c + 0x40);
            spell_chars(readin_buf, readin_buf + 1);
        }
        clause_putsym(SYM_COMMA, NULL);
    }
}

/* 0xf9ea: the next character of the text pipe; the 0x1A marker (DT_SYNC) ends the text: -1 from then on */
static int32_t dttask_getc(stream_t *s)
{
    int32_t c;
    if (dttask_eof) return -1;
    c = dev_getc(s->dev);
    if (dt_log & LOG_INPUT) console_putchar_caret((uint32_t)c);
    if (c == 0x1a) {
        dttask_eof = 1;
        c = -1;
    }
    return c;
}

/* 0xf946: the dttask task (priority 10) */
void dttask_main(void)
{
    msg_t *msg;
    if (!pipe_open(&dttask_in, &cur_stream, 0x40)) {
        kprintf("Bug: call to fpipe in dttask failed\n");       /* ROM 0x18a7a */
        panic(NULL);
    }
    mbox_init_pool(&dttask_pool, 2, 2);
    for (;;) {
        clause_readin(dttask_getc, dttask_in);
        msg = mbox_get(&dttask_pool);
        msg->nwords = 1;
        msg->data[0] = KLSYN_SYNC;
        mbox_put(&klsyn_mbox, msg);
    }
}
