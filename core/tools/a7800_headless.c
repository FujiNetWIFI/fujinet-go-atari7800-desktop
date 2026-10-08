/*
 * a7800_headless -- libmame_fngo with no window: boot the console, run it
 * flat out for a number of frames, and say what happened. The ctest boot
 * checks and the ROM sweep (tools/romsweep/sweep.py) both run it.
 *
 *   a7800_headless [options]
 *     --system a7800|a7800p    the console (NTSC / PAL)          [a7800]
 *     --bios NAME              a7800, a7800pr, a7800p or none    [none]
 *     --rompath DIR            where the BIOS images are         [<data>/roms]
 *     --data DIR               MAME's own files                  [a7800_headless.data]
 *     --image PATH|test        a cartridge (.a78/.bin, or a .zip holding
 *                              one); "test" is the tests' own cartridge
 *     --mode none|staged|direct  how the cartridge boots it      [staged]
 *     --frames N               frames to run                     [600]
 *     --port N                 FujiNet's BoIP port (nothing listening: the
 *                              cartridge runs link-down)         [11510]
 *     --hsc ROM                give games the High Score Cart
 *     --controllers A,B        MAME's CONTROLLERS per port: 0 ProLine,
 *                              1 2600 joystick, 2 light gun, 3 none
 *     --gun X,Y                aim the light guns and pull the trigger
 *                              every second
 *     --input F:TAG:MASK:VAL   set an input field from frame F on (repeat)
 *     --debug F:COMMAND        run a MAME debugger command at frame F
 *                              (repeat); the debugger's console goes to
 *                              stderr as it fills
 *     --shot PATH              the frame at --shot-at (or the last) as PPM
 *     --shot-at N
 *     --hashes PATH            every frame's hash, one a line (to line a
 *                              run up against another)
 *     --expect-colours N       exit 1 unless the shot has >= N colours
 *     --expect-animated        exit 1 unless the picture changed in the
 *                              last 120 frames
 *     --expect-game            exit 1 unless the cartridge reports a game
 *
 * Prints one JSON object on stdout.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fngo_mame.h"
#include "test_files.h"

#define MAX_W 320
#define MAX_H 320
#define MAX_INPUTS 32
#define MAX_HASHES 2048

static fngo_mame *g;

static struct {
    const char *system, *bios, *rompath, *data, *image, *shot, *hsc, *hashes;
    int mode, frames, port, shot_at;
    int controllers[2];
    int gun, gun_x, gun_y;
    int expect_colours, expect_animated, expect_game;
} opt = { "a7800", "none", NULL, "a7800_headless.data", NULL, NULL, NULL, NULL,
          FNGO_BOOT_STAGED, 600, 11510, -1, { -1, -1 }, 0, 160, 112, 0, 0, 0 };

static struct { int frame; char tag[32]; uint32_t mask; int32_t value; } inputs[MAX_INPUTS];
static int ninputs;
static struct { int frame; const char *command; } debugs[MAX_INPUTS];
static int ndebugs;

static uint32_t shot[MAX_W * MAX_H];
static int shot_w, shot_h;
static uint64_t frames_seen, last_change;
static uint64_t prev_hash;
static uint64_t hashes[MAX_HASHES];
static int nhashes;
static long audio_frames;
static int audio_loud;
static uint16_t pcs[4096];
static int npcs;
static int pc_samples;
static fngo_mame_cart_status last_st;
static int last_present;
static uint16_t last_pc;
static FILE *hash_file;
static uint64_t game_frame;          /* when the cartridge first reported a game */
static int nonblank_after_game;      /* frames since then with more than one colour */
static uint64_t last_nonblank;

static uint64_t fnv(const uint32_t *px, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= px[i];
        h *= 1099511628211ull;
    }
    return h;
}

static void on_frame(void *u, const uint32_t *xrgb, int w, int h, uint64_t n, int paced)
{
    uint64_t hash;
    int i;
    (void)u; (void)paced;
    frames_seen = n;
    if (w > MAX_W || h > MAX_H) return;
    hash = fnv(xrgb, (size_t)w * (size_t)h);
    if (hash_file) fprintf(hash_file, "%016llx\n", (unsigned long long)hash);
    if (game_frame) {
        size_t k, total = (size_t)w * (size_t)h;
        for (k = 1; k < total && xrgb[k] == xrgb[0]; k++) ;
        if (k < total) { nonblank_after_game++; last_nonblank = n; }
    }
    if (n > 1 && hash != prev_hash) last_change = n;
    prev_hash = hash;
    for (i = 0; i < nhashes && hashes[i] != hash; i++) ;
    if (i == nhashes && nhashes < MAX_HASHES) hashes[nhashes++] = hash;
    if (opt.shot_at < 0 || (int)n == opt.shot_at) {
        memcpy(shot, xrgb, (size_t)w * (size_t)h * 4);
        shot_w = w; shot_h = h;
    }
    if ((int)n >= opt.frames) fngo_mame_stop(g);
}

static void on_audio(void *u, const int16_t *s, int n)
{
    int i;
    (void)u;
    audio_frames += n;
    for (i = 0; i < 2 * n; i++)
        if (s[i] > 256 || s[i] < -256) { audio_loud = 1; break; }
}

static void on_service(void *u)
{
    static uint64_t last = (uint64_t)-1;
    const uint64_t n = fngo_mame_frame_number(g);
    fngo_mame_cpu c;
    int i;
    (void)u;
    if (n == last) return;
    last = n;
    for (i = 0; i < 2; i++)
        if (opt.controllers[i] >= 0)
            fngo_mame_ioport_setting(g, ":CONTROLLERS", i ? 0x0c : 0x03,
                                     (uint32_t)(i ? opt.controllers[i] << 2 : opt.controllers[i]));
    if (opt.gun) {
        fngo_mame_ioport_set(g, ":LIGHTGUN1_X", 0x1ff, opt.gun_x);
        fngo_mame_ioport_set(g, ":LIGHTGUN1_Y", 0x1ff, opt.gun_y);
        fngo_mame_ioport_set(g, ":LIGHTGUN2_X", 0x1ff, opt.gun_x);
        fngo_mame_ioport_set(g, ":LIGHTGUN2_Y", 0x1ff, opt.gun_y);
        /* one field at a time: a mask selects a single field */
        fngo_mame_ioport_set(g, ":BUTTONS", 0x08, (n % 60) < 6 ? 1 : 0);
        fngo_mame_ioport_set(g, ":BUTTONS", 0x04, (n % 60) < 6 ? 1 : 0);
    }
    for (i = 0; i < ninputs; i++)
        if ((uint64_t)inputs[i].frame == n)
            fngo_mame_ioport_set(g, inputs[i].tag, inputs[i].mask, inputs[i].value);
    for (i = 0; i < ndebugs; i++)
        if ((uint64_t)debugs[i].frame == n && fngo_mame_debug_command(g, debugs[i].command) != 0)
            fprintf(stderr, "debugger command refused: %s\n", debugs[i].command);
    if (ndebugs) {
        static char console[1 << 16];
        static uint32_t seq;
        if (fngo_mame_console_text(g, &seq, console, sizeof console) > 0)
            fputs(console, stderr);
    }
    last_present = fngo_mame_fujinet_status(&last_st);
    if (!game_frame && last_present && last_st.booted_image && last_st.mode >= 2)
        game_frame = n;
    if (n > 60 && fngo_mame_cpu_get(g, &c) == 0) {
        int k;
        pc_samples++;
        last_pc = (uint16_t)c.pc;
        for (k = 0; k < npcs && pcs[k] != (uint16_t)c.pc; k++) ;
        if (k == npcs && npcs < 4096) pcs[npcs++] = (uint16_t)c.pc;
    }
}

static void on_log(void *u, int ch, const char *text)
{
    (void)u;
    if (ch <= FNGO_LOG_WARNING || getenv("A7800_HEADLESS_VERBOSE")) fprintf(stderr, "%s", text);
}

static int arg_pair(const char *s, int *a, int *b)
{
    return sscanf(s, "%d,%d", a, b) == 2;
}

static void json_str(const char *k, const char *v, int comma)
{
    const char *p;
    printf("\"%s\":\"", k);
    for (p = v; *p; p++) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if ((unsigned char)*p < 0x20) printf("\\u%04x", *p);
        else putchar(*p);
    }
    printf("\"%s", comma ? "," : "");
}

int main(int argc, char **argv)
{
    fngo_mame_callbacks cb;
    char rompath[1024], name[256] = "", why[256] = "";
    uint8_t *image = NULL;
    uint32_t size = 0;
    static uint8_t test_image[TEST_A78_SIZE];
    fngo_mame_plan_t plan;
    fngo_mame_cart_status st;
    int i, err, colours = 0, present, fail = 0;
    uint32_t seen[256];

    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--expect-animated")) { opt.expect_animated = 1; continue; }
        if (!strcmp(a, "--expect-game")) { opt.expect_game = 1; continue; }
        if (!v) { fprintf(stderr, "%s needs a value\n", a); return 2; }
        i++;
        if (!strcmp(a, "--system")) opt.system = v;
        else if (!strcmp(a, "--bios")) opt.bios = v;
        else if (!strcmp(a, "--rompath")) opt.rompath = v;
        else if (!strcmp(a, "--data")) opt.data = v;
        else if (!strcmp(a, "--image")) opt.image = v;
        else if (!strcmp(a, "--mode"))
            opt.mode = !strcmp(v, "direct") ? FNGO_BOOT_DIRECT : !strcmp(v, "none") ? FNGO_BOOT_NONE : FNGO_BOOT_STAGED;
        else if (!strcmp(a, "--frames")) opt.frames = atoi(v);
        else if (!strcmp(a, "--port")) opt.port = atoi(v);
        else if (!strcmp(a, "--hsc")) opt.hsc = v;
        else if (!strcmp(a, "--controllers")) {
            if (!arg_pair(v, &opt.controllers[0], &opt.controllers[1])) { fprintf(stderr, "--controllers A,B\n"); return 2; }
        } else if (!strcmp(a, "--gun")) {
            if (!arg_pair(v, &opt.gun_x, &opt.gun_y)) { fprintf(stderr, "--gun X,Y\n"); return 2; }
            opt.gun = 1;
        } else if (!strcmp(a, "--input")) {
            unsigned mask = 0;
            int frame = 0, value = 0;
            char tag[32];
            if (ninputs == MAX_INPUTS || sscanf(v, "%d:%31[^:]:%i:%i", &frame, tag, (int *)&mask, &value) != 4) {
                fprintf(stderr, "--input FRAME:TAG:MASK:VALUE\n");
                return 2;
            }
            inputs[ninputs].frame = frame;
            snprintf(inputs[ninputs].tag, sizeof inputs[ninputs].tag, "%s", tag);
            inputs[ninputs].mask = mask;
            inputs[ninputs].value = value;
            ninputs++;
        }
        else if (!strcmp(a, "--debug")) {
            const char *colon = strchr(v, ':');
            if (ndebugs == MAX_INPUTS || !colon) {
                fprintf(stderr, "--debug FRAME:COMMAND\n");
                return 2;
            }
            debugs[ndebugs].frame = atoi(v);
            debugs[ndebugs].command = colon + 1;
            ndebugs++;
        }
        else if (!strcmp(a, "--shot")) opt.shot = v;
        else if (!strcmp(a, "--shot-at")) opt.shot_at = atoi(v);
        else if (!strcmp(a, "--hashes")) opt.hashes = v;
        else if (!strcmp(a, "--expect-colours")) opt.expect_colours = atoi(v);
        else { fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    if (opt.frames < 1) opt.frames = 1;
    snprintf(rompath, sizeof rompath, "%s", opt.rompath ? opt.rompath : "");
    if (!opt.rompath) snprintf(rompath, sizeof rompath, "%s/roms", opt.data);

    memset(&cb, 0, sizeof cb);
    cb.frame = on_frame;
    cb.audio = on_audio;
    cb.service = on_service;
    cb.log = on_log;
    g = fngo_mame_create(&cb, opt.data);
    if (!g) { fprintf(stderr, "fngo_mame_create failed\n"); return 2; }
    fngo_mame_configure(g, opt.system, opt.bios, rompath);
    fngo_mame_fujinet_link(g, "127.0.0.1", opt.port, getenv("A7800_FUJINET_DEBUG") != NULL);

    memset(&plan, 0, sizeof plan);
    if (opt.image && !strcmp(opt.image, "test")) {
        test_a78_image(test_image, sizeof test_image, "HEADLESS TEST");
        snprintf(name, sizeof name, "test.a78");
        image = test_image;
        size = sizeof test_image;
    } else if (opt.image) {
        if (fngo_mame_archive_read(opt.image, ".a78;.bin;.rom", &image, &size, name, sizeof name) != 0) {
            fprintf(stderr, "%s: cannot read\n", opt.image);
            return 2;
        }
    }
    if (image) {
        if (fngo_mame_plan(image, size, NULL, &plan, why, sizeof why) != 0) {
            printf("{");
            json_str("image", name, 1);
            json_str("refused", why, 0);
            printf("}\n");
            return 3;
        }
        fngo_mame_fujinet_boot(g, opt.mode, image, size, NULL);
        if (image != test_image) fngo_mame_free(image);
    }
    if (opt.hsc) {
        uint8_t *rom = NULL;
        uint32_t n = 0;
        char hn[64];
        if (fngo_mame_archive_read(opt.hsc, ".bin;.rom", &rom, &n, hn, sizeof hn) == 0) {
            fngo_mame_fujinet_hsc(g, rom, n, 1);
            fngo_mame_free(rom);
        }
    }

    if (opt.hashes && !(hash_file = fopen(opt.hashes, "w"))) {
        fprintf(stderr, "%s: cannot write\n", opt.hashes);
        return 2;
    }
    err = fngo_mame_run(g);
    if (hash_file) fclose(hash_file);

    /* as the last frame left it (the machine is gone now) */
    present = last_present;
    st = last_st;
    if (!present) memset(&st, 0, sizeof st);

    for (i = 0; i < shot_w * shot_h; i++) {
        int k;
        for (k = 0; k < colours && seen[k] != shot[i]; k++) ;
        if (k == colours && colours < 256) seen[colours++] = shot[i];
    }
    if (opt.shot && shot_w) {
        FILE *f = fopen(opt.shot, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", shot_w, shot_h);
            for (i = 0; i < shot_w * shot_h; i++) {
                fputc((int)((shot[i] >> 16) & 255), f);
                fputc((int)((shot[i] >> 8) & 255), f);
                fputc((int)(shot[i] & 255), f);
            }
            fclose(f);
        }
    }

    printf("{\"run\":%d,\"frames\":%llu,\"width\":%d,\"height\":%d,\"colours\":%d,"
           "\"distinct_frames\":%d,\"last_change\":%llu,\"hash\":\"%016llx\","
           "\"pcs\":%d,\"pc\":\"%04X\",\"pc_samples\":%d,\"audio_frames\":%ld,\"audio\":%d,",
           err, (unsigned long long)frames_seen, shot_w, shot_h, colours,
           nhashes, (unsigned long long)last_change,
           (unsigned long long)fnv(shot, (size_t)shot_w * (size_t)shot_h),
           npcs, last_pc, pc_samples, audio_frames, audio_loud);
    json_str("image", name, 1);
    json_str("plan_kind", plan.kind, 1);
    printf("\"plan_crc\":\"%08X\",\"plan_size\":%u,\"biosok\":%d,\"in_db\":%d,\"pokey\":%d,",
           plan.crc, plan.size, plan.biosok, plan.in_db, plan.pokey);
    printf("\"present\":%d,\"link_up\":%d,\"mode\":%d,\"handover\":%d,\"booted\":%d,"
           "\"live_crc\":\"%08X\",\"boot_state\":%d,\"boot_err\":%d,\"load_pct\":%d,"
           "\"inptctrl\":%d,\"inpt_locked\":%d,\"tv\":%d,"
           "\"game_frame\":%llu,\"nonblank_after_game\":%d,\"last_nonblank\":%llu,",
           present, st.link_up, st.mode, st.handover, st.booted_image,
           st.live_crc, st.boot_state, st.boot_err, st.load_pct,
           st.inptctrl, st.inpt_locked, st.tv,
           (unsigned long long)game_frame, nonblank_after_game, (unsigned long long)last_nonblank);
    json_str("live_kind", st.live_kind, 1);
    json_str("system", opt.system, 1);
    json_str("bios", opt.bios, 0);
    printf("}\n");
    fflush(stdout);
    fngo_mame_destroy(g);

    if (err != 0) fail = 1;
    if (opt.expect_colours && colours < opt.expect_colours) {
        fprintf(stderr, "expected >= %d colours, got %d\n", opt.expect_colours, colours);
        fail = 1;
    }
    if (opt.expect_animated && (frames_seen < 120 || last_change + 120 < frames_seen)) {
        fprintf(stderr, "the picture stopped changing at frame %llu\n", (unsigned long long)last_change);
        fail = 1;
    }
    if (opt.expect_game && !(present && st.booted_image && st.mode == 2)) {
        fprintf(stderr, "no game running (mode %d, booted %d)\n", st.mode, st.booted_image);
        fail = 1;
    }
    return fail;
}
