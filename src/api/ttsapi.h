/* ttsapi.h - the public API of the DECtalk DTC-01 v1.8 speech library (REFERENCE.md s17).
 *
 * The speech part of the C rebuild (src/speech, with the C DSP program) behind it: DECtalk.dll on Windows,
 * libtts_us.so on Linux (dapi's names). The host terminal emulator (src/host) and the SAY and speak programs
 * (s17.8) use it. Built (2026-09-27, s17.10-17.11): ttsapi.c (the thread, the outputs, the text calls, the tones,
 * the console and the log file), tts_audio.c (the device), tts_dict.c (the user dictionary), tts_stubs.c (the rest).
 *
 * It follows dapi's API (dapi/src/API/TTSAPI.H, dapi/src/dectalk.def): the same header and library names, function
 * names, types, constants, structures and, in dectalk.def, the same ordinals, so a program written for DECtalk's DLL
 * builds against it, and a 32-bit Windows DECtalk program can load the 32-bit DECtalk.dll unchanged. Every dapi
 * export exists; what v1.8 cannot do is a stub that says so (s17.4). Functions and constants dapi does not have are
 * marked [DTC01]; their ordinals start at 100.
 *
 * Model (s17.2): one text queue, like the firmware's text pipe; the library runs the text pipeline and the
 * synthesizer on its own thread; audio comes out at v1.8's 10,000 samples a second, never resampled, to one place at
 * a time: the audio device, a wave file or memory buffers (dapi's three outputs). Index marks, audio start/stop,
 * errors and full buffers are reported through a callback, window messages (Windows) or an event queue with a
 * waitable handle (both systems; the Linux counterpart of window messages).
 *
 * Text is v1.8's: [:np], [:ra n], [:in n], [:dv ...] and so on, as the firmware reads them from its host line
 * (s8). dapi's later spellings ([:name paul], [:index mark n], [:phoneme on]) are not translated.
 */
#ifndef _TTSAPI_H_
#define _TTSAPI_H_

#ifdef __cplusplus
extern "C" {
#endif

/* ---- export decoration (define BLD_DECTALK_DLL when building the library, as dapi does) ----
 * The functions use the C default calling convention (__cdecl), as dapi's DECtalk.dll does; dectalk.def gives the
 * names and ordinals. */
#if defined(_WIN32)
#  ifdef BLD_DECTALK_DLL
#    define TTSAPI __declspec(dllexport)
#  else
#    define TTSAPI __declspec(dllimport)
#  endif
#elif defined(__GNUC__)
#  define TTSAPI __attribute__((visibility("default")))
#else
#  define TTSAPI
#endif

/* ---- types: the Windows ones on Windows, the same sizes elsewhere (as dapi's osf/dtmmedefs.h for Linux) ---- */
#if defined(_WIN32)
#  include <windows.h>
#  include <mmsystem.h>
#else
#  include <stdint.h>
typedef uint32_t DWORD, *LPDWORD;
typedef int32_t LONG;
typedef uint32_t UINT;
typedef uint32_t ULONG;
typedef uint16_t WORD;
typedef int BOOL;
typedef char *LPSTR;
typedef void VOID;
typedef UINT MMRESULT;
#  define MMSYSERR_NOERROR 0
#  define MMSYSERR_ERROR 1
#  define MMSYSERR_BADDEVICEID 2
#  define MMSYSERR_ALLOCATED 4          /* a second instance (s17.2: one instance for now) */
#  define MMSYSERR_INVALHANDLE 5
#  define MMSYSERR_NODRIVER 6           /* no audio device: start again with DO_NOT_USE_AUDIO_DEVICE (as SAY does) */
#  define MMSYSERR_NOMEM 7
#  define MMSYSERR_NOTSUPPORTED 8
#  define MMSYSERR_INVALFLAG 10
#  define MMSYSERR_INVALPARAM 11
#  define WAVERR_BADFORMAT 32
#  define WAVE_MAPPER ((UINT)-1)        /* the default audio device */
#  define WAVE_FORMAT_1M08 0x00000001
#  define WAVE_FORMAT_1M16 0x00000004
#  ifndef TRUE
#    define TRUE 1
#    define FALSE 0
#  endif
#endif
#ifndef WAVE_FORMAT_08M08
#  define WAVE_FORMAT_08M08 0x0007      /* dapi's mu-law format */
#endif
/* The formats of TextToSpeechOpenWaveOutFile and TextToSpeechOpenInMemory choose the sample encoding only. The rate
 * is always v1.8's 10,000 Hz (the DSP's DAC rate) and is never resampled; a wave file's header says 10,000 Hz.
 *   WAVE_FORMAT_1M16   16-bit linear (the 12-bit DAC value, shifted up 4 bits)   dapi: 11,025 Hz
 *   WAVE_FORMAT_1M08   8-bit linear, unsigned                                   dapi: 11,025 Hz
 *   WAVE_FORMAT_08M08  8-bit mu-law                                             dapi: 8,000 Hz
 * Anything else is WAVERR_BADFORMAT. */

/* ---- the handle ---- */
typedef struct TTS_HANDLE_TAG TTS_HANDLE_T;
typedef TTS_HANDLE_T *LPTTS_HANDLE_T;

/* ---- the callback: (lParam1, lParam2, the instance parameter given to Startup, the message) ---- */
typedef VOID (*TTS_CALLBACK_T)(LONG lParam1, LONG lParam2, DWORD dwInstanceParameter, UINT uiMsg);
#define TTS_MSG_BUFFER 0                /* a buffer is full. lParam1 is its address in 32-bit builds, as in dapi;
                                           in 64-bit builds it is 0 and TextToSpeechReturnBuffer hands the buffer over */
#define TTS_MSG_INDEX_MARK 1            /* lParam1 = TTS_INDEX_MARK or TTS_INDEX_REPLY, lParam2 = the value (0-32767) */
#define TTS_MSG_STATUS 2                /* lParam1 = one of the codes below, lParam2 = detail (an MMRESULT) */

/* lParam1 of TTS_MSG_STATUS and TTS_MSG_INDEX_MARK (dapi's values) */
#define ERROR_IN_AUDIO_WRITE 1
#define ERROR_OPENING_WAVE_OUTPUT_DEVICE 2
#define ERROR_GETTING_DEVICE_CAPABILITIES 3
#define ERROR_READING_DICTIONARY 4
#define ERROR_WRITING_FILE 5
#define ERROR_ALLOCATING_INDEX_MARK_MEMORY 6
#define ERROR_OPENING_WAVE_FILE 7
#define ERROR_BAD_WAVE_FILE_FORMAT 8    /* 8-11: dapi's, for reading wave files; never sent (speak tests for them) */
#define ERROR_UNSUPPORTED_WAVE_FILE_FORMAT 9
#define ERROR_UNSUPPORTED_WAVE_AUDIO_FORMAT 10
#define ERROR_READING_WAVE_FILE 11
#define TTS_AUDIO_PLAY_START 12         /* the first sample after silence has been played (or delivered) */
#define TTS_AUDIO_PLAY_STOP 13          /* the output has gone silent (nothing left to say) */
#define TTS_INDEX_MARK 14               /* an index mark ([:in n], DT_INDEX) has been played (or delivered) */
#define TTS_INDEX_REPLY 31              /* [DTC01] an index mark that asks for a reply ([:re n], DT_INDEX_REPLY) */
#define ERROR_PHONEMIC_TEXT 125         /* [DTC01] phonemic text the pipeline could not parse (DSR error 25) */

/* ---- window messages (Windows, TextToSpeechStartup's hWnd form), as dapi: registered with
 * RegisterWindowMessage(name) and posted to hWnd. wParam = lParam1 above, lParam = lParam2 (for a buffer, its
 * address). ---- */
#define TTS_ERROR_MESSAGE_NAME "DECtalkErrorMessage"     /* TTS_MSG_STATUS */
#define TTS_INDEX_MESSAGE_NAME "DECtalkIndexMessage"     /* TTS_MSG_INDEX_MARK */
#define TTS_BUFFER_MESSAGE_NAME "DECtalkBufferMessage"   /* TTS_MSG_BUFFER */

/* ---- [DTC01] the event queue: the same messages, kept for the caller's own thread to read. A waitable handle
 * signals that one is there: on Linux a file descriptor that polls readable (for poll/select, GLib's
 * g_unix_fd_add, Qt's QSocketNotifier), on Windows an event object (for MsgWaitForMultipleObjects). It is how a
 * Linux GUI gets what a Windows one gets from window messages: delivery in its own thread. ---- */
#if defined(_WIN32)
typedef HANDLE TTS_WAITABLE_T;
#else
typedef int TTS_WAITABLE_T;
#endif
typedef struct TTS_EVENT_TAG {
    UINT uiMsg;                         /* TTS_MSG_BUFFER, TTS_MSG_INDEX_MARK or TTS_MSG_STATUS */
    LONG lParam1;
    LONG lParam2;
    DWORD dwSampleNumber;               /* where it happened, counted from startup */
} TTS_EVENT_T, *LPTTS_EVENT_T;

/* ---- TextToSpeechStartup device options ---- */
#define OWN_AUDIO_DEVICE 0x00000001     /* keep the audio device open between utterances (dapi) */
#define REPORT_OPEN_ERROR 0x00000002    /* report a failure to open the device (ERROR_OPENING_WAVE_OUTPUT_DEVICE)
                                           instead of retrying quietly (dapi) */
#define DO_NOT_USE_AUDIO_DEVICE 0x80000000   /* no audio device: a wave file or memory buffers only (dapi) */
#define TTS_MANUAL_CLOCK 0x00000100     /* [DTC01] no thread: the caller runs the synthesizer with TextToSpeechRun
                                           (needs DO_NOT_USE_AUDIO_DEVICE) */
#define TTS_EVENT_QUEUE 0x00000200      /* [DTC01] messages go to the event queue (TextToSpeechGetEvent) */

/* ---- speakers (dapi's numbers; v1.8's voice table 0x16146 has the same order) ---- */
typedef DWORD SPEAKER_T, *LPSPEAKER_T;
#define PAUL 0
#define BETTY 1
#define HARRY 2
#define FRANK 3
#define DENNIS 4                        /* v1.8: a second pointer to Frank's record */
#define KIT 5
#define URSULA 6
#define RITA 7
#define WENDY 8                         /* dapi's slot 8; v1.8 has Variable Val there ([:nv]; Wendy is 2.0's) */
#define VAL 8                           /* [DTC01] */
#define TTS_CURRENT_VOICE 0xFFFFFFFF    /* [DTC01] TextToSpeechGetVoice: the voice being spoken, with its [:dv] changes */

/* ---- TextToSpeechSpeak flags ---- */
#define TTS_NORMAL 0
#define TTS_FORCE 1                     /* also end the clause (a CTRL-K after the text), so it is spoken now */

/* ---- TextToSpeechConvertToPhonemes dwConversionFlags (dapi's) ---- */
#define TTS_SILENT 0x2                  /* convert only: nothing is heard */

/* ---- TextToSpeechGetStatus identifiers ---- */
#define INPUT_CHARACTER_COUNT 0         /* characters in the text queue not yet read by the pipeline (the host
                                           terminal holds back while it is 64 or more: v1.8's pipe, and its XOFF) */
#define STATUS_SPEAKING 1               /* 1 while audio remains to be played or delivered */
#define WAVE_OUT_DEVICE_ID 2            /* the audio device in use */
#define STATUS_LAST_INDEX 100           /* [DTC01] the last index mark reached (DT_INDEX_QUERY) */
#define STATUS_ERRORS 101               /* [DTC01] error bits since the last read, which clears them: 0x08 = DSR 25 */

/* ---- TextToSpeechOpenLogFile / TextToSpeechSetLog flags (= v1.8's DT_LOG bits the speech side acts on) ---- */
#define LOG_TEXT 0x0001                 /* the text the pipeline reads */
#define LOG_PHONEMES 0x0002             /* the phonemes of each clause */
#define LOG_SYLLABLES 0x0010            /* dapi's; v1.8 has none: MMSYSERR_INVALFLAG */
#define LOG_DEBUG 0x0080                /* [DTC01] v1.8's internal messages (e.g. words cut at 79 characters) */

/* ---- [DTC01] TextToSpeechSetMode flags (= v1.8's DT_MODE) ---- */
#define TTS_MODE_SQUARE 0x0001          /* [ ] delimit phonemic text and [: ] commands (on at startup) */
#define TTS_MODE_ASKY 0x0002            /* the one-character phonemic alphabet */
#define TTS_MODE_MINUS 0x0004           /* "-" is spoken "minus" */

/* ---- language (one: American English) ---- */
typedef DWORD LANGUAGE_T, *LPLANGUAGE_T;
#define TTS_AMERICAN_ENGLISH 1
#define TTS_ASCII 0
#define TTS_NOT_SUPPORTED 0x7FFF        /* TextToSpeechStartLang: not this library's language */
#define TTS_NOT_AVAILABLE 0x7FFE
#define TTS_LANG_ERROR 0x4000

typedef struct LANGUAGE_PARAMS_TAG {
    LANGUAGE_T dwLanguage;
    DWORD dwLanguageAttributes;
} LANGUAGE_PARAMS_T, *LPLANGUAGE_PARAMS_T;

typedef struct TTS_CAPS_TAG {           /* v1.8: 1 language, 10000 Hz, 120-350 wpm, 9 speakers (Val included), ASCII */
    DWORD dwNumberOfLanguages;
    LPLANGUAGE_PARAMS_T lpLanguageParamsArray;
    DWORD dwSampleRate;
    DWORD dwMinimumSpeakingRate;
    DWORD dwMaximumSpeakingRate;
    DWORD dwNumberOfPredefinedSpeakers;
    DWORD dwCharacterSet;
    DWORD Version;
} TTS_CAPS_T, *LPTTS_CAPS_T;

typedef struct {
    char lang_code[3];                  /* "US" */
    char lang_name[40];                 /* "American English" */
} LANG_ENTRY, *LPLANG_ENTRY;

typedef struct {
    DWORD Languages;                    /* 1 */
    BOOL MultiLang;                     /* FALSE */
    LPLANG_ENTRY Entries;
} LANG_ENUM, *LPLANG_ENUM;

/* ---- speech to memory (TextToSpeechOpenInMemory, AddBuffer, ReturnBuffer) ---- */
typedef struct TTS_PHONEME_TAG {
    DWORD dwPhoneme;                    /* the phone's code as v1.8's synthesizer has it (0-55 the phonemes of s8.3,
                                           = dapi's except 35; 0 is silence) */
    DWORD dwPhonemeSampleNumber;        /* where it starts (its first frame's first sample), counted from startup */
    DWORD dwPhonemeDuration;            /* in samples (its frames x 64) */
    DWORD dwReserved;
} TTS_PHONEME_T, *LPTTS_PHONEME_T;

typedef struct TTS_INDEX_TAG {
    DWORD dwIndexValue;
    DWORD dwIndexSampleNumber;
    DWORD dwReserved;                   /* [DTC01] TTS_INDEX_MARK or TTS_INDEX_REPLY */
} TTS_INDEX_T, *LPTTS_INDEX_T;

typedef struct TTS_BUFFER_TAG {
    LPSTR lpData;                       /* the samples, in the format given to TextToSpeechOpenInMemory */
    LPTTS_PHONEME_T lpPhonemeArray;
    LPTTS_INDEX_T lpIndexArray;
    DWORD dwMaximumBufferLength;        /* in bytes */
    DWORD dwMaximumNumberOfPhonemeChanges;
    DWORD dwMaximumNumberOfIndexMarks;
    DWORD dwBufferLength;
    DWORD dwNumberOfPhonemeChanges;
    DWORD dwNumberOfIndexMarks;
    DWORD dwReserved;
} TTS_BUFFER_T, *LPTTS_BUFFER_T;

/* ---- the user dictionary: text = the word, a NUL, its substitution (text or [phonemes]), a NUL; fc unused ---- */
#ifndef DIC_ENTRY
#define DIC_ENTRY
struct dic_entry {
    unsigned long fc;
    unsigned char text[128];
};
#endif

/* ---- version: TextToSpeechVersion's string is "DECtalk v1.8". Its number is dapi's layout: bits 24-30 the DECtalk
 * version (1), bits 16-23 its minor version (8), bits 8-15 the DLL's API (1: dapi's DECtalk DLL, which speak
 * checks for), bits 0-7 this library's revision. ---- */
typedef struct {
    DWORD StructSize;
    DWORD StructVersion;
    WORD DLLVersion;
    WORD DTalkVersion;                  /* 0x0108: firmware 1.8 */
    LPSTR VerString;                    /* "DECtalk v1.8" */
    LPSTR Language;                     /* "US" */
    DWORD Features;
} VERSION_INFO, *LPVERSION_INFO;
#define VERSION_STRUCT_VER 0x0001

/* ---- dapi's voice parameters (only for the stubs' prototypes; v1.8's are DTC01_VOICE_T) ---- */
typedef struct SPDEFS_TAG {
    short sex, smoothness, assertiveness, average_pitch, pitch_range, breathiness, richness, num_fixed_samp_og,
        laryngealization, head_size, formant4_res_freq, formant4_bandwidth, formant5_res_freq, formant5_bandwidth,
        parallel4_freq, parallel5_freq, gain_frication, gain_aspiration, gain_voicing, gain_nasalization, gain_cfr1,
        gain_cfr2, gain_cfr3, gain_cfr4, loudness, spectral_tilt, baseline_fall, lax_breathiness, quickness, hat_rise,
        stress_rise, glottal_speed, output_gain_mult;
} SPDEFS;

/* ---- [DTC01] a voice's 28 [:dv] values, in v1.8's order (s8.1) ---- */
typedef struct {
    short sex, sm, as, ap, pr, br, ri, nf, la, hs, f4, b4, f5, b5, p4, p5, gf, gh, gv, gn, g1, g2, g3, g4, g5, ft, bf,
        ef;
} DTC01_VOICE_T;

/* ---- [DTC01] the console: everything v1.8 prints on the local terminal from the speech side ([:dv list] tables,
 * "Illegal voice", the LOG_TEXT/LOG_PHONEMES/LOG_DEBUG lines), as the firmware formats it ---- */
typedef VOID (*TTS_CONSOLE_T)(const char *text, DWORD dwLength, DWORD dwInstanceParameter);

/* ==== functions; @n = the ordinal in dectalk.def (dapi's where dapi has the function) ==== */

/* @1: start the library (one instance for now). Startup is v1.8's speech initialization: Paul, 180 wpm, MODE
 * SQUARE, an empty user dictionary. uiDeviceNumber is WAVE_MAPPER or an audio device's number; dwDeviceOptions as
 * above. Returns MMSYSERR_NODRIVER when there is no audio device and DO_NOT_USE_AUDIO_DEVICE is not given.
 * Windows: dapi's window-message form. hWnd receives the three registered messages (NULL: none).
 * Linux: dapi's Linux form, the same as TextToSpeechStartupEx. */
#if defined(_WIN32)
TTSAPI MMRESULT TextToSpeechStartup(HWND hWnd, LPTTS_HANDLE_T *pphTTS, UINT uiDeviceNumber, DWORD dwDeviceOptions);
#else
TTSAPI MMRESULT TextToSpeechStartup(LPTTS_HANDLE_T *pphTTS, UINT uiDeviceNumber, DWORD dwDeviceOptions,
                                    TTS_CALLBACK_T DtCallbackRoutine, LONG lCallbackParameter);
#endif
/* @26: the callback form on both systems. The callback runs on the library's thread (or, with TTS_MANUAL_CLOCK, in
 * TextToSpeechRun); it must not call back into the library except TextToSpeechReturnBuffer/AddBuffer. */
TTSAPI MMRESULT TextToSpeechStartupEx(LPTTS_HANDLE_T *pphTTS, UINT uiDeviceNumber, DWORD dwDeviceOptions,
                                      TTS_CALLBACK_T DtCallbackRoutine, LONG lCallbackParameter);
TTSAPI MMRESULT TextToSpeechShutdown(LPTTS_HANDLE_T phTTS);                                     /* @2 */

/* @3: queue text, as the host task writes it into the text pipe (8-bit bytes; CTRL-K ends a clause, STX/ETX
 * delimit phonemic text, v1.8's [: ] commands and index marks work in it). Never waits, as dapi's: the queue grows
 * (s17.2). TTS_FORCE adds a CTRL-K. */
TTSAPI MMRESULT TextToSpeechSpeak(LPTTS_HANDLE_T phTTS, LPSTR pszText, DWORD dwFlags);
TTSAPI MMRESULT TextToSpeechPause(LPTTS_HANDLE_T phTTS);                   /* @4: pauses the audio device (dapi) */
TTSAPI MMRESULT TextToSpeechResume(LPTTS_HANDLE_T phTTS);                                       /* @5 */
/* @6: send the output to a wave file (instead of the device) until @7 */
TTSAPI MMRESULT TextToSpeechOpenWaveOutFile(LPTTS_HANDLE_T phTTS, char *pszFileName, DWORD dwFormat);
TTSAPI MMRESULT TextToSpeechCloseWaveOutFile(LPTTS_HANDLE_T phTTS);                             /* @7 */
TTSAPI MMRESULT TextToSpeechGetStatus(LPTTS_HANDLE_T phTTS, LPDWORD pdwIdentifier, LPDWORD pdwStatus,
                                      DWORD dwNumberOfStatusValues);                            /* @8 */
/* @9: DT_STOP: the clause being spoken is cut, the audio not yet played is dropped, and everything queued before
 * this call is dropped. Returns when the pipeline has been flushed (what v1.8's stop task does). bReset also returns
 * to the startup state (voice, rate, modes; the user dictionary is kept) and closes an open wave file. */
TTSAPI MMRESULT TextToSpeechReset(LPTTS_HANDLE_T phTTS, BOOL bReset);
/* @10: dapi's: returns when everything queued before it has been heard (played, or written to the file or the
 * buffers). Ends the clause (TTS_FORCE) and resumes a paused device, as dapi's does. v1.8's DT_SYNC returned
 * earlier, when klsyn had taken the sync marker (its last frames still queued for the DSP). */
TTSAPI MMRESULT TextToSpeechSync(LPTTS_HANDLE_T phTTS);
TTSAPI MMRESULT TextToSpeechGetRate(LPTTS_HANDLE_T phTTS, LPDWORD pdwRate);                     /* @11 */
TTSAPI MMRESULT TextToSpeechSetRate(LPTTS_HANDLE_T phTTS, DWORD dwRate);       /* @12: [:ra n], 120-350, queued */
TTSAPI MMRESULT TextToSpeechGetSpeaker(LPTTS_HANDLE_T phTTS, LPSPEAKER_T pSpeaker);             /* @13 */
TTSAPI MMRESULT TextToSpeechSetSpeaker(LPTTS_HANDLE_T phTTS, SPEAKER_T Speaker);       /* @14: [:np] ..., queued */
TTSAPI MMRESULT TextToSpeechGetLanguage(LPTTS_HANDLE_T phTTS, LPLANGUAGE_T pLanguage);          /* @15 */
TTSAPI MMRESULT TextToSpeechSetLanguage(LPTTS_HANDLE_T phTTS, LANGUAGE_T Language);             /* @16 */
TTSAPI MMRESULT TextToSpeechGetCaps(LPTTS_CAPS_T pTTScaps);                                     /* @17 */
/* @18: a text file of DT_DICT entries, one per line: the word, blanks, its substitution (v1.8 has no dictionary
 * file format; this is the library's, and SaveUserDictionary writes it). The entries replace the dictionary. */
TTSAPI MMRESULT TextToSpeechLoadUserDictionary(LPTTS_HANDLE_T phTTS, LPSTR pszFileName);
TTSAPI MMRESULT TextToSpeechUnloadUserDictionary(LPTTS_HANDLE_T phTTS);   /* @19: empties it (RIS, power-up) */
TTSAPI MMRESULT TextToSpeechOpenInMemory(LPTTS_HANDLE_T phTTS, DWORD dwFormat);                 /* @20 */
TTSAPI MMRESULT TextToSpeechCloseInMemory(LPTTS_HANDLE_T phTTS);                                /* @21 */
TTSAPI MMRESULT TextToSpeechAddBuffer(LPTTS_HANDLE_T phTTS, LPTTS_BUFFER_T pTTSbuffer);         /* @22 */
/* @23: dapi's: the buffer being filled, as it is (NULL if none). [DTC01] In 64-bit builds the full buffers come
 * first, oldest first, since TTS_MSG_BUFFER cannot carry them. */
TTSAPI MMRESULT TextToSpeechReturnBuffer(LPTTS_HANDLE_T phTTS, LPTTS_BUFFER_T *ppTTSbuffer);
/* @24: write the LOG_TEXT / LOG_PHONEMES lines, as v1.8 formats them for its console, to a file */
TTSAPI MMRESULT TextToSpeechOpenLogFile(LPTTS_HANDLE_T phTTS, LPSTR pszFileName, DWORD dwFlags);
TTSAPI MMRESULT TextToSpeechCloseLogFile(LPTTS_HANDLE_T phTTS);                                 /* @25 */
TTSAPI DWORD TextToSpeechVersion(LPSTR *ppszVersion);            /* @27: "DECtalk v1.8"; the number, see above */
TTSAPI DWORD TextToSpeechVersionEx(LPVERSION_INFO *ppVersion);                                  /* @33 */
/* @44: 1 if the word is in the user dictionary (exactly that name), else 0; [DTC01] its substitution is then copied
 * after the name in entry->text */
TTSAPI int TextToSpeechUserDictionaryHit(LPTTS_HANDLE_T phTTS, struct dic_entry *entry);
/* @45: dapi's listing: "Total user dictionary entries: n", then "name, substitution" lines */
TTSAPI MMRESULT TextToSpeechDumpUserDictionary(LPTTS_HANDLE_T phTTS, char *pszFileName);
/* @46: DT_DICT (an entry of the same word is replaced; MMSYSERR_NOMEM = no room, v1.8's R3 = 1) */
TTSAPI MMRESULT TextToSpeechAddUserEntry(LPTTS_HANDLE_T phTTS, struct dic_entry *entry);
TTSAPI MMRESULT TextToSpeechDeleteUserEntry(LPTTS_HANDLE_T phTTS, struct dic_entry *entry);     /* @47 */
/* @48: a new substitution for an entry that is there */
TTSAPI MMRESULT TextToSpeechChangeUserPhoneme(LPTTS_HANDLE_T phTTS, struct dic_entry *entry,
                                              unsigned char *pszNewSubstitution);               /* @48 */
TTSAPI MMRESULT TextToSpeechSaveUserDictionary(LPTTS_HANDLE_T phTTS, char *pszFileName);  /* @49: what @18 reads */

/* ---- dapi functions v1.8 has no counterpart for (s17.4). They exist so that dapi programs link and load; each
 * does the least that is true. (@31, @32, @39 TextToSpeechReserved1-3 and @51 TextToSpeechTuning are exported the
 * same way, without prototypes, as dapi's header gives none.) ---- */
TTSAPI VOID TextToSpeechControlPanel(LPTTS_HANDLE_T phTTS);                   /* @28: nothing (no control panel) */
TTSAPI VOID TextToSpeechTyping(LPTTS_HANDLE_T phTTS, unsigned char key);      /* @29: nothing (no typing mode) */
TTSAPI ULONG TextToSpeechGetLastError(LPTTS_HANDLE_T phTTS);                  /* @30: the last MMRESULT */
TTSAPI UINT TextToSpeechStartLang(char *pszLanguage);          /* @34: "US" = 1, anything else TTS_NOT_SUPPORTED */
TTSAPI BOOL TextToSpeechSelectLang(LPTTS_HANDLE_T phTTS, UINT uiLanguage);    /* @35: TRUE for 1 (and 0) */
TTSAPI BOOL TextToSpeechCloseLang(char *pszLanguage);                         /* @36: TRUE for "US" */
TTSAPI DWORD TextToSpeechGetFeatures(void);                                   /* @37: 0 */
TTSAPI DWORD TextToSpeechEnumLangs(LPLANG_ENUM *ppLangs);                     /* @38: one, "US" */
TTSAPI MMRESULT TextToSpeechGetSpeakerParams(LPTTS_HANDLE_T phTTS, UINT uiIndex, SPDEFS **ppspCur,
                                             SPDEFS **ppspLoLimit, SPDEFS **ppspHiLimit,
                                             SPDEFS **ppspDefault);           /* @40: MMSYSERR_NOTSUPPORTED */
TTSAPI MMRESULT TextToSpeechSetSpeakerParams(LPTTS_HANDLE_T phTTS, SPDEFS *pspSet);   /* @41: MMSYSERR_NOTSUPPORTED */
/* @42: 1 if the word is in the built-in dictionary (the ROM's 6,508 words; exactly, no suffixes); [DTC01] its
 * pronunciation is then copied after the name in entry->text, as phonemic text */
TTSAPI int TextToSpeechDictionaryHit(LPTTS_HANDLE_T phTTS, struct dic_entry *entry);
/* @43: the built-in dictionary as a listing: a count, then "word, pronunciation" lines */
TTSAPI MMRESULT TextToSpeechDumpDictionary(LPTTS_HANDLE_T phTTS, char *pszFileName);
/* @50: the phonemes of szText as v1.8's phoneme log writes them; *dwBufSize in: the buffer's size, out: the bytes
 * written with the NUL. dapi's: it speaks the text too, unless TTS_SILENT; the alphabet flags are ignored. */
TTSAPI MMRESULT TextToSpeechConvertToPhonemes(LPTTS_HANDLE_T phTTS, unsigned char *szPhonemeBuf, DWORD *dwBufSize,
                                              DWORD dwOutPhonemeFlags, unsigned char *szText, DWORD dwInTextFlags,
                                              DWORD dwConversionFlags);
TTSAPI short *TextToSpeechGetPhVdefParams(LPTTS_HANDLE_T phTTS, UINT uiIndex);       /* @52: NULL */

/* ---- [DTC01] what the host terminal and the samples need beyond dapi ---- */
TTSAPI MMRESULT TextToSpeechSetMode(LPTTS_HANDLE_T phTTS, DWORD dwMode);                        /* @100: DT_MODE */
TTSAPI MMRESULT TextToSpeechGetMode(LPTTS_HANDLE_T phTTS, LPDWORD pdwMode);                     /* @101 */
TTSAPI MMRESULT TextToSpeechSetLog(LPTTS_HANDLE_T phTTS, DWORD dwFlags);        /* @102: DT_LOG, to the console */
TTSAPI MMRESULT TextToSpeechGetLog(LPTTS_HANDLE_T phTTS, LPDWORD pdwFlags);                     /* @103 */
TTSAPI MMRESULT TextToSpeechSetConsole(LPTTS_HANDLE_T phTTS, TTS_CONSOLE_T ConsoleRoutine,
                                       DWORD dwInstanceParameter);                              /* @104 */
/* @105: the synthesizer's two-tone generator (DTMF dialing, the power-up self-test): dwHigh and dwLow Hz (0 = that
 * tone off, 0-4095) for dwOnMs, then silence for dwOffMs (10 ms steps). It follows what is queued (a Sync first)
 * and returns when it has been heard, as Sync does (v1.8's dialer waits for each tone message). v1.8's dialer uses
 * 160 ms and 60 ms. */
TTSAPI MMRESULT TextToSpeechPlayTones(LPTTS_HANDLE_T phTTS, DWORD dwHigh, DWORD dwLow, DWORD dwOnMs, DWORD dwOffMs);
/* @106: a voice's [:dv] values: PAUL ... RITA, VAL, or TTS_CURRENT_VOICE */
TTSAPI MMRESULT TextToSpeechGetVoice(LPTTS_HANDLE_T phTTS, SPEAKER_T Speaker, DTC01_VOICE_T *pVoice);
/* @107: all 28 as [:dv] commands, queued (v1.8 clamps each to its range) */
TTSAPI MMRESULT TextToSpeechSetVoice(LPTTS_HANDLE_T phTTS, const DTC01_VOICE_T *pVoice);
/* @108: with TTS_MANUAL_CLOCK, run the pipeline and the synthesizer for dwSamples samples in the caller's thread
 * (callbacks come from it too). For tests: runs are then repeatable, as the emulator's are. */
TTSAPI MMRESULT TextToSpeechRun(LPTTS_HANDLE_T phTTS, DWORD dwSamples);
/* @109: with TTS_EVENT_QUEUE, the handle that signals a queued message */
TTSAPI MMRESULT TextToSpeechGetEventHandle(LPTTS_HANDLE_T phTTS, TTS_WAITABLE_T *pHandle);
/* @110: take the oldest queued message; FALSE when there is none. Never blocks. */
TTSAPI BOOL TextToSpeechGetEvent(LPTTS_HANDLE_T phTTS, LPTTS_EVENT_T pEvent);

#ifdef __cplusplus
}
#endif

#endif
