/*
 * gamepad_hotplug_test -- gamepads come and go: an SDL virtual pad is picked up
 * as player 1 when plugged in, drives the stick (D-pad, left stick as an
 * eight-way switch with diagonals), both buttons and the console's switches,
 * is its own input source (a key or the window letting go never drops its hold,
 * and it never drops theirs), is released when unplugged while held, gets its
 * chosen port back when plugged in again and on the next run (settings key
 * pad_assign), a pad plugged in after a restart is announced, a port set to a
 * light gun takes only the trigger and one set to None no directions, and a
 * trigger held past half travel does not freeze the session. SKIPs (77) where
 * SDL has no virtual joysticks or real pads are in the way. Waits up to 10 s
 * for each change (slow CI runners).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>

#include "a7800session.h"
#include "test_tmpdir.h"

#define PAD_NAME "a7800test virtual pad"
#define B(act) (1u << (act))
#define WAIT_TRIES 500   /* x 20 ms = 10 s */

static int failures;
static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Waits for a port's merged actions and the console's switch mask to be as
 * wanted (the three momentary switches, one bit per a7800_switch). */
static void expect(a7800session *s, int port, unsigned want, unsigned want_sw, const char *what)
{
    unsigned held = 0, sw = 0;
    int i, k;
    for (i = 0; i < WAIT_TRIES; i++) {
        held = a7800session_buttons_held(s, port);
        sw = 0;
        for (k = 0; k < 3; k++)
            if (a7800session_switch_held(s, k)) sw |= 1u << k;
        if (held == want && sw == want_sw) {
            printf("ok: %s\n", what);
            return;
        }
        sleep_ms(20);
    }
    printf("FAIL: %s (held %02X want %02X, switches %X want %X)\n", what, held, want, sw, want_sw);
    failures++;
}

static SDL_Joystick *plug(SDL_JoystickID *vid)
{
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = PAD_NAME;
    *vid = SDL_AttachVirtualJoystick(&desc);
    return *vid ? SDL_OpenJoystick(*vid) : NULL;
}

static int find_pad(a7800session *s)
{
    int i;
    for (i = 0; i < a7800session_gamepad_count(s); i++) {
        char name[64];
        a7800session_gamepad_name(s, i, name, sizeof name);
        if (!strcmp(name, PAD_NAME))
            return i;
    }
    return -1;
}

static int wait_pad(a7800session *s)
{
    int i, idx = -1;
    for (i = 0; i < WAIT_TRIES && (idx = find_pad(s)) < 0; i++)
        sleep_ms(20);
    return idx;
}

int main(void)
{
    char cfg[512], data[512], ev[160];
    a7800session_paths paths;
    a7800session_start_opts opts;
    a7800session *s;
    SDL_JoystickID vid;
    SDL_Joystick *vj;
    int i, idx;
    unsigned gen;

    test_tmpdir(cfg, sizeof cfg, "pcfg");
    test_tmpdir(data, sizeof data, "pdata");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";
    s = a7800session_new(&paths);
    if (!s) return 1;
    a7800session_default_opts(s, &opts);
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 1;
    check(a7800session_start(s, &opts) == 0, "start");
    sleep_ms(700);
    if (a7800session_gamepad_count(s) > 0) {
        printf("SKIP: real pads attached\n");
        a7800session_free(s);
        return 77;
    }
    gen = a7800session_gamepad_generation(s);
    vj = plug(&vid);
    if (!vj) {
        printf("SKIP: no virtual joysticks: %s\n", SDL_GetError());
        a7800session_free(s);
        return 77;
    }
    idx = wait_pad(s);
    check(idx >= 0 && a7800session_gamepad_effective_port(s, idx) == 0, "plugged in: player 1");
    check(a7800session_gamepad_generation(s) != gen, "generation moved");
    a7800session_gamepad_last_event(s, ev, sizeof ev);
    check(strstr(ev, "Connected") != NULL, "connect event");

    /* defaults */
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect(s, 0, B(A7800_ACT_BUTTON1), 0, "South is Button 1");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, true);
    expect(s, 0, B(A7800_ACT_BUTTON2), 0, "East is Button 2");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    expect(s, 0, B(A7800_ACT_UP), 0, "D-pad up");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    expect(s, 0, B(A7800_ACT_UP) | B(A7800_ACT_RIGHT), 0, "D-pad up-right is a diagonal");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false);
    expect(s, 0, 0, 0, "D-pad released");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_BACK, true);
    expect(s, 0, 0, 1u << A7800_SW_SELECT, "Back is Select");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_BACK, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, true);
    expect(s, 0, 0, 1u << A7800_SW_RESET, "Start is Reset");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    expect(s, 0, 0, 1u << A7800_SW_PAUSE, "the left shoulder is Pause");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
    expect(s, 0, 0, 0, "released");

    /* the left stick as an eight-way switch, diagonals included */
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 30000);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, -30000);
    expect(s, 0, B(A7800_ACT_RIGHT) | B(A7800_ACT_UP), 0, "the stick up-right");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, 0);
    expect(s, 0, 0, 0, "the stick centred");
    a7800session_set_analog(s, 0);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 30000);
    sleep_ms(300);
    check(a7800session_buttons_held(s, 0) == 0, "with analog sticks off the stick plays nothing");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 0);
    a7800session_set_analog(s, 1);

    /* sources: the pad, a key and the window are independent */
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    expect(s, 0, B(A7800_ACT_RIGHT), 0, "the pad holds right");
    a7800session_key(s, A7800_KEYSYM_RIGHT, 1);
    a7800session_key(s, A7800_KEYSYM_RIGHT, 0);
    expect(s, 0, B(A7800_ACT_RIGHT), 0, "a key letting go leaves the pad's right");
    a7800session_key(s, A7800_KEYSYM_LEFT, 1);
    expect(s, 0, 0, 0, "the key's left and the pad's right cancel");
    a7800session_key(s, A7800_KEYSYM_LEFT, 0);
    expect(s, 0, B(A7800_ACT_RIGHT), 0, "and the pad's right is back");
    a7800session_press(s, A7800_TARGET_PORT(0, A7800_ACT_RIGHT), 1);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false);
    expect(s, 0, B(A7800_ACT_RIGHT), 0, "the pad letting go leaves the window's right");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    a7800session_press(s, A7800_TARGET_PORT(0, A7800_ACT_BUTTON1), 1);
    a7800session_press(s, A7800_TARGET_PORT(0, A7800_ACT_BUTTON1), 0);
    expect(s, 0, B(A7800_ACT_RIGHT) | B(A7800_ACT_BUTTON1), 0, "the window letting go leaves the pad's Button 1");
    a7800session_release_all(s);
    expect(s, 0, B(A7800_ACT_BUTTON1), 0, "release_all leaves the pad's hold and drops the window's");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
    expect(s, 0, 0, 0, "the pad's button is gone");

    /* a button bound to a difficulty switch flips it on the press only */
    a7800session_binding_set_button(s, A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF), A7800_PAD_BTN_NORTH, NULL, 0);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_NORTH, true);
    for (i = 0; i < WAIT_TRIES && a7800session_switch_get(s, A7800_SW_LEFT_DIFF) != 0; i++)
        sleep_ms(20);
    check(a7800session_switch_get(s, A7800_SW_LEFT_DIFF) == 0, "North flips the left difficulty to B");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_NORTH, false);
    sleep_ms(300);
    check(a7800session_switch_get(s, A7800_SW_LEFT_DIFF) == 0, "and letting go does not flip it back");
    a7800session_switch_set(s, A7800_SW_LEFT_DIFF, 1);
    a7800session_bindings_reset(s);

    /* the controller types: a gun takes only the trigger, None no directions */
    a7800session_set_port_type(s, 0, A7800_CTRL_LIGHTGUN);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect(s, 0, B(A7800_ACT_BUTTON1), 0, "a light gun port: the trigger, no directions");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
    a7800session_set_port_type(s, 0, A7800_CTRL_NONE);
    sleep_ms(200);
    check(a7800session_buttons_held(s, 0) == 0, "a port with nothing in it: no directions");
    a7800session_set_port_type(s, 0, A7800_CTRL_JOY2600);
    expect(s, 0, B(A7800_ACT_LEFT), 0, "a 2600 joystick: directions again");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, false);
    a7800session_set_port_type(s, 0, A7800_CTRL_AUTO);
    expect(s, 0, 0, 0, "Auto: released");

    /* A trigger past half travel once deadlocked the gamepad thread on its
     * own lock; the Controllers window's timers froze with it. A hang here is
     * a ctest timeout. */
    a7800session_binding_set_button(s, A7800_TARGET_SWITCH(A7800_SW_RESET), A7800_PAD_BTN_LEFT_TRIGGER, NULL, 0);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    expect(s, 0, 0, 1u << A7800_SW_RESET, "the left trigger presses its binding");
    (void)a7800session_gamepad_generation(s);
    (void)a7800session_gamepad_effective_port(s, idx);
    printf("ok: the session is not frozen with a trigger held\n");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    expect(s, 0, 0, 0, "the left trigger released");
    a7800session_gamepad_capture_begin(s);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    {
        int btn = A7800_PAD_BTN_NONE, got = 0;
        for (i = 0; i < WAIT_TRIES && !got; i++) {
            got = a7800session_gamepad_capture_poll(s, &btn);
            if (!got) sleep_ms(20);
        }
        check(got && btn == A7800_PAD_BTN_RIGHT_TRIGGER, "map mode captures a trigger");
    }
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    a7800session_bindings_reset(s);

    /* assigned to player 2 (remembered), unplugged while holding a direction and
     * a switch; the keyboard's own hold on player 1 stays */
    a7800session_gamepad_assign(s, idx, 1);
    check(strstr(a7800session_get_str(s, "pad_assign", ""), PAD_NAME) != NULL &&
          strstr(a7800session_get_str(s, "pad_assign", ""), "=1") != NULL, "the assignment is saved in pad_assign");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    expect(s, 1, B(A7800_ACT_RIGHT), 0, "assigned to player 2");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, true);
    expect(s, 1, B(A7800_ACT_RIGHT), 1u << A7800_SW_RESET, "player 2's pad holds right and Reset");
    a7800session_key(s, 'z', 1);
    expect(s, 0, B(A7800_ACT_BUTTON1), 1u << A7800_SW_RESET, "the keyboard holds player 1's Button 1");
    gen = a7800session_gamepad_generation(s);
    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    expect(s, 1, 0, 0, "unplugged while held: released");
    expect(s, 0, B(A7800_ACT_BUTTON1), 0, "while the keyboard's hold stays");
    a7800session_key(s, 'z', 0);
    expect(s, 0, 0, 0, "and lets go itself");
    for (i = 0; i < WAIT_TRIES && a7800session_gamepad_generation(s) == gen; i++)
        sleep_ms(20);
    a7800session_gamepad_last_event(s, ev, sizeof ev);
    check(strstr(ev, "Disconnected") != NULL && find_pad(s) < 0, "unplug event");

    /* plugged in again: player 2 again */
    vj = plug(&vid);
    idx = wait_pad(s);
    check(idx >= 0 && a7800session_gamepad_effective_port(s, idx) == 1, "replugged: player 2 again");
    check(a7800session_gamepad_assignment(s, idx) == 1, "as an explicit assignment");

    /* and on the next run, from the saved setting (the virtual joystick does not
     * survive SDL's joystick subsystem restarting: plug it in again after) */
    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    a7800session_stop(s);
    check(a7800session_start(s, &opts) == 0, "restart");
    vj = plug(&vid);
    check(vj != NULL, "plug after restart");
    idx = wait_pad(s);
    check(idx >= 0 && a7800session_gamepad_effective_port(s, idx) == 1, "a new run: player 2 again");
    a7800session_gamepad_last_event(s, ev, sizeof ev);
    check(strstr(ev, "Connected") != NULL, "a pad plugged in after a restart is announced");

    /* Automatic forgets it */
    a7800session_gamepad_assign(s, idx, -1);
    check(a7800session_gamepad_effective_port(s, idx) == 0 && a7800session_gamepad_assignment(s, idx) == -1, "Automatic: player 1");
    check(strstr(a7800session_get_str(s, "pad_assign", ""), PAD_NAME) == NULL, "and the setting is cleared");
    a7800session_gamepad_assign(s, idx, 2);
    check(a7800session_gamepad_assignment(s, idx) == -1, "port 3 does not exist");

    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    a7800session_stop(s);
    a7800session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
