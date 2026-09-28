/* The host line's commands (REFERENCE.md s5.2-s5.5, s15.32, s15.35): CSI (DA, DSR, DECSTR, DECNVR, DECTST) and
 * DECtalk's DCS commands (DT_PHOTEXT ... DT_TERMINAL), with DT_PHONE's sub-commands and the dialer, rebuilt in C.
 *
 * Index marks and phonemic text go into the text pipe as in-text commands: DT_INDEX writes "\n STX :in n ETX \n",
 * DT_INDEX_REPLY ":re n", DT_PHOTEXT wraps its text in STX ... ETX. DT_SYNC writes the 0x1A marker and waits until
 * klsyn has the text before it; DT_STOP wakes the stop task, which does the same. Both turn speaking back on.
 * There is no DT_MASK (P2 83) in v1.8: like any unknown P2 it is an error (26).
 */
#include <string.h>
#include "host.h"
#include "console.h"

char dcs_text[256];
char *dcs_text_ptr;
int16_t phone_nparams;
int16_t phone_params[16];

/* 0xdff2: DSR, CSI 0 n (or 3 n when an error bit is set); extended, CSI ? 20 n or the errors CSI ? 22 ; ... n, and
 * the error bits are cleared. v1.8 always says 20, never 21 ("first report since power-up"). */
void dsr_reply(int extended)
{
    host_seq_t s = { 0 };
    uint16_t n;
    s.final = 'n';
    s.p[0] = 1;
    s.p[1] = dt_error_flags ? 3 : 0;
    send_escape_response(0x9b, &s);
    if (!extended) return;
    s.priv = 1;
    n = dt_error_flags & 0x01 ? 1 : 0;
    if (n) s.p[1] = 22;                     /* communication failure */
    if (dt_error_flags & 0x02) s.p[++n] = 23;   /* input buffer overflow */
    if (dt_error_flags & 0x04) s.p[++n] = 24;   /* the last NVR operation failed */
    if (dt_error_flags & 0x08) s.p[++n] = 25;   /* phonemic transcription */
    if (dt_error_flags & 0x10) s.p[++n] = 26;   /* a private (DCS) sequence */
    if (dt_error_flags & 0x20) s.p[++n] = 27;   /* the last DECTST */
    s.p[0] = n;
    if (!n) {
        s.p[0] = 1;
        s.p[1] = 20;
    }
    dt_error_flags = 0;
    send_escape_response(0x9b, &s);
}

/* 0xddd8: a CSI sequence */
int32_t csi_command_dispatch(void)
{
    host_seq_t *s = &host_seq;
    int16_t pn, pm;
    switch (s->final) {
    case 'c':                               /* DA */
        if (!s->priv && !s->nint && (!s->p[0] || !s->p[1])) {
            log_trace("%s: primary device attribute request\n", "DA");
            send_escape_response(0x9b, &da_reply);
        }
        break;
    case 'n':                               /* DSR */
        if (s->priv) break;
        if (!s->p[0] || !s->p[1]) {
            log_trace("DSR: extended\n");
            dsr_reply(1);
        } else if (s->p[1] == 5) {
            log_trace("DSR: brief\n");
            dsr_reply(0);
        }
        break;
    case 'p':                               /* DECSTR, CSI ! p */
        if (s->nint == 1 && s->inter[0] == '!') {
            log_trace("DECSTR: soft reset\n");
            settings_reset(1, 0);
        }
        break;
    case 'r':                               /* DECNVR, CSI Pn ; Pm ! r */
        if (s->nint != 1 || s->inter[0] != '!') break;
        pn = s->p[0] ? (int16_t)s->p[1] : 0;
        pm = s->p[0] > 1 ? (int16_t)s->p[2] : 0;
        log_trace("DECNVR: NVR control, Pn = %d, Pm = %d\n", (int32_t)pn, (int32_t)pm);
        if (pn == 0) settings_reset(2, pm);
        else if (pn == 1) nvram_save_settings(pm);
        break;
    case 'y':                               /* DECTST, CSI 5 ; Pn y */
        if (s->p[0] < 2 || s->p[1] != 5) break;
        log_trace("DECTST: self test %d\n", (int32_t)s->p[2]);
        dectst_self_test(s->p[2]);
        break;
    }
    return 0;
}

/* 0xeecc: collects the text after a DCS command's 'z' into dcs_text; the excess is dropped */
void dcs_text_putc(int32_t c)
{
    if (dcs_text_ptr < dcs_text + sizeof dcs_text) *dcs_text_ptr++ = (char)c;
}

/* 0xe8ec: DT_DICT's text "name substitution": the reply's R3, 0 = loaded (or deleted), 1 = no room */
int parse_dict_entry_command(char *text)
{
    char *name, *end, *subst;
    for (name = text; *name == ' ' || *name == '\t'; name++) {}
    if (!*name) return 0;
    for (end = name; *end && *end != ' ' && *end != '\t'; end++) {}
    for (subst = end; *subst == ' ' || *subst == '\t'; subst++) {}
    *end = 0;
    log_trace("DT_DICT: name=\"%s\" definition=\"%s\"\n", name, subst);
    return !dict_hash_insert_or_delete(name, subst) && *subst;
}

/* 0xe152: a DCS sequence, ESC P 0 ; P2 [; P3 ...] z [text] ESC \ */
int32_t dcs_command_dispatch(void)
{
    host_seq_t *s = &host_seq;
    uint16_t p3 = s->p[3];
    int16_t p1, p2;
    int32_t c, i, r3;
    if (s->final != 'z') goto skip;
    p1 = s->p[0] ? (int16_t)s->p[1] : 0;
    if (p1) {
        dt_error_flags |= ERR_DCS;
        log_error("DT_XXX: bad DECtalk DCS, p1 = %d\n", (int32_t)p1);
        goto skip;
    }
    p2 = s->p[0] > 1 ? (int16_t)s->p[2] : 0;
    switch (p2) {
    case 0:                                 /* DT_PHOTEXT */
        log_trace("DT_PHOTEXT: phonemic text\n");
        if (!speak_enabled) return consume_control_string(NULL);
        stream_putc(2);
        c = consume_control_string((void (*)(int32_t))stream_putc);
        stream_putc(3);
        return c;
    case 10:                                /* DT_STOP */
        log_trace("DT_STOP: stop speaking\n");
        dt_stop("host");
        speak_enabled = 1;
        break;
    case 11:                                /* DT_SYNC */
        log_trace("DT_SYNC: synchronize\n");
        emit_sync_marker("host");
        speak_enabled = 1;
        break;
    case 12:                                /* DT_SPEAK */
        if (s->p[0] < 3) {
            log_trace("DT_SPEAK: speak, P3 = 0 (default)\n");
            speak_enabled = 0;
            break;
        }
        log_trace("DT_SPEAK: speak, P3 = %d\n", (int32_t)p3);
        speak_enabled = p3 != 0;
        break;
    case 20:                                /* DT_INDEX */
    case 21:                                /* DT_INDEX_REPLY */
        if (s->p[0] < 3) {
            log_error(p2 == 20 ? "DT_INDEX_TEXT: missing P3\n" : "DT_INDEX_REPLY: missing P3\n");
            break;
        }
        log_trace(p2 == 20 ? "DT_INDEX_TEXT: P3 = %d\n" : "DT_INDEX_REPLY: P3 = %d\n", (int32_t)(p3 & 0x7fff));
        stream_printf(cur_stream, p2 == 20 ? "\n\x02:in %d\x03\n" : "\n\x02:re %d\x03\n", (int32_t)(p3 & 0x7fff));
        break;
    case 22:                                /* DT_INDEX_QUERY */
        log_trace("DT_INDEX_QUERY: last index = %d\n", (int32_t)last_index);
        send_dcs_reply(32, last_index);
        break;
    case 40:                                /* DT_DICT */
        dcs_text_ptr = dcs_text;
        c = consume_control_string(dcs_text_putc);
        if (dcs_text_ptr == dcs_text + sizeof dcs_text) {
            log_error("DT_DICT: text too long\n");
            r3 = 2;
        } else {
            *dcs_text_ptr = 0;
            r3 = parse_dict_entry_command(dcs_text);
        }
        send_dcs_reply(50, (int16_t)r3);
        return c;
    case 60:                                /* DT_PHONE */
        phone_nparams = (int16_t)s->p[0];
        for (i = 0; i < phone_nparams; i++) phone_params[i] = (int16_t)s->p[i + 1];
        dcs_text_ptr = dcs_text;
        c = consume_control_string(dcs_text_putc);
        if (dcs_text_ptr != dcs_text + sizeof dcs_text) {
            *dcs_text_ptr = 0;
            dt_phone_command();
            return c;
        }
        log_error("DT_PHONE_HOME: text too long\n");  /* 256 characters or more: R3 = 3 */
        send_dcs_reply(70, 3);
        return c;
    case 80:                                /* DT_MODE */
        dt_mode = s->p[0] < 3 ? 0 : (int16_t)p3;
        log_trace("DT_MODE: P3 = %d\n", (int32_t)dt_mode);
        break;
    case 81:                                /* DT_LOG */
        dt_log = s->p[0] < 3 ? 0 : (int16_t)p3;
        log_trace("DT_LOG: P3 = %d\n", (int32_t)dt_log);
        break;
    case 82:                                /* DT_TERMINAL */
        dt_terminal = s->p[0] < 3 ? 0 : (int16_t)p3;
        log_trace("DT_TERMINAL: P3 = %d\n", (int32_t)dt_terminal);
        break;
    default:                                /* DT_MASK (83) included */
        dt_error_flags |= ERR_DCS;
        log_error("DT_XXX: P2 = %d\n", (int32_t)s->p[2]);
        break;
    }
skip:
    return consume_control_string(NULL);
}

/* 0xe5f6: DT_PHONE's sub-commands P3, P4, ... in order (s5.3). The reply R3 = the hook state (1 off hook) follows,
 * except after a hang-up: the phone task sends that one, once the line is on hook. */
void dt_phone_command(void)
{
    int16_t *p = &phone_params[2], *end = &phone_params[phone_nparams];
    int32_t cmd, n;
    int hangup = 0;
    while (p < end) {
        cmd = *p++;
        switch (cmd) {
        case 0:
            log_trace("DT_PHONE_HOME: status request\n");
            break;
        case 10:                            /* answer after n rings */
            n = p < end ? *p++ : 0;
            if (!n) n = 1;
            dev_control(&console_dev, DEV_LOCK);
            log_trace("DT_PHONE_HOME: answer in %d ring", n);
            if (n != 1) log_trace("s");
            log_trace("\n");
            dev_control(&console_dev, DEV_UNLOCK);
            answer_rings = (int16_t)n;
            dev_control(&phone_dev, DEV_POST, PHONE_EV_RESET);
            speak_enabled = 1;
            break;
        case 11:
            log_trace("DT_PHONE_HOME: hangup\n");
            hangup = 1;
            answer_rings = 0;
            dev_control(&phone_dev, DEV_POST, PHONE_EV_HANGUP);
            break;
        case 20:
            log_trace("DT_PHONE_HOME: enable keypad\n");
            if (!phone_offhook) log_error("DT_PHONE_HOME: phone on hook\n");
            else dev_control(&phone_dev, PHONE_KEYPAD_ON);
            break;
        case 21:
            log_trace("DT_PHONE_HOME: disable keypad\n");
            dev_control(&phone_dev, PHONE_KEYPAD_OFF);
            break;
        case 30:                            /* the keypad timeout, n seconds (0 cancels) */
            n = p < end ? *p++ : 0;
            keypad_timeout = (int16_t)(n & 0x7fff);
            keypad_ticks = 0;
            dev_control(&console_dev, DEV_LOCK);
            log_trace("DT_PHONE_HOME: ");
            if (!n) {
                log_trace("cancel timeout\n");
            } else {
                log_trace("timeout in %d second", n);
                if (n != 1) log_trace("s");
                log_trace("\n");
            }
            dev_control(&console_dev, DEV_UNLOCK);
            break;
        case 40:
            log_trace("DT_PHONE_HOME: tone dial %s\n", dcs_text);
            phone_dial(1, dcs_text);
            break;
        case 41:
            log_trace("DT_PHONE_HOME: pulse dial %s\n", dcs_text);
            phone_dial(0, dcs_text);
            break;
        default:
            dt_error_flags |= ERR_DCS;
            log_error("DT_PHONE_HOME: illegal command %d\n", cmd);
            break;
        }
    }
    if (!hangup) send_dcs_reply(70, phone_offhook);
}

/* 0xeb54: dial digits, by tone (tone = 1) or pulse. On hook, the line is seized first and the dialer waits (2 s, then
 * 1 s steps) for the phone task to report off hook. '!' pauses 1 s, '^' flashes the hook 250 ms (each repeat adds
 * the same); other characters are skipped. A pulse digit is n breaks of 60 ms and makes of 40 ms, then 800 ms; a tone
 * digit is one message to the DSP queue, which comes back when the tone is over (160 ms of tone, 60 ms of silence;
 * s15.32). The phone task drops the receiver's echo of the digits while phone_dialing is set. */
void phone_dial(int tone, const char *digits)
{
    int32_t c, d, ticks;
    phone_dialing = 1;
    if (!phone_offhook) {
        dev_control(&phone_dev, PHONE_GO_OFFHOOK);
        ticks = 200;
        for (;;) {
            event_wait(ticks, NULL, 0);
            if (phone_offhook == 1) break;
            ticks = 100;
        }
    }
    for (;;) {
        c = *digits++;
        if (!c) break;
        if (c >= '1' && c <= '9') d = c - '0';
        else if (c == '0') d = 10;
        else if (c == '*') d = 11;
        else if (c == '#') d = 12;
        else if (c >= 'A' && c <= 'D') d = c - 0x34;
        else if (c == '!') {
            for (ticks = 100; *digits == '!'; digits++) ticks += 100;
            event_wait(ticks, NULL, 0);
            continue;
        } else if (c == '^') {
            for (ticks = 25; *digits == '^'; digits++) ticks += 25;
            dev_control(&phone_dev, PHONE_HOOK_RELEASE);
            event_wait(ticks, NULL, 0);
            dev_control(&phone_dev, PHONE_HOOK_SEIZE);
            event_wait(25, NULL, 0);
            continue;
        } else continue;
        if (!tone) {
            if (d >= 11) continue;          /* no pulses for * # A-D */
            for (; d; d--) {
                dev_control(&phone_dev, PHONE_HOOK_RELEASE);
                event_wait(6, NULL, 0);
                dev_control(&phone_dev, PHONE_HOOK_SEIZE);
                event_wait(4, NULL, 0);
            }
            event_wait(80, NULL, 0);
        } else {
            mbox_t back;
            msg_t m;
            mbox_init(&back, NULL);
            m.pad6 = 1;
            m.nwords = 1;
            m.home = &back;
            m.data[0] = (int16_t)dial_tone_high[d - 1];
            m.pad0c = (uint32_t)(int32_t)(int16_t)dial_tone_low[d - 1];    /* sign-extended, as in the ROM */
            mbox_put(dsp_link_queue, &m);
            mbox_get(&back);
        }
    }
    phone_dialing = 0;
}

/* 0xee74: "The code memories were generated on 5-Dec-1983." (the date at the end of that ROM) */
void print_rom_date(const char *name, const char *date)
{
    stream_printf(cur_stream, "The %s memories were generated on ", name);
    stream_printf(cur_stream, "%.2s-%.3s-19%.2s.\n", date, date + 2, date + 5);
}

/* 0xe984: DECTST. 1 restarts the unit (power-up), 5 speaks the test message, 2-4 run the loopback tests of the host
 * line (2 data, 3 control) and the local line (4) through their devices, then set the line up again. */
void dectst_self_test(int32_t test)
{
    dt_error_flags &= ~ERR_DECTST;
    switch (test) {
    case 1:
        emit_sync_marker("self test");
        system_restart();
        /* fall through (system_restart does not return) */
    case 5:
        stream_printf(cur_stream, "\n\x02:np :ra 180\x03");
        stream_printf(cur_stream, "Hello. This is DECtalk.\n");
        stream_printf(cur_stream, "The firmware is version %s.\n", "one point eight");
        print_rom_date("code", rom_date_code);
        print_rom_date("dictionary", rom_date_dict);
        stream_printf(cur_stream, "There are %d bytes free.\n", heap_free_total());
        if (dt_error_flags & ERR_NVR) stream_printf(cur_stream, "There was an NVR fault.\n");
        stream_printf(cur_stream, "If you can here this, there is a good\n");
        stream_printf(cur_stream, "chance that your DECtalk is working.\n");
        stream_flush(cur_stream);
        break;
    case 2:
        if (dev_control(&host_dev, 0x40000)) dt_error_flags |= ERR_DECTST;
        line_configure(0);
        break;
    case 3:
        if (dev_control(&host_dev, 0x50000)) dt_error_flags |= ERR_DECTST;
        break;
    case 4:
        if (dev_control(&console_dev, 0x40000)) dt_error_flags |= ERR_DECTST;
        line_configure(1);
        break;
    default:
        log_error("DECTST: unknown test specified\n");
        dt_error_flags |= ERR_DCS;
        break;
    }
}

/* 0xfa52: DT_STOP: wake the stop task (it syncs the pipeline while klsyn drops what it gets) */
void dt_stop(const char *who)
{
    log_debug("dtstop(%s)\n", who);
    task_resume(stop_task);
}

/* 0x3d56: DT_SYNC: the 0x1A marker into the text pipe, then wait until klsyn has posted everything before it */
void emit_sync_marker(const char *who)
{
    log_debug("dtsync(\"%s\")\n", who);
    stream_putc_to(cur_stream, 0x1a);
    stream_flush(cur_stream);
    sem_wait(&sync_sem);
}
