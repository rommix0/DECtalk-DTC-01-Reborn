/* dtc01term's lines (REFERENCE.md s17.14): the unit's two serial lines, the host line and the local terminal, as
 * byte streams with interchangeable backends. The protocol (escape sequences, XON/XOFF) is above them, in the host
 * C and term_dev.c; a line only moves bytes.
 *
 * Backends ("spec"):
 *   console          the program's own console, in raw mode (restored at close)
 *   stdio            standard input and output, binary; the end of the input is LINE_QUIT
 *   tcp:[addr:]port  listens (addr 127.0.0.1 by default) for one client at a time, raw bytes
 *   com:NAME         a serial port: COM10 (Windows) or /dev/ttyUSB0 (POSIX); the only backend where speeds,
 *                    formats, BREAK and the modem lines are real
 *   none             no line: nothing comes in, what goes out is dropped
 *
 * On the local terminal, whatever its backend, Ctrl+] is the program's escape: then b = a BREAK (LINE_BREAK),
 * q = quit (LINE_QUIT), r = a ring on the phone line (LINE_RING), 0-9 * # A-D = the caller presses that key
 * (LINE_KEY; capital B is the key), Ctrl+] = a Ctrl+] itself. Ctrl+Break on the Windows console is a BREAK too.
 */
#ifndef TERM_LINE_H
#define TERM_LINE_H

#define LINE_BREAK (-2)         /* a received BREAK */
#define LINE_QUIT (-3)          /* Ctrl+] q, or the end of stdin */
#define LINE_ESCAPE 0x1d        /* Ctrl+] */
#define LINE_RING (-4)          /* Ctrl+] r: the phone rings once */
#define LINE_FAULT (-5)         /* a COM port received a byte with a parity, framing or overrun error; where the
                                 * byte is known, a SUB (0x1a) follows in its place, as the ROM's receiver gives */
#define LINE_KEY(c) (-0x100 - (c))              /* Ctrl+] and a key: the caller presses it */
#define LINE_IS_KEY(v) ((v) <= -0x100)
#define LINE_KEY_CHAR(v) (-0x100 - (v))

/* What a line receives, on the line's own reader thread: a byte 0-255, LINE_BREAK, LINE_QUIT, LINE_RING,
 * LINE_FAULT or a LINE_KEY. */
typedef void (*line_rx_fn)(void *ctx, int c);

typedef struct term_line term_line_t;

/* local = 1 for the local terminal (the escape key). NULL on failure, with the reason in err. */
term_line_t *line_open(const char *spec, int local, line_rx_fn rx, void *ctx, char *err, int errlen);
void line_write(term_line_t *l, const unsigned char *s, int n);
/* A COM port only: speed and format (bits 7 or 8, parity 'N' 'E' 'O', stop bits 1 or 2). 0 = done, -1 = not a COM
 * port (the setting is then only kept by the caller), -2 = the port refused it. */
int line_set_format(term_line_t *l, long baud, int bits, char parity, int stop);
void line_set_break(term_line_t *l, int on);    /* a COM port only: hold the line in the break state */
void line_set_modem(term_line_t *l, int on);    /* a COM port only: DTR and RTS */
int line_is_com(const term_line_t *l);
const char *line_describe(const term_line_t *l);        /* e.g. "tcp 127.0.0.1:2001", for messages */
void line_set_title(term_line_t *l, const char *text);  /* the console backend only: the window's title */
void line_close(term_line_t *l);

#endif
