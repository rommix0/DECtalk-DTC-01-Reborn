/* dtc01term's devices (REFERENCE.md s17.14): what the ROM's DUART driver and interrupt did for the host line
 * (host_dev) and the local terminal (console_dev), over term_line.c's lines; and an idle phone (phone_dev).
 *
 * The host line keeps the ROM's rules (duart_rx_char 0x1870, the XON hook 0x1996, the set-up 0x15d0): an input ring
 * of 304 bytes; a received byte that finds more than 64 waiting sends XOFF (once); a read that leaves fewer than 16
 * sends XON. A BREAK is read as 0, a plain NUL is dropped, and the host's own XON/XOFF are data. The local terminal
 * has no flow control of its own, but its XON/XOFF hold and release the output to it, as in v1.8.
 */
#ifndef TERM_DEV_H
#define TERM_DEV_H
#include "kernel.h"
#include "term_line.h"

#define TERM_RING 304           /* the ROM's input rings (0x130) */
#define TERM_XOFF_AT 64         /* more than this many waiting: XOFF (0x192c) */
#define TERM_XON_BELOW 16       /* fewer than this left after a read: XON (0x19a8) */

extern chardev_t host_dev, phone_dev;   /* console_dev is kernel.c's */

/* How the devices reach their lines (term_line.c by default; a test can give its own). */
typedef struct {
    void (*write)(term_line_t *l, const unsigned char *s, int n);
    int (*configure)(term_line_t *l, long baud, int bits, char parity, int stop);
    void (*set_break)(term_line_t *l, int on);
    void (*set_modem)(term_line_t *l, int on);
} term_dev_io_t;

/* Set up the three devices on the lines (either may be NULL: no line). io NULL = term_line.c. Call it with the
 * kernel's lock held, after kernel_init. */
void term_dev_init(term_line_t *host, term_line_t *local, const term_dev_io_t *io);
/* line_rx_fn for both lines: ctx = &host_dev or &console_dev. Takes the kernel's lock. */
void term_dev_rx(void *ctx, int c);
/* With the lock held: send what the tasks wrote (the main loop calls it after every kernel_run). */
void term_dev_flush(void);
/* Set by LINE_QUIT (Ctrl+] q, the end of stdin). */
extern volatile int term_quit;

#endif
