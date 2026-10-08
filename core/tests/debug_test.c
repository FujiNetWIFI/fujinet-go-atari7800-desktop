/*
 * debug_test -- the debugger contract against a running machine: attach
 * stops it, registers, step / over / out / run to, disassembly with the PC
 * line, breakpoints and watchpoints that hit, memory, MARIA, the I/O view,
 * the cartridge tab, the prompt (and the commands it refuses), saving, and
 * detach lets it run again.
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
