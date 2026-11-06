/**
 * ggswan - Game Gear emulator for the WonderSwan (nileswan)
 * Z80 -> V30MZ (x86, 16-bit) dynamic recompiler: the translator.
 *
 * The translator itself is portable C (it only writes bytes into a
 * buffer), so it can be tested on a PC: host/drctest.py runs the
 * generated code in an x86 emulator and compares it with the interpreter.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Register mapping inside translated code:
 *   AL = A   AH = F (when "materialized")   CX = BC   DX = DE   BX = HL
 *   BP = SP  SI, DI, ES = scratch           DS = SS = 0 (IRAM)
 * IX, IY, the shadow registers etc. stay in the z80_t structure.
 *
 * Z80 flags S, Z, H, P/V and C sit at the same bit positions as x86
 * SF, ZF, AF, PF and CF in AH after LAHF; the translator keeps the flags
 * of the last flag-setting instruction in the x86 FLAGS register and only
 * writes them to AH when something needs them there (block exits, PUSH AF,
 * x86 instructions that would destroy them).
 *
 * Memory: Z80 address X is ES:X with ES = segtab[X >> 13] (each segment is
 * pre-adjusted so that the offset is X itself; 8 KiB steps so that the RAM
 * mirror at E000-FFFF works too). Work RAM (C000-DFFF) is also reachable as
 * DS:[X + ram_delta].
 */

#ifndef GGSWAN_DRC_H_
#define GGSWAN_DRC_H_

#include <stdint.h>

/// Runtime state shared by translated code and the dispatcher (in IRAM).
typedef struct {
    uint16_t segtab[8];   // ES value for each 8 KiB of the Z80 address space
    int16_t cycles;       // cycles left; blocks subtract their cycles
    uint8_t op;           // helper operation (DRC_H_*)
    uint8_t val;          // helper value (byte in / out)
    uint16_t arg;         // helper argument (address or port)
} drc_state_t;

/// Helper operations: generated code sets drc_state.op (and arg/val),
/// then calls the helper routine. The helper routine stores AX, CX, DX,
/// BX, BP into the z80_t structure, runs drc_helper(), reloads them.
enum {
    DRC_H_WRITE = 1,   // write val to Z80 address arg
    DRC_H_OUT,         // OUT (arg), val
    DRC_H_IN,          // val = IN (arg)
    DRC_H_STEP,        // interpret the instruction at arg (not a jump);
                       // the helper subtracts its cycles
};

typedef struct {
    uint16_t z80_addr;     // IRAM address of the z80_t structure
    uint16_t state_addr;   // IRAM address of the drc_state_t structure
    uint16_t exit_off;     // code-segment offset of the exit routine
    uint16_t helper_off;   // code-segment offset of the helper routine
    int16_t ram_delta;     // IRAM address - Z80 address, for C000-DFFF
} drc_env_t;

typedef struct {
    uint16_t length;       // bytes of x86 code written
    uint16_t z80_instr;    // Z80 instructions translated
    uint16_t z80_end;      // address after the last translated instruction
} drc_result_t;

/// Translate the block at Z80 address pc. The code is written to out,
/// which will be placed at code-segment offset out_off. Returns false if
/// the first instruction can not be translated (the caller interprets it).
int drc_translate(const drc_env_t *env, uint16_t pc, uint8_t *out,
                  uint16_t out_off, uint16_t out_max, drc_result_t *res);

#endif
