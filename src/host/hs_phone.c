/* The phone task (REFERENCE.md s5.3, s15.34, s15.36): the telephone line's events from the phone device, the
 * caller's keys passed to the host, and in stand-alone mode (from power-up until the host sends its first byte) the
 * spoken DTMF menu, rebuilt in C.
 *
 * The device (phone_dev, its interrupt and ring poll) is the kernel's; the task sees events through dev_getc: a ring,
 * off hook (answered, or seized for dialing), the host's reset or hang-up, ringing stopped, the receiver's key codes
 * 0-15 while off hook, and DEV_TIMEOUT from the input timer.
 */
#include "host.h"
#include "console.h"

/* 0x1899a: the key a receiver code stands for, as sent to the host (code 0 is D, 10 is 0) */
static const char dtmf_key_chars[] = "D1234567890*#ABC";

/* 0x1987e: the key names the menu speaks, per receiver code */
static const char *const dtmf_key_names[16] = {
    "dee", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "zero", "star", "sharp", "aye",
    "be", "sea",
};

/* 0x198be: DECTST tests 2-5 as the menu names them */
static const char *const self_test_names[4] = {
    "host line data loopback", "host line control signal loopback", "local line data loopback",
    "speak a canned message",
};

/* digit mode: the keys go to the host while the line is off hook, and DT_PHONE 30's timeout counts the seconds
 * without a key. Returns the event that ends it, which the caller handles as in idle. */
static int32_t phone_keypad_mode(void)
{
    int32_t ev;
    keypad_ticks = 0;
    dev_control(&phone_dev, DEV_RX_TIMER, 100);         /* DEV_TIMEOUT after each second without an event */
    for (;;) {
        ev = dev_getc(&phone_dev);
        if (ev >= 0 && ev < 16) {
            keypad_ticks = 0;
            if (!phone_dialing) host_line_putc(dtmf_key_chars[ev]);     /* not the unit's own dialing */
        } else if (ev == DEV_TIMEOUT) {
            if (keypad_ticks != 0x7fff) keypad_ticks++;
            if (keypad_timeout && keypad_ticks >= keypad_timeout) {
                keypad_timeout = 0;
                emit_sync_marker("dtphon (timeout)");
                send_dcs_reply(70, 2);
            }
        } else {
            break;
        }
    }
    dev_control(&phone_dev, DEV_RX_TIMER_OFF);
    dev_control(&phone_dev, PHONE_KEYPAD_OFF);
    return ev;
}

/* 0xf128: the phone task. Idle: on hook, wait for an event. A ring starts the answer when rings to answer (DT_PHONE
 * 10) is set; once off hook, stand-alone mode gives the caller the menu and hangs up, otherwise the host gets R3 = 1
 * and the keys. Off hook after phone_go_offhook (dialing) goes to digit mode at once. A reset, hang-up or ringing
 * stopped puts the line on hook and waits 2 s; after a hang-up the host gets the delayed R3 = 0. */
void phtask_main(void)
{
    int32_t ev;
    for (;;) {
        phone_offhook = 0;
        dev_control(&phone_dev, DEV_RX_TIMER_OFF);
        ev = dev_getc(&phone_dev);
    again:
        switch (ev) {
        case PHONE_EV_RING:
            if (!answer_rings) break;
            dev_control(&phone_dev, PHONE_START_ANSWER, (int32_t)answer_rings);
            ev = dev_getc(&phone_dev);
            if (ev != PHONE_EV_OFFHOOK) goto again;
            phone_offhook = 1;
            if (phone_standalone) {
                dtmf_diagnostic_menu();
                emit_sync_marker("dtphon (fsm)");
                dev_control(&phone_dev, PHONE_SET_IDLE);
                phone_offhook = 0;
                event_wait(200, NULL, 0);
                break;
            }
            send_dcs_reply(70, 1);
            ev = phone_keypad_mode();
            goto again;
        case PHONE_EV_OFFHOOK:
            phone_offhook = 1;
            ev = phone_keypad_mode();
            goto again;
        case PHONE_EV_RESET:
        case PHONE_EV_HANGUP:
        case PHONE_EV_RING_STOPPED:
            dev_control(&phone_dev, PHONE_SET_IDLE);
            phone_offhook = 0;
            event_wait(200, NULL, 0);
            if (ev == PHONE_EV_HANGUP) send_dcs_reply(70, phone_offhook);
            break;
        default:
            kprintf("Bug: phtask c=%d\n", ev);
            break;
        }
    }
}

/* reads a number keyed by the caller, ended by '#': 0 = aborted (a non-key event), 1 = '*' (quit), 2 = the number.
 * Every key is a decimal digit by its code: '0' (code 10) counts as 0, but D (0) and A-C (13-15) add their codes. */
static int read_keyed_number(int16_t *n)
{
    int32_t ev;
    *n = 0;
    while ((ev = dev_getc(&phone_dev)) != 12) {
        if (ev < 0 || ev > 15) return 0;
        if (ev == 11) return 1;
        *n = (int16_t)(*n * 10);
        if (ev != 10) *n = (int16_t)(*n + ev);
    }
    return 2;
}

/* 0x116e2: the menu the caller hears in stand-alone mode. It speaks a banner, then each key: "You pressed ..."; '*'
 * restores the factory settings (settings_reset(2, 1): the NVRAM record is not written); '#' asks for a test number
 * and a pass count (keyed, ended by '#', '*' quits) and runs DECTST that many times. It returns when no key comes for
 * 30 s (or another event arrives); the keypad stays on. */
void dtmf_diagnostic_menu(void)
{
    int32_t ev;
    int16_t test, passes, pass;
    stream_printf(cur_stream, "\n\x02:np :ra 180\x03 Hello.\n");
    stream_printf(cur_stream, "This is DECtalk.\n");
    stream_printf(cur_stream, "The firmware is version %s.\n", "one point eight");
    if (dt_error_flags & ERR_NVR) {
        stream_printf(cur_stream, "NVR fault.\n");
        stream_printf(cur_stream, "Using factory settings.\n");
    }
    stream_printf(cur_stream, "Press any key for audible echo.\n");
    stream_printf(cur_stream, "Press star to return to factory settings.\n");
    stream_printf(cur_stream, "Press sharp to run self tests.\n");
    dev_control(&phone_dev, PHONE_KEYPAD_ON);
    dev_control(&phone_dev, DEV_RX_TIMER, 3000);
    for (;;) {
        stream_flush(cur_stream);
        ev = dev_getc(&phone_dev);
        if (ev < 0 || ev > 15) {
            dev_control(&phone_dev, DEV_RX_TIMER_OFF);
            return;
        }
        stream_printf(cur_stream, "You pressed %s.\n", dtmf_key_names[ev]);
        if (ev == 11) {                                 /* '*' */
            settings_reset(2, 1);
            stream_printf(cur_stream, "Using factory settings.\n");
            continue;
        }
        if (ev != 12) continue;                         /* '#' */
        stream_printf(cur_stream, "Enter test number, terminated by sharp.\n");
        stream_printf(cur_stream, "Enter star to quit.\n");
        stream_flush(cur_stream);
        switch (read_keyed_number(&test)) {
        case 0:
            return;
        case 1:
            stream_printf(cur_stream, "Quit.\n");
            continue;
        }
        if (test < 1 || test > 5) {
            stream_printf(cur_stream, "DECtalk has no test %d.\n", (int32_t)test);
            continue;
        }
        if (test == 1) {
            stream_printf(cur_stream, "Power up reset test not allowed.\n");
            continue;
        }
        stream_printf(cur_stream, "Test is %s.\n", self_test_names[test - 2]);
        stream_printf(cur_stream, "Enter pass count, terminated by sharp.\n");
        stream_printf(cur_stream, "Enter star to quit.\n");
        stream_flush(cur_stream);
        switch (read_keyed_number(&passes)) {
        case 0:
            return;
        case 1:
            stream_printf(cur_stream, "Quit.\n");
            continue;
        }
        if (passes < 1 || passes > 10) {
            stream_printf(cur_stream, "Pass count must be between one and ten.\n");
            stream_printf(cur_stream, "Running ten passis.\n");      /* sic */
            stream_flush(cur_stream);
            passes = 10;
        }
        for (pass = 1; pass <= passes; pass++) {
            dectst_self_test(test);
            if (dt_error_flags & ERR_DECTST) break;
        }
        if (pass <= passes) stream_printf(cur_stream, "Failed in pass %d.\n", (int32_t)pass);
        else stream_printf(cur_stream, "Passed.\n");
    }
}
