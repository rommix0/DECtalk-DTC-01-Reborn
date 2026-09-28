/* The [:dv ...] voice-design commands: parse_bracket_command 0x7d1e and its helpers 0x7c10/0x7c3c/0x7c7e, and
 * print_voice_param_table 0x8072 ([:dv list] / [:dv listall]). Rebuilt from the DTC-01 v1.8 ROM; see
 * REFERENCE.md s8.1 and s15.22.
 *
 * The klsyn task gets each [:dv ...] as its own work item (first word 0) and copies the text, one byte per word, to
 * 0x81d78; the text pipeline ends it with ';' (" ap 150 save;"). parse_bracket_command runs every command in it:
 * a parameter name (at most 8 letters, case-folded) from dv_param_table, then for voice parameters an optional '='
 * and a decimal value, clamped to the table's range and stored in cur_voice. Then set_formant_limits() and
 * setspdef() rebuild the speaker packet, which the next clause sends.
 */
#include <stdio.h>
#include <string.h>
#include "ph_frame.h"
#include "ph_rom.h"
#include "ph_math.h"

int16_t dv_value;                   /* 0x81ccc: the last value read (also the new sex) */
void (*ph_console_hook)(const char *text);   /* console output of print_voice_param_table (printf level, "\n") */

enum { DV_SEX = 0, DV_LIST = 28, DV_LISTALL = 29, DV_SAVE = 30 };

/* tolower 0x11e48: ASCII only */
static char to_lower(char c)
{
    return (char)(c >= 'A' && c <= 'Z' ? c + 0x20 : c);
}

/* 0x7c10: skip blanks and tabs */
static const char *skip_blanks(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* 0x7c3c: skip blanks, one '=', blanks */
static const char *skip_blanks_eq(const char *p)
{
    p = skip_blanks(p);
    if (*p == '=') p = skip_blanks(p + 1);
    return p;
}

/* 0x7c7e: read an unsigned decimal number, clamp it to rec's range, store it in dv_value. Digits are tested
 * with the ctype table 0x1d354 on (c & 0x7f), so a byte 0xb0-0xb9 also counts, with its signed value. */
static const char *read_dv_value(const char *p, const dv_param_t *rec)
{
    uint32_t v = 0;                                 /* long_multiply: wraps like the 68000 */
    p = skip_blanks_eq(p);
    while ((unsigned)((*p & 0x7f) - '0') <= 9) {
        v = v * 10 + (uint32_t)((int32_t)(signed char)*p - '0');
        p++;
    }
    if ((int32_t)v > rec->max) v = (uint32_t)(int32_t)rec->max;
    else if ((int32_t)v < rec->min) v = (uint32_t)(int32_t)rec->min;
    dv_value = (int16_t)v;
    return p;
}

static void console(const char *s)
{
    if (ph_console_hook) ph_console_hook(s);
}

/* 0x8072: [:dv list] (all = 0: parameters without kind bit 8) or [:dv listall] (all = 1), to the console */
void print_voice_param_table(int all)
{
    const int16_t *v = &cur_voice.sex;
    char line[160];
    int i;
    /* dev_control(&console_dev, lock) */
    console("\n");
    for (i = 0; i < 28; i++) {
        const dv_param_t *r = &dv_param_table[i];
        if (!all && (r->kind & 8)) continue;
        snprintf(line, sizeof line, "%-3s %4d %-2s (%4d .. %4d) %s\n", r->name, v[i], dv_units[r->kind & ~8],
                 r->min, r->max, r->desc);
        console(line);
    }
    /* dev_control(&console_dev, unlock) */
}

/* 0x7d1e: run the commands in p (up to ';', ']', newline or the end); returns where it stopped */
const char *parse_bracket_command(const char *p)
{
    if (!p) return NULL;
    for (;;) {
        char name[9];
        int n, idx;
        const dv_param_t *rec;

        p = skip_blanks(p);
        if (strchr(";]\n", *p)) break;              /* strchr also finds the terminating NUL */
        p = skip_blanks(p);
        for (n = 0; n < 8 && !strchr(";] \t\n", *p); n++, p++) name[n] = to_lower(*p);
        name[n] = '\0';
        if (!name[0]) break;
        for (rec = dv_param_table; rec->name && strcmp(rec->name, name); rec++) {}
        if (!rec->name) {
            dt_error_flags |= 8;
            break;
        }
        idx = (int)(rec - dv_param_table);
        if (idx == DV_SEX) {                        /* m / f / 0 / 1: head size and F4/F5 follow the sex */
            char c;
            p = skip_blanks_eq(p);
            c = to_lower(*p);
            if (c == 'm') { dv_value = 1; p++; }
            else if (c == 'f') { dv_value = 0; p++; }
            else p = read_dv_value(p, rec);
            if (cur_voice.sex != dv_value) {
                cur_voice.sex = dv_value;
                cur_voice.hs = (int16_t)(cur_voice.hs + sex_hs_delta[dv_value]);
                if (cur_voice.f4 != 2500) cur_voice.f4 = q12(sex_f45_q12[dv_value], cur_voice.f4);
                if (cur_voice.f5 != 2500) cur_voice.f5 = q12(sex_f45_q12[dv_value], cur_voice.f5);
            }
        } else if (idx == DV_LIST) {
            print_voice_param_table(0);
        } else if (idx == DV_LISTALL) {
            print_voice_param_table(1);
        } else if (idx == DV_SAVE) {
            save_voice_params();
        } else {
            p = read_dv_value(p, rec);
            (&cur_voice.sex)[idx] = dv_value;
        }
    }
    set_formant_limits();
    setspdef();
    return p;
}
