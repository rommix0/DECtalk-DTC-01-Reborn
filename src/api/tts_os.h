/* tts_os.h - the few OS calls the library needs (REFERENCE.md s17.10): a mutex, a condition variable with a
 * timed wait, and a thread, on Win32 or pthreads. */
#ifndef TTS_OS_H
#define TTS_OS_H
#include <stdlib.h>

#if defined(_MSC_VER)
#define OS_FN static __inline
#else
#define OS_FN static inline
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef CRITICAL_SECTION os_mutex_t;
typedef CONDITION_VARIABLE os_cond_t;
typedef HANDLE os_thread_t;
OS_FN void os_mutex_init(os_mutex_t *m) { InitializeCriticalSection(m); }
OS_FN void os_mutex_free(os_mutex_t *m) { DeleteCriticalSection(m); }
OS_FN void os_lock(os_mutex_t *m) { EnterCriticalSection(m); }
OS_FN void os_unlock(os_mutex_t *m) { LeaveCriticalSection(m); }
OS_FN void os_cond_init(os_cond_t *c) { InitializeConditionVariable(c); }
OS_FN void os_cond_free(os_cond_t *c) { (void)c; }
OS_FN void os_signal(os_cond_t *c) { WakeConditionVariable(c); }
OS_FN void os_broadcast(os_cond_t *c) { WakeAllConditionVariable(c); }
/* ms < 0: no time limit */
OS_FN void os_wait(os_cond_t *c, os_mutex_t *m, int ms) { SleepConditionVariableCS(c, m, ms < 0 ? INFINITE : (DWORD)ms); }
typedef struct { void (*fn)(void *); void *arg; } os_start_t;
OS_FN DWORD WINAPI os_thread_main(LPVOID p)
{
    os_start_t s = *(os_start_t *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}
OS_FN int os_thread_start(os_thread_t *t, void (*fn)(void *), void *arg)
{
    os_start_t *s = (os_start_t *)malloc(sizeof *s);
    if (!s) return 0;
    s->fn = fn;
    s->arg = arg;
    if (!(*t = CreateThread(NULL, 0, os_thread_main, s, 0, NULL))) { free(s); return 0; }
    return 1;
}
OS_FN void os_thread_join(os_thread_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t os_mutex_t;
typedef pthread_cond_t os_cond_t;
typedef pthread_t os_thread_t;
OS_FN void os_mutex_init(os_mutex_t *m) { pthread_mutex_init(m, NULL); }
OS_FN void os_mutex_free(os_mutex_t *m) { pthread_mutex_destroy(m); }
OS_FN void os_lock(os_mutex_t *m) { pthread_mutex_lock(m); }
OS_FN void os_unlock(os_mutex_t *m) { pthread_mutex_unlock(m); }
OS_FN void os_cond_init(os_cond_t *c)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
}
OS_FN void os_cond_free(os_cond_t *c) { pthread_cond_destroy(c); }
OS_FN void os_signal(os_cond_t *c) { pthread_cond_signal(c); }
OS_FN void os_broadcast(os_cond_t *c) { pthread_cond_broadcast(c); }
OS_FN void os_wait(os_cond_t *c, os_mutex_t *m, int ms)
{
    struct timespec t;
    if (ms < 0) {
        pthread_cond_wait(c, m);
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(c, m, &t);
}
typedef struct { void (*fn)(void *); void *arg; } os_start_t;
OS_FN void *os_thread_main(void *p)
{
    os_start_t s = *(os_start_t *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}
OS_FN int os_thread_start(os_thread_t *t, void (*fn)(void *), void *arg)
{
    os_start_t *s = (os_start_t *)malloc(sizeof *s);
    if (!s) return 0;
    s->fn = fn;
    s->arg = arg;
    if (pthread_create(t, NULL, os_thread_main, s)) { free(s); return 0; }
    return 1;
}
OS_FN void os_thread_join(os_thread_t t) { pthread_join(t, NULL); }
#endif

#endif
