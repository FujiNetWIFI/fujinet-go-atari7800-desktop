/*
 * bindings_test -- the remappable table and the keyboard translator, pure:
 * defaults, steal-and-describe, persistence through the settings store, and
 * names. No machine is started.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "a7800session.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    char cfg[512], data[512], stolen[64], name[64];
    a7800session_paths p;
    a7800session *s;
    a7800_binding b;

    test_tmpdir(cfg, sizeof cfg, "bcfg");
    test_tmpdir(data, sizeof data, "bdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = a7800session_new(&p);
    if (!s) return 1;

    /* defaults */
    b = a7800session_binding_get(s, A7800_TARGET_PORT(0, A7800_ACT_BUTTON2));
    check(b.keysym == 'x' && b.button == A7800_PAD_BTN_EAST, "player 1 Button 2 defaults to X / East");
    b = a7800session_binding_get(s, A7800_TARGET_PORT(0, A7800_ACT_BUTTON1));
    check(b.keysym == 'z' && b.button == A7800_PAD_BTN_SOUTH, "player 1 Button 1 defaults to Z / South");
    b = a7800session_binding_get(s, A7800_TARGET_PORT(1, A7800_ACT_UP));
    check(b.keysym == 'i', "player 2 Up defaults to I");
    b = a7800session_binding_get(s, A7800_TARGET_SWITCH(A7800_SW_SELECT));
    check(b.keysym == A7800_KEYSYM_F1 && b.button == A7800_PAD_BTN_BACK, "Select defaults to F1 / Back");
    b = a7800session_binding_get(s, A7800_TARGET_SWITCH(A7800_SW_RESET));
    check(b.keysym == A7800_KEYSYM_F2 && b.button == A7800_PAD_BTN_START, "Reset defaults to F2 / Start");
    b = a7800session_binding_get(s, A7800_TARGET_SWITCH(A7800_SW_PAUSE));
    check(b.keysym == A7800_KEYSYM_F3, "Pause defaults to F3");
    check(a7800session_key_sysaction(s, A7800_KEYSYM_ESCAPE) == A7800_SYSACT_REBOOT_CONFIG, "Escape is Reboot to CONFIG");
    check(a7800session_target_for_key(s, 'X') == A7800_TARGET_PORT(0, A7800_ACT_BUTTON2), "lookup folds case");
    check(a7800session_target_for_button(s, 1, A7800_PAD_BTN_SOUTH) == A7800_TARGET_PORT(1, A7800_ACT_BUTTON1), "button lookup is scoped to the port");

    /* steal */
    a7800session_binding_set_key(s, A7800_TARGET_PORT(1, A7800_ACT_BUTTON2), 'x', stolen, sizeof stolen);
    check(strstr(stolen, "Player 1: Button 2") != NULL, "rebinding X reports the holder it displaced");
    b = a7800session_binding_get(s, A7800_TARGET_PORT(0, A7800_ACT_BUTTON2));
    check(b.keysym == 0, "the old holder lost the key");
    check(a7800session_target_for_key(s, 'x') == A7800_TARGET_PORT(1, A7800_ACT_BUTTON2), "the new holder has it");

    /* the frontends' keys: F9, F11, F12 are never bindable */
    stolen[0] = 'x';
    a7800session_binding_set_key(s, A7800_TARGET_PORT(0, A7800_ACT_UP), A7800_KEYSYM_F9, stolen, sizeof stolen);
    check(stolen[0] == 0 && a7800session_binding_get(s, A7800_TARGET_PORT(0, A7800_ACT_UP)).keysym == A7800_KEYSYM_UP,
          "F9 is refused and the old key stays");
    a7800session_binding_set_key(s, A7800_TARGET_PORT(0, A7800_ACT_UP), A7800_KEYSYM_F11, NULL, 0);
    a7800session_binding_set_key(s, A7800_TARGET_PORT(0, A7800_ACT_UP), A7800_KEYSYM_F12, NULL, 0);
    check(a7800session_binding_get(s, A7800_TARGET_PORT(0, A7800_ACT_UP)).keysym == A7800_KEYSYM_UP, "F11 and F12 too");
    check(a7800session_target_for_key(s, A7800_KEYSYM_F9) == -1 && a7800session_target_for_key(s, A7800_KEYSYM_F12) == -1,
          "no target for them");
    check(a7800session_key_reserved(A7800_KEYSYM_F9) && a7800session_key_reserved(A7800_KEYSYM_F11) &&
          a7800session_key_reserved(A7800_KEYSYM_F12) && !a7800session_key_reserved(A7800_KEYSYM_F8), "key_reserved");
    check(a7800session_binding_get(s, A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF)).keysym == 0, "the difficulty switches have no default key (Alt+L / Alt+R are menu accelerators)");

    /* clear: both the key and the pad button of a target go, and it persists */
    a7800session_binding_set_key(s, A7800_TARGET_PORT(1, A7800_ACT_BUTTON1), 'g', NULL, 0);
    a7800session_binding_set_button(s, A7800_TARGET_PORT(1, A7800_ACT_BUTTON1), A7800_PAD_BTN_NORTH, NULL, 0);
    a7800session_binding_clear(s, A7800_TARGET_PORT(1, A7800_ACT_BUTTON1));
    b = a7800session_binding_get(s, A7800_TARGET_PORT(1, A7800_ACT_BUTTON1));
    check(b.keysym == 0 && b.button == A7800_PAD_BTN_NONE, "clear unbinds the key and the button");
    check(a7800session_target_for_key(s, 'g') == -1 && a7800session_target_for_key(s, 'n') == -1, "and nothing else got them");

    /* names */
    a7800session_keysym_name(A7800_KEYSYM_F12, name, sizeof name);
    check(strcmp(name, "F12") == 0, "F12 names itself");
    a7800session_keysym_name('a', name, sizeof name);
    check(strcmp(name, "A") == 0, "letters name upper-case");
    check(strcmp(a7800_pad_button_name(A7800_PAD_BTN_DPAD_LEFT), "D-pad Left") == 0, "pad button names");
    check(strcmp(a7800_target_name(A7800_TARGET_SYSACT(A7800_SYSACT_REBOOT_CONFIG)), "Reboot to CONFIG") == 0, "system action names");
    check(strcmp(a7800_target_name(A7800_TARGET_PORT(1, A7800_ACT_LEFT)), "Player 2: Left") == 0, "target names");
    check(strcmp(a7800_target_name(A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF)), "Left Difficulty") == 0, "switch names");
    check(strcmp(a7800_target_short_name(A7800_TARGET_PORT(0, A7800_ACT_BUTTON1)), "Button 1") == 0, "short names");

    /* native key codes: evdev KEY_1 is 2, Windows scancode 0x02 is '1', macOS 0x12 is '1' */
    check(a7800session_keysym_from_evdev(2) == '1', "evdev KEY_1 -> '1'");
    check(a7800session_keysym_from_win_scancode(0x02, 0) == '1', "Windows scancode 02 -> '1'");
    check(a7800session_keysym_from_macos_keycode(0x12) == '1', "macOS keycode 0x12 -> '1'");
    check(a7800session_keysym_from_evdev(103) == A7800_KEYSYM_UP, "evdev KEY_UP -> Up");
    check(a7800session_keysym_from_win_scancode(0x48, 1) == A7800_KEYSYM_UP, "Windows E0 48 -> Up");
    check(a7800session_keysym_from_macos_keycode(0x7E) == A7800_KEYSYM_UP, "macOS 0x7E -> Up");

    /* persistence */
    a7800session_free(s);
    s = a7800session_new(&p);
    check(s && a7800session_target_for_key(s, 'x') == A7800_TARGET_PORT(1, A7800_ACT_BUTTON2), "the rebinding persisted");
    b = a7800session_binding_get(s, A7800_TARGET_PORT(1, A7800_ACT_BUTTON1));
    check(b.keysym == 0 && b.button == A7800_PAD_BTN_NONE, "so did the cleared target (bindings_init ran at session start)");
    a7800session_bindings_reset(s);
    check(a7800session_target_for_key(s, 'x') == A7800_TARGET_PORT(0, A7800_ACT_BUTTON2), "reset restores the defaults");
    a7800session_free(s);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
