/* tts_audio.h - the audio device for the library (REFERENCE.md s17.10): 10,000 samples a second, mono, 16-bit,
 * never resampled by us (the system's mixer may). Windows: waveOut (winmm), as dapi. Linux: ALSA, loaded when
 * needed (dlopen("libasound.so.2")), so the library needs no sound library to link or to run without a device.
 *
 * Nothing here waits: the library's thread asks how much the device takes (tts_audio_room), writes that much, and
 * asks how far it has played (tts_audio_done) to report index marks and the end of the audio. */
#ifndef TTS_AUDIO_H
#define TTS_AUDIO_H
#include <stdint.h>

typedef struct tts_audio tts_audio_t;

/* device: WAVE_MAPPER (-1) for the default one, or its number. NULL on failure, with *err an MMSYSERR_ code. */
tts_audio_t *tts_audio_open(unsigned device, unsigned *err);
int tts_audio_room(tts_audio_t *a);                     /* samples it takes now */
int tts_audio_write(tts_audio_t *a, const int16_t *pcm, int n);    /* returns how many it took (<= room) */
int64_t tts_audio_written(tts_audio_t *a);              /* samples written since it was opened */
int64_t tts_audio_done(tts_audio_t *a);                 /* of those, played (or dropped by a flush) */
void tts_audio_flush(tts_audio_t *a);                   /* drop what is not played yet */
void tts_audio_pause(tts_audio_t *a, int on);
void tts_audio_close(tts_audio_t *a);                   /* drops what is not played */

#endif
