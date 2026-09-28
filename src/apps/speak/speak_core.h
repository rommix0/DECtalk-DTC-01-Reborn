/* speak_core.h - the portable part of speak, the speaking text editor (REFERENCE.md s17.12).
 *
 * speak is dapi's SDK sample (samples/speak/Speak.c) ported to v1.8 and split in two: this core, plain C over the
 * library's API, and one front end per system: speak_win.c (Win32, the sample's own window) and speak_gtk.c (GTK 3).
 * The core has what does not depend on the window system: the voice buttons' table, the rate slider's range and
 * steps, the text marked with index marks for highlighting and the word each mark belongs to, find, reading and
 * writing files, conversion to a wave file and the library version check.
 */
#ifndef SPEAK_CORE_H
#define SPEAK_CORE_H

#include <stddef.h>
#include "ttsapi.h"

#define SPEAK_APP_NAME "Speak"

/* ---- the voice buttons: dapi's nine, in dapi's order; v1.8 has Val ([:nv]) in Wendy's place ---- */
#define SPEAK_NVOICES 9
typedef struct {
    const char *command;        /* what the button sends first ("[:np]") */
    const char *test;           /* then what it says (with TTS_FORCE) */
    const char *label;          /* its tooltip */
} speak_voice_t;
extern const speak_voice_t speak_voices[SPEAK_NVOICES];

/* ---- the pictures (speak_pics.c): the voices, then play, pause and stop; each raised and pressed, a BMP file ---- */
enum { SPEAK_PIC_PLAY = SPEAK_NVOICES, SPEAK_PIC_PAUSE, SPEAK_PIC_STOP, SPEAK_NPICS };
typedef struct {
    const unsigned char *bmp;
    size_t size;
} speak_pic_t;
extern const speak_pic_t speak_pics[SPEAK_NPICS][2];

/* ---- the rate slider: v1.8's range (dapi's sample: 75-600), steps of 5 (line) and 20 (page), in wpm ---- */
#define SPEAK_RATE_MIN 120
#define SPEAK_RATE_MAX 350
#define SPEAK_RATE_DEFAULT 200      /* the sample's; v1.8 starts at 180, so speak sets it at startup */
#define SPEAK_RATE_LINE 5
#define SPEAK_RATE_PAGE 20
int speak_rate_clamp(int rate);     /* into the range */
int speak_rate_round(int rate);     /* a dragged position: to a multiple of 5, in the range */

/* ---- highlighting: the text with an index mark before each word ----
 * The user turns it on and off (Edit > Highlighting, or the "Highlight words" box); the front ends remember the
 * choice. On by default: the library's marks leave the speech as it is (REFERENCE s17.13). */
#define SPEAK_HIGHLIGHT_DEFAULT 1
typedef struct {
    unsigned long start, end;       /* the word, as offsets into the text given to speak_marks_build */
} speak_word_t;
typedef struct {
    speak_word_t *words;
    unsigned long nwords, cap;
    unsigned long next;             /* the first word not yet reached */
} speak_marks_t;
/* The text with "[:in n]" before each word: n = 1, 2, ... (after 32767, 1 again), words split at blanks, tabs and
 * line ends. Text in [ ] (commands, phonemes) gets no mark: v1.8 would say an empty clause for a mark before a
 * [:n.] voice command. Returns the new text (free it), NULL if out of memory. */
char *speak_marks_build(speak_marks_t *m, const char *text);
/* The word mark n belongs to (the first not yet reached with that number): 1 with its offsets, 0 if none */
int speak_marks_find(speak_marks_t *m, unsigned n, unsigned long *start, unsigned long *end);
void speak_marks_free(speak_marks_t *m);

/* Speak text (TTS_FORCE at the end). With highlight, the marks go in first (m is rebuilt). */
MMRESULT speak_text(LPTTS_HANDLE_T h, const char *text, int highlight, speak_marks_t *m);

/* ---- conversion to a wave file: dapi's three formats, all at v1.8's 10 kHz ---- */
typedef struct {
    DWORD format;
    const char *label;
} speak_wave_format_t;
#define SPEAK_NWAVE 3
extern const speak_wave_format_t speak_wave_formats[SPEAK_NWAVE];
/* The text into a wave file; returns when it is written. On an error, *what says which call failed. */
MMRESULT speak_to_wave(LPTTS_HANDLE_T h, const char *text, const char *path, DWORD format, const char **what);

/* ---- find ---- */
#define SPEAK_FIND_DOWN 1           /* else up */
#define SPEAK_FIND_CASE 2           /* match case */
#define SPEAK_FIND_WORD 4           /* whole words */
/* Look for what in doc[0..len): down from offset from, or up, ending at or before from. 1 with *start. */
int speak_find(const char *doc, size_t len, const char *what, size_t from, int flags, size_t *start);

/* ---- files: a whole file as a NUL-terminated buffer (free it; *len without the NUL), or written from one ---- */
char *speak_read_file(const char *path, size_t *len);
int speak_write_file(const char *path, const char *data, size_t len);
/* Line ends to CR LF (the Win32 edit control's), or from CR LF to LF (for files written on Linux). Free the result. */
char *speak_to_crlf(const char *text);
char *speak_from_crlf(const char *text);

/* ---- the library: dapi's DLL API 1; highlighting needs its revision 3 or more (the sample's rule). Returns 0 with
 * a message for a library speak cannot use, else 1 (*highlight: whether marks can be used). ---- */
int speak_check_version(int *highlight, char *msg, size_t size);

/* ---- the command line: speak [file] [dictionary] (quotes group blanks); 1 for "-?" / "/?" ---- */
int speak_parse_args(int argc, char **argv, const char **file, const char **dict);
#define SPEAK_USAGE "Usage: speak [filename] [dictionary_name]"

#endif
