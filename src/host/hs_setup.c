/* The main task: the local terminal and SETUP (REFERENCE.md s6, s15.3, s15.37), rebuilt in C.
 *
 * After the boot (speech_init) the main task serves the local terminal. With LOCAL EDITED on it reads lines after a
 * ">" prompt with a small line editor, otherwise single characters; LOCAL SPEAK passes the text to the text pipe and
 * LOCAL HOST to the host line. A BREAK (a NUL) or the SET INTERRUPT character enters SETUP: "SETUP>" lines are split
 * into lower-case words and run by tdparse against the command tree below, until EXIT. The session mode (edited or
 * not) is chosen when a session starts: at power-up and on leaving SETUP.
 *
 * With LOCAL SPOKENSETUP on, everything SETUP prints is also spoken (setup_puts, print_help_text, the prompts).
 */
#include <string.h>
#include "host.h"
#include "console.h"

/* ---- the command tree (0x18ab2-0x18d59, the node table 0x18d5a) ----
 * A node is a list of entries; the first entry whose keyword matches the next word is taken, and a NULL keyword is
 * the entry used when the words have run out. An action is a kind (high byte) and an argument (low byte); the kind
 * SETUP_NODE goes on to the node named by the argument. Keywords: the upper-case letters are the abbreviation
 * that must be typed, the lower-case rest may follow (keyword_match). */
static const setup_entry_t node_top[] = {       /* 0x18ab2 */
    {"Exit", SETUP_EXIT, 2}, {"SAve", SETUP_SAVE, 3}, {"Recall", SETUP_NODE | 1, 0}, {"ONline", SETUP_ONLINE, 7},
    {"OFfline", SETUP_OFFLINE, 8}, {"Break", SETUP_BREAK | 0, 9}, {"Lbreak", SETUP_BREAK | 1, 10},
    {"Test", SETUP_NODE | 2, 0}, {"SHow", SETUP_NODE | 3, 0}, {"SEt", SETUP_NODE | 8, 0},
};
static const setup_entry_t node_recall[] = {    /* 0x18b02: the argument is the NVRAM record, 1 = factory */
    {"Factory", SETUP_RECALL | 1, 6}, {"User", SETUP_RECALL | 0, 5}, {NULL, SETUP_RECALL | 0, 4},
};
static const setup_entry_t node_test[] = {      /* 0x18b1a: the argument is the DECTST number */
    {"Power", SETUP_TEST | 1, 12}, {"HData", SETUP_TEST | 2, 13}, {"HControl", SETUP_TEST | 3, 14},
    {"Ldata", SETUP_TEST | 4, 15}, {"Speak", SETUP_TEST | 5, 16}, {NULL, SETUP_HELP_ONLY, 11},
};
static const setup_entry_t node_show[] = {      /* 0x18b4a */
    {"LOG", SETUP_NODE | 5, 0}, {"LOCal", SETUP_NODE | 6, 0}, {"HOst", SETUP_NODE | 7, 0},
    {"Mode", SETUP_NODE | 13, 0}, {"Interrupt", SETUP_SHOW_INTERRUPT, 43}, {"HIstogram", SETUP_SHOW_HISTOGRAM, 0},
    {NULL, SETUP_HELP_ONLY, 17},
};
static const setup_entry_t node_show_log[] = {  /* 0x18b82: the argument is the flag (setup_flags) or the class */
    {"TExt", SETUP_SHOW_FLAG | 0, 19}, {"Phonemes", SETUP_SHOW_FLAG | 1, 20}, {"Rawhost", SETUP_SHOW_FLAG | 2, 21},
    {"Inhost", SETUP_SHOW_FLAG | 3, 22}, {"Outhost", SETUP_SHOW_FLAG | 4, 23}, {"Error", SETUP_SHOW_FLAG | 5, 24},
    {"TRace", SETUP_SHOW_FLAG | 6, 25}, {"Debug", SETUP_SHOW_FLAG | 7, 0}, {NULL, SETUP_SHOW_CLASS | 1, 18},
};
static const setup_entry_t node_show_local[] = {        /* 0x18bca: the argument of SPEED/FORMAT is the line */
    {"SPEEd", SETUP_SHOW_SPEED | 1, 27}, {"Format", SETUP_SHOW_FORMAT | 1, 28}, {"HOst", SETUP_SHOW_FLAG | 8, 29},
    {"SPEAk", SETUP_SHOW_FLAG | 9, 30}, {"Edited", SETUP_SHOW_FLAG | 10, 31}, {"HArdcopy", SETUP_SHOW_FLAG | 11, 32},
    {"SPOkensetup", SETUP_SHOW_FLAG | 12, 33}, {NULL, SETUP_SHOW_CLASS | 2, 26},
};
static const setup_entry_t node_show_host[] = { /* 0x18c0a */
    {"SPEEd", SETUP_SHOW_SPEED | 0, 35}, {"Format", SETUP_SHOW_FORMAT | 0, 36}, {"SPEAk", SETUP_SHOW_FLAG | 13, 37},
    {"Modem", SETUP_SHOW_FLAG | 14, 38}, {NULL, SETUP_SHOW_CLASS | 4, 34},
};
static const setup_entry_t node_set[] = {       /* 0x18c32 */
    {"LOG", SETUP_NODE | 10, 0}, {"LOCal", SETUP_NODE | 11, 0}, {"HOst", SETUP_NODE | 12, 0},
    {"Mode", SETUP_NODE | 14, 0}, {"Interrupt", SETUP_SET_INTERRUPT, 70}, {"HIstogram", SETUP_SET_HISTOGRAM, 0},
    {NULL, SETUP_HELP_ONLY, 44},
};
static const setup_entry_t node_set_log[] = {   /* 0x18c6a */
    {"TExt", SETUP_SET_FLAG | 0, 46}, {"Phonemes", SETUP_SET_FLAG | 1, 47}, {"Rawhost", SETUP_SET_FLAG | 2, 48},
    {"Inhost", SETUP_SET_FLAG | 3, 49}, {"Outhost", SETUP_SET_FLAG | 4, 50}, {"Error", SETUP_SET_FLAG | 5, 51},
    {"TRace", SETUP_SET_FLAG | 6, 52}, {"Debug", SETUP_SET_FLAG | 7, 0}, {NULL, SETUP_HELP_ONLY, 45},
};
static const setup_entry_t node_set_local[] = { /* 0x18cb2 */
    {"SPEEd", SETUP_SET_SPEED | 1, 54}, {"Format", SETUP_SET_FORMAT | 1, 55}, {"HOst", SETUP_SET_FLAG | 8, 56},
    {"SPEAk", SETUP_SET_FLAG | 9, 57}, {"Edited", SETUP_SET_FLAG | 10, 58}, {"HArdcopy", SETUP_SET_FLAG | 11, 59},
    {"SPOkensetup", SETUP_SET_FLAG | 12, 60}, {NULL, SETUP_HELP_ONLY, 53},
};
static const setup_entry_t node_set_host[] = {  /* 0x18cf2 */
    {"SPEEd", SETUP_SET_SPEED | 0, 62}, {"Format", SETUP_SET_FORMAT | 0, 63}, {"SPEAk", SETUP_SET_FLAG | 13, 64},
    {"Modem", SETUP_SET_FLAG | 14, 65}, {NULL, SETUP_HELP_ONLY, 61},
};
static const setup_entry_t node_show_mode[] = { /* 0x18d1a */
    {"SQuare", SETUP_SHOW_FLAG | 15, 40}, {"ASKy", SETUP_SHOW_FLAG | 16, 41}, {"Minus", SETUP_SHOW_FLAG | 17, 42},
    {NULL, SETUP_SHOW_CLASS | 3, 39},
};
static const setup_entry_t node_set_mode[] = {  /* 0x18d3a */
    {"SQuare", SETUP_SET_FLAG | 15, 67}, {"ASKy", SETUP_SET_FLAG | 16, 68}, {"Minus", SETUP_SET_FLAG | 17, 69},
    {NULL, SETUP_HELP_ONLY, 66},
};
#define NODE(n) {(int16_t)(sizeof n / sizeof n[0]), n}
const setup_node_t setup_nodes[15] = {         /* 0x18d5a; nodes 4 and 9 are empty */
    NODE(node_top), NODE(node_recall), NODE(node_test), NODE(node_show), {0, NULL}, NODE(node_show_log),
    NODE(node_show_local), NODE(node_show_host), NODE(node_set), {0, NULL}, NODE(node_set_log), NODE(node_set_local),
    NODE(node_set_host), NODE(node_show_mode), NODE(node_set_mode),
};

/* 0x18db4: the flags SET and SHOW name: the variable (0 host_modem, 1 DT_LOG, 2 DT_TERMINAL, 3 DT_MODE,
 * 4 speak_enabled), the bit, the name SHOW prints */
const setup_flag_t setup_flags[18] = {
    {1, 0x01, "text"}, {1, 0x02, "phoneme"}, {1, 0x04, "rawhost"}, {1, 0x08, "inhost"}, {1, 0x10, "outhost"},
    {1, 0x20, "error"}, {1, 0x40, "trace"}, {1, 0x80, "debug"},
    {2, 0x01, "host"}, {2, 0x02, "speak"}, {2, 0x04, "edited"}, {2, 0x08, "hardcopy"}, {2, 0x10, "spokensetup"},
    {4, 0x01, "Host speak"}, {0, 0x01, "Host modem"},
    {3, 0x01, "square"}, {3, 0x02, "asky"}, {3, 0x04, "minus"},
};

/* 0x18e44: the speed names by speed code (the DUART's baud-rate codes; NULL = not offered) */
const char *const line_speed_names[16] = {
    "75/1200", "110", NULL, "150", "300", "600", "1200", NULL, "2400", "4800", NULL, "9600", NULL, NULL, NULL, NULL,
};
static const char *const line_names[2] = {"Host", "Local"};                    /* 0x18e84 */
static const char *const line_format_names[3] = {"even", "odd", "none"};      /* 0x18e8c */

static char setup_line[0x85];           /* 0x80626: the line read (kept, so ^R can recall it) */
static char setup_words[0x85];          /* 0x806ac: its copy, split into words */

static void console_puts(const char *s)
{
    for (; *s; s++) dev_putc(&console_dev, *s);
}

/* the spoken prompt (LOCAL SPOKENSETUP) */
static void say_prompt(const char *spoken)
{
    if ((dt_terminal & TERM_SPOKENSETUP) && spoken) {
        for (; *spoken; spoken++) stream_putc_to(cur_stream, *spoken);
        stream_flush(cur_stream);
    }
}

/* the console column after a character (the typing path counts a control character, shown as ^X, as one) */
static int32_t next_column(int32_t col, int32_t c)
{
    return c == '\t' ? (col | 7) + 1 : c < 0x20 ? col + 2 : col + 1;
}

/* 0x2a8c: read a line from the local terminal after `prompt` into buf (at most size - 1 characters). Returns what
 * ended it: CR, NUL (BREAK) or the interrupt character. Editing: DEL/BS erase (on a HARDCOPY terminal the erased
 * characters are echoed between backslashes), ^U starts over, ^R shows the line again; before anything is typed ^R
 * recalls the previous line. Control characters echo as ^X. Bit 7 is dropped. */
int32_t read_line_with_prompt(const char *prompt, const char *spoken, char *buf, int32_t size)
{
    char *p = buf, *q;
    int32_t c, col, to;
    int hard = 0, started = 0;
    console_puts(prompt);
    say_prompt(spoken);
    col = (int32_t)strlen(prompt);
    for (;;) {
        c = dev_getc(&console_dev) & 0x7f;
        if (c == 0 || c == setup_interrupt_char || c == '\r') {
            if (hard) dev_putc(&console_dev, '\\');
            dev_putc(&console_dev, '\r');
            dev_putc(&console_dev, '\n');
            *p = 0;
            return c;
        }
        if (c == 0x12) {                                /* ^R */
            if (started) {
                dev_putc(&console_dev, '\r');
                dev_putc(&console_dev, '\n');
                console_puts(prompt);
            } else {
                for (p = buf; *p; p++)                  /* the previous line */
                    ;
                started = 1;
            }
            col = (int32_t)strlen(prompt);
            for (q = buf; q < p;) {
                to = *q++;
                if (to != '\t' && to < 0x20) {
                    dev_putc(&console_dev, '^');
                    to += 0x40;
                }
                dev_putc(&console_dev, to);
                col = next_column(col, to);
            }
            hard = 0;
            continue;
        }
        started = 1;
        if (c == 0x15) {                                /* ^U */
            dev_putc(&console_dev, '\r');
            dev_putc(&console_dev, '\n');
            console_puts(prompt);
            say_prompt(spoken);
            col = (int32_t)strlen(prompt);
            p = buf;
            hard = 0;
            continue;
        }
        if (c == 0x7f || c == 8) {
            if (p == buf) continue;
            c = *--p;
            if (!(dt_terminal & TERM_HARDCOPY)) {
                if (c == '\t') {                        /* back to the column before the tab */
                    to = (int32_t)strlen(prompt);
                    for (q = buf; q < p; q++) to = *q == '\t' ? (to | 7) + 1 : *q < ' ' ? to + 2 : to + 1;
                    for (; to < col; col--) dev_putc(&console_dev, 8);
                } else {
                    if (c < 0x20) {                     /* ^X takes two columns */
                        dev_putc(&console_dev, 8);
                        dev_putc(&console_dev, ' ');
                        dev_putc(&console_dev, 8);
                        col--;
                    }
                    dev_putc(&console_dev, 8);
                    dev_putc(&console_dev, ' ');
                    dev_putc(&console_dev, 8);
                    col--;
                }
            } else {
                if (!hard) {
                    dev_putc(&console_dev, '\\');
                    hard = 1;
                }
                if (c != '\t' && c < 0x20) {
                    dev_putc(&console_dev, '^');
                    c += 0x40;
                }
                dev_putc(&console_dev, c);
            }
            continue;
        }
        if (hard) {
            dev_putc(&console_dev, '\\');
            hard = 0;
        }
        if (p < buf + size - 1) {                       /* a full line takes no more: no echo */
            *p++ = (char)c;
            if (c != '\t' && c < 0x20) {
                dev_putc(&console_dev, '^');
                c += 0x40;
            }
            dev_putc(&console_dev, c);
            col = next_column(col, c);
        }
    }
}

/* 0x300a: SETUP's output: to the local terminal (LF as CR LF), and spoken with LOCAL SPOKENSETUP */
void setup_puts(const char *s)
{
    int32_t c;
    while ((c = *s++) != 0) {
        if (c == '\n') dev_putc(&console_dev, '\r');
        dev_putc(&console_dev, c);
        if (dt_terminal & TERM_SPOKENSETUP) {
            stream_putc_to(cur_stream, c);
            if (c == '\n') stream_flush(cur_stream);
        }
    }
}

/* 0xfd8c: split s at spaces into at most max words, lower-cased in place. Returns the count, or -1 for too many. */
int32_t tokenize_words(char *s, char **argv, int32_t max)
{
    int32_t n = 0, c;
    for (;; s++) {
        for (c = *s; c != ' ';) {
            if (!c) return n;
            if (n >= max) return -1;
            argv[n++] = s;
            do {
                if (c >= 'A' && c <= 'Z') *s = (char)(c + 0x20);   /* isupper(c & 0x7f), tolower */
                c = *++s;
            } while (c && c != ' ');
            *s = 0;
        }
    }
}

/* 0x10844: does word match key? key's upper-case letters must all be typed, then the word may go on with key's
 * lower-case rest (so "SAve" takes "sa", "sav", "save"). */
int keyword_match(const char *word, const char *key)
{
    char k, w;
    while ((k = *key) != 0 && k >= 'A' && k <= 'Z') {
        if (*word != k + 0x20) return 0;
        key++;
        word++;
    }
    do {
        w = *word++;
        if (!w) return 1;
    } while (*key++ == w);
    return 0;
}

/* 0x1088c: a hexadecimal number (SET HISTOGRAM); other characters are taken as c - 0x37, unchecked */
uint32_t parse_hex(const char *s)
{
    uint32_t v = 0;
    int32_t c;
    for (; (c = *s) != 0; s++) {
        if (c >= '0' && c <= '9') c -= '0';
        else if (c >= 'a' && c <= 'f') c -= 'a' - 10;
        else c -= 0x37;
        v = v * 16 + (uint32_t)c;
    }
    return v;
}

/* 0x10744: ON 1, OFF 0, else -1 */
int32_t parse_boolean(const char *s)
{
    return keyword_match(s, "ON") ? 1 : keyword_match(s, "OFf") ? 0 : -1;
}

/* 0x10774: the speed code of a speed name (exact), or -1 */
int32_t parse_baud_rate(const char *s)
{
    int32_t i;
    for (i = 0; i < 16; i++)
        if (line_speed_names[i] && !strcmp(s, line_speed_names[i])) return i;
    return -1;
}

/* 0x107be: EVEN 0, ODD 1, NONE 2, else -1 */
int32_t parse_parity(const char *s)
{
    return keyword_match(s, "Even") ? 0 : keyword_match(s, "Odd") ? 1 : keyword_match(s, "None") ? 2 : -1;
}

/* 0x10714: SET INTERRUPT's value: OFF 0, a single character (lower case, as the words are), ^X for a control
 * character, a lone ^ for itself; anything longer -1 */
int32_t parse_onoff_or_ctrlchar(const char *s)
{
    const char *p;
    int32_t c;
    if (keyword_match(s, "OFf")) return 0;
    c = *s;
    p = s + 1;
    if (c == '^') {
        p = s + 2;
        c = s[1];
        if (!c) return '^';
        if (c >= 'a' && c <= 'z') c -= 0x20;
        c -= 0x40;
    }
    if (*p) c = -1;
    return c;
}

/* 0xfe84: "Host speed is 1200." */
void print_channel_speed(int32_t line)
{
    const char *name = line_speed_names[line_speed[line]];
    setup_puts(line_names[line]);
    setup_puts(" speed is ");
    setup_puts(name ? name : "");       /* a code SET cannot give; the ROM would print from address 0 (a NUL) */
    setup_puts(".\n");
}

/* 0xfee6: "Local format is none." */
void print_channel_format(int32_t line)
{
    setup_puts(line_names[line]);
    setup_puts(" format is ");
    setup_puts(line_format_names[line_format[line]]);
    setup_puts(".\n");
}

/* 0x107ee: "Log text is on." (the host flags have no class word: "Host speak is on.") */
void showbit(int32_t flag)
{
    const setup_flag_t *f = &setup_flags[flag];
    int16_t v = 0;
    switch (f->var) {
    case 0:
        v = host_modem;
        break;
    case 1:
        setup_puts("Log ");
        v = dt_log;
        break;
    case 2:
        setup_puts("Local ");
        v = dt_terminal;
        break;
    case 3:
        setup_puts("Mode ");
        v = dt_mode;
        break;
    case 4:
        v = speak_enabled;
        break;
    default:
        kprintf("Bug: showbit\n");
        panic(NULL);
    }
    setup_puts(f->name);
    setup_puts(" is ");
    setup_puts(f->mask & (uint16_t)v ? "on.\n" : "off.\n");
}

/* 0xff48: a help text. '@' pads to column 15 on the terminal and is spoken as ". "; '/' is spoken as a space.
 * DT_LOG and DT_MODE are off while it is written (the pipe reads it later, with them back on). */
void print_help_text(int32_t n)
{
    int16_t log = dt_log, mode = dt_mode;
    const char *p = setup_help[n];
    int32_t c, col = 0;
    dt_log = 0;
    dt_mode = 0;
    while ((c = *p++) != 0) {
        if (c == '@') {
            for (; col < 15; col++) dev_putc(&console_dev, ' ');
            if (dt_terminal & TERM_SPOKENSETUP) {
                stream_putc_to(cur_stream, '.');
                stream_putc_to(cur_stream, ' ');
            }
        } else if (c == '\n') {
            dev_putc(&console_dev, '\r');
            dev_putc(&console_dev, '\n');
            col = 0;
            if (dt_terminal & TERM_SPOKENSETUP) {
                stream_putc_to(cur_stream, '\n');
                stream_flush(cur_stream);
            }
        } else {
            dev_putc(&console_dev, c);
            col++;
            if (dt_terminal & TERM_SPOKENSETUP) stream_putc_to(cur_stream, c == '/' ? ' ' : c);
        }
    }
    dt_log = log;
    dt_mode = mode;
}

/* 0xfe16: run a SETUP command line (argc words, lower case). Leading HELP words ask for the help text of the entry
 * the rest leads to (a bare HELP: the summary). Returns 0 for EXIT, -1 for a bad line (an unknown word, a missing or
 * bad value, extra words), else the kind of command done. */
int32_t tdparse(int32_t argc, char **argv)
{
    const setup_entry_t *e;
    const setup_flag_t *f;
    int16_t *var = NULL;
    int32_t i = 0, help = 0, node = 0, k, kind, arg, value = 0, c;
    uint32_t lo = 0, hi = 0;
    char one[2];
    const char *s;
    while (i < argc && keyword_match(argv[i], "Help")) {
        i++;
        help++;
    }
    if (help && i == argc) {
        print_help_text(1);
        return SETUP_HELP_ONLY;
    }
    do {
        e = setup_nodes[node].entries;
        for (k = setup_nodes[node].count; k; k--, e++) {
            if (i < argc) {
                if (e->word && keyword_match(argv[i], e->word)) {
                    i++;
                    break;
                }
            } else if (!e->word) {
                break;
            }
        }
        if (!k) return -1;
        arg = e->action & 0xff;
        kind = e->action & 0xff00;
        node = arg;
    } while (kind == SETUP_NODE);
    if (help) {
        print_help_text(e->help);
        return SETUP_HELP_ONLY;
    }
    if (kind == SETUP_SET_HISTOGRAM) {                  /* two hexadecimal addresses */
        if (argc <= i + 1) return -1;
        lo = parse_hex(argv[i]);
        hi = parse_hex(argv[i + 1]);
        i += 2;
    }
    if (kind == SETUP_SET_FLAG || kind == SETUP_SET_FORMAT || kind == SETUP_SET_SPEED || kind == SETUP_SET_INTERRUPT) {
        if (argc <= i) return -1;
        s = argv[i++];
        if (kind == SETUP_SET_FLAG) value = parse_boolean(s);
        else if (kind == SETUP_SET_SPEED) value = parse_baud_rate(s);
        else if (kind == SETUP_SET_FORMAT) value = parse_parity(s);
        else value = parse_onoff_or_ctrlchar(s);
        if (value < 0) return -1;
    }
    if (i < argc) return -1;
    switch (kind) {
    case SETUP_EXIT:
        return 0;
    case SETUP_SAVE:
        nvram_save_settings(arg);
        if (dt_error_flags & ERR_NVR) setup_puts("Failed.\n");
        return kind;
    case SETUP_RECALL:
        settings_reset(2, arg);
        if (dt_error_flags & ERR_NVR) setup_puts("Failed.\n");
        return kind;
    case SETUP_ONLINE:                                  /* LOG RAWHOST only, LOCAL HOST on, SPEAK and EDITED off */
        dt_log = 4;
        dt_terminal = (int16_t)((dt_terminal & ~(TERM_SPEAK | TERM_EDITED)) | TERM_HOST);
        speak_enabled = 1;
        return kind;
    case SETUP_OFFLINE:                                 /* no logging, LOCAL HOST off, SPEAK and EDITED on */
        dt_log = 0;
        dt_terminal = (int16_t)((dt_terminal & ~TERM_HOST) | TERM_SPEAK | TERM_EDITED);
        speak_enabled = 1;
        return kind;
    case SETUP_BREAK:                                   /* the host line's break (device op 3) for 230 ms or 3.5 s */
        dev_control(&host_dev, 0x30001);
        event_wait(arg ? 350 : 23, NULL, 0);
        dev_control(&host_dev, 0x30000);
        return kind;
    case SETUP_TEST:
        dectst_self_test(arg);
        if (dt_error_flags & ERR_DECTST) setup_puts("Failed.\n");
        return kind;
    case SETUP_SHOW_FLAG:
        showbit(arg);
        return kind;
    case SETUP_SHOW_CLASS:                              /* the lines first, then every flag of the class */
        if (arg == 2) {
            print_channel_speed(1);
            print_channel_format(1);
        } else if (arg == 4) {
            print_channel_speed(0);
            print_channel_format(0);
        }
        for (k = 0; k < 18; k++)
            if (setup_flags[k].var == arg) showbit(k);  /* SHOW HOST leaves out MODEM (class 0) */
        return kind;
    case SETUP_SHOW_SPEED:
        print_channel_speed(arg);
        return kind;
    case SETUP_SHOW_FORMAT:
        print_channel_format(arg);
        return kind;
    case SETUP_SET_FLAG:
        f = &setup_flags[arg];
        switch (f->var) {
        case 0:
            var = &host_modem;
            break;
        case 1:
            var = &dt_log;
            break;
        case 2:
            var = &dt_terminal;
            break;
        case 3:
            var = &dt_mode;
            break;
        case 4:
            var = &speak_enabled;
            break;
        default:
            kprintf("Bug: tdparse");
            panic(NULL);
        }
        *var = (int16_t)(*var & ~f->mask);
        if (value) *var = (int16_t)(*var | f->mask);
        if (f->var == 0) dev_control(&host_dev, host_modem + 0x60000);   /* the modem lines (device op 6) */
        return kind;
    case SETUP_SET_SPEED:
        line_speed[arg] = (int16_t)value;
        line_configure(arg);
        return kind;
    case SETUP_SET_FORMAT:
        line_format[arg] = (int16_t)value;
        line_configure(arg);
        return kind;
    case SETUP_SHOW_HISTOGRAM:
        profile_report();
        return kind;
    case SETUP_SET_HISTOGRAM:
        profile_start(lo, hi);
        return kind;
    case SETUP_SHOW_INTERRUPT:
        setup_puts("Interrupt is ");
        c = setup_interrupt_char;
        if (!c) {
            s = "off";
        } else {
            if (c < 0x20) {
                setup_puts("^");
                c += 0x40;
            }
            one[0] = (char)c;
            one[1] = 0;
            s = one;
        }
        setup_puts(s);
        setup_puts(".\n");
        return kind;
    case SETUP_SET_INTERRUPT:
        setup_interrupt_char = (int16_t)value;
        return kind;
    case SETUP_HELP_ONLY:                               /* e.g. a bare TEST or SET: nothing to do */
        return -1;
    default:
        return kind;
    }
}

/* 0x2700: the main task. After the boot it says nothing unless the self-test jumper is open (IP4), then the banner.
 * Rings to answer starts at 1 in stand-alone phone mode. Then the local terminal and SETUP, forever. */
void main_task(void)
{
    char *argv[10];
    const char *p;
    int16_t log, mode;
    int32_t c, n;
    speech_init();
    stream_printf(cur_stream, "\x02:np :ra 180\x03");
    if (duart_input_port() & 0x10) {
        log = dt_log;
        dt_log = 0;
        mode = dt_mode;
        dt_mode = 0;
        stream_printf(cur_stream, "DECtalk version %s is running.\n", "one point eight");
        if (dt_error_flags & ERR_NVR) {
            stream_printf(cur_stream, "NVR fault.\n");
            stream_printf(cur_stream, "Using factory settings.\n");
        }
        dt_log = log;
        dt_mode = mode;
    }
    answer_rings = 0;
    if (phone_standalone) answer_rings = 1;
    for (;;) {
        if (dt_terminal & TERM_EDITED) {                /* lines after ">" */
            while (read_line_with_prompt(">", NULL, setup_line, sizeof setup_line) == '\r') {
                if (dt_terminal & TERM_SPEAK) {
                    if (!setup_line[0]) stream_putc_to(cur_stream, 0x0b);  /* an empty line flushes the clause */
                    else stream_printf(cur_stream, "%s\n", setup_line);
                    stream_flush(cur_stream);
                }
                if (dt_terminal & TERM_HOST) {
                    for (p = setup_line; *p; p++) dev_putc(&host_dev, *p);
                    dev_putc(&host_dev, '\r');
                }
            }
        } else {                                        /* single characters */
            while ((c = dev_getc(&console_dev)) != 0 && c != setup_interrupt_char) {
                if (dt_terminal & TERM_SPEAK) {
                    stream_putc_to(cur_stream, c);
                    stream_flush(cur_stream);
                }
                if (dt_terminal & TERM_HOST) dev_putc(&host_dev, c);
            }
            dev_putc(&console_dev, '\r');
            dev_putc(&console_dev, '\n');
        }
        emit_sync_marker("main");
        for (;;) {
            if (read_line_with_prompt("SETUP>", "setup.\n", setup_line, sizeof setup_line) != '\r') continue;
            strcpy(setup_words, setup_line);
            n = tokenize_words(setup_words, argv, 10);
            if (!n) continue;
            if (n < 0) {
                setup_puts("Too many words on command line.\n");
                continue;
            }
            n = tdparse(n, argv);
            if (n < 0) {
                setup_puts("Bad setup mode command line.\n");
                continue;
            }
            if (!n) break;
        }
        if (dt_terminal & TERM_SPOKENSETUP) {
            stream_printf(cur_stream, "exit.\n");
            stream_flush(cur_stream);
        }
    }
}
