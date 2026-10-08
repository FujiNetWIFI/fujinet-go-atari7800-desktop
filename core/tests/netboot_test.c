/*
 * netboot_test -- a network boot end to end, through the real in-process
 * FujiNet: the cartridge bring-up's fujiboot client (MOUNT_HOST ->
 * SET_DEVICE_FULLPATH -> MOUNT_IMAGE on host slot 0) runs as the open
 * cartridge, FujiNet streams the image back to the cartridge, the loader
 * copies it into the SRAM and hands over, and the new image runs (hello.a78
 * claims the mailbox, so the link stays open). Then Reboot to CONFIG brings
 * CONFIG back.
 *
 * Needs fujiboot.a78 and hello.a78 from fujinet-firmware's
 * pico/atari-7800/build (./build.sh there): A7800_TESTROM_DIR=/path/to/it.
 * fujiboot mounts the path it was built with (BOOT_PATH, "/soak.bin" by
 * default); the test reads it out of the image and puts hello.a78 there.
 * SKIPs (77) without them or without the FujiNet runtime.
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
#include "a7800debug.h"
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

static int wait_status(a7800session *s, const char *want, int timeout_ms)
{
    char st[160] = "";
    int waited = 0;
    while (waited < timeout_ms) {
        a7800session_cart_status(s, st, sizeof st);
        if (strstr(st, want)) return 1;
        sleep_ms(50); waited += 50;
    }
    printf("  (status: %s)\n", st);
    return 0;
}

static long read_file(const char *p, uint8_t *dst, long max)
{
    FILE *f = fopen(p, "rb");
    long n;
    if (!f) return -1;
    n = (long)fread(dst, 1, (size_t)max, f);
    fclose(f);
    return n;
}

/* The path fujiboot was built to mount: the image's one NUL-terminated
 * string that starts with '/' and names a file. */
static int boot_path(const uint8_t *img, long n, char *dst, int dstsz)
{
    long i;
    for (i = 128; i + 2 < n; i++) {
        long j = i;
        if (img[i] != '/' || (i > 0 && img[i - 1] != 0)) continue;
        while (j < n && img[j] >= 0x20 && img[j] < 0x7f) j++;
        if (j < n && img[j] == 0 && j - i > 4 && j - i < dstsz && memchr(img + i, '.', (size_t)(j - i))) {
            memcpy(dst, img + i, (size_t)(j - i));
            dst[j - i] = '\0';
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    const char *dir = getenv("A7800_TESTROM_DIR");
    char cfg[512], data[512], boot[1024], hello[1024], dest[1200], path[128];
    static uint8_t bimg[0x80000], himg[0x80000];
    long nb, nh;
    uint32_t hello_crc;
    a7800session_paths p;
    a7800session *s;
    a7800session_start_opts o;

    if (!dir) {
        printf("SKIP: set A7800_TESTROM_DIR to fujinet-firmware's pico/atari-7800/build\n");
        return 77;
    }
    snprintf(boot, sizeof boot, "%s/fujiboot.a78", dir);
    snprintf(hello, sizeof hello, "%s/hello.a78", dir);
    nb = read_file(boot, bimg, sizeof bimg);
    nh = read_file(hello, himg, sizeof himg);
    if (nb <= 128 || nh <= 128) {
        printf("SKIP: no fujiboot.a78 / hello.a78 in %s\n", dir);
        return 77;
    }
    if (!boot_path(bimg, nb, path, sizeof path)) {
        printf("SKIP: cannot find the path fujiboot.a78 mounts\n");
        return 77;
    }
    hello_crc = test_crc32(himg + 128, (size_t)(nh - 128));
    printf("fujiboot mounts %s; hello.a78 CRC %08X\n", path, hello_crc);

    test_tmpdir(cfg, sizeof cfg, "ncfg");
    test_tmpdir(data, sizeof data, "ndata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data;
    s = a7800session_new(&p);
    if (!s) return 1;

    a7800session_default_opts(s, &o);
    o.enable_audio = 0; o.enable_gamepad = 0; o.enable_fujinet = 1;
    check(a7800session_start(s, &o) == 0, "session starts with FujiNet");
    if (!a7800session_fujinet_running(s)) {
        printf("SKIP: no FujiNet runtime available\n");
        a7800session_free(s);
        return 77;
    }

    /* the image to boot goes where fujiboot looks, on the SD host (slot 0) */
    snprintf(dest, sizeof dest, "%s%s", a7800session_sd_path(s), path);
    check(test_write_file(dest, himg, (size_t)nh) == 0, "hello.a78 placed on the SD host");

    check(a7800session_load_cart(s, boot) == 0, "fujiboot opened as the cartridge");
    {
        a7800debug *d = a7800session_debugger(s);
        a7800debug_cart c;
        int waited = 0, done = 0;
        memset(&c, 0, sizeof c);
        while (waited < 60000 && !done) {
            a7800debug_cart_get(d, &c);
            done = c.booted_image && c.live_crc == hello_crc && c.mode != 1;
            if (!done) { sleep_ms(100); waited += 100; }
        }
        printf("  (after %d ms: mode %s, live %08X, handover %d, link %d)\n",
               waited, c.mode_name, c.live_crc, c.handover, c.link_up);
        check(done, "the mounted image was streamed, loaded and runs");
        check(c.handover == 2, "the loader started it (no BIOS)");
        check(c.mode == 3, "hello.a78 claims the mailbox: a FujiNet app");
    }
    check(wait_status(s, "connected", 10000), "and its link is up");

    check(a7800session_reboot_to_config(s) == 0, "reboot to CONFIG");
    {
        int waited = 0;
        while (a7800session_cart_booted_game(s) && waited < 10000) { sleep_ms(50); waited += 50; }
        check(!a7800session_cart_booted_game(s), "nothing booted: CONFIG is back");
    }
    check(wait_status(s, "connected", 10000), "with its link up");

    a7800session_stop(s);
    a7800session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
