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

#ifdef GGSWAN_SMS
// (smsswan: 32 name table rows with 224 lines; the queue shorter, so that
// drc_state_t still fits its 512 bytes)
#define DRC_PSG_Q 168
#define DRC_NT_ROWS 32
#else
#define DRC_PSG_Q 184  // PSG writes queued by the recompiled code (<= 255)
#define DRC_NT_ROWS 28
#endif

#include <stdint.h>

/// Runtime state shared by translated code and the dispatcher (in IRAM).
// (64 since patch 138: a road with a scroll value on every line, Super
// Monaco GP's, had its edges in steps of 4 pixels with 24)
#define DRC_MAX_SPLITS 64

typedef struct {
    uint16_t segtab[8];   // ES value for each 8 KiB of the Z80 address space
    int16_t cycles;       // cycles left; blocks subtract their cycles
    uint8_t op;           // helper operation (DRC_H_*)
    uint8_t val;          // helper value (byte in / out)
    uint16_t arg;         // helper argument (address or port)
    uint16_t resume;      // where to continue the block after a helper
    uint8_t reason;       // 0: the block exited, 1: it wants a helper
    uint8_t ic;           // written (never read) by the unlinked exit of a
                          // DYN exit with an inline cache: marks it (drc.c)
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
    uint16_t l0_slot;     // DYN: L0 entry being filled
    uint8_t ram_fail;     // a block in RAM found its code changed
    uint8_t hs_last;      // main.c: VDP register 8 value of the last scroll
                          // split recorded (an equal write changes nothing)
    uint16_t nt_rows[2 * DRC_NT_ROWS]; // main.c: for each Game Gear name table row, the
                          // byte offset of its first and (at 28 +) second
                          // row in the WS screen map, FFFFh: none
    // the line routine at the block exits (drc_rt.c, LINE stub):
    uint8_t fl_off;       // nonzero: not now (diagnostics, self-check, the
                          // per-line test mode, a helper stopped the run)
    uint8_t fast38_bank;  // block table key of the block at 0038h
    int16_t left_base;    // z80.cycles - the run's start in drc_st.cycles
    uint16_t fast38[2];   // the block at 0038h: offset, segment (0: none)
    // main.c: the horizontal scroll splits recorded in this frame (here so
    // that the VDP control routine can record them)
    uint8_t rec_n;
    uint8_t rec_pad[48];  // (the lists were here, 24 entries each: now at
                          // the end; the offsets after them, which the PC
                          // tools know, stay where they were)
    uint16_t nt_flags[8]; // name table routine: WS bits for the Game Gear
                          // entry bits 9-11 (h flip, v flip, palette)
    uint8_t vram_class[65]; // main.c: for each 256-byte VRAM page, bit 0:
                          // in the name table, bit 1: the sprite table;
                          // bit 3: the write goes to the log (main.c:
                          // deferred VRAM writes, see there); bit 4: [64],
                          // past the end (a run that wraps)
    uint8_t defer;        // main.c: VRAM writes deferred (not with X4)
    uint16_t log_top;     // main.c: the log being filled (IRAM): end,
    uint16_t log_end;     // limit, last entry (0: none); entries of VRAM
    uint16_t log_last;    // address (word), length (byte), the bytes
    uint16_t psg_n;       // main.c: PSG writes queued by OUT (n),A (ports
    uint8_t psg_q[DRC_PSG_Q];   // 40-7F), written at the end of the frame
    uint8_t keys_in[2];   // drc_rt.c: IN from port 00h (Start) and DCh (the
                          // joypad), as z80_in: the keys change between frames
    uint8_t rom_mask;     // drc_rt.c: ROM banks - 1 (a power of 2), for the
    uint8_t map_pend;     // mapper routines; bits 2, 3: FFFE, FFFF written by
                          // them, the C side (gg.mapper, gg_slot) not yet
    uint16_t diag_fg;     // diagnostics: at the last scroll split recorded,
    int16_t diag_left;    // frame_given (low word) and the cycles left
    uint16_t log_next;    // the class routine (drc.c): the VRAM address that
                          // joins the last log entry, which it made and is
                          // the top one (FFFFh: none; anything else that
                          // changes the log sets it)
    uint8_t prio_on;      // main.c: screen 2 holds the background tiles with
                          // priority (in front of the sprites): the NT
                          // routine writes them there too (0: the borders'
                          // mask and diagnostics)
    uint8_t rec_line[DRC_MAX_SPLITS], rec_val[DRC_MAX_SPLITS];   // (rec_n)
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
    DRC_H_OUTI_RUN,    // val OUTIs (OUTD: DRC_H_OUTD_RUN) left by the run
    DRC_H_OUTD_RUN,    // routine (not VRAM): z80 registers in and out
    DRC_H_WRITE_MAP,   // write to a mapper register that changes neither
                       // slot 0 nor the slot of the block: it may go on
    DRC_H_SYNC,        // write the deferred tile writes now, stop deferring
    DRC_H_PSG,         // the queue of PSG writes is full: write them
    DRC_H_VWAIT,       // a loop polling the V counter (IN A,(7Eh) / CP n /
                       // JR cc back; arg: n, cc << 8): the iterations that
                       // change nothing, at once (drc_rt.c, vwait)
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
    uint16_t ram_blocks;   // translate code in work RAM too (runtime)
    uint16_t cram_off;     // routine for a byte to CRAM (val; 0: the helper)
    uint16_t cls_off;      // routine for a VRAM byte outside tile data (0: inline)
    uint8_t pad_in;        // IN from 00h and C0h-FFh: drc_state_t keys_in (0: helper)
    uint16_t vcnt_off;     // routine for IN A,(7Eh) (drc_emit_vcnt_stub; 0: helper)
    uint16_t map1_off;     // routines for a bank change of slot 1 (FFFE) and
    uint16_t map2_off;     // slot 2 (FFFF), [val] the bank (0: the helper)
    uint8_t psg_queue;     // OUT (n),A to the PSG: queued in drc_state_t
    uint8_t no_trace;      // diagnostics: do not follow JP nn / JR e in a block
    uint8_t no_inline;     // diagnostics: do not inline CALLed routines
    uint8_t no_fast;       // diagnostics: no rep movsb runs, all flags built
                           // by rotates and shifts (patches 31-32 off)
    uint16_t ldi_off;      // routine for runs of LDI (drc_emit_ldi_stub),
                           // LDD's at ldi_off + DRC_LDD_DELTA (0: interpreted)
    // a block already translated starts at pc (in pc's slot, its current
    // bank): a long block stops there and links to it (NULL: never)
    uint8_t (*known_start)(uint16_t pc);
    // routine for OUTI / OUTI / DEC L x 4 repeated (drc_emit_pairs_stub;
    // 0: each instruction as usual)
    uint16_t pairs_off;
    // routine for two OUTIs that write one name table entry
    // (drc_emit_ntw_stub; 0: none), and for OUTI / OUT (BEh),A
    uint16_t ntw_off;
    uint16_t ntwa_off;
    // the OUTI routine's entry for one OUTI alone (0: none)
    uint16_t outi1_off;
    // routine for OUTI x 3 / OUT (BEh),A: a tile row of three planes and
    // a fourth from A (drc_emit_trow_stub; 0: none)
    uint16_t trow_off;
} drc_env_t;

#define DRC_OUTD_DELTA 0xD30   // (after the other routines: OUTI's has the room up to LDI's)
#define DRC_LDD_DELTA 0xC0

typedef struct {
    uint16_t length;       // bytes of x86 code written
    uint16_t z80_instr;    // Z80 instructions translated
    uint16_t z80_end;      // address after the last translated instruction
} drc_result_t;

/// Translate the block at Z80 address pc. The code is written to out,
/// which will be placed at code-segment offset out_off. Returns false if
/// the first instruction can not be translated (the caller interprets it).
/// Write the routine for runs of OUTI (dec = 0) or OUTD; returns its length
uint16_t drc_emit_cls_stub(const drc_env_t *env, uint8_t *out, uint16_t out_off);
/// (*one, if not null: the offset of the entry for one OUTI alone)
uint16_t drc_emit_run_stub(const drc_env_t *env, uint8_t *out, uint16_t out_off, uint8_t dec, uint16_t *one);

/// Write the routine for runs of LDI (dec = 0) or LDD; returns its length
uint16_t drc_emit_ldi_stub(const drc_env_t *env, uint8_t *out, uint16_t out_off, uint8_t dec);

/// Write the routine for OUT (BFh),A; returns its length
uint16_t drc_emit_ctl_stub(const drc_env_t *env, uint8_t *out, uint16_t out_off);
uint16_t drc_emit_vcnt_stub(const drc_env_t *env, uint8_t *out, uint16_t out_off);

/// Write the routine for OUTI / OUTI / DEC L x 4 repeated (entry at +
/// DRC_PAIRS_ENTRY); returns its length
#define DRC_PAIRS_ENTRY 3
uint16_t drc_emit_pairs_stub(const drc_env_t *env, uint8_t *out, uint16_t out_off);
/// the routine for OUTI / OUTI writing a whole name table entry; returns
/// its length
/// (entered at out_off + DRC_NTW_ENTRY; *entry_a: the offset of the entry
/// for OUTI / OUT (BEh),A)
#define DRC_NTW_ENTRY 2
uint16_t drc_emit_ntw_stub(const drc_env_t *env, uint8_t *out, uint16_t out_off, uint16_t *entry_a);
/// the routine for OUTI x 3 / OUT (BEh),A (entered at out_off +
/// DRC_TROW_ENTRY); returns its length
#define DRC_TROW_ENTRY 2
uint16_t drc_emit_trow_stub(const drc_env_t *env, uint8_t *out, uint16_t out_off);

int drc_translate(const drc_env_t *env, uint16_t pc, uint8_t *out,
                  uint16_t out_off, uint16_t out_max, drc_result_t *res);

#endif
