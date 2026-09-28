/* The host task, rebuilt in C (REFERENCE.md s5, s15.6, s15.35): the escape-sequence parser on the host line and
 * the commands it runs (ESC, CSI and DECtalk's DCS commands, DT_PHONE with the dialer, DECTST, the settings and the
 * NVRAM record); and the phone task (hs_phone.c, s15.36). ROM addresses are given for every function and variable so
 * the C stays traceable.
 */
#ifndef HOST_H
#define HOST_H
#include <stddef.h>
#include <stdint.h>
#include "rtos.h"
#include "stream.h"

/* ---- the parser state (0x81f2c): one escape sequence being collected, also the layout of a reply ---- */
typedef struct {
    int16_t lead;           /* +0x00: 0 = none, ESC 0x1b, CSI 0x9b or DCS 0x90 */
    char final;             /* +0x02: the final character */
    int8_t priv;            /* +0x03: private marker '<' '=' '>' '?' as 4 3 2 1 ('@' - c); -1 = malformed */
    uint16_t p[17];         /* +0x04: p[0] = the parameter count (at most 16), p[1..16] = the parameters */
    int8_t nint;            /* +0x26: intermediates (0x20-0x2f) seen, 3 = too many */
    char inter[2];          /* +0x27 */
} host_seq_t;

extern host_seq_t host_seq;             /* 0x81f2c */
extern int16_t host_last_char;          /* 0x81f64: the last code read_host_char_collect_csi returned */
extern char dcs_text[256];              /* 0x81f66: the text after 'z' (DT_DICT, DT_PHONE) */
extern char *dcs_text_ptr;              /* 0x82066: its write pointer; == dcs_text + 256 when full */
extern int16_t phone_nparams;           /* 0x8206a: DT_PHONE's parameter count */
extern int16_t phone_params[16];        /* 0x8206c: its parameters P1.. (P3 on are the sub-commands) */

/* ---- the settings the host side keeps (0x822ca-0x82305); the first four are the NVRAM record's words 1-3 ---- */
extern int16_t dt_log;                  /* 0x822ca: DT_LOG (defined with the speech side, which reads it) */
extern int16_t dt_terminal;             /* 0x822cc: DT_TERMINAL */
extern int16_t dt_mode;                 /* 0x822ce: DT_MODE (defined with the speech side) */
extern int16_t host_modem;              /* 0x822d0: SET HOST MODEM (bit 0), the host line's device op 6 (record word 12) */
extern int16_t speak_enabled;           /* 0x822d2: DT_SPEAK; 0 = host text is not passed on */
extern int8_t phone_standalone;         /* 0x822d4: stand-alone phone mode, until the host sends a byte (s15.34) */
extern int16_t phone_offhook;           /* 0x822d6: the phone task's hook state (1 = off hook) */
extern int16_t keypad_timeout;          /* 0x822d8: DT_PHONE 30: the keypad timeout in seconds */
extern int16_t answer_rings;            /* 0x822da: DT_PHONE 10: answer after this many rings (0 = no) */
extern int16_t line_speed[2];           /* 0x822dc: speed code of the host line [0] and the local line [1] */
extern int16_t line_format[2];          /* 0x822e0: format code (parity) of each line */
extern int16_t setup_interrupt_char;    /* 0x822e4: SET INTERRUPT: the local character that enters SETUP (word 10) */
extern int8_t c1_receive_7bit;          /* 0x822e6: DECTC1 (1) / DECAC1 (0): strip bit 7 of received bytes */
extern int8_t c1_transmit_7bit;         /* 0x822e8: S7C1T (1) / S8C1T (0): send C1 codes as ESC + letter */
extern int32_t single_shift;            /* 0x822ea: 2 or 3 after SS2/SS3, for the next graphic character */
extern int32_t gl_set, gr_set;          /* 0x822ee, 0x822f2: the character sets in GL and GR */
extern int32_t g_set[4];                /* 0x822f6: G0-G3; 'B' = ASCII, '<' = DEC supplemental */

/* ---- shared with the other tasks ---- */
extern int8_t phone_dialing;            /* 0x81d6c: phone_dial is running (the phone task drops the echo) */
extern int16_t keypad_ticks;            /* 0x81d6e: the phone task's keypad timer, restarted by DT_PHONE 30 */
extern int16_t last_index;              /* 0x81d70: the last index mark spoken (klsyn) */
extern int16_t host_idle;               /* 0x81d72: seconds without host input (host timeout task) */
extern int16_t dt_error_flags;          /* 0x81f12: DSR errors 22-27 as bits 0-5 */
extern stream_t *cur_stream;            /* 0x81f1e: the writing end of the text pipe (to dttask) */
extern ksem_t sync_sem;                 /* 0x81f26: DT_SYNC waits here until klsyn has the text */
extern void *stop_task;                 /* 0x80622: the stop task, resumed by DT_STOP */
extern mbox_t *dsp_link_queue;          /* 0x81cc8: the DSP command queue (the dialer's tones) */
extern chardev_t host_dev, phone_dev;   /* 0x8011e, 0x80552; console_dev is in rtos.h */
extern uint8_t nvram[0x202];            /* 0x94000: the X2212, one nibble in each even byte; 0x200 = store */

/* error bits */
#define ERR_NVR 0x04                    /* 24: the last NVRAM operation failed */
#define ERR_DCS 0x10                    /* 26: a bad DCS sequence */
#define ERR_DECTST 0x20                 /* 27: the last DECTST failed */

/* device operations (dev_control): the op in the high word, an argument in the low word */
#define DEV_LOCK (-0xffff)              /* the console's output lock */
#define DEV_UNLOCK (-0x1ffff)
#define DEV_RX_TIMER (-0x60000)         /* input timer: a waiting dev_getc gets DEV_TIMEOUT after n ticks (3rd arg) */
#define DEV_RX_TIMER_OFF (-0x70000)
#define DEV_POST (-0x80000)             /* hand the device's reader a value (the third argument; dev_rx_post) */
#define DEV_TIMEOUT 0x80000             /* what dev_getc returns when the input timer runs out */
#define PHONE_KEYPAD_ON 0x20000         /* phone_dev ops 2-5, 7-9 (s15.34) */
#define PHONE_KEYPAD_OFF 0x30000
#define PHONE_HOOK_RELEASE 0x40000
#define PHONE_HOOK_SEIZE 0x50000
#define PHONE_SET_IDLE 0x70000          /* on hook, wait for a ring */
#define PHONE_GO_OFFHOOK 0x80000        /* off hook (to dial); the task then gets PHONE_EV_OFFHOOK */
#define PHONE_START_ANSWER 0x90000      /* count rings (3rd arg), then off hook */
#define PHONE_EV_RING 0x81              /* events for the phone task, besides the key codes 0-15 */
#define PHONE_EV_OFFHOOK 0x82
#define PHONE_EV_RESET 0x84
#define PHONE_EV_HANGUP 0x85
#define PHONE_EV_RING_STOPPED 0x86

/* ---- the functions ---- */
void host_task_main(void);                                              /* 0xd59e */
int32_t read_host_byte(void);                                           /* 0xefc8 */
int32_t read_host_char_collect_csi(host_seq_t *s, int32_t (*getc)(void), int strip8);   /* 0x11290 */
int32_t handle_single_shift(int32_t n);                                 /* 0xd83e */
int32_t consume_control_string(void (*put)(int32_t c));                /* 0xd9f4 */
int is_graphic_char(int32_t c);                                         /* 0xd906 */
void output_graphic_char(int32_t c, int32_t set);                       /* 0xd94c */
int32_t dispatch_esc_command(void);                                     /* 0xdad8 */
int32_t csi_command_dispatch(void);                                     /* 0xddd8 */
void dsr_reply(int extended);                                           /* 0xdff2 */
int32_t dcs_command_dispatch(void);                                     /* 0xe152 */
void dcs_text_putc(int32_t c);                                          /* 0xeecc */
int parse_dict_entry_command(char *text);                               /* 0xe8ec */
void dt_phone_command(void);                                            /* 0xe5f6 */
void phone_dial(int tone, const char *digits);                          /* 0xeb54 */
void dectst_self_test(int32_t test);                                    /* 0xe984 */
void print_rom_date(const char *name, const char *date);                /* 0xee74 */
void dt_stop(const char *who);                                          /* 0xfa52 */
void emit_sync_marker(const char *who);                                 /* 0x3d56 */
int settings_reset(int mode, int which);                                /* 0xf3be (formerly spdef_settings_io) */
void line_configure(int line);                                          /* 0xf5c6 */
int nvram_load_settings(int16_t rec[64], int which);                    /* 0xf69a */
int nvram_save_settings(int which);                                     /* 0xf79c */
uint16_t nvram_checksum(const int16_t *w, int n);                       /* 0xf7ca */
void send_control_sequence(int32_t c, const host_seq_t *s, void (*put)(int32_t c), int seven_bit);  /* 0x114e0 */
void send_escape_response(int32_t c, const host_seq_t *s);             /* 0xef5c */
void send_dcs_reply(int16_t r2, int16_t r3);                            /* 0xef00 */
void host_line_putc(int32_t c);                                         /* 0xef90 (formerly echo_digit_to_host) */
void uint_to_decimal_string(char *out, uint32_t v);                     /* 0x1167c */
void log_error(const char *fmt, ...);                                   /* 0xd1ca: DT_LOG 0x20 */
void log_trace(const char *fmt, ...);                                   /* 0xd21e: DT_LOG 0x40 */
void log_debug(const char *fmt, ...);                                   /* 0xd1f4: DT_LOG 0x80 */
void host_timeout_task_main(void);                                      /* 0xf070 (hs_tasks.c) */
void stop_task_main(void);                                              /* 0xfa7a (hs_tasks.c) */
extern int16_t stop_pending;            /* 0x81bec: DT_STOP in progress (defined with the speech side, klsyn reads it) */
void phtask_main(void);                                                 /* 0xf128: the phone task (hs_phone.c) */
void dtmf_diagnostic_menu(void);                                        /* 0x116e2 */

/* ---- the main task: the local terminal and SETUP (hs_setup.c, s6, s15.37) ---- */
#define TERM_HOST 0x01                  /* DT_TERMINAL = the LOCAL flags: typed text goes to the host line */
#define TERM_SPEAK 0x02                 /* ... and is spoken */
#define TERM_EDITED 0x04                /* lines with the line editor (else single characters) */
#define TERM_HARDCOPY 0x08              /* erased characters are echoed, not rubbed out */
#define TERM_SPOKENSETUP 0x10           /* SETUP's output is spoken too */

/* command kinds (the high byte of an action; the low byte is its argument) */
#define SETUP_EXIT 0x0000
#define SETUP_NODE 0x0100               /* go on to node <arg> */
#define SETUP_SAVE 0x0200
#define SETUP_RECALL 0x0300             /* <arg>: 0 user, 1 factory */
#define SETUP_ONLINE 0x0400
#define SETUP_OFFLINE 0x0500
#define SETUP_BREAK 0x0600              /* <arg>: 0 short, 1 long */
#define SETUP_TEST 0x0700               /* <arg>: the DECTST number */
#define SETUP_SHOW_FLAG 0x0800          /* <arg>: setup_flags[] */
#define SETUP_SHOW_CLASS 0x0900         /* <arg>: a class (1 log, 2 local, 3 mode, 4 host) */
#define SETUP_SHOW_SPEED 0x0a00         /* <arg>: the line, 0 host, 1 local */
#define SETUP_SHOW_FORMAT 0x0b00
#define SETUP_SET_FLAG 0x0c00
#define SETUP_SET_SPEED 0x0d00
#define SETUP_SET_FORMAT 0x0e00
#define SETUP_SHOW_HISTOGRAM 0x0f00
#define SETUP_SET_HISTOGRAM 0x1000
#define SETUP_SHOW_INTERRUPT 0x1100
#define SETUP_SET_INTERRUPT 0x1200
#define SETUP_HELP_ONLY 0x1300          /* an entry that only has help text; tdparse also returns it after HELP */

typedef struct {
    const char *word;                   /* the keyword (NULL: taken when the words have run out) */
    uint16_t action;                    /* kind | argument */
    int16_t help;                       /* its help text (setup_help[]) */
} setup_entry_t;                        /* 8 bytes in the ROM */
typedef struct {
    int16_t count;
    const setup_entry_t *entries;
} setup_node_t;                         /* 6 bytes in the ROM */
typedef struct {
    int16_t var;                        /* 0 host_modem, 1 DT_LOG, 2 DT_TERMINAL, 3 DT_MODE, 4 speak_enabled */
    uint16_t mask;
    const char *name;
} setup_flag_t;                         /* 8 bytes in the ROM */

#define SETUP_NHELP 71
extern const setup_node_t setup_nodes[15];      /* 0x18d5a */
extern const setup_flag_t setup_flags[18];      /* 0x18db4 */
extern const char *const line_speed_names[16];  /* 0x18e44 */
extern const char *const setup_help[SETUP_NHELP];   /* 0x19c04 by the offsets at 0x1d2c4 (hs_help.c) */

void main_task(void);                                                   /* 0x2700 */
int32_t read_line_with_prompt(const char *prompt, const char *spoken, char *buf, int32_t size);   /* 0x2a8c */
void setup_puts(const char *s);                                         /* 0x300a */
int32_t tokenize_words(char *s, char **argv, int32_t max);              /* 0xfd8c */
int32_t tdparse(int32_t argc, char **argv);                             /* 0xfe16 */
void print_channel_speed(int32_t line);                                 /* 0xfe84 */
void print_channel_format(int32_t line);                                /* 0xfee6 */
void print_help_text(int32_t n);                                        /* 0xff48 */
int32_t parse_onoff_or_ctrlchar(const char *s);                         /* 0x10714 */
int32_t parse_boolean(const char *s);                                   /* 0x10744 */
int32_t parse_baud_rate(const char *s);                                 /* 0x10774 */
int32_t parse_parity(const char *s);                                    /* 0x107be */
void showbit(int32_t flag);                                             /* 0x107ee */
int keyword_match(const char *word, const char *key);                   /* 0x10844 */
uint32_t parse_hex(const char *s);                                      /* 0x1088c */

/* outside the host side: the boot, the board, the kernel's profiler */
void speech_init(void);                 /* 0x30c8: settings_reset(3), the DSP link, the other tasks, the voice */
uint8_t duart_input_port(void);         /* 0x9801b: the DUART's input port; IP4 (0x10) = the self-test jumper open */
void profile_start(uint32_t lo, uint32_t hi);   /* 0x11aaa: PC sampling over [lo, hi) from the clock interrupt */
void profile_report(void);                      /* 0x11b9e: prints the samples on the console */

/* outside the host side: the user dictionary (speech, tx_dict.c; DT_DICT fills it, RIS empties it) */
typedef struct dict_entry dict_entry_t;
void dict_hash_clear_all(void);                                         /* 0xfb10 */
dict_entry_t *dict_hash_insert_or_delete(const char *name, const char *subst);  /* 0xfc36: NULL = no room */

/* ROM data (hs_rom.c) */
extern const char dec_supplemental_ascii[96];  /* 0x1823e: 0xa0-0xff to the ASCII base letter, 0 = none */
extern const host_seq_t da_reply;               /* 0x18304: ESC [ ? 19 c */
extern const uint16_t dial_tone_high[16];        /* 0x1829e: DSP tone command, column frequency, per digit code */
extern const uint16_t dial_tone_low[16];        /* 0x182be: row frequency */
extern const int16_t factory_settings[64];      /* 0x189da: the factory NVRAM record (version 4) */
extern const int16_t line_format_codes[3];      /* 0x18a5a: DUART format word per parity setting */
extern const char rom_date_code[8];             /* 0x1fff0: the program ROM's date, "05Dec83" */
extern const char rom_date_dict[8];             /* 0x3fff0: the dictionary ROM's date, "11Oct83" */

#endif
