/* dtc01term's lines: the console, stdio, TCP and serial-port backends (term_line.h, REFERENCE.md s17.14).
 *
 * Each open line has a reader thread that hands every received byte to the line's callback (on the local terminal
 * through the escape key first). Writes come from any thread and are serialized per line.
 */
#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <fcntl.h>
#include <io.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "term_line.h"
#include "term_os.h"
#ifdef _WIN32
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define NO_SOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
typedef int sock_t;
#define NO_SOCK (-1)
#define closesocket close
#endif

enum { L_NONE, L_CONSOLE, L_STDIO, L_TCP, L_COM };

struct term_line {
    int kind, local;
    line_rx_fn rx;
    void *ctx;
    int esc;                            /* the escape key was pressed: the next key is a command */
    volatile int stop;
    int has_thread;
    term_thread_t th;
    term_mutex_t wmu;                   /* writes */
    char desc[96];
    sock_t lsock, csock;                /* TCP: listening, the client (NO_SOCK = none) */
#ifdef _WIN32
    HANDLE h;                           /* COM */
    HANDLE hin, hout;                   /* console */
    DWORD old_in, old_out;
    UINT old_cp;
#else
    int fd;                             /* COM */
    struct termios old_tio;             /* console */
    int pstate;                         /* COM: PARMRK parsing */
#endif
};

/* ---- receiving: the escape key on the local terminal ---- */

static void deliver(term_line_t *l, int c)
{
    if (!l->local || c < 0) {
        l->rx(l->ctx, c);
        return;
    }
    if (l->esc) {
        l->esc = 0;
        if (c == 'b' || c == 'B') l->rx(l->ctx, LINE_BREAK);
        else if (c == 'q' || c == 'Q') l->rx(l->ctx, LINE_QUIT);
        else if (c == LINE_ESCAPE) l->rx(l->ctx, LINE_ESCAPE);
        return;                         /* anything else after the escape is dropped */
    }
    if (c == LINE_ESCAPE) l->esc = 1;
    else l->rx(l->ctx, c);
}

static void deliver_bytes(term_line_t *l, const unsigned char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) deliver(l, b[i]);
}

/* ---- the console ---- */

#ifdef _WIN32
static term_line_t *g_console;          /* for Ctrl+Break and closing the window */

static BOOL WINAPI console_ctrl(DWORD ev)
{
    term_line_t *l = g_console;
    if (!l) return FALSE;
    if (ev == CTRL_BREAK_EVENT) {
        l->rx(l->ctx, LINE_BREAK);
        return TRUE;
    }
    if (ev == CTRL_C_EVENT) {           /* only with processed input, which raw mode turns off */
        deliver(l, 3);
        return TRUE;
    }
    if (ev == CTRL_CLOSE_EVENT) {
        l->rx(l->ctx, LINE_QUIT);
        Sleep(2000);                    /* the main loop quits meanwhile */
        return TRUE;
    }
    return FALSE;
}

TERM_THREAD(console_reader, arg)
{
    term_line_t *l = (term_line_t *)arg;
    INPUT_RECORD rec[16];
    DWORD n, i;
    while (!l->stop) {
        if (WaitForSingleObject(l->hin, 100) != WAIT_OBJECT_0) continue;
        if (!ReadConsoleInputW(l->hin, rec, 16, &n)) break;
        for (i = 0; i < n; i++) {
            const KEY_EVENT_RECORD *k = &rec[i].Event.KeyEvent;
            WORD r;
            int c;
            if (rec[i].EventType != KEY_EVENT || !k->bKeyDown || !k->uChar.UnicodeChar) continue;
            c = k->uChar.UnicodeChar < 256 ? k->uChar.UnicodeChar : '?';    /* Latin-1, as the DEC set's base */
            for (r = 0; r < (k->wRepeatCount ? k->wRepeatCount : 1); r++) deliver(l, c);
        }
    }
    TERM_THREAD_END;
}

static int console_open(term_line_t *l, char *err, int errlen)
{
    l->hin = GetStdHandle(STD_INPUT_HANDLE);
    l->hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(l->hin, &l->old_in) || !GetConsoleMode(l->hout, &l->old_out)) {
        snprintf(err, (size_t)errlen, "console: standard input and output are not a console (use stdio)");
        return -1;
    }
    SetConsoleMode(l->hin, ENABLE_EXTENDED_FLAGS | ENABLE_QUICK_EDIT_MODE);    /* raw: no line input, echo, ^C */
    SetConsoleMode(l->hout, l->old_out | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    l->old_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(28591);          /* bytes 0xa0-0xff as Latin-1 */
    g_console = l;
    SetConsoleCtrlHandler(console_ctrl, TRUE);
    return 0;
}

static void console_write(term_line_t *l, const unsigned char *s, int n)
{
    DWORD w;
    WriteFile(l->hout, s, (DWORD)n, &w, NULL);
}

static void console_close(term_line_t *l)
{
    SetConsoleCtrlHandler(console_ctrl, FALSE);
    g_console = NULL;
    SetConsoleMode(l->hin, l->old_in);
    SetConsoleMode(l->hout, l->old_out);
    SetConsoleOutputCP(l->old_cp);
}
#else
TERM_THREAD(console_reader, arg)
{
    term_line_t *l = (term_line_t *)arg;
    unsigned char b[64];
    while (!l->stop) {
        struct pollfd p = { 0, POLLIN, 0 };
        ssize_t n;
        if (poll(&p, 1, 100) <= 0) continue;
        n = read(0, b, sizeof b);
        if (n <= 0) {
            l->rx(l->ctx, LINE_QUIT);
            break;
        }
        deliver_bytes(l, b, (int)n);
    }
    TERM_THREAD_END;
}

static int console_open(term_line_t *l, char *err, int errlen)
{
    struct termios t;
    if (!isatty(0) || tcgetattr(0, &l->old_tio) != 0) {
        snprintf(err, (size_t)errlen, "console: standard input is not a terminal (use stdio)");
        return -1;
    }
    t = l->old_tio;
    t.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t.c_oflag &= ~(tcflag_t)OPOST;      /* the unit sends its own CR LF */
    t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    return 0;
}

static void console_write(term_line_t *l, const unsigned char *s, int n)
{
    (void)l;
    while (n > 0) {
        ssize_t w = write(1, s, (size_t)n);
        if (w <= 0) break;
        s += w;
        n -= (int)w;
    }
}

static void console_close(term_line_t *l) { tcsetattr(0, TCSANOW, &l->old_tio); }
#endif

/* ---- stdio ---- */

TERM_THREAD(stdio_reader, arg)
{
    term_line_t *l = (term_line_t *)arg;
    unsigned char b[256];
    for (;;) {
#ifdef _WIN32
        DWORD n;
        if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), b, sizeof b, &n, NULL) || n == 0) break;
#else
        ssize_t n = read(0, b, sizeof b);
        if (n <= 0) break;
#endif
        if (l->stop) break;
        deliver_bytes(l, b, (int)n);
    }
    if (!l->stop) l->rx(l->ctx, LINE_QUIT);
    TERM_THREAD_END;
}

static void stdio_write(const unsigned char *s, int n)
{
    fwrite(s, 1, (size_t)n, stdout);
    fflush(stdout);
}

/* ---- TCP ---- */

static int sock_ready(sock_t s, int ms)
{
    fd_set r;
    struct timeval tv;
    FD_ZERO(&r);
    FD_SET(s, &r);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select((int)s + 1, &r, NULL, NULL, &tv) > 0;
}

TERM_THREAD(tcp_reader, arg)
{
    term_line_t *l = (term_line_t *)arg;
    unsigned char b[256];
    while (!l->stop) {
        if (l->csock == NO_SOCK) {
            sock_t c;
            if (!sock_ready(l->lsock, 100)) continue;
            c = accept(l->lsock, NULL, NULL);
            if (c == NO_SOCK) continue;
            term_mutex_lock(&l->wmu);
            l->csock = c;
            term_mutex_unlock(&l->wmu);
            continue;
        }
        if (sock_ready(l->lsock, 0)) {  /* one client at a time: close any other at once */
            sock_t c = accept(l->lsock, NULL, NULL);
            if (c != NO_SOCK) closesocket(c);
        }
        if (sock_ready(l->csock, 100)) {
            int n = (int)recv(l->csock, (char *)b, sizeof b, 0);
            if (n <= 0) {                   /* the client left: the host line goes quiet */
                term_mutex_lock(&l->wmu);
                closesocket(l->csock);
                l->csock = NO_SOCK;
                term_mutex_unlock(&l->wmu);
                continue;
            }
            deliver_bytes(l, b, n);
        }
    }
    TERM_THREAD_END;
}

static int tcp_open(term_line_t *l, const char *where, char *err, int errlen)
{
    char host[64] = "127.0.0.1";
    const char *colon = strrchr(where, ':');
    int port;
    struct sockaddr_in a;
#ifdef _WIN32
    static int wsa;
    if (!wsa) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) {
            snprintf(err, (size_t)errlen, "tcp: no Winsock");
            return -1;
        }
        wsa = 1;
    }
#endif
    if (colon) {
        size_t n = (size_t)(colon - where);
        if (n >= sizeof host) n = sizeof host - 1;
        memcpy(host, where, n);
        host[n] = 0;
        port = atoi(colon + 1);
    } else {
        port = atoi(where);
    }
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (port <= 0 || port > 65535 || inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        snprintf(err, (size_t)errlen, "tcp: bad address \"%s\" (tcp:[addr:]port, addr as 1.2.3.4)", where);
        return -1;
    }
    l->csock = NO_SOCK;
    l->lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (l->lsock == NO_SOCK) {
        snprintf(err, (size_t)errlen, "tcp: no socket");
        return -1;
    }
#ifndef _WIN32
    {
        int on = 1;
        setsockopt(l->lsock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    }
#endif
    if (bind(l->lsock, (struct sockaddr *)&a, sizeof a) != 0 || listen(l->lsock, 1) != 0) {
        snprintf(err, (size_t)errlen, "tcp: cannot listen on %s:%d (in use?)", host, port);
        closesocket(l->lsock);
        return -1;
    }
    snprintf(l->desc, sizeof l->desc, "tcp %s:%d", host, port);
    return 0;
}

static void tcp_write(term_line_t *l, const unsigned char *s, int n)
{
    while (n > 0 && l->csock != NO_SOCK) {
        int w = (int)send(l->csock, (const char *)s, n, 0);
        if (w <= 0) break;              /* the reader sees the client go */
        s += w;
        n -= w;
    }
}

/* ---- a serial port ---- */

#ifdef _WIN32
TERM_THREAD(com_reader, arg)
{
    term_line_t *l = (term_line_t *)arg;
    OVERLAPPED ov;
    unsigned char b[256];
    int broke = 0;
    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    SetCommMask(l->h, EV_RXCHAR | EV_BREAK | EV_ERR);
    while (!l->stop) {
        DWORD mask = 0, got, errs;
        COMSTAT st;
        ResetEvent(ov.hEvent);
        if (!WaitCommEvent(l->h, &mask, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) {
                Sleep(10);
            } else {
                /* The event can be missed (seen with com0com): while waiting, look at the queue every 20 ms. */
                for (;;) {
                    if (WaitForSingleObject(ov.hEvent, 20) == WAIT_OBJECT_0) {
                        GetOverlappedResult(l->h, &ov, &got, FALSE);
                        break;
                    }
                    if (l->stop) break;
                    ClearCommError(l->h, &errs, &st);
                    if (errs & CE_BREAK) broke = 1;
                    if (st.cbInQue) {
                        CancelIo(l->h);
                        GetOverlappedResult(l->h, &ov, &got, TRUE);
                        break;
                    }
                }
                if (l->stop) {
                    CancelIo(l->h);
                    break;
                }
            }
        }
        /* A break is the CE_BREAK error, with a NUL in the data where it came; EV_BREAK alone also fires when the
         * other end of a com0com pair is set up. */
        ClearCommError(l->h, &errs, &st);
        for (;;) {
            DWORD want = st.cbInQue < sizeof b ? st.cbInQue : (DWORD)sizeof b, n = 0, k;
            if (errs & CE_BREAK) broke = 1;
            if (!want) break;
            ResetEvent(ov.hEvent);
            if (!ReadFile(l->h, b, want, &n, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(l->h, &ov, &n, TRUE)) break;
            }
            for (k = 0; k < n; k++) {
                if (broke && b[k] == 0) {
                    broke = 0;
                    l->rx(l->ctx, LINE_BREAK);
                } else {
                    deliver(l, b[k]);
                }
            }
            ClearCommError(l->h, &errs, &st);
        }
        if (broke && (mask & EV_BREAK) && !st.cbInQue) {    /* a break with no NUL after it */
            broke = 0;
            l->rx(l->ctx, LINE_BREAK);
        }
    }
    CloseHandle(ov.hEvent);
    TERM_THREAD_END;
}

static int com_open(term_line_t *l, const char *name, char *err, int errlen)
{
    char path[64];
    COMMTIMEOUTS to;
    snprintf(path, sizeof path, "\\\\.\\%s", name);
    l->h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (l->h == INVALID_HANDLE_VALUE) {
        snprintf(err, (size_t)errlen, "com: cannot open %s (error %lu)", name, (unsigned long)GetLastError());
        return -1;
    }
    SetupComm(l->h, 4096, 4096);
    memset(&to, 0, sizeof to);
    SetCommTimeouts(l->h, &to);
    snprintf(l->desc, sizeof l->desc, "com %s", name);
    if (line_configure(l, 9600, 8, 'N', 1) != 0) {
        snprintf(err, (size_t)errlen, "com: %s refuses 9600 8N1", name);
        CloseHandle(l->h);
        return -1;
    }
    line_set_modem(l, 1);
    return 0;
}

static void com_write(term_line_t *l, const unsigned char *s, int n)
{
    OVERLAPPED ov;
    DWORD w;
    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!WriteFile(l->h, s, (DWORD)n, &w, &ov) && GetLastError() == ERROR_IO_PENDING)
        GetOverlappedResult(l->h, &ov, &w, TRUE);
    CloseHandle(ov.hEvent);
}

int line_configure(term_line_t *l, long baud, int bits, char parity, int stop)
{
    DCB d;
    if (!l || l->kind != L_COM) return -1;
    memset(&d, 0, sizeof d);
    d.DCBlength = sizeof d;
    if (!GetCommState(l->h, &d)) return -2;
    d.BaudRate = (DWORD)baud;
    d.ByteSize = (BYTE)bits;
    d.Parity = parity == 'E' ? EVENPARITY : parity == 'O' ? ODDPARITY : NOPARITY;
    d.fParity = parity == 'E' || parity == 'O';
    d.StopBits = stop == 2 ? TWOSTOPBITS : ONESTOPBIT;
    d.fBinary = TRUE;
    d.fOutxCtsFlow = d.fOutxDsrFlow = FALSE;
    d.fOutX = d.fInX = FALSE;           /* XON/XOFF is the unit's, above the line */
    d.fDsrSensitivity = FALSE;
    d.fNull = FALSE;
    d.fAbortOnError = FALSE;
    return SetCommState(l->h, &d) ? 0 : -2;
}

void line_set_break(term_line_t *l, int on)
{
    if (!l || l->kind != L_COM) return;
    if (on) SetCommBreak(l->h);
    else ClearCommBreak(l->h);
}

void line_set_modem(term_line_t *l, int on)
{
    if (!l || l->kind != L_COM) return;
    EscapeCommFunction(l->h, on ? SETDTR : CLRDTR);
    EscapeCommFunction(l->h, on ? SETRTS : CLRRTS);
}
#else
/* PARMRK marks a break as 0377 0 0 and a byte 0377 as 0377 0377 */
static void com_byte(term_line_t *l, unsigned char c)
{
    switch (l->pstate) {
    case 0:
        if (c == 0377) l->pstate = 1;
        else deliver(l, c);
        break;
    case 1:
        if (c == 0377) {
            deliver(l, 0377);
            l->pstate = 0;
        } else {
            l->pstate = 2;              /* 0377 0: a break or an error follows */
        }
        break;
    default:
        if (c == 0) l->rx(l->ctx, LINE_BREAK);
        l->pstate = 0;
        break;
    }
}

TERM_THREAD(com_reader, arg)
{
    term_line_t *l = (term_line_t *)arg;
    unsigned char b[256];
    while (!l->stop) {
        struct pollfd p = { l->fd, POLLIN, 0 };
        ssize_t n, i;
        if (poll(&p, 1, 100) <= 0) continue;
        n = read(l->fd, b, sizeof b);
        if (n <= 0) continue;
        for (i = 0; i < n; i++) com_byte(l, b[i]);
    }
    TERM_THREAD_END;
}

static speed_t baud_code(long baud)
{
    switch (baud) {
    case 50: return B50;
    case 75: return B75;
    case 110: return B110;
    case 134: return B134;
    case 150: return B150;
    case 200: return B200;
    case 300: return B300;
    case 600: return B600;
    case 1200: return B1200;
    case 1800: return B1800;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    }
    return B0;
}

int line_configure(term_line_t *l, long baud, int bits, char parity, int stop)
{
    struct termios t;
    speed_t sp = baud_code(baud);
    if (!l || l->kind != L_COM) return -1;
    if (sp == B0 || tcgetattr(l->fd, &t) != 0) return -2;
    t.c_iflag = PARMRK;                 /* breaks are marked; no XON/XOFF, no CR mapping */
    t.c_oflag = 0;
    t.c_lflag = 0;
    t.c_cflag = CREAD | CLOCAL | (bits == 7 ? CS7 : CS8) | (stop == 2 ? CSTOPB : 0);
    if (parity == 'E') t.c_cflag |= PARENB;
    if (parity == 'O') t.c_cflag |= PARENB | PARODD;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, sp);
    cfsetospeed(&t, sp);
    return tcsetattr(l->fd, TCSANOW, &t) == 0 ? 0 : -2;
}

static int com_open(term_line_t *l, const char *name, char *err, int errlen)
{
    l->fd = open(name, O_RDWR | O_NOCTTY);
    if (l->fd < 0) {
        snprintf(err, (size_t)errlen, "com: cannot open %s (%s)", name, strerror(errno));
        return -1;
    }
    snprintf(l->desc, sizeof l->desc, "com %s", name);
    if (line_configure(l, 9600, 8, 'N', 1) != 0) {
        snprintf(err, (size_t)errlen, "com: %s is not a serial port", name);
        close(l->fd);
        return -1;
    }
    line_set_modem(l, 1);
    return 0;
}

static void com_write(term_line_t *l, const unsigned char *s, int n)
{
    while (n > 0) {
        ssize_t w = write(l->fd, s, (size_t)n);
        if (w <= 0) break;
        s += w;
        n -= (int)w;
    }
}

void line_set_break(term_line_t *l, int on)
{
    if (!l || l->kind != L_COM) return;
    ioctl(l->fd, on ? TIOCSBRK : TIOCCBRK, 0);
}

void line_set_modem(term_line_t *l, int on)
{
    int bits = TIOCM_DTR | TIOCM_RTS;
    if (!l || l->kind != L_COM) return;
    ioctl(l->fd, on ? TIOCMBIS : TIOCMBIC, &bits);
}
#endif

/* ---- the interface ---- */

term_line_t *line_open(const char *spec, int local, line_rx_fn rx, void *ctx, char *err, int errlen)
{
    term_line_t *l = (term_line_t *)calloc(1, sizeof *l);
    term_thread_fn reader = NULL;
    int r = 0;
    if (!l) {
        snprintf(err, (size_t)errlen, "out of memory");
        return NULL;
    }
    l->local = local;
    l->rx = rx;
    l->ctx = ctx;
    l->lsock = l->csock = NO_SOCK;
    term_mutex_init(&l->wmu);
    if (!strcmp(spec, "none")) {
        l->kind = L_NONE;
        strcpy(l->desc, "none");
    } else if (!strcmp(spec, "console")) {
        l->kind = L_CONSOLE;
        strcpy(l->desc, "console");
        r = console_open(l, err, errlen);
        reader = console_reader;
    } else if (!strcmp(spec, "stdio")) {
        l->kind = L_STDIO;
        strcpy(l->desc, "stdio");
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        reader = stdio_reader;
    } else if (!strncmp(spec, "tcp:", 4)) {
        l->kind = L_TCP;
        r = tcp_open(l, spec + 4, err, errlen);
        reader = tcp_reader;
    } else if (!strncmp(spec, "com:", 4)) {
        l->kind = L_COM;
        r = com_open(l, spec + 4, err, errlen);
        reader = com_reader;
    } else {
        snprintf(err, (size_t)errlen, "unknown line \"%s\" (console, stdio, tcp:[addr:]port, com:NAME, none)", spec);
        r = -1;
    }
    if (r != 0) {
        free(l);
        return NULL;
    }
    if (reader) {
        if (!term_thread_start(&l->th, reader, l)) {
            snprintf(err, (size_t)errlen, "%s: no thread", l->desc);
            line_close(l);
            return NULL;
        }
        l->has_thread = 1;
    }
    return l;
}

void line_write(term_line_t *l, const unsigned char *s, int n)
{
    if (!l || n <= 0) return;
    term_mutex_lock(&l->wmu);
    switch (l->kind) {
    case L_CONSOLE: console_write(l, s, n); break;
    case L_STDIO: stdio_write(s, n); break;
    case L_TCP: tcp_write(l, s, n); break;
    case L_COM: com_write(l, s, n); break;
    default: break;
    }
    term_mutex_unlock(&l->wmu);
}

int line_is_com(const term_line_t *l) { return l && l->kind == L_COM; }
const char *line_describe(const term_line_t *l) { return l ? l->desc : "none"; }

void line_close(term_line_t *l)
{
    if (!l) return;
    l->stop = 1;
    if (l->has_thread) {
        if (l->kind == L_STDIO) {       /* blocked in a read of stdin: left to end with the process */
#ifdef _WIN32
            CloseHandle(l->th);
#else
            pthread_detach(l->th);
#endif
        } else {
            term_thread_join(l->th);
        }
    }
    switch (l->kind) {
    case L_CONSOLE: console_close(l); break;
    case L_TCP:
        if (l->csock != NO_SOCK) closesocket(l->csock);
        closesocket(l->lsock);
        break;
#ifdef _WIN32
    case L_COM: CloseHandle(l->h); break;
#else
    case L_COM: close(l->fd); break;
#endif
    default: break;
    }
    if (l->kind != L_STDIO) free(l);        /* the stdio reader may still look at it */
}
