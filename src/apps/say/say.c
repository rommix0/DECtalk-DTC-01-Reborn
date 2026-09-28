/* say.c - speak the command line or standard input (REFERENCE.md s17.12).
 *
 * dapi's SAY sample (samples/SAY/say.c) ported to the DTC-01 v1.8 library, as portable C for Windows and Linux: the
 * same options and behaviour, standard I/O in place of the console handles, and CTRL-C by signal() (Windows, where
 * the handler runs on a thread of its own, as SetConsoleCtrlHandler's did) or a thread waiting in sigwait() (POSIX).
 * Changes for v1.8:
 *   -ls         removed: v1.8 has no syllable log;
 *   -w          writes 16-bit files at v1.8's 10 kHz;
 *   help        v1.8's commands in the examples ([:nb], [:np :ra 200]; dapi's [:phoneme on] is not v1.8's);
 *   stdin       from a terminal each line is spoken when RETURN is pressed (TTS_FORCE), as the sample did; from a
 *               pipe or a file the text is not cut at each line or read block, so sentences that run over a line end
 *               keep their intonation; and SAY holds back while the library has 16K characters not yet read (its
 *               Speak never waits, s17.2).
 */
#if defined(_WIN32)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include "ttsapi.h"

#define INPUT_CMDLINE 0
#define INPUT_STDIN 1

#define OUTPUT_SOUND 0
#define OUTPUT_WAVE 1
#define OUTPUT_LOGTEXT 2
#define OUTPUT_LOGPHONEME 3

#define QUEUE_MAX 16384             /* characters queued and not yet read: hold back above this */

static int input_mode = INPUT_STDIN;
static int output_mode = OUTPUT_SOUND;
static char *prefix_text, *postfix_text, *out_file, *dict_file;
static LPTTS_HANDLE_T tts;
static volatile sig_atomic_t signal_received;

static void error_out(const char *message) { fputs(message, stderr); }

static void sleep_ms(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&t, NULL);
#endif
}

/* wait while the library has much text not yet read (dapi's Speak waited there itself) */
static void hold_back(void)
{
    DWORD id = INPUT_CHARACTER_COUNT, n = 0;
    while (!signal_received && TextToSpeechGetStatus(tts, &id, &n, 1) == MMSYSERR_NOERROR && n >= QUEUE_MAX)
        sleep_ms(20);
}

/* ---- CTRL-C: stop speaking (the sample's CtrlHandler: TextToSpeechReset) and end ---- */
#if defined(_WIN32)
static void on_signal(int sig)
{
    signal_received = 1;
    TextToSpeechReset(tts, TRUE);
    signal(sig, on_signal);
}

static void catch_signals(void)
{
    signal(SIGINT, on_signal);
    signal(SIGBREAK, on_signal);
}
#else
/* The library's calls take locks, which a signal handler must not: a thread takes the signals instead (they are
 * blocked in every other thread, the library's included, which inherit the mask), and interrupts the main thread's
 * read with SIGUSR1. */
static pthread_t main_thread;

static void on_usr1(int sig) { (void)sig; }

static void *signal_thread(void *arg)
{
    sigset_t *set = (sigset_t *)arg;
    int sig;
    if (sigwait(set, &sig) == 0) {
        signal_received = 1;
        if (tts) TextToSpeechReset(tts, TRUE);
        pthread_kill(main_thread, SIGUSR1);
    }
    return NULL;
}

static void catch_signals(void)
{
    static sigset_t set;
    struct sigaction sa;
    pthread_t t;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;             /* no SA_RESTART: a read in progress fails with EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    main_thread = pthread_self();
    if (pthread_create(&t, NULL, signal_thread, &set) == 0) pthread_detach(t);
}
#endif

static void output_help(void)
{
    static const char *const help[] = {
        "SAY  [options] [text]",
        "",
        "Speaks with DECtalk DTC-01 v1.8.",
        "",
        "Help Options:",
        "",
        "    -h or -?          = Help.  Outputs this text to the console.  This",
        "                        option cancels any others on the command line.",
        "",
        "",
        "Output Options:",
        "",
        "    -w outFile        = Convert text into the specified wave file (16-bit,",
        "                        10 kHz, mono: v1.8's rate) instead of speaking",
        "                        to the sound device.",
        "",
        "    -l[t] outFile     = Turn on text logging, which logs all input text",
        "                        to a file.  This text includes any pre and post",
        "                        commands as well as commands sent to DECtalk by",
        "                        the SAY program itself.",
        "",
        "                        Since this is the default logging mode, the 't'",
        "                        immediately following the '-l' is optional.",
        "",
        "    -lp outFile       = Turn on phoneme logging, which converts the",
        "                        input text to phonemes.  This is useful if you",
        "                        want to get DECtalk to sing.  You convert the",
        "                        text to phonemes and then give the phonemes",
        "                        durations and pitches, as in [l'aa<400,24>].",
        "",
        "    If no output options are specified, SAY sends its output to the",
        "    installed sound device.  Only one output option can be specified;",
        "    if you specify more than one, the last one on the command line is",
        "    used.",
        "",
        "",
        "Input Options:",
        "",
        "    -pre preText      = Text to be passed to DECtalk before the normal input.",
        "                        This is useful for passing initializing commands to",
        "                        DECtalk that would normally not be part of the input.",
        "                        If the prefix text has spaces, it must be enclosed in",
        "                        quotes.  An example would be \"[:nb]\" or",
        "                        \"[:np :ra 200]\".",
        "",
        "                        The prefix text is \"forced\" out before the input text",
        "                        is read.",
        "",
        "    -post postText    = Text to be passed to DECtalk after the normal input.",
        "                        This is useful for passing terminating commands to",
        "                        DECtalk that would normally not be part of the input.",
        "                        If the postfix text has spaces, it must be enclosed",
        "                        in quotes.  An example would be \"[:np]\" or",
        "                        \"The End\".",
        "",
        "                        The \"normal\" input is \"forced\" out before the postfix",
        "                        text is read.",
        "",
        "    text              = Text appearing on command line is spoken.  The text",
        "                        to be spoken can either come from the standard",
        "                        input or from the command line.",
        "",
        "                        Anything on the command line that is not an option",
        "                        will be interpreted as text, as will anything following",
        "                        it on the command line.  In other words, text to",
        "                        be spoken must appear on the command line after",
        "                        all options.",
        "",
#if defined(_WIN32)
        "                        If the *first* word in the text has a dash (-) or",
        "                        slash (/) as its first character, you must precede",
        "                        it with another dash or slash.  For example, to tell",
#else
        "                        If the *first* word in the text has a dash (-) as",
        "                        its first character, you must precede it with",
        "                        another dash.  For example, to tell",
#endif
        "                        DECtalk to say the number -123, you would type the",
        "                        command",
        " ",
        "                          SAY --123",
        "",
        "                        This is necessary to avoid having SAY interpret the",
        "                        number as a command line option.",
        "",
        "                        If you embed DECtalk commands into your text, you must",
        "                        enclose them in quotes if they contain spaces.",
        "                        This is because SAY treats each space-delimited",
        "                        command-line argument as a separate \"word\",",
        "                        while DECtalk commands must be processed as",
        "                        single \"words\" by the SAY program.",
        "",
        "    If no text is specified, SAY will take its input from the standard input.",
        "    For example, you could have SAY speak a directory listing in Betty's",
        "    voice by typing",
        "",
#if defined(_WIN32)
        "        DIR | SAY -pre \"[:nb]\"",
#else
        "        ls -l | say -pre \"[:nb]\"",
#endif
        "",
        "    or you could just type the command",
        "",
        "        SAY",
        "",
        "    and then enter text at the console.  In this case, SAY speaks each",
#if defined(_WIN32)
        "    line after you press RETURN, and exits after you press CTRL-Z.  If",
#else
        "    line after you press RETURN, and exits after you press CTRL-D.  If",
#endif
        "    you want SAY to take its input from a file, use file redirection as",
        "    in the following example, which reads the file FOO.TXT in Harry's",
        "    voice.",
        "",
        "        SAY -pre \"[:nh]\" < FOO.TXT",
        "",
        "",
        "Dictionary Options:",
        "",
        "    -d userDict       = Loads the specified user dictionary before",
        "                        speaking: a text file with one entry per line,",
        "                        the word, blanks, then what to say for it (text",
        "                        or [phonemes]).",
        "",
        NULL};
    int i;
    for (i = 0; help[i]; i++) printf("%s\n", help[i]);
}

static int is_option_char(char c)
{
#if defined(_WIN32)
    return c == '-' || c == '/';
#else
    return c == '-';
#endif
}

/* The sample's ParseArgs: returns the index of the first word to speak, 0 for none (standard input), -1 to stop
 * (help), -2 to stop with an error. */
static int parse_args(int ac, char **av)
{
    int i;
    for (i = 1; i < ac; i++) {
        if (!is_option_char(av[i][0])) {                /* not an option: text to speak */
            input_mode = INPUT_CMDLINE;
            return i;
        } else if (is_option_char(av[i][1])) {          /* "--123": text too, without the first dash */
            input_mode = INPUT_CMDLINE;
            av[i]++;
            return i;
        } else if (av[i][1] == 'l') {
            if (av[i][2] == 's') {
                error_out("-ls: DECtalk v1.8 has no syllable log\n");
                return -2;
            }
            if (i < ac - 1) {
                output_mode = av[i][2] == 'p' ? OUTPUT_LOGPHONEME : OUTPUT_LOGTEXT;
                out_file = av[++i];
            }
        } else if (av[i][1] == 'w') {
            if (i < ac - 1) {
                out_file = av[++i];
                output_mode = OUTPUT_WAVE;
            }
        } else if (av[i][1] == 'd') {
            if (i < ac - 1) dict_file = av[++i];
        } else if (!strcmp(&av[i][1], "pre")) {
            if (i < ac - 1) prefix_text = av[++i];
        } else if (!strcmp(&av[i][1], "post")) {
            if (i < ac - 1) postfix_text = av[++i];
        } else if (av[i][1] == 'h' || av[i][1] == '?') {
            output_help();
            return -1;
        } else {
            error_out("Invalid Argument\n");
        }
    }
    return 0;
}

static MMRESULT speak_stdin(void)
{
    char buf[2049];
    int interactive = isatty(fileno(stdin));
    MMRESULT status = MMSYSERR_NOERROR;
    while (!signal_received && fgets(buf, sizeof buf, stdin)) {
        size_t n = strlen(buf);
        status = TextToSpeechSpeak(tts, buf, interactive && n && buf[n - 1] == '\n' ? TTS_FORCE : TTS_NORMAL);
        hold_back();
    }
    return status;
}

static MMRESULT speak_cmdline(int ac, char **av, int first)
{
    MMRESULT status = MMSYSERR_NOERROR;
    int i;
    for (i = first; i < ac && !signal_received; i++) {
        if (i > first) status = TextToSpeechSpeak(tts, " ", TTS_NORMAL);   /* or the words run together */
        status = TextToSpeechSpeak(tts, av[i], TTS_NORMAL);
        hold_back();
    }
    return status;
}

static int startup_error(MMRESULT status)
{
    switch (status) {
    case MMSYSERR_ALLOCATED: error_out("DECtalk is already running in this program.\n"); break;
    case MMSYSERR_NOMEM: error_out("Memory allocation error.\n"); break;
    case MMSYSERR_NODRIVER: error_out("No sound device.\n"); break;
    default: fprintf(stderr, "DECtalk could not start (error %u).\n", (unsigned)status); break;
    }
    return 1;
}

int main(int argc, char **argv)
{
    MMRESULT status;
    int first = parse_args(argc, argv);
    if (first < 0) return first == -1 ? 0 : 1;
    catch_signals();

    /* the sample (ETT 11/04/98): a wave file never needs the device; without a device, write nothing */
    if (output_mode == OUTPUT_WAVE) {
        status = TextToSpeechStartupEx(&tts, WAVE_MAPPER, DO_NOT_USE_AUDIO_DEVICE, NULL, 0);
    } else {
        status = TextToSpeechStartupEx(&tts, WAVE_MAPPER, 0, NULL, 0);
        if (status == MMSYSERR_NODRIVER) status = TextToSpeechStartupEx(&tts, WAVE_MAPPER, DO_NOT_USE_AUDIO_DEVICE, NULL, 0);
    }
    if (status != MMSYSERR_NOERROR) return startup_error(status);

    if (dict_file) {
        TextToSpeechUnloadUserDictionary(tts);
        if (TextToSpeechLoadUserDictionary(tts, dict_file) != MMSYSERR_NOERROR)
            fprintf(stderr, "The user dictionary %s cannot be read.\n", dict_file);
    }

    status = MMSYSERR_NOERROR;
    if (out_file) {
        switch (output_mode) {
        case OUTPUT_WAVE: status = TextToSpeechOpenWaveOutFile(tts, out_file, WAVE_FORMAT_1M16); break;
        case OUTPUT_LOGTEXT: status = TextToSpeechOpenLogFile(tts, out_file, LOG_TEXT); break;
        case OUTPUT_LOGPHONEME: status = TextToSpeechOpenLogFile(tts, out_file, LOG_PHONEMES); break;
        }
    }
    if (status != MMSYSERR_NOERROR) {
        fprintf(stderr, "Cannot write %s.\n", out_file);
        TextToSpeechShutdown(tts);
        return 1;
    }

    if (prefix_text) TextToSpeechSpeak(tts, prefix_text, TTS_FORCE);
    if (input_mode == INPUT_CMDLINE) speak_cmdline(argc, argv, first);
    else speak_stdin();
    if (postfix_text && !signal_received) TextToSpeechSpeak(tts, postfix_text, TTS_FORCE);

    /* sync to make sure everything has come out */
    TextToSpeechSpeak(tts, "        ", TTS_FORCE);
    TextToSpeechSync(tts);

    if (out_file) {
        if (output_mode == OUTPUT_WAVE) TextToSpeechCloseWaveOutFile(tts);     /* after CTRL-C: already closed */
        else TextToSpeechCloseLogFile(tts);
    }
    TextToSpeechShutdown(tts);
    return 0;
}
