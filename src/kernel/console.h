/* The console printer (console.c; REFERENCE.md s15.30). */
#ifndef CONSOLE_H
#define CONSOLE_H
#include <stdarg.h>
#include <stdint.h>

int kprintf(const char *fmt, ...);                      /* 0xd248: only %d %D %c %s; see console.c */
void vformat_string(const char *fmt, va_list ap);       /* 0xd260 */
void console_print_decimal(int32_t v);                  /* 0xd334 */
void console_print_string(const char *s);               /* 0xd38c */
void console_putchar_caret(uint32_t c);                 /* 0xd3e4: control characters as ^X, LF as CR LF */

#endif
