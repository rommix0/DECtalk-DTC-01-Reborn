/* dtc01term's devices: the host line, the local terminal and an idle phone (term_dev.h, REFERENCE.md s17.14). */
#include <string.h>
#include "term_dev.h"

chardev_t host_dev, phone_dev;          /* 0x8011e, 0x80552 */
volatile int term_quit;

/* host_dev and phone_dev ops the host C uses (host.h) */
#define PHONE_GO_OFFHOOK 0x80000
#define PHONE_EV_OFFHOOK 0x82

static void io_write(term_line_t *l, const unsigned char *s, int n) { line_write(l, s, n); }
static const term_dev_io_t default_io = { io_write, line_set_format, line_set_break, line_set_modem };

typedef struct {
    chardev_t *dev;
    term_line_t *line;
    int is_host;
    int xoff_sent;                      /* host: XOFF sent, input held off (dev + 0x42) */
    int out_held;                       /* local: the terminal sent XOFF */
    unsigned char out[4096];            /* written, not yet sent */
    int nout;
    long baud;                          /* the line's settings, applied to a COM port */
    int bits, stop;
    char parity;
} port_t;

static port_t g_port[2];                /* 0 = host, 1 = local */
static const term_dev_io_t *g_io = &default_io;

static void send_now(port_t *p, const unsigned char *s, int n)
{
    if (p->line && n > 0) g_io->write(p->line, s, n);
}

static void flush_port(port_t *p)
{
    if (p->out_held || !p->nout) return;
    send_now(p, p->out, p->nout);
    p->nout = 0;
}

void term_dev_flush(void)
{
    flush_port(&g_port[0]);
    flush_port(&g_port[1]);
}

/* ---- the driver ops (kernel.h kdev_ops_t) ---- */

static void port_putc(void *ctx, int c)
{
    port_t *p = (port_t *)ctx;
    if (p->nout == (int)sizeof p->out) flush_port(p);
    if (p->nout == (int)sizeof p->out) return;      /* held off and full: the ROM's task would wait; dropped */
    p->out[p->nout++] = (unsigned char)c;
}

/* speed codes (line_speed[], SETUP's names) as rates; code 0, "75/1200", is 75 out and 1200 in: 1200 here */
static long code_baud(int code)
{
    static const long rate[16] = { 1200, 110, 0, 150, 300, 600, 1200, 0, 2400, 4800, 0, 9600, 0, 0, 0, 0 };
    return rate[code & 15];
}

static void apply(port_t *p)
{
    if (p->line) g_io->configure(p->line, p->baud, p->bits, p->parity, p->stop);
}

static int32_t port_control(void *ctx, int32_t op, int32_t arg)
{
    port_t *p = (port_t *)ctx;
    int32_t v = op & 0xffff;
    (void)arg;
    switch (op >> 16) {
    case 1:                             /* the speed: 0x11 * code, or 0x60 for code 0 (line_set_format 0xf5c6) */
        p->baud = code_baud(v == 0x60 ? 0 : v & 15);
        apply(p);
        return 0;
    case 2:                             /* the format: the DUART's MR2 << 8 | MR1 (line_format_codes) */
        p->bits = (v & 3) == 3 ? 8 : 7;
        p->parity = (v & 0x18) == 0x10 ? 'N' : (v & 4) ? 'O' : 'E';
        p->stop = (v >> 8 & 0xf) == 0xf ? 2 : 1;
        apply(p);
        return 0;
    case 3:                             /* BREAK on (1) / off (0): SETUP's BREAK and LBREAK */
        if (p->line) g_io->set_break(p->line, v & 1);
        return 0;
    case 4:                             /* the loopback tests (DECTST 2-4): a line here cannot loop back */
    case 5:
        return 1;
    case 6:                             /* the modem lines (SET HOST MODEM) */
        if (p->line) g_io->set_modem(p->line, v & 1);
        return 0;
    }
    return 0;
}

static int port_held(void *ctx) { return ((port_t *)ctx)->xoff_sent; }

/* 0x1996: after a read from the host ring, XON when fewer than 16 are left */
static void port_got(void *ctx, int left)
{
    port_t *p = (port_t *)ctx;
    static const unsigned char xon = 0x11;
    if (p->is_host && p->xoff_sent && left < TERM_XON_BELOW) {
        p->xoff_sent = 0;
        send_now(p, &xon, 1);           /* ahead of what is queued, as the ROM's pending byte (dev + 0x40) */
    }
}

static const kdev_ops_t port_ops = { port_putc, port_control, port_held, port_got };

/* the phone: no line yet (the phone step). It never rings and hears no keys; off hook when asked, so dialing works. */
static void phone_putc(void *ctx, int c) { (void)ctx; (void)c; }
static int32_t phone_control(void *ctx, int32_t op, int32_t arg)
{
    (void)ctx;
    (void)arg;
    if (op == PHONE_GO_OFFHOOK) kernel_device_input(&phone_dev, PHONE_EV_OFFHOOK);
    return 0;
}
static const kdev_ops_t phone_ops = { phone_putc, phone_control, NULL, NULL };

/* ---- receiving (the ROM's duart_rx_char, on the line's reader thread) ---- */

void term_dev_rx(void *ctx, int c)
{
    port_t *p = ctx == (void *)&host_dev ? &g_port[0] : &g_port[1];
    static const unsigned char xoff = 0x13;
    if (c == LINE_QUIT) {
        term_quit = 1;
        return;
    }
    kernel_lock();
    if (c == LINE_BREAK) {
        c = 0;                          /* a received break reads as 0 */
    } else if (c == 0) {
        kernel_unlock();                /* a plain NUL is dropped */
        return;
    } else if (!p->is_host && (c == 0x11 || c == 0x13)) {
        p->out_held = c == 0x13;        /* the terminal's flow control holds our output */
        flush_port(p);
        kernel_unlock();
        return;
    }
    if (p->is_host && kernel_device_count(p->dev) > TERM_XOFF_AT && !p->xoff_sent) {
        p->xoff_sent = 1;
        send_now(p, &xoff, 1);
    }
    kernel_device_input(p->dev, c);
    kernel_unlock();
}

void term_dev_init(term_line_t *host, term_line_t *local, const term_dev_io_t *io)
{
    int i;
    g_io = io ? io : &default_io;
    memset(g_port, 0, sizeof g_port);
    for (i = 0; i < 2; i++) {
        port_t *p = &g_port[i];
        p->dev = i ? &console_dev : &host_dev;
        p->line = i ? local : host;
        p->is_host = !i;
        p->baud = 9600;
        p->bits = 8;
        p->parity = 'N';
        p->stop = 1;
        kernel_device_init(p->dev, &port_ops, p);
    }
    kernel_device_init(&phone_dev, &phone_ops, NULL);
}
