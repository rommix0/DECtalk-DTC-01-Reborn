/* The klsyn task: its mailbox loop and the index-marker callback (REFERENCE.md s15.23).
 *
 * klsyn_task_main 0x3c20 takes the messages the text pipeline posts to klsyn_mbox (dttask's newclause 0x39aa and
 * the DT_SYNC path in dttask_main 0xf946) and hands each to the speech code:
 *   - one word 0x68: a sync point. The host's DT_SYNC (emit_sync_marker 0x3d56, run by the host and stop tasks)
 *     writes 0x1A into the text pipe and waits on sync_sem; dttask posts 0x68 when its clause scanner reaches that
 *     point, and klsyn signals sync_sem once every earlier message has been spoken (posted to the DSP queue);
 *   - first word 0: the text of a [:dv ...] command, one character per word (low byte) from word 1 on;
 *   - anything else: a phoneme/parameter stream, dropped while stop_pending > 0 (DT_STOP).
 * index_mark_reached 0xfaaa is what phsettar calls when a phone carries an index marker.
 */
#include "ph_frame.h"
#include "../kernel/rtos.h"

#define KLSYN_SYNC 0x68         /* the sync message, and the (unused) sync index mark */

mbox_t klsyn_free_pool;         /* 0x80790: 3 messages of 400 words, taken by newclause */
mbox_t klsyn_mbox;              /* 0x807a2 */
ksem_t sync_sem;                /* 0x81f26: DT_SYNC waits here */
int16_t last_index;             /* 0x81d70: the last index marker spoken (DT_INDEX_QUERY reports it) */
void (*ph_index_reply_hook)(int value); /* host side: send_dcs_reply(31, value) 0xef00 */
void (*ph_mark_hook)(uint32_t mark);    /* the library: marks at audio time (REFERENCE s16.13); NULL = the ROM's way */

/* one message: the body of klsyn_task_main's loop. The C copies at most the 200 words of phonemes[]; the ROM's
 * producer never sends more (newclause reports "bug: newclause message size" above 200). */
void klsyn_dispatch(msg_t *msg)
{
    int16_t *w = msg->data;
    int16_t n = msg->nwords;
    int16_t *p;

    if (n == 1 && w[0] == KLSYN_SYNC) {
        sem_signal(&sync_sem);
    } else if (w[0] == 0) {                     /* [:dv ...]: one character per word */
        char *d = (char *)phonemes;             /* the ROM uses the same RAM (0x81d78) as a byte buffer */
        for (p = w + 1; p < w + n && d < (char *)phonemes + sizeof phonemes - 1; p++) *d++ = (char)*p;
        *d = 0;
        mbox_put(msg->home, msg);
        parse_bracket_command((char *)phonemes);
        return;
    } else if (stop_pending <= 0) {             /* a phoneme/parameter stream */
        int16_t *d = phonemes;
        for (p = w; p < w + n && d < phonemes + PH_MAXALLO; p++) *d++ = *p;
        ph_nside = 0;
        if (ph_fixes & PH_FIX_MARKS) {          /* [DTC01]: the clause's marks, after its words */
            int k, count = msg->pad6;
            for (k = 0; k < count && k < PH_SIDE_MAX && n + 3 * k + 3 <= MSG_MAXWORDS; k++) {
                const int16_t *t = w + n + 3 * k;
                ph_side[ph_nside].pos = t[0];
                ph_side[ph_nside++].mark = (uint32_t)((int32_t)t[1] * 0x10000 + (int32_t)t[2]);
            }
        }
        mbox_put(msg->home, msg);
        parse_phoneme_param_stream(n);
        return;
    }
    mbox_put(msg->home, msg);                   /* sync handled, or a stream dropped by DT_STOP */
}

/* 0x3c20, the "klsyn" task (priority 50) */
void klsyn_task_main(void)
{
    mbox_init_pool(&klsyn_free_pool, 3, 400);
    mbox_init(&klsyn_mbox, 0);
    for (;;) klsyn_dispatch(mbox_get(&klsyn_mbox));
}

/* 0xfaaa: phsettar reached a phone with an index marker, code << 16 | value. Code 0x66 is DT_INDEX / [:index mark],
 * 0x67 DT_INDEX_REPLY / [:index reply], which also answers the host with R2 = 31, R3 = value. Code 0x68 would signal
 * sync_sem, but phalloph only stores 0x66/0x67 marks, so that branch is never taken in v1.8.
 * The ROM acts on the mark here, when the frame is computed, up to the DSP queue's depth (48 frames, 307 ms) before
 * it is heard. The library sets ph_mark_hook instead: the mark rides with the frame to the DAC, and the library calls
 * index_mark_spoken when that frame's first sample is played or delivered. */
void index_mark_reached(uint32_t mark)
{
    int16_t code = (int16_t)((int32_t)mark >> 16);
    if (code == KLSYN_SYNC) {
        sem_signal(&sync_sem);
        return;
    }
    if (ph_mark_hook) ph_mark_hook(mark);
    else index_mark_spoken(mark);
}

void index_mark_spoken(uint32_t mark)
{
    int16_t code = (int16_t)((int32_t)mark >> 16), value = (int16_t)mark;
    last_index = value;
    if (code == 0x67 && ph_index_reply_hook) ph_index_reply_hook(value);
}
