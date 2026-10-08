/*
 * Debugger window (GTK4/libadwaita) over MAME's own debugger engine, via
 * core/include/a7800debug.h.
 *
 * Tabs: the Console (MAME's command set and the family's own, with tab
 * completion, symbol loading and file saves), CPU & Memory, the disassembly
 * (click a line to select it, double-click or F9 for its breakpoint), MARIA
 * (registers, the display list list and the palettes), I/O (INPTCTRL, the
 * TIA's sound, RIOT, POKEY and the controls), the FujiNet cartridge, and
 * breakpoints & watchpoints. A toolbar carries the stepping controls; the
 * family's keys apply (F5 run/stop, F7 step, F8 step over, Shift+F8 step
 * out, F9 breakpoint, F12 close).
 *
 * MAME's debugger is always on; showing this window attaches (which stops
 * the machine, as on every sibling), hiding it detaches and lets the
 * machine run on.
 *
 * The window polls the engine's generation counter on a short timer and
 * refreshes only when something changed, so a stopped machine costs
 * nothing and a running one shows live values twice a second.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <stdlib.h>
#include <string.h>

#include "a7800debug.h"
#include "../window.h"

#define DISASM_WINDOW 48
#define MEM_ROWS 64             /* 1K of memory, 16 bytes a row */

typedef struct {
    GtkWindow *win;
    a7800session *session;
    a7800debug *dbg;
    unsigned seen_generation;
    gboolean was_stopped;
    gboolean stopped_machine;   /* attached while a machine was running */
    int running_ticks;
    guint timer;

    GtkLabel *status;
    GtkButton *run_btn;

    /* console */
    GtkTextView *prompt_out;
    GtkEntry *prompt_in;
    GtkScrolledWindow *prompt_scroll;

    /* cpu + memory */
    GtkEntry *reg[6];         /* PC SP A X Y P */
    GtkCheckButton *flag[6];  /* N V D I Z C */
    GtkLabel *cycles;
    GtkTextView *mem_view;
    GtkEntry *mem_from;
    GtkEntry *mem_addr, *mem_val;
    int mem_top;
    gboolean updating_flags;

    /* disassembly */
    GtkTextView *disasm;
    GtkCheckButton *follow_pc;
    GtkEntry *jump;
    int disasm_top;           /* address of the first line */
    int selected;             /* the selected line's address, or -1 */
    uint16_t line_addr[DISASM_WINDOW];
    int line_count;

    /* maria */
    GtkTextView *maria_text;
    GtkTextView *dll_text;
    GtkPicture *palette_pic;
    guint32 *palette_px;

    /* i/o */
    GtkTextView *io_text;

    /* cartridge */
    GtkTextView *cart_text;

    /* breakpoints */
    GtkListBox *bp_list;
    GtkDropDown *bp_type;
    GtkEntry *bp_start, *bp_end, *bp_cond;
    GtkLabel *bp_msg;
} DbgWin;

static DbgWin *g_win;

/* ---- helpers -------------------------------------------------------------- */

static void set_text(GtkTextView *view, const char *text)
{
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(view), text, -1);
}

static void append_text(GtkTextView *view, GtkScrolledWindow *scroll, const char *text)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkTextIter end;
    GtkAdjustment *adj;
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_buffer_insert(b, &end, text, -1);
    adj = gtk_scrolled_window_get_vadjustment(scroll);
    if (adj) gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
}

static int parse_num(const char *text, long *out)
{
    char *end;
    long v;
    while (*text == ' ') text++;
    if (*text == '$') v = strtol(text + 1, &end, 16);
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) v = strtol(text + 2, &end, 16);
    else if (*text == '#') v = strtol(text + 1, &end, 10);
    else v = strtol(text, &end, 16);
    if (end == text || *end) return 0;
    *out = v;
    return 1;
}

/* A label or a number, as typed into an address box. */
static int parse_addr(DbgWin *w, const char *text, long *out)
{
    int a = a7800debug_label_address(w->dbg, text);
    if (a >= 0) { *out = a; return 1; }
    return parse_num(text, out);
}

static GtkWidget *mono_view(GtkTextView **out, gboolean editable)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

static GtkWidget *labeled(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_box_append(GTK_BOX(box), l);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

static GtkWidget *padded(GtkWidget *w)
{
    gtk_widget_set_margin_top(w, 8);
    gtk_widget_set_margin_bottom(w, 8);
    gtk_widget_set_margin_start(w, 8);
    gtk_widget_set_margin_end(w, 8);
    return w;
}

static const char *yes_no(int v) { return v ? "on" : "off"; }

/* The a7800_action bits held on a port, as words. */
static void held_words(unsigned held, char *dst, size_t dstsz)
{
    static const char *const names[A7800_ACT_PER_PORT] = {
        "Up", "Down", "Left", "Right", "Button1", "Button2" };
    size_t len = 0;
    int b;
    dst[0] = '\0';
    for (b = 0; b < A7800_ACT_PER_PORT; b++)
        if (held & (1u << b))
            len += (size_t)g_snprintf(dst + len, dstsz - len, "%s ", names[b]);
    if (!len) g_snprintf(dst, dstsz, "(nothing held)");
}

/* ---- refresh -------------------------------------------------------------- */

static void refresh_cpu(DbgWin *w)
{
    a7800debug_cpu c;
    char buf[160];
    int i;
    a7800debug_cpu_get(w->dbg, &c);
    g_snprintf(buf, sizeof buf, "%04X", c.pc); gtk_editable_set_text(GTK_EDITABLE(w->reg[0]), buf);
    g_snprintf(buf, sizeof buf, "%02X", c.sp & 0xFF); gtk_editable_set_text(GTK_EDITABLE(w->reg[1]), buf);
    g_snprintf(buf, sizeof buf, "%02X", c.a); gtk_editable_set_text(GTK_EDITABLE(w->reg[2]), buf);
    g_snprintf(buf, sizeof buf, "%02X", c.x); gtk_editable_set_text(GTK_EDITABLE(w->reg[3]), buf);
    g_snprintf(buf, sizeof buf, "%02X", c.y); gtk_editable_set_text(GTK_EDITABLE(w->reg[4]), buf);
    g_snprintf(buf, sizeof buf, "%02X", c.ps); gtk_editable_set_text(GTK_EDITABLE(w->reg[5]), buf);
    {
        const int flags[6] = { c.n, c.v, c.d, c.i, c.z, c.c };
        w->updating_flags = TRUE;
        for (i = 0; i < 6; i++)
            gtk_check_button_set_active(w->flag[i], flags[i] != 0);
        w->updating_flags = FALSE;
    }
    g_snprintf(buf, sizeof buf, "cycle %llu   scanline %d   dot %d   frame %u",
               (unsigned long long)c.total_cycles, c.scanline, c.dot, c.frame);
    gtk_label_set_text(w->cycles, buf);
}

static void refresh_mem(DbgWin *w)
{
    uint8_t mem[MEM_ROWS * 16];
    static char text[MEM_ROWS * 16 * 4 + MEM_ROWS * 32 + 128];
    int len = 0, row, col;

    memset(mem, 0, sizeof mem);
    a7800debug_read(w->dbg, (uint16_t)w->mem_top, mem, (int)sizeof mem);
    len += g_snprintf(text + len, sizeof text - len,
                      "        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    for (row = 0; row < MEM_ROWS; row++) {
        len += g_snprintf(text + len, sizeof text - len, "$%04X: ",
                          (w->mem_top + row * 16) & 0xFFFF);
        for (col = 0; col < 16; col++)
            len += g_snprintf(text + len, sizeof text - len, "%02X ", mem[row * 16 + col]);
        len += g_snprintf(text + len, sizeof text - len, " ");
        for (col = 0; col < 16; col++) {
            uint8_t ch = mem[row * 16 + col];
            text[len++] = (char)(ch >= 0x20 && ch < 0x7F ? ch : '.');
        }
        text[len++] = '\n';
        text[len] = '\0';
    }
    set_text(w->mem_view, text);
}

static void refresh_disasm(DbgWin *w)
{
    static a7800debug_line lines[DISASM_WINDOW];
    static char text[DISASM_WINDOW * 200];
    int n, pc_line, i, len = 0;
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GtkTextIter s, e;

    /* Following, the PC sits a third of the way down the window. */
    if (gtk_check_button_get_active(w->follow_pc)) {
        a7800debug_cpu c;
        a7800debug_cpu_get(w->dbg, &c);
        w->disasm_top = a7800debug_row_address(w->dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }

    n = a7800debug_disassemble(w->dbg, (uint16_t)w->disasm_top, lines, DISASM_WINDOW, &pc_line);
    w->line_count = n;
    for (i = 0; i < n; i++) {
        w->line_addr[i] = lines[i].address;
        len += g_snprintf(text + len, sizeof text - len, "%c%c %04X  %-9s %-14s %s%s%s\n",
                          lines[i].has_breakpoint ? '*' : ' ',
                          lines[i].is_pc ? '>' : ' ',
                          lines[i].address, lines[i].bytes, lines[i].label,
                          lines[i].disasm, lines[i].comment[0] ? "  ; " : "",
                          lines[i].comment);
    }
    if (n == 0)
        len += g_snprintf(text + len, sizeof text - len, "(no disassembly)\n");
    gtk_text_buffer_set_text(b, text, -1);

    for (i = 0; i < n; i++) {
        /* the selected line underlined, the PC line in the accent colour */
        if (lines[i].address == w->selected) {
            gtk_text_buffer_get_iter_at_line(b, &s, i);
            gtk_text_buffer_get_iter_at_line(b, &e, i + 1);
            gtk_text_buffer_apply_tag_by_name(b, "sel", &s, &e);
        }
    }
    if (pc_line >= 0 && pc_line < n) {
        gtk_text_buffer_get_iter_at_line(b, &s, pc_line);
        gtk_text_buffer_get_iter_at_line(b, &e, pc_line + 1);
        gtk_text_buffer_apply_tag_by_name(b, "pc", &s, &e);
    }
}

static void show_palette(DbgWin *w)
{
    const int pw = A7800DEBUG_PALETTE_WIDTH, ph = A7800DEBUG_PALETTE_HEIGHT;
    static guint32 px[A7800DEBUG_PALETTE_WIDTH * 2 * A7800DEBUG_PALETTE_HEIGHT * 2];
    GBytes *bytes;
    GdkTexture *tex;
    int x, y;

    if (!a7800debug_palette_image(w->dbg, w->palette_px))
        return;
    /* doubled here, nearest-neighbour, so the swatches stay crisp */
    for (y = 0; y < ph * 2; y++)
        for (x = 0; x < pw * 2; x++)
            px[y * pw * 2 + x] = w->palette_px[(y / 2) * pw + x / 2] | 0xFF000000u;
    bytes = g_bytes_new(px, sizeof px);
    tex = gdk_memory_texture_new(pw * 2, ph * 2, GDK_MEMORY_B8G8R8A8, bytes, (gsize)pw * 2 * 4);
    gtk_picture_set_paintable(w->palette_pic, GDK_PAINTABLE(tex));
    g_object_unref(tex);
    g_bytes_unref(bytes);
}

static void refresh_maria(DbgWin *w)
{
    static const char *const modes[4] = { "160A / 160B", "(unused)", "320B / 320D", "320A / 320C" };
    static char dll[16384];
    a7800debug_maria m;
    char text[2048];
    int len, p;

    a7800debug_maria_get(w->dbg, &m);
    len = g_snprintf(text, sizeof text,
        "CTRL      $%02X   DMA %s   colour kill %s\n"
        "          kangaroo %s   border %s\n"
        "          characters %d byte%s wide\n"
        "          read mode %s\n"
        "CHARBASE  $%02X00\n"
        "DPP       $%04X   (the display list list)\n"
        "Now       DLL $%04X   DL $%04X\n"
        "          offset %d   holey %s   DLI %s\n"
        "MSTAT     VBLANK %s   scanline %d\n\n"
        "BACKGRND  $%02X\n",
        m.ctrl, yes_no(m.dma_on), yes_no(m.color_kill), yes_no(m.kangaroo),
        m.border_control ? "black" : "background",
        m.char_width, m.char_width == 1 ? "" : "s", modes[m.read_mode & 3],
        m.charbase, m.dpp, m.dll, m.dl, m.offset, yes_no(m.holey), yes_no(m.dli),
        yes_no(m.vblank), m.scanline, m.palette[0]);
    for (p = 0; p < 8 && len < (int)sizeof text; p++)
        len += g_snprintf(text + len, sizeof text - len, "P%dC1-3    $%02X $%02X $%02X\n",
                          p, m.palette[p * 4 + 1], m.palette[p * 4 + 2], m.palette[p * 4 + 3]);
    set_text(w->maria_text, text);

    if (a7800debug_dll_text(w->dbg, dll, sizeof dll) <= 0)
        g_snprintf(dll, sizeof dll, "(no display list list: MARIA's DMA is off)");
    set_text(w->dll_text, dll);
    show_palette(w);
}

/* RIOT port A's joystick directions for one player (active low; player 1
 * in the high nibble). A light gun's trigger is wired to the Up line, so a
 * gun port reports that line as it is rather than as a direction. */
static void stick_words(uint8_t swcha, int player, int type, char *dst, size_t dstsz)
{
    static const char *const dirs[4] = { "up", "down", "left", "right" };
    const int shift = player ? 0 : 4;
    size_t len = 0;
    int b;
    dst[0] = '\0';
    if (type == A7800_CTRL_LIGHTGUN) {
        g_snprintf(dst, dstsz, "light gun (trigger line %s)",
                   (swcha & (1u << shift)) ? "high" : "low");
        return;
    }
    for (b = 0; b < 4; b++)
        if (!(swcha & (1u << (shift + b))))
            len += (size_t)g_snprintf(dst + len, dstsz - len, "%s%s", len ? " " : "", dirs[b]);
    if (!len) g_snprintf(dst, dstsz, "centred");
}

static void refresh_io(DbgWin *w)
{
    a7800debug_io io;
    char text[2400], held[2][96], stick[2][40], sw[48];
    int port;

    a7800debug_io_get(w->dbg, &io);
    for (port = 0; port < 2; port++) {
        held_words(io.held[port], held[port], sizeof held[port]);
        stick_words(io.swcha, port, io.port_type[port], stick[port], sizeof stick[port]);
    }
    /* RIOT port B: Reset / Select / Pause active low, the difficulty
     * switches set for A. */
    g_snprintf(sw, sizeof sw, "%s%s%s",
               (io.swchb & 0x01) ? "" : "RESET ", (io.swchb & 0x02) ? "" : "SELECT ",
               (io.swchb & 0x08) ? "" : "PAUSE ");
    g_snprintf(text, sizeof text,
        "INPTCTRL     $%02X   %s\n\n"
        "TIA sound    AUDC  AUDF  AUDV\n"
        "  channel 0  $%X    $%02X   $%X\n"
        "  channel 1  $%X    $%02X   $%X\n\n"
        "RIOT SWCHA   $%02X   player 1 %s, player 2 %s\n"
        "RIOT SWCHB   $%02X   %s   left difficulty %c   right difficulty %c\n\n"
        "TIA INPT0-5  $%02X $%02X $%02X $%02X $%02X $%02X\n\n"
        "POKEY        %s\n",
        io.inptctrl, io.inpt_locked ? "locked" : "unlocked",
        io.audc[0], io.audf[0], io.audv[0], io.audc[1], io.audf[1], io.audv[1],
        io.swcha, stick[0], stick[1],
        io.swchb, sw[0] ? sw : "no switch held",
        (io.swchb & 0x40) ? 'A' : 'B', (io.swchb & 0x80) ? 'A' : 'B',
        io.inpt[0], io.inpt[1], io.inpt[2], io.inpt[3], io.inpt[4], io.inpt[5],
        io.pokey_present ? "on the cartridge" : "(none on this cartridge)");
    if (io.pokey_present) {
        char pk[400];
        g_snprintf(pk, sizeof pk,
            "  AUDF  $%02X $%02X $%02X $%02X\n"
            "  AUDC  $%02X $%02X $%02X $%02X\n"
            "  AUDCTL $%02X\n",
            io.pokey_audf[0], io.pokey_audf[1], io.pokey_audf[2], io.pokey_audf[3],
            io.pokey_audc[0], io.pokey_audc[1], io.pokey_audc[2], io.pokey_audc[3],
            io.pokey_audctl);
        g_strlcat(text, pk, sizeof text);
    }
    for (port = 0; port < 2; port++) {
        char line[200];
        g_snprintf(line, sizeof line, "\nPort %d       %s   held: %s", port + 1,
                   a7800_ctrl_type_name(io.port_type[port]), held[port]);
        g_strlcat(text, line, sizeof text);
    }
    g_strlcat(text, "\n", sizeof text);
    set_text(w->io_text, text);
}

static void refresh_cart(DbgWin *w)
{
    static const char *const handovers[3] = { "none yet", "the BIOS", "the loader" };
    a7800debug_cart c;
    char info[256], text[2048];
    a7800debug_cart_get(w->dbg, &c);
    a7800debug_cart_info(w->dbg, info, sizeof info);
    if (!c.present) {
        set_text(w->cart_text, info);
        return;
    }
    g_snprintf(text, sizeof text,
        "%s\n\n"
        "Link        %s%s%s\n"
        "Worker      %s   queue %u\n"
        "Mode        %s   hand-over by %s\n"
        "Loading     %d%%\n"
        "Image       %s   CRC %08X   %s\n"
        "Staged      %s%s%s",
        info,
        c.link_up ? "up" : "down",
        (!c.link_up && c.link_error[0]) ? ": " : "", c.link_up ? "" : c.link_error,
        c.worker ? "running" : "stopped", c.queue_depth,
        c.mode_name, handovers[c.handover >= 0 && c.handover < 3 ? c.handover : 0],
        c.load_pct,
        c.kind[0] ? c.kind : "-", c.live_crc, c.booted_image ? "(a game)" : "(CONFIG)",
        c.staged ? c.staged_kind : "nothing", c.staged ? "   CRC " : "", "");
    if (c.staged) {
        char crc[16];
        g_snprintf(crc, sizeof crc, "%08X", c.staged_crc);
        g_strlcat(text, crc, sizeof text);
    }
    {
        char more[400];
        g_snprintf(more, sizeof more,
            "\nHigh Score  %s%s%s%s\n"
            "TV          %s\n"
            "Mailbox     ACKSEQ $%02X   last error %u\n"
            "Boot        state $%02X   %u%%   error %u\n",
            (c.hsc & 1) ? "ROM installed" : "no ROM", (c.hsc & 2) ? ", on" : ", off",
            (c.hsc & 4) ? ", saved" : "", (c.hsc & 8) ? ", unsaved changes" : "",
            c.tv ? "PAL" : "NTSC",
            c.ackseq, c.last_error, c.boot_state, c.boot_pct, c.boot_err);
        g_strlcat(text, more, sizeof text);
    }
    set_text(w->cart_text, text);
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud);
static void bp_remove_clicked(GtkButton *b, gpointer ud);

static void refresh_bps(DbgWin *w)
{
    a7800debug_breakpoint bps[128];
    GtkWidget *child;
    int n = a7800debug_breakpoint_list(w->dbg, bps, 128), i;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(w->bp_list))) != NULL)
        gtk_list_box_remove(w->bp_list, child);
    if (n == 0) {
        GtkWidget *l = gtk_label_new("No breakpoints or watchpoints. Double-click a "
                                     "disassembly line, or add one below.");
        gtk_widget_add_css_class(l, "dim-label");
        gtk_list_box_append(w->bp_list, l);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *en = gtk_check_button_new();
        GtkWidget *rm = gtk_button_new_from_icon_name("user-trash-symbolic");
        GtkWidget *l;
        char text[256], range[32];
        const char *type = (bps[i].type & A7800DEBUG_BP_EXEC) ? "Execute"
                         : (bps[i].type == (A7800DEBUG_BP_READ | A7800DEBUG_BP_WRITE)) ? "Read/Write"
                         : (bps[i].type & A7800DEBUG_BP_READ) ? "Read" : "Write";
        if (bps[i].end != bps[i].start)
            g_snprintf(range, sizeof range, "$%04X-$%04X", bps[i].start, bps[i].end);
        else
            g_snprintf(range, sizeof range, "$%04X", bps[i].start);
        g_snprintf(text, sizeof text, "%-10s %-12s %s%s", type, range,
                   bps[i].condition[0] ? "if " : "", bps[i].condition);
        l = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_widget_add_css_class(l, "monospace");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(en), bps[i].enabled != 0);
        g_object_set_data(G_OBJECT(en), "bp", GINT_TO_POINTER(bps[i].id));
        g_object_set_data(G_OBJECT(rm), "bp", GINT_TO_POINTER(bps[i].id));
        g_signal_connect(en, "toggled", G_CALLBACK(bp_enable_toggled), w);
        g_signal_connect(rm, "clicked", G_CALLBACK(bp_remove_clicked), w);
        gtk_widget_add_css_class(rm, "flat");
        gtk_box_append(GTK_BOX(row), en);
        gtk_box_append(GTK_BOX(row), l);
        gtk_box_append(GTK_BOX(row), rm);
        gtk_list_box_append(w->bp_list, row);
    }
}

static void refresh_status(DbgWin *w)
{
    char reason[160], text[256];
    int addr;
    gboolean stopped = a7800debug_is_stopped(w->dbg) != 0;
    a7800debug_stop_reason(w->dbg, reason, sizeof reason, &addr);
    if (stopped && reason[0] && strcmp(reason, "stopped") != 0)
        g_snprintf(text, sizeof text, "Stopped: %s", reason);
    else if (stopped)
        g_snprintf(text, sizeof text, "Stopped");
    else
        g_snprintf(text, sizeof text, "Running");
    gtk_label_set_text(w->status, text);
    gtk_button_set_label(w->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    if (stopped) gtk_widget_add_css_class(GTK_WIDGET(w->run_btn), "a7800-accent");
    else gtk_widget_remove_css_class(GTK_WIDGET(w->run_btn), "a7800-accent");
}

static void refresh_all(DbgWin *w)
{
    refresh_status(w);
    refresh_cpu(w);
    refresh_mem(w);
    refresh_disasm(w);
    refresh_maria(w);
    refresh_io(w);
    refresh_bps(w);
    refresh_cart(w);
}

static void attach_and_refresh(DbgWin *w);

static gboolean tick_refresh(gpointer ud)
{
    DbgWin *w = ud;
    unsigned gen;
    gboolean stopped;
    if (!gtk_widget_get_visible(GTK_WIDGET(w->win))) return G_SOURCE_CONTINUE;
    /* Opened before the session started (A7800_OPEN_DEBUGGER), or showing
     * across a session restart: stop the machine as it comes up. */
    if (!w->stopped_machine && a7800session_is_running(w->session)) {
        attach_and_refresh(w);
        return G_SOURCE_CONTINUE;
    }
    gen = a7800debug_generation(w->dbg);
    stopped = a7800debug_is_stopped(w->dbg) != 0;
    if (gen != w->seen_generation || stopped != w->was_stopped) {
        w->seen_generation = gen;
        w->was_stopped = stopped;
        refresh_all(w);
    } else if (!stopped && ++w->running_ticks >= 5) {
        /* live values twice a second while the machine runs */
        w->running_ticks = 0;
        refresh_status(w);
        refresh_cpu(w);
        refresh_mem(w);
        refresh_maria(w);
        refresh_io(w);
        refresh_cart(w);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- handlers ------------------------------------------------------------- */

static void on_run(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (a7800debug_is_stopped(w->dbg)) a7800debug_resume(w->dbg);
    else a7800debug_stop(w->dbg);
    refresh_all(w);
}

static void on_step(GtkButton *b, gpointer ud) { (void)b; a7800debug_step(((DbgWin *)ud)->dbg); refresh_all(ud); }
static void on_step_over(GtkButton *b, gpointer ud) { (void)b; a7800debug_step_over(((DbgWin *)ud)->dbg); refresh_all(ud); }
static void on_step_out(GtkButton *b, gpointer ud) { (void)b; a7800debug_step_out(((DbgWin *)ud)->dbg); refresh_all(ud); }
static void on_frame(GtkButton *b, gpointer ud) { (void)b; a7800debug_frame(((DbgWin *)ud)->dbg); refresh_all(ud); }

static void on_run_to(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (w->selected < 0) {
        gtk_label_set_text(w->status, "Select a disassembly line to run to");
        return;
    }
    a7800debug_run_to(w->dbg, (uint16_t)w->selected);
    refresh_all(w);
}

static void toggle_selected_breakpoint(DbgWin *w)
{
    if (w->selected < 0) return;
    a7800debug_breakpoint_toggle(w->dbg, (uint16_t)w->selected);
    refresh_disasm(w);
    refresh_bps(w);
}

static void on_prompt(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    static char out[65536];
    const char *cmd = gtk_editable_get_text(GTK_EDITABLE(entry));
    char line[512];
    if (!cmd || !*cmd) return;
    g_snprintf(line, sizeof line, "> %s\n", cmd);
    append_text(w->prompt_out, w->prompt_scroll, line);
    a7800debug_command(w->dbg, cmd, out, sizeof out);
    append_text(w->prompt_out, w->prompt_scroll, out);
    if (out[0] && out[strlen(out) - 1] != '\n')
        append_text(w->prompt_out, w->prompt_scroll, "\n");
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    refresh_all(w);
}

/* Tab completes the current word against the commands and the labels;
 * several matches are listed instead. */
static gboolean on_prompt_key(GtkEventControllerKey *c, guint keyval, guint code,
                              GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    char comps[4096];
    const char *text;
    const char *word;
    int n;
    (void)c; (void)code; (void)st;
    if (keyval != GDK_KEY_Tab) return FALSE;
    text = gtk_editable_get_text(GTK_EDITABLE(w->prompt_in));
    word = strrchr(text, ' ');
    word = word ? word + 1 : text;
    n = a7800debug_completions(w->dbg, word, comps, sizeof comps);
    if (n == 1) {
        char merged[600];
        char *nl = strchr(comps, '\n');
        if (nl) *nl = '\0';
        g_snprintf(merged, sizeof merged, "%.*s%s ", (int)(word - text), text, comps);
        gtk_editable_set_text(GTK_EDITABLE(w->prompt_in), merged);
        gtk_editable_set_position(GTK_EDITABLE(w->prompt_in), -1);
    } else if (n > 1) {
        append_text(w->prompt_out, w->prompt_scroll, comps);
    }
    return TRUE;
}

static void on_reg_activate(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "reg"));
    long v;
    static const int regs[6] = { A7800_REG_PC, A7800_REG_SP, A7800_REG_A, A7800_REG_X, A7800_REG_Y, A7800_REG_PS };
    if (parse_num(gtk_editable_get_text(GTK_EDITABLE(entry)), &v)) {
        /* the stack is page 1, whatever was typed */
        if (regs[i] == A7800_REG_SP) v = 0x100 | (v & 0xFF);
        a7800debug_cpu_set(w->dbg, regs[i], (int)v);
    }
    refresh_all(w);
}

static void on_flag_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "flag"));
    static const int flags[6] = { A7800_FLAG_N, A7800_FLAG_V, A7800_FLAG_D, A7800_FLAG_I, A7800_FLAG_Z, A7800_FLAG_C };
    if (w->updating_flags || !a7800debug_is_stopped(w->dbg)) return;
    a7800debug_cpu_set(w->dbg, flags[i], gtk_check_button_get_active(b));
}

static void on_mem_from(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    long a;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &a)) return;
    w->mem_top = (int)(a & 0xFFF0);
    refresh_mem(w);
}

static void on_mem_write(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    long a, v;
    (void)entry;
    if (parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(w->mem_addr)), &a)
        && parse_num(gtk_editable_get_text(GTK_EDITABLE(w->mem_val)), &v))
        a7800debug_write(w->dbg, (uint16_t)a, (uint8_t)v);
    refresh_all(w);
}

/* One click selects a line (Run to Cursor, F9); a double-click toggles its
 * breakpoint. */
static void on_disasm_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    DbgWin *w = ud;
    GtkTextIter it;
    int bx, by, line;
    (void)g;
    gtk_text_view_window_to_buffer_coords(w->disasm, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    gtk_text_view_get_iter_at_location(w->disasm, &it, bx, by);
    line = gtk_text_iter_get_line(&it);
    if (line < 0 || line >= w->line_count) return;
    w->selected = w->line_addr[line];
    if (n == 2)
        toggle_selected_breakpoint(w);
    else
        refresh_disasm(w);
}

static void on_follow_toggled(GtkCheckButton *b, gpointer ud)
{
    (void)b;
    refresh_disasm(ud);
}

static void on_jump(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    long a;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &a)) return;
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->disasm_top = (int)(a & 0xFFFF);
    w->selected = w->disasm_top;
    refresh_disasm(w);
}

static gboolean on_disasm_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer ud)
{
    DbgWin *w = ud;
    int rows = (int)(dy * 3);
    (void)c; (void)dx;
    if (!rows) rows = dy > 0 ? 1 : -1;
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->disasm_top = a7800debug_row_address(w->dbg, (uint16_t)w->disasm_top, rows);
    refresh_disasm(w);
    return TRUE;
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    a7800debug_breakpoint_enable(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")),
                               gtk_check_button_get_active(b));
    refresh_disasm(w);
}

static void bp_remove_clicked(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    a7800debug_breakpoint_remove(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")));
    refresh_all(w);
}

static void on_bp_add(GtkWidget *wgt, gpointer ud)
{
    DbgWin *w = ud;
    static const int types[4] = { A7800DEBUG_BP_EXEC, A7800DEBUG_BP_READ, A7800DEBUG_BP_WRITE,
                                  A7800DEBUG_BP_READ | A7800DEBUG_BP_WRITE };
    long a, b;
    int id;
    const char *end = gtk_editable_get_text(GTK_EDITABLE(w->bp_end));
    (void)wgt;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(w->bp_start)), &a)) {
        gtk_label_set_text(w->bp_msg, "Not an address or a label");
        return;
    }
    b = a;
    if (end && *end && !parse_addr(w, end, &b)) {
        gtk_label_set_text(w->bp_msg, "The end is not an address or a label");
        return;
    }
    id = a7800debug_breakpoint_add(w->dbg, types[gtk_drop_down_get_selected(w->bp_type) & 3],
                                 (uint16_t)a, (uint16_t)b,
                                 gtk_editable_get_text(GTK_EDITABLE(w->bp_cond)));
    if (id < 0) {
        gtk_label_set_text(w->bp_msg, "The condition does not parse");
        return;
    }
    gtk_label_set_text(w->bp_msg, "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_start), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_end), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_cond), "");
    refresh_all(w);
}

static void on_bp_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    a7800debug_breakpoint_clear(w->dbg);
    refresh_all(w);
}

static void prompt_say(DbgWin *w, const char *msg)
{
    char line[700];
    g_snprintf(line, sizeof line, "%s\n", msg);
    append_text(w->prompt_out, w->prompt_scroll, line);
}

static void on_symbols_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    char msg[512];
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    a7800debug_load_symbols(w->dbg, path, msg, sizeof msg);
    prompt_say(w, msg);
    refresh_all(w);
}

static void on_symbols(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    (void)b;
    gtk_file_dialog_set_title(dlg, "Load Symbols (ld65 -Ln, VICE, ca65 .dbg, name = $1234)");
    gtk_file_dialog_open(dlg, w->win, NULL, on_symbols_chosen, w);
    g_object_unref(dlg);
}

static void on_save_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    const char *kind = g_object_get_data(src, "kind");
    char msg[512];
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    a7800debug_save(w->dbg, kind, path, msg, sizeof msg);
    prompt_say(w, msg);
}

static void on_save(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    const char *kind = g_object_get_data(G_OBJECT(b), "kind");
    char title[64];
    g_snprintf(title, sizeof title, "Save %s", (const char *)g_object_get_data(G_OBJECT(b), "title"));
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_initial_name(dlg, g_object_get_data(G_OBJECT(b), "file"));
    g_object_set_data(G_OBJECT(dlg), "kind", (gpointer)kind);
    gtk_file_dialog_save(dlg, w->win, NULL, on_save_chosen, w);
    g_object_unref(dlg);
}

static void hide_window(DbgWin *w)
{
    a7800debug_detach(w->dbg);
    w->stopped_machine = FALSE;
    gtk_widget_set_visible(GTK_WIDGET(w->win), FALSE);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    (void)c; (void)code;
    switch (keyval) {
    case GDK_KEY_F5: on_run(NULL, w); return TRUE;
    case GDK_KEY_F7: on_step(NULL, w); return TRUE;
    case GDK_KEY_F8:
        if (st & GDK_SHIFT_MASK) on_step_out(NULL, w); else on_step_over(NULL, w);
        return TRUE;
    case GDK_KEY_F9: toggle_selected_breakpoint(w); return TRUE;
    case GDK_KEY_F12: hide_window(w); return TRUE;
    default: return FALSE;
    }
}

static gboolean on_close(GtkWindow *win, gpointer ud)
{
    (void)win;
    hide_window(ud);
    return TRUE;
}

/* ---- construction --------------------------------------------------------- */

static GtkWidget *build_toolbar(DbgWin *w)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b;
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
#define TB(label, cb) do { b = gtk_button_new_with_label(label); \
    g_signal_connect(b, "clicked", G_CALLBACK(cb), w); gtk_box_append(GTK_BOX(bar), b); } while (0)
    w->run_btn = GTK_BUTTON(gtk_button_new_with_label("Stop (F5)"));
    g_signal_connect(w->run_btn, "clicked", G_CALLBACK(on_run), w);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->run_btn));
    TB("Step (F7)", on_step);
    TB("Step Over (F8)", on_step_over);
    TB("Step Out (\xe2\x87\xa7""F8)", on_step_out);
    TB("Frame+1", on_frame);
    TB("Run to Cursor", on_run_to);
#undef TB
    w->status = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->status), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(w->status), TRUE);
    gtk_label_set_xalign(w->status, 1.0);
    gtk_label_set_ellipsize(w->status, PANGO_ELLIPSIZE_START);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->status));
    return bar;
}

static GtkWidget *build_prompt(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = mono_view(&w->prompt_out, FALSE);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b, *save, *pop, *popbox;
    GtkEventController *keys;
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly ($4000-$FFFF)", "disassembly.asm" },
        { "ram", "Console RAM ($1800-$27FF)", "ram.bin" },
        { "mem", "Memory (the whole 64K bus)", "memory.bin" } };
    unsigned i;

    w->prompt_scroll = GTK_SCROLLED_WINDOW(scroll);
    gtk_text_view_set_wrap_mode(w->prompt_out, GTK_WRAP_WORD_CHAR);
    w->prompt_in = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->prompt_in,
        "Command (help, step, go, bpset, wpset, print, dump, cart, maria, ...) \xe2\x80\x94 Tab completes");
    gtk_widget_set_hexpand(GTK_WIDGET(w->prompt_in), TRUE);
    g_signal_connect(w->prompt_in, "activate", G_CALLBACK(on_prompt), w);
    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_prompt_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->prompt_in), keys);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->prompt_in));
    b = gtk_button_new_with_label("Load symbols\xe2\x80\xa6");
    g_signal_connect(b, "clicked", G_CALLBACK(on_symbols), w);
    gtk_box_append(GTK_BOX(row), b);

    save = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(save), "Save\xe2\x80\xa6");
    pop = gtk_popover_new();
    popbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    for (i = 0; i < G_N_ELEMENTS(saves); i++) {
        GtkWidget *sb = gtk_button_new_with_label(saves[i].title);
        gtk_widget_add_css_class(sb, "flat");
        g_object_set_data(G_OBJECT(sb), "kind", (gpointer)saves[i].kind);
        g_object_set_data(G_OBJECT(sb), "title", (gpointer)saves[i].title);
        g_object_set_data(G_OBJECT(sb), "file", (gpointer)saves[i].file);
        g_signal_connect(sb, "clicked", G_CALLBACK(on_save), w);
        gtk_box_append(GTK_BOX(popbox), sb);
    }
    gtk_popover_set_child(GTK_POPOVER(pop), popbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(save), pop);
    gtk_box_append(GTK_BOX(row), save);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    set_text(w->prompt_out,
             "Atari 7800 debugger (MAME's engine). Type 'help' for every command; "
             "'cart', 'maria' and 'labels <file>' are this app's own.\n");
    return padded(box);
}

static GtkWidget *build_cpu(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *regs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *flags = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *mem = mono_view(&w->mem_view, FALSE);
    GtkWidget *edit = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *note;
    static const char *const names[6] = { "PC", "SP", "A", "X", "Y", "P" };
    static const char *const fnames[6] = { "N", "V", "D", "I", "Z", "C" };
    int i;
    for (i = 0; i < 6; i++) {
        w->reg[i] = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_max_length(w->reg[i], 6);
        gtk_editable_set_width_chars(GTK_EDITABLE(w->reg[i]), 5);
        g_object_set_data(G_OBJECT(w->reg[i]), "reg", GINT_TO_POINTER(i));
        g_signal_connect(w->reg[i], "activate", G_CALLBACK(on_reg_activate), w);
        gtk_box_append(GTK_BOX(regs), labeled(names[i], GTK_WIDGET(w->reg[i])));
    }
    for (i = 0; i < 6; i++) {
        w->flag[i] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(fnames[i]));
        g_object_set_data(G_OBJECT(w->flag[i]), "flag", GINT_TO_POINTER(i));
        g_signal_connect(w->flag[i], "toggled", G_CALLBACK(on_flag_toggled), w);
        gtk_box_append(GTK_BOX(flags), GTK_WIDGET(w->flag[i]));
    }
    w->cycles = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->cycles), "dim-label");
    gtk_box_append(GTK_BOX(flags), GTK_WIDGET(w->cycles));

    w->mem_from = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->mem_from), 10);
    gtk_entry_set_placeholder_text(w->mem_from, "$1800");
    g_signal_connect(w->mem_from, "activate", G_CALLBACK(on_mem_from), w);
    note = gtk_label_new("RAM is $1800-$27FF (zero page and the stack mirror $2040 and $2140); "
                         "reads here have no side effects");
    gtk_widget_add_css_class(note, "dim-label");
    gtk_box_append(GTK_BOX(nav), labeled("Memory from", GTK_WIDGET(w->mem_from)));
    gtk_box_append(GTK_BOX(nav), note);

    w->mem_addr = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->mem_addr), 8);
    gtk_entry_set_placeholder_text(w->mem_addr, "$1800");
    w->mem_val = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->mem_val), 4);
    gtk_entry_set_placeholder_text(w->mem_val, "$00");
    g_signal_connect(w->mem_val, "activate", G_CALLBACK(on_mem_write), w);
    gtk_box_append(GTK_BOX(edit), labeled("Write address", GTK_WIDGET(w->mem_addr)));
    gtk_box_append(GTK_BOX(edit), labeled("value", GTK_WIDGET(w->mem_val)));
    gtk_box_append(GTK_BOX(edit), gtk_label_new("(RAM takes it; labels work as addresses)"));

    gtk_box_append(GTK_BOX(box), regs);
    gtk_box_append(GTK_BOX(box), flags);
    gtk_box_append(GTK_BOX(box), nav);
    gtk_box_append(GTK_BOX(box), mem);
    gtk_box_append(GTK_BOX(box), edit);
    w->mem_top = 0x1800;
    return padded(box);
}

static GtkWidget *build_disasm(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = mono_view(&w->disasm, FALSE);
    GtkGesture *click = gtk_gesture_click_new();
    GtkEventController *scrollc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GtkWidget *hint;
    char rgb[16];

    g_snprintf(rgb, sizeof rgb, "#%06x", A7800SESSION_ACCENT_RGB);
    gtk_text_buffer_create_tag(b, "pc", "background", rgb, "foreground", "#000000", NULL);
    gtk_text_buffer_create_tag(b, "sel", "underline", PANGO_UNDERLINE_SINGLE,
                               "weight", PANGO_WEIGHT_BOLD, NULL);

    w->follow_pc = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow PC"));
    gtk_check_button_set_active(w->follow_pc, TRUE);
    g_signal_connect(w->follow_pc, "toggled", G_CALLBACK(on_follow_toggled), w);
    w->jump = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->jump, "$C000 or label");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->jump), 14);
    g_signal_connect(w->jump, "activate", G_CALLBACK(on_jump), w);
    hint = gtk_label_new("Click selects a line, double-click or F9 toggles its "
                         "breakpoint; scroll to browse");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->follow_pc));
    gtk_box_append(GTK_BOX(row), labeled("Jump to", GTK_WIDGET(w->jump)));
    gtk_box_append(GTK_BOX(row), hint);

    g_signal_connect(click, "pressed", G_CALLBACK(on_disasm_click), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), GTK_EVENT_CONTROLLER(click));
    g_signal_connect(scrollc, "scroll", G_CALLBACK(on_disasm_scroll), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), scrollc);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);

    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), scroll);
    w->disasm_top = 0xC000;
    w->selected = -1;
    return padded(box);
}

static GtkWidget *build_maria(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *regs = mono_view(&w->maria_text, FALSE);
    GtkWidget *dll = mono_view(&w->dll_text, FALSE);
    GtkWidget *pal_label = gtk_label_new("Palettes: BACKGRND, then P0-P7 (colours 1-3 each)");

    gtk_widget_set_size_request(left, 520, -1);
    gtk_widget_set_hexpand(left, FALSE);
    gtk_box_append(GTK_BOX(left), regs);
    gtk_widget_add_css_class(pal_label, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(pal_label), 0);
    gtk_box_append(GTK_BOX(left), pal_label);
    w->palette_pic = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_can_shrink(w->palette_pic, FALSE);
    gtk_widget_set_halign(GTK_WIDGET(w->palette_pic), GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(left), GTK_WIDGET(w->palette_pic));

    gtk_box_append(GTK_BOX(right), gtk_label_new("Display list list"));
    gtk_box_append(GTK_BOX(right), dll);

    gtk_box_append(GTK_BOX(box), left);
    gtk_box_append(GTK_BOX(box), right);
    w->palette_px = g_new0(guint32, A7800DEBUG_PALETTE_WIDTH * A7800DEBUG_PALETTE_HEIGHT);
    return padded(box);
}

static GtkWidget *build_io(DbgWin *w)
{
    return padded(mono_view(&w->io_text, FALSE));
}

static GtkWidget *build_breaks(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add, *clear;
    static const char *const types[] = { "Execute", "Read", "Write", "Read/Write", NULL };

    w->bp_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->bp_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(w->bp_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    w->bp_type = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(types));
    w->bp_start = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_start, "$C000 / label");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->bp_start), 12);
    w->bp_end = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_end, "(end)");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->bp_end), 8);
    w->bp_cond = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_cond, "condition (a == $FF && b@$80 > 3) \xe2\x80\x94 optional");
    gtk_widget_set_hexpand(GTK_WIDGET(w->bp_cond), TRUE);
    g_signal_connect(w->bp_start, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_end, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_cond, "activate", G_CALLBACK(on_bp_add), w);
    add = gtk_button_new_with_label("Add");
    g_signal_connect(add, "clicked", G_CALLBACK(on_bp_add), w);
    clear = gtk_button_new_with_label("Clear all");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_bp_clear), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_type));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_start));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_end));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_cond));
    gtk_box_append(GTK_BOX(row), add);
    gtk_box_append(GTK_BOX(row), clear);
    w->bp_msg = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->bp_msg), "a7800-accent-text");
    gtk_label_set_xalign(w->bp_msg, 0);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->bp_msg));
    return padded(box);
}

static GtkWidget *build_cart(DbgWin *w)
{
    return padded(mono_view(&w->cart_text, FALSE));
}

static void on_destroy(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    if (w->timer) g_source_remove(w->timer);
    a7800debug_detach(w->dbg);
    g_free(w->palette_px);
    if (g_win == w) g_win = NULL;
    g_free(w);
}

static void attach_and_refresh(DbgWin *w)
{
    a7800debug_attach(w->dbg);     /* stops the machine */
    w->stopped_machine = a7800session_is_running(w->session) != 0;
    w->seen_generation = a7800debug_generation(w->dbg);
    w->was_stopped = a7800debug_is_stopped(w->dbg) != 0;
    refresh_all(w);
}

void a7800_debugger_show(GtkWindow *parent, a7800session *session)
{
    DbgWin *w;
    GtkWidget *toolbar, *header, *root, *notebook;
    GtkEventController *keys;

    if (g_win) {
        gtk_window_present(g_win->win);
        attach_and_refresh(g_win);
        return;
    }
    w = g_new0(DbgWin, 1);
    w->session = session;
    w->dbg = a7800session_debugger(session);
    g_win = w;

    w->win = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(w->win, "Debugger");
    gtk_window_set_default_size(w->win, 1100, 800);
    gtk_window_set_transient_for(w->win, parent);
    gtk_window_set_application(w->win, gtk_window_get_application(parent));
    gtk_window_set_destroy_with_parent(w->win, TRUE);
    g_signal_connect(w->win, "close-request", G_CALLBACK(on_close), w);
    g_signal_connect(w->win, "destroy", G_CALLBACK(on_destroy), w);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), build_toolbar(w));
    notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_prompt(w), gtk_label_new("Console"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_cpu(w), gtk_label_new("CPU & Memory"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_disasm(w), gtk_label_new("Disassembly"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_maria(w), gtk_label_new("MARIA"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_io(w), gtk_label_new("I/O"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_cart(w), gtk_label_new("Cartridge"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_breaks(w), gtk_label_new("Breakpoints & Watchpoints"));
    gtk_box_append(GTK_BOX(root), notebook);
    {
        const char *tab = g_getenv("A7800_DEBUGGER_TAB");
        if (tab && *tab) gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), atoi(tab));
    }

    header = adw_header_bar_new();
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(w->win), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->win), keys);

    attach_and_refresh(w);
    w->timer = g_timeout_add(100, tick_refresh, w);
    gtk_window_present(w->win);
}

void a7800_debugger_toggle(GtkWindow *parent, a7800session *session)
{
    if (g_win && gtk_widget_get_visible(GTK_WIDGET(g_win->win)))
        hide_window(g_win);
    else
        a7800_debugger_show(parent, session);
}
