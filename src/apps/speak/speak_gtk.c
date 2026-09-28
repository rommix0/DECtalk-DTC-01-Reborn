/* speak_gtk.c - speak's GTK 3 front end (REFERENCE.md s17.12): the Win32 sample's window (speak_win.c) for Linux,
 * on the same core (speak_core.c) and library.
 *
 * The same controls and menus: nine voice buttons, the text, the rate slider, play / pause / stop, open and save, the
 * user dictionary, conversion to a wave file, find, highlighting, and right-click to speak the selection ("what?"
 * without one). The library's messages come through its event queue (TTS_EVENT_QUEUE): its descriptor is watched by
 * GLib's main loop (g_unix_fd_add), so they are handled in the GUI thread, as window messages are on Windows (s17.7).
 * Highlighting: Edit > Highlighting or the "Highlight words" box (s17.13), on by default; the choice is kept in
 * $XDG_CONFIG_HOME/dtc01/speak.ini.
 *
 * Text: GTK's is UTF-8, v1.8 reads 8-bit bytes. What is spoken is the text one byte per character: Latin-1 as it is,
 * typographic quotes and dashes as ' " -, anything else a blank; so a mark's word offsets are character offsets in the
 * buffer. Files are read as UTF-8, or else as Windows-1252 (the sample's files), and written as UTF-8. Not here (GTK 3
 * has no text undo): Edit > Undo; nor dropping files on the window.
 */
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <stdlib.h>
#include <string.h>
#include "speak_core.h"

static GtkWidget *win, *view, *rate_scale, *rate_text, *pics[SPEAK_NPICS], *find_dlg, *find_entry, *find_case,
    *find_word, *find_up, *hl_item, *hl_box;
static GtkTextBuffer *buf;
static GdkPixbuf *pix[SPEAK_NPICS][2];
static LPTTS_HANDLE_T tts;
static int paused, highlight, can_highlight, untitled = 1;
static speak_marks_t marks;
static char *file_name;

static void message(GtkMessageType type, const char *title, const char *text)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", text);
    gtk_window_set_title(GTK_WINDOW(d), title);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static void tts_error(MMRESULT r, const char *title)
{
    char s[64];
    g_snprintf(s, sizeof s, "Error = %u", (unsigned)r);
    message(GTK_MESSAGE_ERROR, title, s);
}

/* the buffer's text, or a range of it, as UTF-8 (g_free it) */
static char *buffer_text(int start, int end)
{
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(buf, &a, start);
    if (end < 0) gtk_text_buffer_get_end_iter(buf, &b);
    else gtk_text_buffer_get_iter_at_offset(buf, &b, end);
    return gtk_text_buffer_get_text(buf, &a, &b, FALSE);
}

/* UTF-8 to the bytes v1.8 reads, one per character (see above; g_free it) */
static char *to_dectalk(const char *u)
{
    char *out = g_malloc(strlen(u) + 1), *o = out;
    for (; *u; u = g_utf8_next_char(u)) {
        gunichar c = g_utf8_get_char(u);
        if (c < 0x80) *o++ = (char)c;
        else if (c == 0xa0) *o++ = ' ';
        else if (c <= 0xff) *o++ = (char)c;
        else if (c == 0x2018 || c == 0x2019 || c == 0x201b || c == 0x2032) *o++ = '\'';
        else if (c == 0x201c || c == 0x201d || c == 0x201e || c == 0x2033) *o++ = '"';
        else if (c >= 0x2010 && c <= 0x2015) *o++ = '-';
        else if (c == 0x2212) *o++ = '-';
        else if (c == 0x2026) *o++ = '.';
        else *o++ = ' ';
    }
    *o = 0;
    return out;
}

static void set_title(void)
{
    char *t = g_strdup_printf("%s - %s", SPEAK_APP_NAME, untitled ? "Untitled" : file_name);
    gtk_window_set_title(GTK_WINDOW(win), t);
    g_free(t);
}

/* ---- the pictures ---- */

static GdkPixbuf *load_pic(const speak_pic_t *p)
{
    GdkPixbufLoader *l = gdk_pixbuf_loader_new_with_type("bmp", NULL);
    GdkPixbuf *px = NULL;
    if (!l) return NULL;
    if (gdk_pixbuf_loader_write(l, p->bmp, p->size, NULL) && gdk_pixbuf_loader_close(l, NULL)) {
        px = gdk_pixbuf_loader_get_pixbuf(l);
        if (px) g_object_ref(px);
    }
    g_object_unref(l);
    return px;
}

static void show_pic(int i, int down) { gtk_image_set_from_pixbuf(GTK_IMAGE(pics[i]), pix[i][down]); }

static void pressed(GtkButton *b, gpointer i)
{
    (void)b;
    if (GPOINTER_TO_INT(i) != SPEAK_PIC_PAUSE) show_pic(GPOINTER_TO_INT(i), 1);
}

static void released(GtkButton *b, gpointer i)
{
    (void)b;
    if (GPOINTER_TO_INT(i) != SPEAK_PIC_PAUSE) show_pic(GPOINTER_TO_INT(i), 0);
}

/* ---- speaking ---- */

static void speak_buffer(void)
{
    char *u = buffer_text(0, -1), *t = to_dectalk(u);
    MMRESULT r;
    speak_marks_free(&marks);
    r = speak_text(tts, t, highlight, &marks);
    g_free(t);
    g_free(u);
    if (r) tts_error(r, "TextToSpeechSpeak");
}

static void set_paused(int p)
{
    paused = p;
    show_pic(SPEAK_PIC_PAUSE, p);
}

static void stop(void)
{
    if (paused && TextToSpeechResume(tts) == MMSYSERR_NOERROR) set_paused(0);
    if (TextToSpeechReset(tts, FALSE)) message(GTK_MESSAGE_ERROR, "ERROR", "Error in TTS Reset");
}

static void clicked(GtkButton *b, gpointer data)
{
    int i = GPOINTER_TO_INT(data);
    (void)b;
    if (i < SPEAK_NVOICES) {
        if (TextToSpeechSpeak(tts, (LPSTR)speak_voices[i].command, TTS_NORMAL) ||
            TextToSpeechSpeak(tts, (LPSTR)speak_voices[i].test, TTS_FORCE))
            message(GTK_MESSAGE_ERROR, "ERROR", "Error in Speak");
    } else if (i == SPEAK_PIC_PLAY) {
        TextToSpeechSetRate(tts, (DWORD)gtk_range_get_value(GTK_RANGE(rate_scale)));
        speak_buffer();
    } else if (i == SPEAK_PIC_PAUSE) {
        if (!paused ? TextToSpeechPause(tts) == MMSYSERR_NOERROR : TextToSpeechResume(tts) == MMSYSERR_NOERROR)
            set_paused(!paused);
    } else {
        stop();
    }
    gtk_widget_grab_focus(view);
}

/* right button: the selection, or "what?" (the edit menu still opens) */
static gboolean view_button(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    GtkTextIter a, b;
    (void)w;
    (void)data;
    if (e->type != GDK_BUTTON_PRESS || e->button != 3) return FALSE;
    if (gtk_text_buffer_get_selection_bounds(buf, &a, &b)) {
        char *u = gtk_text_buffer_get_text(buf, &a, &b, FALSE), *t = to_dectalk(u);
        if (TextToSpeechSpeak(tts, t, TTS_FORCE)) message(GTK_MESSAGE_ERROR, "ERROR", "Error in TTS Speak");
        g_free(t);
        g_free(u);
    } else if (TextToSpeechSpeak(tts, "what?", TTS_FORCE)) {
        message(GTK_MESSAGE_ERROR, "ERROR", "Error in TTS Speak");
    }
    return FALSE;
}

/* ---- the rate ---- */

static gchar *rate_format(GtkScale *s, gdouble v, gpointer d)
{
    (void)s;
    (void)d;
    return g_strdup_printf("%d WPM", (int)v);
}

static gboolean rate_change(GtkRange *r, GtkScrollType t, gdouble v, gpointer d)
{
    (void)t;
    (void)d;
    gtk_range_set_value(r, speak_rate_round((int)(v + 0.5)));
    return TRUE;
}

static void rate_changed(GtkRange *r, gpointer d)
{
    char s[16];
    (void)d;
    g_snprintf(s, sizeof s, "%d WPM", (int)gtk_range_get_value(r));
    gtk_label_set_text(GTK_LABEL(rate_text), s);
    if (tts && TextToSpeechSetRate(tts, (DWORD)gtk_range_get_value(r)))
        message(GTK_MESSAGE_ERROR, "ERROR", "Error Setting Rate");
}

/* ---- the library's messages ---- */

static void index_mark(LONG kind, LONG value)
{
    unsigned long s, e;
    GtkTextIter a, b;
    if (!highlight || kind != TTS_INDEX_MARK || !speak_marks_find(&marks, (unsigned)value, &s, &e)) return;
    gtk_text_buffer_get_iter_at_offset(buf, &a, (gint)s);
    gtk_text_buffer_get_iter_at_offset(buf, &b, (gint)e);
    gtk_text_buffer_select_range(buf, &a, &b);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(view), &a, 0.1, FALSE, 0, 0);
}

static void status(LONG code)
{
    const char *text = NULL, *title = "Async Error";
    GtkTextIter a, b;
    switch (code) {
    case TTS_AUDIO_PLAY_STOP:
        if (marks.nwords && gtk_text_buffer_get_selection_bounds(buf, &a, &b)) gtk_text_buffer_place_cursor(buf, &b);
        return;
    case ERROR_IN_AUDIO_WRITE: text = "Error in Writing Audio"; break;
    case ERROR_OPENING_WAVE_OUTPUT_DEVICE:
        text = "The wave device is in use by another application\nDECtalk will wait until the device is free.";
        title = "Warning";
        break;
    case ERROR_GETTING_DEVICE_CAPABILITIES: text = "Error Getting Audio Device Caps"; break;
    case ERROR_WRITING_FILE: text = "Error Writing File"; break;
    case ERROR_OPENING_WAVE_FILE: text = "Error Opening Wave File"; break;
    default: return;
    }
    message(GTK_MESSAGE_WARNING, title, text);
}

static gboolean tts_events(gint fd, GIOCondition c, gpointer d)
{
    TTS_EVENT_T e;
    (void)fd;
    (void)c;
    (void)d;
    while (tts && TextToSpeechGetEvent(tts, &e)) {
        if (e.uiMsg == TTS_MSG_INDEX_MARK) index_mark(e.lParam1, e.lParam2);
        else if (e.uiMsg == TTS_MSG_STATUS) status(e.lParam1);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- files ---- */

static char *choose(int save, const char *title, const char *pattern, const char *pattern_name)
{
    GtkWidget *d = gtk_file_chooser_dialog_new(title, GTK_WINDOW(win),
                                               save ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN,
                                               "_Cancel", GTK_RESPONSE_CANCEL, save ? "_Save" : "_Open",
                                               GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *f;
    char *path = NULL;
    if (pattern) {
        f = gtk_file_filter_new();
        gtk_file_filter_set_name(f, pattern_name);
        gtk_file_filter_add_pattern(f, pattern);
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), f);
        f = gtk_file_filter_new();
        gtk_file_filter_set_name(f, "All Files");
        gtk_file_filter_add_pattern(f, "*");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), f);
    }
    if (save) gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(d), TRUE);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
    gtk_widget_destroy(d);
    return path;
}

static int load_file(const char *path)
{
    size_t n;
    char *raw = speak_read_file(path, &n), *u, *lf;
    if (!raw) {
        message(GTK_MESSAGE_ERROR, "ERROR", "File open failed.");
        return 0;
    }
    if (g_utf8_validate(raw, (gssize)n, NULL)) u = g_strdup(raw);
    else if (!(u = g_convert(raw, (gssize)n, "UTF-8", "WINDOWS-1252", NULL, NULL, NULL)))
        u = g_convert(raw, (gssize)n, "UTF-8", "ISO-8859-1", NULL, NULL, NULL);
    free(raw);
    lf = u ? speak_from_crlf(u) : NULL;
    g_free(u);
    if (!lf) {
        message(GTK_MESSAGE_ERROR, "ERROR", "The file cannot be read as text.");
        return 0;
    }
    gtk_text_buffer_set_text(buf, lf, -1);
    free(lf);
    gtk_text_buffer_set_modified(buf, FALSE);
    g_free(file_name);
    file_name = g_strdup(path);
    untitled = 0;
    set_title();
    return 1;
}

static int save_to(const char *path)
{
    char *t = buffer_text(0, -1);
    int ok = speak_write_file(path, t, strlen(t));
    g_free(t);
    if (!ok) {
        message(GTK_MESSAGE_ERROR, "ERROR", "File save failed.");
        return 0;
    }
    gtk_text_buffer_set_modified(buf, FALSE);
    return 1;
}

static int save_as(void)
{
    char *path = choose(1, "Save File As", "*.txt", "Text Files");
    int ok = path && save_to(path);
    if (ok) {
        g_free(file_name);
        file_name = path;
        untitled = 0;
        set_title();
    } else {
        g_free(path);
    }
    return ok;
}

static int save(void) { return untitled ? save_as() : save_to(file_name); }

static int ask_to_save(void)                    /* 0: go on, 1: cancel */
{
    GtkWidget *d;
    int r;
    if (!gtk_text_buffer_get_modified(buf)) return 0;
    d = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                               "The Text in this file has changed.\n\nDo you wish to save the changes?");
    gtk_dialog_add_buttons(GTK_DIALOG(d), "_Yes", GTK_RESPONSE_YES, "_No", GTK_RESPONSE_NO, "_Cancel",
                           GTK_RESPONSE_CANCEL, NULL);
    r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (r == GTK_RESPONSE_YES) return !save();
    return r != GTK_RESPONSE_NO;
}

static void on_new(GtkMenuItem *m, gpointer d)
{
    (void)m;
    (void)d;
    if (ask_to_save()) return;
    gtk_text_buffer_set_text(buf, "", -1);
    gtk_text_buffer_set_modified(buf, FALSE);
    untitled = 1;
    set_title();
}

static void on_open(GtkMenuItem *m, gpointer d)
{
    char *path;
    (void)m;
    (void)d;
    if (ask_to_save() || !(path = choose(0, "Open a File", "*.txt", "Text Files"))) return;
    if (load_file(path)) TextToSpeechReset(tts, FALSE);
    g_free(path);
}

static void on_save(GtkMenuItem *m, gpointer d) { (void)m; (void)d; save(); }
static void on_save_as(GtkMenuItem *m, gpointer d) { (void)m; (void)d; save_as(); }

static void on_load_dict(GtkMenuItem *m, gpointer d)
{
    char *path = choose(0, "Load a Dictionary", "*.txt", "User Dictionaries");
    MMRESULT r;
    (void)m;
    (void)d;
    if (!path) return;
    TextToSpeechUnloadUserDictionary(tts);
    r = TextToSpeechLoadUserDictionary(tts, path);
    g_free(path);
    if (r == MMSYSERR_ERROR) message(GTK_MESSAGE_ERROR, "Load User Dictionary", "The dictionary cannot be read.");
    else if (r) tts_error(r, "Load User Dictionary");
}

static void on_unload_dict(GtkMenuItem *m, gpointer d)
{
    MMRESULT r = TextToSpeechUnloadUserDictionary(tts);
    (void)m;
    (void)d;
    if (r) tts_error(r, "Unload User Dictionary");
}

static void on_wave(GtkMenuItem *m, gpointer data)
{
    int k = GPOINTER_TO_INT(data);
    char *path = choose(1, "Convert to Wave File", "*.wav", "Wave Files"), *u, *t;
    const char *what;
    MMRESULT r;
    (void)m;
    if (!path) return;
    u = buffer_text(0, -1);
    t = to_dectalk(u);
    stop();
    r = speak_to_wave(tts, t, path, speak_wave_formats[k].format, &what);
    g_free(t);
    g_free(u);
    g_free(path);
    if (r) tts_error(r, what);
}

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w;
    (void)e;
    (void)d;
    return ask_to_save() ? TRUE : FALSE;
}

static void on_quit(GtkMenuItem *m, gpointer d)
{
    (void)m;
    (void)d;
    if (!ask_to_save()) gtk_widget_destroy(win);
}

/* ---- edit ---- */

static void on_cut(GtkMenuItem *m, gpointer d)
{
    (void)m;
    (void)d;
    gtk_text_buffer_cut_clipboard(buf, gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), TRUE);
}
static void on_copy(GtkMenuItem *m, gpointer d)
{
    (void)m;
    (void)d;
    gtk_text_buffer_copy_clipboard(buf, gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
}
static void on_paste(GtkMenuItem *m, gpointer d)
{
    (void)m;
    (void)d;
    gtk_text_buffer_paste_clipboard(buf, gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), NULL, TRUE);
}
static void on_delete_sel(GtkMenuItem *m, gpointer d)
{
    (void)m;
    (void)d;
    gtk_text_buffer_delete_selection(buf, TRUE, TRUE);
}
static void on_select_all(GtkMenuItem *m, gpointer d)
{
    GtkTextIter a, b;
    (void)m;
    (void)d;
    gtk_text_buffer_get_bounds(buf, &a, &b);
    gtk_text_buffer_select_range(buf, &a, &b);
    gtk_widget_grab_focus(view);
}
/* ---- highlighting on or off: the menu item and the box show the same; the choice is kept in speak.ini ---- */

static char *settings_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "dtc01", "speak.ini", NULL);
}

static void load_settings(void)
{
    GKeyFile *k = g_key_file_new();
    char *path = settings_path();
    GError *e = NULL;
    highlight = SPEAK_HIGHLIGHT_DEFAULT;
    if (g_key_file_load_from_file(k, path, G_KEY_FILE_NONE, NULL)) {
        gboolean v = g_key_file_get_boolean(k, "speak", "highlight", &e);
        if (!e) highlight = v;
        g_clear_error(&e);
    }
    g_free(path);
    g_key_file_free(k);
}

static void save_settings(void)
{
    GKeyFile *k = g_key_file_new();
    char *path = settings_path(), *dir = g_path_get_dirname(path);
    g_key_file_load_from_file(k, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_set_boolean(k, "speak", "highlight", highlight);
    g_mkdir_with_parents(dir, 0700);
    g_key_file_save_to_file(k, path, NULL);
    g_free(dir);
    g_free(path);
    g_key_file_free(k);
}

static void set_highlight(int on)
{
    static int busy;                            /* setting the other control calls back here */
    GtkTextIter a, b;
    if (busy) return;
    busy = 1;
    highlight = on && can_highlight;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(hl_item), highlight);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hl_box), highlight);
    if (!highlight && marks.nwords && gtk_text_buffer_get_selection_bounds(buf, &a, &b))
        gtk_text_buffer_place_cursor(buf, &b);  /* off while speaking: the selection goes now */
    save_settings();
    busy = 0;
}

static void on_highlight(GtkCheckMenuItem *m, gpointer d)
{
    (void)d;
    set_highlight(gtk_check_menu_item_get_active(m));
}

static void on_highlight_box(GtkToggleButton *t, gpointer d)
{
    (void)d;
    set_highlight(gtk_toggle_button_get_active(t));
}

/* find, on the UTF-8 text (ASCII letters match either case) */
static void find_next(void)
{
    GtkTextIter a, b;
    const char *what = gtk_entry_get_text(GTK_ENTRY(find_entry));
    char *t = buffer_text(0, -1);
    size_t at, from;
    int flags = (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find_up)) ? 0 : SPEAK_FIND_DOWN) |
                (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find_case)) ? SPEAK_FIND_CASE : 0) |
                (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find_word)) ? SPEAK_FIND_WORD : 0);
    gtk_text_buffer_get_selection_bounds(buf, &a, &b);
    from = (size_t)(g_utf8_offset_to_pointer(t, gtk_text_iter_get_offset(flags & SPEAK_FIND_DOWN ? &b : &a)) - t);
    if (*what && speak_find(t, strlen(t), what, from, flags, &at)) {
        glong s = g_utf8_pointer_to_offset(t, t + at), e = g_utf8_pointer_to_offset(t, t + at + strlen(what));
        gtk_text_buffer_get_iter_at_offset(buf, &a, (gint)s);
        gtk_text_buffer_get_iter_at_offset(buf, &b, (gint)e);
        gtk_text_buffer_select_range(buf, &a, &b);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(view), &a, 0.1, FALSE, 0, 0);
    } else {
        char *m = g_strdup_printf("Search string \"%s\" was not found.", what);
        message(GTK_MESSAGE_INFO, "Find Error", m);
        g_free(m);
    }
    g_free(t);
}

static void find_response(GtkDialog *d, gint r, gpointer data)
{
    (void)data;
    if (r == 1) {
        find_next();
        return;
    }
    gtk_widget_destroy(GTK_WIDGET(d));
    find_dlg = NULL;
}

static void on_find(GtkMenuItem *m, gpointer d)
{
    GtkWidget *box, *row, *down;
    GtkTextIter a, b;
    (void)m;
    (void)d;
    if (find_dlg) {
        gtk_window_present(GTK_WINDOW(find_dlg));
        return;
    }
    find_dlg = gtk_dialog_new_with_buttons("Find", GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, "_Find Next", 1,
                                           "_Close", GTK_RESPONSE_CLOSE, NULL);
    box = gtk_dialog_get_content_area(GTK_DIALOG(find_dlg));
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new_with_mnemonic("Fi_nd what:"), FALSE, FALSE, 0);
    find_entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(find_entry), TRUE);
    gtk_box_pack_start(GTK_BOX(row), find_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 6);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    find_word = gtk_check_button_new_with_mnemonic("Match _whole word only");
    find_case = gtk_check_button_new_with_mnemonic("Match _case");
    find_up = gtk_radio_button_new_with_mnemonic(NULL, "_Up");
    down = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(find_up), "_Down");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(down), TRUE);
    gtk_box_pack_start(GTK_BOX(row), find_word, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), find_case, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), down, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), find_up, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 6);
    gtk_dialog_set_default_response(GTK_DIALOG(find_dlg), 1);
    if (gtk_text_buffer_get_selection_bounds(buf, &a, &b)) {
        char *s = gtk_text_buffer_get_text(buf, &a, &b, FALSE);
        gtk_entry_set_text(GTK_ENTRY(find_entry), s);
        g_free(s);
    }
    g_signal_connect(find_dlg, "response", G_CALLBACK(find_response), NULL);
    gtk_widget_show_all(find_dlg);
}

static void on_about(GtkMenuItem *m, gpointer d)
{
    LPSTR vs;
    DWORD v = TextToSpeechVersion(&vs);
    char *t = g_strdup_printf("SPEAK\nSpeaking Text Editor\n\ndapi's sample program, ported to the DTC-01 v1.8 "
                              "library\n\nDECtalk %s\nLibrary version %lu.%02lu (build %lX)\n\nGTK %u.%u.%u",
                              vs, (unsigned long)((v >> 24) & 0x7f), (unsigned long)((v >> 16) & 0xff),
                              (unsigned long)(v & 0xffff), gtk_get_major_version(), gtk_get_minor_version(),
                              gtk_get_micro_version());
    (void)m;
    (void)d;
    message(GTK_MESSAGE_INFO, "About Speak", t);
    g_free(t);
}

/* ---- the window ---- */

static GtkWidget *item(GtkWidget *menu, const char *label, GCallback cb, gpointer data, GtkAccelGroup *acc, guint key)
{
    GtkWidget *i = gtk_menu_item_new_with_mnemonic(label);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), i);
    if (cb) g_signal_connect(i, "activate", cb, data);
    if (acc && key) gtk_widget_add_accelerator(i, "activate", acc, key, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    return i;
}

static GtkWidget *submenu(GtkWidget *bar, const char *label)
{
    GtkWidget *m = gtk_menu_new(), *i = gtk_menu_item_new_with_mnemonic(label);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(i), m);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), i);
    return m;
}

static GtkWidget *menus(void)
{
    GtkAccelGroup *acc = gtk_accel_group_new();
    GtkWidget *bar = gtk_menu_bar_new(), *m, *w;
    int k;
    gtk_window_add_accel_group(GTK_WINDOW(win), acc);
    m = submenu(bar, "_File");
    item(m, "_New", G_CALLBACK(on_new), NULL, acc, GDK_KEY_n);
    item(m, "_Open...", G_CALLBACK(on_open), NULL, acc, GDK_KEY_o);
    item(m, "_Save", G_CALLBACK(on_save), NULL, acc, GDK_KEY_s);
    item(m, "S_ave as...", G_CALLBACK(on_save_as), NULL, NULL, 0);
    item(m, "_Close", G_CALLBACK(on_new), NULL, NULL, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    item(m, "_Load User Dictionary...", G_CALLBACK(on_load_dict), NULL, NULL, 0);
    item(m, "_Unload User Dictionary", G_CALLBACK(on_unload_dict), NULL, NULL, 0);
    w = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item(m, "_Convert to Wave File", NULL, NULL, NULL, 0)), w);
    for (k = 0; k < SPEAK_NWAVE; k++) {
        char *l = g_convert(speak_wave_formats[k].label, -1, "UTF-8", "ISO-8859-1", NULL, NULL, NULL);
        char *l3 = g_strconcat(l ? l : "", "...", NULL);
        item(w, l3, G_CALLBACK(on_wave), GINT_TO_POINTER(k), NULL, 0);
        g_free(l3);
        g_free(l);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    item(m, "E_xit", G_CALLBACK(on_quit), NULL, acc, GDK_KEY_q);
    m = submenu(bar, "_Edit");
    item(m, "Cu_t", G_CALLBACK(on_cut), NULL, NULL, 0);
    item(m, "_Copy", G_CALLBACK(on_copy), NULL, NULL, 0);
    item(m, "_Paste", G_CALLBACK(on_paste), NULL, NULL, 0);
    item(m, "De_lete", G_CALLBACK(on_delete_sel), NULL, NULL, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    item(m, "Select _All", G_CALLBACK(on_select_all), NULL, NULL, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    item(m, "_Find...", G_CALLBACK(on_find), NULL, acc, GDK_KEY_f);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    hl_item = gtk_check_menu_item_new_with_mnemonic("_Highlighting");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(hl_item), highlight);
    gtk_widget_set_sensitive(hl_item, can_highlight);
    g_signal_connect(hl_item, "toggled", G_CALLBACK(on_highlight), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), hl_item);
    m = submenu(bar, "_Help");
    item(m, "_About Speak", G_CALLBACK(on_about), NULL, NULL, 0);
    return bar;
}

static GtkWidget *picture_button(int i, const char *tip)
{
    GtkWidget *b = gtk_button_new();
    pics[i] = gtk_image_new_from_pixbuf(pix[i][0]);
    gtk_button_set_image(GTK_BUTTON(b), pics[i]);
    gtk_button_set_always_show_image(GTK_BUTTON(b), TRUE);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_can_focus(b, FALSE);
    gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "pressed", G_CALLBACK(pressed), GINT_TO_POINTER(i));
    g_signal_connect(b, "released", G_CALLBACK(released), GINT_TO_POINTER(i));
    g_signal_connect(b, "clicked", G_CALLBACK(clicked), GINT_TO_POINTER(i));
    return b;
}

static void build_window(void)
{
    static const char *tips[] = {"Play", "Pause / Resume", "Stop"};
    GtkWidget *box, *faces, *scroll, *bottom;
    int i;
    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), 640, 480);
    g_signal_connect(win, "delete-event", G_CALLBACK(on_delete), NULL);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    for (i = 0; i < SPEAK_NPICS; i++) {
        pix[i][0] = load_pic(&speak_pics[i][0]);
        pix[i][1] = load_pic(&speak_pics[i][1]);
    }
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win), box);
    gtk_box_pack_start(GTK_BOX(box), menus(), FALSE, FALSE, 0);

    faces = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    for (i = 0; i < SPEAK_NVOICES; i++)
        gtk_box_pack_start(GTK_BOX(faces), picture_button(i, speak_voices[i].label), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), faces, FALSE, FALSE, 2);

    view = gtk_text_view_new();
    buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    g_signal_connect(view, "button-press-event", G_CALLBACK(view_button), NULL);
    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 2);

    bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(bottom), gtk_label_new("Speaking Rate"), FALSE, FALSE, 4);
    rate_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, SPEAK_RATE_MIN, SPEAK_RATE_MAX, SPEAK_RATE_LINE);
    gtk_range_set_increments(GTK_RANGE(rate_scale), SPEAK_RATE_LINE, SPEAK_RATE_PAGE);
    gtk_scale_set_draw_value(GTK_SCALE(rate_scale), FALSE);
    gtk_widget_set_size_request(rate_scale, 160, -1);
    gtk_range_set_value(GTK_RANGE(rate_scale), SPEAK_RATE_DEFAULT);
    g_signal_connect(rate_scale, "change-value", G_CALLBACK(rate_change), NULL);
    g_signal_connect(rate_scale, "format-value", G_CALLBACK(rate_format), NULL);
    gtk_box_pack_start(GTK_BOX(bottom), rate_scale, FALSE, FALSE, 0);
    rate_text = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(bottom), rate_text, FALSE, FALSE, 4);
    hl_box = gtk_check_button_new_with_mnemonic("Highlight _words");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hl_box), highlight);
    gtk_widget_set_sensitive(hl_box, can_highlight);
    gtk_widget_set_can_focus(hl_box, FALSE);
    g_signal_connect(hl_box, "toggled", G_CALLBACK(on_highlight_box), NULL);
    gtk_box_pack_start(GTK_BOX(bottom), hl_box, FALSE, FALSE, 8);
    for (i = SPEAK_PIC_STOP; i >= SPEAK_PIC_PLAY; i--)
        gtk_box_pack_end(GTK_BOX(bottom), picture_button(i, tips[i - SPEAK_PIC_PLAY]), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bottom, FALSE, FALSE, 2);
    g_signal_connect(rate_scale, "value-changed", G_CALLBACK(rate_changed), NULL);
    rate_changed(GTK_RANGE(rate_scale), NULL);
}

static int start_tts(void)
{
    TTS_WAITABLE_T fd;
    MMRESULT r = TextToSpeechStartupEx(&tts, WAVE_MAPPER, REPORT_OPEN_ERROR | TTS_EVENT_QUEUE, NULL, 0);
    char s[128];
    if (r == MMSYSERR_NODRIVER || r == WAVERR_BADFORMAT || r == MMSYSERR_BADDEVICEID) {
        message(GTK_MESSAGE_WARNING, "Warning",
                "No compatible wave device present\nYou can continue but only to write wave files");
        r = TextToSpeechStartupEx(&tts, 0, DO_NOT_USE_AUDIO_DEVICE | TTS_EVENT_QUEUE, NULL, 0);
    }
    if (r == MMSYSERR_NOERROR) r = TextToSpeechGetEventHandle(tts, &fd);
    if (r) {
        g_snprintf(s, sizeof s, "TTS startup failed\n\nError = %u\n\nExiting out of application", (unsigned)r);
        message(GTK_MESSAGE_ERROR, "Speak cannot be started.", s);
        return 0;
    }
    g_unix_fd_add(fd, G_IO_IN, tts_events, NULL);
    TextToSpeechSetRate(tts, SPEAK_RATE_DEFAULT);
    return 1;
}

int main(int argc, char **argv)
{
    const char *file, *dict;
    char msg[160];
    int i;
    gtk_init(&argc, &argv);
    if (speak_parse_args(argc, argv, &file, &dict)) {
        g_print("%s\n", SPEAK_USAGE);
        return 0;
    }
    if (!speak_check_version(&can_highlight, msg, sizeof msg)) {
        g_printerr("%s\n", msg);
        return 1;
    }
    load_settings();
    if (!can_highlight) highlight = 0;
    build_window();
    set_title();
    gtk_widget_show_all(win);
    if (!start_tts()) return 1;
    if (dict) {
        MMRESULT r;
        TextToSpeechUnloadUserDictionary(tts);
        if ((r = TextToSpeechLoadUserDictionary(tts, (LPSTR)dict)) != MMSYSERR_NOERROR)
            message(GTK_MESSAGE_ERROR, "Load User Dictionary",
                    r == MMSYSERR_ERROR ? "The dictionary cannot be read." : "Error in TTS Load Dictionary");
    }
    if (file && load_file(file)) speak_buffer();
    gtk_widget_grab_focus(view);
    gtk_main();
    TextToSpeechShutdown(tts);
    tts = NULL;
    speak_marks_free(&marks);
    for (i = 0; i < SPEAK_NPICS; i++) {
        if (pix[i][0]) g_object_unref(pix[i][0]);
        if (pix[i][1]) g_object_unref(pix[i][1]);
    }
    g_free(file_name);
    return 0;
}
