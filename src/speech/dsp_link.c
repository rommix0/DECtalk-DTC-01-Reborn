/* dsp_link.c - the 68000's link to the DSP, and the DAC clock (REFERENCE.md s16.11).
 *
 * The 68000's side is the ROM's: dsp_link_init 0x7aec, dsp_queue_init 0x12226, dsp_post_frame 0x7b56 (the queueing
 * half; ph_frame.c builds the words), spc_irq_enable 0x12216, dsp_command_queue_isr 0x12258 with its helpers
 * spc_reset_wait 0x122fa and dsp_send_speech_frame 0x122a4, and the tone hooks 0x124a2 / 0x124fe. The SPC
 * behaves as native/dtc01.c has it. The interrupt handler runs between two DSP passes: when the semaphore rises
 * with the interrupt on, and when a post turns the interrupt on.
 */
#include <string.h>
#include "dsp_link.h"

/* SPC flag writes (0x9C000) */
enum { SPC_RESET = 0x01, SPC_ACK = 0x02, SPC_IRQ = 0x40 };

#define TONE_ON_TICKS 16        /* 160 ms of tone */
#define TONE_OFF_TICKS 6        /* then 60 ms of silence */

static void isr(dsp_link_t *l);

/* A tagged frame has reached the DSP: its tag goes out with the next sample. */
static void tag_read(dsp_link_t *l, uint32_t tag)
{
    if (!tag) return;
    if (l->ntags == DSP_TAGS_MAX) {             /* cannot happen (one frame per pass); report it now rather than lose it */
        if (l->hooks.mark) l->hooks.mark(l->hooks.ctx, tag, l->samples);
        return;
    }
    l->tags[l->ntags++] = tag;
}

/* ---- the SPC ---- */
static void spc_flags(dsp_link_t *l, int v)
{
    int i;
    l->irq_en = (v & SPC_IRQ) != 0;
    if (v & SPC_RESET) {                        /* hold the DSP in reset; the FIFO and the semaphore clear */
        for (i = 0; i < DSP_LINK_FIFO; i++) tag_read(l, l->fifo_tag[i]);    /* never lose a tag */
        memset(l->fifo_tag, 0, sizeof l->fifo_tag);
        memset(l->fifo, 0, sizeof l->fifo);
        l->fifo_head = l->fifo_tail = l->fifo_count = 0;
        l->in_reset = 1;
        l->err = l->sem = 0;
    } else if (l->in_reset) {                   /* let it go: COLD_START, which raises the semaphore */
        l->in_reset = 0;
        l->resets++;
        if (l->hooks.reset) l->hooks.reset(l->hooks.ctx);
        dsp_reset(&l->dsp);
    }
    if (v & SPC_ACK) l->err = l->sem = 0;
}

static void fifo_write(dsp_link_t *l, uint16_t w, uint32_t tag)
{
    if (l->fifo_count == DSP_LINK_FIFO) { tag_read(l, tag); return; }
    l->fifo[l->fifo_head] = w;
    l->fifo_tag[l->fifo_head] = tag;
    l->fifo_head = (l->fifo_head + 1) % DSP_LINK_FIFO;
    l->fifo_count++;
}

/* the DSP's ports */
static int io_waiting(void *ctx, int where)
{
    dsp_link_t *l = (dsp_link_t *)ctx;
    if (l->hooks.poll) l->hooks.poll(l->hooks.ctx, where, !l->sem);
    return !l->sem;
}

static uint16_t io_read(void *ctx)
{
    dsp_link_t *l = (dsp_link_t *)ctx;
    uint16_t w = l->fifo[l->fifo_tail];         /* an empty FIFO gives its last word again */
    if (l->fifo_count > 0) {
        tag_read(l, l->fifo_tag[l->fifo_tail]);
        l->fifo_tag[l->fifo_tail] = 0;
        l->fifo_tail = (l->fifo_tail + 1) % DSP_LINK_FIFO;
        l->fifo_count--;
    }
    if (l->hooks.word) l->hooks.word(l->hooks.ctx, w);
    return w;
}

static void io_signal(void *ctx, uint16_t v)
{
    dsp_link_t *l = (dsp_link_t *)ctx;
    if (l->hooks.signal) l->hooks.signal(l->hooks.ctx, v);
    l->sem = 1;
    l->err = v & 1;
}

/* ---- the DAC side: one DSP pass; a sample goes to the held queue until the caller takes it ---- */
static void service(dsp_link_t *l);

/* Would the interrupt handler reset the DSP, if it ran now? */
static int isr_resets(const dsp_link_t *l)
{
    const dsp_msg_t *m = l->head;
    if (l->err) return 1;
    if (!m || (m->w[0] & 0x8000)) return 0;
    return l->tone_active || m->w[0] == 0x6000;
}

/* The 68000 answers the semaphore within microseconds, before the DSP has finished its pass: a reset then kills
 * the pass, and its sample never reaches the DAC. Anything else the handler does (words into the FIFO, the
 * acknowledge) only shows at the DSP's next poll, after the pass, so the sample goes out first. */
static int pass(dsp_link_t *l)
{
    uint16_t w;
    long resets = l->resets;
    int made, killed;
    if (l->in_reset) return 0;
    made = dsp_step(&l->dsp, &w);
    if (l->sem && l->irq_en && !l->in_isr && isr_resets(l)) service(l);
    else if (made && l->hooks.mid_pass) l->hooks.mid_pass(l->hooks.ctx);
    killed = l->resets != resets;
    if (made && !killed) {
        int i;
        for (i = 0; i < l->ntags; i++)
            if (l->hooks.mark) l->hooks.mark(l->hooks.ctx, l->tags[i], l->samples);
        l->ntags = 0;
        l->samples++;
        if (l->hooks.sample) l->hooks.sample(l->hooks.ctx, w);
        if (l->nheld < DSP_HELD_MAX) {
            l->held[(l->held_head + l->nheld) % DSP_HELD_MAX] = (int16_t)(w & 0xFFF0);
            l->nheld++;
        }
    }
    service(l);
    return made && !killed;
}

/* The 68000 waits until the DSP raises its semaphore; the DSP goes on making samples meanwhile. */
static void wait_semaphore(dsp_link_t *l)
{
    long guard = 0;
    while (!l->sem && guard++ < 100000) pass(l);
}

/* ---- the 68000's side ---- */
static void give_back(dsp_link_t *l, dsp_msg_t *m)     /* mbox_put(msg->home, msg) */
{
    if (l->hooks.returned) l->hooks.returned(l->hooks.ctx, m);
    m->next = l->free_list;
    l->free_list = m;
    l->nfree++;
}

static dsp_msg_t *dequeue(dsp_link_t *l)
{
    dsp_msg_t *m = l->head;
    l->head = m->next;
    if (!l->head) l->tail = NULL;
    return m;
}

static void service(dsp_link_t *l)
{
    if (!l->in_isr && l->sem && l->irq_en) isr(l);
}

/* spc_reset_wait 0x122fa */
static void reset_dsp(dsp_link_t *l)
{
    spc_flags(l, SPC_RESET);
    spc_flags(l, 0);
    wait_semaphore(l);
}

/* dsp_send_speech_frame 0x122a4: the 24-word speaker frame, then acknowledge with the interrupt on */
static void send_speaker(dsp_link_t *l, const dsp_msg_t *m, uint32_t tag)
{
    int i;
    for (i = 0; i < 24; i++) fifo_write(l, m->w[i], i ? 0 : tag);
    spc_flags(l, SPC_IRQ | SPC_ACK);
}

/* dsp_command_queue_isr 0x12258 */
static void isr(dsp_link_t *l)
{
    dsp_msg_t *m;
    int i;
    l->in_isr = 1;
    if (l->err) {                                   /* the DSP went idle after speech, or found a bad frame */
        reset_dsp(l);
        if (l->speaker) send_speaker(l, l->speaker, 0);
        goto out;
    }
    m = l->head;
    if (!m) {                                       /* nothing to send: interrupt off, semaphore left up */
        spc_flags(l, 0);
        goto out;
    }
    if (m->w[0] & 0x8000) {                         /* a tone item: both words, each waited for; it stays queued */
        fifo_write(l, m->w[0], m->tag);
        spc_flags(l, SPC_ACK);
        wait_semaphore(l);
        fifo_write(l, m->tone_arg, 0);
        spc_flags(l, SPC_ACK);
        wait_semaphore(l);
        l->tone_active = 1;
        l->tone_hook = DSP_TONE_TIMEOUT;
        l->tone_ticks = m->on_ticks;
        l->tone_off_ticks = m->off_ticks;
        l->tick_samples = 0;
        goto out;
    }
    if (l->tone_active) {                           /* out of tone mode: reset, and the voice again first */
        l->tone_active = 0;
        reset_dsp(l);
        if (m->w[0] != 0x6000 && l->speaker) {
            send_speaker(l, l->speaker, 0);
            goto out;
        }
    }
    dequeue(l);
    if (m->w[0] == 0x6000) {                        /* a speaker frame: reset first, and keep it */
        reset_dsp(l);
        send_speaker(l, m, m->tag);
        if (l->speaker) give_back(l, l->speaker);
        l->speaker = m;
    } else {
        for (i = 0; i < 19; i++) fifo_write(l, m->w[i], i ? 0 : m->tag);
        spc_flags(l, SPC_IRQ | SPC_ACK);
        give_back(l, m);
    }
out:
    l->in_isr = 0;
}

/* the tick hook 0x82272: dsp_tone_timeout_hook 0x124a2 (both tones to 0 Hz), dsp_tone_done_hook 0x124fe */
void dsp_link_tick(dsp_link_t *l)
{
    if (l->tone_hook == DSP_TONE_OFF || --l->tone_ticks > 0) return;
    if (l->tone_hook == DSP_TONE_TIMEOUT) {
        fifo_write(l, 0x8000, 0);
        spc_flags(l, SPC_ACK);
        wait_semaphore(l);
        fifo_write(l, 0x9000, 0);
        spc_flags(l, SPC_ACK);
        wait_semaphore(l);
        l->tone_hook = DSP_TONE_DONE;
        l->tone_ticks = l->tone_off_ticks;
    } else {
        give_back(l, dequeue(l));
        l->tone_hook = DSP_TONE_OFF;
        spc_flags(l, SPC_IRQ);
        service(l);
    }
}

/* The library's Reset; v1.8 has no counterpart (its DT_STOP lets the queue play out). Every queued message goes back
 * to the pool, a tone stops, the DSP is reset and gets the speaker frame again (as after an error), and the tags of
 * what is dropped are dropped too. The DSP then waits for the next frame. (Samples already made stay: they are
 * counted, and the caller drops them if it wants.) */
void dsp_link_flush(dsp_link_t *l)
{
    while (l->head) give_back(l, dequeue(l));
    l->tone_active = 0;
    l->tone_hook = DSP_TONE_OFF;
    memset(l->fifo_tag, 0, sizeof l->fifo_tag);  /* reset_dsp would pass them on to the next sample */
    l->in_isr = 1;                                  /* no interrupt while the DSP restarts */
    reset_dsp(l);
    if (l->speaker) send_speaker(l, l->speaker, 0);
    l->in_isr = 0;
    l->ntags = 0;
    service(l);
}

static dsp_msg_t *take_message(dsp_link_t *l)
{
    dsp_msg_t *m = l->free_list;
    if (!m) return NULL;
    l->free_list = m->next;
    l->nfree--;
    m->next = NULL;
    return m;
}

/* mbox_put(dsp_link_queue): append, then the notify, spc_irq_enable */
static void enqueue(dsp_link_t *l, dsp_msg_t *m)
{
    if (l->tail) l->tail->next = m;
    else l->head = m;
    l->tail = m;
    spc_flags(l, SPC_IRQ);
    service(l);
}

/* ---- the public side ---- */
void dsp_link_init(dsp_link_t *l, const dsp_link_hooks_t *hooks)
{
    int i;
    memset(l, 0, sizeof *l);
    if (hooks) l->hooks = *hooks;
    l->io.ctx = l;
    l->io.frame_waiting = io_waiting;
    l->io.read_word = io_read;
    l->io.signal = io_signal;
    dsp_init(&l->dsp, &l->io);
    for (i = DSP_LINK_POOL - 1; i >= 0; i--) {
        l->pool[i].next = l->free_list;
        l->free_list = &l->pool[i];
    }
    l->nfree = DSP_LINK_POOL;
    l->in_reset = 1;                                /* the DSP is held in reset from power-up */
    reset_dsp(l);
}

int dsp_link_post(dsp_link_t *l, const int16_t *w, int n, uint32_t tag)
{
    dsp_msg_t *m;
    uint16_t last;
    int i;
    if (n < 1 || n >= DSP_LINK_WORDS || !(m = take_message(l))) return 0;
    if ((uint16_t)w[0] == 0x6000) {                 /* the checksum: all words sum to 0 */
        last = 0x6000;
        for (i = 1; i < n; i++) last = (uint16_t)(last + (uint16_t)w[i]);
        last = (uint16_t)-last;
    } else {
        last = 0x43D4;                              /* the trailer */
    }
    for (i = 0; i < n; i++) m->w[i] = (uint16_t)w[i];
    m->w[n] = last;
    m->n = n + 1;
    m->tag = tag;
    enqueue(l, m);
    return 1;
}

int dsp_link_post_tone(dsp_link_t *l, uint16_t w0, uint16_t arg, uint32_t tag)
{
    return dsp_link_post_tone_ticks(l, w0, arg, TONE_ON_TICKS, TONE_OFF_TICKS, tag);
}

int dsp_link_post_tone_ticks(dsp_link_t *l, uint16_t w0, uint16_t arg, int on_ticks, int off_ticks, uint32_t tag)
{
    dsp_msg_t *m = take_message(l);
    if (!m) return 0;
    m->w[0] = w0;
    m->tone_arg = arg;
    m->n = 1;
    m->on_ticks = on_ticks < 1 ? 1 : on_ticks;
    m->off_ticks = off_ticks < 1 ? 1 : off_ticks;
    m->tag = tag;
    enqueue(l, m);
    return 1;
}

int dsp_link_tone_done(const dsp_link_t *l)
{
    return l->tone_active && l->tone_hook == DSP_TONE_OFF && !l->head;
}

int dsp_link_room(const dsp_link_t *l)
{
    return l->nfree;
}

int dsp_link_quiet(const dsp_link_t *l)
{
    const dsp_t *d = &l->dsp;
    if (l->head || l->fifo_count || l->nheld) return 0;
    if (dsp_link_tone_done(l) || l->in_reset || l->sem) return 1;
    return d->mode == DSP_WAIT_READY || (d->mode == DSP_SPEECH && d->frame_due && d->idle > 2);
}

int dsp_link_step(dsp_link_t *l, int16_t *pcm, int *starved)
{
    int made = 0;
    *starved = 0;
    if (!l->nheld) pass(l);
    if (l->nheld) {
        *pcm = l->held[l->held_head];
        l->held_head = (l->held_head + 1) % DSP_HELD_MAX;
        l->nheld--;
        made = 1;
    } else {
        *starved = l->sem || l->in_reset;           /* nothing until the 68000 acknowledges */
    }
    return made;
}

int dsp_link_run(dsp_link_t *l, int16_t *pcm, int max)
{
    int n = 0, starved;
    while (n < max) {
        if (dsp_link_step(l, &pcm[n], &starved)) {
            n++;
            if (++l->tick_samples >= DSP_TICK_SAMPLES) {
                l->tick_samples = 0;
                dsp_link_tick(l);
            }
        }
        else if (starved) break;
        if (l->yield) {
            l->yield = 0;
            break;
        }
    }
    return n;
}
