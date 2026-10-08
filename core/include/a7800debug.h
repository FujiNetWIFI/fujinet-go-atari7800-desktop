/*
 * a7800debug -- the debugger contract: MAME's own debugger engine (its 6502
 * disassembler, breakpoints and watchpoints, expression evaluator and
 * command console) behind a C API the four native debugger windows share.
 *
 * MAME's debugger is always on in this app (it cannot be attached to a
 * running machine), so "attach" only stops the machine, as on every sibling
 * when the debugger window opens, and "detach" lets it run on.
 *
 * Calls are safe from the UI thread at any time: they run on the emulation
 * thread at its next turn and wait for it. Inspection is meant for the
 * stopped state; while running it answers with whatever is current.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef A7800DEBUG_H
#define A7800DEBUG_H

#include <stdint.h>

#include "a7800session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lazily created; lives as long as the session. */
a7800debug *a7800debug_get(a7800session *s);

/* ---- attach / stop / go ------------------------------------------------------ */
/* Stop at the next instruction (the window opened). Idempotent. */
void a7800debug_attach(a7800debug *d);
/* Let the machine run (the window closed). Breakpoints are kept. */
void a7800debug_detach(a7800debug *d);
int  a7800debug_is_attached(a7800debug *d);

int  a7800debug_is_stopped(a7800debug *d);
void a7800debug_stop(a7800debug *d);         /* break at the next instruction */
void a7800debug_resume(a7800debug *d);       /* "run" */
/* Why the machine last stopped ("stopped", "breakpoint 1 at $C123",
 * "watchpoint 2: write $2000", "step", ...) and the PC, -1 if none.
 * Returns length. */
int  a7800debug_stop_reason(a7800debug *d, char *dst, int dstsz, int *address);
/* Bumped whenever a command ran or the machine stopped/resumed, so a window
 * knows when to refresh. */
unsigned a7800debug_generation(a7800debug *d);

/* ---- the prompt -------------------------------------------------------------- */
/* Run one of MAME's debugger commands ("help" lists them: step, over, out,
 * go [addr], gvblank, bpset <addr>[,cond], wpset <addr>,<len>,<r|w|rw>,
 * print <expr>, dump, find, trace ...) and a few of the family's own:
 * "cart" (the FujiNet cartridge), "maria" (its registers and display
 * list), "labels <file>" (load symbols). Output text into dst; returns its
 * length. Commands that would restore or replay the machine's past (state
 * save and load, hardreset, exit) are refused: they would replay mailbox
 * transactions fujinet-pc has already acted on. */
int  a7800debug_command(a7800debug *d, const char *command, char *dst, int dstsz);
/* Completions for a prefix (commands and symbols), one per line. Returns
 * the count. */
int  a7800debug_completions(a7800debug *d, const char *prefix, char *dst, int dstsz);

/* ---- stepping shortcuts (the toolbar) --------------------------------------- */
void a7800debug_step(a7800debug *d);          /* F7: one instruction */
void a7800debug_step_over(a7800debug *d);     /* F8: over a JSR */
void a7800debug_step_out(a7800debug *d);      /* Shift+F8: to the RTS/RTI */
void a7800debug_frame(a7800debug *d);         /* to the next VBLANK */
/* Run until PC == addr. */
void a7800debug_run_to(a7800debug *d, uint16_t addr);

/* ---- CPU --------------------------------------------------------------------- */
typedef struct {
    int pc, sp, a, x, y, ps;
    int n, v, d, i, z, c;
    uint64_t total_cycles;
    int scanline, dot;          /* where the screen's beam is */
    uint32_t frame;
} a7800debug_cpu;
void a7800debug_cpu_get(a7800debug *d, a7800debug_cpu *out);
typedef enum { A7800_REG_PC, A7800_REG_SP, A7800_REG_A, A7800_REG_X, A7800_REG_Y,
               A7800_REG_PS, A7800_FLAG_N, A7800_FLAG_V, A7800_FLAG_D,
               A7800_FLAG_I, A7800_FLAG_Z, A7800_FLAG_C } a7800debug_reg;
void a7800debug_cpu_set(a7800debug *d, int reg, int value);

/* ---- MARIA ------------------------------------------------------------------- */
typedef struct {
    uint8_t ctrl;               /* the last CTRL written ($3C) */
    int dma_on;                 /* DMA enabled */
    int color_kill, kangaroo, border_control;
    int char_width;             /* 1 or 2 bytes a character */
    int read_mode;              /* 0 160A/B, 2 320B/D, 3 320A/C */
    uint8_t charbase;           /* CHARBASE ($34) */
    uint16_t dpp;               /* the display list list's address ($2C/$30) */
    uint16_t dll, dl;           /* where DMA is in the list now */
    int offset, holey, dli;     /* the current zone's */
    int vblank;                 /* MSTAT bit 7 */
    uint8_t palette[32];        /* BACKGRND and P0C1 ... P7C3, as written */
    int scanline;
} a7800debug_maria;
void a7800debug_maria_get(a7800debug *d, a7800debug_maria *out);

/* The display list list as text, zone by zone ("$1800: 16 lines, DL $1900,
 * DLI"), one per line. Returns length. */
int  a7800debug_dll_text(a7800debug *d, char *dst, int dstsz);

/* A palette entry (0-255, the console's colours) as XRGB. */
uint32_t a7800debug_color(a7800debug *d, uint8_t index);
/* The 8 palettes as a 256x64 XRGB image (32x16 swatches: BACKGRND, then
 * P0C1..P7C3 in rows of 8 palettes x 3 colours, BACKGRND repeated). Returns
 * 1 when drawn. */
#define A7800DEBUG_PALETTE_WIDTH  256
#define A7800DEBUG_PALETTE_HEIGHT 64
int  a7800debug_palette_image(a7800debug *d, uint32_t *dst);

/* ---- TIA, RIOT, POKEY and the controls ---------------------------------------- */
typedef struct {
    uint8_t inptctrl;           /* as the FujiNet cartridge last saw it written */
    int inpt_locked;
    uint8_t audc[2], audf[2], audv[2];   /* the TIA's two sound channels */
    uint8_t swcha, swchb;       /* RIOT port A (joysticks) and B (console) */
    uint8_t inpt[6];            /* TIA INPT0-5 (buttons, paddle lines) */
    int pokey_present;          /* a POKEY on the running cartridge */
    uint8_t pokey_audf[4], pokey_audc[4], pokey_audctl;
    unsigned held[2];           /* a7800_action bits held per port */
    int port_type[2];           /* a7800_ctrl_type in effect */
} a7800debug_io;
void a7800debug_io_get(a7800debug *d, a7800debug_io *out);

/* ---- memory ------------------------------------------------------------------ */
/* 6502 bus reads without side effects. */
int  a7800debug_read(a7800debug *d, uint16_t addr, uint8_t *dst, int n);
/* A debugger edit (RAM takes it). */
void a7800debug_write(a7800debug *d, uint16_t addr, uint8_t value);

/* ---- disassembly --------------------------------------------------------------- */
typedef struct {
    uint16_t address;
    int is_pc;
    int has_breakpoint;
    int is_code;            /* always 1: the 6502 has no code/data map here */
    char bytes[16];
    char label[48];
    char disasm[64];
    char comment[64];
} a7800debug_line;
/* Up to `max` lines starting at `addr`. Returns the count; *pc_line is the
 * index of the PC's line in `out`, -1 when not among them. */
int  a7800debug_disassemble(a7800debug *d, uint16_t addr, a7800debug_line *out,
                            int max, int *pc_line);
/* The address `rows` disassembly rows before (negative) or after `addr` --
 * for scrolling, and for "Follow PC" (PC a third of the way down). */
int  a7800debug_row_address(a7800debug *d, uint16_t addr, int rows);
/* Address of a label, or -1. Label of an address into dst (may be empty). */
int  a7800debug_label_address(a7800debug *d, const char *label);
int  a7800debug_address_label(a7800debug *d, uint16_t addr, char *dst, int dstsz);
int  a7800debug_set_label(a7800debug *d, uint16_t addr, const char *label);
/* Load a symbol file: ld65 -Ln / VICE ("al C:1234 .name"), a ca65 .dbg, or
 * "name = $1234" lines. With path NULL, <cart>.lbl/.dbg/.sym next to the
 * opened cartridge. Message into msg; returns labels loaded or -1. */
int  a7800debug_load_symbols(a7800debug *d, const char *path, char *msg, int msgsz);

/* ---- the FujiNet cartridge ------------------------------------------------------ */
typedef struct {
    int present, link_up, worker, booted_image;
    int mode;               /* 0 boot block, 1 loading, 2 game, 3 FujiNet app */
    char mode_name[24];
    int handover;           /* 0 none, 1 the BIOS, 2 the loader */
    int load_pct;
    char kind[16];          /* the live image's mapper ("a78_sg" ...) */
    uint32_t live_crc;
    int staged;
    char staged_kind[16];
    uint32_t staged_crc;
    int hsc;                /* bit0 ROM, bit1 on, bit2 saved, bit3 unsaved */
    int tv;                 /* 0 NTSC, 1 PAL */
    uint8_t ackseq, last_error, boot_state, boot_pct, boot_err;
    uint32_t queue_depth;
    char link_error[128];
} a7800debug_cart;
void a7800debug_cart_get(a7800debug *d, a7800debug_cart *out);
/* One line: "FujiNet cartridge: game (a78_sg, CRC 12345678), link up". */
int  a7800debug_cart_info(a7800debug *d, char *dst, int dstsz);

/* ---- breakpoints and watchpoints ------------------------------------------------ */
typedef enum { A7800DEBUG_BP_EXEC = 1, A7800DEBUG_BP_READ = 2, A7800DEBUG_BP_WRITE = 4 } a7800debug_bp_type;
typedef struct {
    int id;                /* breakpoints 1.., watchpoints 1001.. */
    int type;              /* a7800debug_bp_type bits */
    uint16_t start, end;
    int enabled;
    char condition[128];
} a7800debug_breakpoint;
/* Execute breakpoint at addr: added if absent, removed if present. Returns
 * 1 when one is now set. */
int  a7800debug_breakpoint_toggle(a7800debug *d, uint16_t addr);
int  a7800debug_breakpoint_check(a7800debug *d, uint16_t addr);
/* EXEC is a breakpoint at start; READ/WRITE a watchpoint over start..end.
 * Returns the new id, or -1 if the condition does not parse. */
int  a7800debug_breakpoint_add(a7800debug *d, int type, uint16_t start, uint16_t end,
                               const char *condition);
void a7800debug_breakpoint_remove(a7800debug *d, int id);
void a7800debug_breakpoint_enable(a7800debug *d, int id, int enabled);
int  a7800debug_breakpoint_list(a7800debug *d, a7800debug_breakpoint *out, int max);
void a7800debug_breakpoint_clear(a7800debug *d);

/* ---- files -------------------------------------------------------------------- */
/* Save with the path a native file picker supplied. kind: "dis" (the
 * disassembly of $4000-$FFFF), "ram" (the console's RAM, $1800-$27FF),
 * "mem" (the whole 64K bus as the 6502 sees it now). Message into msg;
 * returns 0 or -1. */
int  a7800debug_save(a7800debug *d, const char *kind, const char *path,
                     char *msg, int msgsz);

#ifdef __cplusplus
}
#endif

#endif /* A7800DEBUG_H */
