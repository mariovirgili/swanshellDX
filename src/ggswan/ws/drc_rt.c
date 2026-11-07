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
#ifdef GGSWAN_HOST
// (the PC tests mirror it at HOST_ST_ADDR, 100h bytes before the VDP)
_Static_assert(sizeof(drc_state_t) <= HOST_VDP_ADDR - HOST_ST_ADDR, "drc_state_t too big");
#endif
uint16_t drc_entry[2];   // far pointer used by drc_enter: offset, segment

#define STUB_EXIT   0x0000
#define STUB_HELPER 0x0020
#define STUB_DYN    0x0040
#define STUB_NT     0x0180
#define STUB_OUTI   0x0240   // (OUTD: + DRC_OUTD_DELTA)
#define STUB_LDI    0x04C0   // (LDD: + DRC_LDD_DELTA)
#define STUB_HCALL  0x05F0   // helpers called without leaving the block
#define STUB_CTL    0x0640   // OUT (BFh),A
#define CACHE_START 0x0700
// translation buffer in free IRAM (between the font tiles and the mask tile)
#define BUF_ADDR    0xA600
/// DYN L0 cache: 512 direct-mapped entries of 8 bytes in free IRAM (in the
/// WS Color 4bpp mode C000-FDFF is free): Z80 pc (FFFF: empty), bank,
/// -, offset, segment of the block
#define L0_ADDR     0xD000
/// block table key of the code in work RAM (banks are below 64)
#define RAM_KEY     0xFE
#define L0_SIZE     0x1000
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
uint32_t drc_stat_blocks, drc_stat_flushes, drc_stat_interp, drc_stat_irq;
uint16_t drc_last_interp_pc;
/// far address of drc_helper_tramp (drc_asm.s fills it in), called by the
/// HCALL stub
uint16_t drc_tramp[2];

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
// point the jump at link (in code segment seg) at target_seg:target; a
// guarded exit (to another slot) also gets the bank it is valid for
static void link_patch(uint16_t seg, uint16_t link, uint16_t target_seg, uint16_t target,
                       uint8_t guard, uint8_t bank) {
    uint8_t j[5];
    if (target_seg == seg) {
        uint16_t rel = target - (link + 3);
        j[0] = 0xE9; j[1] = rel; j[2] = rel >> 8; j[3] = 0x90; j[4] = 0x90;
    } else {
        j[0] = 0xEA; j[1] = target; j[2] = target >> 8; j[3] = target_seg; j[4] = target_seg >> 8;
    }
    psram_write((seg - DRC_CODE_SEG) >> 12, link, j, 5);
    if (guard)
        psram_write((seg - DRC_CODE_SEG) >> 12, link - 3, &bank, 1);
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
    uint16_t far_off = env.state_addr + offsetof(drc_state_t, far_off);
    uint16_t l0_slot = env.state_addr + offsetof(drc_state_t, l0_slot);
    uint8_t *found[4];
    (void) bank_index;
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
    PB(0x72); uint8_t *rom = p++;                    // jb rom
    // work RAM: its blocks check their own bytes (key RAM_KEY)
    PB(0x3D); PW(0xDFC0);                            // cmp ax, DFC0h
    PB(0x73); PREL(miss);                        // jae MISS
    PB(0xB3); PB(RAM_KEY);                           // mov bl, RAM_KEY
    PB(0xEB); uint8_t *have = p++;                   // jmp have
    *rom = (uint8_t) (p - rom - 1);
    PB(0x3D); PW(0x8000);                            // cmp ax, 8000h
    PB(0x72); PB(0x07);                              // jb +7
    PB(0xF6); PB(0x06); PW(keys); PB(0x08);          // test byte [keys], 8
    PB(0x75); PREL(miss);                        // jnz MISS (cartridge RAM)
    PB(0x89); PB(0xC6);                              // mov si, ax
    PB(0xC1); PB(0xC6); PB(0x02);                    // rol si, 2
    PB(0x83); PB(0xE6); PB(0x03);                    // and si, 3
    PB(0x8A); PB(0x9C); PW(keys + 1);                // mov bl, [si + keys + 1]
    *have = (uint8_t) (p - have - 1);
    PB(0x89); PB(0xDF);                              // mov di, bx
    PB(0x81); PB(0xE7); PW(0x00FF);                  // and di, 00FFh
    PB(0xC1); PB(0xE7); PB(0x05);                    // shl di, 5
    PB(0x31); PB(0xC7);                              // xor di, ax
    PB(0x89); PB(0xC6);                              // mov si, ax
    PB(0xC1); PB(0xEE); PB(0x09);                    // shr si, 9
    PB(0x31); PB(0xF7);                              // xor di, si
    PB(0x81); PB(0xE7); PW((HASH_SIZE - 1) & ~3);    // and di, set mask
    // L0 first: entry (di & 7FCh) * 2
    PB(0x89); PB(0xFE);                              // mov si, di
    PB(0x81); PB(0xE6); PW(0x07FC);                  // and si, 7FCh
    PB(0xD1); PB(0xE6);                              // shl si, 1
    PB(0x3B); PB(0x84); PW(L0_ADDR);                 // cmp ax, [si + L0]
    PB(0x75); uint8_t *l0miss1 = p++;                // jne
    PB(0x3A); PB(0x9C); PW(L0_ADDR + 2);             // cmp bl, [si + L0 + 2]
    PB(0x75); uint8_t *l0miss2 = p++;                // jne
    PB(0x5B); PB(0x58);                              // pop bx; pop ax
    PB(0xFF); PB(0xAC); PW(L0_ADDR + 4);             // jmp far [si + L0 + 4]
    *l0miss1 = (uint8_t) (p - l0miss1 - 1);
    *l0miss2 = (uint8_t) (p - l0miss2 - 1);
    PB(0x89); PB(0x36); PW(l0_slot);                 // mov [l0_slot], si
    PB(0x8E); PB(0x06); PW(tseg);                    // mov es, [table_seg]
    for (uint8_t way = 0; way < 4; way++) {
        uint8_t *next[2];
        PB(0x89); PB(0xFE);                          // mov si, di
        PB(0xD1); PB(0xE6);                          // shl si, 1
        PB(0x26); PB(0x3B); PB(0x04);                // cmp ax, es:[si]        (T_PC)
        PB(0x75); next[0] = p++;
        PB(0x26); PB(0x3A); PB(0x9D); PW(T_BANK);    // cmp bl, es:[di + T_BANK]
        PB(0x75); next[1] = p++;
        PB(0x26); PB(0x8B); PB(0xB4); PW(T_OFF);     // mov si, es:[si + T_OFF]
        PB(0x83); PB(0xFE); PB(0x01);                // cmp si, 1
        PB(0x77); found[way] = p++;                  // ja FOUND
        for (uint8_t k = 0; k < 2; k++) *next[k] = (uint8_t) (p - next[k] - 1);
        PB(0x47);                                    // inc di
    }
    PB(0xE9); PW((uint16_t) (miss - stub) - ((uint16_t) (p - stub) + 2));   // jmp MISS
    // FOUND: SI = offset, DI = entry; jump far to its code bank
    for (uint8_t way = 0; way < 4; way++) *found[way] = (uint8_t) (p - found[way] - 1);
    PB(0x89); PB(0x36); PW(far_off);                 // mov [far_off], si
    PB(0x8A); PB(0xFB);                              // mov bh, bl (bank)
    PB(0x26); PB(0x8A); PB(0x9D); PW(T_CODE);        // mov bl, es:[di + T_CODE]
    PB(0x8B); PB(0x3E); PW(l0_slot);                 // mov di, [l0_slot]
    PB(0x89); PB(0x85); PW(L0_ADDR);                 // mov [di + L0], ax (pc)
    PB(0x88); PB(0xBD); PW(L0_ADDR + 2);             // mov [di + L0 + 2], bh (bank)
    PB(0x89); PB(0xB5); PW(L0_ADDR + 4);             // mov [di + L0 + 4], si (offset)
    PB(0x88); PB(0xDF);                              // mov bh, bl
    PB(0xC0); PB(0xE7); PB(0x04);                    // shl bh, 4
    PB(0x80); PB(0xC7); PB(DRC_CODE_SEG >> 8);       // add bh, code segment
    PB(0x30); PB(0xDB);                              // xor bl, bl
    PB(0x89); PB(0x1E); PW(far_off + 2);             // mov [far_seg], bx
    PB(0x89); PB(0x9D); PW(L0_ADDR + 6);             // mov [di + L0 + 6], bx (segment)
    PB(0x5B); PB(0x58);                              // pop bx; pop ax
    PB(0xFF); PB(0x2E); PW(far_off);                 // jmp far [far_off]
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
    uint16_t rows = env.state_addr + offsetof(drc_state_t, nt_rows);
    PB(0x50); PB(0x53); PB(0x52); PB(0x57);             // push ax bx dx di
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
    PB(0x83); PB(0xE3); PB(0x3E);                        // and bx, 3Eh (column * 2)
    PB(0xC1); PB(0xE8); PB(0x05);                        // shr ax, 5
    PB(0x24); PB(0xFE);                                  // and al, FEh (row * 2)
    for (uint8_t i = 0; i < 2; i++) {
        PB(0x89); PB(0xC7);                              // mov di, ax
        PB(0x8B); PB(0xBD); PW(rows + i * 56);           // mov di, [di + rows]
        PB(0x83); PB(0xFF); PB(0xFF);                    // cmp di, -1
        PB(0x74); PB(0x04);                              // je +4 (none)
        PB(0x89); PB(0x91); PW(map1);                    // mov [bx + di + map1], dx
    }
    PB(0x5F); PB(0x5A); PB(0x5B); PB(0x58);             // pop di dx bx ax
    PB(0xC3);                                            // ret
#undef PB
#undef PW
    return (uint16_t) (p - stub);
}

// the block at 0038h (IM 1 interrupts), for drc_fast_reentry: offset,
// segment (0: unknown), block table key
static uint16_t fast38_off, fast38_seg;
static uint8_t fast38_bank;

static void flush(void) {
    pending_link = 0;
    fast38_seg = 0;
    memset(IRAM(L0_ADDR), 0xFF, L0_SIZE);
    psram_write(DRC_CODE_BANKS, T_OFF, NULL, HASH_SIZE * 2);
    cache_bank = 0;
    cache_top = CACHE_START;
    drc_stat_flushes++;
}

// diagnostics: switch off parts of the block building (before the first
// translation)
void drc_rt_options(uint8_t no_trace, uint8_t no_inline) {
    env.no_trace = no_trace;
    env.no_inline = no_inline;
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
    env.no_fast = drc_no_fast;
    // following JP/JR inside a block is off by default: it breaks the
    // scroll and the status bar of Wonder Boy III on the WonderSwan (not
    // on the PC tests, cause not found) and gained under 1%
    env.no_trace = 1;
    drc_st.table_seg = DRC_TABLE_SEG;
    env.ram_delta = 0x8000 - 0xC000;   // Game Gear RAM at IRAM 8000h

    for (cache_bank = 0; cache_bank < DRC_CODE_BANKS; cache_bank++) {
        uint8_t *stub = (uint8_t*) IRAM(BUF_ADDR);   // (translation buffer)
        memset(stub, 0xCC, CACHE_START);
        uint16_t reason = env.state_addr + offsetof(drc_state_t, reason);
        uint16_t resume = env.state_addr + offsetof(drc_state_t, resume);
        uint16_t cur_seg = env.state_addr + offsetof(drc_state_t, cur_seg);
        uint8_t *p = stub + STUB_EXIT;
        *p++ = 0xC6; *p++ = 0x06; *p++ = reason; *p++ = reason >> 8; *p++ = 0;   // reason = 0
        uint8_t *common = p;
        p = put_store_regs(p);
        *p++ = 0x8C; *p++ = 0x0E; *p++ = cur_seg; *p++ = cur_seg >> 8;          // mov [cur_seg], cs
        *p++ = 0xCB;                                                             // retf
#ifdef GGSWAN_HOST
        if (p > stub + STUB_HELPER) { printf("EXIT stub too long\n"); exit(1); }
#endif
        p = stub + STUB_HELPER;
        *p++ = 0x8F; *p++ = 0x06; *p++ = resume; *p++ = resume >> 8;             // pop [resume]
        *p++ = 0xC6; *p++ = 0x06; *p++ = reason; *p++ = reason >> 8; *p++ = 1;   // reason = 1
        *p++ = 0xEB; *p = (uint8_t) (common - (p + 1)); p++;                     // jmp common
        // HCALL: store the registers, call the helper, load them, return
        p = put_store_regs(stub + STUB_HCALL);
#ifdef GGSWAN_HOST
        *p++ = 0xCD; *p++ = 0x80;                                                // int 80h (drcsys.py)
#else
        *p++ = 0xFF; *p++ = 0x1E; *p++ = (uint16_t) drc_tramp; *p++ = (uint16_t) drc_tramp >> 8;   // call far [drc_tramp]
#endif
        {
            uint16_t z = env.z80_addr, sp = z + offsetof(z80_t, sp);
            *p++ = 0xA1; *p++ = z; *p++ = z >> 8;                                // mov ax, [a]
            *p++ = 0x8B; *p++ = 0x0E; *p++ = z + 2; *p++ = (z + 2) >> 8;         // mov cx, [c]
            *p++ = 0x8B; *p++ = 0x16; *p++ = z + 4; *p++ = (z + 4) >> 8;         // mov dx, [e]
            *p++ = 0x8B; *p++ = 0x1E; *p++ = z + 6; *p++ = (z + 6) >> 8;         // mov bx, [l]
            *p++ = 0x8B; *p++ = 0x2E; *p++ = sp; *p++ = sp >> 8;                 // mov bp, [sp]
            *p++ = 0xC3;                                                         // ret
        }
#ifdef GGSWAN_HOST
        if (p > stub + STUB_CTL) { printf("HCALL stub too long\n"); exit(1); }
#endif
        env.helper_off = STUB_HCALL;
        env.ram_blocks = 1;
        env.ctl_off = STUB_CTL;
        uint16_t l5 = drc_emit_ctl_stub(&env, stub + STUB_CTL, STUB_CTL);
#ifdef GGSWAN_HOST
        if (STUB_CTL + l5 > CACHE_START) { printf("CTL stub too long\n"); exit(1); }
#else
        (void) l5;
#endif
        env.dyn_off = put_dyn(stub, cache_bank);   // (the same in every bank)
        env.nt_stub_off = STUB_NT;
        uint16_t end = put_nt(stub, STUB_NT);
        env.outi_off = STUB_OUTI;
        uint16_t l1 = drc_emit_run_stub(&env, stub + STUB_OUTI, STUB_OUTI, 0);
        uint16_t l2 = drc_emit_run_stub(&env, stub + STUB_OUTI + DRC_OUTD_DELTA, STUB_OUTI + DRC_OUTD_DELTA, 1);
        env.ldi_off = STUB_LDI;
        uint16_t l3 = drc_emit_ldi_stub(&env, stub + STUB_LDI, STUB_LDI, 0);
        uint16_t l4 = drc_emit_ldi_stub(&env, stub + STUB_LDI + DRC_LDD_DELTA, STUB_LDI + DRC_LDD_DELTA, 1);
#ifdef GGSWAN_HOST
        if (getenv("GGSWAN_STUBS")) printf("stubs: NT end %X, OUTI %X, OUTD %X, LDI %X, LDD %X\n", end, l1, l2, l3, l4);
        if (end > STUB_OUTI || l1 > DRC_OUTD_DELTA || STUB_OUTI + DRC_OUTD_DELTA + l2 > STUB_LDI
            || l3 > DRC_LDD_DELTA || STUB_LDI + DRC_LDD_DELTA + l4 > STUB_HCALL) { printf("stubs too long\n"); exit(1); }
#else
        (void) end; (void) l1; (void) l2; (void) l3; (void) l4;
#endif
        code_write(0, stub, CACHE_START);
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
    prof_phase = PROF_TRANSLATE;
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
    prof_phase = PROF_INTERP;
    z80_step();
    prof_phase = PROF_DISPATCH;
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
volatile uint8_t prof_phase;
uint8_t drc_no_links;
uint8_t drc_no_fast;

uint32_t drc_stat_helper[5], drc_stat_dispatch, drc_stat_nolink;
static void drc_helper(void) {
    drc_stat_helper[drc_st.op == DRC_H_WRITE_MAP ? DRC_H_WRITE
                    : drc_st.op >= DRC_H_OUTI_RUN ? DRC_H_OUT : drc_st.op]++;
    prof_phase = drc_st.op == DRC_H_STEP ? PROF_STEP : PROF_HELPER;
    uint8_t m0 = gg.mapper[0], m1 = gg.mapper[1], m2 = gg.mapper[2], m3 = gg.mapper[3];
    uint8_t safe = drc_st.op == DRC_H_WRITE_MAP;
    drc_helper_op();
    if (!remapped && (z80_break || m1 != gg.mapper[1]
                      || (!safe && (m0 != gg.mapper[0] || m2 != gg.mapper[2] || m3 != gg.mapper[3])))) {
        // the mapping changed: the rest of the block must not follow links
        // to blocks translated for the old mapping (cycles <= 0 stops them);
        // or the timing must be planned again (z80_break): stop too
        remapped = 1;
        remap_saved = drc_st.cycles;
        drc_st.cycles = -30000;
    }
}

// the helper stub calls this (through drc_helper_tramp, drc_asm.s)
void drc_helper_direct(void) {
    drc_helper();
    prof_phase = PROF_CODE;
}

static void drc_helper_op(void) {
    switch (drc_st.op) {
    case DRC_H_WRITE: case DRC_H_WRITE_MAP: z80_write(drc_st.arg, drc_st.val); break;
    case DRC_H_OUT:
#ifdef GGSWAN_HOST
        { extern uint32_t hs_out[256]; hs_out[drc_st.arg & 255]++; }
#endif
        z80_out(drc_st.arg, drc_st.val); break;
    case DRC_H_OUTI_RUN: case DRC_H_OUTD_RUN: {
        // the OUTIs the routine left (palette, other ports): straight in C,
        // not instruction by instruction; flags as the routine's
        uint16_t hl = (z80.h << 8) | z80.l;
        int8_t d = drc_st.op == DRC_H_OUTI_RUN ? 1 : -1;
        uint8_t n = drc_st.val, v = 0;
        drc_st.cycles -= 16 * n;   // (the routine gave them back)
        while (n--) {
            v = z80_read(hl);
            hl += d;
            z80.b--;
            z80_out(z80.c, v);
        }
        z80.h = hl >> 8; z80.l = hl;
        z80.f = (z80.b & 0x80) | (z80.b ? 0 : 0x40) | ((v & 0x80) ? 0x02 : 0);
        break;
    }
    case DRC_H_IN:
#ifdef GGSWAN_HOST
        { extern uint32_t hs_in[256]; hs_in[drc_st.arg & 255]++; }
#endif
        drc_st.val = z80_in(drc_st.arg); break;
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
        // (the EXIT stub starts with mov byte [reason], 0: C6h)
        if (i < DRC_CODE_BANKS && *((const uint8_t __far*) MK_FP(DRC_CODE_SEG + ((uint16_t) i << 12), 0)) != 0xC6)
            return i + 1;
    }
    return 0;
}

/* === Self-check (diagnostics): every block is also run by the interpreter === */

uint8_t drc_check;
static z80_t check_z;
static gg_vdp_t check_vdp;   // (the VDP ports are partly handled natively)

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
    check_vdp = gg.vdp;
    psram_write(DRC_CODE_BANKS + 1, 0, (const uint8_t*) IRAM(0x8000), 0x2000);
}

// after a block: run the interpreter from the saved state and compare
static void check_end(uint16_t pc, int16_t used) {
    z80_t d = z80;
    uint16_t sum_d = ram_sum();
    int32_t left = z80.cycles;
    // back to the state before the block
    z80 = check_z;
    gg.vdp = check_vdp;
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
        if ((z80.pc == d.pc && c >= used) || c > used + 100 || steps > 20000)
            break;
    }
    z80.irq = irq;
    uint16_t sum_r = ram_sum();
    // (a wait loop ends the run at once: the interpreter spins until the
    // cycles run out, a few more)
    if (d.pc == pc && c > used && c < used + 30)
        c = used;
    if (z80.pc != d.pc || c != used || z80.a != d.a || (z80.f & 0xD7) != (d.f & 0xD7)
        || z80.b != d.b || z80.c != d.c || z80.d != d.d || z80.e != d.e
        || z80.h != d.h || z80.l != d.l || z80.sp != d.sp
        || z80.ixl != d.ixl || z80.ixh != d.ixh || z80.iyl != d.iyl || z80.iyh != d.iyh
        || sum_r != sum_d)
        drc_check_failed(pc, &check_z, &d, &z80, used, c, sum_d, sum_r);
    z80.cycles = left;
}

/*
 * Called by drc_enter each time the translated code returns: the most
 * common reason in games with line interrupts (Super Monaco GP: on every
 * line) is a block that ran out of cycles at the start of the line of the
 * next interrupt. Then the line starts here (gg_fast_line), the interrupt
 * is taken at once if the Z80 accepts it, and drc_enter goes on with the
 * block at 0038h (or, through DYN, with the block at pc), without the
 * dispatcher and the frame loop. Returns 1 with drc_entry set, 0 when
 * anything differs (the dispatcher does it all as usual).
 */
uint8_t drc_fast_reentry(void) {
    if (drc_no_fast || drc_st.reason || drc_st.cycles > 0 || drc_check || z80_break
        || drc_flush_request || drc_st.ram_fail || remapped
        || z80.ei_pending || z80.halted)
        return 0;
    // an interrupt accepted at once: IM 1 with the stack in work RAM only
    uint16_t sp = z80.sp - 2;
    uint8_t take = z80.iff1;
    if (take && (z80.im == 2 || sp < 0xC000 || sp >= 0xDFFF))
        return 0;
    int32_t left = drc_cycles_left();
    if (left > 0 || left <= -GG_CYCLES_PER_LINE)
        return 0;
    prof_phase = PROF_DISPATCH;
    int16_t want = gg_fast_line((int16_t) left);
    if (!want) {
        prof_phase = PROF_CODE;
        return 0;
    }
    // (the line interrupt is now requested: z80.irq)
    drc_st.cycles += want;
    drc_st.link = 0;
    // on through DYN (it looks the block of pc up, or exits to the
    // dispatcher): the same in every code bank
    drc_entry[0] = env.dyn_off;
    drc_entry[1] = DRC_CODE_SEG;
    if (take) {
        // IM 1, the same as the dispatcher
        z80.iff1 = z80.iff2 = 0;
        z80.r = (z80.r & 0x80) | ((z80.r + 1) & 0x7F);
        z80.sp = sp;
        gg_ram[sp & 0x1FFF] = (uint8_t) z80.pc;
        gg_ram[(sp + 1) & 0x1FFF] = z80.pc >> 8;
        z80.pc = 0x38;
        drc_st.cycles -= 13;
        drc_stat_irq++;
        if (fast38_seg && fast38_bank == bank_key(0x38)) {
            drc_entry[0] = fast38_off;
            drc_entry[1] = fast38_seg;
        }
    }
    prof_phase = PROF_CODE;
    return 1;
}

int32_t drc_z80_run(int32_t cycles) {
    prof_phase = PROF_DISPATCH;
    z80.cycles += cycles;
    drc_st.nolink = drc_no_links;
    for (;;) {
        if (z80.cycles <= 0 || z80_break) {
            // out of cycles, or the timing to plan again (z80_break): the
            // next event without going back to the frame loop (the same
            // work: gg_line_event); the end of the frame still returns
            if (drc_no_fast)
                break;
            int32_t want = gg_line_event(z80.cycles);
            if (want < 0)
                break;
            z80_break = 0;
            z80.cycles += want;
            continue;
        }
        uint16_t pc = z80.pc;
        if (drc_flush_request) { drc_flush_request = 0; flush(); }
        if (z80.halted) {
            pending_link = 0;
            if (z80.irq && z80.iff1) { drc_stat_irq++; interp_one(); continue; }
            z80.cycles = 0;   // (nothing to do up to the next event)
            continue;
        }
        if (z80.irq && z80.iff1) {
            pending_link = 0;
            uint16_t sp = z80.sp - 2;
            if (!z80.ei_pending && z80.im != 2 && sp >= 0xC000 && sp < 0xDFFF) {
                // IM 1 with the stack in work RAM (the usual case): taken
                // here, the same as the interpreter's take_irq()
                z80.iff1 = z80.iff2 = 0;
                z80.r = (z80.r & 0x80) | ((z80.r + 1) & 0x7F);
                z80.sp = sp;
                gg_ram[sp & 0x1FFF] = (uint8_t) z80.pc;
                gg_ram[(sp + 1) & 0x1FFF] = z80.pc >> 8;
                z80.pc = 0x38;
                z80.cycles -= 13;
                drc_stat_irq++;
                continue;
            }
            // interrupt: taken by the interpreter (or, just after EI,
            // the one instruction before it)
            if (!z80.ei_pending) drc_stat_irq++;
            interp_one();
            continue;
        }
        // (no interrupt waiting: the instruction after EI needs nothing
        // special, an interrupt can only arrive after the block)
        z80.ei_pending = 0;
        // work RAM C000-DFFF: translated blocks that check their bytes
        // (key RAM_KEY); the mirror and cartridge RAM: interpreter
        uint8_t ram_code = pc >= 0xC000 && pc < 0xDFC0;
        if ((pc >= 0xC000 && !ram_code) || (pc >= 0x8000 && pc < 0xC000 && (drc_st.keys[0] & 0x08))) {
            // code in RAM (it may change): interpreter
            pending_link = 0;
            drc_last_interp_pc = pc;
#ifdef GGSWAN_HOST
            { extern uint32_t hs_ram_pc[8192]; hs_ram_pc[pc & 8191]++; }
#endif
            interp_one();
            continue;
        }
        uint8_t bank = ram_code ? RAM_KEY : bank_key(pc);
        // 4-way: the four slots of the set of h
        uint16_t h0 = hash(pc, bank) & ~3;
        // L0 first (internal RAM, the same one DYN uses): pc, bank,
        // offset, code segment
        uint16_t *l0 = (uint16_t*) IRAM(L0_ADDR + (h0 & 0x7FC) * 2);
        const uint16_t __far *tw = MK_FP(DRC_TABLE_SEG, 0);
        const uint8_t __far *tb = (const uint8_t __far*) tw;
        uint16_t h = h0, off = 0;
        uint8_t i;
        if (l0[0] == pc && (uint8_t) l0[1] == bank) {
            off = l0[2];
            drc_entry[1] = l0[3];
            goto have_block;
        }
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
        if (off == DRC_NOT_TRANSLATED) {
            // (repeated instructions, LDIR, OTIR...: all their turns here)
            pending_link = 0;
            drc_last_interp_pc = pc;
            do interp_one();
            while (z80.pc == pc && z80.cycles > 0 && !z80_break && !z80.halted
                   && !(z80.irq && z80.iff1) && !drc_flush_request);
            continue;
        }
        drc_entry[1] = DRC_CODE_SEG + ((uint16_t) tb[T_CODE + h] << 12);
        l0[0] = pc; l0[1] = bank; l0[2] = off; l0[3] = drc_entry[1];
    have_block:
        if (pc == 0x38) {
            fast38_off = off; fast38_seg = drc_entry[1]; fast38_bank = bank;
        }
        // the previous block can jump straight here from now on
        if (pending_link && !drc_no_links) {
            // (a guarded exit has "cmp byte [key], bank; jne X" just before
            // L; any other has "jle X" there: see drc.c)
            const uint8_t __far *g = MK_FP(pending_seg, pending_link - 2);
            link_patch(pending_seg, pending_link, drc_entry[1], off,
                       g[0] == 0x75, bank);
        }
        pending_link = 0;
        int16_t start = z80.cycles > 30000 ? 30000 : (int16_t) z80.cycles;
        drc_st.cycles = start;
        block_start = start;
        uint32_t io_before = 0;
        if (drc_check) {
            io_before = drc_stat_helper[1] + drc_stat_helper[2] + drc_stat_helper[3] + drc_stat_helper[4];
            check_begin();
        }
#ifdef GGSWAN_HOST
        host_trace(pc, z80.b, z80.h << 8 | z80.l);
#endif
        in_block = 1;
        prof_phase = PROF_CODE;
        drc_enter(off);
        while (drc_st.reason) {
            drc_entry[1] = drc_st.cur_seg;   // (blocks may jump far)
            drc_helper();
            prof_phase = PROF_CODE;
            drc_enter(drc_st.resume);
        }
        drc_entry[1] = drc_st.cur_seg;
        uint8_t ram_failed = drc_st.ram_fail;
        if (drc_st.ram_fail) {
            // a block in RAM whose code changed (at z80.pc; entered from
            // here, a link or DYN): the interpreter from now on
            uint16_t marker = DRC_NOT_TRANSLATED;
            uint16_t f0 = hash(z80.pc, RAM_KEY) & ~3;
            drc_st.ram_fail = 0;
            for (uint16_t f = f0; f < f0 + 4; f++)
                if (tw[(T_PC / 2) + f] == z80.pc && tb[T_BANK + f] == RAM_KEY)
                    psram_write(DRC_CODE_BANKS, T_OFF + f * 2, (const uint8_t*) &marker, 2);
            memset(IRAM(L0_ADDR), 0xFF, L0_SIZE);   // (DYN may know it)
#ifdef GGSWAN_HOST
            { extern uint32_t hs_ram_fail; hs_ram_fail++; }
#endif
        }
        prof_phase = PROF_DISPATCH;
        in_block = 0;
        if (remapped) {
            remapped = 0;
            drc_st.cycles = remap_saved + (drc_st.cycles + 30000);
        } else if (drc_st.link) {
            pending_link = drc_st.link;
            pending_seg = drc_entry[1];
        } else {
            drc_stat_nolink++;
#ifdef GGSWAN_HOST
            { extern void hs_nolink_pc(uint16_t, int16_t); hs_nolink_pc(z80.pc, drc_st.cycles); }
            { extern uint32_t hs_nolink_kind[8]; uint8_t a = pc >> 14, b = z80.pc >> 14;
              hs_nolink_kind[b >= 3 ? 3 : (a == b ? 0 : (b == 0 ? 1 : 2))]++; }
#endif
        }
        drc_stat_dispatch++;
        // (blocks that used helpers are not checked: running their I/O
        // twice would not give the same results)
        // (nor blocks in RAM that found their code changed: they ran
        // nothing, the interpreter goes on from there)
        if (drc_check && !ram_failed
            && io_before == drc_stat_helper[1] + drc_stat_helper[2] + drc_stat_helper[3] + drc_stat_helper[4])
            check_end(pc, start - drc_st.cycles);
        z80.cycles -= start - drc_st.cycles;
    }
    prof_phase = PROF_OTHER;
    return z80.cycles;
}
