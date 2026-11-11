/*
 * a7800vidtab -- the Video tab's controller. All four frontends draw the same
 * tab from it, so what they own is widgets: a view and zoom chooser, overlay
 * toggles, a picture, the inspector and four tables. The lessons of the
 * other debuggers' video tabs live here once:
 *
 *   - the machine is only COPIED: a finished frame (picture, MARIA's per-row
 *     record, the beam) is captured once per frame on the emulation thread,
 *     and a stopped machine is read while it is parked. Decoding, scaling and
 *     overlays run on the caller's (UI) thread;
 *   - the picture is rebuilt only when something it shows changed, into one
 *     reused buffer;
 *   - hover and pin are state here, so a mouse move never re-renders the
 *     picture: draw the marks from a7800vid_tab_marks() over the cached image;
 *   - pixels are 0xFFRRGGBB (opaque), whatever the toolkit does with alpha;
 *   - one table of overlays and one env-var parser, so the frontends cannot
 *     disagree.
 *
 * MARIA has no frame buffer: every scanline it DMAs the display list's
 * objects into a 160-cell line RAM and shows it on the next line (a cell is
 * two screen pixels in every read mode). The host records, per row, the
 * zone (DLL entry), the objects it read, the line RAM and the registers it
 * was drawn with, so the Display view is the frame as the TV showed it,
 * Decoded draws each display list object's extent over it (or the zone
 * bands, or the 32 palette registers), Beam redraws this frame from the line
 * RAM above the beam and dims the last frame below it, and Values shows the
 * raw palette and colour numbers (cool = palettes 0-3, warm = 4-7).
 *
 * UI thread only. Not thread-safe.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A7800VIDTAB_H
#define A7800VIDTAB_H

#include <stdint.h>

#include "a7800debug.h"
#include "a7800session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct a7800vid_tab a7800vid_tab;
struct a7800vid_capture;      /* a7800vidcap.h; tests feed one */

enum a7800vid_view {
    A7800VID_V_DISPLAY = 0,     /* the last finished frame, as the TV showed it */
    A7800VID_V_DECODED,         /* the display list through MARIA's records (see a7800vid_source) */
    A7800VID_V_BEAM,            /* this frame above the beam, the last one dimmed below (stopped) */
    A7800VID_V_VALUES,          /* raw palette/colour numbers; cool = palettes 0-3, warm = 4-7 */
    A7800VID_V_COUNT
};

/* What Decoded shows. */
enum a7800vid_source {
    A7800VID_D_OBJECTS = 0,     /* the dimmed frame, each object's extent filled in its palette colour */
    A7800VID_D_ZONES,           /* the dimmed frame, one band per display list list entry */
    A7800VID_D_PALETTE,         /* the 32 MARIA palette registers as a grid */
    A7800VID_D_COUNT
};

/* Overlays: one table, bit per entry. */
enum {
    A7800VID_OV_ZONES   = 1u << 0,  /* a line at the first row of every zone (DLL entry) */
    A7800VID_OV_DLI     = 1u << 1,  /* the rows a display list interrupt fires on */
    A7800VID_OV_OBJECTS = 1u << 2,  /* a box on every display list object */
    A7800VID_OV_BEAM    = 1u << 3,  /* where the beam is (stopped) */
    A7800VID_OV_HEAT    = 1u << 4,  /* DMA cycles each row used of the 426 it has (red: over budget) */
    A7800VID_OV_CHANGED = 1u << 5,  /* rows where the palette, CTRL or DPP differ from the row above */
    A7800VID_OV_ACTIVE  = 1u << 6,  /* outline of the rows MARIA's DMA runs on (Display, Beam) */
    A7800VID_OV_ALL     = 0x7Fu
};
typedef struct {
    unsigned bit;
    const char *name;           /* the env name: zones, dli, ... */
    const char *label;          /* the checkbox text */
} a7800vid_overlay_info;
const a7800vid_overlay_info *a7800vid_overlays(int *count);
const char *a7800vid_view_name(int view);            /* "Display", ... */
const char *a7800vid_view_key(int view);             /* "display", ... (env) */
const char *a7800vid_source_name(int source);        /* "Objects", "Zones", "Palette" */
const char *a7800vid_source_key(int source);         /* "objects", "zones", "palette" (env) */

/* Right-hand sections. */
enum { A7800VID_S_PIXEL = 0, A7800VID_S_REGISTERS, A7800VID_S_COLOURS,
       A7800VID_S_LINES, A7800VID_S_TIMING, A7800VID_S_COUNT };
const char *a7800vid_section_name(int section);
const char *a7800vid_section_key(int section);

/* s is the session the tab captures from and asks whether the machine is
 * stopped and adds watchpoints to (through its debugger); NULL (tests) makes
 * every Break on... return -1 and the machine count as running. The tab asks
 * the host to capture frames until it is freed. */
a7800vid_tab *a7800vid_tab_new(a7800session *s);
void          a7800vid_tab_free(a7800vid_tab *t);

/* ---- what to show (they mark the picture stale) ------------------------------------ */
void     a7800vid_tab_set_view(a7800vid_tab *t, int view);
int      a7800vid_tab_view(const a7800vid_tab *t);
void     a7800vid_tab_set_source(a7800vid_tab *t, int source);   /* of Decoded */
int      a7800vid_tab_source(const a7800vid_tab *t);
void     a7800vid_tab_set_overlays(a7800vid_tab *t, unsigned ov);
unsigned a7800vid_tab_overlays(const a7800vid_tab *t);
/* 1, 2, 3, 4, 6, 8, 12, 16, or 0 for "fit the viewport" */
void     a7800vid_tab_set_zoom(a7800vid_tab *t, int zoom);
int      a7800vid_tab_zoom(const a7800vid_tab *t);
/* the room the picture has, for Fit; call when the widget resizes */
void     a7800vid_tab_set_viewport(a7800vid_tab *t, int w, int h);
/* the zoom steps the chooser offers (0 = Fit first) */
const int *a7800vid_zooms(int *count);

/*
 * The scripted-capture variables, read once by a7800vid_tab_new:
 *   A7800_DEBUGGER_VIDEO_VIEW      display decoded beam values, or a Decoded source:
 *                                  objects zones palette
 *   A7800_DEBUGGER_VIDEO_OVERLAYS  zones,dli,objects,beam,heat,changed,active | all | none
 *   A7800_DEBUGGER_VIDEO_ZOOM      1..16 or fit
 *   A7800_DEBUGGER_VIDEO_SECTION   pixel registers colours lines timing   (names, never indexes)
 * Returns the starting section, A7800VID_S_PIXEL when unset.
 */
int a7800vid_tab_env_section(const a7800vid_tab *t);

/* ---- refresh ------------------------------------------------------------------------ */
#define A7800VID_TAB_PICTURE 1u  /* repaint the picture (a7800vid_tab_picture) */
#define A7800VID_TAB_TABLES  2u  /* refill the status line and the sections */
#define A7800VID_TAB_OFF     4u  /* no machine: show a7800vid_tab_why() instead of a picture */

/* Take the machine's state, update the trackers, and say what to redraw.
 * force: redraw everything. */
unsigned a7800vid_tab_refresh(a7800vid_tab *t, int force);

/* For tests: use this frame instead of the machine. live: the machine is
 * stopped (the Beam view works; cap->beam_row says how far the beam has
 * drawn the frame); 0: a running machine, so no Beam view. */
void a7800vid_tab_feed(a7800vid_tab *t, const struct a7800vid_capture *cap, int live);

/* The console colour (0-255) when no debugger can say (tests, no machine):
 * a hue by the high nibble, brightness by the low one. */
uint32_t a7800vid_fallback_colour(uint8_t index);

/* ---- the picture ------------------------------------------------------------------- */
/*
 * The rendered picture, 0xFFRRGGBB, valid until the next refresh or set_*. NULL
 * when this view cannot be drawn: a7800vid_tab_why() says why, and the tab
 * should show that text.
 */
const uint32_t *a7800vid_tab_picture(a7800vid_tab *t, int *w, int *h);
const char     *a7800vid_tab_why(const a7800vid_tab *t);
int             a7800vid_tab_effective_zoom(const a7800vid_tab *t);

/* ---- the tables -------------------------------------------------------------------- */
const char *a7800vid_tab_status(const a7800vid_tab *t);        /* two lines, '\n' between */

typedef struct {
    const char *name;           /* "CTRL", "DPP", ... */
    char value[24];             /* "5B", "$1800" */
    char meaning[112];
    int age;                    /* refreshes since it last changed (A7800VID_REG_FADE+: not recent) */
} a7800vid_reg;
#define A7800VID_REG_FADE 12
int a7800vid_tab_regs(const a7800vid_tab *t, const a7800vid_reg **out);

/* The 32 palette registers, $20-$3F (BACKGRND, P0C1 .. P7C3; every fourth
 * one mirrors BACKGRND). */
typedef struct {
    int index;                  /* 0..31: palette * 4 + colour */
    char name[12];              /* "BACKGRND", "P0C1", "P3C0" */
    unsigned address;           /* $20 + index */
    uint8_t value;              /* the console colour as written */
    uint32_t rgb;               /* 0xFFRRGGBB through a7800debug_color */
    long uses;                  /* screen pixels of valid rows that use it */
    int inspected;              /* the inspected pixel uses it */
} a7800vid_swatch;
int a7800vid_tab_swatches(const a7800vid_tab *t, const a7800vid_swatch **out);
/* A write watchpoint on that register (A7800DEBUG_BP_WRITE); its id, or -1 */
int a7800vid_tab_break_on_swatch(a7800vid_tab *t, int index);

/* The per-row table: one entry for the first drawn row and for each row whose
 * zone, palette, CTRL or DPP differ from the row above. */
enum { A7800VID_LC_PALETTE = 1, A7800VID_LC_CTRL = 2, A7800VID_LC_DPP = 4, A7800VID_LC_ZONE = 8 };
typedef struct {
    int row;                    /* frame row */
    int scanline;               /* the screen scanline (row + the visible area's top) */
    uint8_t ctrl, charbase;
    uint16_t dpp, dll, dl;
    int offset, holey, dli;     /* the zone's */
    int objects;                /* DL objects DMA read for the row */
    int cycles;                 /* DMA cycles the row used */
    uint8_t background;         /* BACKGRND as written */
    int changed_mask;           /* A7800VID_LC_* */
} a7800vid_line_row;
int a7800vid_tab_lines(const a7800vid_tab *t, const a7800vid_line_row **out);

/* the timing section, as ready-made "label\tvalue" lines */
int  a7800vid_tab_timing(const a7800vid_tab *t, const char *const **lines);
/* where the beam is in the whole frame, 0..1000 (0 when unknown) */
int  a7800vid_tab_beam_permille(const a7800vid_tab *t);

/* A section as plain text (the console's `video` command shares it). */
int  a7800vid_tab_text(a7800vid_tab *t, int section, char *dst, int dstsz);

/* ---- hover, pin ---------------------------------------------------------------------- */
#define A7800VID_PIXEL_OBJECTS 8
typedef struct {
    uint16_t header, graphics;
    uint8_t hpos, width, palette, flags;    /* a7800vid_object */
    int first_cell, cells;      /* the cells of the row it filled */
    int top;                    /* 1: the object that drew the pixel's cell (the last one covering it) */
} a7800vid_pixel_object;
typedef struct {
    int valid;
    int active;                 /* a row MARIA drew (not above or below the screen) */
    int x, y;                   /* picture pixel */
    int row;                    /* frame row, -1 in a picture that is not the frame */
    int scanline;
    int cell;                   /* line RAM cell 0..159, -1 if outside */
    int half;                   /* which of the cell's two screen pixels */
    int read_mode;              /* CTRL bits 0-1 for the row */
    unsigned dll;               /* the zone's DLL entry address */
    unsigned dl;                /* the display list the row read */
    int offset, holey, dli;     /* the zone's */
    int nobjects;               /* objects covering the cell (also those not kept) */
    a7800vid_pixel_object obj[A7800VID_PIXEL_OBJECTS];   /* topmost first */
    int cell_value;             /* the line RAM cell, palette << 2 | colour */
    int reg_index;              /* palette register the pixel read, 0..31 */
    int palette, colour;        /* 0-7, 0-3 */
    uint8_t reg_value;          /* that register */
    uint32_t rgb;               /* 0xFFRRGGBB */
    char text[768];             /* ready-made inspector text */
} a7800vid_pixel;

/* x, y in picture pixels at the current zoom; -1,-1 when the pointer left.
 * Returns 1 when the inspector changed. */
int  a7800vid_tab_hover(a7800vid_tab *t, int x, int y);
void a7800vid_tab_pin(a7800vid_tab *t, int x, int y);      /* a click; the same spot unpins */
void a7800vid_tab_unpin(a7800vid_tab *t);
/* the pinned pixel if any, else the hovered one, else NULL */
const a7800vid_pixel *a7800vid_tab_pixel(const a7800vid_tab *t);
/* rectangles x,y,w,h in picture pixels for the marks drawn over the picture;
 * bit0 of the result: hover valid, bit1: pin valid */
int  a7800vid_tab_marks(const a7800vid_tab *t, int hover[4], int pin[4]);
/* Write watchpoints (A7800DEBUG_BP_WRITE) on the inspected pixel's objects'
 * header bytes, its zone's DLL entry and the palette register it read. Not
 * added twice. The first id, or -1. */
int  a7800vid_tab_break_on_pixel(a7800vid_tab *t);
/* the palette register (0..31) the inspected pixel read, or -1 */
int  a7800vid_tab_pixel_register(const a7800vid_tab *t);

#ifdef __cplusplus
}
#endif

#endif
