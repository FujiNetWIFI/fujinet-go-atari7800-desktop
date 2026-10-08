/*
 * media -- reading cartridge images, and routing a file the user dropped on
 * the window to the right place.
 *
 * Three destinations, and which one a file wants is not a matter of taste:
 *
 *   Cartridges (.a78, .bin, or a .zip / .7z holding one -- how most 7800
 *   sets are distributed) go to the cartridge directory, unpacked, and are
 *   opened onto the FujiNet cartridge, whose own mapper engine maps them.
 *
 *   A BIOS or the High Score Cart's ROM (recognised by size and CRC-32,
 *   whatever its name: dumps of them often come named .a78) is imported
 *   into the ROM directory (core/src/roms.c).
 *
 *   Anything else goes to the FujiNet SD folder, because FujiNet is what
 *   serves it. Copying it into the cartridge directory would look like it
 *   worked and then fail to boot.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "fngo_mame.h"
#include "session_internal.h"

/* What the FujiNet cartridge can be handed: .a78 (with its 128-byte header)
 * or a raw .bin; the archives are looked inside for one of those. */
#define IMAGE_EXTS ".a78;.bin;.rom"

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    return slash ? slash + 1 : path;
}

static int ext_is(const char *path, const char *const *exts)
{
    const char *dot = strrchr(base_name(path), '.');
    int i;
    if (!dot) return 0;
    for (i = 0; exts[i]; i++) {
        const char *a = dot + 1, *b = exts[i];
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            if (ca != *b) break;
            a++; b++;
        }
        if (!*a && !*b) return 1;
    }
    return 0;
}

static const char *const cart_exts[] = { "a78", "bin", "zip", "7z", NULL };
static const char *const archive_exts[] = { "zip", "7z", NULL };

int media_read_image(const char *path, uint8_t **data, uint32_t *size,
                     char *why, int whysz)
{
    char member[256];

    *data = NULL;
    *size = 0;
    if (!path || !*path) {
        snprintf(why, (size_t)whysz, "No file");
        return -1;
    }
    if (fngo_mame_archive_read(path, IMAGE_EXTS, data, size, member, sizeof member) != 0) {
        if (ext_is(path, archive_exts))
            snprintf(why, (size_t)whysz, "%s holds no .a78 or .bin image", base_name(path));
        else
            snprintf(why, (size_t)whysz, "Cannot read %s", path);
        return -1;
    }
    return 0;
}

static int write_file(const char *dst, const uint8_t *data, uint32_t size)
{
    FILE *out = fopen(dst, "wb");
    if (!out) return -1;
    if (fwrite(data, 1, size, out) != size) {
        fclose(out);
        remove(dst);
        return -1;
    }
    return fclose(out) == 0 ? 0 : -1;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    char buf[16384];
    size_t n;

    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }
    fclose(in);
    if (fclose(out) != 0) return -1;
    return 0;
}

/* A cartridge into `dir`: an archive's image under its own name, a plain
 * file as it is. */
static int place_cartridge(a7800session *s, const char *src_path, const char *dir,
                           char *dest_out, int dest_sz)
{
    if (ext_is(src_path, archive_exts)) {
        uint8_t *data = NULL;
        uint32_t size = 0;
        char member[256];

        if (fngo_mame_archive_read(src_path, IMAGE_EXTS, &data, &size, member, sizeof member) != 0) {
            session_set_error(s, "%s holds no .a78 or .bin image", base_name(src_path));
            return -1;
        }
        snprintf(dest_out, (size_t)dest_sz, "%s/%s", dir, base_name(member));
        if (write_file(dest_out, data, size) != 0) {
            fngo_mame_free(data);
            session_set_error(s, "Could not write %s", dest_out);
            return -1;
        }
        fngo_mame_free(data);
        return 0;
    }
    snprintf(dest_out, (size_t)dest_sz, "%s/%s", dir, base_name(src_path));
    if (copy_file(src_path, dest_out) != 0) {
        session_set_error(s, "Could not copy %s to %s", base_name(src_path), dir);
        return -1;
    }
    return 0;
}

static int sd_ready(a7800session *s)
{
    /* The SD tree only exists once the FujiNet runtime has been provisioned.
     * Test the DIRECTORY, not just the path string: the path is always
     * computed, so a string check passes and the copy then fails with "could
     * not copy", which tells the user nothing about the actual problem. */
    struct stat st;
    return s->fujinet_sd[0] && stat(s->fujinet_sd, &st) == 0 && S_ISDIR(st.st_mode);
}

int a7800session_import_media(a7800session *s, const char *src_path,
                              char *dest_out, int dest_sz)
{
    const char *name;

    if (!src_path || !*src_path) {
        session_set_error(s, "No file to import");
        return -1;
    }
    name = base_name(src_path);

    if (a7800session_media_is_rom(src_path)) {
        char what[96];
        if (a7800session_import_rom(s, src_path, what, sizeof what) != 0)
            return -1;
        snprintf(dest_out, (size_t)dest_sz, "%s", s->roms_dir);
        return 0;
    }
    if (ext_is(src_path, cart_exts))
        return place_cartridge(s, src_path, s->carts_dir, dest_out, dest_sz);

    if (!sd_ready(s)) {
        session_set_error(s,
            "%s goes to FujiNet's SD folder -- but the FujiNet runtime is not "
            "available, so there is nowhere to put it.", name);
        return -1;
    }
    snprintf(dest_out, (size_t)dest_sz, "%s/%s", s->fujinet_sd, name);
    if (copy_file(src_path, dest_out) != 0) {
        session_set_error(s, "Could not copy %s to %s", name, s->fujinet_sd);
        return -1;
    }
    return 0;
}

int a7800session_media_is_cartridge(const char *path)
{
    return path ? ext_is(path, cart_exts) : 0;
}

/* Into the SD root, so CONFIG shows it at the top of the SD host with no
 * navigation, unpacked: FujiNet serves files as bytes, and the cartridge
 * boots whatever its mapper engine can map (the runtime pushes .a78, .bin
 * and .rom files to the cartridge). */
int a7800session_import_cart_to_sd(a7800session *s, const char *src_path,
                                   char *dest_out, int dest_sz)
{
    if (!src_path || !*src_path) {
        session_set_error(s, "No file to import");
        return -1;
    }
    if (!sd_ready(s)) {
        session_set_error(s,
            "The FujiNet runtime is not available, so there is no SD folder "
            "to import %s into.", base_name(src_path));
        return -1;
    }
    return place_cartridge(s, src_path, s->fujinet_sd, dest_out, dest_sz);
}
