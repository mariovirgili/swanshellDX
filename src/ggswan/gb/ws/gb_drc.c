/**
 * gbswan - the recompiler (SM83 -> V30MZ): the translator and the
 * dispatcher. GB-ABI.md describes the translated code; gb_drc_asm.s has
 * the routines it calls.
 *
 * A block is a run of Game Boy instructions up to a jump, call or return
 * (or 40 of them). It is translated the first time it is reached and kept
 * in a PSRAM bank reached through the linear area; a table in another
 * bank finds it again by address and ROM bank. An exit to a known address
 * is a jump the dispatcher points at the next block once that is there,
 * so translated code runs from block to block until the cycles to the
 * next event run out. What the translator does not do (HALT, STOP, DAA,
 * the two SP + e, most CB operations on (HL)) leaves to the interpreter
 * for that instruction. Code in RAM is translated too, with a copy of its
 * bytes that is compared every time the block is entered.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wonderful.h>
#include <ws.h>
#include "gb.h"
#include "gb_drc.h"
#include "gb_ws.h"
extern uint16_t gbf_rom0_off, gbf_romx_off, gbf_ram_off;

_Static_assert(offsetof(gb_t, a) == GB_A && offsetof(gb_t, c) == GB_BC && offsetof(gb_t, e) == GB_DE
    && offsetof(gb_t, l) == GB_HL && offsetof(gb_t, sp) == GB_SP && offsetof(gb_t, pc) == GB_PC
    && offsetof(gb_t, lcd_on) == GB_LCD_ON && offsetof(gb_t, keys) == GB_KEYS
    && offsetof(gb_t, bank_lo) == GB_BANK_LO && offsetof(gb_t, romx_bank) == GB_ROMX
    && offsetof(gb_t, ime) == GB_IME && offsetof(gb_t, halted) == GB_HALTED && offsetof(gb_t, frame_done) == GB_FRAME_DONE, "gb_drc.h");

uint8_t drc_ah, drc_n, drc_tmp, drc_ei, drc_interp;
const uint8_t __far *drc_cmp_p;     // gbd_ram_cmp's: the block's copy of its bytes,
uint16_t drc_cmp_ram;               // where they are in the internal RAM,
uint8_t drc_cmp_len;                // how many
uint8_t gbd_ram_cmp(void);          // gb_asm.s
uint16_t drc_link, drc_link_seg, drc_segtab[4];
uint16_t drc_key0, drc_keyx, drc_hash0, drc_hashx, drc_tseg = DRC_TABLE_SEG;
uint16_t drc_ram_jmp[2];        // (gb_drc_asm.s, s_lookup_ram: the block to go to)
uint8_t drc_must, drc_ram_ok;
uint8_t drc_mbc_and, drc_mbc_z1, drc_p1_tab[4];
uint16_t drc_mbc_top, drc_mbc_lo, drc_mbc_or, drc_mbc_hi, drc_bank_mask;
uint8_t drc_ramg;
uint16_t drc_ram_seg;
uint16_t drc_blocks, drc_flushes, drc_cold_fails;

/* ---- the blocks' table and the code ---- */

typedef struct {
    uint16_t pc;
    uint16_t key;       // 0: free; ROM bank + 1; FFFFh: in RAM
    uint16_t off, seg;
} blk_t;
#define TABLE ((blk_t __far*) MK_FP(DRC_TABLE_SEG, 0))
#define TABLE_N 8192        /* (64 KiB: all of segment 4000h. 4096 until patch 164: about 3,000 blocks, which Castlevania II has by its second screen, and a place was not found in 8 tries: everything forgotten and translated again) */
#define KEY_RAM 0xFFFF
#define CODE_LIMIT 0xF000

// A block is put together in the console's own RAM and copied to the
// code segment when it is whole (translate): until patch 174 every byte
// went straight there through a far pointer, a segment register loaded
// each time, and that was most of what translate_block did - eighteen
// bytes or so for a Game Boy instruction. `code` is where the segment's
// offset 0 would be were the block in place, so code[code_top] and the
// places patched afterwards are written as before. The room is the 7.5 KiB
// the echo of the work RAM would have (GB-MEMORY-MAP.md); a block is
// under 4 KiB (CODE_LIMIT).
#define DRC_EBUF 0x8000u
static uint16_t code_seg, code_top;
static uint8_t *code;

// The table and the translated code are PSRAM read (and run) through the
// linear area, segments 4000h-C000h. They cannot be written there: on the
// cartridge a write to the linear area does nothing (first run on a
// WonderSwan, 2026-11-16: hw_test in gb_main.c read back 55h after writing
// AAh; nileswan-medem and gbrun.py had let it through until then). They are
// written as the other plugins' code is: through the RAM window with the
// PSRAM mapped there (the "flash" port), at the bank the segment is.
// Between gbd_lin_open and gbd_lin_close the cartridge's RAM is not at
// 1000h: nothing of the Game Boy's may be read or written.
uint16_t drc_lin_hi, drc_lin_mask = 0xFFFF;     // (gb_main.c: port C0h << 4, the ROM banks' mask)
static uint16_t lin_prev;
void gbd_lin_open(uint16_t seg) {
    lin_prev = inportw(WS_CART_EXTBANK_RAM_PORT);
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, (drc_lin_hi | seg >> 12) & drc_lin_mask);
}
void gbd_lin_close(void) {
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, lin_prev);
}
// (src: not in the RAM window)
void gbd_lin_put(uint16_t seg, uint16_t off, const void __far *src, uint16_t n) {
    gbd_lin_open(seg);
    _fmemcpy(MK_FP(0x1000, off), src, n);
    gbd_lin_close();
}

static void code_bank(uint16_t seg) {
    code_seg = seg;
    gbd_lin_put(seg, 0, drc_stubs, drc_stubs_size);
    code_top = drc_stubs_size;
}

// a bit for each first place in the table: is there a block in ROM whose
// first place it is? scan_block asks, at every instruction from the
// sixth, whether a block already starts there, and nearly always none
// does: the bit says so without the table being searched (find was 4% of
// the time at Race Drivin's start, and the asking twice that). In the
// room after DRC_EBUF's 4 KiB.
#define DRC_HAS ((uint8_t*) 0x9800)
static const uint8_t bit_of[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };

static void flush(void) {
    memset(DRC_HAS, 0, TABLE_N / 8);
    memset((uint8_t*) DRC_RC0, 0xFF, 0x2000);       // (no address is FFFFh: both small tables empty)
    gbd_lin_open(DRC_TABLE_SEG);
    _fmemset(MK_FP(0x1000, 0), 0, 0x8000);
    _fmemset(MK_FP(0x1000, 0x8000), 0, 0x8000);
    gbd_lin_close();
    code_bank(DRC_CODE_SEG);
    drc_link = 0;
    drc_flushes++;
}

// (gb_main.c: a state brought back - nothing translated is its)
void gbd_flush(void) {
    flush();
}

static uint16_t hash(uint16_t pc, uint16_t key) {
    return (pc ^ (pc >> 6) ^ (key << 5)) & (TABLE_N - 1);
}

#define E(b) (code[code_top++] = (uint8_t) (b))
static inline void EW(uint16_t w) {
    *(uint16_t*) (code + code_top) = w;
    code_top += 2;
}
#define ADDR(x) ((uint16_t) (uintptr_t) &(x))

// a write from translated code that is not plain memory (gb_drc_asm.s:
// s_wrt); drc_must: whether the dispatcher is needed after it: 1 an
// interrupt can be taken now, or the time to the next event has run out
// (or the first ROM area has another bank); 2 the second ROM area has
// another bank (the dispatcher if the code is there); 0 on with the block
void gbd_write(uint16_t a, uint8_t v) {
    if (a < 0x8000) {
        uint16_t k0 = drc_key0, kx = drc_keyx;
        gbf_write(a, v);
        drc_must = drc_key0 != k0 ? 1 : drc_keyx != kx ? 2 : 0;
    } else {
        // An event that is due (a block goes on to its end past the
        // event's cycle) is done before a register is written: the write
        // may set the next event from the time now, and the one that was
        // due would never be looked at. Super Mario Land lost its STAT
        // interrupt that way on the frames a timer interrupt came just
        // before it, and its picture was drawn with the status bar's
        // scroll for a frame (patches 160-166).
        if (a >= 0xFF00 && gbf_cyc <= 0) gbf_host_event();
        if (a >= 0xFF00 && a < 0xFF80) gbf_io_write(a & 0xFF, v);
        else gbf_write(a, v);
        drc_must = a >= 0xFF00 && ((gb.ime && (gb_ie & gb_io[GB_IF] & 0x1F)) || gbf_cyc <= 0);
    }
}

// what P1 reads for each selection of rows (bits 5 and 4), with the keys
// held now (gb_drc_asm.s: s_p1)
void gbd_keys(void) {
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t low = 15;
        if (!(i & 1)) low &= ~(gb.keys & 15);
        if (!(i & 2)) low &= ~(gb.keys >> 4);
        drc_p1_tab[i] = 0xC0 | i << 4 | low;
    }
}

/* ---- the translator ---- */

#if 0   // (in op_inf since patch 174)
static const uint8_t __far op_len[256] = {
    1,3,1,1,1,1,2,1, 3,1,1,1,1,1,2,1, 2,3,1,1,1,1,2,1, 2,1,1,1,1,1,2,1,
    2,3,1,1,1,1,2,1, 2,1,1,1,1,1,2,1, 2,3,1,1,1,1,2,1, 2,1,1,1,1,1,2,1,
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    1,1,3,3,3,1,2,1, 1,1,3,2,3,3,2,1, 1,1,3,1,3,1,2,1, 1,1,3,1,3,1,2,1,
    2,1,1,1,1,1,2,1, 2,1,3,1,1,1,2,1, 2,1,1,1,1,1,2,1, 2,1,3,1,1,1,2,1 };
#endif
// machine cycles (a conditional one not taken)
#if 0   // (in op_inf since patch 174)
static const uint8_t __far op_cyc[256] = {
    1,3,2,2,1,1,2,1, 5,2,2,2,1,1,2,1, 1,3,2,2,1,1,2,1, 3,2,2,2,1,1,2,1,
    2,3,2,2,1,1,2,1, 2,2,2,2,1,1,2,1, 2,3,2,2,3,3,3,1, 2,2,2,2,1,1,2,1,
    1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1,
    1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1, 2,2,2,2,2,2,1,2, 1,1,1,1,1,1,2,1,
    1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1,
    1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1,
    2,3,3,4,3,4,2,4, 2,4,3,1,3,6,2,4, 2,3,3,1,3,4,2,4, 2,4,3,1,3,1,2,4,
    3,3,2,1,1,4,2,4, 4,1,4,1,1,1,2,4, 3,3,2,1,1,4,2,4, 3,2,4,1,1,1,2,4 };
#endif

// the flags an instruction writes (bits 0-2: Z, H, C) and needs (bits
// 3-5); 40h: it ends the block; 80h: it may leave the block (all the
// flags are needed, where the Game Boy has them)
#define FZ 1
#define FH 2
#define FC 4
#define FALL 7
#define RD(x) ((x) << 3)
#define F_END 0x40
#define F_OUT 0x80

// The bytes being translated: read once, where they are (the ROM's
// segments, the work RAM, the high RAM), not a call of gbf_read for each
// look at each byte (a block's translation took about 9,000 cycles)
uint8_t tbuf[128];
uint16_t tbase;
static uint8_t tlen;
static inline uint8_t rb(uint16_t a) {
    uint16_t o = a - tbase;
    return o < tlen ? tbuf[o] : gbf_read(a);
}
static void tfill(uint16_t pc) {
    tbase = pc;
    uint16_t top = pc < 0x4000 ? 0x4000 : pc < 0x8000 ? 0x8000 : pc >= 0xC000 && pc < 0xE000 ? 0xE000 : pc >= 0xFF80 ? 0xFFFF : pc;
    uint16_t n = top - pc;
    if (n > sizeof(tbuf)) n = sizeof(tbuf);
    tlen = n;
    if (!n) return;
    if (pc < 0x8000) _fmemcpy(tbuf, MK_FP(drc_segtab[pc >= 0x4000], pc), n);
    else memcpy(tbuf, (const uint8_t*) (pc - 0x6000), n);
}

// the instructions left to the interpreter: 08h, 10h, 39h, 76h, E8h, F8h
// and the eleven that are none (D3h, DBh, DDh, E3h, E4h, EBh, ECh, EDh,
// F4h, FCh, FDh), a bit each. (A switch until patch 174: seventeen
// compares for every instruction translated.)
static const uint8_t interp_bits[32] = {
    0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x28, 0x18, 0x39, 0x10, 0x31
};
static inline uint8_t interp_op(uint8_t op, uint8_t cb) {
    (void) cb;
    return interp_bits[op >> 3] >> (op & 7) & 1;
}

// what an instruction does with the flags and whether it can leave the
// block: by opcode from a table (the function below, for every opcode: the
// translator spent a tenth of its time in it), but for CB's, which go by
// their second byte
static const uint8_t __far op_flags_tab[256] = {
    0x00, 0x00, 0x80, 0x00, 0x03, 0x03, 0x00, 0x07, 0xC0, 0x06, 0x00, 0x00, 0x03, 0x03, 0x00, 0x07,
    0xC0, 0x00, 0x80, 0x00, 0x03, 0x03, 0x00, 0x27, 0xC0, 0x06, 0x00, 0x00, 0x03, 0x03, 0x00, 0x27,
    0x80, 0x00, 0x80, 0x00, 0x03, 0x03, 0x00, 0x37, 0x80, 0x06, 0x00, 0x00, 0x03, 0x03, 0x00, 0x02,
    0x80, 0x00, 0x80, 0x00, 0x83, 0x83, 0x80, 0x06, 0x80, 0xC0, 0x00, 0x00, 0x03, 0x03, 0x00, 0x26,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xC0, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x80, 0x00, 0x80, 0xC0, 0x80, 0x00, 0x07, 0xC0, 0x80, 0xC0, 0x80, 0x07, 0x80, 0xC0, 0x27, 0xC0,
    0x80, 0x00, 0x80, 0xC0, 0x80, 0x00, 0x07, 0xC0, 0x80, 0xC0, 0x80, 0xC0, 0x80, 0xC0, 0x27, 0xC0,
    0x80, 0x00, 0x80, 0xC0, 0xC0, 0x00, 0x07, 0xC0, 0xC0, 0xC0, 0x80, 0xC0, 0xC0, 0xC0, 0x07, 0xC0,
    0x00, 0x07, 0x00, 0x00, 0xC0, 0x38, 0x07, 0xC0, 0xC0, 0x00, 0x00, 0x00, 0xC0, 0xC0, 0x07, 0xC0
};
static inline uint8_t op_flags(uint8_t op, uint8_t cb) {
    if (op != 0xCB) return op_flags_tab[op];
    uint8_t cx = cb >> 6, cy = (cb >> 3) & 7;
    if ((cb & 7) == 6 && cx != 1) return F_OUT;     // (the write to (HL) may leave)
    if (cx == 0) return FALL | ((cy == 2 || cy == 3) ? RD(FC) : 0);
    if (cx == 1) return FZ | FH;
    return 0;
}
#if 0
__attribute__((noinline)) static uint8_t op_flags_all(uint8_t op, uint8_t cb) {
    if (interp_op(op, cb)) return F_END | F_OUT;
    uint8_t x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    if (x == 1) return z == 6 ? 0 : y == 6 ? F_OUT : 0;
    if (x == 2 || (x == 3 && z == 6)) return FALL | ((y == 1 || y == 3) ? RD(FC) : 0);
    if (x == 0) {
        switch (z) {
        case 0: return y == 0 ? 0 : y == 3 ? F_END | F_OUT : F_OUT;
        case 1: return (op & 8) ? FH | FC : 0;
        case 2: return (y & 1) ? 0 : F_OUT;
        case 3: return 0;
        case 4: case 5: return y == 6 ? F_OUT | FZ | FH : FZ | FH;
        case 6: return y == 6 ? F_OUT : 0;
        default:
            if (y < 4) return FALL | (y >= 2 ? RD(FC) : 0);
            if (y == 4) return FALL | RD(FH | FC);      // DAA
            if (y == 5) return FH;
            if (y == 6) return FH | FC;
            return FH | FC | RD(FC);
        }
    }
    switch (z) {
    case 0:
        if (y < 4) return F_OUT;
        return y == 4 ? F_OUT : 0;      // (LDH (n), A may leave: io_write)
    case 1:
        if (!(op & 8)) return op == 0xF1 ? FALL : 0;
        return op == 0xF9 ? 0 : F_END | F_OUT;
    case 2:
        if (y < 4) return F_OUT;
        return (y == 4 || y == 5) ? F_OUT : 0;
    case 3:
        if (y == 0) return F_END | F_OUT;
        if (y == 1) {
            uint8_t cx = cb >> 6, cy = (cb >> 3) & 7;
            if ((cb & 7) == 6 && cx != 1) return F_OUT;     // (the write to (HL) may leave)
            if (cx == 0) return FALL | ((cy == 2 || cy == 3) ? RD(FC) : 0);
            if (cx == 1) return FZ | FH;
            return 0;
        }
        return 0;       // DI, EI
    case 4: return F_OUT;
    case 5: return (op & 8) ? F_END | F_OUT : (op == 0xF5 ? RD(FALL) : 0);
    default: return F_END | F_OUT;      // RST
    }
}
#endif

#define MAXI 40
uint16_t i_pc[MAXI + 1];
uint8_t i_live[MAXI];    // the flags needed before each instruction
// BC, DE, HL before each instruction, when the block's own code says what
// they are (LD rr, nn and then only INC, DEC and the like) and that is an
// address in the work RAM or the high RAM: its place in the internal RAM
// (0: not known, or elsewhere). An access through the register is then an
// access to that place, with nothing to check.
uint16_t i_dir[3][MAXI];
// ... and what part of the memory their high byte says they point into,
// when the block's own code says what that byte is or lies between (LD H,
// n; LD rr, nn; then INC and DEC of it or of the pair): 1 the tiles'
// patterns or the work RAM (read and written where they are, with nothing
// to check), 2 the video memory (read so), 3 and 4 the two ROM areas (read
// through their segments). Race Drivin' reads its tables by LD H, n / LD
// L, A / LD r, (HL): 54 bytes of code and six instructions a read came to
// 4 and one.
uint8_t i_cls[3][MAXI];
uint8_t n_ins;

// the translator's state
static uint8_t fl;          // the flags: 0 in AH, 1 in the V30MZ's
static uint8_t hfix;        // (in the V30MZ's) H: 0 is AF, 1 is clear, 2 is set
static int8_t nst, nmem;    // N: what it is (-1: what drc_n has), what drc_n is known to have
static uint16_t cyc;        // cycles not yet taken off gbf_cyc
static uint8_t live;        // the flags needed before the instruction being translated
uint16_t blk_pc;
uint8_t in_ram;
// by opcode, in one place for the two passes (three tables until patch
// 174, a far read each): what it does with the flags (op_flags_tab), then
// its length (bits 0-1), its cycles (bits 2-4), whether scan_block has
// something of its own to do for it (bit 5: LD rr, nn, LD B / D / H, n,
// LDH A, (n), EI; and CB, whose flags are by its second byte), whether
// it changes or uses what scan_block knows of BC, DE, HL (bit 6) and
// whether it is left to the interpreter (bit 7: interp_op). gb_asm.s,
// gbd_scan_fast, does the instructions with neither 5 nor (once
// something is known) 6
const uint8_t op_inf[256][2] = {
    { 0x00, 0x05 }, { 0x00, 0x2F }, { 0x80, 0x49 }, { 0x00, 0x49 }, { 0x03, 0x45 }, { 0x03, 0x45 }, { 0x00, 0x6A }, { 0x07, 0x05 },
    { 0xC0, 0x97 }, { 0x06, 0x49 }, { 0x00, 0x09 }, { 0x00, 0x49 }, { 0x03, 0x45 }, { 0x03, 0x45 }, { 0x00, 0x4A }, { 0x07, 0x05 },
    { 0xC0, 0x86 }, { 0x00, 0x2F }, { 0x80, 0x49 }, { 0x00, 0x49 }, { 0x03, 0x45 }, { 0x03, 0x45 }, { 0x00, 0x6A }, { 0x27, 0x05 },
    { 0xC0, 0x0E }, { 0x06, 0x49 }, { 0x00, 0x09 }, { 0x00, 0x49 }, { 0x03, 0x45 }, { 0x03, 0x45 }, { 0x00, 0x4A }, { 0x27, 0x05 },
    { 0x80, 0x0A }, { 0x00, 0x2F }, { 0x80, 0x49 }, { 0x00, 0x49 }, { 0x03, 0x45 }, { 0x03, 0x45 }, { 0x00, 0x6A }, { 0x37, 0x05 },
    { 0x80, 0x0A }, { 0x06, 0x49 }, { 0x00, 0x49 }, { 0x00, 0x49 }, { 0x03, 0x45 }, { 0x03, 0x45 }, { 0x00, 0x4A }, { 0x02, 0x05 },
    { 0x80, 0x0A }, { 0x00, 0x0F }, { 0x80, 0x49 }, { 0x00, 0x09 }, { 0x83, 0x4D }, { 0x83, 0x4D }, { 0x80, 0x4E }, { 0x06, 0x05 },
    { 0x80, 0x0A }, { 0xC0, 0xC9 }, { 0x00, 0x49 }, { 0x00, 0x09 }, { 0x03, 0x05 }, { 0x03, 0x05 }, { 0x00, 0x0A }, { 0x26, 0x05 },
    { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x49 }, { 0x00, 0x45 },
    { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x49 }, { 0x00, 0x45 },
    { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x49 }, { 0x00, 0x45 },
    { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x49 }, { 0x00, 0x45 },
    { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x49 }, { 0x00, 0x45 },
    { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x45 }, { 0x00, 0x49 }, { 0x00, 0x45 },
    { 0x80, 0x49 }, { 0x80, 0x49 }, { 0x80, 0x49 }, { 0x80, 0x49 }, { 0x80, 0x49 }, { 0x80, 0x49 }, { 0xC0, 0xC5 }, { 0x80, 0x49 },
    { 0x00, 0x05 }, { 0x00, 0x05 }, { 0x00, 0x05 }, { 0x00, 0x05 }, { 0x00, 0x05 }, { 0x00, 0x05 }, { 0x00, 0x09 }, { 0x00, 0x05 },
    { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x09 }, { 0x07, 0x05 },
    { 0x27, 0x05 }, { 0x27, 0x05 }, { 0x27, 0x05 }, { 0x27, 0x05 }, { 0x27, 0x05 }, { 0x27, 0x05 }, { 0x27, 0x09 }, { 0x27, 0x05 },
    { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x09 }, { 0x07, 0x05 },
    { 0x27, 0x05 }, { 0x27, 0x05 }, { 0x27, 0x05 }, { 0x27, 0x05 }, { 0x27, 0x05 }, { 0x27, 0x05 }, { 0x27, 0x09 }, { 0x27, 0x05 },
    { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x09 }, { 0x07, 0x05 },
    { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x09 }, { 0x07, 0x05 },
    { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x09 }, { 0x07, 0x05 },
    { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x05 }, { 0x07, 0x09 }, { 0x07, 0x05 },
    { 0x80, 0x09 }, { 0x00, 0x4D }, { 0x80, 0x0F }, { 0xC0, 0x13 }, { 0x80, 0x0F }, { 0x00, 0x11 }, { 0x07, 0x0A }, { 0xC0, 0x11 },
    { 0x80, 0x09 }, { 0xC0, 0x11 }, { 0x80, 0x0F }, { 0x07, 0x66 }, { 0x80, 0x0F }, { 0xC0, 0x1B }, { 0x27, 0x0A }, { 0xC0, 0x11 },
    { 0x80, 0x09 }, { 0x00, 0x4D }, { 0x80, 0x0F }, { 0xC0, 0x85 }, { 0x80, 0x0F }, { 0x00, 0x11 }, { 0x07, 0x0A }, { 0xC0, 0x11 },
    { 0x80, 0x09 }, { 0xC0, 0x11 }, { 0x80, 0x0F }, { 0xC0, 0x85 }, { 0x80, 0x0F }, { 0xC0, 0x85 }, { 0x27, 0x0A }, { 0xC0, 0x11 },
    { 0x80, 0x0E }, { 0x00, 0x4D }, { 0x80, 0x09 }, { 0xC0, 0x85 }, { 0xC0, 0x85 }, { 0x00, 0x11 }, { 0x07, 0x0A }, { 0xC0, 0x11 },
    { 0xC0, 0x92 }, { 0xC0, 0x05 }, { 0x80, 0x13 }, { 0xC0, 0x85 }, { 0xC0, 0x85 }, { 0xC0, 0x85 }, { 0x07, 0x0A }, { 0xC0, 0x11 },
    { 0x00, 0x2E }, { 0x07, 0x0D }, { 0x00, 0x09 }, { 0x00, 0x05 }, { 0xC0, 0x85 }, { 0x38, 0x11 }, { 0x07, 0x0A }, { 0xC0, 0x11 },
    { 0xC0, 0xCE }, { 0x00, 0x09 }, { 0x00, 0x13 }, { 0x00, 0x25 }, { 0xC0, 0x85 }, { 0xC0, 0x85 }, { 0x07, 0x0A }, { 0xC0, 0x11 },
};
static uint8_t i_known;         // (scan_block: the first instruction i_dir and i_cls are written for; FFh: none)
uint8_t i_inf[MAXI];     // (scan_block: each instruction's second byte of op_inf)
static uint8_t after_ei;    // the instruction after EI: whatever leaves the block goes to the dispatcher
static uint8_t idle;        // the block is a loop that waits: 1 on memory, 2 on LY, 3 on STAT
static uint8_t idle_i;      // ... and this instruction is its jump back
static uint8_t cur_i;       // the instruction being translated
static uint16_t cur_dir;    // the place its access through BC, DE or HL is known to be at (i_dir), or 0
static uint16_t cur_cart;   // ... or the address, when that is in the cartridge's RAM
static uint8_t cur_cls;     // ... or the part of the memory (i_cls), or 0
static uint8_t cur_wait;    // translating a waiting loop's read of STAT: the modes it goes round in
static uint8_t idle_first;  // translating that loop's first instruction (its read of STAT)
static uint8_t stat_at[3], stat_mask[3], n_stat;   // such loops inside the block: their STAT reads, the modes
static uint8_t idle_modes;  // (on STAT) the modes it goes round in, when that is all it depends on (else 0)

static const uint8_t __far R8[8] = { 5, 1, 6, 2, 7, 3, 0xFF, 0 };  // B C D E H L - A: CH CL DH DL BH BL AL
static const uint8_t __far R16[4] = { 1, 2, 3, 5 };                // BC DE HL SP: CX DX BX BP

// the flags into AH
static void mat(void) {
    if (!fl) return;
    E(0x9F);                                    // lahf
    if (hfix == 1) { E(0x80); E(0xE4); E(0xEF); }   // and ah, EFh
    else if (hfix == 2) { E(0x80); E(0xCC); E(0x10); }  // or ah, 10h
    fl = 0;
}
// before code that changes the V30MZ's flags
static void clob(void) {
    if (fl && live) mat();
    else if (fl) { fl = 0; /* (not needed: AH is as wrong as they are) */ }
}
// the flags into the V30MZ's (for a conditional jump)
static void to_flags(void) {
    if (fl) return;
    E(0x9E);                                    // sahf
    fl = 1;
    hfix = 0;
}
// C into CF, before an instruction that takes it or keeps it (the rest of
// the V30MZ's flags are then that instruction's)
static void carry_in(void) {
    if (!fl) { E(0xD0); E(0xEC); }              // shr ah, 1
}
static void nflush(void) {
    if (nst >= 0 && nst != nmem) {
        E(0xC6); E(0x06); EW(ADDR(drc_n)); E(nst ? 0x40 : 0);
        nmem = nst;
    }
}
static uint16_t stub_off[ST_COUNT];     // (drc_stub_offs, copied: gbd_init)

// The slow ways out of line. An access through a pointer was "is it the
// work RAM: do it, jump over the rest; the rest": the jump, taken every
// time, was four cycles of the ten (17% of the translated code's time in
// ZAS and in Race Drivin' was in those jumps). The rest - the high RAM,
// the ROM, the routine - is now written apart (9000h-97FFh while a block
// is translated) and put after the block's code, or, when the place the
// tests jump from would be too far for a short jump, in the middle of it
// with a jump over; each ends with a jump back to after the access.
// While it is written, `code` points there and code_top is where it was:
// what is relative to the place (the calls, the jump back) is noted and
// put right when the piece is moved to its place.
#define COLD_SCR ((uint8_t*) 0x9000)
#define COLD_SIZE 0x800
#define COLD_MAX 8
static struct {
    uint16_t pos, len, base;
    uint16_t rel[4], pj[3], poff[3];
    uint8_t nrel, npatch;
} cold[COLD_MAX];
#ifndef COLD_MASK
#define COLD_MASK 4
#endif
static uint8_t cold_n, in_cold, cold_off, cold_fail;
static uint16_t cold_used, cold_first;
static uint8_t *cold_code;
static uint16_t cold_top;
static void cold_flush(uint8_t jump);
static void cold_begin(void) {
    if (cold_n == COLD_MAX || cold_used > COLD_SIZE - 96) cold_flush(1);
    cold_code = code;
    cold_top = code_top;
    cold[cold_n].pos = cold_used;
    cold[cold_n].base = code_top;
    cold[cold_n].nrel = cold[cold_n].npatch = 0;
    code = COLD_SCR + cold_used - code_top;
    in_cold = 1;
}
// (the test at j, in the block's code, jumps to here)
static void cold_label(uint16_t j) {
    uint8_t n = cold[cold_n].npatch++;
    cold[cold_n].pj[n] = j;
    cold[cold_n].poff[n] = code_top - cold[cold_n].base;
    if (!cold_first || j < cold_first) cold_first = j;
}
static void cold_rel(void) {
    if (in_cold) cold[cold_n].rel[cold[cold_n].nrel++] = code_top - cold[cold_n].base;
}
static void cold_back(uint16_t done) {
    E(0xE9);
    cold_rel();
    EW(done - (code_top + 2));
}
static void cold_end(void) {
    cold[cold_n].len = code_top - cold[cold_n].base;
    cold_used += cold[cold_n].len;
    code = cold_code;
    code_top = cold_top;
    in_cold = 0;
    cold_n++;
}
static void cold_flush(uint8_t jump) {
    if (!cold_n) return;
    if (jump) {
        E(0xE9);
        EW(cold_used);
    }
    for (uint8_t i = 0; i < cold_n; i++) {
        uint16_t at = code_top, d = at - cold[i].base;
        memcpy(code + at, COLD_SCR + cold[i].pos, cold[i].len);
        for (uint8_t k = 0; k < cold[i].nrel; k++)
            *(uint16_t*) (code + at + cold[i].rel[k]) -= d;
        for (uint8_t k = 0; k < cold[i].npatch; k++) {
            uint16_t t = at + cold[i].poff[k] - cold[i].pj[k];
            if (t > 127) cold_fail = 1;     // (too far: the block is translated again without this)
            code[cold[i].pj[k] - 1] = t;
        }
        code_top += cold[i].len;
    }
    cold_n = 0;
    cold_used = 0;
    cold_first = 0;
}

static void call_stub(uint8_t st) {
    E(0xE8);
    cold_rel();
    EW(stub_off[st / 3] - (code_top + 2));
}
static void jmp_stub(uint8_t st) {
    E(0xE9);
    cold_rel();
    EW(stub_off[st / 3] - (code_top + 2));
}
// the cycles so far off the count (the V30MZ's flags are changed)
static void sub_cyc(void) {
    if (!cyc) return;
    E(0x83); E(0x2E); EW(ADDR(gbf_cyc)); E(cyc);
    cyc = 0;
}

// Code in RAM whose operands change (a program that writes the n of an LD
// r, n or the address of a jump before running it): the operands that
// have been seen to change are read from the RAM as the code runs, and
// are not compared when the block is entered. tmpl_req: the bytes of the
// block (a bit each, 128) to be read so; tmpl_can: those that could be,
// found as it is translated (the operands of LD r, n; LD rr, nn; JP nn;
// JR e; CALL nn in a block wholly in the work RAM or the high RAM).
static uint8_t tmpl_req[16], tmpl_can[16], ram_direct;
static uint8_t tmpl_bit(uint16_t a) {
    uint16_t o = a - blk_pc;
    return o < 128 && (tmpl_req[o >> 3] >> (o & 7) & 1);
}
// the n bytes at a are an operand that can be read as the code runs: is it?
static uint8_t operand(uint16_t a, uint8_t n) {
    if (!in_ram || !ram_direct) return 0;
    uint16_t o = a - blk_pc;
    if (o + n > 128) return 0;
    uint8_t dyn = 0;
    for (uint8_t k = 0; k < n; k++) {
        tmpl_can[(o + k) >> 3] |= 1 << ((o + k) & 7);
        if (tmpl_req[(o + k) >> 3] >> ((o + k) & 7) & 1) dyn = 1;
    }
    if (dyn) for (uint8_t k = 0; k < n; k++) tmpl_req[(o + k) >> 3] |= 1 << ((o + k) & 7);
    return dyn;
}

// N at a jump to code in ROM: is it written there before anything reads
// it? What writes H writes N, and what reads N (DAA, PUSH AF) reads H: the
// table has it. Then drc_n need not be stored before the jump ("mov byte
// [drc_n], n" is three cycles and a half on the console at nearly every
// exit). An interrupt taken between the two blocks pushes and pops the old
// N with AF, and the code there then writes it all the same.
static uint8_t n_dead(uint16_t a) {
    for (uint8_t k = 0; k < 16; k++) {
        if (a >= 0x7FFE) return 0;
        uint8_t op = rb(a);
        uint8_t f = op_flags(op, op == 0xCB ? rb(a + 1) : 0);
        if (f & RD(FH)) return 0;
        if (f & FH) return 1;
        if (f & F_END) return 0;
        if ((f & F_OUT) && op != 0xCB) {
            // (a conditional jump, call or return: the other way is not looked at)
            if ((op & 0xE7) == 0x20 || (op >= 0xC0 && (op & 0xE7) == 0xC0) || (op & 0xE7) == 0xC2 || (op & 0xE7) == 0xC4) return 0;
        }
        a += op == 0xCB ? 2 : (op_inf[op][1] & 3);
    }
    return 0;
}
static uint16_t loop_body;  // in a block that starts with a call for a whole loop: where the loop's own code starts (0: no such call)
// leave for a known address; link: the dispatcher may join the jump to
// the block there
static void exit_to(uint16_t target, uint8_t extra) {
    mat();
    uint8_t link = in_ram ? target == blk_pc : (target < 0x4000 || (blk_pc >= 0x4000 && target >= 0x4000 && target < 0x8000));
    if (!(link && !in_ram && !after_ei && nst >= 0 && nst != nmem && n_dead(target))) nflush();
    if (after_ei) {
        if (cyc + extra) { E(0x83); E(0x2E); EW(ADDR(gbf_cyc)); E(cyc + extra); }
        E(0xC7); E(0x06); EW(ADDR(gb) + GB_PC); EW(target);
        E(0xC7); E(0x06); EW(ADDR(drc_link)); EW(0);
        jmp_stub(ST_EXIT);
        return;
    }
    if (!link && !in_ram) {
        // (not to be joined: the table is looked in by the routine)
        if (cyc + extra) { E(0x83); E(0x2E); EW(ADDR(gbf_cyc)); E(cyc + extra); }
        E(0xBE); EW(target);                    // mov si, target
        jmp_stub(cyc + extra ? ST_EXIT_DYN_F : ST_EXIT_DYN);
        return;
    }
    E(0x83); E(cyc + extra ? 0x2E : 0x3E); EW(ADDR(gbf_cyc)); E(cyc + extra);     // sub (or cmp) word [gbf_cyc], n
    E(0x7E); E(0x05);                           // jle +5
    uint16_t site = code_top;
    if (loop_body && target == blk_pc) {
        // (a loop with a routine called at its start, for all its turns at
        // once: round to after that call, not through it every turn)
        uint16_t rel = loop_body - (code_top + 3);
        E(0xE9); EW(rel); E(0x90); E(0x90);
        link = 0;
    } else {
        E(0xE9); EW(2); E(0x90); E(0x90);       // jmp near (to the next line, until joined)
    }
    E(0xC7); E(0x06); EW(ADDR(gb) + GB_PC); EW(target);
    E(0xC7); E(0x06); EW(ADDR(drc_link)); EW(link ? site : 0);
    if (link) { E(0xC7); E(0x06); EW(ADDR(drc_link_seg)); EW(code_seg); }
    jmp_stub(ST_EXIT);
}
// leave for the address in SI
static void exit_dyn(uint8_t extra) {
    mat();
    nflush();
    if (cyc + extra) { E(0x83); E(0x2E); EW(ADDR(gbf_cyc)); E(cyc + extra); }
    if (after_ei) {
        E(0x89); E(0x36); EW(ADDR(gb) + GB_PC);     // mov [gb.pc], si
        jmp_stub(ST_EXIT);
        return;
    }
    jmp_stub(cyc + extra ? ST_EXIT_DYN_F : ST_EXIT_DYN);
}
// the instruction at pc is the interpreter's
static void exit_interp(uint16_t pc) {
    mat();
    nflush();
    sub_cyc();
    E(0xC7); E(0x06); EW(ADDR(gb) + GB_PC); EW(pc);
    E(0xC6); E(0x06); EW(ADDR(drc_interp)); E(1);
    jmp_stub(ST_EXIT);
}

// "cmp; jb slow; cmp; jae high" on the address in BX (hl) or SI: the fast
// case, the work RAM (and its echo's place), follows; returns where the
// two jumps' distances go. "high" (slow_starts) sends the high RAM back
// to the fast case too: its place in the internal RAM is by the same
// rule (programs keep counters and pointers' targets there)
static uint16_t range_check(uint8_t hl) {
    if (hl) { E(0x80); E(0xFF); E(0xC0); } else { E(0x81); E(0xFE); EW(0xC000); }
    E(0x72); E(0);
    uint16_t j = code_top;
    // (to DFFFh: the echo above it, E000h-FDFFh, is the routines'. Until
    // patch 176 the block took it for its own, at [address - 6000h]: not
    // the work RAM but 8000h-9DFFh, where the translator now keeps things)
    if (hl) { E(0x80); E(0xFF); E(0xE0); } else { E(0x81); E(0xFE); EW(0xE000); }
    E(0x73); E(0);
    return j;
}
// the fast case is done: jump over the slow one, which starts here
// (wr: a write, which IE, FFFFh, is not for here: an interrupt may be due)
static uint16_t slow_starts(uint16_t j, uint8_t hl, uint8_t wr) {
    E(0xEB); E(0);
    uint16_t done = code_top;
    // (the pieces written apart so far, if they are few, here, where
    // nothing falls into them: put after the access they were jumped over
    // by a jump the access' own jump landed on - ten cycles for four at
    // every read after a PUSH)
    if (cold_n && !in_cold && cold_used <= 32) cold_flush(0);
    uint16_t fast = j + (hl ? 5 : 6);
    code[fast - 1] = code_top - fast;
    E(0x81); E(hl ? 0xFB : 0xFE); EW(0xFF80);       // cmp bx/si, FF80h
    if (wr) {
        E(0x72); E(6);                              // jb slow
        E(0x81); E(hl ? 0xFB : 0xFE); EW(0xFFFF);
        E(0x75);                                    // jne fast
        uint8_t rel = fast - (code_top + 1);
        E(rel);
    } else {
        E(0x73);                                    // jae fast
        uint8_t rel = fast - (code_top + 1);
        E(rel);
    }
    code[j - 1] = code_top - j;
    return done;
}
#define SLOW_ENDS(done) (code[(done) - 1] = code_top - (done))

// An access to a known address in the cartridge's RAM: "is it plain RAM
// and enabled; ES = its segment", then the caller's instruction on
// es:[address]; cart_else: "jmp done", and the usual code follows for when
// it is not
static uint16_t cart_open(void) {
    E(0x80); E(0x3E); EW(ADDR(drc_ram_ok)); E(1);   // cmp byte [drc_ram_ok], 1
    E(0x75); E(0);                                  // jne slow
    uint16_t j = code_top;
    E(0x8E); E(0x06); EW(ADDR(drc_ram_seg));        // mov es, [drc_ram_seg]
    E(0x26);                                        // es:
    return j;
}
static uint16_t cart_else(uint16_t j) {
    E(0xEB); E(0);
    uint16_t done = code_top;
    code[j - 1] = code_top - j;
    return done;
}

// A read through BX (hl) or SI that is not in the RAM above C000h: from
// the ROM here, by its area's segment (tables are read this way all the
// time); "is the address below 8000h; ES = its area's segment", the
// caller's instruction with an ES prefix follows, then rom_else and the
// routine's call for the rest
static uint16_t rom_open(uint8_t hl) {
    if (hl) { E(0xF6); E(0xC7); E(0x80); } else { E(0xF7); E(0xC6); EW(0x8000); }   // test bh, 80h / test si, 8000h
    E(0x75); E(0);                                  // jnz: not the ROM
    uint16_t j = code_top;
    E(0x8E); E(0x06); EW(ADDR(drc_segtab));         // mov es, [drc_segtab]
    if (hl) { E(0x80); E(0xFF); E(0x40); } else { E(0x81); E(0xFE); EW(0x4000); }   // cmp bh, 40h / cmp si, 4000h
    E(0x72); E(4);                                  // jb +4
    E(0x8E); E(0x06); EW(ADDR(drc_segtab) + 2);     // mov es, [drc_segtab + 2]
    return j;
}
#define rom_else cart_else

// An access through BX (hl) or SI that may be in the cartridge's RAM, for a
// program that keeps its variables there (Super Mario Land 2: 135 reads
// and 58 writes a frame through the routines, some 57 cycles each on the
// console against 30 here): "is it A000h-BFFFh, plain RAM and enabled; ES
// = its segment", the caller's instruction with an ES prefix follows, then
// cartp_else and the routine's call for the rest. Only once the routines
// have counted enough such accesses (drc_cartp, gb_drc_asm.s): it is 26
// bytes at every access through a pointer.
uint8_t drc_cartp, drc_cart_req;
uint16_t drc_cart_n = 3000;
static uint16_t cartp_j[2];
static uint16_t cartp_open(uint8_t hl) {
    if (hl) { E(0x80); E(0xFF); E(0xA0); } else { E(0x81); E(0xFE); EW(0xA000); }   // cmp bh, A0h / cmp si, A000h
    E(0x72); E(0);                                  // jb: not it
    uint16_t j1 = code_top;
    if (hl) { E(0x80); E(0xFF); E(0xC0); } else { E(0x81); E(0xFE); EW(0xC000); }
    E(0x73); E(0);                                  // jae: not it
    uint16_t j2 = code_top;
    E(0x80); E(0x3E); EW(ADDR(drc_ram_ok)); E(1);   // cmp byte [drc_ram_ok], 1
    E(0x75); E(0);                                  // jne: not it
    uint16_t j3 = code_top;
    E(0x8E); E(0x06); EW(ADDR(drc_ram_seg));        // mov es, [drc_ram_seg]
    cartp_j[0] = j1; cartp_j[1] = j2;
    return j3;
}
static uint16_t cartp_else(uint16_t j3) {
    E(0xEB); E(0);
    uint16_t done = code_top;
    code[j3 - 1] = code_top - j3;
    code[cartp_j[0] - 1] = code_top - cartp_j[0];
    code[cartp_j[1] - 1] = code_top - cartp_j[1];
    return done;
}

// r (an x86 8 bit register) = the byte at HL (hl) or at SI
static void read_to(uint8_t r, uint8_t hl) {
    if (cur_dir) {
        E(0x8A); E(0x06 | r << 3); EW(cur_dir);     // mov r, [..]
        return;
    }
    if (cur_cls && !cur_cart) {
        if (cur_cls >= 3) {
            E(0x8E); E(0x06); EW(ADDR(drc_segtab) + (cur_cls == 4 ? 2 : 0));    // mov es, [..]
            E(0x26); E(0x8A); E(r << 3 | (hl ? 7 : 4));                         // mov r, es:[bx or si]
        } else {
            E(0x8A); E(0x80 | r << 3 | (hl ? 7 : 4)); EW(0xA000);               // mov r, [bx or si - 6000h]
        }
        return;
    }
    clob();
    uint16_t cdone = 0;
    if (cur_cart) {
        uint16_t cj = cart_open();
        E(0x8A); E(0x06 | r << 3); EW(cur_cart);    // mov r, es:[..]
        cdone = cart_else(cj);
    }
    if ((COLD_MASK & 1) && !cold_off && !cur_cart) {
        uint16_t j = range_check(hl), j2 = j + (hl ? 5 : 6);
        E(0x8A); E(0x80 | r << 3 | (hl ? 7 : 4)); EW(0xA000);   // mov r, [bx or si - 6000h]
        uint16_t done = code_top;
        cold_begin();
        cold_label(j2);                                 // from E000h up: the high RAM?
        E(0x81); E(hl ? 0xFB : 0xFE); EW(0xFF80);       // cmp bx/si, FF80h
        E(0x72); E(0);
        uint16_t p = code_top;
        E(0x8A); E(0x80 | r << 3 | (hl ? 7 : 4)); EW(0xA000);
        cold_back(done);
        code[p - 1] = code_top - p;
        cold_label(j);                                  // below C000h
        uint16_t rj = rom_open(hl);
        E(0x26); E(0x8A); E(r << 3 | (hl ? 7 : 4));     // mov r, es:[bx or si]
        cold_back(done);
        code[rj - 1] = code_top - rj;
        if (hl) { E(0x89); E(0xDE); }                   // mov si, bx
        if (r == 0) {
            call_stub(ST_RD);
        } else {
            call_stub(ST_RDT);
            E(0x8A); E(0x06 | r << 3); EW(ADDR(drc_tmp));
        }
        cold_back(done);
        cold_end();
        return;
    }
    uint16_t j = range_check(hl);
    E(0x8A); E(0x80 | r << 3 | (hl ? 7 : 4)); EW(0xA000);   // mov r, [bx or si - 6000h]
    uint16_t done = slow_starts(j, hl, 0);
    uint16_t rj = rom_open(hl);
    E(0x26); E(0x8A); E(r << 3 | (hl ? 7 : 4));     // mov r, es:[bx or si]
    uint16_t rdone = rom_else(rj);
    uint16_t kdone = 0;
    if (drc_cartp && !cdone) {
        uint16_t k = cartp_open(hl);
        E(0x26); E(0x8A); E(r << 3 | (hl ? 7 : 4)); // mov r, es:[bx or si]
        kdone = cartp_else(k);
    }
    if (hl) { E(0x89); E(0xDE); }               // mov si, bx
    if (r == 0) {
        call_stub(ST_RD);
    } else {
        call_stub(ST_RDT);
        E(0x8A); E(0x06 | r << 3); EW(ADDR(drc_tmp));
    }
    SLOW_ENDS(done);
    SLOW_ENDS(rdone);
    if (cdone) SLOW_ENDS(cdone);
    if (kdone) SLOW_ENDS(kdone);
}

// the byte at HL (hl) or at SI = r, or = imm when r is FFh; next: the
// address after this instruction
static void write_from(uint8_t r, uint8_t imm, uint8_t hl, uint16_t next) {
    if (cur_dir) {
        if (r != 0xFF) { E(0x88); E(0x06 | r << 3); EW(cur_dir); }      // mov [..], r
        else { E(0xC6); E(0x06); EW(cur_dir); E(imm); }
        return;
    }
    if (cur_cls == 1 && !cur_cart) {
        if (r != 0xFF) { E(0x88); E(0x80 | r << 3 | (hl ? 7 : 4)); EW(0xA000); }
        else { E(0xC6); E(0x80 | (hl ? 7 : 4)); EW(0xA000); E(imm); }
        return;
    }
    if (cur_cls == 2 && !cur_cart) {
        // the video memory for sure: its routine, which comes back here
        // (nothing it does can need the dispatcher)
        clob();
        if (r != 0xFF) { E(0x88); E(0x06 | r << 3); EW(ADDR(drc_tmp)); }
        else { E(0xC6); E(0x06); EW(ADDR(drc_tmp)); E(imm); }
        if (hl) { E(0x89); E(0xDE); }
        call_stub(ST_WRV);
        return;
    }
    mat();
    // (N is stored where the routine is called, which may leave the block:
    // the write to the work RAM itself does not need it - three cycles and
    // a half at every write through a pointer)
    if ((COLD_MASK & 2) && !cold_off && !cur_cart) nflush();
    uint16_t cdone = 0;
    if (cur_cart) {
        uint16_t cj = cart_open();
        if (r != 0xFF) { E(0x88); E(0x06 | r << 3); EW(cur_cart); }
        else { E(0xC6); E(0x06); EW(cur_cart); E(imm); }
        cdone = cart_else(cj);
    }
    if ((COLD_MASK & 2) && !cold_off && !cur_cart) {
        uint16_t j = range_check(hl), j2 = j + (hl ? 5 : 6);
        if (r != 0xFF) { E(0x88); E(0x80 | r << 3 | (hl ? 7 : 4)); EW(0xA000); }
        else { E(0xC6); E(0x80 | (hl ? 7 : 4)); EW(0xA000); E(imm); }
        uint16_t done = code_top;
        cold_begin();
        cold_label(j2);                                 // from E000h up: the high RAM but IE?
        E(0x81); E(hl ? 0xFB : 0xFE); EW(0xFF80);       // cmp bx/si, FF80h
        E(0x72); E(0);
        uint16_t p1 = code_top;
        E(0x81); E(hl ? 0xFB : 0xFE); EW(0xFFFF);
        E(0x74); E(0);
        uint16_t p2 = code_top;
        if (r != 0xFF) { E(0x88); E(0x80 | r << 3 | (hl ? 7 : 4)); EW(0xA000); }
        else { E(0xC6); E(0x80 | (hl ? 7 : 4)); EW(0xA000); E(imm); }
        cold_back(done);
        code[p1 - 1] = code_top - p1;
        code[p2 - 1] = code_top - p2;
        cold_label(j);                                  // below C000h
        if (r != 0xFF) { E(0x88); E(0x06 | r << 3); EW(ADDR(drc_tmp)); }
        else { E(0xC6); E(0x06); EW(ADDR(drc_tmp)); E(imm); }
        if (hl) { E(0x89); E(0xDE); }
        call_stub(ST_WRT);
        EW(next);
        cold_back(done);
        cold_end();
        return;
    }
    uint16_t j = range_check(hl);
    if (r != 0xFF) {
        E(0x88); E(0x80 | r << 3 | (hl ? 7 : 4)); EW(0xA000);
    } else {
        E(0xC6); E(0x80 | (hl ? 7 : 4)); EW(0xA000); E(imm);
    }
    uint16_t done = slow_starts(j, hl, 1);
    uint16_t kdone = 0;
    if (drc_cartp && !cdone) {
        uint16_t k = cartp_open(hl);
        E(0x26);
        if (r != 0xFF) { E(0x88); E(r << 3 | (hl ? 7 : 4)); }   // mov es:[bx or si], r
        else { E(0xC6); E(hl ? 7 : 4); E(imm); }
        kdone = cartp_else(k);
    }
    if (nst >= 0 && nst != nmem) { E(0xC6); E(0x06); EW(ADDR(drc_n)); E(nst ? 0x40 : 0); }  // (nmem as it was: the other ways have not stored it)
    if (r != 0xFF) {
        E(0x88); E(0x06 | r << 3); EW(ADDR(drc_tmp));
    } else {
        E(0xC6); E(0x06); EW(ADDR(drc_tmp)); E(imm);
    }
    if (hl) { E(0x89); E(0xDE); }
    call_stub(ST_WRT);
    EW(next);
    SLOW_ENDS(done);
    if (cdone) SLOW_ENDS(cdone);
    if (kdone) SLOW_ENDS(kdone);
}

// A = A op x, the Game Boy's operation number (ADD ADC SUB SBC AND XOR OR
// CP); what x is, is written by the caller after the opcode byte
static const uint8_t __far alu_x86[8] = { 0x00, 0x10, 0x28, 0x18, 0x20, 0x30, 0x08, 0x38 };
static void alu_done(uint8_t op) {
    fl = 1;
    hfix = op == 4 ? 2 : op >= 5 && op != 7 ? 1 : 0;
    nst = op == 2 || op == 3 || op == 7;
}

// LDH A, (n) and LD A, (FFnn)
static void io_read(uint8_t n) {
    if (n >= 0x80 || n == GB_P1 || n == GB_SB || n == GB_TMA || n == GB_LCDC || n == GB_SCY || n == GB_SCX
        || (n >= GB_LYC && n <= GB_WX) || (n >= 0x30 && n < 0x40)) {
        E(0xA0); EW(0x9F00 + n);                // mov al, [..]
        return;
    }
    clob();
    sub_cyc();
    if (n == GB_STAT && cur_wait) {
        // (the read of a loop that waits for a mode: the wait is done there)
        call_stub(ST_WAIT_STAT);
        E(cur_wait);
        return;
    }
    if (n == GB_STAT || n == GB_LY) {
        call_stub(n == GB_STAT ? ST_STAT : ST_LY);
        return;
    }
    E(0xBE); EW(0xFF00 | n);                    // mov si, FFnn
    call_stub(ST_RD);
}
// A before the instruction being translated, when the block's own code
// says it: LD A, n and then only instructions that leave A alone (-1: not
// known)
static int16_t known_a(void) {
    for (uint8_t j = cur_i; j-- > 0;) {
        uint8_t op = rb(i_pc[j]);
        if (op == 0x3E) return in_ram && tmpl_bit(i_pc[j] + 1) ? -1 : rb(i_pc[j] + 1);
        if (op == 0xAF) return 0;
        // LD r, n; LD r, A; LDH (n), A; LD (nn), A; NOP
        if (op == 0x06 || op == 0x0E || op == 0x16 || op == 0x1E || op == 0x26 || op == 0x2E
            || op == 0x47 || op == 0x4F || op == 0x57 || op == 0x5F || op == 0x67 || op == 0x6F
            || op == 0xE0 || op == 0xEA || op == 0x00) continue;
        break;
    }
    return -1;
}
// LDH (n), A and LD (FFnn), A; true: the block ends here
static uint8_t io_write(uint8_t n, uint16_t next) {
    if ((n >= 0x80 && n != 0xFF) || (n >= 0x10 && n < 0x40)) {
        E(0xA2); EW(0x9F00 + n);                // mov [..], al
        if (n == 0x14 || n == 0x19 || n == 0x1E || n == 0x23) {
            // (a channel's start is remembered: gbf_hw.c, gbf_trig)
            clob();
            E(0x84); E(0xC0);                   // test al, al
            E(0x79); E(0x05);                   // jns +5
            E(0x80); E(0x0E); EW(ADDR(gbf_trig)); E(n == 0x14 ? 1 : n == 0x19 ? 2 : n == 0x1E ? 4 : 8);
        }
        return 0;
    }
    if (n == GB_SCY || n == GB_SCX || n == GB_BGP || n == GB_WY || n == GB_WX) {
        // (the register and the frame's list of changes: gb_drc_asm.s, s_vreg)
        mat();
        nflush();
        sub_cyc();
        call_stub(ST_VREG);
        E(n);
        E(n == GB_SCX ? 2 : n == GB_SCY ? 3 : n == GB_WY ? 4 : n == GB_WX ? 5 : 6);
        return 0;
    }
    if (n == GB_P1) {
        // (A a constant, as it mostly is here: the byte P1 then reads,
        // from the table, with no call)
        int16_t k = known_a();
        if (k >= 0) {
            E(0x89); E(0xC6);                                       // mov si, ax
            E(0xA0); EW(ADDR(drc_p1_tab) + ((k >> 4) & 3));         // mov al, [..]
            E(0xA2); EW(0x9F00);                                    // mov [9F00h], al
            E(0x89); E(0xF0);                                       // mov ax, si
            return 0;
        }
        mat();
        call_stub(ST_P1);
        return 0;
    }
    mat();
    nflush();
    sub_cyc();
    E(0xA2); EW(ADDR(drc_tmp));
    E(0xBE); EW(0xFF00 | n);
    call_stub(ST_WRT);
    EW(next);
    return 0;
}

// The stack in the cartridge's RAM (where it is as the block is
// translated: Super Mario Land 2 keeps it there): "is SP in it (from lo to
// hi), is it plain RAM and enabled; ES = its segment", the fast case
// follows; returns where the three jumps to the slow case are counted from
static uint8_t sp_cart;
static uint16_t cart_sp_check(uint16_t lo, uint16_t hi) {
    E(0x81); E(0xFD); EW(lo);                   // cmp bp, lo
    E(0x72); E(0);                              // jb slow
    uint16_t j = code_top;
    E(0x81); E(0xFD); EW(hi);                   // cmp bp, hi
    E(0x77); E(0);                              // ja slow
    E(0x80); E(0x3E); EW(ADDR(drc_ram_ok)); E(1);
    E(0x75); E(0);                              // jne slow
    E(0x8E); E(0x06); EW(ADDR(drc_ram_seg));    // mov es, [drc_ram_seg]
    return j;
}
static uint16_t cart_sp_slow(uint16_t j) {
    E(0xEB); E(0);
    uint16_t done = code_top;
    code[j - 1] = code_top - j;
    code[j + 5] = code_top - (j + 6);
    code[j + 12] = code_top - (j + 13);
    return done;
}

// push the word in the x86 register r (0xFF: the constant v)
static void push16(uint8_t r, uint16_t v) {
    clob();
    if (sp_cart) {
        uint16_t j = cart_sp_check(0xA002, 0xC000);
        if (r != 0xFF) { E(0x26); E(0x89); E(0x46 | r << 3); E(0xFE); }     // mov es:[bp - 2], r
        else { E(0x26); E(0xC7); E(0x46); E(0xFE); EW(v); }
        E(0x8D); E(0x6E); E(0xFE);              // lea bp, [bp - 2]
        uint16_t done = cart_sp_slow(j);
        if (r != 0xFF) { E(0x89); E(0xC6 | r << 3); }
        else { E(0xBE); EW(v); }
        call_stub(ST_PUSH);
        SLOW_ENDS(done);
        return;
    }
    E(0x81); E(0xFD); EW(0xC002);               // cmp bp, C002h
    E(0x72); E(0);
    uint16_t j = code_top;
    if (r != 0xFF) { E(0x89); E(0x86 | r << 3); EW(0x9FFE); }      // mov [bp - 6002h], r
    else { E(0xC7); E(0x86); EW(0x9FFE); EW(v); }
    E(0x8D); E(0x6E); E(0xFE);                  // lea bp, [bp - 2]
    if ((COLD_MASK & 4) && !cold_off) {
        uint16_t done = code_top;
        cold_begin();
        cold_label(j);
        if (r != 0xFF) { E(0x89); E(0xC6 | r << 3); }               // mov si, r
        else { E(0xBE); EW(v); }
        call_stub(ST_PUSH);
        cold_back(done);
        cold_end();
        return;
    }
    E(0xEB); E(0);
    uint16_t done = code_top;
    code[j - 1] = code_top - j;
    if (r != 0xFF) { E(0x89); E(0xC6 | r << 3); }                   // mov si, r
    else { E(0xBE); EW(v); }
    call_stub(ST_PUSH);
    SLOW_ENDS(done);
}
// pop a word into the x86 register r (6: SI)
static void pop16(uint8_t r) {
    clob();
    if (sp_cart) {
        uint16_t j = cart_sp_check(0xA000, 0xBFFE);
        E(0x26); E(0x8B); E(0x46 | r << 3); E(0x00);    // mov r, es:[bp]
        E(0x8D); E(0x6E); E(0x02);              // lea bp, [bp + 2]
        uint16_t done = cart_sp_slow(j);
        call_stub(ST_POP);
        if (r != 6) { E(0x89); E(0xF0 | r); }
        SLOW_ENDS(done);
        return;
    }
    E(0x81); E(0xFD); EW(0xC000);               // cmp bp, C000h
    E(0x72); E(0);
    uint16_t j = code_top;
    E(0x8B); E(0x86 | r << 3); EW(0xA000);      // mov r, [bp - 6000h]
    E(0x8D); E(0x6E); E(0x02);                  // lea bp, [bp + 2]
    if ((COLD_MASK & 4) && !cold_off) {
        uint16_t done = code_top;
        cold_begin();
        cold_label(j);
        call_stub(ST_POP);
        if (r != 6) { E(0x89); E(0xF0 | r); }   // mov r, si
        cold_back(done);
        cold_end();
        return;
    }
    E(0xEB); E(0);
    uint16_t done = code_top;
    code[j - 1] = code_top - j;
    call_stub(ST_POP);
    if (r != 6) { E(0x89); E(0xF0 | r); }       // mov r, si
    SLOW_ENDS(done);
}

// a conditional jump over what is done when the Game Boy's condition (NZ
// Z NC C) holds; returns where its distance goes
static uint16_t unless(uint8_t c) {
    static const uint8_t __far inv[4] = { 0x74, 0x75, 0x72, 0x73 };     // jz, jnz, jc, jnc
    to_flags();
    E(inv[c]); E(0);
    return code_top;
}

// a loop that waits, about to go round again: the time nothing can change
// in is passed at once. On memory: to the next event (only an interrupt
// can change it). On LY or STAT: to the next line or mode
static void wait_skip(void) {
    mat();
    if (idle == 1) { E(0xC7); E(0x06); EW(ADDR(gbf_cyc)); EW(0); }
    else if (idle == 4) call_stub(ST_SKIP_HL);
    else if (idle == 3 && idle_modes) { }       // (its read of STAT does the waiting: io_read)
    else call_stub(idle == 2 ? ST_SKIP_LINE : ST_SKIP_MODE);
}

__attribute__((noinline)) static void translate_block(void) {
    uint8_t ended = 0;
    uint16_t block_start = code_top;
    fl = 0;
    hfix = 0;
    nst = nmem = -1;
    cyc = 0;
    after_ei = 0;
    // (the loops that copy bytes: gb_drc_asm.s, s_copy0-4)
    // (code in RAM: its jumps to its own start go there at once. Joined
    // by the dispatcher, as until patch 168, a form of the code could be
    // joined to another form, the one in RAM when the jump was first
    // taken, and went to it ever after without the bytes being compared:
    // R-Type II's routine in RAM that sets LYC for its status bar)
    loop_body = in_ram ? block_start : 0;
    if (!in_ram) {
        static const uint8_t __far copy_loops[5][9] = {
            { 6, 0x2A, 0x12, 0x13, 0x05, 0x20, 0xFA },
            { 6, 0x1A, 0x22, 0x13, 0x05, 0x20, 0xFA },
            { 8, 0x2A, 0x12, 0x13, 0x0B, 0x78, 0xB1, 0x20, 0xF8 },
            { 8, 0x1A, 0x22, 0x13, 0x0B, 0x78, 0xB1, 0x20, 0xF8 },
            { 6, 0x0A, 0x03, 0x22, 0x15, 0x20, 0xFA },
        };
        // (they start with LD A, (HL+), LD A, (DE) or LD A, (BC))
        uint8_t first = rb(blk_pc);
        if (first == 0x2A || first == 0x1A || first == 0x0A)
        for (uint8_t k = 0; k < 5; k++) {
            uint8_t len = copy_loops[k][0], j = 0;
            while (j < len && rb(blk_pc + j) == copy_loops[k][1 + j]) j++;
            if (j == len) {
                call_stub(ST_COPY0 + k * 3);
                break;
            }
        }
        if (rb(blk_pc) == 0x22 && (rb(blk_pc + 1) == 0x05 || rb(blk_pc + 1) == 0x15) && rb(blk_pc + 2) == 0x20 && rb(blk_pc + 3) == 0xFC)
            call_stub(rb(blk_pc + 1) == 0x05 ? ST_FILL0 : ST_FILL1);
    }
    // (a countdown of BC, a program's delay: gb_drc_asm.s, s_delay_bc)
    if (!in_ram && n_ins >= 4 && rb(blk_pc) == 0x0B && rb(blk_pc + 1) == 0x78 && rb(blk_pc + 2) == 0xB1
        && rb(blk_pc + 3) == 0x20 && rb(blk_pc + 4) == 0xFB)
        call_stub(ST_DELAY_BC);
    if (code_top != block_start) loop_body = code_top;
    uint8_t prev_ei = 0, far_end = 0;
    uint16_t far_patch = 0;
    // (the instruction's bytes straight from tbuf while three are there)
    uint16_t tl2 = tlen > 2 ? tlen - 2 : 0;
    cur_dir = cur_cart = 0;
    cur_cls = 0;
    for (uint8_t i = 0; i < n_ins && !ended; i++) {
        // (the slow ways not yet placed: here, with a jump over them, when
        // the first test that goes to them would be more than a short
        // jump back after this instruction)
        uint16_t pc = i_pc[i], next = i_pc[i + 1];
        // (an instruction's code is under 80 bytes; with more allowed
        // before the next - 90 where it is a short one - more blocks had
        // to be translated again the old way, and ZAS was slower)
        if (cold_n && code_top - cold_first > 40) cold_flush(1);
        uint8_t op, b1, b2;
        {
            uint16_t o = pc - tbase;
            if (o < tl2) {
                const uint8_t *t = tbuf + o;
                op = t[0];
                b1 = t[1]; b2 = t[2];
            } else { op = rb(pc); b1 = rb(pc + 1); b2 = rb(pc + 2); }
        }
        if (far_end) {
            if (i == far_end) {
                // (the seven instructions done here: their cycles, and
                // where s_far's own doing of them comes back to)
                sub_cyc();
                uint16_t d = code_top - far_patch;
                code[far_patch - 2] = d;
                code[far_patch - 1] = d >> 8;
                far_end = 0;
            }
        } else if (op == 0xEA && !in_ram && !prev_ei && i + 7 < n_ins && i_pc[i + 7] == pc + 16 && pc + 16 <= 0x4000) {
            // a byte fetched from another bank (gb_drc_asm.s, s_far)
            uint16_t sh = b1 | (uint16_t) b2 << 8, bk = rb(pc + 4) | (uint16_t) rb(pc + 5) << 8;
            uint8_t o1 = rb(pc + 6), o2 = rb(pc + 7);
            uint8_t v = o1 == 0x7E && o2 == 0x12 ? 0 : o1 == 0x2A && o2 == 0x02 ? 1 : o1 == 0x7E && o2 == 0x02 ? 2 : 0xFF;
            if (v != 0xFF && sh >= 0xA000 && sh < 0xC000 && rb(pc + 3) == 0xEA && rb(pc + 8) == 0x3E
                && rb(pc + 10) == 0xEA && rb(pc + 11) == b1 && rb(pc + 12) == b2
                && rb(pc + 13) == 0xEA && rb(pc + 14) == (uint8_t) bk && rb(pc + 15) == (uint8_t) (bk >> 8)
                && (bk >> 8) >= drc_mbc_lo && (bk >> 8) < drc_mbc_top && (((bk >> 8) | drc_mbc_or) & 1)) {
                uint8_t c = 0;
                for (uint8_t k = 0; k < 7; k++) c += (i_inf[i + k] >> 2) & 7;
                mat();
                nflush();
                sub_cyc();
                call_stub(ST_FAR);
                E(v); EW(sh); E(rb(pc + 9)); E(c);
                E(0x72); E(0x03);                   // jc: the instructions themselves
                E(0xE9); EW(0);                     // jmp done
                far_patch = code_top;
                far_end = i + 7;
            }
        }
        uint16_t nn = b1 | (uint16_t) b2 << 8;
        uint8_t x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = (op >> 4) & 3;
        live = i_live[i];
        idle_first = i == 0;
        cur_i = i;
        cur_wait = 0;
        if (idle | n_stat) {
            if (i == 0 && idle == 3) cur_wait = idle_modes;
            for (uint8_t k = 0; k < n_stat; k++)
                if (stat_at[k] == i) cur_wait = stat_mask[k];
        }
        after_ei = prev_ei;         // (the instruction before was EI)
        prev_ei = 0;                // (set by EI, below)
        uint8_t inf = i_inf[i];
        if (inf & 0x80) {
        if (op == 0x76 && !after_ei) {
            // HALT with no interrupt asked for (else the interpreter's: it
            // may be the one that reads the next byte twice): halted, and
            // out by the routine that waits (gb_drc_asm.s, s_exit)
            mat();
            nflush();
            sub_cyc();
            E(0x50);                                            // push ax
            E(0xA0); EW(0x9FFF);                                // mov al, [IE]
            E(0x22); E(0x06); EW(0x9F0F);                       // and al, [IF]
            E(0xA8); E(0x1F);                                   // test al, 1Fh
            E(0x58);                                            // pop ax
            E(0x74); E(14);                                     // jz halt
            E(0xC7); E(0x06); EW(ADDR(gb) + GB_PC); EW(pc);
            E(0xC6); E(0x06); EW(ADDR(drc_interp)); E(1);
            jmp_stub(ST_EXIT);
            E(0x83); E(0x2E); EW(ADDR(gbf_cyc)); E(1);          // halt: sub word [gbf_cyc], 1
            E(0xC6); E(0x06); EW(ADDR(gb) + GB_HALTED); E(1);
            E(0xC7); E(0x06); EW(ADDR(gb) + GB_PC); EW(next);
            E(0xC7); E(0x06); EW(ADDR(drc_link)); EW(0);
            jmp_stub(ST_EXIT);
            ended = 1;
            break;
        }
            exit_interp(pc);
            ended = 1;
            break;
        }
        // (the instruction's cycles are counted before it: an exit inside
        // it has them)
        cyc += (inf >> 2) & 7;
        if (i >= i_known) {
            if (x == 0 && z == 2 && y < 4) {        // (through BC or DE)
                cur_dir = i_dir[y >> 1][i];
                cur_cls = i_cls[y >> 1][i];
            } else {
                cur_dir = i_dir[2][i];
                cur_cls = i_cls[2][i];
            }
            cur_cart = 0;
            if (cur_dir >= 0xA000) { cur_cart = cur_dir; cur_dir = 0; }
        }
        if (x == 1) {
            if (z == 6) read_to(R8[y], 1);
            else if (y == 6) write_from(R8[z], 0, 1, next);
            else if (y != z) { E(0x88); E(0xC0 | R8[z] << 3 | R8[y]); }
            continue;
        }
        if (x == 2) {
            if (z == 6 && cur_dir) {
                if (y == 1 || y == 3) carry_in();
                E(alu_x86[y] | 2); E(0x06); EW(cur_dir);        // op al, [..]
            } else if (z == 6 && cur_cls && !cur_cart) {
                if (cur_cls >= 3) { E(0x8E); E(0x06); EW(ADDR(drc_segtab) + (cur_cls == 4 ? 2 : 0)); }
                if (y == 1 || y == 3) carry_in();
                if (cur_cls >= 3) { E(0x26); E(alu_x86[y] | 2); E(0x07); }      // op al, es:[bx]
                else { E(alu_x86[y] | 2); E(0x87); EW(0xA000); }                // op al, [bx - 6000h]
            } else if (z == 6) {
                clob();
                uint16_t cdone = 0;
                if (cur_cart) {
                    // (ADC and SBC take C from AH, which the test would
                    // lose: the usual way for those)
                    if (y != 1 && y != 3) {
                        uint16_t cj = cart_open();
                        E(alu_x86[y] | 2); E(0x06); EW(cur_cart);
                        cdone = cart_else(cj);
                    }
                }
                if ((COLD_MASK & 1) && !cold_off && !cur_cart) {
                    uint16_t j = range_check(1), j2 = j + 5;
                    if (y == 1 || y == 3) carry_in();
                    E(alu_x86[y] | 2); E(0x87); EW(0xA000);     // op al, [bx - 6000h]
                    uint16_t done = code_top;
                    cold_begin();
                    cold_label(j2);
                    E(0x81); E(0xFB); EW(0xFF80);               // cmp bx, FF80h
                    E(0x72); E(0);
                    uint16_t p = code_top;
                    if (y == 1 || y == 3) carry_in();
                    E(alu_x86[y] | 2); E(0x87); EW(0xA000);
                    cold_back(done);
                    code[p - 1] = code_top - p;
                    cold_label(j);
                    uint16_t rj = rom_open(1);
                    if (y == 1 || y == 3) carry_in();
                    E(0x26); E(alu_x86[y] | 2); E(0x07);        // op al, es:[bx]
                    cold_back(done);
                    code[rj - 1] = code_top - rj;
                    E(0x89); E(0xDE);
                    call_stub(ST_RDT);
                    if (y == 1 || y == 3) carry_in();
                    E(alu_x86[y] | 2); E(0x06); EW(ADDR(drc_tmp));
                    cold_back(done);
                    cold_end();
                    alu_done(y);
                    continue;
                }
                uint16_t j = range_check(1);
                if (y == 1 || y == 3) carry_in();
                E(alu_x86[y] | 2); E(0x87); EW(0xA000);         // op al, [bx - 6000h]
                uint16_t done = slow_starts(j, 1, 0);
                uint16_t rj = rom_open(1);
                if (y == 1 || y == 3) carry_in();
                E(0x26); E(alu_x86[y] | 2); E(0x07);            // op al, es:[bx]
                uint16_t rdone = rom_else(rj);
                E(0x89); E(0xDE);
                call_stub(ST_RDT);
                if (y == 1 || y == 3) carry_in();
                E(alu_x86[y] | 2); E(0x06); EW(ADDR(drc_tmp));
                SLOW_ENDS(done);
                SLOW_ENDS(rdone);
                if (cdone) SLOW_ENDS(cdone);
            } else {
                if (y == 1 || y == 3) carry_in();
                E(alu_x86[y] | 2); E(0xC0 | R8[z]);              // op al, r
            }
            alu_done(y);
            continue;
        }
        if (x == 0) {
            switch (z) {
            case 0:
                if (y == 0) break;                              // NOP
                if (y == 3) {                                   // JR e
                    if (operand(pc + 1, 1)) {
                        mat();
                        nflush();
                        E(0x50);                                // push ax
                        E(0xA0); EW(pc + 1 - 0x6000);           // mov al, [e]
                        E(0x98);                                // cbw
                        E(0x05); EW(next);                      // add ax, next
                        E(0x89); E(0xC6);                       // mov si, ax
                        E(0x58);                                // pop ax
                        exit_dyn(0);
                        ended = 1;
                        break;
                    }
                    if (idle && i == idle_i && (uint16_t) (next + (int8_t) b1) == blk_pc) wait_skip();
                    exit_to(next + (int8_t) b1, 0);
                    ended = 1;
                } else {                                        // JR cc, e
                    uint16_t j = unless(y - 4);
                    uint8_t s_fl = fl, s_hfix = hfix;
                    int8_t s_nmem = nmem;
                    if (idle && i == idle_i && (uint16_t) (next + (int8_t) b1) == blk_pc) wait_skip();
                    exit_to(next + (int8_t) b1, 1);
                    fl = s_fl; hfix = s_hfix; nmem = s_nmem;
                    code[j - 1] = code_top - j;
                }
                break;
            case 1:
                if (!(op & 8)) {                                // LD rr, nn
                    if (operand(pc + 1, 2)) { E(0x8B); E(0x06 | R16[p] << 3); EW(pc + 1 - 0x6000); }     // mov rr, [nn]
                    else { E(0xB8 | R16[p]); EW(nn); }
                } else {                                        // ADD HL, rr: Z kept, H and C the high byte's
                    uint8_t r = R16[p];     // CX, DX or BX: the low byte is register r, the high r + 4
                    // (H and C not looked at before they are next set -
                    // an address worked out by ADD HL, HL five times over,
                    // ZAS's - : the sum and no more; Z is in AH or will be)
                    if (!((i + 1 < n_ins ? i_live[i + 1] : FALL) & (FH | FC))) {
                        clob();
                        E(0x01); E(0xC0 | r << 3 | 3);          // add bx, rr
                        nst = 0;
                        break;
                    }
                    // (C needed, H and Z not: the sum's carry is it)
                    if (((i + 1 < n_ins ? i_live[i + 1] : FALL) & FALL) == FC) {
                        fl = 0;
                        E(0x01); E(0xC0 | r << 3 | 3);          // add bx, rr
                        E(0x9F);                                // lahf
                        E(0x80); E(0xE4); E(0x01);              // and ah, 1
                        nst = 0;
                        break;
                    }
                    mat();
                    E(0x89); E(0xC7);                           // mov di, ax
                    E(0x00); E(0xC0 | r << 3 | 3);              // add bl, r.low
                    E(0x10); E(0xC0 | (r + 4) << 3 | 7);        // adc bh, r.high
                    E(0x9F);                                    // lahf
                    E(0x80); E(0xE4); E(0x11);                  // and ah, 11h
                    E(0x81); E(0xE7); EW(0x4000);               // and di, 4000h
                    E(0x09); E(0xF8);                           // or ax, di
                    nst = 0;
                }
                break;
            case 2:
                if (y < 4) {                                    // (BC), (DE)
                    E(0x89); E(y < 2 ? 0xCE : 0xD6);            // mov si, cx / dx
                    if (y & 1) read_to(0, 0);
                    else write_from(0, 0, 0, next);
                } else if (y & 1) {                             // LD A, (HL+) / (HL-)
                    read_to(0, 1);
                    E(0x8D); E(0x5F); E(y < 6 ? 0x01 : 0xFF);   // lea bx, [bx +- 1]
                } else {
                    // LD (HL+), A / (HL-): HL moved first (the write may
                    // leave the block for the next instruction)
                    E(0x89); E(0xDE);                           // mov si, bx
                    E(0x8D); E(0x5F); E(y < 6 ? 0x01 : 0xFF);
                    write_from(0, 0, 0, next);
                }
                break;
            case 3: {                                           // INC rr, DEC rr: no flags
                uint8_t d = (op & 8) ? 0xFF : 0x01;
                // (the V30MZ's own INC and DEC change its flags: only
                // when the Game Boy's are in AH already, or not needed)
                if (!fl || !live) {
                    fl = 0;
                    E(((op & 8) ? 0x48 : 0x40) | R16[p]);       // inc / dec rr
                    break;
                }
                if (p == 2) { E(0x8D); E(0x5F); E(d); }
                else if (p == 3) { E(0x8D); E(0x6E); E(d); }
                else {
                    E(0x89); E(p == 0 ? 0xCE : 0xD6);           // mov si, cx / dx
                    E(0x8D); E(p == 0 ? 0x4C : 0x54); E(d);     // lea cx / dx, [si +- 1]
                }
                break;
            }
            case 4: case 5:
                if (y == 6 && cur_dir) {
                    carry_in();
                    E(0xFE); E(z == 4 ? 0x06 : 0x0E); EW(cur_dir);     // inc / dec byte [..]
                    fl = 1;
                    hfix = 0;
                } else if (y == 6 && cur_cls == 1 && !cur_cart) {
                    carry_in();
                    E(0xFE); E(z == 4 ? 0x87 : 0x8F); EW(0xA000);       // inc / dec byte [bx - 6000h]
                    fl = 1;
                    hfix = 0;
                } else if (y == 6) {
                    mat();
                    nflush();
                    call_stub(z == 4 ? ST_INC_HL : ST_DEC_HL);
                    EW(next);
                } else if (!((i + 1 < n_ins ? i_live[i + 1] : FALL) & (FZ | FH))) {
                    // Z and H set again before anything reads them: the
                    // V30MZ's inc / dec and nothing else - it leaves CF
                    // alone, and AH (C, when the flags are there)
                    E(0xFE); E((z == 4 ? 0xC0 : 0xC8) | R8[y]);
                } else {
                    carry_in();
                    E(0xFE); E((z == 4 ? 0xC0 : 0xC8) | R8[y]);
                    fl = 1;
                    hfix = 0;
                }
                nst = z == 5;
                break;
            case 6:
                if (y == 6) write_from(0xFF, b1, 1, next);
                else if (operand(pc + 1, 1)) { E(0x8A); E(0x06 | R8[y] << 3); EW(pc + 1 - 0x6000); }       // mov r, [n]
                else { E(0xB0 | R8[y]); E(b1); }
                break;
            default:
                if (y < 4) {                                    // RLCA RRCA RLA RRA
                    if (y >= 2) carry_in();
                    E(0xD0); E(0xC0 | y << 3);
                    E(0x9F);
                    E(0x80); E(0xE4); E(0x01);                  // lahf; and ah, 1
                    fl = 0;
                    nst = 0;
                } else if (y == 4) {                            // DAA
                    mat();
                    nflush();
                    call_stub(ST_DAA);
                } else if (y == 5) {                            // CPL
                    E(0xF6); E(0xD0);                           // not al
                    if (fl) hfix = 2;
                    else { E(0x80); E(0xCC); E(0x10); }
                    nst = 1;
                } else if (y == 6) {                            // SCF
                    if (fl) { E(0xF9); hfix = 1; }
                    else { E(0x80); E(0xE4); E(0x40); E(0x80); E(0xCC); E(0x01); }
                    nst = 0;
                } else {                                        // CCF
                    if (fl) { E(0xF5); hfix = 1; }
                    else { E(0x80); E(0xE4); E(0x41); E(0x80); E(0xF4); E(0x01); }
                    nst = 0;
                }
                break;
            }
            continue;
        }
        // C0h-FFh
        switch (z) {
        case 0:
            if (y < 4) {                                        // RET cc
                uint16_t j = unless(y);
                uint8_t s_fl = fl, s_hfix = hfix;
                int8_t s_nmem = nmem;
                pop16(6);
                exit_dyn(3);
                fl = s_fl; hfix = s_hfix; nmem = s_nmem;
                code[j - 1] = code_top - j;
            } else if (y == 4) {
                ended = io_write(b1, next);
            } else {
                io_read(b1);
            }
            break;
        case 1:
            if (!(op & 8)) {                                    // POP
                if (p == 3) {
                    clob();
                    call_stub(ST_POP_AF);
                    fl = 0;
                    nst = nmem = -1;
                } else {
                    pop16(R16[p]);
                }
            } else if (p == 0 || p == 1) {                      // RET, RETI
                pop16(6);
                if (p == 1) {
                    // RETI: by the dispatcher, like what follows EI: an
                    // interrupt that has been waiting is taken now. (Until
                    // patch 168 the return went on by the table, and the
                    // interrupt waited for the next event: R-Type II's
                    // LYC interrupt at line 144, behind the vertical
                    // blank's, was taken at the next frame's first line
                    // instead of line 150, and when that was too late the
                    // status bar was drawn with the playfield's scroll.)
                    // (Only then: the other returns go on by the table.)
                    E(0xC6); E(0x06); EW(ADDR(gb.ime)); E(1);
                    mat();
                    nflush();
                    sub_cyc();
                    E(0x50);                                    // push ax
                    E(0xA0); EW(ADDR(gb_ie));                   // mov al, [gb_ie]
                    E(0x22); E(0x06); EW(0x9F0F);               // and al, [IF]
                    E(0xA8); E(0x1F);                           // test al, 1Fh
                    E(0x58);                                    // pop ax
                    E(0x74); E(0x07);                           // jz +7
                    E(0x89); E(0x36); EW(ADDR(gb) + GB_PC);     // mov [gb.pc], si
                    jmp_stub(ST_EXIT);
                }
                exit_dyn(0);
                ended = 1;
            } else if (p == 2) {                                // JP HL
                E(0x89); E(0xDE);
                exit_dyn(0);
                ended = 1;
            } else {                                            // LD SP, HL
                E(0x89); E(0xDD);
            }
            break;
        case 2:
            if (y < 4) {                                        // JP cc, nn
                uint16_t j = unless(y);
                uint8_t s_fl = fl, s_hfix = hfix;
                int8_t s_nmem = nmem;
                if (idle && i == idle_i && nn == blk_pc) wait_skip();
                exit_to(nn, 1);
                fl = s_fl; hfix = s_hfix; nmem = s_nmem;
                code[j - 1] = code_top - j;
            } else if (y == 4 || y == 6) {                      // (FF00 + C)
                clob();
                E(0x89); E(0xCE);                               // mov si, cx
                E(0x81); E(0xCE); EW(0xFF00);                   // or si, FF00h
                if (y == 6) {
                    call_stub(ST_RD);
                } else {
                    mat();
                    nflush();
                    E(0xA2); EW(ADDR(drc_tmp));
                    call_stub(ST_WRT);
                    EW(next);
                }
            } else if (nn >= 0xFF00) {                          // LD (nn), A / LD A, (nn): an I/O register
                if (y == 5) ended = io_write(nn & 0xFF, next);
                else io_read(nn & 0xFF);
            } else if (nn >= 0xC000 && nn < 0xFE00) {
                E(y == 5 ? 0xA2 : 0xA0); EW((nn >= 0xE000 ? nn - 0x2000 : nn) - 0x6000);
            } else if (nn >= 0xA000 && nn < 0xC000) {
                // the cartridge's RAM (some programs keep their variables
                // there): when it is plain RAM and enabled, through its
                // segment here; else the routine
                if (y == 5) { mat(); nflush(); } else clob();
                E(0x80); E(0x3E); EW(ADDR(drc_ram_ok)); E(1);   // cmp byte [drc_ram_ok], 1
                E(0x75); E(0);                                  // jne slow
                uint16_t j = code_top;
                E(0x8E); E(0x06); EW(ADDR(drc_ram_seg));        // mov es, [drc_ram_seg]
                E(0x26); E(y == 5 ? 0xA2 : 0xA0); EW(nn);       // mov es:[nn], al / mov al, es:[nn]
                E(0xEB); E(0);
                uint16_t done = code_top;
                code[j - 1] = code_top - j;
                E(0xBE); EW(nn);
                if (y == 5) {
                    E(0xA2); EW(ADDR(drc_tmp));
                    call_stub(ST_WRT);
                    EW(next);
                } else {
                    call_stub(ST_RD);
                }
                SLOW_ENDS(done);
            } else if (y == 5 && nn < 0x8000 && (nn >> 8) >= drc_mbc_lo && (nn >> 8) < drc_mbc_top
                       && (((nn >> 8) | drc_mbc_or) & 1)) {
                // the second ROM area's bank (gb_drc_asm.s: wrt_bank's own tests)
                mat();
                nflush();
                if (next <= 0x4000 && !in_ram) call_stub(ST_BANK0);
                else { call_stub(ST_BANKX); EW(next); }
            } else {
                E(0xBE); EW(nn);                                // mov si, nn
                if (y == 5) {
                    mat();
                    nflush();
                    E(0xA2); EW(ADDR(drc_tmp));
                    call_stub(ST_WRT);
                    EW(next);
                } else {
                    clob();
                    call_stub(ST_RD);
                }
            }
            break;
        case 3:
            if (y == 0 && operand(pc + 1, 2)) {                 // JP nn, nn read as the code runs
                mat();
                nflush();
                E(0x8B); E(0x36); EW(pc + 1 - 0x6000);          // mov si, [nn]
                exit_dyn(0);
                ended = 1;
            } else if (y == 0) {                                // JP nn
                if (idle && i == idle_i && nn == blk_pc) wait_skip();
                exit_to(nn, 0);
                ended = 1;
            } else if (y == 6) {                                // DI
                E(0xC6); E(0x06); EW(ADDR(gb.ime)); E(0);
                E(0xC6); E(0x06); EW(ADDR(drc_ei)); E(0);
            } else if (y == 7) {                                // EI: after the next instruction
                E(0xC6); E(0x06); EW(ADDR(drc_ei)); E(1);
                prev_ei = 1;
            } else {                                            // CB
                cyc++;
                uint8_t cx = b1 >> 6, cy = (b1 >> 3) & 7, r = R8[b1 & 7];
                if ((b1 & 7) == 6 && cx == 1) {                 // BIT n, (HL)
                    cyc++;
                    mat();
                    E(0x80); E(0xE4); E(0x01);                  // and ah, 1
                    E(0x80); E(0xCC); E(0x10);                  // or ah, 10h
                    if (cur_dir) {
                        E(0xF6); E(0x06); EW(cur_dir); E(1 << cy);
                    } else {
                        uint16_t j = range_check(1);
                        E(0xF6); E(0x87); EW(0xA000); E(1 << cy);   // test byte [bx - 6000h], m
                        uint16_t done = slow_starts(j, 1, 0);
                        E(0x89); E(0xDE);
                        call_stub(ST_RDT);
                        E(0xF6); E(0x06); EW(ADDR(drc_tmp)); E(1 << cy);
                        SLOW_ENDS(done);
                    }
                    E(0x75); E(0x03);                           // jnz +3
                    E(0x80); E(0xCC); E(0x40);                  // or ah, 40h
                    nst = 0;
                } else if ((b1 & 7) == 6 && cur_dir) {
                    // the others on (HL), at a known place: as on a register
                    uint16_t t = cur_dir;
                    cyc += 2;
                    if (cx == 0) {
                        if (cy == 2 || cy == 3) carry_in();
                        switch (cy) {
                        case 0: case 1: case 2: case 3:
                            E(0xD0); E(cy << 3 | 0x06); EW(t);
                            E(0xFE); E(0x06); EW(t); E(0xFE); E(0x0E); EW(t);
                            break;
                        case 4: E(0xD0); E(0x26); EW(t); break;
                        case 5: E(0xD0); E(0x3E); EW(t); break;
                        case 7: E(0xD0); E(0x2E); EW(t); break;
                        default:
                            E(0xC0); E(0x06); EW(t); E(4);
                            E(0x80); E(0x0E); EW(t); E(0);
                            break;
                        }
                        fl = 1;
                        hfix = 1;
                        nst = 0;
                    } else {
                        clob();
                        E(0x80); E(cx == 2 ? 0x26 : 0x0E); EW(t); E(cx == 2 ? (uint8_t) ~(1 << cy) : 1 << cy);
                    }
                } else if ((b1 & 7) == 6) {
                    // the others on (HL): in the work RAM or the high
                    // RAM where the byte is; elsewhere the byte to
                    // drc_tmp, the instruction on it there, and written
                    // back (which may leave the block, as any write
                    // through HL). The flags end in AH either way.
                    cyc += 2;
                    mat();
                    if (cx == 0) nst = 0;
                    nflush();
                    uint16_t j = range_check(1), done = 0;
                    for (uint8_t slow = 0; slow < 2; slow++) {
                        // the operand: [bx - 6000h], then [drc_tmp]
                        uint8_t rm = slow ? 0x06 : 0x87;
                        uint16_t t = slow ? ADDR(drc_tmp) : 0xA000;
                        if (slow) {
                            done = slow_starts(j, 1, 1);
                            E(0x89); E(0xDE);                   // mov si, bx
                            call_stub(ST_RDT);
                        }
                        if (cx == 0) {
                            if (cy == 2 || cy == 3) { E(0xD0); E(0xEC); }       // shr ah, 1: CF = C
                            switch (cy) {
                            case 0: case 1: case 2: case 3:     // RLC, RRC, RL, RR
                                E(0xD0); E(cy << 3 | rm); EW(t);
                                E(0xFE); E(rm); EW(t); E(0xFE); E(0x08 | rm); EW(t);    // inc, dec: ZF, CF kept
                                break;
                            case 4: E(0xD0); E(0x20 | rm); EW(t); break;    // SLA
                            case 5: E(0xD0); E(0x38 | rm); EW(t); break;    // SRA
                            case 7: E(0xD0); E(0x28 | rm); EW(t); break;    // SRL
                            default:                            // SWAP
                                E(0xC0); E(rm); EW(t); E(4);    // rol byte [..], 4
                                E(0x80); E(0x08 | rm); EW(t); E(0);     // or byte [..], 0
                                break;
                            }
                            E(0x9F); E(0x80); E(0xE4); E(0xEF); // lahf; and ah, EFh (H clear)
                        } else {                                // RES, SET
                            E(0x80); E((cx == 2 ? 0x20 : 0x08) | rm); EW(t); E(cx == 2 ? (uint8_t) ~(1 << cy) : 1 << cy);
                        }
                        if (slow) {
                            E(0x89); E(0xDE);
                            call_stub(ST_WRT);
                            EW(next);
                        }
                    }
                    SLOW_ENDS(done);
                    fl = 0;
                } else if (cx == 0) {
                    switch (cy) {
                    case 0: case 1:                             // RLC, RRC
                        E(0xD0); E(0xC0 | cy << 3 | r);
                        E(0xFE); E(0xC0 | r); E(0xFE); E(0xC8 | r);    // inc r; dec r: ZF, CF kept
                        break;
                    case 2: case 3:                             // RL, RR
                        carry_in();
                        E(0xD0); E(0xC0 | cy << 3 | r);
                        E(0xFE); E(0xC0 | r); E(0xFE); E(0xC8 | r);
                        break;
                    case 4: E(0xD0); E(0xE0 | r); break;        // SLA
                    case 5: E(0xD0); E(0xF8 | r); break;        // SRA
                    case 7: E(0xD0); E(0xE8 | r); break;        // SRL
                    default:                                    // SWAP
                        E(0xC0); E(0xC0 | r); E(4);             // rol r, 4
                        E(0x08); E(0xC0 | r << 3 | r);          // or r, r
                        break;
                    }
                    fl = 1;
                    hfix = 1;
                    nst = 0;
                } else if (cx == 1) {                           // BIT: Z, H set, C kept
                    mat();
                    E(0x80); E(0xE4); E(0x01);
                    E(0x80); E(0xCC); E(0x10);
                    E(0xF6); E(0xC0 | r); E(1 << cy);           // test r, m
                    E(0x75); E(0x03);
                    E(0x80); E(0xCC); E(0x40);
                    nst = 0;
                } else {                                        // RES, SET
                    clob();
                    E(0x80); E((cx == 2 ? 0xE0 : 0xC8) | r); E(cx == 2 ? (uint8_t) ~(1 << cy) : 1 << cy);
                }
            }
            break;
        case 4: {                                               // CALL cc, nn
            uint16_t j = unless(y);
            uint8_t s_fl = fl, s_hfix = hfix;
            int8_t s_nmem = nmem;
            push16(0xFF, next);
            exit_to(nn, 3);
            fl = s_fl; hfix = s_hfix; nmem = s_nmem;
            code[j - 1] = code_top - j;
            break;
        }
        case 5:
            if (!(op & 8)) {                                    // PUSH
                if (p == 3) {
                    mat();
                    nflush();
                    call_stub(ST_PUSH_AF);
                } else {
                    push16(R16[p], 0);
                }
            } else {                                            // CALL nn
                push16(0xFF, next);
                if (operand(pc + 1, 2)) {
                    mat();
                    nflush();
                    E(0x8B); E(0x36); EW(pc + 1 - 0x6000);      // mov si, [nn]
                    exit_dyn(0);
                } else {
                    exit_to(nn, 0);
                }
                ended = 1;
            }
            break;
        case 6:
            if (y == 1 || y == 3) carry_in();
            E(alu_x86[y] | 4); E(b1);                           // op al, n
            alu_done(y);
            break;
        default:                                                // RST
            push16(0xFF, next);
            exit_to(y * 8, 0);
            ended = 1;
            break;
        }
        // (EI: the instruction after it, then the dispatcher, which
        // enables the interrupts)
        if (!ended && after_ei) {
            exit_to(next, 0);
            ended = 1;
        }
    }
    after_ei = 0;
    if (!ended) exit_to(i_pc[n_ins], 0);
    cold_flush(0);
}

// a loop that waits on STAT (its instructions s to n - 1, the last the
// jump back): the modes it goes round in, if the mode alone decides (the
// same whatever STAT's other bits are); 0 if not, or if it is not known
static uint8_t wait_modes(uint8_t s, uint8_t n) {
    uint8_t mask = 0;
    for (uint8_t m = 0; m < 4; m++) {
        uint8_t taken[2];
        for (uint8_t v = 0; v < 2; v++) {
            uint8_t a = (v ? 0xFC : 0x80) | m, z = 0, c = 0, cknown = 0, seen = 0;
            for (uint8_t i = s; i < n; i++) {
                uint8_t op = rb(i_pc[i]), b1 = rb(i_pc[i] + 1);
                if (op == 0xF0 || op == 0xFA) {
                    if (b1 != 0x41 || seen || i != s) return 0;     // (STAT, once, first)
                    seen = 1;
                } else if (op == 0xE6) { a &= b1; z = !a; c = 0; cknown = 1; }
                else if (op == 0xF6) { a |= b1; z = !a; c = 0; cknown = 1; }
                else if (op == 0xEE) { a ^= b1; z = !a; c = 0; cknown = 1; }
                else if (op == 0xFE) { z = a == b1; c = a < b1; cknown = 1; }
                else if (op == 0xA7 || op == 0xB7) { z = !a; c = 0; cknown = 1; }
                else if (op == 0x2F) { a = ~a; }
                else if (op == 0xCB && (b1 >> 6) == 1 && (b1 & 7) == 7) { z = !(a >> ((b1 >> 3) & 7) & 1); cknown = 0; }
                else if (op == 0x00) { }
                else if (i == n - 1) {
                    uint8_t cc = (op >> 3) & 3;
                    if (cc >= 2 && !cknown) return 0;
                    taken[v] = cc == 0 ? !z : cc == 1 ? z : cc == 2 ? !c : c;
                } else {
                    return 0;
                }
            }
        }
        if (taken[0] != taken[1]) return 0;
        if (taken[0]) mask |= 1 << m;
    }
    return mask == 15 ? 0 : mask;
}

static blk_t __far *find(uint16_t pc, uint16_t key);
// the instructions of the block at pc, and the flags needed before each
// (gb_asm.s, gbd_scan_fast: the instructions scan_block has nothing of
// its own to do for, as many as come. 0: the next is scan_block's, or the
// block is full; 1: the block ends; 2: a block may start at scan_pc; 3:
// scan_cdir and scan_ccls are wanted)
uint8_t gbd_scan_fast(void);
uint16_t scan_pc, scan_tl1, scan_cdir[3];
uint8_t scan_total, scan_anyk, scan_cok, scan_ccls[3];
void gbd_scan_live(void);       // (gb_asm.s: i_live from i_fa, last to first)
uint8_t i_fa[MAXI];
#define fa i_fa
#define total scan_total
__attribute__((noinline)) static void scan_block(uint16_t pc) {
    n_ins = 0;
    scan_anyk = 0;
    i_known = 0xFF;
    uint8_t after_ei = 0;
    total = 0;
    uint16_t kv[3] = { 0, 0, 0 };
    uint8_t kn[3] = { 0, 0, 0 }, any = 0, has41 = 0;
    uint8_t clo[3], chi[3], ckn[3] = { 0, 0, 0 }, anyc = 0;
    uint16_t tl1 = tlen > 1 ? tlen - 1 : 0;
    scan_tl1 = tl1;
    while (n_ins < MAXI && total < 90) {
        if (!after_ei) {
            scan_pc = pc;
            uint8_t r = gbd_scan_fast();
            pc = scan_pc;
            if (r == 1) break;
            if (r == 2) {
                if (find(pc, pc < 0x4000 ? drc_key0 : drc_keyx)) break;
                continue;
            }
            if (r == 3) {
                // what the next instruction's i_dir and i_cls are, as the
                // lines below would make them
                for (uint8_t k = 0; k < 3; k++) {
                    scan_cdir[k] = !(any && kn[k]) ? 0
                        : (kv[k] >= 0xC000 && kv[k] < 0xE000) || (kv[k] >= 0xFF80 && kv[k] < 0xFFFF) ? kv[k] - 0x6000
                        : kv[k] >= 0xA000 && kv[k] < 0xC000 ? kv[k] : 0;
                    uint8_t lo = clo[k], hi = chi[k];
                    scan_ccls[k] = !(anyc && ckn[k]) ? 0 : lo >= 0xC0 && hi <= 0xDF ? 1
                        : lo >= 0x80 && hi <= 0x9F ? 2 : hi <= 0x3F ? 3 : lo >= 0x40 && hi <= 0x7F ? 4 : 0;
                }
                scan_cok = 1;
                continue;
            }
            if (!(n_ins < MAXI && total < 90)) break;
        }
        uint8_t op, cb;
        {
            uint16_t o = pc - tbase;
            if (o < tl1) { op = tbuf[o]; cb = tbuf[o + 1]; }
            else { op = rb(pc); cb = rb(pc + 1); }
        }
        const uint8_t *inf = op_inf[op];
        uint8_t m = inf[1];
        total += ((m >> 2) & 7) + 2;
        uint8_t f = op != 0xCB ? inf[0] : op_flags(op, cb);
        // (what is known of BC, DE, HL: nothing, and nothing kept, until
        // the block's first LD rr, nn or LD B / D / H, n - i_known)
        if (any | anyc) {
            i_dir[0][n_ins] = i_dir[1][n_ins] = i_dir[2][n_ins] = 0;
            i_cls[0][n_ins] = i_cls[1][n_ins] = i_cls[2][n_ins] = 0;
        }
        if (anyc) {
            for (uint8_t k = 0; k < 3; k++)
                if (ckn[k]) {
                    uint8_t lo = clo[k], hi = chi[k];
                    i_cls[k][n_ins] = lo >= 0xC0 && hi <= 0xDF ? 1
                        : lo >= 0x80 && hi <= 0x9F ? 2 : hi <= 0x3F ? 3 : lo >= 0x40 && hi <= 0x7F ? 4 : 0;
                }
        }
        if ((m & 0x20) && op < 0x30) {
            // LD rr, nn; LD B / D / H, n (unless read as the code runs)
            uint8_t k = op >> 4;
            uint16_t at = pc + ((op & 0x0F) == 0x01 ? 2 : 1);
            if (in_ram && tmpl_bit(at)) ckn[k] = 0;
            else { clo[k] = chi[k] = rb(at); ckn[k] = 1; anyc = 1; if (i_known == 0xFF) i_known = n_ins + 1; }
        } else if (anyc) {
            uint8_t k = (op >> 4) & 3, lowop = op & 0x0F;
            if (op < 0x30 && lowop == 0x04) { if (chi[k] == 0xFF) ckn[k] = 0; else { clo[k]++; chi[k]++; } }
            else if (op < 0x30 && lowop == 0x05) { if (clo[k] == 0) ckn[k] = 0; else { clo[k]--; chi[k]--; } }
            else if ((op < 0x30 && lowop == 0x03) || op == 0x22 || op == 0x2A) { if (chi[k] == 0xFF) ckn[k] = 0; else chi[k]++; }
            else if (op < 0x30 && lowop == 0x0B) { if (clo[k] == 0) ckn[k] = 0; else clo[k]--; }
            else if (op == 0x32 || op == 0x3A) { if (clo[2] == 0) ckn[2] = 0; else clo[2]--; }
            else if ((op & 0xCF) == 0x09 || op == 0xF8) ckn[2] = 0;                             // ADD HL, rr; LD HL, SP + e
            else if (op >= 0x40 && op < 0x70 && !(op & 0x08) && (op & 7) != ((op >> 3) & 7)) ckn[(op >> 4) & 3] = 0;   // LD B / D / H, x
            else if ((op & 0xCF) == 0xC1 && op < 0xF0) ckn[k] = 0;                              // POP rr
            else if (op == 0xCB && (cb >> 6) != 1 && (cb & 7) < 6 && !(cb & 1)) ckn[(cb & 7) >> 1] = 0;
        }
        if ((m & 0x20) && op == 0xF0 && cb == 0x41 && n_ins) has41 = 1;
        if ((m & 0x20) && (op & 0xCF) == 0x01 && !(in_ram && (tmpl_bit(pc + 1) || tmpl_bit(pc + 2)))) {
            // LD rr, nn: known from here (unless it is read as the code runs)
            uint8_t rr = op >> 4;
            kn[rr] = 1;
            kv[rr] = cb | (uint16_t) rb(pc + 2) << 8;
            any = 1;
            if (i_known == 0xFF) i_known = n_ins + 1;
        } else if (any) {
            // (only once one of them is known: most blocks never come here)
            for (uint8_t k = 0; k < 3; k++)
                if (kn[k])
                    i_dir[k][n_ins] = (kv[k] >= 0xC000 && kv[k] < 0xE000) || (kv[k] >= 0xFF80 && kv[k] < 0xFFFF) ? kv[k] - 0x6000
                        : kv[k] >= 0xA000 && kv[k] < 0xC000 ? kv[k] : 0;    // (A000h up: the cartridge's RAM, the address itself)
            // the flags again, for an access that cannot leave the block
            uint8_t ox = op >> 6, oy = (op >> 3) & 7, oz = op & 7;
            if (i_dir[2][n_ins] && i_dir[2][n_ins] < 0xA000) {
                if ((ox == 1 && oy == 6 && oz != 6) || op == 0x36 || op == 0x22 || op == 0x32) f = 0;
                else if (op == 0x34 || op == 0x35) f = FZ | FH;
                else if (op == 0xCB && (cb & 7) == 6 && (cb >> 6) != 1)
                    f = (cb >> 6) ? 0 : FALL | (((cb >> 3) & 7) == 2 || ((cb >> 3) & 7) == 3 ? RD(FC) : 0);
            }
            if ((op == 0x02 && i_dir[0][n_ins] && i_dir[0][n_ins] < 0xA000) || (op == 0x12 && i_dir[1][n_ins] && i_dir[1][n_ins] < 0xA000)) f = 0;
            // what the instruction does to BC, DE, HL
            uint8_t rr = (op >> 4) & 3;
            if (ox == 0 && oz == 3 && rr < 3) kv[rr] += (op & 8) ? -1 : 1;
            else if (op == 0x22 || op == 0x2A) kv[2]++;
            else if (op == 0x32 || op == 0x3A) kv[2]--;
            else if (ox == 0 && oz == 1 && (op & 8)) kn[2] = 0;                        // ADD HL, rr
            else if ((ox == 1 && oy < 6) || (ox == 0 && (oz == 4 || oz == 5 || oz == 6) && oy < 6)) kn[oy >> 1] = 0;   // LD r, x; INC r; DEC r; LD r, n
            else if (ox == 3 && oz == 1 && !(op & 8) && rr < 3) kn[rr] = 0;            // POP rr
            else if (op == 0xCB && (cb & 7) < 6 && (cb >> 6) != 1) kn[(cb & 7) >> 1] = 0;
            else if (op == 0xF8) kn[2] = 0;
        }
        i_pc[n_ins] = pc;
        i_inf[n_ins] = m;
        fa[n_ins++] = f;
        pc += m & 3;
        if ((f & F_END) || after_ei) break;
        if ((m & 0x20) && op == 0xFB) after_ei = 1;
        if (in_ram && pc - blk_pc > 48) break;
        // a block already starts at the next instruction: this one ends
        // there and goes on into it (a jump the dispatcher joins), not
        // through a second translation of the same code. Race Drivin'
        // jumps into the middle of its rows of unrolled instructions at
        // every place: 34,779 bytes of it translated for 22,102 there are.
        if (n_ins >= 6 && !in_ram && !after_ei && pc < 0x8000) {
            // (drc_hash0, drc_hashx: the key's part of hash(), kept by the bank's change)
            uint16_t h = (pc ^ (pc >> 6) ^ (pc < 0x4000 ? drc_hash0 : drc_hashx)) & (TABLE_N - 1);
            if ((DRC_HAS[h >> 3] & bit_of[h & 7]) && find(pc, pc < 0x4000 ? drc_key0 : drc_keyx)) break;
        }
        scan_anyk = any | anyc;
        scan_cok = 0;       // (scan_cdir, scan_ccls: made when gbd_scan_fast asks)
    }
    i_pc[n_ins] = pc;
    // a loop that waits: A loaded from memory or an I/O register, tested,
    // and a conditional jump back to the start; nothing written, no
    // register but A changed
    idle = 0;
    // (CP (HL) / JR cc to itself: a wait for the byte HL points at to be,
    // or stop being, A. Donkey Kong Land waits for a line that way, HL at
    // LY: what HL is, is seen when it runs. gb_drc_asm.s, s_skip_hl)
    if (!in_ram && n_ins >= 2 && rb(blk_pc) == 0xBE && (rb(blk_pc + 1) & 0xE7) == 0x20 && rb(blk_pc + 2) == 0xFD) {
        idle = 4;
        idle_i = 1;
        idle_modes = 0;
    } else
    if (!in_ram) {
        uint8_t kind = 1, loaded = 0;
        for (uint8_t i = 0; i < n_ins && i < 6; i++) {
            uint8_t op = rb(i_pc[i]), b1 = rb(i_pc[i] + 1);
            if ((op & 0xE7) == 0x20 || (op & 0xE7) == 0xC2 || op == 0x18 || op == 0xC3) {
                // the first jump: back to the block's start?
                uint16_t t = (op & 0xC0) ? b1 | (uint16_t) rb(i_pc[i] + 2) << 8 : i_pc[i] + 2 + (int8_t) b1;
                // (a jump to itself is a wait too: for an interrupt)
                if (t == blk_pc && (loaded || (i == 0 && (op == 0x18 || op == 0xC3)))) {
                    idle = kind;
                    idle_i = i;
                    idle_modes = kind == 3 && op != 0x18 && op != 0xC3 ? wait_modes(0, i + 1) : 0;
                }
                break;
            } else if (op == 0xF0 || op == 0xFA) {
                uint16_t a = op == 0xF0 ? 0xFF00 | b1 : b1 | (uint16_t) rb(i_pc[i] + 2) << 8;
                if (a == 0xFF04 || a == 0xFF05) break;
                if (a == 0xFF44) { if (kind < 2) kind = 2; }
                else if (a == 0xFF41) kind = 3;
                loaded = 1;
            } else if ((op >= 0xB8 && op <= 0xBF && op != 0xBE) || (op >= 0xA0 && op <= 0xB7 && (op & 7) != 6)
                || op == 0xFE || op == 0xE6 || op == 0xF6 || op == 0xEE || op == 0x00 || op == 0x2F
                || (op == 0xCB && (b1 >> 6) == 1 && (b1 & 7) == 7)) {
                // CP, AND, OR, XOR with a register or a constant, CPL, BIT n, A
                if (!loaded) break;
            } else {
                break;
            }
        }
    }
    // the same wait further on in the block (LDH A, (41h), tests, a
    // conditional jump back to that read): the read does the waiting too
    n_stat = 0;
    if (!in_ram && has41) {
        for (uint8_t s = 1; s + 1 < n_ins && n_stat < 3; s++) {
            if (rb(i_pc[s]) != 0xF0 || rb(i_pc[s] + 1) != 0x41) continue;
            for (uint8_t i = s + 1; i < n_ins && i < s + 6; i++) {
                uint8_t op = rb(i_pc[i]), b1 = rb(i_pc[i] + 1);
                if ((op & 0xE7) == 0x20 || (op & 0xE7) == 0xC2) {
                    uint16_t t = (op & 0xC0) ? b1 | (uint16_t) rb(i_pc[i] + 2) << 8 : i_pc[i] + 2 + (int8_t) b1;
                    uint8_t m = t == i_pc[s] ? wait_modes(s, i + 1) : 0;
                    if (m) {
                        stat_at[n_stat] = s;
                        stat_mask[n_stat++] = m;
                    }
                    break;
                }
                if (op == 0xE6 || op == 0xF6 || op == 0xEE || op == 0xFE || op == 0xA7 || op == 0xB7 || op == 0x2F
                    || op == 0x00 || (op == 0xCB && (b1 >> 6) == 1 && (b1 & 7) == 7)) continue;
                break;
            }
        }
    }
    gbd_scan_live();
}

#undef fa
#undef total

// A block's places in the table: the hash's, then far from it. (Until
// patch 167 the next seven: the blocks of one routine, a few bytes apart,
// have neighbouring first places and soon filled each other's; Donkey
// Kong Land forgot everything with 1,247 blocks of 8,192.)
#define PROBES 12
#define PLACE(h, i) (((h) ^ (uint16_t) (i) * 0x0B6Du) & (TABLE_N - 1))
#define RAM_FORMS 6     // the forms a block of code in RAM is translated in, besides its first
static blk_t __far *find(uint16_t pc, uint16_t key) {
    uint16_t h = hash(pc, key);
    for (uint8_t i = 0; i < PROBES; i++) {
        blk_t __far *b = &TABLE[PLACE(h, i)];
        if (!b->key) return 0;
        if (b->pc == pc && b->key == key) return b;
    }
    return 0;
}

// code in RAM: still the bytes that were translated? Before the block's
// code: the bytes that can be read as the code runs (16, a bit each),
// those that are (16; not compared), are there any, the forms met
// since (gbd_run_frame), the bytes translated, their number.
#define RAM_HEAD 34
static uint8_t ram_same(blk_t __far *b, uint16_t pc) {
    const uint8_t __far *c = MK_FP(b->seg, b->off - 1);
    uint8_t len = *c;
    c -= len;
    if ((pc >= 0xC000 && pc <= 0xE000 - len) || (pc >= 0xFF80 && pc <= 0xFFFF - len)) {
        // (the work RAM, the high RAM: where they are)
        // (gb_asm.s: a block with nothing read as it runs has no bit set)
        drc_cmp_p = c;
        drc_cmp_ram = pc - 0x6000;
        drc_cmp_len = len;
        return gbd_ram_cmp();
    }
    for (uint8_t i = 0; i < len; i++)
        if (c[i] != gbf_read(pc + i)) return 0;
    return 1;
}

static uint8_t tmpl_forms;      // (the count of forms a block of code in RAM starts with)
__attribute__((noinline)) static blk_t __far *translate(uint16_t pc, uint16_t key) {
    for (;;) {
        if (code_top > CODE_LIMIT) {
            if (code_seg >= DRC_CODE_SEG + (DRC_CODE_SEGS - 1) * 0x1000u) flush();
            else code_bank(code_seg + 0x1000);
        }
        uint16_t h = hash(pc, key);
        blk_t __far *b = 0;
        for (uint8_t i = 0; i < PROBES && !b; i++) {
            blk_t __far *t = &TABLE[PLACE(h, i)];
            if (!t->key || (t->pc == pc && t->key == key)) b = t;
        }
        if (!b) {
            flush();
            continue;
        }
        blk_pc = pc;
        in_ram = key >= 0x8000;
        sp_cart = gb.sp >= 0xA000 && gb.sp < 0xC000;
        tfill(pc);
        scan_block(pc);
        uint16_t first = code_top;
        code = (uint8_t*) (uint16_t) (DRC_EBUF - first);
        if (in_ram) {
            // the forms of it met since (gbd_run_frame), the bytes
            // translated, then their number, before the code
            uint8_t len = i_pc[n_ins] - pc;
            if ((code_top + RAM_HEAD + len + 1) & 1) E(0x90);   // (the block's code at an even address: see below)
            ram_direct = (pc >= 0xC000 && pc <= 0xE000 - len) || (pc >= 0xFF80 && pc <= 0xFFFF - len);
            memset(tmpl_can, 0, 16);
            code_top += RAM_HEAD - 1;
            E(tmpl_forms);
            for (uint8_t i = 0; i < len; i++) E(rb(pc + i));
            E(len);
        }
        // (a jump to an odd address takes the console a cycle more: every
        // block starts at an even one)
        if (!in_ram && (code_top & 1)) E(0x90);
        {
            blk_t nb;
            nb.pc = pc;
            nb.key = key;
            nb.off = code_top;
            nb.seg = code_seg;
            gbd_lin_put(DRC_TABLE_SEG, FP_OFF(b), &nb, sizeof(nb));
        }
        if (pc < 0x8000) DRC_HAS[h >> 3] |= bit_of[h & 7];
        cold_off = in_ram;
        cold_fail = 0;
        cold_n = 0; cold_used = 0; cold_first = 0;
        translate_block();
        if (cold_fail) {
            // (a slow way ended up too far from its test: the block
            // again, the old way)
            code_top = b->off;
            cold_off = 1;
            cold_fail = 0;
            translate_block();
            drc_cold_fails++;
        }
        if (in_ram) {
            uint8_t *h = code + b->off - 1;
            h -= *h + RAM_HEAD;
            uint8_t any = 0;
            for (uint8_t i = 0; i < 16; i++) {
                h[i] = tmpl_can[i];
                h[16 + i] = tmpl_req[i];
                any |= tmpl_req[i];
            }
            h[32] = any;
        }
        gbd_lin_put(code_seg, first, (const uint8_t*) DRC_EBUF, code_top - first);
        drc_blocks++;
        return b;
    }
}

/* ---- the dispatcher ---- */

static void to_gbf(void) {
    gb.f = ((drc_ah & 0x40) ? GB_FLAG_Z : 0) | ((drc_ah & 0x10) ? GB_FLAG_H : 0) | ((drc_ah & 1) ? GB_FLAG_C : 0)
        | (drc_n ? GB_FLAG_N : 0);
}
static void from_gbf(void) {
    drc_ah = ((gb.f & GB_FLAG_Z) ? 0x40 : 0) | ((gb.f & GB_FLAG_H) ? 0x10 : 0) | ((gb.f & GB_FLAG_C) ? 1 : 0);
    drc_n = (gb.f & GB_FLAG_N) ? 0x40 : 0;
}

void gbd_init(void) {
    for (uint8_t i = 0; i < ST_COUNT; i++) stub_off[i] = drc_stub_offs[i];
    flush();
    drc_flushes = 0;
    drc_ei = drc_interp = 0;
    from_gbf();
    gbd_keys();
}

void gbd_run_frame(void) {
    gb.frame_done = 0;
    if (!drc_cartp) {
        // (the program keeps going to the cartridge's RAM through
        // pointers, 3000 times in 32 frames: everything again, with that
        // in the blocks' own code. Donkey Kong Land gets to 3000 in half a
        // minute of play, less than once a frame: not for it - the blocks
        // would be longer for nothing and translating them all again is a
        // third of a second.)
        static uint8_t fr;
        if (drc_cart_req) {
            drc_cartp = 1;
            flush();
        } else if (!(++fr & 31)) drc_cart_n = 3000;
    }
    while (!gb.frame_done) {
        // (the event that ends the frame may have been done by the routine
        // that leaves a block, gb_drc_asm.s, s_exit: it comes back here
        // with the frame done and time left)
        while (gbf_cyc > 0 && !gb.frame_done) {
            if (drc_ei) {
                drc_ei = 0;
                gb.ime = 1;
            }
            uint8_t pend = gb_ie & gb_io[GB_IF] & 0x1F;
            if (gb.halted) {
                if (!pend) {
                    gbf_cyc = 0;        // nothing to do until the next event
                    break;
                }
                gb.halted = 0;
                gbf_cyc--;
            }
            if (gb.ime && pend) {
                // (the stack in the work RAM or the high RAM, as it always
                // is: the address pushed where it goes, not through the
                // interpreter's two writes by gbf_write; five cycles)
                uint16_t sp = gb.sp;
                if ((sp >= 0xC002 && sp <= 0xE000) || sp >= 0xFF82) {
                    *(uint16_t*) (sp - 0x6002) = gb.pc;
                    gb.sp = sp - 2;
                    gb.ime = 0;
                    uint8_t n = 0;
                    while (!(pend >> n & 1)) n++;
                    gb_io[GB_IF] &= ~(1 << n);
                    gb.pc = 0x40 + n * 8;
                    gbf_cyc -= 5;
                } else {
                    gb_cpu_interrupt();
                }
                drc_link = 0;
                continue;
            }
            if (drc_interp || gb.stopped || gb.locked) {
                drc_interp = 0;
                drc_link = 0;
                to_gbf();
                gb_step();
                from_gbf();
                continue;
            }
            uint16_t pc = gb.pc;
            uint16_t key = pc < 0x4000 ? gb.rom0_bank + 1 : pc < 0x8000 ? gb.romx_bank + 1 : KEY_RAM;
            blk_t __far *b = find(pc, key);
            if (b && key == KEY_RAM && !ram_same(b, pc)) {
                // Code in RAM that is no longer the bytes translated. A
                // program that keeps changing a routine there (an operand,
                // a jump's target: Donkey Kong Land, R-Type II, Race
                // Drivin') had it translated again every time, thousands
                // of blocks, the table and the code's room filled and
                // forgotten. So each form of it is kept: besides the first
                // (key FFFFh), under a key made of its first bytes, as
                // many as the first form has (12 at most).
                const uint8_t __far *c = MK_FP(b->seg, b->off - 1);
                // The first form counts the others met (the byte before
                // its copy of the bytes): after RAM_FORMS the code there
                // is taken for one made anew every time (Donkey Kong
                // Land's LD HL, nn / CALL and LD SP, nn / JP in RAM, their
                // operands written before every use: seven blocks a frame
                // translated, never used twice) and is run by the
                // interpreter from then on, as far as the first form
                // went, to a jump taken or to an interrupt due.
                uint8_t __far *cnt = MK_FP(FP_SEG(c), FP_OFF(c) - *c - 1);
                // First: are the bytes that changed all operands that can
                // be read as the code runs (ram_same's header)? Then the
                // block is translated once more, to read them so, and
                // that is the end of it: Race Drivin's line filler writes
                // the n of an LD D, n and the e of a JR into itself for
                // every line it draws, Donkey Kong Land the nn of its LD
                // HL, nn / CALL.
                if ((pc >= 0xC000 && pc <= 0xE000 - *c) || (pc >= 0xFF80 && pc <= 0xFFFF - *c)) {
                    const uint8_t __far *was = cnt + 1;
                    const uint8_t *now = (const uint8_t*) (pc - 0x6000);
                    uint8_t n = *c > 128 ? 128 : *c, ok = *c <= 128;
                    memset(tmpl_req, 0, 16);
                    for (uint8_t i = 0; i < n; i++)
                        if (was[i] != now[i]) tmpl_req[i >> 3] |= 1 << (i & 7);
                    for (uint8_t i = 0; i < 16; i++) {
                        if (tmpl_req[i] & ~was[(int16_t) i - RAM_HEAD]) ok = 0;
                        tmpl_req[i] |= was[(int16_t) i - RAM_HEAD + 16];
                    }
                    if (ok) {
                        uint16_t flushes = drc_flushes;
                        tmpl_forms = *cnt;
                        gb_phase = GB_PHASE_TRANS;
                        b = translate(pc, KEY_RAM);
                        gb_phase = GB_PHASE_DISP;
                        tmpl_forms = 0;
                        memset(tmpl_req, 0, 16);
                        if (drc_flushes != flushes) drc_link = 0;
                        goto run;
                    }
                    memset(tmpl_req, 0, 16);
                }
                if (*cnt >= RAM_FORMS) {
                    uint16_t end = pc + *c;
                    drc_link = 0;
                    to_gbf();
                    do {
                        gb_step();
                    } while (gbf_cyc > 0 && gb.pc > pc && gb.pc < end && !gb.halted && !gb.stopped
                             && !(gb.ime && (gb_ie & gb_io[GB_IF] & 0x1F)));
                    from_gbf();
                    continue;
                }
                uint8_t len = *c;
                if (len > 12) len = 12;
                uint16_t h = 0;
                if ((pc >= 0xC000 && pc <= 0xE000 - len) || (pc >= 0xFF80 && pc <= 0xFFFF - len)) {
                    const uint8_t *m = (const uint8_t*) (pc - 0x6000);
                    for (uint8_t i = 0; i < len; i++) h = (h << 3 | h >> 13) ^ m[i];
                } else {
                    for (uint8_t i = 0; i < len; i++) h = (h << 3 | h >> 13) ^ gbf_read(pc + i);
                }
                key = 0x8000 | (h & 0x7FFE);
                b = find(pc, key);
                if (b && !ram_same(b, pc)) b = 0;
                if (!b) {
                    uint8_t more = *cnt + 1;
                    gbd_lin_put(FP_SEG(cnt), FP_OFF(cnt), &more, 1);
                }
            }
            if (!b) {
                uint16_t flushes = drc_flushes;
                gb_phase = GB_PHASE_TRANS;
                b = translate(pc, key);
                gb_phase = GB_PHASE_DISP;
                if (drc_flushes != flushes) drc_link = 0;
            }
        run:
            if (pc < 0x8000) {
                // into the small table by address (gb_drc_asm.s,
                // s_exit_dyn_f, which fills it only from a block at its
                // first place in the big one: a return to any other came
                // through here every time)
                uint16_t *rc = (uint16_t*) ((pc < 0x4000 ? DRC_RC0 : DRC_RCX) + ((pc << 3) & 0x0FF8));
                rc[0] = pc;
                rc[1] = key;
                rc[2] = b->off;
                rc[3] = b->seg;
            }
            if (drc_link) {
                // the exit just taken, joined to this block
                uint8_t site[5];
                int16_t near8 = (int16_t) (b->off - drc_link);
                if (drc_link_seg == b->seg && near8 >= -128 && near8 <= 127) {
                    // (near enough for a conditional jump to reach: "jle
                    // slow; jmp near there" becomes "jg there; jmp short
                    // slow", four cycles for six on the console, three for
                    // five as Mednafen counts: the loops, mostly)
                    site[0] = 0x7F;
                    site[1] = (uint8_t) near8;
                    site[2] = 0xEB;
                    site[3] = 0x03;
                    gbd_lin_put(drc_link_seg, drc_link - 2, site, 4);
                } else if (drc_link_seg == b->seg) {
                    uint16_t rel = b->off - (drc_link + 3);
                    site[1] = rel & 0xFF;
                    site[2] = rel >> 8;
                    gbd_lin_put(drc_link_seg, drc_link + 1, site + 1, 2);
                } else {
                    site[1] = b->off & 0xFF;
                    site[2] = b->off >> 8;
                    site[3] = b->seg & 0xFF;
                    site[4] = b->seg >> 8;
                    site[0] = 0xEA;
                    gbd_lin_put(drc_link_seg, drc_link, site, 5);
                }
                drc_link = 0;
            }
            gb_phase = GB_PHASE_RUN;
            gbd_enter(b->off, b->seg);
            gb_phase = GB_PHASE_DISP;
        }
        if (!gb.frame_done) gbf_host_event();
    }
}
