/*
 * a7800session's private state. Not installed; only the core/src sources
 * include it. Plain C so the C modules (settings, paths, media, roms,
 * audio, gamepads, bindings) and the C++ session (session.cpp, which owns
 * the MameHost) share one struct.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef A7800_SESSION_INTERNAL_H
#define A7800_SESSION_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "a7800session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define A7800_PATH_MAX 1024

typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

struct a7800session {
    char config_dir[A7800_PATH_MAX];
    char data_dir[A7800_PATH_MAX];
    char carts_dir[A7800_PATH_MAX];
    char roms_dir[A7800_PATH_MAX];     /* <data>/roms: imported BIOS and HSC images */
    char mame_dir[A7800_PATH_MAX];     /* <data>/mame: MAME's own files */
    char settings_file[A7800_PATH_MAX];

    setting_kv *settings;
    pthread_mutex_t settings_mtx;
    int settings_dirty;

    char last_error[256];

    /* ---- FujiNet runtime (fujinet_runtime.c) ---- */
    char fujinet_root[A7800_PATH_MAX];    /* <data>/fujinet */
    char fujinet_config[A7800_PATH_MAX];  /* .../fnconfig.ini */
    char fujinet_sd[A7800_PATH_MAX];      /* .../SD */
    char fujinet_data[A7800_PATH_MAX];    /* .../data */
    char fujinet_lib[A7800_PATH_MAX];     /* resolved libfujinet path, "" until then */
    char fujinet_runtime_src[A7800_PATH_MAX]; /* caller-given pristine tree, or "" */
    char webui_url[64];                   /* http://127.0.0.1:11511/ */
    int  fujinet_running;

    /* cross-thread system-action latch (see a7800session_sysaction_post) */
    pthread_mutex_t sysact_mtx;
    unsigned sysact_pending;

    /* the running configuration */
    a7800session_start_opts opts;
    char cart_path[A7800_PATH_MAX];
    int running_region;                   /* A7800_REGION_NTSC / _PAL */
    int detected[2];                      /* what AUTO resolved to, per port */
    uint32_t live_crc;                    /* the image running in the cartridge */

    /* keys the keyboard currently holds, by target, so a release clears
     * exactly what its press asserted even if the binding changed meanwhile */
    uint32_t held_keysym[A7800_TARGET_COUNT];

    /* the difficulty switches (1 = A) */
    int difficulty[2];

    void *host;               /* MameHost*, session.cpp only */
    void *audio;              /* audio_sdl.c state, NULL until started */
    void *gamepad;            /* gamepad_sdl.c state, NULL until started */
    void *debugger;           /* a7800debug, lazily created */
    int running;

    /* the last gamepad hot-plug event, for a frontend toast */
    pthread_mutex_t pad_event_mtx;
    char pad_event[128];
};

void settings_init(struct a7800session *s);
void settings_free_all(struct a7800session *s);

int paths_init(struct a7800session *s, const char *config_dir,
               const char *data_dir);
/* Locate libfujinet and provision the runtime tree (fnconfig.ini + data/ +
 * SD/) into <data>/fujinet on first run. Returns 0, or -1 if no runtime is
 * available (not fatal to the session -- see fujinet_start). */
int paths_provision_fujinet(struct a7800session *s);
/* mkdir -p; 0 when the directory exists afterwards. */
int paths_mkdir_p(const char *path);

void session_set_error(struct a7800session *s, const char *fmt, ...);

/* fujinet_runtime.c */
int  fujinet_start(struct a7800session *s);
void fujinet_stop(struct a7800session *s);
/* Block (up to timeout_ms) until the BoIP port accepts, so the emulator's
 * first dial-out finds the listener. Returns 0 once up, -1 on timeout. */
int  fujinet_wait_for_boip(struct a7800session *s, int timeout_ms);

/* bindings.c */
void bindings_init(struct a7800session *s);

/* audio_sdl.c */
int  audio_start(struct a7800session *s);
void audio_stop(struct a7800session *s);

/* gamepad_sdl.c */
int  gamepad_start(struct a7800session *s);
void gamepad_stop(struct a7800session *s);
/* Called by the gamepad thread with the button/axis state it resolved. */
void session_gamepad_apply(struct a7800session *s, int port, int act, int down);
/* Called by the gamepad thread on a connect/disconnect. */
void session_gamepad_event(struct a7800session *s, const char *text);

/* media.c: read a cartridge image (a raw .a78/.bin, or the first such
 * member of a .zip/.7z) into a malloc'd buffer. 0, or -1 with why. */
int  media_read_image(const char *path, uint8_t **data, uint32_t *size,
                      char *why, int whysz);

/* roms.c: the BIOS images MAME's a7800 knows, identified by size and CRC. */
int  roms_identify(const uint8_t *data, uint32_t size);   /* bios index, -1, or ROMS_HSC */
#define ROMS_HSC 1000
int  roms_hsc_load(struct a7800session *s, uint8_t *rom4k);  /* 0 if imported */
/* MAME's BIOS name for a console: the chosen one if its files are there,
 * else "none". */
const char *roms_bios_name(struct a7800session *s, int region);
/* A developer build's embedded images, into the ROM directory, once. */
void roms_provision_embedded(struct a7800session *s);

/* session.cpp: the BIOS choice changed (a new console if running); the
 * HSC choice changed (applies at the next power cycle). */
void session_machine_changed(struct a7800session *s);
void session_hsc_changed(struct a7800session *s);

#ifdef __cplusplus
}
#endif

#endif /* A7800_SESSION_INTERNAL_H */
