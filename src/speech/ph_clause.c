/* The klsyn task's input side: parse_phoneme_param_stream 0x7788 (one work item -> clauses), klclause 0x7a04
 * (dapi phclause: one clause through phalloph, phtiming and the frame loop), load_voice_definition 0x8150,
 * save_voice_params 0x81a8 and ms_to_frames 0xd54c. Rebuilt from the DTC-01 v1.8 ROM; see REFERENCE.md s15.21.
 *
 * A work item is a stream of words. A word without bits 0x3000 is a symbol (phoneme, stress or boundary; see
 * ph_alloph.c). With bits 0x3000 = k (1-3), the low byte is a symbol code followed by k more words:
 *   - an ordinary symbol with a user duration (frames) and, for k >= 2, a user F0 target, as written in phonemic
 *     text like [aa<250,120>]; they go to allodurs/f0tar[n + 5] for phalloph (n = its index in the clause);
 *   - a pseudo-phoneme (sym_flags bit 0x4000, always one argument word): 0x64 :vo voice, 0x65 :ra rate,
 *     0x66/0x67 index markers (kept in the stream for phalloph), 0x69 :pp and 0x6a :cp pause lengths (ms).
 * A voice or rate change ends the clause so far. The item is compacted in place in phonemes[] (0x81d78, where the
 * klsyn task copied it), so the output pointer never passes the input pointer.
 */
#include "ph_frame.h"
#include "ph_rom.h"

int16_t voice_code = 0x6b;     /* 0x82298: current voice, 0x6b + index into voice_defs (.data 0x6b = Paul) */
int16_t voice_last = 0x6b;     /* 0x8229a: copy of voice_code after a successful :vo */
int16_t draw_mode_82294 = 's'; /* 0x82294: 's' or 'a' = draw frames; .data 's', no writer in the ROM */
int16_t dt_log;                /* 0x822ca: DT_LOG flags (0x80 = print the voice table at each speaker packet) */
voice_t val_voice;             /* 0x81c7c: the :nv (Val) voice record, the 9th voice_table entry */

enum { SYM_CLSTART = 0x41, SYM_CLEND = 0x42,
       PSEUDO_VOICE = 0x64, PSEUDO_RATE = 0x65, PSEUDO_INDEX1 = 0x66, PSEUDO_INDEX2 = 0x67,
       PSEUDO_PERPAUSE = 0x69, PSEUDO_COMPAUSE = 0x6a };

static void log_error(const char *fmt, int a)
{
    if (ph_error_hook) ph_error_hook(fmt, a, 0);
}

/* 0xd54c: milliseconds -> 6.4 ms frames, rounded (-1 = "not set" passes through) */
int16_t ms_to_frames(int16_t ms)
{
    if (ms == -1) return ms;
    return (int16_t)(((int32_t)ms + 4) * 10 / 64);   /* long_multiply, long_divide (truncates toward zero) */
}

/* 0x8150: copy voice record voice_code - 0x6b into cur_voice, then rebuild the speaker packet */
void load_voice_definition(void)
{
    int i = voice_code - 0x6b;
    const int16_t *src = i < 8 ? voice_defs[i] : &val_voice.sex;
    int16_t *dst = &cur_voice.sex;
    int k;
    for (k = 0; k < 28; k++) dst[k] = src[k];
    set_formant_limits();
    setspdef();
}

/* 0x81a8: the current voice becomes the :nv (Val) voice (speech_init at boot, so Val starts as Paul; and
 * parse_bracket_command) */
void save_voice_params(void)
{
    val_voice = cur_voice;
}

/* The clause's last symbol: end[-1] in the ROM. [DTC01] PH_FIX_MARKS: the last one that is not an index marker or
 * its value word (found from the start: a value word can look like any symbol). A clause that ends in a mark after
 * its punctuation ("said [:in 4]--": the comma takes the word boundary's place before the marker) would get a second
 * clause end, a pause, in the ROM. */
static int16_t clause_last_symbol(const int16_t *ph, const int16_t *end)
{
    int16_t last = end[-1];
    if (ph_fixes & PH_FIX_MARKS) {
        last = *ph;
        while (ph < end) {
            if (*ph == PSEUDO_INDEX1 || *ph == PSEUDO_INDEX2) {
                ph += 2;
                continue;
            }
            last = *ph++;
        }
    }
    return last;
}

/* 0x7a04 (dapi phclause): ph[0..end) is one clause */
void klclause(int16_t *ph, int16_t *end)
{
    nphonemes = (int16_t)(end - ph);
    if (nphonemes <= 1) {
        int i;                                  /* [DTC01] PH_FIX_MARKS: nothing to say; its marks go with the next */
        for (i = 0; i < ph_ncmarks; i++) index_mark_reached(ph_cmarks[i].mark);     /* frame posted */
        ph_ncmarks = 0;
        return;
    }
    if (*ph != SYM_CLSTART) {
        dt_error_flags |= 8;
        log_error("Missing CLSTART in klclause, *pbuf=%d\n", *ph);
        *ph = SYM_CLSTART;
    }
    if (spdef_dirty) {                          /* a byte in the ROM (set by setspdef) */
        spdef_dirty = 0;
        dsp_post_frame(spdef_packet, SP_WORDS - 1);
        if (dt_log & 0x80) print_voice_param_table(1);
    }
    if (clause_last_symbol(ph, end) < SYM_CLEND) {  /* no clause end yet: add a comma */
        if (end < phonemes + PH_MAXALLO) *end = SYM_CLEND;
        else nphonemes = SYM_CLEND;             /* phonemes[200] is nphonemes in the ROM */
        nphonemes++;
    }
    phalloph();
    ph_ncmarks = 0;                             /* [DTC01] PH_FIX_MARKS: placed */
    phtiming();
    if (draw_mode_82294 == 's' || draw_mode_82294 == 'a') phclause_draw_frames();
}

/* [DTC01] PH_FIX_SPLIT: after the part before a :vo or :ra has been spoken, its durations (phtiming's, in
 * allodurs[]) and F0 targets are cleared, as at the item's start. In the ROM the rest of the item reads them as user
 * durations: "to [:nb] Betty," gets a 1,728-sample tap in "Betty" (REFERENCE s17.13). */
static void clear_user_values(void)
{
    int i;
    if (!(ph_fixes & PH_FIX_SPLIT)) return;
    for (i = 0; i < PH_MAXALLO; i++) {
        allodurs[i] = 0;
        f0tar[i] = 0;
    }
}

/* [DTC01] PH_FIX_MARKS: the item's marks (ph_side, at positions in the message) up to input position `at` go to the
 * clause being gathered, at the position its next symbol gets (ph_cmarks) */
static int side_next;

static void side_to_clause(int16_t at, int16_t pos)
{
    while (side_next < ph_nside && ph_side[side_next].pos <= at && ph_ncmarks < PH_SIDE_MAX) {
        ph_cmarks[ph_ncmarks].pos = pos;
        ph_cmarks[ph_ncmarks++].mark = ph_side[side_next++].mark;
    }
}

/* 0x7788: nwords words of a klsyn work item, already copied to phonemes[] */
void parse_phoneme_param_stream(int nwords)
{
    int16_t *in = phonemes, *out = phonemes, *end;
    int i;

    if (nwords > PH_MAXALLO) nwords = PH_MAXALLO;   /* cannot happen: klsyn messages hold 400 bytes */
    end = phonemes + nwords;
    for (i = 0; i < PH_MAXALLO; i++) {
        allodurs[i] = 0;
        f0tar[i] = 0;
    }
    f0mode = 0;
    side_next = 0;
    ph_ncmarks = 0;
    while (in < end) {
        int16_t w, code, arg;
        if (ph_nside) side_to_clause((int16_t)(in - phonemes), (int16_t)(out - phonemes));
        w = *in++;
        if (!(w & 0x3000)) {                    /* a plain symbol */
            *out++ = w;
            continue;
        }
        code = (int16_t)(w & 0xff);
        if (!(sym_flags[code] & 0x4000)) {      /* a symbol with user duration [and F0] */
            int16_t slot, nextra;
            *out++ = code;
            slot = (int16_t)((out - phonemes) + 4);
            if (slot >= PH_MAXALLO) slot = PH_MAXALLO - 1;
            allodurs[slot] = *in;
            nextra = (int16_t)((w & 0x3000) >> 12);
            if (nextra > 1) {
                f0mode = 1;
                f0tar[slot] = in[1];
            }
            in += nextra;
            continue;
        }
        arg = *in++;                            /* a pseudo-phoneme and its argument */
        switch (code) {
        case PSEUDO_VOICE:
            klclause(phonemes, out);
            clear_user_values();
            out = phonemes + 1;
            voice_code = (int16_t)(arg + 0x6b);
            if (voice_code < 0x6b || voice_code > 0x73) {
                dt_error_flags |= 8;
                log_error("Illegal voice %d\n", voice_code - 0x6b);
            } else {
                load_voice_definition();
                voice_last = voice_code;
            }
            break;
        case PSEUDO_RATE:
            klclause(phonemes, out);
            clear_user_values();
            out = phonemes + 1;
            sprate = arg;
            break;
        case PSEUDO_INDEX1:
        case PSEUDO_INDEX2:
            *out++ = code;
            *out++ = arg;
            break;
        case PSEUDO_PERPAUSE:
            perpause = ms_to_frames(arg);
            break;
        case PSEUDO_COMPAUSE:
            compause = ms_to_frames(arg);
            break;
        default:                                /* 0x68 and every other pseudo-phoneme */
            dt_error_flags |= 8;
            log_error("Bad pseudo-phoneme %d\n", w);
            break;
        }
    }
    if (ph_nside) side_to_clause(0x7fff, (int16_t)(out - phonemes));  /* marks after the last word */
    klclause(phonemes, out);
    ph_nside = 0;
}
