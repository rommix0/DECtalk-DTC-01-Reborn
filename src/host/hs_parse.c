/* The host line's escape-sequence parser (REFERENCE.md s5.1, s5.5, s5.6, s15.6, s15.35): the host task's loop
 * and the character level under it, rebuilt in C.
 *
 * read_host_char_collect_csi returns one "logical character" at a time: a plain byte, or, once an escape sequence is
 * complete, its lead-in (ESC, CSI 0x9b or DCS 0x90) with the sequence in host_seq. ESC followed by 0x40-0x5f is the
 * 7-bit form of a C1 code. host_task_main acts on each: control characters, the ESC and CSI commands, DECtalk's DCS
 * commands, and graphic characters, which go into the text pipe to dttask through the current character sets.
 */
#include <stdarg.h>
#include <string.h>
#include "host.h"
#include "console.h"

host_seq_t host_seq;
int16_t host_last_char;
int16_t dt_terminal = 6;
int16_t host_modem;
int16_t speak_enabled = 1;
int8_t phone_standalone;
int16_t phone_offhook;
int16_t keypad_timeout;
int16_t answer_rings;
int16_t line_speed[2];
int16_t line_format[2];
int16_t setup_interrupt_char;
int8_t c1_receive_7bit;
int8_t c1_transmit_7bit;
int32_t single_shift;
int32_t gl_set = 'B', gr_set = '<';
int32_t g_set[4] = { 'B', 'B', '<', '<' };
int8_t phone_dialing;
int16_t keypad_ticks;
int16_t host_idle;

/* 0xd1ca, 0xd21e, 0xd1f4: the DT_LOG loggers (LOG_ERROR 0x20, LOG_TRACE 0x40, the debug bit 0x80), on the console */
void log_error(const char *fmt, ...)
{
    va_list ap;
    if (!(dt_log & 0x20)) return;
    va_start(ap, fmt);
    vformat_string(fmt, ap);
    va_end(ap);
}

void log_trace(const char *fmt, ...)
{
    va_list ap;
    if (!(dt_log & 0x40)) return;
    va_start(ap, fmt);
    vformat_string(fmt, ap);
    va_end(ap);
}

void log_debug(const char *fmt, ...)
{
    va_list ap;
    if (!(dt_log & 0x80)) return;
    va_start(ap, fmt);
    vformat_string(fmt, ap);
    va_end(ap);
}

/* 0xefc8: the next byte from the host line. Any byte but XON ends the stand-alone phone mode of the power-up (the
 * phone task is told to reset); DT_LOG 0x04 copies the byte to the console as it is, 0x08 in its visible form. */
int32_t read_host_byte(void)
{
    int32_t c = dev_getc(&host_dev);
    host_idle = 0;
    if (phone_standalone && c != 0x11) {
        phone_standalone = 0;
        answer_rings = 0;
        dev_control(&phone_dev, DEV_POST, PHONE_EV_RESET);
    }
    if (dt_log & 0x04) dev_putc(&console_dev, c);
    if (dt_log & 0x08) console_putchar_caret((uint32_t)c);
    return c;
}

/* start collecting a sequence with lead-in c */
static void seq_start(host_seq_t *s, int32_t c)
{
    s->lead = (int16_t)c;
    s->nint = 0;
    s->priv = 0;
    s->p[0] = 0;
    s->p[1] = 0;
}

/* 0x11290: the next logical character (see the header comment). Inside a sequence: CAN (0x15), SUB (0x1a) and C1
 * codes abort it and are returned; other C0 codes and DEL are returned and the sequence goes on; 0x20-0x2f are
 * intermediates (at most 2); for CSI and DCS, a '<' '=' '>' '?' first is the private marker, digits and ';' are
 * parameters (at most 16, each at most 65535), anything else in 0x30-0x3f makes the sequence malformed, and
 * 0x40-0x7e ends it. strip8 (DECTC1) drops bit 7 of every byte. */
int32_t read_host_char_collect_csi(host_seq_t *s, int32_t (*getc)(void), int strip8)
{
    int32_t c, u;
    for (;;) {
        c = getc();
        if (strip8) c &= 0x7f;
        for (;;) {
            if (c == 0x1b || c == 0x9b || c == 0x90) {
                seq_start(s, c);
                break;
            }
            if (!s->lead) return c;
            if ((c >= 0x80 && c < 0xa0) || c == 0x15 || c == 0x1a) {
                s->lead = 0;
                return c;
            }
            if (c < 0x20 || c == 0x7f || c > 0xfe) return c;
            u = c & 0x7f;
            if (u < 0x30) {                     /* an intermediate */
                if (s->nint < 2) s->inter[s->nint++] = (char)u;
                else s->nint = 3;
                break;
            }
            if (s->lead != 0x1b) {              /* CSI or DCS */
                if (u >= 0x40) {
                    if (!s->p[0]) s->p[0] = 1;
                    goto final;
                }
                if (u < 0x3c) {
                    if (!s->p[0]) s->p[0] = 1;
                    if (s->nint) {              /* an intermediate before a parameter */
                        s->nint = 0;
                        s->priv = -1;
                    }
                    if (u < 0x3a) {             /* a digit */
                        if (s->p[0] < 17) {
                            uint16_t *p = &s->p[s->p[0]];
                            if (*p > 0x1998 + (u - '0' < 6)) *p = 0xffff;
                            else *p = (uint16_t)(*p * 10 + u - '0');
                        }
                        break;
                    }
                    if (u == ';') {
                        if (s->p[0] < 16) s->p[++s->p[0]] = 0;
                        else s->p[0] = 17;
                        break;
                    }
                } else if (!s->p[0]) {          /* the private marker */
                    s->priv = (int8_t)('@' - u);
                    s->p[0]++;
                    break;
                }
                s->priv = -1;                   /* ':' or a late marker */
                break;
            }
            if (s->nint || (c & 0x3f) > 0x1f) goto final;
            c = u + 0x40;                       /* ESC 0x40-0x5f: the C1 code */
        }
        continue;
    final:
        if (s->p[0] > 16) s->p[0] = 16;
        s->final = (char)u;
        c = s->lead;
        s->lead = 0;
        return c;
    }
}

static int32_t next_char(void)
{
    return read_host_char_collect_csi(&host_seq, read_host_byte, c1_receive_7bit);
}

/* 0xd906 */
int is_graphic_char(int32_t c)
{
    return (c >= 0x20 && c <= 0x7e) || (c >= 0xa0 && c <= 0xfe);
}

/* 0xd94c: a graphic character through character set `set`: ASCII passes, DEC supplemental is spoken as its base
 * letter, anything else (DEC special graphics) is dropped */
void output_graphic_char(int32_t c, int32_t set)
{
    if (set == 'B' || (set == '<' && (c = dec_supplemental_ascii[c - 0x20]) != 0)) stream_putc(c);
}

/* 0xd83e: SS2 or SS3 (n = 2, 3): the next graphic character, possibly after a SO or SI, comes from G2 or G3 */
int32_t handle_single_shift(int32_t n)
{
    int32_t c = next_char();
    if (!is_graphic_char(c)) {
        if (c != 0x0f && c != 0x0e) return c;
        gl_set = c == 0x0e ? g_set[1] : g_set[0];
        c = next_char();
        if (!is_graphic_char(c)) return c;
    }
    single_shift = n;
    return c;
}

/* 0xd9f4: the text of a control string (OSC, PM, APC, or the text after a DCS command's 'z'), up to the next code
 * that is not text; each character goes to put (none: dropped). SO and SI still switch GL; HT-CR and 0x20-0x7e pass
 * as they are, 0xa0-0xfe as their DEC supplemental base letter. The code that ends it (ST, ESC, ...) is returned. */
int32_t consume_control_string(void (*put)(int32_t c))
{
    int32_t c;
    for (;;) {
        c = next_char();
        if (c == 0x0f) {
            gl_set = g_set[0];
            continue;
        }
        if (c == 0x0e) {
            gl_set = g_set[1];
            continue;
        }
        if ((c >= 8 && c <= 0x0d) || (c >= 0x20 && c <= 0x7e)) {
            if (put) put(c);
            continue;
        }
        if (c < 0xa0 || c > 0xfe) return c;
        c = dec_supplemental_ascii[c - 0xa0];
        if (c && put) put(c);
    }
}

/* 0xdad8: an ESC sequence (host_seq has the final and the intermediates) */
int32_t dispatch_esc_command(void)
{
    host_seq_t *s = &host_seq;
    switch (s->final) {
    case 'G':
        if (s->nint == 1 && s->inter[0] == ' ') {
            log_trace("S8C1T: select 8 bit C1 transmission\n");
            c1_transmit_7bit = 0;
        }
        return 0;
    case '6':
        if (s->nint == 1 && s->inter[0] == ' ') {
            log_trace("DECTC1: truncate C1 controls\n");
            c1_receive_7bit = 1;
        }
        return 0;
    case '7':
        if (s->nint == 1 && s->inter[0] == ' ') {
            log_trace("DECAC1: accept C1 controls\n");
            c1_receive_7bit = 0;
        }
        return 0;
    case '<':
    case 'B':
        if (!s->nint) {
            log_error("missing Gn designation\n");
            return 0;
        }
        log_debug("SGR: I = %c F = %c\n", (int32_t)s->inter[0], (int32_t)s->final);
        switch (s->inter[0]) {
        case '(': g_set[0] = s->final; break;
        case ')': g_set[1] = s->final; break;
        case '*': g_set[2] = s->final; break;
        case '+': g_set[3] = s->final; break;
        default: log_error("bad Gn designation %c\n", (int32_t)s->inter[0]); break;
        }
        return 0;
    case 'F':
        if (s->nint == 1 && s->inter[0] == ' ') {
            log_trace("S7C1T: select 7 bit C1 transmission\n");
            c1_transmit_7bit = 1;
        }
        return 0;
    case 'o':
        log_debug("LS3: GL = %c\n", g_set[3]);
        gl_set = g_set[3];
        return 0;
    case 'c':
        log_trace("RIS: reset\n");
        settings_reset(0, 0);
        return 0;
    case 'n':
        log_debug("LS2: GL = %c\n", g_set[2]);
        gl_set = g_set[2];
        return 0;
    case '|':
        log_debug("LS3R: GL = %c\n", g_set[3]);
        gr_set = g_set[3];
        return 0;
    case '}':
        log_debug("LS2R: GL = %c\n", g_set[2]);
        gr_set = g_set[2];
        return 0;
    case '~':
        log_debug("LS1R: GL = %c\n", g_set[1]);
        gr_set = g_set[1];
        return 0;
    }
    return 0;
}

/* 0xd59e: the host task */
void host_task_main(void)
{
    int32_t c, set, ss;
    host_line_putc(0x11);                       /* XON */
    for (;;) {
        c = 0;
    again:
        for (;;) {
            if (!c) {
                c = next_char();
                host_last_char = (int16_t)c;
            }
            if (c == 0x8e) c = handle_single_shift(2);
            else if (c == 0x8f) c = handle_single_shift(3);
            else break;
        }
        switch (c) {
        case 0x00: case 0x11: case 0x13: case 0x7f: case 0x9c: case 0xff:
            continue;                           /* NUL, XON, XOFF, DEL, ST and 0xff are dropped */
        case 0x0e:
            gl_set = g_set[1];                  /* SO: LS1 */
            continue;
        case 0x0f:
            gl_set = g_set[0];                  /* SI: LS0 */
            continue;
        case 0x0d:
            c = '\n';                           /* CR is a line feed */
            break;
        case 0x1a:
            if (speak_enabled) stream_putc(0x0b);   /* SUB (a character received in error) ends the clause */
            continue;
        case 0x1b:
            c = dispatch_esc_command();
            goto again;
        case 0x90:
            c = dcs_command_dispatch();
            goto again;
        case 0x9b:
            c = csi_command_dispatch();
            goto again;
        case 0x9a:                              /* DECID: as DA */
            log_trace("%s: primary device attribute request\n", "OLDID");
            send_escape_response(0x9b, &da_reply);
            continue;
        case 0x9d: case 0x9e: case 0x9f:
            log_trace("OSC/PM/APC ignored\n");
            c = consume_control_string(NULL);
            goto again;
        }
        if (c < 0) {
            log_error("bad sequence %d\n", c);
            host_seq.lead = 0;
            continue;
        }
        ss = single_shift;
        if (ss) {
            single_shift = 0;
            if (!speak_enabled) continue;
            set = g_set[ss];
        } else {
            if (c < 0x20) {                     /* the other C0 codes go to the pipe as they are */
                if (speak_enabled) stream_putc(c);
                continue;
            }
            if (c <= 0x7f) {
                set = gl_set;
            } else {
                if (c < 0xa0) {
                    log_error("Bad C1 control %c\n", c);
                    continue;
                }
                set = gr_set;
            }
            if (!speak_enabled) continue;
        }
        output_graphic_char(c & 0x7f, set);
    }
}
