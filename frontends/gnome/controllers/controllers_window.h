/*
 * The Controllers window -- both ProLine joysticks on screen, held buttons
 * lit, the console's switches, Map mode for rebinding any control to a key
 * or a gamepad button, and the ports' controller types and gamepad
 * assignments.
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
