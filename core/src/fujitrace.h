/*
 * fujitrace -- the debugger's FujiNet trace: the SLIP frames on the
 * cartridge's link, both ways, decoded into FujiBus packets and kept in a
 * ring of the newest A7800DEBUG_FUJI_TRACE_MAX. Byte-identical exchanges in
 * a row become one entry with a repeat count.
 *
 * No MAME and no session here, so a test can drive it with bytes. Every
 * call locks: the cartridge's worker thread feeds it while a window reads.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef A7800_FUJITRACE_H
#define A7800_FUJITRACE_H

#include <stddef.h>
#include <stdint.h>

#include "a7800debug.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fujitrace fujitrace;

fujitrace *fujitrace_new(void);
void fujitrace_free(fujitrace *t);
/* Bytes off the wire: dir 1 the console to FujiNet, 0 FujiNet to the
 * console. A frame may come whole or in pieces. */
void fujitrace_feed(fujitrace *t, int dir, const uint8_t *raw, size_t len);
/* The newest `max` packets, oldest first. Returns the count. */
int  fujitrace_get(fujitrace *t, a7800debug_fuji_packet *out, int max);
void fujitrace_clear(fujitrace *t);

#ifdef __cplusplus
}
#endif

#endif /* A7800_FUJITRACE_H */
