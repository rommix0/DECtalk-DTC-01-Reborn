/* tts_stubs.c - the library's small exports (REFERENCE.md s17.4, s17.10, s17.11): version, capabilities,
 * language, rate, speaker, modes, v1.8's voices, and the dapi functions v1.8 has no counterpart for (s17.4). The rest
 * is in ttsapi.c and tts_dict.c; every export of dectalk.def exists, so dapi programs load. */
#define BLD_DECTALK_DLL
#include <stdio.h>
#include <string.h>
#include "ttsapi.h"
#include "tts_lib.h"
#include "../speech/engine.h"

#define REVISION 3              /* the library's revision: speak turns highlighting on from 3 (s17.1) */
#define VERSION_STRING "DECtalk v1.8"

/* ---- version, capabilities, language ---- */

/* @27: DECtalk 1.8, DLL API 1 (dapi's DECtalk DLL), this library's revision */
TTSAPI DWORD TextToSpeechVersion(LPSTR *ppszVersion)
{
    static char s[] = VERSION_STRING;
    if (ppszVersion) *ppszVersion = s;
    return (DWORD)1 << 24 | (DWORD)8 << 16 | (DWORD)1 << 8 | REVISION;
}

/* @33 */
TTSAPI DWORD TextToSpeechVersionEx(LPVERSION_INFO *ppVersion)
{
    static char ver[] = VERSION_STRING, lang[] = "US";
    static VERSION_INFO v;
    v.StructSize = sizeof v;
    v.StructVersion = VERSION_STRUCT_VER;
    v.DLLVersion = 1 << 8 | REVISION;
    v.DTalkVersion = 0x0108;
    v.VerString = ver;
    v.Language = lang;
    v.Features = 0;
    if (ppVersion) *ppVersion = &v;
    return TextToSpeechVersion(NULL);
}

/* @17: one language, 10,000 Hz, 120-350 wpm, 9 speakers (Val included) */
TTSAPI MMRESULT TextToSpeechGetCaps(LPTTS_CAPS_T pTTScaps)
{
    static LANGUAGE_PARAMS_T lp = { TTS_AMERICAN_ENGLISH, 0 };
    if (!pTTScaps) return MMSYSERR_INVALPARAM;
    pTTScaps->dwNumberOfLanguages = 1;
    pTTScaps->lpLanguageParamsArray = &lp;
    pTTScaps->dwSampleRate = 10000;
    pTTScaps->dwMinimumSpeakingRate = 120;
    pTTScaps->dwMaximumSpeakingRate = 350;
    pTTScaps->dwNumberOfPredefinedSpeakers = 9;
    pTTScaps->dwCharacterSet = TTS_ASCII;
    pTTScaps->Version = TextToSpeechVersion(NULL);
    return MMSYSERR_NOERROR;
}

TTSAPI ULONG TextToSpeechGetLastError(LPTTS_HANDLE_T phTTS) { return tts_last_error(phTTS); }     /* @30 */

TTSAPI MMRESULT TextToSpeechGetLanguage(LPTTS_HANDLE_T phTTS, LPLANGUAGE_T pLanguage)            /* @15 */
{
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    if (!pLanguage) return tts_error(phTTS, MMSYSERR_INVALPARAM);
    *pLanguage = TTS_AMERICAN_ENGLISH;
    return MMSYSERR_NOERROR;
}

TTSAPI MMRESULT TextToSpeechSetLanguage(LPTTS_HANDLE_T phTTS, LANGUAGE_T Language)               /* @16 */
{
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    return Language == TTS_AMERICAN_ENGLISH ? MMSYSERR_NOERROR : tts_error(phTTS, MMSYSERR_INVALPARAM);
}

TTSAPI UINT TextToSpeechStartLang(char *pszLanguage)                                            /* @34 */
{
    return pszLanguage && !strcmp(pszLanguage, "US") ? 1 : TTS_NOT_SUPPORTED;
}

TTSAPI BOOL TextToSpeechSelectLang(LPTTS_HANDLE_T phTTS, UINT uiLanguage)                        /* @35 */
{
    (void)phTTS;
    return uiLanguage <= 1;
}

TTSAPI BOOL TextToSpeechCloseLang(char *pszLanguage) { return pszLanguage && !strcmp(pszLanguage, "US"); }  /* @36 */
TTSAPI DWORD TextToSpeechGetFeatures(void) { return 0; }                                         /* @37 */

TTSAPI DWORD TextToSpeechEnumLangs(LPLANG_ENUM *ppLangs)                                         /* @38 */
{
    static LANG_ENTRY e = { "US", "American English" };
    static LANG_ENUM l;
    l.Languages = 1;
    l.MultiLang = FALSE;
    l.Entries = &e;
    if (ppLangs) *ppLangs = &l;
    return 1;
}

/* ---- rate, speaker, modes: what a host would send ---- */

static MMRESULT queue_command(LPTTS_HANDLE_T h, const char *cmd)
{
    char s[64];
    snprintf(s, sizeof s, "\x02%s\x03", cmd);   /* STX/ETX work in any DT_MODE */
    return TextToSpeechSpeak(h, s, TTS_NORMAL);
}

TTSAPI MMRESULT TextToSpeechSetRate(LPTTS_HANDLE_T phTTS, DWORD dwRate)                          /* @12 */
{
    char s[32];
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    snprintf(s, sizeof s, ":ra %lu", (unsigned long)dwRate);
    return queue_command(phTTS, s);
}

TTSAPI MMRESULT TextToSpeechGetRate(LPTTS_HANDLE_T phTTS, LPDWORD pdwRate)                       /* @11 */
{
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    if (!pdwRate) return tts_error(phTTS, MMSYSERR_INVALPARAM);
    *pdwRate = (DWORD)engine_get(ENGINE_RATE);  /* the rate being spoken now (queued changes come later) */
    return MMSYSERR_NOERROR;
}

TTSAPI MMRESULT TextToSpeechSetSpeaker(LPTTS_HANDLE_T phTTS, SPEAKER_T Speaker)                  /* @14 */
{
    static const char letter[] = "pbhfdkurv";   /* dapi's numbers, v1.8's :n commands (8 = Val) */
    char s[8];
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    if (Speaker > 8) return tts_error(phTTS, MMSYSERR_INVALPARAM);
    snprintf(s, sizeof s, ":n%c", letter[Speaker]);
    return queue_command(phTTS, s);
}

TTSAPI MMRESULT TextToSpeechGetSpeaker(LPTTS_HANDLE_T phTTS, LPSPEAKER_T pSpeaker)               /* @13 */
{
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    if (!pSpeaker) return tts_error(phTTS, MMSYSERR_INVALPARAM);
    *pSpeaker = (SPEAKER_T)engine_get(ENGINE_VOICE);
    return MMSYSERR_NOERROR;
}

TTSAPI MMRESULT TextToSpeechSetMode(LPTTS_HANDLE_T phTTS, DWORD dwMode)                          /* @100 */
{
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    if (dwMode & ~(DWORD)(TTS_MODE_SQUARE | TTS_MODE_ASKY | TTS_MODE_MINUS)) return tts_error(phTTS, MMSYSERR_INVALFLAG);
    engine_set(ENGINE_MODE, (int)dwMode);
    return MMSYSERR_NOERROR;
}

TTSAPI MMRESULT TextToSpeechGetMode(LPTTS_HANDLE_T phTTS, LPDWORD pdwMode)                       /* @101 */
{
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    if (!pdwMode) return tts_error(phTTS, MMSYSERR_INVALPARAM);
    *pdwMode = (DWORD)engine_get(ENGINE_MODE);
    return MMSYSERR_NOERROR;
}

/* ---- v1.8's voices ---- */

/* @106: a voice's 28 [:dv] values (DTC01_VOICE_T, s8.1 order): PAUL ... RITA the built-in records, VAL (8) the
 * :nv record, TTS_CURRENT_VOICE the voice being spoken now, with its [:dv] changes */
TTSAPI MMRESULT TextToSpeechGetVoice(LPTTS_HANDLE_T phTTS, SPEAKER_T Speaker, DTC01_VOICE_T *pVoice)
{
    int16_t v[28];
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    if (!pVoice || !engine_voice(Speaker == TTS_CURRENT_VOICE ? -1 : Speaker > 8 ? 99 : (int)Speaker, v))
        return tts_error(phTTS, MMSYSERR_INVALPARAM);
    memcpy(pVoice, v, sizeof v);
    return MMSYSERR_NOERROR;
}

/* @107: [:dv] with all 28 values, queued (two [:dv] commands, each within v1.8's clause buffer); v1.8 clamps each
 * to its range. The voice then stays until the next [:n?] or [:dv]; [:dv save] makes it Val's. */
TTSAPI MMRESULT TextToSpeechSetVoice(LPTTS_HANDLE_T phTTS, const DTC01_VOICE_T *pVoice)
{
    int16_t v[28];
    char s[256];
    MMRESULT r;
    int part;
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    if (!pVoice) return tts_error(phTTS, MMSYSERR_INVALPARAM);
    memcpy(v, pVoice, sizeof v);
    for (part = 0; part < 2; part++) {
        if (engine_voice_command(v, s, sizeof s, part) < 0) return tts_error(phTTS, MMSYSERR_ERROR);
        if ((r = TextToSpeechSpeak(phTTS, s, TTS_NORMAL)) != MMSYSERR_NOERROR) return r;
    }
    return MMSYSERR_NOERROR;
}

/* ---- dapi functions v1.8 has no counterpart for (s17.4) ---- */

TTSAPI VOID TextToSpeechControlPanel(LPTTS_HANDLE_T phTTS) { (void)phTTS; }                      /* @28 */
TTSAPI VOID TextToSpeechTyping(LPTTS_HANDLE_T phTTS, unsigned char key) { (void)phTTS; (void)key; }   /* @29 */
/* @31, @32, @39, @51: dapi's header gives no prototype; they take nothing and return 0 (cdecl: any arguments are
 * the caller's to remove) */
TTSAPI DWORD TextToSpeechReserved1(void) { return 0; }
TTSAPI DWORD TextToSpeechReserved2(void) { return 0; }
TTSAPI DWORD TextToSpeechReserved3(void) { return 0; }
TTSAPI DWORD TextToSpeechTuning(void) { return 0; }
TTSAPI MMRESULT TextToSpeechGetSpeakerParams(LPTTS_HANDLE_T phTTS, UINT uiIndex, SPDEFS **ppspCur, SPDEFS **ppspLoLimit,
                                             SPDEFS **ppspHiLimit, SPDEFS **ppspDefault)                /* @40 */
{
    (void)phTTS; (void)uiIndex; (void)ppspCur; (void)ppspLoLimit; (void)ppspHiLimit; (void)ppspDefault;
    return MMSYSERR_NOTSUPPORTED;
}
TTSAPI MMRESULT TextToSpeechSetSpeakerParams(LPTTS_HANDLE_T phTTS, SPDEFS *pspSet) { (void)phTTS; (void)pspSet; return MMSYSERR_NOTSUPPORTED; }  /* @41 */
TTSAPI short *TextToSpeechGetPhVdefParams(LPTTS_HANDLE_T phTTS, UINT uiIndex) { (void)phTTS; (void)uiIndex; return NULL; }  /* @52 */
