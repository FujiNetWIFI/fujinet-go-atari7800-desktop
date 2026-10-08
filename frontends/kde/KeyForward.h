/*
 * Qt key events to X11/xkb keysyms.
 *
 * The session's bindings are keyed by the PHYSICAL key -- the unshifted
 * US-layout symbol of the hardware key -- so that Shift cannot change what a
 * key means, and so that Qt's already-shifted key() cannot change which
 * binding a key hits. The route is
 * the hardware scan code (evdev on Linux) through the session's HID table,
 * with a Qt::Key fallback for anything the table has no usage for.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QKeyEvent>
#include <cstdint>

/* Returns the keysym, or 0 when the key has no equivalent worth forwarding. */
uint32_t a7800KeysymFromQt(const QKeyEvent *e);
