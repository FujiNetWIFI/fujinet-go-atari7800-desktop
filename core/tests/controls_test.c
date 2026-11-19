/*
 * controls_test -- the Controllers window's layout, pure (no session): two
 * ports each drawn by effective type (ProLine, CX40, light gun, none) over the
 * console strip; every type has the same extent so the window never resizes;
 * every pressable part inside the extent and driving the right target; the hit
 * test (round parts are ellipses, the topmost pressable part wins); the stick
 * drag maths; and the shared colour defines.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "a7800session.h"

static int failures;
static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

static int find_target(const a7800_control *ctl, int num, int target)
{
    int idx;
    for (idx = 0; idx < num; idx++)
        if (ctl[idx].target == target) return idx;
    return -1;
}

static int count_kind(const a7800_control *ctl, int num, int kind, int port)
{
    int idx, total = 0;
    for (idx = 0; idx < num; idx++)
        if (ctl[idx].kind == kind && ctl[idx].port == port) total++;
    return total;
}

static void centre(const a7800_control *ctl, float *xpos, float *ypos)
{
    *xpos = ctl->x + ctl->w / 2;
    *ypos = ctl->y + ctl->h / 2;
}

int main(void)
{
    static const int types[] = { A7800_CTRL_AUTO, A7800_CTRL_PROLINE, A7800_CTRL_JOY2600,
                                 A7800_CTRL_LIGHTGUN, A7800_CTRL_NONE };
    const a7800_control *ctl;
    float width, height, w2, h2, xpos, ypos;
    int num, n2, left, right, idx, port, hit;
    unsigned mask;

    /* every type pair: the same extent, every part inside it, a part count */
    ctl = a7800session_controls_layout(A7800_CTRL_PROLINE, A7800_CTRL_PROLINE, &num, &width, &height);
    check(num > 0 && fabsf(width - 33.0f) < 0.01f, "two 16 u ports and a 1 u gap are 33 u wide");
    check(height > 14.0f && height < 16.0f, "and about 15 u tall");
    for (left = 0; left < 5; left++)
        for (right = 0; right < 5; right++) {
            ctl = a7800session_controls_layout(types[left], types[right], &n2, &w2, &h2);
            check(n2 > 0 && w2 == width && h2 == height, "the extent never depends on the types");
            for (idx = 0; idx < n2; idx++)
                if (ctl[idx].x < -0.001f || ctl[idx].y < -0.001f ||
                    ctl[idx].x + ctl[idx].w > width + 0.001f || ctl[idx].y + ctl[idx].h > height + 0.001f) {
                    printf("part %d (%s) outside the extent\n", idx, ctl[idx].label);
                    check(0, "every part inside the extent");
                    break;
                }
        }

    /* ProLine: stick plus two buttons per port, targets of the port */
    ctl = a7800session_controls_layout(A7800_CTRL_PROLINE, A7800_CTRL_PROLINE, &num, &width, &height);
    for (port = 0; port < 2; port++) {
        check(count_kind(ctl, num, A7800_CTL_STICK, port) == 1, "one stick per ProLine port");
        check(count_kind(ctl, num, A7800_CTL_ROUND_BUTTON, port) == 2, "two red buttons per ProLine port");
        idx = find_target(ctl, num, A7800_TARGET_PORT(port, A7800_ACT_BUTTON1));
        hit = find_target(ctl, num, A7800_TARGET_PORT(port, A7800_ACT_BUTTON2));
        check(idx >= 0 && hit >= 0 && ctl[idx].x < ctl[hit].x, "Button 1 is the left one, Button 2 the right");
        check(ctl[idx].device == A7800_CTRL_PROLINE, "tagged as a ProLine");
    }
    /* the second port is the first, shifted by one port pitch */
    idx = find_target(ctl, num, A7800_TARGET_PORT(0, A7800_ACT_BUTTON1));
    hit = find_target(ctl, num, A7800_TARGET_PORT(1, A7800_ACT_BUTTON1));
    check(fabsf((ctl[hit].x - ctl[idx].x) - 17.0f) < 0.01f && ctl[hit].y == ctl[idx].y, "port 2 is port 1 plus 17 u");

    /* the console strip: three keys with their targets, two difficulty slides */
    check(find_target(ctl, num, A7800_TARGET_SWITCH(A7800_SW_SELECT)) >= 0 &&
          find_target(ctl, num, A7800_TARGET_SWITCH(A7800_SW_RESET)) >= 0 &&
          find_target(ctl, num, A7800_TARGET_SWITCH(A7800_SW_PAUSE)) >= 0, "SELECT, RESET and PAUSE are targets");
    idx = find_target(ctl, num, A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF));
    hit = find_target(ctl, num, A7800_TARGET_SWITCH(A7800_SW_RIGHT_DIFF));
    check(idx >= 0 && hit >= 0 && ctl[idx].kind == A7800_CTL_SWITCH && ctl[hit].kind == A7800_CTL_SWITCH &&
          ctl[idx].x < ctl[hit].x, "two difficulty slides, left then right");
    for (idx = 0; idx < num; idx++)
        if (ctl[idx].target >= A7800_TARGET_SYSACT(0)) check(0, "no system action on the picture");
    check(find_target(ctl, num, A7800_TARGET_SYSACT(A7800_SYSACT_REBOOT_CONFIG)) < 0, "reboot is not drawn");

    /* the 2600 joystick: one fire, which is Button 1; no Button 2 */
    ctl = a7800session_controls_layout(A7800_CTRL_JOY2600, A7800_CTRL_PROLINE, &num, &width, &height);
    check(count_kind(ctl, num, A7800_CTL_ROUND_BUTTON, 0) == 1 &&
          find_target(ctl, num, A7800_TARGET_PORT(0, A7800_ACT_BUTTON1)) >= 0 &&
          ctl[find_target(ctl, num, A7800_TARGET_PORT(0, A7800_ACT_BUTTON1))].device == A7800_CTRL_JOY2600,
          "a CX40 has one fire: Button 1");
    check(count_kind(ctl, num, A7800_CTL_ROUND_BUTTON, 1) == 2, "while port 2 is still a ProLine");
    idx = find_target(ctl, num, A7800_TARGET_PORT(0, A7800_ACT_BUTTON1));
    check(ctl[idx].x > 4.0f && ctl[idx].x + ctl[idx].w < 12.0f, "the CX40 is centred in its slot");

    /* light gun and none: nothing to press on the port, but words */
    ctl = a7800session_controls_layout(A7800_CTRL_LIGHTGUN, A7800_CTRL_NONE, &num, &width, &height);
    check(count_kind(ctl, num, A7800_CTL_STICK, 0) == 0 && count_kind(ctl, num, A7800_CTL_ROUND_BUTTON, 0) == 0 &&
          count_kind(ctl, num, A7800_CTL_STICK, 1) == 0 && count_kind(ctl, num, A7800_CTL_ROUND_BUTTON, 1) == 0,
          "a gun and an empty port have no targets");
    check(find_target(ctl, num, A7800_TARGET_PORT(0, A7800_ACT_BUTTON1)) < 0 &&
          find_target(ctl, num, A7800_TARGET_PORT(1, A7800_ACT_UP)) < 0, "and no port action is listed");
    for (idx = 0; idx < num; idx++)
        if (!strcmp(ctl[idx].label, "Aim on the picture")) break;
    check(idx < num && ctl[idx].kind == A7800_CTL_CAPTION && ctl[idx].device == A7800_CTRL_LIGHTGUN, "the gun's caption");
    for (idx = 0; idx < num; idx++)
        if (!strcmp(ctl[idx].label, "No controller")) break;
    check(idx < num, "the empty socket's words");

    /* hit test: round parts are ellipses; bodies and captions are never hit */
    ctl = a7800session_controls_layout(A7800_CTRL_PROLINE, A7800_CTRL_PROLINE, &num, &width, &height);
    idx = find_target(ctl, num, A7800_TARGET_PORT(1, A7800_ACT_BUTTON2));
    centre(&ctl[idx], &xpos, &ypos);
    check(a7800session_controls_hit(A7800_CTRL_PROLINE, A7800_CTRL_PROLINE, xpos, ypos) == idx, "a button's centre hits it");
    check(a7800session_controls_hit(A7800_CTRL_PROLINE, A7800_CTRL_PROLINE, ctl[idx].x + 0.05f, ctl[idx].y + 0.05f) != idx,
          "its box corner is outside the circle");
    for (idx = 0; idx < num; idx++)
        if (ctl[idx].kind == A7800_CTL_STICK && ctl[idx].port == 0) break;
    centre(&ctl[idx], &xpos, &ypos);
    check(a7800session_controls_hit(A7800_CTRL_PROLINE, A7800_CTRL_PROLINE, xpos, ypos) == idx, "the stick's centre hits it");
    check(a7800session_controls_hit(A7800_CTRL_PROLINE, A7800_CTRL_PROLINE, xpos + ctl[idx].w * 0.49f, ypos + ctl[idx].h * 0.49f) == -1,
          "but not the corner of its box");
    check(a7800session_controls_hit(A7800_CTRL_PROLINE, A7800_CTRL_PROLINE, 15.0f, 0.5f) == -1, "a caption is not a target");
    check(a7800session_controls_hit(A7800_CTRL_PROLINE, A7800_CTRL_PROLINE, -1.0f, -1.0f) == -1, "outside is nothing");
    idx = find_target(ctl, num, A7800_TARGET_SWITCH(A7800_SW_RESET));
    centre(&ctl[idx], &xpos, &ypos);
    check(a7800session_controls_hit(A7800_CTRL_PROLINE, A7800_CTRL_PROLINE, xpos, ypos) == idx, "RESET is hit");
    idx = find_target(ctl, num, A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF));
    centre(&ctl[idx], &xpos, &ypos);
    check(a7800session_controls_hit(A7800_CTRL_PROLINE, A7800_CTRL_PROLINE, xpos, ypos) == idx, "a difficulty slide is hit");
    /* POWER is drawn but is no target */
    for (idx = 0; idx < num; idx++)
        if (!strcmp(ctl[idx].label, "POWER")) break;
    centre(&ctl[idx], &xpos, &ypos);
    check(idx < num && ctl[idx].target == -1 &&
          a7800session_controls_hit(A7800_CTRL_PROLINE, A7800_CTRL_PROLINE, xpos, ypos) == -1, "POWER is not pressable");

    /* the stick drag: a quarter of the reach is nothing, then eight sectors */
    check(a7800session_stick_from_vector(0.0f, 0.0f, 2.5f) == 0, "centre is nothing");
    check(a7800session_stick_from_vector(0.6f, 0.0f, 2.5f) == 0, "inside the dead zone (a quarter of the reach)");
    check(a7800session_stick_from_vector(0.7f, 0.0f, 2.5f) == (1u << A7800_ACT_RIGHT), "just outside it: right");
    check(a7800session_stick_from_vector(-2.0f, 0.1f, 2.5f) == (1u << A7800_ACT_LEFT), "left");
    check(a7800session_stick_from_vector(0.1f, -2.0f, 2.5f) == (1u << A7800_ACT_UP), "up (y is down)");
    check(a7800session_stick_from_vector(0.0f, 2.0f, 2.5f) == (1u << A7800_ACT_DOWN), "down");
    mask = a7800session_stick_from_vector(2.0f, -2.0f, 2.5f);
    check(mask == ((1u << A7800_ACT_RIGHT) | (1u << A7800_ACT_UP)), "up-right");
    mask = a7800session_stick_from_vector(-2.0f, 2.0f, 2.5f);
    check(mask == ((1u << A7800_ACT_LEFT) | (1u << A7800_ACT_DOWN)), "down-left");
    check(a7800session_stick_from_vector(2.0f, 0.7f, 2.5f) == (1u << A7800_ACT_RIGHT), "a shallow angle is still just right");
    check(a7800session_stick_from_vector(2.0f, 1.2f, 2.5f) == ((1u << A7800_ACT_RIGHT) | (1u << A7800_ACT_DOWN)), "a steep one is a diagonal");
    check(a7800session_stick_from_vector(100.0f, 0.0f, 2.5f) == (1u << A7800_ACT_RIGHT), "past the reach is the same");
    check(a7800session_stick_from_vector(1.0f, 0.0f, 0.0f) == 0, "no radius, no direction");

    /* the colours the frontends share */
    check(A7800SESSION_HELD_RGB == 0xFFB04A && A7800SESSION_CASE_RGB == 0x1E1E1F &&
          A7800SESSION_BUTTON_RGB == 0xD9482B && A7800SESSION_DISH_RGB == 0x7A7A7C, "the shared colour defines");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
