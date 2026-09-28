/* The telephone interface (TLC) as the ROM's phone driver sees it (REFERENCE.md s15.34, s17.14.1).
 *
 * hs_phonedev.c is the ROM's driver (phone_dev's ops, its interrupt and its ring poll); it reaches the hardware only
 * through the three tlc_* calls, which a line backend supplies (dtc01term: term_phone.c, a simulated line). The
 * register at 0x9c004 reads back its written bits with the status bits, so the ROM's ori.w/andi.w on it keep the
 * other written bits; reading it clears a pending interrupt.
 */
#ifndef PHONEDEV_H
#define PHONEDEV_H
#include <stdint.h>

#define TLC_RING 0x8000         /* read: the ring signal */
#define TLC_TONE 0x0080         /* read: a DTMF tone present (StD) */
#define TLC_RING_INT 0x4000     /* write: interrupt on the ring signal */
#define TLC_HOOK 0x0100         /* write: the hook relay, 1 = off hook */
#define TLC_TONE_INT 0x0040     /* write: interrupt on a tone */
#define TLC_WRITTEN (TLC_RING_INT | TLC_HOOK | TLC_TONE_INT)

/* the line backend */
uint16_t tlc_read(void);        /* 0x9c004: the written bits and the status; clears a pending interrupt */
uint16_t tlc_read_code(void);   /* 0x9c006: the DTMF receiver's code (0-15) in the low byte */
void tlc_write(uint16_t v);     /* 0x9c004 */

/* the driver (hs_phonedev.c) */
void phone_init_impl(void);     /* 0x22ba: phone_dev with its ops, idle, the ring interrupt on */
void phone_tlc_isr(void);       /* 0x214e: the TLC interrupt; the backend calls it when it requests one */

#endif
