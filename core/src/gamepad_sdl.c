/*
 * gamepad_sdl -- SDL3 gamepads (and unmapped joysticks) as Atari 7800
 * joysticks, on their own polling thread.
 *
 * A polling thread rather than SDL's event queue on the UI thread: this
 * app's video belongs to GTK/Qt/AppKit/Win32, not SDL, and only the gamepad
 * subsystem is initialised here -- never SDL video. Devices are added and
 * removed as they appear on the system: SDL posts the JOYSTICK_ and
 * GAMEPAD_ add/remove pairs and both are taken, with idempotent open/close,
 * so neither kind of device can be missed. A device SDL has no gamepad
 * mapping for (a plain HID adapter) is opened as a raw joystick instead:
 * its buttons and hats are bindable in the raw bands, just unnamed.
 *
 * What a pad drives is the port's joystick: the D-pad (and, with "analog
 * sticks drive the joystick" on, the left stick as an eight-way switch with
 * hysteresis) is the stick; its two buttons come from the bindings table. A
 * port set to "None" takes nothing.
 *
 * Buttons go through the bindings table (a port's buttons, the console's
 * switches, the system actions); a D-pad direction the user has bound to
 * something explicit stops also nudging the directional role.
 *
 * Pads are assigned to ports in connection order unless assigned explicitly.
 * A pad that is unplugged and plugged back in (matched by its SDL GUID and
 * name) gets its explicit assignment back, and every connect/disconnect is
 * reported to the session for the frontend's toast.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gamepad_sdl.h"
#include "session_internal.h"

#define MAX_PADS 8
#define MAX_HATS 4

/* A third of full deflection engages, a sixth releases: low enough that a
 * worn stick still registers, high enough that resting drift does not, and
 * a gap wide enough that jitter across one threshold cannot flutter. */
#define STICK_ENTER 0.35f
#define STICK_EXIT  0.15f
#define TRIGGER_ON  0.5f

typedef struct {
    SDL_JoystickID id;
    SDL_Gamepad *handle;      /* mapped gamepad, or NULL */
    SDL_Joystick *joy;        /* raw joystick when not a gamepad */
    int assign;               /* explicit port, -1 = automatic */
    int port;                 /* the port it drove on the last poll, or -1 */
    int stick_dir;            /* left stick's four-way state (hysteresis) */
    int dpad_dir;             /* D-pad / hat 0 state */
    uint8_t sent[A7800_ACT_PER_PORT]; /* directional actions currently held */
    SDL_GUID guid;
    char name[64];
    int trigger_down[2];
    Uint8 last_hat[MAX_HATS];
} pad_slot;

typedef struct {
    struct a7800session *session;
    pthread_t thread;
    pthread_mutex_t lock;
    int running;
    volatile int stop_requested;
    pad_slot pads[MAX_PADS];
    int npads;
    unsigned generation;
    /* Map-mode capture */
    int cap_active, cap_have, cap_result;
    /* explicit assignments of pads that went away, for when they come back */
    struct { SDL_GUID guid; char name[64]; int assign; int used; } gone[MAX_PADS];
    int gone_next;
} gamepad_state;

static gamepad_state *g_state;   /* one per process: SDL's subsystem is */

/* ---- pure helpers ----------------------------------------------------------- */

int a7800_dir_from_stick(float x, float y, int prev, float enter, float exit)
{
    int dir = 0;
    /* each axis independently, so diagonals are two bits and a stick that
     * is exactly horizontal never picks up a vertical bit from noise */
    if (x <= -enter || (x <= -exit && (prev & A7800_DIR_LEFT))) dir |= A7800_DIR_LEFT;
    if (x >=  enter || (x >=  exit && (prev & A7800_DIR_RIGHT))) dir |= A7800_DIR_RIGHT;
    if (y <= -enter || (y <= -exit && (prev & A7800_DIR_UP))) dir |= A7800_DIR_UP;
    if (y >=  enter || (y >=  exit && (prev & A7800_DIR_DOWN))) dir |= A7800_DIR_DOWN;
    return dir;
}

int a7800_pad_for_port(const int *assign, int npads, int port)
{
    int i, next = 0;
    for (i = 0; i < npads; i++)
        if (assign[i] == port) return i;
    for (i = 0; i < npads; i++) {
        if (assign[i] >= 0) continue;   /* explicitly elsewhere */
        if (next == port) return i;
        next++;
    }
    return -1;
}

int a7800_pad_button_from_sdl(uint8_t button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return A7800_PAD_BTN_SOUTH;
    case SDL_GAMEPAD_BUTTON_EAST: return A7800_PAD_BTN_EAST;
    case SDL_GAMEPAD_BUTTON_WEST: return A7800_PAD_BTN_WEST;
    case SDL_GAMEPAD_BUTTON_NORTH: return A7800_PAD_BTN_NORTH;
    case SDL_GAMEPAD_BUTTON_BACK: return A7800_PAD_BTN_BACK;
    case SDL_GAMEPAD_BUTTON_GUIDE: return A7800_PAD_BTN_GUIDE;
    case SDL_GAMEPAD_BUTTON_START: return A7800_PAD_BTN_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return A7800_PAD_BTN_LEFT_STICK;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return A7800_PAD_BTN_RIGHT_STICK;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return A7800_PAD_BTN_LEFT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return A7800_PAD_BTN_RIGHT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return A7800_PAD_BTN_DPAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return A7800_PAD_BTN_DPAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return A7800_PAD_BTN_DPAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return A7800_PAD_BTN_DPAD_RIGHT;
    default: return A7800_PAD_BTN_NONE;
    }
}

int a7800_sdl_button_from_pad(int button)
{
    switch (button) {
    case A7800_PAD_BTN_SOUTH: return SDL_GAMEPAD_BUTTON_SOUTH;
    case A7800_PAD_BTN_EAST: return SDL_GAMEPAD_BUTTON_EAST;
    case A7800_PAD_BTN_WEST: return SDL_GAMEPAD_BUTTON_WEST;
    case A7800_PAD_BTN_NORTH: return SDL_GAMEPAD_BUTTON_NORTH;
    case A7800_PAD_BTN_BACK: return SDL_GAMEPAD_BUTTON_BACK;
    case A7800_PAD_BTN_GUIDE: return SDL_GAMEPAD_BUTTON_GUIDE;
    case A7800_PAD_BTN_START: return SDL_GAMEPAD_BUTTON_START;
    case A7800_PAD_BTN_LEFT_STICK: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case A7800_PAD_BTN_RIGHT_STICK: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case A7800_PAD_BTN_LEFT_SHOULDER: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case A7800_PAD_BTN_RIGHT_SHOULDER: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case A7800_PAD_BTN_DPAD_UP: return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case A7800_PAD_BTN_DPAD_DOWN: return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case A7800_PAD_BTN_DPAD_LEFT: return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case A7800_PAD_BTN_DPAD_RIGHT: return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    default: return -1;
    }
}

/* ---- slots ------------------------------------------------------------------ */

static int slot_for_id(gamepad_state *g, SDL_JoystickID id)
{
    int i;
    for (i = 0; i < g->npads; i++)
        if (g->pads[i].id == id) return i;
    return -1;
}

static const char *slot_name(const pad_slot *slot)
{
    const char *n = slot->handle ? SDL_GetGamepadName(slot->handle)
                                 : SDL_GetJoystickName(slot->joy);
    return n ? n : "Gamepad";
}

static void slot_release_all(gamepad_state *g, pad_slot *slot)
{
    int a;
    if (slot->port < 0) return;
    for (a = 0; a < A7800_ACT_PER_PORT; a++)
        if (slot->sent[a]) {
            slot->sent[a] = 0;
            session_gamepad_apply(g->session, slot->port, a, 0);
        }
}

static void slot_close(gamepad_state *g, pad_slot *slot)
{
    slot_release_all(g, slot);
    if (slot->handle) SDL_CloseGamepad(slot->handle);
    if (slot->joy) SDL_CloseJoystick(slot->joy);
    slot->handle = NULL;
    slot->joy = NULL;
}

static int effective_port_locked(gamepad_state *g, int idx);

/* Idempotent, and the single place the gamepad/joystick decision is made:
 * both SDL_EVENT_JOYSTICK_ADDED and SDL_EVENT_GAMEPAD_ADDED land here (SDL
 * posts both for a mapped device) and the slot_for_id guard makes the second
 * one a no-op. */
static void open_pad(gamepad_state *g, SDL_JoystickID id)
{
    pthread_mutex_lock(&g->lock);
    if (g->npads < MAX_PADS && slot_for_id(g, id) < 0) {
        /* Not an else-if on SDL_IsGamepad: a device SDL calls a gamepad can
         * still fail to open as one. Falling through to the raw joystick
         * keeps it usable -- every button is still bindable, just unnamed. */
        SDL_Gamepad *gp = SDL_IsGamepad(id) ? SDL_OpenGamepad(id) : NULL;
        SDL_Joystick *j = gp ? NULL : SDL_OpenJoystick(id);
        if (gp || j) {
            pad_slot *slot = &g->pads[g->npads++];
            int h;
            memset(slot, 0, sizeof *slot);
            slot->id = id;
            slot->handle = gp;
            slot->joy = j;
            slot->assign = -1;
            slot->port = -1;
            for (h = 0; h < MAX_HATS; h++) slot->last_hat[h] = SDL_HAT_CENTERED;
            slot->guid = SDL_GetJoystickGUIDForID(id);
            snprintf(slot->name, sizeof slot->name, "%s", slot_name(slot));
            for (h = 0; h < MAX_PADS; h++) {
                if (g->gone[h].used && !memcmp(&g->gone[h].guid, &slot->guid, sizeof slot->guid)
                    && !strcmp(g->gone[h].name, slot->name)) {
                    slot->assign = g->gone[h].assign;
                    g->gone[h].used = 0;
                    break;
                }
            }
            g->generation++;
            fprintf(stderr, "a7800 gamepad: %s connected (%s)\n", slot->name,
                    gp ? "gamepad" : "raw joystick");
            {
                char msg[128];
                int port = effective_port_locked(g, g->npads - 1);
                if (port >= 0)
                    snprintf(msg, sizeof msg, "Connected: %s (player %d)", slot->name, port + 1);
                else
                    snprintf(msg, sizeof msg, "Connected: %s", slot->name);
                session_gamepad_event(g->session, msg);
            }
        }
    }
    pthread_mutex_unlock(&g->lock);
}

static void close_pad(gamepad_state *g, SDL_JoystickID id)
{
    int idx, i;
    pthread_mutex_lock(&g->lock);
    idx = slot_for_id(g, id);
    if (idx >= 0) {
        pad_slot *slot = &g->pads[idx];
        char msg[128];
        fprintf(stderr, "a7800 gamepad: %s disconnected\n", slot->name);
        snprintf(msg, sizeof msg, "Disconnected: %s", slot->name);
        if (slot->assign >= 0) {
            int k = g->gone_next++ % MAX_PADS;
            g->gone[k].guid = slot->guid;
            memcpy(g->gone[k].name, slot->name, sizeof g->gone[k].name);
            g->gone[k].assign = slot->assign;
            g->gone[k].used = 1;
        }
        slot_close(g, slot);
        session_gamepad_event(g->session, msg);
        for (i = idx; i < g->npads - 1; i++) g->pads[i] = g->pads[i + 1];
        g->npads--;
        g->generation++;
    }
    pthread_mutex_unlock(&g->lock);
}

/* ---- buttons ---------------------------------------------------------------- */

/* The port a slot drives right now. Locked by the caller. */
static int effective_port_locked(gamepad_state *g, int idx)
{
    int assign[MAX_PADS], i, port;
    for (i = 0; i < MAX_PADS; i++) assign[i] = -1;
    for (i = 0; i < g->npads; i++) assign[i] = g->pads[i].assign;
    for (port = 0; port < 2; port++)
        if (a7800_pad_for_port(assign, g->npads, port) == idx) return port;
    return -1;
}

static void dispatch_button(gamepad_state *g, SDL_JoystickID which, int b, int down)
{
    int idx, port, target;
    if (b == A7800_PAD_BTN_NONE) return;

    pthread_mutex_lock(&g->lock);
    if (g->cap_active) {
        /* Map mode: every button event is swallowed while armed, not just
         * the captured one, so the press that maps a button does not also
         * fire whatever it used to do. */
        if (down && !g->cap_have) { g->cap_have = 1; g->cap_result = b; }
        pthread_mutex_unlock(&g->lock);
        return;
    }
    idx = slot_for_id(g, which);
    port = idx >= 0 ? effective_port_locked(g, idx) : -1;
    pthread_mutex_unlock(&g->lock);
    if (port < 0) return;

    target = a7800session_target_for_button(g->session, port, b);
    if (target < 0) return;
    if (target >= A7800_TARGET_SYSACT(0)) {
        if (down) a7800session_sysaction_post(g->session, target - A7800_TARGET_SYSACT(0));
        return;
    }
    if (target < 2 * A7800_ACT_PER_PORT)
        session_gamepad_apply(g->session, target / A7800_ACT_PER_PORT,
                              target % A7800_ACT_PER_PORT, down);
    else
        a7800session_press(g->session, target, down);
}

static int hat_dir_from_mask(Uint8 mask)
{
    switch (mask) {
    case SDL_HAT_UP: return 0;
    case SDL_HAT_RIGHTUP: return 1;
    case SDL_HAT_RIGHT: return 2;
    case SDL_HAT_RIGHTDOWN: return 3;
    case SDL_HAT_DOWN: return 4;
    case SDL_HAT_LEFTDOWN: return 5;
    case SDL_HAT_LEFT: return 6;
    case SDL_HAT_LEFTUP: return 7;
    default: return -1;
    }
}

static void handle_hat_motion(gamepad_state *g, SDL_JoystickID which, int hat, Uint8 mask)
{
    int idx, prev_dir, dir;
    if (hat < 0 || hat >= MAX_HATS) return;
    pthread_mutex_lock(&g->lock);
    idx = slot_for_id(g, which);
    if (idx < 0 || g->pads[idx].handle) { pthread_mutex_unlock(&g->lock); return; }
    prev_dir = hat_dir_from_mask(g->pads[idx].last_hat[hat]);
    g->pads[idx].last_hat[hat] = mask;
    pthread_mutex_unlock(&g->lock);
    dir = hat_dir_from_mask(mask);
    /* hat positions are bindable buttons in their own band; hat 0's four
     * cardinal points also feed the directional role in poll_sticks */
    if (prev_dir >= 0)
        dispatch_button(g, which, A7800_PAD_HAT_BASE + hat * A7800_PAD_HAT_DIRS + prev_dir, 0);
    if (dir >= 0)
        dispatch_button(g, which, A7800_PAD_HAT_BASE + hat * A7800_PAD_HAT_DIRS + dir, 1);
}

/* ---- the directional role and the analog controllers ------------------------ */

static float axis_norm(Sint16 v)
{
    return v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f;
}

static void slot_stick(const pad_slot *slot, int right, float *x, float *y)
{
    if (slot->handle) {
        *x = axis_norm(SDL_GetGamepadAxis(slot->handle, right ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX));
        *y = axis_norm(SDL_GetGamepadAxis(slot->handle, right ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY));
    } else {
        int base = right ? 2 : 0;
        *x = SDL_GetNumJoystickAxes(slot->joy) > base ? axis_norm(SDL_GetJoystickAxis(slot->joy, base)) : 0.f;
        *y = SDL_GetNumJoystickAxes(slot->joy) > base + 1 ? axis_norm(SDL_GetJoystickAxis(slot->joy, base + 1)) : 0.f;
    }
}

static int slot_dpad(gamepad_state *g, const pad_slot *slot, int port)
{
    int dir = 0;
    if (slot->handle) {
        /* a D-pad direction the user bound explicitly no longer plays the
         * directional role -- otherwise a button remapped to a keypad key
         * would also nudge the joystick every poll */
        if (a7800session_target_for_button(g->session, port, A7800_PAD_BTN_DPAD_UP) < 0
            && SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_UP)) dir |= A7800_DIR_UP;
        if (a7800session_target_for_button(g->session, port, A7800_PAD_BTN_DPAD_DOWN) < 0
            && SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) dir |= A7800_DIR_DOWN;
        if (a7800session_target_for_button(g->session, port, A7800_PAD_BTN_DPAD_LEFT) < 0
            && SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) dir |= A7800_DIR_LEFT;
        if (a7800session_target_for_button(g->session, port, A7800_PAD_BTN_DPAD_RIGHT) < 0
            && SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) dir |= A7800_DIR_RIGHT;
    } else if (SDL_GetNumJoystickHats(slot->joy) > 0) {
        Uint8 m = SDL_GetJoystickHat(slot->joy, 0);
        if (m & SDL_HAT_UP) dir |= A7800_DIR_UP;
        if (m & SDL_HAT_DOWN) dir |= A7800_DIR_DOWN;
        if (m & SDL_HAT_LEFT) dir |= A7800_DIR_LEFT;
        if (m & SDL_HAT_RIGHT) dir |= A7800_DIR_RIGHT;
    }
    return dir;
}

static void want_action(uint8_t *want, int act, int on)
{
    if (on) want[act] = 1;
}

static void poll_one(gamepad_state *g, pad_slot *slot, int port)
{
    const struct a7800session *s = g->session;
    uint8_t want[A7800_ACT_PER_PORT];
    int type, dir, a;
    float lx, ly;

    memset(want, 0, sizeof want);
    if (port != slot->port) {
        /* moved to another port (or none): release what it held there */
        slot_release_all(g, slot);
        slot->port = port;
        slot->stick_dir = 0;
        if (port < 0) return;
    }

    /* what the port has now: AUTO is whatever the running game asked for */
    type = s->opts.port_type[port];
    if (type == A7800_CTRL_AUTO) type = s->detected[port];
    slot->dpad_dir = slot_dpad(g, slot, port);
    slot_stick(slot, 0, &lx, &ly);

    /* joysticks take the directions; a light gun aims with the pointer and
     * takes only its trigger (Button 1) from a pad */
    if (type == A7800_CTRL_PROLINE || type == A7800_CTRL_JOY2600) {
        dir = slot->dpad_dir;
        if (s->opts.analog_joystick) {
            slot->stick_dir = a7800_dir_from_stick(lx, ly, slot->stick_dir, STICK_ENTER, STICK_EXIT);
            dir |= slot->stick_dir;
        }
        want_action(want, A7800_ACT_UP, dir & A7800_DIR_UP);
        want_action(want, A7800_ACT_DOWN, dir & A7800_DIR_DOWN);
        want_action(want, A7800_ACT_LEFT, dir & A7800_DIR_LEFT);
        want_action(want, A7800_ACT_RIGHT, dir & A7800_DIR_RIGHT);
    }

    for (a = 0; a < A7800_ACT_PER_PORT; a++) {
        if (want[a] != slot->sent[a]) {
            slot->sent[a] = want[a];
            session_gamepad_apply(g->session, port, a, want[a]);
        }
    }

    /* the triggers, as buttons */
    if (slot->handle) {
        int t;
        for (t = 0; t < 2; t++) {
            float v = axis_norm(SDL_GetGamepadAxis(slot->handle,
                t ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
            int down = v >= TRIGGER_ON;
            if (down != slot->trigger_down[t]) {
                slot->trigger_down[t] = down;
                dispatch_button(g, slot->id, t ? A7800_PAD_BTN_RIGHT_TRIGGER : A7800_PAD_BTN_LEFT_TRIGGER, down);
            }
        }
    }
}

static void poll_sticks(gamepad_state *g)
{
    int i;
    pthread_mutex_lock(&g->lock);
    for (i = 0; i < g->npads; i++)
        poll_one(g, &g->pads[i], effective_port_locked(g, i));
    pthread_mutex_unlock(&g->lock);
}

/* ---- the thread ------------------------------------------------------------- */

static void *thread_main(void *arg)
{
    gamepad_state *g = arg;
    int count = 0, i;
    /* Every joystick, not just the ones SDL has a gamepad mapping for:
     * SDL_GetGamepads would silently omit a plain HID adapter entirely. */
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (ids) {
        for (i = 0; i < count; i++) open_pad(g, ids[i]);
        SDL_free(ids);
    }

    while (!g->stop_requested) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_JOYSTICK_ADDED: open_pad(g, ev.jdevice.which); break;
            case SDL_EVENT_JOYSTICK_REMOVED: close_pad(g, ev.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_ADDED: open_pad(g, ev.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: close_pad(g, ev.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                dispatch_button(g, ev.gbutton.which,
                                a7800_pad_button_from_sdl(ev.gbutton.button),
                                ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP: {
                /* a mapped gamepad also emits these for the same press it
                 * reported as a GAMEPAD_BUTTON event; the slot's kind decides */
                int gamepad, idx;
                pthread_mutex_lock(&g->lock);
                idx = slot_for_id(g, ev.jbutton.which);
                gamepad = idx >= 0 && g->pads[idx].handle != NULL;
                pthread_mutex_unlock(&g->lock);
                if (gamepad || ev.jbutton.button >= 64) break;
                dispatch_button(g, ev.jbutton.which,
                                A7800_PAD_BTN_RAW_BASE + ev.jbutton.button,
                                ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
                break;
            }
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                handle_hat_motion(g, ev.jhat.which, ev.jhat.hat, ev.jhat.value);
                break;
            default:
                break;
            }
        }
        poll_sticks(g);
        SDL_Delay(16);   /* ~60 Hz, the machine's own sampling rate */
    }

    pthread_mutex_lock(&g->lock);
    for (i = 0; i < g->npads; i++) slot_close(g, &g->pads[i]);
    g->npads = 0;
    pthread_mutex_unlock(&g->lock);
    return NULL;
}

int gamepad_start(struct a7800session *s)
{
    gamepad_state *g;
    if (s->gamepad) return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* Bluetooth pads over hidapi and the joystick subsystem together. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        session_set_error(s, "SDL gamepad init failed: %s", SDL_GetError());
        return -1;
    }
    g = calloc(1, sizeof *g);
    if (!g) { SDL_QuitSubSystem(SDL_INIT_GAMEPAD); return -1; }
    g->session = s;
    pthread_mutex_init(&g->lock, NULL);
    /* An explicit stack: the platform default is 512 KB on macOS, and SDL's
     * HID enumeration runs on this thread. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4u << 20);
    if (pthread_create(&g->thread, &attr, thread_main, g) != 0) {
        pthread_attr_destroy(&attr);
        session_set_error(s, "gamepad thread could not start");
        pthread_mutex_destroy(&g->lock);
        free(g);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    pthread_attr_destroy(&attr);
    g->running = 1;
    g_state = g;
    s->gamepad = g;
    return 0;
}

void gamepad_stop(struct a7800session *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    g->stop_requested = 1;
    pthread_join(g->thread, NULL);
    pthread_mutex_destroy(&g->lock);
    s->gamepad = NULL;
    if (g_state == g) g_state = NULL;
    free(g);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

/* ---- public API ------------------------------------------------------------- */

int a7800session_gamepad_count(a7800session *s)
{
    gamepad_state *g = s->gamepad;
    int n;
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    n = g->npads;
    pthread_mutex_unlock(&g->lock);
    return n;
}

int a7800session_gamepad_name(a7800session *s, int idx, char *dst, int dstsz)
{
    gamepad_state *g = s->gamepad;
    int n = 0;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads)
        n = snprintf(dst, (size_t)dstsz, "%s", slot_name(&g->pads[idx]));
    pthread_mutex_unlock(&g->lock);
    return n;
}

void a7800session_gamepad_assign(a7800session *s, int idx, int port)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads)
        g->pads[idx].assign = (port >= 0 && port < 2) ? port : -1;
    pthread_mutex_unlock(&g->lock);
}

int a7800session_gamepad_assignment(a7800session *s, int idx)
{
    gamepad_state *g = s->gamepad;
    int r = -1;
    if (!g) return -1;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) r = g->pads[idx].assign;
    pthread_mutex_unlock(&g->lock);
    return r;
}

int a7800session_gamepad_effective_port(a7800session *s, int idx)
{
    gamepad_state *g = s->gamepad;
    int r = -1;
    if (!g) return -1;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) r = effective_port_locked(g, idx);
    pthread_mutex_unlock(&g->lock);
    return r;
}

unsigned a7800session_gamepad_generation(a7800session *s)
{
    gamepad_state *g = s->gamepad;
    unsigned r;
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    r = g->generation;
    pthread_mutex_unlock(&g->lock);
    return r;
}

void a7800session_gamepad_capture_begin(a7800session *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    g->cap_active = 1; g->cap_have = 0; g->cap_result = A7800_PAD_BTN_NONE;
    pthread_mutex_unlock(&g->lock);
}

void a7800session_gamepad_capture_cancel(a7800session *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    g->cap_active = 0; g->cap_have = 0;
    pthread_mutex_unlock(&g->lock);
}

int a7800session_gamepad_capture_poll(a7800session *s, int *button)
{
    gamepad_state *g = s->gamepad;
    int got = 0;
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    if (g->cap_active && g->cap_have) {
        if (button) *button = g->cap_result;
        g->cap_active = 0; g->cap_have = 0;
        got = 1;
    }
    pthread_mutex_unlock(&g->lock);
    return got;
}
