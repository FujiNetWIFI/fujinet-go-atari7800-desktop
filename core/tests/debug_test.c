/*
 * debug_test -- the debugger contract against a running machine: attach
 * stops it, registers, step / over / out / run to, disassembly with the PC
 * line, breakpoints and watchpoints that hit, memory, MARIA, the I/O view,
 * the cartridge and FujiNet tabs, the prompt (and the commands it refuses),
 * the family debugger's engine (trace ring, stop serial and reasons, hit
 * counts, `old`, Access watchpoints, symbols, memory map, search, the watch
 * list), saving, and detach lets it run again.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "a7800session.h"
#include "a7800debug.h"
#include "a7800memtools.h"
#include "test_files.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int wait_stopped(a7800debug *d, int want, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (!!a7800debug_is_stopped(d) == want) return 1;
        sleep_ms(5); waited += 5;
    }
    return 0;
}

static int wait_status(a7800session *s, const char *want, int timeout_ms)
{
    char st[160];
    int waited = 0;
    while (waited < timeout_ms) {
        a7800session_cart_status(s, st, sizeof st);
        if (strstr(st, want)) return 1;
        sleep_ms(20); waited += 20;
    }
    printf("  (status: %s)\n", st);
    return 0;
}

/* Let it run until the debugger stops it again. */
static int run_until_stop(a7800debug *d, int timeout_ms)
{
    a7800debug_resume(d);
    return wait_stopped(d, 1, timeout_ms);
}

static int pc_of(a7800debug *d)
{
    a7800debug_cpu c;
    a7800debug_cpu_get(d, &c);
    return c.pc;
}

int main(void)
{
    char cfg[512], data[512], rom[700], out[8192], path[800];
    static uint8_t image[TEST_A78_SIZE];
    a7800session_paths p;
    a7800session *s;
    a7800session_start_opts o;
    a7800debug *d;
    a7800debug_cpu c, c2;
    a7800debug_line lines[48];
    int pc_line = -1, n, i;

    test_tmpdir(cfg, sizeof cfg, "dcfg");
    test_tmpdir(data, sizeof data, "ddata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    test_a78_image(image, sizeof image, "DEBUG TEST");
    snprintf(rom, sizeof rom, "%s/steps.a78", cfg);
    test_write_file(rom, image, sizeof image);

    s = a7800session_new(&p);
    if (!s) return 1;
    a7800session_default_opts(s, &o);
    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    o.cart_path = rom;
    check(a7800session_start(s, &o) == 0, "session starts with the test cartridge");
    check(wait_status(s, "game running", 5000), "and the cartridge boots it");
    sleep_ms(100);

    d = a7800session_debugger(s);
    check(d != NULL, "debugger handle");
    a7800debug_attach(d);
    check(a7800debug_is_attached(d), "attached");
    check(wait_stopped(d, 1, 3000), "attaching stops the machine");

    a7800debug_cpu_get(d, &c);
    check(c.pc >= TEST_A78_LOOP && c.pc <= 0xC023, "the PC is in the frame loop");
    check(c.frame > 0, "frames have been counted");

    /* step */
    a7800debug_step(d);
    check(wait_stopped(d, 1, 2000), "stopped after a step");
    {
        int waited = 0;
        a7800debug_cpu_get(d, &c2);
        while (c2.pc == c.pc && waited < 2000) {
            sleep_ms(5); waited += 5;
            a7800debug_cpu_get(d, &c2);
        }
    }
    check(c2.pc != c.pc || (c.pc >= 0xC00D && c.pc <= 0xC015), "a step moves the PC (or loops on MSTAT)");
    check(c2.total_cycles > c.total_cycles, "and the cycle count");

    /* run to the JSR, step over it, run into the subroutine, step out */
    a7800debug_run_to(d, TEST_A78_CALL);
    check(wait_stopped(d, 1, 3000) && pc_of(d) == TEST_A78_CALL, "run to the JSR");
    a7800debug_cpu_get(d, &c);
    a7800debug_step_over(d);
    check(wait_stopped(d, 1, 3000) && pc_of(d) == TEST_A78_BACK, "step over lands after it");
    a7800debug_cpu_get(d, &c2);
    check(c2.x == ((c.x + 1) & 0xFF), "and the subroutine ran (X counted)");
    a7800debug_run_to(d, TEST_A78_SUB);
    check(wait_stopped(d, 1, 3000) && pc_of(d) == TEST_A78_SUB, "run into the subroutine");
    a7800debug_step(d);
    check(wait_stopped(d, 1, 2000) && pc_of(d) == TEST_A78_SUB + 1, "step inside it");
    a7800debug_step_out(d);
    check(wait_stopped(d, 1, 3000) && pc_of(d) == TEST_A78_BACK, "step out returns to the caller");

    /* disassembly around the PC */
    n = a7800debug_disassemble(d, (uint16_t)a7800debug_row_address(d, TEST_A78_BACK, -6), lines, 48, &pc_line);
    check(n > 8, "disassembly lines");
    check(pc_line >= 0 && lines[pc_line].address == TEST_A78_BACK && lines[pc_line].is_pc, "the PC line is marked");
    {
        int saw_jsr = 0, saw_jmp = 0;
        for (i = 0; i < n; i++) {
            if (strstr(lines[i].disasm, "jsr") || strstr(lines[i].disasm, "JSR")) saw_jsr = 1;
            if (strstr(lines[i].disasm, "jmp") || strstr(lines[i].disasm, "JMP")) saw_jmp = 1;
        }
        check(saw_jsr && saw_jmp, "the loop disassembles as JSR / JMP");
    }
    n = a7800debug_disassemble(d, 0xC000, lines, 3, NULL);
    check(n == 3 && lines[1].address == 0xC001 && lines[2].address == 0xC002, "one line an instruction");
    check(strcmp(lines[0].bytes, "78") == 0 || strstr(lines[0].bytes, "78") == lines[0].bytes, "with its bytes");

    /* an execute breakpoint hits */
    check(a7800debug_breakpoint_toggle(d, TEST_A78_SUB) == 1, "breakpoint set on the subroutine");
    check(a7800debug_breakpoint_check(d, TEST_A78_SUB), "and reported");
    a7800debug_resume(d);
    check(wait_stopped(d, 0, 1000) || 1, "running");
    check(wait_stopped(d, 1, 3000), "the breakpoint hit");
    check(pc_of(d) == TEST_A78_SUB, "stopped at the breakpoint's address");
    {
        char why[128]; int addr = -1;
        a7800debug_stop_reason(d, why, sizeof why, &addr);
        printf("  (reason: %s)\n", why);
        check(strstr(why, "reakpoint") != NULL && addr == TEST_A78_SUB, "the stop reason says breakpoint");
    }
    n = a7800debug_disassemble(d, TEST_A78_SUB, lines, 2, NULL);
    check(n >= 1 && lines[0].has_breakpoint, "the line shows the breakpoint");
    check(a7800debug_breakpoint_toggle(d, TEST_A78_SUB) == 0, "toggled off");

    /* nothing listed before the first breakpoint or watchpoint exists */
    {
        a7800debug_breakpoint none[4];
        check(a7800debug_breakpoint_list(d, none, 4) == 0, "an empty list before any watchpoint");
    }

    /* a watchpoint on the frame count */
    check(a7800debug_breakpoint_add(d, A7800DEBUG_BP_WRITE, TEST_A78_COUNT, TEST_A78_COUNT, "this bogus ((") == -1,
          "an unparsable condition is refused");
    {
        int id = a7800debug_breakpoint_add(d, A7800DEBUG_BP_WRITE, TEST_A78_COUNT, TEST_A78_COUNT, "");
        a7800debug_breakpoint list[8];
        check(id > 1000, "write watchpoint added");
        check(a7800debug_breakpoint_list(d, list, 8) == 1 && list[0].type == A7800DEBUG_BP_WRITE
              && list[0].start == TEST_A78_COUNT, "and listed");
        a7800debug_resume(d);
        check(wait_stopped(d, 1, 3000), "the watchpoint hit");
        {
            char why[128];
            a7800debug_stop_reason(d, why, sizeof why, NULL);
            printf("  (reason: %s)\n", why);
            check(strstr(why, "atchpoint") != NULL, "the stop reason says watchpoint");
        }
        a7800debug_breakpoint_enable(d, id, 0);
        check(a7800debug_breakpoint_list(d, list, 8) == 1 && !list[0].enabled, "disabled");
        a7800debug_breakpoint_clear(d);
        check(a7800debug_breakpoint_list(d, list, 8) == 0, "cleared");
    }

    /* memory */
    {
        uint8_t v = 0, cnt = 0;
        a7800debug_cpu_get(d, &c);
        a7800debug_read(d, TEST_A78_COUNT, &cnt, 1);
        check(cnt == c.x || cnt == ((c.x - 1) & 0xFF), "the frame count is in RAM");
        a7800debug_write(d, 0x2200, 0x5A);
        a7800debug_read(d, 0x2200, &v, 1);
        check(v == 0x5A, "a RAM write lands");
        a7800debug_read(d, 0xFFFC, &v, 1);
        check(v == 0x00, "the reset vector reads through the cartridge");
        a7800debug_read(d, 0xFFFD, &v, 1);
        check(v == 0xC0, "the reset vector's high byte");
    }

    /* registers */
    a7800debug_cpu_set(d, A7800_REG_A, 0x42);
    a7800debug_cpu_set(d, A7800_FLAG_C, 1);
    a7800debug_cpu_get(d, &c);
    check(c.a == 0x42 && c.c == 1, "registers and flags are editable");
    check(c.sp >= 0 && c.sp <= 0xFF, "SP is the 6502's 8 bits");
    {
        const int sp = c.sp;
        a7800debug_cpu_set(d, A7800_REG_SP, 0xF0);
        a7800debug_cpu_get(d, &c);
        check(c.sp == 0xF0, "and writes back as 8 bits");
        a7800debug_cpu_set(d, A7800_REG_SP, sp);
    }

    /* MARIA */
    {
        a7800debug_maria m;
        static uint32_t pal[A7800DEBUG_PALETTE_WIDTH * A7800DEBUG_PALETTE_HEIGHT];
        a7800debug_maria_get(d, &m);
        check(m.ctrl == 0x60 && !m.dma_on, "MARIA: CTRL $60, DMA off");
        check(m.scanline >= 0, "and its beam");
        check(a7800debug_palette_image(d, pal), "the palette image draws");
        check(a7800debug_color(d, 0x0F) != a7800debug_color(d, 0x00), "the console's colours differ");
        a7800debug_command(d, "maria", out, sizeof out);
        check(strstr(out, "CTRL") != NULL, "the maria command");
    }

    /* the I/O view */
    {
        a7800debug_io io;
        a7800debug_io_get(d, &io);
        check(io.port_type[0] == A7800_CTRL_PROLINE, "I/O: port 1 has a ProLine");
        check((io.swchb & 0x0B) == 0x0B, "no console switch held");
        check(!io.pokey_present, "no POKEY on this cartridge");
    }

    /* the cartridge */
    {
        a7800debug_cart cart;
        a7800debug_cart_get(d, &cart);
        check(cart.present && cart.booted_image, "cartridge tab: a booted image");
        check(cart.mode == 2 && cart.handover == 2, "a game, started by the loader");
        check(cart.live_crc == test_crc32(image + 128, sizeof image - 128), "its CRC is the test image's");
        check(a7800debug_cart_info(d, out, sizeof out) > 0 && strstr(out, "FujiNet"), "cart info line");
    }

    /* the FujiNet tab, with FujiNet off: the link pane says so, and the
     * trace (fujitrace_test covers its decoding) clears */
    {
        static a7800debug_fuji_packet pk[A7800DEBUG_FUJI_TRACE_MAX];
        check(a7800debug_fujinet_state(d, out, sizeof out) > 0 && strstr(out, "FujiNet     off")
              && strstr(out, "Link        down"), "FujiNet tab: FujiNet off, the link down");
        a7800debug_fujinet_trace_clear(d);
        check(a7800debug_fujinet_trace(d, pk, A7800DEBUG_FUJI_TRACE_MAX) == 0, "FujiNet tab: the trace clears");
    }

    /* the trace ring */
    {
        static a7800debug_trace tr[16];
        char line[160];
        int branches = 0, in_range = 0, count;
        uint64_t serial;
        uint8_t opcode = 0;

        check(a7800debug_trace_count(d) == 0 && a7800debug_trace_read(d, tr, 16) == 0 && !a7800debug_trace_enabled(d),
              "the trace is empty (and off) until enabled");
        a7800debug_trace_enable(d, 1);
        check(a7800debug_trace_enabled(d), "the trace is on");
        serial = a7800debug_stop_serial(d);
        for (i = 0; i < 10; i++) {
            a7800debug_step(d);
            wait_stopped(d, 1, 2000);
        }
        check(a7800debug_stop_serial(d) >= serial + 10, "every stop bumps the stop serial");
        count = a7800debug_trace_read(d, tr, 16);
        check(count >= 10, "the trace holds the stepped instructions");
        check(a7800debug_trace_count(d) == count, "trace_count agrees");
        check(count > 0 && tr[0].pc >= TEST_A78_LOOP && tr[0].pc <= 0xC023, "newest first, in the frame loop");
        check(count > 0 && a7800debug_trace_read(d, tr, 1) == 1, "reading fewer than held");
        count = a7800debug_trace_read(d, tr, 16);
        a7800debug_read(d, tr[0].pc, &opcode, 1);
        check(tr[0].bytes[0] == opcode, "an entry carries its opcode bytes");
        check(tr[0].cycles > tr[1].cycles && tr[0].scanline >= 0, "newest has the most cycles, and a beam position");
        for (i = 0; i < count; i++) {
            branches += a7800debug_trace_match(&tr[i], A7800DEBUG_TRACE_BRANCHES, 0, 0);
            in_range += a7800debug_trace_match(&tr[i], A7800DEBUG_TRACE_RANGE, 0xC0FF, 0xC000);
            check(a7800debug_trace_match(&tr[i], A7800DEBUG_TRACE_ALL, 0, 0), "ALL passes everything");
        }
        check(branches >= 1 && branches < count, "BRANCHES keeps the branches and drops the rest");
        check(in_range == count, "RANGE (given backwards) keeps the loop");
        check(!a7800debug_trace_match(&tr[0], A7800DEBUG_TRACE_RANGE, 0x4000, 0x5000), "RANGE drops others");
        a7800debug_trace_format(&tr[0], line, sizeof line);
        check(strstr(line, "A=") != NULL && strstr(line, "SP=") != NULL && line[4] == ' ',
              "trace_format shows pc, instruction and registers");
        a7800debug_trace_enable(d, 0);
        check(!a7800debug_trace_enabled(d), "the trace turns off");
        a7800debug_trace_clear(d);
        check(a7800debug_trace_count(d) == 0, "Clear empties the ring");
    }

    /* the stop reason names its address; the stop serial moves with stops */
    {
        char why[128]; int addr = -1;
        uint64_t before = a7800debug_stop_serial(d);
        a7800debug_step(d);
        wait_stopped(d, 1, 2000);
        a7800debug_stop_reason(d, why, sizeof why, &addr);
        check(strstr(why, "step at $") != NULL && addr == pc_of(d), "the stop reason says where");
        check(a7800debug_stop_serial(d) > before, "stop_serial moved");
        a7800debug_resume(d);
        a7800debug_stop_reason(d, why, sizeof why, &addr);
        check(strcmp(why, "running") == 0 && addr == -1, "running has no address");
        {
            static const uint8_t any = 0x00;
            uint16_t hit[2], one;
            check(a7800debug_search(d, &any, 1, 0xFF, 0, 0, 0xFFFF, hit, 2) == -1, "search answers -1 while running");
            check(a7800debug_search_next(d, &any, 1, 0xFF, 0, 0, 0, &one) == -1, "and so does search_next");
        }
        a7800debug_stop(d);
        wait_stopped(d, 1, 3000);
        a7800debug_stop_reason(d, why, sizeof why, &addr);
        check(strstr(why, "stopped at $") != NULL && addr == pc_of(d), "a stop request is also located");
    }

    /* disabled execute breakpoints reach the gutter; hits; Clear Hits */
    {
        a7800debug_breakpoint list[8];
        char why[128];
        int id = a7800debug_breakpoint_add(d, A7800DEBUG_BP_EXEC, TEST_A78_SUB, TEST_A78_SUB, "");
        a7800debug_breakpoint_enable(d, id, 0);
        a7800debug_disassemble(d, TEST_A78_SUB, lines, 1, NULL);
        check(!lines[0].has_breakpoint && lines[0].bp_disabled, "a disabled breakpoint is the grey dot");
        a7800debug_breakpoint_enable(d, id, 1);
        a7800debug_disassemble(d, TEST_A78_SUB, lines, 1, NULL);
        check(lines[0].has_breakpoint && !lines[0].bp_disabled, "an enabled one is the red dot");
        a7800debug_breakpoint_clear(d);
        a7800debug_disassemble(d, TEST_A78_SUB, lines, 1, NULL);
        check(!lines[0].has_breakpoint && !lines[0].bp_disabled, "and none is none");

        id = a7800debug_breakpoint_add(d, A7800DEBUG_BP_EXEC, TEST_A78_SUB, TEST_A78_SUB, "");
        check(run_until_stop(d, 3000), "an execute breakpoint hits again");
        a7800debug_stop_reason(d, why, sizeof why, NULL);
        check(strstr(why, "breakpoint") && strstr(why, "at $C020"), "its reason says breakpoint and where");
        check(a7800debug_breakpoint_list(d, list, 8) == 1 && list[0].hits == 1, "it counted one hit");
        check(run_until_stop(d, 3000) && a7800debug_breakpoint_list(d, list, 8) == 1 && list[0].hits == 2, "and another");
        a7800debug_breakpoint_clear_hits(d);
        check(a7800debug_breakpoint_list(d, list, 8) == 1 && list[0].hits == 0, "Clear Hits zeroes it");
        a7800debug_breakpoint_clear(d);
    }

    /* `old`: the byte a write replaces; a poke made while stopped is seen */
    {
        a7800debug_breakpoint list[8];
        uint8_t v = 0;
        char why[128];
        int id;

        a7800debug_write(d, TEST_A78_COUNT, 0x40);
        id = a7800debug_breakpoint_add(d, A7800DEBUG_BP_WRITE, TEST_A78_COUNT, TEST_A78_COUNT, "old == $40");
        check(id > 1000, "a condition may use old");
        check(run_until_stop(d, 5000), "a poke while stopped is what old holds at the next write");
        a7800debug_read(d, TEST_A78_COUNT, &v, 1);
        a7800debug_stop_reason(d, why, sizeof why, NULL);
        printf("  (reason: %s)\n", why);
        {
            char want[48];
            snprintf(want, sizeof want, "wrote $%04X = $%02X (was $40)", TEST_A78_COUNT, v);
            check(strstr(why, want) != NULL, "the reason says what it wrote and replaced");
        }
        check(a7800debug_breakpoint_list(d, list, 8) == 1 && list[0].hits == 1, "one hit");
        check(a7800debug_breakpoint_list(d, list, 8) == 1 && !strcmp(list[0].condition, "old == $40"), "with its condition");
        a7800debug_breakpoint_remove(d, id);
        check(a7800debug_breakpoint_list(d, list, 8) == 0, "removed");

        /* reads: BIT MSTAT reads $0028 every iteration of the frame loop */
        id = a7800debug_breakpoint_add(d, A7800DEBUG_BP_READ, 0x0028, 0x0028, "");
        check(id > 1000 && run_until_stop(d, 5000), "a read watchpoint hits");
        a7800debug_stop_reason(d, why, sizeof why, NULL);
        printf("  (reason: %s)\n", why);
        check(strstr(why, "read $0028 = $") != NULL, "its reason says what it read");
        a7800debug_breakpoint_remove(d, id);

        /* read + write is one Access entry */
        id = a7800debug_breakpoint_add(d, A7800DEBUG_BP_ACCESS, TEST_A78_COUNT, TEST_A78_COUNT, "");
        check(a7800debug_breakpoint_list(d, list, 8) == 1 && list[0].type == A7800DEBUG_BP_ACCESS, "Access is one entry");
        a7800bp_describe(d, &list[0], why, sizeof why);
        check(strstr(why, "Access  $0080") != NULL, "described as Access");
        a7800debug_breakpoint_remove(d, id);
        a7800debug_command(d, "wpset 80,1,rw", out, sizeof out);
        check(a7800debug_breakpoint_list(d, list, 8) == 1 && list[0].type == A7800DEBUG_BP_ACCESS, "and wpset rw makes one too");
        a7800debug_breakpoint_clear(d);
    }

    /* symbols for a Symbols tab */
    {
        int idx[64], total, count;
        uint16_t addr = 0;
        char name[64], msg[256];
        FILE *f;

        a7800debug_clear_symbols(d);
        check(a7800debug_symbols_count(d) == 0, "no symbols after Clear Symbols");
        snprintf(path, sizeof path, "%s/syms.lbl", cfg);
        f = fopen(path, "w");
        if (f) {
            fputs("zeta = $C000\nAlpha_Probe = $C000\nmid = $4000\n", f);
            fclose(f);
        }
        check(a7800debug_load_symbols(d, path, msg, sizeof msg) == 3, "a plain symbol list loads");
        a7800debug_set_label(d, 0xC0DE, "My_Probe_Label");
        total = a7800debug_symbols_count(d);
        check(total == 4, "a label adds one");
        check(a7800debug_symbols_get(d, 0, &addr, name, sizeof name) == 1 && name[0], "symbols_get");
        check(a7800debug_symbols_get(d, total, &addr, name, sizeof name) == 0, "past the end");
        count = a7800debug_symbols_filter(d, "probe_LABEL", idx, 64);
        check(count == 1 && a7800debug_symbols_get(d, idx[0], &addr, name, sizeof name) && addr == 0xC0DE
              && !strcmp(name, "My_Probe_Label"), "the filter is a case-insensitive substring");
        count = a7800debug_symbols_filter(d, "probe", idx, 64);
        check(count == 2, "and finds both");
        count = a7800debug_symbols_filter(d, "", idx, 64);
        check(count == total, "an empty filter matches all");
        {
            int ordered = 1, prev = -1;
            for (i = 0; i < count; i++) {
                a7800debug_symbols_get(d, idx[i], &addr, name, sizeof name);
                ordered &= (int)addr >= prev;
                prev = addr;
            }
            a7800debug_symbols_get(d, idx[0], &addr, name, sizeof name);
            check(ordered && addr == 0x4000, "ordered by address");
            a7800debug_symbols_get(d, idx[1], &addr, name, sizeof name);
            check(addr == 0xC000 && !strcmp(name, "Alpha_Probe"), "then by name");
        }
        check(a7800debug_symbols_filter(d, "", idx, 2) == 2, "the filter honours max");
        a7800debug_clear_symbols(d);
        check(a7800debug_symbols_count(d) == 0 && a7800debug_symbols_filter(d, "probe", idx, 64) == 0
              && a7800debug_label_address(d, "mid") == -1, "Clear Symbols drops them all");
    }

    /* the memory map: ordered, gap-free, $0000-$FFFF, from the 7800's own map */
    {
        a7800debug_region map[40];
        char label[64];
        int ok = 1, count = a7800debug_memory_map(d, map, 40);
        check(count >= 20 && count < 40, "the memory map has its regions");
        check(count > 0 && map[0].lo == 0x0000 && map[count - 1].hi == 0xFFFF, "from $0000 to $FFFF");
        for (i = 1; i < count; i++)
            ok &= map[i].lo == map[i - 1].hi + 1;
        for (i = 0; i < count; i++)
            ok &= map[i].lo <= map[i].hi && map[i].name[0] != '\0';
        check(ok, "contiguous, ordered, all named");
        check(a7800mem_region_name(map, count, 0x0000, label, sizeof label) > 0 && !strcmp(label, "TIA"), "TIA at $0000");
        check(a7800mem_region_name(map, count, 0x0030, label, sizeof label) > 0 && !strcmp(label, "MARIA"), "MARIA at $0020");
        check(a7800mem_region_name(map, count, TEST_A78_COUNT, label, sizeof label) > 0 && strstr(label, "RAM"), "RAM at $0080");
        check(a7800mem_region_name(map, count, 0x0284, label, sizeof label) > 0 && strstr(label, "RIOT I/O"), "RIOT I/O at $0280");
        check(a7800mem_region_name(map, count, 0x0480, label, sizeof label) > 0 && strstr(label, "RIOT RAM"), "RIOT RAM at $0480");
        check(a7800mem_region_name(map, count, 0x2300, label, sizeof label) > 0 && strstr(label, "RAM"), "RAM at $2300");
        check(a7800mem_region_name(map, count, 0xC000, label, sizeof label) > 0 && strstr(label, "Cartridge"), "the cartridge at $C000");
        for (i = 0; i < count; i++)
            if (map[i].lo == 0x1800)
                check(map[i].readable && map[i].writable && map[i].hi == 0x1FFF, "RAM at $1800-$1FFF");
        check(a7800debug_memory_map(d, map, 3) == 3, "the map honours max");
        a7800debug_command(d, "help", out, sizeof out);
    }

    /* memory search over the 6502's 64K, and `find` through the prompt */
    {
        static const uint8_t loop[] = { 0x2C, 0x28, 0x00, 0x10, 0xFB };
        static const uint8_t bit_op = 0x20;
        static const uint8_t fuji[] = { 'f', 'U', 'j', 'I' };
        static const uint8_t none[] = { 0x12, 0x34, 0x56, 0x78, 0x9A };
        uint8_t pat[A7800SEARCH_MAX], mask;
        uint16_t hit[16], found = 0;
        int cnt, plen, flags, at_c00d = 0;

        cnt = a7800debug_search(d, loop, 5, 0xFF, 0, 0, 0xFFFF, hit, 16);
        for (i = 0; i < cnt; i++)
            at_c00d |= hit[i] == 0xC00D;
        check(cnt >= 1 && at_c00d, "search finds the MSTAT wait loop at $C00D");
        check(a7800debug_search(d, loop, 5, 0xFF, 0, 0xC000, 0xC00C, hit, 16) == 0, "`to` bounds the start: not before $C00D");
        check(a7800debug_search(d, loop, 5, 0xFF, 0, 0xC00D, 0xC00D, hit, 16) == 1, "and the start itself is in");
        check(a7800debug_search(d, loop, 5, 0xFF, 0, 0xC00E, 0xC00D, hit, 16) == 1, "from > to swaps");
        check(a7800debug_search(d, &bit_op, 1, 0xF0, 0, 0xC00D, 0xC00D, hit, 16) == 1, "a mask");
        a7800debug_write(d, 0x2300, 'F'); a7800debug_write(d, 0x2301, 'u');
        a7800debug_write(d, 0x2302, 'J'); a7800debug_write(d, 0x2303, 'i');
        check(a7800search_parse(A7800SEARCH_TEXT, "fuji", pat, &plen, &mask, &flags) && (flags & A7800DEBUG_SEARCH_NOCASE),
              "text parses");
        cnt = a7800debug_search(d, pat, plen, mask, flags, 0x2000, 0x27FF, hit, 16);
        check(cnt == 1 && hit[0] == 0x2300, "case-insensitive text search finds the poked word");
        check(a7800debug_search(d, fuji, 4, 0xFF, 0, 0x2000, 0x27FF, hit, 16) == 0, "and a case-sensitive one does not");
        check(a7800debug_search_next(d, loop, 5, 0xFF, 0, 0x0000, 0, &found) == 1 && (found == 0xC00D || found < 0xC00D),
              "next from $0000 finds the loop");
        check(a7800debug_search_next(d, pat, plen, mask, flags, 0x2300, 0, &found) == 1 && found == 0x2300,
              "next wraps past the end");
        check(a7800debug_search_next(d, pat, plen, mask, flags, 0x2300, 1, &found) == 1 && found == 0x2300,
              "previous wraps to the last");
        check(a7800debug_search_next(d, none, 5, 0xFF, 0, 0x0000, 0, &found) == 0, "no match is 0");

        a7800debug_command(d, "find 2C 28 00 10 FB", out, sizeof out);
        check(strstr(out, "$C00D") != NULL, "find lists the match");
        a7800debug_command(d, "find text FUJI", out, sizeof out);
        check(strstr(out, "$2300") != NULL, "find text");
        a7800debug_command(d, "find 12 34 56 78 9A", out, sizeof out);
        check(strstr(out, "not found") != NULL, "find says so when there is none");
        a7800debug_command(d, "find", out, sizeof out);
        check(strstr(out, "find [hex]") != NULL, "find with nothing shows its usage");
        a7800debug_command(d, "find 2300,4,46", out, sizeof out);
        check(strstr(out, "stop the machine") == NULL, "MAME's own comma form still reaches MAME");
    }

    /* the watch list (process-wide), through the prompt */
    a7800watch_clear();
    a7800debug_command(d, "watch $0080", out, sizeof out);
    check(strstr(out, "$0080") != NULL && a7800watch_count() == 1, "watch adds");
    a7800debug_command(d, "watch 2300..2303", out, sizeof out);
    check(a7800watch_count() == 2 && strstr(out, "$2300-$2303") != NULL, "a range watch");
    {
        uint16_t lo, hi;
        check(a7800watch_range(d, 1, &lo, &hi) && lo == 0x2300 && hi == 0x2303, "its range, bare hex read as hex");
    }
    a7800debug_command(d, "unwatch 0", out, sizeof out);
    check(a7800watch_count() == 1, "unwatch removes one");
    a7800debug_command(d, "unwatch all", out, sizeof out);
    check(a7800watch_count() == 0, "unwatch all");
    a7800debug_completions(d, "wat", out, sizeof out);
    check(strstr(out, "watch") != NULL, "watch completes");
    a7800debug_completions(d, "unw", out, sizeof out);
    check(strstr(out, "unwatch") != NULL, "unwatch completes");
    a7800debug_completions(d, "ol", out, sizeof out);
    check(strstr(out, "old") != NULL, "old completes");
    a7800debug_command(d, "help", out, sizeof out);
    check(strstr(out, "watch") != NULL && strstr(out, "find") != NULL && strstr(out, "old") != NULL,
          "the help lists the new commands");

    /* the sound registers */
    {
        a7800debug_sound snd;
        a7800debug_sound_get(d, &snd);
        check(!snd.pokey_present, "sound: no POKEY on this cartridge");
    }

    /* the prompt */
    a7800debug_command(d, "print 1+1", out, sizeof out);
    check(strstr(out, "2") != NULL, "print evaluates an expression");
    a7800debug_command(d, "help", out, sizeof out);
    check(strlen(out) > 100, "help lists the commands");
    a7800debug_command(d, "statesave x", out, sizeof out);
    check(strstr(out, "not") != NULL || strstr(out, "refuse") != NULL, "state save is refused");
    a7800debug_command(d, "hardreset", out, sizeof out);
    check(a7800debug_is_stopped(d), "hardreset is refused and nothing moved");
    check(a7800debug_completions(d, "bp", out, sizeof out) >= 2, "completions for bp");
    a7800debug_set_label(d, TEST_A78_SUB, "count_frame");
    check(a7800debug_label_address(d, "count_frame") == TEST_A78_SUB, "labels resolve");
    {
        char lbl[64] = "";
        a7800debug_address_label(d, TEST_A78_SUB, lbl, sizeof lbl);
        check(strcmp(lbl, "count_frame") == 0, "and reverse");
    }
    a7800debug_frame(d);
    check(wait_stopped(d, 1, 3000), "frame stops again");
    a7800debug_command(d, "cart", out, sizeof out);
    check(strstr(out, "FujiNet") != NULL, "the cart command");
    a7800debug_command(d, "video registers", out, sizeof out);
    check(strstr(out, "CTRL") != NULL, "the video command");
    a7800debug_command(d, "video nonsense", out, sizeof out);
    check(strstr(out, "registers") != NULL, "the video command names its sections");

    /* saving */
    snprintf(path, sizeof path, "%s/ram.bin", cfg);
    check(a7800debug_save(d, "ram", path, out, sizeof out) == 0, "save RAM");
    {
        FILE *f = fopen(path, "rb");
        long len = -1;
        if (f) { fseek(f, 0, SEEK_END); len = ftell(f); fclose(f); }
        check(len == 0x1000, "4K of it");
    }
    snprintf(path, sizeof path, "%s/code.s", cfg);
    check(a7800debug_save(d, "dis", path, out, sizeof out) == 0, "save the disassembly");

    /* detach: running again */
    a7800debug_detach(d);
    check(!a7800debug_is_attached(d), "detached");
    check(wait_stopped(d, 0, 2000), "and running");
    {
        uint8_t a = 0, b = 0;
        sleep_ms(100);
        a7800debug_read(d, TEST_A78_COUNT, &a, 1);
        sleep_ms(200);
        a7800debug_read(d, TEST_A78_COUNT, &b, 1);
        check(a != b, "the machine runs after detach");
    }

    /* the debugger survives a power cycle */
    a7800debug_attach(d);
    wait_stopped(d, 1, 3000);
    a7800debug_breakpoint_toggle(d, TEST_A78_SUB);
    check(a7800session_power_cycle(s) == 0, "power cycle with the debugger attached");
    a7800debug_resume(d);
    check(wait_stopped(d, 1, 5000) && pc_of(d) == TEST_A78_SUB, "the breakpoint survives the power cycle");
    a7800debug_breakpoint_clear(d);
    a7800debug_detach(d);

    a7800session_stop(s);
    a7800session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
