/* The speech side's kernel (REFERENCE.md s17.9): the rtos.h calls, for the library.
 *
 * The ROM's tasks are written as blocking loops (dttask waits in dev_getc and for a free message, klsyn in mbox_get
 * and for room in the DSP queue), often deep in their call stacks. So each task keeps its own stack here: it is an OS
 * thread. But only one of them runs at a time, as on the 68000: a task runs until it waits in a kernel call, and a
 * task woken by a higher-priority one takes over at that call (the ROM's preemption, which only kernel calls and
 * interrupts cause). The code the tasks run therefore needs no locks, and the order of everything is fixed: the same
 * input gives the same output.
 *
 * The caller's thread (the library's) is the scheduler and the interrupt level: kernel_run() runs the tasks until
 * all of them wait, and between runs the caller may do what the ROM's interrupt handlers do (the DSP link gives back
 * messages, the host side writes into the pipe). Everything the tasks share with other threads is under
 * kernel_lock(); the tasks themselves run inside it.
 *
 * One kernel per process: the firmware's state is in globals.
 */
#ifndef KERNEL_H
#define KERNEL_H
#include "rtos.h"

typedef struct ktask ktask_t;

typedef struct {
    void *ctx;
    /* the running task calls dev_getc, mbox_get or sem_wait on obj (a chardev_t, mbox_t or ksem_t); called on every
     * call, before the task would wait. A test harness may supply what it waits for here. */
    void (*call)(void *ctx, const void *obj);
    void (*got)(void *ctx, mbox_t *mb, msg_t *m);       /* mbox_get returns m */
    void (*console)(void *ctx, int c);                  /* a byte for the console (console_dev) */
    void (*panic)(void *ctx, const char *task, const char *msg); /* the task stops for good */
} kernel_hooks_t;

void kernel_init(const kernel_hooks_t *hooks);
/* A task, ready to run from entry (the ROM's task_create). Higher priority runs first. NULL: no thread. */
ktask_t *kernel_task(const char *name, void (*entry)(void), int priority);
/* Run the ready tasks until every task waits. Call it with the lock held, from the caller's thread. */
void kernel_run(void);
/* 1 if a task is ready to run (woken since the last kernel_run). */
int kernel_ready(void);
/* The running task, and its name (NULL when the caller's thread runs). */
ktask_t *kernel_current(void);
const char *kernel_task_name(const ktask_t *t);
/* Stop every task (each leaves its wait) and free what the kernel allocated. Call it with the lock held; it
 * releases it. */
void kernel_shutdown(void);

void kernel_lock(void);
void kernel_unlock(void);

/* The writing end of a pipe (pipe_open), for the caller's thread: n bytes, never blocking (the pipe grows). */
void kernel_pipe_write(chardev_t *pipe, const char *s, int n);
int kernel_pipe_count(const chardev_t *pipe);           /* bytes not yet read */
void kernel_pipe_clear(chardev_t *pipe);                /* drop them */

/* ---- for a program that runs the host side (dtc01term, REFERENCE.md s17.14); the library uses none of this ---- */

/* A device with a driver: what dev_putc, dev_control (the device's own ops) and dev_rx_held do is the driver's;
 * what dev_getc reads is an input ring that the caller's side fills with kernel_device_input, as the ROM's receive
 * interrupt fills a device's ring. */
typedef struct {
    void (*putc)(void *ctx, int c);                         /* a byte out */
    int32_t (*control)(void *ctx, int32_t op);              /* an op >= 0: the op in the high word, its argument in
                                                             * the low word; the result is dev_control's */
    int (*rx_held)(void *ctx);                              /* dev_rx_held: the device holds its input off (XOFF) */
    void (*got)(void *ctx, int left);                       /* dev_getc took a value; left = values still waiting */
} kdev_ops_t;

#define KDEV_RING 512
struct chardev {
    int kind;
    unsigned char *buf;         /* a pipe: a ring that grows */
    int cap, head, count;
    void *waiters;
    /* a device with a driver */
    const kdev_ops_t *ops;
    void *ctx;
    int32_t ring[KDEV_RING];    /* received values: bytes, device events */
    int rhead, rcount;
    int32_t rx_timer, rx_left;  /* the input timer (DEV_RX_TIMER): its period in ticks (0 = off), ticks to go */
    int timed_out;              /* the timer ran out while a reader waited: its dev_getc returns DEV_TIMEOUT */
};

/* d becomes a device with a driver (console_dev too). */
void kernel_device_init(chardev_t *d, const kdev_ops_t *ops, void *ctx);
/* The caller's side, with the lock held: a received value for the device's reader (dropped when the ring is full). */
void kernel_device_input(chardev_t *d, int32_t v);
int kernel_device_count(const chardev_t *d);            /* values waiting */

/* The clock: the caller calls kernel_tick every 10 ms (the ROM's tick), with the lock held, then kernel_run. It
 * wakes the tasks in event_wait whose time has come, runs the input timers and re-checks kernel_wait_until. */
void kernel_tick(void);
uint32_t kernel_ticks(void);
/* The running task waits until done(ctx) is true; checked at every tick and every kernel_poke. */
void kernel_wait_until(int (*done)(void *ctx), void *ctx);
/* The caller's side, with the lock held: re-check the tasks in kernel_wait_until now (something they wait for may
 * have happened). */
void kernel_poke(void);

#endif
