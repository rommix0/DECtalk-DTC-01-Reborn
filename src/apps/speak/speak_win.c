/* speak_win.c - speak's Win32 front end (REFERENCE.md s17.12): dapi's sample window (samples/speak/Speak.c) on
 * the core (speak_core.c) and the v1.8 library.
 *
 * As the sample: nine voice buttons, an edit box, the rate slider, play / pause / stop, open and save, the user
 * dictionary, conversion to a wave file, find, and right-click to speak the selection ("what?" without one). The
 * library's messages come as window messages (TextToSpeechStartup's hWnd form): index marks highlight the word being
 * spoken. The user turns that on and off with Edit > Highlighting or the "Highlight words" box by the rate slider
 * (s17.13); it is on by default. Not ported, as v1.8 has no counterpart: the language menus, the control panel, the
 * typing demo, the help file, the registry's licence lines and the palettes (s17.8). The window's place, size, last
 * file and the highlighting choice are kept in HKCU\Software\DTC-01\Speak, as the sample kept its under DEC's key.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "speak_core.h"
#include "speak_res.h"

#if defined(_MSC_VER)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

/* the sample's layout */
#define FB_W 64                     /* face buttons */
#define FB_H 64
#define ST_W 100                    /* the rate's value */
#define SB_W 154                    /* the rate slider */
#define SB_H 17
#define SL_W 100                    /* "Speaking Rate" */
#define SL_H 20
#define BORDER 2
#define PB_W 32                     /* play, pause, stop */
#define PB_H 32
#define ET_W (600 - BORDER * 4)
#define ET_H (330 - BORDER * 4)
#define AW_W (ET_W + BORDER * 4 + 4)
#define AW_H (FB_H + 60 + SL_H + BORDER * 4 + ET_H)
#define ID_BUTTON 1000              /* + the picture's number */
#define ID_HIGHLIGHT_BOX 1100       /* the "Highlight words" check box */
#define HB_W 120
#define REG_KEY "Software\\DTC-01\\Speak"

static HINSTANCE g_inst;
static HWND g_wnd, g_edit, g_rate, g_rate_text, g_rate_label, g_find, g_tips, g_hl_box;
static HWND g_button[SPEAK_NPICS];
static HFONT g_font;
static LPTTS_HANDLE_T g_tts;
static UINT g_wm_error, g_wm_index, g_wm_find;
static int g_rate_value = SPEAK_RATE_DEFAULT;
static int g_paused, g_can_highlight;
static int g_highlight = SPEAK_HIGHLIGHT_DEFAULT;   /* the user's choice (kept in the registry) */
static int g_untitled = 1;          /* the sample's SaveSaveAs */
static speak_marks_t g_marks;
static char g_file[MAX_PATH];
static char g_find_text[256];
static FINDREPLACEA g_fr;

static void error_box(const char *text, const char *title) { MessageBoxA(g_wnd, text, title, MB_OK | MB_ICONSTOP); }

static void tts_error_box(MMRESULT r, const char *title)
{
    char s[64];
    sprintf(s, "Error = %u", (unsigned)r);
    error_box(s, title);
}

/* the edit box's text (free it) */
static char *edit_text(size_t *len)
{
    int n = GetWindowTextLengthA(g_edit);
    char *t = (char *)malloc((size_t)n + 1);
    if (!t) return NULL;
    GetWindowTextA(g_edit, t, n + 1);
    if (len) *len = strlen(t);
    return t;
}

static void set_title(void)
{
    char t[MAX_PATH + 32];
    sprintf(t, "%s - %s", SPEAK_APP_NAME, g_untitled ? "Untitled" : g_file);
    SetWindowTextA(g_wnd, t);
}

/* ---- speaking ---- */

static void speak_edit(void)
{
    char *t = edit_text(NULL);
    MMRESULT r;
    if (!t) return;
    speak_marks_free(&g_marks);
    r = speak_text(g_tts, t, g_highlight, &g_marks);
    free(t);
    if (r) tts_error_box(r, "TextToSpeechSpeak");
}

static void play(void)
{
    TextToSpeechSetRate(g_tts, (DWORD)g_rate_value);
    speak_edit();
    SetFocus(g_edit);
}

static void set_paused(int paused)
{
    g_paused = paused;
    InvalidateRect(g_button[SPEAK_PIC_PAUSE], NULL, FALSE);
}

static void stop(void)
{
    if (g_paused && TextToSpeechResume(g_tts) == MMSYSERR_NOERROR) set_paused(0);
    if (TextToSpeechReset(g_tts, FALSE)) error_box("Error in TTS Reset", "ERROR");
    SetFocus(g_edit);
}

/* right button in the edit box: the selection, or "what?" */
static void speak_selection(void)
{
    DWORD s = 0, e = 0;
    SendMessageA(g_edit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    if (s == e) {
        if (TextToSpeechSpeak(g_tts, "what?", TTS_FORCE)) error_box("Error in TTS Speak", "ERROR");
    } else {
        char *t = edit_text(NULL);
        if (!t) return;
        t[e] = 0;
        if (TextToSpeechSpeak(g_tts, t + s, TTS_FORCE)) error_box("Error in TTS Speak", "ERROR");
        free(t);
    }
}

/* ---- files ---- */

static int file_dialog(int save, const char *filter, const char *ext, const char *title, char *path)
{
    OPENFILENAMEA o;
    memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o;
    o.hwndOwner = g_wnd;
    o.lpstrFilter = filter;
    o.nFilterIndex = 1;
    o.lpstrFile = path;
    o.nMaxFile = MAX_PATH;
    o.lpstrDefExt = ext;
    o.lpstrTitle = title;
    o.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return save ? GetSaveFileNameA(&o) : GetOpenFileNameA(&o);
}

static const char text_filter[] = "Text Files (*.TXT)\0*.TXT\0All Files (*.*)\0*.*\0";

/* read a file into the edit box: 1 if done */
static int load_file(const char *path)
{
    char *t, *c;
    if (!(t = speak_read_file(path, NULL))) {
        error_box("File open failed.", "ERROR");
        return 0;
    }
    c = speak_to_crlf(t);
    free(t);
    if (!c) {
        error_box("Insufficient memory to load file", "ERROR");
        return 0;
    }
    SetWindowTextA(g_edit, c);
    free(c);
    SendMessageA(g_edit, EM_SETMODIFY, FALSE, 0);
    strncpy(g_file, path, sizeof g_file - 1);
    g_untitled = 0;
    set_title();
    return 1;
}

static int save_to(const char *path)
{
    size_t n;
    char *t = edit_text(&n);
    int ok = t && speak_write_file(path, t, n);
    free(t);
    if (!ok) {
        error_box("File save failed.", "ERROR");
        return 0;
    }
    SendMessageA(g_edit, EM_SETMODIFY, FALSE, 0);
    return 1;
}

static int save_as(void)
{
    char path[MAX_PATH] = "";
    if (!file_dialog(1, text_filter, "txt", "Save File As", path) || !save_to(path)) return 0;
    strcpy(g_file, path);
    g_untitled = 0;
    set_title();
    return 1;
}

static int save(void) { return g_untitled ? save_as() : save_to(g_file); }

/* the sample's AskToSave: 0 to go on (saved, or not wanted), 1 for cancel */
static int ask_to_save(void)
{
    if (!SendMessageA(g_edit, EM_GETMODIFY, 0, 0)) return 0;
    switch (MessageBoxA(g_wnd, "The Text in this file has changed.\n\nDo you wish to save the changes?", SPEAK_APP_NAME,
                        MB_YESNOCANCEL | MB_ICONEXCLAMATION)) {
    case IDYES:
        return !save();
    case IDNO:
        return 0;
    default:
        return 1;
    }
}

static void open_file(void)
{
    char path[MAX_PATH] = "";
    if (ask_to_save()) return;
    if (file_dialog(0, text_filter, "txt", "Open a File", path) && load_file(path)) TextToSpeechReset(g_tts, FALSE);
}

static void new_file(void)
{
    if (ask_to_save()) return;
    SetWindowTextA(g_edit, "");
    SendMessageA(g_edit, EM_SETMODIFY, FALSE, 0);
    g_untitled = 1;
    g_file[0] = 0;
    set_title();
}

static void load_dictionary(void)
{
    char path[MAX_PATH] = "";
    MMRESULT r;
    if (!file_dialog(0, "User Dictionaries (*.TXT;*.DIC)\0*.TXT;*.DIC\0All Files (*.*)\0*.*\0", "txt",
                     "Load a Dictionary", path))
        return;
    TextToSpeechUnloadUserDictionary(g_tts);
    r = TextToSpeechLoadUserDictionary(g_tts, path);
    if (r == MMSYSERR_ERROR) error_box("The dictionary cannot be read.", "Load User Dictionary");
    else if (r) tts_error_box(r, "Load User Dictionary");
}

static void to_wave(int k)
{
    char path[MAX_PATH] = "", *t;
    const char *what;
    HCURSOR old;
    MMRESULT r;
    if (!file_dialog(1, "Wave Files (*.WAV)\0*.WAV\0All Files (*.*)\0*.*\0", "wav", "Convert to Wave File", path))
        return;
    if (!(t = edit_text(NULL))) return;
    stop();                                         /* not after what is being said */
    old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    r = speak_to_wave(g_tts, t, path, speak_wave_formats[k].format, &what);
    SetCursor(old);
    free(t);
    if (r) tts_error_box(r, what);
}

/* ---- find ---- */

static void find_next(const FINDREPLACEA *fr)
{
    DWORD s = 0, e = 0;
    size_t len, at;
    int flags = (fr->Flags & FR_DOWN ? SPEAK_FIND_DOWN : 0) | (fr->Flags & FR_MATCHCASE ? SPEAK_FIND_CASE : 0) |
                (fr->Flags & FR_WHOLEWORD ? SPEAK_FIND_WORD : 0);
    char *t = edit_text(&len);
    if (!t) return;
    SendMessageA(g_edit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    if (speak_find(t, len, fr->lpstrFindWhat, flags & SPEAK_FIND_DOWN ? e : s, flags, &at)) {
        SendMessageA(g_edit, EM_SETSEL, at, at + strlen(fr->lpstrFindWhat));
        SendMessageA(g_edit, EM_SCROLLCARET, 0, 0);
    } else {
        char m[300];
        _snprintf(m, sizeof m, "Search string \"%s\" was not found.", fr->lpstrFindWhat);
        m[sizeof m - 1] = 0;
        MessageBoxA(g_find ? g_find : g_wnd, m, "Find Error", MB_OK | MB_ICONEXCLAMATION);
    }
    free(t);
}

static void open_find(void)
{
    DWORD s = 0, e = 0;
    if (g_find) {
        SetFocus(g_find);
        return;
    }
    SendMessageA(g_edit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    if (e > s && e - s < sizeof g_find_text) {
        char *t = edit_text(NULL);
        if (t) {
            memcpy(g_find_text, t + s, e - s);
            g_find_text[e - s] = 0;
            free(t);
        }
    }
    memset(&g_fr, 0, sizeof g_fr);
    g_fr.lStructSize = sizeof g_fr;
    g_fr.hwndOwner = g_wnd;
    g_fr.Flags = FR_DOWN;
    g_fr.lpstrFindWhat = g_find_text;
    g_fr.wFindWhatLen = sizeof g_find_text;
    g_find = FindTextA(&g_fr);
}

/* ---- the window ---- */

static void draw_button(const DRAWITEMSTRUCT *d)
{
    int i = (int)d->CtlID - ID_BUTTON, down;
    const unsigned char *bmp;
    const BITMAPINFOHEADER *bi;
    if (i < 0 || i >= SPEAK_NPICS) return;
    down = (d->itemState & ODS_SELECTED) != 0;
    if (i == SPEAK_PIC_PAUSE) down = g_paused;
    bmp = speak_pics[i][down].bmp;
    bi = (const BITMAPINFOHEADER *)(bmp + sizeof(BITMAPFILEHEADER));
    SetDIBitsToDevice(d->hDC, 0, 0, (DWORD)bi->biWidth, (DWORD)bi->biHeight, 0, 0, 0, (UINT)bi->biHeight,
                      bmp + ((const BITMAPFILEHEADER *)bmp)->bfOffBits, (const BITMAPINFO *)bi, DIB_RGB_COLORS);
}

static void set_rate(int rate)
{
    char s[16];
    g_rate_value = speak_rate_clamp(rate);
    SetScrollPos(g_rate, SB_CTL, g_rate_value, TRUE);
    sprintf(s, "%d WPM", g_rate_value);
    SetWindowTextA(g_rate_text, s);
}

/* the sample's HandleScrollBar */
static void rate_scrolled(WPARAM w)
{
    int r = g_rate_value;
    switch (LOWORD(w)) {
    case SB_PAGEDOWN: r += SPEAK_RATE_PAGE; break;
    case SB_LINEDOWN: r += SPEAK_RATE_LINE; break;
    case SB_PAGEUP: r -= SPEAK_RATE_PAGE; break;
    case SB_LINEUP: r -= SPEAK_RATE_LINE; break;
    case SB_TOP: r = SPEAK_RATE_MIN; break;
    case SB_BOTTOM: r = SPEAK_RATE_MAX; break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK: r = speak_rate_round(HIWORD(w)); break;
    default: return;
    }
    set_rate(r);
    if (TextToSpeechSetRate(g_tts, (DWORD)g_rate_value)) error_box("Error Setting Rate", "ERROR");
}

static void layout(void)
{
    RECT rc;
    int i, y;
    GetClientRect(g_wnd, &rc);
    y = rc.bottom - (2 * BORDER + SB_H + 10);
    MoveWindow(g_edit, BORDER * 2, BORDER * 2 + FB_H, rc.right - 8, rc.bottom - 112, TRUE);
    MoveWindow(g_rate_label, BORDER, y, SL_W, SL_H, TRUE);
    MoveWindow(g_rate, BORDER + SL_W, y, SB_W, SB_H, TRUE);
    MoveWindow(g_rate_text, SL_W + BORDER + SB_W, y, ST_W, SB_H, TRUE);
    MoveWindow(g_hl_box, SL_W + BORDER + SB_W + ST_W + 10, y, HB_W, SB_H, TRUE);
    for (i = SPEAK_PIC_PLAY; i <= SPEAK_PIC_STOP; i++)
        MoveWindow(g_button[i], 488 + (i - SPEAK_PIC_PLAY) * PB_W, rc.bottom - (PB_H + BORDER * 2), PB_W, PB_H, TRUE);
}

static void add_tip(HWND button, const char *text)
{
    TOOLINFOA ti;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = sizeof ti;
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_wnd;
    ti.uId = (UINT_PTR)button;
    ti.lpszText = (LPSTR)text;
    SendMessageA(g_tips, TTM_ADDTOOLA, 0, (LPARAM)&ti);
}

static int create_children(HWND w)
{
    static const char *tips[] = {"Play", "Pause / Resume", "Stop"};
    int i;
    MMRESULT r;
    g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                             WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOHSCROLL |
                                 ES_AUTOVSCROLL | ES_NOHIDESEL,
                             BORDER * 2, BORDER * 2 + FB_H, ET_W, ET_H, w, NULL, g_inst, NULL);
    SendMessageA(g_edit, EM_LIMITTEXT, 0, 0);
    SendMessageA(g_edit, WM_SETFONT, (WPARAM)g_font, FALSE);
    g_rate = CreateWindowExA(0, "SCROLLBAR", "", WS_CHILD | WS_VISIBLE | SBS_HORZ, 0, 0, SB_W, SB_H, w, NULL, g_inst,
                             NULL);
    SetScrollRange(g_rate, SB_CTL, SPEAK_RATE_MIN, SPEAK_RATE_MAX, TRUE);
    g_rate_text = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE | SS_CENTER | WS_BORDER, 0, 0, ST_W, SB_H, w,
                                  NULL, g_inst, NULL);
    g_rate_label = CreateWindowExA(0, "STATIC", "Speaking Rate", WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, SL_W, SL_H,
                                   w, NULL, g_inst, NULL);
    SendMessageA(g_rate_text, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessageA(g_rate_label, WM_SETFONT, (WPARAM)g_font, FALSE);
    g_hl_box = CreateWindowExA(0, "BUTTON", "Highlight &words", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                               0, 0, HB_W, SB_H, w, (HMENU)(INT_PTR)ID_HIGHLIGHT_BOX, g_inst, NULL);
    SendMessageA(g_hl_box, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessageA(g_hl_box, BM_SETCHECK, g_highlight ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(g_hl_box, g_can_highlight);
    g_tips = CreateWindowExA(0, TOOLTIPS_CLASSA, NULL, WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0, 0, w, NULL, g_inst, NULL);
    for (i = 0; i < SPEAK_NPICS; i++) {
        int voice = i < SPEAK_NVOICES;
        g_button[i] = CreateWindowExA(0, "BUTTON", "", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                      voice ? i * FB_W + 10 : 0, voice ? BORDER : 0, voice ? FB_W : PB_W,
                                      voice ? FB_H : PB_H, w, (HMENU)(INT_PTR)(ID_BUTTON + i), g_inst, NULL);
        add_tip(g_button[i], voice ? speak_voices[i].label : tips[i - SPEAK_NVOICES]);
    }
    set_rate(SPEAK_RATE_DEFAULT);
    layout();
    for (i = 0; i < SPEAK_NWAVE; i++) {               /* the core's labels (with the mu) */
        char s[64];
        sprintf(s, "%s...", speak_wave_formats[i].label);
        ModifyMenuA(GetMenu(w), IDM_WAVE1116 + i, MF_BYCOMMAND | MF_STRING, IDM_WAVE1116 + i, s);
    }

    g_wm_error = RegisterWindowMessageA(TTS_ERROR_MESSAGE_NAME);
    g_wm_index = RegisterWindowMessageA(TTS_INDEX_MESSAGE_NAME);
    g_wm_find = RegisterWindowMessageA(FINDMSGSTRINGA);
    r = TextToSpeechStartup(w, &g_tts, WAVE_MAPPER, REPORT_OPEN_ERROR);
    if (r == MMSYSERR_NODRIVER || r == WAVERR_BADFORMAT || r == MMSYSERR_BADDEVICEID) {
        MessageBoxA(w, "No compatible wave device present\nYou can continue but only to write wave files", "Warning",
                    MB_OK);
        r = TextToSpeechStartup(w, &g_tts, 0, DO_NOT_USE_AUDIO_DEVICE);
    }
    if (r) {
        char s[128];
        sprintf(s, "TTS startup failed\n\nError = %u\n\nExiting out of application", (unsigned)r);
        MessageBoxA(NULL, s, "Speak cannot be started.", MB_OK | MB_ICONSTOP);
        return 0;
    }
    TextToSpeechSetRate(g_tts, (DWORD)g_rate_value);
    return 1;
}

/* the library's status and error messages (the sample's texts) */
static void tts_status(WPARAM code)
{
    const char *text = NULL, *title = "Async Error";
    switch (code) {
    case TTS_AUDIO_PLAY_STOP:
        if (g_marks.nwords) {                       /* the sample: the selection goes */
            SendMessageA(g_edit, EM_SETSEL, (WPARAM)-1, 0);
            SendMessageA(g_edit, EM_SCROLLCARET, 0, 0);
        }
        return;
    case ERROR_IN_AUDIO_WRITE: text = "Error in Writing Audio"; break;
    case ERROR_OPENING_WAVE_OUTPUT_DEVICE:
        text = "The wave device is in use by another application\nDECtalk will wait until the device is free.";
        title = "Warning";
        break;
    case ERROR_GETTING_DEVICE_CAPABILITIES: text = "Error Getting Audio Device Caps"; break;
    case ERROR_WRITING_FILE: text = "Error Writing File"; break;
    case ERROR_OPENING_WAVE_FILE: text = "Error Opening Wave File"; break;
    default: return;                                /* play start; v1.8's phonemic-text errors */
    }
    MessageBoxA(g_wnd, text, title, MB_OK | MB_ICONSTOP);
}

/* Highlighting on or off (the menu item and the box show the same). Off while speaking: the selection goes now. */
static void set_highlight(int on)
{
    g_highlight = on && g_can_highlight;
    SendMessageA(g_hl_box, BM_SETCHECK, g_highlight ? BST_CHECKED : BST_UNCHECKED, 0);
    if (!g_highlight && g_marks.nwords) SendMessageA(g_edit, EM_SETSEL, (WPARAM)-1, 0);
}

static void index_mark(WPARAM kind, LPARAM value)
{
    unsigned long s, e;
    if (!g_highlight || kind != TTS_INDEX_MARK || !speak_marks_find(&g_marks, (unsigned)value, &s, &e)) return;
    SendMessageA(g_edit, EM_SETSEL, s, e);
    SendMessageA(g_edit, EM_SCROLLCARET, 0, 0);
}

static void menu_popup(HMENU m)
{
    DWORD s = 0, e = 0;
    UINT sel, text;
    SendMessageA(g_edit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    sel = s != e ? MF_ENABLED : MF_GRAYED;
    text = GetWindowTextLengthA(g_edit) ? MF_ENABLED : MF_GRAYED;
    EnableMenuItem(m, ID_EDIT_PASTE, IsClipboardFormatAvailable(CF_TEXT) ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(m, ID_EDIT_CUT, sel);
    EnableMenuItem(m, ID_EDIT_COPY, sel);
    EnableMenuItem(m, ID_EDIT_CLEAR, sel);
    EnableMenuItem(m, IDM_SAVE, text);
    EnableMenuItem(m, IDM_SAVEAS, text);
    EnableMenuItem(m, ID_EDIT_UNDO, SendMessageA(g_edit, EM_CANUNDO, 0, 0) ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(m, IDM_HIGHLIGHT, g_can_highlight ? MF_ENABLED : MF_GRAYED);
    CheckMenuItem(m, IDM_HIGHLIGHT, g_highlight ? MF_CHECKED : MF_UNCHECKED);
}

static void os_version(char *s, size_t n)
{
    typedef LONG(WINAPI * rtl_get_version_t)(OSVERSIONINFOW *);
    OSVERSIONINFOW v;
    rtl_get_version_t f = (rtl_get_version_t)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    if (f && f(&v) == 0)
        _snprintf(s, n, "Windows %lu.%lu (build %lu), %d-bit Speak", v.dwMajorVersion, v.dwMinorVersion,
                  v.dwBuildNumber, (int)(8 * sizeof(void *)));
    else
        _snprintf(s, n, "Windows");
    s[n - 1] = 0;
}

static INT_PTR CALLBACK about_proc(HWND d, UINT msg, WPARAM w, LPARAM l)
{
    char s[128];
    LPSTR vs;
    DWORD v;
    (void)l;
    switch (msg) {
    case WM_INITDIALOG:
        os_version(s, sizeof s);
        SetDlgItemTextA(d, IDD_ABOUT_VERSION_OS, s);
        v = TextToSpeechVersion(&vs);
        sprintf(s, "%lu.%02lu (build %lX)", (v >> 24) & 0x7f, (v >> 16) & 0xff, v & 0xffff);
        SetDlgItemTextA(d, IDD_ABOUT_DECTALK_VERSION, s);
        SetDlgItemTextA(d, IDD_ABOUT_DECTALK_VERSION_STR, vs);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) EndDialog(d, LOWORD(w) == IDOK);
        return TRUE;
    }
    return FALSE;
}

static void command(WPARAM w, LPARAM l)
{
    int id = LOWORD(w);
    if (id >= ID_BUTTON && id < ID_BUTTON + SPEAK_NPICS && HIWORD(w) == BN_CLICKED) {
        int i = id - ID_BUTTON;
        (void)l;
        if (i < SPEAK_NVOICES) {
            if (TextToSpeechSpeak(g_tts, (LPSTR)speak_voices[i].command, TTS_NORMAL) ||
                TextToSpeechSpeak(g_tts, (LPSTR)speak_voices[i].test, TTS_FORCE))
                error_box("Error in Speak", "ERROR");
            SetFocus(g_edit);
        } else if (i == SPEAK_PIC_PLAY) {
            play();
        } else if (i == SPEAK_PIC_PAUSE) {
            if (!g_paused ? TextToSpeechPause(g_tts) == MMSYSERR_NOERROR : TextToSpeechResume(g_tts) == MMSYSERR_NOERROR)
                set_paused(!g_paused);
            SetFocus(g_edit);
        } else {
            stop();
        }
        return;
    }
    switch (id) {
    case IDM_FILE_NEW:
    case IDM_FILE_CLOSE: new_file(); break;
    case IDM_FILE_OPEN: open_file(); break;
    case IDM_SAVE: save(); break;
    case IDM_SAVEAS: save_as(); break;
    case IDM_LOAD_DIC: load_dictionary(); break;
    case IDM_UNLOAD_DIC: {
        MMRESULT r = TextToSpeechUnloadUserDictionary(g_tts);
        if (r) tts_error_box(r, "Unload User Dictionary");
        break;
    }
    case IDM_WAVE1116: to_wave(0); break;
    case IDM_WAVE1108: to_wave(1); break;
    case IDM_MULAW: to_wave(2); break;
    case IDM_EXIT: SendMessageA(g_wnd, WM_CLOSE, 0, 0); break;
    case ID_EDIT_UNDO: SendMessageA(g_edit, EM_UNDO, 0, 0); break;
    case ID_EDIT_CUT: SendMessageA(g_edit, WM_CUT, 0, 0); break;
    case ID_EDIT_COPY: SendMessageA(g_edit, WM_COPY, 0, 0); break;
    case ID_EDIT_PASTE: SendMessageA(g_edit, WM_PASTE, 0, 0); break;
    case ID_EDIT_CLEAR: SendMessageA(g_edit, WM_CLEAR, 0, 0); break;
    case ID_EDIT_SELECT_ALL:
        SendMessageA(g_edit, EM_SETSEL, 0, -1);
        SetFocus(g_edit);
        break;
    case ID_FIND: open_find(); break;
    case IDM_HIGHLIGHT: set_highlight(!g_highlight); break;
    case ID_HIGHLIGHT_BOX:
        if (HIWORD(w) == BN_CLICKED) set_highlight(SendMessageA(g_hl_box, BM_GETCHECK, 0, 0) == BST_CHECKED);
        break;
    case IDM_ABOUT: DialogBoxA(g_inst, MAKEINTRESOURCEA(DLG_ABOUT), g_wnd, about_proc); break;
    }
}

/* ---- the window's place, size and file (the sample's Get/SetApplicationParameters) ---- */

static void load_placement(int *x, int *y, int *w, int *h, int *show)
{
    HKEY k;
    DWORD v, n;
    *x = *y = CW_USEDEFAULT;
    *w = AW_W;
    *h = AW_H;
    *show = SW_SHOWNORMAL;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return;
    n = sizeof v;
    if (RegQueryValueExA(k, "Show", NULL, NULL, (LPBYTE)&v, &n) == ERROR_SUCCESS && v != SW_SHOWMINIMIZED)
        *show = (int)v;
    n = sizeof v;
    if (RegQueryValueExA(k, "Position", NULL, NULL, (LPBYTE)&v, &n) == ERROR_SUCCESS) {
        *x = (short)(v & 0xffff);
        *y = (short)(v >> 16);
    }
    n = sizeof v;
    if (RegQueryValueExA(k, "Size", NULL, NULL, (LPBYTE)&v, &n) == ERROR_SUCCESS && (v & 0xffff) >= 200 &&
        (v >> 16) >= 200) {
        *w = (int)(v & 0xffff);
        *h = (int)(v >> 16);
    }
    n = sizeof v;
    if (RegQueryValueExA(k, "Highlight", NULL, NULL, (LPBYTE)&v, &n) == ERROR_SUCCESS) g_highlight = v != 0;
    RegCloseKey(k);
}

static void save_placement(void)
{
    HKEY k;
    WINDOWPLACEMENT p;
    DWORD v;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    p.length = sizeof p;
    if (GetWindowPlacement(g_wnd, &p)) {
        v = p.showCmd;
        RegSetValueExA(k, "Show", 0, REG_DWORD, (const BYTE *)&v, sizeof v);
        v = ((DWORD)(p.rcNormalPosition.top & 0xffff) << 16) | (DWORD)(p.rcNormalPosition.left & 0xffff);
        RegSetValueExA(k, "Position", 0, REG_DWORD, (const BYTE *)&v, sizeof v);
        v = ((DWORD)(p.rcNormalPosition.bottom - p.rcNormalPosition.top) << 16) |
            (DWORD)(p.rcNormalPosition.right - p.rcNormalPosition.left);
        RegSetValueExA(k, "Size", 0, REG_DWORD, (const BYTE *)&v, sizeof v);
    }
    RegSetValueExA(k, "File", 0, REG_SZ, (const BYTE *)g_file, (DWORD)strlen(g_file) + 1);
    v = (DWORD)g_highlight;
    RegSetValueExA(k, "Highlight", 0, REG_DWORD, (const BYTE *)&v, sizeof v);
    RegCloseKey(k);
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_wnd = w;
        return create_children(w) ? 0 : -1;
    case WM_SETFOCUS:
        SetFocus(g_edit);
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = AW_W;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = 300;
        return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(w, &rc);
        FillRect((HDC)wp, &rc, (HBRUSH)GetStockObject(LTGRAY_BRUSH));
        return 1;
    }
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, RGB(192, 192, 192));
        return (LRESULT)GetStockObject(LTGRAY_BRUSH);
    case WM_DRAWITEM:
        draw_button((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_COMMAND:
        command(wp, lp);
        return 0;
    case WM_HSCROLL:
        if ((HWND)lp == g_rate) rate_scrolled(wp);
        return 0;
    case WM_INITMENUPOPUP:
        menu_popup((HMENU)wp);
        return 0;
    case WM_PARENTNOTIFY:
        if (LOWORD(wp) == WM_RBUTTONDOWN) speak_selection();
        return 0;
    case WM_DROPFILES: {
        char path[MAX_PATH];
        if (!ask_to_save() && DragQueryFileA((HDROP)wp, 0, path, sizeof path)) {
            TextToSpeechReset(g_tts, FALSE);
            if (load_file(path)) speak_edit();
        }
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_CLOSE:
        if (!ask_to_save()) DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        save_placement();
        if (g_tts) TextToSpeechShutdown(g_tts);
        g_tts = NULL;
        speak_marks_free(&g_marks);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_wm_index && msg) {
        index_mark(wp, lp);
        return 0;
    }
    if (msg == g_wm_error && msg) {
        tts_status(wp);
        return 0;
    }
    if (msg == g_wm_find && msg) {
        const FINDREPLACEA *fr = (const FINDREPLACEA *)lp;
        if (fr->Flags & FR_DIALOGTERM) g_find = NULL;
        else if (fr->Flags & FR_FINDNEXT) find_next(fr);
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    MSG m;
    INITCOMMONCONTROLSEX icc;
    const char *file, *dict;
    char msg[160];
    int x, y, w, h, s;
    (void)prev;
    (void)cmd;
    (void)show;
    g_inst = inst;
    if (speak_parse_args(__argc, __argv, &file, &dict)) {
        MessageBoxA(NULL, SPEAK_USAGE, "Usage of Speak", MB_OK);
        return 0;
    }
    if (!speak_check_version(&g_can_highlight, msg, sizeof msg)) {
        MessageBoxA(NULL, msg, "DECtalk.DLL", MB_OK | MB_ICONSTOP);
        return 0;
    }
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);
    memset(&wc, 0, sizeof wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(ICON_APP));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(LTGRAY_BRUSH);
    wc.lpszMenuName = MAKEINTRESOURCEA(SPEAK_MENU);
    wc.lpszClassName = SPEAK_APP_NAME;
    if (!RegisterClassA(&wc)) return 0;
    load_placement(&x, &y, &w, &h, &s);
    if (!CreateWindowExA(0, SPEAK_APP_NAME, SPEAK_APP_NAME, WS_OVERLAPPEDWINDOW, x, y, w, h, NULL, NULL, inst, NULL))
        return 0;
    set_title();
    ShowWindow(g_wnd, s);
    UpdateWindow(g_wnd);
    DragAcceptFiles(g_wnd, TRUE);
    if (dict) {
        MMRESULT r;
        TextToSpeechUnloadUserDictionary(g_tts);
        if ((r = TextToSpeechLoadUserDictionary(g_tts, (LPSTR)dict)) != MMSYSERR_NOERROR)
            error_box(r == MMSYSERR_ERROR ? "The dictionary cannot be read." : "Error in TTS Load Dictionary",
                      "Load User Dictionary");
    }
    if (file && load_file(file)) speak_edit();
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        if (g_find && IsDialogMessageA(g_find, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return (int)m.wParam;
}
