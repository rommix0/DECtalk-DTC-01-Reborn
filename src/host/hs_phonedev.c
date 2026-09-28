/* The ROM's phone driver (0x1ff4-0x231a; REFERENCE.md s15.34, s17.14.1): phone_dev's ops, the TLC interrupt and the
 * ring poll, rebuilt in C. It reaches the telephone interface only through phonedev.h's tlc_* calls.
 *
 * phone_dev's state (+0x68): 2 idle (on hook, the ring interrupt on), 3 off hook, 1 answering (off hook, 0x82 due);
 * while it counts rings to answer, the ring signal's last level (0 or 0x8000). The ring poll is the device's timer
 * (+0x54): the handler runs when its countdown (+0x60) runs out, and re-arms it itself.
 */
#include "kernel.h"
#include "host.h"
#include "phonedev.h"

#define RING_HIGH (-0x8000)           /* the state while the ring signal is on: 0x8000 as a word */

int16_t phdev_state;            /* phone_dev + 0x68 */
int16_t phdev_rings;            /* + 0x6a: rings to answer on */
int16_t phdev_count;            /* + 0x6c: rings counted */
int16_t phdev_polls;            /* + 0x6e: polls since the signal last changed */
static int32_t poll_left;       /* the ring poll's countdown (+ 0x60) in ticks; 0 = the timer is off */

/* 0x1ff4, op 7: on hook and idle, the ring interrupt on */
static int32_t phone_set_idle(void)
{
    if (phdev_state != 2) {
        phdev_state = 2;
        poll_left = 0;
        tlc_write(0x4000);
    }
    return 0;
}

/* 0x2034, op 8: off hook (to dial); the task gets 0x82 */
static int32_t phone_go_offhook(void)
{
    if (phdev_state != 3) {
        phdev_state = 3;
        poll_left = 0;
        kernel_device_input(&phone_dev, PHONE_EV_OFFHOOK);
        tlc_write(0x100);
    }
    return 0;
}

/* 0x2084, op 9: count n rings, then answer (phone_ring_poll) */
static int32_t phone_start_answer(int32_t n)
{
    if (phdev_state != 2) phone_set_idle();
    phdev_rings = (int16_t)n;
    phdev_state = (int16_t)(tlc_read() & 0x8000);
    poll_left = 4;
    phdev_count = 0;
    phdev_polls = 0;
    tlc_write(0);
    return 0;
}

/* 0x214e: the TLC interrupt. Idle: a ring is event 0x81. Off hook: a tone is the receiver's code. */
void phone_tlc_isr(void)
{
    int16_t v = (int16_t)tlc_read();
    if (phdev_state == 2) {
        if (v & 0x8000) kernel_device_input(&phone_dev, PHONE_EV_RING);
    } else if (phdev_state == 3 && (v & 0x80)) {
        kernel_device_input(&phone_dev, tlc_read_code() & 0xff);
    }
}

/* 0x21ca: every 4 ticks while counting. A ring counts when the signal falls after at least two polls high; after the
 * wanted number, off hook, and 0x82 2.5 s later. 250 polls (10 s) without a change: idle again, event 0x86. */
static void phone_ring_poll(void)
{
    int16_t was = phdev_state, now;
    if (was == 1) {
        phdev_state = 3;
        poll_left = 0;
        kernel_device_input(&phone_dev, PHONE_EV_OFFHOOK);
        return;
    }
    phdev_polls++;
    now = (int16_t)(tlc_read() & 0x8000);
    if (now != was && phdev_polls >= 2 && was == RING_HIGH && ++phdev_count >= phdev_rings) {
        phdev_state = 1;
        poll_left = 250;
        tlc_write(0x100);
        return;
    }
    if (phdev_polls >= 250) {
        phdev_state = 2;
        poll_left = 0;
        kernel_device_input(&phone_dev, PHONE_EV_RING_STOPPED);
        tlc_write(0x4000);
        return;
    }
    poll_left = 4;
    if (now != was) {
        phdev_state = now;
        phdev_polls = 0;
    }
}

static void phdev_tick(void *ctx)
{
    (void)ctx;
    if (poll_left && !--poll_left) phone_ring_poll();
}

/* the ops table 0x2632: nine entries, 1 and 6 empty; ops 2-5 (0x2106, 0x2118, 0x212a, 0x213c) are ori.w/andi.w on
 * the register */
static int32_t phdev_control(void *ctx, int32_t op, int32_t arg)
{
    (void)ctx;
    switch (op) {
    case PHONE_KEYPAD_ON: tlc_write((uint16_t)(tlc_read() | 0x40)); return 0;
    case PHONE_KEYPAD_OFF: tlc_write((uint16_t)(tlc_read() & 0xffbf)); return 0;
    case PHONE_HOOK_RELEASE: tlc_write((uint16_t)(tlc_read() & 0xfeff)); return 0;
    case PHONE_HOOK_SEIZE: tlc_write((uint16_t)(tlc_read() | 0x100)); return 0;
    case PHONE_SET_IDLE: return phone_set_idle();
    case PHONE_GO_OFFHOOK: return phone_go_offhook();
    case PHONE_START_ANSWER: return phone_start_answer(arg);
    }
    return 0;
}

static const kdev_ops_t phdev_ops = { NULL, phdev_control, NULL, NULL, phdev_tick, PHONE_START_ANSWER };

/* 0x22ba: phone_dev with its ops and ring poll, the interrupt handler installed, idle */
void phone_init_impl(void)
{
    kernel_device_init(&phone_dev, &phdev_ops, NULL);
    poll_left = 0;
    phdev_state = 2;
    tlc_write(0x4000);
}
