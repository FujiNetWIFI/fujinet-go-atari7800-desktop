/*
 * Preferences dialog for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a7800session.h"

G_BEGIN_DECLS

typedef struct _A7800Window A7800Window;

/* Shows the preferences dialog. The console (TV system, BIOS, High Score
 * Cart), controller types, the analog switch and the aspect apply live; the
 * host options (FujiNet, audio, gamepads) are read when the session starts,
 * so the dialog restarts the session on close if one of those changed. */
void a7800_prefs_show(A7800Window *parent, a7800session *session,
                      void (*restart)(A7800Window *parent));

G_END_DECLS
