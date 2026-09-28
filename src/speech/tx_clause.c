/* The clause buffer (REFERENCE.md s15.28): clause_putsym and newclause, the end of the text pipeline.
 *
 * Everything the text side says arrives here as 16-bit symbols: phonemes 0-55, the marks 56-69 (0x38 # between the
 * parts of a hyphenated word, 0x39-0x3b stress, 0x3c syllable, 0x3d *, 0x3e word boundary, 0x3f (, 0x40 ), 0x41 [
 * the clause start, 0x42 , 0x43 ! 0x44 ? 0x45 .), codes above them, and symbols carrying values (bits 12-13 = the
 * number of value words that follow, e.g. a sung phoneme's duration and pitch). A clause is one message of
 * klsyn_free_pool; newclause posts it to klsyn_mbox, where the klsyn task picks it up, and takes the next.
 *
 * clause_putsym keeps one mark per boundary: a weaker mark right after a mark is dropped, a stronger one replaces it.
 * A , ! ? or . ends the clause, and so does a full buffer: past 174 words the next word boundary becomes a comma, and
 * at 199 words any symbol does. With DT_LOG phoneme (0x02) newclause writes each clause by name to the console.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "tx_text.h"
#include "tx_rom.h"
#include "../kernel/rtos.h"

#define CLAUSE_WORDS 199                /* the payload the ROM uses: end = payload + 0x18e bytes */
#define CLAUSE_SOFT 25                  /* soft end = end - 0x32 bytes */
#define SYM_SILENCE 0
#define SYM_HYPHEN 0x38
#define SYM_STAR 0x3d                   /* the lowest boundary mark */
#define SYM_SPACE 0x3e                  /* word boundary */
#define SYM_OPEN 0x3f                   /* ( */
#define SYM_CLOSE 0x40                  /* ) */
#define SYM_START 0x41                  /* [ : a clause's first symbol */
#define SYM_COMMA 0x42
#define SYM_PERIOD 0x45                 /* . : the highest mark */
#define LOG_PHONEME 0x02                /* dt_log bit */
#define MODE_ASKY 0x02                  /* dt_mode bit: the 1-character phoneme alphabet */
#define CT_SPACE 0x08                   /* ctype_tab bit */

extern mbox_t klsyn_free_pool;          /* 0x80790 (ph_task.c) */
extern mbox_t klsyn_mbox;               /* 0x807a2 */

msg_t *clause_msg;
int16_t *clause_wr, *clause_end, *clause_soft_end, *clause_last, *clause_mark;
int16_t clause_run;
int16_t clause_bound;
int8_t clause_silence;
int16_t name_nval, name_nvalues;
int8_t name_in_frames, name_pseudo;
char name_buf[32];
void (*tx_log_hook)(const char *text);


/* the log stream stdout_ (0x8231a, the console), at printf level: the stream adds CR before LF */
static void log_printf(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (tx_log_hook) tx_log_hook(buf);
}

static void log_putc(char c)
{
    char s[2];
    s[0] = c;
    s[1] = 0;
    if (tx_log_hook) tx_log_hook(s);
}

/* 0xd50c: a duration in 6.4 ms frames -> ms (-1 stays -1), cut to 16 bits */
static int16_t frames_to_ms(int16_t v)
{
    if (v != -1) v = (int16_t)((int32_t)v * 64 / 10);
    return v;
}

/* 0x10d9c: the name of symbol c for the log. A symbol with values returns NULL, and the value words that follow are
 * appended one call at a time: "name<v1,v2>" (the first value of a phoneme 0-69 is a duration, shown in ms), or
 * "name v " for a 0x4000 symbol, whose value comes as the next plain word. The last call returns the whole text. */
const char *phoneme_name(int16_t c)
{
    char *p = name_buf;
    if (name_nvalues <= 0) {
        name_nvalues = 0;
        if ((c & 0xff) > sym_max) {
            sprintf(name_buf, sym_bad_fmt, (int)c);
        } else {
            const tx_sym_t *rec = &sym_table[c & 0xff];
            if (dt_mode & MODE_ASKY) {
                name_buf[0] = rec->ascii;
                name_buf[1] = 0;
            } else {
                sprintf(name_buf, sym_name_fmt, rec->name ? rec->name : "");   /* NULL: ROM address 0, which holds 0 */
            }
            if (c & 0x3000) {
                name_nvalues = (int16_t)((c & 0x3000) >> 12);
                name_in_frames = rec <= &sym_table[69];
                name_pseudo = (rec->code & 0x4000) != 0;
            } else if (rec->code & 0x4000) {
                name_pseudo = 1;
                name_nvalues = 1;
            }
        }
    } else {
        while (*p) p++;
        *p++ = name_pseudo ? ' ' : name_nval == 0 ? '<' : ',';
        sprintf(p, sym_value_fmt, (unsigned)(int32_t)(name_in_frames ? frames_to_ms(c) : c));
        name_in_frames = 0;
        name_nval++;
        if (name_nval >= name_nvalues) {
            strcat(p, name_pseudo ? sym_end_pseudo : sym_end_values);
            name_nvalues = 0;
            name_nval = 0;
            return name_buf;
        }
    }
    return name_nvalues > 0 ? NULL : name_buf;
}

/* 0x3a3e: DT_LOG phoneme. The clause's symbols by name, a new line before column 79 (or 71 at a word boundary) and
 * after each period; the text of a [:...] command as ":dv" and its characters. The ROM passes stdout_ and the name
 * "newclause", which it does not use. */
static void log_clause(const msg_t *msg)
{
    const int16_t *p = msg->data, *end = p + msg->nwords;
    int16_t col = 0, c;
    int8_t text = 0;
    char one[2];
    const char *s;
    if (*p == 0) {
        text = 1;
        p++;
    } else if (*p == SYM_START) {
        p++;
    }
    /* dev_control(&console_dev, lock) */
    if (text) {
        log_printf(log_dv);
        col = 3;
    }
    while (p < end) {
        c = *p++;
        if (text) {
            one[0] = (char)c;
            one[1] = 0;
            s = one;
        } else if (!(s = phoneme_name(c))) {
            continue;
        }
        col = (int16_t)(col + (int16_t)strlen(s));
        if (col > 78 || (col > 70 && ((text && (ctype_tab[c & 0x7f] & CT_SPACE)) || c == SYM_SPACE))) {
            log_putc('\n');
            col = (int16_t)strlen(s);
        }
        log_printf(log_fmt, s);
        if (p >= end || (text && c == ';') || c == SYM_PERIOD) log_putc('\n');
    }
    /* dev_control(&console_dev, unlock) */
}

/* ---- [DTC01] TX_FIX_MARKS: index marks waiting for their word, and kept out of the clause (tx_text.h) ----
 * A mark waits (clause_mark_defer) until the first word that came after it in the text is spoken. It then goes into
 * the clause's side list: its position (the index the next symbol will have) and the mark. It takes no room in the
 * clause, which the ROM fills to 199 words and cuts at 174 (with marks in it, a long sentence was cut elsewhere). The
 * side list goes to klsyn after the clause's words in the same message (3 words each: position, code, value; their
 * count in msg->pad6, which klsyn messages do not use), and klsyn puts each mark where its marker would have been
 * (ph_clause.c). A clause that holds nothing but marks gets them as symbols, as in the ROM, so that klsyn says it (a
 * short silence) and they are reported. */
#define MARKS_MAX 32
#define SIDE_MAX 64                     /* per clause; the message has room for 200 more words */

int tx_fixes;
static struct {
    int16_t sym, val;
    uint32_t token;
} marks[MARKS_MAX];
static int nmarks;
static struct {
    int16_t pos, sym, val;
} side[SIDE_MAX];
static int nside;

/* a mark's symbol, as parse_phonemic_text would put it */
static void put_inband(int16_t sym, int16_t val)
{
    int8_t silence = clause_silence;    /* a value symbol leaves clause_last alone; the silence rule stays too */
    clause_putsym(sym, &val);
    clause_silence = silence;
}

static void put_mark(int i)
{
    if (nside < SIDE_MAX && clause_msg) {
        side[nside].pos = (int16_t)(clause_wr - (int16_t *)clause_msg->data);
        side[nside].sym = marks[i].sym;
        side[nside++].val = marks[i].val;
    } else {
        put_inband(marks[i].sym, marks[i].val);
    }
}

void clause_mark_defer(int16_t sym, int16_t val, uint32_t token)
{
    if (nmarks == MARKS_MAX) {                  /* no room: the oldest goes now */
        put_mark(0);
        memmove(marks, marks + 1, sizeof marks[0] * (size_t)--nmarks);
    }
    marks[nmarks].sym = sym;
    marks[nmarks].val = val;
    marks[nmarks++].token = token;
}

void clause_marks_upto(uint32_t token)
{
    int i = 0;
    while (i < nmarks && (int32_t)(token - marks[i].token) >= 0) put_mark(i++);
    if (i) {
        nmarks -= i;
        memmove(marks, marks + i, sizeof marks[0] * (size_t)nmarks);
    }
}

void clause_marks_all(void)
{
    int i;
    for (i = 0; i < nmarks; i++) put_mark(i);
    nmarks = 0;
}

void clause_marks_clear(void)
{
    nmarks = 0;
    nside = 0;
}

/* newclause: the side list into the message being posted (TX_FIX_MARKS) */
static void attach_side(msg_t *msg)
{
    int i;
    int16_t *t;
    if (msg->nwords == 1 && nside) {            /* only the start: the marks as symbols, and klsyn says it */
        for (i = 0; i < nside; i++) put_inband(side[i].sym, side[i].val);
        msg->nwords = (int16_t)(clause_wr - msg->data);
        nside = 0;
    }
    t = msg->data + msg->nwords;
    for (i = 0; i < nside && t + 3 <= msg->data + MSG_MAXWORDS; i++) {
        *t++ = side[i].pos;
        *t++ = (int16_t)(side[i].sym & 0xff);
        *t++ = side[i].val;
    }
    msg->pad6 = (int16_t)i;
    nside = 0;
}

/* The clause holds only its start, and marks wait in its side list. A , ! ? or . here is not stored (the old mark is
 * the clause start), but the ROM posts the clause anyway, and klsyn skips it as empty. With marks it would be said, a
 * pause of its own ("brackets) [:in 7]-- and": the ) and the -- each end a clause); so it is kept instead, and its
 * marks go with what follows. */
static int only_marks(void)
{
    return (tx_fixes & TX_FIX_MARKS) && clause_msg && nside && clause_wr == (int16_t *)clause_msg->data + 1;
}

/* newclause(0) for a clause that is kept (only_marks): the state a new clause starts with, the marks left in it */
static void restart_clause(void)
{
    clause_silence = 0;
    clause_mark = clause_last;
    clause_run = 0;
    prev_word_stressed = 0;
    wh_question = -1;
}

/* the ROM moved the words after position d down by one: so do the side list's positions */
static void side_deleted(int16_t d)
{
    int i;
    for (i = 0; i < nside; i++)
        if (side[i].pos > d) side[i].pos--;
}

/* 0x3912: post the clause (if any) to klsyn and start the next: flag 0 = a phoneme clause (it starts with [ 0x41),
 * 1 = the text of a [:...] command (it starts with 0, then one character per word). */
void newclause(int32_t flag)
{
    msg_t *msg = clause_msg;
    clause_silence = 0;
    if (msg) {
        msg->nwords = (int16_t)(clause_wr - msg->data);
        if (tx_fixes & TX_FIX_MARKS) attach_side(msg);
        if (msg->nwords > 200) kprintf(newclause_size_fmt, (int)msg->nwords, 200);
        if (dt_log & LOG_PHONEME) log_clause(msg);
        mbox_put(&klsyn_mbox, msg);
    }
    clause_msg = msg = mbox_get(&klsyn_free_pool);
    clause_wr = msg->data;
    clause_end = clause_wr + CLAUSE_WORDS;
    clause_soft_end = clause_end - CLAUSE_SOFT;
    clause_mark = clause_wr;
    clause_last = clause_mark;
    *clause_wr++ = (int8_t)flag ? 0 : SYM_START;
    clause_run = 0;
    prev_word_stressed = 0;
    wh_question = -1;
}

/* 0x35f8: add one symbol to the clause. val: NULL, CLAUSE_RAW (a character of [:...] text, stored as it is), or the
 * value words of a symbol with bits 12-13 set. */
void clause_putsym(int32_t code, const int16_t *val)
{
    int16_t sym = (int16_t)code;        /* the ROM reads the low word */
    int32_t n;
    if (val == CLAUSE_RAW) {
        if (clause_wr >= clause_end) {
            kprintf(spdef_overflow_fmt, (int)(clause_end - clause_wr));
            *clause_wr++ = SYM_COMMA;
            newclause(1);
        }
        *clause_wr++ = sym;
        return;
    }
    n = sym & 0x3000;
    if (n) {
        if (!val) {
            kprintf(pvalue_bug_fmt, (int)sym);
            panic("phone has value, no pvalue[]");
        }
        n >>= 12;
        if (clause_wr + n >= clause_end) {
            *clause_wr++ = SYM_COMMA;
            newclause(0);
        }
        *clause_wr++ = sym;
        while (--n >= 0) *clause_wr++ = *val++;
        clause_silence = 0;
        return;
    }

    if (sym == SYM_SILENCE) {                   /* a second silence in a row is dropped */
        if (clause_silence) return;
        clause_silence = 1;
    } else {
        clause_silence = 0;
    }
    if (sym > SYM_OPEN && sym <= SYM_PERIOD) {  /* ) , ! ? . start a new run */
        clause_run = 0;
        if (*clause_last <= SYM_PERIOD) clause_mark = clause_last;
    }
    if (sym >= SYM_STAR && sym <= SYM_PERIOD && *clause_last >= SYM_STAR && *clause_last <= SYM_PERIOD) {
        /* a mark right after a mark: keep the stronger */
        if (sym == SYM_CLOSE && (clause_bound == SYM_CLOSE || clause_bound == SYM_OPEN)) return;
        if (*clause_last < sym) {
            if (sym == SYM_OPEN) {              /* ( only after 26 symbols, and then as ) */
                if (clause_run <= 25) return;
                sym = SYM_CLOSE;
                clause_run = 0;
            }
            if (clause_mark == clause_last && *clause_mark != SYM_START) {
                *clause_mark = sym;
            } else if (*clause_mark != SYM_START) {
                int16_t *p = clause_mark;
                if (tx_fixes & TX_FIX_MARKS) side_deleted((int16_t)(clause_mark - (int16_t *)clause_msg->data));
                while (p < clause_last - 1) {
                    p[0] = p[1];
                    p++;
                }
                clause_mark = p;
                p++;
                while (p < clause_wr - 1) {
                    p[0] = p[1];
                    p++;
                }
                clause_wr--;
                clause_last--;
                p[-1] = sym;
            }
        }
    } else if (sym == SYM_HYPHEN && *clause_last == SYM_SPACE) {
        *clause_last = SYM_HYPHEN;
    } else {
        if ((clause_wr >= clause_soft_end && sym == SYM_SPACE) || clause_wr >= clause_end) {
            *clause_wr++ = SYM_COMMA;
            newclause(0);
        }
        clause_last = clause_wr;
        *clause_wr++ = sym;
        clause_run++;
    }
    if (sym > SYM_STAR && sym <= SYM_PERIOD) {
        if (sym >= SYM_COMMA) {
            if (only_marks()) restart_clause();  /* [DTC01] TX_FIX_MARKS */
            else newclause(0);
        } else {
            clause_bound = sym;
        }
    }
}
