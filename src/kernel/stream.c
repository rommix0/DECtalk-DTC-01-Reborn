/* The ROM's small stdio on character devices (REFERENCE.md s15.16, s15.35): putc, _flsbuf, the flush and
 * fprintf with its engine _doprnt, rebuilt in C.
 *
 * A stream is {cnt, ptr, base, flags, dev}. fdopen makes it unbuffered (base NULL, cnt 0), so every putc goes to
 * stream_flsbuf and from there to dev_putc; that is how the host side writes the text pipe. A buffered stream would
 * collect up to 0x200 bytes first.
 *
 * _doprnt is not kprintf (console.c): it is a real printf, with the flags '-' and '0', a width and a precision
 * (digits, or '*' or '?' for an argument), an 'l' or 'L' size letter, and the conversions b d o u x (and B D O U X,
 * which are unsigned: only a small d prints a minus), c and s ({NULL} for a null pointer). The precision also cuts
 * numbers. Hex digits are small letters for x and X alike. Two more conversions exist in the ROM, q (a fixed-point
 * number) and r (a nested format); nothing on the host side uses them, so they are not rebuilt (panic).
 */
#include <stdarg.h>
#include <string.h>
#include "stream.h"

void panic(const char *msg);

int32_t stream_putc_to(stream_t *s, int32_t c)
{
    if (s->cnt-- < 1) return stream_flsbuf(c, s);
    *s->ptr++ = (char)c;
    return (int32_t)(int8_t)s->ptr[-1];
}

/* 0xd99e */
int32_t stream_putc(int32_t c)
{
    return stream_putc_to(cur_stream, c);
}

/* 0x11efe: write out the buffer, then c; a new buffer allowance of 0x200 (0 when unbuffered) */
int32_t stream_flsbuf(int32_t c, stream_t *s)
{
    char *p;
    if (!(s->flags & STREAM_WRITE)) return -1;
    if (s->base)
        for (p = s->base; p < s->ptr; p++) {
            if (*p == '\n' && (s->flags & STREAM_CRLF)) dev_putc(s->dev, '\r');
            dev_putc(s->dev, (int32_t)(int8_t)*p);
        }
    if (c == '\n' && (s->flags & STREAM_CRLF)) dev_putc(s->dev, '\r');
    dev_putc(s->dev, c);
    s->ptr = s->base;
    s->cnt = s->base ? 0x200 : 0;
    return c;
}

/* 0x11ec4: flush only when something is buffered (the NUL that _flsbuf then writes goes out too) */
int32_t stream_flush(stream_t *s)
{
    return s->base < s->ptr ? stream_flsbuf(0, s) : 0;
}

/* the conversions with a number: {letter, unsigned, base}, ROM 0x1d3d4 */
static const struct { char conv, uns, base; } num_convs[] = {
    {'b', 0, 2}, {'d', 0, 10}, {'o', 0, 8}, {'u', 0, 10}, {'x', 0, 16},
    {'B', 1, 2}, {'D', 1, 10}, {'O', 1, 8}, {'U', 1, 10}, {'X', 1, 16},
};

/* 0x12556 */
static int32_t doprnt(const char *fmt, va_list ap, stream_t *s)
{
    char buf[34], *p;
    int32_t c, width, prec, len, n, i, left;
    char pad;
    for (;;) {
        c = (int8_t)*fmt++;
        if (!c) break;
        if (c != '%') {
            stream_putc_to(s, c);
            continue;
        }
        pad = ' ';
        c = (int8_t)*fmt++;
        left = c == '-';
        if (left) c = (int8_t)*fmt++;
        if (c == '0') {
            pad = '0';
            c = (int8_t)*fmt++;
        }
        width = 0;
        if (c == '?' || c == '*') {
            width = va_arg(ap, int32_t);
            c = (int8_t)*fmt++;
        } else {
            while (c >= '0' && c <= '9') {      /* _ctype bit 4 */
                width = width * 10 + c - '0';
                c = (int8_t)*fmt++;
            }
        }
        prec = 0x7fff;
        if (c == '.') {
            c = (int8_t)*fmt++;
            prec = 0;
            if (c == '?' || c == '*') {
                prec = va_arg(ap, int32_t);
                c = (int8_t)*fmt++;
            } else {
                while (c >= '0' && c <= '9') {
                    prec = prec * 10 + c - '0';
                    c = (int8_t)*fmt++;
                }
            }
        }
        if (c == 'l' || c == 'L') {             /* a size letter makes the conversion unsigned */
            c = (int8_t)*fmt;
            if (c == 'X' || c == 'B' || c == 'D' || c == 'O' || c == 'U') fmt++;
            else if (c == 'b' || c == 'd' || c == 'o' || c == 'u' || c == 'x') { c -= 0x20; fmt++; }
            else c = 'U';
        }
        for (i = 0; i < (int32_t)(sizeof num_convs / sizeof num_convs[0]) && num_convs[i].conv != c; i++) {}
        if (i < (int32_t)(sizeof num_convs / sizeof num_convs[0])) {
            uint32_t v = va_arg(ap, uint32_t);
            int neg = 0, base = num_convs[i].base;
            if (!num_convs[i].uns && c == 'd' && (int32_t)v < 0) {
                v = (uint32_t)-(int32_t)v;
                neg = 1;
            }
            p = buf + sizeof buf - 1;
            *p = 0;
            if (!v) {
                *--p = '0';
            } else {
                do {
                    n = (int32_t)(v % (uint32_t)base);
                    *--p = (char)(n + (n < 10 ? '0' : 'W'));
                    v /= (uint32_t)base;
                } while (v);
                if (neg) *--p = '-';
            }
        } else if (c == 'c') {
            buf[0] = (char)va_arg(ap, int32_t);
            buf[1] = 0;
            p = buf;
        } else if (c == 's') {
            p = va_arg(ap, char *);
            if (!p) p = "{NULL}";
        } else if (c == 'q' || c == 'r') {
            panic("stream_printf: %q and %r are not rebuilt");
            return 0;
        } else {                                /* any other letter prints itself */
            buf[0] = (char)c;
            buf[1] = 0;
            p = buf;
        }
        len = (int32_t)strlen(p);
        if (len > prec) len = prec;
        if (!left)
            while (--width >= len) stream_putc_to(s, pad);
        for (n = prec; *p && n > 0; n--) stream_putc_to(s, *p++);
        if (left)
            while (--width >= len) stream_putc_to(s, ' ');
    }
    return (s->flags & STREAM_NUL_END) ? stream_putc_to(s, 0) : 0;
}

/* 0x11c88 */
int32_t stream_printf(stream_t *s, const char *fmt, ...)
{
    va_list ap;
    int32_t r;
    va_start(ap, fmt);
    r = doprnt(fmt, ap, s);
    va_end(ap);
    return r;
}
