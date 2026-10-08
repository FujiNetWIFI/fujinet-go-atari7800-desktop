/*
 * FujiNet Go Atari 7800 -- the GNOME (GTK4/libadwaita) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <adwaita.h>
#include <stdlib.h>
#include <string.h>

#include "a7800session.h"
#include "window.h"

#define APP_ID "online.fujinet.go.atari7800.gnome"

static a7800session *g_session;
static char *g_cart_arg;

/* The installed icons are named after the desktop-entry id. Running straight
 * out of the build tree there is no such icon, so point GTK's icon theme at
 * the in-tree artwork (named for the project, not the id) and use that --
 * otherwise GTK's window-icon lookup fails with a NULL texture on every
 * frame it tries to paint the icon. */
const char *a7800_icon_name(void)
{
    static const char *name;
    GtkIconTheme *theme;

    if (name)
        return name;

    name = APP_ID;
    theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    if (theme && !gtk_icon_theme_has_icon(theme, name)) {
        gtk_icon_theme_add_search_path(theme, A7800_SOURCE_ICON_DIR);
        if (gtk_icon_theme_has_icon(theme, "fujinet-go-atari7800"))
            name = "fujinet-go-atari7800";
    }
    return name;
}

/* The controllers, debugger and log windows are hidden rather than destroyed
 * and belong to the application (so the shell groups them with it), which
 * means the application would outlive its main window. Closing the main
 * window is quitting. */
static gboolean on_main_close(GtkWindow *win, gpointer user_data)
{
    (void)win;
    g_application_quit(G_APPLICATION(user_data));
    return FALSE;
}

static void on_activate(AdwApplication *app, gpointer user_data)
{
    a7800session_start_opts opts;
    GtkWidget *win;
    (void)user_data;

    /* One window per process: the machine, and the FujiNet cartridge inside
     * it, are process singletons. Raising the existing window is the honest
     * response to a second activation. */
    win = GTK_WIDGET(gtk_application_get_active_window(GTK_APPLICATION(app)));
    if (win) {
        gtk_window_present(GTK_WINDOW(win));
        return;
    }

    win = a7800_window_new(app, g_session);
    g_signal_connect(win, "close-request", G_CALLBACK(on_main_close), app);

    a7800session_default_opts(g_session, &opts);
    if (g_cart_arg)
        opts.cart_path = g_cart_arg;

    if (a7800session_start(g_session, &opts) != 0)
        g_warning("%s", a7800session_last_error(g_session));
    gtk_window_present(GTK_WINDOW(win));
}

static void on_open(GApplication *app, GFile **files, gint n_files,
                    const gchar *hint, gpointer user_data)
{
    (void)hint; (void)user_data;
    if (n_files > 0) {
        g_free(g_cart_arg);
        g_cart_arg = g_file_get_path(files[0]);
    }
    g_application_activate(app);
}

int main(int argc, char **argv)
{
    AdwApplication *app;
    int status;

    g_session = a7800session_new(NULL);
    if (!g_session) {
        g_printerr("Could not create the session (unusable config/data "
                   "directories?)\n");
        return 1;
    }

    app = adw_application_new(APP_ID, G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);

    status = g_application_run(G_APPLICATION(app), argc, argv);

    a7800session_stop(g_session);
    a7800session_free(g_session);
    g_object_unref(app);
    g_free(g_cart_arg);
    return status;
}
