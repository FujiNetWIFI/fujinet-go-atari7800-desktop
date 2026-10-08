/*
 * a78info -- what an Atari 7800 cartridge image says about itself: its .a78
 * header (title, TV system, controllers) and, where the header is wrong or
 * missing, what is known about the game by the CRC-32 of its ROM data.
 *
 * Used for the AUTO console region (an opened cartridge) and the AUTO
 * controller type (an opened cartridge, or a game booted over the network,
 * known by the CRC the FujiNet cartridge reports).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef A78INFO_H
#define A78INFO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int has_header;          /* an .a78 header ("ATARI7800" at offset 1) */
    char title[33];
    int tv;                  /* 0 NTSC, 1 PAL, -1 unknown */
    int ctrl[2];             /* a7800_ctrl_type per port (never AUTO) */
    uint32_t crc;            /* CRC-32 of the ROM data after any header */
} a78info;

/* Parse an image. Always fills *out (defaults: ProLine joysticks, TV
 * unknown). */
void a78info_parse(const uint8_t *image, uint32_t size, a78info *out);

/* The controllers a game known by its ROM CRC needs, if the CRC is in the
 * table of games whose headers are wrong (the light gun titles): 1 and
 * ctrl[] filled, else 0. */
int a78info_known_controllers(uint32_t crc, int ctrl[2]);

uint32_t a78info_crc32(const uint8_t *data, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* A78INFO_H */
