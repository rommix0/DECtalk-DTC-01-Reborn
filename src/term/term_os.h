/* dtc01term's few OS calls (REFERENCE.md s17.14): threads, a mutex, sleeping. Windows and POSIX. */
#ifndef TERM_OS_H
#define TERM_OS_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
typedef HANDLE term_thread_t;
typedef CRITICAL_SECTION term_mutex_t;
typedef DWORD(WINAPI *term_thread_fn)(LPVOID);
#define TERM_THREAD(name, arg) static DWORD WINAPI name(LPVOID arg)
#define TERM_THREAD_END return 0
static inline int term_thread_start(term_thread_t *t, term_thread_fn fn, void *arg)
{
    return (*t = CreateThread(NULL, 0, fn, arg, 0, NULL)) != NULL;
}
static inline void term_thread_join(term_thread_t t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
static inline void term_mutex_init(term_mutex_t *m) { InitializeCriticalSection(m); }
static inline void term_mutex_lock(term_mutex_t *m) { EnterCriticalSection(m); }
static inline void term_mutex_unlock(term_mutex_t *m) { LeaveCriticalSection(m); }
static inline void term_sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <pthread.h>
#include <time.h>
typedef pthread_t term_thread_t;
typedef pthread_mutex_t term_mutex_t;
typedef void *(*term_thread_fn)(void *);
#define TERM_THREAD(name, arg) static void *name(void *arg)
#define TERM_THREAD_END return NULL
static inline int term_thread_start(term_thread_t *t, term_thread_fn fn, void *arg)
{
    return pthread_create(t, NULL, fn, arg) == 0;
}
static inline void term_thread_join(term_thread_t t) { pthread_join(t, NULL); }
static inline void term_mutex_init(term_mutex_t *m) { pthread_mutex_init(m, NULL); }
static inline void term_mutex_lock(term_mutex_t *m) { pthread_mutex_lock(m); }
static inline void term_mutex_unlock(term_mutex_t *m) { pthread_mutex_unlock(m); }
static inline void term_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

#endif
