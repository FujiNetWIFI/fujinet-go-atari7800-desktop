/*
 * 6502 disassembler, table driven: the decoder the trace ring's formatting
 * and branch filter use (the entries carry their opcode bytes, so a banked
 * cartridge does no harm). Written fresh; no code from any emulator's
 * debugger is present.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef A7800_M6502DASM_H
#define A7800_M6502DASM_H

#include <stdint.h>

#define A7800DASM_JUMP     0x01
#define A7800DASM_CALL     0x02
#define A7800DASM_RET      0x04
#define A7800DASM_COND     0x08
#define A7800DASM_RELATIVE 0x10
#define A7800DASM_HALT     0x20  /* BRK or an illegal opcode */
#define A7800DASM_ILLEGAL  0x40  /* not a documented 6502 opcode */

/* One decoded instruction. */
typedef struct {
    uint16_t addr;
    uint8_t  len;         /* 1..3 */
    uint8_t  bytes[3];
    char     text[32];    /* "LDA $C0F0,X" */
    uint16_t target;      /* branch/jump/call destination, else 0 */
    uint8_t  flags;       /* A7800DASM_* */
    uint8_t  imm;         /* immediate operand: its fetch is not a data read */
} a7800dasm_line;

/* Decode one instruction from `bytes` (at least 3 readable bytes; the length
 * is determined by the opcode alone) placed at `addr`. Returns the length
 * in bytes (1..3). */
int a7800dasm_decode(uint16_t addr, const uint8_t *bytes, a7800dasm_line *out);

/* Instruction length for an opcode, without decoding it. Always 1..3. */
int a7800dasm_length(uint8_t opcode);

#endif /* A7800_M6502DASM_H */
