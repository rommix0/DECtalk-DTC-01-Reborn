/* The speech side's kernel: the rtos.h calls for the library (kernel.h; REFERENCE.md s17.9).
 *
 * Each task is an OS thread, but they take turns: the running one holds the kernel's mutex, and hands it on only in
 * a kernel call (give_back). kernel_run(), on the caller's thread, gives the turn to the highest-priority ready task
 * and gets it back when that task waits or is preempted. A task waits on a list in the object (mbox_t.waiters,
 * ksem_t.waiters, the pipe's), first come first served, as in the ROM.
 *
 * What the speech side does not call (event_wait, task_suspend/resume, the heap, system_restart) is not here.
 */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "stream.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef CRITICAL_SECTION kmutex_t;
typedef CONDITION_VARIABLE kcond_t;
typedef HANDLE kthread_t;
static void mu_init(kmutex_t *m) { InitializeCriticalSection(m); }
static void mu_lock(kmutex_t *m) { EnterCriticalSection(m); }
static void mu_unlock(kmutex_t *m) { LeaveCriticalSection(m); }
static void cv_init(kcond_t *c) { InitializeConditionVariable(c); }
static void cv_wait(kcond_t *c, kmutex_t *m) { SleepConditionVariableCS(c, m, INFINITE); }
static void cv_signal(kcond_t *c) { WakeConditionVariable(c); }
#else
#include <pthread.h>
typedef pthread_mutex_t kmutex_t;
typedef pthread_cond_t kcond_t;
typedef pthread_t kthread_t;
static void mu_init(kmutex_t *m) { pthread_mutex_init(m, NULL); }
static void mu_lock(kmutex_t *m) { pthread_mutex_lock(m); }
static void mu_unlock(kmutex_t *m) { pthread_mutex_unlock(m); }
static void cv_init(kcond_t *c) { pthread_cond_init(c, NULL); }
static void cv_wait(kcond_t *c, kmutex_t *m) { pthread_cond_wait(c, m); }
static void cv_signal(kcond_t *c) { pthread_cond_signal(c); }
#endif

#define KTASKS 8
enum { K_READY, K_WAIT, K_DEAD };

struct ktask {
    const char *name;
    void (*entry)(void);
    int pri, state;
    struct ktask *next;         /* in a wait list */
    kcond_t turn;               /* signalled when it is this task's turn */
    kthread_t th;
    jmp_buf out;                /* panic and kernel_shutdown leave the task through here */
};

enum { DEV_CONSOLE, DEV_PIPE };
struct chardev {
    int kind;
    unsigned char *buf;         /* a pipe: a ring that grows */
    int cap, head, count;
    void *waiters;
};

chardev_t console_dev = { DEV_CONSOLE, NULL, 0, 0, 0, NULL };

static kmutex_t k_mu;
static kcond_t k_back;          /* signalled when a task hands the turn back */
static int k_inited;
static ktask_t k_task[KTASKS];
static int k_ntask;
static ktask_t *k_cur;          /* the task whose turn it is; NULL = the caller's thread */
static int k_stop;
static kernel_hooks_t k_hooks;

typedef struct kblock { struct kblock *next; } kblock_t;
static kblock_t *k_allocs;      /* what the kernel allocated, freed by kernel_shutdown */
static chardev_t *k_pipes[4];   /* and the pipes, whose rings grow */
static int k_npipes;

static void *kalloc(size_t n)
{
    kblock_t *b = (kblock_t *)calloc(1, sizeof(kblock_t) + n);
    if (!b) {
        if (k_hooks.panic) k_hooks.panic(k_hooks.ctx, k_cur ? k_cur->name : "kernel", "out of memory");
        abort();
    }
    b->next = k_allocs;
    k_allocs = b;
    return b + 1;
}

/* ---- turns ---- */

/* The running task hands the turn back to kernel_run and waits for its next one. */
static void give_back(void)
{
    ktask_t *t = k_cur;
    k_cur = NULL;
    cv_signal(&k_back);
    while (k_cur != t) cv_wait(&t->turn, &k_mu);
    if (k_stop) longjmp(t->out, 1);
}

static void task_body(ktask_t *t)
{
    mu_lock(&k_mu);
    while (k_cur != t) cv_wait(&t->turn, &k_mu);
    if (!k_stop && !setjmp(t->out)) t->entry();
    t->state = K_DEAD;                          /* the ROM's tasks never return; panic and shutdown end here */
    k_cur = NULL;
    cv_signal(&k_back);
    mu_unlock(&k_mu);
}

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID p) { task_body((ktask_t *)p); return 0; }
static int th_start(ktask_t *t) { return (t->th = CreateThread(NULL, 0, thread_main, t, 0, NULL)) != NULL; }
static void th_join(ktask_t *t) { WaitForSingleObject(t->th, INFINITE); CloseHandle(t->th); }
#else
static void *thread_main(void *p) { task_body((ktask_t *)p); return NULL; }
static int th_start(ktask_t *t) { return pthread_create(&t->th, NULL, thread_main, t) == 0; }
static void th_join(ktask_t *t) { pthread_join(t->th, NULL); }
#endif

/* The running task waits on a list (FIFO) until wake_first takes it off. */
static void wait_on(void **list)
{
    ktask_t *t = k_cur, **p = (ktask_t **)list;
    if (!t) {                                   /* only tasks can wait */
        if (k_hooks.panic) k_hooks.panic(k_hooks.ctx, "kernel", "a wait outside a task");
        abort();
    }
    while (*p) p = &(*p)->next;
    t->next = NULL;
    *p = t;
    t->state = K_WAIT;
    give_back();
}

/* Make the first waiter ready. If a task is running and the woken one has a higher priority, it takes over now. */
static void wake_first(void **list)
{
    ktask_t *t = (ktask_t *)*list;
    if (!t) return;
    *list = t->next;
    t->next = NULL;
    t->state = K_READY;
    if (k_cur && t->pri > k_cur->pri) give_back();      /* preempted: k_cur stays ready */
}

static ktask_t *pick(void)
{
    ktask_t *best = NULL;
    int i;
    for (i = 0; i < k_ntask; i++)
        if (k_task[i].state == K_READY && (!best || k_task[i].pri > best->pri)) best = &k_task[i];
    return best;
}

/* ---- the caller's side ---- */

void kernel_init(const kernel_hooks_t *hooks)
{
    if (!k_inited) {
        mu_init(&k_mu);
        cv_init(&k_back);
        k_inited = 1;
    }
    memset(&k_hooks, 0, sizeof k_hooks);
    if (hooks) k_hooks = *hooks;
    k_ntask = 0;
    k_cur = NULL;
    k_stop = 0;
}

ktask_t *kernel_task(const char *name, void (*entry)(void), int priority)
{
    ktask_t *t;
    if (k_ntask == KTASKS) return NULL;
    t = &k_task[k_ntask];
    memset(t, 0, sizeof *t);
    t->name = name;
    t->entry = entry;
    t->pri = priority;
    t->state = K_READY;
    cv_init(&t->turn);
    if (!th_start(t)) return NULL;
    k_ntask++;
    return t;
}

void kernel_run(void)
{
    ktask_t *t;
    while ((t = pick()) != NULL) {
        k_cur = t;
        cv_signal(&t->turn);
        while (k_cur) cv_wait(&k_back, &k_mu);
    }
}

int kernel_ready(void) { return pick() != NULL; }
ktask_t *kernel_current(void) { return k_cur; }
const char *kernel_task_name(const ktask_t *t) { return t ? t->name : NULL; }

void kernel_shutdown(void)
{
    int i;
    k_stop = 1;
    for (i = 0; i < k_ntask; i++) {
        ktask_t *t = &k_task[i];
        if (t->state == K_DEAD) continue;
        k_cur = t;                              /* it leaves its wait (give_back) through t->out */
        cv_signal(&t->turn);
        while (k_cur) cv_wait(&k_back, &k_mu);
    }
    mu_unlock(&k_mu);
    for (i = 0; i < k_ntask; i++) th_join(&k_task[i]);
    for (i = 0; i < k_npipes; i++) free(k_pipes[i]->buf);
    k_npipes = 0;
    while (k_allocs) {
        kblock_t *b = k_allocs;
        k_allocs = b->next;
        free(b);
    }
    k_ntask = 0;
    k_stop = 0;
}

void kernel_lock(void) { mu_lock(&k_mu); }
void kernel_unlock(void) { mu_unlock(&k_mu); }

/* ---- pipes ---- */

static void pipe_push(chardev_t *d, unsigned char c)
{
    if (d->count == d->cap) {
        int cap = d->cap ? d->cap * 2 : 256, i;
        unsigned char *b = (unsigned char *)malloc((size_t)cap);
        if (!b) {
            if (k_hooks.panic) k_hooks.panic(k_hooks.ctx, "kernel", "out of memory");
            abort();
        }
        for (i = 0; i < d->count; i++) b[i] = d->buf[(d->head + i) % d->cap];
        free(d->buf);
        d->buf = b;
        d->cap = cap;
        d->head = 0;
    }
    d->buf[(d->head + d->count++) % d->cap] = c;
}

void kernel_pipe_write(chardev_t *pipe, const char *s, int n)
{
    int i;
    if (!pipe || pipe->kind != DEV_PIPE) return;
    for (i = 0; i < n; i++) pipe_push(pipe, (unsigned char)s[i]);
    if (n > 0) wake_first(&pipe->waiters);
}

int kernel_pipe_count(const chardev_t *pipe) { return pipe && pipe->kind == DEV_PIPE ? pipe->count : 0; }

void kernel_pipe_clear(chardev_t *pipe)
{
    if (pipe && pipe->kind == DEV_PIPE) pipe->head = pipe->count = 0;
}

/* 0x11fdc. The size is the ROM's (0x40 bytes for the text pipe); here the pipe grows instead, so the host side never
 * waits for dttask. */
chardev_t *pipe_open(stream_t **rd, stream_t **wr, int size)
{
    chardev_t *d = (chardev_t *)kalloc(sizeof *d);
    stream_t *r = (stream_t *)kalloc(sizeof *r), *w = (stream_t *)kalloc(sizeof *w);
    (void)size;
    if (k_npipes == (int)(sizeof k_pipes / sizeof k_pipes[0])) return NULL;
    k_pipes[k_npipes++] = d;
    d->kind = DEV_PIPE;
    r->dev = w->dev = d;
    r->flags = STREAM_READ;
    w->flags = STREAM_WRITE;
    *rd = r;
    *wr = w;
    return d;
}

/* ---- the rtos.h calls ---- */

int32_t dev_getc(chardev_t *d)
{
    int32_t c;
    if (k_hooks.call) k_hooks.call(k_hooks.ctx, d);
    if (d->kind != DEV_PIPE)
        for (;;) wait_on(&d->waiters);          /* the library has no console input */
    while (!d->count) wait_on(&d->waiters);
    c = d->buf[d->head];
    d->head = (d->head + 1) % d->cap;
    d->count--;
    return c;
}

void dev_putc(chardev_t *d, int32_t c)
{
    if (d->kind == DEV_PIPE) {
        pipe_push(d, (unsigned char)c);
        wake_first(&d->waiters);
    } else if (k_hooks.console) {
        k_hooks.console(k_hooks.ctx, (int)(c & 0xff));
    }
}

int32_t dev_control(chardev_t *d, int32_t op, ...)
{
    (void)d;
    (void)op;
    return 0;                                   /* the console lock: only one task runs at a time anyway */
}

int dev_rx_held(chardev_t *d)
{
    (void)d;
    return 0;
}

void mbox_init(mbox_t *mb, void (*notify)(void))
{
    memset(mb, 0, sizeof *mb);
    mb->notify = notify;
}

void mbox_init_pool(mbox_t *pool, int n, int nwords)
{
    int i;
    mbox_init(pool, NULL);
    for (i = 0; i < n; i++) {
        msg_t *m = (msg_t *)kalloc(sizeof *m);
        m->home = pool;
        m->nwords = (int16_t)nwords;
        mbox_put(pool, m);
    }
}

msg_t *mbox_get(mbox_t *mb)
{
    msg_t *m;
    if (k_hooks.call) k_hooks.call(k_hooks.ctx, mb);
    while (!mb->head) wait_on(&mb->waiters);
    m = mb->head;
    mb->head = m->next;
    if (!mb->head) mb->tail = NULL;
    mb->count--;
    m->next = NULL;
    if (k_hooks.got) k_hooks.got(k_hooks.ctx, mb, m);
    return m;
}

void mbox_put(mbox_t *mb, msg_t *msg)
{
    msg->next = NULL;
    if (mb->tail) mb->tail->next = msg;
    else mb->head = msg;
    mb->tail = msg;
    mb->count++;
    if (mb->notify) mb->notify();
    wake_first(&mb->waiters);
}

void sem_wait(ksem_t *s)
{
    if (k_hooks.call) k_hooks.call(k_hooks.ctx, s);
    if (s->count > 0) {
        s->count--;
        return;
    }
    wait_on(&s->waiters);                       /* sem_signal hands the count straight to the first waiter */
}

void sem_signal(ksem_t *s)
{
    if (s->waiters) wake_first(&s->waiters);
    else s->count++;
}

void panic(const char *msg)
{
    if (k_hooks.panic) k_hooks.panic(k_hooks.ctx, k_cur ? k_cur->name : "kernel", msg ? msg : "");
    if (k_cur) longjmp(k_cur->out, 1);
    abort();
}
