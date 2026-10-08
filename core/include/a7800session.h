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
    int enable_audio;         /* open the SDL audio device */
    int enable_gamepad;       /* start the SDL gamepad thread */
} a7800session_start_opts;

/* Fills opts from the settings store (keys: cart region port0_type
 * port1_type analog_joystick hsc enable_fujinet enable_audio
 * enable_gamepad). */
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
void a7800session_sysaction(a7800session *s, int sysact);
/* Momentary console switches as a whole: press, hold a few frames, release
 * (for menu items and on-screen buttons). */
void a7800session_switch_pulse(a7800session *s, int sw);
/* The difficulty switches' positions: 1 = A (pro), 0 = B (novice). Others
 * report whether they are held. Persisted as left_diff / right_diff. */
int  a7800session_switch_get(a7800session *s, int sw);
void a7800session_switch_set(a7800session *s, int sw, int on);
/* Release everything the keyboard holds (focus loss). */
void a7800session_release_all(a7800session *s);
/* What the console sees held on a port right now (keyboard, gamepads and
 * the on-screen controller together), one bit per a7800_action. For a
 * controller window's highlights. */
unsigned a7800session_buttons_held(a7800session *s, int port);

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
void a7800session_bindings_reset(a7800session *s);
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

/* ---- gamepads (SDL, hotplugged; started by a7800session_start) -------------
 * Pads are assigned to ports in connection order unless assigned
 * explicitly. A pad that disconnects and reconnects gets its port back. */
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
int         a7800session_fujinet_running(const a7800session *s);
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
