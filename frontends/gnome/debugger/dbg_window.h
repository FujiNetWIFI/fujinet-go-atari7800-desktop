/*
 * Debugger window for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a7800session.h"

G_BEGIN_DECLS

/* Shows (creating on first use) the debugger window for the session.
 * Showing attaches the debugger engine, which stops the machine. */
void a7800_debugger_show(GtkWindow *parent, a7800session *session);
/* F12: shows it, or hides it (detaching, so the machine runs on). */
void a7800_debugger_toggle(GtkWindow *parent, a7800session *session);

G_END_DECLS
