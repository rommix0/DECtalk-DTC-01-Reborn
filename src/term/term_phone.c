/* dtc01term's telephone line: a simulated line behind the TLC interface (term_phone.h, REFERENCE.md s17.14.1).
 * It follows the emulator's line (native/dtc01.c: tlc_update_irq, spc_tlc_read16, phone_line_tick). */
#include <string.h>
#include "term_phone.h"

#define RING_ON 200             /* ticks: 2 s of ring signal, then 4 s without (dtc01_phone_ring's US cadence) */
#define RING_OFF 400
#define KEY_ON 15               /* a caller's key: 150 ms of tone and a 150 ms gap (hostfeed.h's \k) */
#define KEY_OFF 15
#define OWN_TONE 16             /* the dialer's tone: 160 ms (s15.32) */

static int g_enabled;
static void (*g_isr)(void);
static void (*g_status)(const char *);
static uint16_t g_ctl;                          /* the written bits of 0x9c004 */
static int g_rings, g_ring_on, g_ring_left;     /* rings still to come; the signal; ticks left in this phase */
static int g_tone_left, g_tone_own, g_gap;      /* the tone the receiver hears: ticks left, the unit's own; the gap */
static int g_code;                              /* the receiver's code (latched) */
static char g_keys[64];                         /* the caller's keys not yet sent */
static int g_nkeys;
static int g_cond, g_irq;                       /* the interrupt's condition, and a request not yet taken */
static const char *g_shown;

int term_phone_key_code(int key)
{
    static const char codes[] = "D1234567890*#ABC";
    const char *p = key ? strchr(codes, key) : NULL;
    return p ? (int)(p - codes) : -1;
}

/* The TLC's interrupt is latched: it is requested when "tone and bit 6" or "ring and bit 14" becomes true, and the
 * 68000 takes it at once; the handler's read of 0x9c004 clears it. */
static void update(void)
{
    int c = (g_tone_left && (g_ctl & TLC_TONE_INT)) || (g_ring_on && (g_ctl & TLC_RING_INT));
    const char *s;
    if (c && !g_cond) g_irq = 1;
    g_cond = c;
    if (g_irq && g_isr) g_isr();
    s = (g_ctl & TLC_HOOK) ? "off hook" : g_ring_on || g_rings ? "ringing" : "on hook";
    if (s != g_shown) {
        g_shown = s;
        if (g_status) g_status(s);
    }
}

void term_phone_init(int enabled, void (*isr)(void), void (*status)(const char *state))
{
    g_enabled = enabled;
    g_isr = isr;
    g_status = status;
    g_ctl = 0;
    g_rings = g_ring_on = g_ring_left = 0;
    g_tone_left = g_tone_own = g_gap = g_code = 0;
    g_nkeys = 0;
    g_cond = g_irq = 0;
    g_shown = NULL;
}

uint16_t tlc_read(void)
{
    g_irq = 0;
    return (uint16_t)(g_ctl | (g_tone_left ? TLC_TONE : 0) | (g_ring_on ? TLC_RING : 0));
}

uint16_t tlc_read_code(void) { return (uint16_t)g_code; }

void tlc_write(uint16_t v)
{
    g_ctl = v & TLC_WRITTEN;
    if (g_ctl & TLC_HOOK) {
        g_rings = g_ring_on = g_ring_left = 0;  /* the exchange stops ringing on the answer */
    } else {
        g_tone_left = g_gap = 0;                /* on hook the receiver hears nothing */
    }
    update();
}

static void start_tone(int code, int ticks, int own)
{
    g_code = code;
    g_tone_left = ticks;
    g_tone_own = own;
    g_gap = 0;
}

void term_phone_tick(void)
{
    if (g_ring_left && !--g_ring_left && g_ring_on) {      /* the ring's end: then the quiet part */
        g_ring_on = 0;
        g_ring_left = RING_OFF;
    }
    if (!g_ring_left && g_rings && !(g_ctl & TLC_HOOK)) {   /* the next ring */
        g_rings--;
        g_ring_on = 1;
        g_ring_left = RING_ON;
    }
    if (g_tone_left) {
        if (!--g_tone_left) g_gap = g_tone_own ? 0 : KEY_OFF;
    } else if (g_gap) {
        g_gap--;
    }
    if (!g_tone_left && !g_gap && g_nkeys && (g_ctl & TLC_HOOK)) {     /* the caller's next key */
        start_tone(term_phone_key_code(g_keys[0]), KEY_ON, 0);
        memmove(g_keys, g_keys + 1, (size_t)--g_nkeys);
    }
    update();
}

void term_phone_ring(void)
{
    if (!g_enabled) return;
    g_rings++;
    update();
}

void term_phone_key(int key)
{
    if (!g_enabled || term_phone_key_code(key) < 0 || g_nkeys == (int)sizeof g_keys) return;
    g_keys[g_nkeys++] = (char)key;
}

/* the nearest of four frequencies */
static int nearest(int hz, const int f[4])
{
    int i, best = 0;
    for (i = 1; i < 4; i++)
        if ((hz > f[i] ? hz - f[i] : f[i] - hz) < (hz > f[best] ? hz - f[best] : f[best] - hz)) best = i;
    return best;
}

void term_phone_own_tone(int hz1, int hz2)
{
    static const int rows[4] = { 697, 770, 852, 941 }, cols[4] = { 1209, 1336, 1477, 1633 };
    static const char pad[] = "123A456B789C*0#D";
    int lo = hz1 < hz2 ? hz1 : hz2, hi = hz1 < hz2 ? hz2 : hz1;
    if (!(g_ctl & TLC_HOOK) || lo < 600) return;
    start_tone(term_phone_key_code(pad[nearest(lo, rows) * 4 + nearest(hi, cols)]), OWN_TONE, 1);
    update();
}
