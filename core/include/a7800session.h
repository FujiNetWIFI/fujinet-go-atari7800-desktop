/*
 * a7800session -- the toolkit-agnostic desktop session for FujiNet Go
 * Atari 7800.
 *
 * Owns the emulator (MAME's a7800 driver with the FujiNet cartridge, run
 * from libmame_fngo on a thread of its own -- see core/mame/MameHost.h), the
 * SDL audio and gamepad backends, the in-process FujiNet runtime, the shared
 * settings store, the remappable key/pad bindings, the imported BIOS and
 * High Score Cart images and the media path layout. Frontends (GTK4, Qt6,
 * AppKit, Win32) drive this API and do only windowing, painting and event
 * translation. A frontend that needs something which is not one of those
 * three things belongs here instead.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef A7800SESSION_H
#define A7800SESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MARIA paints 320 pixels a line; MAME shows 224 lines of an NTSC frame and
 * 260 of a PAL one. Frontends show it at 4:3 or with square pixels, per the
 * "aspect" setting. */
#define A7800SESSION_FB_WIDTH      320
#define A7800SESSION_FB_MAX_HEIGHT 272

/* FujiNet's BoIP listener and its web admin UI. High ports of this app's own
 * so a standalone fujinet-pc, or a sibling FujiNet Go app, never collides:
 * ADAM uses 65216/65214, Apple II 1985/8000, CoCo 65504, MSX 65505/64003,
 * Intellivision 65503/64003, Astrocade 11500/11501, ColecoVision
 * 11502/11503, Atari 2600 11504/11505, NES 11506/11507, Master System
 * 11508/11509, and the 7800 cartridge's own MAME dev harness uses 9995.
 *
 * Direction: FujiNet LISTENS and the 7800 FujiNet cartridge dials in, as on
 * every sibling. That decides startup ordering -- see a7800session_start. */
#define A7800SESSION_BOIP_PORT  11510
#define A7800SESSION_WEBUI_PORT 11511

/* The host audio device rate; MAME mixes TIA and POKEY to this. */
#define A7800SESSION_AUDIO_RATE 48000

/* The accent colour every frontend uses for its highlights (the Map target,
 * the debugger's current line, the FujiNet status dot): a slightly deeper
 * orange than the Atari 2600 sibling's #FFA645, and the icon's background
 * (tools/icons/make-icons.py). */
#define A7800SESSION_ACCENT_RGB 0xF0932F

/* The app's accent colour (the debugger's Run button, the current line, the
 * FujiNet status dot): the orange of the icon. */
#define A7800SESSION_ACCENT_RGB 0xF0932F
/* 60% of the accent: ink (glyph strokes) drawn on an accent-filled control. */
#define A7800SESSION_ACCENT_DARK_RGB 0x90581C

/* The Controllers window's art (controllers-port/DESIGN-7800.md, read from
 * photos of the real hardware; +/- 8%). One set for every toolkit. */
#define A7800SESSION_CASE_RGB          0x1E1E1F   /* the ProLine's black body, the console's bar */
#define A7800SESSION_CASE_BEVEL_RGB    0x34343A   /* bevel highlight */
#define A7800SESSION_CASE_SHADOW_RGB   0x0E0E10   /* deep shadow */
#define A7800SESSION_PANEL_EDGE_RGB    0x2C2C30   /* the raised triangle panel's edge */
#define A7800SESSION_DISH_RGB          0x7A7A7C   /* the grey stick's dish */
#define A7800SESSION_DISH_RECESS_RGB   0x5E5E60   /* the four notches in it */
#define A7800SESSION_CAP_RGB           0x8A8A8C   /* the stick's round cap */
#define A7800SESSION_CAP_HI_RGB        0xA8A8AA   /* ... its highlight */
#define A7800SESSION_BUTTON_RGB        0xD9482B   /* the two red buttons */
#define A7800SESSION_BUTTON_HI_RGB     0xF07A5A   /* ... their highlight */
#define A7800SESSION_BUTTON_RECESS_RGB 0x101010   /* ... the dark recess round each */
#define A7800SESSION_SWOOSH_RGB        0x161618   /* the ribbed swoosh beside them */
#define A7800SESSION_NUMERAL_RGB       0x4A4A4C   /* the embossed 1 and 2 */
#define A7800SESSION_LOGO_RGB          0xB8B8BC   /* the embossed fuji mark and ATARI */
#define A7800SESSION_CABLE_RGB         0x0E0E0E
#define A7800SESSION_SOCKET_RGB        0x2A2A2E   /* the empty port's outline */
#define A7800SESSION_BAND_HI_RGB       0xD8D8DC   /* the console's brushed silver band (top) */
#define A7800SESSION_BAND_LO_RGB       0x9A9AA0   /* ... (bottom) */
#define A7800SESSION_BAND_TEXT_RGB     0x7C7C82   /* "7800 PROSYSTEM" */
#define A7800SESSION_RAINBOW_ORANGE_RGB 0xE8762A  /* the rainbow line under the band */
#define A7800SESSION_RAINBOW_VIOLET_RGB 0x8A3A8A
#define A7800SESSION_RAINBOW_BLUE_RGB   0x3A5FC8
#define A7800SESSION_RAINBOW_GREEN_RGB  0x58A83A
#define A7800SESSION_KEY_RGB           0x141416   /* SELECT / RESET / PAUSE keys */
#define A7800SESSION_KEY_LABEL_RGB     0xD8D8DC   /* ... their legends */
#define A7800SESSION_SLIDE_SLOT_RGB    0x101010   /* a difficulty slide's slot */
#define A7800SESSION_SLIDE_KNOB_RGB    0xC9C9C9   /* ... its silver slider */
/* The CX40 on a 2600-joystick port (DESIGN-atari.md section 2). */
#define A7800SESSION_CX40_CASE_RGB     0x1C1C1C
#define A7800SESSION_CX40_BEVEL_RGB    0x2A2A2A
#define A7800SESSION_CX40_FIRE_RGB     0xE23B1F
#define A7800SESSION_CX40_FIRE_HI_RGB  0xF05A38
#define A7800SESSION_CX40_RING_RGB     0x101010
#define A7800SESSION_CX40_TICK_RGB     0x3A3A3A
/* A held part: a ring in this colour with a 35% fill of it; a held red button
 * also darkens its fill to HELD_BUTTON_FILL. */
#define A7800SESSION_HELD_RGB          0xFFB04A
#define A7800SESSION_HELD_BUTTON_FILL_RGB 0xB02A14

typedef struct a7800session a7800session;
typedef struct a7800debug a7800debug;

/* All members optional (NULL = default).
 *  config_dir:  default $XDG_CONFIG_HOME/fujinet-go-atari7800
 *  data_dir:    default $XDG_DATA_HOME/fujinet-go-atari7800
 *  fujinet_lib: path to libfujinet.so/.dylib/.dll; default searches
 *               $FUJINET_LIB, the executable's directory, the install
 *               libdir, then tools/fujinet/work/out. "" disables FujiNet.
 *  fujinet_runtime_src: directory holding the pristine fnconfig.ini + data/
 *               + SD/ used to provision the user's runtime tree on first
 *               start (a macOS app passes its bundle's runtime dir). */
typedef struct {
    const char *config_dir;
    const char *data_dir;
    const char *fujinet_lib;
    const char *fujinet_runtime_src;
} a7800session_paths;

a7800session *a7800session_new(const a7800session_paths *paths);
void a7800session_free(a7800session *s);

/* ---- settings (shared INI; one store for every frontend of this target) --- */
int         a7800session_get_int(a7800session *s, const char *key, int def);
void        a7800session_set_int(a7800session *s, const char *key, int value);
const char *a7800session_get_str(a7800session *s, const char *key,
                                 const char *def);
void        a7800session_set_str(a7800session *s, const char *key,
                                 const char *value);
void        a7800session_settings_flush(a7800session *s);

/* ---- machine options ------------------------------------------------------ */

/* The console: NTSC (MAME's a7800) or PAL (a7800p). AUTO follows an opened
 * cartridge's header, then the cartridge's database, and is NTSC
 * otherwise. */
typedef enum {
    A7800_REGION_AUTO = 0, A7800_REGION_NTSC, A7800_REGION_PAL,
    A7800_REGION_COUNT
} a7800_region;

/* Controller ports the console has. */
#define A7800_PORTS 2

/* What is plugged into a controller port. AUTO follows the running game:
 * the light gun for the light-gun titles, a ProLine joystick otherwise. */
typedef enum {
    A7800_CTRL_AUTO = 0, A7800_CTRL_PROLINE, A7800_CTRL_JOY2600,
    A7800_CTRL_LIGHTGUN, A7800_CTRL_NONE,
    A7800_CTRL_COUNT
} a7800_ctrl_type;

/* Human-readable, NULL past the end (for filling combo boxes). */
const char *a7800_region_name(int r);
const char *a7800_ctrl_type_name(int t);

/* ---- lifecycle ------------------------------------------------------------ */
typedef struct {
    const char *cart_path;    /* cartridge image; NULL boots the CONFIG client */
    int region;               /* a7800_region */
    int port_type[2];         /* a7800_ctrl_type per port (0 = player 1) */
    int analog_joystick;      /* gamepad sticks drive the joystick too */
    int hsc;                  /* games get the High Score Cart (needs its ROM) */
    int enable_fujinet;       /* start the in-process FujiNet runtime */
    int external_fujinet;     /* 1: don't start it; the cartridge dials the
                               * FujiNet at fujinet_addr instead */
    const char *fujinet_addr; /* "host:port" or "port" (external only);
                               * NULL/"" = 127.0.0.1:11510 */
    int enable_audio;         /* open the SDL audio device */
    int enable_gamepad;       /* start the SDL gamepad thread */
} a7800session_start_opts;

/* Fills opts from the settings store (keys: cart region port0_type
 * port1_type analog_joystick hsc enable_fujinet fujinet_external
 * fujinet_external_addr enable_audio enable_gamepad). */
void a7800session_default_opts(a7800session *s, a7800session_start_opts *opts);

/* Starts FujiNet (if enabled) and then the emulator.
 *
 * The order is not arbitrary: FujiNet listens and the cartridge dials in, so
 * the listener has to exist before the machine's first transaction or the
 * CONFIG client boots reporting no link. start() brings FujiNet up first and
 * waits briefly for the port.
 *
 * Returns 0, or -1 with a7800session_last_error() set. FujiNet failing to
 * start is NOT fatal: the machine boots with the cartridge reporting the
 * link down, which is far more useful than refusing to run. */
int  a7800session_start(a7800session *s, const a7800session_start_opts *opts);
void a7800session_stop(a7800session *s);
int  a7800session_is_running(const a7800session *s);
const char *a7800session_last_error(const a7800session *s);

/* ---- cartridges ------------------------------------------------------------
 * The FujiNet cartridge is always in the slot; CONFIG is its resident image.
 *
 * load_cart (.a78, .bin, or a .zip/.7z holding one) powers the console on
 * with the image staged in the cartridge, so its boot block, loader and
 * hand-over start it exactly as a network boot would; an image the cartridge
 * cannot map is refused with a reason. The path is remembered in the "cart"
 * setting, so it boots again next start. Returns 0 or -1 + error. */
int  a7800session_load_cart(a7800session *s, const char *path);
const char *a7800session_cart_path(const a7800session *s);
/* 1 if the image can run on the FujiNet cartridge; else 0 and why. */
int  a7800session_check_cart(const char *path, char *why, int whysz);

/* Power cycle: the console's power switch. Whatever was opened with
 * load_cart boots again; a network-booted game gives way to CONFIG, as on
 * the hardware. */
int  a7800session_power_cycle(a7800session *s);
/* Reboot to CONFIG: a power cycle that also forgets an opened cartridge
 * (clears the "cart" setting). Escape by default. */
int  a7800session_reboot_to_config(a7800session *s);
/* Eject: the same as Reboot to CONFIG (there is no "no cartridge" state
 * worth having on a FujiNet cartridge). */
int  a7800session_eject(a7800session *s);

/* ---- video ---------------------------------------------------------------
 * copy_frame copies the latest frame into dst (FB_WIDTH*FB_MAX_HEIGHT uint32
 * XRGB8888 pixels) iff its serial differs from *serial_inout, updates it,
 * writes the frame's line count into *height and returns 1; returns 0 when
 * unchanged, leaving dst alone. Pass 0 to force a copy (e.g. the first paint
 * after a window map). */
int  a7800session_copy_frame(a7800session *s, uint32_t *dst, int *height,
                             uint64_t *serial_inout);
/* Ask to be told when a new frame is ready, for a frontend with no frame
 * clock of its own: cb(ctx) runs on the machine thread after each frame is
 * published. Keep it quick (post an event and return) and do not call into
 * the session from it. A frame identical to the previous one is not
 * published again. Passing NULL stops the calls, and returns only after any
 * call in flight has finished, so ctx may be freed right after. */
typedef void (*a7800session_frame_cb)(void *ctx);
void a7800session_set_frame_callback(a7800session *s, a7800session_frame_cb cb, void *ctx);
/* Frames the machine has run since it started. Unlike copy_frame's serial it
 * counts every frame, including the ones identical to their predecessor:
 * use it to ask "is the machine running", not "is the picture changing". */
uint64_t a7800session_frame_count(a7800session *s);
/* 60 or 50: the running console's refresh rate (rounded). */
int  a7800session_refresh_rate(a7800session *s);

/* Feed the UI's frame-clock ticks (CLOCK_MONOTONIC ns). While a steady
 * stream near the console's refresh rate arrives, the emulator phase-locks
 * one frame per tick; otherwise it paces on the wall clock. A frontend with
 * no frame clock simply never calls this. */
void a7800session_notify_vsync(a7800session *s, int64_t frame_time_ns);

/* ---- audio ---------------------------------------------------------------
 * Owned by the session (SDL) when opts.enable_audio was set. A frontend
 * that wants the device itself can pull interleaved stereo float frames at
 * A7800SESSION_AUDIO_RATE instead. Returns the frames written (silence is
 * written for any shortfall). */
int  a7800session_render_audio(a7800session *s, float *out, int nframes);
void a7800session_set_volume(a7800session *s, int percent);

/* ---- input ---------------------------------------------------------------
 * Every control the machine has, flattened into one target index so the
 * bindings table, the settings store and the frontends can all name them:
 * per port the joystick (a ProLine's two buttons; a 2600 joystick's one
 * button is Button 1; a light gun's trigger is Button 1 too), then the
 * console's switches, then the session's own actions. */
typedef enum {
    A7800_ACT_UP = 0, A7800_ACT_DOWN, A7800_ACT_LEFT, A7800_ACT_RIGHT,
    A7800_ACT_BUTTON1, A7800_ACT_BUTTON2,
    A7800_ACT_PER_PORT
} a7800_action;

typedef enum {
    A7800_SW_SELECT = 0,              /* F1 by default */
    A7800_SW_RESET,                   /* F2: the console's RESET switch */
    A7800_SW_PAUSE,                   /* F3 (in 2600 mode, Color / B&W) */
    A7800_SW_LEFT_DIFF,               /* toggles A (pro) / B (novice) */
    A7800_SW_RIGHT_DIFF,
    A7800_SW_COUNT
} a7800_switch;

typedef enum {
    A7800_SYSACT_REBOOT_CONFIG = 0,   /* Escape by default */
    A7800_SYSACT_PAUSE,               /* stop in the debugger */
    A7800_SYSACT_COUNT
} a7800_sysaction;

#define A7800_TARGET_PORT(port, act) ((port) * A7800_ACT_PER_PORT + (act))
#define A7800_TARGET_SWITCH(sw)      (2 * A7800_ACT_PER_PORT + (sw))
#define A7800_TARGET_SYSACT(sa)      (2 * A7800_ACT_PER_PORT + A7800_SW_COUNT + (sa))
#define A7800_TARGET_COUNT           (2 * A7800_ACT_PER_PORT + A7800_SW_COUNT + A7800_SYSACT_COUNT)

/* Apply one control directly. `down` is press/release. The momentary
 * console switches (Select, Reset, Pause) follow `down`; the difficulty
 * switches TOGGLE on press. */
void a7800session_press(a7800session *s, int target, int down);
/* (a7800session_press is the pointer source's hold: see "input sources".) */
void a7800session_sysaction(a7800session *s, int sysact);
/* Momentary console switches as a whole: press, hold a few frames, release
 * (for menu items and on-screen buttons). */
void a7800session_switch_pulse(a7800session *s, int sw);
/* The difficulty switches' positions: 1 = A (pro), 0 = B (novice). Others
 * report whether they are held. Persisted as left_diff / right_diff. */
int  a7800session_switch_get(a7800session *s, int sw);
void a7800session_switch_set(a7800session *s, int sw, int on);

/* ---- input sources -------------------------------------------------------
 * Every way of holding an input is a source of its own (the keyboard, the
 * Controllers window's pointer, each gamepad's buttons and its stick), and the
 * console sees their merge, opposed directions cancelled. A source can only
 * ever release what it held itself: a key release never drops a pad's hold,
 * and a pad unplugged never drops a key's.
 *
 * a7800session_press (above) and a7800session_pointer_set are the WINDOW's
 * pointer source; a7800session_key is the KEYBOARD's. */
/* The pointer's hold on `port`'s actions, as a whole: bit per a7800_action (the
 * direction bits from a7800session_stick_from_vector, a button bit, or both),
 * ORed with every other source. A drag that changes direction passes the new
 * mask without letting go; pass 0 when the drag ends or the pointer leaves.
 * Distinct from a7800session_pointer (the light gun's aim over the picture). */
void a7800session_pointer_set(a7800session *s, int port, unsigned mask);
/* Release everything the keyboard and the window's pointer hold (focus loss,
 * entering Map mode). A gamepad is a physical thing and is left alone. */
void a7800session_release_all(a7800session *s);
/* Only the window's pointer (a pressed part, the stick drag), or only the
 * keyboard's holds. release_all is both. */
void a7800session_release_pointer(a7800session *s);
void a7800session_release_keyboard(a7800session *s);
/* The keyboard's and the pointer's holds, both (same as release_all; the name
 * the sibling cores use). */
void a7800session_clear_held(a7800session *s);
/* What the console sees held on a port right now (keyboard, gamepads and
 * the on-screen controller together, opposed pairs cancelled), one bit per
 * a7800_action. For a controller window's highlights: the window is a gamepad
 * tester. */
unsigned a7800session_buttons_held(a7800session *s, int port);
/* Whether a console switch is held from any source: Select, Reset and Pause
 * follow their keys (and a menu's pulse); the difficulty switches report their
 * position (1 = A, pro), as a7800session_switch_get. */
int a7800session_switch_held(a7800session *s, int sw);
/* Lit state of a target for the window's highlights: a port action or a
 * momentary switch is 1 while held; a difficulty switch is 1 at A; a system
 * action is never held. */
int a7800session_target_held(a7800session *s, int target);

/* The pointer over the picture, for light guns: (x, y) in frame pixels
 * (0..FB_WIDTH-1, 0..height-1), `inside` whether it is over the picture at
 * all, `buttons` bit 0 the primary (fires every light-gun port), bit 1 the
 * secondary (fires off-screen, which some games use to reload). Ports with a
 * light gun aim where the pointer is; other ports ignore it. */
void a7800session_pointer(a7800session *s, int x, int y, int inside, unsigned buttons);
/* 1 if either port has a light gun now (a frontend then shows a crosshair
 * cursor over the picture). */
int  a7800session_lightgun_active(a7800session *s);

/* ---- keyboard translation ------------------------------------------------
 * keysym is an X11/xkb keysym (== a GDK keyval; Qt, Win32 and AppKit map
 * through the HID tables below), so one bindings table serves every
 * frontend. Returns 1 if the key drives a target (and has been applied /
 * released), 0 if it should be ignored. System actions are reported through
 * a7800session_key_sysaction instead and left to the frontend. */
int  a7800session_key(a7800session *s, uint32_t keysym, int down);
/* The system action a keysym is bound to, or -1. */
int  a7800session_key_sysaction(a7800session *s, uint32_t keysym);

/* Non-printing keys in the keysym space the frontends translate to. */
enum {
    A7800_KEYSYM_NONE = 0,
    A7800_KEYSYM_UP = 0xff52, A7800_KEYSYM_DOWN = 0xff54,
    A7800_KEYSYM_LEFT = 0xff51, A7800_KEYSYM_RIGHT = 0xff53,
    A7800_KEYSYM_ESCAPE = 0xff1b, A7800_KEYSYM_RETURN = 0xff0d,
    A7800_KEYSYM_BACKSPACE = 0xff08, A7800_KEYSYM_TAB = 0xff09,
    A7800_KEYSYM_SPACE = 0x20,
    A7800_KEYSYM_F1 = 0xffbe, A7800_KEYSYM_F2, A7800_KEYSYM_F3, A7800_KEYSYM_F4,
    A7800_KEYSYM_F5, A7800_KEYSYM_F6, A7800_KEYSYM_F7, A7800_KEYSYM_F8,
    A7800_KEYSYM_F9, A7800_KEYSYM_F10, A7800_KEYSYM_F11, A7800_KEYSYM_F12,
    A7800_KEYSYM_LSHIFT = 0xffe1, A7800_KEYSYM_RSHIFT = 0xffe2,
    A7800_KEYSYM_LCTRL = 0xffe3, A7800_KEYSYM_RCTRL = 0xffe4,
    A7800_KEYSYM_LALT = 0xffe9, A7800_KEYSYM_RALT = 0xffea,
    A7800_KEYSYM_KP_0 = 0xffb0, A7800_KEYSYM_KP_1, A7800_KEYSYM_KP_2,
    A7800_KEYSYM_KP_3, A7800_KEYSYM_KP_4, A7800_KEYSYM_KP_5, A7800_KEYSYM_KP_6,
    A7800_KEYSYM_KP_7, A7800_KEYSYM_KP_8, A7800_KEYSYM_KP_9,
    A7800_KEYSYM_KP_ENTER = 0xff8d, A7800_KEYSYM_KP_MULTIPLY = 0xffaa,
    A7800_KEYSYM_KP_DIVIDE = 0xffaf, A7800_KEYSYM_KP_PERIOD = 0xffae,
    A7800_KEYSYM_SCROLL_LOCK = 0xff14
};
/* Native key codes for the platforms whose toolkits do not deliver keysyms:
 * Windows scan code (set 1, with the E0 flag), Linux evdev code (GTK/Qt
 * keycode minus 8) and macOS virtual key code, each mapped to a keysym. 0
 * when unknown. */
uint32_t a7800session_keysym_from_win_scancode(unsigned scancode, int extended);
uint32_t a7800session_keysym_from_evdev(unsigned code);
uint32_t a7800session_keysym_from_macos_keycode(unsigned keycode);
/* Name for a keysym ("F1", "Space", "a", "Keypad 5"); returns length. */
int a7800session_keysym_name(uint32_t keysym, char *dst, int dstsz);

/* ---- remappable bindings --------------------------------------------------
 * Every target can be driven by one keyboard key and one gamepad button.
 * Rebinding STEALS: a key drives exactly one target, because one keystroke
 * doing two things is worse than losing the old binding. Persisted in the
 * settings store under "bindings" as only the entries that differ from the
 * defaults. */
typedef enum {
    A7800_PAD_BTN_NONE = -1,
    A7800_PAD_BTN_SOUTH = 0, A7800_PAD_BTN_EAST, A7800_PAD_BTN_WEST,
    A7800_PAD_BTN_NORTH, A7800_PAD_BTN_BACK, A7800_PAD_BTN_GUIDE,
    A7800_PAD_BTN_START, A7800_PAD_BTN_LEFT_STICK, A7800_PAD_BTN_RIGHT_STICK,
    A7800_PAD_BTN_LEFT_SHOULDER, A7800_PAD_BTN_RIGHT_SHOULDER,
    A7800_PAD_BTN_DPAD_UP, A7800_PAD_BTN_DPAD_DOWN, A7800_PAD_BTN_DPAD_LEFT,
    A7800_PAD_BTN_DPAD_RIGHT,
    A7800_PAD_BTN_LEFT_TRIGGER, A7800_PAD_BTN_RIGHT_TRIGGER,
    A7800_PAD_BTN_COUNT,
    /* Raw joystick bands for devices SDL has no gamepad mapping for (a
     * plain HID adapter): button index, and hat directions (hat*8 + dir). */
    A7800_PAD_BTN_RAW_BASE = 64, A7800_PAD_BTN_RAW_LAST = 127,
    A7800_PAD_HAT_BASE = 192, A7800_PAD_HAT_LAST = 255
} a7800_pad_button;
#define A7800_PAD_HAT_DIRS 8
#define A7800_PAD_BTN_IS_NAMED(b) ((b) >= 0 && (b) < A7800_PAD_BTN_COUNT)
#define A7800_PAD_BTN_IS_RAW(b)   ((b) >= A7800_PAD_BTN_RAW_BASE && (b) <= A7800_PAD_BTN_RAW_LAST)
#define A7800_PAD_BTN_IS_HAT(b)   ((b) >= A7800_PAD_HAT_BASE && (b) <= A7800_PAD_HAT_LAST)

typedef struct {
    uint32_t keysym;     /* 0 = no key */
    int      button;     /* a7800_pad_button, NONE = no gamepad button */
} a7800_binding;

const char *a7800_target_name(int target);           /* "Player 1: Up" */
const char *a7800_target_short_name(int target);     /* "Up" */
a7800_binding a7800session_binding_get(a7800session *s, int target);
/* Bind; the previous holder of the key/button (if any) is described into
 * `stolen` (may be NULL). keysym 0 / button NONE unbinds. */
void a7800session_binding_set_key(a7800session *s, int target, uint32_t keysym,
                                  char *stolen, int stolensz);
void a7800session_binding_set_button(a7800session *s, int target, int button,
                                     char *stolen, int stolensz);
/* Unbind both the key and the pad button of a target. */
void a7800session_binding_clear(a7800session *s, int target);
void a7800session_bindings_reset(a7800session *s);
/* 1 for the keys the frontends own and no binding can take: F9 (the
 * Controllers window), F11 (full screen), F12 (the debugger). A set_key with
 * one is ignored; a stored one is dropped on load. */
int  a7800session_key_reserved(uint32_t keysym);
const char *a7800_pad_button_name(int button);
/* The target a keysym / pad button drives, or -1. */
int a7800session_target_for_key(a7800session *s, uint32_t keysym);
int a7800session_target_for_button(a7800session *s, int port, int button);

/* Map mode: after begin(), the next gamepad button pressed on any pad is
 * reported by poll() (returns 1 and the button). cancel() disarms. */
void a7800session_gamepad_capture_begin(a7800session *s);
void a7800session_gamepad_capture_cancel(a7800session *s);
int  a7800session_gamepad_capture_poll(a7800session *s, int *button);

/* ---- controller types (live) ---------------------------------------------
 * Change what is plugged into a port without restarting -- a booted game
 * survives. Persisted as port0_type / port1_type. */
void a7800session_set_port_type(a7800session *s, int port, int type);
int  a7800session_port_type(a7800session *s, int port);
/* What AUTO resolved to for the running game (never AUTO itself). */
int  a7800session_port_detected(a7800session *s, int port);
/* The controller to draw on a port: what is set, or for AUTO what the game
 * needs; never AUTO. Pass both ports to a7800session_controls_layout. */
int  a7800session_view_type(a7800session *s, int port);
void a7800session_set_analog(a7800session *s, int joystick);

/* ---- region (takes effect at once: a new console) ------------------------*/
void a7800session_set_region(a7800session *s, int region);
int  a7800session_region(a7800session *s);
/* The console actually running: A7800_REGION_NTSC or A7800_REGION_PAL. */
int  a7800session_running_region(a7800session *s);

/* ---- the BIOS and the High Score Cart --------------------------------------
 * The console boots the FujiNet cartridge directly; no BIOS is needed or
 * shipped. Import BIOS copies the user's own image into the ROM directory,
 * identified by size and CRC-32 against MAME's table, and Preferences may
 * then pick it per console -- it shows the Atari logo and starts the games
 * it accepts itself. The High Score Cart's ROM is imported the same way. */
typedef struct {
    const char *name;         /* MAME's BIOS name: "a7800" */
    const char *desc;         /* "Atari 7800 (NTSC)" */
    int region;               /* A7800_REGION_NTSC / _PAL */
    const char *file;         /* MAME's file name: "7800.u7" */
    uint32_t size, crc;
} a7800_bios_info;
int  a7800session_bios_count(void);
const a7800_bios_info *a7800session_bios_info(int i);
/* 1 if the image is in the ROM directory. */
int  a7800session_bios_available(a7800session *s, int i);
/* The BIOS chosen for a console: an index, or -1 for none. Takes effect at
 * once (a new console). Persisted as bios_ntsc / bios_pal. */
int  a7800session_bios(a7800session *s, int region);
void a7800session_set_bios(a7800session *s, int region, int index);
/* Import a BIOS or HSC ROM (a raw image, or a .zip holding one). 0 and what
 * it was ("Atari 7800 (NTSC) BIOS") or -1 + error. */
int  a7800session_import_rom(a7800session *s, const char *path,
                             char *what, int whatsz);
/* 1 if `path` is a BIOS or HSC image, by size and CRC. */
int  a7800session_media_is_rom(const char *path);
int  a7800session_hsc_available(a7800session *s);
/* Games get the HSC (with its ROM imported). Takes effect at the next power
 * cycle. Persisted as hsc. */
void a7800session_set_hsc(a7800session *s, int on);
int  a7800session_hsc(a7800session *s);

/* ---- the on-screen controllers ---------------------------------------------
 * One layout for every toolkit: a view per port that follows what is plugged
 * in (a ProLine joystick, a 2600 CX40, the XG-1 light gun or nothing), over
 * the console strip. In the layout units of DESIGN-7800.md (a port's view is
 * 16 x 8 u, the console strip 33 x 5.5 u) from the top-left, so a frontend
 * draws each device's art in its own local coordinates plus the part's
 * offset. Every type has the same extent, so the window never changes size when
 * a port's type or the running game changes it. `target` is the binding target
 * a part drives (A7800_TARGET_*), or -1 for the parts that are dragged (the
 * stick) and for bodies and captions that are only drawn. The returned table
 * is static and valid for the process.
 *
 * How to drive and light each kind:
 *  - STICK: press and drag; a7800session_stick_from_vector(dx, dy, w / 2) from
 *    the part's centre gives the direction mask for a7800session_pointer_set(port,
 *    mask) (OR in the button bits the pointer also holds; there are none at the
 *    same time in one gesture). 0 on release. Light the direction sectors
 *    from a7800session_buttons_held(port) and shift the cap toward them.
 *  - ROUND_BUTTON / BUTTON: press and release with a7800session_press(target,
 *    down); lit while a7800session_target_held(target).
 *  - SWITCH (a difficulty slide): a7800session_press(target, 1) flips it on
 *    the click (the release does nothing); its position is
 *    a7800session_switch_get(sw): 1 = A (pro, slider at the right), 0 = B
 *    (novice, slider at the left). A position, not a held light.
 *  - BODY entries name the art by `device` (a7800_ctrl_type, never AUTO; -1 for
 *    the console's) and `label`; a LIGHTGUN or NONE port has no pressable part. */
typedef enum {
    A7800_CTL_CAPTION = 0, /* text: "Player 1", "Aim on the picture", "No controller" */
    A7800_CTL_BODY,        /* drawn only: a controller, the console's bar, its band, ... (see label) */
    A7800_CTL_STICK,       /* the joystick's dish / ring: drag to tilt it; round */
    A7800_CTL_ROUND_BUTTON,/* a red fire button (ProLine Button 1 / 2, CX40 fire); hit as a circle */
    A7800_CTL_BUTTON,      /* a flat console key: SELECT, RESET, PAUSE; hit as a rectangle */
    A7800_CTL_SWITCH       /* a latching difficulty slide with its A and B letters */
} a7800_control_kind;

typedef struct {
    int kind;            /* a7800_control_kind */
    const char *label;   /* "FIRE", "1", "2", "SELECT", "Console", "Band", "7800 PROSYSTEM", ... */
    const char *legend;  /* a hint ("left difficulty"), or NULL */
    int port;            /* 0 or 1, -1 for the console */
    int target;          /* binding target, or -1 */
    int device;          /* a7800_ctrl_type of the port's view, -1 for the console */
    float x, y, w, h;
} a7800_control;

/* The layout for the two ports' effective types (a7800_ctrl_type; AUTO and
 * anything unknown is drawn as a ProLine, so pass a7800session_view_type); *n
 * is its entry count and width / height (layout units) its extent. Entries are
 * in paint order. */
const a7800_control *a7800session_controls_layout(int left_type, int right_type, int *n,
                                                  float *width, float *height);
/* The topmost pressable part under a point in layout units, as an index into
 * the layout, or -1 (bodies and captions are never hit). Round parts are
 * ellipses. */
int a7800session_controls_hit(int left_type, int right_type, float x, float y);
/* The joystick drag: the direction bits (1 << A7800_ACT_UP etc., one or two)
 * for a pointer at (dx, dy) from the stick's centre (+x right, +y down, any
 * unit) with `radius` the stick's reach: nothing inside a quarter of it, else
 * one of eight 45-degree sectors. Feed it to a7800session_pointer_set. */
unsigned a7800session_stick_from_vector(float dx, float dy, float radius);

/* ---- gamepads (SDL, hotplugged; started by a7800session_start) -------------
 * Pads are assigned to ports in connection order unless assigned
 * explicitly. An explicit assignment is remembered in the "pad_assign" setting
 * (SDL GUID + name -> port): the pad gets it back when it reconnects and on the
 * next run; assigning -1 (Automatic) forgets it. Only ports 0 and 1 exist. */
int  a7800session_gamepad_count(a7800session *s);
int  a7800session_gamepad_name(a7800session *s, int idx, char *dst, int dstsz);
void a7800session_gamepad_assign(a7800session *s, int idx, int port); /* -1 = auto */
int  a7800session_gamepad_assignment(a7800session *s, int idx);
int  a7800session_gamepad_effective_port(a7800session *s, int idx);
/* Bumped on every add/remove so a frontend can refresh its lists cheaply. */
unsigned a7800session_gamepad_generation(a7800session *s);
/* The last hot-plug event as text ("Connected: 8BitDo SN30 Pro (player 1)",
 * "Disconnected: ..."), for a toast; returns length, 0 if none yet. */
int  a7800session_gamepad_last_event(a7800session *s, char *dst, int dstsz);

/* ---- cross-thread system actions ------------------------------------------
 * The gamepad thread cannot call into a UI toolkit, so a system action it
 * resolves is posted here and a frontend's timer takes it. */
void a7800session_sysaction_post(a7800session *s, int sysact);
int  a7800session_sysaction_take(a7800session *s, int *out);

/* ---- FujiNet -------------------------------------------------------------*/
/* Parses the "Use external FujiNet" address field: "host:port" (host a name
 * or an IPv4 literal) or a bare "port". Empty text means the default,
 * 127.0.0.1:11510. Returns 0 and fills host/port, or -1 when the text is not
 * an address:port (nothing is written then). */
int         a7800session_parse_hostport(const char *text, char *host,
                                        int hostsz, int *port);
int         a7800session_fujinet_running(const a7800session *s);
/* 1 when the session was started to dial another FujiNet instead of running
 * its own. */
int         a7800session_fujinet_external(const a7800session *s);
/* "host:port" the cartridge dials (the running session's, or the default
 * 127.0.0.1:11510 before the first start). */
const char *a7800session_boip_endpoint(const a7800session *s);
const char *a7800session_fujinet_webui_url(const a7800session *s);
int         a7800session_fujinet_copy_log(a7800session *s, char *dst, int max);
/* The cartridge's link to FujiNet: 1 up, 0 down, -1 not running. */
int         a7800session_cart_link_up(a7800session *s);
/* Status text from the cartridge ("connected", "link down: ...", "loading
 * 42%", "game running; mailbox closed", ...). Returns length. */
int         a7800session_cart_status(a7800session *s, char *dst, int dstsz);
/* 1 once anything other than CONFIG is in the cartridge (a network boot or
 * an opened cartridge file). */
int         a7800session_cart_booted_game(a7800session *s);

/* ---- media ---------------------------------------------------------------
 * Import a cartridge into FujiNet's SD folder so CONFIG can boot it through
 * the cartridge (an archive is unpacked to the image inside). Returns 0 and
 * the destination path, or -1 with the error. */
int  a7800session_import_cart_to_sd(a7800session *s, const char *src_path,
                                    char *dest_out, int dest_sz);
/* Routes a dropped file: cartridge images (.a78, .bin, archives holding one)
 * to the cartridge directory (the returned path is usable with load_cart),
 * BIOS and HSC images to the ROM directory, anything else to the FujiNet SD
 * folder. */
int  a7800session_import_media(a7800session *s, const char *src_path,
                               char *dest_out, int dest_sz);
int  a7800session_media_is_cartridge(const char *path);

const char *a7800session_config_path(const a7800session *s);
const char *a7800session_data_path(const a7800session *s);
const char *a7800session_carts_path(const a7800session *s);
const char *a7800session_roms_path(const a7800session *s);
const char *a7800session_sd_path(const a7800session *s);

/* ---- debugger ------------------------------------------------------------*/
a7800debug *a7800session_debugger(a7800session *s);

#ifdef __cplusplus
}
#endif

#endif /* A7800SESSION_H */
