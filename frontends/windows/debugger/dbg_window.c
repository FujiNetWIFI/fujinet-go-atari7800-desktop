/*
 * Debugger window (Win32) over MAME's debugger engine, via
 * core/include/a7800debug.h. Mirrors the GTK and Qt debuggers tab for tab --
 * Console, CPU & Memory, Disassembly, MARIA, I/O, Cartridge, Breakpoints --
 * built from plain common controls.
 *
 * MAME's debugger is always on in this app; showing the window attaches
 * (which stops the machine, as on every sibling) and hiding it detaches,
 * which lets the machine run on at full speed. The window refreshes on a
 * timer keyed to the engine's generation counter rather than on every tick.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a7800debug.h"

#define DISASM_WINDOW 64
#define MAX_BPS 128
#define MEM_BYTES 0x1000          /* the memory view shows 4K from its base */
#define GUTTER 18                 /* the disassembly's breakpoint column */

#define WM_DBG_ACCEPT (WM_APP + 2)   /* wp: control id -- Enter in an edit */
#define WM_DBG_TAB    (WM_APP + 3)   /* Tab in the prompt: complete */

#define TIMER_REFRESH 1

enum {
    PAGE_CONSOLE = 0, PAGE_CPU, PAGE_DISASM, PAGE_MARIA, PAGE_IO, PAGE_CART, PAGE_BREAKS,
    PAGE_COUNT
};

enum {
    IDC_RUN = 1000, IDC_STEP, IDC_OVER, IDC_OUT, IDC_FRAME, IDC_RUN_TO,
    IDC_STATUS, IDC_TABS,
    IDC_PROMPT_OUT, IDC_PROMPT_IN, IDC_LOAD_SYMBOLS, IDC_SAVE,
    IDC_REG0, IDC_REG_LAST = IDC_REG0 + 5,
    IDC_FLAG0, IDC_FLAG_LAST = IDC_FLAG0 + 5,
    IDC_CYCLES, IDC_MEM_VIEW, IDC_MEM_BASE, IDC_MEM_ADDR, IDC_MEM_VAL,
    IDC_FOLLOW_PC, IDC_JUMP, IDC_DISASM, IDC_TOGGLE_BP,
    IDC_MARIA_TEXT, IDC_MARIA_PIC,
    IDC_IO,
    IDC_BP_LIST, IDC_BP_TYPE, IDC_BP_START, IDC_BP_END, IDC_BP_COND, IDC_BP_ADD,
    IDC_BP_REMOVE, IDC_BP_ENABLE, IDC_BP_CLEAR,
    IDC_CART,
    IDC_SAVE_KIND0, IDC_SAVE_KIND_LAST = IDC_SAVE_KIND0 + 2,
    IDC_LABEL_FIRST
};

typedef struct {
    HWND hwnd;
    HWND tabs;
    HWND run_btn, step_btn, over_btn, out_btn, frame_btn, run_to_btn, status;

    HWND prompt_out, prompt_in, load_symbols, save;

    HWND reg_label[6], reg_edit[6], flag[6], cycles;
    HWND mem_label, mem_base, mem_view, mem_addr_label, mem_addr, mem_val_label, mem_val;

    HWND follow_pc, jump_label, jump, disasm_hint, disasm;

    HWND maria_text, maria_pic;

    HWND io;

    HWND bp_list, bp_type, bp_start_label, bp_start, bp_end_label, bp_end, bp_cond_label, bp_cond;
    HWND bp_add, bp_remove, bp_enable, bp_clear;

    HWND cart;

    HFONT mono, ui;
    HACCEL accel;
    HBRUSH accent;
    int line_h;

    a7800session *session;
    a7800debug *dbg;

    int page;
    unsigned seen_gen;
    int was_stopped;
    int running_ticks;

    int disasm_top;            /* address of the first disassembly line */
    a7800debug_line lines[DISASM_WINDOW];
    int line_count;
    int pc_line;
    int sel_addr;              /* the selected disassembly line's address, -1 none */

    int mem_base_addr;

    a7800debug_breakpoint bps[MAX_BPS];
    int nbps;

    /* The MARIA tab's picture: the palettes as the engine draws them, and
     * the 32 colour registers as written. */
    uint32_t pal_img[A7800DEBUG_PALETTE_WIDTH * A7800DEBUG_PALETTE_HEIGHT];
    int pal_ok;
    uint8_t pal_regs[32];
    uint32_t pal_rgb[32];
} debugger;

static debugger *g_dbg;

static const char *const kSaveKinds[3] = { "dis", "ram", "mem" };
static const char *const kSaveTitles[3] = { "Disassembly ($4000-$FFFF)...", "Console RAM ($1800-$27FF)...",
                                            "The whole 64K bus..." };

/* ---- helpers ---------------------------------------------------------------- */

static void set_text(HWND h, const char *text) { SetWindowTextA(h, text); }

/* EDIT controls want CRLF and render control characters as boxes. */
static char *to_crlf(const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = malloc(n * 2 + 1);
    if (!buf) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0x20 || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    return buf;
}

static void set_text_lf(HWND h, const char *text)
{
    char *buf = to_crlf(text);
    set_text(h, buf ? buf : text);
    free(buf);
}

/* Replace a view's text but keep where it was scrolled to. */
static void set_text_keep_scroll(HWND h, const char *text)
{
    const int first = (int)SendMessageA(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    set_text_lf(h, text);
    SendMessageA(h, EM_LINESCROLL, 0, first);
}

static void append_text_lf(HWND h, const char *text)
{
    char *buf = to_crlf(text);
    int len;
    if (!buf) return;
    len = GetWindowTextLengthA(h);
    SendMessageA(h, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(h, EM_REPLACESEL, FALSE, (LPARAM)buf);
    SendMessageA(h, EM_SCROLLCARET, 0, 0);
    free(buf);
}

static void edit_text(HWND h, char *out, int outsz) { GetWindowTextA(h, out, outsz); }

/* $hex, 0xhex, #dec, or bare hex, as the prompt reads numbers. */
static int parse_num(const char *text, long *out)
{
    char buf[64];
    const char *p;
    char *end;
    long v;
    int base = 16;

    snprintf(buf, sizeof buf, "%s", text);
    p = buf;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '$') p++;
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    else if (*p == '#') { p++; base = 10; }
    if (!*p) return 0;
    v = strtol(p, &end, base);
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end) return 0;
    *out = v;
    return 1;
}

static int resolve_addr(debugger *d, const char *text)
{
    long v;
    int addr = -1;
    const char *p = text;
    while (*p == ' ') p++;
    if (!*p) return -1;
    if (*p != '$' && *p != '#' && !(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')))
        addr = a7800debug_label_address(d->dbg, p);
    if (addr < 0 && parse_num(p, &v) && v >= 0 && v <= 0xffff) addr = (int)v;
    return addr;
}

static size_t addf(char *s, size_t len, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (len >= cap) return len;
    va_start(ap, fmt);
    n = vsnprintf(s + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) return len;
    return len + (size_t)n < cap ? len + (size_t)n : cap - 1;
}

/* ---- refreshers -------------------------------------------------------------- */

static void refresh_status(debugger *d)
{
    char reason[160], buf[220];
    int addr;
    const int stopped = a7800debug_is_stopped(d->dbg);
    a7800debug_stop_reason(d->dbg, reason, sizeof reason, &addr);
    if (stopped) snprintf(buf, sizeof buf, "Stopped%s%s", reason[0] ? ": " : "", reason);
    else snprintf(buf, sizeof buf, "Running");
    set_text(d->status, buf);
    set_text(d->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    InvalidateRect(d->run_btn, NULL, FALSE);
}

static void refresh_cpu(debugger *d)
{
    a7800debug_cpu c;
    char buf[160];
    int i;
    a7800debug_cpu_get(d->dbg, &c);
    {
        const int v[6] = { c.pc, c.sp, c.a, c.x, c.y, c.ps };
        for (i = 0; i < 6; i++) {
            if (GetFocus() == d->reg_edit[i]) continue;   /* do not fight the user's typing */
            snprintf(buf, sizeof buf, i == 0 ? "%04X" : "%02X", v[i]);
            set_text(d->reg_edit[i], buf);
        }
    }
    {
        const int flags[6] = { c.n, c.v, c.d, c.i, c.z, c.c };
        for (i = 0; i < 6; i++)
            SendMessageA(d->flag[i], BM_SETCHECK, flags[i] ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    snprintf(buf, sizeof buf, "cycle %llu   scanline %d   dot %d   frame %u",
             (unsigned long long)c.total_cycles, c.scanline, c.dot, c.frame);
    set_text(d->cycles, buf);
}

static void refresh_mem(debugger *d)
{
    static uint8_t mem[MEM_BYTES];
    static char text[MEM_BYTES / 16 * 84 + 128];
    size_t len = 0;
    int row, col, n;

    n = a7800debug_read(d->dbg, (uint16_t)d->mem_base_addr, mem,
                        d->mem_base_addr + MEM_BYTES > 0x10000 ? 0x10000 - d->mem_base_addr : MEM_BYTES);
    len = addf(text, len, sizeof text, "        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\r\n");
    for (row = 0; row * 16 < n; row++) {
        len = addf(text, len, sizeof text, "$%04X: ", d->mem_base_addr + row * 16);
        for (col = 0; col < 16; col++)
            len = addf(text, len, sizeof text, "%02X ", mem[row * 16 + col]);
        len = addf(text, len, sizeof text, " ");
        for (col = 0; col < 16; col++) {
            const uint8_t c = mem[row * 16 + col];
            len = addf(text, len, sizeof text, "%c", c >= 0x20 && c < 0x7f ? c : '.');
        }
        len = addf(text, len, sizeof text, "\r\n");
    }
    {
        /* keep the scroll position across a refresh */
        int first = (int)SendMessageA(d->mem_view, EM_GETFIRSTVISIBLELINE, 0, 0);
        set_text(d->mem_view, text);
        SendMessageA(d->mem_view, EM_LINESCROLL, 0, first);
    }
}

/* The disassembly is an owner-drawn list: one line an instruction, the PC's
 * in the accent colour, a dot in the gutter for a breakpoint. */
static void refresh_disasm(debugger *d)
{
    a7800debug_cpu c;
    int pc_line = -1, n, i, sel = -1;
    const int follow = SendMessageA(d->follow_pc, BM_GETCHECK, 0, 0) == BST_CHECKED;

    if (follow) {
        a7800debug_cpu_get(d->dbg, &c);
        d->disasm_top = a7800debug_row_address(d->dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 4));
    }
    n = a7800debug_disassemble(d->dbg, (uint16_t)d->disasm_top, d->lines, DISASM_WINDOW, &pc_line);
    d->line_count = n;
    d->pc_line = pc_line;

    SendMessageA(d->disasm, WM_SETREDRAW, FALSE, 0);
    SendMessageA(d->disasm, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < n; i++) {
        const a7800debug_line *l = &d->lines[i];
        char text[220];
        snprintf(text, sizeof text, "%04X  %-9s %-14s %s%s%s", l->address, l->bytes,
                 l->label[0] ? l->label : "", l->disasm, l->comment[0] ? "   ; " : "", l->comment);
        SendMessageA(d->disasm, LB_ADDSTRING, 0, (LPARAM)text);
        if (l->address == d->sel_addr) sel = i;
    }
    if (n == 0) SendMessageA(d->disasm, LB_ADDSTRING, 0, (LPARAM)"(no disassembly)");
    SendMessageA(d->disasm, LB_SETCURSEL, (WPARAM)sel, 0);
    SendMessageA(d->disasm, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(d->disasm, NULL, TRUE);
}

static const char *read_mode_name(int m)
{
    switch (m) {
    case 0: return "160A/160B";
    case 2: return "320B/320D";
    case 3: return "320A/320C";
    default: return "(reserved)";
    }
}

static void refresh_maria(debugger *d)
{
    a7800debug_maria m;
    static char s[16384];
    size_t len = 0;
    int p, i;

    a7800debug_maria_get(d->dbg, &m);
    len = addf(s, len, sizeof s,
        "Scanline %d   %s\n\n"
        "CTRL      $3C  %02X   DMA %s, colour kill %s, kangaroo %s, border %s\n"
        "                     %d-byte characters, read mode %s\n"
        "CHARBASE  $34  %02X\n"
        "DPP   $2C/$30  $%04X\n\n"
        "Now: DLL $%04X   DL $%04X   offset %d%s%s\n\n"
        "BACKGRND  $20  %02X\n",
        m.scanline, m.vblank ? "VBLANK" : "drawing",
        m.ctrl, m.dma_on ? "on" : "off", m.color_kill ? "on" : "off", m.kangaroo ? "on" : "off",
        m.border_control ? "black" : "background", m.char_width, read_mode_name(m.read_mode),
        m.charbase, m.dpp, m.dll, m.dl, m.offset, m.holey ? ", holey DMA" : "", m.dli ? ", DLI" : "",
        m.palette[0]);
    for (p = 0; p < 8; p++)
        len = addf(s, len, sizeof s, "P%dC1-3    $%02X  %02X %02X %02X\n", p, 0x21 + 4 * p,
                   m.palette[1 + 3 * p], m.palette[2 + 3 * p], m.palette[3 + 3 * p]);
    len = addf(s, len, sizeof s, "\nDisplay list list\n");
    if (len < sizeof s) a7800debug_dll_text(d->dbg, s + len, (int)(sizeof s - len));
    set_text_keep_scroll(d->maria_text, s);

    d->pal_ok = a7800debug_palette_image(d->dbg, d->pal_img);
    for (i = 0; i < 32; i++) {
        d->pal_regs[i] = m.palette[i];
        d->pal_rgb[i] = a7800debug_color(d->dbg, m.palette[i]);
    }
    InvalidateRect(d->maria_pic, NULL, FALSE);
}

static void dir_names(char *out, size_t outsz, int up, int down, int left, int right)
{
    snprintf(out, outsz, "%s%s%s%s%s", up ? " Up" : "", down ? " Down" : "", left ? " Left" : "",
             right ? " Right" : "", (up || down || left || right) ? "" : " (centred)");
}

static void refresh_io(debugger *d)
{
    static const char *const acts[A7800_ACT_PER_PORT] = { "Up", "Down", "Left", "Right", "Button 1", "Button 2" };
    a7800debug_io io;
    char s[3000], dirs[2][64];
    size_t len = 0;
    int i, port;

    a7800debug_io_get(d->dbg, &io);
    /* SWCHA reads low for a direction held: player 1 in the high nibble
     * (Right, Left, Down, Up from bit 7), player 2 in the low one. */
    dir_names(dirs[0], sizeof dirs[0], !(io.swcha & 0x10), !(io.swcha & 0x20), !(io.swcha & 0x40), !(io.swcha & 0x80));
    dir_names(dirs[1], sizeof dirs[1], !(io.swcha & 0x01), !(io.swcha & 0x02), !(io.swcha & 0x04), !(io.swcha & 0x08));
    len = addf(s, len, sizeof s,
        "INPTCTRL  $%02X  %s\n\n"
        "TIA sound   AUDC0 %02X  AUDF0 %02X  AUDV0 %02X\n"
        "            AUDC1 %02X  AUDF1 %02X  AUDV1 %02X\n\n"
        "RIOT SWCHA  $%02X   player 1:%s   player 2:%s\n"
        "     SWCHB  $%02X  %s%s%s%s   left difficulty %s, right difficulty %s\n\n"
        "TIA INPT0-5 ",
        io.inptctrl, io.inpt_locked ? "(locked)" : "(unlocked)",
        io.audc[0], io.audf[0], io.audv[0], io.audc[1], io.audf[1], io.audv[1],
        io.swcha, dirs[0], dirs[1],
        io.swchb, !(io.swchb & 0x01) ? " Reset" : "", !(io.swchb & 0x02) ? " Select" : "",
        !(io.swchb & 0x08) ? " Pause" : "", (io.swchb & 0x0b) == 0x0b ? " (no switch held)" : "",
        (io.swchb & 0x40) ? "A" : "B", (io.swchb & 0x80) ? "A" : "B");
    for (i = 0; i < 6; i++) len = addf(s, len, sizeof s, " %02X", io.inpt[i]);
    len = addf(s, len, sizeof s, "\n\n");
    if (io.pokey_present) {
        len = addf(s, len, sizeof s, "POKEY       AUDCTL %02X\n", io.pokey_audctl);
        for (i = 0; i < 4; i++)
            len = addf(s, len, sizeof s, "            AUDF%d %02X  AUDC%d %02X\n", i + 1, io.pokey_audf[i],
                       i + 1, io.pokey_audc[i]);
    } else {
        len = addf(s, len, sizeof s, "POKEY       none on this cartridge\n");
    }
    len = addf(s, len, sizeof s, "\n");
    for (port = 0; port < 2; port++) {
        const char *type = a7800_ctrl_type_name(io.port_type[port]);
        int any = 0;
        len = addf(s, len, sizeof s, "Player %d    %s; held:", port + 1, type ? type : "?");
        for (i = 0; i < A7800_ACT_PER_PORT; i++)
            if (io.held[port] & (1u << i)) { len = addf(s, len, sizeof s, " %s", acts[i]); any = 1; }
        len = addf(s, len, sizeof s, "%s\n", any ? "" : " nothing");
    }
    set_text_lf(d->io, s);
}

static void refresh_bps(debugger *d)
{
    int i, sel = (int)SendMessageA(d->bp_list, LB_GETCURSEL, 0, 0);
    d->nbps = a7800debug_breakpoint_list(d->dbg, d->bps, MAX_BPS);
    SendMessageA(d->bp_list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < d->nbps; i++) {
        const a7800debug_breakpoint *b = &d->bps[i];
        char line[256], kind[8] = "";
        if (b->type & A7800DEBUG_BP_EXEC) strcat(kind, "x");
        if (b->type & A7800DEBUG_BP_READ) strcat(kind, "r");
        if (b->type & A7800DEBUG_BP_WRITE) strcat(kind, "w");
        if (b->start == b->end)
            snprintf(line, sizeof line, "%c %4d  %-3s  $%04X        %s%s", b->enabled ? '+' : '-',
                     b->id, kind, b->start, b->condition[0] ? "if " : "", b->condition);
        else
            snprintf(line, sizeof line, "%c %4d  %-3s  $%04X-$%04X  %s%s", b->enabled ? '+' : '-',
                     b->id, kind, b->start, b->end, b->condition[0] ? "if " : "", b->condition);
        SendMessageA(d->bp_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (d->nbps == 0)
        SendMessageA(d->bp_list, LB_ADDSTRING, 0,
                     (LPARAM)"(no breakpoints or watchpoints -- F9 on a disassembly line, or add one below)");
    if (sel >= 0 && sel < d->nbps) SendMessageA(d->bp_list, LB_SETCURSEL, (WPARAM)sel, 0);
}

static void refresh_cart(debugger *d)
{
    a7800debug_cart c;
    char info[256];
    static char s[2048];
    a7800debug_cart_get(d->dbg, &c);
    a7800debug_cart_info(d->dbg, info, sizeof info);
    if (!c.present) { set_text_lf(d->cart, "No FujiNet cartridge is running."); return; }
    snprintf(s, sizeof s,
        "%s\n\n"
        "Link            %s\n"
        "Mailbox worker  %s\n"
        "Running         %s%s\n"
        "Hand-over       %s\n"
        "Loading         %d%%\n"
        "Live image      %s, CRC %08X\n"
        "Staged image    %s%s, CRC %08X\n"
        "High Score Cart %s%s%s%s\n"
        "TV              %s\n\n"
        "ACKSEQ $%02X   last error %u\n"
        "Boot state $%02X  %u%%  error %u\n"
        "Worker queue %u\n%s%s",
        info,
        c.link_up ? "up" : "down",
        c.worker ? "running" : "stopped",
        c.mode_name, c.booted_image ? " (a booted game)" : "",
        c.handover == 1 ? "the BIOS started it" : c.handover == 2 ? "the loader started it" : "none yet",
        c.load_pct,
        c.kind[0] ? c.kind : "-", c.live_crc,
        c.staged ? "" : "none ", c.staged ? c.staged_kind : "", c.staged_crc,
        (c.hsc & 1) ? "ROM installed" : "no ROM", (c.hsc & 2) ? ", on" : ", off",
        (c.hsc & 4) ? ", saved" : "", (c.hsc & 8) ? ", unsaved scores" : "",
        c.tv ? "PAL" : "NTSC",
        c.ackseq, c.last_error, c.boot_state, c.boot_pct, c.boot_err, c.queue_depth,
        c.link_error[0] ? "\nLink: " : "", c.link_error);
    set_text_lf(d->cart, s);
}

static void refresh_all(debugger *d)
{
    refresh_status(d);
    refresh_cpu(d);
    refresh_mem(d);
    refresh_disasm(d);
    refresh_maria(d);
    refresh_io(d);
    refresh_bps(d);
    refresh_cart(d);
}

/* ---- actions ------------------------------------------------------------------ */

static void toggle_run(debugger *d)
{
    if (a7800debug_is_stopped(d->dbg)) a7800debug_resume(d->dbg);
    else a7800debug_stop(d->dbg);
    refresh_all(d);
}

static void run_prompt(debugger *d)
{
    static char out[65536];
    char cmd[512], echo[540];
    edit_text(d->prompt_in, cmd, sizeof cmd);
    if (!cmd[0]) return;
    snprintf(echo, sizeof echo, "> %s\n", cmd);
    append_text_lf(d->prompt_out, echo);
    a7800debug_command(d->dbg, cmd, out, sizeof out);
    append_text_lf(d->prompt_out, out);
    append_text_lf(d->prompt_out, "\n");
    set_text(d->prompt_in, "");
    refresh_all(d);
}

static void complete_prompt(debugger *d)
{
    char text[512], comps[4096];
    const char *word;
    char *sp;
    int n;
    edit_text(d->prompt_in, text, sizeof text);
    sp = strrchr(text, ' ');
    word = sp ? sp + 1 : text;
    n = a7800debug_completions(d->dbg, word, comps, sizeof comps);
    if (n == 1) {
        char line[256], joined[800];
        char *nl;
        snprintf(line, sizeof line, "%.255s", comps);
        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (sp) sp[1] = '\0'; else text[0] = '\0';
        snprintf(joined, sizeof joined, "%s%s ", text, line);
        set_text(d->prompt_in, joined);
        SendMessageA(d->prompt_in, EM_SETSEL, (WPARAM)strlen(joined), (LPARAM)strlen(joined));
    } else if (n > 1) {
        append_text_lf(d->prompt_out, comps);
    }
}

static void jump_to(debugger *d, const char *text)
{
    const int addr = resolve_addr(d, text);
    if (addr < 0) {
        append_text_lf(d->prompt_out, "No such label or address.\n");
        return;
    }
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
    d->disasm_top = addr;
    d->sel_addr = addr;
    refresh_disasm(d);
}

/* F9: the selected disassembly line's breakpoint, on or off. */
static void toggle_selected_breakpoint(debugger *d)
{
    if (d->sel_addr < 0) {
        set_text(d->status, "Select a disassembly line first");
        return;
    }
    a7800debug_breakpoint_toggle(d->dbg, (uint16_t)d->sel_addr);
    refresh_disasm(d);
    refresh_bps(d);
}

static void run_to_selected(debugger *d)
{
    if (d->sel_addr < 0) {
        set_text(d->status, "Select a disassembly line to run to");
        return;
    }
    a7800debug_run_to(d->dbg, (uint16_t)d->sel_addr);
    refresh_all(d);
}

static int pick_file(debugger *d, int save, const char *title, const char *filter, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = d->hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    if (save) {
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        return GetSaveFileNameA(&ofn) ? 1 : 0;
    }
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static void save_file(debugger *d, int which)
{
    char path[MAX_PATH], msg[512], title[64];
    snprintf(title, sizeof title, "Save %s", kSaveTitles[which]);
    if (!pick_file(d, 1, title, "All files\0*.*\0\0", path, sizeof path)) return;
    a7800debug_save(d->dbg, kSaveKinds[which], path, msg, sizeof msg);
    append_text_lf(d->prompt_out, msg);
    append_text_lf(d->prompt_out, "\n");
}

static void save_menu(debugger *d)
{
    HMENU m = CreatePopupMenu();
    RECT r;
    int i;
    for (i = 0; i < 3; i++) AppendMenuA(m, MF_STRING, (UINT_PTR)(IDC_SAVE_KIND0 + i), kSaveTitles[i]);
    GetWindowRect(d->save, &r);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, d->hwnd, NULL);
    DestroyMenu(m);
}

static void load_symbols(debugger *d)
{
    char path[MAX_PATH], msg[512];
    if (!pick_file(d, 0, "Load symbols",
                   "Symbol files (*.lbl;*.dbg;*.sym;*.labels)\0*.lbl;*.dbg;*.sym;*.labels\0All files\0*.*\0\0",
                   path, sizeof path))
        return;
    a7800debug_load_symbols(d->dbg, path, msg, sizeof msg);
    append_text_lf(d->prompt_out, msg);
    append_text_lf(d->prompt_out, "\n");
    refresh_all(d);
}

static void add_breakpoint(debugger *d)
{
    char a[64], b[64], cond[256];
    static const int types[4] = { A7800DEBUG_BP_EXEC, A7800DEBUG_BP_READ, A7800DEBUG_BP_WRITE,
                                  A7800DEBUG_BP_READ | A7800DEBUG_BP_WRITE };
    int start, end, sel, id;
    edit_text(d->bp_start, a, sizeof a);
    edit_text(d->bp_end, b, sizeof b);
    edit_text(d->bp_cond, cond, sizeof cond);
    start = resolve_addr(d, a);
    if (start < 0) { MessageBoxA(d->hwnd, "The start is not an address or label.", "Breakpoint", MB_ICONWARNING); return; }
    end = b[0] ? resolve_addr(d, b) : start;
    if (end < start) end = start;
    sel = (int)SendMessageA(d->bp_type, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel > 3) sel = 0;
    id = a7800debug_breakpoint_add(d->dbg, types[sel], (uint16_t)start, (uint16_t)end, cond);
    if (id < 0) { MessageBoxA(d->hwnd, "The condition does not parse.", "Breakpoint", MB_ICONWARNING); return; }
    set_text(d->bp_start, "");
    set_text(d->bp_end, "");
    set_text(d->bp_cond, "");
    refresh_bps(d);
    refresh_disasm(d);
}

static void on_accept(debugger *d, int id)
{
    char buf[512];
    long v;

    if (id >= IDC_REG0 && id <= IDC_REG_LAST) {
        static const int reg_ids[6] = { A7800_REG_PC, A7800_REG_SP, A7800_REG_A, A7800_REG_X, A7800_REG_Y, A7800_REG_PS };
        edit_text(d->reg_edit[id - IDC_REG0], buf, sizeof buf);
        if (parse_num(buf, &v)) a7800debug_cpu_set(d->dbg, reg_ids[id - IDC_REG0], (int)v);
        SetFocus(d->hwnd);
        refresh_all(d);
        return;
    }
    switch (id) {
    case IDC_PROMPT_IN: run_prompt(d); break;
    case IDC_MEM_BASE: {
        int a;
        edit_text(d->mem_base, buf, sizeof buf);
        a = resolve_addr(d, buf);
        if (a >= 0) {
            d->mem_base_addr = a & 0xfff0;
            SendMessageA(d->mem_view, EM_LINESCROLL, 0, -0x7fff);
            refresh_mem(d);
        }
        break;
    }
    case IDC_MEM_ADDR:
    case IDC_MEM_VAL: {
        char abuf[64];
        int a;
        edit_text(d->mem_addr, abuf, sizeof abuf);
        edit_text(d->mem_val, buf, sizeof buf);
        a = resolve_addr(d, abuf);
        if (a >= 0 && parse_num(buf, &v)) a7800debug_write(d->dbg, (uint16_t)a, (uint8_t)v);
        refresh_all(d);
        break;
    }
    case IDC_JUMP:
        edit_text(d->jump, buf, sizeof buf);
        jump_to(d, buf);
        break;
    case IDC_BP_START:
    case IDC_BP_END:
    case IDC_BP_COND:
        add_breakpoint(d);
        break;
    default: break;
    }
}

/* ---- the MARIA picture --------------------------------------------------------------- */

static void paint_maria(debugger *d, HDC dc, const RECT *c)
{
    HFONT old = (HFONT)SelectObject(dc, d->mono);
    int x0 = 8, y = 8, p, k;

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    if (d->pal_ok) {
        /* The engine's own picture of the palettes, twice size. */
        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = A7800DEBUG_PALETTE_WIDTH;
        bmi.bmiHeader.biHeight = -A7800DEBUG_PALETTE_HEIGHT;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, x0, y, A7800DEBUG_PALETTE_WIDTH * 2, A7800DEBUG_PALETTE_HEIGHT * 2,
                      0, 0, A7800DEBUG_PALETTE_WIDTH, A7800DEBUG_PALETTE_HEIGHT, d->pal_img, &bmi,
                      DIB_RGB_COLORS, SRCCOPY);
        y += A7800DEBUG_PALETTE_HEIGHT * 2 + 14;
    }

    /* The colour registers as written: BACKGRND, then P0..P7's three. */
    for (p = -1; p < 8 && y + 22 < c->bottom; p++) {
        char name[16];
        RECT label = { x0, y, x0 + 80, y + 20 };
        snprintf(name, sizeof name, p < 0 ? "BACKGRND" : "P%d", p);
        DrawTextA(dc, name, -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        for (k = 0; k < (p < 0 ? 1 : 3); k++) {
            const int i = p < 0 ? 0 : 1 + 3 * p + k;
            const uint32_t rgb = d->pal_rgb[i];
            HBRUSH b = CreateSolidBrush(RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff));
            RECT sw = { x0 + 84 + k * 96, y, x0 + 84 + k * 96 + 28, y + 20 };
            RECT val = { sw.right + 6, y, sw.right + 60, y + 20 };
            char hex[8];
            FillRect(dc, &sw, b);
            FrameRect(dc, &sw, (HBRUSH)GetStockObject(GRAY_BRUSH));
            DeleteObject(b);
            snprintf(hex, sizeof hex, "$%02X", d->pal_regs[i]);
            DrawTextA(dc, hex, -1, &val, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        y += 24;
    }
    SelectObject(dc, old);
}

static LRESULT CALLBACK pic_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c;
        GetClientRect(hwnd, &c);
        FillRect(dc, &c, (HBRUSH)(COLOR_BTNFACE + 1));
        if (d) paint_maria(d, dc, &c);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- the disassembly list ------------------------------------------------------------ */

static void draw_disasm_line(debugger *d, const DRAWITEMSTRUCT *di)
{
    RECT r = di->rcItem, text;
    char line[256] = "";
    const int i = (int)di->itemID;
    const int is_pc = i >= 0 && i == d->pc_line;
    const int has_bp = i >= 0 && i < d->line_count && d->lines[i].has_breakpoint;
    const int selected = (di->itemState & ODS_SELECTED) != 0;
    COLORREF fg;

    if (i < 0) return;
    if (is_pc) {
        FillRect(di->hDC, &r, d->accent);
        fg = RGB(255, 255, 255);
    } else if (selected) {
        FillRect(di->hDC, &r, GetSysColorBrush(COLOR_HIGHLIGHT));
        fg = GetSysColor(COLOR_HIGHLIGHTTEXT);
    } else {
        FillRect(di->hDC, &r, GetSysColorBrush(COLOR_WINDOW));
        fg = GetSysColor(COLOR_WINDOWTEXT);
    }
    if (has_bp) {
        /* a red dot in the gutter */
        HBRUSH red = CreateSolidBrush(RGB(0xd0, 0x20, 0x20));
        HGDIOBJ ob = SelectObject(di->hDC, red), op = SelectObject(di->hDC, GetStockObject(NULL_PEN));
        const int sz = d->line_h - 6 > 6 ? d->line_h - 6 : 6, cy = (r.top + r.bottom) / 2;
        Ellipse(di->hDC, r.left + 4, cy - sz / 2, r.left + 4 + sz, cy - sz / 2 + sz);
        SelectObject(di->hDC, op);
        SelectObject(di->hDC, ob);
        DeleteObject(red);
    }
    if (selected && is_pc) {
        RECT f = r;
        InflateRect(&f, -1, -1);
        FrameRect(di->hDC, &f, GetSysColorBrush(COLOR_HIGHLIGHT));
    }
    SendMessageA(di->hwndItem, LB_GETTEXT, (WPARAM)i, (LPARAM)line);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, fg);
    text = r;
    text.left += GUTTER + 2;
    DrawTextA(di->hDC, line, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_EXPANDTABS);
}

static WNDPROC g_edit_proc;
static WNDPROC g_disasm_proc;

/* A click in the gutter toggles that line's breakpoint; anywhere else
 * selects the line (for F9 and Run To). The wheel and the arrows browse,
 * and turn Follow PC off, as in the other frontends. */
static LRESULT CALLBACK disasm_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d) return CallWindowProcA(g_disasm_proc, hwnd, msg, wp, lp);
    if (msg == WM_LBUTTONDOWN && GET_X_LPARAM(lp) < GUTTER) {
        const LRESULT hitres = SendMessageA(hwnd, LB_ITEMFROMPOINT, 0, lp);
        const int line = LOWORD(hitres);
        if (!HIWORD(hitres) && line < d->line_count) {
            a7800debug_breakpoint_toggle(d->dbg, d->lines[line].address);
            refresh_disasm(d);
            refresh_bps(d);
        }
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        int rows = -GET_WHEEL_DELTA_WPARAM(wp) / 40;
        SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
        if (rows) d->disasm_top = a7800debug_row_address(d->dbg, (uint16_t)d->disasm_top, rows);
        refresh_disasm(d);
        return 0;
    }
    if (msg == WM_KEYDOWN && (wp == VK_PRIOR || wp == VK_NEXT)) {
        const int rows = wp == VK_PRIOR ? -DISASM_WINDOW / 2 : DISASM_WINDOW / 2;
        SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
        d->disasm_top = a7800debug_row_address(d->dbg, (uint16_t)d->disasm_top, rows);
        refresh_disasm(d);
        return 0;
    }
    return CallWindowProcA(g_disasm_proc, hwnd, msg, wp, lp);
}

/* ---- edit subclassing ------------------------------------------------------------- */

/* Enter in a single-line field means "apply this value"; Tab in the prompt
 * completes. Neither may beep its way through the default handler. */
static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int id = (int)GetWindowLongPtrA(hwnd, GWLP_ID);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_ACCEPT, (WPARAM)id, 0);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN) return 0;
    if (id == IDC_PROMPT_IN && msg == WM_KEYDOWN && wp == VK_TAB) {
        PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_TAB, 0, 0);
        return 0;
    }
    if (id == IDC_PROMPT_IN && msg == WM_CHAR && wp == VK_TAB) return 0;
    if (id == IDC_PROMPT_IN && msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return CallWindowProcA(g_edit_proc, hwnd, msg, wp, lp);
}

/* ---- construction ------------------------------------------------------------------ */

static HWND child(debugger *d, const char *cls, const char *text, DWORD style, int id, HFONT font)
{
    HWND h = CreateWindowExA(0, cls, text, WS_CHILD | style, 0, 0, 10, 10, d->hwnd,
                             (HMENU)(INT_PTR)id, (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE), NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

static HWND mono_view(debugger *d, int id, int wrap)
{
    return child(d, "EDIT", "",
                 WS_BORDER | WS_VSCROLL | (wrap ? 0 : WS_HSCROLL) | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                 id, d->mono);
}

static HWND field(debugger *d, int id, HFONT font)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, id, font);
    g_edit_proc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)edit_subclass);
    return h;
}

static HWND label(debugger *d, const char *text)
{
    static int next_id;
    return child(d, "STATIC", text, SS_LEFT, IDC_LABEL_FIRST + next_id++, d->ui);
}

static HWND button(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, id, d->ui);
}

static HWND checkbox(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, id, d->ui);
}

static HWND combo(debugger *d, int id, const char *const *items, int n)
{
    HWND c = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id, d->ui);
    int i;
    for (i = 0; i < n; i++) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
    SendMessageA(c, CB_SETCURSEL, 0, 0);
    return c;
}

static void build_controls(debugger *d)
{
    TCITEMA item;
    static const char *const tabs[PAGE_COUNT] = { "Console", "CPU && Memory", "Disassembly", "MARIA",
                                                  "I/O", "Cartridge", "Breakpoints && Watchpoints" };
    static const char *const reg_names[6] = { "PC", "SP", "A", "X", "Y", "P" };
    static const char *const flag_names[6] = { "N", "V", "D", "I", "Z", "C" };
    static const char *const bp_types[4] = { "Execute", "Read", "Write", "Read/Write" };
    int i;

    d->tabs = child(d, WC_TABCONTROLA, "", WS_VISIBLE | WS_CLIPSIBLINGS, IDC_TABS, d->ui);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (i = 0; i < PAGE_COUNT; i++) {
        item.pszText = (char *)tabs[i];
        SendMessageA(d->tabs, TCM_INSERTITEMA, (WPARAM)i, (LPARAM)&item);
    }

    /* Toolbar. Run/Stop is owner-drawn so it can wear the accent colour
     * while the machine is stopped. */
    d->run_btn = child(d, "BUTTON", "Stop (F5)", BS_OWNERDRAW | WS_TABSTOP, IDC_RUN, d->ui);
    d->step_btn = button(d, "Step (F7)", IDC_STEP);
    d->over_btn = button(d, "Over (F8)", IDC_OVER);
    d->out_btn = button(d, "Out (Shift+F8)", IDC_OUT);
    d->frame_btn = button(d, "Frame", IDC_FRAME);
    d->run_to_btn = button(d, "Run To Cursor", IDC_RUN_TO);
    d->status = child(d, "STATIC", "Running", SS_RIGHT | SS_ENDELLIPSIS, IDC_STATUS, d->ui);
    {
        HWND bar[7] = { d->run_btn, d->step_btn, d->over_btn, d->out_btn, d->frame_btn, d->run_to_btn, d->status };
        for (i = 0; i < 7; i++) ShowWindow(bar[i], SW_SHOW);
    }

    /* Console */
    d->prompt_out = mono_view(d, IDC_PROMPT_OUT, 1);
    set_text(d->prompt_out, "MAME's debugger console. Type 'help' for every command; 'cart', 'maria' and\r\n"
                            "'labels <file>' are this app's own.\r\n");
    d->prompt_in = field(d, IDC_PROMPT_IN, d->mono);
    SendMessageA(d->prompt_in, EM_SETCUEBANNER, TRUE,
                 (LPARAM)L"command (help, step, bpset, wpset, print, dump, go, ...) - Tab completes");
    d->load_symbols = button(d, "Load symbols...", IDC_LOAD_SYMBOLS);
    d->save = button(d, "Save...", IDC_SAVE);

    /* CPU & Memory */
    for (i = 0; i < 6; i++) {
        d->reg_label[i] = label(d, reg_names[i]);
        d->reg_edit[i] = field(d, IDC_REG0 + i, d->mono);
        SendMessageA(d->reg_edit[i], EM_SETLIMITTEXT, 6, 0);
    }
    for (i = 0; i < 6; i++) d->flag[i] = checkbox(d, flag_names[i], IDC_FLAG0 + i);
    d->cycles = label(d, "");
    d->mem_label = label(d, "Memory from");
    d->mem_base = field(d, IDC_MEM_BASE, d->mono);
    SendMessageA(d->mem_base, EM_SETCUEBANNER, TRUE, (LPARAM)L"$1800");
    d->mem_view = mono_view(d, IDC_MEM_VIEW, 0);
    d->mem_addr_label = label(d, "Write address");
    d->mem_addr = field(d, IDC_MEM_ADDR, d->mono);
    d->mem_val_label = label(d, "value");
    d->mem_val = field(d, IDC_MEM_VAL, d->mono);

    /* Disassembly */
    d->follow_pc = checkbox(d, "Follow PC", IDC_FOLLOW_PC);
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_CHECKED, 0);
    d->jump_label = label(d, "Jump to");
    d->jump = field(d, IDC_JUMP, d->mono);
    d->disasm_hint = label(d, "F9 or a click in the gutter toggles a breakpoint; scroll to browse");
    d->disasm = child(d, "LISTBOX", "",
                      WS_BORDER | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | WS_TABSTOP,
                      IDC_DISASM, d->mono);
    g_disasm_proc = (WNDPROC)SetWindowLongPtrA(d->disasm, GWLP_WNDPROC, (LONG_PTR)disasm_subclass);

    /* MARIA */
    d->maria_text = mono_view(d, IDC_MARIA_TEXT, 0);
    d->maria_pic = child(d, "A7800DbgMaria", "", 0, IDC_MARIA_PIC, d->ui);

    /* I/O */
    d->io = mono_view(d, IDC_IO, 1);

    /* Cartridge */
    d->cart = mono_view(d, IDC_CART, 1);

    /* Breakpoints & Watchpoints */
    d->bp_list = child(d, "LISTBOX", "", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP, IDC_BP_LIST, d->mono);
    d->bp_type = combo(d, IDC_BP_TYPE, bp_types, 4);
    d->bp_start_label = label(d, "Address");
    d->bp_start = field(d, IDC_BP_START, d->mono);
    d->bp_end_label = label(d, "to");
    d->bp_end = field(d, IDC_BP_END, d->mono);
    d->bp_cond_label = label(d, "if");
    d->bp_cond = field(d, IDC_BP_COND, d->mono);
    SendMessageA(d->bp_cond, EM_SETCUEBANNER, TRUE, (LPARAM)L"optional condition, e.g. a == $FF");
    d->bp_add = button(d, "Add", IDC_BP_ADD);
    d->bp_remove = button(d, "Remove", IDC_BP_REMOVE);
    d->bp_enable = button(d, "Enable/Disable", IDC_BP_ENABLE);
    d->bp_clear = button(d, "Clear all", IDC_BP_CLEAR);
}

static void show_page(debugger *d, int page)
{
    HWND prompt[] = { d->prompt_out, d->prompt_in, d->load_symbols, d->save };
    HWND cpu[] = { d->cycles, d->mem_label, d->mem_base, d->mem_view, d->mem_addr_label, d->mem_addr,
                   d->mem_val_label, d->mem_val };
    HWND dis[] = { d->follow_pc, d->jump_label, d->jump, d->disasm_hint, d->disasm };
    HWND maria[] = { d->maria_text, d->maria_pic };
    HWND brk[] = { d->bp_list, d->bp_type, d->bp_start_label, d->bp_start, d->bp_end_label, d->bp_end,
                   d->bp_cond_label, d->bp_cond, d->bp_add, d->bp_remove, d->bp_enable, d->bp_clear };
    size_t i;
    d->page = page;
#define SHOW(arr, p) for (i = 0; i < sizeof(arr) / sizeof((arr)[0]); i++) ShowWindow((arr)[i], page == (p) ? SW_SHOW : SW_HIDE)
    SHOW(prompt, PAGE_CONSOLE);
    SHOW(cpu, PAGE_CPU);
    for (i = 0; i < 6; i++) {
        ShowWindow(d->reg_label[i], page == PAGE_CPU ? SW_SHOW : SW_HIDE);
        ShowWindow(d->reg_edit[i], page == PAGE_CPU ? SW_SHOW : SW_HIDE);
        ShowWindow(d->flag[i], page == PAGE_CPU ? SW_SHOW : SW_HIDE);
    }
    SHOW(dis, PAGE_DISASM);
    SHOW(maria, PAGE_MARIA);
    ShowWindow(d->io, page == PAGE_IO ? SW_SHOW : SW_HIDE);
    ShowWindow(d->cart, page == PAGE_CART ? SW_SHOW : SW_HIDE);
    SHOW(brk, PAGE_BREAKS);
#undef SHOW
}

static void layout(debugger *d)
{
    RECT client, page;
    int y = 8, bx = 8, px, py, pw, ph, i;

    GetClientRect(d->hwnd, &client);

    MoveWindow(d->run_btn, bx, y, 96, 26, TRUE);      bx += 102;
    MoveWindow(d->step_btn, bx, y, 90, 26, TRUE);     bx += 96;
    MoveWindow(d->over_btn, bx, y, 90, 26, TRUE);     bx += 96;
    MoveWindow(d->out_btn, bx, y, 110, 26, TRUE);     bx += 116;
    MoveWindow(d->frame_btn, bx, y, 70, 26, TRUE);    bx += 76;
    MoveWindow(d->run_to_btn, bx, y, 110, 26, TRUE);  bx += 116;
    MoveWindow(d->status, bx, y + 5, client.right - bx - 8 > 40 ? client.right - bx - 8 : 40, 20, TRUE);

    MoveWindow(d->tabs, 8, 40, client.right - 16, client.bottom - 48, TRUE);
    page = (RECT){ 8, 40, client.right - 8, client.bottom - 8 };
    SendMessageA(d->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    px = page.left + 4; py = page.top + 4;
    pw = page.right - page.left - 8; ph = page.bottom - page.top - 8;
    if (pw < 200) pw = 200;
    if (ph < 200) ph = 200;

    /* Console */
    MoveWindow(d->prompt_out, px, py, pw, ph - 32, TRUE);
    MoveWindow(d->prompt_in, px, py + ph - 26, pw - 236, 24, TRUE);
    MoveWindow(d->load_symbols, px + pw - 228, py + ph - 27, 120, 26, TRUE);
    MoveWindow(d->save, px + pw - 102, py + ph - 27, 102, 26, TRUE);

    /* CPU & Memory */
    {
        int x = px, ry = py;
        for (i = 0; i < 6; i++) {
            MoveWindow(d->reg_label[i], x, ry + 4, 24, 18, TRUE); x += 26;
            MoveWindow(d->reg_edit[i], x, ry, 62, 24, TRUE); x += 74;
        }
        ry += 32;
        x = px;
        for (i = 0; i < 6; i++) { MoveWindow(d->flag[i], x, ry, 40, 22, TRUE); x += 44; }
        MoveWindow(d->cycles, x + 8, ry + 3, pw - (x - px) - 8, 18, TRUE);
        ry += 30;
        MoveWindow(d->mem_label, px, ry + 4, 80, 18, TRUE);
        MoveWindow(d->mem_base, px + 84, ry, 90, 24, TRUE);
        ry += 30;
        MoveWindow(d->mem_view, px, ry, pw, ph - (ry - py) - 34, TRUE);
        ry = py + ph - 26;
        MoveWindow(d->mem_addr_label, px, ry + 4, 90, 18, TRUE);
        MoveWindow(d->mem_addr, px + 94, ry, 80, 24, TRUE);
        MoveWindow(d->mem_val_label, px + 184, ry + 4, 40, 18, TRUE);
        MoveWindow(d->mem_val, px + 226, ry, 60, 24, TRUE);
    }

    /* Disassembly */
    MoveWindow(d->follow_pc, px, py + 2, 90, 22, TRUE);
    MoveWindow(d->jump_label, px + 96, py + 4, 50, 18, TRUE);
    MoveWindow(d->jump, px + 148, py, 150, 24, TRUE);
    MoveWindow(d->disasm_hint, px + 308, py + 4, pw - 308 > 40 ? pw - 308 : 40, 18, TRUE);
    MoveWindow(d->disasm, px, py + 30, pw, ph - 30, TRUE);

    /* MARIA: registers and the display list left, the colours right */
    {
        const int rw = A7800DEBUG_PALETTE_WIDTH * 2 + 24;
        const int lw = pw - rw - 10 > 300 ? pw - rw - 10 : 300;
        MoveWindow(d->maria_text, px, py, lw, ph, TRUE);
        MoveWindow(d->maria_pic, px + lw + 10, py, pw - lw - 10, ph, TRUE);
    }

    /* I/O, Cartridge */
    MoveWindow(d->io, px, py, pw, ph, TRUE);
    MoveWindow(d->cart, px, py, pw, ph, TRUE);

    /* Breakpoints & Watchpoints */
    {
        int ry = py + ph - 62;
        MoveWindow(d->bp_list, px, py, pw, ry - py - 6, TRUE);
        MoveWindow(d->bp_type, px, ry, 100, 200, TRUE);
        MoveWindow(d->bp_start_label, px + 108, ry + 4, 50, 18, TRUE);
        MoveWindow(d->bp_start, px + 160, ry, 90, 24, TRUE);
        MoveWindow(d->bp_end_label, px + 256, ry + 4, 18, 18, TRUE);
        MoveWindow(d->bp_end, px + 276, ry, 90, 24, TRUE);
        MoveWindow(d->bp_cond_label, px + 374, ry + 4, 14, 18, TRUE);
        MoveWindow(d->bp_cond, px + 390, ry, pw - 390 - 70 > 80 ? pw - 390 - 70 : 80, 24, TRUE);
        MoveWindow(d->bp_add, px + pw - 64, ry - 1, 64, 26, TRUE);
        ry += 32;
        MoveWindow(d->bp_remove, px, ry, 90, 26, TRUE);
        MoveWindow(d->bp_enable, px + 96, ry, 120, 26, TRUE);
        MoveWindow(d->bp_clear, px + 222, ry, 90, 26, TRUE);
    }
}

/* ---- attach / detach -------------------------------------------------------------- */

static void attach(debugger *d)
{
    a7800debug_attach(d->dbg);
    d->seen_gen = a7800debug_generation(d->dbg);
    d->was_stopped = 1;
    refresh_all(d);
    SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);
}

static void detach_and_hide(debugger *d)
{
    KillTimer(d->hwnd, TIMER_REFRESH);
    a7800debug_detach(d->dbg);
    ShowWindow(d->hwnd, SW_HIDE);
}

/* ---- window proc -------------------------------------------------------------------- */

static void draw_run_button(debugger *d, const DRAWITEMSTRUCT *di)
{
    char text[32];
    const int stopped = a7800debug_is_stopped(d->dbg);
    RECT r = di->rcItem;
    if (stopped) {
        FillRect(di->hDC, &r, d->accent);
        FrameRect(di->hDC, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(di->hDC, RGB(255, 255, 255));
    } else {
        DrawFrameControl(di->hDC, &r, DFC_BUTTON, DFCS_BUTTONPUSH | ((di->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
        SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
    }
    SetBkMode(di->hDC, TRANSPARENT);
    GetWindowTextA(di->hwndItem, text, sizeof text);
    DrawTextA(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (di->itemState & ODS_FOCUS) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(di->hDC, &r);
    }
}

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d || d->hwnd != hwnd) return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        layout(d);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_REFRESH && IsWindowVisible(hwnd)) {
            const unsigned gen = a7800debug_generation(d->dbg);
            const int stopped = a7800debug_is_stopped(d->dbg);
            if (gen != d->seen_gen || stopped != d->was_stopped) {
                d->seen_gen = gen;
                d->was_stopped = stopped;
                refresh_all(d);
            } else if (!stopped && ++d->running_ticks >= 5) {
                d->running_ticks = 0;
                refresh_status(d);
                if (d->page == PAGE_CPU) { refresh_cpu(d); refresh_mem(d); }
                else if (d->page == PAGE_DISASM) refresh_disasm(d);
                else if (d->page == PAGE_MARIA) refresh_maria(d);
                else if (d->page == PAGE_IO) refresh_io(d);
                else if (d->page == PAGE_CART) refresh_cart(d);
            }
        }
        return 0;
    case WM_DBG_ACCEPT:
        on_accept(d, (int)wp);
        return 0;
    case WM_DBG_TAB:
        complete_prompt(d);
        return 0;
    case WM_NOTIFY:
        if (((LPNMHDR)lp)->code == (UINT)TCN_SELCHANGE) {
            show_page(d, (int)SendMessageA(d->tabs, TCM_GETCURSEL, 0, 0));
            layout(d);
            return 0;
        }
        break;
    case WM_MEASUREITEM:
        if (wp == IDC_DISASM) {
            ((MEASUREITEMSTRUCT *)lp)->itemHeight = (UINT)d->line_h;
            return TRUE;
        }
        break;
    case WM_DRAWITEM:
        if (wp == IDC_RUN) { draw_run_button(d, (const DRAWITEMSTRUCT *)lp); return TRUE; }
        if (wp == IDC_DISASM) { draw_disasm_line(d, (const DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == d->status || (HWND)lp == d->cycles || (HWND)lp == d->disasm_hint) {
            SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= IDC_FLAG0 && id <= IDC_FLAG_LAST && HIWORD(wp) == BN_CLICKED) {
            static const int flag_ids[6] = { A7800_FLAG_N, A7800_FLAG_V, A7800_FLAG_D, A7800_FLAG_I, A7800_FLAG_Z, A7800_FLAG_C };
            if (a7800debug_is_stopped(d->dbg))
                a7800debug_cpu_set(d->dbg, flag_ids[id - IDC_FLAG0],
                                 SendMessageA(d->flag[id - IDC_FLAG0], BM_GETCHECK, 0, 0) == BST_CHECKED);
            refresh_cpu(d);
            return 0;
        }
        if (id >= IDC_SAVE_KIND0 && id <= IDC_SAVE_KIND_LAST) { save_file(d, id - IDC_SAVE_KIND0); return 0; }
        switch (id) {
        case IDC_RUN: toggle_run(d); return 0;
        case IDC_STEP: a7800debug_step(d->dbg); refresh_all(d); return 0;
        case IDC_OVER: a7800debug_step_over(d->dbg); refresh_all(d); return 0;
        case IDC_OUT: a7800debug_step_out(d->dbg); refresh_all(d); return 0;
        case IDC_FRAME: a7800debug_frame(d->dbg); refresh_all(d); return 0;
        case IDC_RUN_TO: run_to_selected(d); return 0;
        case IDC_TOGGLE_BP: toggle_selected_breakpoint(d); return 0;
        case IDC_LOAD_SYMBOLS: load_symbols(d); return 0;
        case IDC_SAVE: save_menu(d); return 0;
        case IDC_FOLLOW_PC: refresh_disasm(d); return 0;
        case IDC_DISASM:
            if (HIWORD(wp) == LBN_SELCHANGE) {
                const int sel = (int)SendMessageA(d->disasm, LB_GETCURSEL, 0, 0);
                d->sel_addr = sel >= 0 && sel < d->line_count ? d->lines[sel].address : -1;
            } else if (HIWORD(wp) == LBN_DBLCLK) {
                toggle_selected_breakpoint(d);
            }
            return 0;
        case IDC_BP_ADD: add_breakpoint(d); return 0;
        case IDC_BP_REMOVE:
        case IDC_BP_ENABLE: {
            int sel = (int)SendMessageA(d->bp_list, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < d->nbps) {
                if (id == IDC_BP_REMOVE) a7800debug_breakpoint_remove(d->dbg, d->bps[sel].id);
                else a7800debug_breakpoint_enable(d->dbg, d->bps[sel].id, !d->bps[sel].enabled);
                refresh_bps(d);
                refresh_disasm(d);
            }
            return 0;
        }
        case IDC_BP_LIST:
            if (HIWORD(wp) == LBN_DBLCLK) {
                int sel = (int)SendMessageA(d->bp_list, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < d->nbps && (d->bps[sel].type & A7800DEBUG_BP_EXEC)) {
                    char a[16];
                    snprintf(a, sizeof a, "$%04X", d->bps[sel].start);
                    TabCtrl_SetCurSel(d->tabs, PAGE_DISASM);
                    show_page(d, PAGE_DISASM);
                    layout(d);
                    jump_to(d, a);
                }
            }
            return 0;
        case IDC_BP_CLEAR:
            a7800debug_breakpoint_clear(d->dbg);
            refresh_bps(d);
            refresh_disasm(d);
            return 0;
        case IDCANCEL:
            detach_and_hide(d);
            return 0;
        default: break;
        }
        break;
    }
    case WM_CLOSE:
        /* Hide and let the machine run; F12 brings it back, stopped. */
        detach_and_hide(d);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        DeleteObject(d->mono);
        DeleteObject(d->accent);
        DestroyAcceleratorTable(d->accel);
        free(d);
        g_dbg = NULL;
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- entry points ---------------------------------------------------------------------- */

void a7800_debugger_show(HWND parent, a7800session *session)
{
    HINSTANCE inst;
    WNDCLASSA wc;
    debugger *d;
    ACCEL accels[7];
    const char *tab;

    if (g_dbg) {
        if (!IsWindowVisible(g_dbg->hwnd)) {
            ShowWindow(g_dbg->hwnd, SW_SHOW);
            attach(g_dbg);
        } else {
            a7800debug_stop(g_dbg->dbg);
            refresh_all(g_dbg);
        }
        SetForegroundWindow(g_dbg->hwnd);
        return;
    }

    inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = dbg_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "A7800DebuggerWindow";
    RegisterClassA(&wc);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = pic_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "A7800DbgMaria";
    RegisterClassA(&wc);

    d = calloc(1, sizeof *d);
    if (!d) return;
    d->session = session;
    d->dbg = a7800session_debugger(session);
    d->sel_addr = -1;
    d->mem_base_addr = 0x1800;
    g_dbg = d;

    d->hwnd = CreateWindowExA(0, "A7800DebuggerWindow", "Debugger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1180, 820, NULL, NULL, inst, NULL);
    if (!d->hwnd) { free(d); g_dbg = NULL; return; }

    d->ui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    d->mono = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    d->accent = CreateSolidBrush(RGB((A7800SESSION_ACCENT_RGB >> 16) & 0xff, (A7800SESSION_ACCENT_RGB >> 8) & 0xff,
                                     A7800SESSION_ACCENT_RGB & 0xff));
    {
        /* the disassembly's line height, from the font it is drawn in */
        HDC dc = GetDC(d->hwnd);
        TEXTMETRICA tm;
        HGDIOBJ of = SelectObject(dc, d->mono);
        d->line_h = GetTextMetricsA(dc, &tm) ? tm.tmHeight + 3 : 18;
        SelectObject(dc, of);
        ReleaseDC(d->hwnd, dc);
    }

    build_controls(d);

    /* F5/F7/F8/Shift+F8/F9/F12 work wherever the focus is inside the window. */
    accels[0].fVirt = FVIRTKEY;          accels[0].key = VK_F5;  accels[0].cmd = IDC_RUN;
    accels[1].fVirt = FVIRTKEY;          accels[1].key = VK_F7;  accels[1].cmd = IDC_STEP;
    accels[2].fVirt = FVIRTKEY;          accels[2].key = VK_F8;  accels[2].cmd = IDC_OVER;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;  accels[3].cmd = IDC_OUT;
    accels[4].fVirt = FVIRTKEY;          accels[4].key = VK_F12; accels[4].cmd = IDCANCEL;
    accels[5].fVirt = FVIRTKEY | FSHIFT; accels[5].key = VK_F7;  accels[5].cmd = IDC_FRAME;
    accels[6].fVirt = FVIRTKEY;          accels[6].key = VK_F9;  accels[6].cmd = IDC_TOGGLE_BP;
    d->accel = CreateAcceleratorTableA(accels, 7);

    tab = getenv("A7800_DEBUGGER_TAB");
    d->page = PAGE_CONSOLE;
    if (tab && *tab) {
        int t = atoi(tab);
        if (t >= 0 && t < PAGE_COUNT) d->page = t;
    }
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)d->page, 0);
    show_page(d, d->page);
    layout(d);

    ShowWindow(d->hwnd, SW_SHOW);
    attach(d);
}

int a7800_debugger_pretranslate(MSG *msg)
{
    if (!g_dbg || !g_dbg->accel) return 0;
    if (msg->hwnd != g_dbg->hwnd && !IsChild(g_dbg->hwnd, msg->hwnd)) return 0;
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_F12) { detach_and_hide(g_dbg); return 1; }
    return TranslateAcceleratorA(g_dbg->hwnd, g_dbg->accel, msg) ? 1 : 0;
}

int a7800_debugger_visible(void)
{
    return g_dbg && IsWindowVisible(g_dbg->hwnd);
}

void a7800_debugger_toggle(HWND parent, a7800session *session)
{
    if (a7800_debugger_visible()) detach_and_hide(g_dbg);
    else a7800_debugger_show(parent, session);
}
