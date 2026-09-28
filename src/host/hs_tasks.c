/* The two small host-side tasks (REFERENCE.md s3, s15.23, s15.38), rebuilt in C: "host timeout", which flushes
 * the text the host left unfinished, and "stop", which carries out DT_STOP.
 */
#include "host.h"

/* 0xf070: the "host timeout" task (priority -100, the lowest). It sleeps until the host sends its first byte
 * (read_host_byte sets host_idle to 0), then counts the seconds without host input. At 5 it puts a CTRL-K into the
 * text pipe, which ends the clause the host left open so it is spoken, and sleeps again until the next byte. While
 * the unit holds the host off (XOFF sent), the host is not idle: the count starts over. */
void host_timeout_task_main(void)
{
    host_idle = 5;
    for (;;) {
        event_wait(100, NULL, 0);                       /* one second */
        if (host_idle == 5) continue;
        if (dev_rx_held(&host_dev)) {
            host_idle = 0;
            continue;
        }
        if (++host_idle == 5) {
            log_debug("Host timeout\n");
            stream_putc_to(cur_stream, 0x0b);
            stream_flush(cur_stream);
        }
    }
}

/* 0xfa7a: the "stop" task (priority 0). DT_STOP (dt_stop) wakes it. While it syncs the pipeline, stop_pending tells
 * klsyn to cut the clause it is speaking and to drop the phoneme streams it takes (s15.23). */
void stop_task_main(void)
{
    for (;;) {
        task_suspend(current_task);
        stop_pending++;
        emit_sync_marker("h_stop");
        stop_pending--;
    }
}
