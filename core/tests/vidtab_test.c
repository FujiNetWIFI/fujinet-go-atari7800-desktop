/*
 * vidtab_test -- the Video tab's controller (core/src/vidtab.c) fed
 * synthetic frames: no machine, no ROMs. Zones and objects, the Decoded
 * boxes pixel for pixel, a palette change and a DPP change mid-frame in the
 * Lines table, the inspector's text, Values, Display / Beam, the 320 read
 * mode, register ages, and a fuzz over random captures, every view, source,
 * overlay and zoom.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a7800vidcap.h"
#include "a7800vidtab.h"

/* MinGW has no setenv/unsetenv; _putenv_s does the same job there. */
static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (line %d)\n", m, __LINE__); fails++; } \
                         else printf("ok: %s\n", m); } while (0)

#define FBW 320
#define FBH 224
#define VIS_TOP 16

static a7800vid_capture cap;

static uint32_t console(uint8_t byte)
{
    return a7800vid_fallback_colour(byte);
}

/* the zone every row belongs to: 16 rows each; objects in zones 1 and 2 */
static void make_cap(a7800vid_capture *c, int fbh)
{
    memset(c, 0, sizeof *c);
    c->fb_w = FBW; c->fb_h = fbh;
    c->vis_left = 0; c->vis_top = VIS_TOP;
    c->total_lines = 263;
    c->vpos = 200; c->hpos = 100;
    c->beam_row = fbh;
    c->frame = 5;
    c->serial = 1;
    for (int i = 0; i < FBW * fbh; i++) c->fb[i] = 0x00123456u;
    for (int row = 0; row < fbh; row++) {
        a7800vid_line *ln = &c->lines[row + VIS_TOP];
        const int zone = row / 16;
        ln->valid = 1;
        ln->ctrl = 0x40;                    /* DMA on, 160A/B */
        ln->charbase = 0x20;
        ln->dpp = 0x1800;
        ln->dll = (uint16_t)(0x1900 + 3 * zone);
        ln->dl = (uint16_t)(0x2000 + 16 * zone);
        ln->offset = (uint8_t)(15 - row % 16);
        ln->dli = zone == 3;
        ln->cycles = 100;
        for (int i = 0; i < 32; i++) ln->palette[i] = (uint8_t)(i * 7 + 3);
        for (int i = 0; i < 32; i += 4) ln->palette[i] = ln->palette[0];
        if (zone == 1) {
            ln->nobj = 1;
            ln->obj[0] = (a7800vid_object){ 0x2010, 0x3000, 20, 4, 2, 0 };
            for (int i = 20; i < 36; i++) ln->cells[i] = (2 << 2) | 1;
        } else if (zone == 2) {
            ln->nobj = 1;
            ln->obj[0] = (a7800vid_object){ 0x2020, 0x3100, 60, 2, 5, A7800VID_OBJ_WRITE | A7800VID_OBJ_EXT };
            for (int i = 60; i < 64; i++) ln->cells[i] = (5 << 2) | 2;
        }
    }
}

static void settle(a7800vid_tab *t, int view, int live)
{
    a7800vid_tab_set_view(t, view);
    a7800vid_tab_feed(t, &cap, live);
    a7800vid_tab_refresh(t, 1);
}

int main(void)
{
    a7800vid_tab *t;
    const uint32_t *pic;
    int w, h;

    set_env("A7800_DEBUGGER_VIDEO_VIEW", "zones");
    set_env("A7800_DEBUGGER_VIDEO_OVERLAYS", "zones,dli");
    set_env("A7800_DEBUGGER_VIDEO_ZOOM", "3");
    set_env("A7800_DEBUGGER_VIDEO_SECTION", "tim");
    t = a7800vid_tab_new(NULL);
    CHECK(t && a7800vid_tab_view(t) == A7800VID_V_DECODED && a7800vid_tab_source(t) == A7800VID_D_ZONES &&
          a7800vid_tab_zoom(t) == 3 && a7800vid_tab_overlays(t) == (A7800VID_OV_ZONES | A7800VID_OV_DLI) &&
          a7800vid_tab_env_section(t) == A7800VID_S_TIMING, "env variables name the view, overlays, zoom, section");
    a7800vid_tab_free(t);
    set_env("A7800_DEBUGGER_VIDEO_VIEW", "beam");
    set_env("A7800_DEBUGGER_VIDEO_OVERLAYS", "none");
    set_env("A7800_DEBUGGER_VIDEO_ZOOM", "fit");
    set_env("A7800_DEBUGGER_VIDEO_SECTION", "");
    t = a7800vid_tab_new(NULL);
    CHECK(t && a7800vid_tab_view(t) == A7800VID_V_BEAM && a7800vid_tab_overlays(t) == 0 && a7800vid_tab_zoom(t) == 0 &&
          a7800vid_tab_env_section(t) == A7800VID_S_PIXEL, "view names, none, fit");
    a7800vid_tab_free(t);
    set_env("A7800_DEBUGGER_VIDEO_VIEW", "");
    set_env("A7800_DEBUGGER_VIDEO_OVERLAYS", "all");
    t = a7800vid_tab_new(NULL);
    CHECK(a7800vid_tab_overlays(t) == A7800VID_OV_ALL, "overlays all");
    a7800vid_tab_free(t);
    set_env("A7800_DEBUGGER_VIDEO_OVERLAYS", "beam,active");
    set_env("A7800_DEBUGGER_VIDEO_ZOOM", "");

    t = a7800vid_tab_new(NULL);
    a7800vid_tab_set_overlays(t, 0);
    a7800vid_tab_set_zoom(t, 1);

    /* ---- Decoded: the objects' boxes ---- */
    make_cap(&cap, FBH);
    settle(t, A7800VID_V_DECODED, 0);
    a7800vid_tab_set_source(t, A7800VID_D_OBJECTS);
    a7800vid_tab_refresh(t, 1);
    pic = a7800vid_tab_picture(t, &w, &h);
    CHECK(pic && w == FBW && h == FBH, "Decoded is the frame's size");
    {
        const uint32_t rgb1 = console(cap.lines[VIS_TOP + 20].palette[2 * 4 + 1]);
        const uint32_t rgb2 = console(cap.lines[VIS_TOP + 40].palette[5 * 4 + 1]);
        const uint32_t dim = 0xff000000u | (((0x12u * 106) >> 8) << 16) | (((0x34u * 106) >> 8) << 8) | ((0x56u * 106) >> 8);
        /* the tint: 90/256 of the object's colour over the dimmed frame */
        const uint32_t tint = 0xff000000u | ((((dim >> 16 & 255) * 166 + (rgb1 >> 16 & 255) * 90) >> 8) << 16) |
                              ((((dim >> 8 & 255) * 166 + (rgb1 >> 8 & 255) * 90) >> 8) << 8) |
                              (((dim & 255) * 166 + (rgb1 & 255) * 90) >> 8);
        CHECK(pic && pic[5 * w + 5] == dim, "the rest of the frame is the dimmed capture");
        CHECK(pic && pic[20 * w + 50] == tint && pic[24 * w + 60] == tint, "object 1's inside is the dimmed frame tinted with its colour");
        CHECK(pic && pic[20 * w + 50] != rgb1, "...not an opaque fill");
        CHECK(pic && pic[20 * w + 39] == dim && pic[20 * w + 72] == dim && pic[15 * w + 50] == dim && pic[32 * w + 50] == dim,
              "nothing outside its extent: cells 20-35, pixels 40-71, rows 16-31");
        CHECK(pic && pic[20 * w + 40] != tint && pic[20 * w + 40] != dim && pic[20 * w + 71] == pic[20 * w + 40], "a left and right outline");
        CHECK(pic && pic[16 * w + 50] != tint && pic[16 * w + 50] != dim && pic[31 * w + 50] == pic[16 * w + 50], "a top and bottom outline");
        CHECK(pic && pic[40 * w + 120] != dim && pic[40 * w + 127] == pic[40 * w + 120] && pic[40 * w + 128] == dim && pic[40 * w + 119] == dim,
              "a write mode object is 2 cells a byte: two bytes are 4 cells, 8 pixels");
        CHECK(pic && pic[40 * w + 120] != pic[20 * w + 40] && rgb1 != rgb2, "each object's outline is in its own colour");
    }

    /* ---- Zones ---- */
    a7800vid_tab_set_source(t, A7800VID_D_ZONES);
    a7800vid_tab_refresh(t, 1);
    pic = a7800vid_tab_picture(t, &w, &h);
    CHECK(pic && w == FBW && h == FBH && pic[5 * w + 5] != pic[20 * w + 5], "zones: bands are tinted differently");

    /* ---- Palette ---- */
    a7800vid_tab_set_source(t, A7800VID_D_PALETTE);
    a7800vid_tab_refresh(t, 1);
    pic = a7800vid_tab_picture(t, &w, &h);
    CHECK(pic && w == 192 && h == 96, "palette: 8 x 4 swatches");
    a7800vid_tab_hover(t, 3 * 24 + 5, 2 * 24 + 5);
    CHECK(a7800vid_tab_pixel(t) && a7800vid_tab_pixel(t)->reg_index == 14 &&
          a7800vid_tab_pixel(t)->reg_value == (uint8_t)(14 * 7 + 3) && strstr(a7800vid_tab_pixel(t)->text, "P3C2"),
          "palette: the cell is the register (P3C2 = index 14)");
    a7800vid_tab_hover(t, -1, -1);
    a7800vid_tab_set_source(t, A7800VID_D_OBJECTS);

    /* ---- the Lines table: a palette change, a DPP change ---- */
    for (int row = 100; row < FBH; row++) cap.lines[row + VIS_TOP].palette[9] = 0xEE;
    for (int row = 150; row < FBH; row++) cap.lines[row + VIS_TOP].dpp = 0x1A00;
    a7800vid_tab_feed(t, &cap, 0);
    a7800vid_tab_refresh(t, 1);
    {
        const a7800vid_line_row *rows;
        const int n = a7800vid_tab_lines(t, &rows);
        int pal = 0, dpp = 0, zones = 0;
        for (int i = 0; i < n; i++) {
            if ((rows[i].changed_mask & A7800VID_LC_PALETTE) && rows[i].row == 100) pal++;
            if ((rows[i].changed_mask & A7800VID_LC_DPP) && rows[i].row == 150) dpp++;
            if (rows[i].changed_mask & A7800VID_LC_ZONE) zones++;
        }
        CHECK(pal == 1, "the Lines table finds the palette change at row 100");
        CHECK(dpp == 1, "and the DPP change at row 150");
        CHECK(zones == FBH / 16 && rows[0].row == 0 && rows[0].scanline == VIS_TOP && rows[1].row == 16,
              "and a zone start every 16 rows");
    }

    /* ---- Display == capture, the inspector ---- */
    make_cap(&cap, FBH);
    settle(t, A7800VID_V_DISPLAY, 0);
    pic = a7800vid_tab_picture(t, &w, &h);
    CHECK(pic && w == FBW && h == FBH && pic[0] == 0xff123456u && pic[FBW * FBH - 1] == 0xff123456u, "Display is the captured frame");
    cap.fb[77 * FBW + 11] = 0x00abcdef;
    settle(t, A7800VID_V_DISPLAY, 0);
    pic = a7800vid_tab_picture(t, &w, &h);
    CHECK(pic && pic[77 * w + 11] == 0xffabcdefu, "...pixel for pixel");
    cap.fb[77 * FBW + 11] = 0x00123456u;
    settle(t, A7800VID_V_DISPLAY, 0);

    a7800vid_tab_hover(t, 45, 20);
    {
        const a7800vid_pixel *p = a7800vid_tab_pixel(t);
        CHECK(p && p->active && p->cell == 22 && p->half == 1 && p->row == 20 && p->scanline == 20 + VIS_TOP &&
              p->dll == 0x1903 && p->dl == 0x2010 && p->offset == 11 && p->nobjects == 1 &&
              p->obj[0].header == 0x2010 && p->obj[0].graphics == 0x3000 && p->obj[0].palette == 2 &&
              p->obj[0].first_cell == 20 && p->obj[0].cells == 16 && p->obj[0].top &&
              p->cell_value == 9 && p->reg_index == 9 && p->palette == 2 && p->colour == 1 &&
              p->reg_value == (uint8_t)(9 * 7 + 3), "the inspector: zone, object, cell, palette register");
        CHECK(p && strstr(p->text, "DLL entry $1903") && strstr(p->text, "header $2010") && strstr(p->text, "graphics $3000") &&
              strstr(p->text, "palette 2") && strstr(p->text, "P2C1 ($29)") && strstr(p->text, "Object (topmost)"),
              "...and its text");
    }
    a7800vid_tab_hover(t, 200, 20);
    CHECK(a7800vid_tab_pixel(t) && a7800vid_tab_pixel(t)->nobjects == 0 && strstr(a7800vid_tab_pixel(t)->text, "background"),
          "a pixel no object covers says so");
    a7800vid_tab_hover(t, 5, 218);
    CHECK(a7800vid_tab_pixel(t) && a7800vid_tab_pixel(t)->active, "a row near the bottom");
    a7800vid_tab_hover(t, 5, 223);
    CHECK(a7800vid_tab_pixel(t) && a7800vid_tab_pixel(t)->row == 223, "the last row");
    CHECK(a7800vid_tab_break_on_pixel(t) == -1, "no debugger: no watchpoints");
    CHECK(a7800vid_tab_break_on_swatch(t, 3) == -1, "...nor on a swatch");
    a7800vid_tab_pin(t, 10, 20);
    CHECK(a7800vid_tab_pixel(t) && a7800vid_tab_pixel(t)->x == 10, "pin");
    a7800vid_tab_hover(t, 50, 50);
    CHECK(a7800vid_tab_pixel(t)->x == 10, "the pin wins over the hover");
    {
        int hv[4], pn[4];
        CHECK(a7800vid_tab_marks(t, hv, pn) == 3 && pn[0] == 10 && pn[1] == 20 && hv[0] == 50, "marks");
    }
    a7800vid_tab_pin(t, 10, 20);
    CHECK(a7800vid_tab_pixel(t)->x == 50, "the same spot unpins");
    a7800vid_tab_unpin(t);
    a7800vid_tab_hover(t, -1, -1);
    CHECK(a7800vid_tab_pixel(t) == NULL, "the pointer left");

    /* ---- the 320 read modes ---- */
    make_cap(&cap, FBH);
    for (int row = 60; row < 64; row++) {
        a7800vid_line *ln = &cap.lines[row + VIS_TOP];
        ln->ctrl = 0x40 | 3;            /* 320A/C */
        ln->cells[30] = 0x1E;
        ln = &cap.lines[row + 8 + VIS_TOP];
        ln->ctrl = 0x40 | 2;            /* 320B/D */
        ln->cells[30] = 0x1B;
    }
    settle(t, A7800VID_V_DISPLAY, 0);
    a7800vid_tab_hover(t, 60, 61);
    CHECK(a7800vid_tab_pixel(t) && a7800vid_tab_pixel(t)->reg_index == 0x1E && a7800vid_tab_pixel(t)->read_mode == 3, "320A/C: first pixel of the cell");
    a7800vid_tab_hover(t, 61, 61);
    CHECK(a7800vid_tab_pixel(t)->reg_index == 0x1C, "320A/C: second pixel");
    a7800vid_tab_hover(t, 60, 69);
    CHECK(a7800vid_tab_pixel(t)->reg_index == ((0x10) | (0x02) | 1) && a7800vid_tab_pixel(t)->read_mode == 2, "320B/D: first pixel");
    a7800vid_tab_hover(t, 61, 69);
    CHECK(a7800vid_tab_pixel(t)->reg_index == ((0x10) | ((0x1B << 1) & 2) | ((0x1B >> 2) & 1)), "320B/D: second pixel");
    a7800vid_tab_hover(t, -1, -1);

    /* ---- Values, overlays all on (no crash), swatch uses ---- */
    make_cap(&cap, FBH);
    settle(t, A7800VID_V_VALUES, 0);
    pic = a7800vid_tab_picture(t, &w, &h);
    CHECK(pic && w == FBW && h == FBH, "Values is the frame's size");
    {
        const uint32_t cool = pic ? pic[20 * w + 45] : 0, warm = pic ? pic[40 * w + 122] : 0;
        CHECK(pic && (cool & 0xff) > ((cool >> 16) & 0xff), "palette 2 is cool (blue over red)");
        CHECK(pic && ((warm >> 16) & 0xff) > (warm & 0xff), "palette 5 is warm (red over blue)");
        CHECK(pic && pic[20 * w + 45] != pic[20 * w + 200], "colour 1 and 0 differ");
    }
    a7800vid_tab_set_overlays(t, A7800VID_OV_ALL);
    a7800vid_tab_set_zoom(t, 4);
    settle(t, A7800VID_V_DECODED, 0);
    pic = a7800vid_tab_picture(t, &w, &h);
    CHECK(pic && w == FBW * 4 && h == FBH * 4, "zoom 4 with every overlay");
    {
        int opaque = 1;
        for (int i = 0; pic && i < w * h; i++) if ((pic[i] >> 24) != 0xff) opaque = 0;
        CHECK(opaque, "every pixel opaque");
    }
    for (int v = 0; v < A7800VID_V_COUNT; v++) {
        a7800vid_tab_set_source(t, v % A7800VID_D_COUNT);
        settle(t, v, 1);
    }
    CHECK(1, "every view and source with every overlay, stopped");
    a7800vid_tab_set_overlays(t, 0);
    a7800vid_tab_set_zoom(t, 1);
    a7800vid_tab_set_source(t, A7800VID_D_OBJECTS);
    a7800vid_tab_feed(t, &cap, 0);
    a7800vid_tab_refresh(t, 1);
    {
        const a7800vid_swatch *sw;
        long sum = 0;
        const int n = a7800vid_tab_swatches(t, &sw);
        for (int i = 0; i < n; i++) sum += sw[i].uses;
        CHECK(n == 32 && sum == (long)FBW * FBH, "32 swatches whose uses add up to every pixel of every drawn row");
        CHECK(n == 32 && sw[9].uses == 16 * 2 * 16 && sw[22].uses == 16 * 2 * 4,
              "...P2C1 is the 16 cells of its object on 16 rows, P5C2 the 4 of the other");
        CHECK(!strcmp(sw[0].name, "BACKGRND") && !strcmp(sw[1].name, "P0C1") && !strcmp(sw[31].name, "P7C3") &&
              sw[9].address == 0x29 && sw[9].rgb == console(sw[9].value), "swatch names, addresses and colours");
    }

    /* ---- Beam ---- */
    settle(t, A7800VID_V_BEAM, 0);
    CHECK(a7800vid_tab_picture(t, &w, &h) == NULL && strlen(a7800vid_tab_why(t)) > 10,
          "Beam needs a stopped machine and says why");
    for (int row = 0; row < FBH; row++) cap.lines[row + VIS_TOP].palette[9] = 0x55;
    cap.beam_row = 100;
    a7800vid_tab_feed(t, &cap, 1);
    a7800vid_tab_refresh(t, 1);
    pic = a7800vid_tab_picture(t, &w, &h);
    CHECK(pic && w == FBW && h == FBH && pic[20 * w + 45] == console(0x55) && pic[20 * w + 45] != 0xff123456u,
          "Beam: above the beam the frame is redrawn from the line RAM");
    CHECK(pic && pic[150 * w + 5] != 0xff123456u && (pic[150 * w + 5] & 0xff) < 0x56 && (pic[150 * w + 5] & 0xff) > 0,
          "Beam: below it the last frame, dimmed");
    {
        const char *const *tl;
        CHECK(a7800vid_tab_timing(t, &tl) >= 8 && strchr(tl[0], '\t'), "timing lines are label<TAB>value");
        CHECK(a7800vid_tab_beam_permille(t) == 200 * 1000 / 263, "beam per-mille");
    }

    /* ---- registers, ages, text ---- */
    make_cap(&cap, FBH);
    a7800vid_tab_feed(t, &cap, 0);
    a7800vid_tab_refresh(t, 1);
    {
        const a7800vid_reg *r;
        const int n = a7800vid_tab_regs(t, &r);
        CHECK(n >= 10 && !strcmp(r[0].name, "CTRL") && !strcmp(r[0].value, "40") && strstr(r[0].meaning, "DMA on") &&
              !strcmp(r[2].name, "DPP") && !strcmp(r[2].value, "$1800"), "registers with meanings");
        for (int row = 0; row < FBH; row++) cap.lines[row + VIS_TOP].dpp = 0x1C00;
        a7800vid_tab_feed(t, &cap, 0);
        a7800vid_tab_refresh(t, 1);
        a7800vid_tab_regs(t, &r);
        CHECK(r[2].age == 0 && r[0].age > 0, "a changed register has age 0, others older");
        a7800vid_tab_refresh(t, 1);
        a7800vid_tab_regs(t, &r);
        CHECK(r[2].age == 1, "age counts refreshes");
    }
    {
        char buf[8192];
        for (int s = 0; s < A7800VID_S_COUNT; s++) {
            const int n = a7800vid_tab_text(t, s, buf, sizeof buf);
            CHECK(n > 5, "every section has text");
        }
    }

    /* ---- hover keeps up with refreshes; an empty frame ---- */
    {
        a7800vid_capture *empty = calloc(1, sizeof *empty);
        empty->serial = 1;
        a7800vid_tab_feed(t, empty, 0);
        a7800vid_tab_set_view(t, A7800VID_V_DISPLAY);
        a7800vid_tab_refresh(t, 1);
        CHECK(a7800vid_tab_picture(t, &w, &h) == NULL && strlen(a7800vid_tab_why(t)) > 5, "no frame yet says why");
        a7800vid_tab_set_view(t, A7800VID_V_VALUES);
        a7800vid_tab_refresh(t, 1);
        a7800vid_tab_hover(t, 5, 5);
        CHECK(1, "an empty capture does not crash any view");
        free(empty);
    }
    a7800vid_tab_free(t);

    /* ---- fuzz ---- */
    t = a7800vid_tab_new(NULL);
    srand(7);
    for (int iter = 0; iter < 150; iter++) {
        static const int heights[] = { 224, 260, 100, 288 };
        const int fbh = heights[rand() % 4];
        make_cap(&cap, fbh);
        cap.vis_left = rand() % 40 - 10;
        cap.vis_top = rand() % 60;
        for (int i = 0; i < A7800VID_LINES; i++) {
            a7800vid_line *ln = &cap.lines[i];
            ln->valid = (uint8_t)(rand() % 8 != 0);
            ln->ctrl = (uint8_t)rand();
            ln->nobj = (uint8_t)(rand() % 40);
            ln->offset = (uint8_t)rand(); ln->holey = (uint8_t)(rand() & 3); ln->dli = (uint8_t)(rand() & 1);
            ln->dll = (uint16_t)rand(); ln->dl = (uint16_t)rand(); ln->dpp = (uint16_t)rand();
            ln->cycles = (uint16_t)(rand() % 600);
            for (int k = 0; k < 32; k++) ln->palette[k] = (uint8_t)rand();
            for (int k = 0; k < A7800VID_CELLS; k++) ln->cells[k] = (uint8_t)rand();
            for (int k = 0; k < A7800VID_OBJECTS; k++) {
                ln->obj[k].header = (uint16_t)rand(); ln->obj[k].graphics = (uint16_t)rand();
                ln->obj[k].hpos = (uint8_t)rand(); ln->obj[k].width = (uint8_t)(rand() % 33);
                ln->obj[k].palette = (uint8_t)(rand() & 7); ln->obj[k].flags = (uint8_t)(rand() & 7);
            }
        }
        cap.fb_w = (rand() % 10 == 0) ? 0 : FBW;
        cap.beam_row = rand() % (fbh + 1);
        cap.total_lines = rand() % 400;
        cap.serial = (uint64_t)(rand() % 3);
        a7800vid_tab_set_view(t, rand() % A7800VID_V_COUNT);
        a7800vid_tab_set_source(t, rand() % A7800VID_D_COUNT);
        a7800vid_tab_set_overlays(t, (unsigned)rand());
        a7800vid_tab_set_zoom(t, (int[]){ 0, 1, 2, 3, 4, 6, 8, 12, 16 }[rand() % 9]);
        a7800vid_tab_feed(t, &cap, rand() & 1);
        a7800vid_tab_refresh(t, 1);
        pic = a7800vid_tab_picture(t, &w, &h);
        for (int k = 0; k < 4 && pic; k++) {
            a7800vid_tab_hover(t, rand() % (w + 4) - 2, rand() % (h + 4) - 2);
            if (k == 1) a7800vid_tab_pin(t, rand() % w, rand() % h);
        }
        a7800vid_tab_refresh(t, 0);
        {
            char buf[2048];
            for (int s = 0; s < A7800VID_S_COUNT; s++) a7800vid_tab_text(t, s, buf, sizeof buf);
        }
    }
    CHECK(1, "fuzz: 150 random captures through every view survived");
    a7800vid_tab_free(t);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
