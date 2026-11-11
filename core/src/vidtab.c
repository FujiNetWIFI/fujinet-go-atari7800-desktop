/*
 * vidtab.c -- the Video tab's controller (core/include/a7800vidtab.h).
 *
 * MARIA's own row decoder (maria.cpp in the MAME fork, draw_scanline's
 * line RAM to pixels) is repeated here for the 160-cell rows the host
 * captures, so the Beam view can redraw the part of this frame the beam has
 * drawn, and the Values view can show the palette register each pixel read.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "a7800vidtab.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "a7800vidcap.h"
#include "session_internal.h"

#define MAXPIX 6000000L         /* the largest picture we will build */
#define MAX_REGS 16
#define MAX_TIMING 12
#define DMA_BUDGET 426
#define DMA_LINE 454
#define SWATCH_PX 24
#define DEFAULT_W 320
#define DEFAULT_H 224

static const int k_zooms[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16 };

enum { BK_FRAME = 0, BK_OTHER };   /* what the base picture is */

struct a7800vid_tab {
    a7800session *s;
    a7800debug *d;
    int view, source, zoom, vw, vh;
    unsigned ov;
    int env_section, capture_on;

    /* the machine's state as presented */
    a7800vid_capture *cap;      /* working frame */
    a7800vid_capture *last;     /* last captured by the emulation thread */
    uint64_t last_serial;
    int have, has_fb, has_live, stopped, fed, cap_is_last;
    int fb_w, fb_h, ref_row;
    uint64_t sig;
    uint32_t rgbtab[256];
    int rgb_valid, rgb_h;

    /* the picture */
    uint32_t *base, *pic;
    size_t base_cap, pic_cap;
    int bw, bh, pw, ph, ez, kind;
    int dirty;
    char why[200];

    /* tables */
    a7800vid_reg regs[MAX_REGS];
    char prev_val[MAX_REGS][24];
    int nregs, regs_primed;
    a7800vid_swatch sw[32];
    long uses[32];
    a7800vid_line_row rows[A7800VID_FB_MAXH];
    int nrows, nzones, nchanges, ndli, nover, peak, peak_row;
    char status[400];
    char timing_buf[MAX_TIMING][112];
    const char *timing_ptr[MAX_TIMING];
    int ntiming;

    /* pixels */
    int hx, hy, px, py;         /* -1: none */
    a7800vid_pixel hover, pin;
};

/* ---- tables of names ----------------------------------------------------------------- */

static const a7800vid_overlay_info k_overlays[] = {
    { A7800VID_OV_ZONES,   "zones",   "Zones (DLL entries)" },
    { A7800VID_OV_DLI,     "dli",     "Display list interrupts" },
    { A7800VID_OV_OBJECTS, "objects", "Objects" },
    { A7800VID_OV_BEAM,    "beam",    "Beam" },
    { A7800VID_OV_HEAT,    "heat",    "DMA cycles" },
    { A7800VID_OV_CHANGED, "changed", "Palette / CTRL / DPP changes" },
    { A7800VID_OV_ACTIVE,  "active",  "DMA outline" },
};
#define N_OVERLAYS ((int)(sizeof k_overlays / sizeof *k_overlays))

const a7800vid_overlay_info *a7800vid_overlays(int *count)
{
    if (count) *count = N_OVERLAYS;
    return k_overlays;
}

static const char *const k_view_names[] = { "Display", "Decoded", "Beam", "Values" };
static const char *const k_view_keys[] = { "display", "decoded", "beam", "values" };
static const char *const k_src_names[] = { "Objects", "Zones", "Palette" };
static const char *const k_src_keys[] = { "objects", "zones", "palette" };
static const char *const k_sec_names[] = { "Pixel", "Registers", "Colours", "Lines", "Timing" };
static const char *const k_sec_keys[] = { "pixel", "registers", "colours", "lines", "timing" };

const char *a7800vid_view_name(int v) { return v >= 0 && v < A7800VID_V_COUNT ? k_view_names[v] : ""; }
const char *a7800vid_view_key(int v) { return v >= 0 && v < A7800VID_V_COUNT ? k_view_keys[v] : ""; }
const char *a7800vid_source_name(int s) { return s >= 0 && s < A7800VID_D_COUNT ? k_src_names[s] : ""; }
const char *a7800vid_source_key(int s) { return s >= 0 && s < A7800VID_D_COUNT ? k_src_keys[s] : ""; }
const char *a7800vid_section_name(int s) { return s >= 0 && s < A7800VID_S_COUNT ? k_sec_names[s] : ""; }
const char *a7800vid_section_key(int s) { return s >= 0 && s < A7800VID_S_COUNT ? k_sec_keys[s] : ""; }

const int *a7800vid_zooms(int *count)
{
    if (count) *count = (int)(sizeof k_zooms / sizeof *k_zooms);
    return k_zooms;
}

/* ---- small helpers ---------------------------------------------------------------------- */

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--) { h ^= *b++; h *= 1099511628211ull; }
    return h;
}

static uint32_t blend(uint32_t dst, uint32_t rgb, int a256)
{
    const uint32_t dr = (dst >> 16) & 255, dg = (dst >> 8) & 255, db = dst & 255;
    const uint32_t sr = (rgb >> 16) & 255, sg = (rgb >> 8) & 255, sb = rgb & 255;
    const uint32_t r = (dr * (uint32_t)(256 - a256) + sr * (uint32_t)a256) >> 8;
    const uint32_t g = (dg * (uint32_t)(256 - a256) + sg * (uint32_t)a256) >> 8;
    const uint32_t b = (db * (uint32_t)(256 - a256) + sb * (uint32_t)a256) >> 8;
    return 0xff000000u | r << 16 | g << 8 | b;
}

static int fits_zoom(int w, int h, int z)
{
    return (long)w * z * (long)h * z <= MAXPIX;
}

static void why_set(a7800vid_tab *t, const char *s)
{
    snprintf(t->why, sizeof t->why, "%s", s);
}

static int ensure(uint32_t **buf, size_t *cap, size_t need)
{
    if (need > *cap) {
        uint32_t *grown = realloc(*buf, need * sizeof **buf);
        if (!grown) return 0;
        *buf = grown;
        *cap = need;
    }
    return 1;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

uint32_t a7800vid_fallback_colour(uint8_t idx)
{
    const int hue = idx >> 4, lum = idx & 15;
    const uint32_t r = (uint32_t)clampi(lum * 14 + (hue * 37 & 0x3f), 0, 255);
    const uint32_t g = (uint32_t)clampi(lum * 14 + (hue * 71 & 0x3f), 0, 255);
    const uint32_t b = (uint32_t)clampi(lum * 14 + (hue * 113 & 0x3f), 0, 255);
    return 0xff000000u | r << 16 | g << 8 | b;
}

/* the console's colour for a palette byte; the debugger's table when there is one */
static uint32_t console_rgb(a7800vid_tab *t, uint8_t idx)
{
    if (!t->rgb_valid) {
        for (int i = 0; i < 256; i++)
            t->rgbtab[i] = t->d ? (a7800debug_color(t->d, (uint8_t)i) | 0xff000000u)
                                : a7800vid_fallback_colour((uint8_t)i);
        t->rgb_valid = 1;
        t->rgb_h = t->fb_h;
    }
    return t->rgbtab[idx];
}

/* ---- MARIA's records ------------------------------------------------------------------------ */

static const a7800vid_line k_blank_line;

/* the record of frame row `row`, or NULL when MARIA drew none */
static const a7800vid_line *row_line(const a7800vid_capture *c, int row)
{
    const int scanline = row + c->vis_top;
    if (row < 0 || scanline < 0 || scanline >= A7800VID_LINES) return NULL;
    return c->lines[scanline].valid ? &c->lines[scanline] : NULL;
}

/* the cells an object filled on its row: [*first, *first + *count) */
static void obj_cells(const a7800vid_line *ln, const a7800vid_object *o, int *first, int *count)
{
    const int per_byte = (o->flags & A7800VID_OBJ_WRITE) ? 2 : 4;
    const int chars = ((o->flags & A7800VID_OBJ_INDIRECT) && (ln->ctrl & 0x10)) ? 2 : 1;
    int n = o->width * chars * per_byte;
    if (o->hpos >= A7800VID_CELLS) n = 0;
    if (o->hpos + n > A7800VID_CELLS) n = A7800VID_CELLS - o->hpos;
    *first = o->hpos;
    *count = n;
}

/* the palette register index (0..31) of the two screen pixels of cell i */
static void cell_regs(const a7800vid_line *ln, int i, int out[2])
{
    const int cell = ln->cells[i];
    switch (ln->ctrl & 3) {
    case 2:
        out[0] = (cell & 0x10) | (cell & 0x02) | ((cell >> 3) & 1);
        out[1] = (cell & 0x10) | ((cell << 1) & 0x02) | ((cell >> 2) & 1);
        break;
    case 3:
        out[0] = (cell & 0x1c) | (cell & 0x02);
        out[1] = (cell & 0x1c) | ((cell << 1) & 0x02);
        break;
    default:
        out[0] = out[1] = cell;
        break;
    }
    out[0] &= 31;
    out[1] &= 31;
}

/* the register index of every screen pixel of a row, 0..2 * cells - 1 */
static void decode_row(const a7800vid_line *ln, uint8_t out[2 * A7800VID_CELLS])
{
    if (!ln) {
        memset(out, 0, 2 * A7800VID_CELLS);
        return;
    }
    for (int i = 0; i < A7800VID_CELLS; i++) {
        int pair[2];
        cell_regs(ln, i, pair);
        out[2 * i] = (uint8_t)pair[0];
        out[2 * i + 1] = (uint8_t)pair[1];
    }
}

static uint32_t reg_rgb(a7800vid_tab *t, const a7800vid_line *ln, int reg)
{
    uint8_t byte = ln ? ln->palette[reg & 31] : 0;
    if (ln && (ln->ctrl & 0x80)) byte &= 0x0f;       /* colour kill */
    return console_rgb(t, byte);
}

/* the frame row where the registers shown in the tables are read: the row
 * the beam is on (stopped), else the last one drawn */
static const a7800vid_line *ref_line(const a7800vid_tab *t, int *row_out)
{
    const a7800vid_capture *c = t->cap;
    int start = t->has_live ? c->beam_row - 1 : t->fb_h - 1;
    start = clampi(start, 0, t->fb_h > 0 ? t->fb_h - 1 : 0);
    for (int y = start; y >= 0; y--)
        if (row_line(c, y)) { if (row_out) *row_out = y; return row_line(c, y); }
    for (int y = start + 1; y < t->fb_h; y++)
        if (row_line(c, y)) { if (row_out) *row_out = y; return row_line(c, y); }
    if (row_out) *row_out = -1;
    return &k_blank_line;
}

static int zone_start(const a7800vid_capture *c, int row)
{
    const a7800vid_line *ln = row_line(c, row), *prev = row_line(c, row - 1);
    if (!ln) return 0;
    if (!prev) return 1;
    return ln->dll != prev->dll || ln->offset > prev->offset;
}

/* ---- new / free / settings -------------------------------------------------------------- */

a7800vid_tab *a7800vid_tab_new(a7800session *s)
{
    a7800vid_tab *t = calloc(1, sizeof *t);
    const char *e;
    if (!t) return NULL;
    t->s = s;
    t->d = s ? a7800debug_get(s) : NULL;
    t->cap = calloc(1, sizeof *t->cap);
    t->last = calloc(1, sizeof *t->last);
    if (!t->cap || !t->last) {
        a7800vid_tab_free(t);
        return NULL;
    }
    t->view = A7800VID_V_DISPLAY;
    t->source = A7800VID_D_OBJECTS;
    t->ov = A7800VID_OV_BEAM | A7800VID_OV_ACTIVE;
    t->zoom = 0;
    t->fb_w = DEFAULT_W;
    t->fb_h = DEFAULT_H;
    t->vw = DEFAULT_W * 2;
    t->vh = DEFAULT_H * 2;
    t->hx = t->hy = t->px = t->py = -1;
    t->dirty = 1;

    if ((e = getenv("A7800_DEBUGGER_VIDEO_VIEW")) && *e) {
        int hit = 0;
        for (int i = 0; i < A7800VID_D_COUNT; i++)
            if (!strcasecmp(e, k_src_keys[i])) { t->view = A7800VID_V_DECODED; t->source = i; hit = 1; break; }
        for (int i = 0; !hit && i < A7800VID_V_COUNT; i++)
            if (!strncasecmp(e, k_view_keys[i], strlen(e))) { t->view = i; break; }
    }
    if ((e = getenv("A7800_DEBUGGER_VIDEO_OVERLAYS"))) {
        char buf[200], *tok, *rest = NULL;
        unsigned ov = 0;
        snprintf(buf, sizeof buf, "%s", e);
        for (tok = strtok_r(buf, ", ", &rest); tok; tok = strtok_r(NULL, ", ", &rest)) {
            if (!strcasecmp(tok, "all")) ov |= A7800VID_OV_ALL;
            else if (!strcasecmp(tok, "none")) ov = 0;
            else
                for (int i = 0; i < N_OVERLAYS; i++)
                    if (!strcasecmp(tok, k_overlays[i].name)) ov |= k_overlays[i].bit;
        }
        t->ov = ov;
    }
    if ((e = getenv("A7800_DEBUGGER_VIDEO_ZOOM")) && *e) {
        if (!strcasecmp(e, "fit")) t->zoom = 0;
        else {
            const int z = atoi(e);
            for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
                if (k_zooms[i] == z) t->zoom = z;
        }
    }
    t->env_section = A7800VID_S_PIXEL;
    if ((e = getenv("A7800_DEBUGGER_VIDEO_SECTION")) && *e) {
        for (int i = 0; i < A7800VID_S_COUNT; i++)
            if (!strncasecmp(e, k_sec_keys[i], strlen(e))) { t->env_section = i; break; }
    }
    if (s) {
        session_capture_enable(s, 1);
        t->capture_on = 1;
    }
    return t;
}

void a7800vid_tab_free(a7800vid_tab *t)
{
    if (!t) return;
    if (t->capture_on)
        session_capture_enable(t->s, 0);
    free(t->cap);
    free(t->last);
    free(t->base);
    free(t->pic);
    free(t);
}

void a7800vid_tab_set_view(a7800vid_tab *t, int v)
{
    if (t && v >= 0 && v < A7800VID_V_COUNT && v != t->view) { t->view = v; t->dirty = 1; }
}
int a7800vid_tab_view(const a7800vid_tab *t) { return t ? t->view : 0; }
void a7800vid_tab_set_source(a7800vid_tab *t, int s)
{
    if (t && s >= 0 && s < A7800VID_D_COUNT && s != t->source) { t->source = s; t->dirty = 1; }
}
int a7800vid_tab_source(const a7800vid_tab *t) { return t ? t->source : 0; }
void a7800vid_tab_set_overlays(a7800vid_tab *t, unsigned ov)
{
    if (t && (ov & A7800VID_OV_ALL) != t->ov) { t->ov = ov & A7800VID_OV_ALL; t->dirty = 1; }
}
unsigned a7800vid_tab_overlays(const a7800vid_tab *t) { return t ? t->ov : 0; }
void a7800vid_tab_set_zoom(a7800vid_tab *t, int z)
{
    if (t && z >= 0 && z <= 16 && z != t->zoom) { t->zoom = z; t->dirty = 1; }
}
int a7800vid_tab_zoom(const a7800vid_tab *t) { return t ? t->zoom : 0; }
void a7800vid_tab_set_viewport(a7800vid_tab *t, int w, int h)
{
    if (!t) return;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w != t->vw || h != t->vh) {
        t->vw = w; t->vh = h;
        if (t->zoom == 0) t->dirty = 1;
    }
}
int a7800vid_tab_env_section(const a7800vid_tab *t) { return t ? t->env_section : 0; }

/* ---- feeding ----------------------------------------------------------------------------- */

static void set_geometry(a7800vid_tab *t)
{
    const a7800vid_capture *c = t->cap;
    t->fb_w = c->fb_w > 0 ? c->fb_w : DEFAULT_W;
    t->fb_h = c->fb_h > 0 ? c->fb_h : (c->total_lines > 300 ? 260 : DEFAULT_H);
    if (t->fb_w > A7800VID_FB_MAXW) t->fb_w = A7800VID_FB_MAXW;
    if (t->fb_h > A7800VID_FB_MAXH) t->fb_h = A7800VID_FB_MAXH;
    if (t->fb_h != t->rgb_h) t->rgb_valid = 0;      /* another console: its colours */
}

void a7800vid_tab_feed(a7800vid_tab *t, const struct a7800vid_capture *cap, int live)
{
    if (!t || !cap) return;
    memcpy(t->cap, cap, sizeof *t->cap);
    t->fed = 1;
    t->have = 1;
    t->has_fb = cap->serial != 0 && cap->fb_w > 0 && cap->fb_h > 0;
    t->has_live = live != 0;
    t->stopped = live != 0;
    if (!live) t->cap->beam_row = t->cap->fb_h;
    set_geometry(t);
}

/* Take what the machine has. 0 if there is no machine. */
static int acquire(a7800vid_tab *t)
{
    int changed;

    if (t->fed)
        return t->have;
    t->have = 0;
    if (!t->s || !session_capture_running(t->s))
        return 0;
    changed = session_capture_get(t->s, t->last, &t->last_serial);
    t->stopped = t->d && a7800debug_is_stopped(t->d);
    t->has_fb = t->last->serial != 0;

    if (t->stopped || !t->has_fb) {
        /* a parked machine is ours to read; before the first capture the
         * frame in progress is all there is */
        if (!session_capture_fill(t->s, t->cap))
            return 0;
        t->has_live = t->stopped;
        t->cap_is_last = 0;
        if (!t->stopped)
            t->cap->beam_row = t->cap->fb_h;
        t->has_fb = t->has_fb || t->cap->fb_w > 0;
    } else {
        t->has_live = 0;
        if (changed || !t->cap_is_last)
            memcpy(t->cap, t->last, sizeof *t->cap);
        t->cap_is_last = 1;
    }
    t->have = 1;
    set_geometry(t);
    return 1;
}

static uint64_t signature(const a7800vid_tab *t)
{
    const a7800vid_capture *c = t->cap;
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, c->lines, sizeof c->lines);
    h = fnv(h, &c->serial, sizeof c->serial);
    h = fnv(h, &c->vpos, sizeof c->vpos);
    h = fnv(h, &c->hpos, sizeof c->hpos);
    h = fnv(h, &c->frame, sizeof c->frame);
    h = fnv(h, &c->beam_row, sizeof c->beam_row);
    h = fnv(h, &c->ctrl, sizeof c->ctrl);
    h = fnv(h, &t->has_live, sizeof t->has_live);
    h = fnv(h, &t->has_fb, sizeof t->has_fb);
    h = fnv(h, &t->fb_w, sizeof t->fb_w);
    h = fnv(h, &t->fb_h, sizeof t->fb_h);
    return h;
}

/* ---- the false colours of the Values view -------------------------------------------------- */

static uint32_t value_colour(int reg)
{
    const int pal = (reg >> 2) & 7, colour = reg & 3;
    const int i = (pal & 3) * 4 + colour;
    if (pal >= 4)       /* palettes 4-7: warm */
        return 0xff000000u | (uint32_t)(70 + i * 12) << 16 | (uint32_t)(40 + i * 8) << 8 | (uint32_t)(i * 3);
    return 0xff000000u | (uint32_t)(i * 4) << 16 | (uint32_t)(50 + i * 10) << 8 | (uint32_t)(70 + i * 12);
}

/* ---- the base picture (1x) -------------------------------------------------------------------- */

/* One row as MARIA showed it, redrawn from its line RAM: fb_w pixels. */
static void synth_row(a7800vid_tab *t, int row, uint32_t *dst, int values)
{
    const a7800vid_capture *c = t->cap;
    const a7800vid_line *ln = row_line(c, row);
    uint8_t reg[2 * A7800VID_CELLS];
    uint32_t rgb[32];

    decode_row(ln, reg);
    if (!values)
        for (int i = 0; i < 32; i++) rgb[i] = reg_rgb(t, ln, i);
    for (int x = 0; x < t->fb_w; x++) {
        const int sx = x + c->vis_left;
        const int r = sx >= 0 && sx < 2 * A7800VID_CELLS ? reg[sx] : 0;
        dst[x] = !ln ? 0xff000000u : values ? value_colour(r) : rgb[r];
    }
}

/* Decode every valid row for the uses of the palette registers; dst (frame
 * size) may be NULL. */
static void count_uses(a7800vid_tab *t)
{
    const a7800vid_capture *c = t->cap;
    uint8_t reg[2 * A7800VID_CELLS];
    memset(t->uses, 0, sizeof t->uses);
    for (int y = 0; y < t->fb_h; y++) {
        const a7800vid_line *ln = row_line(c, y);
        if (!ln) continue;
        decode_row(ln, reg);
        for (int x = 0; x < t->fb_w; x++) {
            const int sx = x + c->vis_left;
            t->uses[sx >= 0 && sx < 2 * A7800VID_CELLS ? reg[sx] : 0]++;
        }
    }
}

typedef struct { uint32_t *p; int w, h; } canvas;

static void px_blend(canvas *cv, int x, int y, uint32_t rgb, int a)
{
    if (x >= 0 && y >= 0 && x < cv->w && y < cv->h)
        cv->p[(size_t)y * (size_t)cv->w + (size_t)x] = blend(cv->p[(size_t)y * (size_t)cv->w + (size_t)x], rgb, a);
}
static void hline(canvas *cv, int y, int x0, int x1, uint32_t rgb, int a)
{
    for (int x = x0; x < x1; x++) px_blend(cv, x, y, rgb, a);
}
static void vline(canvas *cv, int x, int y0, int y1, uint32_t rgb, int a)
{
    for (int y = y0; y < y1; y++) px_blend(cv, x, y, rgb, a);
}
static void fill_rect(canvas *cv, int x, int y, int w, int h, uint32_t rgb, int a)
{
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) px_blend(cv, x + xx, y + yy, rgb, a);
}

/* an object's extent on its row in frame pixels, clipped */
static void obj_span(const a7800vid_tab *t, const a7800vid_line *ln, const a7800vid_object *o, int *x0, int *x1)
{
    int first, count;
    obj_cells(ln, o, &first, &count);
    *x0 = clampi(2 * first - t->cap->vis_left, 0, t->fb_w);
    *x1 = clampi(2 * (first + count) - t->cap->vis_left, 0, t->fb_w);
}

static const uint32_t k_zone_tints[] = { 0xff3060ff, 0xff30c060, 0xffe0a030, 0xffc040c0, 0xff30c0c0, 0xffe05050 };

static int build_other(a7800vid_tab *t)
{
    const a7800vid_line *ln = ref_line(t, NULL);
    const int w = 8 * SWATCH_PX, h = 4 * SWATCH_PX;
    if (!ensure(&t->base, &t->base_cap, (size_t)w * (size_t)h)) return 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const int reg = (x / SWATCH_PX) * 4 + y / SWATCH_PX;
            uint32_t rgb = reg_rgb(t, ln == &k_blank_line ? NULL : ln, reg);
            if (x % SWATCH_PX == 0 || y % SWATCH_PX == 0) rgb = blend(rgb, 0, 90);
            t->base[(size_t)y * (size_t)w + (size_t)x] = rgb;
        }
    t->bw = w; t->bh = h;
    t->kind = BK_OTHER;
    return 1;
}

static void dim_frame(a7800vid_tab *t)
{
    const size_t n = (size_t)t->fb_w * (size_t)t->fb_h;
    if (t->has_fb && t->cap->fb_w == t->fb_w && t->cap->fb_h == t->fb_h) {
        for (size_t i = 0; i < n; i++) t->base[i] = blend(t->cap->fb[i], 0, 150);
    } else {
        for (int y = 0; y < t->fb_h; y++) {
            synth_row(t, y, &t->base[(size_t)y * (size_t)t->fb_w], 0);
            for (int x = 0; x < t->fb_w; x++)
                t->base[(size_t)y * (size_t)t->fb_w + (size_t)x] = blend(t->base[(size_t)y * (size_t)t->fb_w + (size_t)x], 0, 150);
        }
    }
}

static int build_base(a7800vid_tab *t)
{
    const a7800vid_capture *c = t->cap;
    const int w = t->fb_w, h = t->fb_h;

    if (t->view == A7800VID_V_DECODED && t->source == A7800VID_D_PALETTE)
        return build_other(t);
    if (!ensure(&t->base, &t->base_cap, (size_t)w * (size_t)h)) return 0;
    t->bw = w; t->bh = h;
    t->kind = BK_FRAME;
    switch (t->view) {
    case A7800VID_V_DISPLAY:
        if (!t->has_fb) {
            why_set(t, "No frame has finished yet. Let the machine run for a moment.");
            return 0;
        }
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++) t->base[i] = c->fb[i] | 0xff000000u;
        return 1;
    case A7800VID_V_BEAM: {
        const int beam = c->beam_row;
        if (!t->has_live) {
            why_set(t, "The Beam view needs the machine stopped: it shows this frame as far as the beam has drawn it, and the last one dimmed below.");
            return 0;
        }
        for (int y = 0; y < h; y++) {
            uint32_t *row = &t->base[(size_t)y * (size_t)w];
            if (y < beam) {
                synth_row(t, y, row, 0);
            } else if (t->has_fb) {
                for (int x = 0; x < w; x++) row[x] = blend(c->fb[(size_t)y * (size_t)w + (size_t)x], 0, 150);
            } else {
                for (int x = 0; x < w; x++) row[x] = 0xff000000u;
            }
        }
        return 1;
    }
    case A7800VID_V_VALUES:
        for (int y = 0; y < h; y++) synth_row(t, y, &t->base[(size_t)y * (size_t)w], 1);
        return 1;
    default:
        break;
    }
    /* Decoded: Objects, Zones */
    dim_frame(t);
    {
        canvas cv = { t->base, w, h };
        int zone = 0;
        for (int y = 0; y < h; y++) {
            const a7800vid_line *ln = row_line(c, y);
            if (!ln) continue;
            if (zone_start(c, y)) zone++;
            if (t->source == A7800VID_D_ZONES) {
                hline(&cv, y, 0, w, k_zone_tints[(zone - 1) % 6], 90);
            } else {
                for (int i = 0; i < ln->nobj && i < A7800VID_OBJECTS; i++) {
                    int x0, x1;
                    obj_span(t, ln, &ln->obj[i], &x0, &x1);
                    if (x1 > x0) {
                        const uint32_t rgb = reg_rgb(t, ln, ln->obj[i].palette * 4 + 1);
                        for (int x = x0; x < x1; x++) t->base[(size_t)y * (size_t)w + (size_t)x] = rgb;
                    }
                }
            }
        }
    }
    return 1;
}

/* ---- overlays ---------------------------------------------------------------------------- */

static int same_object(const a7800vid_object *a, const a7800vid_object *b)
{
    return a->header == b->header && a->hpos == b->hpos && a->width == b->width;
}

static int has_object(const a7800vid_line *ln, const a7800vid_object *o)
{
    if (!ln) return 0;
    for (int i = 0; i < ln->nobj && i < A7800VID_OBJECTS; i++)
        if (same_object(&ln->obj[i], o)) return 1;
    return 0;
}

static void draw_overlays(a7800vid_tab *t, canvas *cv, int z)
{
    const a7800vid_capture *c = t->cap;
    int act0 = -1, act1 = -1;

    for (int y = 0; y < t->bh; y++) {
        const a7800vid_line *ln = row_line(c, y);
        if (!ln) continue;
        if (ln->cycles > 0) { if (act0 < 0) act0 = y; act1 = y; }
        if (t->ov & A7800VID_OV_HEAT) {
            if (ln->cycles >= DMA_BUDGET)
                fill_rect(cv, 0, y * z, t->bw * z, z, 0xffff3030u, 150);
            else if (ln->cycles > 0)
                fill_rect(cv, 0, y * z, ln->cycles * t->bw / DMA_LINE * z, z, 0xffff8a00u, 190);
        }
        if ((t->ov & A7800VID_OV_CHANGED) && y > 0) {
            const a7800vid_line *prev = row_line(c, y - 1);
            if (prev && (memcmp(prev->palette, ln->palette, sizeof ln->palette) || prev->ctrl != ln->ctrl ||
                         prev->charbase != ln->charbase || prev->dpp != ln->dpp))
                hline(cv, y * z, 0, t->bw * z, 0xff00e0ffu, 230);
        }
        if ((t->ov & A7800VID_OV_ZONES) && zone_start(c, y))
            hline(cv, y * z, 0, t->bw * z, 0xffffffffu, 200);
        if ((t->ov & A7800VID_OV_DLI) && ln->dli && ln->offset == 0)
            hline(cv, (y + 1) * z - 1, 0, t->bw * z, 0xffff5ad2u, 230);
        if (t->ov & A7800VID_OV_OBJECTS) {
            const a7800vid_line *above = row_line(c, y - 1), *below = row_line(c, y + 1);
            for (int i = 0; i < ln->nobj && i < A7800VID_OBJECTS; i++) {
                int x0, x1;
                const uint32_t rgb = 0xff00ff80u + ((uint32_t)(i * 41 & 0x7f) << 16);
                obj_span(t, ln, &ln->obj[i], &x0, &x1);
                if (x1 <= x0) continue;
                vline(cv, x0 * z, y * z, (y + 1) * z, rgb, 230);
                vline(cv, x1 * z - 1, y * z, (y + 1) * z, rgb, 230);
                if (!has_object(above, &ln->obj[i])) hline(cv, y * z, x0 * z, x1 * z, rgb, 230);
                if (!has_object(below, &ln->obj[i])) hline(cv, (y + 1) * z - 1, x0 * z, x1 * z, rgb, 230);
            }
        }
    }
    if ((t->ov & A7800VID_OV_BEAM) && t->has_live) {
        const int y = c->beam_row;
        if (y >= 0 && y < t->bh) hline(cv, y * z, 0, t->bw * z, 0xffff4040u, 230);
    }
    if ((t->ov & A7800VID_OV_ACTIVE) && act0 >= 0 && (t->view == A7800VID_V_DISPLAY || t->view == A7800VID_V_BEAM)) {
        const int y0 = act0 * z, y1 = (act1 + 1) * z;
        hline(cv, y0, 0, t->bw * z, 0xffffffffu, 160);
        hline(cv, y1 - 1, 0, t->bw * z, 0xffffffffu, 160);
        vline(cv, 0, y0, y1, 0xffffffffu, 160);
        vline(cv, t->bw * z - 1, y0, y1, 0xffffffffu, 160);
    }
}

static int pick_zoom(const a7800vid_tab *t)
{
    int z = t->zoom;
    if (z == 0) {
        z = 1;
        for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
            if (t->bw * k_zooms[i] <= t->vw && t->bh * k_zooms[i] <= t->vh)
                z = k_zooms[i];
    }
    while (z > 1 && !fits_zoom(t->bw, t->bh, z)) {
        int lower = 1;
        for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
            if (k_zooms[i] < z) lower = k_zooms[i];
        z = lower;
    }
    return z;
}

static void build_picture(a7800vid_tab *t)
{
    int z;
    canvas cv;

    t->why[0] = '\0';
    t->pw = t->ph = 0;
    if (!build_base(t))
        return;
    z = pick_zoom(t);
    t->ez = z;
    t->pw = t->bw * z;
    t->ph = t->bh * z;
    if (!ensure(&t->pic, &t->pic_cap, (size_t)t->pw * (size_t)t->ph)) {
        t->pw = t->ph = 0;
        why_set(t, "Out of memory.");
        return;
    }
    if (z == 1)
        memcpy(t->pic, t->base, (size_t)t->bw * (size_t)t->bh * sizeof *t->pic);
    else
        for (int y = 0; y < t->ph; y++) {
            const uint32_t *src = &t->base[(size_t)(y / z) * (size_t)t->bw];
            uint32_t *dst = &t->pic[(size_t)y * (size_t)t->pw];
            for (int x = 0; x < t->pw; x++)
                dst[x] = src[x / z];
        }
    cv.p = t->pic; cv.w = t->pw; cv.h = t->ph;
    if (t->kind == BK_FRAME)
        draw_overlays(t, &cv, z);
}

/* ---- the inspector ----------------------------------------------------------------------- */

static void reg_name(int reg, char *dst, size_t size)
{
    const int pal = (reg >> 2) & 7;
    if (reg == 0) snprintf(dst, size, "BACKGRND");
    else snprintf(dst, size, "P%dC%d", pal, reg & 3);
}

static const char *holey_text(int holey)
{
    static const char *const names[] = { "none", "$8800 (16-high)", "$9000 (8-high)", "$8800 and $9000" };
    return names[holey & 3];
}

static const char *mode_text(int rm)
{
    return (rm & 3) == 2 ? "320B/D" : (rm & 3) == 3 ? "320A/C" : "160A/B";
}

/* append to an inspector text, never past its end; the new length */
static int add_text(char *text, size_t size, int n, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int add_text(char *text, size_t size, int n, const char *fmt, ...)
{
    va_list ap;
    int added;
    if ((size_t)n >= size - 1) return n;
    va_start(ap, fmt);
    added = vsnprintf(text + n, size - (size_t)n, fmt, ap);
    va_end(ap);
    return added < 0 ? n : (size_t)(n + added) >= size ? (int)size - 1 : n + added;
}

static void locate_other(a7800vid_tab *t, int px, int py, a7800vid_pixel *o)
{
    const a7800vid_line *ln = ref_line(t, NULL);
    char name[12];
    const int reg = clampi(px / SWATCH_PX, 0, 7) * 4 + clampi(py / SWATCH_PX, 0, 3);

    if (ln == &k_blank_line) ln = NULL;
    o->active = 1;
    o->reg_index = reg;
    o->palette = reg >> 2;
    o->colour = reg & 3;
    o->reg_value = ln ? ln->palette[reg] : 0;
    o->rgb = reg_rgb(t, ln, reg);
    reg_name(reg, name, sizeof name);
    snprintf(o->text, sizeof o->text, "Palette register %s ($%02X)\nValue $%02X   #%06X\nUsed by %ld screen pixels",
             name, 0x20 + reg, o->reg_value, o->rgb & 0xffffff, t->uses[reg]);
}

static void locate(a7800vid_tab *t, int x, int y, a7800vid_pixel *o)
{
    const a7800vid_capture *c = t->cap;
    const int z = t->ez ? t->ez : 1;
    const a7800vid_line *ln;
    int px, py, sx, n;
    uint8_t reg[2 * A7800VID_CELLS];

    memset(o, 0, sizeof *o);
    o->row = -1; o->cell = -1; o->reg_index = -1;
    if (!t->pw || x < 0 || y < 0 || x >= t->pw || y >= t->ph)
        return;
    px = x / z; py = y / z;
    o->valid = 1;
    o->x = px; o->y = py;
    if (t->kind == BK_OTHER) {
        locate_other(t, px, py, o);
        return;
    }
    o->row = py;
    o->scanline = py + c->vis_top;
    o->rgb = t->base ? t->base[(size_t)py * (size_t)t->bw + (size_t)px] : 0xff000000u;
    ln = row_line(c, py);
    n = snprintf(o->text, sizeof o->text, "Pixel %d,%d   row %d, scanline %d\n", px, py, o->row, o->scanline);
    if (!ln) {
        add_text(o->text, sizeof o->text, n,
                 "MARIA drew nothing here (no row record: blanking, or the machine has not got this far)\nColour #%06X",
                 o->rgb & 0xffffff);
        return;
    }
    sx = px + c->vis_left;
    o->active = 1;
    o->read_mode = ln->ctrl & 3;
    o->dll = ln->dll;
    o->dl = ln->dl;
    o->offset = ln->offset;
    o->holey = ln->holey;
    o->dli = ln->dli;
    if (sx < 0 || sx >= 2 * A7800VID_CELLS) {
        add_text(o->text, sizeof o->text, n, "Outside the 160 cells (screen x %d)\nColour #%06X", sx, o->rgb & 0xffffff);
        return;
    }
    o->cell = sx / 2;
    o->half = sx & 1;
    o->cell_value = ln->cells[o->cell];
    decode_row(ln, reg);
    o->reg_index = reg[sx];
    o->palette = o->reg_index >> 2;
    o->colour = o->reg_index & 3;
    o->reg_value = ln->palette[o->reg_index];
    n = add_text(o->text, sizeof o->text, n,
                  "Cell %d of 160 (screen x %d), read mode %s\n"
                  "Zone: DLL entry $%04X, offset %d, holes %s%s\n"
                  "Display list at $%04X, %d object%s this row\n",
                  o->cell, sx, mode_text(o->read_mode), o->dll, o->offset, holey_text(o->holey),
                  o->dli ? ", DLI" : "", o->dl, ln->nobj, ln->nobj == 1 ? "" : "s");
    if (t->d && o->dll >= 0x1800) {
        uint8_t entry[3];
        if (a7800debug_read(t->d, (uint16_t)o->dll, entry, 3) == 3)
            n = add_text(o->text, sizeof o->text, n, "DLL entry bytes %02X %02X %02X\n", entry[0], entry[1], entry[2]);
    }
    for (int i = ln->nobj < A7800VID_OBJECTS ? ln->nobj - 1 : A7800VID_OBJECTS - 1; i >= 0; i--) {
        const a7800vid_object *ob = &ln->obj[i];
        int first, count;
        obj_cells(ln, ob, &first, &count);
        if (o->cell < first || o->cell >= first + count) continue;
        if (o->nobjects < A7800VID_PIXEL_OBJECTS) {
            a7800vid_pixel_object *po = &o->obj[o->nobjects];
            po->header = ob->header; po->graphics = ob->graphics;
            po->hpos = ob->hpos; po->width = ob->width; po->palette = ob->palette; po->flags = ob->flags;
            po->first_cell = first; po->cells = count;
            po->top = o->nobjects == 0;
        }
        o->nobjects++;
    }
    for (int i = 0; i < o->nobjects && i < A7800VID_PIXEL_OBJECTS; i++) {
        const a7800vid_pixel_object *po = &o->obj[i];
        char bytes[40] = "";
        if (t->d) {
            uint8_t hb[5];
            const int len = (po->flags & A7800VID_OBJ_EXT) ? 5 : 4;
            if (a7800debug_read(t->d, po->header, hb, len) == len) {
                int k = 0;
                for (int j = 0; j < len; j++) k += snprintf(bytes + k, sizeof bytes - (size_t)k, "%s%02X", j ? " " : " [", hb[j]);
                snprintf(bytes + k, sizeof bytes - (size_t)k, "]");
            }
        }
        n = add_text(o->text, sizeof o->text, n,
                      "%s header $%04X%s graphics $%04X, cells %d-%d, %d byte%s, palette %d, %s%s%s\n",
                      i == 0 ? "Object (topmost)" : "Object", po->header, bytes, po->graphics,
                      po->first_cell, po->first_cell + po->cells - 1, po->width, po->width == 1 ? "" : "s", po->palette,
                      (po->flags & A7800VID_OBJ_INDIRECT) ? "indirect" : "direct",
                      (po->flags & A7800VID_OBJ_WRITE) ? ", write mode" : "",
                      (po->flags & A7800VID_OBJ_EXT) ? ", extended header" : "");
    }
    if (o->nobjects == 0)
        n = add_text(o->text, sizeof o->text, n, "No object covers this cell (background)\n");
    {
        char name[12];
        reg_name(o->reg_index, name, sizeof name);
        add_text(o->text, sizeof o->text, n,
                 "Cell value $%02X: palette %d, colour %d -> %s ($%02X) = $%02X   #%06X",
                 o->cell_value, o->palette, o->colour, name, 0x20 + o->reg_index, o->reg_value,
                 reg_rgb(t, ln, o->reg_index) & 0xffffff);
    }
}

int a7800vid_tab_hover(a7800vid_tab *t, int x, int y)
{
    char old[sizeof t->hover.text];
    if (!t) return 0;
    snprintf(old, sizeof old, "%s", t->hover.valid ? t->hover.text : "");
    t->hx = x; t->hy = y;
    locate(t, x, y, &t->hover);
    return strcmp(old, t->hover.valid ? t->hover.text : "") != 0;
}

void a7800vid_tab_pin(a7800vid_tab *t, int x, int y)
{
    int z;
    if (!t) return;
    z = t->ez ? t->ez : 1;
    if (t->px >= 0 && x / z == t->px / z && y / z == t->py / z) {
        a7800vid_tab_unpin(t);
        return;
    }
    t->px = x; t->py = y;
    locate(t, x, y, &t->pin);
    if (!t->pin.valid) a7800vid_tab_unpin(t);
}

void a7800vid_tab_unpin(a7800vid_tab *t)
{
    if (!t) return;
    t->px = t->py = -1;
    memset(&t->pin, 0, sizeof t->pin);
}

const a7800vid_pixel *a7800vid_tab_pixel(const a7800vid_tab *t)
{
    if (!t) return NULL;
    if (t->pin.valid) return &t->pin;
    if (t->hover.valid) return &t->hover;
    return NULL;
}

int a7800vid_tab_marks(const a7800vid_tab *t, int hover[4], int pin[4])
{
    int r = 0, z;
    if (!t) return 0;
    z = t->ez ? t->ez : 1;
    if (t->hover.valid) {
        const int cell = t->kind == BK_OTHER ? SWATCH_PX : 1;
        hover[0] = t->kind == BK_OTHER ? t->hover.x / cell * cell * z : t->hover.x * z;
        hover[1] = t->kind == BK_OTHER ? t->hover.y / cell * cell * z : t->hover.y * z;
        hover[2] = cell * z; hover[3] = cell * z;
        r |= 1;
    }
    if (t->pin.valid) {
        const int cell = t->kind == BK_OTHER ? SWATCH_PX : 1;
        pin[0] = t->kind == BK_OTHER ? t->pin.x / cell * cell * z : t->pin.x * z;
        pin[1] = t->kind == BK_OTHER ? t->pin.y / cell * cell * z : t->pin.y * z;
        pin[2] = cell * z; pin[3] = cell * z;
        r |= 2;
    }
    return r;
}

/* a write watchpoint on [lo, hi], unless one is there; the id or -1 */
static int add_write_bp(a7800debug *d, unsigned lo, unsigned hi)
{
    a7800debug_breakpoint bl[128];
    const int n = a7800debug_breakpoint_list(d, bl, 128);
    for (int i = 0; i < n; i++)
        if ((bl[i].type & A7800DEBUG_BP_WRITE) && bl[i].start == lo && bl[i].end == hi)
            return bl[i].id;
    return a7800debug_breakpoint_add(d, A7800DEBUG_BP_WRITE, (uint16_t)lo, (uint16_t)hi, NULL);
}

/* the CPU address of a palette register: every fourth one is BACKGRND's mirror */
static unsigned reg_address(int reg)
{
    return 0x20u + (unsigned)((reg & 3) ? reg : 0);
}

int a7800vid_tab_break_on_pixel(a7800vid_tab *t)
{
    const a7800vid_pixel *p = a7800vid_tab_pixel(t);
    int first = -1;

    if (!p || !t->d || !p->active || p->reg_index < 0) return -1;
    if (t->kind == BK_OTHER) return add_write_bp(t->d, reg_address(p->reg_index), reg_address(p->reg_index));
    for (int i = 0; i < p->nobjects && i < A7800VID_PIXEL_OBJECTS; i++) {
        const int id = add_write_bp(t->d, p->obj[i].header, p->obj[i].header + ((p->obj[i].flags & A7800VID_OBJ_EXT) ? 4u : 3u));
        if (first < 0) first = id;
    }
    {
        const int id = add_write_bp(t->d, p->dll, p->dll + 2u);
        if (first < 0) first = id;
    }
    {
        const int id = add_write_bp(t->d, reg_address(p->reg_index), reg_address(p->reg_index));
        if (first < 0) first = id;
    }
    return first;
}

int a7800vid_tab_pixel_register(const a7800vid_tab *t)
{
    const a7800vid_pixel *p = a7800vid_tab_pixel(t);
    return p && p->valid && p->active && p->reg_index >= 0 ? p->reg_index : -1;
}

int a7800vid_tab_break_on_swatch(a7800vid_tab *t, int index)
{
    if (!t || !t->d || index < 0 || index > 31) return -1;
    return add_write_bp(t->d, reg_address(index), reg_address(index));
}

/* ---- the tables ---------------------------------------------------------------------------- */

static void reg_set(a7800vid_tab *t, int *n, const char *name, const char *value,
                    const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void reg_set(a7800vid_tab *t, int *n, const char *name, const char *value,
                    const char *fmt, ...)
{
    a7800vid_reg *r;
    va_list ap;
    int prev_age;
    if (*n >= MAX_REGS) return;
    r = &t->regs[*n];
    prev_age = r->age;
    r->name = name;
    snprintf(r->value, sizeof r->value, "%s", value);
    va_start(ap, fmt);
    vsnprintf(r->meaning, sizeof r->meaning, fmt, ap);
    va_end(ap);
    if (!t->regs_primed || strcmp(t->prev_val[*n], r->value) != 0)
        r->age = t->regs_primed ? 0 : 99;
    else
        r->age = prev_age < 99 ? prev_age + 1 : 99;
    memcpy(t->prev_val[*n], r->value, sizeof t->prev_val[*n]);
    (*n)++;
}

static void build_tables(a7800vid_tab *t)
{
    const a7800vid_capture *c = t->cap;
    const a7800vid_line *ref = ref_line(t, NULL);
    const a7800vid_line *ln = ref == &k_blank_line ? NULL : ref;
    int n = 0;
    char v[24];

    /* registers (ages carry over from the previous build) */
    snprintf(v, sizeof v, "%02X", ref->ctrl);
    reg_set(t, &n, "CTRL", v, "%s%s, %s, %d-byte characters%s, %s",
            (ref->ctrl & 0x80) ? "colour kill, " : "",
            ((ref->ctrl >> 5) & 3) == 2 ? "DMA on" : ((ref->ctrl >> 5) & 3) == 3 ? "DMA off" : "DMA test mode",
            (ref->ctrl & 0x08) ? "border background" : "border black", (ref->ctrl & 0x10) ? 2 : 1,
            (ref->ctrl & 0x04) ? ", kangaroo" : "", mode_text(ref->ctrl & 3));
    snprintf(v, sizeof v, "%02X", ref->charbase);
    reg_set(t, &n, "CHARBASE", v, "characters at $%04X + offset x $100", ref->charbase << 8);
    snprintf(v, sizeof v, "$%04X", ref->dpp);
    reg_set(t, &n, "DPP", v, "display list list at $%04X", ref->dpp);
    snprintf(v, sizeof v, "$%04X", ref->dll);
    reg_set(t, &n, "DLL", v, "the zone's DLL entry");
    snprintf(v, sizeof v, "$%04X", ref->dl);
    reg_set(t, &n, "DL", v, "the display list the row read");
    snprintf(v, sizeof v, "%d", ref->offset);
    reg_set(t, &n, "OFFSET", v, "line within the zone (the last is 0)");
    snprintf(v, sizeof v, "%d", ref->holey);
    reg_set(t, &n, "HOLEY", v, "holey DMA: %s", holey_text(ref->holey));
    snprintf(v, sizeof v, "%d", ref->dli);
    reg_set(t, &n, "DLI", v, "%s", ref->dli ? "an interrupt at the zone's last line" : "none in this zone");
    snprintf(v, sizeof v, "%02X", ref->palette[0]);
    reg_set(t, &n, "BACKGRND", v, "#%06X", reg_rgb(t, ln, 0) & 0xffffff);
    snprintf(v, sizeof v, "%d", ref->nobj);
    reg_set(t, &n, "OBJECTS", v, "display list objects DMA read for the row");
    snprintf(v, sizeof v, "%d", ref->cycles);
    reg_set(t, &n, "DMA", v, "cycles of %d (%d a scanline)", DMA_BUDGET, DMA_LINE);
    t->nregs = n;
    t->regs_primed = 1;

    /* colours */
    count_uses(t);
    {
        const a7800vid_pixel *p = a7800vid_tab_pixel(t);
        for (int i = 0; i < 32; i++) {
            t->sw[i].index = i;
            reg_name(i, t->sw[i].name, sizeof t->sw[i].name);
            t->sw[i].address = 0x20u + (unsigned)i;
            t->sw[i].value = ln ? ln->palette[i] : 0;
            t->sw[i].rgb = reg_rgb(t, ln, i);
            t->sw[i].uses = t->uses[i];
            t->sw[i].inspected = p && p->valid && p->active && p->reg_index == i;
        }
    }

    /* rows: where the zone, palette, CTRL or DPP change */
    t->nrows = t->nzones = t->nchanges = t->ndli = t->nover = t->peak = 0;
    t->peak_row = -1;
    for (int y = 0; y < t->fb_h; y++) {
        const a7800vid_line *R = row_line(c, y), *P = row_line(c, y - 1);
        int mask = 0;
        if (!R) continue;
        if (R->cycles > t->peak) { t->peak = R->cycles; t->peak_row = y; }
        if (R->cycles >= DMA_BUDGET) t->nover++;
        if (R->dli && R->offset == 0) t->ndli++;
        if (zone_start(c, y)) { mask |= A7800VID_LC_ZONE; t->nzones++; }
        if (P) {
            if (memcmp(R->palette, P->palette, sizeof R->palette)) mask |= A7800VID_LC_PALETTE;
            if (R->ctrl != P->ctrl || R->charbase != P->charbase) mask |= A7800VID_LC_CTRL;
            if (R->dpp != P->dpp) mask |= A7800VID_LC_DPP;
        }
        if (mask & (A7800VID_LC_PALETTE | A7800VID_LC_CTRL | A7800VID_LC_DPP)) t->nchanges++;
        if (t->nrows == 0 || mask) {
            a7800vid_line_row *r = &t->rows[t->nrows++];
            r->row = y;
            r->scanline = y + c->vis_top;
            r->ctrl = R->ctrl; r->charbase = R->charbase;
            r->dpp = R->dpp; r->dll = R->dll; r->dl = R->dl;
            r->offset = R->offset; r->holey = R->holey; r->dli = R->dli;
            r->objects = R->nobj; r->cycles = R->cycles;
            r->background = R->palette[0];
            r->changed_mask = mask;
        }
    }

    /* timing */
    t->ntiming = 0;
#define TL(...) do { if (t->ntiming < MAX_TIMING) { \
        snprintf(t->timing_buf[t->ntiming], sizeof t->timing_buf[0], __VA_ARGS__); \
        t->timing_ptr[t->ntiming] = t->timing_buf[t->ntiming]; t->ntiming++; } } while (0)
    TL("Beam\tscanline %d of %d, dot %d of %d", c->vpos, c->total_lines, c->hpos, DMA_LINE);
    TL("Beam position\t%d.%d%% of the frame", a7800vid_tab_beam_permille(t) / 10, a7800vid_tab_beam_permille(t) % 10);
    TL("VBLANK\t%s",
       t->has_live ? (c->vpos < c->vis_top || c->vpos >= c->vis_top + t->fb_h ? "yes, the beam is above or below the picture"
                                                                              : "no, the beam is drawing the picture")
                   : "not while running");
    TL("Frame\t%llu", (unsigned long long)c->frame);
    TL("Standard\t%s, %d lines", c->total_lines > 300 ? "PAL" : "NTSC", c->total_lines);
    TL("Picture\t%d x %d, scanlines %d-%d", t->fb_w, t->fb_h, c->vis_top, c->vis_top + t->fb_h - 1);
    TL("DMA this row\t%d of %d cycles (%d a scanline)", ref->cycles, DMA_BUDGET, DMA_LINE);
    TL("DMA peak\t%d cycles%s%d", t->peak, t->peak_row >= 0 ? " at row " : "", t->peak_row >= 0 ? t->peak_row : 0);
    TL("DMA over budget\t%d row%s", t->nover, t->nover == 1 ? "" : "s");
    TL("Zones\t%d, %d with a display list interrupt", t->nzones, t->ndli);
    TL("Changes this frame\t%d row%s", t->nchanges, t->nchanges == 1 ? "" : "s");
    TL("Machine\t%s", t->stopped ? "stopped" : "running");
#undef TL

    snprintf(t->status, sizeof t->status,
             "%s view%s, %d zone%s, %d change%s in the frame\n%s%s",
             k_view_names[t->view],
             t->view == A7800VID_V_DECODED ? (t->source == A7800VID_D_OBJECTS ? " (objects)" :
                                              t->source == A7800VID_D_ZONES ? " (zones)" : " (palette)") : "",
             t->nzones, t->nzones == 1 ? "" : "s", t->nchanges, t->nchanges == 1 ? "" : "s",
             t->stopped ? "Stopped" : "Running: showing the last finished frame",
             t->stopped ? (t->has_fb ? "" : " (no frame has finished yet)") : "");
}

/* ---- refresh -------------------------------------------------------------------------------- */

unsigned a7800vid_tab_refresh(a7800vid_tab *t, int force)
{
    uint64_t sig;
    unsigned r = 0;
    int fresh;

    if (!t) return 0;
    if (!acquire(t)) {
        why_set(t, "The machine is not running.");
        t->pw = t->ph = 0;
        return A7800VID_TAB_OFF | A7800VID_TAB_PICTURE | A7800VID_TAB_TABLES;
    }
    sig = signature(t);
    fresh = force || sig != t->sig;
    if (fresh)
        t->sig = sig;
    if (fresh || t->dirty) {
        build_picture(t);
        t->dirty = 0;
        r |= A7800VID_TAB_PICTURE;
    }
    if (fresh) {
        if (t->hx >= 0) locate(t, t->hx, t->hy, &t->hover);
        if (t->px >= 0) locate(t, t->px, t->py, &t->pin);
        build_tables(t);
        r |= A7800VID_TAB_TABLES;
    }
    return r;
}

const uint32_t *a7800vid_tab_picture(a7800vid_tab *t, int *w, int *h)
{
    if (!t || !t->pw) return NULL;
    if (w) *w = t->pw;
    if (h) *h = t->ph;
    return t->pic;
}
const char *a7800vid_tab_why(const a7800vid_tab *t) { return t ? t->why : ""; }
int a7800vid_tab_effective_zoom(const a7800vid_tab *t) { return t ? t->ez : 1; }
const char *a7800vid_tab_status(const a7800vid_tab *t) { return t ? t->status : ""; }
int a7800vid_tab_regs(const a7800vid_tab *t, const a7800vid_reg **out)
{
    if (out) *out = t->regs;
    return t->nregs;
}
int a7800vid_tab_swatches(const a7800vid_tab *t, const a7800vid_swatch **out)
{
    /* the inspected pixel moves between refreshes */
    const a7800vid_pixel *p = a7800vid_tab_pixel(t);
    a7800vid_swatch *sw = ((a7800vid_tab *)t)->sw;
    for (int i = 0; i < 32; i++)
        sw[i].inspected = p && p->valid && p->active && p->reg_index == i;
    if (out) *out = t->sw;
    return t->have ? 32 : 0;
}
int a7800vid_tab_lines(const a7800vid_tab *t, const a7800vid_line_row **out)
{
    if (out) *out = t->rows;
    return t->nrows;
}
int a7800vid_tab_timing(const a7800vid_tab *t, const char *const **lines)
{
    if (lines) *lines = (const char *const *)t->timing_ptr;
    return t->ntiming;
}
int a7800vid_tab_beam_permille(const a7800vid_tab *t)
{
    return t && t->have && t->cap->total_lines > 0 ? t->cap->vpos * 1000 / t->cap->total_lines : 0;
}

int a7800vid_tab_text(a7800vid_tab *t, int section, char *dst, int dstsz)
{
    int n = 0;
#define P(...) do { if (n < dstsz) n += snprintf(dst + n, (size_t)(dstsz - n), __VA_ARGS__); } while (0)
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!t || !t->have) { P("The machine is not running.\n"); return n < dstsz ? n : dstsz - 1; }
    switch (section) {
    case A7800VID_S_REGISTERS:
        for (int i = 0; i < t->nregs; i++)
            P("%-9s %-6s %s\n", t->regs[i].name, t->regs[i].value, t->regs[i].meaning);
        break;
    case A7800VID_S_COLOURS:
        for (int i = 0; i < 32; i++)
            P("%-9s $%02X = $%02X  #%06X  %ld pixels\n", t->sw[i].name, t->sw[i].address, t->sw[i].value,
              t->sw[i].rgb & 0xffffff, t->sw[i].uses);
        break;
    case A7800VID_S_LINES:
        for (int i = 0; i < t->nrows; i++) {
            const a7800vid_line_row *r = &t->rows[i];
            P("row %3d scanline %3d  zone $%04X off %2d  DPP $%04X  DL $%04X  CTRL %02X CB %02X BG %02X  objs %2d  dma %3d  %s%s%s%s%s\n",
              r->row, r->scanline, r->dll, r->offset, r->dpp, r->dl, r->ctrl, r->charbase, r->background,
              r->objects, r->cycles, r->dli ? "dli " : "",
              (r->changed_mask & A7800VID_LC_ZONE) ? "zone " : "", (r->changed_mask & A7800VID_LC_PALETTE) ? "palette " : "",
              (r->changed_mask & A7800VID_LC_CTRL) ? "ctrl " : "", (r->changed_mask & A7800VID_LC_DPP) ? "dpp" : "");
        }
        break;
    case A7800VID_S_TIMING:
        for (int i = 0; i < t->ntiming; i++) P("%s\n", t->timing_buf[i]);
        break;
    default:
        P("%s\n", t->status);
        if (a7800vid_tab_pixel(t)) P("%s\n", a7800vid_tab_pixel(t)->text);
        break;
    }
#undef P
    return n < dstsz ? n : dstsz - 1;
}
