/*
 * a78info -- see a78info.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "a7800session.h"
#include "a78info.h"

/* The XG-1 light gun titles, by the CRC-32 of their ROM data (No-Intro
 * dumps; the FujiNet cartridge reports the same CRC for a game it booted).
 * Their headers cannot be trusted for the controller: Alien Brigade (USA)
 * and Crossbow (USA) claim paddles, Barnyard Blaster (USA) and the
 * Sentinel prototype a joystick, most European dumps a joystick or
 * nothing. The gun is on the left port. */
static const uint32_t s_lightgun_crcs[] = {
    0xC8849D36, 0x6A19F0FE,     /* Alien Brigade (USA), (Europe) */
    0xED0A587D, 0x02764A86,     /* Barnyard Blaster (USA), (Europe) */
    0xD2EA5686, 0xE93D8894,     /* Crossbow (USA), (Europe) */
    0x4A8F2171, 0x177FC850,     /* Meltdown (USA), (Europe) */
    0x2FDDAD78, 0x47340DF9,     /* Sentinel (USA) (Proto), (Europe) */
};

uint32_t a78info_crc32(const uint8_t *data, uint32_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    int k;

    for (i = 0; i < size; i++) {
        crc ^= data[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

int a78info_known_controllers(uint32_t crc, int ctrl[2])
{
    size_t i;

    for (i = 0; i < sizeof s_lightgun_crcs / sizeof s_lightgun_crcs[0]; i++) {
        if (s_lightgun_crcs[i] == crc) {
            ctrl[0] = A7800_CTRL_LIGHTGUN;
            ctrl[1] = A7800_CTRL_PROLINE;
            return 1;
        }
    }
    return 0;
}

/* The .a78 header's controller byte (offsets 55 and 56): 1 ProLine, 2 the
 * light gun, 3 paddles, 4 a trak-ball, 5 a 2600 joystick, ... -- only the
 * ones this app can plug in count; anything else stays a ProLine. */
static int header_controller(uint8_t b)
{
    switch (b) {
    case 2:  return A7800_CTRL_LIGHTGUN;
    case 5:  return A7800_CTRL_JOY2600;
    default: return A7800_CTRL_PROLINE;
    }
}

void a78info_parse(const uint8_t *image, uint32_t size, a78info *out)
{
    const uint8_t *data = image;
    uint32_t len = size;
    int i;

    memset(out, 0, sizeof *out);
    out->tv = -1;
    out->ctrl[0] = out->ctrl[1] = A7800_CTRL_PROLINE;

    if (image && size >= 128 && memcmp(image + 1, "ATARI7800", 9) == 0) {
        out->has_header = 1;
        memcpy(out->title, image + 17, 32);
        out->title[32] = '\0';
        for (i = 31; i >= 0 && (out->title[i] == ' ' || out->title[i] == '\0'); i--)
            out->title[i] = '\0';
        out->tv = image[57] & 1;
        out->ctrl[0] = header_controller(image[55]);
        out->ctrl[1] = header_controller(image[56]);
        data = image + 128;
        len = size - 128;
    }
    out->crc = image ? a78info_crc32(data, len) : 0;
    a78info_known_controllers(out->crc, out->ctrl);
}
