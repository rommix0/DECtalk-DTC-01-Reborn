/* SETUP's help texts (REFERENCE.md s6, s15.37), extracted from the ROM: 71 NUL-terminated texts at 0x19c04,
 * addressed by the word offsets at 0x1d2c4 (0 = "No help text for this command.", 1 = the summary for a bare HELP;
 * the command tree gives each entry its text). In a text, '@' pads the console line to column 15 (". " when spoken)
 * and '/' is spoken as a space (print_help_text, hs_setup.c).
 */
#include "host.h"

const char *const setup_help[SETUP_NHELP] = {
    /* 0 */
    "No help text for this command.\n",
    /* 1 */
    "SUMMARY.\n"
    "The following commands can be entered in SETUP mode.\n"
    "More information about a specific command may be obtained by typing\n"
    "HELP, followed by the command line.\n"
    "EXIT@Leave SETUP mode.\n"
    "SAVE@Save the state of DECtalk into the non volatile memory.\n"
    "RECALL@Recall the state of DECtalk from the non volatile memory.\n"
    "ONLINE@Configure DECtalk for operation with a host computer.\n"
    "OFFLINE@Configure DECtalk for operation with only a local terminal.\n"
    "BREAK@Send a short break to the host.\n"
    "LBREAK@Send a long break to the host.\n"
    "TEST@Run DECtalk internal diagnostics.\n"
    "SHOW@Show DECtalk parameters.\n"
    "SET@Set DECtalk parameters.\n",
    /* 2 */
    "EXIT.\n"
    "DECtalk leaves SETUP mode, and begins processing text in accordance\n"
    "with the currently specified options.\n",
    /* 3 */
    "SAVE.\n"
    "Save the state of DECtalk into the USER non volatile memory.\n",
    /* 4 */
    "RECALL memory.\n"
    "Recall the state of DECtalk from the specified non volatile memory.\n"
    "The \"memory\" is either USER, which specifies the memory used by the SAVE\n"
    "command, or FACTORY, which specifies the factory default settings. If\n"
    "no \"memory\" is specified, the USER memory is used.\n",
    /* 5 */
    "RECALL USER.\n"
    "Recall the state of DECtalk from the USER non volatile memory.\n",
    /* 6 */
    "RECALL FACTORY.\n"
    "Recall the state of DECtalk from the FACTORY non volatile memory.\n",
    /* 7 */
    "ONLINE.\n"
    "DECtalk is configured for operation with a host computer. All characters\n"
    "typed on the local terminal are sent to the host. All characters received\n"
    "from the host are sent to the local terminal.\n",
    /* 8 */
    "OFFLINE.\n"
    "DECtalk is configured for operation with only a local terminal. Lines\n"
    "of text typed on the local terminal will be spoken. \n",
    /* 9 */
    "BREAK.\n"
    "A short break (approximately 230 msec. long) is sent to the host.\n",
    /* 10 */
    "LBREAK.\n"
    "A long break (approximately 3.5 seconds long) is sent to the host.\n",
    /* 11 */
    "TEST name.\n"
    "Run the DECtalk internal diagnostic specified by \"name\". The following\n"
    "tests may be specified:\n"
    "POWER@Simulate a power-up reset.\n"
    "HDATA@Host port data loopback test. Requires loopback connector.\n"
    "HCONTROL@Host port control loopback test. Requires loopback connector.\n"
    "LDATA@Local line data loopback test. Requires loopback connector.\n"
    "SPEAK@Speak a canned message.\n",
    /* 12 */
    "TEST POWER.\n"
    "DECtalk will simulate a power-up reset.\n",
    /* 13 */
    "TEST HDATA.\n"
    "DECtalk will run a data loopback test on the host port. A loopback connector\n"
    "is required on the port.\n",
    /* 14 */
    "TEST HCONTROL.\n"
    "DECtalk will run a control signal loopback test on the host port. A loopback\n"
    "connector is required on the port.\n",
    /* 15 */
    "TEST LDATA.\n"
    "DECtalk will (attempt) to run a data loopback test on the local port. Since\n"
    "there is a terminal (rather than a loopback connector) on the port, this test\n"
    "will fail when run from SETUP mode.\n",
    /* 16 */
    "TEST SPEAK.\n"
    "DECtalk will speak a canned message.\n",
    /* 17 */
    "SHOW class parameter.\n"
    "The SHOW command displays DECtalk internal parameters. The \"class\"\n"
    "specifies which group of parameters is being referenced. If no \"parameter\"\n"
    "is specified then all parameters in the class are displayed. The following\n"
    "\"classes\" can be specified:\n"
    "LOG@Logging flags, for debugging.\n"
    "LOCAL@Local terminal handling options.\n"
    "HOST@Host line handling options.\n"
    "MODE@Text-to-speech processing options.\n"
    "INTERRUPT@Characters that force entry to SETUP mode.\n",
    /* 18 */
    "SHOW LOG flag.\n"
    "The SHOW LOG command displays DECtalk logging parameters. If no \"flag\" is\n"
    "specified than all logging flags are displayed. The following \"flags\" can\n"
    "be specified:\n"
    "TEXT@Text input to text-to-speech.\n"
    "PHONEMES@Phonemic text input to synthesizer.\n"
    "RAWHOST@Text received from the host line.\n"
    "INHOST@Text received from the host line, slightly interpreted.\n"
    "OUTHOST@Text sent to the host line, slightly interpreted.\n"
    "ERROR@Error messages.\n"
    "TRACE@Trace of DECtalk escape sequences.\n",
    /* 19 */
    "SHOW LOG TEXT.\n"
    "Display the TEXT logging flag. If ON, all text input to text-to-speech\n"
    "is logged on the local terminal.\n",
    /* 20 */
    "SHOW LOG PHONEMES.\n"
    "Display the PHONEME logging flag. If ON, all phonemic text input to the\n"
    "synthesizer is logged on the local terminal.\n",
    /* 21 */
    "SHOW LOG RAWHOST.\n"
    "Display the RAWHOST logging flag. If ON, all characters received from\n"
    "the host line are logged on the local terminal.\n",
    /* 22 */
    "SHOW LOG INHOST.\n"
    "Display the INHOST logging flag. If ON, all characters received from the\n"
    "host line are logged on the local terminal. Some interpretation is done,\n"
    "to make control characters visable and to prevent escape sequences from\n"
    "being interpreted by the logging device.\n",
    /* 23 */
    "SHOW LOG OUTHOST.\n"
    "Display the OUTHOST logging flag. If ON, all characters sent to the host\n"
    "line by DECtalk are logged on the local terminal. Some interpretation is\n"
    "done, to make control characters visable and to prevent escape sequences\n"
    "from being interpreted by the logging device.\n",
    /* 24 */
    "SHOW LOG ERROR.\n"
    "Display the ERROR logging flag. If ON, DECtalk will display error messages\n"
    "on the local terminal.\n",
    /* 25 */
    "SHOW LOG TRACE.\n"
    "Display the TRACE logging flag. If ON, DECtalk will print, on the local\n"
    "terminal, a trace of all escape sequences, received from the host line,\n"
    "that affect its operation. Character set selection sequences are not\n"
    "logged, to reduce clutter on the local terminal.\n",
    /* 26 */
    "SHOW LOCAL parameter.\n"
    "The SHOW LOCAL command displays local terminal parameters. If no \"parameter\"\n"
    "is specified then all parameters are displayed. The following parameters can\n"
    "be specified:\n"
    "SPEED@Local line speed.\n"
    "FORMAT@Local line data format.\n"
    "HOST@If ON, send text to host line.\n"
    "SPEAK@If ON, speak local terminal characters.\n"
    "EDITED@If ON, enable line editing.\n"
    "HARDCOPY@If ON, use hardcopy terminal editing conventions.\n"
    "SPOKENSETUP@If ON, SETUP mode is spoken.\n",
    /* 27 */
    "SHOW LOCAL SPEED.\n"
    "Display the speed of the local terminal line.\n",
    /* 28 */
    "SHOW LOCAL FORMAT.\n"
    "Display the format of the local terminal line. The format is one of EVEN\n"
    "(7 bits, even parity), ODD (7 bits, odd parity) or NONE (8 bits, no parity).\n",
    /* 29 */
    "SHOW LOCAL HOST.\n"
    "Display the HOST local terminal flag. If ON, all characters typed on the\n"
    "local terminal are sent to the host line.\n",
    /* 30 */
    "SHOW LOCAL SPEAK.\n"
    "Display the SPEAK local terminal flag. If ON, all characters typed on the\n"
    "local terminal are spoken.\n",
    /* 31 */
    "SHOW LOCAL EDITED.\n"
    "Display the EDITED local terminal flag. If ON, all local terminal input is\n"
    "processed line at a time. No characters are sent to DECtalk until a carriage\n"
    "return is typed.\n",
    /* 32 */
    "SHOW LOCAL HARDCOPY.\n"
    "Display the HARDCOPY local terminal flag. If ON, all local terminal echo\n"
    "is performed in a way that is suited to hardcopy terminals. If OFF, all local\n"
    "terminal echo is performed in a way that is suited to video terminals.\n",
    /* 33 */
    "SHOW LOCAL SPOKENSETUP.\n"
    "Display the SPOKENSETUP local terminal flag. If ON, SETUP mode is spoken.\n",
    /* 34 */
    "SHOW HOST parameter.\n"
    "The SHOW HOST command displays host line parameters. If no \"parameter\" is\n"
    "specified then all host line parameters are specified. The following parameters\n"
    "can be specified:\n"
    "SPEED@Host line speed.\n"
    "FORMAT@Host line data format.\n"
    "SPEAK@If ON, speak received text.\n"
    "MODEM@If ON, perform full modem control.\n",
    /* 35 */
    "SHOW HOST SPEED.\n"
    "Display the speed of the host line.\n",
    /* 36 */
    "SHOW HOST FORMAT.\n"
    "Display the format of the host terminal line. The format is one of EVEN\n"
    "(7 bits, even parity), ODD (7 bits, odd parity) or NONE (8 bits, no parity).\n",
    /* 37 */
    "SHOW HOST SPEAK.\n"
    "Display the SPEAK host flag. If ON, all text received from the host line\n"
    "is spoken.\n",
    /* 38 */
    "SHOW HOST MODEM.\n"
    "Display the MODEM host flag. If ON, the host port performs full modem \n"
    "control. If OFF, the host port is data leads only.\n",
    /* 39 */
    "SHOW MODE parameter.\n"
    "The SHOW MODE command displays text-to-speech processing modes. If no\n"
    "\"parameter\" is specified, then all parameters are displayed. The following\n"
    "parameters can be specified:\n"
    "SQUARE@If ON, the \"[\" and \"]\" characters are phonemic delimiters.\n"
    "ASKY@If ON, the ASKY  phonemic alphabet is used.\n"
    "MINUS@If ON, the \"-\" character is pronounced as \"minus\".\n",
    /* 40 */
    "SHOW MODE SQUARE.\n"
    "Display the SQUARE mode flag. If ON, the \"[\" and \"]\" characters are\n"
    "phonemic brackets. If OFF, they are ordinary characters.\n",
    /* 41 */
    "SHOW MODE ASKY.\n"
    "Display the ASKY mode flag. If ON, the ASKY phonemic alphabet is used.\n"
    "If OFF, the ARPABET phonemic alphabet is used.\n",
    /* 42 */
    "SHOW MODE MINUS.\n"
    "Display the MINUS mode flag. If ON, the \"-\" character is pronounced as\n"
    "\"minus\". If OFF, the \"-\" character is pronounced as \"dash\".\n",
    /* 43 */
    "SHOW INTERRUPT.\n"
    "Display the character used to enter SETUP mode. If the character is\n"
    "displayed as OFF then only the BREAK key enters SETUP mode. If the interrupt\n"
    "character is a control character it is displayed with a leading ^; a\n"
    "CONTROL/C is displayed as ^C.\n",
    /* 44 */
    "SET class parameter value.\n"
    "The SET command sets DECtalk internal parameters. The \"class\" specifies\n"
    "which group of parameters are being set. The \"parameter\" specifies the\n"
    "particular parameter in the group. The \"value\" is the new value for the\n"
    "parameter. The following classes can be specified:\n"
    "LOG@Logging flags, for debugging.\n"
    "LOCAL@Local terminal handling options.\n"
    "HOST@Host line handling options.\n"
    "MODE@Text-to-speech processing options.\n"
    "INTERRUPT@Characters that force entry to SETUP mode.\n",
    /* 45 */
    "SET LOG flag value.\n"
    "The SET LOG command sets DECtalk logging flags. The following flags can\n"
    "be specified:\n"
    "TEXT@Text input to text-to-speech.\n"
    "PHONEMES@Phonemic text input to synthesizer.\n"
    "RAWHOST@Text received from the host line.\n"
    "INHOST@Text received from the host line, slightly interpreted.\n"
    "OUTHOST@Text sent to the host line, slightly interpreted.\n"
    "ERROR@Error messages.\n"
    "TRACE@Trace of DECtalk escape sequences.\n",
    /* 46 */
    "SET LOG TEXT ON/OFF.\n"
    "Set the TEXT logging flag. If ON, all text input to text-to-speech\n"
    "is logged on the local terminal.\n",
    /* 47 */
    "SET LOG PHONEMES ON/OFF.\n"
    "Set the PHONEME logging flag. If ON, all phonemic text input to the\n"
    "synthesizer is logged on the local terminal.\n",
    /* 48 */
    "SET LOG RAWHOST ON/OFF.\n"
    "Set the RAWHOST logging flag. If ON, all characters received from\n"
    "the host line are logged on the local terminal.\n",
    /* 49 */
    "SET LOG INHOST ON/OFF.\n"
    "Set the INHOST logging flag. If ON, all characters received from the\n"
    "host line are logged on the local terminal. Some interpretation is done\n"
    "to make control characters visable and to prevent escape sequences from\n"
    "being interpreted by the logging device.\n",
    /* 50 */
    "SET LOG OUTHOST ON/OFF.\n"
    "Set the OUTHOST logging flag. If ON, all characters sent to the host\n"
    "line by DECtalk are logged on the local terminal. Some interpretation is\n"
    "done, to make control characters visable and to prevent escape sequences\n"
    "from being interpreted by the logging device.\n",
    /* 51 */
    "SET LOG ERROR ON/OFF.\n"
    "Set the ERROR logging flag. If ON, DECtalk will display error messages\n"
    "on the local terminal.\n",
    /* 52 */
    "SET LOG TRACE ON/OFF.\n"
    "Set the TRACE logging flag. If ON, DECtalk will print, on the local\n"
    "terminal, a trace of all escape sequences, received from the host line,\n"
    "that affect its operation. Character set selection sequences are not\n"
    "logged, to reduce clutter on the local terminal.\n",
    /* 53 */
    "SET LOCAL parameter value.\n"
    "The SET LOCAL command sets local line parameters. The following parameters\n"
    "can be specified:\n"
    "SPEED@Local line speed.\n"
    "FORMAT@Local line data format.\n"
    "HOST@If ON, send text to host line.\n"
    "SPEAK@If ON, speak local terminal characters.\n"
    "EDITED@If ON, enable line editing.\n"
    "HARDCOPY@If ON, use hardcopy terminal editing conventions.\n"
    "SPOKENSETUP@If ON, SETUP mode is spoken.\n",
    /* 54 */
    "SET LOCAL SPEED speed.\n"
    "Set the speed of the local line. Only 75/1200, 110, 150, 300, 600, 1200, \n"
    "4800, and 9600 baud can be specified. Two stop bits are automatically used\n"
    "at 75/1200 and 110 baud.\n",
    /* 55 */
    "SET LOCAL FORMAT format.\n"
    "Set the format of the local terminal line. The format is one of EVEN\n"
    "(7 bits, even parity), ODD (7 bits, odd parity) or NONE (8 bits, no parity).\n",
    /* 56 */
    "SET LOCAL HOST ON/OFF.\n"
    "Set the HOST local terminal flag. If ON, all characters typed on the\n"
    "local terminal are sent to the host line.\n",
    /* 57 */
    "SET LOCAL SPEAK ON/OFF.\n"
    "Set the SPEAK local terminal flag. If ON, all characters typed on the\n"
    "local terminal are spoken.\n",
    /* 58 */
    "SET LOCAL EDITED ON/OFF.\n"
    "Set the EDITED local terminal flag. If ON, all local terminal input is\n"
    "processed line at a time. No characters are sent to DECtalk until a carriage\n"
    "return is typed.\n",
    /* 59 */
    "SET LOCAL HARDCOPY ON/OFF.\n"
    "Set the HARDCOPY local terminal flag. If ON, all local terminal echo\n"
    "is performed in a way that is suited to hardcopy terminals. If OFF, all local\n"
    "terminal echo is performed in a way that is suited to video terminals.\n",
    /* 60 */
    "SET LOCAL SPOKENSETUP ON/OFF.\n"
    "Set the SPOKENSETUP local terminal flag. If ON, SETUP mode is spoken.\n",
    /* 61 */
    "SET HOST parameter value.\n"
    "The SET HOST command sets host line parameters. The following parameters \n"
    "can be specified:\n"
    "SPEED@Host line speed.\n"
    "FORMAT@Host line data format.\n"
    "SPEAK@If ON, speak received text.\n"
    "MODEM@If ON, perform full modem control.\n",
    /* 62 */
    "SET HOST SPEED speed.\n"
    "Set the speed of the host line. Only 75/1200, 110, 150, 300, 600, 1200, \n"
    "4800, and 9600 baud can be specified. Two stop bits are automatically used\n"
    "at 75/1200 and 110 baud.\n",
    /* 63 */
    "SET HOST FORMAT format.\n"
    "Set the format of the host terminal line. The format is one of EVEN\n"
    "(7 bits, even parity), ODD (7 bits, odd parity) or NONE (8 bits, no parity).\n",
    /* 64 */
    "SET HOST SPEAK ON/OFF.\n"
    "Set the SPEAK host flag. If ON, all text received from the host line\n"
    "is spoken.\n",
    /* 65 */
    "SET HOST MODEM ON/OFF.\n"
    "Set the MODEM host flag. If ON, the host port performs full modem \n"
    "control. If OFF, the host port is data leads only.\n",
    /* 66 */
    "SET MODE flag value.\n"
    "The SET MODE command sets text-to-speech processing modes. The following\n"
    "flags can be specified:\n"
    "SQUARE@If ON, the \"[\" and \"]\" characters are phonemic delimiters.\n"
    "ASKY@If ON, the ASKY  phonemic alphabet is used.\n"
    "MINUS@If ON, the \"-\" character is pronounced as \"minus\".\n",
    /* 67 */
    "SET MODE SQUARE ON/OFF.\n"
    "Set the SQUARE mode flag. If ON, the \"[\" and \"]\" characters are\n"
    "phonemic brackets. If OFF, they are ordinary characters.\n",
    /* 68 */
    "SET MODE ASKY ON/OFF.\n"
    "Set the ASKY mode flag. If ON, the ASKY phonemic alphabet is used.\n"
    "If OFF, the ARPABET phonemic alphabet is used.\n",
    /* 69 */
    "SET MODE MINUS ON/OFF.\n"
    "Set the MINUS mode flag. If ON, the \"-\" character is pronounced as\n"
    "\"minus\". If OFF, the \"-\" character is pronounced as \"dash\".\n",
    /* 70 */
    "SET INTERRUPT character.\n"
    "Set the character used to enter SETUP mode. If the \"character\" is\n"
    "OFF then only the BREAK key will be used to enter SETUP mode. Control\n"
    "characters may be specified by means of a leading ^. For example, the\n"
    "interrupt character can be set to CONTROL/C by typing SET INTERRUPT ^C.\n",
};
