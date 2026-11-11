/*
 * The debugger's Video tab (GTK4): a thin view over core/include/a7800vidtab.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A7800_GNOME_DBG_VIDEO_H
#define A7800_GNOME_DBG_VIDEO_H

#include <gtk/gtk.h>

#include "a7800session.h"

/* The page. It owns its controller and frees it when destroyed. */
GtkWidget *dbg_video_new(a7800session *session);

/* Bring the page up to date (only what changed is redrawn). The first call
 * after the page is mapped, or force, redraws everything. */
void dbg_video_refresh(GtkWidget *page, gboolean force);

#endif
