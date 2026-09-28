/* speak_core.c - the portable part of speak (REFERENCE.md s17.12); see speak_core.h */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "speak_core.h"

#define MARK_MAX 32767              /* an index mark's largest value */

/* dapi's buttons: [:np] ... [:nr], and [:nw] (Wendy) in the ninth; v1.8's ninth voice is Val, [:nv] (s8.1) */
const speak_voice_t speak_voices[SPEAK_NVOICES] = {
    {"[:np]", "Paul. ", "Perfect Paul"},
    {"[:nb]", "Betty. ", "Beautiful Betty"},
    {"[:nh]", "Harry. ", "Huge Harry"},
    {"[:nf]", "Frank. ", "Frail Frank"},
    {"[:nd]", "Dennis. ", "Doctor Dennis (in v1.8, Frank's voice)"},
    {"[:nk]", "Kit. ", "Kit the Kid"},
    {"[:nu]", "Ursula. ", "Uppity Ursula"},
    {"[:nr]", "Rita. ", "Rough Rita"},
    {"[:nv]", "Val. ", "Variable Val (Paul until [:dv save])"},
};

const speak_wave_format_t speak_wave_formats[SPEAK_NWAVE] = {
    {WAVE_FORMAT_1M16, "Mono 10 kHz, 16-Bit"},
    {WAVE_FORMAT_1M08, "Mono 10 kHz, 8-Bit"},
    {WAVE_FORMAT_08M08, "Mono 10 kHz, \xB5-Law"},
};

int speak_rate_clamp(int rate)
{
    return rate < SPEAK_RATE_MIN ? SPEAK_RATE_MIN : rate > SPEAK_RATE_MAX ? SPEAK_RATE_MAX : rate;
}

int speak_rate_round(int rate)
{
    return speak_rate_clamp(rate / SPEAK_RATE_LINE * SPEAK_RATE_LINE);
}

/* ---- highlighting ---- */

static int blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int add_word(speak_marks_t *m, unsigned long start, unsigned long end)
{
    if (m->nwords == m->cap) {
        unsigned long cap = m->cap ? 2 * m->cap : 256;
        speak_word_t *w = (speak_word_t *)realloc(m->words, cap * sizeof *w);
        if (!w) return 0;
        m->words = w;
        m->cap = cap;
    }
    m->words[m->nwords].start = start;
    m->words[m->nwords++].end = end;
    return 1;
}

char *speak_marks_build(speak_marks_t *m, const char *text)
{
    size_t len = strlen(text), i = 0, o = 0, words = 0;
    char *out;
    int depth = 0;
    for (i = 0; i < len; i++)                   /* at most one mark per word start */
        if (!blank(text[i]) && (i == 0 || blank(text[i - 1]))) words++;
    if (!(out = (char *)malloc(len + words * 12 + 1))) return NULL;
    m->nwords = 0;
    m->next = 0;
    i = 0;
    while (i < len) {
        size_t start;
        while (i < len && blank(text[i])) out[o++] = text[i++];
        if (i >= len) break;
        start = i;
        if (text[i] != '[') {                   /* a word: its mark first */
            if (!add_word(m, (unsigned long)start, 0)) {
                free(out);
                return NULL;
            }
            o += (size_t)sprintf(out + o, "[:in %lu]", (m->nwords - 1) % MARK_MAX + 1);
        }
        while (i < len && (depth > 0 || !blank(text[i]))) {    /* to the next blank outside [ ] */
            if (text[i] == '[') depth++;
            else if (text[i] == ']' && depth > 0) depth--;
            out[o++] = text[i++];
        }
        if (text[start] != '[') m->words[m->nwords - 1].end = (unsigned long)i;
    }
    out[o] = 0;
    return out;
}

int speak_marks_find(speak_marks_t *m, unsigned n, unsigned long *start, unsigned long *end)
{
    unsigned long k;
    if (n < 1 || n > MARK_MAX) return 0;
    for (k = m->next; k < m->nwords; k++) {
        if (k % MARK_MAX + 1 == n) {
            m->next = k + 1;
            *start = m->words[k].start;
            *end = m->words[k].end;
            return 1;
        }
    }
    return 0;
}

void speak_marks_free(speak_marks_t *m)
{
    free(m->words);
    memset(m, 0, sizeof *m);
}

MMRESULT speak_text(LPTTS_HANDLE_T h, const char *text, int highlight, speak_marks_t *m)
{
    MMRESULT r;
    char *t;
    if (!highlight) return TextToSpeechSpeak(h, (LPSTR)text, TTS_FORCE);
    if (!(t = speak_marks_build(m, text))) return MMSYSERR_NOMEM;
    r = TextToSpeechSpeak(h, t, TTS_FORCE);
    free(t);
    return r;
}

MMRESULT speak_to_wave(LPTTS_HANDLE_T h, const char *text, const char *path, DWORD format, const char **what)
{
    MMRESULT r;
    *what = "TextToSpeechOpenWaveOutFile";
    if ((r = TextToSpeechOpenWaveOutFile(h, (char *)path, format)) != MMSYSERR_NOERROR) return r;
    *what = "TextToSpeechSpeak";
    r = TextToSpeechSpeak(h, (LPSTR)text, TTS_FORCE);
    if (r == MMSYSERR_NOERROR) *what = "TextToSpeechCloseWaveOutFile";
    if (TextToSpeechCloseWaveOutFile(h) != MMSYSERR_NOERROR && r == MMSYSERR_NOERROR) r = MMSYSERR_ERROR;
    return r;
}

/* ---- find ---- */

static int word_char(char c) { return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }

static int match_at(const char *doc, size_t len, const char *what, size_t wl, size_t at, int flags)
{
    size_t j;
    if (at + wl > len) return 0;
    for (j = 0; j < wl; j++) {
        char a = doc[at + j], b = what[j];
        if (flags & SPEAK_FIND_CASE ? a != b : tolower((unsigned char)a) != tolower((unsigned char)b)) return 0;
    }
    if (flags & SPEAK_FIND_WORD) {
        if (at > 0 && word_char(doc[at - 1])) return 0;
        if (at + wl < len && word_char(doc[at + wl])) return 0;
    }
    return 1;
}

int speak_find(const char *doc, size_t len, const char *what, size_t from, int flags, size_t *start)
{
    size_t wl = strlen(what), at;
    if (!wl || wl > len) return 0;
    if (from > len) from = len;
    if (flags & SPEAK_FIND_DOWN) {
        for (at = from; at + wl <= len; at++)
            if (match_at(doc, len, what, wl, at, flags)) break;
        if (at + wl > len) return 0;
    } else {
        if (from < wl) return 0;
        for (at = from - wl + 1; at-- > 0;)
            if (match_at(doc, len, what, wl, at, flags)) break;
        if (at == (size_t)-1) return 0;
    }
    *start = at;
    return 1;
}

/* ---- files ---- */

char *speak_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *b = NULL, *nb;
    size_t n = 0, cap = 0, k;
    if (!f) return NULL;
    for (;;) {
        if (cap - n < 4096) {
            cap = cap ? 2 * cap : 65536;
            if (!(nb = (char *)realloc(b, cap + 1))) {
                free(b);
                fclose(f);
                return NULL;
            }
            b = nb;
        }
        k = fread(b + n, 1, cap - n, f);
        n += k;
        if (!k) break;
    }
    fclose(f);
    b[n] = 0;
    if (len) *len = n;
    return b;
}

int speak_write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) return 0;
    ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

char *speak_to_crlf(const char *text)
{
    size_t n = 0, i;
    char *out, *o;
    for (i = 0; text[i]; i++) n += text[i] == '\n' && (i == 0 || text[i - 1] != '\r') ? 2 : 1;
    if (!(o = out = (char *)malloc(n + 1))) return NULL;
    for (i = 0; text[i]; i++) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) *o++ = '\r';
        *o++ = text[i];
    }
    *o = 0;
    return out;
}

char *speak_from_crlf(const char *text)
{
    char *out = (char *)malloc(strlen(text) + 1), *o = out;
    if (!out) return NULL;
    for (; *text; text++)
        if (!(text[0] == '\r' && text[1] == '\n')) *o++ = *text;
    *o = 0;
    return out;
}

/* ---- the library ---- */

int speak_check_version(int *highlight, char *msg, size_t size)
{
    LPSTR s;
    DWORD v = TextToSpeechVersion(&s);
    unsigned api = (v >> 8) & 0xff, rev = v & 0xff;
    *highlight = 0;
    if (api != 1) {
        snprintf(msg, size, "Incorrect DECtalk library version\nVersion %u.%02u found, 1.xx expected.", api, rev);
        return 0;
    }
    *highlight = rev >= 3;
    return 1;
}

int speak_parse_args(int argc, char **argv, const char **file, const char **dict)
{
    *file = *dict = NULL;
    if (argc > 1 && (!strcmp(argv[1], "-?") || !strcmp(argv[1], "/?") || !strcmp(argv[1], "-h") ||
                     !strcmp(argv[1], "--help")))
        return 1;
    if (argc > 1) *file = argv[1];
    if (argc > 2) *dict = argv[2];
    return 0;
}
