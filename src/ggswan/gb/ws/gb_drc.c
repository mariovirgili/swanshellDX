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
    && offsetof(gb_t, bank_lo) == GB_BANK_LO && offsetof(gb_t, romx_bank) == GB_ROMX, "gb_drc.h");

uint8_t drc_ah, drc_n, drc_tmp, drc_ei, drc_interp;
uint16_t drc_link, drc_link_seg, drc_segtab[4];
uint16_t drc_key0, drc_keyx, drc_hash0, drc_hashx, drc_tseg = DRC_TABLE_SEG;
uint8_t drc_must, drc_ram_ok;
uint8_t drc_mbc_and, drc_mbc_z1, drc_p1_tab[4];
uint16_t drc_mbc_top, drc_mbc_hi, drc_bank_mask;
uint16_t drc_ram_seg;
uint16_t drc_blocks, drc_flushes;

/* ---- the blocks' table and the code ---- */

typedef struct {
    uint16_t pc;
    uint16_t key;       // 0: free; ROM bank + 1; FFFFh: in RAM
    uint16_t off, seg;
} blk_t;
#define TABLE ((blk_t __far*) MK_FP(DRC_TABLE_SEG, 0))
#define TABLE_N 4096
#define KEY_RAM 0xFFFF
#define CODE_LIMIT 0xF000

static uint16_t code_seg, code_top;
static uint8_t __far *code;

static void code_bank(uint16_t seg) {
    code_seg = seg;
    code = MK_FP(seg, 0);
    _fmemcpy(code, drc_stubs, drc_stubs_size);
    code_top = drc_stubs_size;
}

static void flush(void) {
    _fmemset(TABLE, 0, TABLE_N * sizeof(blk_t));
    code_bank(DRC_CODE_SEG);
    drc_link = 0;
    drc_flushes++;
}

static uint16_t hash(uint16_t pc, uint16_t key) {
    return (pc ^ (pc >> 6) ^ (key << 5)) & (TABLE_N - 1);
}

#define E(b) (code[code_top++] = (uint8_t) (b))
static void EW(uint16_t w) {
    code[code_top++] = w & 0xFF;
    code[code_top++] = w >> 8;
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
        gbf_write(a, v);
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

static const uint8_t __far op_len[256] = {
    1,3,1,1,1,1,2,1, 3,1,1,1,1,1,2,1, 2,3,1,1,1,1,2,1, 2,1,1,1,1,1,2,1,
    2,3,1,1,1,1,2,1, 2,1,1,1,1,1,2,1, 2,3,1,1,1,1,2,1, 2,1,1,1,1,1,2,1,
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    1,1,3,3,3,1,2,1, 1,1,3,2,3,3,2,1, 1,1,3,1,3,1,2,1, 1,1,3,1,3,1,2,1,
    2,1,1,1,1,1,2,1, 2,1,3,1,1,1,2,1, 2,1,1,1,1,1,2,1, 2,1,3,1,1,1,2,1 };
// machine cycles (a conditional one not taken)
static const uint8_t __far op_cyc[256] = {
    1,3,2,2,1,1,2,1, 5,2,2,2,1,1,2,1, 1,3,2,2,1,1,2,1, 3,2,2,2,1,1,2,1,
    2,3,2,2,1,1,2,1, 2,2,2,2,1,1,2,1, 2,3,2,2,3,3,3,1, 2,2,2,2,1,1,2,1,
    1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1,
    1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1, 2,2,2,2,2,2,1,2, 1,1,1,1,1,1,2,1,
    1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1,
    1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1, 1,1,1,1,1,1,2,1,
    2,3,3,4,3,4,2,4, 2,4,3,1,3,6,2,4, 2,3,3,1,3,4,2,4, 2,4,3,1,3,1,2,4,
    3,3,2,1,1,4,2,4, 4,1,4,1,1,1,2,4, 3,3,2,1,1,4,2,4, 3,2,4,1,1,1,2,4 };

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

static uint8_t interp_op(uint8_t op, uint8_t cb) {
    switch (op) {
    case 0x08: case 0x10: case 0x27: case 0x39: case 0x76: case 0xE8: case 0xF8:
    case 0xD3: case 0xDB: case 0xDD: case 0xE3: case 0xE4: case 0xEB: case 0xEC: case 0xED: case 0xF4: case 0xFC: case 0xFD:
        return 1;
    case 0xCB:
        return (cb & 7) == 6 && (cb >> 6) != 1;
    default:
        return 0;
    }
}

static uint8_t op_flags(uint8_t op, uint8_t cb) {
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

#define MAXI 40
static uint16_t i_pc[MAXI + 1];
static uint8_t i_live[MAXI];    // the flags needed before each instruction
static uint8_t n_ins;

// the translator's state
static uint8_t fl;          // the flags: 0 in AH, 1 in the V30MZ's
static uint8_t hfix;        // (in the V30MZ's) H: 0 is AF, 1 is clear, 2 is set
static int8_t nst, nmem;    // N: what it is (-1: what drc_n has), what drc_n is known to have
static uint16_t cyc;        // cycles not yet taken off gbf_cyc
static uint8_t live;        // the flags needed before the instruction being translated
static uint16_t blk_pc;
static uint8_t in_ram;
static uint8_t after_ei;    // the instruction after EI: whatever leaves the block goes to the dispatcher
static uint8_t idle;        // the block is a loop that waits: 1 on memory, 2 on LY, 3 on STAT
static uint8_t idle_i;      // ... and this instruction is its jump back
static uint8_t idle_first;  // translating that loop's first instruction (its read of STAT)
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
static void call_stub(uint8_t st) {
    E(0xE8);
    EW(st - (code_top + 2));
}
static void jmp_stub(uint8_t st) {
    E(0xE9);
    EW(st - (code_top + 2));
}
// the cycles so far off the count (the V30MZ's flags are changed)
static void sub_cyc(void) {
    if (!cyc) return;
    E(0x83); E(0x2E); EW(ADDR(gbf_cyc)); E(cyc);
    cyc = 0;
}

// leave for a known address; link: the dispatcher may join the jump to
// the block there
static void exit_to(uint16_t target, uint8_t extra) {
    mat();
    nflush();
    uint8_t link = in_ram ? target == blk_pc : (target < 0x4000 || (blk_pc >= 0x4000 && target >= 0x4000 && target < 0x8000));
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
        jmp_stub(ST_EXIT_DYN);
        return;
    }
    E(0x83); E(cyc + extra ? 0x2E : 0x3E); EW(ADDR(gbf_cyc)); E(cyc + extra);     // sub (or cmp) word [gbf_cyc], n
    E(0x7E); E(0x05);                           // jle +5
    uint16_t site = code_top;
    E(0xE9); EW(2); E(0x90); E(0x90);           // jmp near (to the next line, until joined)
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
    jmp_stub(ST_EXIT_DYN);
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

// "cmp; jb slow; cmp; jae slow" on the address in BX (hl) or SI: the fast
// case, the work RAM and what is above it but the I/O registers, follows;
// returns where the two jumps' distances go
static uint16_t range_check(uint8_t hl) {
    if (hl) { E(0x80); E(0xFF); E(0xC0); } else { E(0x81); E(0xFE); EW(0xC000); }
    E(0x72); E(0);
    uint16_t j = code_top;
    if (hl) { E(0x80); E(0xFF); E(0xFE); } else { E(0x81); E(0xFE); EW(0xFE00); }
    E(0x73); E(0);
    return j;
}
// the fast case is done: jump over the slow one, which starts here
static uint16_t slow_starts(uint16_t j, uint8_t hl) {
    E(0xEB); E(0);
    uint16_t done = code_top;
    code[j - 1] = code_top - j;
    j += hl ? 5 : 6;
    code[j - 1] = code_top - j;
    return done;
}
#define SLOW_ENDS(done) (code[(done) - 1] = code_top - (done))

// r (an x86 8 bit register) = the byte at HL (hl) or at SI
static void read_to(uint8_t r, uint8_t hl) {
    clob();
    uint16_t j = range_check(hl);
    E(0x8A); E(0x80 | r << 3 | (hl ? 7 : 4)); EW(0xA000);   // mov r, [bx or si - 6000h]
    uint16_t done = slow_starts(j, hl);
    if (hl) { E(0x89); E(0xDE); }               // mov si, bx
    if (r == 0) {
        call_stub(ST_RD);
    } else {
        call_stub(ST_RDT);
        E(0x8A); E(0x06 | r << 3); EW(ADDR(drc_tmp));
    }
    SLOW_ENDS(done);
}

// the byte at HL (hl) or at SI = r, or = imm when r is FFh; next: the
// address after this instruction
static void write_from(uint8_t r, uint8_t imm, uint8_t hl, uint16_t next) {
    mat();
    nflush();
    uint16_t j = range_check(hl);
    if (r != 0xFF) {
        E(0x88); E(0x80 | r << 3 | (hl ? 7 : 4)); EW(0xA000);
    } else {
        E(0xC6); E(0x80 | (hl ? 7 : 4)); EW(0xA000); E(imm);
    }
    uint16_t done = slow_starts(j, hl);
    if (r != 0xFF) {
        E(0x88); E(0x06 | r << 3); EW(ADDR(drc_tmp));
    } else {
        E(0xC6); E(0x06); EW(ADDR(drc_tmp)); E(imm);
    }
    if (hl) { E(0x89); E(0xDE); }
    call_stub(ST_WRT);
    EW(next);
    SLOW_ENDS(done);
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
    if (n == GB_STAT && idle == 3 && idle_modes && idle_first) {
        // (the read of a loop that waits for a mode: the wait is done there)
        call_stub(ST_WAIT_STAT);
        E(idle_modes);
        return;
    }
    if (n == GB_STAT || n == GB_LY) {
        call_stub(n == GB_STAT ? ST_STAT : ST_LY);
        return;
    }
    E(0xBE); EW(0xFF00 | n);                    // mov si, FFnn
    call_stub(ST_RD);
}
// LDH (n), A and LD (FFnn), A; true: the block ends here
static uint8_t io_write(uint8_t n, uint16_t next) {
    if ((n >= 0x80 && n != 0xFF) || (n >= 0x10 && n < 0x40)) {
        E(0xA2); EW(0x9F00 + n);                // mov [..], al
        return 0;
    }
    if (n == GB_P1) {
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

// push the word in the x86 register r (0xFF: the constant v)
static void push16(uint8_t r, uint16_t v) {
    clob();
    E(0x81); E(0xFD); EW(0xC002);               // cmp bp, C002h
    E(0x72); E(0);
    uint16_t j = code_top;
    if (r != 0xFF) { E(0x89); E(0x86 | r << 3); EW(0x9FFE); }      // mov [bp - 6002h], r
    else { E(0xC7); E(0x86); EW(0x9FFE); EW(v); }
    E(0x8D); E(0x6E); E(0xFE);                  // lea bp, [bp - 2]
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
    E(0x81); E(0xFD); EW(0xC000);               // cmp bp, C000h
    E(0x72); E(0);
    uint16_t j = code_top;
    E(0x8B); E(0x86 | r << 3); EW(0xA000);      // mov r, [bp - 6000h]
    E(0x8D); E(0x6E); E(0x02);                  // lea bp, [bp + 2]
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
    else if (idle == 3 && idle_modes) { }       // (its read of STAT does the waiting: io_read)
    else call_stub(idle == 2 ? ST_SKIP_LINE : ST_SKIP_MODE);
}

static void translate_block(void) {
    uint8_t ended = 0;
    fl = 0;
    hfix = 0;
    nst = nmem = -1;
    cyc = 0;
    after_ei = 0;
    for (uint8_t i = 0; i < n_ins && !ended; i++) {
        uint16_t pc = i_pc[i], next = i_pc[i + 1];
        uint8_t op = gbf_read(pc), b1 = gbf_read(pc + 1), b2 = gbf_read(pc + 2);
        uint16_t nn = b1 | (uint16_t) b2 << 8;
        uint8_t x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = (op >> 4) & 3;
        live = i_live[i];
        idle_first = i == 0;
        after_ei = i > 0 && gbf_read(i_pc[i - 1]) == 0xFB;
        if (interp_op(op, b1)) {
            exit_interp(pc);
            ended = 1;
            break;
        }
        // (the instruction's cycles are counted before it: an exit inside
        // it has them)
        cyc += op_cyc[op];
        if (x == 1) {
            if (z == 6) read_to(R8[y], 1);
            else if (y == 6) write_from(R8[z], 0, 1, next);
            else if (y != z) { E(0x88); E(0xC0 | R8[z] << 3 | R8[y]); }
            continue;
        }
        if (x == 2) {
            if (z == 6) {
                clob();
                uint16_t j = range_check(1);
                if (y == 1 || y == 3) carry_in();
                E(alu_x86[y] | 2); E(0x87); EW(0xA000);         // op al, [bx - 6000h]
                uint16_t done = slow_starts(j, 1);
                E(0x89); E(0xDE);
                call_stub(ST_RDT);
                if (y == 1 || y == 3) carry_in();
                E(alu_x86[y] | 2); E(0x06); EW(ADDR(drc_tmp));
                SLOW_ENDS(done);
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
                    E(0xB8 | R16[p]); EW(nn);
                } else {                                        // ADD HL, rr: Z kept, H and C the high byte's
                    mat();
                    uint8_t r = R16[p];     // CX, DX or BX: the low byte is register r, the high r + 4
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
                if (p == 2) { E(0x8D); E(0x5F); E(d); }
                else if (p == 3) { E(0x8D); E(0x6E); E(d); }
                else {
                    E(0x89); E(p == 0 ? 0xCE : 0xD6);           // mov si, cx / dx
                    E(0x8D); E(p == 0 ? 0x4C : 0x54); E(d);     // lea cx / dx, [si +- 1]
                }
                break;
            }
            case 4: case 5:
                if (y == 6) {
                    mat();
                    nflush();
                    call_stub(z == 4 ? ST_INC_HL : ST_DEC_HL);
                    EW(next);
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
                if (p == 1) { E(0xC6); E(0x06); EW(ADDR(gb.ime)); E(1); }
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
            if (y == 0) {                                       // JP nn
                if (idle && i == idle_i && nn == blk_pc) wait_skip();
                exit_to(nn, 0);
                ended = 1;
            } else if (y == 6) {                                // DI
                E(0xC6); E(0x06); EW(ADDR(gb.ime)); E(0);
                E(0xC6); E(0x06); EW(ADDR(drc_ei)); E(0);
            } else if (y == 7) {                                // EI: after the next instruction
                E(0xC6); E(0x06); EW(ADDR(drc_ei)); E(1);
            } else {                                            // CB
                cyc++;
                uint8_t cx = b1 >> 6, cy = (b1 >> 3) & 7, r = R8[b1 & 7];
                if ((b1 & 7) == 6) {                            // BIT n, (HL)
                    cyc++;
                    mat();
                    E(0x80); E(0xE4); E(0x01);                  // and ah, 1
                    E(0x80); E(0xCC); E(0x10);                  // or ah, 10h
                    uint16_t j = range_check(1);
                    E(0xF6); E(0x87); EW(0xA000); E(1 << cy);   // test byte [bx - 6000h], m
                    uint16_t done = slow_starts(j, 1);
                    E(0x89); E(0xDE);
                    call_stub(ST_RDT);
                    E(0xF6); E(0x06); EW(ADDR(drc_tmp)); E(1 << cy);
                    SLOW_ENDS(done);
                    E(0x75); E(0x03);                           // jnz +3
                    E(0x80); E(0xCC); E(0x40);                  // or ah, 40h
                    nst = 0;
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
                exit_to(nn, 0);
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
        if (!ended && i > 0 && gbf_read(i_pc[i - 1]) == 0xFB) {
            exit_to(next, 0);
            ended = 1;
        }
    }
    after_ei = 0;
    if (!ended) exit_to(i_pc[n_ins], 0);
}

// a loop that waits on STAT (its instructions 0 to n - 1, the last the
// jump back): the modes it goes round in, if the mode alone decides (the
// same whatever STAT's other bits are); 0 if not, or if it is not known
static uint8_t wait_modes(uint8_t n) {
    uint8_t mask = 0;
    for (uint8_t m = 0; m < 4; m++) {
        uint8_t taken[2];
        for (uint8_t v = 0; v < 2; v++) {
            uint8_t a = (v ? 0xFC : 0x80) | m, z = 0, c = 0, cknown = 0, seen = 0;
            for (uint8_t i = 0; i < n; i++) {
                uint8_t op = gbf_read(i_pc[i]), b1 = gbf_read(i_pc[i] + 1);
                if (op == 0xF0 || op == 0xFA) {
                    if (b1 != 0x41 || seen || i != 0) return 0;     // (STAT, once, first)
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

// the instructions of the block at pc, and the flags needed before each
static void scan_block(uint16_t pc) {
    uint8_t fa[MAXI];
    n_ins = 0;
    uint8_t after_ei = 0, total = 0;
    while (n_ins < MAXI && total < 90) {
        uint8_t op = gbf_read(pc);
        total += op_cyc[op] + 2;
        uint8_t f = op_flags(op, gbf_read(pc + 1));
        i_pc[n_ins] = pc;
        fa[n_ins++] = f;
        pc += op_len[op];
        if ((f & F_END) || after_ei) break;
        if (op == 0xFB) after_ei = 1;
        if (in_ram && pc - blk_pc > 48) break;
    }
    i_pc[n_ins] = pc;
    // a loop that waits: A loaded from memory or an I/O register, tested,
    // and a conditional jump back to the start; nothing written, no
    // register but A changed
    idle = 0;
    if (!in_ram) {
        uint8_t kind = 1, loaded = 0;
        for (uint8_t i = 0; i < n_ins && i < 6; i++) {
            uint8_t op = gbf_read(i_pc[i]), b1 = gbf_read(i_pc[i] + 1);
            if ((op & 0xE7) == 0x20 || (op & 0xE7) == 0xC2 || op == 0x18 || op == 0xC3) {
                // the first jump: back to the block's start?
                uint16_t t = (op & 0xC0) ? b1 | (uint16_t) gbf_read(i_pc[i] + 2) << 8 : i_pc[i] + 2 + (int8_t) b1;
                // (a jump to itself is a wait too: for an interrupt)
                if (t == blk_pc && (loaded || (i == 0 && (op == 0x18 || op == 0xC3)))) {
                    idle = kind;
                    idle_i = i;
                    idle_modes = kind == 3 && op != 0x18 && op != 0xC3 ? wait_modes(i + 1) : 0;
                }
                break;
            } else if (op == 0xF0 || op == 0xFA) {
                uint16_t a = op == 0xF0 ? 0xFF00 | b1 : b1 | (uint16_t) gbf_read(i_pc[i] + 2) << 8;
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
    uint8_t need = FALL;        // (after the block: all of them)
    for (uint8_t i = n_ins; i-- > 0;) {
        uint8_t f = fa[i];
        if (f & F_OUT) need = FALL;
        else need = (need & ~(f & 7)) | ((f >> 3) & 7);
        i_live[i] = need;
    }
}

static blk_t __far *find(uint16_t pc, uint16_t key) {
    uint16_t h = hash(pc, key);
    for (uint8_t i = 0; i < 8; i++) {
        blk_t __far *b = &TABLE[(h + i) & (TABLE_N - 1)];
        if (!b->key) return 0;
        if (b->pc == pc && b->key == key) return b;
    }
    return 0;
}

static blk_t __far *translate(uint16_t pc, uint16_t key) {
    for (;;) {
        if (code_top > CODE_LIMIT) {
            if (code_seg >= DRC_CODE_SEG + (DRC_CODE_SEGS - 1) * 0x1000u) flush();
            else code_bank(code_seg + 0x1000);
        }
        uint16_t h = hash(pc, key);
        blk_t __far *b = 0;
        for (uint8_t i = 0; i < 8 && !b; i++) {
            blk_t __far *t = &TABLE[(h + i) & (TABLE_N - 1)];
            if (!t->key || (t->pc == pc && t->key == key)) b = t;
        }
        if (!b) {
            flush();
            continue;
        }
        blk_pc = pc;
        in_ram = key == KEY_RAM;
        scan_block(pc);
        if (in_ram) {
            // the bytes translated, then their number, before the code
            uint8_t len = i_pc[n_ins] - pc;
            for (uint8_t i = 0; i < len; i++) E(gbf_read(pc + i));
            E(len);
        }
        b->pc = pc;
        b->key = key;
        b->off = code_top;
        b->seg = code_seg;
        translate_block();
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
    flush();
    drc_flushes = 0;
    drc_ei = drc_interp = 0;
    from_gbf();
    gbd_keys();
}

void gbd_run_frame(void) {
    gb.frame_done = 0;
    while (!gb.frame_done) {
        while (gbf_cyc > 0) {
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
                gb_cpu_interrupt();
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
            if (b && key == KEY_RAM) {
                // code in RAM: still the bytes that were translated?
                const uint8_t __far *c = MK_FP(b->seg, b->off - 1);
                uint8_t len = *c;
                for (uint8_t i = 0; i < len; i++)
                    if (c[(int16_t) i - len] != gbf_read(pc + i)) {
                        b = 0;
                        break;
                    }
            }
            if (!b) {
                uint16_t flushes = drc_flushes;
                gb_phase = GB_PHASE_TRANS;
                b = translate(pc, key);
                gb_phase = GB_PHASE_DISP;
                if (drc_flushes != flushes) drc_link = 0;
            }
            if (drc_link) {
                // the exit just taken, joined to this block
                uint8_t __far *site = MK_FP(drc_link_seg, drc_link);
                if (drc_link_seg == b->seg) {
                    uint16_t rel = b->off - (drc_link + 3);
                    site[1] = rel & 0xFF;
                    site[2] = rel >> 8;
                } else {
                    site[1] = b->off & 0xFF;
                    site[2] = b->off >> 8;
                    site[3] = b->seg & 0xFF;
                    site[4] = b->seg >> 8;
                    site[0] = 0xEA;
                }
                drc_link = 0;
            }
            gb_phase = GB_PHASE_RUN;
            gbd_enter(b->off, b->seg);
            gb_phase = GB_PHASE_DISP;
        }
        gbf_host_event();
    }
}
