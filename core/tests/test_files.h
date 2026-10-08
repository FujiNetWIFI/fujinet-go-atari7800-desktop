/*
 * Files the tests make for themselves: a small Atari 7800 cartridge, .zip
 * archives (stored, no compression -- what any unzip reads), and images
 * with a chosen CRC-32 (so the BIOS-recognition paths are exercised without
 * any of Atari's copyrighted bytes).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A7800_TEST_FILES_H
#define A7800_TEST_FILES_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint32_t test_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* Make the last four bytes of buf whatever gives the whole a CRC-32 of
 * `want`. CRC-32 is affine in its input over GF(2), so the four bytes are a
 * linear solve: probe the effect of each of their 32 bits, then eliminate. */
static inline void test_forge_crc(uint8_t *buf, size_t n, uint32_t want)
{
    uint8_t *tail = buf + n - 4;
    uint32_t base, m[32], sel[32], target, x = 0;
    int bit, row, r = 0;

    memset(tail, 0, 4);
    base = test_crc32(buf, n);
    for (bit = 0; bit < 32; bit++) {
        memset(tail, 0, 4);
        tail[bit / 8] = (uint8_t)(1u << (bit % 8));
        m[bit] = test_crc32(buf, n) ^ base;
        sel[bit] = 1u << bit;
    }
    target = want ^ base;
    for (row = 31; row >= 0 && r < 32; row--) {
        int piv = -1;
        for (bit = r; bit < 32; bit++)
            if ((m[bit] >> row) & 1) { piv = bit; break; }
        if (piv < 0) continue;
        { uint32_t t = m[piv]; m[piv] = m[r]; m[r] = t; t = sel[piv]; sel[piv] = sel[r]; sel[r] = t; }
        for (bit = 0; bit < 32; bit++)
            if (bit != r && ((m[bit] >> row) & 1)) { m[bit] ^= m[r]; sel[bit] ^= sel[r]; }
        r++;
    }
    /* m is now reduced: each pivot row owns one leading bit */
    for (row = 0; row < r; row++) {
        int lead = 31;
        while (lead >= 0 && !((m[row] >> lead) & 1)) lead--;
        if (lead >= 0 && ((target >> lead) & 1)) { target ^= m[row]; x ^= sel[row]; }
    }
    tail[0] = (uint8_t)x;
    tail[1] = (uint8_t)(x >> 8);
    tail[2] = (uint8_t)(x >> 16);
    tail[3] = (uint8_t)(x >> 24);
}

static inline int test_write_file(const char *path, const uint8_t *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(data, 1, n, f);
    return fclose(f);
}

static inline int test_file_equals(const char *path, const uint8_t *data, size_t n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    size_t got;
    int same;
    if (!f) return 0;
    buf = (uint8_t *)malloc(n + 1);
    got = fread(buf, 1, n + 1, f);
    fclose(f);
    same = got == n && memcmp(buf, data, n) == 0;
    free(buf);
    return same;
}

static inline void put16(FILE *f, unsigned v) { fputc((int)(v & 0xff), f); fputc((int)((v >> 8) & 0xff), f); }
static inline void put32(FILE *f, uint32_t v) { put16(f, v & 0xffff); put16(f, v >> 16); }

/* A .zip holding one member, stored. */
static inline int test_write_zip(const char *path, const char *member, const uint8_t *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    const uint32_t crc = test_crc32(data, n);
    const size_t namelen = strlen(member);
    long central;
    if (!f) return -1;
    put32(f, 0x04034b50u); put16(f, 20); put16(f, 0); put16(f, 0);   /* local header, stored */
    put16(f, 0); put16(f, 0x21);                                     /* time, date */
    put32(f, crc); put32(f, (uint32_t)n); put32(f, (uint32_t)n);
    put16(f, (unsigned)namelen); put16(f, 0);
    fwrite(member, 1, namelen, f);
    fwrite(data, 1, n, f);
    central = ftell(f);
    put32(f, 0x02014b50u); put16(f, 20); put16(f, 20); put16(f, 0); put16(f, 0);
    put16(f, 0); put16(f, 0x21);
    put32(f, crc); put32(f, (uint32_t)n); put32(f, (uint32_t)n);
    put16(f, (unsigned)namelen); put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0);
    put32(f, 0); put32(f, 0);                                        /* attributes, local header offset */
    fwrite(member, 1, namelen, f);
    {
        long end = ftell(f);
        put32(f, 0x06054b50u); put16(f, 0); put16(f, 0); put16(f, 1); put16(f, 1);
        put32(f, (uint32_t)(end - central)); put32(f, (uint32_t)central); put16(f, 0);
    }
    return fclose(f);
}

/* A 16K Atari 7800 cartridge with an .a78 header: MARIA's DMA off, and the
 * background colour (BACKGRND, $20) stepped once a frame by a subroutine
 * that also keeps the count in $80, so a test can tell that it runs, that
 * frames come out, and has a JSR to step over and out of. It sets its own
 * stack, as every game must: a hand-over leaves S where the BIOS does
 * ($16, in the TIA's mirror). The NMI and IRQ vectors point at an RTI.
 * Written for these tests; free to use.
 *
 *   C000  78        sei
 *   C001  D8        cld
 *   C002  A2 FF     ldx #$FF
 *   C004  9A        txs
 *   C005  A9 60     lda #$60         ; CTRL: DMA off
 *   C007  85 3C     sta $3C
 *   C009  A2 00     ldx #$00
 *   C00B  86 20     stx $20          ; BACKGRND
 *   C00D  2C 28 00  bit $0028        ; MSTAT: wait for VBLANK ...
 *   C010  10 FB     bpl $C00D
 *   C012  2C 28 00  bit $0028        ; ... and for it to end
 *   C015  30 FB     bmi $C012
 *   C017  20 20 C0  jsr $C020
 *   C01A  4C 0B C0  jmp $C00B
 *   C01D  40        rti
 *   C020  E8        inx              ; the frame count
 *   C021  86 80     stx $80
 *   C023  60        rts
 */
#define TEST_A78_SIZE (128 + 0x4000)
#define TEST_A78_LOOP 0xC00B          /* the frame loop */
#define TEST_A78_CALL 0xC017          /* the JSR */
#define TEST_A78_BACK 0xC01A          /* where it returns */
#define TEST_A78_SUB  0xC020          /* the subroutine */
#define TEST_A78_COUNT 0x80           /* its frame count */
static inline void test_a78_image(uint8_t *out, size_t size, const char *title)
{
    static const uint8_t code[] = {
        0x78, 0xD8, 0xA2, 0xFF, 0x9A, 0xA9, 0x60, 0x85, 0x3C, 0xA2,
        0x00, 0x86, 0x20, 0x2C, 0x28, 0x00, 0x10, 0xFB, 0x2C, 0x28,
        0x00, 0x30, 0xFB, 0x20, 0x20, 0xC0, 0x4C, 0x0B, 0xC0, 0x40,
    };
    static const uint8_t sub[] = { 0xE8, 0x86, 0x80, 0x60 };
    uint8_t *rom = out + 128;
    memset(out, 0, size);
    out[0] = 1;
    memcpy(out + 1, "ATARI7800", 9);
    strncpy((char *)out + 17, title, 32);
    out[49] = 0x00; out[50] = 0x00; out[51] = 0x40; out[52] = 0x00;   /* 16K, big-endian */
    out[53] = 0x00; out[54] = 0x00;                                   /* a plain ROM */
    out[55] = 1; out[56] = 1;                                         /* ProLine joysticks */
    out[57] = 0;                                                      /* NTSC */
    memcpy(out + 100, "ACTUAL CART DATA STARTS HERE", 28);
    memset(rom, 0xFF, 0x4000);
    memcpy(rom, code, sizeof code);
    memcpy(rom + 0x20, sub, sizeof sub);
    rom[0x3FFA] = 0x1D; rom[0x3FFB] = 0xC0;                          /* NMI -> RTI */
    rom[0x3FFC] = 0x00; rom[0x3FFD] = 0xC0;                          /* RESET */
    rom[0x3FFE] = 0x1D; rom[0x3FFF] = 0xC0;                          /* IRQ -> RTI */
}

#endif
