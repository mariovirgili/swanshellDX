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
// (the PC tests mirror it at HOST_ST_ADDR, see drc_rt.h)
_Static_assert(sizeof(drc_state_t) <= HOST_ST_END - HOST_ST_ADDR, "drc_state_t too big");
_Static_assert(sizeof(z80_t) <= HOST_VDP_ADDR - HOST_Z80_ADDR, "z80_t too big");
_Static_assert(sizeof(gg_vdp_t) <= HOST_ST_ADDR - HOST_VDP_ADDR, "gg_vdp_t too big");
#endif
uint16_t drc_entry[2];   // far pointer used by drc_enter: offset, segment

#define STUB_EXIT   0x0000
#define STUB_HELPER 0x0020
#define STUB_DYN    0x0040
#define STUB_CLS    0x0DA0   // a VRAM byte outside tile data (drc.c)
#define STUB_NTW    0x1010   // OUTI / OUTI to a name table entry (drc.c)
#define STUB_TROW   0x0E90   // OUTI x 3 / OUT (BEh),A: a tile row (drc.c)
#define STUB_CRAM   0x01E0   // a byte to CRAM
#define STUB_OUTI   0x0240   // (OUTD: + DRC_OUTD_DELTA)
#define STUB_LDI    0x0600   // (LDD: + DRC_LDD_DELTA)
#define STUB_HCALL  0x0730   // helpers called without leaving the block
#define STUB_LINE   0x0780   // the exits' common part, the line routine
#define STUB_CTL    0x0920   // OUT (BFh),A
#define STUB_MAP    0x0B80   // a bank change of slot 1 or 2 (put_map)
#define STUB_VCNT   0x0BF8   // IN A,(7Eh) (drc_emit_vcnt_stub)
#define STUB_PAIRS  0x0C48   // OUTI / OUTI / DEC L x 4 repeated (drc.c)
#define STUB_NT     0x0D20   // a name table byte (put_nt)
#define STUB_VWAIT  0x1110   // IN A,(7Eh) / CP n|AND A|OR A / JR|JP cc back (drc.c)
// (the VWAIT stub is 10Dh bytes since patch 253; only the host build checks
// that the stubs fit)
#define CACHE_START 0x1220
// translation buffer in free IRAM (between the font tiles and the mask tile)
#ifdef GGSWAN_SG1000
// sgswan: A600h-BEFFh are tiles there (main.c, sg_video.inc: the Game
// Gear's VRAM is not the WS's tiles, which leaves only what the screen
// needs of them): the buffer at C000h-D8FFh, the L0 cache after the flag
// tables, half the size (256 entries)
#define BUF_ADDR    0xC000
#define L0_ADDR     0xF200
#define L0_SIZE     0x0800
#else
#define BUF_ADDR    0xA600
/// DYN L0 cache: 512 direct-mapped entries of 8 bytes in free IRAM (in the
/// WS Color 4bpp mode C000-FDFF is free): Z80 pc (FFFF: empty), bank,
/// -, offset, segment of the block
#define L0_ADDR     0xD000
#define L0_SIZE     0x1000
#endif
/// block table key of the code in work RAM (banks are below 64)
#define RAM_KEY     0xFE
/// the L0 entry of Z80 address pc (the bank is in the entry): DYN and the
/// dispatcher index it the same way
#define L0_MASK     (L0_SIZE - 8)
#define L0_ENTRY(pc) (L0_ADDR + (((pc) << 3) & L0_MASK))
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
static uint16_t run_pc;   // pc when drc_z80_run last returned
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
        // (in pieces: see l0_clear; a long block or the routines of a code
        // bank are 4 KiB and more)
        while (len > 256) {
            _fmemcpy(dst, src, 256);
            dst += 256;
            src += 256;
            len -= 256;
        }
        _fmemcpy(dst, src, len);
    } else {
        for (uint16_t i = 0; i < len; i++)
            dst[i] = 0;
    }
    ws_ram_window_restore();   // back to Game Gear slot 2 (main.c)
}

/// (main.c, a state kept: bytes of the internal RAM to the PSRAM bank
/// `which` after the code cache's first; the RAM window is put back)
void drc_psram_put(uint8_t which, uint16_t off, const uint8_t *src, uint16_t len) {
    psram_write(which, off, src, len);
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
    if (!guard && target_seg == seg) {
        int16_t near8 = (int16_t) (target - link);
        const uint8_t __far *g = MK_FP(seg, link - 2);
        if (g[0] == 0x7E && near8 >= -128 && near8 <= 127) {
            uint8_t j4[4] = { 0x7F, (uint8_t) near8, 0xEB, 0x03 };
            outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
            outportw(WS_CART_EXTBANK_RAM_PORT, code_bank + ((seg - DRC_CODE_SEG) >> 12));
            uint8_t __far *dst = MK_FP(0x1000, link - 2);
            for (uint8_t i = 0; i < 4; i++)
                dst[i] = j4[i];
            ws_ram_window_restore();
#ifdef GGSWAN_HOST
            host_code_changed();
#endif
            return;
        }
    }
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

// a DYN exit with an inline cache (drc.c, exit_reg) whose link is at
// link: if it is one, write target into its compare and link it to
// target_seg:target (or, a target that can not be linked from its slot,
// FFFFh into the compare: DYN every time); returns 0 if it is not one
static uint8_t ic_link(uint16_t seg, uint16_t link, uint16_t pc, uint16_t target_seg,
                       uint16_t target, uint8_t bank) {
    const uint8_t __far *x = MK_FP(seg, link + 15);
    uint16_t ic = env.state_addr + offsetof(drc_state_t, ic);
    if (x[0] != 0xC6 || x[1] != 0x06 || x[2] != (uint8_t) ic || x[3] != (uint8_t) (ic >> 8))
        return 0;
    __attribute__((unused)) uint8_t slot = x[4] - 1;
#ifdef GGSWAN_SF7000
    uint8_t lform = 0;
    uint8_t ok = 1;
    (void) bank;
#else
    uint8_t lform = (slot == 0);
    uint8_t ok = lform ? (pc < 0x8000) : (pc < 0xC000 && ((pc >> 14) == 0 || (pc >> 14) == slot));
#endif
    uint16_t cmp_off = lform ? (uint16_t) (link - 21) : (uint16_t) (link - 14);
    const uint8_t __far *c = MK_FP(seg, cmp_off);
    uint16_t v = ok ? pc : 0xFFFF;
    uint8_t cmp[4] = { 0x81, (uint8_t) (0xF8 | (c[1] & 7)), (uint8_t) v, (uint8_t) (v >> 8) };
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, code_bank + ((seg - DRC_CODE_SEG) >> 12));
    uint8_t __far *dst = MK_FP(0x1000, cmp_off);
    for (uint8_t i = 0; i < 4; i++)
        dst[i] = cmp[i];
#ifndef GGSWAN_SF7000
    if (lform && ok && pc >= 0x4000) {
        // check slot 1's bank before following the link (D is at link + 23,
        // jne ends at link - 8 -> rel8 = 31)
        uint16_t k2 = env.state_addr + offsetof(drc_state_t, keys) + 2;
        uint8_t chk[7] = { 0x80, 0x3E, (uint8_t) k2, (uint8_t) (k2 >> 8), bank, 0x75, 31 };
        uint8_t __far *gd = MK_FP(0x1000, link - 15);
        for (uint8_t i = 0; i < 7; i++)
            gd[i] = chk[i];
    }
#endif
    ws_ram_window_restore();
#ifdef GGSWAN_HOST
    host_code_changed();
#endif
    if (ok)
        link_patch(seg, link, target_seg, target, 0, 0);
    return 1;
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
 * line interrupts on many lines: Super Monaco GP on every line, Sonic
 * Spinball every 8) does not go back to C:
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
    // (the interrupt's line: started + line_counter, where the counter
    // reaches zero; the lines before it start here too, nothing happens
    // on them: a wait loop ends its run early, lines before)
    PB(0x8B); PB(0x36); PW(V_(started));             // mov si, [started]
    PB(0x85); PB(0xF6);                              // test si, si
    BAIL(0x74);                                      // jz common (line 0)
    PB(0x8B); PB(0x3E); PW(V_(reg) + 10);            // mov di, [reg10]
    PB(0x81); PB(0xE7); PW(0x00FF);                  // and di, FFh (r)
    PB(0x81); PB(0xFF); PW(128);                     // cmp di, 128
    BAIL(0x73);                                      // jae common
    PB(0x01); PB(0xF7);                              // add di, si
    PB(0x47);                                        // inc di
    PB(0x50);                                        // push ax
    PB(0xA0); PW(V_(line_counter));                  // mov al, [line_counter]
    PB(0x30); PB(0xE4);                              // xor ah, ah
    PB(0x01); PB(0xC7);                              // add di, ax (e + r + 1)
    PB(0xB4); PB(GG_CYCLES_PER_LINE);                // mov ah, 228
    PB(0xF6); PB(0xE4);                              // mul ah (line_counter * 228)
    PB(0x03); PB(0x06); PW(V_(line_pos));            // add ax, [line_pos] (e * 228)
    PB(0x89); PB(0xC6);                              // mov si, ax
    PB(0x58);                                        // pop ax
#ifdef GGSWAN_SMS
    PB(0x3B); PB(0x3E); PW(V_(present_m1));          // cmp di, [present - 1] (191 or 223)
#else
    PB(0x81); PB(0xFF); PW(GG_PRESENT_LINE - 1);     // cmp di, 167
#endif
    BAIL(0x73);                                      // jae common (next not in the view)
    PB(0x83); PB(0x3E); PW(V_(frame_given) + 2); PB(0);   // cmp word [frame_given + 2], 0
    BAIL(0x75);                                      // jne common
    PB(0x3B); PB(0x36); PW(V_(frame_given));         // cmp si, [frame_given] (e * 228)
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
    PB(0xA0); PW(V_(line_counter));                  // mov al, [line_counter]
    PB(0x30); PB(0xE4);                              // xor ah, ah
    PB(0x03); PB(0x06); PW(V_(started));             // add ax, [started] (e)
    PB(0xA3); PW(V_(vcounter));                      // mov [vcounter], ax
    PB(0x40);                                        // inc ax
    PB(0xA3); PW(V_(started));                       // mov [started], ax
    PB(0xA1); PW(V_(frame_given));                   // mov ax, [frame_given] (e * 228)
    PB(0x05); PW(GG_CYCLES_PER_LINE);                // add ax, 228
    PB(0xA3); PW(V_(line_pos));                      // mov [line_pos], ax
    PB(0xA0); PW(V_(reg) + 10);                      // mov al, [reg10]
    PB(0xA2); PW(V_(line_counter));                  // mov [line_counter], al
    PB(0x58);                                        // pop ax
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
    // (the cycles run past the start of the line are not counted: left =
    // want, as gg_fast_line; see event_step in gg.c)
    PB(0x8B); PB(0x3E); PW(S_(left_base));           // mov di, [left_base]
    PB(0x03); PB(0x3E); PW(S_(cycles));              // add di, [cycles] (left, 0 or less)
    PB(0x29); PB(0x3E); PW(V_(irq_debt));            // sub [irq_debt], di
    PB(0x8B); PB(0x3E); PW(S_(left_base));           // mov di, [left_base]
    PB(0xF7); PB(0xDF);                              // neg di
    PB(0x01); PB(0xF7);                              // add di, si
    PB(0x89); PB(0x3E); PW(S_(cycles));              // mov [cycles], di (left = want)
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
#ifdef GGSWAN_SF7000
    // (sfswan: any address, one key)
    (void) keys; (void) miss;
    PB(0x30); PB(0xDB);                              // xor bl, bl
#else
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
#endif
    // L0 first: entry (pc << 3) & FF8h (L0_ENTRY)
    PB(0x89); PB(0xC6);                              // mov si, ax
    PB(0xC1); PB(0xE6); PB(0x03);                    // shl si, 3
    PB(0x81); PB(0xE6); PW(L0_MASK);                 // and si, FF8h
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
#ifdef GGSWAN_SF7000
    // (ES was the table's: the memory's again for the block)
    PB(0x8E); PB(0x06); PW(env.state_addr + offsetof(drc_state_t, segtab));   // mov es, [segtab]
#endif
    PB(0x5B); PB(0x58);                              // pop bx; pop ax
    PB(0xFF); PB(0x2E); PW(far_off);                 // jmp far [far_off]
#undef PB
#undef PW
#undef REL8
#undef PREL
#ifdef GGSWAN_HOST
    if (p > stub + STUB_CRAM) { printf("DYN stub too long\n"); exit(1); }
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
    uint16_t map2 = ws_map2_iram();
    uint16_t prio = env.state_addr + offsetof(drc_state_t, prio_on);
    PB(0x50); PB(0x53); PB(0x51); PB(0x52); PB(0x57);   // push ax bx cx dx di
    PB(0x89); PB(0xF3);                                  // mov bx, si
    PB(0x81); PB(0xE3); PW(0x3FFE);                      // and bx, 3FFEh
    PB(0x8B); PB(0x87); PW(env.vram_addr);               // mov ax, [bx + vram]
    PB(0x89); PB(0xC2);                                  // mov dx, ax
    PB(0x81); PB(0xE2); PW(0x01FF);                      // and dx, 1FFh
    // palette, h flip, v flip: bits 11, 9, 10 -> a table of 8
    PB(0x89); PB(0xC3);                                  // mov bx, ax
    PB(0xC1); PB(0xEB); PB(0x08);                        // shr bx, 8
    PB(0x88); PB(0xD9);                                  // mov cl, bl (bit 4: priority)
    PB(0x83); PB(0xE3); PB(0x0E);                        // and bx, 0Eh
    PB(0x0B); PB(0x97); PW(env.state_addr + offsetof(drc_state_t, nt_flags));   // or dx, [bx + nt_flags]
    // screen 2: the entry if it has priority, else the empty tile
    PB(0xF6); PB(0xC1); PB(0x10);                        // test cl, 10h
    PB(0xB9); PW(WS_PRIO_BLANK);                         // mov cx, blank
    PB(0x74); PB(0x02);                                  // jz +2
    PB(0x89); PB(0xD1);                                  // mov cx, dx
    PB(0x89); PB(0xF0);                                  // mov ax, si
    PB(0x2B); PB(0x06); PW(nt);                          // sub ax, [nt_base]
    PB(0x89); PB(0xC3);                                  // mov bx, ax
    PB(0x83); PB(0xE3); PB(0x3E);                        // and bx, 3Eh (column * 2)
    PB(0xC1); PB(0xE8); PB(0x05);                        // shr ax, 5
    PB(0x24); PB(0xFE);                                  // and al, FEh (row * 2)
    for (uint8_t i = 0; i < 2; i++) {
        PB(0x89); PB(0xC7);                              // mov di, ax
        PB(0x8B); PB(0xBD); PW(rows + i * 2 * DRC_NT_ROWS);   // mov di, [di + rows]
        PB(0x83); PB(0xFF); PB(0xFF);                    // cmp di, -1
        PB(0x74); PB(0x0F);                              // je +15 (none)
        PB(0x89); PB(0x91); PW(map1);                    // mov [bx + di + map1], dx
        PB(0x80); PB(0x3E); PW(prio); PB(0);             // cmp byte [prio_on], 0
        PB(0x74); PB(0x04);                              // je +4
        PB(0x89); PB(0x89); PW(map2);                    // mov [bx + di + map2], cx
    }
    PB(0x5F); PB(0x5A); PB(0x59); PB(0x5B); PB(0x58);   // pop di dx cx bx ax
    PB(0xC3);                                            // ret
#undef PB
#undef PW
    return (uint16_t) (p - stub);
}

/*
 * CRAM (at STUB_CRAM): a byte (in val) written to the VDP data port with
 * code 3, as vdp_write_data and gg_platform_cram_write: the even byte is
 * latched, the odd one writes the color (Game Gear ----BBBBGGGGRRRR) and
 * its WS copy (----RRRRGGGGBBBB) in drc_st.pal, which goes to the WS's
 * palettes with the frame (main.c: pal_present). Read buffer, address + 1,
 * latch. Uses SI.
 */
static uint16_t put_cram(uint8_t *stub, uint16_t at) {
    uint8_t *p = stub + at;
    uint16_t a;
#define PB(x) (*p++ = (x))
#define PW(x) (a = (x), *p++ = a, *p++ = a >> 8)
#define V_(f) ((uint16_t) (env.vdp_addr + offsetof(gg_vdp_t, f)))
#define S_(f) ((uint16_t) (env.state_addr + offsetof(drc_state_t, f)))
#ifndef GGSWAN_SG1000
// the WS color in AX to color BX / 2 of drc_st.pal
#define PAL_STORE() do { \
    PB(0x89); PB(0x87); PW(S_(pal));                     /* mov [bx + pal], ax */ \
    PB(0xC6); PB(0x06); PW(S_(pal_dirty)); PB(1);        /* mov byte [pal_dirty], 1 */ \
    } while (0)
#else
// (sgswan: the TMS9918A has no color RAM, the routine is never called)
#define PAL_STORE() do { \
    PB(0x89); PB(0x87); PW(0xFE00);                      /* mov [bx + FE00h], ax */ \
    PB(0x80); PB(0xFB); PB(0x20);                        /* cmp bl, 20h */ \
    PB(0x72); PB(0x04);                                  /* jb +4 */ \
    PB(0x89); PB(0x87); PW(0xFEE0);                      /* mov [bx + FEE0h], ax */ \
    } while (0)
#endif
    uint16_t val = env.state_addr + offsetof(drc_state_t, val);
    PB(0x50); PB(0x53);                                  // push ax; push bx
    PB(0xA0); PW(val);                                   // mov al, [val]
    PB(0xA2); PW(V_(read_buffer));                       // mov [read_buffer], al
    PB(0xC6); PB(0x06); PW(V_(latch_full)); PB(0);       // mov byte [latch_full], 0
    PB(0x8B); PB(0x1E); PW(V_(addr));                    // mov bx, [addr]
    PB(0x8D); PB(0x77); PB(0x01);                        // lea si, [bx + 1]
    PB(0x81); PB(0xE6); PW(0x3FFF);                      // and si, 3FFFh
    PB(0x89); PB(0x36); PW(V_(addr));                    // mov [addr], si
#ifdef GGSWAN_SMS
    // smsswan: one byte a color (--BBGGRR), its WS color from the table
    // of 64 at WS_CRAM_TABLE (main.c video_init)
    PB(0x83); PB(0xE3); PB(0x1F);                        // and bx, 1Fh
    PB(0x24); PB(0x3F);                                  // and al, 3Fh
    PB(0x88); PB(0x87); PW(V_(cram));                    // mov [bx + cram], al
    PB(0x30); PB(0xE4);                                  // xor ah, ah
    PB(0x89); PB(0xC6);                                  // mov si, ax
    PB(0x01); PB(0xF6);                                  // add si, si
    PB(0x8B); PB(0x84); PW(WS_CRAM_TABLE);               // mov ax, [si + table]
    PB(0x01); PB(0xDB);                                  // add bx, bx
    PAL_STORE();
    PB(0x5B); PB(0x58); PB(0xC3);                        // pop bx; pop ax; ret
#else
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
    PAL_STORE();
    PB(0x5B); PB(0x58); PB(0xC3);                        // pop bx; pop ax; ret
#endif
#undef PB
#undef PW
#undef V_
#undef S_
#undef PAL_STORE
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
static uint16_t put_map(uint8_t *stub, uint16_t at, uint16_t *map2, uint8_t cm) {
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
        if (cm) {
            PB(0xA2); PW(S_(map_val) + slot - 1);            // mov [map_val + slot - 1], al
        } else {
            PB(0xA2); PW((uint16_t) (0x1FFD + slot + 0xC000 + env.ram_delta));   // mirror
        }
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
    // (not with a Codemasters cartridge, whose banks are written at 0000h,
    // 4000h and 8000h: FFFEh and FFFFh are plain RAM there, and these
    // routines would map a bank for a byte stored in them)
    env.codemasters = gg.codemasters;
    if (!drc_no_fast && b && b <= 256 && !(b & (b - 1))
#ifdef GGSWAN_SG1000
        && 0   // (no mapper: FFFCh-FFFFh are RAM)
#endif
#ifdef GGSWAN_HOST
        && !getenv("GGSWAN_NOMAP")
#endif
        ) {
#ifdef GGSWAN_SMS
        if (gg.codemasters) {
            // rewrite STUB_MAP across the code banks so it saves into
            // drc_st.map_val instead of Sega's work RAM mirror (FFFE/FFFF)
            uint8_t *stub = (uint8_t*) IRAM(BUF_ADDR);
            uint16_t map_end = put_map(stub, STUB_MAP, &map2_at, 1);
            for (uint8_t i = 0; i < DRC_CODE_BANKS; i++)
                psram_write(i, STUB_MAP, stub + STUB_MAP, map_end - STUB_MAP);
        }
#endif
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
#ifdef GGSWAN_SMS
    if (gg.codemasters) {
        if (p & 4) z80_write(0x4000, drc_st.map_val[0]);
        if (p & 8) z80_write(0x8000, drc_st.map_val[1]);
        return;
    }
#endif
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

/*
 * The L0 cache emptied, in pieces. One REP STOSW over its 4 KiB is 6000
 * cycles, 24 WS lines, and Mednafen's V30MZ (nileswan-medem) runs a REP to
 * its end before it takes an interrupt (the real CPU takes it between two
 * turns): the line interrupt at the end of the WS screen came that late,
 * the new scroll written well into the next screen, its top with the old
 * one (Shinobi's fence, vertical bars, broken in two for a frame).
 */
static void l0_clear(void) {
    for (uint16_t a = 0; a < L0_SIZE; a += 256)
        memset(IRAM(L0_ADDR + a), 0xFF, 256);
}

#ifdef GGSWAN_SF7000
#include "sf7000.h"
/*
 * sfswan: where translated code came from (drc.c: FLAT). sf_mark is the
 * translator's, for every instruction of a block: a byte of the code map
 * for its 16 bytes (the internal RAM: translated code looks there before
 * it writes), a bit for each of its bytes (the tables' bank: the RAM
 * window moved there to set them, the ROM's first window to read them).
 * z80_write (gg.c) calls gg_platform_code_write for a write where the map
 * says code: if a byte of an instruction was written, all the blocks are
 * thrown away (when the block running ends: drc_helper), and both maps
 * with them.
 */
#define sf_cmap_ptr ((uint8_t*) IRAM(SF_CMAP_ADDR))
static void sf_mark(uint16_t pc, uint16_t len) {
    outportw(WS_CART_EXTBANK_RAM_PORT, SF_VRAM_BANK);
    {
        uint8_t __far *bits = MK_FP(0x1000, SF_CBITS_OFF);
        for (uint16_t a = pc; len--; a++) {
            sf_cmap_ptr[a >> 4] = 1;
            bits[a >> 3] |= (uint8_t) (1 << (a & 7));
        }
    }
    ws_ram_window_restore();
}

/*
 * A byte of a translated instruction written: the blocks go (the one
 * running stops there: drc_helper), and the byte is remembered for good,
 * a bit for each byte after the others' in the tables' bank and a byte
 * for each 256 in the internal RAM: an instruction with such a byte is
 * the interpreter's from now on (sf_smc, the translator's question; drc.c
 * says why).
 */
#define sf_smc_page ((uint8_t*) IRAM(SF_SMC_PAGE_ADDR))
uint16_t sf_smc_count;   // (diagnostics: how many bytes)
void gg_platform_code_write(uint16_t a) {
    const uint8_t __far *bits = MK_FP(0x2000, SF_CBITS_OFF);
    if (bits[a >> 3] & (1 << (a & 7))) {
        drc_flush_request = 1;
        outportw(WS_CART_EXTBANK_RAM_PORT, SF_VRAM_BANK);
        ((uint8_t __far*) MK_FP(0x1000, SF_SMCBITS_OFF))[a >> 3] |= (uint8_t) (1 << (a & 7));
        ws_ram_window_restore();
        sf_smc_page[a >> 8] = 1;
        sf_smc_count++;
#ifdef GGSWAN_HOST
        if (getenv("SF_SMC")) fprintf(stderr, "code written at %04X (pc %04X, block %04X)\n", a, z80.pc, drc_last_tr_pc);
#endif
    }
}

static uint8_t sf_smc(uint16_t pc, uint16_t len) {
    uint16_t end = pc + len - 1;
    if (!sf_smc_page[pc >> 8] && !sf_smc_page[end >> 8])
        return 0;
    const uint8_t __far *bits = MK_FP(0x2000, SF_SMCBITS_OFF);
    for (uint16_t a = pc; len--; a++)
        if (bits[a >> 3] & (1 << (a & 7)))
            return 1;
    return 0;
}

static void sf_maps_clear(void) {
    memset(sf_cmap_ptr, 0, 0x1000);
    outportw(WS_CART_EXTBANK_RAM_PORT, SF_VRAM_BANK);
    _fmemset(MK_FP(0x1000, SF_CBITS_OFF), 0, 0x2000);
    ws_ram_window_restore();
}
#endif

static void flush(void) {
    pending_link = 0;
    fast38_seg = 0;
#ifdef GGSWAN_SF7000
    sf_maps_clear();
#endif
    l0_clear();
    psram_write(DRC_CODE_BANKS, T_OFF, NULL, HASH_SIZE * 2);
    cache_bank = 0;
    cache_top = CACHE_START;
    drc_stat_flushes++;
}

// diagnostics: switch off parts of the block building (before the first
// translation)
static uint8_t known_start(uint16_t pc);

void drc_rt_options(uint8_t no_trace, uint8_t no_inline) {
    env.no_trace = no_trace;
    env.no_inline = no_inline;
}

// diagnostics/measurement: long blocks do not stop at blocks already
// translated (patch 101 off)
void drc_rt_known_starts(uint8_t on) {
    env.known_start = on ? known_start : 0;
}

#ifdef GGSWAN_SG1000
static uint16_t sg_at[4];   // (the offsets of drc_emit_sg_stubs' routines)
#endif
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
    env.known_start = known_start;
    drc_st.table_seg = DRC_TABLE_SEG;
    env.ram_delta = 0x8000 - 0xC000;   // Game Gear RAM at IRAM 8000h
#ifdef GGSWAN_SF7000
    // sfswan: the memory is one segment, the RAM window (drc.c: FLAT)
    env.flat = 1;
    env.ram_delta = 0;
    env.cmap = SF_CMAP_ADDR;
    env.mark = sf_mark;
    env.smc = sf_smc;
    env.pad_in = 0;   // (ports C0h-FFh: the keyboard, the disk: all by the helper)
    env.sf_vseg = 0x2000;
    env.sf_rbank = SF_RAM_BANK;
#ifdef GGSWAN_HOST
    env.sf_vbank = 0xFFFF;   // (the PC tools: no bank to move)
#else
    env.sf_vbank = SF_VRAM_BANK;
#endif
    sf_cmap = sf_cmap_ptr;
    // (nothing seen written yet)
    memset(sf_smc_page, 0, 256);
    outportw(WS_CART_EXTBANK_RAM_PORT, SF_VRAM_BANK);
    _fmemset(MK_FP(0x1000, SF_SMCBITS_OFF), 0, 0x2000);
    ws_ram_window_restore();
    sf_smc_count = 0;
#endif

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
#ifdef GGSWAN_SF7000
            {
                // sfswan: the helper found code written (drc_st.pad2): not
                // back into the block, which may be what was written, but
                // out by the exit, with the Z80's pc as the block left it
                // before the call (drc.c: flat_pc) or the step moved it
                uint16_t ab = env.state_addr + offsetof(drc_state_t, pad2);
                uint16_t lk = env.state_addr + offsetof(drc_state_t, link);
                *p++ = 0x80; *p++ = 0x3E; *p++ = ab; *p++ = ab >> 8; *p++ = 0;   // cmp byte [pad2], 0
                *p++ = 0x74; *p++ = 17;                                          // je ret
                *p++ = 0xC6; *p++ = 0x06; *p++ = ab; *p++ = ab >> 8; *p++ = 0;   // mov byte [pad2], 0
                *p++ = 0x83; *p++ = 0xC4; *p++ = 0x02;                           // add sp, 2
                *p++ = 0xC7; *p++ = 0x06; *p++ = lk; *p++ = lk >> 8; *p++ = 0; *p++ = 0;   // link = 0
                uint16_t rel = STUB_EXIT - (uint16_t) (p + 3 - stub);
                *p++ = 0xE9; *p++ = (uint8_t) rel; *p++ = (uint8_t) (rel >> 8);  // jmp EXIT
            }
#endif
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
#ifdef GGSWAN_HOST
        if (end > CACHE_START) { printf("NT stub: no room (%X)\n", end); exit(1); }
#endif
        uint16_t l6 = drc_emit_cls_stub(&env, stub + STUB_CLS, STUB_CLS);
#ifdef GGSWAN_HOST
        if (STUB_CTL + l5 > STUB_MAP || end > STUB_CLS || STUB_CLS + l6 > STUB_TROW) { printf("CLS stub: no room\n"); exit(1); }
        if (getenv("GGSWAN_STUBS")) printf("CLS stub %X bytes\n", l6);
#else
        (void) l6;
#endif
        uint16_t cram_end = put_cram(stub, STUB_CRAM);
        {
            uint16_t map_end = put_map(stub, STUB_MAP, &map2_at, 0);
#ifdef GGSWAN_HOST
            if (map_end > STUB_VCNT) { printf("MAP stub: no room (%X)\n", map_end); exit(1); }
            if (getenv("GGSWAN_STUBS")) printf("MAP stubs end %X\n", map_end);
#else
            (void) map_end;
#endif
            uint16_t l7 = drc_emit_vcnt_stub(&env, stub + STUB_VCNT, STUB_VCNT);
#ifdef GGSWAN_HOST
            if (STUB_VCNT + l7 > STUB_PAIRS) { printf("VCNT stub: no room (%X)\n", l7); exit(1); }
#else
            (void) l7;
#endif
            if (!drc_no_fast)
                env.vcnt_off = STUB_VCNT;
        }
        env.outi_off = STUB_OUTI;
        uint16_t outi1 = 0;
        uint16_t l1 = drc_emit_run_stub(&env, stub + STUB_OUTI, STUB_OUTI, 0, drc_no_fast ? 0 : &outi1);
        uint16_t l2 = drc_emit_run_stub(&env, stub + STUB_OUTI + DRC_OUTD_DELTA, STUB_OUTI + DRC_OUTD_DELTA, 1, 0);
        env.outi1_off = outi1;
        env.pairs_off = 0;
        env.ntw_off = 0;
        env.ntwa_off = 0;
        env.trow_off = 0;
        env.vwait_off = 0;
        if (!drc_no_fast) {
            uint16_t ntwa = 0;
            uint16_t l9 = drc_emit_ntw_stub(&env, stub + STUB_NTW, STUB_NTW, &ntwa);
#ifdef GGSWAN_HOST
            if (STUB_NTW + l9 > STUB_VWAIT) { printf("NTW stub: no room (%X)\n", l9); exit(1); }
            if (getenv("GGSWAN_STUBS")) printf("NTW stub %X bytes\n", l9);
#else
            (void) l9;
#endif
            env.ntw_off = STUB_NTW + DRC_NTW_ENTRY;
            env.ntwa_off = ntwa;
            env.trow_off = 0;
            if (env.cls_off) {
                uint16_t l10 = drc_emit_trow_stub(&env, stub + STUB_TROW, STUB_TROW);
#ifdef GGSWAN_HOST
                if (STUB_TROW + l10 > CACHE_START) { printf("TROW stub: no room (%X)\n", l10); exit(1); }
                if (getenv("GGSWAN_STUBS")) printf("TROW stub %X bytes\n", l10);
#else
                (void) l10;
#endif
                env.trow_off = STUB_TROW + DRC_TROW_ENTRY;
            }
            uint16_t l8 = drc_emit_pairs_stub(&env, stub + STUB_PAIRS, STUB_PAIRS);
#ifdef GGSWAN_HOST
            if (STUB_PAIRS + l8 > CACHE_START) { printf("PAIRS stub: no room (%X)\n", l8); exit(1); }
            if (getenv("GGSWAN_STUBS")) printf("PAIRS stub %X bytes\n", l8);
#else
            (void) l8;
#endif
            env.pairs_off = STUB_PAIRS + DRC_PAIRS_ENTRY;
            uint16_t l11 = drc_emit_vwait_stub(&env, stub + STUB_VWAIT, STUB_VWAIT);
#ifdef GGSWAN_HOST
            if (STUB_VWAIT + l11 > CACHE_START) { printf("VWAIT stub: no room (%X)\n", l11); exit(1); }
            if (getenv("GGSWAN_STUBS")) printf("VWAIT stub %X bytes\n", l11);
#else
            (void) l11;
#endif
            env.vwait_off = STUB_VWAIT;
        }
        env.ldi_off = STUB_LDI;
        uint16_t l3 = drc_emit_ldi_stub(&env, stub + STUB_LDI, STUB_LDI, 0);
        uint16_t l4 = drc_emit_ldi_stub(&env, stub + STUB_LDI + DRC_LDD_DELTA, STUB_LDI + DRC_LDD_DELTA, 1);
#ifdef GGSWAN_HOST
        if (getenv("GGSWAN_STUBS")) printf("stubs: NT end %X, CRAM end %X, OUTI %X, OUTD %X, LDI %X, LDD %X\n", end, cram_end, l1, l2, l3, l4);
        if (cram_end > STUB_OUTI || STUB_OUTI + l1 > STUB_LDI || STUB_OUTI + DRC_OUTD_DELTA < STUB_TROW + 0xE0
            || STUB_OUTI + DRC_OUTD_DELTA + l2 > STUB_NTW
            || l3 > DRC_LDD_DELTA || STUB_LDI + DRC_LDD_DELTA + l4 > STUB_HCALL) { printf("stubs too long\n"); exit(1); }
#else
        (void) end; (void) cram_end; (void) l1; (void) l2; (void) l3; (void) l4;
#endif
#ifdef GGSWAN_SG1000
        // sgswan: its own routines for the ports, where the Game Gear's
        // control port routine is (not used: see below)
        {
            env.sg_vdp = env.vdp_addr;
            uint16_t ls = drc_emit_sg_stubs(&env, stub + STUB_CTL, STUB_CTL, sg_at);
#ifdef GGSWAN_HOST
            if (STUB_CTL + ls > STUB_MAP) { printf("SG stubs: no room (%X)\n", ls); exit(1); }
#else
            (void) ls;
#endif
        }
#endif
        code_write(0, stub, CACHE_START);
    }
#ifdef GGSWAN_SG1000
    // sgswan: the routines above are for the Game Gear's VDP, whose VRAM
    // is the WS's tiles. The TMS9918A's is not (main.c, sg_video.inc):
    // its ports go to z80_out and z80_in, runs of OUTI to the interpreter
    env.vdp_addr = 0;
    env.ctl_off = 0;
    env.cram_off = 0;
    env.cls_off = 0;
    env.nt_stub_off = 0;
    env.vcnt_off = 0;
    env.vwait_off = 0;
    env.outi_off = 0;
    env.outi1_off = 0;
    env.pairs_off = 0;
    env.ntw_off = 0;
    env.ntwa_off = 0;
    env.trow_off = 0;
    // (GGSWAN_SG_NOSTUBS: a build for the tests, every port by the helper)
#ifndef GGSWAN_SG_NOSTUBS
    if (!drc_no_fast)
#else
    if (0)
#endif
    {
        env.sg_out_off = sg_at[0];
        env.sg_in_off = sg_at[1];
        env.sg_st_off = sg_at[2];
        env.sg_ctl_off = sg_at[3];
    }
#endif
    drc_st.segtab[6] = (uint16_t) (0x0800 - 0x0C00);
    drc_st.segtab[7] = (uint16_t) (0x0800 - 0x0E00);
#ifdef GGSWAN_SF7000
    for (uint8_t i = 0; i < 8; i++)
        drc_st.segtab[i] = 0x1000;
    memset(drc_st.keys, 0, sizeof(drc_st.keys));
#endif
    flush();
}

void drc_rt_set_no_links(uint8_t on) {
    drc_no_links = on;
    drc_rt_init(code_bank);   // (DYN without links)
}

static inline uint8_t bank_key(uint16_t pc) {
#ifdef GGSWAN_SF7000
    (void) pc;
    return 0;   // (one memory)
#else
    return drc_st.keys[1 + (pc >> 14)];
#endif
}

static inline uint16_t hash(uint16_t pc, uint8_t bank) {
    return (pc ^ (pc >> 9) ^ ((uint16_t) bank << 5)) & (HASH_SIZE - 1);
}

// (drc_env_t known_start: the translator asks while it decodes a long
// block, the same table lookup as drc_z80_run)
static uint8_t known_start(uint16_t pc) {
    uint8_t bank = bank_key(pc);
    uint16_t h = hash(pc, bank) & ~3;
    const uint16_t __far *tw = MK_FP(DRC_TABLE_SEG, 0);
    const uint8_t __far *tb = (const uint8_t __far*) tw;
    for (uint8_t i = 0; i < 4; i++, h++) {
        uint16_t off = tw[(T_OFF / 2) + h];
        if (off && off != DRC_NOT_TRANSLATED && tw[(T_PC / 2) + h] == pc && tb[T_BANK + h] == bank)
            return 1;
    }
    return 0;
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

#ifdef GGSWAN_SF7000
// sfswan: port A of the SF-7000's 8255 for translated code (drc.c, IN
// A,(E4h)): its value in drc_st.keys_in[1] while keys_in[0] says that
// the cycles the block has left can not change it. Worked out when a
// block is entered, and again after a helper that touched the 8255 or
// the controller (ports E0h-E7h) or ran instructions.
static void sf_port_a_plan(int16_t cycles) {
    uint8_t v = 0;
    // (a block ends at an exit: some more than what it was given)
    drc_st.keys_in[0] = sf_port_a_steady((uint32_t) (cycles > 0 ? cycles : 0) + 2000, &v);
    drc_st.keys_in[1] = v;
}
#endif

// The helper routine's C side: the registers are in z80
static void drc_helper_op(void);
#ifdef GGSWAN_SF7000
// (sfswan: code was written over, or the IPL came or went: the block
// running goes back to the runtime at its end, as after a mapper write)
#define SF_FLUSH drc_flush_request
#else
#define SF_FLUSH 0
#endif
uint8_t drc_flush_request;
volatile uint8_t prof_phase;
uint8_t drc_no_links;
uint8_t drc_no_fast;

uint32_t drc_stat_helper[5], drc_stat_dispatch, drc_stat_nolink;
uint32_t drc_stat_quick;   // (dispatches by drc_fast_reentry from L0)
uint16_t drc_stat_op[16];   // helper calls by DRC_H_* (diagnostics)
uint16_t drc_stat_op_lines[16];   // ...and the WS lines they took
static void drc_helper_body(void);

/*
 * A block IN A,(7Eh) / CP n / JR cc back to it (drc.c): a wait for a line.
 * Each turn takes 30 cycles (11 + 7 + 12) and ends the block's run when
 * the cycles left reach 0. The turns whose V counter would keep it going
 * are taken off here at once (as many as the cycles allow, as the block
 * would have run them): then the block reads the counter as usual. The V
 * counter as the routine computes it (drc_emit_vcnt_stub). arg: n, the
 * JR's condition (0 NZ, 1 Z, 2 NC, 3 C) << 8.
 */
static void vwait(uint16_t arg) {
    int16_t cyc = drc_st.cycles;
    if (remapped || drc_st.fl_off || cyc <= 0 || (gg.vdp.frame_given >> 16))
        return;   // (as the routine: the helper's way)
    uint8_t n = arg, cc = arg >> 8;
    uint16_t kmax = (uint16_t) (cyc - 1) / 30;   // turns that leave cycles > 0
    uint16_t fg = (uint16_t) gg.vdp.frame_given;
    uint16_t k = 0;
    for (;;) {
        int16_t di = drc_st.left_base + (int16_t) (cyc - 30 * k);
        uint16_t pos = di <= 0 ? fg - di : ((uint16_t) di > fg ? 0 : fg - di);
        uint16_t line = pos / GG_CYCLES_PER_LINE;
        if (line > GG_LINES - 1)
            line = GG_LINES - 1;
#ifdef GGSWAN_SMS
        uint8_t vc = line > gg.vdp.vcnt_max ? line - 6 : line;
#else
        uint8_t vc = line > 0xDA ? line - 6 : line;
#endif
        uint8_t again = cc == 0 ? vc != n : cc == 1 ? vc == n : cc == 2 ? vc >= n : vc < n;
        if (!again || (di > 0 && (uint16_t) di > fg))
            break;
        if (line >= GG_LINES - 1) {
            k = kmax;
            break;
        }
        // the first turn on the next line
        uint16_t kk = k + ((line + 1) * GG_CYCLES_PER_LINE - pos + 29) / 30;
        if (kk > kmax) {
            k = kmax;
            break;
        }
        k = kk;
    }
    drc_st.cycles = cyc - 30 * k;
}

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
    if (drc_st.op == DRC_H_VWAIT) {
        prof_phase = PROF_HELPER;
        vwait(drc_st.arg);
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
#ifdef GGSWAN_SF7000
        if ((uint8_t) drc_st.arg >= 0xE0)
            sf_port_a_plan(remapped ? 0 : drc_st.cycles);
#endif
        if (z80_break || drc_flush_request)
            drc_st.fl_off = 1;
        if (!remapped && (z80_break || SF_FLUSH)) {
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
#ifdef GGSWAN_SF7000
    // (code written over by this write or step: the block stops here)
    if (drc_flush_request && (drc_st.op == DRC_H_WRITE || drc_st.op == DRC_H_STEP))
        drc_st.pad2 = 1;
    if (drc_st.op == DRC_H_STEP || drc_st.op == DRC_H_OUTI_RUN || drc_st.op == DRC_H_OUTD_RUN)
        sf_port_a_plan(remapped ? 0 : drc_st.cycles);
#endif
    if (!remapped && (z80_break || SF_FLUSH || m1 != gg.mapper[1]
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

/* === Benchmark (diagnostics): IRAM against the PSRAM code cache === */

#ifndef GGSWAN_HOST

// the bodies: a mix like translated code (add bx,dx; mov al,[bx];
// mov [E7F0h],al; sub word [E7F2h],5; jle $+2; mov ah,al), a chain of
// short jumps, a chain of near jumps
static const uint8_t bench_mix[] = { 0x01, 0xD3, 0x8A, 0x07, 0xA2, 0xF0, 0xE7,
    0x83, 0x2E, 0xF2, 0xE7, 0x05, 0x7E, 0x00, 0x88, 0xC4 };
static const uint8_t bench_jmp8[] = { 0xEB, 0x00 };
static const uint8_t bench_jmp16[] = { 0xE9, 0x00, 0x00 };

#define BENCH_IRAM 0xE400u    // (free IRAM: E400h-E7FFh)
#define BENCH_OFF 0xF000u     // in code bank 0

static uint16_t bench_build(uint8_t *b, const uint8_t *body, uint8_t len, uint8_t copies, uint16_t n) {
    uint8_t *p = b;
    *p++ = 0x50; *p++ = 0x53; *p++ = 0x51; *p++ = 0x52;   // push ax, bx, cx, dx
    *p++ = 0xB9; *p++ = n; *p++ = n >> 8;                 // mov cx, n
    uint8_t *loop = p;
    for (uint8_t i = 0; i < copies; i++)
        for (uint8_t j = 0; j < len; j++)
            *p++ = body[j];
    *p++ = 0x49;                                          // dec cx
    *p++ = 0x74; *p++ = 3;                                // jz done
    uint16_t rel = (uint16_t) (loop - (p + 3));
    *p++ = 0xE9; *p++ = rel; *p++ = rel >> 8;             // jmp loop
    *p++ = 0x5A; *p++ = 0x59; *p++ = 0x5B; *p++ = 0x58;   // done: pop...
    *p++ = 0xCB;                                          // retf
    return (uint16_t) (p - b);
}

static uint16_t bench_time(uint16_t seg, uint16_t off) {
    uint16_t fp[2] = { off, seg };
    ia16_disable_irq();
    outportb(WS_TIMER_CTRL_PORT, WS_TIMER_CTRL_HBL_DISABLE);
    outportw(WS_TIMER_HBL_RELOAD_PORT, 0xFFFF);   // (also sets the counter)
    outportb(WS_TIMER_CTRL_PORT, WS_TIMER_CTRL_HBL_ONESHOT);
    __asm__ volatile ("lcall *%0" : : "m" (fp) : "ax", "bx", "cx", "dx", "memory");
    uint16_t left = inportw(WS_TIMER_HBL_COUNTER_PORT);
    outportb(WS_TIMER_CTRL_PORT, WS_TIMER_CTRL_HBL_DISABLE);
    ia16_enable_irq();
    return 0xFFFF - left;
}

/// Run the same code from IRAM and from the PSRAM code cache: out[2 * i]
/// and out[2 * i + 1] are the lines (256 cycles each) for test i (0: mix
/// like translated code, 1: short jumps, 2: near jumps). Call before
/// translating anything (it uses code bank 0 at F000h).
void drc_rt_bench(uint16_t *out) {
    static uint8_t b[300];
    static const struct { const uint8_t *body; uint8_t len, copies; uint16_t n; } t[3] = {
        { bench_mix, sizeof(bench_mix), 16, 8000 },
        { bench_jmp8, sizeof(bench_jmp8), 64, 8000 },
        { bench_jmp16, sizeof(bench_jmp16), 64, 8000 },
    };
    for (uint8_t i = 0; i < 3; i++) {
        uint16_t len = bench_build(b, t[i].body, t[i].len, t[i].copies, t[i].n);
        memcpy((uint8_t*) BENCH_IRAM, b, len);
        psram_write(0, BENCH_OFF, b, len);
        out[2 * i] = bench_time(0x0000, BENCH_IRAM);
        out[2 * i + 1] = bench_time(DRC_CODE_SEG, BENCH_OFF);
    }
    // (also at IRAM E7E0h, after the marker 'BENC', for a debugger)
    uint16_t *r = (uint16_t*) 0xE7E0;
    r[0] = 0x4542; r[1] = 0x434E;
    for (uint8_t i = 0; i < 6; i++)
        r[2 + i] = out[i];
}
#endif

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
/// The stack where the translated code reaches it in place (drc.c,
/// sp_loaded_exit): anywhere else, the interpreter
#ifdef GGSWAN_SF7000
// (sfswan: the stack is reached wherever it is, and no look at the code
// map for what goes on it)
#define SP_OK(sp) 1
#define STACK_OK(sp) 1
#define PUSH_PC(sp) do { sf_ram[sp] = (uint8_t) z80.pc; sf_ram[(uint16_t) ((sp) + 1)] = z80.pc >> 8; } while (0)
#else
#define SP_OK(sp) ((uint16_t) ((sp) - 0xC002u) <= 0x1FFEu)
#define STACK_OK(sp) ((sp) >= 0xC000 && (sp) < 0xDFFF)
#define PUSH_PC(sp) do { gg_ram[(sp) & 0x1FFF] = (uint8_t) z80.pc; gg_ram[((sp) + 1) & 0x1FFF] = z80.pc >> 8; } while (0)
#endif

#ifndef GGSWAN_HOST
_Static_assert(HASH_SIZE == 8192 && T_PC == 0 && T_OFF == 0x4000
               && T_BANK == 0x8000 && T_CODE == 0xA000, "G3 asm: table layout");
_Static_assert(DRC_TABLE_SEG == 0xA000 && DRC_CODE_SEG == 0x4000, "G3 asm: segments");
_Static_assert(RAM_KEY == 0xFE && DRC_NOT_TRANSLATED == 1, "G3 asm: keys");
_Static_assert(PROF_CODE == 1 && PROF_DISPATCH == 2, "G3 asm: prof_phase");
#ifdef GGSWAN_SG1000
_Static_assert(L0_ADDR == 0xF200 && L0_MASK == 0x07F8, "G3 asm: L0 constants");
#define DRC_L0_ADDR_S "0xF200"
#define DRC_L0_P2_S   "0xF202"
#define DRC_L0_P3_S   "0xF203"
#define DRC_L0_P4_S   "0xF204"
#define DRC_L0_P6_S   "0xF206"
#define DRC_L0_MASK_S "$0x07F8"
#else
_Static_assert(L0_ADDR == 0xD000 && L0_MASK == 0x0FF8, "G3 asm: L0 constants");
#define DRC_L0_ADDR_S "0xD000"
#define DRC_L0_P2_S   "0xD002"
#define DRC_L0_P3_S   "0xD003"
#define DRC_L0_P4_S   "0xD004"
#define DRC_L0_P6_S   "0xD006"
#define DRC_L0_MASK_S "$0x0FF8"
#endif

// V30MZ fast path for drc_fast_reentry() when drc_st.cycles > 0.
// All variables addressed via absolute symbols ("i" / %c) and all modified
// registers listed in clobbers (no hidden base-register aliasing).
// Returns 1 on L0 hit (drc_entry[0..1] filled), 0 on miss/exit, 2 if map_pend != 0.
static __attribute__((noinline)) uint8_t drc_fast_reentry_l0_asm(void) {
    uint8_t ret;
    __asm__ volatile (
        "movb %c[reason], %%al\n\t"
        "orb %c[no_fast], %%al\n\t"
        "orb %c[chk], %%al\n\t"
        "orb %c[brk], %%al\n\t"
        "orb %c[flreq], %%al\n\t"
        "orb %c[ram_fail], %%al\n\t"
        "orb %c[rem], %%al\n\t"
        "orb %c[z_eip], %%al\n\t"
        "orb %c[z_hlt], %%al\n\t"
        "jnz 8f\n\t"
        "cmpb $0, %c[mpend]\n\t"
        "jne 7f\n\t"
        "cmpb $0, %c[z_irq]\n\t"
        "jnz 10f\n\t"
    "1:\n\t"
#ifndef GGSWAN_SF7000
        "movw %c[z_sp], %%ax\n\t"
        "subw $0xC002, %%ax\n\t"
        "cmpw $0x1FFE, %%ax\n\t"
        "ja 8f\n\t"
#endif
        "movw %c[link], %%bx\n\t"
        "testw %%bx, %%bx\n\t"
        "jnz 11f\n\t"
    "3:\n\t"
        "movw %c[z_pc], %%ax\n\t"
#ifdef GGSWAN_SF7000
        "xorb %%dl, %%dl\n\t"
#else
        "cmpw $0x8000, %%ax\n\t"
        "jae 12f\n\t"
    "4:\n\t"
        "movw %%ax, %%bx\n\t"
        "rolw $2, %%bx\n\t"
        "andw $3, %%bx\n\t"
        "movb %c[keys]+1(%%bx), %%dl\n\t"
#endif
        "movw %%ax, %%bx\n\t"
        "shlw $3, %%bx\n\t"
        "andw " DRC_L0_MASK_S ", %%bx\n\t"
        "cmpw %%ax, " DRC_L0_ADDR_S "(%%bx)\n\t"
        "jne 8f\n\t"
        "cmpb %%dl, " DRC_L0_P2_S "(%%bx)\n\t"
        "jne 8f\n\t"
        "movw " DRC_L0_P4_S "(%%bx), %%ax\n\t"
        "movw %%ax, %c[ent]\n\t"
        "movw " DRC_L0_P6_S "(%%bx), %%ax\n\t"
        "movw %%ax, %c[ent]+2\n\t"
        "movb $1, %%al\n\t"
        "jmp 9f\n\t"

    "8:\n\t"
        "xorb %%al, %%al\n\t"
        "jmp 9f\n\t"
    "7:\n\t"
        "movb $2, %%al\n\t"
        "jmp 9f\n\t"

    "10:\n\t"
        "cmpb $0, %c[z_iff1]\n\t"
        "jnz 8b\n\t"
        "jmp 1b\n\t"
    "11:\n\t"
        "cmpb $0, %c[no_links]\n\t"
        "jnz 8b\n\t"
        "pushw %%es\n\t"
        "movw %c[cseg], %%ax\n\t"
        "movw %%ax, %%es\n\t"
        "cmpb $0x75, %%es:-2(%%bx)\n\t"
        "jne 13f\n\t"
        "cmpb $0xFF, %%es:-3(%%bx)\n\t"
        "je 13f\n\t"
        "popw %%es\n\t"
        "jmp 3b\n\t"
    "13:\n\t"
        "popw %%es\n\t"
        "jmp 8b\n\t"
#ifndef GGSWAN_SF7000
    "12:\n\t"
        "cmpw $0xC000, %%ax\n\t"
        "jae 8b\n\t"
        "testb $0x08, %c[keys]\n\t"
        "jnz 8b\n\t"
        "jmp 4b\n\t"
#endif
    "9:\n\t"
        : [ret] "=a" (ret)
        : [reason] "i" (&drc_st.reason),
          [no_fast] "i" (&drc_no_fast),
          [chk] "i" (&drc_check),
          [brk] "i" (&z80_break),
          [flreq] "i" (&drc_flush_request),
          [ram_fail] "i" (&drc_st.ram_fail),
          [rem] "i" (&remapped),
          [z_eip] "i" (&z80.ei_pending),
          [z_hlt] "i" (&z80.halted),
          [mpend] "i" (&drc_st.map_pend),
          [z_irq] "i" (&z80.irq),
          [z_iff1] "i" (&z80.iff1),
          [link] "i" (&drc_st.link),
          [no_links] "i" (&drc_no_links),
          [cseg] "i" (&drc_st.cur_seg),
          [z_pc] "i" (&z80.pc),
#ifndef GGSWAN_SF7000
          [z_sp] "i" (&z80.sp),
          [keys] "i" (drc_st.keys),
#endif
          [ent] "i" (drc_entry)
        : "bx", "dx", "cc", "memory"
    );
    return ret;
}

// V30MZ fast lookup for drc_z80_run() when z80.cycles > 0 && !z80_break.
// Checks rare states, pending_link (already linked = no-op), L0 hit, then 4-way
// PSRAM table scan. Updates drc_entry[1], L0, fast38, clears ei_pending and
// pending_link. Returns block offset (>= CACHE_START) or 0 to fall back to C.
static __attribute__((noinline)) uint16_t drc_lookup_asm(void) {
    uint16_t ret;
    __asm__ volatile (
        "movb %c[flreq], %%al\n\t"
        "orb %c[z_hlt], %%al\n\t"
        "orb %c[chk], %%al\n\t"
#ifdef GGSWAN_SF7000
        "orb %c[sf_rom], %%al\n\t"
#endif
        "jnz 8f\n\t"
        "cmpb $0, %c[z_irq]\n\t"
        "jnz 9f\n\t"
    "1:\n\t"
#ifndef GGSWAN_SF7000
        "movw %c[z_sp], %%ax\n\t"
        "subw $0xC002, %%ax\n\t"
        "cmpw $0x1FFE, %%ax\n\t"
        "ja 8f\n\t"
#endif
        "movw %c[plink], %%bx\n\t"
        "testw %%bx, %%bx\n\t"
        "jnz 10f\n\t"
    "2:\n\t"
        "movw %c[z_pc], %%ax\n\t"
#ifdef GGSWAN_SF7000
        "xorb %%dl, %%dl\n\t"
#else
        "cmpw $0x8000, %%ax\n\t"
        "jae 19f\n\t"
    "3:\n\t"
        "movw %%ax, %%bx\n\t"
        "rolw $2, %%bx\n\t"
        "andw $3, %%bx\n\t"
        "movb %c[keys]+1(%%bx), %%dl\n\t"
    "4:\n\t"
#endif
        "movw %%ax, %%bx\n\t"
        "shlw $3, %%bx\n\t"
        "andw " DRC_L0_MASK_S ", %%bx\n\t"
        "cmpw %%ax, " DRC_L0_ADDR_S "(%%bx)\n\t"
        "jne 29f\n\t"
        "cmpb %%dl, " DRC_L0_P2_S "(%%bx)\n\t"
        "jne 29f\n\t"
        "movw " DRC_L0_P4_S "(%%bx), %%si\n\t"
        "movw " DRC_L0_P6_S "(%%bx), %%cx\n\t"
        "movw %%cx, %c[ent]+2\n\t"
    "5:\n\t"
        "movb $0, %c[z_eip]\n\t"
        "movw $0, %c[plink]\n\t"
        "cmpw $0x0038, %%ax\n\t"
        "je 25f\n\t"
    "6:\n\t"
        "movw %%si, %%ax\n\t"
        "jmp 99f\n\t"

    "8:\n\t"
        "xorw %%ax, %%ax\n\t"
        "jmp 99f\n\t"
    "29:\n\t"
        "jmp 20f\n\t"
#ifndef GGSWAN_SF7000
    "19:\n\t"
        "jmp 12f\n\t"
#endif
    "9:\n\t"
        "cmpb $0, %c[z_iff1]\n\t"
        "jnz 8b\n\t"
        "jmp 1b\n\t"
    "10:\n\t"
        "cmpb $0, %c[no_links]\n\t"
        "jz 14f\n\t"
        "jmp 2b\n\t"
    "14:\n\t"
        "pushw %%es\n\t"
        "movw %c[pseg], %%ax\n\t"
        "movw %%ax, %%es\n\t"
        "movb %%es:-2(%%bx), %%al\n\t"
        "cmpb $0x75, %%al\n\t"
        "je 11f\n\t"
        "cmpb $0x7E, %%al\n\t"
        "jne 15f\n\t"
        "cmpb $0xEB, %%es:(%%bx)\n\t"
        "je 11f\n\t"
    "15:\n\t"
        "popw %%es\n\t"
        "jmp 2b\n\t"
    "11:\n\t"
        "popw %%es\n\t"
        "jmp 8b\n\t"
    "25:\n\t"
        "movw %%si, %c[f38]\n\t"
        "movw %%cx, %c[f38]+2\n\t"
        "movb %%dl, %c[f38_b]\n\t"
        "jmp 6b\n\t"
#ifndef GGSWAN_SF7000
    "12:\n\t"
        "cmpw $0xC000, %%ax\n\t"
        "jae 13f\n\t"
        "testb $0x08, %c[keys]\n\t"
        "jnz 8b\n\t"
        "jmp 3b\n\t"
    "13:\n\t"
        "cmpw $0xDFC0, %%ax\n\t"
        "jae 8b\n\t"
        "movb $0xFE, %%dl\n\t"
        "jmp 4b\n\t"
#endif
    "20:\n\t"                                  /* L0 miss: 4-way PSRAM scan (AX=pc, DL=bank) */
        "movb %%dl, %%bl\n\t"
        "xorb %%bh, %%bh\n\t"
        "shlw $5, %%bx\n\t"
        "xorw %%ax, %%bx\n\t"
        "movw %%ax, %%si\n\t"
        "shrw $9, %%si\n\t"
        "xorw %%si, %%bx\n\t"
        "andw $0x1FFC, %%bx\n\t"
        "pushw %%es\n\t"
        "movw $0xA000, %%di\n\t"
        "movw %%di, %%es\n\t"
        "movw %%bx, %%si\n\t"
        "shlw $1, %%si\n\t"
        "movw $4, %%cx\n\t"
    "21:\n\t"
        "movw %%es:0x4000(%%si), %%di\n\t"
        "testw %%di, %%di\n\t"
        "jz 22f\n\t"
        "cmpw %%ax, %%es:(%%si)\n\t"
        "jne 22f\n\t"
        "cmpb %%dl, %%es:0x8000(%%bx)\n\t"
        "je 23f\n\t"
    "22:\n\t"
        "incw %%bx\n\t"
        "addw $2, %%si\n\t"
        "loop 21b\n\t"
        "popw %%es\n\t"
        "jmp 8b\n\t"
    "23:\n\t"
        "cmpw $1, %%di\n\t"
        "jne 24f\n\t"
        "popw %%es\n\t"
        "jmp 8b\n\t"
    "24:\n\t"
        "movb %%es:0xA000(%%bx), %%ch\n\t"
        "popw %%es\n\t"
        "shlb $4, %%ch\n\t"
        "addb $0x40, %%ch\n\t"
        "xorb %%cl, %%cl\n\t"
        "movw %%cx, %c[ent]+2\n\t"
        "movw %%ax, %%bx\n\t"
        "shlw $3, %%bx\n\t"
        "andw " DRC_L0_MASK_S ", %%bx\n\t"
        "movw %%ax, " DRC_L0_ADDR_S "(%%bx)\n\t"
        "movb %%dl, " DRC_L0_P2_S "(%%bx)\n\t"
        "movb $0, " DRC_L0_P3_S "(%%bx)\n\t"
        "movw %%di, " DRC_L0_P4_S "(%%bx)\n\t"
        "movw %%cx, " DRC_L0_P6_S "(%%bx)\n\t"
        "movw %%di, %%si\n\t"
        "jmp 5b\n\t"
    "99:\n\t"
        : [ret] "=a" (ret)
        : [flreq] "i" (&drc_flush_request),
          [z_hlt] "i" (&z80.halted),
          [chk] "i" (&drc_check),
          [z_irq] "i" (&z80.irq),
          [z_iff1] "i" (&z80.iff1),
          [z_pc] "i" (&z80.pc),
          [plink] "i" (&pending_link),
          [pseg] "i" (&pending_seg),
          [no_links] "i" (&drc_no_links),
          [z_eip] "i" (&z80.ei_pending),
          [ent] "i" (drc_entry),
          [f38] "i" (drc_st.fast38),
          [f38_b] "i" (&fast38_bank)
#ifdef GGSWAN_SF7000
          , [sf_rom] "i" (&sf.rom_on)
#else
          , [z_sp] "i" (&z80.sp),
          [keys] "i" (drc_st.keys)
#endif
        : "bx", "cx", "dx", "si", "di", "cc", "memory"
    );
    return ret;
}

// V30MZ pre-block cycle/state setup when z80.cycles > 0 and drc_check/z80_break/flush are 0.
static inline __attribute__((always_inline)) void drc_enter_setup_asm(void) {
    __asm__ volatile (
        "movw %c[z_cyc], %%ax\n\t"
        "movw %c[z_cyc]+2, %%dx\n\t"
        "testw %%dx, %%dx\n\t"
        "jnz 1f\n\t"
        "cmpw $30000, %%ax\n\t"
        "ja 1f\n\t"
        "movw %%ax, %c[st_cyc]\n\t"
        "movw %%ax, %c[bstart]\n\t"
        "movw $0, %c[lbase]\n\t"
#ifdef GGSWAN_SG1000
        "movb $1, %c[fl_off]\n\t"
#else
        "movb %c[no_fast], %%al\n\t"
        "orb %c[ev_line], %%al\n\t"
        "negb %%al\n\t"
        "sbbb %%al, %%al\n\t"
        "negb %%al\n\t"
        "movb %%al, %c[fl_off]\n\t"
#endif
        "jmp 2f\n\t"
    "1:\n\t"
        "movw $30000, %c[st_cyc]\n\t"
        "movw $30000, %c[bstart]\n\t"
        "subw $30000, %%ax\n\t"
        "sbbw $0, %%dx\n\t"
        "movw %%ax, %c[lbase]\n\t"
#ifdef GGSWAN_SG1000
        "movb $1, %c[fl_off]\n\t"
#else
        "testw %%dx, %%dx\n\t"
        "jnz 3f\n\t"
        "cmpw $30000, %%ax\n\t"
        "ja 3f\n\t"
        "movb %c[no_fast], %%al\n\t"
        "orb %c[ev_line], %%al\n\t"
        "negb %%al\n\t"
        "sbbb %%al, %%al\n\t"
        "negb %%al\n\t"
        "movb %%al, %c[fl_off]\n\t"
        "jmp 2f\n\t"
    "3:\n\t"
        "movb $1, %c[fl_off]\n\t"
#endif
    "2:\n\t"
        "movb $1, %c[in_blk]\n\t"
        "movb $1, %c[prof]\n\t"
        :
        : [z_cyc] "i" (&z80.cycles),
          [st_cyc] "i" (&drc_st.cycles),
          [bstart] "i" (&block_start),
          [lbase] "i" (&drc_st.left_base),
          [fl_off] "i" (&drc_st.fl_off),
          [in_blk] "i" (&in_block),
          [prof] "i" (&prof_phase)
#ifndef GGSWAN_SG1000
          , [no_fast] "i" (&drc_no_fast),
          [ev_line] "i" (&gg_every_line)
#endif
        : "ax", "dx", "cc", "memory"
    );
}

// V30MZ fast post-block accounting after drc_enter().
// Returns 1 if handled (common case), 0 to fall back to C slow path.
static __attribute__((noinline)) uint8_t drc_dispatch_post_asm(void) {
    uint8_t ret;
    __asm__ volatile (
        "movb %c[reason], %%al\n\t"
        "orb %c[mpend], %%al\n\t"
        "orb %c[ram_fail], %%al\n\t"
        "orb %c[rem], %%al\n\t"
        "orb %c[chk], %%al\n\t"
        "jnz 8f\n\t"
        "movw %c[cseg], %%dx\n\t"
        "movw %%dx, %c[ent]+2\n\t"
        "movb $2, %c[prof]\n\t"
        "movb $0, %c[in_blk]\n\t"
        "movw %c[link], %%ax\n\t"
        "testw %%ax, %%ax\n\t"
        "jz 1f\n\t"
        "movw %%ax, %c[plink]\n\t"
        "movw %%dx, %c[pseg]\n\t"
    "2:\n\t"
        "addw $1, %c[s_disp]\n\t"
        "adcw $0, %c[s_disp]+2\n\t"
        "movw %c[bstart], %%ax\n\t"
        "subw %c[st_cyc], %%ax\n\t"
        "cwd\n\t"
        "subw %%ax, %c[z_cyc]\n\t"
        "sbbw %%dx, %c[z_cyc]+2\n\t"
        "movb $1, %%al\n\t"
        "jmp 9f\n\t"
    "1:\n\t"
        "addw $1, %c[s_nolk]\n\t"
        "adcw $0, %c[s_nolk]+2\n\t"
        "jmp 2b\n\t"
    "8:\n\t"
        "xorb %%al, %%al\n\t"
    "9:\n\t"
        : [ret] "=a" (ret)
        : [reason] "i" (&drc_st.reason),
          [mpend] "i" (&drc_st.map_pend),
          [ram_fail] "i" (&drc_st.ram_fail),
          [rem] "i" (&remapped),
          [chk] "i" (&drc_check),
          [cseg] "i" (&drc_st.cur_seg),
          [link] "i" (&drc_st.link),
          [bstart] "i" (&block_start),
          [st_cyc] "i" (&drc_st.cycles),
          [ent] "i" (drc_entry),
          [prof] "i" (&prof_phase),
          [in_blk] "i" (&in_block),
          [plink] "i" (&pending_link),
          [pseg] "i" (&pending_seg),
          [s_nolk] "i" (&drc_stat_nolink),
          [s_disp] "i" (&drc_stat_dispatch),
          [z_cyc] "i" (&z80.cycles)
        : "dx", "cc", "memory"
    );
    return ret;
}

static __attribute__((noinline)) uint8_t drc_fast_reentry_slow(void) {
#else
uint8_t drc_fast_reentry(void) {
#endif
#ifdef GGSWAN_SF7000
    if (sf.rom_on)
        return 0;   // (the IPL: the interpreter's)
#endif
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
        && !z80.ei_pending && !z80.halted && !(z80.irq && z80.iff1) && SP_OK(z80.sp)) {
        uint16_t pc = z80.pc;
#ifdef GGSWAN_SF7000
        {
#else
        if (pc < 0x8000 || (pc < 0xC000 && !(drc_st.keys[0] & 0x08))) {
#endif
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
        || z80.ei_pending || z80.halted || !SP_OK(z80.sp))
        return 0;
    // an interrupt accepted at once: IM 1 with the stack in work RAM only
    uint16_t sp = z80.sp - 2;
    uint8_t take = z80.iff1;
    if (take && (z80.im == 2 || !STACK_OK(sp)))
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
#ifdef GGSWAN_SF7000
    sf_port_a_plan(drc_st.cycles);
#endif
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
        PUSH_PC(sp);
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

#ifndef GGSWAN_HOST
uint8_t drc_fast_reentry(void) {
#ifdef GGSWAN_SF7000
    if (sf.rom_on)
        return 0;
#endif
    if (drc_st.cycles <= 0) {
#ifndef GGSWAN_SG1000
        // When !fl_off, STUB_LINE already ran the exact same fast-line check and bailed.
        if (drc_st.fl_off && !drc_no_fast && !drc_st.reason && !drc_check
            && !z80_break && !drc_flush_request && !drc_st.ram_fail && !remapped)
            return drc_fast_reentry_slow();
#endif
        return 0;
    }
    uint8_t r = drc_fast_reentry_l0_asm();
    if (r == 1) {
        drc_stat_dispatch++;
        drc_stat_nolink++;
        drc_stat_quick++;
        return 1;
    }
    return r == 2 ? drc_fast_reentry_slow() : 0;
}
#endif

int32_t drc_z80_run(int32_t cycles) {
#ifdef GGSWAN_HOST
    { extern void host_late_commit(void); host_late_commit(); }
#endif
    prof_phase = PROF_DISPATCH;
    z80.cycles += cycles;
    // (pc changed outside: smsswan's Pause, a non-maskable interrupt
    // between two frames; the last block's exit must not be linked to it)
    if (z80.pc != run_pc)
        pending_link = 0;
    drc_st.nolink = drc_no_links;
    // IN from 00h and DCh in the generated code (z80_in's values)
    drc_st.keys_in[0] = ((gg.keys & GG_KEY_START) ? 0x00 : 0x80) | 0x40;
    drc_st.keys_in[1] = ~(gg.keys & 0x3F);
    const uint16_t __far *const tw = MK_FP(DRC_TABLE_SEG, 0);
    const uint8_t __far *const tb = (const uint8_t __far*) tw;
    for (;;) {
        uint16_t pc = 0;
        int16_t start = 0;
        uint32_t io_before = 0;
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
#ifndef GGSWAN_HOST
        {
            uint16_t fast_off = drc_lookup_asm();
            if (fast_off) {
                drc_enter_setup_asm();
#ifdef GGSWAN_SF7000
                sf_port_a_plan(block_start);
#endif
                drc_enter(fast_off);
                if (drc_dispatch_post_asm())
                    continue;
                start = block_start;
                goto post_block_slow;
            }
        }
#endif
        pc = z80.pc;
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
            if (!z80.ei_pending && z80.im != 2 && STACK_OK(sp)) {
                // IM 1 with the stack in work RAM (the usual case): taken
                // here, the same as the interpreter's take_irq()
                z80.iff1 = z80.iff2 = 0;
                z80.r = (z80.r & 0x80) | ((z80.r + 1) & 0x7F);
                z80.sp = sp;
                PUSH_PC(sp);
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
#ifdef GGSWAN_SF7000
        // sfswan: all of it is RAM and translated; with the IPL at 0000h,
        // the interpreter (short: its long parts are the plugin's)
        uint8_t ram_code = 0;
        if (sf.rom_on) {
#else
        uint8_t ram_code = pc >= 0xC000 && pc < 0xDFC0;
        if ((pc >= 0xC000 && !ram_code) || (pc >= 0x8000 && pc < 0xC000 && (drc_st.keys[0] & 0x08))
                || !SP_OK(z80.sp)) {
#endif
            // code in RAM (it may change): interpreter. And any code
            // while the stack is not in the work RAM (SP_OK)
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
            // L; any other has "jle X" there, or "jg target" once linked
            // short; an unlinked one has "jmp X" (EBh) at L)
            const uint8_t __far *g = MK_FP(pending_seg, pending_link - 2);
            if (g[0] == 0x7E) {
                if (g[2] == 0xEB && !ic_link(pending_seg, pending_link, pc, drc_entry[1], off, bank))
                    link_patch(pending_seg, pending_link, drc_entry[1], off, 0, bank);
            } else if (g[0] == 0x75) {
                link_patch(pending_seg, pending_link, drc_entry[1], off, 1, bank);
            }
        }
        pending_link = 0;
        start = z80.cycles > 30000 ? 30000 : (int16_t) z80.cycles;
        drc_st.cycles = start;
        block_start = start;
        // the line routine at the exits (LINE stub): left = left_base +
        // [cycles], as drc_cycles_left
        int32_t lb = z80.cycles - start;
        drc_st.left_base = (int16_t) lb;
        drc_st.fl_off = drc_no_fast || drc_check || gg_every_line || lb > 30000
            || z80_break || drc_flush_request;
#ifdef GGSWAN_SG1000
        drc_st.fl_off = 1;   // (no line interrupts: nothing for the routine)
#endif
        if (drc_check) {
            io_before = drc_stat_helper[1] + drc_stat_helper[2] + drc_stat_helper[3] + drc_stat_helper[4];
            check_begin();
        }
#ifdef GGSWAN_HOST
        host_trace(pc, z80.b, z80.h << 8 | z80.l);
#endif
        in_block = 1;
#ifdef GGSWAN_SF7000
        sf_port_a_plan(start);
#endif
        prof_phase = PROF_CODE;
        drc_enter(off);
#ifndef GGSWAN_HOST
    post_block_slow:
#endif
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
            // DYN may know it: its L0 entry (one for each address) goes.
            // The whole L0 was emptied here: Shinobi jumps through RAM
            // (D103h: JP nn, the frame interrupt's routine, another one
            // every frame), the block of that JP is linked from the ROM
            // and finds its bytes changed every other frame: 6000 cycles
            // of REP STOSW each time, and the L0 to fill again
            {
                uint16_t *e = (uint16_t*) IRAM(L0_ENTRY(z80.pc));
                if (e[0] == z80.pc && (uint8_t) e[1] == RAM_KEY)
                    e[0] = 0xFFFF;
            }
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
    run_pc = z80.pc;
    prof_phase = PROF_OTHER;
    return z80.cycles;
}
