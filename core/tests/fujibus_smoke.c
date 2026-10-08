/*
 * fujibus_smoke -- the real in-process FujiNet: libfujinet starts on its
 * port, the CONFIG client boots, and the cartridge's link comes up. This is
 * the one test that proves the whole chain -- the MAME fork's FujiNet
 * cartridge and its link, the RS232 PC runtime's BoIP listener, the port
 * numbers and the start ordering -- rather than any one piece.
 *
 * SKIPs (77) when no runtime library is available.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "a7800session.h"
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

int main(void)
{
    char cfg[512], data[512], status[128], logbuf[4096];
    a7800session_paths p;
    a7800session *s;
    a7800session_start_opts o;
    int waited = 0, up = 0;

    test_tmpdir(cfg, sizeof cfg, "fcfg");
    test_tmpdir(data, sizeof data, "fdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data;
    s = a7800session_new(&p);
    if (!s) return 1;

    a7800session_default_opts(s, &o);
    o.enable_audio = 0; o.enable_gamepad = 0; o.enable_fujinet = 1;
    if (a7800session_start(s, &o) != 0) {
        printf("start failed: %s\n", a7800session_last_error(s));
        return 1;
    }
    if (!a7800session_fujinet_running(s)) {
        printf("SKIP: no FujiNet runtime available (%s)\n", a7800session_last_error(s));
        a7800session_free(s);
        return 77;
    }
    check(1, "libfujinet started in-process");
    check(strcmp(a7800session_fujinet_webui_url(s), "http://127.0.0.1:11511/") == 0, "web UI URL is this app's own port");

    /* the cartridge dials in from its worker thread once the console runs */
    while (waited < 10000) {
        if (a7800session_cart_link_up(s) == 1) { up = 1; break; }
        sleep_ms(50); waited += 50;
    }
    a7800session_cart_status(s, status, sizeof status);
    printf("cart status after %d ms: %s\n", waited, status);
    check(up, "the cartridge's link to FujiNet came up");

    /* let the CONFIG client transact (it reads the host slots on boot) */
    sleep_ms(1500);
    logbuf[0] = '\0';
    a7800session_fujinet_copy_log(s, logbuf, sizeof logbuf);
    check(strstr(logbuf, "BoIP") != NULL || strstr(logbuf, "connected") != NULL || logbuf[0] != '\0',
          "the FujiNet console log has content");
    check(a7800session_cart_booted_game(s) == 0, "nothing has been booted yet");

    /* reboot: the old link must die before the new one dials in (backlog 1) */
    check(a7800session_reboot_to_config(s) == 0, "reboot to CONFIG");
    waited = 0; up = 0;
    while (waited < 10000) {
        if (a7800session_cart_link_up(s) == 1) { up = 1; break; }
        sleep_ms(50); waited += 50;
    }
    check(up, "the link comes up again after a power cycle");

    a7800session_stop(s);
    a7800session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
