/*
 * The main application window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a7800session.h"

#define A7800_TYPE_WINDOW (a7800_window_get_type())
G_DECLARE_FINAL_TYPE(A7800Window, a7800_window, A7800, WINDOW,
                     AdwApplicationWindow)

GtkWidget *a7800_window_new(AdwApplication *app, a7800session *session);
/* main.c: the icon name to use (the installed id, or the in-tree art). */
const char *a7800_icon_name(void);
void a7800_window_toast(A7800Window *self, const char *text);
/* The display's shape: 0 = the television's 4:3, 1 = square pixels. */
void a7800_window_apply_aspect(A7800Window *self, int aspect);

/* The accent colour (A7800SESSION_ACCENT_RGB) as a CSS class every window
 * can use: ".a7800-accent" paints a widget's background in it, and
 * ".a7800-accent-text" its label. Installed once by the main window. */
void a7800_install_accent_css(void);
