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
 * Block linking: an exit to a constant address that stays valid (same
 * 16 KiB slot, or slot 0, and no mapper change possible in the block) ends
 * with "JG L; JMP exit; L: JMP exit" and stores L in drc_state.link; the
 * runtime then points the JMP at L to the next block, so translated code
 * runs from block to block until its cycles run out.
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
    uint16_t resume;      // where to continue the block after a helper
    uint8_t reason;       // 0: the block exited, 1: it wants a helper
    uint8_t pad;
    uint16_t link;        // set by every exit: offset of a JMP the runtime
                          // may point at the next block (0: none)
    uint8_t keys[4];      // [0]: 8 if cartridge RAM is at 8000h; [1..3]: the
                          // ROM bank of each slot (block table keys)
    uint16_t table_seg;   // segment of the block table
    uint8_t nolink;       // diagnostics: never jump between blocks
    uint8_t pad2;
    uint16_t nt_base;     // VRAM name table and sprite table: writes there
    uint16_t sat_base;    // need more than the byte stored (see drc_rt.c)
    uint8_t map_top;      // platform's screen map state (main.c)
    uint8_t sprites_dirty;
    uint16_t far_off;     // DYN: far jump to a block in another code bank
    uint16_t far_seg;
    uint16_t cur_seg;     // code segment of the last exit (set by the stubs)
} drc_state_t;

/// Helper operations: generated code sets drc_state.op (and arg/val),
/// then calls the helper routine. The helper routine stores AX, CX, DX,
/// BX, BP into the z80_t structure, runs drc_helper(), reloads them.
enum {
    DRC_H_WRITE = 1,   // write val to Z80 address arg
    DRC_H_OUT,         // OUT (arg), val
    DRC_H_IN,          // val = IN (arg)
    DRC_H_STEP,        // interpret val instructions from arg (no jumps);
                       // the helper subtracts their cycles
    DRC_H_WRITE_MAP,   // write to a mapper register that changes neither
                       // slot 0 nor the slot of the block: it may go on
};

typedef struct {
    uint16_t z80_addr;     // IRAM address of the z80_t structure
    uint16_t state_addr;   // IRAM address of the drc_state_t structure
    uint16_t exit_off;     // code-segment offset of the exit routine
    uint16_t helper_off;   // code-segment offset of the helper routine
    int16_t ram_delta;     // IRAM address - Z80 address, for C000-DFFF
    uint16_t dyn_off;      // offset of the routine that looks up the block
                           // at z80.pc itself (0: exits go to exit_off)
    uint16_t vdp_addr;     // IRAM address of gg.vdp (gg_vdp_t), for the fast
                           // VRAM write path (0: always the helper)
    uint16_t vram_addr;    // IRAM address of the Game Gear VRAM
    uint16_t nt_stub_off;  // routine updating the screen map after a name
                           // table write (SI = VRAM address written)
    uint16_t outi_off;     // routine for runs of OUTI (drc_emit_run_stub),
                           // OUTD's at outi_off + DRC_OUTD_DELTA
    uint16_t ctl_off;      // routine for OUT (BFh),A (drc_emit_ctl_stub; 0: helper)
    uint16_t ldi_off;      // routine for runs of LDI (drc_emit_ldi_stub),
                           // LDD's at ldi_off + DRC_LDD_DELTA (0: interpreted)
} drc_env_t;

#define DRC_OUTD_DELTA 0x100
#define DRC_LDD_DELTA 0x80

typedef struct {
    uint16_t length;       // bytes of x86 code written
    uint16_t z80_instr;    // Z80 instructions translated
    uint16_t z80_end;      // address after the last translated instruction
} drc_result_t;

/// Translate the block at Z80 address pc. The code is written to out,
/// which will be placed at code-segment offset out_off. Returns false if
/// the first instruction can not be translated (the caller interprets it).
/// Write the routine for runs of OUTI (dec = 0) or OUTD; returns its length
uint16_t drc_emit_run_stub(const drc_env_t *env, uint8_t *out, uint16_t out_off, uint8_t dec);

/// Write the routine for runs of LDI (dec = 0) or LDD; returns its length
uint16_t drc_emit_ldi_stub(const drc_env_t *env, uint8_t *out, uint16_t out_off, uint8_t dec);

/// Write the routine for OUT (BFh),A; returns its length
uint16_t drc_emit_ctl_stub(const drc_env_t *env, uint8_t *out, uint16_t out_off);

int drc_translate(const drc_env_t *env, uint16_t pc, uint8_t *out,
                  uint16_t out_off, uint16_t out_max, drc_result_t *res);

#endif
