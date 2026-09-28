/* dtc01term - the DTC-01's host terminal on the speech library (REFERENCE.md s17.14).
 *
 * The unit's host side, the ROM's own tasks rebuilt in C (src/host: the host line's escape parser and commands, the
 * local terminal and SETUP, the host timeout, DT_STOP, an idle phone), runs on the kernel (src/kernel), talks over
 * two lines (term_line.c) and speaks through DECtalk.dll / libtts_us.so (term_speech.c).
 *
 * usage: dtc01term [--host LINE] [--local LINE] [-w FILE] [-d N] [-q]
 *   LINE: console, stdio, tcp:[addr:]port, com:NAME or none
 *   defaults: --host tcp:127.0.0.1:2001 --local console
 *   -w FILE  speak into a wave file instead of the audio device    -d N  the audio device's number
 *   -q       the self-test jumper closed: no banner at power-up
 * On the local terminal Ctrl+] b sends a BREAK (SETUP), Ctrl+] q quits. With a stdio line the program ends at the end
 * of its input, once everything has been spoken.
 *
 * The main loop is the unit's clock and interrupt level: every 10 ms tick it runs the kernel's timers, hands the
 * library's events to the tasks, runs the tasks until they all wait, and sends what they wrote.
 */
#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "ttsapi.h"
#include "term_dev.h"
#include "term_line.h"
#include "term_os.h"
#include "term_speech.h"
#ifdef _WIN32
#include <mmsystem.h>
#endif

#define DRAIN_MAX_MS 120000     /* after the end of a stdio input, wait at most this long for the speech */

static double now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
#endif
}

static void usage(void)
{
    fprintf(stderr,
            "usage: dtc01term [--host LINE] [--local LINE] [-w FILE] [-d N] [-q]\n"
            "  LINE is console, stdio, tcp:[addr:]port, com:NAME or none\n"
            "  defaults: --host tcp:127.0.0.1:2001 --local console\n"
            "  -w FILE  speak into a wave file     -d N  the audio device\n"
            "  -q       no banner at power-up (the self-test jumper closed)\n"
            "  On the local terminal: Ctrl+] b = BREAK (enters SETUP), Ctrl+] q = quit.\n");
}

int main(int argc, char **argv)
{
    const char *host_spec = "tcp:127.0.0.1:2001", *local_spec = "console", *wave = NULL;
    unsigned device = WAVE_MAPPER;
    int quiet = 0, i, drain;
    char err[256];
    term_line_t *host = NULL, *local = NULL;
    double t0, drain_start = 0;
    uint32_t ticks = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) host_spec = argv[++i];
        else if (!strcmp(argv[i], "--local") && i + 1 < argc) local_spec = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) wave = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) device = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else {
            usage();
            return strcmp(argv[i], "-h") && strcmp(argv[i], "--help") ? 1 : 0;
        }
    }
    if ((!strcmp(host_spec, "stdio") || !strcmp(host_spec, "console")) && !strcmp(host_spec, local_spec)) {
        fprintf(stderr, "dtc01term: the host line and the local terminal cannot both be %s\n", host_spec);
        return 1;
    }
    drain = !strcmp(host_spec, "stdio") || !strcmp(local_spec, "stdio");

    if (term_speech_start(device, wave, quiet, err, sizeof err) != 0) {
        fprintf(stderr, "dtc01term: %s\n", err);
        return 1;
    }
    kernel_init(term_speech_hooks());
    kernel_lock();                      /* the lines' first bytes wait until the devices are set up */
    host = line_open(host_spec, 0, term_dev_rx, &host_dev, err, sizeof err);
    if (host) local = line_open(local_spec, 1, term_dev_rx, &console_dev, err, sizeof err);
    if (!host || !local) {
        fprintf(stderr, "dtc01term: %s\n", err);
        kernel_unlock();
        line_close(host);
        term_speech_stop();
        return 1;
    }
    if (strcmp(local_spec, "stdio"))
        fprintf(stderr, "dtc01term: host line %s, local terminal %s. Ctrl+] b = BREAK (SETUP), Ctrl+] q = quit.\n",
                line_describe(host), line_describe(local));
    term_dev_init(host, local, NULL);
    term_speech_boot();
#ifdef _WIN32
    timeBeginPeriod(1);
#endif

    t0 = now_ms();
    for (;;) {
        uint32_t due = (uint32_t)((now_ms() - t0) / 10.0), n = 0;
        while (ticks < due && n++ < 50) {   /* the 10 ms clock (catching up at most 0.5 s) */
            kernel_tick();
            ticks++;
        }
        ticks = due > ticks ? due : ticks;
        term_speech_tick();
        kernel_poke();
        kernel_run();
        term_speech_flush();
        term_dev_flush();
        if (term_speech_restart_wanted()) {         /* DECTST 1, TEST POWER */
            term_speech_restart();
            term_dev_init(host, local, NULL);
            term_speech_boot();
            continue;
        }
        if (term_quit) {
            if (!drain) break;
            if (!drain_start) drain_start = now_ms();
            if ((term_speech_idle() && !kernel_device_count(&host_dev) && !kernel_device_count(&console_dev)) ||
                now_ms() - drain_start > DRAIN_MAX_MS)
                break;
        }
        kernel_unlock();
        term_sleep_ms(2);
        kernel_lock();
    }

    kernel_shutdown();                  /* releases the lock */
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    line_close(local);
    line_close(host);
    term_speech_stop();
    return 0;
}
