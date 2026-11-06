/**
 * ggswan - Game Gear emulator for the WonderSwan (nileswan)
 * Z80 interpreter core.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include "z80.h"

z80_t z80;
#ifdef Z80_COUNT
uint32_t z80_count;
#endif

// S, Z, undocumented 5/3 bits of a value; and the same plus parity
#ifdef __ia16__
// (WonderSwan: in the free internal RAM, see ws/main.c "IRAM layout")
#define sz_table ((uint8_t*) 0xF000)
#define szp_table ((uint8_t*) 0xF100)
#else
static uint8_t sz_table[256];
static uint8_t szp_table[256];
#endif

#define FC Z80_FLAG_C
#define FN Z80_FLAG_N
#define FP Z80_FLAG_P
#define FV Z80_FLAG_V
#define FH Z80_FLAG_H
#define FZ Z80_FLAG_Z
#define FS Z80_FLAG_S
#define FXY (Z80_FLAG_3 | Z80_FLAG_5)

// base cycles of the unprefixed opcodes (taken branches add more)
static const uint8_t cycles_main[256] = {
     4,10, 7, 6, 4, 4, 7, 4, 4,11, 7, 6, 4, 4, 7, 4,
     8,10, 7, 6, 4, 4, 7, 4,12,11, 7, 6, 4, 4, 7, 4,
     7,10,16, 6, 4, 4, 7, 4, 7,11,16, 6, 4, 4, 7, 4,
     7,10,13, 6,11,11,10, 4, 7,11,13, 6, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     7, 7, 7, 7, 7, 7, 4, 7, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     5,10,10,10,10,11, 7,11, 5,10,10, 0,10,17, 7,11,
     5,10,10,11,10,11, 7,11, 5, 4,10,11,10, 0, 7,11,
     5,10,10,19,10,11, 7,11, 5, 4,10, 4,10, 0, 7,11,
     5,10,10, 4,10,11, 7,11, 5, 6,10, 4,10, 0, 7,11
};

void z80_init(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t f = (i & (FS | FXY)) | (i == 0 ? FZ : 0);
        sz_table[i] = f;
        uint8_t p = i;
        p ^= p >> 4;
        p ^= p >> 2;
        p ^= p >> 1;
        szp_table[i] = f | ((p & 1) ? 0 : FP);
    }
    z80_reset();
}

void z80_reset(void) {
    z80.a = 0xFF; z80.f = 0xFF;
    z80.b = z80.c = z80.d = z80.e = z80.h = z80.l = 0;
    z80.a_ = z80.f_ = z80.b_ = z80.c_ = z80.d_ = z80.e_ = z80.h_ = z80.l_ = 0;
    z80.ixh = z80.ixl = z80.iyh = z80.iyl = 0xFF;
    z80.sp = 0xDFF0;
    z80.pc = 0;
    z80.i = 0; z80.r = 0;
    z80.iff1 = z80.iff2 = 0;
    z80.im = 0;
    z80.halted = 0;
    z80.ei_pending = 0;
    z80.irq = 0;
    z80.cycles = 0;
}

/* === Helpers === */

#define RD(a) z80_read(a)
#define WR(a, v) z80_write((a), (v))

static inline uint8_t fetch(void) {
    return RD(z80.pc++);
}

static inline uint16_t fetch16(void) {
    uint8_t lo = fetch();
    return lo | (fetch() << 8);
}

static inline uint16_t read16(uint16_t a) {
    uint8_t lo = RD(a);
    return lo | (RD((uint16_t) (a + 1)) << 8);
}

static inline void write16(uint16_t a, uint16_t v) {
    WR(a, v & 0xFF);
    WR((uint16_t) (a + 1), v >> 8);
}

static inline void push16(uint16_t v) {
    z80.sp -= 2;
    write16(z80.sp, v);
}

static inline uint16_t pop16(void) {
    uint16_t v = read16(z80.sp);
    z80.sp += 2;
    return v;
}

static inline void inc_r(void) {
    z80.r = (z80.r & 0x80) | ((z80.r + 1) & 0x7F);
}

#define BC() ((uint16_t) ((z80.b << 8) | z80.c))
#define DE() ((uint16_t) ((z80.d << 8) | z80.e))
#define SET_BC(v) do { uint16_t _v = (v); z80.b = _v >> 8; z80.c = _v; } while (0)
#define SET_DE(v) do { uint16_t _v = (v); z80.d = _v >> 8; z80.e = _v; } while (0)

// The "HL" of the current instruction: HL, IX or IY (DD/FD prefixes).
static uint8_t idx;
static uint8_t *rh, *rl;

#define HLX() ((uint16_t) ((*rh << 8) | *rl))
#define SET_HLX(v) do { uint16_t _v = (v); *rh = _v >> 8; *rl = _v; } while (0)
#define HL() ((uint16_t) ((z80.h << 8) | z80.l))
#define SET_HL(v) do { uint16_t _v = (v); z80.h = _v >> 8; z80.l = _v; } while (0)

// Address of the (HL) operand: HL, or IX/IY + displacement.
static inline uint16_t addr_hlx(void) {
    if (!idx)
        return HLX();
    return (uint16_t) (HLX() + (int8_t) fetch());
}

// Register by its 3-bit code (6 = (HL): not handled here). With index,
// H/L are IXH/IXL (undocumented) unless "real" is set.
static inline uint8_t *reg_ptr(uint8_t n, uint8_t real) {
    switch (n) {
    case 0: return &z80.b;
    case 1: return &z80.c;
    case 2: return &z80.d;
    case 3: return &z80.e;
    case 4: return real ? &z80.h : rh;
    case 5: return real ? &z80.l : rl;
    default: return &z80.a;
    }
}

/* === ALU === */

static inline void alu_add(uint8_t v, uint8_t carry) {
    uint16_t r = z80.a + v + carry;
    z80.f = sz_table[r & 0xFF] | ((z80.a ^ v ^ r) & FH) | ((r >> 8) & FC)
        | ((((z80.a ^ ~v) & (z80.a ^ r)) & 0x80) >> 5);
    z80.a = r;
}

static inline uint8_t alu_sub(uint8_t v, uint8_t carry) {
    uint16_t r = z80.a - v - carry;
    z80.f = sz_table[r & 0xFF] | FN | ((z80.a ^ v ^ r) & FH) | ((r >> 8) & FC)
        | ((((z80.a ^ v) & (z80.a ^ r)) & 0x80) >> 5);
    return r;
}

static inline void alu_op(uint8_t op, uint8_t v) {
    switch (op & 7) {
    case 0: alu_add(v, 0); break;
    case 1: alu_add(v, z80.f & FC); break;
    case 2: z80.a = alu_sub(v, 0); break;
    case 3: z80.a = alu_sub(v, z80.f & FC); break;
    case 4: z80.a &= v; z80.f = szp_table[z80.a] | FH; break;
    case 5: z80.a ^= v; z80.f = szp_table[z80.a]; break;
    case 6: z80.a |= v; z80.f = szp_table[z80.a]; break;
    case 7:
        alu_sub(v, 0);
        // CP: the 5/3 bits come from the operand
        z80.f = (z80.f & ~FXY) | (v & FXY);
        break;
    }
}

static inline uint8_t alu_inc(uint8_t v) {
    uint8_t r = v + 1;
    z80.f = (z80.f & FC) | sz_table[r] | ((r & 0x0F) ? 0 : FH) | (r == 0x80 ? FV : 0);
    return r;
}

static inline uint8_t alu_dec(uint8_t v) {
    uint8_t r = v - 1;
    z80.f = (z80.f & FC) | FN | sz_table[r] | ((v & 0x0F) ? 0 : FH) | (r == 0x7F ? FV : 0);
    return r;
}

static inline uint16_t alu_add16(uint16_t a, uint16_t v) {
    uint32_t r = (uint32_t) a + v;
    z80.f = (z80.f & (FS | FZ | FV)) | (((a ^ v ^ r) >> 8) & FH) | ((r >> 16) & FC)
        | ((r >> 8) & FXY);
    return r;
}

static inline uint16_t alu_adc16(uint16_t a, uint16_t v) {
    uint32_t r = (uint32_t) a + v + (z80.f & FC);
    uint16_t r16 = r;
    z80.f = ((r16 >> 8) & (FS | FXY)) | (r16 ? 0 : FZ) | (((a ^ v ^ r) >> 8) & FH)
        | ((r >> 16) & FC) | ((((a ^ ~v) & (a ^ r)) & 0x8000) >> 13);
    return r16;
}

static inline uint16_t alu_sbc16(uint16_t a, uint16_t v) {
    uint32_t r = (uint32_t) a - v - (z80.f & FC);
    uint16_t r16 = r;
    z80.f = ((r16 >> 8) & (FS | FXY)) | (r16 ? 0 : FZ) | FN | (((a ^ v ^ r) >> 8) & FH)
        | ((r >> 16) & FC) | ((((a ^ v) & (a ^ r)) & 0x8000) >> 13);
    return r16;
}

// CB rotates and shifts
static inline uint8_t alu_rot(uint8_t op, uint8_t v) {
    uint8_t r, c;
    switch (op & 7) {
    case 0: c = v >> 7; r = (v << 1) | c; break;                    // RLC
    case 1: c = v & 1; r = (v >> 1) | (c << 7); break;              // RRC
    case 2: c = v >> 7; r = (v << 1) | (z80.f & FC); break;         // RL
    case 3: c = v & 1; r = (v >> 1) | ((z80.f & FC) << 7); break;   // RR
    case 4: c = v >> 7; r = v << 1; break;                          // SLA
    case 5: c = v & 1; r = (v >> 1) | (v & 0x80); break;            // SRA
    case 6: c = v >> 7; r = (v << 1) | 1; break;                    // SLL
    default: c = v & 1; r = v >> 1; break;                          // SRL
    }
    z80.f = szp_table[r] | c;
    return r;
}

static inline void alu_bit(uint8_t b, uint8_t v) {
    uint8_t m = v & (1 << b);
    z80.f = (z80.f & FC) | FH | (v & FXY) | (m ? (m & FS) : (FZ | FP));
}

static void daa(void) {
    uint8_t a = z80.a, diff = 0, c = z80.f & FC, h;
    if ((z80.f & FH) || (a & 0x0F) > 9)
        diff = 0x06;
    if (c || a > 0x99) {
        diff |= 0x60;
        c = FC;
    }
    if (z80.f & FN) {
        h = ((z80.f & FH) && (a & 0x0F) < 6) ? FH : 0;
        a -= diff;
    } else {
        h = ((a & 0x0F) > 9) ? FH : 0;
        a += diff;
    }
    z80.f = szp_table[a] | (z80.f & FN) | c | h;
    z80.a = a;
}

static inline uint8_t cond(uint8_t n) {
    switch (n & 7) {
    case 0: return !(z80.f & FZ);
    case 1: return z80.f & FZ;
    case 2: return !(z80.f & FC);
    case 3: return z80.f & FC;
    case 4: return !(z80.f & FP);
    case 5: return z80.f & FP;
    case 6: return !(z80.f & FS);
    default: return z80.f & FS;
    }
}

/* === CB prefix === */

static int exec_cb(void) {
    uint16_t addr = 0;
    uint8_t op, v;
    if (idx) {
        // DD CB d op
        addr = (uint16_t) (HLX() + (int8_t) fetch());
        op = fetch();
        v = RD(addr);
    } else {
        op = fetch();
        inc_r();
        if ((op & 7) == 6) {
            addr = HL();
            v = RD(addr);
        } else {
            v = *reg_ptr(op & 7, 1);
        }
    }

    uint8_t b = (op >> 3) & 7;
    uint8_t r;
    switch (op >> 6) {
    case 0: r = alu_rot(op >> 3, v); break;
    case 1:
        alu_bit(b, v);
        if (idx || (op & 7) == 6) {
            // 5/3 bits from the high byte of the address (approximation)
            z80.f = (z80.f & ~FXY) | ((addr >> 8) & FXY);
            return idx ? 20 : 12;
        }
        return 8;
    case 2: r = v & ~(1 << b); break;
    default: r = v | (1 << b); break;
    }

    if (idx) {
        WR(addr, r);
        // undocumented: the result also goes to the register
        if ((op & 7) != 6)
            *reg_ptr(op & 7, 1) = r;
        return 23;
    }
    if ((op & 7) == 6) {
        WR(addr, r);
        return 15;
    }
    *reg_ptr(op & 7, 1) = r;
    return 8;
}

/* === ED prefix === */

static int exec_ed(void) {
    uint8_t op = fetch();
    inc_r();
    uint8_t v;
    uint16_t hl, t;

    switch (op) {
    // IN r,(C)
    case 0x40: case 0x48: case 0x50: case 0x58: case 0x60: case 0x68: case 0x70: case 0x78:
        v = z80_in(z80.c);
        z80.f = (z80.f & FC) | szp_table[v];
        if (op != 0x70)
            *reg_ptr((op >> 3) & 7, 1) = v;
        return 12;
    // OUT (C),r
    case 0x41: case 0x49: case 0x51: case 0x59: case 0x61: case 0x69: case 0x71: case 0x79:
        z80_out(z80.c, op == 0x71 ? 0 : *reg_ptr((op >> 3) & 7, 1));
        return 12;
    // SBC HL,rr / ADC HL,rr
    case 0x42: SET_HL(alu_sbc16(HL(), BC())); return 15;
    case 0x52: SET_HL(alu_sbc16(HL(), DE())); return 15;
    case 0x62: SET_HL(alu_sbc16(HL(), HL())); return 15;
    case 0x72: SET_HL(alu_sbc16(HL(), z80.sp)); return 15;
    case 0x4A: SET_HL(alu_adc16(HL(), BC())); return 15;
    case 0x5A: SET_HL(alu_adc16(HL(), DE())); return 15;
    case 0x6A: SET_HL(alu_adc16(HL(), HL())); return 15;
    case 0x7A: SET_HL(alu_adc16(HL(), z80.sp)); return 15;
    // LD (nn),rr / LD rr,(nn)
    case 0x43: write16(fetch16(), BC()); return 20;
    case 0x53: write16(fetch16(), DE()); return 20;
    case 0x63: write16(fetch16(), HL()); return 20;
    case 0x73: write16(fetch16(), z80.sp); return 20;
    case 0x4B: SET_BC(read16(fetch16())); return 20;
    case 0x5B: SET_DE(read16(fetch16())); return 20;
    case 0x6B: SET_HL(read16(fetch16())); return 20;
    case 0x7B: z80.sp = read16(fetch16()); return 20;
    // NEG
    case 0x44: case 0x4C: case 0x54: case 0x5C: case 0x64: case 0x6C: case 0x74: case 0x7C:
        v = z80.a;
        z80.a = 0;
        z80.a = alu_sub(v, 0);
        return 8;
    // RETN / RETI
    case 0x45: case 0x4D: case 0x55: case 0x5D: case 0x65: case 0x6D: case 0x75: case 0x7D:
        z80.pc = pop16();
        z80.iff1 = z80.iff2;
        return 14;
    // IM
    case 0x46: case 0x4E: case 0x66: case 0x6E: z80.im = 0; return 8;
    case 0x56: case 0x76: z80.im = 1; return 8;
    case 0x5E: case 0x7E: z80.im = 2; return 8;
    // LD I,A / LD R,A / LD A,I / LD A,R
    case 0x47: z80.i = z80.a; return 9;
    case 0x4F: z80.r = z80.a; return 9;
    case 0x57:
        z80.a = z80.i;
        z80.f = (z80.f & FC) | sz_table[z80.a] | (z80.iff2 ? FV : 0);
        return 9;
    case 0x5F:
        z80.a = z80.r;
        z80.f = (z80.f & FC) | sz_table[z80.a] | (z80.iff2 ? FV : 0);
        return 9;
    // RRD / RLD
    case 0x67:
        hl = HL();
        v = RD(hl);
        WR(hl, (z80.a << 4) | (v >> 4));
        z80.a = (z80.a & 0xF0) | (v & 0x0F);
        z80.f = (z80.f & FC) | szp_table[z80.a];
        return 18;
    case 0x6F:
        hl = HL();
        v = RD(hl);
        WR(hl, (v << 4) | (z80.a & 0x0F));
        z80.a = (z80.a & 0xF0) | (v >> 4);
        z80.f = (z80.f & FC) | szp_table[z80.a];
        return 18;

    // block transfers: LDI LDD LDIR LDDR
    case 0xA0: case 0xA8: case 0xB0: case 0xB8: {
        int8_t step = (op & 8) ? -1 : 1;
        hl = HL();
        t = DE();
        v = RD(hl);
        WR(t, v);
        SET_HL(hl + step);
        SET_DE(t + step);
        SET_BC(BC() - 1);
        uint8_t n = v + z80.a;
        z80.f = (z80.f & (FS | FZ | FC)) | (BC() ? FV : 0) | (n & Z80_FLAG_3) | ((n & 0x02) << 4);
        if ((op & 0x10) && BC()) {
            z80.pc -= 2;
            return 21;
        }
        return 16;
    }
    // CPI CPD CPIR CPDR
    case 0xA1: case 0xA9: case 0xB1: case 0xB9: {
        int8_t step = (op & 8) ? -1 : 1;
        hl = HL();
        v = RD(hl);
        uint8_t r = z80.a - v;
        uint8_t h = (z80.a ^ v ^ r) & FH;
        SET_HL(hl + step);
        SET_BC(BC() - 1);
        uint8_t n = r - (h ? 1 : 0);
        z80.f = (z80.f & FC) | FN | (sz_table[r] & (FS | FZ)) | h | (BC() ? FV : 0)
            | (n & Z80_FLAG_3) | ((n & 0x02) << 4);
        if ((op & 0x10) && BC() && r) {
            z80.pc -= 2;
            return 21;
        }
        return 16;
    }
    // INI IND INIR INDR
    case 0xA2: case 0xAA: case 0xB2: case 0xBA: {
        int8_t step = (op & 8) ? -1 : 1;
        v = z80_in(z80.c);
        hl = HL();
        WR(hl, v);
        SET_HL(hl + step);
        z80.b--;
        z80.f = sz_table[z80.b] | ((v & 0x80) ? FN : 0);
        if ((op & 0x10) && z80.b) {
            z80.pc -= 2;
            return 21;
        }
        return 16;
    }
    // OUTI OUTD OTIR OTDR
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: {
        int8_t step = (op & 8) ? -1 : 1;
        hl = HL();
        v = RD(hl);
        z80.b--;
        z80_out(z80.c, v);
        SET_HL(hl + step);
        z80.f = sz_table[z80.b] | ((v & 0x80) ? FN : 0);
        if ((op & 0x10) && z80.b) {
            z80.pc -= 2;
            return 21;
        }
        return 16;
    }
    default:
        // undefined ED opcodes: two NOPs
        return 8;
    }
}

/* === Main opcodes === */

static int exec_op(uint8_t op) {
    uint8_t v, *p;
    uint16_t a, t;
    int extra = 0;

    switch (op) {
    case 0x00: break; // NOP
    // LD rr,nn
    case 0x01: SET_BC(fetch16()); break;
    case 0x11: SET_DE(fetch16()); break;
    case 0x21: SET_HLX(fetch16()); break;
    case 0x31: z80.sp = fetch16(); break;
    // LD (BC),A / LD (DE),A / LD A,(BC) / LD A,(DE)
    case 0x02: WR(BC(), z80.a); break;
    case 0x12: WR(DE(), z80.a); break;
    case 0x0A: z80.a = RD(BC()); break;
    case 0x1A: z80.a = RD(DE()); break;
    // LD (nn),HL / LD HL,(nn) / LD (nn),A / LD A,(nn)
    case 0x22: write16(fetch16(), HLX()); break;
    case 0x2A: SET_HLX(read16(fetch16())); break;
    case 0x32: WR(fetch16(), z80.a); break;
    case 0x3A: z80.a = RD(fetch16()); break;
    // INC rr / DEC rr
    case 0x03: SET_BC(BC() + 1); break;
    case 0x13: SET_DE(DE() + 1); break;
    case 0x23: SET_HLX(HLX() + 1); break;
    case 0x33: z80.sp++; break;
    case 0x0B: SET_BC(BC() - 1); break;
    case 0x1B: SET_DE(DE() - 1); break;
    case 0x2B: SET_HLX(HLX() - 1); break;
    case 0x3B: z80.sp--; break;
    // ADD HL,rr
    case 0x09: SET_HLX(alu_add16(HLX(), BC())); break;
    case 0x19: SET_HLX(alu_add16(HLX(), DE())); break;
    case 0x29: SET_HLX(alu_add16(HLX(), HLX())); break;
    case 0x39: SET_HLX(alu_add16(HLX(), z80.sp)); break;
    // INC r / DEC r / LD r,n
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x3C:
        p = reg_ptr(op >> 3, 0);
        *p = alu_inc(*p);
        break;
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x3D:
        p = reg_ptr(op >> 3, 0);
        *p = alu_dec(*p);
        break;
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x3E:
        *reg_ptr(op >> 3, 0) = fetch();
        break;
    case 0x34:
        a = addr_hlx();
        WR(a, alu_inc(RD(a)));
        if (idx) extra = 8;
        break;
    case 0x35:
        a = addr_hlx();
        WR(a, alu_dec(RD(a)));
        if (idx) extra = 8;
        break;
    case 0x36:
        a = addr_hlx();
        WR(a, fetch());
        if (idx) extra = 5;
        break;
    // rotates of A
    case 0x07: // RLCA
        z80.a = (z80.a << 1) | (z80.a >> 7);
        z80.f = (z80.f & (FS | FZ | FP)) | (z80.a & (FC | FXY));
        break;
    case 0x0F: // RRCA
        z80.f = (z80.f & (FS | FZ | FP)) | (z80.a & FC);
        z80.a = (z80.a >> 1) | (z80.a << 7);
        z80.f |= z80.a & FXY;
        break;
    case 0x17: // RLA
        v = z80.a >> 7;
        z80.a = (z80.a << 1) | (z80.f & FC);
        z80.f = (z80.f & (FS | FZ | FP)) | v | (z80.a & FXY);
        break;
    case 0x1F: // RRA
        v = z80.a & 1;
        z80.a = (z80.a >> 1) | ((z80.f & FC) << 7);
        z80.f = (z80.f & (FS | FZ | FP)) | v | (z80.a & FXY);
        break;
    case 0x08: // EX AF,AF'
        v = z80.a; z80.a = z80.a_; z80.a_ = v;
        v = z80.f; z80.f = z80.f_; z80.f_ = v;
        break;
    case 0x10: // DJNZ
        v = fetch();
        if (--z80.b) {
            z80.pc += (int8_t) v;
            extra = 5;
        }
        break;
    case 0x18: // JR
        v = fetch();
        z80.pc += (int8_t) v;
        break;
    case 0x20: case 0x28: case 0x30: case 0x38: // JR cc
        v = fetch();
        if (cond((op >> 3) & 3)) {
            z80.pc += (int8_t) v;
            extra = 5;
        }
        break;
    case 0x27: daa(); break;
    case 0x2F: // CPL
        z80.a = ~z80.a;
        z80.f = (z80.f & (FS | FZ | FP | FC)) | FH | FN | (z80.a & FXY);
        break;
    case 0x37: // SCF
        z80.f = (z80.f & (FS | FZ | FP)) | FC | (z80.a & FXY);
        break;
    case 0x3F: // CCF
        z80.f = ((z80.f & (FS | FZ | FP | FC)) | ((z80.f & FC) ? FH : 0) | (z80.a & FXY)) ^ FC;
        break;

    case 0x76: // HALT
        z80.halted = 1;
        break;

    // CB / ED / DD / FD handled by the caller

    // RET cc / RET / CALL cc / CALL / JP cc / JP / RST
    case 0xC0: case 0xC8: case 0xD0: case 0xD8: case 0xE0: case 0xE8: case 0xF0: case 0xF8:
        if (cond(op >> 3)) {
            z80.pc = pop16();
            extra = 6;
        }
        break;
    case 0xC9: z80.pc = pop16(); break;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: case 0xE2: case 0xEA: case 0xF2: case 0xFA:
        t = fetch16();
        if (cond(op >> 3))
            z80.pc = t;
        break;
    case 0xC3: z80.pc = fetch16(); break;
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: case 0xE4: case 0xEC: case 0xF4: case 0xFC:
        t = fetch16();
        if (cond(op >> 3)) {
            push16(z80.pc);
            z80.pc = t;
            extra = 7;
        }
        break;
    case 0xCD:
        t = fetch16();
        push16(z80.pc);
        z80.pc = t;
        break;
    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        push16(z80.pc);
        z80.pc = op & 0x38;
        break;
    // PUSH / POP
    case 0xC5: push16(BC()); break;
    case 0xD5: push16(DE()); break;
    case 0xE5: push16(HLX()); extra = idx ? 0 : 0; break;
    case 0xF5: push16((z80.a << 8) | z80.f); break;
    case 0xC1: SET_BC(pop16()); break;
    case 0xD1: SET_DE(pop16()); break;
    case 0xE1: SET_HLX(pop16()); break;
    case 0xF1: t = pop16(); z80.a = t >> 8; z80.f = t; break;
    // ALU A,n
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
        alu_op(op >> 3, fetch());
        break;
    case 0xD3: z80_out(fetch(), z80.a); break;          // OUT (n),A
    case 0xDB: z80.a = z80_in(fetch()); break;          // IN A,(n)
    case 0xD9: // EXX
        v = z80.b; z80.b = z80.b_; z80.b_ = v;
        v = z80.c; z80.c = z80.c_; z80.c_ = v;
        v = z80.d; z80.d = z80.d_; z80.d_ = v;
        v = z80.e; z80.e = z80.e_; z80.e_ = v;
        v = z80.h; z80.h = z80.h_; z80.h_ = v;
        v = z80.l; z80.l = z80.l_; z80.l_ = v;
        break;
    case 0xE3: // EX (SP),HL
        t = read16(z80.sp);
        write16(z80.sp, HLX());
        SET_HLX(t);
        break;
    case 0xE9: z80.pc = HLX(); break;                   // JP (HL)
    case 0xEB: // EX DE,HL (never IX/IY)
        v = z80.d; z80.d = z80.h; z80.h = v;
        v = z80.e; z80.e = z80.l; z80.l = v;
        break;
    case 0xF3: z80.iff1 = z80.iff2 = 0; break;          // DI
    case 0xFB: z80.iff1 = z80.iff2 = 1; z80.ei_pending = 1; break; // EI
    case 0xF9: z80.sp = HLX(); break;                   // LD SP,HL

    default:
        if (op >= 0x40 && op < 0x80) {
            // LD r,r'
            uint8_t dst = (op >> 3) & 7, src = op & 7;
            if (src == 6) {
                v = RD(addr_hlx());
                *reg_ptr(dst, idx) = v;
                if (idx) extra = 8;
            } else if (dst == 6) {
                a = addr_hlx();
                WR(a, *reg_ptr(src, idx));
                if (idx) extra = 8;
            } else {
                *reg_ptr(dst, 0) = *reg_ptr(src, 0);
            }
        } else if (op >= 0x80 && op < 0xC0) {
            // ALU A,r
            if ((op & 7) == 6) {
                v = RD(addr_hlx());
                if (idx) extra = 8;
            } else {
                v = *reg_ptr(op & 7, 0);
            }
            alu_op(op >> 3, v);
        }
        break;
    }
    return cycles_main[op] + extra;
}

/* === Interrupts and the main loop === */

static void take_irq(void) {
    z80.halted = 0;
    z80.iff1 = z80.iff2 = 0;
    inc_r();
    push16(z80.pc);
    if (z80.im == 2) {
        z80.pc = read16((uint16_t) ((z80.i << 8) | 0xFF));
        z80.cycles -= 19;
    } else {
        // IM 1 (and IM 0 with $FF on the bus): RST 38h
        z80.pc = 0x38;
        z80.cycles -= 13;
    }
}

void z80_nmi(void) {
    z80.halted = 0;
    z80.iff2 = z80.iff1;
    z80.iff1 = 0;
    push16(z80.pc);
    z80.pc = 0x66;
    z80.cycles -= 11;
}

// one instruction, or taking an interrupt; subtracts its cycles
static inline void step(void) {
    if (z80.irq && z80.iff1 && !z80.ei_pending) {
        take_irq();
        return;
    }
    z80.ei_pending = 0;
    if (z80.halted) {
        // nothing to do until an interrupt
        if (z80.cycles > 0)
            z80.cycles = 0;
        return;
    }
#ifdef Z80_COUNT
    z80_count++;
#endif
    uint8_t op = fetch();
    inc_r();
    int c;
    if (op == 0xDD || op == 0xFD) {
        // index prefixes (repeated ones: the last one counts)
        c = 0;
        do {
            idx = (op == 0xDD) ? 1 : 2;
            c += 4;
            op = fetch();
            inc_r();
        } while (op == 0xDD || op == 0xFD);
        if (idx == 1) { rh = &z80.ixh; rl = &z80.ixl; }
        else { rh = &z80.iyh; rl = &z80.iyl; }
        if (op == 0xCB)
            c += exec_cb() - 4;
        else if (op == 0xED) {
            idx = 0; rh = &z80.h; rl = &z80.l;
            c += exec_ed();
        } else
            c += exec_op(op);
        idx = 0;
        rh = &z80.h;
        rl = &z80.l;
    } else {
        idx = 0;
        rh = &z80.h;
        rl = &z80.l;
        if (op == 0xCB)
            c = exec_cb();
        else if (op == 0xED)
            c = exec_ed();
        else
            c = exec_op(op);
    }
    z80.cycles -= c;
}

void z80_step(void) {
    step();
}

volatile uint8_t z80_break;

int32_t z80_run(int32_t cycles) {
    z80.cycles += cycles;
    while (z80.cycles > 0 && !z80_break)
        step();
    return z80.cycles;
}
