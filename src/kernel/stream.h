/* The ROM's small stdio on character devices (stream.c; REFERENCE.md s15.16, s15.35). stream_t is in rtos.h. */
#ifndef STREAM_H
#define STREAM_H
#include <stdint.h>
#include "rtos.h"

#define STREAM_READ 0x01
#define STREAM_WRITE 0x02
#define STREAM_NUL_END 0x40         /* stream_printf ends its output with a NUL */
#define STREAM_CRLF 0x100           /* a CR goes out before each LF */

extern stream_t *cur_stream;        /* 0x81f1e: the text pipe's writing end (dttask opens it, the host side writes) */

int32_t stream_putc_to(stream_t *s, int32_t c);                  /* putc, inlined in the ROM */
int32_t stream_putc(int32_t c);                                  /* 0xd99e: putc(c, cur_stream) */
int32_t stream_flsbuf(int32_t c, stream_t *s);                   /* 0x11efe (_flsbuf) */
int32_t stream_flush(stream_t *s);                               /* 0x11ec4 (fflush_if_pending) */
int32_t stream_printf(stream_t *s, const char *fmt, ...);        /* 0x11c88 (fprintf), 0x12556 (_doprnt) */

#endif
