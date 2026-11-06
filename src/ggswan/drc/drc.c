/**
 * ggswan - Game Gear emulator for the WonderSwan (nileswan)
 * Z80 -> V30MZ dynamic recompiler: the translator. See drc.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stddef.h>
#include "drc.h"
#include "../core/z80.h"

/* === Z80 flags === */
#define FS 0x80
#define FZ 0x40
#define FH 0x10
#define FP 0x04
#define FN 0x02
#define FC 0x01
#define FALL (FS | FZ | FH | FP | FN | FC)

/* === Decoded instructions === */

#define MAX_INSTR 48

enum {
    T_NONE = 0,    // ordinary instruction
    T_END,         // control flow: translated, ends the block
    T_STOP,        // not translated: the block ends before it
    T_END_AFTER,   // translated, then the block ends (mapper write, EI)
};

typedef struct {
    uint16_t pc;
    uint8_t len;
    uint8_t pre;       // 0, 0xDD, 0xFD, 0xED, 0xCB (0xDD/0xFD + CB: pre = 0xDD/0xFD, cb = 1)
    uint8_t op;        // opcode after the prefix(es)
    uint8_t cb;        // DD CB d op / FD CB d op
    int8_t d;          // displacement
    uint16_t nn;       // immediate (n or nn)
    uint8_t fread, fwrite;
    uint8_t term;
    uint8_t step;      // run with the interpreter (DRC_H_STEP)...
    uint8_t count;     // ... this many instructions
} ins_t;

static ins_t ins[MAX_INSTR];
static uint8_t live_out[MAX_INSTR];

/* === Output === */

static uint8_t *o, *o_start, *o_limit;
static uint16_t o_base;
static const drc_env_t *env;

static inline void B(uint8_t x) { *o++ = x; }
static inline void W(uint16_t x) { B(x); B(x >> 8); }
static inline uint16_t here(void) { return o_base + (uint16_t) (o - o_start); }

// Absolute IRAM addresses of fields
#define ZA(field) ((uint16_t) (env->z80_addr + offsetof(z80_t, field)))
#define SA(field) ((uint16_t) (env->state_addr + offsetof(drc_state_t, field)))

// short forward jumps: returns the position of the rel8 byte
static uint8_t *jcc8(uint8_t opcode) { B(opcode); B(0); return o - 1; }
static void land8(uint8_t *p) { *p = (uint8_t) (o - p - 1); }

static void jmp_near(uint16_t target) { B(0xE9); W(target - (here() + 2)); }
static void call_near(uint16_t target) { B(0xE8); W(target - (here() + 2)); }

/* === Registers === */

// Z80 register index (B C D E H L (HL) A) -> x86 8-bit register
//                                  B  C  D  E  H  L  -  A
static const uint8_t r8x[8] = {     5, 1, 6, 2, 7, 3, 0, 0 };
// x86 16-bit registers
#define X_AX 0
#define X_CX 1
#define X_DX 2
#define X_BX 3
#define X_BP 5
#define X_SI 6
#define X_DI 7
// Z80 pair index (BC DE HL SP) -> x86 16-bit register
static const uint8_t r16x[4] = { X_CX, X_DX, X_BX, X_BP };

/* === Flag state === */

enum { K_AH = 0, K_ADD, K_SUB, K_AND, K_OR, K_INC, K_DEC };
static uint8_t fkind;      // where the flags are (K_AH: in AH)
static uint8_t cf_valid;   // K_INC / K_DEC: x86 CF holds Z80 C
static uint8_t hl_es;      // ES is set up for the slot of HL

static void materialize(void) {
    switch (fkind) {
    case K_ADD: case K_INC: case K_SUB: case K_DEC: {
        uint8_t mask = (fkind == K_SUB || fkind == K_DEC) ? 0xD3 : 0xD1;
        B(0x9F);                         // lahf
        B(0x70); B(0x05);                // jo +5
        B(0x80); B(0xE4); B(mask);       // and ah, mask (P/V = 0)
        B(0xEB); B(0x06);                // jmp +6
        B(0x80); B(0xE4); B(mask);       // and ah, mask
        B(0x80); B(0xCC); B(0x04);       // or ah, 4 (P/V = 1)
        break;
    }
    case K_AND:
        B(0x9F); B(0x80); B(0xE4); B(0xC5); B(0x80); B(0xCC); B(0x10);
        break;
    case K_OR:
        B(0x9F); B(0x80); B(0xE4); B(0xC5);
        break;
    }
    fkind = K_AH;
}

// x86 flags are about to be destroyed: save the Z80 flags if needed
static void clobber(uint8_t live) {
    if (fkind != K_AH) {
        if (live)
            materialize();
        else
            fkind = K_AH;   // dead: AH holds stale flags nobody reads
    }
}

// The instruction updates AH directly (partial flag writers)
static void need_ah(uint8_t live) { clobber(live); }

// x86 CF must hold Z80 C (ADC, SBC, RL, RR, INC/DEC keeping C)
// (with the INC/DEC kinds, CF is valid whenever C is live: see INC/DEC)
static void need_cf(void) {
    if (fkind == K_AH)
        B(0x9E);   // sahf
}

/* === Cycles and exits === */

static uint16_t cyc;   // cycles of the instructions translated so far
static uint16_t block_start_off;
static uint16_t block_pc;

static uint8_t force_nolink;   // the next exit must return to the runtime
// (a helper that changes the mapping makes the runtime end the block
// without following links: see drc_rt.c)

static int linkable(uint16_t target) {
    if (force_nolink || target >= 0xC000) return 0;
    return (target >> 14) == 0 || (target >> 14) == (block_pc >> 14);
}

static void set_link(uint16_t v) { B(0xC7); B(0x06); W(SA(link)); W(v); }

static void sub_cycles(uint16_t n) {
    if (n < 128) { B(0x83); B(0x2E); W(SA(cycles)); B(n); }
    else { B(0x81); B(0x2E); W(SA(cycles)); W(n); }
}

static void exit_const(uint16_t target, uint16_t extra) {
    materialize();
    sub_cycles(cyc + extra);
    B(0xC7); B(0x06); W(ZA(pc)); W(target);   // mov word [pc], target
    if (linkable(target)) {
        set_link(here() + 6 + 2 + 3);         // L
        B(0x7F); B(0x03);                     // jg L
        jmp_near(env->exit_off);
        jmp_near(env->exit_off);              // L: (the runtime may change it)
    } else {
        set_link(0);
        jmp_near(env->exit_off);
    }
}

static void exit_reg(uint8_t reg16, uint16_t extra) {
    materialize();
    sub_cycles(cyc + extra);
    B(0x89); B(0x06 | (reg16 << 3)); W(ZA(pc));   // mov [pc], reg
    set_link(0);
    jmp_near(env->exit_off);
}

/* === Memory === */

// ES for the slot of BX (HL)
static void seg_hl(uint8_t live) {
    if (hl_es) return;
    clobber(live);
    B(0x89); B(0xDE);                    // mov si, bx
    B(0xC1); B(0xC6); B(0x04);           // rol si, 4
    B(0x83); B(0xE6); B(0x0E);           // and si, 14
    B(0x8E); B(0x84); W(SA(segtab));     // mov es, [si + segtab]
    hl_es = 1;
}

// ES for the slot of SI
static void seg_si(uint8_t live) {
    clobber(live);
    hl_es = 0;
    B(0x89); B(0xF7);                    // mov di, si
    B(0xC1); B(0xC7); B(0x04);           // rol di, 4
    B(0x83); B(0xE7); B(0x0E);           // and di, 14
    B(0x8E); B(0x85); W(SA(segtab));     // mov es, [di + segtab]
}

// SI = IX + d or IY + d
static void ix_addr(uint8_t pre, int8_t d) {
    B(0x8B); B(0x36); W(pre == 0xDD ? ZA(ixl) : ZA(iyl));   // mov si, [ix]
    if (d) { B(0x8D); B(0x74); B((uint8_t) d); }           // lea si, [si + d]
}

static void helper(uint8_t op) {
    B(0xC6); B(0x06); W(SA(op)); B(op);   // mov byte [op], op
    call_near(env->helper_off);
    hl_es = 0;   // the mapping may have changed
}

/*
 * Write a byte to the address in BX (HL) or SI. src: x86 8-bit register,
 * or 0xFF with imm. Work RAM directly, anything else through the helper.
 */
static void write_byte(uint8_t addr_reg, uint8_t src, uint8_t imm, uint8_t live) {
    clobber(live);
    uint8_t rm = addr_reg == X_BX ? 7 : 4;   // [bx+d16] / [si+d16]
    uint8_t *j1, *j2, *j3;
    if (addr_reg == X_BX) {
        B(0x80); B(0xFF); B(0xC0); j1 = jcc8(0x72);   // cmp bh, C0h; jb
        B(0x80); B(0xFF); B(0xE0); j2 = jcc8(0x73);   // cmp bh, E0h; jae
    } else {
        B(0x81); B(0xFE); W(0xC000); j1 = jcc8(0x72);
        B(0x81); B(0xFE); W(0xE000); j2 = jcc8(0x73);
    }
    if (src == 0xFF) { B(0xC6); B(0x80 | rm); W(env->ram_delta); B(imm); }
    else { B(0x88); B(0x80 | (src << 3) | rm); W(env->ram_delta); }
    j3 = jcc8(0xEB);
    land8(j1); land8(j2);
    if (src == 0xFF) { B(0xC6); B(0x06); W(SA(val)); B(imm); }
    else { B(0x88); B(0x06 | (src << 3)); W(SA(val)); }
    B(0x89); B(0x06 | (addr_reg << 3)); W(SA(arg));   // mov [arg], reg
    helper(DRC_H_WRITE);
    land8(j3);
    if (addr_reg == X_BX) hl_es = 0;
}

static void write_const(uint16_t addr, uint8_t src, uint8_t live) {
    if (addr >= 0xC000 && addr < 0xE000) {
        if (src == X_AX) { B(0xA2); }
        else { B(0x88); B(0x06 | (src << 3)); }
        W(addr + env->ram_delta);
        return;
    }
    clobber(live);
    B(0x88); B(0x06 | (src << 3)); W(SA(val));
    B(0xC7); B(0x06); W(SA(arg)); W(addr);
    helper(DRC_H_WRITE);
}

// Read a byte from a constant address into an 8-bit register
static void read_const(uint16_t addr, uint8_t dst) {
    if (addr >= 0xC000) {
        uint16_t a = (addr & 0x1FFF) + 0xC000 + env->ram_delta;
        if (dst == X_AX) { B(0xA0); } else { B(0x8A); B(0x06 | (dst << 3)); }
        W(a);
        return;
    }
    B(0x8E); B(0x06); W(SA(segtab) + (addr >> 13) * 2);   // mov es, [segtab+n]
    hl_es = 0;
    B(0x26);
    if (dst == X_AX) { B(0xA0); } else { B(0x8A); B(0x06 | (dst << 3)); }
    W(addr);
}

static void read_const16(uint16_t addr, uint8_t dst) {
    if (addr >= 0xC000 && addr != 0xDFFF && addr != 0xFFFF) {
        B(0x8B); B(0x06 | (dst << 3)); W((addr & 0x1FFF) + 0xC000 + env->ram_delta);
        return;
    }
    B(0x8E); B(0x06); W(SA(segtab) + (addr >> 13) * 2);
    hl_es = 0;
    B(0x26); B(0x8B); B(0x06 | (dst << 3)); W(addr);
}

/* === Decoder === */

static uint8_t rd(uint16_t a) { return z80_read(a); }

static int is_cond_true_flag(uint8_t cc) {
    static const uint8_t m[4] = { FZ, FC, FP, FS };
    return m[cc >> 1];
}

// Fill in the instruction at pc. Returns 0 if the block must end before it.
static void decode(uint16_t pc, ins_t *I) {
    uint8_t op = rd(pc);
    I->pc = pc; I->pre = 0; I->cb = 0; I->d = 0; I->nn = 0;
    I->fread = 0; I->fwrite = 0; I->term = T_NONE; I->step = 0; I->count = 1;
    I->len = 1; I->op = op;

    if (op == 0xCB) {
        I->pre = 0xCB; I->op = op = rd(pc + 1); I->len = 2;
        uint8_t r = op & 7;
        if (op < 0x40) {
            if (r == 6 || (op & 0xF8) == 0x30) { I->step = 1; return; }
            I->fwrite = FALL;
            if ((op & 0xF0) == 0x10) I->fread = FC;   // RL, RR
        } else if (op < 0x80) {
            I->fwrite = FZ | FH | FP | FN | FS;
        } else if (r == 6) {
            // SET/RES (HL): RAM directly, otherwise interpreted
        }
        return;
    }

    if (op == 0xED) {
        I->pre = 0xED; I->op = op = rd(pc + 1); I->len = 2;
        switch (op) {
        case 0x44: I->fwrite = FALL; return;                        // NEG
        case 0x43: case 0x53: case 0x73:                            // LD (nn),rr
            I->nn = rd(pc + 2) | (rd(pc + 3) << 8); I->len = 4;
            if (I->nn < 0xC000 || I->nn >= 0xDFFF) I->step = 1;
            return;
        case 0x4B: case 0x5B: case 0x7B:                            // LD rr,(nn)
            I->nn = rd(pc + 2) | (rd(pc + 3) << 8); I->len = 4; return;
        case 0x45: case 0x4D: case 0x55: case 0x5D:
        case 0x65: case 0x6D: case 0x75: case 0x7D:                 // RETN, RETI
            I->term = T_STOP; return;
        }
        if (op == 0xA0 || op == 0xA8 || op == 0xA3 || op == 0xAB) {
            // LDI LDD OUTI OUTD: games unroll long runs of them (VRAM
            // uploads); a run is one interpreted step of n instructions
            uint8_t k = 1;
            while (k < 64 && rd(pc + k * 2) == 0xED && rd(pc + k * 2 + 1) == op) k++;
            I->len = k * 2; I->count = k; I->step = 1;
            return;
        }
        if ((op & 0xC7) == 0x41 && op != 0x71) return;                      // OUT (C),r
        if (op >= 0xB0 && op <= 0xBB && !(op & 4)) {
            // LDIR, CPIR, INIR, OTIR...: the interpreter runs them between
            // blocks, so that long ones still let interrupts happen
            I->term = T_STOP; return;
        }
        // everything else: block instructions, I/O, LD A,I... interpreted
        I->step = 1;
        if (op == 0x63 || op == 0x6B) { I->len = 4; }   // LD (nn),HL / LD HL,(nn) (ED form)
        else if ((op & 0xC7) == 0x43) { I->len = 4; }
        return;
    }

    if (op == 0xDD || op == 0xFD) {
        uint8_t op2 = rd(pc + 1);
        I->pre = op; I->op = op2; I->len = 2;
        if (op2 == 0xCB) {
            I->cb = 1; I->d = (int8_t) rd(pc + 2); I->op = op2 = rd(pc + 3); I->len = 4;
            if (op2 >= 0x40 && op2 < 0x80 && (op2 & 7) == 6) {
                I->fwrite = FZ | FH | FP | FN | FS;   // BIT b,(IX+d)
                return;
            }
            if (op2 >= 0x80 && (op2 & 7) == 6) return;   // SET/RES b,(IX+d)
            I->step = 1;                                 // rotates etc.
            return;
        }
        switch (op2) {
        case 0x21: case 0x22: case 0x2A:                              // LD IX,nn / (nn)
            I->nn = rd(pc + 2) | (rd(pc + 3) << 8); I->len = 4;
            if (op2 == 0x22 && (I->nn < 0xC000 || I->nn >= 0xDFFF)) I->step = 1;
            return;
        case 0x23: case 0x2B: case 0xE1: case 0xE5: case 0xF9: case 0xE3:
            return;
        case 0xE9: I->term = T_END; return;                           // JP (IX)
        case 0x09: case 0x19: case 0x29: case 0x39:                   // ADD IX,rr
            I->fwrite = FH | FN | FC; I->fread = FS | FZ | FP; return;
        case 0x36:                                                    // LD (IX+d),n
            I->d = (int8_t) rd(pc + 2); I->nn = rd(pc + 3); I->len = 4; return;
        case 0x34: case 0x35:                                         // INC/DEC (IX+d)
            I->d = (int8_t) rd(pc + 2); I->len = 3; I->step = 1; return;
        }
        if ((op2 >= 0x46 && op2 <= 0x7E && (op2 & 7) == 6 && op2 != 0x76)    // LD r,(IX+d)
            || (op2 >= 0x70 && op2 <= 0x77 && op2 != 0x76)) {              // LD (IX+d),r
            I->d = (int8_t) rd(pc + 2); I->len = 3; return;
        }
        if (op2 >= 0x86 && op2 <= 0xBE && (op2 & 7) == 6) {             // ALU A,(IX+d)
            I->d = (int8_t) rd(pc + 2); I->len = 3; I->fwrite = FALL;
            if (op2 == 0x8E || op2 == 0x9E) I->fread = FC;
            return;
        }
        // other DD/FD forms (IXH, IXL...) or a prefix on a plain opcode:
        // leave it to the interpreter, between blocks
        I->term = T_STOP;
        return;
    }

    // unprefixed
    uint8_t x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    switch (x) {
    case 1:
        if (op == 0x76) I->term = T_STOP;   // HALT
        return;   // LD r,r'
    case 2:
        I->fwrite = FALL;
        if (y == 1 || y == 3) I->fread = FC;
        return;
    case 0:
        switch (z) {
        case 0:
            if (y == 0) return;                                   // NOP
            if (y == 1) { I->fread = FALL; I->fwrite = FALL; return; }   // EX AF,AF'
            I->nn = rd(pc + 1); I->len = 2; I->term = T_END;      // DJNZ, JR
            if (y >= 4) I->fread = is_cond_true_flag(y - 4);
            return;
        case 1:
            if (y & 1) { I->fwrite = FH | FN | FC; I->fread = FS | FZ | FP; return; }   // ADD HL,rr
            I->nn = rd(pc + 1) | (rd(pc + 2) << 8); I->len = 3; return;              // LD rr,nn
        case 2:
            if (y >= 4) { I->nn = rd(pc + 1) | (rd(pc + 2) << 8); I->len = 3;
                          if (y == 4 || y == 6) {
                              // LD (nn),HL / LD (nn),A: a mapper write ends the block
                              if (I->nn >= 0xFFFC - (y == 4)) I->term = T_END_AFTER;
                          } }
            return;
        case 3: return;                                       // INC/DEC rr
        case 4: case 5:                                       // INC/DEC r
            I->fwrite = FS | FZ | FH | FP | FN;
            if (y == 6) I->fread = FC;   // (HL): kept through AH
            return;
        case 6: I->nn = rd(pc + 1); I->len = 2; return;       // LD r,n
        case 7:
            switch (y) {
            case 0: case 1: case 2: case 3:                   // RLCA RRCA RLA RRA
                I->fwrite = FH | FN | FC; I->fread = FS | FZ | FP | (y >= 2 ? FC : 0); return;
            case 4: I->step = 1; I->fread = FALL; return;     // DAA
            case 5: I->fwrite = FH | FN; I->fread = FS | FZ | FP | FC; return;   // CPL
            case 6: I->fwrite = FH | FN | FC; I->fread = FS | FZ | FP; return;   // SCF
            case 7: I->fwrite = FH | FN | FC; I->fread = FS | FZ | FP | FC; return; // CCF
            }
        }
        return;
    case 3:
        switch (z) {
        case 0: I->term = T_END; I->fread = is_cond_true_flag(y >> 1); return;   // RET cc
        case 1:
            if (y == 1 || y == 5) { I->term = T_END; return; }   // RET, JP (HL)
            if (y == 3) return;                                  // EXX
            if (y == 7) return;                                  // LD SP,HL
            if (y == 2 || y == 0 || y == 4) return;              // POP
            I->fwrite = FALL;                                    // POP AF
            return;
        case 2: case 4:
            I->nn = rd(pc + 1) | (rd(pc + 2) << 8); I->len = 3; I->term = T_END;
            I->fread = is_cond_true_flag(y >> 1);
            return;
        case 3:
            switch (y) {
            case 0: I->nn = rd(pc + 1) | (rd(pc + 2) << 8); I->len = 3; I->term = T_END; return;
            case 2: case 3: I->nn = rd(pc + 1); I->len = 2; return;   // OUT (n),A / IN A,(n)
            case 4: case 5: return;                                   // EX (SP),HL / EX DE,HL
            case 6: return;                                           // DI
            case 7: I->step = 1; I->term = T_END_AFTER; return;       // EI
            }
            return;
        case 5:
            if (y == 1) { I->nn = rd(pc + 1) | (rd(pc + 2) << 8); I->len = 3; I->term = T_END; return; }
            if (y == 6) { I->fread = FALL; return; }   // PUSH AF
            return;                                     // PUSH rr
        case 6:
            I->nn = rd(pc + 1); I->len = 2; I->fwrite = FALL;
            if (y == 1 || y == 3) I->fread = FC;
            return;
        case 7: I->term = T_END; return;   // RST
        }
    }
}

/* === Code generation === */

static const uint8_t alu_x86[8] = { 0x00, 0x10, 0x28, 0x18, 0x20, 0x30, 0x08, 0x38 };
static const uint8_t alu_kind[8] = { K_ADD, K_ADD, K_SUB, K_SUB, K_AND, K_OR, K_OR, K_SUB };

static void alu_done(uint8_t y) { fkind = alu_kind[y]; cf_valid = 1; }

// Conditional jump to a label when Z80 condition cc is true
static uint8_t *jcond(uint8_t cc) {
    uint8_t f = cc >> 1, want = cc & 1;   // want: flag set
    if (fkind == K_AH) {
        static const uint8_t m[4] = { FZ, FC, FP, FS };
        B(0xF6); B(0xC4); B(m[f]);            // test ah, mask
        return jcc8(want ? 0x75 : 0x74);      // jnz / jz
    }
    uint8_t opc;
    switch (f) {
    case 0: opc = 0x74; break;   // jz
    case 1: opc = 0x72; break;   // jc
    case 2: opc = (fkind == K_AND || fkind == K_OR) ? 0x7A : 0x70; break;   // jp / jo
    default: opc = 0x78; break;  // js
    }
    return jcc8(want ? opc : opc ^ 1);
}

static void inc_dec_flags(uint8_t dec, uint8_t live_c) {
    if (fkind == K_AH) {
        if (live_c) { B(0x9E); cf_valid = 1; }   // sahf
        else cf_valid = 0;
    } else if (fkind == K_INC || fkind == K_DEC) {
        // cf_valid unchanged
    } else {
        cf_valid = 1;
    }
    (void) dec;
}


// Taken branch to target: loop back inside the block when possible
static void branch_to(uint16_t target, uint16_t extra) {
    if (target == block_pc) {
        materialize();
        sub_cycles(cyc + extra);
        // jg block start, else exit with the cycles already counted
        int16_t rel = (int16_t) (block_start_off - (here() + 2));
        if (rel >= -128) { B(0x7F); B((uint8_t) rel); }
        else { B(0x7E); B(0x03); jmp_near(block_start_off); }
        B(0xC7); B(0x06); W(ZA(pc)); W(target);
        set_link(0);
        jmp_near(env->exit_off);
        return;
    }
    exit_const(target, extra);
}

static void push_imm(uint16_t value) {
    B(0x8D); B(0x6E); B(0xFE);                                  // lea bp, [bp-2]
    B(0xC7); B(0x86); W(env->ram_delta); W(value);              // mov word [bp+d], imm
}

// Returns 0 if the instruction could not be emitted (block ends before it)
static int emit(int i) {
    ins_t *I = &ins[i];
    // flags whose current value is still needed: read by this instruction,
    // or needed later and not rewritten by it
    uint8_t live = (live_out[i] & ~I->fwrite) | I->fread;
    uint8_t op = I->op;
    uint16_t next = I->pc + I->len;

    if (I->step) {
        materialize();
        B(0xC7); B(0x06); W(SA(arg)); W(I->pc);
        B(0xC6); B(0x06); W(SA(val)); B(I->count);   // instructions
        helper(DRC_H_STEP);
        fkind = K_AH;
        if (I->term == T_END_AFTER) { force_nolink = 1; exit_const(next, 0); }
        return 1;
    }

    if (I->pre == 0xCB) {
        uint8_t r = op & 7, b = (op >> 3) & 7;
        uint8_t xr = r8x[r];
        if (r == 4 || r == 5) hl_es = 0;   // H or L changes (except BIT: harmless)
        if (op < 0x40) {
            uint8_t y = op >> 3;
            if (y <= 3) {
                // RLC RRC RL RR: rol ror rcl rcr
                if (y >= 2) {
                    if (fkind == K_AH) B(0x9E);   // sahf: CF = C
                }
                B(0xD0); B(0xC0 | (y << 3) | xr);
                B(0x19); B(0xF6);                 // sbb si, si
                B(0x84); B(0xC0 | (xr << 3) | xr);// test r, r
                B(0x9F);                          // lahf
                B(0x80); B(0xE4); B(0xC4);        // and ah, C4h
                B(0xD1); B(0xEE);                 // shr si, 1
                B(0x80); B(0xD4); B(0x00);        // adc ah, 0
            } else {
                static const uint8_t sh[8] = { 0, 0, 0, 0, 4, 7, 0, 5 };   // SLA SRA - SRL
                B(0xD0); B(0xC0 | (sh[y] << 3) | xr);
                B(0x9F); B(0x80); B(0xE4); B(0xC5);
            }
            fkind = K_AH;
            cyc += 8;
            return 1;
        }
        if (op < 0x80) {
            // BIT b,r / BIT b,(HL)
            need_ah(live);
            if (r == 6) seg_hl(live);
            B(0x80); B(0xE4); B(0x01);           // and ah, 1
            B(0x80); B(0xCC); B(0x10);           // or ah, 10h
            if (r == 6) { B(0x26); B(0xF6); B(0x07); B(1 << b); }
            else { B(0xF6); B(0xC0 | xr); B(1 << b); }
            if (b == 7) {
                B(0x75); B(0x05); B(0x80); B(0xCC); B(0x44); B(0xEB); B(0x03);
                B(0x80); B(0xCC); B(0x80);
            } else {
                B(0x75); B(0x03); B(0x80); B(0xCC); B(0x44);
            }
            fkind = K_AH;
            cyc += r == 6 ? 12 : 8;
            return 1;
        }
        // SET / RES
        uint8_t set = op >= 0xC0;
        uint8_t imm = set ? (1 << b) : (uint8_t) ~(1 << b);
        clobber(live);
        if (r != 6) {
            B(0x80); B((set ? 0xC8 : 0xE0) | xr); B(imm);
            cyc += 8;
            return 1;
        }
        // (HL): RAM directly, anything else with the interpreter
        materialize();
        B(0x80); B(0xFF); B(0xC0); uint8_t *j1 = jcc8(0x72);
        B(0x80); B(0xFF); B(0xE0); uint8_t *j2 = jcc8(0x73);
        B(0x80); B(0x87 | (set ? 0x08 : 0x20)); W(env->ram_delta); B(imm);
        B(0x83); B(0x2E); W(SA(cycles)); B(15);
        uint8_t *j3 = jcc8(0xEB);
        land8(j1); land8(j2);
        B(0xC7); B(0x06); W(SA(arg)); W(I->pc);
        B(0xC6); B(0x06); W(SA(val)); B(1);
        helper(DRC_H_STEP);
        land8(j3);
        hl_es = 0;
        return 1;
    }

    if (I->pre == 0xED) {
        switch (op) {
        case 0x44:   // NEG
            B(0xF6); B(0xD8);
            fkind = K_SUB; cf_valid = 1; cyc += 8;
            return 1;
        case 0x41: case 0x49: case 0x51: case 0x59: case 0x61: case 0x69: case 0x79:   // OUT (C),r
            clobber(live);
            B(0x88); B(0x06 | (r8x[(op >> 3) & 7] << 3)); W(SA(val));
            B(0x88); B(0x0E); W(SA(arg));
            helper(DRC_H_OUT);
            cyc += 12;
            return 1;
        case 0x43: case 0x53: case 0x73: {   // LD (nn),rr
            uint8_t xr = r16x[(op >> 4) & 3];
            if (I->nn >= 0xC000 && I->nn < 0xDFFF) {
                B(0x89); B(0x06 | (xr << 3)); W(I->nn + env->ram_delta);
                cyc += 20;
                return 1;
            }
            return 0;
        }
        case 0x4B: case 0x5B: case 0x7B: {   // LD rr,(nn)
            read_const16(I->nn, r16x[(op >> 4) & 3]);
            if (op == 0x6B) hl_es = 0;
            cyc += 20;
            return 1;
        }
        }
        return 0;
    }

    if (I->pre == 0xDD || I->pre == 0xFD) {
        uint16_t ixa = I->pre == 0xDD ? ZA(ixl) : ZA(iyl);
        if (I->cb) {
            uint8_t b = (op >> 3) & 7;
            if (op < 0x80) {   // BIT b,(IX+d)
                need_ah(live);
                ix_addr(I->pre, I->d);
                seg_si(live);
                B(0x80); B(0xE4); B(0x01);
                B(0x80); B(0xCC); B(0x10);
                B(0x26); B(0xF6); B(0x04); B(1 << b);
                if (b == 7) {
                    B(0x75); B(0x05); B(0x80); B(0xCC); B(0x44); B(0xEB); B(0x03);
                    B(0x80); B(0xCC); B(0x80);
                } else {
                    B(0x75); B(0x03); B(0x80); B(0xCC); B(0x44);
                }
                fkind = K_AH;
                cyc += 20;
                return 1;
            }
            // SET/RES b,(IX+d)
            uint8_t set = op >= 0xC0;
            uint8_t imm = set ? (1 << b) : (uint8_t) ~(1 << b);
            materialize();
            ix_addr(I->pre, I->d);
            B(0x81); B(0xFE); W(0xC000); uint8_t *j1 = jcc8(0x72);
            B(0x81); B(0xFE); W(0xE000); uint8_t *j2 = jcc8(0x73);
            B(0x80); B(0x84 | (set ? 0x08 : 0x20)); W(env->ram_delta); B(imm);
            B(0x83); B(0x2E); W(SA(cycles)); B(23);
            uint8_t *j3 = jcc8(0xEB);
            land8(j1); land8(j2);
            B(0xC7); B(0x06); W(SA(arg)); W(I->pc);
            B(0xC6); B(0x06); W(SA(val)); B(1);
            helper(DRC_H_STEP);
            land8(j3);
            return 1;
        }
        switch (op) {
        case 0x21: B(0xC7); B(0x06); W(ixa); W(I->nn); cyc += 14; return 1;
        case 0x2A: read_const16(I->nn, X_SI); B(0x89); B(0x36); W(ixa); cyc += 20; return 1;
        case 0x22:
            if (I->nn >= 0xC000 && I->nn < 0xDFFF) {
                B(0x8B); B(0x36); W(ixa);
                B(0x89); B(0x36); W(I->nn + env->ram_delta);
                cyc += 20;
                return 1;
            }
            return 0;
        case 0x23: case 0x2B:
            clobber(live);
            B(0xFF); B(op == 0x23 ? 0x06 : 0x0E); W(ixa);   // inc/dec word [ix]
            cyc += 10;
            return 1;
        case 0xE5:   // PUSH IX
            B(0x8B); B(0x36); W(ixa);
            B(0x8D); B(0x6E); B(0xFE);
            B(0x89); B(0xB6); W(env->ram_delta);
            cyc += 15;
            return 1;
        case 0xE1:   // POP IX
            B(0x8B); B(0xB6); W(env->ram_delta);
            B(0x8D); B(0x6E); B(0x02);
            B(0x89); B(0x36); W(ixa);
            cyc += 14;
            return 1;
        case 0xE3:   // EX (SP),IX
            B(0x8B); B(0x36); W(ixa);
            B(0x87); B(0xB6); W(env->ram_delta);
            B(0x89); B(0x36); W(ixa);
            cyc += 23;
            return 1;
        case 0xF9: B(0x8B); B(0x2E); W(ixa); cyc += 10; return 1;   // LD SP,IX
        case 0xE9:   // JP (IX)
            B(0x8B); B(0x36); W(ixa);
            cyc += 8;
            exit_reg(X_SI, 0);
            return 1;
        case 0x09: case 0x19: case 0x29: case 0x39: {   // ADD IX,rr
            uint8_t rr = (op >> 4) & 3;
            need_ah(live);
            B(0x8B); B(0x3E); W(ixa);                    // mov di, [ix]
            B(0x80); B(0xE4); B(0xC4);                   // and ah, C4h
            B(0x89); B(0xFE);                            // mov si, di
            if (rr == 2) { B(0x01); B(0xFF); }           // add di, di
            else { B(0x01); B(0xC7 | (r16x[rr] << 3)); } // add di, rr
            B(0x80); B(0xD4); B(0x00);                   // adc ah, 0
            if (rr == 2) {
                B(0xF7); B(0xC6); W(0x0800);             // test si, 0800h
            } else {
                B(0x31); B(0xC6 | (r16x[rr] << 3));      // xor si, rr
                B(0x31); B(0xFE);                        // xor si, di
                B(0xF7); B(0xC6); W(0x1000);
            }
            B(0x74); B(0x03); B(0x80); B(0xCC); B(0x10);
            B(0x89); B(0x3E); W(ixa);
            fkind = K_AH;
            cyc += 15;
            return 1;
        }
        case 0x36:   // LD (IX+d),n
            ix_addr(I->pre, I->d);
            write_byte(X_SI, 0xFF, I->nn, live);
            cyc += 19;
            return 1;
        }
        if (op >= 0x70 && op <= 0x77) {   // LD (IX+d),r
            ix_addr(I->pre, I->d);
            write_byte(X_SI, r8x[op & 7], 0, live);
            cyc += 19;
            return 1;
        }
        if ((op & 7) == 6 && op >= 0x46 && op <= 0x7E) {   // LD r,(IX+d)
            uint8_t dst = r8x[(op >> 3) & 7];
            ix_addr(I->pre, I->d);
            seg_si(live);
            B(0x26); B(0x8A); B(0x04 | (dst << 3));
            if (dst == X_BX || dst == 7) hl_es = 0;
            cyc += 19;
            return 1;
        }
        if (op >= 0x86 && op <= 0xBE) {   // ALU A,(IX+d)
            uint8_t y = (op >> 3) & 7;
            ix_addr(I->pre, I->d);
            seg_si(live);
            if (y == 1 || y == 3) need_cf();
            B(0x26); B(alu_x86[y] + 2); B(0x04);
            alu_done(y);
            cyc += 19;
            return 1;
        }
        return 0;
    }

    uint8_t x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    switch (x) {
    case 1: {   // LD r,r'
        if (z == 6) {
            seg_hl(live);
            B(0x26); B(0x8A); B(0x07 | (r8x[y] << 3));
            if (y == 4 || y == 5) hl_es = 0;
            cyc += 7;
        } else if (y == 6) {
            write_byte(X_BX, r8x[z], 0, live);
            cyc += 7;
        } else {
            if (y != z) { B(0x8A); B(0xC0 | (r8x[y] << 3) | r8x[z]); }
            if (y == 4 || y == 5) hl_es = 0;
            cyc += 4;
        }
        return 1;
    }
    case 2:   // ALU A,r
        if (z == 6) {
            seg_hl(live);
            if (y == 1 || y == 3) need_cf();
            B(0x26); B(alu_x86[y] + 2); B(0x07);
            cyc += 7;
        } else {
            if (y == 1 || y == 3) need_cf();
            B(alu_x86[y] + 2); B(0xC0 | r8x[z]);
            cyc += 4;
        }
        alu_done(y);
        return 1;
    case 0:
        switch (z) {
        case 0:
            if (y == 0) { cyc += 4; return 1; }
            if (y == 1) {   // EX AF,AF'
                materialize();
                B(0x87); B(0x06); W(ZA(a_));
                cyc += 4;
                return 1;
            }
            {
                uint16_t target = next + (int8_t) I->nn;
                if (y == 2) {   // DJNZ
                    clobber(live);
                    B(0xFE); B(0xCD);            // dec ch
                    uint8_t *j = jcc8(0x74);     // jz: not taken
                    branch_to(target, 13);
                    land8(j);
                    exit_const(next, 8);
                    return 1;
                }
                if (y == 3) { branch_to(target, 12); return 1; }   // JR
                uint8_t *j = jcond(y - 4);
                // not taken
                uint8_t save_kind = fkind;
                exit_const(next, 7);
                land8(j);
                fkind = save_kind;
                branch_to(target, 12);
                return 1;
            }
        case 1:
            if (y & 1) {   // ADD HL,rr
                uint8_t rr = y >> 1;
                hl_es = 0;
                if (!(live_out[i] & (FS | FZ | FP | FH | FN))) {
                    clobber(0);
                    B(0x01); B(0xC3 | (r16x[rr] << 3));   // add bx, rr
                    fkind = K_ADD; cf_valid = 1;
                    cyc += 11;
                    return 1;
                }
                need_ah(live);
                B(0x80); B(0xE4); B(0xC4);
                B(0x89); B(0xDE);                          // mov si, bx
                B(0x01); B(0xC3 | (r16x[rr] << 3));        // add bx, rr
                B(0x80); B(0xD4); B(0x00);
                if (rr == 2) {
                    B(0xF7); B(0xC6); W(0x0800);
                } else {
                    B(0x31); B(0xC6 | (r16x[rr] << 3));
                    B(0x31); B(0xDE);
                    B(0xF7); B(0xC6); W(0x1000);
                }
                B(0x74); B(0x03); B(0x80); B(0xCC); B(0x10);
                fkind = K_AH;
                cyc += 11;
                return 1;
            }
            B(0xB8 + r16x[y >> 1]); W(I->nn);   // LD rr,nn
            if (y == 4) hl_es = 0;
            cyc += 10;
            return 1;
        case 2:
            switch (y) {
            case 0: case 2: {   // LD (BC),A / LD (DE),A
                B(0x89); B(y ? 0xD6 : 0xCE);   // mov si, dx / cx
                write_byte(X_SI, X_AX, 0, live);
                cyc += 7;
                return 1;
            }
            case 1: case 3: {   // LD A,(BC) / (DE)
                B(0x89); B(y == 3 ? 0xD6 : 0xCE);
                seg_si(live);
                B(0x26); B(0x8A); B(0x04);
                cyc += 7;
                return 1;
            }
            case 4:   // LD (nn),HL
                if (I->nn >= 0xC000 && I->nn < 0xDFFF) {
                    B(0x89); B(0x1E); W(I->nn + env->ram_delta);
                } else {
                    write_const(I->nn, X_BX, live);
                    write_const(I->nn + 1, 7, live);
                }
                cyc += 16;
                if (I->term == T_END_AFTER) { force_nolink = 1; exit_const(next, 0); }
                return 1;
            case 5:   // LD HL,(nn)
                read_const16(I->nn, X_BX);
                hl_es = 0;
                cyc += 16;
                return 1;
            case 6:   // LD (nn),A
                write_const(I->nn, X_AX, live);
                cyc += 13;
                if (I->term == T_END_AFTER) { force_nolink = 1; exit_const(next, 0); }
                return 1;
            case 7:   // LD A,(nn)
                read_const(I->nn, X_AX);
                cyc += 13;
                return 1;
            }
            return 0;
        case 3: {   // INC/DEC rr
            uint8_t rr = y >> 1, dec = y & 1;
            uint8_t xr = r16x[rr];
            if (xr == X_BX || xr == X_BP) {
                B(0x8D); B(xr == X_BX ? 0x5F : 0x6E); B(dec ? 0xFF : 0x01);
                if (xr == X_BX) hl_es = 0;
            } else if (fkind != K_AH && live) {
                // keep the x86 flags: xchg, lea, xchg
                B(0x87); B(0xC3 | (xr << 3));
                B(0x8D); B(0x5F); B(dec ? 0xFF : 0x01);
                B(0x87); B(0xC3 | (xr << 3));
            } else {
                clobber(0);
                B((dec ? 0x48 : 0x40) + xr);
            }
            cyc += 6;
            return 1;
        }
        case 4: case 5: {   // INC/DEC r
            uint8_t dec = z == 5;
            if (y == 6) {
                // (HL): RAM directly, anything else with the interpreter
                materialize();
                B(0x80); B(0xFF); B(0xC0); uint8_t *j1 = jcc8(0x72);
                B(0x80); B(0xFF); B(0xE0); uint8_t *j2 = jcc8(0x73);
                B(0x9E);                                              // sahf
                B(0xFE); B(0x87 | (dec ? 0x08 : 0)); W(env->ram_delta);
                fkind = dec ? K_DEC : K_INC; cf_valid = 1;
                materialize();
                B(0x83); B(0x2E); W(SA(cycles)); B(11);
                uint8_t *j3 = jcc8(0xEB);
                land8(j1); land8(j2);
                B(0xC7); B(0x06); W(SA(arg)); W(I->pc);
                B(0xC6); B(0x06); W(SA(val)); B(1);
                helper(DRC_H_STEP);
                land8(j3);
                fkind = K_AH;
                return 1;
            }
            inc_dec_flags(dec, live_out[i] & FC);
            B(0xFE); B((dec ? 0xC8 : 0xC0) | r8x[y]);
            fkind = dec ? K_DEC : K_INC;
            if (y == 4 || y == 5) hl_es = 0;
            cyc += 4;
            return 1;
        }
        case 6:   // LD r,n
            if (y == 6) { write_byte(X_BX, 0xFF, I->nn, live); cyc += 10; return 1; }
            B(0xB0 + r8x[y]); B(I->nn);
            if (y == 4 || y == 5) hl_es = 0;
            cyc += 7;
            return 1;
        case 7:
            switch (y) {
            case 0: case 1: case 2: case 3: {
                static const uint8_t rot[4] = { 0xC0, 0xC8, 0xD0, 0xD8 };   // rol ror rcl rcr al
                need_ah(live);
                B(0x80); B(0xE4); B(y >= 2 ? 0xC5 : 0xC4);
                B(0x9E);
                B(0xD0); B(rot[y]);
                B(0x9F);
                B(0x80); B(0xE4); B(0xD5);
                fkind = K_AH;
                cyc += 4;
                return 1;
            }
            case 5:   // CPL
                need_ah(live);
                B(0xF6); B(0xD0);
                B(0x80); B(0xCC); B(0x12);
                cyc += 4;
                return 1;
            case 6:   // SCF
                need_ah(live);
                B(0x80); B(0xE4); B(0xC4);
                B(0x80); B(0xCC); B(0x01);
                cyc += 4;
                return 1;
            case 7:   // CCF
                need_ah(live);
                B(0x80); B(0xE4); B(0xC5);
                B(0xF6); B(0xC4); B(0x01);
                B(0x74); B(0x03); B(0x80); B(0xCC); B(0x10);
                B(0x80); B(0xF4); B(0x01);
                cyc += 4;
                return 1;
            }
            return 0;
        }
        return 0;
    case 3:
        switch (z) {
        case 0: {   // RET cc
            uint8_t *j = jcond(y);
            uint8_t save_kind = fkind;
            exit_const(next, 5);
            land8(j);
            fkind = save_kind;
            B(0x8B); B(0xB6); W(env->ram_delta);   // mov si, [bp+d]
            B(0x8D); B(0x6E); B(0x02);
            exit_reg(X_SI, 11);
            return 1;
        }
        case 1:
            if (y == 1) {   // RET
                B(0x8B); B(0xB6); W(env->ram_delta);
                B(0x8D); B(0x6E); B(0x02);
                exit_reg(X_SI, 10);
                return 1;
            }
            if (y == 5) { exit_reg(X_BX, 4); return 1; }   // JP (HL)
            if (y == 3) {   // EXX
                B(0x87); B(0x0E); W(ZA(c_));
                B(0x87); B(0x16); W(ZA(e_));
                B(0x87); B(0x1E); W(ZA(l_));
                hl_es = 0;
                cyc += 4;
                return 1;
            }
            if (y == 7) { B(0x89); B(0xDD); cyc += 6; return 1; }   // LD SP,HL
            // POP
            if (y == 6) {
                B(0x8B); B(0x86); W(env->ram_delta);   // mov ax, [bp+d]
                B(0x86); B(0xC4);                      // xchg al, ah
                fkind = K_AH;
            } else {
                uint8_t xr = r16x[y >> 1];
                B(0x8B); B(0x86 | (xr << 3)); W(env->ram_delta);
                if (xr == X_BX) hl_es = 0;
            }
            B(0x8D); B(0x6E); B(0x02);
            cyc += 10;
            return 1;
        case 2: {   // JP cc,nn
            uint8_t *j = jcond(y);
            uint8_t save_kind = fkind;
            exit_const(next, 10);
            land8(j);
            fkind = save_kind;
            branch_to(I->nn, 10);
            return 1;
        }
        case 3:
            switch (y) {
            case 0: branch_to(I->nn, 10); return 1;   // JP nn
            case 2:   // OUT (n),A
                clobber(live);
                B(0xA2); W(SA(val));
                B(0xC7); B(0x06); W(SA(arg)); W(I->nn);
                helper(DRC_H_OUT);
                cyc += 11;
                return 1;
            case 3:   // IN A,(n)
                clobber(live);
                B(0xC7); B(0x06); W(SA(arg)); W(I->nn);
                helper(DRC_H_IN);
                B(0xA0); W(SA(val));
                cyc += 11;
                return 1;
            case 6:   // DI
                B(0xC7); B(0x06); W(ZA(iff1)); W(0);   // iff1 = iff2 = 0
                cyc += 4;
                return 1;
            case 4:   // EX (SP),HL
                B(0x87); B(0x9E); W(env->ram_delta);
                hl_es = 0;
                cyc += 19;
                return 1;
            case 5:   // EX DE,HL
                B(0x87); B(0xD3);
                hl_es = 0;
                cyc += 4;
                return 1;
            }
            return 0;
        case 4: {   // CALL cc,nn
            uint8_t *j = jcond(y);
            uint8_t save_kind = fkind;
            exit_const(next, 10);
            land8(j);
            fkind = save_kind;
            push_imm(next);
            exit_const(I->nn, 17);
            return 1;
        }
        case 5:
            if (y == 1) {   // CALL nn
                push_imm(next);
                exit_const(I->nn, 17);
                return 1;
            }
            // PUSH
            B(0x8D); B(0x6E); B(0xFE);
            if (y == 6) {
                materialize();
                B(0x86); B(0xC4);                          // xchg al, ah
                B(0x89); B(0x86); W(env->ram_delta);
                B(0x86); B(0xC4);
            } else {
                B(0x89); B(0x86 | (r16x[y >> 1] << 3)); W(env->ram_delta);
            }
            cyc += 11;
            return 1;
        case 6:   // ALU A,n
            if (y == 1 || y == 3) need_cf();
            B(alu_x86[y] + 4); B(I->nn);
            alu_done(y);
            cyc += 7;
            return 1;
        case 7:   // RST
            push_imm(next);
            exit_const(y << 3, 11);
            return 1;
        }
    }
    return 0;
}

int drc_translate(const drc_env_t *e, uint16_t pc, uint8_t *out,
                  uint16_t out_off, uint16_t out_max, drc_result_t *res) {
    env = e;
    // decode
    int n = 0;
    uint16_t p = pc;
    // every instruction fits in 96 bytes of x86 code: stop early enough
    int max_n = (out_max - 128) / 96;
    if (max_n > MAX_INSTR) max_n = MAX_INSTR;
    while (n < max_n) {
        if (n && ((p ^ pc) & 0xC000)) break;   // stay inside one 16 KiB slot
        decode(p, &ins[n]);
        if (ins[n].term == T_STOP) break;
        p += ins[n].len;
        n++;
        if (ins[n - 1].term == T_END || ins[n - 1].term == T_END_AFTER) break;
    }
    if (n == 0) return 0;

    // liveness of the flags (all live at the end of the block)
    uint8_t live = FALL;
    for (int i = n - 1; i >= 0; i--) {
        live_out[i] = live;
        if (ins[i].term == T_END) live = FALL;
        live = (live & ~ins[i].fwrite) | ins[i].fread;
        if (ins[i].step) live = FALL;
    }

    // generate
    o = o_start = out; o_base = out_off; o_limit = out + out_max;
    block_start_off = out_off; block_pc = pc;
    fkind = K_AH; cf_valid = 0; hl_es = 0; cyc = 0;
    force_nolink = 0;
    int i;
    for (i = 0; i < n; i++) {
        // emit() can only refuse an instruction that decode() accepted by
        // mistake: the liveness analysis assumed it, so give up entirely
        if (!emit(i))
            return 0;
        if (ins[i].term == T_END || ins[i].term == T_END_AFTER) { i++; goto done; }
    }
    if (i == 0) return 0;
    // fell off the end: continue at the next instruction
    exit_const(ins[i - 1].pc + ins[i - 1].len, 0);
done:
    res->length = (uint16_t) (o - o_start);
    res->z80_instr = i;
    res->z80_end = ins[i - 1].pc + ins[i - 1].len;
    return 1;
}
