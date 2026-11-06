/**
 * ggswan - Game Gear emulator for the WonderSwan (nileswan)
 * Recompiler runtime: block cache, dispatcher, helper routine.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Translated code lives in DRC_CODE_BANKS PSRAM banks of 64 KiB, executed
 * through the linear ROM area at segments DRC_CODE_SEG, + 1000h... and
 * written through the RAM window (flash enabled). Each starts with two stubs:
 *   EXIT   (offset 0):    store AX CX DX BX BP into z80, reason = 0, RETF
 *   HELPER (offset 0x20): pop the return address into drc_st.resume, store
 *                         the registers, reason = 1, RETF
 * drc_enter() (drc_asm.s) loads the registers from z80 and far-calls a
 * block; the stubs return to it.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef GGSWAN_HOST
#include "wsshim.h"
#else
#include <ws.h>
#define IRAM(a) ((uint8_t*) (a))
#endif
#include "gg.h"
#include "z80.h"
#include "../drc/drc.h"
#include "drc_rt.h"

drc_state_t drc_st;
uint16_t drc_entry[2];   // far pointer used by drc_enter: offset, segment

#define STUB_EXIT   0x0000
#define STUB_HELPER 0x0020
#define CACHE_START 0x0040
// translation buffer in free IRAM (between the font tiles and the mask tile)
#define BUF_ADDR    0xA600
#define BUF_SIZE    0x1900

/*
 * Block table: hash of (Z80 address, ROM bank) -> code offset, 8192
 * entries in the PSRAM bank after the code cache, read through the linear
 * area (segment DRC_TABLE_SEG) and written through the RAM window:
 *   0000h: Z80 address (words)   4000h: code offset (words, 0 = empty)
 *   8000h: ROM bank (bytes)     A000h: code bank (bytes)
 */
#define HASH_SIZE 8192
#define T_PC   0x0000
#define T_OFF  0x4000
#define T_BANK 0x8000
#define T_CODE 0xA000
#define DRC_NOT_TRANSLATED 1   // (code offsets are even)

static uint16_t code_bank;   // first PSRAM bank of the code cache
static uint8_t cache_bank;   // code bank being filled (0..DRC_CODE_BANKS-1)
static uint16_t cache_top;
static uint8_t victim;
static uint16_t pending_link, pending_seg;   // exit to link to the next block
static drc_env_t env;
uint32_t drc_stat_blocks, drc_stat_flushes, drc_stat_interp;

// Write to PSRAM bank code_bank + which through the RAM window
static void psram_write(uint8_t which, uint16_t off, const uint8_t *src, uint16_t len) {
    uint8_t flash = inportb(WS_CART_BANK_FLASH_PORT);
    uint16_t bank = inportw(WS_CART_EXTBANK_RAM_PORT);
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, code_bank + which);
    uint8_t __far *dst = MK_FP(0x1000, off);
    if (src) {
        for (uint16_t i = 0; i < len; i++)
            dst[i] = src[i];
    } else {
        for (uint16_t i = 0; i < len; i++)
            dst[i] = 0;
    }
    outportw(WS_CART_EXTBANK_RAM_PORT, bank);
    outportb(WS_CART_BANK_FLASH_PORT, flash);
}

static void code_write(uint16_t off, const uint8_t *src, uint16_t len) {
    psram_write(cache_bank, off, src, len);
#ifdef GGSWAN_HOST
    host_code_changed();
#endif
}

// point the JMP at link (code bank of segment seg) at offset target
static void link_patch(uint16_t seg, uint16_t link, uint16_t target) {
    uint16_t rel = target - (link + 3);
    psram_write((seg - DRC_CODE_SEG) >> 12, link + 1, (const uint8_t*) &rel, 2);
#ifdef GGSWAN_HOST
    host_code_changed();
#endif
}

static uint8_t *put_store_regs(uint8_t *p) {
    uint16_t z = env.z80_addr;
    *p++ = 0xA3; *p++ = z; *p++ = z >> 8;                                   // mov [a], ax
    *p++ = 0x89; *p++ = 0x0E; *p++ = (z + 2); *p++ = (z + 2) >> 8;          // mov [c], cx
    *p++ = 0x89; *p++ = 0x16; *p++ = (z + 4); *p++ = (z + 4) >> 8;          // mov [e], dx
    *p++ = 0x89; *p++ = 0x1E; *p++ = (z + 6); *p++ = (z + 6) >> 8;          // mov [l], bx
    uint16_t sp = z + offsetof(z80_t, sp);
    *p++ = 0x89; *p++ = 0x2E; *p++ = sp; *p++ = sp >> 8;                    // mov [sp], bp
    return p;
}

static void flush(void) {
    pending_link = 0;
    psram_write(DRC_CODE_BANKS, T_OFF, NULL, HASH_SIZE * 2);
    cache_bank = 0;
    cache_top = CACHE_START;
    drc_stat_flushes++;
}

void drc_rt_init(uint16_t bank) {
    code_bank = bank;
#ifdef GGSWAN_HOST
    env.z80_addr = HOST_Z80_ADDR;
    env.state_addr = HOST_ST_ADDR;
#else
    env.z80_addr = (uint16_t) &z80;
    env.state_addr = (uint16_t) &drc_st;
#endif
    env.exit_off = STUB_EXIT;
    env.helper_off = STUB_HELPER;
    env.ram_delta = 0x8000 - 0xC000;   // Game Gear RAM at IRAM 8000h

    uint8_t stub[0x40];
    memset(stub, 0xCC, sizeof(stub));
    uint16_t reason = env.state_addr + offsetof(drc_state_t, reason);
    uint16_t resume = env.state_addr + offsetof(drc_state_t, resume);
    uint8_t *p = put_store_regs(stub + STUB_EXIT);
    *p++ = 0xC6; *p++ = 0x06; *p++ = reason; *p++ = reason >> 8; *p++ = 0;   // reason = 0
    *p++ = 0xCB;                                                             // retf
    p = stub + STUB_HELPER;
    *p++ = 0x8F; *p++ = 0x06; *p++ = resume; *p++ = resume >> 8;             // pop [resume]
    p = put_store_regs(p);
    *p++ = 0xC6; *p++ = 0x06; *p++ = reason; *p++ = reason >> 8; *p++ = 1;   // reason = 1
    *p++ = 0xCB;
    for (cache_bank = 0; cache_bank < DRC_CODE_BANKS; cache_bank++)
        code_write(0, stub, sizeof(stub));
    drc_st.segtab[6] = (uint16_t) ((0x8000 - 0xC000) >> 4);   // C000-DFFF -> IRAM 8000h
    drc_st.segtab[7] = (uint16_t) ((0x8000 - 0xE000) >> 4);   // E000-FFFF (mirror)
    flush();
}

static inline uint8_t bank_key(uint16_t pc) {
    return gg.mapper[1 + (pc >> 14)];
}

static inline uint16_t hash(uint16_t pc, uint8_t bank) {
    return (pc ^ (pc >> 9) ^ ((uint16_t) bank << 5)) & (HASH_SIZE - 1);
}

static uint16_t translate(uint16_t pc, uint8_t bank, uint16_t h) {
    if (cache_top > 0xFFF0 - BUF_SIZE) {
        // this bank is full: the next one, or start again
        if (++cache_bank < DRC_CODE_BANKS)
            cache_top = CACHE_START;
        else
            flush();
    }
    drc_result_t r;
    uint8_t *buf = (uint8_t*) IRAM(BUF_ADDR);
    if (!drc_translate(&env, pc, buf, cache_top, BUF_SIZE, &r)) {
        // remember that this one is left to the interpreter
        uint16_t marker = DRC_NOT_TRANSLATED;
        psram_write(DRC_CODE_BANKS, T_PC + h * 2, (const uint8_t*) &pc, 2);
        psram_write(DRC_CODE_BANKS, T_OFF + h * 2, (const uint8_t*) &marker, 2);
        psram_write(DRC_CODE_BANKS, T_BANK + h, &bank, 1);
        return DRC_NOT_TRANSLATED;
    }
    uint16_t off = cache_top;
    code_write(off, buf, r.length);
    cache_top = (cache_top + r.length + 1) & ~1;
    psram_write(DRC_CODE_BANKS, T_PC + h * 2, (const uint8_t*) &pc, 2);
    psram_write(DRC_CODE_BANKS, T_OFF + h * 2, (const uint8_t*) &off, 2);
    psram_write(DRC_CODE_BANKS, T_BANK + h, &bank, 1);
    psram_write(DRC_CODE_BANKS, T_CODE + h, &cache_bank, 1);
#ifdef GGSWAN_HOST
    {
        const uint16_t __far *tw = MK_FP(DRC_TABLE_SEG, 0);
        if (tw[(T_OFF / 2) + h] != off || tw[(T_PC / 2) + h] != pc)
            host_trace(0xBAD0, h >> 8, off);
    }
#endif
    drc_stat_blocks++;
#ifdef GGSWAN_HOST
    host_translated(pc, r.length, r.z80_instr);
#endif
    return off;
}

// one instruction (or an interrupt) with the interpreter
static void interp_one(void) {
    int32_t left = z80.cycles;
    z80.cycles = 0;
    int32_t r = z80_run(1);
    z80.cycles = left - (1 - r);
    drc_stat_interp++;
}

// The helper routine's C side: the registers are in z80
static void drc_helper_op(void);
static int16_t remap_saved;   // cycles left when the mapping changed
static uint8_t remapped;
uint8_t drc_flush_request;

uint32_t drc_stat_helper[5], drc_stat_dispatch, drc_stat_nolink;
static void drc_helper(void) {
    drc_stat_helper[drc_st.op < 5 ? drc_st.op : 0]++;
    uint8_t m0 = gg.mapper[0], m1 = gg.mapper[1], m2 = gg.mapper[2], m3 = gg.mapper[3];
    drc_helper_op();
    if (!remapped && (m0 != gg.mapper[0] || m1 != gg.mapper[1]
                      || m2 != gg.mapper[2] || m3 != gg.mapper[3])) {
        // the mapping changed: the rest of the block must not follow links
        // to blocks translated for the old mapping (cycles <= 0 stops them)
        remapped = 1;
        remap_saved = drc_st.cycles;
        drc_st.cycles = -30000;
    }
}

static void drc_helper_op(void) {
    switch (drc_st.op) {
    case DRC_H_WRITE: z80_write(drc_st.arg, drc_st.val); break;
    case DRC_H_OUT: z80_out(drc_st.arg, drc_st.val); break;
    case DRC_H_IN: drc_st.val = z80_in(drc_st.arg); break;
    case DRC_H_STEP: {
        int32_t left = z80.cycles;
        uint8_t irq = z80.irq;
        z80.irq = 0;
        uint8_t n = drc_st.val ? drc_st.val : 1;
        z80.pc = drc_st.arg;
        while (n--) {
            uint16_t p = z80.pc;
            do { drc_st.cycles -= (z80.cycles = 0, 1 - z80_run(1)); } while (z80.pc == p);   // (repeating ones)
        }
        z80.irq = irq;
        z80.cycles = left;
        break;
    }
    }
}

int32_t drc_z80_run(int32_t cycles) {
    z80.cycles += cycles;
    while (z80.cycles > 0) {
        uint16_t pc = z80.pc;
        if (drc_flush_request) { drc_flush_request = 0; flush(); }
        if (z80.halted) {
            pending_link = 0;
            if (z80.irq && z80.iff1) { interp_one(); continue; }
            z80.cycles = 0;
            break;
        }
        if ((z80.irq && z80.iff1) || z80.ei_pending
            || pc >= 0xC000 || (pc >= 0x8000 && (gg.mapper[0] & 0x08))) {
            // interrupts, and code in RAM (it may change): interpreter
            pending_link = 0;
            interp_one();
            continue;
        }
        uint8_t bank = bank_key(pc);
        // 4-way: the four slots of the set of h
        uint16_t h0 = hash(pc, bank) & ~3;
        const uint16_t __far *tw = MK_FP(DRC_TABLE_SEG, 0);
        const uint8_t __far *tb = (const uint8_t __far*) tw;
        uint16_t h = h0, off = 0;
        uint8_t i;
        for (i = 0; i < 4; i++, h++) {
            off = tw[(T_OFF / 2) + h];
            if (off && tw[(T_PC / 2) + h] == pc && tb[T_BANK + h] == bank)
                break;
        }
        if (i == 4) {
#ifdef GGSWAN_HOST
            host_miss(1);
#endif
            // a free slot of the set, or replace one in turn
            for (h = h0; h < h0 + 4 && tw[(T_OFF / 2) + h]; h++) {}
            if (h == h0 + 4)
                h = h0 + (victim++ & 3);
            off = translate(pc, bank, h);
        }
        if (off == DRC_NOT_TRANSLATED) { pending_link = 0; interp_one(); continue; }
        drc_entry[1] = DRC_CODE_SEG + ((uint16_t) tb[T_CODE + h] << 12);
        // the previous block can jump straight here from now on
        if (pending_link && pending_seg == drc_entry[1])
            link_patch(pending_seg, pending_link, off);
        pending_link = 0;
        int16_t start = z80.cycles > 30000 ? 30000 : (int16_t) z80.cycles;
        drc_st.cycles = start;
#ifdef GGSWAN_HOST
        host_trace(pc, z80.b, z80.h << 8 | z80.l);
#endif
        drc_enter(off);
        while (drc_st.reason) {
            drc_helper();
            drc_enter(drc_st.resume);
        }
        if (remapped) {
            remapped = 0;
            drc_st.cycles = remap_saved + (drc_st.cycles + 30000);
        } else if (drc_st.link) {
            pending_link = drc_st.link;
            pending_seg = drc_entry[1];
        } else {
            drc_stat_nolink++;
        }
        drc_stat_dispatch++;
        z80.cycles -= start - drc_st.cycles;
    }
    return z80.cycles;
}
