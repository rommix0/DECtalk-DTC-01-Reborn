/* The settings and the NVRAM record (REFERENCE.md s5.5, s15.33, s15.35): resets (RIS, DECSTR, DECNVR, power-up),
 * the line set-up and the X2212 record, rebuilt in C.
 *
 * The record is 64 words: 0 the version (4), 1-3 DT_LOG, DT_TERMINAL, DT_MODE, 4-5 the line speeds (host, local),
 * 6-7 their formats, 8 S7C1T, 9 DECTC1, 10 unused, 11 DT_SPEAK, 12 the host line's op 6 argument, 63 the checksum.
 * The X2212 holds it as 256 nibbles, word k's nibble j (least significant first) in the even byte 2 * (4k + j);
 * writing 0x200 stores the RAM copy in the EEPROM.
 */
#include "host.h"
#include "console.h"

uint8_t nvram[0x202];

/* 0xf7ca: u = 0xffff; per word u = (w ^ u) * 2, plus 1 when that sets bit 16; the low 16 bits */
uint16_t nvram_checksum(const int16_t *w, int n)
{
    uint32_t u = 0xffff;
    while (n--) {
        u = ((uint32_t)(int32_t)*w++ ^ u) * 2;
        if (u & 0x10000) u++;
    }
    return (uint16_t)u;
}

/* 0xf69a: which = 0: the user record from the NVRAM, 1 only when its checksum and version (4) are right; which = 1:
 * the factory record */
int nvram_load_settings(int16_t rec[64], int which)
{
    int k, j, a;
    if (which == 0) {
        event_wait(1, NULL, 0);
        a = 0x1fe;
        for (k = 63; k >= 0; k--) {
            uint16_t w = 0;
            for (j = 0; j < 4; j++, a -= 2) w = (uint16_t)((nvram[a] & 0xf) | w << 4);
            rec[k] = (int16_t)w;
        }
        return (int16_t)nvram_checksum(rec, 63) == rec[63] && rec[0] == 4;
    }
    if (which == 1) {
        for (k = 0; k < 64; k++) rec[k] = factory_settings[k];
        return 1;
    }
    return 0;
}

/* 0xf79c: DECNVR 1 (and SETUP SAVE): the settings into the user record (which = 0); the factory record (1) cannot be
 * written, which is an NVR error */
int nvram_save_settings(int which)
{
    int16_t rec[64];
    int k, j, a;
    dt_error_flags &= ~ERR_NVR;
    if (which != 0) {
        dt_error_flags |= ERR_NVR;
        return 0;
    }
    for (k = 0; k < 64; k++) rec[k] = 0;
    rec[0] = 4;
    rec[1] = dt_log;
    rec[2] = dt_terminal;
    rec[3] = dt_mode;
    rec[11] = speak_enabled;
    rec[12] = host_modem;
    for (k = 0; k < 2; k++) {
        rec[k + 4] = line_speed[k];
        rec[k + 6] = line_format[k];
    }
    rec[8] = c1_transmit_7bit;
    rec[9] = c1_receive_7bit;
    rec[10] = setup_interrupt_char;
    rec[63] = (int16_t)nvram_checksum(rec, 63);
    for (a = 0, k = 0; k < 64; k++) {
        int16_t w = rec[k];
        for (j = 0; j < 4; j++, a += 2, w >>= 4) nvram[a] = (uint8_t)(w & 0xf);
    }
    nvram[0x200] = 1;                       /* store */
    event_wait(1, NULL, 0);
    return 1;
}

/* 0xf5c6: program a line's DUART channel from the settings: op 1 the speed (code c as 0x11 * c; 0 = 0x60), op 2 the
 * format (0x800 more at the two lowest speeds), and for the host line op 6 */
void line_configure(int line)
{
    chardev_t *d = line ? &console_dev : &host_dev;
    int32_t v = line_speed[line];
    v = v ? (v << 4 | v) : 0x60;
    dev_control(d, v + 0x10000);
    v = line_format_codes[line_format[line]];
    if (line_speed[line] < 2) v |= 0x800;
    dev_control(d, v + 0x20000);
    if (line == 0) dev_control(d, host_modem + 0x60000);
}

/* 0xf3be (formerly spdef_settings_io): mode 0 = RIS, 1 = DECSTR, 2 = DECNVR 0 (restore from record `which`), 3 = the
 * power-up. A failing user record is an NVR error (24); RIS and the power-up then take the factory record.
 * RIS, DECNVR and the power-up restore DT_LOG, DT_TERMINAL, DT_MODE and DT_SPEAK; DECNVR and the power-up also the
 * lines, and the power-up starts the stand-alone phone mode. RIS and DECSTR turn speaking on, clear the index and the
 * phone's keypad timeout and answer count, and reset the phone task; RIS also resets the character sets and
 * empties the user dictionary. */
int settings_reset(int mode, int which)
{
    int16_t rec[64];
    int i;
    if (mode == 2) {
        dt_error_flags &= ~ERR_NVR;
        if (!nvram_load_settings(rec, which)) {
            dt_error_flags |= ERR_NVR;
            return 0;
        }
    } else if (mode != 1) {
        dt_error_flags &= ~ERR_NVR;
        if (!nvram_load_settings(rec, 0)) {
            dt_error_flags |= ERR_NVR;
            if (!nvram_load_settings(rec, 1)) {
                kprintf("Bug: no factory settings\n");
                panic(NULL);
            }
        }
    }
    if (mode != 1) {
        dt_log = rec[1];
        dt_terminal = rec[2];
        dt_mode = rec[3];
        speak_enabled = rec[11];
    }
    if (mode == 3) phone_standalone = 1;
    if (mode == 2 || mode == 3) {
        host_modem = rec[12];
        c1_transmit_7bit = (int8_t)rec[8];
        c1_receive_7bit = (int8_t)rec[9];
        setup_interrupt_char = rec[10];
        for (i = 0; i < 2; i++) {
            line_speed[i] = rec[i + 4];
            line_format[i] = rec[i + 6];
            line_configure(i);
        }
    }
    if (mode == 0 || mode == 1) {
        keypad_timeout = 0;
        keypad_ticks = 0;
        speak_enabled = 1;
        last_index = 0;
        answer_rings = 0;
        dev_control(&phone_dev, DEV_POST, PHONE_EV_RESET);
        if (mode == 0) {
            single_shift = 0;
            gl_set = 'B';
            gr_set = '<';
            g_set[0] = 'B';
            g_set[1] = 'B';
            g_set[2] = '<';
            g_set[3] = '<';
            dict_hash_clear_all();
        }
    }
    return 1;
}
