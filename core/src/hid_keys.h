/*
 * hid_keys -- private interface to core/src/hid_keys.c, for bindings.c's
 * name tables. The native-code -> HID-usage translators the frontends call
 * are public instead, declared in a7800session.h beside the keysym bands they
 * feed (A7800SESSION_KEYSYM_HID_BASE and friends).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef A7800_HID_KEYS_H
#define A7800_HID_KEYS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Human-readable name for a USB HID Keyboard usage ID -- "F16", "Keypad A",
 * "International 3" -- using jzIntv's own vocabulary where it has one (see
 * core/jzintv-generated/src/event/event_tbl.inc, whose names these mirror so
 * a binding reads the same here as in a jzIntv hackfile). NULL for a usage
 * with no name, which is the caller's cue to synthesize one; callers must
 * handle that rather than print NULL. */
const char *a7800_hid_usage_name(uint32_t usage);

/* The HID-usage keysym band: a key with no X11 keysym of its own (a media
 * key, an international key) is still bindable through this. */
#define A7800SESSION_KEYSYM_HID_BASE 0x10000u
#define A7800SESSION_HID_USAGE_MAX   0xE7u

/* USB HID keyboard usage IDs from each platform's native key code; 0 when
 * unknown. The public a7800session_keysym_from_* wrappers chain these
 * through a7800session_keysym_from_hid. */
uint32_t a7800session_hid_from_win_scancode(unsigned scancode, int extended);
uint32_t a7800session_hid_from_evdev(unsigned code);
uint32_t a7800session_hid_from_macos_keycode(unsigned keycode);
uint32_t a7800session_keysym_from_hid(uint32_t usage);

#ifdef __cplusplus
}
#endif

#endif /* A7800_HID_KEYS_H */
