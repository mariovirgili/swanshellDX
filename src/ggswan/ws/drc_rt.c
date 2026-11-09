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
#define STUB_CLS    0x0980   // a VRAM byte outside tile data (drc.c)
#define STUB_CRAM   0x01E0   // a byte to CRAM
#define STUB_OUTI   0x0240   // (OUTD: + DRC_OUTD_DELTA)
#define STUB_LDI    0x04E0   // (LDD: + DRC_LDD_DELTA)
#define STUB_HCALL  0x0610   // helpers called without leaving the block
#define STUB_LINE   0x0660   // the exits' common part, the line routine
#define STUB_CTL    0x0800   // OUT (BFh),A
#define STUB_MAP    0x0A60   // a bank change of slot 1 or 2 (put_map)
#define STUB_VCNT   0x0AD8   // IN A,(7Eh) (drc_emit_vcnt_stub)
#define CACHE_START 0x0B28
// translation buffer in free IRAM (between the font tiles and the mask tile)
#define BUF_ADDR    0xA600
/// DYN L0 cache: 512 direct-mapped entries of 8 bytes in free IRAM (in the
/// WS Color 4bpp mode C000-FDFF is free): Z80 pc (FFFF: empty), bank,
/// -, offset, segment of the block
#define L0_ADDR     0xD000
/// block table key of the code in work RAM (banks are below 64)
#define RAM_KEY     0xFE
#define L0_SIZE     0x1000
/// the L0 entry of Z80 address pc (the bank is in the entry): DYN and the
/// dispatcher index it the same way
#define L0_ENTRY(pc) (L0_ADDR + (((pc) << 3) & 0x0FF8))
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
uint16_t drc_diag_irq_line;   // (diagnostics: the line of the last interrupt taken in C)
uint16_t drc_last_interp_pc;
uint16_t drc_last_tr_pc;   // (diagnostics: the last block translated)
/// far address of drc_helper_tramp (drc_asm.s fills it in), called by the
/// HCALL stub
uint16_t drc_tramp[2];

// Write to PSRAM bank code_bank + which through the RAM window
static void psram_write(uint8_t which, uint16_t off, const uint8_t *src, uint16_t len) {
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, code_bank + which);
    uint8_t __far *dst = MK_FP(0x1000, off);
    if (src) {
        _fmemcpy(dst, src, len);
    } else {
        for (uint16_t i = 0; i < len; i++)
            dst[i] = 0;
    }
    ws_ram_window_restore();   // back to Game Gear slot 2 (main.c)
}

// a block table entry (code == 0xFF: leave T_CODE alone), in one window
// switch instead of one per field
static void table_put(uint16_t h, uint16_t pc, uint16_t off, uint8_t bank, uint8_t code) {
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, code_bank + DRC_CODE_BANKS);
    uint16_t __far *w = MK_FP(0x1000, 0);
    uint8_t __far *b = MK_FP(0x1000, 0);
    w[(T_PC / 2) + h] = pc;
    w[(T_OFF / 2) + h] = off;
    b[T_BANK + h] = bank;
    if (code != 0xFF)
        b[T_CODE + h] = code;
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
    // (one window switch for the jump and the guard byte)
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, code_bank + ((seg - DRC_CODE_SEG) >> 12));
    uint8_t __far *dst = MK_FP(0x1000, link);
    for (uint8_t i = 0; i < 5; i++)
        dst[i] = j[i];
    if (guard)
        dst[-3] = bank;
    ws_ram_window_restore();
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
 * LINE (at STUB_LINE; EXIT jumps here): the common part of the exits, and
 * before it the line routine. A block that ran out of cycles at the start
 * of the line of the next line interrupt (the common case in games with
 * line interrupts on many lines: Super Monaco GP) does not go back to C:
 * the line starts here (as gg_fast_line), the interrupt is taken (IM 1, as
 * the dispatcher) and the code goes on at the block of 0038h, or through
 * DYN at pc if the Z80 does not take it now. Anything else: the usual exit
 * (registers stored, back to drc_enter). The same conditions and the same
 * results as drc_fast_reentry (C), with the Z80 registers still in the x86
 * ones. Returns the offset of the common part (for HELPER).
 */
static uint16_t put_line(uint8_t *stub) {
    uint8_t *p = stub + STUB_LINE;
    uint16_t a;
#define PB(x) (*p++ = (x))
#define PW(x) (a = (x), *p++ = a, *p++ = a >> 8)
#define S_(f) ((uint16_t) (env.state_addr + offsetof(drc_state_t, f)))
#define Z_(f) ((uint16_t) (env.z80_addr + offsetof(z80_t, f)))
#define V_(f) ((uint16_t) (env.vdp_addr + offsetof(gg_vdp_t, f)))
    uint8_t *bail[4]; uint8_t nb = 0;
    uint8_t *common, *tramp;
    // a conditional jump to common: backwards, through a "jmp common" put
    // in reach when common is too far
#define BAIL(op) do { \
        if (p + 2 - tramp > 120) { \
            uint16_t r_ = (uint16_t) (common - (p + 5)); \
            PB(0xEB); PB(3); tramp = p; PB(0xE9); PW(r_); \
        } \
        uint8_t b_ = (uint8_t) (tramp - (p + 2)); \
        PB(op); PB(b_); \
    } while (0)
    // EXIT2: out of cycles, line routine on?
    PB(0x83); PB(0x3E); PW(S_(cycles)); PB(0);       // cmp word [cycles], 0
    PB(0x7F); bail[nb++] = p++;                      // jg common
    PB(0x80); PB(0x3E); PW(S_(fl_off)); PB(0);       // cmp byte [fl_off], 0
    PB(0x74); uint8_t *to_line = p++;                // je LINE
    common = tramp = p;
    for (uint8_t i = 0; i < nb; i++) *bail[i] = (uint8_t) (common - (bail[i] + 1));
    p = put_store_regs(p);
    uint16_t cur_seg = S_(cur_seg);
    PB(0x8C); PB(0x0E); PW(cur_seg);                 // mov [cur_seg], cs
    PB(0xCB);                                        // retf
    *to_line = (uint8_t) (p - (to_line + 1));
    // LINE: the checks (nothing changed yet)
    PB(0x80); PB(0x3E); PW(S_(ram_fail)); PB(0);     // cmp byte [ram_fail], 0
    BAIL(0x75);                                      // jne common
    PB(0x8B); PB(0x36); PW(S_(cycles));              // mov si, [cycles]
    PB(0x03); PB(0x36); PW(S_(left_base));           // add si, [left_base] (left)
    PB(0x83); PB(0xFE); PB(0x00);                    // cmp si, 0
    BAIL(0x7F);                                      // jg common
    PB(0x81); PB(0xFE); PW((uint16_t) -GG_CYCLES_PER_LINE);   // cmp si, -228
    BAIL(0x7E);                                      // jle common
    PB(0xF6); PB(0x06); PW(V_(reg)); PB(0x10);       // test byte [reg0], 10h
    BAIL(0x74);                                      // jz common (no line interrupt)
    PB(0x80); PB(0x3E); PW(V_(line_counter)); PB(0); // cmp byte [line_counter], 0
    BAIL(0x75);                                      // jne common
    PB(0x8B); PB(0x36); PW(V_(started));             // mov si, [started] (e)
    PB(0x85); PB(0xF6);                              // test si, si
    BAIL(0x74);                                      // jz common (line 0)
    PB(0x8B); PB(0x3E); PW(V_(reg) + 10);            // mov di, [reg10]
    PB(0x81); PB(0xE7); PW(0x00FF);                  // and di, FFh (r)
    PB(0x81); PB(0xFF); PW(128);                     // cmp di, 128
    BAIL(0x73);                                      // jae common
    PB(0x01); PB(0xF7);                              // add di, si
    PB(0x47);                                        // inc di (e + r + 1)
    PB(0x81); PB(0xFF); PW(GG_PRESENT_LINE - 1);     // cmp di, 167
    BAIL(0x73);                                      // jae common (next not in the view)
    PB(0x83); PB(0x3E); PW(V_(frame_given) + 2); PB(0);   // cmp word [frame_given + 2], 0
    BAIL(0x75);                                      // jne common
    PB(0x8B); PB(0x3E); PW(V_(line_pos));            // mov di, [line_pos] (e * 228)
    PB(0x3B); PB(0x3E); PW(V_(frame_given));         // cmp di, [frame_given]
    BAIL(0x75);                                      // jne common
    PB(0x80); PB(0x3E); PW(Z_(ei_pending)); PB(0);   // cmp byte [ei_pending], 0
    BAIL(0x75);                                      // jne common
    PB(0x80); PB(0x3E); PW(Z_(halted)); PB(0);       // cmp byte [halted], 0
    BAIL(0x75);                                      // jne common
    PB(0x80); PB(0x3E); PW(Z_(iff1)); PB(0);         // cmp byte [iff1], 0
    PB(0x74); uint8_t *no_irq = p++;                 // je NOIRQ
    PB(0x80); PB(0x3E); PW(Z_(im)); PB(2);           // cmp byte [im], 2
    BAIL(0x74);                                      // je common
    PB(0x8D); PB(0x7E); PB(0xFE);                    // lea di, [bp - 2]
    PB(0x81); PB(0xFF); PW(0xC000);                  // cmp di, C000h
    BAIL(0x72);                                      // jb common
    PB(0x81); PB(0xFF); PW(0xDFFF);                  // cmp di, DFFFh
    BAIL(0x73);                                      // jae common
    // start the line, take the interrupt
    PB(0xE8); uint8_t *call1 = p; PW(0);             // call START
    PB(0xC6); PB(0x06); PW(Z_(iff1)); PB(0);         // iff1 = 0
    PB(0xC6); PB(0x06); PW(Z_(iff2)); PB(0);         // iff2 = 0
    PB(0x50);                                        // push ax
    PB(0xA0); PW(Z_(r));                             // mov al, [r]
    PB(0x88); PB(0xC4);                              // mov ah, al
    PB(0xFE); PB(0xC0);                              // inc al
    PB(0x25); PW(0x807F);                            // and ax, 807Fh
    PB(0x08); PB(0xE0);                              // or al, ah
    PB(0xA2); PW(Z_(r));                             // mov [r], al
    PB(0x8D); PB(0x6E); PB(0xFE);                    // lea bp, [bp - 2]
    PB(0x8B); PB(0x36); PW(Z_(pc));                  // mov si, [pc]
    PB(0x89); PB(0xB6); PW((uint16_t) env.ram_delta);   // mov [bp + RAM], si (push pc)
    PB(0xC7); PB(0x06); PW(Z_(pc)); PW(0x0038);      // pc = 0038h
    PB(0x83); PB(0x2E); PW(S_(cycles)); PB(13);      // sub word [cycles], 13
#ifndef GGSWAN_HOST
    PB(0x83); PB(0x06); PW((uint16_t) &drc_stat_irq); PB(1);          // add word [stat_irq], 1
    PB(0x83); PB(0x16); PW((uint16_t) &drc_stat_irq + 2); PB(0);      // adc word [stat_irq + 2], 0
#endif
    // the block at 0038h, if known for this slot 0 bank; else DYN
    PB(0x83); PB(0x3E); PW(S_(fast38) + 2); PB(0);   // cmp word [fast38 + 2], 0
    PB(0x74); uint8_t *to_dyn1 = p++;                // je DYN
    PB(0xA0); PW(S_(keys) + 1);                      // mov al, [keys + 1] (slot 0)
    PB(0x3A); PB(0x06); PW(S_(fast38_bank));         // cmp al, [fast38_bank]
    PB(0x58);                                        // pop ax
    PB(0x75); uint8_t *to_dyn2 = p++;                // jne DYN
    PB(0xFF); PB(0x2E); PW(S_(fast38));              // jmp far [fast38]
    // NOIRQ: start the line, on at pc through DYN
    *no_irq = (uint8_t) (p - (no_irq + 1));
    PB(0xE8); uint8_t *call2 = p; PW(0);             // call START
    PB(0x50);                                        // push ax (as above)
    *to_dyn1 = (uint8_t) (p - (to_dyn1 + 1));        // (DYN with ax pushed)
    PB(0x58);                                        // pop ax
    *to_dyn2 = (uint8_t) (p - (to_dyn2 + 1));
    {
        uint16_t r_ = (uint16_t) (env.dyn_off - (uint16_t) (p + 3 - stub));
        PB(0xE9); PW(r_);                            // jmp DYN
    }
    // START: the line starts (as gg_fast_line); SI, DI only
    uint8_t *start = p;
    {
        uint16_t r1 = (uint16_t) (start - (call1 + 2)), r2 = (uint16_t) (start - (call2 + 2));
        call1[0] = (uint8_t) r1; call1[1] = (uint8_t) (r1 >> 8);
        call2[0] = (uint8_t) r2; call2[1] = (uint8_t) (r2 >> 8);
    }
    PB(0xC6); PB(0x06); PW(V_(line_pending)); PB(1); // line_pending = 1
    PB(0x8B); PB(0x36); PW(V_(reg) + 10);            // mov si, [reg10]
    PB(0x81); PB(0xE6); PW(0x00FF);                  // and si, FFh (r)
    PB(0x50);                                        // push ax
    PB(0xA0); PW(V_(reg) + 10);                      // mov al, [reg10]
    PB(0xA2); PW(V_(line_counter));                  // mov [line_counter], al
    PB(0x58);                                        // pop ax
    PB(0x8B); PB(0x3E); PW(V_(started));             // mov di, [started]
    PB(0x89); PB(0x3E); PW(V_(vcounter));            // mov [vcounter], di
    PB(0x47);                                        // inc di
    PB(0x89); PB(0x3E); PW(V_(started));             // mov [started], di
    PB(0x81); PB(0x06); PW(V_(line_pos)); PW(GG_CYCLES_PER_LINE);   // add word [line_pos], 228
    PB(0xC6); PB(0x06); PW(Z_(irq)); PB(1);          // irq = 1 (update_irq: line, on)
    // want = (r + 1) * 228 = 256x + 4x - 32x
    PB(0x46);                                        // inc si (x)
    PB(0x89); PB(0xF7);                              // mov di, si
    PB(0xC1); PB(0xE6); PB(0x08);                    // shl si, 8
    PB(0xC1); PB(0xE7); PB(0x02);                    // shl di, 2
    PB(0x01); PB(0xFE);                              // add si, di
    PB(0xC1); PB(0xE7); PB(0x03);                    // shl di, 3
    PB(0x29); PB(0xFE);                              // sub si, di
    PB(0x01); PB(0x36); PW(V_(frame_given));         // add [frame_given], si
    PB(0x83); PB(0x16); PW(V_(frame_given) + 2); PB(0);   // adc word [frame_given + 2], 0
    PB(0x01); PB(0x36); PW(S_(cycles));              // add [cycles], si
    PB(0xC7); PB(0x06); PW(S_(link)); PW(0);         // link = 0
    PB(0xC3);                                        // ret
#ifdef GGSWAN_HOST
    if (p > stub + STUB_CTL) { printf("LINE stub too long\n"); exit(1); }
    if (getenv("GGSWAN_STUBS")) printf("LINE stub %X bytes\n", (unsigned) (p - stub - STUB_LINE));
#endif
#undef PB
#undef PW
#undef S_
#undef Z_
#undef V_
#undef BAIL
    return (uint16_t) (common - stub);
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
    // (no links, a diagnostics option: always back to the runtime; the
    // stubs are made again when it is set, drc_rt_set_no_links)
    if (drc_no_links) { PB(0xEB); PREL(exit0); }     // jmp EXIT0
    (void) nolink;
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
    // L0 first: entry (pc << 3) & FF8h (L0_ENTRY)
    PB(0x89); PB(0xC6);                              // mov si, ax
    PB(0xC1); PB(0xE6); PB(0x03);                    // shl si, 3
    PB(0x81); PB(0xE6); PW(0x0FF8);                  // and si, FF8h
    PB(0x3B); PB(0x84); PW(L0_ADDR);                 // cmp ax, [si + L0]
    PB(0x75); uint8_t *l0miss1 = p++;                // jne
    PB(0x3A); PB(0x9C); PW(L0_ADDR + 2);             // cmp bl, [si + L0 + 2]
    PB(0x75); uint8_t *l0miss2 = p++;                // jne
    PB(0x5B); PB(0x58);                              // pop bx; pop ax
    PB(0xFF); PB(0xAC); PW(L0_ADDR + 4);             // jmp far [si + L0 + 4]
    *l0miss1 = (uint8_t) (p - l0miss1 - 1);
    *l0miss2 = (uint8_t) (p - l0miss2 - 1);
    PB(0x89); PB(0x36); PW(l0_slot);                 // mov [l0_slot], si
    // the block table: hash and 4-way set as drc_z80_run
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
    // palette, h flip, v flip: bits 11, 9, 10 -> a table of 8
    PB(0x89); PB(0xC3);                                  // mov bx, ax
    PB(0xC1); PB(0xEB); PB(0x08);                        // shr bx, 8
    PB(0x83); PB(0xE3); PB(0x0E);                        // and bx, 0Eh
    PB(0x0B); PB(0x97); PW(env.state_addr + offsetof(drc_state_t, nt_flags));   // or dx, [bx + nt_flags]
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

/*
 * CRAM (at STUB_CRAM): a byte (in val) written to the VDP data port with
 * code 3, as vdp_write_data and gg_platform_cram_write: the even byte is
 * latched, the odd one writes the color (Game Gear ----BBBBGGGGRRRR) and
 * its WS copy (----RRRRGGGGBBBB): palette 0-15 at FE00h, 16-31 at FE20h
 * and at FF00h (sprites). Read buffer, address + 1, latch. Uses SI.
 */
static uint16_t put_cram(uint8_t *stub, uint16_t at) {
    uint8_t *p = stub + at;
    uint16_t a;
#define PB(x) (*p++ = (x))
#define PW(x) (a = (x), *p++ = a, *p++ = a >> 8)
#define V_(f) ((uint16_t) (env.vdp_addr + offsetof(gg_vdp_t, f)))
    uint16_t val = env.state_addr + offsetof(drc_state_t, val);
    PB(0x50); PB(0x53);                                  // push ax; push bx
    PB(0xA0); PW(val);                                   // mov al, [val]
    PB(0xA2); PW(V_(read_buffer));                       // mov [read_buffer], al
    PB(0xC6); PB(0x06); PW(V_(latch_full)); PB(0);       // mov byte [latch_full], 0
    PB(0x8B); PB(0x1E); PW(V_(addr));                    // mov bx, [addr]
    PB(0x8D); PB(0x77); PB(0x01);                        // lea si, [bx + 1]
    PB(0x81); PB(0xE6); PW(0x3FFF);                      // and si, 3FFFh
    PB(0x89); PB(0x36); PW(V_(addr));                    // mov [addr], si
    PB(0x83); PB(0xE3); PB(0x3F);                        // and bx, 3Fh
    PB(0xF6); PB(0xC3); PB(0x01);                        // test bl, 1
    PB(0x75); PB(0x06);                                  // jnz odd
    PB(0xA2); PW(V_(cram_latch));                        // mov [cram_latch], al
    PB(0x5B); PB(0x58); PB(0xC3);                        // pop bx; pop ax; ret
    // odd:
    PB(0x24); PB(0x0F);                                  // and al, 0Fh
    PB(0x8A); PB(0x26); PW(V_(cram_latch));              // mov ah, [cram_latch]
    PB(0x88); PB(0xA7); PW(V_(cram) - 1);                // mov [bx + cram - 1], ah
    PB(0x88); PB(0x87); PW(V_(cram));                    // mov [bx + cram], al
    PB(0x89); PB(0xC6);                                  // mov si, ax
    PB(0x25); PW(0x0F0F);                                // and ax, 0F0Fh (R, B)
    PB(0x81); PB(0xE6); PW(0xF000);                      // and si, F000h (G)
    PB(0xC1); PB(0xEE); PB(0x08);                        // shr si, 8
    PB(0x09); PB(0xF0);                                  // or ax, si
    PB(0x80); PB(0xE3); PB(0x3E);                        // and bl, 3Eh (color * 2)
    PB(0x89); PB(0x87); PW(0xFE00);                      // mov [bx + FE00h], ax
    PB(0x80); PB(0xFB); PB(0x20);                        // cmp bl, 20h
    PB(0x72); PB(0x04);                                  // jb +4
    PB(0x89); PB(0x87); PW(0xFEE0);                      // mov [bx + FEE0h], ax (FF00h + color * 2 - 20h)
    PB(0x5B); PB(0x58); PB(0xC3);                        // pop bx; pop ax; ret
#undef PB
#undef PW
#undef V_
    return (uint16_t) (p - stub);
}

/*
 * LD (FFFE),A / LD (FFFF),A from a block outside that slot (drc.c): the
 * new bank in [val]. The RAM mirror, the block table key, the WS window
 * (ROM window 1 for slot 1, the RAM window on PSRAM for slot 2: as
 * main.c's gg_platform_bank, GG_PSRAM_FIRST_BANK 0), the segments of the
 * generated code; map_pend tells the C side (drc_map_sync: gg.mapper,
 * gg_slot) to catch up. Slot 2 with the cartridge RAM enabled (keys[0]):
 * carry set, the helper. A helper each was ~10 a frame in Sonic, ~6 in
 * Super Monaco GP II (FFFF). Returns the end.
 */
static uint16_t put_map(uint8_t *stub, uint16_t at, uint16_t *map2) {
    uint8_t *p = stub + at;
    uint16_t a;
#define PB(x) (*p++ = (x))
#define PW(x) (a = (x), *p++ = a, *p++ = a >> 8)
#define S_(f) ((uint16_t) (env.state_addr + offsetof(drc_state_t, f)))
    for (uint8_t slot = 1; slot <= 2; slot++) {
        if (slot == 2) {
            *map2 = (uint16_t) (p - stub);
            PB(0x80); PB(0x3E); PW(S_(keys)); PB(0);         // cmp byte [keys], 0
            PB(0x74); PB(0x02);                              // je +2
            PB(0xF9); PB(0xC3);                              // stc; ret
        }
        PB(0x50);                                            // push ax
        PB(0xA0); PW(S_(val));                               // mov al, [val]
        PB(0xA2); PW((uint16_t) (0x1FFD + slot + 0xC000 + env.ram_delta));   // mirror
        PB(0x80); PB(0x0E); PW(S_(map_pend)); PB(slot == 1 ? 4 : 8);   // or byte [map_pend], bit
        PB(0x22); PB(0x06); PW(S_(rom_mask));                // and al, [rom_mask]
        PB(0xA2); PW(S_(keys) + 1 + slot);                   // mov [keys + 1 + slot], al
        PB(0x88); PB(0xC4);                                  // mov ah, al
        PB(0xC0); PB(0xE8); PB(0x02);                        // shr al, 2
        PB(0x86); PB(0xE0);                                  // xchg ah, al
        PB(0x50);                                            // push ax (AH: WS bank)
        PB(0x24); PB(0x03);                                  // and al, 3
        PB(0xC0); PB(0xE0); PB(0x02);                        // shl al, 2
        PB(0x88); PB(0xC4);                                  // mov ah, al
        PB(0x30); PB(0xC0);                                  // xor al, al (AX: offset / 16)
        PB(0x05); PW(slot == 1 ? 0x3000 - 0x400 : 0x1000 - 0x800);   // add ax, window - slot
        PB(0xA3); PW(S_(segtab) + slot * 4);                 // mov [segtab + 2 * slot * 2], ax
        PB(0xA3); PW(S_(segtab) + slot * 4 + 2);
        PB(0x58);                                            // pop ax
        PB(0x88); PB(0xE0);                                  // mov al, ah
        PB(0x30); PB(0xE4);                                  // xor ah, ah
        // out ROM1 / RAM bank port, ax (16 bits: D4h / D0h on the WS;
        // patch 74 wrote C3h / C1h, the PC tests' ports, which on the WS
        // are the 8-bit ports and also wrote C4h / C2h: ROM0, slot 0's
        // window, set to 0 at each slot 2 change)
        PB(0xE7); PB(slot == 1 ? WS_CART_EXTBANK_ROM1_PORT : WS_CART_EXTBANK_RAM_PORT);
        PB(0x58);                                            // pop ax
        PB(0xF8); PB(0xC3);                                  // clc; ret
    }
#undef PB
#undef PW
#undef S_
    return (uint16_t) (p - stub);
}

// the mapper routines (put_map), once the ROM size is known (gg_init):
// only with a power of 2 banks (bank % banks as a mask), not with X4
static uint16_t map2_at;
void drc_rt_rom_banks(uint16_t b) {
    drc_st.rom_mask = (uint8_t) (b - 1);
    if (!drc_no_fast && b && b <= 256 && !(b & (b - 1))
#ifdef GGSWAN_HOST
        && !getenv("GGSWAN_NOMAP")
#endif
        ) {
        env.map1_off = STUB_MAP;
        env.map2_off = map2_at;
    }
}

// the C side of the mapper routines' bank changes (put_map): at every
// return from the generated code to C, before anything reads gg.mapper or
// gg_slot
static void drc_map_sync(void) {
    uint8_t p = drc_st.map_pend;
    if (!p)
        return;
    drc_st.map_pend = 0;
    if (p & 4)
        z80_write(0xFFFE, gg_ram[0x1FFE]);
    if (p & 8)
        z80_write(0xFFFF, gg_ram[0x1FFF]);
}

// the block at 0038h (IM 1 interrupts), for the line routine and
// drc_fast_reentry: drc_st.fast38 (offset, segment: 0 unknown), fast38_bank
#define fast38_off drc_st.fast38[0]
#define fast38_seg drc_st.fast38[1]
#define fast38_bank drc_st.fast38_bank

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
    // the name table routine's table (as conv_entry, main.c)
    for (uint8_t i = 0; i < 8; i++)
        drc_st.nt_flags[i] = (i & 1 ? 0x4000 : 0) | (i & 2 ? 0x8000 : 0) | (i & 4 ? 0x0200 : 0);
    env.no_fast = drc_no_fast;
    env.cram_off = drc_no_fast ? 0 : STUB_CRAM;   // (X4: CRAM through the helper)
    env.cls_off = drc_no_fast ? 0 : STUB_CLS;
    env.psg_queue = !drc_no_fast;
    env.pad_in = !drc_no_fast;     // (X4: inline, no deferred tile writes)
    // following JP/JR inside a block is off by default: it breaks the
    // scroll and the status bar of Wonder Boy III on the WonderSwan (not
    // on the PC tests, cause not found) and gained under 1%
    env.no_trace = 1;
    drc_st.table_seg = DRC_TABLE_SEG;
    env.ram_delta = 0x8000 - 0xC000;   // Game Gear RAM at IRAM 8000h

    for (cache_bank = 0; cache_bank < DRC_CODE_BANKS; cache_bank++) {
        uint8_t *stub = (uint8_t*) IRAM(BUF_ADDR);   // (translation buffer)
        memset(stub, 0xCC, CACHE_START);
        env.dyn_off = put_dyn(stub, cache_bank);   // (the same in every bank)
        uint16_t reason = env.state_addr + offsetof(drc_state_t, reason);
        uint16_t resume = env.state_addr + offsetof(drc_state_t, resume);
        uint8_t *p = stub + STUB_EXIT;
        *p++ = 0xC6; *p++ = 0x06; *p++ = reason; *p++ = reason >> 8; *p++ = 0;   // reason = 0
        *p++ = 0xE9; *p++ = (uint8_t) (STUB_LINE - (STUB_EXIT + 8));            // jmp LINE stub
        *p++ = (uint8_t) ((STUB_LINE - (STUB_EXIT + 8)) >> 8);
        uint16_t common = put_line(stub);
        p = stub + STUB_HELPER;
        *p++ = 0x8F; *p++ = 0x06; *p++ = resume; *p++ = resume >> 8;             // pop [resume]
        *p++ = 0xC6; *p++ = 0x06; *p++ = reason; *p++ = reason >> 8; *p++ = 1;   // reason = 1
        {
            uint16_t rel = common - (uint16_t) (p + 3 - stub);
            *p++ = 0xE9; *p++ = (uint8_t) rel; *p++ = (uint8_t) (rel >> 8);     // jmp common
        }
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
        if (p > stub + STUB_LINE) { printf("HCALL stub too long\n"); exit(1); }
#endif
        env.helper_off = STUB_HCALL;
        env.ram_blocks = 1;
        env.ctl_off = STUB_CTL;
        uint16_t l5 = drc_emit_ctl_stub(&env, stub + STUB_CTL, STUB_CTL);
#ifdef GGSWAN_HOST
        if (STUB_CTL + l5 > CACHE_START) { printf("CTL stub too long\n"); exit(1); }
        if (getenv("GGSWAN_STUBS")) printf("CTL stub %X bytes\n", l5);
#else
        (void) l5;
#endif
        env.nt_stub_off = STUB_NT;
        uint16_t end = put_nt(stub, STUB_NT);
        uint16_t l6 = drc_emit_cls_stub(&env, stub + STUB_CLS, STUB_CLS);
#ifdef GGSWAN_HOST
        if (STUB_CTL + l5 > STUB_CLS || STUB_CLS + l6 > CACHE_START) { printf("CLS stub: no room\n"); exit(1); }
        if (getenv("GGSWAN_STUBS")) printf("CLS stub %X bytes\n", l6);
#else
        (void) l6;
#endif
        uint16_t cram_end = put_cram(stub, STUB_CRAM);
        {
            uint16_t map_end = put_map(stub, STUB_MAP, &map2_at);
#ifdef GGSWAN_HOST
            if (STUB_CLS + l6 > STUB_MAP || map_end > STUB_VCNT) { printf("MAP stub: no room (%X)\n", map_end); exit(1); }
            if (getenv("GGSWAN_STUBS")) printf("MAP stubs end %X\n", map_end);
#else
            (void) map_end;
#endif
            uint16_t l7 = drc_emit_vcnt_stub(&env, stub + STUB_VCNT, STUB_VCNT);
#ifdef GGSWAN_HOST
            if (STUB_VCNT + l7 > CACHE_START) { printf("VCNT stub: no room (%X)\n", l7); exit(1); }
#else
            (void) l7;
#endif
            if (!drc_no_fast)
                env.vcnt_off = STUB_VCNT;
        }
        env.outi_off = STUB_OUTI;
        uint16_t l1 = drc_emit_run_stub(&env, stub + STUB_OUTI, STUB_OUTI, 0);
        uint16_t l2 = drc_emit_run_stub(&env, stub + STUB_OUTI + DRC_OUTD_DELTA, STUB_OUTI + DRC_OUTD_DELTA, 1);
        env.ldi_off = STUB_LDI;
        uint16_t l3 = drc_emit_ldi_stub(&env, stub + STUB_LDI, STUB_LDI, 0);
        uint16_t l4 = drc_emit_ldi_stub(&env, stub + STUB_LDI + DRC_LDD_DELTA, STUB_LDI + DRC_LDD_DELTA, 1);
#ifdef GGSWAN_HOST
        if (getenv("GGSWAN_STUBS")) printf("stubs: NT end %X, CRAM end %X, OUTI %X, OUTD %X, LDI %X, LDD %X\n", end, cram_end, l1, l2, l3, l4);
        if (end > STUB_CRAM || cram_end > STUB_OUTI || l1 > DRC_OUTD_DELTA || STUB_OUTI + DRC_OUTD_DELTA + l2 > STUB_LDI
            || l3 > DRC_LDD_DELTA || STUB_LDI + DRC_LDD_DELTA + l4 > STUB_HCALL) { printf("stubs too long\n"); exit(1); }
#else
        (void) end; (void) cram_end; (void) l1; (void) l2; (void) l3; (void) l4;
#endif
        code_write(0, stub, CACHE_START);
    }
    drc_st.segtab[6] = (uint16_t) (0x0800 - 0x0C00);
    drc_st.segtab[7] = (uint16_t) (0x0800 - 0x0E00);
    flush();
}

void drc_rt_set_no_links(uint8_t on) {
    drc_no_links = on;
    drc_rt_init(code_bank);   // (DYN without links)
}

static inline uint8_t bank_key(uint16_t pc) {
    return drc_st.keys[1 + (pc >> 14)];
}

static inline uint16_t hash(uint16_t pc, uint8_t bank) {
    return (pc ^ (pc >> 9) ^ ((uint16_t) bank << 5)) & (HASH_SIZE - 1);
}

static uint16_t translate(uint16_t pc, uint8_t bank, uint16_t h) {
    prof_phase = PROF_TRANSLATE;
    drc_last_tr_pc = pc;
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
        table_put(h, pc, DRC_NOT_TRANSLATED, bank, 0xFF);
        return DRC_NOT_TRANSLATED;
    }
    uint16_t off = cache_top;
    code_write(off, buf, r.length);
    cache_top = (cache_top + r.length + 1) & ~1;
    table_put(h, pc, off, bank, cache_bank);
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
    if (getenv("GGSWAN_TRLOG")) printf("TRBLK %u %u %u %u\n", (unsigned) ((DRC_CODE_SEG + cache_bank * 0x1000u) * 16u + off), (unsigned) r.z80_instr, (unsigned) r.length, (unsigned) gg.frame);
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

static int16_t remap_saved;   // cycles left when the mapping changed
static uint8_t remapped;
int32_t drc_cycles_left(void) {
    if (!in_block)
        return z80.cycles;
    // after a mapping change the block counts down from -30000 (to stop
    // at its end): the real count is remap_saved, less what ran since;
    // in 32 bits (block_start - cycles passes the 16-bit int: Sonic's
    // scroll write after its register 1 write gave line 106, not 193)
    int16_t c = remapped ? (int16_t) (remap_saved + (drc_st.cycles + 30000)) : drc_st.cycles;
    return z80.cycles - ((int32_t) block_start - c);
}

// The helper routine's C side: the registers are in z80
static void drc_helper_op(void);
uint8_t drc_flush_request;
volatile uint8_t prof_phase;
uint8_t drc_no_links;
uint8_t drc_no_fast;

uint32_t drc_stat_helper[5], drc_stat_dispatch, drc_stat_nolink;
uint32_t drc_stat_quick;   // (dispatches by drc_fast_reentry from L0)
uint16_t drc_stat_op[16];   // helper calls by DRC_H_* (diagnostics)
uint16_t drc_stat_op_lines[16];   // ...and the WS lines they took
static void drc_helper_body(void);
static void drc_helper(void) {
    uint8_t op = drc_st.op & 15;
    uint8_t l0 = inportb(WS_DISPLAY_LINE_PORT);
    drc_helper_body();
    uint8_t l1 = inportb(WS_DISPLAY_LINE_PORT);
    drc_stat_op[op]++;
    drc_stat_op_lines[op] += (uint8_t) (l1 >= l0 ? l1 - l0 : l1 + 159 - l0);
}
static void drc_helper_body(void) {
    drc_map_sync();
    if (drc_st.op == DRC_H_SYNC) {
        // deferred tile writes (main.c): nothing else changes. A full log
        // (the routines' retry): all of it; a VRAM read (prefetch or data
        // port, at addr): only that address. (Profiled as PO, the helpers:
        // VI is only the end of the frame, video_present)
        prof_phase = PROF_HELPER;
        if ((uint16_t) (drc_st.log_end - drc_st.log_top) < 260)
            gg_platform_vram_sync();
        else
            gg_platform_vram_sync_at(gg.vdp.addr & 0x3FFF);
        return;
    }
    if (drc_st.op == DRC_H_PSG) {
        // the PSG queue full (main.c)
        prof_phase = PROF_HELPER;
        gg_platform_psg_flush();
        return;
    }
    if (drc_st.op == DRC_H_OUT || drc_st.op == DRC_H_IN) {
        // ports: they never change the mapping, only the timing plan
        // (z80_break): no mapper snapshot
        drc_stat_helper[drc_st.op]++;
        prof_phase = PROF_HELPER;
        drc_helper_op();
        if (z80_break || drc_flush_request)
            drc_st.fl_off = 1;
        if (!remapped && z80_break) {
            remapped = 1;
            remap_saved = drc_st.cycles;
            drc_st.cycles = -30000;
            drc_st.fl_off = 1;   // (the generated routines: C, which knows)
        }
        return;
    }
    drc_stat_helper[drc_st.op == DRC_H_WRITE_MAP ? DRC_H_WRITE
                    : drc_st.op >= DRC_H_OUTI_RUN ? DRC_H_OUT : drc_st.op]++;
    prof_phase = drc_st.op == DRC_H_STEP ? PROF_STEP : PROF_HELPER;
    uint8_t m0 = gg.mapper[0], m1 = gg.mapper[1], m2 = gg.mapper[2], m3 = gg.mapper[3];
    uint8_t safe = drc_st.op == DRC_H_WRITE_MAP;
    drc_helper_op();
    if (z80_break || drc_flush_request)
        drc_st.fl_off = 1;   // (the line routine: back to C now)
    if (!remapped && (z80_break || m1 != gg.mapper[1]
                      || (!safe && (m0 != gg.mapper[0] || m2 != gg.mapper[2] || m3 != gg.mapper[3])))) {
        // the mapping changed: the rest of the block must not follow links
        // to blocks translated for the old mapping (cycles <= 0 stops them);
        // or the timing must be planned again (z80_break): stop too
        remapped = 1;
        remap_saved = drc_st.cycles;
        drc_st.cycles = -30000;
        drc_st.fl_off = 1;   // (the generated routines: C, which knows)
    }
}

// the helper stub calls this (through drc_helper_tramp, drc_asm.s)
void drc_helper_direct(void) {
    drc_helper();
    prof_phase = PROF_CODE;
}

static void drc_helper_op(void) {
    switch (drc_st.op) {
    case DRC_H_WRITE: case DRC_H_WRITE_MAP:
        z80_write(drc_st.arg, drc_st.val); break;
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
    drc_map_sync();
    // A block that ended at an exit that can not be linked (RET, JP (HL),
    // another slot...) with cycles left and nothing waiting: the next
    // block from the L0 cache, without going back to drc_z80_run. Each
    // trip there cost a few hundred cycles (Aladdin ~20 a frame, DI 13-16%
    // on the device build run in Unicorn, wsrun.py). The block goes on
    // counting from the same start (drc_st.cycles, block_start, left_base
    // as they are), as one long block would. Not taken: exits that can be
    // linked (the dispatcher links them), RAM code, an L0 miss, and all
    // that drc_z80_run handles (interrupts, events, breaks, flushes).
    // (a guarded exit, linked already for another bank of its slot, that
    // found the slot on a different one: counts as one that can not be
    // linked. The dispatcher patched it to the current bank each time,
    // Aladdin's ~9 a frame: two banks taking turns)
    uint16_t link = drc_st.link;
    if (link && !drc_no_links) {
        const uint8_t __far *g = MK_FP(drc_st.cur_seg, link - 3);
        if (g[1] == 0x75 && g[0] != 0xFF)
            link = 0;
    }
    if (drc_st.cycles > 0 && !drc_st.reason && !link && !drc_no_fast && !drc_check
        && !z80_break && !drc_flush_request && !drc_st.ram_fail && !remapped
        && !z80.ei_pending && !z80.halted && !(z80.irq && z80.iff1)) {
        uint16_t pc = z80.pc;
        if (pc < 0x8000 || (pc < 0xC000 && !(drc_st.keys[0] & 0x08))) {
            uint8_t bank = bank_key(pc);
            const uint16_t *l0 = (const uint16_t*) IRAM(L0_ENTRY(pc));
            if (l0[0] == pc && (uint8_t) l0[1] == bank) {
                drc_entry[0] = l0[2];
                drc_entry[1] = l0[3];
                drc_stat_dispatch++;
                drc_stat_nolink++;
                drc_stat_quick++;
                return 1;
            }
        }
    }
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
#ifdef GGSWAN_HOST
    { extern void host_late_commit(void); host_late_commit(); }
#endif
    prof_phase = PROF_DISPATCH;
    z80.cycles += cycles;
    drc_st.nolink = drc_no_links;
    // IN from 00h and DCh in the generated code (z80_in's values)
    drc_st.keys_in[0] = ((gg.keys & GG_KEY_START) ? 0x00 : 0x80) | 0x40;
    drc_st.keys_in[1] = ~(gg.keys & 0x3F);
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
            if (z80.irq && z80.iff1) { drc_stat_irq++; drc_diag_irq_line = gg_current_line(); interp_one(); continue; }
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
                drc_diag_irq_line = gg_current_line();
                continue;
            }
            // interrupt: taken by the interpreter (or, just after EI,
            // the one instruction before it)
            if (!z80.ei_pending) { drc_stat_irq++; drc_diag_irq_line = gg_current_line(); }
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
        uint16_t *l0 = (uint16_t*) IRAM(L0_ENTRY(pc));
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
        // the line routine at the exits (LINE stub): left = left_base +
        // [cycles], as drc_cycles_left
        int32_t lb = z80.cycles - start;
        drc_st.left_base = (int16_t) lb;
        drc_st.fl_off = drc_no_fast || drc_check || gg_every_line || lb > 30000
            || z80_break || drc_flush_request;
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
        drc_map_sync();
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
