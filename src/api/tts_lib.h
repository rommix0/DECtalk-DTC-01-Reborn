/* tts_lib.h - what ttsapi.c shares with the library's other files (REFERENCE.md s17.10). */
#ifndef TTS_LIB_H
#define TTS_LIB_H
#include "ttsapi.h"

int tts_valid(LPTTS_HANDLE_T h);                        /* the one live handle */
MMRESULT tts_error(LPTTS_HANDLE_T h, MMRESULT r);       /* remember r for TextToSpeechGetLastError; returns it */
MMRESULT tts_last_error(LPTTS_HANDLE_T h);

#endif
