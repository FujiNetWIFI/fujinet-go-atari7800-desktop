/*
 * 6502 disassembler. See m6502dasm.h.
 *
 * The table is the documented NMOS 6502 opcode matrix (the 7800's Sally is
 * one). Every other opcode, the 65C02 additions included, is a one-byte
 * "???" rather than a guessed undocumented instruction.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m6502dasm.h"

#include <stdio.h>
#include <string.h>

/* Addressing modes. */
enum {
    IMP,   /* implied              CLC        */
    ACC,   /* accumulator          ASL        */
    IMM,   /* immediate            LDA #$nn   */
    ZP,    /* zero page            LDA $nn    */
    ZPX,   /* zero page,X          LDA $nn,X  */
    ZPY,   /* zero page,Y          LDX $nn,Y  */
    IZX,   /* (zero page,X)        LDA ($nn,X)*/
    IZY,   /* (zero page),Y        LDA ($nn),Y*/
    ABS,   /* absolute             LDA $nnnn  */
    ABX,   /* absolute,X           LDA $nnnn,X*/
    ABY,   /* absolute,Y           LDA $nnnn,Y*/
    IND,   /* (absolute)           JMP ($nnnn)*/
    REL,   /* relative             BNE $nnnn  */
    ILL,   /* not a documented opcode          */
};

static const uint8_t k_len[] = {
    [IMP] = 1, [ACC] = 1, [IMM] = 2, [ZP] = 2,  [ZPX] = 2, [ZPY] = 2,
    [IZX] = 2, [IZY] = 2, [ABS] = 3, [ABX] = 3, [ABY] = 3,
    [IND] = 3, [REL] = 2, [ILL] = 1,
};

typedef struct {
    const char *mn;
    uint8_t mode;
} op_t;

#define X { "???", ILL }

/* Transcribed row by row from the NMOS 6502 opcode matrix. */
static const op_t k_ops[256] = {
/* 00 */ {"BRK",IMP},{"ORA",IZX},X,X,X,{"ORA",ZP},{"ASL",ZP},X,
         {"PHP",IMP},{"ORA",IMM},{"ASL",ACC},X,X,{"ORA",ABS},{"ASL",ABS},X,
/* 10 */ {"BPL",REL},{"ORA",IZY},X,X,X,{"ORA",ZPX},{"ASL",ZPX},X,
         {"CLC",IMP},{"ORA",ABY},X,X,X,{"ORA",ABX},{"ASL",ABX},X,
/* 20 */ {"JSR",ABS},{"AND",IZX},X,X,{"BIT",ZP},{"AND",ZP},{"ROL",ZP},X,
         {"PLP",IMP},{"AND",IMM},{"ROL",ACC},X,{"BIT",ABS},{"AND",ABS},{"ROL",ABS},X,
/* 30 */ {"BMI",REL},{"AND",IZY},X,X,X,{"AND",ZPX},{"ROL",ZPX},X,
         {"SEC",IMP},{"AND",ABY},X,X,X,{"AND",ABX},{"ROL",ABX},X,
/* 40 */ {"RTI",IMP},{"EOR",IZX},X,X,X,{"EOR",ZP},{"LSR",ZP},X,
         {"PHA",IMP},{"EOR",IMM},{"LSR",ACC},X,{"JMP",ABS},{"EOR",ABS},{"LSR",ABS},X,
/* 50 */ {"BVC",REL},{"EOR",IZY},X,X,X,{"EOR",ZPX},{"LSR",ZPX},X,
         {"CLI",IMP},{"EOR",ABY},X,X,X,{"EOR",ABX},{"LSR",ABX},X,
/* 60 */ {"RTS",IMP},{"ADC",IZX},X,X,X,{"ADC",ZP},{"ROR",ZP},X,
         {"PLA",IMP},{"ADC",IMM},{"ROR",ACC},X,{"JMP",IND},{"ADC",ABS},{"ROR",ABS},X,
/* 70 */ {"BVS",REL},{"ADC",IZY},X,X,X,{"ADC",ZPX},{"ROR",ZPX},X,
         {"SEI",IMP},{"ADC",ABY},X,X,X,{"ADC",ABX},{"ROR",ABX},X,
/* 80 */ X,{"STA",IZX},X,X,{"STY",ZP},{"STA",ZP},{"STX",ZP},X,
         {"DEY",IMP},X,{"TXA",IMP},X,{"STY",ABS},{"STA",ABS},{"STX",ABS},X,
/* 90 */ {"BCC",REL},{"STA",IZY},X,X,{"STY",ZPX},{"STA",ZPX},{"STX",ZPY},X,
         {"TYA",IMP},{"STA",ABY},{"TXS",IMP},X,X,{"STA",ABX},X,X,
/* A0 */ {"LDY",IMM},{"LDA",IZX},{"LDX",IMM},X,{"LDY",ZP},{"LDA",ZP},{"LDX",ZP},X,
         {"TAY",IMP},{"LDA",IMM},{"TAX",IMP},X,{"LDY",ABS},{"LDA",ABS},{"LDX",ABS},X,
/* B0 */ {"BCS",REL},{"LDA",IZY},X,X,{"LDY",ZPX},{"LDA",ZPX},{"LDX",ZPY},X,
         {"CLV",IMP},{"LDA",ABY},{"TSX",IMP},X,{"LDY",ABX},{"LDA",ABX},{"LDX",ABY},X,
/* C0 */ {"CPY",IMM},{"CMP",IZX},X,X,{"CPY",ZP},{"CMP",ZP},{"DEC",ZP},X,
         {"INY",IMP},{"CMP",IMM},{"DEX",IMP},X,{"CPY",ABS},{"CMP",ABS},{"DEC",ABS},X,
/* D0 */ {"BNE",REL},{"CMP",IZY},X,X,X,{"CMP",ZPX},{"DEC",ZPX},X,
         {"CLD",IMP},{"CMP",ABY},X,X,X,{"CMP",ABX},{"DEC",ABX},X,
/* E0 */ {"CPX",IMM},{"SBC",IZX},X,X,{"CPX",ZP},{"SBC",ZP},{"INC",ZP},X,
         {"INX",IMP},{"SBC",IMM},{"NOP",IMP},X,{"CPX",ABS},{"SBC",ABS},{"INC",ABS},X,
/* F0 */ {"BEQ",REL},{"SBC",IZY},X,X,X,{"SBC",ZPX},{"INC",ZPX},X,
         {"SED",IMP},{"SBC",ABY},X,X,X,{"SBC",ABX},{"INC",ABX},X,
};

#undef X

int a7800dasm_length(uint8_t opcode)
{
    return k_len[k_ops[opcode].mode];
}

static uint8_t flags_for(uint8_t opcode, const op_t *op)
{
    uint8_t f = 0;

    switch (op->mode) {
    case ILL:
        return A7800DASM_ILLEGAL | A7800DASM_HALT;
    case REL:
        f |= A7800DASM_RELATIVE | A7800DASM_JUMP;
        return f | A7800DASM_COND;      /* the 6502 has no unconditional one */
    default:
        break;
    }

    switch (opcode) {
    case 0x20: return A7800DASM_CALL;                    /* JSR */
    case 0x4C: case 0x6C: return A7800DASM_JUMP; /* JMP */
    case 0x40: case 0x60: return A7800DASM_RET;          /* RTI, RTS */
    case 0x00: return A7800DASM_HALT;                    /* BRK */
    default:   return 0;
    }
}

int a7800dasm_decode(uint16_t addr, const uint8_t *bytes,
                      a7800dasm_line *out)
{
    const uint8_t opcode = bytes[0];
    const op_t *op = &k_ops[opcode];
    const int len = k_len[op->mode];
    const uint8_t lo = bytes[1];
    const uint8_t hi = bytes[2];
    const uint16_t abs16 = (uint16_t)(lo | (hi << 8));
    int i;

    memset(out, 0, sizeof(*out));
    out->addr = addr;
    out->len = (uint8_t)len;
    for (i = 0; i < len; i++)
        out->bytes[i] = bytes[i];
    out->flags = flags_for(opcode, op);
    out->imm = op->mode == IMM;

    switch (op->mode) {
    case ILL:
        snprintf(out->text, sizeof(out->text), "??? $%02X", opcode);
        break;
    case IMP:
        snprintf(out->text, sizeof(out->text), "%s", op->mn);
        break;
    case ACC:
        snprintf(out->text, sizeof(out->text), "%s A", op->mn);
        break;
    case IMM:
        snprintf(out->text, sizeof(out->text), "%s #$%02X", op->mn, lo);
        break;
    case ZP:
        snprintf(out->text, sizeof(out->text), "%s $%02X", op->mn, lo);
        break;
    case ZPX:
        snprintf(out->text, sizeof(out->text), "%s $%02X,X", op->mn, lo);
        break;
    case ZPY:
        snprintf(out->text, sizeof(out->text), "%s $%02X,Y", op->mn, lo);
        break;
    case IZX:
        snprintf(out->text, sizeof(out->text), "%s ($%02X,X)", op->mn, lo);
        break;
    case IZY:
        snprintf(out->text, sizeof(out->text), "%s ($%02X),Y", op->mn, lo);
        break;
    case ABS:
        snprintf(out->text, sizeof(out->text), "%s $%04X", op->mn, abs16);
        out->target = abs16;
        break;
    case ABX:
        snprintf(out->text, sizeof(out->text), "%s $%04X,X", op->mn, abs16);
        break;
    case ABY:
        snprintf(out->text, sizeof(out->text), "%s $%04X,Y", op->mn, abs16);
        break;
    case IND:
        snprintf(out->text, sizeof(out->text), "%s ($%04X)", op->mn, abs16);
        out->target = abs16;
        break;
    case REL: {
        /* The branch is relative to the address AFTER the instruction. */
        const uint16_t dest = (uint16_t)(addr + 2 + (int8_t)lo);
        snprintf(out->text, sizeof(out->text), "%s $%04X", op->mn, dest);
        out->target = dest;
        break;
    }
    default:
        snprintf(out->text, sizeof(out->text), "%s", op->mn);
        break;
    }

    /* ABS on a non-branching instruction is an operand address, not a control
     * transfer -- only keep target where it means "goes here". */
    if (!(out->flags & (A7800DASM_JUMP | A7800DASM_CALL)))
        out->target = 0;

    return len;
}
