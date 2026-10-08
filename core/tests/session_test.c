/*
 * session_test -- the frontend contract end to end, through the public API
 * only: settings round-trip and persist across sessions, paths resolve
 * inside the given tree, CONFIG boots with no BIOS and paints, a cartridge
 * opens through the cartridge's own boot path and runs, a power cycle keeps
 * it, Reboot to CONFIG ejects it, a PAL cartridge brings up the PAL console,
 * the console switches and controller types, an image the cartridge cannot
 * map is refused with a reason, and stop/start survive.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "a7800session.h"
#include "test_files.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint32_t px[A7800SESSION_FB_WIDTH * A7800SESSION_FB_MAX_HEIGHT];

static int wait_frames(a7800session *s, uint64_t *serial, int count, int timeout_ms)
{
    int got = 0, waited = 0, h;
    while (waited < timeout_ms) {
        if (a7800session_copy_frame(s, px, &h, serial)) {
            if (++got >= count) return 1;
        }
        sleep_ms(2); waited += 2;
    }
    return 0;
}

/* Poll the cartridge status until it contains `want`, or time out. */
static int wait_status(a7800session *s, const char *want, int timeout_ms)
{
    char st[160];
    int waited = 0;
    while (waited < timeout_ms) {
        a7800session_cart_status(s, st, sizeof st);
        if (strstr(st, want)) return 1;
        sleep_ms(20); waited += 20;
    }
    printf("  (status: %s)\n", st);
    return 0;
}

/* The test cartridge turns DMA off and steps the background colour once a
 * frame, so every pixel of a frame is one colour and the colour moves. */
static int background_steps(a7800session *s, int timeout_ms)
{
    uint64_t serial = 0;
    uint32_t first = 0;
    int h = 0, waited = 0, have = 0;
    while (waited < timeout_ms) {
        if (a7800session_copy_frame(s, px, &h, &serial)) {
            int i, flat = 1;
            for (i = 1; i < A7800SESSION_FB_WIDTH * h; i++)
                if (px[i] != px[0]) { flat = 0; break; }
            if (flat) {
                if (!have) { first = px[0]; have = 1; }
                else if (px[0] != first) return 1;
            }
        }
        sleep_ms(5); waited += 5;
    }
    return 0;
}

int main(void)
{
    char cfg[512], data[512], rom[700], pal[700], big[700], why[256];
    static uint8_t image[TEST_A78_SIZE];
    a7800session_paths p;
    a7800session *s;
    a7800session_start_opts o;
    uint64_t serial = 0;
    int h;

    test_tmpdir(cfg, sizeof cfg, "cfg");
    test_tmpdir(data, sizeof data, "data");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data;
    p.fujinet_lib = "";     /* no runtime: the cart runs link-down */

    test_a78_image(image, sizeof image, "SESSION TEST");
    snprintf(rom, sizeof rom, "%s/steps.a78", cfg);
    check(test_write_file(rom, image, sizeof image) == 0, "wrote an NTSC test cartridge");
    image[57] = 1;
    snprintf(pal, sizeof pal, "%s/steps-pal.a78", cfg);
    check(test_write_file(pal, image, sizeof image) == 0, "and a PAL one");
    {
        /* 512K: more than the cartridge's 448K of SRAM for images */
        const size_t n = 128 + 0x80000;
        uint8_t *huge = (uint8_t *)calloc(1, n);
        memcpy(huge, image, 128);
        huge[49] = 0x00; huge[50] = 0x08; huge[51] = 0x00; huge[52] = 0x00;
        snprintf(big, sizeof big, "%s/huge.a78", cfg);
        check(huge && test_write_file(big, huge, n) == 0, "and one too big to map");
        free(huge);
    }

    s = a7800session_new(&p);
    check(s != NULL, "session created");
    if (!s) return 1;

    check(strcmp(a7800session_config_path(s), cfg) == 0, "config path is the given tree");
    check(strncmp(a7800session_carts_path(s), data, strlen(data)) == 0, "carts dir is under the data tree");
    check(strncmp(a7800session_roms_path(s), data, strlen(data)) == 0, "ROM dir is under the data tree");
    check(strncmp(a7800session_sd_path(s), data, strlen(data)) == 0, "SD path is under the data tree");

    a7800session_set_int(s, "answer", 42);
    a7800session_set_str(s, "greeting", "hello");
    check(a7800session_get_int(s, "answer", 0) == 42, "int setting round-trips");
    check(strcmp(a7800session_get_str(s, "greeting", ""), "hello") == 0, "string setting round-trips");
    check(a7800session_get_int(s, "nope", 7) == 7, "missing setting yields its default");

    check(strcmp(a7800_region_name(A7800_REGION_PAL), "PAL") == 0 && a7800_region_name(A7800_REGION_COUNT) == NULL,
          "region names, NULL past the end");
    check(a7800_ctrl_type_name(A7800_CTRL_LIGHTGUN) != NULL && a7800_ctrl_type_name(A7800_CTRL_COUNT) == NULL,
          "controller names, NULL past the end");

    a7800session_default_opts(s, &o);
    check(o.enable_fujinet == 1 && o.enable_audio == 1, "default opts enable FujiNet and audio");
    check(o.port_type[0] == A7800_CTRL_AUTO && o.region == A7800_REGION_AUTO,
          "default opts: controllers and region follow the game");
    check(o.cart_path == NULL, "no remembered cartridge: CONFIG");
    check(a7800session_bios(s, A7800_REGION_NTSC) == -1 && a7800session_bios(s, A7800_REGION_PAL) == -1,
          "no BIOS chosen: the console starts the cartridge directly");

    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    check(a7800session_start(s, &o) == 0, "session starts (CONFIG, no BIOS, no FujiNet)");
    if (!a7800session_is_running(s)) { printf("error: %s\n", a7800session_last_error(s)); return 1; }

    check(wait_frames(s, &serial, 30, 5000), "frames arrive");
    check(wait_status(s, "link down", 5000), "CONFIG runs, link down");
    check(!a7800session_cart_booted_game(s), "CONFIG is not a booted game");
    check(a7800session_running_region(s) == A7800_REGION_NTSC, "the NTSC console");
    {
        uint64_t z = 0; int distinct = 0, i, tries;
        check(a7800session_copy_frame(s, px, &h, &z) == 1, "a forced copy (serial 0) always copies");
        check(h == 224, "an NTSC frame is 224 lines");
        /* CONFIG draws text on a plain background: wait for more than one
         * colour (up to 15 s: a loaded CI runner can be slow to first paint) */
        for (tries = 0; tries < 150; tries++) {
            uint32_t first = px[0];
            distinct = 0;
            for (i = 0; i < A7800SESSION_FB_WIDTH * h; i++) if (px[i] != first) distinct++;
            if (distinct > 100) break;
            sleep_ms(100);
            z = 0;
            a7800session_copy_frame(s, px, &h, &z);
        }
        check(distinct > 100, "the CONFIG client painted something");
    }
    check(a7800session_refresh_rate(s) == 60, "NTSC: 60 Hz");

    /* keyboard: the default map */
    check(a7800session_key(s, 'z', 1) == 1, "Z is bound (player 1 Button 1)");
    check(a7800session_buttons_held(s, 0) & (1u << A7800_ACT_BUTTON1), "and the console sees it held");
    check(a7800session_key(s, 'z', 0) == 1, "and released");
    check(!(a7800session_buttons_held(s, 0) & (1u << A7800_ACT_BUTTON1)), "and let go");
    check(a7800session_key(s, 0xffc8 /* F11 */, 1) == 0, "an unbound key is ignored");
    check(a7800session_key_sysaction(s, A7800_KEYSYM_ESCAPE) == A7800_SYSACT_REBOOT_CONFIG,
          "Escape is the Reboot to CONFIG system action");
    check(a7800session_target_for_key(s, A7800_KEYSYM_F2) == A7800_TARGET_SWITCH(A7800_SW_RESET),
          "F2 is the console's RESET");
    check(wait_frames(s, &serial, 10, 3000), "the machine keeps running after input");

    /* the difficulty switches toggle on press and persist */
    {
        const int was = a7800session_switch_get(s, A7800_SW_LEFT_DIFF);
        check(was == 1, "left difficulty starts at A");
        a7800session_press(s, A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF), 1);
        a7800session_press(s, A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF), 0);
        check(a7800session_switch_get(s, A7800_SW_LEFT_DIFF) == 0, "a press toggles it to B");
        check(a7800session_get_int(s, "left_diff", -1) == 0, "and it is remembered");
        a7800session_switch_pulse(s, A7800_SW_LEFT_DIFF);
        check(a7800session_switch_get(s, A7800_SW_LEFT_DIFF) == 1, "a pulse toggles it back");
    }

    /* an image the cartridge cannot map */
    check(a7800session_check_cart(big, why, sizeof why) == 0, "a 512K image is refused");
    check(strstr(why, "448K") != NULL, "with the reason");
    check(a7800session_load_cart(s, big) == -1, "load_cart refuses it");
    check(!a7800session_cart_booted_game(s) && a7800session_cart_path(s)[0] == '\0',
          "and CONFIG keeps running");

    /* open a cartridge: staged, it boots through the boot block and loader */
    check(a7800session_check_cart(rom, NULL, 0) == 1, "the test cartridge is accepted");
    check(a7800session_load_cart(s, rom) == 0, "load_cart");
    check(strcmp(a7800session_cart_path(s), rom) == 0, "the cart path is remembered");
    check(strcmp(a7800session_get_str(s, "cart", ""), rom) == 0, "and persisted");
    check(wait_status(s, "game running", 5000), "the cartridge reports a game (mailbox closed)");
    check(a7800session_cart_booted_game(s), "a booted game");
    check(background_steps(s, 3000), "the image is running (its background steps)");
    check(a7800session_port_detected(s, 0) == A7800_CTRL_PROLINE, "its header asks for ProLine joysticks");

    /* the power switch: the same image boots again */
    check(a7800session_power_cycle(s) == 0, "power cycle");
    check(wait_status(s, "game running", 5000), "the game boots again");
    check(strcmp(a7800session_cart_path(s), rom) == 0, "and is still the open cartridge");

    /* controller types, live */
    a7800session_set_port_type(s, 0, A7800_CTRL_LIGHTGUN);
    check(a7800session_port_type(s, 0) == A7800_CTRL_LIGHTGUN, "player 1 has a light gun");
    check(a7800session_lightgun_active(s), "so the pointer aims it");
    a7800session_pointer(s, 160, 112, 1, 1);
    a7800session_pointer(s, 0, 0, 0, 0);
    a7800session_set_port_type(s, 0, A7800_CTRL_AUTO);
    check(!a7800session_lightgun_active(s), "back to AUTO: the game's joystick");
    a7800session_set_port_type(s, 1, A7800_CTRL_NONE);
    check(a7800session_port_type(s, 1) == A7800_CTRL_NONE, "player 2 unplugged");
    a7800session_set_port_type(s, 1, A7800_CTRL_AUTO);
    check(background_steps(s, 3000), "the game survives controller swaps");

    /* a PAL cartridge with the region on AUTO: the PAL console */
    check(a7800session_load_cart(s, pal) == 0, "open a PAL cartridge");
    check(a7800session_running_region(s) == A7800_REGION_PAL, "the PAL console is running");
    serial = 0;
    check(wait_frames(s, &serial, 10, 5000), "frames flow from the PAL console");
    {
        uint64_t z = 0;
        a7800session_copy_frame(s, px, &h, &z);
        check(h == 260, "a PAL frame is 260 lines");
    }
    check(wait_status(s, "game running", 5000), "the PAL game boots");
    {
        int tries;
        for (tries = 0; tries < 100 && a7800session_refresh_rate(s) != 50; tries++) sleep_ms(50);
        check(a7800session_refresh_rate(s) == 50, "PAL: 50 Hz");
    }

    /* Reboot to CONFIG: a power cycle that ejects it */
    check(a7800session_reboot_to_config(s) == 0, "reboot to CONFIG");
    check(a7800session_cart_path(s)[0] == '\0', "the cartridge is ejected");
    check(strcmp(a7800session_get_str(s, "cart", "x"), "") == 0, "and forgotten");
    serial = 0;
    check(wait_frames(s, &serial, 10, 5000), "frames flow from the new console");
    check(wait_status(s, "link down", 5000), "CONFIG is back");
    check(!a7800session_cart_booted_game(s), "no game in the cartridge");
    check(a7800session_cart_link_up(s) == 0, "with no runtime the cart reports link down");

    /* the region setting, explicit */
    a7800session_set_region(s, A7800_REGION_NTSC);
    check(a7800session_region(s) == A7800_REGION_NTSC && a7800session_running_region(s) == A7800_REGION_NTSC,
          "NTSC chosen: the NTSC console");
    check(wait_status(s, "link down", 5000), "running CONFIG");
    a7800session_set_region(s, A7800_REGION_AUTO);

    /* a remembered cartridge boots at the next start */
    check(a7800session_load_cart(s, rom) == 0, "open the cartridge again");
    a7800session_stop(s);
    check(!a7800session_is_running(s), "session stops");
    a7800session_free(s);

    s = a7800session_new(&p);
    check(s && a7800session_get_int(s, "answer", 0) == 42, "settings persisted across sessions");
    if (s) {
        check(a7800session_switch_get(s, A7800_SW_LEFT_DIFF) == 1, "and the difficulty switches");
        a7800session_default_opts(s, &o);
        check(o.cart_path && strcmp(o.cart_path, rom) == 0, "the cartridge is remembered");
        o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
        check(a7800session_start(s, &o) == 0, "restart");
        check(wait_status(s, "game running", 5000), "and it boots the remembered cartridge");
        check(background_steps(s, 3000), "which runs");
        a7800session_stop(s);
        a7800session_free(s);
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
