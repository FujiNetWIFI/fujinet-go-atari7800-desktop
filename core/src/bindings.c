/*
 * bindings -- the remappable key/gamepad table, the keyboard translator,
 * and every name the bindings editor and the settings store need.
 *
 * One flat target table (a7800session.h's A7800_TARGET_* indices): per port
 * the joystick's four directions and two buttons; then the console's
 * switches; then the session's own actions.
 * Each target holds at most one keysym and one gamepad button. Rebinding
 * STEALS -- a key drives exactly one target, because one keystroke doing
 * two things is worse than losing the old binding: the second effect is
 * invisible until it matters.
 *
 * Seeds its defaults LAZILY so the table is usable with no session behind
 * it: a unit test with no settings store gets the documented default map.
 * Persisted as one packed "bindings" key holding only the entries that
 * differ from the defaults ("<target>.k:<keysym>" / "<target>.b:<button>").
 *
 * Keysyms are X11's, folded to lower case before lookup so a binding made
 * with 'w' still fires while Shift is held for something else.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hid_keys.h"
#include "session_internal.h"

typedef struct {
    uint32_t keysym;
    int button;
} slot;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static slot s_table[A7800_TARGET_COUNT];
static slot s_defaults[A7800_TARGET_COUNT];
static int s_seeded;
static struct a7800session *s_store;   /* whose settings hold "bindings" */

/* ---- defaults ------------------------------------------------------------- */

static void seed_key(int target, uint32_t keysym)
{
    s_defaults[target].keysym = keysym;
}

static void seed_button(int target, int button)
{
    s_defaults[target].button = button;
}

/* Player 1 on the arrows with Z = left button (Button 1) and X = right
 * button (the ProLine's own left-to-right order; a 2600 joystick's one
 * button and a light gun's trigger are Button 1); player 2 on I J K L with
 * N and M. The console's Select, Reset and Pause are F1, F2 and F3 -- the
 * Atari 2600 sibling's Select / Reset / Color-B&W keys, and the 7800's Pause
 * is that switch in 2600 mode. The difficulty switches toggle from the
 * Console menu (Alt+L / Alt+R in the frontends). Escape is Reboot to
 * CONFIG. F5/F7/F8/F12 are the family's debugger keys and stay free of the
 * machine. */
static void compute_defaults_locked(void)
{
    int t;
    for (t = 0; t < A7800_TARGET_COUNT; t++) {
        s_defaults[t].keysym = 0;
        s_defaults[t].button = A7800_PAD_BTN_NONE;
    }

    /* player 1 */
    seed_key(A7800_TARGET_PORT(0, A7800_ACT_UP), A7800_KEYSYM_UP);
    seed_key(A7800_TARGET_PORT(0, A7800_ACT_DOWN), A7800_KEYSYM_DOWN);
    seed_key(A7800_TARGET_PORT(0, A7800_ACT_LEFT), A7800_KEYSYM_LEFT);
    seed_key(A7800_TARGET_PORT(0, A7800_ACT_RIGHT), A7800_KEYSYM_RIGHT);
    seed_key(A7800_TARGET_PORT(0, A7800_ACT_BUTTON1), 'z');
    seed_key(A7800_TARGET_PORT(0, A7800_ACT_BUTTON2), 'x');

    /* player 2 */
    seed_key(A7800_TARGET_PORT(1, A7800_ACT_UP), 'i');
    seed_key(A7800_TARGET_PORT(1, A7800_ACT_DOWN), 'k');
    seed_key(A7800_TARGET_PORT(1, A7800_ACT_LEFT), 'j');
    seed_key(A7800_TARGET_PORT(1, A7800_ACT_RIGHT), 'l');
    seed_key(A7800_TARGET_PORT(1, A7800_ACT_BUTTON1), 'n');
    seed_key(A7800_TARGET_PORT(1, A7800_ACT_BUTTON2), 'm');

    /* the console and the session */
    seed_key(A7800_TARGET_SWITCH(A7800_SW_SELECT), A7800_KEYSYM_F1);
    seed_key(A7800_TARGET_SWITCH(A7800_SW_RESET), A7800_KEYSYM_F2);
    seed_key(A7800_TARGET_SWITCH(A7800_SW_PAUSE), A7800_KEYSYM_F3);
    seed_key(A7800_TARGET_SYSACT(A7800_SYSACT_REBOOT_CONFIG), A7800_KEYSYM_ESCAPE);

    /* gamepad defaults, uniform on both ports, by position: South is the
     * left button (Button 1, the one every 7800 game uses), East the right
     * one. Back and Start are the console's Select and Reset, and the left
     * shoulder its Pause. Directions come from the D-pad and the left stick
     * in gamepad_sdl.c, not from bindings. */
    {
        int port;
        for (port = 0; port < 2; port++) {
            seed_button(A7800_TARGET_PORT(port, A7800_ACT_BUTTON1), A7800_PAD_BTN_SOUTH);
            seed_button(A7800_TARGET_PORT(port, A7800_ACT_BUTTON2), A7800_PAD_BTN_EAST);
        }
        seed_button(A7800_TARGET_SWITCH(A7800_SW_SELECT), A7800_PAD_BTN_BACK);
        seed_button(A7800_TARGET_SWITCH(A7800_SW_RESET), A7800_PAD_BTN_START);
        seed_button(A7800_TARGET_SWITCH(A7800_SW_PAUSE), A7800_PAD_BTN_LEFT_SHOULDER);
    }
}

static uint32_t fold(uint32_t keysym)
{
    if (keysym >= 'A' && keysym <= 'Z') return keysym + 32;
    /* the numeric keypad's digits mean the digits: one binding drives both */
    if (keysym >= A7800_KEYSYM_KP_0 && keysym <= A7800_KEYSYM_KP_9)
        return '0' + (keysym - A7800_KEYSYM_KP_0);
    if (keysym == A7800_KEYSYM_KP_ENTER) return A7800_KEYSYM_RETURN;
    if (keysym == A7800_KEYSYM_KP_MULTIPLY) return '*';
    if (keysym == A7800_KEYSYM_KP_DIVIDE) return '/';
    if (keysym == A7800_KEYSYM_KP_PERIOD) return '.';
    return keysym;
}

/* ---- persistence ---------------------------------------------------------- */

static void apply_persisted_locked(const char *packed)
{
    const char *p = packed;
    while (p && *p) {
        int target = -1; char kind = 0; long value = 0;
        const char *end = strchr(p, ' ');
        if (sscanf(p, "%d.%c:%ld", &target, &kind, &value) == 3
            && target >= 0 && target < A7800_TARGET_COUNT) {
            if (kind == 'k') s_table[target].keysym = (uint32_t)value;
            else if (kind == 'b') s_table[target].button = (int)value;
        }
        if (!end) break;
        p = end + 1;
    }
}

static void pack_locked(char *out, size_t outsz)
{
    size_t len = 0;
    int t;
    out[0] = '\0';
    for (t = 0; t < A7800_TARGET_COUNT; t++) {
        if (s_table[t].keysym != s_defaults[t].keysym)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.k:%u",
                                    len ? " " : "", t, s_table[t].keysym);
        if (len >= outsz) break;
        if (s_table[t].button != s_defaults[t].button)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.b:%d",
                                    len ? " " : "", t, s_table[t].button);
        if (len >= outsz) break;
    }
}

static void persist_locked(void)
{
    char packed[8192];
    if (!s_store) return;
    pack_locked(packed, sizeof packed);
    a7800session_set_str(s_store, "bindings", packed);
}

static void ensure_seeded_locked(void)
{
    if (s_seeded) return;
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
}

void bindings_init(struct a7800session *s)
{
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    s_store = s;
    if (s)
        apply_persisted_locked(a7800session_get_str(s, "bindings", ""));
    pthread_mutex_unlock(&s_lock);
}

/* ---- lookups -------------------------------------------------------------- */

static int target_for_key_locked(uint32_t keysym)
{
    int t;
    if (!keysym) return -1;
    for (t = 0; t < A7800_TARGET_COUNT; t++)
        if (s_table[t].keysym == keysym) return t;
    return -1;
}

int a7800session_target_for_key(a7800session *s, uint32_t keysym)
{
    int t;
    (void)s;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    t = target_for_key_locked(fold(keysym));
    pthread_mutex_unlock(&s_lock);
    return t;
}

/* Gamepad buttons are scoped to the pad's port: the same button index is
 * free to mean different things on different ports, so only that port's
 * targets and the machine-wide switches/actions are searched. */
int a7800session_target_for_button(a7800session *s, int port, int button)
{
    int t, found = -1;
    (void)s;
    if (button == A7800_PAD_BTN_NONE) return -1;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    for (t = 0; t < A7800_ACT_PER_PORT; t++)
        if (s_table[A7800_TARGET_PORT(port, t)].button == button) {
            found = A7800_TARGET_PORT(port, t);
            break;
        }
    if (found < 0)
        for (t = 2 * A7800_ACT_PER_PORT; t < A7800_TARGET_COUNT; t++)
            if (s_table[t].button == button) { found = t; break; }
    pthread_mutex_unlock(&s_lock);
    return found;
}

a7800_binding a7800session_binding_get(a7800session *s, int target)
{
    a7800_binding b = { 0, A7800_PAD_BTN_NONE };
    (void)s;
    if (target < 0 || target >= A7800_TARGET_COUNT) return b;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    b.keysym = s_table[target].keysym;
    b.button = s_table[target].button;
    pthread_mutex_unlock(&s_lock);
    return b;
}

/* A key may legitimately drive several targets (two players' controls can
 * be put on one key by hand). An explicit rebinding is the user saying
 * "this key does exactly this", so it displaces every holder -- and names
 * them all, comma-separated, so nothing is lost silently. */
static void describe_stolen(const int *victims, int nvictims, char *stolen,
                            int stolensz)
{
    int i, len = 0;
    if (!stolen || stolensz <= 0) return;
    stolen[0] = '\0';
    for (i = 0; i < nvictims && len < stolensz; i++)
        len += snprintf(stolen + len, (size_t)(stolensz - len), "%s%s",
                        i ? ", " : "", a7800_target_name(victims[i]));
}

void a7800session_binding_set_key(a7800session *s, int target, uint32_t keysym,
                                  char *stolen, int stolensz)
{
    int victims[A7800_TARGET_COUNT], nvictims = 0, t;
    (void)s;
    if (target < 0 || target >= A7800_TARGET_COUNT) return;
    keysym = fold(keysym);
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (keysym) {
        for (t = 0; t < A7800_TARGET_COUNT; t++)
            if (t != target && s_table[t].keysym == keysym) {
                s_table[t].keysym = 0;
                victims[nvictims++] = t;
            }
    }
    s_table[target].keysym = keysym;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void a7800session_binding_set_button(a7800session *s, int target, int button,
                                     char *stolen, int stolensz)
{
    int victims[A7800_TARGET_COUNT], nvictims = 0, t;
    int port = target / A7800_ACT_PER_PORT;   /* 2 = switches/sysactions */
    (void)s;
    if (target < 0 || target >= A7800_TARGET_COUNT) return;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (button != A7800_PAD_BTN_NONE) {
        for (t = 0; t < A7800_TARGET_COUNT; t++) {
            int tport = t / A7800_ACT_PER_PORT;
            /* steal only within the same scope: this port, or the
             * machine-wide targets, which every pad reaches */
            if (t != target && s_table[t].button == button
                && (tport == port || tport >= 2 || port >= 2)) {
                s_table[t].button = A7800_PAD_BTN_NONE;
                victims[nvictims++] = t;
            }
        }
    }
    s_table[target].button = button;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void a7800session_bindings_reset(a7800session *s)
{
    (void)s;
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    if (s_store) a7800session_set_str(s_store, "bindings", "");
    pthread_mutex_unlock(&s_lock);
}

/* ---- names ---------------------------------------------------------------- */

static const char *const act_names[A7800_ACT_PER_PORT] = {
    "Up", "Down", "Left", "Right", "Button 1 (left)", "Button 2 (right)",
};
static const char *const act_short[A7800_ACT_PER_PORT] = {
    "Up", "Down", "Left", "Right", "Button 1", "Button 2",
};
static const char *const switch_names[A7800_SW_COUNT] = {
    "Select", "Reset", "Pause", "Left Difficulty", "Right Difficulty",
};
static const char *const sysact_names[A7800_SYSACT_COUNT] = {
    "Reboot to CONFIG", "Pause (debugger)",
};

const char *a7800_target_name(int target)
{
    static char buf[64];
    if (target < 0 || target >= A7800_TARGET_COUNT) return "";
    if (target < 2 * A7800_ACT_PER_PORT) {
        snprintf(buf, sizeof buf, "Player %d: %s", target < A7800_ACT_PER_PORT ? 1 : 2,
                 act_names[target % A7800_ACT_PER_PORT]);
        return buf;
    }
    target -= 2 * A7800_ACT_PER_PORT;
    if (target < A7800_SW_COUNT) return switch_names[target];
    return sysact_names[target - A7800_SW_COUNT];
}

const char *a7800_target_short_name(int target)
{
    if (target < 0 || target >= A7800_TARGET_COUNT) return "";
    if (target < 2 * A7800_ACT_PER_PORT) return act_short[target % A7800_ACT_PER_PORT];
    target -= 2 * A7800_ACT_PER_PORT;
    if (target < A7800_SW_COUNT) return switch_names[target];
    return sysact_names[target - A7800_SW_COUNT];
}

static const char *const pad_button_names[A7800_PAD_BTN_COUNT] = {
    "A", "B", "X", "Y", "Back", "Guide", "Start", "Left Stick", "Right Stick",
    "Left Shoulder", "Right Shoulder", "D-pad Up", "D-pad Down", "D-pad Left",
    "D-pad Right", "Left Trigger", "Right Trigger",
};

const char *a7800_pad_button_name(int button)
{
    static char buf[32];
    if (A7800_PAD_BTN_IS_NAMED(button)) return pad_button_names[button];
    if (A7800_PAD_BTN_IS_RAW(button)) {
        snprintf(buf, sizeof buf, "Button %d", button - A7800_PAD_BTN_RAW_BASE);
        return buf;
    }
    if (A7800_PAD_BTN_IS_HAT(button)) {
        static const char *const dirs[A7800_PAD_HAT_DIRS] =
            { "Up", "Up-Right", "Right", "Down-Right", "Down", "Down-Left",
              "Left", "Up-Left" };
        int h = (button - A7800_PAD_HAT_BASE) / A7800_PAD_HAT_DIRS;
        int d = (button - A7800_PAD_HAT_BASE) % A7800_PAD_HAT_DIRS;
        snprintf(buf, sizeof buf, "Hat %d %s", h, dirs[d]);
        return buf;
    }
    return "";
}

int a7800session_keysym_name(uint32_t keysym, char *dst, int dstsz)
{
    const char *name = NULL;
    if (!dst || dstsz <= 0) return 0;
    switch (keysym) {
    case 0: name = ""; break;
    case A7800_KEYSYM_UP: name = "Up"; break;
    case A7800_KEYSYM_DOWN: name = "Down"; break;
    case A7800_KEYSYM_LEFT: name = "Left"; break;
    case A7800_KEYSYM_RIGHT: name = "Right"; break;
    case A7800_KEYSYM_ESCAPE: name = "Escape"; break;
    case A7800_KEYSYM_RETURN: name = "Return"; break;
    case A7800_KEYSYM_BACKSPACE: name = "Backspace"; break;
    case A7800_KEYSYM_TAB: name = "Tab"; break;
    case A7800_KEYSYM_SPACE: name = "Space"; break;
    case A7800_KEYSYM_LSHIFT: name = "Left Shift"; break;
    case A7800_KEYSYM_RSHIFT: name = "Right Shift"; break;
    case A7800_KEYSYM_LCTRL: name = "Left Ctrl"; break;
    case A7800_KEYSYM_RCTRL: name = "Right Ctrl"; break;
    case A7800_KEYSYM_LALT: name = "Left Alt"; break;
    case A7800_KEYSYM_RALT: name = "Right Alt"; break;
    case A7800_KEYSYM_KP_ENTER: name = "Keypad Enter"; break;
    case A7800_KEYSYM_KP_MULTIPLY: name = "Keypad *"; break;
    case A7800_KEYSYM_KP_DIVIDE: name = "Keypad /"; break;
    case A7800_KEYSYM_KP_PERIOD: name = "Keypad ."; break;
    case 0xffff: name = "Delete"; break;
    case 0xff63: name = "Insert"; break;
    case 0xff50: name = "Home"; break;
    case 0xff57: name = "End"; break;
    case 0xff55: name = "Page Up"; break;
    case 0xff56: name = "Page Down"; break;
    case 0xffe5: name = "Caps Lock"; break;
    default: break;
    }
    if (name) return snprintf(dst, (size_t)dstsz, "%s", name);
    if (keysym >= A7800_KEYSYM_F1 && keysym <= A7800_KEYSYM_F12)
        return snprintf(dst, (size_t)dstsz, "F%u", keysym - A7800_KEYSYM_F1 + 1);
    if (keysym >= A7800_KEYSYM_KP_0 && keysym <= A7800_KEYSYM_KP_9)
        return snprintf(dst, (size_t)dstsz, "Keypad %u", keysym - A7800_KEYSYM_KP_0);
    if (keysym >= 0x21 && keysym <= 0x7e) {
        char c = (char)keysym;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        return snprintf(dst, (size_t)dstsz, "%c", c);
    }
    if (keysym >= A7800SESSION_KEYSYM_HID_BASE
        && keysym <= A7800SESSION_KEYSYM_HID_BASE + A7800SESSION_HID_USAGE_MAX) {
        const char *hid = a7800_hid_usage_name(keysym - A7800SESSION_KEYSYM_HID_BASE);
        if (hid) return snprintf(dst, (size_t)dstsz, "%s", hid);
        return snprintf(dst, (size_t)dstsz, "HID 0x%02X",
                        (unsigned)(keysym - A7800SESSION_KEYSYM_HID_BASE));
    }
    return snprintf(dst, (size_t)dstsz, "Key 0x%X", keysym);
}

/* ---- the keyboard translator --------------------------------------------- */

int a7800session_key_sysaction(a7800session *s, uint32_t keysym)
{
    int t;
    t = a7800session_target_for_key(s, keysym);
    if (t < A7800_TARGET_SYSACT(0)) return -1;
    return t - A7800_TARGET_SYSACT(0);
}

int a7800session_key(a7800session *s, uint32_t keysym, int down)
{
    uint32_t folded = fold(keysym);
    int t;

    if (!s) return 0;
    if (down) {
        int hit = 0;
        /* every target the key drives; a system action is the frontend's
         * to fire and is not a machine input */
        pthread_mutex_lock(&s_lock);
        ensure_seeded_locked();
        for (t = 0; t < A7800_TARGET_SYSACT(0); t++) {
            if (s_table[t].keysym != folded) continue;
            hit = 1;
            if (s->held_keysym[t] == folded) continue;  /* auto-repeat */
            s->held_keysym[t] = folded;
            pthread_mutex_unlock(&s_lock);
            a7800session_press(s, t, 1);
            pthread_mutex_lock(&s_lock);
        }
        pthread_mutex_unlock(&s_lock);
        return hit;
    }
    {
        int hit = 0;
        /* remembered by the key, so a release clears exactly what its press
         * asserted even if the binding changed in between */
        for (t = 0; t < A7800_TARGET_COUNT; t++) {
            if (s->held_keysym[t] == folded) {
                s->held_keysym[t] = 0;
                a7800session_press(s, t, 0);
                hit = 1;
            }
        }
        return hit;
    }
}

void a7800session_release_all(a7800session *s)
{
    int t;
    if (!s) return;
    for (t = 0; t < A7800_TARGET_COUNT; t++) {
        if (s->held_keysym[t]) {
            s->held_keysym[t] = 0;
            a7800session_press(s, t, 0);
        }
    }
}
