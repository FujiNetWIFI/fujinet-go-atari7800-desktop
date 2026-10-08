/*
 * The Win32 Controllers window: both joysticks on screen, the console's
 * switches and the Map row, in a fixed-size tool window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "a7800session.h"

/* Show / hide (F9). Created on first use, hidden not destroyed after. */
void a7800_controller_window_toggle(HWND parent, a7800session *session);

/* Called when the gamepad set (or a port's controller type) changed, so the
 * per-port lines refresh. */
void a7800_controller_window_gamepads_changed(void);

/* Give the window first refusal on a message from the main loop. Returns 1
 * if consumed. */
int a7800_controller_pretranslate(MSG *msg);
