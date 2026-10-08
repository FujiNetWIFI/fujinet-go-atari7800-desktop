/*
 * AppKit key events to X11/xkb keysyms, by hardware key code through the
 * session's HID table -- the same physical-key route the other frontends
 * take, so a binding names the key whatever Shift or the layout is doing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>
#include <stdint.h>

/* Returns the keysym, or 0 when the key has no equivalent worth forwarding. */
uint32_t A7800KeysymFromEvent(NSEvent *event);

/* The keysym for a modifier flag change (flagsChanged), or 0. Sets *down. */
uint32_t A7800KeysymFromFlagsChange(NSEvent *event, int *down);

/* This app's accent colour (A7800SESSION_ACCENT_RGB). */
NSColor *A7800AccentColor(void);
