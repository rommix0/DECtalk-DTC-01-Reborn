/* tts_audio.c - the audio device (tts_audio.h; REFERENCE.md s17.10). */
#include <stdlib.h>
#include <string.h>
#include "tts_audio.h"

#define RATE 10000

#ifdef _WIN32
/* ---- Windows: waveOut, a ring of short buffers ---- */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

#define NBUF 8                  /* 8 x 25.6 ms: enough to ride over a late wake-up, little to drop on a Reset */
#define BUFSAMPLES 256

struct tts_audio {
    HWAVEOUT wo;
    WAVEHDR hdr[NBUF];
    int16_t data[NBUF][BUFSAMPLES];
    int next, busy;             /* the next buffer to fill; buffers with the device */
    int64_t written;
    int64_t base;               /* samples done before the device's position last went back to 0 (a reset) */
};

static void reap(tts_audio_t *a)
{
    while (a->busy) {
        WAVEHDR *h = &a->hdr[(a->next - a->busy + NBUF) % NBUF];
        if (!(h->dwFlags & WHDR_DONE)) break;
        waveOutUnprepareHeader(a->wo, h, sizeof *h);
        a->busy--;
    }
}

tts_audio_t *tts_audio_open(unsigned device, unsigned *err)
{
    WAVEFORMATEX f;
    MMRESULT r;
    tts_audio_t *a = (tts_audio_t *)calloc(1, sizeof *a);
    if (!a) {
        *err = MMSYSERR_NOMEM;
        return NULL;
    }
    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 1;
    f.nSamplesPerSec = RATE;
    f.nAvgBytesPerSec = 2 * RATE;
    f.nBlockAlign = 2;
    f.wBitsPerSample = 16;
    r = waveOutOpen(&a->wo, device, &f, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) {
        free(a);
        *err = r;
        return NULL;
    }
    *err = MMSYSERR_NOERROR;
    return a;
}

int tts_audio_room(tts_audio_t *a)
{
    reap(a);
    return (NBUF - a->busy) * BUFSAMPLES;
}

int tts_audio_write(tts_audio_t *a, const int16_t *pcm, int n)
{
    int done = 0;
    reap(a);
    while (n > 0 && a->busy < NBUF) {
        WAVEHDR *h = &a->hdr[a->next];
        int k = n < BUFSAMPLES ? n : BUFSAMPLES;
        memcpy(a->data[a->next], pcm, (size_t)k * sizeof *pcm);
        memset(h, 0, sizeof *h);
        h->lpData = (LPSTR)a->data[a->next];
        h->dwBufferLength = (DWORD)k * 2;
        if (waveOutPrepareHeader(a->wo, h, sizeof *h) != MMSYSERR_NOERROR) break;
        if (waveOutWrite(a->wo, h, sizeof *h) != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(a->wo, h, sizeof *h);
            break;
        }
        a->next = (a->next + 1) % NBUF;
        a->busy++;
        a->written += k;
        pcm += k;
        n -= k;
        done += k;
    }
    return done;
}

int64_t tts_audio_written(tts_audio_t *a) { return a->written; }

int64_t tts_audio_done(tts_audio_t *a)
{
    MMTIME t;
    int64_t d;
    reap(a);
    if (!a->busy) return a->written;            /* every buffer is back */
    memset(&t, 0, sizeof t);
    t.wType = TIME_SAMPLES;
    if (waveOutGetPosition(a->wo, &t, sizeof t) != MMSYSERR_NOERROR) return a->base;
    if (t.wType == TIME_SAMPLES) d = a->base + t.u.sample;
    else if (t.wType == TIME_BYTES) d = a->base + t.u.cb / 2;
    else return a->base;
    return d < a->written ? d : a->written;
}

void tts_audio_flush(tts_audio_t *a)
{
    waveOutReset(a->wo);                        /* every buffer comes back done; the position goes back to 0 */
    reap(a);
    a->base = a->written;
}

void tts_audio_pause(tts_audio_t *a, int on)
{
    if (on) waveOutPause(a->wo);
    else waveOutRestart(a->wo);
}

void tts_audio_close(tts_audio_t *a)
{
    if (!a) return;
    waveOutReset(a->wo);
    reap(a);
    waveOutClose(a->wo);
    free(a);
}

#else
/* ---- Linux: ALSA, loaded with dlopen ---- */
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>

#define MMSYSERR_NOERROR 0
#define MMSYSERR_BADDEVICEID 2
#define MMSYSERR_NODRIVER 6
#define MMSYSERR_NOMEM 7

typedef struct snd_pcm snd_pcm_t;
enum { PCM_PLAYBACK = 0, PCM_NONBLOCK = 1, FORMAT_S16_LE = 2, ACCESS_RW_INTERLEAVED = 3 };
enum { STATE_PREPARED = 2, STATE_RUNNING = 3, STATE_XRUN = 4 };

static struct {
    void *lib;
    int (*open)(snd_pcm_t **, const char *, int, int);
    int (*set_params)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned);
    long (*writei)(snd_pcm_t *, const void *, unsigned long);
    long (*avail)(snd_pcm_t *);
    int (*delay)(snd_pcm_t *, long *);
    int (*drop)(snd_pcm_t *);
    int (*prepare)(snd_pcm_t *);
    int (*recover)(snd_pcm_t *, int, int);
    int (*pause)(snd_pcm_t *, int);
    int (*start)(snd_pcm_t *);
    int (*state)(snd_pcm_t *);
    int (*close)(snd_pcm_t *);
} A;

static int load(void)
{
    if (A.lib) return 1;
    if (!(A.lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL))) return 0;
#define SYM(f, n) if (!(*(void **)&A.f = dlsym(A.lib, n))) goto bad;
    SYM(open, "snd_pcm_open") SYM(set_params, "snd_pcm_set_params") SYM(writei, "snd_pcm_writei")
    SYM(avail, "snd_pcm_avail") SYM(delay, "snd_pcm_delay") SYM(drop, "snd_pcm_drop")
    SYM(prepare, "snd_pcm_prepare") SYM(recover, "snd_pcm_recover") SYM(pause, "snd_pcm_pause")
    SYM(start, "snd_pcm_start") SYM(state, "snd_pcm_state") SYM(close, "snd_pcm_close")
#undef SYM
    return 1;
bad:
    dlclose(A.lib);
    A.lib = NULL;
    return 0;
}

struct tts_audio {
    snd_pcm_t *pcm;
    int64_t written;
};

tts_audio_t *tts_audio_open(unsigned device, unsigned *err)
{
    char name[32];
    tts_audio_t *a;
    if (!load()) {
        *err = MMSYSERR_NODRIVER;
        return NULL;
    }
    if (!(a = (tts_audio_t *)calloc(1, sizeof *a))) {
        *err = MMSYSERR_NOMEM;
        return NULL;
    }
    if (device == (unsigned)-1) strcpy(name, "default");
    else snprintf(name, sizeof name, "plughw:%u", device);
    if (A.open(&a->pcm, name, PCM_PLAYBACK, PCM_NONBLOCK) < 0) {
        free(a);
        *err = device == (unsigned)-1 ? MMSYSERR_NODRIVER : MMSYSERR_BADDEVICEID;
        return NULL;
    }
    /* soft resampling on (the system's rate converter, not ours), 100 ms of buffer */
    if (A.set_params(a->pcm, FORMAT_S16_LE, ACCESS_RW_INTERLEAVED, 1, RATE, 1, 100000) < 0) {
        A.close(a->pcm);
        free(a);
        *err = MMSYSERR_NODRIVER;
        return NULL;
    }
    *err = MMSYSERR_NOERROR;
    return a;
}

int tts_audio_room(tts_audio_t *a)
{
    long n = A.avail(a->pcm);
    if (n < 0) {                                /* an underrun: the device played everything; start again */
        A.recover(a->pcm, (int)n, 1);
        n = A.avail(a->pcm);
    }
    return n < 0 ? 0 : (int)n;
}

int tts_audio_write(tts_audio_t *a, const int16_t *pcm, int n)
{
    long k = A.writei(a->pcm, pcm, (unsigned long)n);
    if (k == -EPIPE || k == -ESTRPIPE) {
        A.recover(a->pcm, (int)k, 1);
        k = A.writei(a->pcm, pcm, (unsigned long)n);
    }
    if (k <= 0) return 0;
    if (A.state(a->pcm) == STATE_PREPARED) A.start(a->pcm);    /* play now, not when the buffer is full */
    a->written += k;
    return (int)k;
}

int64_t tts_audio_written(tts_audio_t *a) { return a->written; }

int64_t tts_audio_done(tts_audio_t *a)
{
    long d = 0;
    int s = A.state(a->pcm);
    if (s != STATE_RUNNING && s != STATE_PREPARED) return a->written;  /* stopped after an underrun: all played */
    if (A.delay(a->pcm, &d) < 0 || d < 0) return a->written;
    return d > a->written ? 0 : a->written - d;
}

void tts_audio_flush(tts_audio_t *a)
{
    A.drop(a->pcm);
    A.prepare(a->pcm);
}

void tts_audio_pause(tts_audio_t *a, int on)
{
    A.pause(a->pcm, on);                        /* not every device can; then the buffered 100 ms plays on */
}

void tts_audio_close(tts_audio_t *a)
{
    if (!a) return;
    A.drop(a->pcm);
    A.close(a->pcm);
    free(a);
}
#endif
