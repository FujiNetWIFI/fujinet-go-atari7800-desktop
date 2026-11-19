/*
 * input_test -- the merge of input sources, with no machine running: the
 * keyboard, the Controllers window's pointer and each pad (buttons and stick
 * separately) are sources of their own, so one letting go never drops another's
 * hold; opposed directions cancel; release_all is the keyboard's and the
 * pointer's only; the difficulty switches are toggles from any source and stay
 * persisted; reserved keys are refused; a pad cleared lets go of exactly what
 * it held.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "a7800session.h"
#include "input_merge.h"
#include "test_tmpdir.h"

static int failures;
static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

#define B(act) (1u << (act))

int main(void)
{
    char cfg[512], data[512];
    a7800session_paths paths;
    a7800session *s;

    test_tmpdir(cfg, sizeof cfg, "icfg");
    test_tmpdir(data, sizeof data, "idata");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";
    s = a7800session_new(&paths);
    if (!s) return 1;

    /* a key and the window on the same input */
    check(a7800session_buttons_held(s, 0) == 0, "nothing held at first");
    a7800session_key(s, A7800_KEYSYM_RIGHT, 1);
    check(a7800session_buttons_held(s, 0) == B(A7800_ACT_RIGHT), "the key holds right");
    a7800session_press(s, A7800_TARGET_PORT(0, A7800_ACT_RIGHT), 1);
    a7800session_key(s, A7800_KEYSYM_RIGHT, 0);
    check(a7800session_buttons_held(s, 0) == B(A7800_ACT_RIGHT), "the key letting go leaves the window's right");
    a7800session_press(s, A7800_TARGET_PORT(0, A7800_ACT_RIGHT), 0);
    check(a7800session_buttons_held(s, 0) == 0, "and the window lets go of its own");

    /* a key release does not drop a pad's direction (the old bug) */
    input_press(s, A7800_SRC_STICK(0), A7800_TARGET_PORT(0, A7800_ACT_UP), 1);
    a7800session_key(s, A7800_KEYSYM_UP, 1);
    a7800session_key(s, A7800_KEYSYM_UP, 0);
    check(a7800session_buttons_held(s, 0) == B(A7800_ACT_UP), "key release keeps a pad's Up");
    check(a7800session_buttons_held(s, 1) == 0, "port 2 is untouched");

    /* opposed pairs cancel, across sources, and come back */
    a7800session_key(s, A7800_KEYSYM_DOWN, 1);
    check(a7800session_buttons_held(s, 0) == 0, "key Down and pad Up cancel");
    a7800session_key(s, A7800_KEYSYM_DOWN, 0);
    check(a7800session_buttons_held(s, 0) == B(A7800_ACT_UP), "and the pad's Up is back");
    a7800session_pointer_set(s, 0, B(A7800_ACT_LEFT) | B(A7800_ACT_DOWN));
    check(a7800session_buttons_held(s, 0) == B(A7800_ACT_LEFT), "a window drag down-left cancels Up and keeps Left");
    a7800session_key(s, A7800_KEYSYM_RIGHT, 1);
    check(a7800session_buttons_held(s, 0) == 0, "Left and Right cancel as well");
    a7800session_key(s, A7800_KEYSYM_RIGHT, 0);

    /* pointer vs pad: the drag letting go leaves the pad, the pad letting go leaves the drag */
    a7800session_pointer_set(s, 0, 0);
    check(a7800session_buttons_held(s, 0) == B(A7800_ACT_UP), "the drag ending leaves the pad's Up");
    a7800session_pointer_set(s, 0, B(A7800_ACT_RIGHT));
    input_press(s, A7800_SRC_STICK(0), A7800_TARGET_PORT(0, A7800_ACT_UP), 0);
    check(a7800session_buttons_held(s, 0) == B(A7800_ACT_RIGHT), "the pad letting go leaves the drag");
    a7800session_pointer_set(s, 0, 0);
    check(a7800session_buttons_held(s, 0) == 0, "all let go");

    /* two pads on one port do not fight: buttons are a plain OR */
    input_press(s, A7800_SRC_PAD(0), A7800_TARGET_PORT(1, A7800_ACT_BUTTON1), 1);
    input_press(s, A7800_SRC_PAD(1), A7800_TARGET_PORT(1, A7800_ACT_BUTTON1), 1);
    input_press(s, A7800_SRC_PAD(0), A7800_TARGET_PORT(1, A7800_ACT_BUTTON1), 0);
    check(a7800session_buttons_held(s, 1) == B(A7800_ACT_BUTTON1), "the second pad still holds Button 1");
    input_pad_clear(s, 1);
    check(a7800session_buttons_held(s, 1) == 0, "a pad cleared lets go of what it held");

    /* release_all: keyboard and pointer, never a pad */
    a7800session_key(s, 'z', 1);
    a7800session_press(s, A7800_TARGET_PORT(0, A7800_ACT_LEFT), 1);
    input_press(s, A7800_SRC_PAD(2), A7800_TARGET_PORT(0, A7800_ACT_BUTTON2), 1);
    input_press(s, A7800_SRC_PAD(2), A7800_TARGET_SWITCH(A7800_SW_PAUSE), 1);
    a7800session_press(s, A7800_TARGET_SWITCH(A7800_SW_SELECT), 1);
    check(a7800session_buttons_held(s, 0) == (B(A7800_ACT_BUTTON1) | B(A7800_ACT_LEFT) | B(A7800_ACT_BUTTON2)), "z, the window's Left and a pad's Button 2");
    check(a7800session_switch_held(s, A7800_SW_SELECT) && a7800session_switch_held(s, A7800_SW_PAUSE), "Select and Pause held");
    a7800session_release_all(s);
    check(a7800session_buttons_held(s, 0) == B(A7800_ACT_BUTTON2), "release_all keeps only the pad's hold");
    check(!a7800session_switch_held(s, A7800_SW_SELECT) && a7800session_switch_held(s, A7800_SW_PAUSE), "and the window's Select went, the pad's Pause stayed");
    check(a7800session_target_held(s, A7800_TARGET_PORT(0, A7800_ACT_BUTTON2)) == 1 &&
          a7800session_target_held(s, A7800_TARGET_SWITCH(A7800_SW_PAUSE)) == 1 &&
          a7800session_target_held(s, A7800_TARGET_SWITCH(A7800_SW_RESET)) == 0 &&
          a7800session_target_held(s, A7800_TARGET_SYSACT(A7800_SYSACT_PAUSE)) == 0, "target_held");
    /* the key's release after release_all is a no-op, not a stale release */
    a7800session_key(s, 'z', 0);
    check(a7800session_buttons_held(s, 0) == B(A7800_ACT_BUTTON2), "a key released after release_all changes nothing");
    input_pad_clear(s, 2);
    check(a7800session_buttons_held(s, 0) == 0 && !a7800session_switch_held(s, A7800_SW_PAUSE), "pad 3 cleared");

    /* clear_held and release_pointer / release_keyboard */
    a7800session_key(s, 'i', 1);
    a7800session_pointer_set(s, 1, B(A7800_ACT_BUTTON2));
    a7800session_release_pointer(s);
    check(a7800session_buttons_held(s, 1) == B(A7800_ACT_UP), "release_pointer leaves the key");
    a7800session_pointer_set(s, 1, B(A7800_ACT_BUTTON2));
    a7800session_release_keyboard(s);
    check(a7800session_buttons_held(s, 1) == B(A7800_ACT_BUTTON2), "release_keyboard leaves the pointer");
    a7800session_clear_held(s);
    check(a7800session_buttons_held(s, 1) == 0, "clear_held lets go of both");

    /* difficulty switches: latched, toggled by a press from any source, persisted */
    check(a7800session_switch_get(s, A7800_SW_LEFT_DIFF) == 1, "the left difficulty starts at A");
    a7800session_press(s, A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF), 1);
    a7800session_press(s, A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF), 0);
    check(a7800session_switch_get(s, A7800_SW_LEFT_DIFF) == 0, "a click flips it to B; the release does not flip it back");
    input_press(s, A7800_SRC_PAD(0), A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF), 1);
    input_press(s, A7800_SRC_PAD(0), A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF), 0);
    check(a7800session_switch_get(s, A7800_SW_LEFT_DIFF) == 1, "a pad button flips it back");
    a7800session_switch_set(s, A7800_SW_RIGHT_DIFF, 0);
    check(a7800session_target_held(s, A7800_TARGET_SWITCH(A7800_SW_RIGHT_DIFF)) == 0 &&
          a7800session_target_held(s, A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF)) == 1, "target_held reads the position");
    check(a7800session_get_int(s, "right_diff", 1) == 0 && a7800session_get_int(s, "left_diff", 0) == 1, "persisted as left_diff / right_diff");
    a7800session_release_all(s);
    check(a7800session_switch_get(s, A7800_SW_RIGHT_DIFF) == 0, "release_all does not touch a latched switch");
    /* a key bound to a difficulty switch flips it once per press, not per auto-repeat */
    a7800session_binding_set_key(s, A7800_TARGET_SWITCH(A7800_SW_RIGHT_DIFF), 'q', NULL, 0);
    a7800session_key(s, 'q', 1);
    a7800session_key(s, 'q', 1);
    a7800session_key(s, 'q', 0);
    check(a7800session_switch_get(s, A7800_SW_RIGHT_DIFF) == 1, "a bound key flips it once");
    a7800session_bindings_reset(s);

    /* ranges */
    input_press(s, -1, A7800_TARGET_PORT(0, A7800_ACT_UP), 1);
    input_press(s, A7800_SRC_COUNT, A7800_TARGET_PORT(0, A7800_ACT_UP), 1);
    input_press(s, A7800_SRC_KEYBOARD, A7800_TARGET_SYSACT(0), 1);
    input_set_port(s, A7800_SRC_POINTER, 2, 1);
    check(a7800session_buttons_held(s, 0) == 0 && a7800session_buttons_held(s, 2) == 0 &&
          a7800session_buttons_held(s, -1) == 0, "out-of-range sources, targets and ports are ignored");

    a7800session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
