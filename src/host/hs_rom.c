/* The host side's ROM data as named, typed C tables (REFERENCE.md s13 item 15, s15.35). Written by hand from
 * the ROM (the addresses are given); the host side's strings are inline in the code that prints them.
 */
#include "host.h"

/* 0x1823e: a DEC supplemental graphic character (0xa0-0xff, or 0x20-0x7f in a set designated '<') as the ASCII
 * letter spoken for it: accented letters lose the accent, the rest are dropped (0). The ROM reaches it as 0x1819e +
 * c for c = 0xa0-0xff (consume_control_string) and as 0x1821e + (c & 0x7f) (output_graphic_char). */
const char dec_supplemental_ascii[96] = {
    ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 'a', 0, 0, 0, 0, 0,                           /* 0xa0 */
    0, 0, '2', '3', 0, 0, 0, 0, 0, '1', 'o', 0, 0, 0, 0, 0,                       /* 0xb0 */
    'A', 'A', 'A', 'A', 'A', 'A', 0, 'C', 'E', 'E', 'E', 'E', 'I', 'I', 'I', 'I', /* 0xc0 */
    0, 'N', 'O', 'O', 'O', 'O', 'O', 0, '0', 'U', 'U', 'U', 'U', 'Y', 0, 0,       /* 0xd0 */
    'a', 'a', 'a', 'a', 'a', 'a', 0, 'c', 'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i', /* 0xe0 */
    0, 'n', 'o', 'o', 'o', 'o', 'o', 0, '0', 'u', 'u', 'u', 'u', 'y', 0, 0,       /* 0xf0 */
};

/* 0x18304: the device attributes reply, CSI ? 19 c (primary DA and DECID) */
const host_seq_t da_reply = { 0x9b, 'c', 1, { 1, 19 }, 0, { 0, 0 } };

/* 0x1829e, 0x182be: the DSP tone command for each dialed digit (codes 1-9, 10 = 0, 11 = *, 12 = #, 13-16 = A-D):
 * 0x8000 | column frequency and 0x9000 | row frequency in Hz (s15.32) */
const uint16_t dial_tone_high[16] = {
    0x84b9, 0x8538, 0x85c5, 0x84b9, 0x8538, 0x85c5,
    0x84b9, 0x8538, 0x85c5, 0x8538, 0x84b9, 0x85c5,
    0x8661, 0x8661, 0x8661, 0x8661,
};
const uint16_t dial_tone_low[16] = {
    0x92b9, 0x92b9, 0x92b9, 0x9302, 0x9302, 0x9302,
    0x9354, 0x9354, 0x9354, 0x93ad, 0x93ad, 0x93ad,
    0x92b9, 0x9302, 0x9354, 0x93ad,
};

/* 0x189da: the factory settings record (s15.33): version 4, DT_LOG 0, DT_TERMINAL 6, DT_MODE 1, line speeds 6 (host)
 * and 11 (local), formats 2 and 2, S7C1T 1, DECTC1 1, word 10 = 0, DT_SPEAK 1, host op 6 = 0; no checksum is stored */
const int16_t factory_settings[64] = { 4, 0, 6, 1, 6, 11, 2, 2, 1, 1, 0, 1, 0 };

/* 0x18a5a: the DUART format word for each parity setting (line_configure) */
const int16_t line_format_codes[3] = { 0x702, 0x706, 0x713 };

/* 0x1fff0, 0x3fff0: the dates at the end of the program ROM and of the dictionary ROM (DECTST 5 speaks them) */
const char rom_date_code[8] = "05Dec83";
const char rom_date_dict[8] = "11Oct83";
