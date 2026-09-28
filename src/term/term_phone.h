/* dtc01term's telephone line (REFERENCE.md s17.14.1): a simulated line behind phonedev.h's TLC interface.
 *
 * The user plays the caller from the local terminal (Ctrl+] r rings once, Ctrl+] and a key presses it). The line
 * rings in the US cadence (2 s on, 4 s off) and stops ringing when the unit goes off hook. Its DTMF receiver hears
 * the caller's keys (150 ms of tone, 150 ms gap; they wait while the unit is on hook) and the unit's own dialing
 * (160 ms), and raises a latched interrupt as the emulator's does. A VoIP or modem backend would replace this file.
 * All calls with the kernel's lock held.
 */
#ifndef TERM_PHONE_H
#define TERM_PHONE_H
#include "phonedev.h"

/* enabled = 0: a line that never rings (rings and keys are ignored). isr: the driver's interrupt handler.
 * status (may be NULL): "on hook", "ringing" or "off hook", on each change. */
void term_phone_init(int enabled, void (*isr)(void), void (*status)(const char *state));
void term_phone_tick(void);                     /* every 10 ms, before kernel_tick */
void term_phone_ring(void);                     /* one more ring */
void term_phone_key(int key);                   /* the caller presses '0'-'9', '*', '#' or 'A'-'D' */
void term_phone_own_tone(int hz1, int hz2);     /* the unit plays a DTMF pair (dialing); a 0 Hz pair is silence */
int term_phone_key_code(int key);               /* the receiver's code: the index in "D1234567890*#ABC"; -1 = none */

#endif
