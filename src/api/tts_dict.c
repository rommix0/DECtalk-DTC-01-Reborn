/* tts_dict.c - the user dictionary through the API (REFERENCE.md s17.11): v1.8's DT_DICT table (tx_dict.c),
 * under the engine's lock.
 *
 * An entry is v1.8's: a name without blanks, and a substitution, which is text or [phonemes] and is spoken in place of
 * the name. A capital in the name matches only a capital, a small letter either. An entry of the same name is
 * replaced; an empty substitution deletes (v1.8's rule). DT_DICT's text is at most 255 bytes.
 *
 * The files are text, one entry per line as DT_DICT takes it: the name, blanks, the substitution. (v1.8 has no
 * dictionary file; dapi's are compiled and are not read.) Save writes that, Load reads it; Dump writes dapi's
 * listing: a count, then "name, substitution" lines.
 *
 * The built-in dictionary (the ROM's trie, 6,508 words, s15.25) can be asked too: DictionaryHit and DumpDictionary.
 * Its pronunciations come out as phonemic text, with the names v1.8's phoneme log uses, so they can be spoken in
 * [ ] or given to a user entry.
 */
#define BLD_DECTALK_DLL
#include <stdio.h>
#include <string.h>
#include "ttsapi.h"
#include "tts_lib.h"
#include "../speech/engine.h"

#define DICT_TEXT_MAX 255       /* DT_DICT's text in v1.8: dcs_text holds 256 bytes */

/* the name and substitution of a dic_entry (text = name NUL substitution NUL); 0 if the name is not one v1.8 takes */
static int entry_parts(const struct dic_entry *e, const char **name, const char **subst)
{
    const char *t = (const char *)e->text;
    size_t n = strnlen(t, sizeof e->text);
    if (!n || n >= sizeof e->text - 1 || strpbrk(t, " \t")) return 0;
    *name = t;
    *subst = t + n + 1;
    if (strnlen(*subst, sizeof e->text - n - 1) >= sizeof e->text - n - 1) return 0;
    return 1;
}

static void write_line(void *ctx, const char *name, const char *subst) { fprintf((FILE *)ctx, "%s %s\n", name, subst); }
static void dump_line(void *ctx, const char *name, const char *subst) { fprintf((FILE *)ctx, "%s, %s\n", name, subst); }
static void count(void *ctx, const char *name, const char *subst) { (void)name; (void)subst; ++*(int *)ctx; }

static MMRESULT set(LPTTS_HANDLE_T h, const char *name, const char *subst)
{
    if (strlen(name) + 1 + strlen(subst) > DICT_TEXT_MAX) return tts_error(h, MMSYSERR_INVALPARAM);
    return engine_dict_set(name, subst) ? MMSYSERR_NOERROR : tts_error(h, MMSYSERR_NOMEM);
}

/* @46: DT_DICT: add, or replace the entry of the same name; an empty substitution deletes */
TTSAPI MMRESULT TextToSpeechAddUserEntry(LPTTS_HANDLE_T phTTS, struct dic_entry *entry)
{
    const char *name, *subst;
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    if (!entry || !entry_parts(entry, &name, &subst)) return tts_error(phTTS, MMSYSERR_INVALPARAM);
    return set(phTTS, name, subst);
}

/* @47: MMSYSERR_ERROR if there is no entry of that name */
TTSAPI MMRESULT TextToSpeechDeleteUserEntry(LPTTS_HANDLE_T phTTS, struct dic_entry *entry)
{
    const char *name, *subst;
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    if (!entry || !entry_parts(entry, &name, &subst)) return tts_error(phTTS, MMSYSERR_INVALPARAM);
    if (!engine_dict_get(name, NULL, 0)) return tts_error(phTTS, MMSYSERR_ERROR);
    engine_dict_set(name, "");
    return MMSYSERR_NOERROR;
}

/* @48: a new substitution for an entry that is there (MMSYSERR_ERROR if not) */
TTSAPI MMRESULT TextToSpeechChangeUserPhoneme(LPTTS_HANDLE_T phTTS, struct dic_entry *entry,
                                              unsigned char *pszNewSubstitution)
{
    const char *name, *subst;
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    if (!entry || !entry_parts(entry, &name, &subst) || !pszNewSubstitution || !*pszNewSubstitution)
        return tts_error(phTTS, MMSYSERR_INVALPARAM);
    if (!engine_dict_get(name, NULL, 0)) return tts_error(phTTS, MMSYSERR_ERROR);
    return set(phTTS, name, (const char *)pszNewSubstitution);
}

/* @44: 1 if there is an entry of exactly this name, 0 if not (-1: a bad handle, as dapi). [DTC01] Its substitution
 * is then copied after the name, when it fits in text[]. */
TTSAPI int TextToSpeechUserDictionaryHit(LPTTS_HANDLE_T phTTS, struct dic_entry *entry)
{
    const char *name, *subst;
    char s[DICT_TEXT_MAX + 1];
    size_t n;
    if (!tts_valid(phTTS)) return -1;
    if (!entry || !entry_parts(entry, &name, &subst)) return 0;
    if (!engine_dict_get(name, s, sizeof s)) return 0;
    n = strlen(name) + 1;
    if (n + strlen(s) + 1 <= sizeof entry->text) strcpy((char *)entry->text + n, s);
    return 1;
}

/* @42: 1 if the word is in the built-in dictionary (exactly: not through the user dictionary, and without v1.8's
 * suffix stripping), else 0 (-1: a bad handle, as dapi). [DTC01] Its pronunciation is then copied after the name,
 * when it fits in text[]. */
TTSAPI int TextToSpeechDictionaryHit(LPTTS_HANDLE_T phTTS, struct dic_entry *entry)
{
    const char *name, *subst;
    char s[256];
    size_t n;
    if (!tts_valid(phTTS)) return -1;
    if (!entry || !entry_parts(entry, &name, &subst)) return 0;
    if (!engine_dict_builtin(name, s, sizeof s)) return 0;
    n = strlen(name) + 1;
    if (n + strlen(s) + 1 <= sizeof entry->text) strcpy((char *)entry->text + n, s);
    return 1;
}

/* @43: the built-in dictionary as dapi's listing: "Total dictionary entries: n", then "word, pronunciation" lines in
 * the trie's order */
TTSAPI MMRESULT TextToSpeechDumpDictionary(LPTTS_HANDLE_T phTTS, char *pszFileName)
{
    FILE *f;
    int n = 0;
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    if (!pszFileName) return tts_error(phTTS, MMSYSERR_INVALPARAM);
    if (!(f = fopen(pszFileName, "w"))) return tts_error(phTTS, MMSYSERR_ERROR);
    engine_dict_builtin_list(count, &n);
    fprintf(f, "Total dictionary entries: %d\n\n", n);
    engine_dict_builtin_list(dump_line, f);
    if (fclose(f)) return tts_error(phTTS, MMSYSERR_ERROR);
    return MMSYSERR_NOERROR;
}

/* @19: empty it (RIS and power-up in v1.8) */
TTSAPI MMRESULT TextToSpeechUnloadUserDictionary(LPTTS_HANDLE_T phTTS)
{
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    engine_dict_clear();
    return MMSYSERR_NOERROR;
}

/* @18: the entries of a file (see above) replace the dictionary. Blank lines are skipped. MMSYSERR_ERROR: the file
 * cannot be read; MMSYSERR_NOMEM: no room (the entries so far stay). */
TTSAPI MMRESULT TextToSpeechLoadUserDictionary(LPTTS_HANDLE_T phTTS, LPSTR pszFileName)
{
    char line[1024];
    FILE *f;
    MMRESULT r = MMSYSERR_NOERROR;
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    if (!pszFileName) return tts_error(phTTS, MMSYSERR_INVALPARAM);
    if (!(f = fopen(pszFileName, "r"))) return tts_error(phTTS, MMSYSERR_ERROR);
    engine_dict_clear();
    while (r == MMSYSERR_NOERROR && fgets(line, sizeof line, f)) {
        char *name, *end, *subst;
        line[strcspn(line, "\r\n")] = 0;
        for (name = line; *name == ' ' || *name == '\t'; name++) {}        /* as parse_dict_entry_command 0xe8ec */
        if (!*name) continue;
        for (end = name; *end && *end != ' ' && *end != '\t'; end++) {}
        for (subst = end; *subst == ' ' || *subst == '\t'; subst++) {}
        *end = 0;
        if (!*subst) continue;
        r = set(phTTS, name, subst);
    }
    fclose(f);
    return r;
}


/* @49: the dictionary as a file Load reads, in name order */
TTSAPI MMRESULT TextToSpeechSaveUserDictionary(LPTTS_HANDLE_T phTTS, char *pszFileName)
{
    FILE *f;
    int n;
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    if (!pszFileName) return tts_error(phTTS, MMSYSERR_INVALPARAM);
    if (!(f = fopen(pszFileName, "w"))) return tts_error(phTTS, MMSYSERR_ERROR);
    n = engine_dict_list(write_line, f);
    if (fclose(f) || n < 0) return tts_error(phTTS, MMSYSERR_ERROR);
    return MMSYSERR_NOERROR;
}

/* @45: dapi's listing: "Total user dictionary entries: n", a blank line, then "name, substitution" in name order */
TTSAPI MMRESULT TextToSpeechDumpUserDictionary(LPTTS_HANDLE_T phTTS, char *pszFileName)
{
    FILE *f;
    int n = 0;
    if (!tts_valid(phTTS)) return MMSYSERR_INVALHANDLE;
    if (!pszFileName) return tts_error(phTTS, MMSYSERR_INVALPARAM);
    if (!(f = fopen(pszFileName, "w"))) return tts_error(phTTS, MMSYSERR_ERROR);
    engine_dict_list(count, &n);
    fprintf(f, "Total user dictionary entries: %d\n\n", n);
    engine_dict_list(dump_line, f);
    if (fclose(f)) return tts_error(phTTS, MMSYSERR_ERROR);
    return MMSYSERR_NOERROR;
}
