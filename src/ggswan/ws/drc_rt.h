/**
 * ggswan - recompiler runtime (see drc_rt.c)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
// smsswan: the WS colors of the Master System's 64 (IRAM, free: 128 bytes;
// filled by main.c video_init, used by the recompiler's CRAM routine)
#define WS_CRAM_TABLE 0xE600
#ifndef GGSWAN_DRC_RT_H_
#define GGSWAN_DRC_RT_H_

#include <stdint.h>
#include "../drc/drc.h"
#include "z80.h"

/// Segments of the linear ROM area where the code cache is executed
/// (DRC_CODE_BANKS banks of 64 KiB)...
#define DRC_CODE_SEG 0x4000u
#define DRC_CODE_BANKS 6
/// ... and the block table (the PSRAM bank after them; unsigned: past
/// 0x7FFF on a 16-bit int)
#define DRC_TABLE_SEG (DRC_CODE_SEG + DRC_CODE_BANKS * 0x1000u)

extern drc_state_t drc_st;
extern uint16_t drc_entry[2];
extern uint32_t drc_stat_blocks, drc_stat_flushes, drc_stat_interp, drc_stat_irq;
/// last instruction interpreted outside interrupts (RAM, not translatable)
extern uint16_t drc_last_interp_pc;
extern uint16_t drc_last_tr_pc;
extern uint32_t drc_stat_helper[5], drc_stat_dispatch, drc_stat_nolink;
/// diagnostics: do not link blocks
extern uint8_t drc_no_links;
/// diagnostics (set before drc_rt_init): none of patches 29-32 (line events
/// in the dispatcher, the fast reentry, rep movsb runs, rotate flags)
extern uint8_t drc_no_fast;
extern uint16_t drc_stat_op[16];   // helper calls by DRC_H_* (diagnostics)
extern uint16_t drc_stat_op_lines[16];   // ...and the WS lines they took
void drc_rt_rom_banks(uint16_t banks);   // after gg_init
/// diagnostics: run every block with the interpreter too, compare
extern uint8_t drc_check;
/// main.c: show a difference found by the self-check (does not return)
void drc_check_failed(uint16_t pc, const z80_t *before, const z80_t *drc,
                      const z80_t *ref, int16_t used, int16_t used_ref,
                      uint16_t sum_drc, uint16_t sum_ref);

/// bank: PSRAM bank behind linear segment DRC_CODE_SEG
void drc_rt_init(uint16_t bank);
/// diagnostics: blocks always back to the runtime (the stubs made again)
void drc_rt_set_no_links(uint8_t on);
/// diagnostics: do not follow JP/JR in blocks, do not inline CALLs
void drc_rt_options(uint8_t no_trace, uint8_t no_inline);
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
    PROF_IDLE,        // waiting: never faster than the Game Gear (not shown)
    PROF_COUNT
};
extern volatile uint8_t prof_phase;

/// main.c: IRAM address of the WS screen map of the Game Gear background
uint16_t ws_map1_iram(void);
uint16_t ws_map2_iram(void);
/// screen 2 entry of a background tile without priority: tile 1023, empty
/// (main.c clears it): the sprites show through
#define WS_PRIO_BLANK 0x21FF

/// main.c: put the RAM window back as Game Gear slot 2 needs it
void ws_ram_window_restore(void);
/// check the PSRAM banks of the recompiler (0: all right)
uint8_t drc_rt_test_banks(void);
/// Diagnostics: the same code timed in IRAM and in PSRAM (see drc_rt.c)
void drc_rt_bench(uint16_t *out);
/// cycles left in the current run (for gg_cpu_left)
int32_t drc_cycles_left(void);
/// drc_enter: after the code returns, a line event handled at once (see
/// drc_rt.c); 1: run again from drc_entry
uint8_t drc_fast_reentry(void);
/// z80_run() equivalent using the recompiler
int32_t drc_z80_run(int32_t cycles);
#ifdef GGSWAN_SF7000
/// sfswan: the PSRAM banks before the code's (main.c has the whole list):
/// the SF-7000's RAM, the screen's tables with the VRAM in the last 16 KiB,
/// the disk. And the two maps of the code translated (drc.c: FLAT): a byte
/// for each 16 bytes of the Z80's memory in the internal RAM, where the
/// other builds have the work RAM (what translated code looks at before a
/// write), and a bit for each byte in the tables' bank (what the C side
/// looks at when that says "maybe")
#define SF_RAM_BANK 0
#define SF_VRAM_BANK 1
#define SF_DISK_BANK 2
#define SF_SYS_BANK 5
#define SF_BANKS 8
#define SF_CMAP_ADDR 0x8000
#define SF_CBITS_OFF 0x4000
// (and the bytes of code a program was seen to write: a bit each in the
// tables' bank, a byte for each 256 in the internal RAM)
#define SF_SMCBITS_OFF 0x6000
#define SF_SMC_PAGE_ADDR 0x9000
#endif

/// Run translated code at DRC_CODE_SEG:offset (drc_asm.s)
void drc_enter(uint16_t offset);
/// a helper called from translated code (stub HCALL)
void drc_helper_direct(void);

#ifdef GGSWAN_HOST
#ifdef GGSWAN_SG1000
// (sgswan: C000h is the translation buffer; the mirrors where the device
// keeps the ROM's first KiB, an array of its own on the PC)
#define HOST_Z80_ADDR 0xEC00
#define HOST_VDP_ADDR 0xEC40
#define HOST_ST_ADDR  0xED00
#define HOST_ST_END   0xEF80   // (drcsys.py's x86 stack is at EFFCh)
#else
#define HOST_Z80_ADDR 0xC000
// (the PC tests' mirrors in IRAM: z80 at C000h, the VDP's registers after
// it, the state from C100h up to the log of deferred VRAM writes, main.c
// LOG_A, at C400h)
#define HOST_VDP_ADDR 0xC040
#define HOST_ST_ADDR  0xC100
#define HOST_ST_END   0xC400
#endif
void host_code_changed(void);
void host_miss(int evict);
void host_step(uint16_t pc);
void host_lookup_fail(uint16_t pc, uint8_t bank, uint16_t h, uint16_t off, uint16_t tpc, uint8_t tbank);
void host_trace(uint16_t pc, uint8_t b, uint16_t hl);
void host_translated(uint16_t pc, uint16_t len, uint16_t n);
#endif

#endif
