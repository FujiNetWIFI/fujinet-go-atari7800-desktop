/*
 * media_test -- where a dropped file goes, archives, BIOS recognition and
 * the SD import.
 *
 * The BIOS used here is not Atari's: a filler image whose last four bytes
 * are chosen so its CRC-32 is the NTSC BIOS's (CRC-32 is linear, so any
 * image can be given any CRC). The archives are written by the test itself
 * (stored, no compression).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "a7800session.h"
#include "test_files.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

int main(void)
{
    char cfg[512], data[512], src[700], dest[1200], sd[1200], path[1300];
    uint8_t image[0x4080], bios[0x1000];
    a7800session_paths p;
    a7800session *s;
    size_t i;

    test_tmpdir(cfg, sizeof cfg, "mcfg");
    test_tmpdir(data, sizeof data, "mdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = a7800session_new(&p);
    if (!s) return 1;

    check(a7800session_media_is_cartridge("game.a78"), ".a78 is a cartridge");
    check(a7800session_media_is_cartridge("GAME.A78"), ".A78 is a cartridge (case-insensitive)");
    check(a7800session_media_is_cartridge("game.bin"), "a headerless .bin is one too");
    check(a7800session_media_is_cartridge("Game (USA).zip"), "so is a .zip (looked inside)");
    check(!a7800session_media_is_cartridge("disk.atr"), ".atr is not");
    check(!a7800session_media_is_cartridge("notes.txt"), ".txt is nothing");

    /* a cartridge, plain */
    test_a78_image(image, sizeof image, "TEST GAME");
    snprintf(src, sizeof src, "%s/test.a78", cfg);
    test_write_file(src, image, sizeof image);
    check(a7800session_check_cart(src, dest, sizeof dest), "the cartridge can map it");
    check(a7800session_import_media(s, src, dest, sizeof dest) == 0, "a cartridge imports");
    check(strncmp(dest, a7800session_carts_path(s), strlen(a7800session_carts_path(s))) == 0, "into the cartridge directory");
    check(exists(dest), "and the copy exists");

    /* a cartridge in an archive: unpacked, under the member's name */
    snprintf(src, sizeof src, "%s/Test Game (USA).zip", cfg);
    test_write_zip(src, "Test Game (USA).a78", image, sizeof image);
    check(a7800session_check_cart(src, dest, sizeof dest), "the cartridge can map the image in a .zip");
    check(a7800session_import_media(s, src, dest, sizeof dest) == 0, "a .zip imports");
    snprintf(path, sizeof path, "%s/Test Game (USA).a78", a7800session_carts_path(s));
    check(strcmp(dest, path) == 0 && test_file_equals(dest, image, sizeof image), "unpacked to its .a78");

    /* a BIOS, named like a cartridge, recognised by its CRC */
    for (i = 0; i < sizeof bios; i++) bios[i] = (uint8_t)(i * 7 + 3);
    test_forge_crc(bios, sizeof bios, 0x5d13730cu);
    snprintf(src, sizeof src, "%s/[BIOS] Atari 7800 (USA).a78", cfg);
    test_write_file(src, bios, sizeof bios);
    check(a7800session_media_is_rom(src), "a BIOS is recognised whatever its name");
    check(!a7800session_bios_available(s, 0), "no BIOS before the import");
    check(a7800session_import_media(s, src, dest, sizeof dest) == 0, "a dropped BIOS imports");
    check(a7800session_bios_available(s, 0), "into the ROM directory, under MAME's name");
    snprintf(path, sizeof path, "%s/a7800/7800.u7", a7800session_roms_path(s));
    check(test_file_equals(path, bios, sizeof bios), "byte for byte");
    check(a7800session_bios(s, A7800_REGION_NTSC) == -1, "importing does not choose it");
    a7800session_set_bios(s, A7800_REGION_NTSC, 0);
    check(a7800session_bios(s, A7800_REGION_NTSC) == 0, "Preferences can choose it for the NTSC console");
    check(strcmp(a7800session_get_str(s, "bios_ntsc", ""), "a7800") == 0, "by MAME's name");
    a7800session_set_bios(s, A7800_REGION_NTSC, 2);
    check(a7800session_bios(s, A7800_REGION_NTSC) == -1, "a PAL BIOS is no choice for the NTSC console");
    a7800session_set_bios(s, A7800_REGION_NTSC, -1);
    check(a7800session_bios(s, A7800_REGION_NTSC) == -1, "and none is a choice");

    /* the PAL BIOS, in a .zip */
    {
        static uint8_t pal[0x4000];
        for (i = 0; i < sizeof pal; i++) pal[i] = (uint8_t)(i * 13 + 1);
        test_forge_crc(pal, sizeof pal, 0xd5b61170u);
        snprintf(src, sizeof src, "%s/[BIOS] Atari 7800 (Europe).zip", cfg);
        test_write_zip(src, "[BIOS] Atari 7800 (Europe).a78", pal, sizeof pal);
        check(a7800session_media_is_rom(src), "a BIOS in a .zip is recognised");
        check(a7800session_import_media(s, src, dest, sizeof dest) == 0, "it imports");
        check(a7800session_bios_available(s, 2), "as the PAL BIOS");
        snprintf(path, sizeof path, "%s/a7800p/c300558-001b.u7", a7800session_roms_path(s));
        check(test_file_equals(path, pal, sizeof pal), "under MAME's name for it");
    }
    check(!a7800session_hsc_available(s), "no High Score Cart ROM");

    /* anything else wants the SD folder -- there is no runtime here */
    snprintf(src, sizeof src, "%s/notes.txt", cfg);
    test_write_file(src, (const uint8_t *)"notes", 5);
    check(a7800session_import_media(s, src, dest, sizeof dest) == -1, "a non-cartridge with no SD folder is refused");
    check(strstr(a7800session_last_error(s), "SD folder") != NULL, "with a useful message");

    /* the SD import, made by hand the way provisioning would */
    snprintf(src, sizeof src, "%s/Test Game (USA).zip", cfg);
    check(a7800session_import_cart_to_sd(s, src, dest, sizeof dest) == -1, "SD import refuses when there is no SD folder");
    snprintf(sd, sizeof sd, "%s/fujinet", data); test_mkdir(sd);
    snprintf(sd, sizeof sd, "%s/fujinet/SD", data); test_mkdir(sd);
    check(a7800session_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "SD import unpacks into the SD root");
    snprintf(sd, sizeof sd, "%s/fujinet/SD/Test Game (USA).a78", data);
    check(strcmp(dest, sd) == 0 && test_file_equals(dest, image, sizeof image), "at the top of the SD tree");

    a7800session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
