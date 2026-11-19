/*
 * input_merge -- every way of holding a 7800 input is a separate SOURCE, and
 * what the console sees is their merge. So letting go of one source (a key
 * release, the Controllers window's mouse, a gamepad that is unplugged) can
 * never cancel another's hold, and two pads on one port do not fight.
 *
 * Sources: the host keyboard, the Controllers window's pointer, and for each
 * connected pad (by its stable owner number, 0 to A7800_MAX_PADS - 1) its
 * buttons and its stick / D-pad separately.
 *
 * Per port the stick's four directions (opposed pairs cancel, as on a real
 * joystick whose switches cannot close both) and both buttons are merged; the
 * console's momentary switches (Select, Reset, Pause) are a plain OR. The
 * difficulty switches are latched toggles and are not sources: a press from
 * any source flips them.
 *
 * The functions are C so the gamepad thread (gamepad_sdl.c) can call them;
 * they live in input.cpp. This header is not input.h on purpose: macOS file
 * systems ignore case.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A7800_INPUT_MERGE_H
#define A7800_INPUT_MERGE_H

#include "a7800session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define A7800_MAX_PADS 8

#define A7800_SRC_KEYBOARD  0
#define A7800_SRC_POINTER   1
#define A7800_SRC_PAD0      2                                    /* + owner: its buttons */
#define A7800_SRC_STICK0    (A7800_SRC_PAD0 + A7800_MAX_PADS)    /* + owner: stick, D-pad */
#define A7800_SRC_COUNT     (A7800_SRC_STICK0 + A7800_MAX_PADS)
#define A7800_SRC_PAD(owner)   (A7800_SRC_PAD0 + (owner))
#define A7800_SRC_STICK(owner) (A7800_SRC_STICK0 + (owner))

struct a7800session;

/* Hold or release a port action or a console switch (target < A7800_TARGET_SYSACT(0))
 * for a source. A difficulty switch flips on a press (down = 1) and ignores the
 * release. Anything else is ignored. */
void input_press(struct a7800session *s, int src, int target, int down);

/* Set a source's whole mask of port actions (1 << a7800_action) at once. */
void input_set_port(struct a7800session *s, int src, int port, unsigned mask);

/* Let go of everything a connected pad holds (its buttons' and its stick's
 * sources). Called when a pad goes away or changes port. */
void input_pad_clear(struct a7800session *s, int owner);

#ifdef __cplusplus
}
#endif

#endif /* A7800_INPUT_MERGE_H */
