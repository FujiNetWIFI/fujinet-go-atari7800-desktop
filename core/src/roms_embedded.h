/*
 * roms_embedded -- the table tools/roms/embed-roms.py generates into the
 * build tree: empty (the shipping configuration, WITH_A7800_ROMS=OFF) or the
 * recognised images a developer put in tools/roms/ (NOT redistributable;
 * see COMPLIANCE.md).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef A7800_ROMS_EMBEDDED_H
#define A7800_ROMS_EMBEDDED_H

#include <stdint.h>

typedef struct {
    const char *file;          /* MAME's file name: "7800.u7" */
    const uint8_t *data;
    uint32_t size;
} a7800_embedded_rom;

extern const a7800_embedded_rom a7800_embedded_roms[];
extern const unsigned a7800_embedded_rom_count;

#endif /* A7800_ROMS_EMBEDDED_H */
