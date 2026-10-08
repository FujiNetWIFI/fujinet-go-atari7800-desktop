/*
 * roms -- the Atari 7800 BIOSes and the High Score Cart's ROM: what MAME
 * knows, what the user imported, and what a developer build embedded.
 *
 * None of these is ever required. The console boots the FujiNet cartridge
 * directly through the MAME fork's "none" BIOS, and the High Score Cart is
 * an option. They are copyrighted Atari firmware and this project does not
 * redistribute them (see COMPLIANCE.md): "Import BIOS..." copies a user's
 * own image into the ROM directory, recognised by size and CRC-32 against
 * MAME's ROM_START tables (src/mame/atari/a7800.cpp) and stored under
 * MAME's file name where MAME's ROM path finds it (<roms>/a7800/7800.u7).
 *
 * WITH_A7800_ROMS=ON embeds the recognised images a developer put in
 * tools/roms/; published builds are built with it OFF and
 * core/tests/no_embedded_roms.py checks that claim against the shipped
 * binaries.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "a78info.h"
#include "roms_embedded.h"
#include "session_internal.h"

/* MAME's ROM_SYSTEM_BIOS entries, in the menus' order, and the directory
 * (MAME's set name) each lives in under the ROM path. */
typedef struct {
    a7800_bios_info info;
    const char *set;
} bios_entry;

static const bios_entry s_bios[] = {
    { { "a7800",   "Atari 7800 (NTSC)",                 A7800_REGION_NTSC,
        "7800.u7",         0x1000, 0x5d13730cu }, "a7800" },
    { { "a7800pr", "Atari 7800 (NTSC, with Asteroids)", A7800_REGION_NTSC,
        "c300558-001a.u7", 0x4000, 0xa0e10edfu }, "a7800" },
    { { "a7800p",  "Atari 7800 (PAL)",                  A7800_REGION_PAL,
        "c300558-001b.u7", 0x4000, 0xd5b61170u }, "a7800p" },
};
#define BIOS_COUNT ((int)(sizeof s_bios / sizeof s_bios[0]))

/* The High Score Cart's ROM (MAME's hiscore software list entry). */
#define HSC_FILE "highscre.bin"
#define HSC_SIZE 0x1000u
#define HSC_CRC  0x9be408d3u

int a7800session_bios_count(void)
{
    return BIOS_COUNT;
}

const a7800_bios_info *a7800session_bios_info(int i)
{
    return (i >= 0 && i < BIOS_COUNT) ? &s_bios[i].info : NULL;
}

int roms_identify(const uint8_t *data, uint32_t size)
{
    uint32_t crc;
    int i;

    if (!data || (size != 0x1000 && size != 0x4000))
        return -1;
    crc = a78info_crc32(data, size);
    for (i = 0; i < BIOS_COUNT; i++)
        if (s_bios[i].info.size == size && s_bios[i].info.crc == crc)
            return i;
    if (size == HSC_SIZE && crc == HSC_CRC)
        return ROMS_HSC;
    return -1;
}

static void bios_path(struct a7800session *s, int i, char *dst, size_t dstsz)
{
    snprintf(dst, dstsz, "%s/%s/%s", s->roms_dir, s_bios[i].set, s_bios[i].info.file);
}

static void hsc_path(struct a7800session *s, char *dst, size_t dstsz)
{
    snprintf(dst, dstsz, "%s/hsc/%s", s->roms_dir, HSC_FILE);
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int a7800session_bios_available(a7800session *s, int i)
{
    char path[A7800_PATH_MAX + 64];

    if (i < 0 || i >= BIOS_COUNT)
        return 0;
    bios_path(s, i, path, sizeof path);
    return file_exists(path);
}

int a7800session_hsc_available(a7800session *s)
{
    char path[A7800_PATH_MAX + 64];
    hsc_path(s, path, sizeof path);
    return file_exists(path);
}

int roms_hsc_load(struct a7800session *s, uint8_t *rom4k)
{
    char path[A7800_PATH_MAX + 64];
    FILE *f;
    size_t got;

    hsc_path(s, path, sizeof path);
    f = fopen(path, "rb");
    if (!f)
        return -1;
    got = fread(rom4k, 1, HSC_SIZE, f);
    fclose(f);
    return got == HSC_SIZE && a78info_crc32(rom4k, HSC_SIZE) == HSC_CRC ? 0 : -1;
}

static int write_rom(const char *dir, const char *path, const uint8_t *data, uint32_t size)
{
    FILE *f;

    paths_mkdir_p(dir);
    f = fopen(path, "wb");
    if (!f)
        return -1;
    if (fwrite(data, 1, size, f) != size) {
        fclose(f);
        remove(path);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

/* Store a recognised image under MAME's name: 0, or -1 + error. */
static int store(struct a7800session *s, int which, const uint8_t *data, uint32_t size)
{
    char dir[A7800_PATH_MAX + 32], path[A7800_PATH_MAX + 64];

    if (which == ROMS_HSC) {
        snprintf(dir, sizeof dir, "%s/hsc", s->roms_dir);
        hsc_path(s, path, sizeof path);
    } else {
        snprintf(dir, sizeof dir, "%s/%s", s->roms_dir, s_bios[which].set);
        bios_path(s, which, path, sizeof path);
    }
    if (write_rom(dir, path, data, size) != 0) {
        session_set_error(s, "Cannot write %s", path);
        return -1;
    }
    return 0;
}

int a7800session_import_rom(a7800session *s, const char *path, char *what, int whatsz)
{
    uint8_t *data = NULL;
    uint32_t size = 0;
    char why[256];
    int which;

    if (what && whatsz > 0)
        what[0] = '\0';
    if (media_read_image(path, &data, &size, why, sizeof why) != 0) {
        session_set_error(s, "%s", why);
        return -1;
    }
    which = roms_identify(data, size);
    if (which < 0) {
        free(data);
        session_set_error(s, "This is not an Atari 7800 BIOS or High Score Cart ROM that "
                             "MAME knows (it is identified by size and CRC-32).");
        return -1;
    }
    if (store(s, which, data, size) != 0) {
        free(data);
        return -1;
    }
    free(data);
    if (what && whatsz > 0)
        snprintf(what, (size_t)whatsz, "%s", which == ROMS_HSC ? "High Score Cart ROM"
                                                              : s_bios[which].info.desc);
    return 0;
}

int a7800session_media_is_rom(const char *path)
{
    uint8_t *data = NULL;
    uint32_t size = 0;
    char why[64];
    int which;

    if (media_read_image(path, &data, &size, why, sizeof why) != 0)
        return 0;
    which = roms_identify(data, size);
    free(data);
    return which >= 0;
}

/* Put a developer build's embedded images where MAME looks, once. */
void roms_provision_embedded(struct a7800session *s)
{
    unsigned i;

    for (i = 0; i < a7800_embedded_rom_count; i++) {
        const a7800_embedded_rom *e = &a7800_embedded_roms[i];
        const int which = roms_identify(e->data, e->size);
        char path[A7800_PATH_MAX + 64];

        if (which < 0)
            continue;
        if (which == ROMS_HSC)
            hsc_path(s, path, sizeof path);
        else
            bios_path(s, which, path, sizeof path);
        if (!file_exists(path))
            store(s, which, e->data, e->size);
    }
}

/* ---- the choice per console ------------------------------------------------- */

static const char *bios_key(int region)
{
    return region == A7800_REGION_PAL ? "bios_pal" : "bios_ntsc";
}

int a7800session_bios(a7800session *s, int region)
{
    const char *name;
    int i;

    if (region != A7800_REGION_PAL)
        region = A7800_REGION_NTSC;
    name = a7800session_get_str(s, bios_key(region), NULL);
    if (!name) {
        /* No choice made yet: a developer build boots its embedded BIOS. */
        if (a7800_embedded_rom_count > 0)
            for (i = 0; i < BIOS_COUNT; i++)
                if (s_bios[i].info.region == region && a7800session_bios_available(s, i))
                    return i;
        return -1;
    }
    for (i = 0; i < BIOS_COUNT; i++)
        if (s_bios[i].info.region == region && strcmp(s_bios[i].info.name, name) == 0)
            return i;
    return -1;
}

/* The MAME BIOS name a console boots with: the chosen one if its files are
 * there, else "none". */
const char *roms_bios_name(struct a7800session *s, int region)
{
    const int i = a7800session_bios(s, region);
    if (i >= 0 && a7800session_bios_available(s, i))
        return s_bios[i].info.name;
    return "none";
}

void a7800session_set_bios(a7800session *s, int region, int index)
{
    if (region != A7800_REGION_PAL)
        region = A7800_REGION_NTSC;
    if (index >= 0 && index < BIOS_COUNT && s_bios[index].info.region == region)
        a7800session_set_str(s, bios_key(region), s_bios[index].info.name);
    else
        a7800session_set_str(s, bios_key(region), "none");
    session_machine_changed(s);
}

void a7800session_set_hsc(a7800session *s, int on)
{
    s->opts.hsc = on ? 1 : 0;
    a7800session_set_int(s, "hsc", s->opts.hsc);
    session_hsc_changed(s);
}

int a7800session_hsc(a7800session *s)
{
    return a7800session_get_int(s, "hsc", 0);
}
