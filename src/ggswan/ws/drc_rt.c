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
#include <stdio.h>
#include <stdlib.h>
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
#define STUB_DYN    0x0040
#define STUB_NT     0x0140
#define CACHE_START 0x0240
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
    ws_ram_window_restore();   // back to Game Gear slot 2 (main.c)
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

/*
 * DYN: the exit of a block whose next address is only known at run time
 * (RET, JP (HL)...): z80.pc is set, the cycles subtracted. Looks the block
 * up in the table (the same hash and 4-way set as drc_z80_run) and jumps
 * to it if it is in this code bank; otherwise exits to the runtime.
 */
static uint16_t put_dyn(uint8_t *stub, uint8_t bank_index) {
    uint8_t *p = stub + STUB_DYN;
    uint16_t a;
#define PB(x) (*p++ = (x))
#define PW(x) (a = (x), *p++ = a, *p++ = a >> 8)
#define REL8(target) ((uint8_t) ((target) - (p + 1)))
#define PREL(target) do { uint8_t r_ = REL8(target); PB(r_); } while (0)
    uint16_t cyc = env.state_addr + offsetof(drc_state_t, cycles);
    uint16_t pc = env.z80_addr + offsetof(z80_t, pc);
    uint16_t keys = env.state_addr + offsetof(drc_state_t, keys);
    uint16_t tseg = env.state_addr + offsetof(drc_state_t, table_seg);
    uint16_t nolink = env.state_addr + offsetof(drc_state_t, nolink);
    uint16_t link = env.state_addr + offsetof(drc_state_t, link);
    // (the exits first, so that the short jumps to them go backwards)
    uint8_t *miss = p;
    PB(0x5B); PB(0x58);                              // MISS: pop bx; pop ax
    uint8_t *exit0 = p;
    PB(0xC7); PB(0x06); PW(link); PW(0);             // EXIT0: link = 0
    PB(0xE9); PW(STUB_EXIT - ((uint16_t) (p - stub) + 2));   // jmp EXIT
    uint16_t entry = (uint16_t) (p - stub);
    PB(0x83); PB(0x3E); PW(cyc); PB(0);              // cmp word [cycles], 0
    PB(0x7E); PREL(exit0);                       // jle EXIT0
    PB(0x80); PB(0x3E); PW(nolink); PB(0);           // cmp byte [nolink], 0
    PB(0x75); PREL(exit0);                       // jne EXIT0
    PB(0x50); PB(0x53);                              // push ax; push bx
    PB(0xA1); PW(pc);                                // mov ax, [pc]
    PB(0x3D); PW(0xC000);                            // cmp ax, C000h
    PB(0x73); PREL(miss);                        // jae MISS
    PB(0x3D); PW(0x8000);                            // cmp ax, 8000h
    PB(0x72); PB(0x07);                              // jb +7
    PB(0xF6); PB(0x06); PW(keys); PB(0x08);          // test byte [keys], 8
    PB(0x75); PREL(miss);                        // jnz MISS (cartridge RAM)
    PB(0x89); PB(0xC6);                              // mov si, ax
    PB(0xC1); PB(0xC6); PB(0x02);                    // rol si, 2
    PB(0x83); PB(0xE6); PB(0x03);                    // and si, 3
    PB(0x8A); PB(0x9C); PW(keys + 1);                // mov bl, [si + keys + 1]
    PB(0x89); PB(0xDF);                              // mov di, bx
    PB(0x81); PB(0xE7); PW(0x00FF);                  // and di, 00FFh
    PB(0xC1); PB(0xE7); PB(0x05);                    // shl di, 5
    PB(0x31); PB(0xC7);                              // xor di, ax
    PB(0x89); PB(0xC6);                              // mov si, ax
    PB(0xC1); PB(0xEE); PB(0x09);                    // shr si, 9
    PB(0x31); PB(0xF7);                              // xor di, si
    PB(0x81); PB(0xE7); PW((HASH_SIZE - 1) & ~3);    // and di, set mask
    PB(0x8E); PB(0x06); PW(tseg);                    // mov es, [table_seg]
    for (uint8_t way = 0; way < 4; way++) {
        uint8_t *next[4];
        PB(0x89); PB(0xFE);                          // mov si, di
        PB(0xD1); PB(0xE6);                          // shl si, 1
        PB(0x26); PB(0x3B); PB(0x04);                // cmp ax, es:[si]        (T_PC)
        PB(0x75); next[0] = p++;
        PB(0x26); PB(0x3A); PB(0x9D); PW(T_BANK);    // cmp bl, es:[di + T_BANK]
        PB(0x75); next[1] = p++;
        PB(0x26); PB(0x80); PB(0xBD); PW(T_CODE); PB(bank_index);   // cmp byte es:[di + T_CODE], bank
        PB(0x75); next[2] = p++;
        PB(0x26); PB(0x8B); PB(0xB4); PW(T_OFF);     // mov si, es:[si + T_OFF]
        PB(0x83); PB(0xFE); PB(0x01);                // cmp si, 1
        PB(0x76); next[3] = p++;                     // jbe (empty, not translated)
        PB(0x5B); PB(0x58);                          // pop bx; pop ax
        PB(0xFF); PB(0xE6);                          // jmp si
        for (uint8_t k = 0; k < 4; k++) *next[k] = (uint8_t) (p - next[k] - 1);
        PB(0x47);                                    // inc di
    }
    PB(0xE9); PW((uint16_t) (miss - stub) - ((uint16_t) (p - stub) + 2));   // jmp MISS
#undef PB
#undef PW
#undef REL8
#undef PREL
#ifdef GGSWAN_HOST
    if (p > stub + STUB_NT) { printf("DYN stub too long\n"); exit(1); }
#endif
    return entry;
}

/*
 * NT: after a name table byte was written (SI = its VRAM address), puts the
 * converted entry into the WS screen map, like map_put() in main.c: Game
 * Gear row r appears at WS rows (v & 31) for v = r, r + 28, r + 56 within
 * map_top .. map_top + 31. Keeps every register except the x86 flags.
 */
static uint16_t put_nt(uint8_t *stub, uint16_t at) {
    uint8_t *p = stub + at;
    uint16_t a;
#define PB(x) (*p++ = (x))
#define PW(x) (a = (x), *p++ = a, *p++ = a >> 8)
    uint16_t map1 = ws_map1_iram();
    uint16_t nt = env.state_addr + offsetof(drc_state_t, nt_base);
    uint16_t top = env.state_addr + offsetof(drc_state_t, map_top);
    PB(0x50); PB(0x53); PB(0x51); PB(0x52); PB(0x57);   // push ax bx cx dx di
    PB(0x89); PB(0xF3);                                  // mov bx, si
    PB(0x81); PB(0xE3); PW(0x3FFE);                      // and bx, 3FFEh
    PB(0x8B); PB(0x87); PW(env.vram_addr);               // mov ax, [bx + vram]
    PB(0x89); PB(0xC2);                                  // mov dx, ax
    PB(0x81); PB(0xE2); PW(0x01FF);                      // and dx, 1FFh
    PB(0xA9); PW(0x0800); PB(0x74); PB(0x04); PB(0x81); PB(0xCA); PW(0x0200);   // palette
    PB(0xA9); PW(0x0200); PB(0x74); PB(0x04); PB(0x81); PB(0xCA); PW(0x4000);   // h flip
    PB(0xA9); PW(0x0400); PB(0x74); PB(0x04); PB(0x81); PB(0xCA); PW(0x8000);   // v flip
    PB(0x89); PB(0xF0);                                  // mov ax, si
    PB(0x2B); PB(0x06); PW(nt);                          // sub ax, [nt_base]
    PB(0x89); PB(0xC3);                                  // mov bx, ax
    PB(0xD1); PB(0xEB);                                  // shr bx, 1
    PB(0x83); PB(0xE3); PB(0x1F);                        // and bx, 31 (column)
    PB(0xC1); PB(0xE8); PB(0x06);                        // shr ax, 6 (row)
    PB(0x8A); PB(0x0E); PW(top);                         // mov cl, [map_top]
    PB(0x30); PB(0xED);                                  // xor ch, ch
    for (uint8_t i = 0; i < 3; i++) {
        uint8_t *s1, *s2;
        PB(0x39); PB(0xC8);                              // cmp ax, cx
        PB(0x72); s1 = p++;                              // jb skip
        PB(0x89); PB(0xCF);                              // mov di, cx
        PB(0x83); PB(0xC7); PB(0x20);                    // add di, 32
        PB(0x39); PB(0xF8);                              // cmp ax, di
        PB(0x73); s2 = p++;                              // jae skip
        PB(0x89); PB(0xC7);                              // mov di, ax
        PB(0x83); PB(0xE7); PB(0x1F);                    // and di, 31
        PB(0xC1); PB(0xE7); PB(0x05);                    // shl di, 5
        PB(0x01); PB(0xDF);                              // add di, bx
        PB(0xD1); PB(0xE7);                              // shl di, 1
        PB(0x89); PB(0x95); PW(map1);                    // mov [di + map1], dx
        *s1 = (uint8_t) (p - s1 - 1);
        *s2 = (uint8_t) (p - s2 - 1);
        PB(0x83); PB(0xC0); PB(28);                      // add ax, 28
    }
    PB(0x5F); PB(0x5A); PB(0x59); PB(0x5B); PB(0x58);   // pop di dx cx bx ax
    PB(0xC3);                                            // ret
#undef PB
#undef PW
    return (uint16_t) (p - stub);
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
    env.vdp_addr = HOST_VDP_ADDR;
#else
    env.z80_addr = (uint16_t) &z80;
    env.state_addr = (uint16_t) &drc_st;
    env.vdp_addr = (uint16_t) &gg.vdp;
#endif
    env.vram_addr = 0x4000;   // Game Gear VRAM = WS tiles (main.c)
    env.exit_off = STUB_EXIT;
    env.helper_off = STUB_HELPER;
    drc_st.table_seg = DRC_TABLE_SEG;
    env.ram_delta = 0x8000 - 0xC000;   // Game Gear RAM at IRAM 8000h

    for (cache_bank = 0; cache_bank < DRC_CODE_BANKS; cache_bank++) {
        uint8_t stub[CACHE_START];
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
        env.dyn_off = put_dyn(stub, cache_bank);   // (the same in every bank)
        env.nt_stub_off = STUB_NT;
        uint16_t end = put_nt(stub, STUB_NT);
#ifdef GGSWAN_HOST
        if (end > CACHE_START) { printf("stubs too long\n"); exit(1); }
#else
        (void) end;
#endif
        code_write(0, stub, sizeof(stub));
    }
    drc_st.segtab[6] = (uint16_t) (0x0800 - 0x0C00);
    drc_st.segtab[7] = (uint16_t) (0x0800 - 0x0E00);
    flush();
}

static inline uint8_t bank_key(uint16_t pc) {
    return drc_st.keys[1 + (pc >> 14)];
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
    z80_step();
    drc_stat_interp++;
}

// While a block runs, z80.cycles still holds the cycles left when it
// started; the block counts down drc_st.cycles from block_start.
static int16_t block_start;
static uint8_t in_block;

int32_t drc_cycles_left(void) {
    return in_block ? z80.cycles - (block_start - drc_st.cycles) : z80.cycles;
}

// The helper routine's C side: the registers are in z80
static void drc_helper_op(void);
static int16_t remap_saved;   // cycles left when the mapping changed
static uint8_t remapped;
uint8_t drc_flush_request;
uint8_t drc_no_links;

uint32_t drc_stat_helper[5], drc_stat_dispatch, drc_stat_nolink;
static void drc_helper(void) {
    drc_stat_helper[drc_st.op < 5 ? drc_st.op : 0]++;
    uint8_t m0 = gg.mapper[0], m1 = gg.mapper[1], m2 = gg.mapper[2], m3 = gg.mapper[3];
    drc_helper_op();
    if (!remapped && (z80_break || m0 != gg.mapper[0] || m1 != gg.mapper[1]
                      || m2 != gg.mapper[2] || m3 != gg.mapper[3])) {
        // the mapping changed: the rest of the block must not follow links
        // to blocks translated for the old mapping (cycles <= 0 stops them);
        // or the timing must be planned again (z80_break): stop too
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
        // the interpreter counts down the real cycles left in z80.cycles
        int32_t block_left = z80.cycles;
        int32_t before = drc_cycles_left();
        z80.cycles = before;
        in_block = 0;
        uint8_t irq = z80.irq;
        z80.irq = 0;
        uint8_t n = drc_st.val ? drc_st.val : 1;
        z80.pc = drc_st.arg;
#ifdef GGSWAN_HOST
        host_step(drc_st.arg);
#endif
        while (n--) {
            uint16_t p = z80.pc;
            do { z80_step(); } while (z80.pc == p);   // (repeating ones)
        }
        z80.irq = irq;
        drc_st.cycles -= (int16_t) (before - z80.cycles);
        z80.cycles = block_left;
        in_block = 1;
        break;
    }
    }
}

/// Check that every PSRAM bank used is the one seen through the linear
/// area: returns 0, or 1 + the index of the first bad bank.
uint8_t drc_rt_test_banks(void) {
    for (uint8_t i = 0; i <= DRC_CODE_BANKS + 1; i++) {
        uint8_t pattern[2] = { 0x5A, i };
        psram_write(i, 0xFFF0, pattern, 2);
        const uint8_t __far *p = MK_FP(DRC_CODE_SEG + ((uint16_t) i << 12), 0xFFF0);
        if (p[0] != 0x5A || p[1] != i)
            return i + 1;
        if (i < DRC_CODE_BANKS && *((const uint8_t __far*) MK_FP(DRC_CODE_SEG + ((uint16_t) i << 12), 0)) != 0xA3)
            return i + 1;
    }
    return 0;
}

/* === Self-check (diagnostics): every block is also run by the interpreter === */

uint8_t drc_check;
static z80_t check_z;

#define CHECK_SEG (DRC_TABLE_SEG + 0x1000)   // PSRAM bank code_bank + DRC_CODE_BANKS + 1

static uint16_t ram_sum(void) {
    const uint8_t *p = (const uint8_t*) IRAM(0x8000);
    uint16_t s = 0;
    for (uint16_t i = 0; i < 0x2000; i++)
        s = (uint16_t) ((s << 1) | (s >> 15)) + p[i];
    return s;
}

static void check_begin(void) {
    check_z = z80;
    psram_write(DRC_CODE_BANKS + 1, 0, (const uint8_t*) IRAM(0x8000), 0x2000);
}

// after a block: run the interpreter from the saved state and compare
static void check_end(uint16_t pc, int16_t used) {
    z80_t d = z80;
    uint16_t sum_d = ram_sum();
    int32_t left = z80.cycles;
    // back to the state before the block
    z80 = check_z;
    const uint8_t __far *src = MK_FP(CHECK_SEG, 0);
    uint8_t *dst = (uint8_t*) IRAM(0x8000);
    for (uint16_t i = 0; i < 0x2000; i++)
        dst[i] = src[i];
    uint8_t irq = z80.irq;
    z80.irq = 0;
    int16_t c = 0;
    uint16_t steps = 0;
    while (1) {
        uint8_t f0 = z80.f, op0 = z80_read(z80.pc);
        z80.cycles = 0;
        z80_step();
        c += (int16_t) -z80.cycles;
        steps++;
        // (the recompiler leaves the undocumented bits 3 and 5 of F at 0)
        if (z80.f != f0 && op0 != 0xF1 && op0 != 0x08)
            z80.f &= 0xD7;
        if ((z80.pc == d.pc && c >= used) || c > used + 100 || steps > 2000)
            break;
    }
    z80.irq = irq;
    uint16_t sum_r = ram_sum();
    if (z80.pc != d.pc || c != used || z80.a != d.a || (z80.f & 0xD7) != (d.f & 0xD7)
        || z80.b != d.b || z80.c != d.c || z80.d != d.d || z80.e != d.e
        || z80.h != d.h || z80.l != d.l || z80.sp != d.sp
        || z80.ixl != d.ixl || z80.ixh != d.ixh || z80.iyl != d.iyl || z80.iyh != d.iyh
        || sum_r != sum_d)
        drc_check_failed(pc, &check_z, &d, &z80, used, c, sum_d, sum_r);
    z80.cycles = left;
}

int32_t drc_z80_run(int32_t cycles) {
    z80.cycles += cycles;
    drc_st.nolink = drc_no_links;
    while (z80.cycles > 0 && !z80_break) {
        uint16_t pc = z80.pc;
        if (drc_flush_request) { drc_flush_request = 0; flush(); }
        if (z80.halted) {
            pending_link = 0;
            if (z80.irq && z80.iff1) { interp_one(); continue; }
            z80.cycles = 0;
            break;
        }
        if ((z80.irq && z80.iff1) || z80.ei_pending
            || pc >= 0xC000 || (pc >= 0x8000 && (drc_st.keys[0] & 0x08))) {
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
        if (pending_link && pending_seg == drc_entry[1] && !drc_no_links)
            link_patch(pending_seg, pending_link, off);
        pending_link = 0;
        int16_t start = z80.cycles > 30000 ? 30000 : (int16_t) z80.cycles;
        drc_st.cycles = start;
        block_start = start;
        uint32_t io_before = drc_stat_helper[1] + drc_stat_helper[2] + drc_stat_helper[3] + drc_stat_helper[4];
        if (drc_check)
            check_begin();
#ifdef GGSWAN_HOST
        host_trace(pc, z80.b, z80.h << 8 | z80.l);
#endif
        in_block = 1;
        drc_enter(off);
        while (drc_st.reason) {
            drc_helper();
            drc_enter(drc_st.resume);
        }
        in_block = 0;
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
        // (blocks that used helpers are not checked: running their I/O
        // twice would not give the same results)
        if (drc_check && io_before == drc_stat_helper[1] + drc_stat_helper[2] + drc_stat_helper[3] + drc_stat_helper[4])
            check_end(pc, start - drc_st.cycles);
        z80.cycles -= start - drc_st.cycles;
    }
    return z80.cycles;
}
