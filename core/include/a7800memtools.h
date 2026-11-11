/*
 * Watches, range parsing, memory-search queries and breakpoint descriptions,
 * kept in the core so the debugger windows only lay them out.
 *
 * A watch is text the user typed (an address, a label, an expression, or a
 * range lo-hi / lo..hi) shown with its live value. It is stored as text and
 * resolved every time it is shown, so loading symbols later makes a label
 * start resolving.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A7800_MEMTOOLS_H
#define A7800_MEMTOOLS_H

#include <stdint.h>

#include "a7800debug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "lo..hi", "lo-hi" (plain numbers or labels on both sides), or one
 * expression (an address, label, label+3, [ptr]). A bare token of 1-4 hex
 * digits ("C000") is hex unless it names a label; "$C000" and expressions
 * work as in the console (MAME's evaluator). Returns 1 and fills lo and hi
 * (inclusive, lo <= hi), 0 if the text means nothing. */
int a7800sym_parse_range(a7800debug *d, const char *text, uint16_t *lo,
                         uint16_t *hi);

/* ---- watches ---- */
#define A7800WATCH_MAX 64
/* The process-wide list, like the symbol tables: it outlives the window. */
int  a7800watch_count(void);
/* Returns the new index, or -1 if empty / full. A watch that does not resolve
 * yet is kept (it shows "?"). */
int  a7800watch_add(const char *text);
void a7800watch_remove(int index);
void a7800watch_clear(void);
const char *a7800watch_text(int index);
unsigned a7800watch_generation(void);
/* "score  $0080  $42 (66)". While the machine runs the value is "-"; a range
 * shows its first 8 bytes. Returns the length. */
int  a7800watch_format(a7800debug *d, int index, char *dst, int dstsz);
/* The range a watch covers (for "show in Memory"). 1 if it resolves. */
int  a7800watch_range(a7800debug *d, int index, uint16_t *lo, uint16_t *hi);

/* ---- memory search ---- */
enum { A7800SEARCH_HEX = 0, A7800SEARCH_TEXT };
#define A7800SEARCH_MAX 32
/* Hex: byte pairs, optional "$" / "0x" / spaces. Text: one byte per
 * character, case-insensitive. Fills pat, n, mask (0xFF) and flags for
 * a7800debug_search. Returns 1, or 0 for empty / malformed text. */
int a7800search_parse(int kind, const char *text, uint8_t *pat, int *n,
                      uint8_t *mask, int *flags);

/* ---- breakpoints ---- */
/* "Write  $0080 (count)  if value != old"; read + write is "Access". */
int a7800bp_describe(a7800debug *d, const a7800debug_breakpoint *b, char *dst,
                     int dstsz);

/* ---- memory map ---- */
/* The name of the region of `map` (from a7800debug_memory_map) holding addr
 * ("RAM (zero page)", "Cartridge"), for labelling Memory rows. Fetch the map
 * once per refresh and look rows up in it. Returns the length, 0 and "" when
 * no region covers addr. A watch's range for "Show in Memory" is
 * a7800watch_range above. */
int a7800mem_region_name(const a7800debug_region *map, int count, uint16_t addr,
                         char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* A7800_MEMTOOLS_H */
