/*
 * The emulator display: a GtkWidget that pulls frames from the session on
 * the compositor's own frame clock, and aims the light guns with the mouse.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a7800session.h"

#define A7800_TYPE_DISPLAY (a7800_display_get_type())
G_DECLARE_FINAL_TYPE(A7800Display, a7800_display, A7800, DISPLAY, GtkWidget)

GtkWidget *a7800_display_new(a7800session *session);
/* The 4:3 picture a television showed, or square pixels (320 x 224). */
void a7800_display_set_tv_aspect(A7800Display *self, gboolean tv);
void a7800_display_set_smooth(A7800Display *self, gboolean smooth);
