/*
 * a7800vidcap -- a displayed frame as MARIA left it, for the Video tab
 * (a7800vidtab.h). Taken once per published frame on the emulation thread
 * while a tab exists (MameHost::CaptureEnable), so a reader sees one
 * consistent frame while the game runs. The per-row records mirror the MAME
 * fork's fngo_maria_line (fngo_mame.h; MameHost.cpp asserts the sizes).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A7800VIDCAP_H
#define A7800VIDCAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define A7800VID_LINES    320   /* screen scanlines held (FNGO_MARIA_LINES) */
#define A7800VID_OBJECTS  24    /* display list objects kept per line */
#define A7800VID_CELLS    160   /* line RAM cells; a cell is 2 screen pixels */
#define A7800VID_FB_MAXW  336
#define A7800VID_FB_MAXH  288

#define A7800VID_OBJ_EXT      1u    /* extended header (5 bytes) */
#define A7800VID_OBJ_INDIRECT 2u    /* graphics are character map bytes */
#define A7800VID_OBJ_WRITE    4u    /* write mode (2 cells a byte) */

typedef struct {
    uint16_t header, graphics;  /* DL header address, graphics (or map) address */
    uint8_t hpos, width, palette, flags;
} a7800vid_object;

/* What produced one screen row (the DMA of the previous line, as MARIA
 * double-buffers) and the registers it was drawn with. */
typedef struct {
    uint8_t valid, ctrl, charbase, nobj;
    uint8_t offset, holey, dli, pad;
    uint16_t dpp, dll, dl, cycles;
    uint8_t palette[32];        /* $20-$3F as written: BACKGRND, P0C1..P7C3 */
    uint8_t cells[A7800VID_CELLS];   /* line RAM as displayed: palette << 2 | colour */
    a7800vid_object obj[A7800VID_OBJECTS];
} a7800vid_line;

typedef struct a7800vid_capture {
    a7800vid_line lines[A7800VID_LINES];     /* index = screen scanline */
    uint32_t fb[A7800VID_FB_MAXW * A7800VID_FB_MAXH];   /* XRGB, fb_w x fb_h, packed */
    int fb_w, fb_h;                          /* the visible area the host publishes */
    int vis_left, vis_top;                   /* frame row = scanline - vis_top; x = 2 * cell - vis_left */
    int total_lines;                         /* 263 NTSC / 313 PAL */
    int vpos, hpos;                          /* the beam */
    int beam_row;                            /* frame rows drawn this frame so far (fb_h: all) */
    uint8_t ctrl;                            /* the last CTRL written */
    uint64_t frame;                          /* frames the screen has completed */
    uint64_t serial;                         /* 0: nothing captured yet */
} a7800vid_capture;

#ifdef __cplusplus
}
#endif

#endif
