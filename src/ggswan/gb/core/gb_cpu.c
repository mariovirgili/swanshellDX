/**
 * gbswan - the Game Boy's CPU (Sharp SM83), the reference interpreter:
 * every access to the bus is a machine cycle, and the rest of the
 * hardware is moved by that cycle before the access is made (gb.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gb.h"

static uint8_t rd(uint16_t a) {
    gb_tick();
    return gb_read(a);
}
static void wr(uint16_t a, uint8_t v) {
    gb_tick();
    gb_write(a, v);
}
#define idle() gb_tick()

static uint8_t imm8(void) {
    return rd(gb.pc++);
}
static uint16_t imm16(void) {
    uint8_t lo = imm8();
    return lo | (uint16_t) imm8() << 8;
}
static void push16(uint16_t v) {
    wr(--gb.sp, v >> 8);
    wr(--gb.sp, v & 0xFF);
}
static uint16_t pop16(void) {
    uint8_t lo = rd(gb.sp++);
    return lo | (uint16_t) rd(gb.sp++) << 8;
}

#define BC() ((uint16_t) gb.b << 8 | gb.c)
#define DE() ((uint16_t) gb.d << 8 | gb.e)
#define HL() ((uint16_t) gb.h << 8 | gb.l)
static void set_hl(uint16_t v) {
    gb.h = v >> 8;
    gb.l = v & 0xFF;
}

// the 8 bit operand by its number in the opcode: B C D E H L (HL) A
static uint8_t get_r(uint8_t r) {
    switch (r) {
    case 0: return gb.b;
    case 1: return gb.c;
    case 2: return gb.d;
    case 3: return gb.e;
    case 4: return gb.h;
    case 5: return gb.l;
    case 6: return rd(HL());
    default: return gb.a;
    }
}
static void set_r(uint8_t r, uint8_t v) {
    switch (r) {
    case 0: gb.b = v; break;
    case 1: gb.c = v; break;
    case 2: gb.d = v; break;
    case 3: gb.e = v; break;
    case 4: gb.h = v; break;
    case 5: gb.l = v; break;
    case 6: wr(HL(), v); break;
    default: gb.a = v; break;
    }
}
// the 16 bit one: BC DE HL SP
static uint16_t get_rp(uint8_t p) {
    switch (p) {
    case 0: return BC();
    case 1: return DE();
    case 2: return HL();
    default: return gb.sp;
    }
}
static void set_rp(uint8_t p, uint16_t v) {
    switch (p) {
    case 0: gb.b = v >> 8; gb.c = v & 0xFF; break;
    case 1: gb.d = v >> 8; gb.e = v & 0xFF; break;
    case 2: set_hl(v); break;
    default: gb.sp = v; break;
    }
}

static bool cond(uint8_t c) {
    switch (c) {
    case 0: return !(gb.f & GB_FLAG_Z);
    case 1: return gb.f & GB_FLAG_Z;
    case 2: return !(gb.f & GB_FLAG_C);
    default: return gb.f & GB_FLAG_C;
    }
}

static void alu(uint8_t op, uint8_t v) {
    uint8_t a = gb.a, c = (gb.f & GB_FLAG_C) ? 1 : 0;
    uint16_t r;
    switch (op) {
    case 0:     // ADD
    case 1:     // ADC
        if (op == 0) c = 0;
        r = (uint16_t) a + v + c;
        gb.f = ((r & 0xFF) ? 0 : GB_FLAG_Z) | (((a & 15) + (v & 15) + c) > 15 ? GB_FLAG_H : 0)
            | (r > 0xFF ? GB_FLAG_C : 0);
        gb.a = r & 0xFF;
        break;
    case 2:     // SUB
    case 3:     // SBC
    case 7:     // CP
        if (op != 3) c = 0;
        r = (uint16_t) a - v - c;
        gb.f = GB_FLAG_N | ((r & 0xFF) ? 0 : GB_FLAG_Z) | ((a & 15) < (v & 15) + c ? GB_FLAG_H : 0)
            | (r > 0xFF ? GB_FLAG_C : 0);
        if (op != 7) gb.a = r & 0xFF;
        break;
    case 4:
        gb.a &= v;
        gb.f = (gb.a ? 0 : GB_FLAG_Z) | GB_FLAG_H;
        break;
    case 5:
        gb.a ^= v;
        gb.f = gb.a ? 0 : GB_FLAG_Z;
        break;
    default:
        gb.a |= v;
        gb.f = gb.a ? 0 : GB_FLAG_Z;
        break;
    }
}

static uint8_t inc8(uint8_t v) {
    v++;
    gb.f = (gb.f & GB_FLAG_C) | (v ? 0 : GB_FLAG_Z) | ((v & 15) ? 0 : GB_FLAG_H);
    return v;
}
static uint8_t dec8(uint8_t v) {
    v--;
    gb.f = (gb.f & GB_FLAG_C) | GB_FLAG_N | (v ? 0 : GB_FLAG_Z) | ((v & 15) == 15 ? GB_FLAG_H : 0);
    return v;
}

// SP + a signed byte, with the flags of the low byte's addition
static uint16_t sp_plus(uint8_t e) {
    uint16_t sp = gb.sp;
    gb.f = (((sp & 15) + (e & 15)) > 15 ? GB_FLAG_H : 0) | (((sp & 0xFF) + e) > 0xFF ? GB_FLAG_C : 0);
    return sp + (uint16_t) (int16_t) (int8_t) e;
}

static void op_cb(void) {
    uint8_t op = imm8();
    uint8_t r = op & 7, y = (op >> 3) & 7;
    uint8_t v = get_r(r), c;
    switch (op >> 6) {
    case 0:
        switch (y) {
        case 0: c = v >> 7; v = v << 1 | c; break;                              // RLC
        case 1: c = v & 1; v = v >> 1 | c << 7; break;                          // RRC
        case 2: c = v >> 7; v = v << 1 | ((gb.f & GB_FLAG_C) ? 1 : 0); break;   // RL
        case 3: c = v & 1; v = v >> 1 | ((gb.f & GB_FLAG_C) ? 0x80 : 0); break; // RR
        case 4: c = v >> 7; v <<= 1; break;                                     // SLA
        case 5: c = v & 1; v = (v & 0x80) | v >> 1; break;                      // SRA
        case 6: c = 0; v = v << 4 | v >> 4; break;                              // SWAP
        default: c = v & 1; v >>= 1; break;                                     // SRL
        }
        gb.f = (v ? 0 : GB_FLAG_Z) | (c ? GB_FLAG_C : 0);
        set_r(r, v);
        break;
    case 1:     // BIT
        gb.f = (gb.f & GB_FLAG_C) | GB_FLAG_H | ((v >> y & 1) ? 0 : GB_FLAG_Z);
        break;
    case 2:     // RES
        set_r(r, v & ~(1 << y));
        break;
    default:    // SET
        set_r(r, v | 1 << y);
        break;
    }
}

static void interrupt(void) {
    gb.ime = 0;
    idle();
    idle();
    wr(--gb.sp, gb.pc >> 8);
    // (which one, and whether any, is decided here: the push of the high
    // byte may have written IE)
    uint8_t pend = gb.ie & gb.io[GB_IF] & 0x1F;
    wr(--gb.sp, gb.pc & 0xFF);
    if (!pend) {
        gb.pc = 0;
    } else {
        uint8_t n = 0;
        while (!(pend >> n & 1)) n++;
        gb.io[GB_IF] &= ~(1 << n);
        gb.pc = 0x40 + n * 8;
    }
    idle();
}

void gb_step(void) {
    if (gb.locked) {
        gb_tick();
        return;
    }
    if (gb.stopped) {
        gb_tick();
        if (gb.keys) gb.stopped = 0;
        return;
    }
    if (gb.halted) {
        gb_tick();
        if (!(gb.ie & gb.io[GB_IF] & 0x1F)) return;
        gb.halted = 0;
    }
    if (gb.ime && (gb.ie & gb.io[GB_IF] & 0x1F)) {
        interrupt();
        return;
    }
    if (gb.ei_delay) {
        gb.ei_delay = 0;
        gb.ime = 1;
    }

    uint8_t op = rd(gb.pc);
    if (gb.halt_bug) gb.halt_bug = 0;
    else gb.pc++;

    uint8_t y = (op >> 3) & 7, z = op & 7, p = (op >> 4) & 3;
    uint8_t v, c;
    uint16_t w, hl;
    switch (op >> 6) {
    case 1:
        if (op == 0x76) {       // HALT
            if (!gb.ime && (gb.ie & gb.io[GB_IF] & 0x1F)) gb.halt_bug = 1;
            else gb.halted = 1;
        } else {
            set_r(y, get_r(z));
        }
        return;
    case 2:
        alu(y, get_r(z));
        return;
    case 0:
        switch (z) {
        case 0:
            switch (y) {
            case 0: return;                                 // NOP
            case 1:                                         // LD (nn), SP
                w = imm16();
                wr(w, gb.sp & 0xFF);
                wr(w + 1, gb.sp >> 8);
                return;
            case 2:                                         // STOP
                gb.pc++;
                gb.stopped = 1;
                gb.div = 0;
                return;
            case 3:                                         // JR e
                v = imm8();
                idle();
                gb.pc += (uint16_t) (int16_t) (int8_t) v;
                return;
            default:                                        // JR cc, e
                v = imm8();
                if (cond(y - 4)) {
                    idle();
                    gb.pc += (uint16_t) (int16_t) (int8_t) v;
                }
                return;
            }
        case 1:
            if (!(op & 8)) {                                // LD rr, nn
                set_rp(p, imm16());
            } else {                                        // ADD HL, rr
                hl = HL();
                w = get_rp(p);
                gb.f = (gb.f & GB_FLAG_Z) | (((hl & 0xFFF) + (w & 0xFFF)) > 0xFFF ? GB_FLAG_H : 0)
                    | ((uint32_t) hl + w > 0xFFFF ? GB_FLAG_C : 0);
                set_hl(hl + w);
                idle();
            }
            return;
        case 2:
            switch (y) {
            case 0: wr(BC(), gb.a); return;
            case 1: gb.a = rd(BC()); return;
            case 2: wr(DE(), gb.a); return;
            case 3: gb.a = rd(DE()); return;
            case 4: hl = HL(); wr(hl, gb.a); set_hl(hl + 1); return;
            case 5: hl = HL(); gb.a = rd(hl); set_hl(hl + 1); return;
            case 6: hl = HL(); wr(hl, gb.a); set_hl(hl - 1); return;
            default: hl = HL(); gb.a = rd(hl); set_hl(hl - 1); return;
            }
        case 3:                                             // INC rr, DEC rr
            set_rp(p, get_rp(p) + ((op & 8) ? 0xFFFF : 1));
            idle();
            return;
        case 4: set_r(y, inc8(get_r(y))); return;
        case 5: set_r(y, dec8(get_r(y))); return;
        case 6: set_r(y, imm8()); return;
        default:
            switch (y) {
            case 0:                                         // RLCA
                c = gb.a >> 7;
                gb.a = gb.a << 1 | c;
                gb.f = c ? GB_FLAG_C : 0;
                return;
            case 1:                                         // RRCA
                c = gb.a & 1;
                gb.a = gb.a >> 1 | c << 7;
                gb.f = c ? GB_FLAG_C : 0;
                return;
            case 2:                                         // RLA
                c = gb.a >> 7;
                gb.a = gb.a << 1 | ((gb.f & GB_FLAG_C) ? 1 : 0);
                gb.f = c ? GB_FLAG_C : 0;
                return;
            case 3:                                         // RRA
                c = gb.a & 1;
                gb.a = gb.a >> 1 | ((gb.f & GB_FLAG_C) ? 0x80 : 0);
                gb.f = c ? GB_FLAG_C : 0;
                return;
            case 4: {                                       // DAA
                uint8_t a = gb.a, adj = 0, cy = gb.f & GB_FLAG_C;
                if (gb.f & GB_FLAG_N) {
                    if (gb.f & GB_FLAG_H) adj |= 0x06;
                    if (cy) adj |= 0x60;
                    a -= adj;
                } else {
                    if ((gb.f & GB_FLAG_H) || (a & 15) > 9) adj |= 0x06;
                    if (cy || a > 0x99) { adj |= 0x60; cy = GB_FLAG_C; }
                    a += adj;
                }
                gb.a = a;
                gb.f = (gb.f & GB_FLAG_N) | (a ? 0 : GB_FLAG_Z) | cy;
                return;
            }
            case 5:                                         // CPL
                gb.a = ~gb.a;
                gb.f |= GB_FLAG_N | GB_FLAG_H;
                return;
            case 6:                                         // SCF
                gb.f = (gb.f & GB_FLAG_Z) | GB_FLAG_C;
                return;
            default:                                        // CCF
                gb.f = (gb.f & (GB_FLAG_Z | GB_FLAG_C)) ^ GB_FLAG_C;
                return;
            }
        }
    default:
        break;
    }

    // C0h-FFh
    switch (z) {
    case 0:
        if (y < 4) {                                        // RET cc
            idle();
            if (cond(y)) {
                gb.pc = pop16();
                idle();
            }
        } else if (y == 4) {                                // LDH (n), A
            v = imm8();
            wr(0xFF00 | v, gb.a);
        } else if (y == 6) {                                // LDH A, (n)
            v = imm8();
            gb.a = rd(0xFF00 | v);
        } else if (y == 5) {                                // ADD SP, e
            v = imm8();
            w = sp_plus(v);
            idle();
            idle();
            gb.sp = w;
        } else {                                            // LD HL, SP + e
            v = imm8();
            set_hl(sp_plus(v));
            idle();
        }
        return;
    case 1:
        if (!(op & 8)) {                                    // POP
            w = pop16();
            switch (p) {
            case 0: gb.b = w >> 8; gb.c = w & 0xFF; break;
            case 1: gb.d = w >> 8; gb.e = w & 0xFF; break;
            case 2: set_hl(w); break;
            default: gb.a = w >> 8; gb.f = w & 0xF0; break;
            }
        } else if (p == 0) {                                // RET
            gb.pc = pop16();
            idle();
        } else if (p == 1) {                                // RETI
            gb.pc = pop16();
            idle();
            gb.ime = 1;
        } else if (p == 2) {                                // JP HL
            gb.pc = HL();
        } else {                                            // LD SP, HL
            gb.sp = HL();
            idle();
        }
        return;
    case 2:
        if (y < 4) {                                        // JP cc, nn
            w = imm16();
            if (cond(y)) {
                idle();
                gb.pc = w;
            }
        } else if (y == 4) {                                // LD (FF00 + C), A
            wr(0xFF00 | gb.c, gb.a);
        } else if (y == 6) {
            gb.a = rd(0xFF00 | gb.c);
        } else if (y == 5) {                                // LD (nn), A
            w = imm16();
            wr(w, gb.a);
        } else {
            w = imm16();
            gb.a = rd(w);
        }
        return;
    case 3:
        if (y == 0) {                                       // JP nn
            w = imm16();
            idle();
            gb.pc = w;
        } else if (y == 1) {
            op_cb();
        } else if (y == 6) {                                // DI
            gb.ime = 0;
            gb.ei_delay = 0;
        } else if (y == 7) {                                // EI
            if (!gb.ime) gb.ei_delay = 1;
        } else {
            gb.locked = 1;
        }
        return;
    case 4:
        if (y < 4) {                                        // CALL cc, nn
            w = imm16();
            if (cond(y)) {
                idle();
                push16(gb.pc);
                gb.pc = w;
            }
        } else {
            gb.locked = 1;
        }
        return;
    case 5:
        if (!(op & 8)) {                                    // PUSH
            idle();
            switch (p) {
            case 0: push16(BC()); break;
            case 1: push16(DE()); break;
            case 2: push16(HL()); break;
            default: push16((uint16_t) gb.a << 8 | gb.f); break;
            }
        } else if (p == 0) {                                // CALL nn
            w = imm16();
            idle();
            push16(gb.pc);
            gb.pc = w;
        } else {
            gb.locked = 1;
        }
        return;
    case 6:
        alu(y, imm8());
        return;
    default:                                                // RST
        idle();
        push16(gb.pc);
        gb.pc = y * 8;
        return;
    }
}

void gb_run_frame(void) {
    uint32_t end = gb.clock + GB_FRAME_TICKS;
    gb.frame_done = 0;
    while (!gb.frame_done && (int32_t) (gb.clock - end) < 0)
        gb_step();
}
