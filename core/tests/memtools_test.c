/*
 * memtools_test -- search parsing, the watch list, range parsing, breakpoint
 * descriptions, region names and the trace ring's formatting and filters.
 * Pure: no machine, no SDL. The debugger calls memtools makes are stubbed
 * with a tiny one-label machine.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a7800memtools.h"

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)

/* ---- the stub machine: label "score" = $0080, RAM[$0080] = $42 ---- */
static int stopped = 1;
int a7800debug_label_address(a7800debug *d, const char *label)
{
    (void)d;
    return label && !strcmp(label, "score") ? 0x0080 : -1;
}
int a7800debug_address_label(a7800debug *d, uint16_t addr, char *dst, int dstsz)
{
    (void)d;
    if (addr == 0x0080) return snprintf(dst, (size_t)dstsz, "score");
    if (dstsz > 0) dst[0] = '\0';
    return 0;
}
int a7800debug_is_stopped(a7800debug *d) { (void)d; return stopped; }
int a7800debug_read(a7800debug *d, uint16_t addr, uint8_t *dst, int n)
{
    (void)d;
    for (int i = 0; i < n; i++) dst[i] = (uint8_t)(addr + i == 0x0080 ? 0x42 : 0x10 + i);
    return n;
}
/* "$hex", "label", "label+n" and decimal only */
int a7800debug_eval(a7800debug *d, const char *text, long *out, char *err, int errsz)
{
    long base = 0, add = 0;
    const char *plus = strchr(text, '+');
    char name[32];
    size_t len = plus ? (size_t)(plus - text) : strlen(text);
    (void)d;
    if (len >= sizeof name) return -1;
    memcpy(name, text, len);
    name[len] = '\0';
    if (name[0] == '$') base = strtol(name + 1, NULL, 16);
    else if (!strcmp(name, "score")) base = 0x80;
    else if (name[0] >= '0' && name[0] <= '9') base = strtol(name, NULL, 10);
    else { snprintf(err, (size_t)errsz, "undefined symbol"); return -1; }
    if (plus) add = strtol(plus + 1, NULL, 10);
    *out = base + add;
    return 0;
}

int main(void)
{
    uint8_t pat[A7800SEARCH_MAX], mask;
    uint16_t lo, hi;
    int n, flags;
    char buf[160];
    a7800debug *machine = (a7800debug *)&fails;      /* an opaque handle the stubs ignore */
    a7800debug_trace jsr = { .pc = 0xC017, .a = 1, .x = 2, .y = 3, .p = 0x34, .sp = 0xFD, .bytes = { 0x20, 0x20, 0xC0 } };
    a7800debug_trace lda = { .pc = 0x2000, .bytes = { 0xA9, 0x05, 0x00 } };
    a7800debug_trace bne = { .pc = 0xC000, .bytes = { 0xD0, 0xFE, 0x00 } };

    /* search patterns */
    CHECK(a7800search_parse(A7800SEARCH_HEX, "5A 3c,$FF 0x10", pat, &n, &mask, &flags), "hex parses");
    CHECK(n == 4 && pat[0] == 0x5A && pat[1] == 0x3C && pat[2] == 0xFF && pat[3] == 0x10, "hex bytes");
    CHECK(mask == 0xFF && flags == 0, "hex mask and flags");
    CHECK(a7800search_parse(A7800SEARCH_HEX, "5", pat, &n, &mask, &flags) && n == 1 && pat[0] == 5, "a lone digit");
    CHECK(!a7800search_parse(A7800SEARCH_HEX, "zz", pat, &n, &mask, &flags), "bad hex");
    CHECK(!a7800search_parse(A7800SEARCH_HEX, "  ", pat, &n, &mask, &flags), "empty hex");
    CHECK(a7800search_parse(A7800SEARCH_TEXT, "Ab", pat, &n, &mask, &flags) && n == 2 && pat[0] == 'A' &&
          (flags & A7800DEBUG_SEARCH_NOCASE), "text is case-insensitive");

    /* the watch list */
    a7800watch_clear();
    CHECK(a7800watch_add("  $0080 ") == 0 && a7800watch_add("score") == 1, "add");
    CHECK(a7800watch_add("   ") == -1, "an empty watch is refused");
    CHECK(a7800watch_count() == 2 && !strcmp(a7800watch_text(0), "$0080"), "text is trimmed");
    {
        unsigned generation = a7800watch_generation();
        a7800watch_remove(0);
        CHECK(a7800watch_count() == 1 && !strcmp(a7800watch_text(0), "score"), "remove");
        CHECK(a7800watch_generation() != generation, "the generation moves");
    }
    n = a7800watch_format(machine, 0, buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "$0080") && strstr(buf, "$42 (66)"), "a watch shows its value");
    stopped = 0;
    a7800watch_format(machine, 0, buf, sizeof buf);
    CHECK(strstr(buf, "  -") != NULL, "and a dash while the machine runs");
    stopped = 1;
    a7800watch_clear();
    a7800watch_add("$0080-$0090");
    a7800watch_format(machine, 0, buf, sizeof buf);
    CHECK(strstr(buf, "$0080-$0090") && strstr(buf, "...") && strstr(buf, " 42 "), "a range shows its first bytes");
    CHECK(a7800watch_range(machine, 0, &lo, &hi) && lo == 0x80 && hi == 0x90, "a watch's range");
    a7800watch_clear();
    a7800watch_add("nosuch");
    a7800watch_format(machine, 0, buf, sizeof buf);
    CHECK(strstr(buf, "?") != NULL, "an unresolved watch shows ?");
    a7800watch_clear();
    CHECK(a7800watch_count() == 0, "clear");

    /* ranges */
    CHECK(a7800sym_parse_range(NULL, "C000", &lo, &hi) == 0, "no machine, no range");
    CHECK(a7800sym_parse_range(machine, "C000", &lo, &hi) && lo == 0xC000 && hi == 0xC000, "bare hex is hex");
    CHECK(a7800sym_parse_range(machine, "$80..$8F", &lo, &hi) && lo == 0x80 && hi == 0x8F, "lo..hi");
    CHECK(a7800sym_parse_range(machine, "$8F-$80", &lo, &hi) && lo == 0x80 && hi == 0x8F, "lo-hi, swapped");
    CHECK(a7800sym_parse_range(machine, "score+2", &lo, &hi) && lo == 0x82 && hi == 0x82, "label + offset");
    CHECK(!a7800sym_parse_range(machine, "nosuch", &lo, &hi), "an unknown name means nothing");

    /* the trace ring's filters and format */
    CHECK(a7800debug_trace_match(&jsr, A7800DEBUG_TRACE_ALL, 0, 0), "all");
    CHECK(a7800debug_trace_match(&jsr, A7800DEBUG_TRACE_BRANCHES, 0, 0), "JSR is a branch");
    CHECK(a7800debug_trace_match(&bne, A7800DEBUG_TRACE_BRANCHES, 0, 0), "so is BNE");
    CHECK(!a7800debug_trace_match(&lda, A7800DEBUG_TRACE_BRANCHES, 0, 0), "LDA is not");
    CHECK(a7800debug_trace_match(&jsr, A7800DEBUG_TRACE_RANGE, 0xC000, 0xC0FF), "in range");
    CHECK(!a7800debug_trace_match(&lda, A7800DEBUG_TRACE_RANGE, 0xC000, 0xC0FF), "out of range");
    CHECK(a7800debug_trace_match(&jsr, A7800DEBUG_TRACE_RANGE, 0xC0FF, 0xC000), "a range given backwards");
    a7800debug_trace_format(&jsr, buf, sizeof buf);
    CHECK(!strncmp(buf, "C017  JSR $C020", 15), "the instruction");
    CHECK(strstr(buf, "A=01 X=02 Y=03 P=34 SP=FD") != NULL, "and the registers");
    {
        /* every control-flow opcode of the NMOS 6502, and plain ones that are not */
        static const struct { uint8_t bytes[3]; int branch; const char *what; } cases[] = {
            { { 0x4C, 0x00, 0x10 }, 1, "JMP abs" }, { { 0x6C, 0x00, 0x10 }, 1, "JMP (ind)" },
            { { 0x20, 0x00, 0x10 }, 1, "JSR" }, { { 0x60 }, 1, "RTS" }, { { 0x40 }, 1, "RTI" },
            { { 0x10, 0x05 }, 1, "BPL" }, { { 0x30, 0x05 }, 1, "BMI" }, { { 0x50, 0x05 }, 1, "BVC" },
            { { 0x70, 0x05 }, 1, "BVS" }, { { 0x90, 0x05 }, 1, "BCC" }, { { 0xB0, 0x05 }, 1, "BCS" },
            { { 0xD0, 0x05 }, 1, "BNE" }, { { 0xF0, 0x05 }, 1, "BEQ" },
            { { 0xA9, 0x05 }, 0, "LDA #" }, { { 0x85, 0x80 }, 0, "STA zp" }, { { 0xEA }, 0, "NOP" },
            { { 0x2C, 0x28, 0x00 }, 0, "BIT abs" }, { { 0x00 }, 0, "BRK" }, { { 0xE8 }, 0, "INX" },
        };
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            a7800debug_trace e = { .pc = 0x100 };
            memcpy(e.bytes, cases[i].bytes, 3);
            CHECK(!!a7800debug_trace_match(&e, A7800DEBUG_TRACE_BRANCHES, 0, 0) == cases[i].branch, cases[i].what);
        }
    }
    {
        a7800debug_trace e = { .pc = 0x100, .bytes = { 0xFF, 0x00, 0x00 } };
        a7800debug_trace_format(&e, buf, sizeof buf);
        CHECK(strstr(buf, "???") != NULL, "an illegal opcode is ???");
        e.bytes[0] = 0xD0; e.bytes[1] = 0xFE;
        a7800debug_trace_format(&e, buf, sizeof buf);
        CHECK(strstr(buf, "BNE $0100") != NULL, "a branch shows its destination");
    }

    /* breakpoint descriptions */
    {
        a7800debug_breakpoint bp;
        memset(&bp, 0, sizeof bp);
        bp.type = A7800DEBUG_BP_WRITE; bp.start = bp.end = 0x80;
        snprintf(bp.condition, sizeof bp.condition, "value != old");
        a7800bp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "Write  $0080 (score)  if value != old"), "write watchpoint with label and condition");
        bp.type = A7800DEBUG_BP_READ; bp.start = 0x2000; bp.end = 0x27FF; bp.condition[0] = 0;
        a7800bp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "Read  $2000-$27FF"), "read range");
        bp.type = A7800DEBUG_BP_READ | A7800DEBUG_BP_WRITE; bp.start = bp.end = 0x0280;
        a7800bp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "Access  $0280"), "read + write is one Access entry");
        bp.type = A7800DEBUG_BP_EXEC; bp.start = bp.end = 0xC020;
        a7800bp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "Exec  $C020"), "execute breakpoint");
    }

    /* region names */
    {
        static const a7800debug_region map[] = {
            { 0x0000, 0x001F, 1, 1, "TIA" }, { 0x0020, 0x003F, 1, 1, "MARIA" },
            { 0x0040, 0xFFFF, 1, 1, "RAM and the rest" },
        };
        CHECK(a7800mem_region_name(map, 3, 0x0021, buf, sizeof buf) > 0 && !strcmp(buf, "MARIA"), "a MARIA address");
        CHECK(a7800mem_region_name(map, 3, 0x1234, buf, sizeof buf) > 0 && strstr(buf, "RAM"), "a RAM address");
        CHECK(a7800mem_region_name(map, 0, 0x1000, buf, sizeof buf) == 0 && !buf[0], "none for an empty map");
    }

    printf(fails ? "memtools_test: %d failed\n" : "memtools_test: ok\n", fails);
    return fails != 0;
}
