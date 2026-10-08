/*
 * window.c -- the main window: the display, a header-bar menu, keyboard
 * capture, the console switches, gamepad hot-plug toasts and the FujiNet
 * status.
 *
 * Keyboard events are translated by hardware keycode (evdev, via the
 * session's HID table) rather than by GDK keyval, so a binding names the
 * physical key whatever Shift is doing and whatever layout is active -- the
 * same path the Qt, Win32 and AppKit frontends take, which is what lets one
 * tested table serve all four.
 *
 * Select, Reset and Pause (F1-F3) are key BINDINGS, so the menu items only
 * name their keys: registering them as accelerators too would fire each
 * switch twice per keystroke. The difficulty switches have no binding by
 * default and are Alt+L / Alt+R accelerators instead.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include "display.h"
#include "fujilog.h"
#include "prefs.h"
#include "debugger/dbg_window.h"
#include "controllers/controllers_window.h"

#include <stdlib.h>
#include <string.h>

struct _A7800Window {
    AdwApplicationWindow parent_instance;

    a7800session *session;
    GtkWidget *display;
    GtkWidget *toast_overlay;
    GtkWidget *status;          /* the FujiNet link indicator */
    GtkWidget *status_dot;
    guint status_id;
    guint sysact_id;
    guint shot_id;
    gboolean sysact_down[A7800_SYSACT_COUNT];
    unsigned pad_generation;
    gboolean fullscreen;
};

G_DEFINE_FINAL_TYPE(A7800Window, a7800_window, ADW_TYPE_APPLICATION_WINDOW)

void a7800_window_toast(A7800Window *self, const char *text)
{
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toast_overlay),
                                adw_toast_new(text));
}

void a7800_install_accent_css(void)
{
    static gboolean done;
    GtkCssProvider *css;
    char buf[512];
    if (done) return;
    done = TRUE;
    g_snprintf(buf, sizeof buf,
        ".a7800-accent { background: #%06x; color: #000000; }\n"
        ".a7800-accent:hover { background: #%06x; }\n"
        ".a7800-accent-text { color: #%06x; font-weight: bold; }\n"
        ".a7800-dot { border-radius: 6px; min-width: 12px; min-height: 12px; }\n"
        ".a7800-dot-on { background: #%06x; }\n"
        ".a7800-dot-off { background: #808080; }\n",
        A7800SESSION_ACCENT_RGB, A7800SESSION_ACCENT_RGB,
        A7800SESSION_ACCENT_RGB, A7800SESSION_ACCENT_RGB);
    css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, buf);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* ---- input ---------------------------------------------------------------- */

static guint32 keysym_of(guint keyval, guint keycode)
{
    /* the hardware key first; a keyval only for keys evdev has no HID
     * usage for (rare: media keys) */
    guint32 k = keycode >= 8 ? a7800session_keysym_from_evdev(keycode - 8) : 0;
    return k ? k : keyval;
}

static void run_sysaction(A7800Window *self, int sa)
{
    switch (sa) {
    case A7800_SYSACT_REBOOT_CONFIG:
        a7800session_sysaction(self->session, sa);
        a7800_window_toast(self, "Back to the FujiNet CONFIG client");
        break;
    case A7800_SYSACT_PAUSE:
        /* the debugger window attaches, which stops the machine */
        a7800_debugger_show(GTK_WINDOW(self), self->session);
        break;
    default:
        break;
    }
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    A7800Window *self = user_data;
    guint32 keysym;
    int sa;
    (void)ctrl;

    /* The window's own keys, deliberately not bindable: they are how you
     * reach the panels that do the binding. */
    if (keyval == GDK_KEY_F9) {
        a7800_controllers_window_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    }
    if (keyval == GDK_KEY_F12) {
        a7800_debugger_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    }
    if (keyval == GDK_KEY_F11) {
        gtk_widget_activate_action(GTK_WIDGET(self), "win.fullscreen", NULL);
        return TRUE;
    }
    if ((state & GDK_CONTROL_MASK) || (state & GDK_ALT_MASK))
        return FALSE;   /* menu accelerators */

    keysym = keysym_of(keyval, keycode);
    /* A system action is checked BEFORE the machine keys, so Escape always
     * gets back to CONFIG whatever else the key table says. Leading edge
     * only: GTK4 has no repeat flag. */
    sa = a7800session_key_sysaction(self->session, keysym);
    if (sa >= 0) {
        if (!self->sysact_down[sa]) {
            self->sysact_down[sa] = TRUE;
            run_sysaction(self, sa);
        }
        return TRUE;
    }
    return a7800session_key(self->session, keysym, 1) ? TRUE : FALSE;
}

static gboolean on_key_released(GtkEventControllerKey *ctrl, guint keyval,
                                guint keycode, GdkModifierType state,
                                gpointer user_data)
{
    A7800Window *self = user_data;
    guint32 keysym;
    int sa;
    (void)ctrl; (void)state;

    if (keyval == GDK_KEY_F9 || keyval == GDK_KEY_F11 || keyval == GDK_KEY_F12)
        return TRUE;
    keysym = keysym_of(keyval, keycode);
    sa = a7800session_key_sysaction(self->session, keysym);
    if (sa >= 0) {
        self->sysact_down[sa] = FALSE;
        return TRUE;
    }
    return a7800session_key(self->session, keysym, 0) ? TRUE : FALSE;
}

/* Losing focus with keys held would leave the machine believing they are
 * still down. */
static void on_focus_leave(GtkEventControllerFocus *ctrl, gpointer user_data)
{
    A7800Window *self = user_data;
    (void)ctrl;
    a7800session_release_all(self->session);
    memset(self->sysact_down, 0, sizeof self->sysact_down);
}

/* The difficulty switches can be flipped from the menu, the Controllers
 * window, a bound key or a gamepad: the menu's check marks follow the
 * session rather than remembering their own state. */
static void sync_switches(A7800Window *self)
{
    static const struct { const char *action; int sw; } diffs[] = {
        { "left-diff", A7800_SW_LEFT_DIFF }, { "right-diff", A7800_SW_RIGHT_DIFF } };
    unsigned i;
    for (i = 0; i < G_N_ELEMENTS(diffs); i++) {
        GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), diffs[i].action);
        gboolean on = a7800session_switch_get(self->session, diffs[i].sw) != 0;
        g_autoptr(GVariant) cur = g_action_get_state(a);
        if (g_variant_get_boolean(cur) != on)
            g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_boolean(on));
    }
}

/* The gamepad thread cannot call into GTK; it posts system actions and this
 * timer takes them. It also watches for gamepads coming and going, and says
 * so: a pad that drops off Bluetooth mid-game should not be a mystery. */
static gboolean sysact_drain_tick(gpointer user_data)
{
    A7800Window *self = user_data;
    unsigned gen;
    int sa;
    while (a7800session_sysaction_take(self->session, &sa))
        run_sysaction(self, sa);

    gen = a7800session_gamepad_generation(self->session);
    if (gen != self->pad_generation) {
        char text[160];
        self->pad_generation = gen;
        if (a7800session_gamepad_last_event(self->session, text, sizeof text) > 0)
            a7800_window_toast(self, text);
    }
    sync_switches(self);
    return G_SOURCE_CONTINUE;
}

/* ---- status --------------------------------------------------------------- */

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static gboolean update_status(gpointer user_data)
{
    A7800Window *self = user_data;
    char text[320], st[160];
    gboolean on = FALSE;

    if (!a7800session_is_running(self->session)) {
        g_snprintf(text, sizeof text, "Stopped");
    } else {
        const char *cart = a7800session_cart_path(self->session);
        int region = a7800session_running_region(self->session);
        int bios = a7800session_bios(self->session, region);
        gboolean has_bios = bios >= 0 && a7800session_bios_available(self->session, bios);
        a7800session_cart_status(self->session, st, sizeof st);
        on = a7800session_cart_link_up(self->session) == 1;
        g_snprintf(text, sizeof text, "%s \xe2\x80\x94 FujiNet %s \xc2\xb7 %s \xc2\xb7 %s",
                   cart && *cart ? base_name(cart) : "CONFIG", st,
                   a7800_region_name(region), has_bios ? "BIOS" : "no BIOS");
    }
    gtk_label_set_text(GTK_LABEL(self->status), text);
    if (on) {
        gtk_widget_add_css_class(self->status_dot, "a7800-dot-on");
        gtk_widget_remove_css_class(self->status_dot, "a7800-dot-off");
    } else {
        gtk_widget_add_css_class(self->status_dot, "a7800-dot-off");
        gtk_widget_remove_css_class(self->status_dot, "a7800-dot-on");
    }
    return G_SOURCE_CONTINUE;
}

/* ---- cartridges ------------------------------------------------------------ */

/* A cartridge the FujiNet cartridge cannot run (a board it does not emulate,
 * an image too big for it) says why in a dialog: a toast would vanish before
 * the reason was read, and the reason is the whole point. */
static void show_error(A7800Window *self, const char *heading, const char *body)
{
    AdwDialog *dlg = adw_alert_dialog_new(heading, body);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "ok", "OK");
    adw_dialog_present(dlg, GTK_WIDGET(self));
}

static void open_cart_path(A7800Window *self, const char *path)
{
    char msg[1200];
    if (a7800session_load_cart(self->session, path) != 0) {
        show_error(self, "Cannot Open Cartridge", a7800session_last_error(self->session));
        return;
    }
    g_snprintf(msg, sizeof msg, "Running %s", base_name(path));
    a7800_window_toast(self, msg);
    update_status(self);
}

static void import_rom_path(A7800Window *self, const char *path)
{
    char what[128], msg[300];
    if (a7800session_import_rom(self->session, path, what, sizeof what) != 0) {
        show_error(self, "Cannot Import BIOS", a7800session_last_error(self->session));
        return;
    }
    g_snprintf(msg, sizeof msg, "Imported the %s \xe2\x80\x94 choose it in Preferences", what);
    a7800_window_toast(self, msg);
}

/* A dropped file: a cartridge is copied to the cartridge folder and runs, a
 * BIOS or High Score Cart ROM is imported, anything else goes to FujiNet's
 * SD folder. */
static void load_media(A7800Window *self, const char *path)
{
    char dest[1024];

    if (a7800session_media_is_rom(path)) {
        import_rom_path(self, path);
        return;
    }
    if (a7800session_import_media(self->session, path, dest, sizeof dest) != 0) {
        a7800_window_toast(self, a7800session_last_error(self->session));
        return;
    }
    if (a7800session_media_is_cartridge(path)) {
        open_cart_path(self, dest);
        return;
    }
    a7800_window_toast(self, "Copied to FujiNet's SD folder \xe2\x80\x94 mount it "
                             "from the CONFIG client");
}

static GtkFileDialog *file_dialog(const char *title, const char *filter_name,
                                  const char *const *patterns)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *carts = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    int i;

    gtk_file_filter_set_name(carts, filter_name);
    for (i = 0; patterns[i]; i++) {
        char *upper = g_ascii_strup(patterns[i], -1);
        gtk_file_filter_add_pattern(carts, patterns[i]);
        gtk_file_filter_add_pattern(carts, upper);
        g_free(upper);
    }
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, carts);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    g_object_unref(carts);
    g_object_unref(all);
    g_object_unref(filters);
    return dlg;
}

static GtkFileDialog *cart_dialog(const char *title)
{
    static const char *const pats[] = { "*.a78", "*.bin", "*.zip", "*.7z", NULL };
    return file_dialog(title, "Atari 7800 cartridges (*.a78, *.bin, *.zip, *.7z)", pats);
}

static void on_cart_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    A7800Window *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    if (!file) return;
    path = g_file_get_path(file);
    if (path) open_cart_path(self, path);
}

static void action_open(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A7800Window *self = user_data;
    GtkFileDialog *dlg = cart_dialog("Open Cartridge");
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_cart_chosen, self);
    g_object_unref(dlg);
}

static void on_sd_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    A7800Window *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    char dest[1024], msg[1200];
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    if (a7800session_import_cart_to_sd(self->session, path, dest, sizeof dest) != 0) {
        a7800_window_toast(self, a7800session_last_error(self->session));
        return;
    }
    g_snprintf(msg, sizeof msg, "%s is on the SD host \xe2\x80\x94 boot it from "
               "the CONFIG client", base_name(dest));
    a7800_window_toast(self, msg);
}

static void action_import_sd(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A7800Window *self = user_data;
    GtkFileDialog *dlg = cart_dialog("Import Cartridge to SD");
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_sd_chosen, self);
    g_object_unref(dlg);
}

static void on_bios_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    A7800Window *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    if (!file) return;
    path = g_file_get_path(file);
    if (path) import_rom_path(self, path);
}

static void action_import_bios(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    static const char *const pats[] = { "*.bin", "*.rom", "*.u7", "*.a78", "*.zip", "*.7z", NULL };
    A7800Window *self = user_data;
    GtkFileDialog *dlg = file_dialog("Import BIOS",
        "BIOS and High Score Cart images (*.bin, *.rom, *.u7, *.a78, *.zip, *.7z)", pats);
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_bios_chosen, self);
    g_object_unref(dlg);
}

static void action_eject(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A7800Window *self = user_data;
    (void)a; (void)p;
    a7800session_eject(self->session);
    a7800_window_toast(self, "Cartridge ejected \xe2\x80\x94 back to CONFIG");
    update_status(self);
}

static void action_power_cycle(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A7800Window *self = user_data;
    (void)a; (void)p;
    a7800session_power_cycle(self->session);
    a7800_window_toast(self, "Power cycled");
}

static void action_reboot_config(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    run_sysaction(user_data, A7800_SYSACT_REBOOT_CONFIG);
    update_status(user_data);
}

static gboolean on_drop(GtkDropTarget *t, const GValue *value, double x,
                        double y, gpointer user_data)
{
    A7800Window *self = user_data;
    g_autofree char *path = NULL;
    (void)t; (void)x; (void)y;
    if (!G_VALUE_HOLDS(value, G_TYPE_FILE)) return FALSE;
    path = g_file_get_path(G_FILE(g_value_get_object(value)));
    if (!path) return FALSE;
    load_media(self, path);
    return TRUE;
}

/* ---- the console switches ------------------------------------------------- */

static void action_select(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A7800Window *self = user_data;
    (void)a; (void)p;
    a7800session_switch_pulse(self->session, A7800_SW_SELECT);
}

static void action_reset(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A7800Window *self = user_data;
    (void)a; (void)p;
    a7800session_switch_pulse(self->session, A7800_SW_RESET);
}

static void action_pause(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A7800Window *self = user_data;
    (void)a; (void)p;
    a7800session_switch_pulse(self->session, A7800_SW_PAUSE);
}

static void toggle_difficulty(GSimpleAction *a, A7800Window *self, int sw)
{
    gboolean on = !a7800session_switch_get(self->session, sw);
    a7800session_switch_set(self->session, sw, on);
    g_simple_action_set_state(a, g_variant_new_boolean(on));
}

static void action_left_diff(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)p;
    toggle_difficulty(a, user_data, A7800_SW_LEFT_DIFF);
}

static void action_right_diff(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)p;
    toggle_difficulty(a, user_data, A7800_SW_RIGHT_DIFF);
}

/* ---- view, FujiNet and the application ------------------------------------ */

static void action_controllers(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A7800Window *self = user_data;
    (void)a; (void)p;
    a7800_controllers_window_toggle(GTK_WINDOW(self), self->session);
}

static void action_debugger(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A7800Window *self = user_data;
    (void)a; (void)p;
    a7800_debugger_toggle(GTK_WINDOW(self), self->session);
}

static void action_fullscreen(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A7800Window *self = user_data;
    (void)a; (void)p;
    self->fullscreen = !self->fullscreen;
    if (self->fullscreen) gtk_window_fullscreen(GTK_WINDOW(self));
    else gtk_window_unfullscreen(GTK_WINDOW(self));
}

static void action_fujinet_config(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A7800Window *self = user_data;
    (void)a; (void)p;
    if (!a7800session_fujinet_running(self->session)) {
        a7800_window_toast(self, "FujiNet is not running");
        return;
    }
    a7800_fujiconfig_show(GTK_WINDOW(self), self->session);
}

static void action_fujinet_log(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A7800Window *self = user_data;
    (void)a; (void)p;
    a7800_fujilog_show(GTK_WINDOW(self), self->session);
}

/* Stop, re-read the settings store, start. Preferences hands this to
 * a7800_prefs_show() as its close callback. */
static void restart_session(A7800Window *self)
{
    a7800session_start_opts o;
    a7800session_settings_flush(self->session);
    a7800session_default_opts(self->session, &o);
    a7800session_stop(self->session);
    if (a7800session_start(self->session, &o) != 0) {
        a7800_window_toast(self, a7800session_last_error(self->session));
        return;
    }
    a7800_window_toast(self, "Host options applied (session restarted)");
}

static void action_prefs(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A7800Window *self = user_data;
    (void)a; (void)p;
    a7800_prefs_show(self, self->session, restart_session);
}

static void action_about(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A7800Window *self = user_data;
    AdwDialog *about;
    (void)a; (void)p;
    about = adw_about_dialog_new();
    adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "FujiNet Go Atari 7800");
    adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), a7800_icon_name());
    adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), A7800_VERSION_STRING);
    adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(about), "Thomas Cherryhomes");
    adw_about_dialog_set_website(ADW_ABOUT_DIALOG(about), "https://fujinet.online/");
    adw_about_dialog_set_issue_url(ADW_ABOUT_DIALOG(about),
        "https://github.com/FujiNetWIFI/fujinet-go-atari7800-desktop/issues");
    adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(about), GTK_LICENSE_GPL_3_0);
    adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about),
        "An Atari 7800 with a built-in FujiNet. The emulator is MAME's Atari "
        "7800 (GPL-2.0-or-later; the a7800 driver BSD-3-Clause), by the MAME "
        "team, with the FujiNet 7800 cartridge. No BIOS is needed.");
    adw_dialog_present(about, GTK_WIDGET(self));
}

static void action_aspect(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A7800Window *self = user_data;
    g_autoptr(GVariant) cur = g_action_get_state(G_ACTION(a));
    gboolean tv = !g_variant_get_boolean(cur);
    (void)p;
    g_simple_action_set_state(a, g_variant_new_boolean(tv));
    a7800_display_set_tv_aspect(A7800_DISPLAY(self->display), tv);
    /* "aspect": 0 = the television's 4:3, 1 = square pixels */
    a7800session_set_int(self->session, "aspect", tv ? 0 : 1);
}

static void action_smooth(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A7800Window *self = user_data;
    g_autoptr(GVariant) cur = g_action_get_state(G_ACTION(a));
    gboolean sm = !g_variant_get_boolean(cur);
    (void)p;
    g_simple_action_set_state(a, g_variant_new_boolean(sm));
    a7800_display_set_smooth(A7800_DISPLAY(self->display), sm);
    a7800session_set_int(self->session, "smooth", sm ? 1 : 0);
}

void a7800_window_apply_aspect(A7800Window *self, int aspect)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), "tv-aspect");
    a7800_display_set_tv_aspect(A7800_DISPLAY(self->display), aspect == 0);
    if (a)
        g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_boolean(aspect == 0));
}

static const GActionEntry win_actions[] = {
    { "open", action_open, NULL, NULL, NULL, { 0 } },
    { "eject", action_eject, NULL, NULL, NULL, { 0 } },
    { "import-sd", action_import_sd, NULL, NULL, NULL, { 0 } },
    { "import-bios", action_import_bios, NULL, NULL, NULL, { 0 } },
    { "power-cycle", action_power_cycle, NULL, NULL, NULL, { 0 } },
    { "reboot-config", action_reboot_config, NULL, NULL, NULL, { 0 } },
    { "select", action_select, NULL, NULL, NULL, { 0 } },
    { "reset", action_reset, NULL, NULL, NULL, { 0 } },
    { "pause", action_pause, NULL, NULL, NULL, { 0 } },
    { "left-diff", action_left_diff, NULL, "true", NULL, { 0 } },
    { "right-diff", action_right_diff, NULL, "true", NULL, { 0 } },
    { "controllers", action_controllers, NULL, NULL, NULL, { 0 } },
    { "debugger", action_debugger, NULL, NULL, NULL, { 0 } },
    { "fullscreen", action_fullscreen, NULL, NULL, NULL, { 0 } },
    { "tv-aspect", action_aspect, NULL, "true", NULL, { 0 } },
    { "smooth", action_smooth, NULL, "false", NULL, { 0 } },
    { "fujinet-config", action_fujinet_config, NULL, NULL, NULL, { 0 } },
    { "fujinet-log", action_fujinet_log, NULL, NULL, NULL, { 0 } },
    { "prefs", action_prefs, NULL, NULL, NULL, { 0 } },
    { "about", action_about, NULL, NULL, NULL, { 0 } },
};

/* ---- construction --------------------------------------------------------- */

static GMenu *build_menu(void)
{
    GMenu *menu = g_menu_new();
    GMenu *cart = g_menu_new();
    GMenu *sw = g_menu_new();
    GMenu *view = g_menu_new();
    GMenu *fuji = g_menu_new();
    GMenu *app = g_menu_new();

    g_menu_append(cart, "_Open Cartridge...", "win.open");
    g_menu_append(cart, "_Eject Cartridge", "win.eject");
    g_menu_append(cart, "_Import Cartridge to SD...", "win.import-sd");
    g_menu_append(cart, "Import _BIOS...", "win.import-bios");
    g_menu_append(cart, "_Power Cycle", "win.power-cycle");
    g_menu_append(cart, "Reboot to _CONFIG (Esc)", "win.reboot-config");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(cart));

    g_menu_append(sw, "_Select (F1)", "win.select");
    g_menu_append(sw, "_Reset (F2)", "win.reset");
    g_menu_append(sw, "P_ause (F3)", "win.pause");
    g_menu_append(sw, "_Left Difficulty A", "win.left-diff");
    g_menu_append(sw, "Ri_ght Difficulty A", "win.right-diff");
    g_menu_append_section(menu, "Console Switches", G_MENU_MODEL(sw));

    g_menu_append(view, "_Controllers (F9)", "win.controllers");
    g_menu_append(view, "_Debugger (F12)", "win.debugger");
    g_menu_append(view, "_TV Aspect (4:3)", "win.tv-aspect");
    g_menu_append(view, "S_mooth Scaling", "win.smooth");
    g_menu_append(view, "_Fullscreen (F11)", "win.fullscreen");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(view));

    g_menu_append(fuji, "FujiNet _Web UI", "win.fujinet-config");
    g_menu_append(fuji, "Console _Log", "win.fujinet-log");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(fuji));

    g_menu_append(app, "_Preferences", "win.prefs");
    g_menu_append(app, "_About FujiNet Go Atari 7800", "win.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(app));

    g_object_unref(cart);
    g_object_unref(sw);
    g_object_unref(view);
    g_object_unref(fuji);
    g_object_unref(app);
    return menu;
}

/* ---- A7800_SCREENSHOT: the windows, rendered offscreen, for tests --------- */

/* Renders a widget the way GTK would draw it, with a renderer of its own
 * rather than the window's (on Broadway or a headless CI there is no
 * renderer that can read back), and saves it as a PNG. */
static gboolean save_widget_png(GtkWidget *widget, const char *path)
{
    int w = gtk_widget_get_width(widget), h = gtk_widget_get_height(widget);
    GdkPaintable *paintable;
    GtkSnapshot *snapshot;
    GskRenderNode *node;
    GskRenderer *renderer;
    GdkTexture *texture;
    GError *err = NULL;
    gboolean ok;

    if (w <= 0 || h <= 0)
        return FALSE;
    paintable = gtk_widget_paintable_new(widget);
    snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snapshot, w, h);
    node = gtk_snapshot_free_to_node(snapshot);
    g_object_unref(paintable);
    if (!node)
        return FALSE;

    renderer = gsk_cairo_renderer_new();
#if GTK_CHECK_VERSION(4, 14, 0)
    ok = gsk_renderer_realize_for_display(renderer, gtk_widget_get_display(widget), &err);
#else
    ok = gsk_renderer_realize(renderer, NULL, &err);
#endif
    if (!ok) {
        g_warning("screenshot: %s", err ? err->message : "no renderer");
        g_clear_error(&err);
        g_object_unref(renderer);
        gsk_render_node_unref(node);
        return FALSE;
    }
    texture = gsk_renderer_render_texture(renderer, node, &GRAPHENE_RECT_INIT(0, 0, w, h));
    ok = gdk_texture_save_to_png(texture, path);
    g_object_unref(texture);
    gsk_renderer_unrealize(renderer);
    g_object_unref(renderer);
    gsk_render_node_unref(node);
    return ok;
}

/* The main window to <file>.png and every other visible window of the app
 * to <file>-<title>.png, then quit. */
static gboolean screenshot_tick(gpointer user_data)
{
    A7800Window *self = user_data;
    const char *path = g_getenv("A7800_SCREENSHOT");
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));
    g_autofree char *stem = NULL;
    GList *l;

    self->shot_id = 0;
    stem = g_str_has_suffix(path, ".png") ? g_strndup(path, strlen(path) - 4) : g_strdup(path);
    for (l = app ? gtk_application_get_windows(app) : NULL; l; l = l->next) {
        GtkWidget *win = l->data;
        g_autofree char *file = NULL;
        if (!gtk_widget_get_visible(win))
            continue;
        if (win == GTK_WIDGET(self)) {
            file = g_strdup_printf("%s.png", stem);
        } else {
            const char *title = gtk_window_get_title(GTK_WINDOW(win));
            g_autofree char *slug = g_ascii_strdown(title ? title : "window", -1);
            g_strdelimit(slug, " /", '-');
            file = g_strdup_printf("%s-%s.png", stem, slug);
        }
        if (save_widget_png(win, file))
            g_print("screenshot: %s\n", file);
        else
            g_printerr("screenshot: could not save %s\n", file);
    }
    if (app)
        g_application_quit(G_APPLICATION(app));
    return G_SOURCE_REMOVE;
}

static void a7800_window_dispose(GObject *object)
{
    A7800Window *self = A7800_WINDOW(object);
    if (self->status_id) {
        g_source_remove(self->status_id);
        self->status_id = 0;
    }
    if (self->sysact_id) {
        g_source_remove(self->sysact_id);
        self->sysact_id = 0;
    }
    if (self->shot_id) {
        g_source_remove(self->shot_id);
        self->shot_id = 0;
    }
    G_OBJECT_CLASS(a7800_window_parent_class)->dispose(object);
}

static void a7800_window_class_init(A7800WindowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = a7800_window_dispose;
}

static void a7800_window_init(A7800Window *self)
{
    (void)self;
}

GtkWidget *a7800_window_new(AdwApplication *app, a7800session *session)
{
    A7800Window *self = g_object_new(A7800_TYPE_WINDOW, "application", app, NULL);
    GtkWidget *box, *header, *menu_button, *toolbar, *status_box;
    GtkEventController *keys, *focus;
    g_autoptr(GMenu) menu = NULL;

    self->session = session;
    a7800_install_accent_css();

    gtk_window_set_title(GTK_WINDOW(self), "FujiNet Go Atari 7800");
    gtk_window_set_icon_name(GTK_WINDOW(self), a7800_icon_name());
    /* 320 x 240 at 3x: MARIA's line at the television's 4:3, three times
     * over, and the header bar. */
    gtk_window_set_default_size(GTK_WINDOW(self), 960, 720 + 46);

    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                    G_N_ELEMENTS(win_actions), self);
    {
        static const char *const prefs_accels[] = { "<Control>comma", NULL };
        static const char *const open_accels[] = { "<Control>o", NULL };
        static const char *const reboot_accels[] = { "<Control>r", NULL };
        static const char *const ldiff_accels[] = { "<Alt>l", NULL };
        static const char *const rdiff_accels[] = { "<Alt>r", NULL };
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.prefs", prefs_accels);
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.open", open_accels);
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.reboot-config", reboot_accels);
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.left-diff", ldiff_accels);
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.right-diff", rdiff_accels);
    }

    header = adw_header_bar_new();
    menu = build_menu();
    menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_button);

    status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    self->status_dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(self->status_dot, "a7800-dot");
    gtk_widget_add_css_class(self->status_dot, "a7800-dot-off");
    gtk_widget_set_valign(self->status_dot, GTK_ALIGN_CENTER);
    self->status = gtk_label_new("Starting...");
    gtk_widget_add_css_class(self->status, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(self->status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(status_box), self->status_dot);
    gtk_box_append(GTK_BOX(status_box), self->status);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), status_box);

    self->display = a7800_display_new(session);
    a7800_display_set_tv_aspect(A7800_DISPLAY(self->display),
        a7800session_get_int(session, "aspect", 0) == 0);
    a7800_display_set_smooth(A7800_DISPLAY(self->display),
        a7800session_get_int(session, "smooth", 0) != 0);
    {
        GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), "tv-aspect");
        g_simple_action_set_state(G_SIMPLE_ACTION(a),
            g_variant_new_boolean(a7800session_get_int(session, "aspect", 0) == 0));
        a = g_action_map_lookup_action(G_ACTION_MAP(self), "smooth");
        g_simple_action_set_state(G_SIMPLE_ACTION(a),
            g_variant_new_boolean(a7800session_get_int(session, "smooth", 0) != 0));
    }

    {
        GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
        g_signal_connect(drop, "drop", G_CALLBACK(on_drop), self);
        gtk_widget_add_controller(self->display, GTK_EVENT_CONTROLLER(drop));
    }

    self->toast_overlay = adw_toast_overlay_new();
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(self->toast_overlay), self->display);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->toast_overlay);
    gtk_box_append(GTK_BOX(box), toolbar);
    gtk_widget_set_vexpand(toolbar, TRUE);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), box);

    /* Capture on the WINDOW so input works no matter what has focus. */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), focus);

    self->status_id = g_timeout_add_seconds(1, update_status, self);
    /* The current pad set is the baseline: no toast for pads that were
     * already plugged in at launch. */
    self->pad_generation = a7800session_gamepad_generation(session);
    self->sysact_id = g_timeout_add(250, sysact_drain_tick, self);
    sync_switches(self);
    update_status(self);

    /* A7800_OPEN_CONTROLLERS=1 / A7800_OPEN_DEBUGGER=1 / A7800_OPEN_SETTINGS=1
     * open those windows at launch, following the family's convention: the
     * way in when the app misbehaves before the menu is reachable.
     * A7800_SCREENSHOT=<file.png> saves every open window after
     * A7800_SCREENSHOT_DELAY_MS (3 s) and quits. */
    {
        const char *env = g_getenv("A7800_OPEN_CONTROLLERS");
        if (env && *env && *env != '0')
            a7800_controllers_window_toggle(GTK_WINDOW(self), session);
        env = g_getenv("A7800_OPEN_DEBUGGER");
        if (env && *env && *env != '0')
            a7800_debugger_show(GTK_WINDOW(self), session);
        env = g_getenv("A7800_OPEN_SETTINGS");
        if (env && *env && *env != '0')
            a7800_prefs_show(self, session, restart_session);
        env = g_getenv("A7800_SCREENSHOT");
        if (env && *env) {
            const char *delay = g_getenv("A7800_SCREENSHOT_DELAY_MS");
            self->shot_id = g_timeout_add(delay && *delay ? (guint)atoi(delay) : 3000,
                                          screenshot_tick, self);
        }
    }
    return GTK_WIDGET(self);
}
