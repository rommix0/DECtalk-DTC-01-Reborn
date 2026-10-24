/* narrate - Perfect Paul for the explainer video (videos/DESIGN.md). Runs text through the speech engine (src/speech,
 * the ROM's code checked word for word) with every library fix off, so the voice is exactly v1.8's, and writes the
 * samples to a wave file: 16-bit mono at the DAC's 10 kHz, never resampled.
 *
 * usage: narrate out.wav "text" ["lead-in"]
 *   The lead-in is spoken first and not written. make_video.py passes the power-up greeting, so that the state carried
 *   from one clause to the next is the ROM's when the text starts (as pitch_graphs/pitchlog.c does).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"

static FILE *wf;
static long nsamples;

static void put32(unsigned char *p, unsigned long v)
{
    int i;
    for (i = 0; i < 4; i++) p[i] = (unsigned char)(v >> 8 * i);
}

/* the 44-byte RIFF/WAVE header for n samples */
static void header(long n)
{
    static const unsigned char tmpl[45] ="RIFF....WAVEfmt \x10\0\0\0\x01\0\x01\0....\x00\x00\x00\x00\x02\0\x10\0data....";
    unsigned char h[44];
    memcpy(h, tmpl, 44);
    put32(h + 4, 36 + 2 * n);
    put32(h + 24, 10000);
    put32(h + 28, 20000);
    put32(h + 40, 2 * n);
    fseek(wf, 0, SEEK_SET);
    fwrite(h, 1, 44, wf);
}

static void say(const char *text, int keep)
{
    static int16_t pcm[4096];
    int k, n, i;
    engine_write(text, (int)strlen(text));
    engine_write("\v", 1);                      /* CTRL-K: speak the last clause now */
    for (k = 0; k < 100000; k++) {
        n = engine_run(pcm, 4096);
        if (keep)
            for (i = 0; i < n; i++) {
                fputc(pcm[i] & 0xFF, wf);
                fputc((pcm[i] >> 8) & 0xFF, wf);
            }
        nsamples += keep ? n : 0;
        if (n < 4096 && engine_quiet()) break;
    }
}

int main(int argc, char **argv)
{
    static int16_t pcm[4096];
    if (argc < 3) {
        fprintf(stderr, "usage: narrate out.wav \"text\" [\"lead-in\"]\n");
        return 2;
    }
    wf = fopen(argv[1], "wb");
    if (!wf) {
        perror(argv[1]);
        return 1;
    }
    header(0);
    if (!engine_init(0, NULL)) return 1;
    while (engine_run(pcm, 4096) == 4096) {}   /* whatever the boot queues */
    if (argc > 3) say(argv[3], 0);
    say(argv[2], 1);
    header(nsamples);
    fclose(wf);
    engine_shutdown();
    return 0;
}
