/*
 * bindings -- private interface to core/src/bindings.c, for the two other
 * translators that need to see inside the remappable-bindings table:
 * session.c (to seed/reload it at session start) and gamepad_sdl.c (to
 * resolve a gamepad button against it on its own polling thread, and to
 * suppress a D-pad direction the user has remapped away from the disc).
 * a7800session.h carries the frontend-facing subset of this API.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef A7800_BINDINGS_H
#define A7800_BINDINGS_H

#include "a7800session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reloads the bindings table: every slot to its upstream default, then
 * overridden by whatever "bindings" holds in s's settings store. Called once
 * per a7800session_new (see session.c) -- process-global storage, like
 * gamepad_sdl.c's own pad table, because jzIntv's machine is a process
 * singleton (core/jzintv/a7800_host.h) and there is only ever one session's
 * settings store live at a time. */
void bindings_init(struct a7800session *s);

/* Gamepad-button lookup, scoped to one side only -- unlike the keyboard
 * (a7800session_key_from_keysym_bound), a physical pad drives at most one
 * side at a time (a7800session_gamepad_assign), so the same button index is
 * free to mean different things on different sides; nothing to steal across
 * them. MAP_NONE if `button` drives nothing on `side`, MAP_KEY/MAP_DISC/
 * MAP_SYSACT otherwise -- a button can be bound to a keypad key, a disc
 * segment, or a system action. The one exception to "scoped to one side
 * only": a system action is machine-global (always stored on LEFT, see
 * a7800session_target_sysaction), so this also falls back to LEFT's two
 * sysaction slots when `side` isn't LEFT and the per-side scan misses --
 * see the function's own comment. */
a7800session_key_mapping bindings_target_from_button(a7800session_pad_side side,
                                                     a7800session_pad_button button);

/* True iff `button` is NOT currently bound to any keypad key or disc segment
 * on `side` -- gamepad_sdl.c's poll_sticks gates each D-pad direction it
 * reads for the disc on this, so a D-pad button the user remapped to a
 * keypad digit (or explicitly to a different disc segment) stops also
 * nudging the disc from the raw D-pad state every poll. */
int bindings_button_is_free(a7800session_pad_side side,
                            a7800session_pad_button button);

/* The gamepad button explicitly bound to each of `side`'s 16 disc positions,
 * indexed by clock position (0 = East, ... see a7800session.h), NONE where
 * unbound. Returns how many of the 16 are bound, so poll_sticks can skip
 * straight to the D-pad/stick fallback in the common (all-default) case.
 * gamepad_sdl.c's own polling thread is the only reader; there is no public
 * equivalent because only it needs per-position resolution priority. */
int bindings_disc_buttons(a7800session_pad_side side,
                          a7800session_pad_button out[A7800SESSION_DISC_POSITIONS]);

#ifdef __cplusplus
}
#endif

#endif /* A7800_BINDINGS_H */
