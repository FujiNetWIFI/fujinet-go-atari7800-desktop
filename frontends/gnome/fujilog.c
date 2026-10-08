/*
 * The FujiNet console log window: a live view of the in-process runtime's
 * captured output, refreshed once a second. And the configuration, which is
 * the FujiNet web UI in the system browser.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fujilog.h"

typedef struct {
    GtkWindow *win;
    GtkTextView *view;
    GtkScrolledWindow *scroll;
    a7800session *session;
    guint timer;
} LogWindow;

static void on_destroy(GtkWidget *w, gpointer user_data)
{
    LogWindow *lw = user_data;
    (void)w;
    if (lw->timer) {
        g_source_remove(lw->timer);
        lw->timer = 0;
    }
    g_free(lw);
}

static gboolean refresh(gpointer user_data)
{
    LogWindow *lw = user_data;
    static char buf[128 * 1024];
    GtkTextBuffer *b = gtk_text_view_get_buffer(lw->view);
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(lw->scroll);
    gboolean at_end = TRUE;
    int n;

    if (adj) {
        const double page = gtk_adjustment_get_page_size(adj);
        const double upper = gtk_adjustment_get_upper(adj);
        at_end = gtk_adjustment_get_value(adj) >= upper - page - 1.0;
    }

    n = a7800session_fujinet_copy_log(lw->session, buf, sizeof buf);
    gtk_text_buffer_set_text(b, n > 0 ? buf : "(no FujiNet output yet)", -1);

    /* Follow the tail only while the reader is already at the bottom. */
    if (at_end && adj)
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
                                          gtk_adjustment_get_page_size(adj));
    return G_SOURCE_CONTINUE;
}

void a7800_fujilog_show(GtkWindow *parent, a7800session *session)
{
    static GtkWindow *existing;
    LogWindow *lw;
    GtkWidget *scroll, *view;

    if (existing) {
        gtk_window_present(existing);
        return;
    }

    lw = g_new0(LogWindow, 1);
    lw->session = session;

    view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);

    lw->win = GTK_WINDOW(adw_window_new());
    lw->view = GTK_TEXT_VIEW(view);
    lw->scroll = GTK_SCROLLED_WINDOW(scroll);
    gtk_window_set_title(lw->win, "FujiNet Console Log");
    gtk_window_set_default_size(lw->win, 820, 560);
    gtk_window_set_transient_for(lw->win, parent);
    gtk_window_set_application(lw->win, gtk_window_get_application(parent));

    {
        GtkWidget *toolbar = adw_toolbar_view_new();
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                     adw_header_bar_new());
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);
        adw_window_set_content(ADW_WINDOW(lw->win), toolbar);
    }

    g_signal_connect(lw->win, "destroy", G_CALLBACK(on_destroy), lw);
    lw->timer = g_timeout_add(1000, refresh, lw);
    refresh(lw);

    existing = lw->win;
    g_object_add_weak_pointer(G_OBJECT(lw->win), (gpointer *)&existing);
    gtk_window_present(lw->win);
}

void a7800_fujiconfig_show(GtkWindow *parent, a7800session *session)
{
    GtkUriLauncher *l = gtk_uri_launcher_new(a7800session_fujinet_webui_url(session));
    gtk_uri_launcher_launch(l, parent, NULL, NULL, NULL);
    g_object_unref(l);
}
