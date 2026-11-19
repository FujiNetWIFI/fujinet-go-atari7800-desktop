/*
 * The Controllers window -- both ports drawn as plugged in (ProLine, CX40,
 * XG-1 or nothing) over the console's front panel, held parts lit, Map mode
 * for rebinding any control to a key or a gamepad button, and the ports'
 * controller types and gamepad assignments.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a7800session.h"

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. `parent` is only used the first
 * time, to set the transient-for relationship. */
void a7800_controllers_window_toggle(GtkWindow *parent, a7800session *session);
gboolean a7800_controllers_window_is_visible(void);
/* Shows (or raises) the window. */
void a7800_controllers_window_show(GtkWindow *parent, a7800session *session);
/* Bindings changed outside the window: redraw it and rebuild its pad rows. */
void a7800_controllers_window_refresh(void);
