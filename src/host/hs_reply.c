/* Replies on the host line (REFERENCE.md s5.1, s15.6, s15.35): the sequence writer and its callers, rebuilt in C.
 * A reply is built as a host_seq_t, the parser's own layout, and written with send_control_sequence.
 */
#include "host.h"
#include "console.h"

/* 0xef90 (formerly echo_digit_to_host): one byte to the host line; DT_LOG 0x10 (LOG_OUTHOST) shows it on the console */
void host_line_putc(int32_t c)
{
    if (dt_log & 0x10) console_putchar_caret((uint32_t)c);
    dev_putc(&host_dev, c);
}

/* 0x1167c: v in decimal, without leading zeros (by subtracting powers of ten, ROM table 0x19862) */
void uint_to_decimal_string(char *out, uint32_t v)
{
    static const uint32_t pow10[7] = { 10000000, 1000000, 100000, 10000, 1000, 100, 10 };
    int i, started = 0;
    for (i = 0; i < 7; i++) {
        *out = '0';
        for (; pow10[i] <= v; v -= pow10[i]) (*out)++;
        if (started || *out != '0') {
            out++;
            started = 1;
        }
    }
    out[0] = (char)(v + '0');
    out[1] = 0;
}

/* 0x114e0: the code c (a C1 code as ESC and a letter when seven_bit), then for ESC, CSI and DCS the sequence in s:
 * the private marker, the parameters separated by ';' (a zero parameter is left empty), the intermediates and the
 * final character */
void send_control_sequence(int32_t c, const host_seq_t *s, void (*put)(int32_t c), int seven_bit)
{
    char digits[12], *d;
    uint32_t n, i;
    if (seven_bit && c >= 0x80 && c < 0xa0) {
        put(0x1b);
        put(c - 0x40);
    } else {
        put(c);
    }
    if (c != 0x1b && c != 0x9b && c != 0x90) return;
    n = s->p[0];
    if (n > 16) n = 16;
    for (i = 1; i <= n; i++) {
        if (i != 1) put(';');
        else if (s->priv > 0) put(0x40 - s->priv);
        if (s->p[i]) {
            uint_to_decimal_string(digits, (uint32_t)(int32_t)(int16_t)s->p[i]);
            for (d = digits; *d; d++) put(*d);
        }
    }
    n = (uint32_t)s->nint;
    if (n > 2) n = 2;
    for (i = 0; i < n; i++) put(s->inter[i]);
    if (s->final) put(s->final);
}

/* 0xef5c */
void send_escape_response(int32_t c, const host_seq_t *s)
{
    send_control_sequence(c, s, host_line_putc, c1_transmit_7bit);
}

/* 0xef00: the R2/R3 reply, DCS 0 ; r2 ; r3 z ST (the zero P1 is left empty: ESC P ; 70 ; 1 z ESC \) */
void send_dcs_reply(int16_t r2, int16_t r3)
{
    host_seq_t s = { 0 };
    s.final = 'z';
    s.p[0] = 3;
    s.p[2] = (uint16_t)r2;
    s.p[3] = (uint16_t)r3;
    send_escape_response(0x90, &s);
    s.final = 0;
    s.p[0] = 0;
    send_escape_response(0x9c, &s);
}
