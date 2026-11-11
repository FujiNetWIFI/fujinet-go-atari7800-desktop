/*
 * vidtab_live_test -- the Video tab against the real machine: FujiNet's
 * CONFIG client (a real display list with real objects, no ROMs). Running:
 * the last finished frame, the tables and the inspector. Stopped: Display
 * is exactly the frame the machine published, and the Beam view -- the
 * frame redrawn from MARIA's line RAM -- is the same picture at a vblank
 * stop, which proves the row decoder and the captured line RAM. Watchpoints
 * on a pixel's objects and on a palette register.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "a7800debug.h"
#include "a7800session.h"
#include "a7800vidtab.h"
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

static int wait_stopped(a7800debug *d, int want, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (!!a7800debug_is_stopped(d) == want) return 1;
        sleep_ms(5);
    }
    return 0;
}

static uint32_t host_px[A7800SESSION_FB_WIDTH * A7800SESSION_FB_MAX_HEIGHT];
static uint32_t shown[A7800SESSION_FB_WIDTH * A7800SESSION_FB_MAX_HEIGHT];

int main(void)
{
    char cfg[512], data[512];
    a7800session_paths p;
    a7800session *s;
    a7800session_start_opts o;
    a7800debug *d;
    a7800vid_tab *t;
    const uint32_t *pic;
    int w, h, host_h = 0;
    uint64_t serial = 0;

    test_tmpdir(cfg, sizeof cfg, "vcfg");
    test_tmpdir(data, sizeof data, "vdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = a7800session_new(&p);
    if (!s) return 1;
    a7800session_default_opts(s, &o);
    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    check(a7800session_start(s, &o) == 0, "session starts (CONFIG)");
    d = a7800session_debugger(s);
    t = a7800vid_tab_new(s);
    check(t != NULL, "tab");
    {
        const uint64_t start = a7800session_frame_count(s);
        for (int waited = 0; waited < 8000 && a7800session_frame_count(s) < start + 240; waited += 10) sleep_ms(10);
    }

    /* running */
    a7800vid_tab_set_zoom(t, 1);
    a7800vid_tab_set_overlays(t, 0);
    check((a7800vid_tab_refresh(t, 1) & A7800VID_TAB_PICTURE) != 0, "refresh while running");
    pic = a7800vid_tab_picture(t, &w, &h);
    check(pic && w == 320 && h >= 224, "Display (running) is the 320-wide frame");
    {
        const a7800vid_line_row *rows;
        const a7800vid_reg *regs;
        char buf[4096];
        check(a7800vid_tab_lines(t, &rows) > 0 && rows[0].cycles > 0, "the Lines table has rows with DMA cycles");
        check(a7800vid_tab_regs(t, &regs) >= 10, "registers");
        check(a7800vid_tab_text(t, A7800VID_S_TIMING, buf, sizeof buf) > 20 && strstr(buf, "Zones"), "timing text");
    }
    {
        uint64_t force = 0;
        int diff = 0, hh = 0;
        a7800session_copy_frame(s, host_px, &hh, &force);
        a7800vid_tab_refresh(t, 1);
        pic = a7800vid_tab_picture(t, &w, &h);
        for (int i = 0; pic && i < w * h; i++) if (pic[i] != (host_px[i] | 0xff000000u)) diff++;
        printf("  running: %d pixels differ from the machine's frame\n", diff);
        check(diff == 0, "Display (running, no overlays) is the machine's frame");
    }
    a7800vid_tab_set_view(t, A7800VID_V_BEAM);
    a7800vid_tab_refresh(t, 1);
    check(a7800vid_tab_picture(t, &w, &h) == NULL && strlen(a7800vid_tab_why(t)) > 10, "Beam while running says why");
    a7800vid_tab_set_view(t, A7800VID_V_DISPLAY);

    /* stopped at a vblank */
    a7800debug_attach(d);
    check(wait_stopped(d, 1, 3000), "stopped");
    a7800debug_frame(d);
    check(wait_stopped(d, 1, 3000), "stopped at the next vblank");
    sleep_ms(50);
    {
        static uint32_t before[A7800SESSION_FB_WIDTH * A7800SESSION_FB_MAX_HEIGHT];
        int diff_before = 0, diff_after = 0, before_h = 0;
        uint64_t force = 0;
        a7800session_copy_frame(s, before, &before_h, &force);
        a7800vid_tab_set_zoom(t, 1);
        a7800vid_tab_refresh(t, 1);
        pic = a7800vid_tab_picture(t, &w, &h);
        force = 0;
        a7800session_copy_frame(s, host_px, &host_h, &force);
        for (int i = 0; pic && i < w * h; i++) {
            if (pic[i] != (before[i] | 0xff000000u)) diff_before++;
            if (pic[i] != (host_px[i] | 0xff000000u)) diff_after++;
        }
        printf("  Display %dx%d, host %dx%d: %d pixels differ from the frame before, %d from the one after\n",
               w, h, 320, host_h, diff_before, diff_after);
        check(pic && w == 320 && h == host_h && h == before_h && diff_before == 0 && diff_after == 0,
              "Display at a stop is exactly the machine's frame");
        memcpy(shown, pic ? pic : host_px, (size_t)w * (size_t)h * sizeof *shown);
    }
    a7800vid_tab_set_view(t, A7800VID_V_BEAM);
    a7800vid_tab_refresh(t, 1);
    pic = a7800vid_tab_picture(t, &w, &h);
    {
        int diff = 0, first = -1;
        const char *const *tl;
        a7800vid_tab_timing(t, &tl);
        printf("  (%s | %s)\n", tl[0], tl[2]);
        for (int i = 0; pic && i < w * h; i++)
            if (pic[i] != shown[i]) { if (first < 0) first = i; diff++; }
        printf("  Beam differs from Display in %d of %d pixels (first at %d,%d)\n", diff, w * h,
               first < 0 ? -1 : first % w, first < 0 ? -1 : first / w);
        check(pic && diff == 0, "the Beam view (redrawn from the line RAM) equals the frame at a vblank stop");
    }
    {
        int distinct = 0;
        uint32_t seen[8] = { 0 };
        for (int i = 0; i < w * h && distinct < 8; i++) {
            int known = 0;
            for (int k = 0; k < distinct; k++) if (seen[k] == shown[i]) known = 1;
            if (!known) seen[distinct++] = shown[i];
        }
        check(distinct >= 2, "the CONFIG frame has colours (the comparison is not of a blank screen)");
    }

    /* the inspector and watchpoints, on an object */
    a7800vid_tab_set_view(t, A7800VID_V_DISPLAY);
    a7800vid_tab_refresh(t, 1);
    {
        int found = 0;
        const a7800vid_pixel *px = NULL;
        for (int y = 0; y < h && !found; y += 2)
            for (int x = 0; x < w && !found; x += 2) {
                a7800vid_tab_hover(t, x, y);
                px = a7800vid_tab_pixel(t);
                if (px && px->active && px->nobjects > 0) found = 1;
            }
        check(found, "an object covers some pixel");
        if (found) {
            printf("%s\n", px->text);
            check(strstr(px->text, "header $") && strstr(px->text, "DLL entry bytes"), "the inspector reads the header and DLL bytes from memory");
            check(a7800vid_tab_break_on_pixel(t) > 0, "break on pixel: watchpoints on the object, its zone and its register");
            check(a7800vid_tab_break_on_pixel(t) > 0, "...again: not added twice");
            {
                a7800debug_breakpoint bl[64];
                const int n = a7800debug_breakpoint_list(d, bl, 64);
                check(n >= 2 && n <= 8, "a handful of watchpoints");
            }
        }
    }
    check(a7800vid_tab_break_on_swatch(t, 9) > 0, "break on swatch");
    a7800debug_breakpoint_clear(d);

    a7800vid_tab_free(t);
    a7800debug_detach(d);
    a7800session_stop(s);
    a7800session_free(s);
    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
