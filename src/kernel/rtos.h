/* The RTOS calls the rebuilt C code uses (REFERENCE.md s15.16).
 *
 * In the ROM these are TRAP #1 system calls (mailbox handlers in C at 0x12088-0x12178). The rebuilt code only
 * declares them here; a C build of the whole firmware, or a standalone speech synth, supplies an implementation, and
 * the test harness (decomp/test/test_frames.c) supplies stubs. The structures follow the ROM's layout so the field
 * meanings stay traceable; only the C code's own use of them matters.
 */
#ifndef RTOS_H
#define RTOS_H
#include <stdint.h>

#define MSG_MAXWORDS 400    /* the largest pool in the ROM: klsyn_free_pool, 3 messages of 400 words */

typedef struct mbox mbox_t;

/* msg_t: +0 next, +4 nwords, +8 home mailbox, +0x10 payload. A consumer hands a message back with
 * mbox_put(msg->home, msg). */
typedef struct msg {
    struct msg *next;
    int16_t nwords;
    int16_t pad6;
    mbox_t *home;
    uint32_t pad0c;
    int16_t data[MSG_MAXWORDS];
} msg_t;

/* mbox_t {head, tail, notify, waiters, count}; a pool is a mailbox pre-filled with free messages */
struct mbox {
    msg_t *head, *tail;
    void (*notify)(void);
    void *waiters;
    int16_t count;
};

/* ksem_t {waiters, count} */
typedef struct {
    void *waiters;
    int16_t count;
} ksem_t;

/* character devices and the stdio-like stream on them: stream_t {cnt, ptr, base, flags (0x10 end, 0x20 error), dev} */
typedef struct chardev chardev_t;
typedef struct {
    int32_t cnt;
    char *ptr, *base;
    int32_t flags;
    chardev_t *dev;
} stream_t;

extern chardev_t console_dev;                           /* 0x80328: the console (the local terminal port) */
int32_t dev_getc(chardev_t *d);                         /* 0xc00: waits for a character */
void dev_putc(chardev_t *d, int32_t c);                 /* 0xc70 */
/* 0xe04: the op in the high word, an argument in the low word (the console lock is -0xffff, its unlock -0x1ffff); some
 * ops take a third argument (-0x80000: a value handed to the device's reader); the result depends on the op */
int32_t dev_control(chardev_t *d, int32_t op, ...);
/* dev + 0x42: the device has sent XOFF and holds its input off (set and cleared by the DUART interrupt, which the ROM
 * reads directly) */
int dev_rx_held(chardev_t *d);
/* 0x11fdc: a pipe of size bytes; *rd gets the reading stream, *wr the writing one; NULL = no memory */
chardev_t *pipe_open(stream_t **rd, stream_t **wr, int size);

void mbox_init(mbox_t *mb, void (*notify)(void));       /* 0x876 */
void mbox_init_pool(mbox_t *pool, int n, int nwords);   /* 0x121c0: n free messages of nwords words */
msg_t *mbox_get(mbox_t *mb);                            /* 0x84c: waits for a message */
void mbox_put(mbox_t *mb, msg_t *msg);                  /* 0x860 */
void sem_wait(ksem_t *s);                               /* 0x88c */
void sem_signal(ksem_t *s);                             /* 0x8b0 */
void event_wait(int32_t ticks, void *a, int32_t b);     /* 0x986: event_wait(n, 0, 0) sleeps n ticks of 10 ms */
void task_resume(void *task);                           /* 0x8fe */
void task_suspend(void *task);                          /* 0x8d2: until task_resume */
extern void *current_task;                              /* 0x80004: the running task's TCB */
int32_t heap_free_total(void);                          /* 0x1494: free bytes in the heap */
void system_restart(void);                              /* 0x10e8: TRAP #14, the power-up restart; never returns */
void panic(const char *msg);                            /* 0x1d618 */

#endif
