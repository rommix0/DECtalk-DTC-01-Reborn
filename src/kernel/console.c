/* The console printer (REFERENCE.md s15.30): kprintf and the routines under it, rebuilt in C.
 *
 * kprintf is the ROM's own small printf, not the C library's: it knows only %d (or %D), %c and %s. Any other letter
 * after a % is printed as it is and takes no argument, so "0x%x" prints "0xx". Every character goes out through
 * console_putchar_caret, which shows control characters as ^X (LF as CR LF), bit 7 as a ~ before the character, and
 * ESC, DCS, CSI and ST by name. The console lock (dev_control) is held around each call, as in the ROM.
 */
#include <stdarg.h>
#include "rtos.h"
#include "console.h"

#define LOCK (-0xffff)
#define UNLOCK (-0x1ffff)

/* 0xd3e4: one character, as the console shows it */
void console_putchar_caret(uint32_t c)
{
    const char *name;
    dev_control(&console_dev, LOCK);
    switch (c) {
    case 0x1b: name = "<ESC>"; break;   /* ROM 0x18226 */
    case 0x90: name = "<DCS>"; break;   /* 0x1822c */
    case 0x9b: name = "<CSI>"; break;   /* 0x18232 */
    case 0x9c: name = "<ST>"; break;    /* 0x18238 */
    default:
        c &= 0xff;
        if (c & 0x80) {
            dev_putc(&console_dev, '~');
            c &= ~0x80u;
        }
        if (c < 0x20 && c != '\n' && c != 8) {
            dev_putc(&console_dev, '^');
            c += 0x40;
        }
        if (c == '\n') dev_putc(&console_dev, '\r');
        dev_putc(&console_dev, (int32_t)c);
        dev_control(&console_dev, UNLOCK);
        return;
    }
    console_print_string(name);
    dev_control(&console_dev, UNLOCK);
}

/* 0xd38c: a string; the characters are sign-extended, so bytes 0x80-0xff print as ~ and their low 7 bits */
void console_print_string(const char *s)
{
    dev_control(&console_dev, LOCK);
    for (; *s; s++) console_putchar_caret((uint32_t)(int32_t)(int8_t)*s);
    dev_control(&console_dev, UNLOCK);
}

/* 0xd334: a number >= 0, most significant digit first */
void console_print_decimal(int32_t v)
{
    int32_t q = v / 10;
    if (q) console_print_decimal(q);
    console_putchar_caret((uint32_t)(v % 10 + '0'));
}

/* 0xd260: the formatter under kprintf and the DT_LOG loggers */
void vformat_string(const char *fmt, va_list ap)
{
    int32_t c, v;
    dev_control(&console_dev, LOCK);
    for (;;) {
        c = (int8_t)*fmt++;
        if (!c) break;
        if (c != '%') {
            console_putchar_caret((uint32_t)c);
            continue;
        }
        c = (int8_t)*fmt++;             /* a % at the very end takes the NUL and reads on, as in the ROM */
        if (c == 'D' || c == 'd') {
            v = va_arg(ap, int32_t);
            if (v < 0) {
                console_putchar_caret('-');
                v = -v;
            }
            console_print_decimal(v);
        } else if (c == 'c') {
            console_putchar_caret((uint32_t)va_arg(ap, int32_t));
        } else if (c == 's') {
            console_print_string(va_arg(ap, const char *));
        } else {
            console_putchar_caret((uint32_t)c);     /* the letter itself; no argument is taken */
        }
    }
    dev_control(&console_dev, UNLOCK);
}

/* 0xd248 */
int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vformat_string(fmt, ap);
    va_end(ap);
    return 0;
}
