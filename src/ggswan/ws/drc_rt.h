/**
 * ggswan - recompiler runtime (see drc_rt.c)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GGSWAN_DRC_RT_H_
#define GGSWAN_DRC_RT_H_

#include <stdint.h>
#include "../drc/drc.h"
#include "z80.h"

/// Segments of the linear ROM area where the code cache is executed
/// (DRC_CODE_BANKS banks of 64 KiB)...
#define DRC_CODE_SEG 0x4000
#define DRC_CODE_BANKS 6
/// ... and the block table (the PSRAM bank after them)
#define DRC_TABLE_SEG (DRC_CODE_SEG + DRC_CODE_BANKS * 0x1000)

extern drc_state_t drc_st;
extern uint16_t drc_entry[2];
extern uint32_t drc_stat_blocks, drc_stat_flushes, drc_stat_interp, drc_stat_irq;
/// last instruction interpreted outside interrupts (RAM, not translatable)
extern uint16_t drc_last_interp_pc;
extern uint32_t drc_stat_helper[5], drc_stat_dispatch, drc_stat_nolink;
/// diagnostics: do not link blocks
extern uint8_t drc_no_links;
/// diagnostics: run every block with the interpreter too, compare
extern uint8_t drc_check;
/// main.c: show a difference found by the self-check (does not return)
void drc_check_failed(uint16_t pc, const z80_t *before, const z80_t *drc,
                      const z80_t *ref, int16_t used, int16_t used_ref,
                      uint16_t sum_drc, uint16_t sum_ref);

/// bank: PSRAM bank behind linear segment DRC_CODE_SEG
void drc_rt_init(uint16_t bank);
/// set when slot 0 changes bank: links into it are no longer valid
extern uint8_t drc_flush_request;
/// Profiler: what the plugin is doing now (sampled by an interrupt)
enum {
    PROF_OTHER = 0,   // main loop, frame timing
    PROF_CODE,        // translated code
    PROF_DISPATCH,    // dispatcher (C)
    PROF_HELPER,      // helpers: ports, memory writes (C)
    PROF_STEP,        // instructions interpreted inside blocks
    PROF_INTERP,      // interpreter (interrupts, RAM code, interpreter mode)
    PROF_TRANSLATE,   // translating blocks
    PROF_VIDEO,       // end of frame: scroll, sprites, colors
    PROF_COUNT
};
extern volatile uint8_t prof_phase;

/// main.c: IRAM address of the WS screen map of the Game Gear background
uint16_t ws_map1_iram(void);
/// main.c: put the RAM window back as Game Gear slot 2 needs it
void ws_ram_window_restore(void);
/// check the PSRAM banks of the recompiler (0: all right)
uint8_t drc_rt_test_banks(void);
/// cycles left in the current run (for gg_cpu_left)
int32_t drc_cycles_left(void);
/// z80_run() equivalent using the recompiler
int32_t drc_z80_run(int32_t cycles);
/// Run translated code at DRC_CODE_SEG:offset (drc_asm.s)
void drc_enter(uint16_t offset);

#ifdef GGSWAN_HOST
#define HOST_Z80_ADDR 0xC000
#define HOST_ST_ADDR  0xC100
#define HOST_VDP_ADDR 0xC200
void host_code_changed(void);
void host_miss(int evict);
void host_step(uint16_t pc);
void host_lookup_fail(uint16_t pc, uint8_t bank, uint16_t h, uint16_t off, uint16_t tpc, uint8_t tbank);
void host_trace(uint16_t pc, uint8_t b, uint16_t hl);
void host_translated(uint16_t pc, uint16_t len, uint16_t n);
#endif

#endif
