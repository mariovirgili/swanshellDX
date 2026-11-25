/**
 * ggswan - Game Gear emulator for the WonderSwan (nileswan)
 * Z80 -> V30MZ dynamic recompiler: the translator. See drc.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include "drc.h"
#include "../core/z80.h"
#include "../core/gg.h"

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
    uint8_t trace;     // 1: JP nn / JR e followed inside the block (no
                       // code); 2: CALL nn whose routine follows inline;
                       // 3: its RET (nn: the expected return address)
    uint8_t dyn;       // LD A,(nn) whose nn an earlier instruction of the
                       // block rewrites (code in RAM): read nn at run time
} ins_t;

#ifdef __ia16__
// (WonderSwan: in the free internal RAM above E000, not in the crowded
// area below 4000 where the C data, the stack and the screen maps are)
#define ins ((ins_t*) 0xE000)
#define live_out ((uint8_t*) (0xE000 + MAX_INSTR * sizeof(ins_t)))
#else
static ins_t ins[MAX_INSTR];
static uint8_t live_out[MAX_INSTR];
#endif

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
#define VA(field) ((uint16_t) (env->vdp_addr + offsetof(gg_vdp_t, field)))
/*
 * sfswan (env->flat): the SF-7000's memory is 64 KiB of RAM and nothing
 * else, one segment of the WonderSwan's (the RAM window on a PSRAM bank),
 * Z80 address X at offset X. ES is that segment wherever translated code
 * runs: the runtime enters the blocks with it and every routine they call
 * leaves it so. So no segment table and no "is it work RAM": a read is
 * ES:[X] and so is a write, after a look at the code map: a byte for each
 * 16 of the memory (env->cmap, in the internal RAM), not 0 where a block
 * was translated from. Those writes go to the helper, which knows byte by
 * byte and throws the blocks away if code was really written. The stack is
 * not looked at: it is where the program puts it (a program that pushes
 * its code into place is not served). ram_delta is 0 here, and FLAT_ES
 * goes before what the other builds reach through DS.
 */
#ifdef GGSWAN_SF7000
#define FLAT (env->flat)
#define FENV(f) (env->f)
#else
// (the other plugins: none of it is compiled, their code is as it was)
#define FLAT 0
#define FENV(f) 0
#endif
#define FLAT_ES() do { if (FLAT) B(0x26); } while (0)
/*
 * Code that writes over code. Disk BASIC wipes its own start-up, and
 * machine code routines keep their variables in their instructions (LD
 * (lab + 1),A ... lab: LD A,0). When the helper finds that a byte of a
 * translated instruction was written, the runtime throws the blocks away
 * and remembers the byte (drc_rt.c). Two things make that exact:
 *   - the block stops at the write, not at its end: the instruction
 *     written may be its next one. So before the helper's call the Z80's
 *     pc is set to the address after the instruction being translated
 *     (flat_pc), and the helper's routine leaves the block there when the
 *     runtime says so;
 *   - an instruction with a remembered byte in it is not translated any
 *     more: the interpreter runs it where it is (a step, env->smc says
 *     which), so that it is not "translated code" either and writing it
 *     again costs a helper's call, no more. After it the block goes on
 *     only if the Z80 is where the next instruction is (it may have
 *     become a jump).
 */
static uint16_t flat_next;   // the address after the instruction being translated
static void flat_pc(void) {
    B(0xC7); B(0x06); W(ZA(pc)); W(flat_next);          // mov word [pc], next
}

// short forward jumps: returns the position of the rel8 byte
static uint8_t *jcc8(uint8_t opcode) { B(opcode); B(0); return o - 1; }
static void land8(uint8_t *p) {
    if (!p) return;   // (a jump that was not needed)
#ifdef GGSWAN_HOST
    if (o - p - 1 > 127) { extern void abort(void); abort(); }   // (short jump too far)
#endif
    *p = (uint8_t) (o - p - 1);
}

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

// (the cold tail: see cold_emit_all)
#define MAX_COLD 16
enum { COLD_WRITE = 1, COLD_IX_LD, COLD_IX_ALU, COLD_IX_BIT, COLD_IX_WR, COLD_PV, COLD_SEG_HL, COLD_CRAM, COLD_PSG, COLD_STEP, COLD_HELPER, COLD_EI_EXIT };
typedef struct {
    uint8_t *jpos;       // rel8 byte of the JAE jump to this cold stub
    uint8_t *jpos2;      // (COLD_STEP: the second jump, or 0)
    uint16_t resume_off; // code-segment offset to jmp_near back to
    uint8_t kind;        // COLD_*
    uint8_t a, b, imm;   // kind-specific fields
    int8_t d;            // IX/IY displacement (or hl_es keep for COLD_WRITE)
} cold_t;
static cold_t cold[MAX_COLD];
static uint8_t cold_n, cold_off, cold_fail;
static uint8_t cold_emitting;   // (cold_emit_all: no new cold stubs)
// a free cold slot (else: the inline form)
#define COLD_FREE() (!cold_off && !cold_emitting && cold_n < MAX_COLD)
static void cold_add(uint8_t *jpos, uint16_t resume_off, uint8_t kind,
                     uint8_t a, uint8_t b, int8_t d, uint8_t imm);

/* === Flag state === */

enum { K_AH = 0, K_ADD, K_SUB, K_AND, K_OR, K_INC, K_DEC, K_CARRY };
// K_CARRY: x86 CF holds Z80 C, and C is the only flag still read (the
// others are dead: the liveness analysis says nobody reads them)
static uint8_t fkind;      // where the flags are (K_AH: in AH)
static uint8_t cf_valid;   // K_INC / K_DEC: x86 CF holds Z80 C
static uint8_t hl_es;      // 1: ES is set up for the slot of HL; 2: for that
                           // of HL - 1 (INC HL just before: emit, seg_hl)

// The flags in AH; need: the ones read later (the others may be left
// wrong: nobody reads them before they are written again)
static void materialize_need(uint8_t need) {
    switch (fkind) {
    case K_ADD: case K_INC: case K_SUB: case K_DEC: {
        uint8_t mask = (fkind == K_SUB || fkind == K_DEC) ? 0xD3 : 0xD1;
        B(0x9F);                         // lahf
        if ((need & FP) && COLD_FREE()) {
            // P/V = overflow: no overflow (the common case) falls through,
            // the overflow sets P/V in the cold tail
            uint8_t *j = jcc8(0x70);     // jo cold
            B(0x80); B(0xE4); B(mask);   // and ah, mask (P/V = 0)
            cold_add(j, here(), COLD_PV, mask, 0, 0, 0);
        } else if (need & FP) {
            // P/V = overflow
            B(0x70); B(0x05);            // jo +5
            B(0x80); B(0xE4); B(mask);   // and ah, mask (P/V = 0)
            B(0xEB); B(0x06);            // jmp +6
            B(0x80); B(0xE4); B(mask);   // and ah, mask
            B(0x80); B(0xCC); B(0x04);   // or ah, 4 (P/V = 1)
        } else if (need & ~FC) {
            B(0x80); B(0xE4); B(mask);   // and ah, mask (P/V not read)
        }
        // (only C: as LAHF leaves it)
        break;
    }
    case K_AND:
        B(0x9F); B(0x80); B(0xE4); B(0xC5); B(0x80); B(0xCC); B(0x10);
        break;
    case K_OR:
        B(0x9F); B(0x80); B(0xE4); B(0xC5);
        break;
    case K_CARRY:
        B(0x9F);                         // lahf (C; the rest is dead)
        break;
    }
    fkind = K_AH;
}

static void materialize(void) { materialize_need(FALL); }

// x86 flags are about to be destroyed: save the Z80 flags if needed
static void clobber(uint8_t live) {
    if (fkind != K_AH) {
        if (live)
            materialize_need(live);
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

// (the stack where the translated code can reach it: see sp_loaded_exit)
#define SP_IN_RAM(v) (FLAT || (uint16_t) ((v) - 0xC002u) <= 0x1FFEu)
static uint8_t force_nolink;   // the next exit must return to the runtime
// (a helper that changes the mapping makes the runtime end the block
// without following links: see drc_rt.c)

static int linkable(uint16_t target) {
    if (FLAT) return !force_nolink;   // (one memory: an address is one block)
    // (blocks in work RAM check their bytes at entry: they can be linked)
    if (!force_nolink && env->ram_blocks && target >= 0xC000 && target < 0xDFC0) return 1;
    if (force_nolink || target >= 0xC000) return 0;
    return (target >> 14) == 0 || (target >> 14) == (block_pc >> 14);
}

// to another ROM slot: linked behind a check of the slot's bank
static int guarded(uint16_t target) {
    if (FLAT) return 0;
    return !force_nolink && target < 0xC000 && (target >> 14) != 0
        && (target >> 14) != (block_pc >> 14) && env->ldi_off;
}

static void set_link(uint16_t v) { B(0xC7); B(0x06); W(SA(link)); W(v); }

static void sub_cycles(uint16_t n) {
    if (n < 128) { B(0x83); B(0x2E); W(SA(cycles)); B(n); }
    else { B(0x81); B(0x2E); W(SA(cycles)); W(n); }
}

static uint8_t idle_block;
static uint16_t idle_target;   // (is_idle 2: the EI the loop goes through; 3: the JR back)
static void idle_exit(uint16_t target, uint16_t extra);
static void exit_const(uint16_t target, uint16_t extra) {
    if (idle_block == 3 && target == idle_target) {
        idle_exit(target, extra);
        return;
    }
    materialize();
    sub_cycles(cyc + extra);
    // (z80.pc and the link are only needed by the runtime: a linked exit
    // is just SUB + JG + JMP)
    if (linkable(target)) {
        // (V30MZ: a branch not taken costs 1 cycle, a taken one about 4)
        B(0x7E); B(5);                        // jle X
        uint16_t l = here();
        B(0xEB); B(3);                        // L: jmp X until the runtime
        B(0x90); B(0x90); B(0x90);            // links it (near or far jump)
        B(0xC7); B(0x06); W(ZA(pc)); W(target);   // X: mov word [pc], target
        set_link(l);
        jmp_near(env->exit_off);
    } else if (guarded(target)) {
        // to another slot: cycles left, then (slot 2: no cartridge RAM
        // there) the bank the link was made for; the CMPs come after the
        // JLE because they replace the flags of the SUB
        uint8_t slot = target >> 14;
        uint8_t *x1 = jcc8(0x7E), *x2 = 0;               // jle X
        if (slot == 2) {
            B(0x80); B(0x3E); W(SA(keys)); B(0);          // cmp byte [keys], 0
            x2 = jcc8(0x75);                              // jne X
        }
        B(0x80); B(0x3E); W(SA(keys) + 1 + slot); B(0xFF);   // cmp byte [key], bank (L-3)
        uint8_t *x3 = jcc8(0x75);                         // jne X
        uint16_t l = here();
        B(0xEB); B(3);                                    // L: jmp X until the runtime
        B(0x90); B(0x90); B(0x90);                        // links it and sets the bank
        land8(x1); if (x2) land8(x2); land8(x3);          // X:
        B(0xC7); B(0x06); W(ZA(pc)); W(target);
        set_link(l);
        jmp_near(env->exit_off);
    } else {
        B(0xC7); B(0x06); W(ZA(pc)); W(target);
        set_link(0);
        jmp_near(env->exit_off);
    }
}

static void exit_reg(uint8_t reg16, uint16_t extra) {
    materialize();
    if (env->dyn_off && !env->no_fast && !force_nolink) {
        /*
         * Inline cache: the first target this exit goes to (RET to the
         * same caller, JP (HL) to the same routine: about half to two
         * thirds of the DYN lookups in Shinobi and Sonic Spinball) gets a
         * compare and a link here, like an exit to a constant address;
         * any other goes through DYN as before. The first time the
         * compare is CMP reg, reg (equal) and the jump unlinked: X exits
         * to the runtime, which (drc_rt.c, ic_link) writes the target
         * into the compare and links the jump, or writes FFFFh (never
         * equal) for a target that can not be linked from here. The
         * layout is fixed: the runtime finds the compare at L - 14 and
         * (or L - 21 for slot 0, which leaves 7 bytes at L - 15 for an
         * optional slot 1 bank check) and recognizes these exits by the
         * MOV BYTE [ic] at X + 10 (after MOV [pc], reg).
         */
        uint8_t lform = !FLAT && (block_pc >> 14) == 0;
        B(0x39); B(0xC0 | (reg16 << 3) | reg16);             // cmp reg, reg (later: cmp reg, target)
        B(0x90); B(0x90);
        uint8_t *d = jcc8(0x75);                             // jne D
        if (lform) {
            // 7-byte slot for "cmp byte [keys + 2], bank; jne D" when
            // returning from slot 0 into slot 1; skipped otherwise
            B(0xEB); B(0x05);
            B(0x90); B(0x90); B(0x90); B(0x90); B(0x90);
        }
        B(0x81); B(0x2E); W(SA(cycles)); W(cyc + extra);     // sub word [cycles], n
        B(0x7E); B(5);                                       // jle X
        uint16_t l = here();
        B(0xEB); B(3); B(0x90); B(0x90); B(0x90);            // L: jmp X until linked
        B(0x89); B(0x06 | (reg16 << 3)); W(ZA(pc));          // X: mov [pc], reg
        B(0xC7); B(0x06); W(SA(link)); W(l);                 // mov [link], L
        B(0xC6); B(0x06); W(SA(ic)); B(1 + (block_pc >> 14));   // mov byte [ic], slot + 1
        jmp_near(env->exit_off);
        land8(d);
        B(0x89); B(0x06 | (reg16 << 3)); W(ZA(pc));          // D: mov [pc], reg
        sub_cycles(cyc + extra);                             // D:
        jmp_near(env->dyn_off);                              // look the next block up
        return;
    }
    sub_cycles(cyc + extra);
    B(0x89); B(0x06 | (reg16 << 3)); W(ZA(pc));   // mov [pc], reg
    if (env->dyn_off) {
        jmp_near(env->dyn_off);   // look the next block up (drc_rt.c)
        return;
    }
    set_link(0);
    jmp_near(env->exit_off);
}

/* === Memory === */

// ES for the slot of BX (HL)
static void seg_hl(uint8_t live) {
    if (FLAT) { hl_es = 1; return; }
    if (hl_es == 1) return;
    clobber(live);
    if (hl_es == 2) {
        // after INC HL (LD E,(HL) / INC HL / LD D,(HL), everywhere): the
        // same slot unless HL has just entered the next 8 KiB
        B(0xF7); B(0xC3); W(0x1FFF);     // test bx, 1FFFh
        if (COLD_FREE()) {
            // the next 8 KiB (rare): the segment in the cold tail
            uint8_t *j = jcc8(0x74);     // jz cold
            cold_add(j, here(), COLD_SEG_HL, 0, 0, 0, 0);
            hl_es = 1;
            return;
        }
        B(0x75); B(12);                  // jnz +12 (the same)
    }
    B(0x89); B(0xDE);                    // mov si, bx
    B(0xC1); B(0xC6); B(0x04);           // rol si, 4
    B(0x83); B(0xE6); B(0x0E);           // and si, 14
    B(0x8E); B(0x84); W(SA(segtab));     // mov es, [si + segtab]
    hl_es = 1;
}

// ES for the slot of SI
static void seg_si(uint8_t live) {
    if (FLAT) return;
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
 * Cold tail (out-of-line slow paths placed after the block's final exit):
 * on the real V30MZ a taken branch costs 4-5 cycles while a not-taken one
 * costs 1. Moving the rare slow paths of work-RAM writes and (IX+d)/(IY+d)
 * accesses past the end of the block makes the common work-RAM case fall
 * straight through with zero taken branches and keeps hot code compact.
 * If a long block's jump to the cold tail would exceed rel8 (+127 bytes),
 * drc_translate() retries generation once with cold_off = 1 (inline paths).
 */

static void cold_add(uint8_t *jpos, uint16_t resume_off, uint8_t kind,
                     uint8_t a, uint8_t b, int8_t d, uint8_t imm) {
    if (cold_n >= MAX_COLD) {
        cold_fail = 1;
        return;
    }
    cold_t *c = &cold[cold_n++];
    c->jpos = jpos;
    c->jpos2 = 0;
    c->resume_off = resume_off;
    c->kind = kind;
    c->a = a;
    c->b = b;
    c->d = d;
    c->imm = imm;
}

// The interpreter for one instruction (not work RAM: rare) in the cold
// tail, reached by the two jumps of mem_check; 0: no room, inline
static int step_cold(uint8_t *j1, uint8_t *j2, uint16_t pc) {
    if (!COLD_FREE() || !j1) return 0;
    cold_add(j1, here(), COLD_STEP, (uint8_t) pc, (uint8_t) (pc >> 8), 0, 0);
    cold[cold_n - 1].jpos2 = j2;
    hl_es = 0;   // (as after the helper)
    return 1;
}

static void ix_far(int8_t d);
static const uint8_t alu_x86[8];

static void cold_emit_all(void) {
    fkind = K_AH;
    cold_emitting = 1;
    for (uint8_t i = 0; i < cold_n; i++) {
        const cold_t *c = &cold[i];
        if (o + 48 >= o_limit || (uint16_t) (o - c->jpos - 1) > 127) {
            cold_fail = 1;
            return;
        }
        if (c->jpos2 && (uint16_t) (o - c->jpos2 - 1) > 127) {
            cold_fail = 1;
            return;
        }
        *c->jpos = (uint8_t) (o - c->jpos - 1);
        if (c->jpos2) *c->jpos2 = (uint8_t) (o - c->jpos2 - 1);
        switch (c->kind) {
        case COLD_WRITE:
            if (c->b == 0xFF) { B(0xC6); B(0x06); W(SA(val)); B(c->imm); }
            else { B(0x88); B(0x06 | (c->b << 3)); W(SA(val)); }
            B(0x89); B(0x06 | (c->a << 3)); W(SA(arg));
            helper(DRC_H_WRITE);
            if (c->d) seg_hl(0);
            break;
        case COLD_IX_WR:
            if (c->d) { B(0x8D); B(0x74); B((uint8_t) c->d); }
            if (c->a == 0xFF) { B(0xC6); B(0x06); W(SA(val)); B(c->imm); }
            else { B(0x88); B(0x06 | (c->a << 3)); W(SA(val)); }
            B(0x89); B(0x36); W(SA(arg));
            helper(DRC_H_WRITE);
            if (c->b) seg_hl(0);
            break;
        case COLD_IX_LD:
            ix_far(c->d);
            B(0x26); B(0x8A); B(0x04 | (c->a << 3));       // mov r, es:[si]
            B(0x07);                                       // pop es
            break;
        case COLD_IX_ALU:
            ix_far(c->d);
            if (c->a == 1 || c->a == 3) B(0x9E);           // sahf (CF = Z80 C)
            B(0x26); B(alu_x86[c->a] + 2); B(0x04);        // op al, es:[si]
            B(0x07);                                       // pop es
            break;
        case COLD_SEG_HL:
            B(0x89); B(0xDE);                              // mov si, bx
            B(0xC1); B(0xC6); B(0x04);                     // rol si, 4
            B(0x83); B(0xE6); B(0x0E);                     // and si, 14
            B(0x8E); B(0x84); W(SA(segtab));               // mov es, [si + segtab]
            break;
        case COLD_CRAM:
            B(0x88); B(0x06 | (c->a << 3)); W(SA(val));    // mov [val], src
            call_near(env->cram_off);
            break;
        case COLD_STEP:
            B(0xC7); B(0x06); W(SA(arg)); W(c->a | (c->b << 8));
            B(0xC6); B(0x06); W(SA(val)); B(1);
            helper(DRC_H_STEP);
            break;
        case COLD_HELPER:
            helper(c->a);
            break;
        case COLD_EI_EXIT:
            // EI with an interrupt waiting: out, the runtime takes it after
            // the next instruction (a: cycles, resume_off: next; no return)
            B(0xC6); B(0x06); W(ZA(ei_pending)); B(1);
            sub_cycles(c->a | (c->b << 8));
            B(0xC7); B(0x06); W(ZA(pc)); W(c->resume_off);
            set_link(0);
            jmp_near(env->exit_off);
            continue;
        case COLD_PSG:
            helper(DRC_H_PSG);
            break;
        case COLD_PV:
            B(0x80); B(0xE4); B(c->a);                     // and ah, mask
            B(0x80); B(0xCC); B(0x04);                     // or ah, 4 (P/V = 1)
            break;
        case COLD_IX_BIT:
            ix_far(c->d);
            B(0x26); B(0xF6); B(0x04); B(1 << c->a);       // test byte es:[si], mask
            B(0x07);                                       // pop es
            break;
        }
        jmp_near(c->resume_off);
    }
}

/*
 * (IX + d) and (IY + d) in work RAM, where the games keep what they reach
 * this way (Sonic's objects: a fifth of its blocks start with DD or FD):
 * SI = IX, one comparison that holds for any d (IX - 128 and IX + 127
 * both in C000-DFFF), then the byte in place at [SI + d + ram], 7 cycles
 * with the jump against 12 through the segment table, and ES left to HL.
 * Returns the jump to the direct access; the code after the call is the
 * other case (SI = IX: the caller adds d).
 */
static uint8_t *ix_ram(uint8_t pre, uint8_t live) {
    clobber(live);
    B(0x8B); B(0x36); W(pre == 0xDD ? ZA(ixl) : ZA(iyl));   // mov si, [ix]
    B(0x8D); B(0xBC); W((uint16_t) (0u - 0xC080u));        // lea di, [si - C080h]
    B(0x81); B(0xFF); W(0x1F00);                           // cmp di, 1F00h
    return jcc8(0x72);                                     // jb direct
}

static uint8_t *ix_ram_cold(uint8_t pre, uint8_t live) {
    clobber(live);
    B(0x8B); B(0x36); W(pre == 0xDD ? ZA(ixl) : ZA(iyl));   // mov si, [ix]
    B(0x8D); B(0xBC); W((uint16_t) (0u - 0xC080u));        // lea di, [si - C080h]
    B(0x81); B(0xFF); W(0x1F00);                           // cmp di, 1F00h
    return jcc8(0x73);                                     // jae cold
}

// the other case of a read: ES for SI + d, kept for HL (PUSH and POP)
static void ix_far(int8_t d) {
    B(0x06);                             // push es
    if (d) { B(0x8D); B(0x74); B((uint8_t) d); }   // lea si, [si + d]
    B(0x89); B(0xF7);                    // mov di, si
    B(0xC1); B(0xC7); B(0x04);           // rol di, 4
    B(0x83); B(0xE7); B(0x0E);           // and di, 14
    B(0x8E); B(0x85); W(SA(segtab));     // mov es, [di + segtab]
}

// sfswan: a byte to the address in BX or SI (src: an 8-bit register, or
// 0xFF with imm): there, unless its 16 bytes have code in them (the
// helper then). x86 flags destroyed; uses DI.
static void flat_write(uint8_t addr_reg, uint8_t src, uint8_t imm) {
    B(0x89); B(addr_reg == X_BX ? 0xDF : 0xF7);         // mov di, bx / si
    B(0xC1); B(0xEF); B(0x04);                          // shr di, 4
    B(0x80); B(0xBD); W(FENV(cmap)); B(0);               // cmp byte [di + cmap], 0
    uint8_t *fast = jcc8(0x74);                         // je fast
    if (src == 0xFF) { B(0xC6); B(0x06); W(SA(val)); B(imm); }
    else { B(0x88); B(0x06 | (src << 3)); W(SA(val)); }
    B(0x89); B(0x06 | (addr_reg << 3)); W(SA(arg));     // mov [arg], reg
    flat_pc();
    helper(DRC_H_WRITE);
    uint8_t *done = jcc8(0xEB);
    land8(fast);
    uint8_t rm = addr_reg == X_BX ? 7 : 4;              // [bx] / [si]
    B(0x26);
    if (src == 0xFF) { B(0xC6); B(rm); B(imm); }
    else { B(0x88); B((src << 3) | rm); }
    land8(done);
}

// Before the byte at the address in BX, SI or DX is changed where it is:
// the jumps taken when that can not be done (work RAM only; sfswan:
// anywhere but in code). *j2 may be none. x86 flags destroyed; sfswan: DI.
static uint8_t *mem_check(uint8_t reg, uint8_t **j2) {
    uint8_t *j1;
    if (FLAT) {
        B(0x89); B(0xC7 | (reg << 3));                  // mov di, reg
        B(0xC1); B(0xEF); B(0x04);                      // shr di, 4
        B(0x80); B(0xBD); W(FENV(cmap)); B(0);           // cmp byte [di + cmap], 0
        *j2 = 0;
        return jcc8(0x75);                              // jne slow
    }
    if (reg == X_SI) {
        B(0x81); B(0xFE); W(0xC000); j1 = jcc8(0x72);   // cmp si, C000h; jb slow
        B(0x81); B(0xFE); W(0xE000); *j2 = jcc8(0x73);  // cmp si, E000h; jae slow
    } else {
        uint8_t m = reg == X_BX ? 0xFF : 0xFE;          // bh / dh
        B(0x80); B(m); B(0xC0); j1 = jcc8(0x72);
        B(0x80); B(m); B(0xE0); *j2 = jcc8(0x73);
    }
    return j1;
}

// sfswan: a register's 16 bits to a constant address (LD (nn),HL): there,
// unless one of its two bytes is in 16 that have code (the helper then,
// byte by byte). x86 flags destroyed.
static void flat_write16(uint16_t addr, uint8_t xr, uint8_t live) {
    clobber(live);
    uint8_t *slow = 0;
    if (((uint16_t) (addr + 1) >> 4) != (addr >> 4)) {
        B(0x80); B(0x3E); W(FENV(cmap) + ((uint16_t) (addr + 1) >> 4)); B(0);
        slow = jcc8(0x75);                              // jne slow
    }
    B(0x80); B(0x3E); W(FENV(cmap) + (addr >> 4)); B(0); // cmp byte [cmap + its 16 bytes], 0
    uint8_t *fast = jcc8(0x74);                         // je fast
    land8(slow);
    // (val is a byte, the helper's operation before it and its address
    // after it: the word at val for the low byte, at val - 1 for the high
    // one, the rest set afterwards)
    // (the first byte with the helper that never stops the block: the
    // second one is to be written too; it stops it for both)
    B(0x89); B(0x06 | (xr << 3)); W(SA(val));
    B(0xC7); B(0x06); W(SA(arg)); W(addr);
    helper(DRC_H_WRITE_MAP);
    B(0x89); B(0x06 | (xr << 3)); W(SA(val) - 1);
    B(0xC7); B(0x06); W(SA(arg)); W((uint16_t) (addr + 1));
    flat_pc();
    helper(DRC_H_WRITE);
    uint8_t *done = jcc8(0xEB);
    land8(fast);
    B(0x26); B(0x89); B(0x06 | (xr << 3)); W(addr);     // mov es:[addr], reg
    land8(done);
}

// LD (IX + d),r / n (src: x86 8-bit register, or 0xFF with imm)
static void ix_write(uint8_t pre, int8_t d, uint8_t src, uint8_t imm, uint8_t live) {
    if (FLAT) {
        clobber(live);
        ix_addr(pre, d);
        flat_write(X_SI, src, imm);
        return;
    }
    uint8_t keep = hl_es == 1;
    uint16_t disp = (uint16_t) (d + (int16_t) env->ram_delta);
    if (!cold_off) {
        uint8_t *j = ix_ram_cold(pre, live);
        if (src == 0xFF) { B(0xC6); B(0x84); W(disp); B(imm); }
        else { B(0x88); B(0x84 | (src << 3)); W(disp); }
        cold_add(j, here(), COLD_IX_WR, src, keep, d, imm);
        hl_es = keep;
        return;
    }
    uint8_t *f = ix_ram(pre, live);
    if (d) { B(0x8D); B(0x74); B((uint8_t) d); }           // lea si, [si + d]
    if (src == 0xFF) { B(0xC6); B(0x06); W(SA(val)); B(imm); }
    else { B(0x88); B(0x06 | (src << 3)); W(SA(val)); }
    B(0x89); B(0x36); W(SA(arg));                           // mov [arg], si
    helper(DRC_H_WRITE);
    if (keep) seg_hl(0);   // (ES for HL again: both ways leave it so)
    uint8_t *done = jcc8(0xEB);
    land8(f);
    if (src == 0xFF) { B(0xC6); B(0x84); W(disp); B(imm); }
    else { B(0x88); B(0x84 | (src << 3)); W(disp); }
    land8(done);
    hl_es = keep;
}

/*
 * Write a byte to the address in BX (HL) or SI. src: x86 8-bit register,
 * or 0xFF with imm. Work RAM directly, anything else through the helper.
 */
static void write_byte(uint8_t addr_reg, uint8_t src, uint8_t imm, uint8_t live) {
    clobber(live);
    if (FLAT) {
        flat_write(addr_reg, src, imm);
        return;
    }
    if (!env->no_fast) {
        /*
         * Work RAM first, reached by the jump taken (3 cycles; falling
         * into it and jumping over the helper's call afterwards was 5),
         * one comparison for SI too (C000-DFFF + 4000h wraps to
         * 0000-1FFF). And ES stays HL's: the other way sets it again
         * after the helper (a mapper register may have been written), so
         * that a copy loop (LD A,(HL) / LD (DE),A / INC HL / INC DE) does
         * not load it on every turn. (2, ES for HL - 1: still true, or
         * better.)
         */
        uint8_t keep = hl_es;
        uint16_t d = (uint16_t) (env->ram_delta + 0xC000);   // (IRAM of C000)
        if (addr_reg == X_BX) { B(0x8D); B(0xB7); W(0x4000); }          // lea si, [bx + 4000h]
        else { B(0x8D); B(0xBC); W(0x4000); }                           // lea di, [si + 4000h]
        B(0x81); B(addr_reg == X_BX ? 0xFE : 0xFF); W(0x2000);          // cmp si / di, 2000h
        if (!cold_off) {
            uint8_t *slow = jcc8(0x73);                                 // jae cold
            uint8_t m = addr_reg == X_BX ? 0x84 : 0x85;
            if (src == 0xFF) { B(0xC6); B(m); W(d); B(imm); }
            else { B(0x88); B(m | (src << 3)); W(d); }
            cold_add(slow, here(), COLD_WRITE, addr_reg, src, (int8_t) keep, imm);
            hl_es = keep;
            return;
        }
        uint8_t *fast = jcc8(0x72);                                     // jb fast
        if (src == 0xFF) { B(0xC6); B(0x06); W(SA(val)); B(imm); }
        else { B(0x88); B(0x06 | (src << 3)); W(SA(val)); }
        B(0x89); B(0x06 | (addr_reg << 3)); W(SA(arg));                 // mov [arg], reg
        helper(DRC_H_WRITE);
        if (keep) seg_hl(0);
        uint8_t *done = jcc8(0xEB);
        land8(fast);
        // [si + d] with SI = BX + 4000h, or [di + d] with DI = SI + 4000h
        uint8_t m = addr_reg == X_BX ? 0x84 : 0x85;
        if (src == 0xFF) { B(0xC6); B(m); W(d); B(imm); }
        else { B(0x88); B(m | (src << 3)); W(d); }
        land8(done);
        hl_es = keep;
        return;
    }
    uint8_t rm = addr_reg == X_BX ? 7 : 4;   // [bx+d16] / [si+d16]
    uint8_t *j1, *j2, *j3;
    if (addr_reg == X_BX) {
        // C000-DFFF + 4000h wraps to 0000-1FFF: one comparison
        B(0x8D); B(0xB7); W(0x4000);                   // lea si, [bx + 4000h]
        B(0x81); B(0xFE); W(0x2000); j1 = jcc8(0x73);  // cmp si, 2000h; jae slow
        j2 = 0;
        uint16_t d = (uint16_t) (env->ram_delta + 0xC000);   // (IRAM of C000)
        if (src == 0xFF) { B(0xC6); B(0x84); W(d); B(imm); }
        else { B(0x88); B(0x84 | (src << 3)); W(d); }
    } else {
        B(0x81); B(0xFE); W(0xC000); j1 = jcc8(0x72);
        B(0x81); B(0xFE); W(0xE000); j2 = jcc8(0x73);
        if (src == 0xFF) { B(0xC6); B(0x80 | rm); W(env->ram_delta); B(imm); }
        else { B(0x88); B(0x80 | (src << 3) | rm); W(env->ram_delta); }
    }
    j3 = jcc8(0xEB);
    land8(j1); if (j2) land8(j2);
    if (src == 0xFF) { B(0xC6); B(0x06); W(SA(val)); B(imm); }
    else { B(0x88); B(0x06 | (src << 3)); W(SA(val)); }
    B(0x89); B(0x06 | (addr_reg << 3)); W(SA(arg));   // mov [arg], reg
    helper(DRC_H_WRITE);
    land8(j3);
    if (addr_reg == X_BX) hl_es = 0;
}

/*
 * A mapper register write that leaves the block's own slot (and slot 0)
 * alone: the block may go on and its exits stay linked (links to the
 * other slots check the bank anyway). FFFC (cartridge RAM) and FFFF
 * change slot 2, FFFE slot 1, FFFD slot 0.
 */
static int map_write_safe(uint16_t addr) {
    if (env->codemasters) {
        if (addr >= 0xC000 || (addr & 0x3FFF)) return 1;
        uint8_t slot = addr >> 14;
        return slot != 0 && slot != (block_pc >> 14);
    }
    if (addr < 0xFFFC) return 1;
    uint8_t slot = addr == 0xFFFD ? 0 : addr == 0xFFFE ? 1 : 2;
    return slot != 0 && slot != (block_pc >> 14);
}

static void write_const(uint16_t addr, uint8_t src, uint8_t live) {
    if (FLAT) {
        clobber(live);
        B(0x80); B(0x3E); W(FENV(cmap) + (addr >> 4)); B(0);   // cmp byte [cmap + its 16 bytes], 0
        uint8_t *fast = jcc8(0x74);
        B(0x88); B(0x06 | (src << 3)); W(SA(val));
        B(0xC7); B(0x06); W(SA(arg)); W(addr);
        flat_pc();
        helper(DRC_H_WRITE);
        uint8_t *done = jcc8(0xEB);
        land8(fast);
        B(0x26);
        if (src == X_AX) { B(0xA2); }
        else { B(0x88); B(0x06 | (src << 3)); }
        W(addr);
        land8(done);
        return;
    }
    if (addr >= 0xC000 && (addr < 0xE000 || env->codemasters)) {
        uint16_t a = (addr & 0x1FFF) + 0xC000 + env->ram_delta;
        if (src == X_AX) { B(0xA2); }
        else { B(0x88); B(0x06 | (src << 3)); }
        W(a);
        return;
    }
    clobber(live);
    uint8_t *same = 0;
    if (!env->codemasters && addr >= 0xFFFD && env->psg_queue) {
        // a bank register written with the bank it maps already (a
        // quarter of the mapper writes in Super Monaco GP II, Aladdin,
        // Shinobi): the RAM mirror only, no helper. drc_st.keys[1..3]: the
        // banks of slots 0-2 (as mapped, % ROM banks: a value past the ROM
        // takes the helper); slot 2 only while it is ROM (keys[0] == 0)
        uint8_t k = addr - 0xFFFC;
        uint8_t *other = 0;
        if (k == 3) {
            B(0x80); B(0x3E); W(SA(keys)); B(0);     // cmp byte [keys], 0
            other = jcc8(0x75);                      // jne helper
        }
        B(0x3A); B(0x06 | (src << 3)); W(SA(keys) + k);   // cmp r8, [keys + k]
        uint8_t *diff = jcc8(0x75);                  // jne helper
        B(0x88); B(0x06 | (src << 3)); W((addr & 0x1FFF) + 0xC000 + env->ram_delta);   // mirror
        same = jcc8(0xEB);                           // jmp done
        land8(other); land8(diff);
    } else if (env->codemasters && (addr == 0x4000 || addr == 0x8000) && env->psg_queue) {
        uint8_t k = 1 + (addr >> 14);                // keys[2] (slot 1) or keys[3] (slot 2)
        B(0x3A); B(0x06 | (src << 3)); W(SA(keys) + k);   // cmp r8, [keys + k]
        same = jcc8(0x74);                           // je done (same bank already mapped)
    }
    B(0x88); B(0x06 | (src << 3)); W(SA(val));
    uint8_t *mapped = 0;
    uint16_t map_off = env->codemasters
        ? (addr == 0x4000 ? env->map1_off : addr == 0x8000 ? env->map2_off : 0)
        : (addr == 0xFFFE ? env->map1_off : addr == 0xFFFF ? env->map2_off : 0);
    if (map_off && map_write_safe(addr)) {
        // another slot's bank (not slot 0, not the block's own): the
        // routine maps it (drc_rt.c), the C side later; carry: the helper
        call_near(map_off);
        mapped = jcc8(0x73);                         // jnc done
    }
    B(0xC7); B(0x06); W(SA(arg)); W(addr);
    uint8_t is_map = env->codemasters
        ? (addr < 0xC000 && !(addr & 0x3FFF))
        : (addr >= 0xFFFC);
    helper(is_map && map_write_safe(addr) ? DRC_H_WRITE_MAP : DRC_H_WRITE);
    land8(mapped);
    land8(same);
}

// Read a byte from a constant address into an 8-bit register
static void read_const(uint16_t addr, uint8_t dst) {
    if (FLAT) {
        B(0x26);
        if (dst == X_AX) { B(0xA0); } else { B(0x8A); B(0x06 | (dst << 3)); }
        W(addr);
        return;
    }
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
    if (FLAT) {
        B(0x26); B(0x8B); B(0x06 | (dst << 3)); W(addr);
        return;
    }
    if (addr >= 0xC000 && addr != 0xDFFF && addr != 0xFFFF) {
        B(0x8B); B(0x06 | (dst << 3)); W((addr & 0x1FFF) + 0xC000 + env->ram_delta);
        return;
    }
    B(0x8E); B(0x06); W(SA(segtab) + (addr >> 13) * 2);
    hl_es = 0;
    B(0x26); B(0x8B); B(0x06 | (dst << 3)); W(addr);
}

/*
 * Fast path of a write to the VDP data port (VRAM; CRAM goes to *slow1).
 * The Game Gear VRAM is the WS tile memory: tile data needs nothing more;
 * name table writes also update the WS screen map (routine at
 * nt_stub_off), sprite table writes mark the sprites to be rebuilt.
 * src: x86 8-bit register. x86 flags destroyed; uses SI and DI.
 */
static void vram_fast(uint8_t src, uint8_t **slow1, uint8_t inl) {
    B(0x80); B(0x3E); W(VA(code)); B(3);                 // cmp byte [code], 3
    uint8_t *cram_end = 0;
    int8_t cram_cold = -1;
    if (env->cram_off && inl && COLD_FREE()) {
        // CRAM (rare: palettes): the routine in the cold tail
        uint8_t *j = jcc8(0x74);                         // je cold
        cram_cold = (int8_t) cold_n;
        cold_add(j, 0, COLD_CRAM, src, 0, 0, 0);         // (resume: at the end)
        *slow1 = 0;
    } else if (env->cram_off) {
        // CRAM: the routine (no slow path left)
        uint8_t *not_cram = jcc8(0x75);                  // jne
        B(0x88); B(0x06 | (src << 3)); W(SA(val));       // mov [val], src
        call_near(env->cram_off);
        cram_end = jcc8(0xEB);
        land8(not_cram);
        *slow1 = 0;
    } else {
        *slow1 = jcc8(0x74);                             // je slow (CRAM)
    }
    B(0x8B); B(0x36); W(VA(addr));                       // mov si, [addr]
    B(0x81); B(0xE6); W(0x3FFF);                         // and si, 3FFFh
    if (env->cls_off) {
        // the page class (main.c) first: tile data written here; the name
        // table, the sprite table, deferred writes (the log): the routine,
        // which stores the byte itself (from read_buffer)
        B(0x88); B(0x06 | (src << 3)); W(VA(read_buffer));   // mov [read_buffer], src
        B(0x89); B(0xF7);                                // mov di, si
        B(0xC1); B(0xEF); B(0x08);                       // shr di, 8
        B(0xF6); B(0x85); W(SA(vram_class)); B(0x0B);    // test byte [di + class], 0Bh
        uint8_t *plain = jcc8(0x74);                     // jz (tile data)
        // deferred, the byte after the last one logged (Sailor Moon S's
        // tiles, a byte an OUT, ~190 a frame): appended here, as the
        // routine's first case, without the call (in blocks: inl)
        uint8_t *r1 = 0, *r2 = 0, *r3 = 0, *done1 = 0, *done2 = 0, *done3 = 0;
        if (inl) {
            B(0xF6); B(0x85); W(SA(vram_class)); B(0x08); // test byte [di + class], 8
            uint8_t *live = jcc8(0x74);                  // jz live
            B(0x3B); B(0x36); W(SA(log_next));           // cmp si, [log_next]
            r2 = jcc8(0x75);                             // jne routine
            B(0x8B); B(0x3E); W(SA(log_top));            // mov di, [log_top]
            B(0x3B); B(0x3E); W(SA(log_end));            // cmp di, [log_end]
            r3 = jcc8(0x73);                             // jae routine
            B(0x88); B(0x05 | (src << 3));               // mov [di], src
            B(0x47);                                     // inc di
            B(0x89); B(0x3E); W(SA(log_top));            // mov [log_top], di
            B(0x8B); B(0x3E); W(SA(log_last));           // mov di, [log_last]
            B(0xFE); B(0x45); B(0x02);                   // inc byte [di + 2]
            B(0x80); B(0x7D); B(0x02); B(0xFF);          // cmp byte [di + 2], FFh
            uint8_t *full = jcc8(0x74);                  // je full
            B(0xFF); B(0x06); W(SA(log_next));           // inc word [log_next]
            done1 = jcc8(0xEB);
            land8(full);
            B(0xC7); B(0x06); W(SA(log_next)); W(0xFFFF); // mov word [log_next], FFFFh
            done2 = jcc8(0xEB);
            // live, the sprite table alone (Sonic sends its 64 Y and
            // the X and tile of each sprite in use a byte an OUT, every
            // frame): stored here, as the routine does (inl bit 1: OUT
            // (n),A; with it OUT (C),r's short jump over all this would
            // not reach)
            land8(live);
            if (inl & 2) {
                B(0xF6); B(0x85); W(SA(vram_class)); B(0x01); // test byte [di + class], 1
                r1 = jcc8(0x75);                         // jnz routine (name table)
                B(0x88); B(0x84 | (src << 3)); W(env->vram_addr);    // mov [si + vram], src
                B(0xC6); B(0x06); W(SA(sprites_dirty)); B(1);    // sprites_dirty = 1
                done3 = jcc8(0xEB);
            }
            land8(r1); land8(r2); land8(r3);
        }
        call_near(env->cls_off);
        uint8_t *done = jcc8(0xEB);
        land8(plain);
        B(0x88); B(0x84 | (src << 3)); W(env->vram_addr);    // mov [si + vram], src
        land8(done);
        if (done1) { land8(done1); land8(done2); land8(done3); }
        B(0x46);                                         // inc si
        B(0x81); B(0xE6); W(0x3FFF);                     // and si, 3FFFh
        B(0x89); B(0x36); W(VA(addr));                   // mov [addr], si
        B(0xC6); B(0x06); W(VA(latch_full)); B(0);       // mov byte [latch_full], 0
        land8(cram_end);
        if (cram_cold >= 0) cold[cram_cold].resume_off = here();
        return;
    }
    B(0x88); B(0x84 | (src << 3)); W(env->vram_addr);    // mov [si + vram], src
    B(0x88); B(0x06 | (src << 3)); W(VA(read_buffer));   // mov [read_buffer], src
    // the page class (main.c): 1 name table, 2 sprite table, 0 tile data
    B(0x89); B(0xF7);                                    // mov di, si
    B(0xC1); B(0xEF); B(0x08);                           // shr di, 8
    B(0xF6); B(0x85); W(SA(vram_class)); B(3);           // test byte [di + class], 3
    uint8_t *plain = jcc8(0x74);                         // jz (tile data)
    B(0xF6); B(0x85); W(SA(vram_class)); B(1);           // test byte [di + class], 1
    uint8_t *not_nt = jcc8(0x74);                        // jz
    call_near(env->nt_stub_off);                         // name table
    uint8_t *after = jcc8(0xEB);
    land8(not_nt);
    B(0xC6); B(0x06); W(SA(sprites_dirty)); B(1);        // sprites_dirty = 1
    land8(after);
    land8(plain);
    B(0x46);                                             // inc si
    B(0x81); B(0xE6); W(0x3FFF);                         // and si, 3FFFh
    B(0x89); B(0x36); W(VA(addr));                       // mov [addr], si
    B(0xC6); B(0x06); W(VA(latch_full)); B(0);           // mov byte [latch_full], 0
    land8(cram_end);
    if (cram_cold >= 0) cold[cram_cold].resume_off = here();
}

/*
 * Routine for a VRAM byte that is not plain tile data (at env->cls_off),
 * called by the VRAM paths before the byte is stored, with SI = VRAM
 * address, DI = its page, the byte in read_buffer. Deferred (class bit 3,
 * main.c): the byte goes to the log, joined to the last entry when it
 * follows it; a full log is written now (helper SYNC) and the byte goes to
 * the empty one. Otherwise (the sprite table, or X4 classes): stored, then
 * the name table routine or sprites_dirty = 1. Keeps SI and the registers.
 */
uint16_t drc_emit_cls_stub(const drc_env_t *e, uint8_t *out, uint16_t out_off) {
    env = e;
    o = o_start = out; o_base = out_off;
    B(0xF6); B(0x85); W(SA(vram_class)); B(8);       // test byte [di + class], 8
    uint8_t *deferred = jcc8(0x75);                  // jnz deferred
    // live: stored, then the name table routine or sprites_dirty; the
    // byte already there: nothing to do (the screen map and the sprites
    // are those of VRAM as it is; Sonic sends a row and a column of its
    // map again on every frame it scrolls)
    B(0x50);                                         // push ax
    B(0xA0); W(VA(read_buffer));                     // mov al, [read_buffer]
    B(0x38); B(0x84); W(e->vram_addr);               // cmp [si + vram], al
    uint8_t *same = jcc8(0x74);                      // je same
    B(0x88); B(0x84); W(e->vram_addr);               // mov [si + vram], al
    B(0x58);                                         // pop ax
    B(0xF6); B(0x85); W(SA(vram_class)); B(1);       // test byte [di + class], 1
    uint8_t *sat = jcc8(0x74);                       // jz sat
    B(0xE9); W((uint16_t) (e->nt_stub_off - (o_base + (uint16_t) (o - o_start) + 2)));   // jmp NT
    land8(sat);
    B(0xC6); B(0x06); W(SA(sprites_dirty)); B(1);    // sprites_dirty = 1
    B(0xC3);                                         // ret
    land8(same);
    B(0x58);                                         // pop ax
    B(0xC3);                                         // ret
    land8(deferred);
    // the byte that follows the last entry this routine made (log_next):
    // joined to it without saving registers
    B(0x3B); B(0x36); W(SA(log_next));               // cmp si, [log_next]
    uint8_t *s1 = jcc8(0x75);                        // jne slow
    B(0x8B); B(0x3E); W(SA(log_top));                // mov di, [log_top]
    B(0x3B); B(0x3E); W(SA(log_end));                // cmp di, [log_end]
    uint8_t *s2 = jcc8(0x73);                        // jae slow
    B(0x50);                                         // push ax
    B(0xA0); W(VA(read_buffer));                     // mov al, [read_buffer]
    B(0x88); B(0x05);                                // mov [di], al
    B(0x58);                                         // pop ax
    B(0x47);                                         // inc di
    B(0x89); B(0x3E); W(SA(log_top));                // mov [log_top], di
    B(0x8B); B(0x3E); W(SA(log_last));               // mov di, [log_last]
    B(0xFE); B(0x45); B(0x02);                       // inc byte [di + 2]
    B(0x80); B(0x7D); B(0x02); B(0xFF);              // cmp byte [di + 2], 255
    uint8_t *f255 = jcc8(0x74);                      // je full entry
    B(0xFF); B(0x06); W(SA(log_next));               // inc word [log_next]
    B(0xC3);                                         // ret
    land8(f255);
    B(0xC7); B(0x06); W(SA(log_next)); W(0xFFFF);    // mov word [log_next], FFFFh
    B(0xC3);                                         // ret
    land8(s1); land8(s2);
    uint8_t *retry = o;
    B(0x50); B(0x53); B(0x51); B(0x57);              // push ax, bx, cx, di
    B(0xA0); W(VA(read_buffer));                     // mov al, [read_buffer] (the byte)
    B(0x8B); B(0x1E); W(SA(log_last));               // mov bx, [log_last]
    B(0x09); B(0xDB);                                // or bx, bx
    uint8_t *n1 = jcc8(0x74);                        // jz new
    B(0x8A); B(0x4F); B(0x02);                       // mov cl, [bx + 2] (length)
    B(0x30); B(0xED);                                // xor ch, ch
    B(0x80); B(0xF9); B(0xFF);                       // cmp cl, 255
    uint8_t *n2 = jcc8(0x74);                        // je new
    B(0x89); B(0xDF);                                // mov di, bx
    B(0x01); B(0xCF);                                // add di, cx
    B(0x83); B(0xC7); B(0x03);                       // add di, 3
    B(0x3B); B(0x3E); W(SA(log_top));                // cmp di, [log_top] (the last entry?)
    uint8_t *n3 = jcc8(0x75);                        // jne new
    B(0x8B); B(0x3F);                                // mov di, [bx] (its address)
    B(0x01); B(0xCF);                                // add di, cx
    B(0x39); B(0xF7);                                // cmp di, si (this byte follows it?)
    uint8_t *n4 = jcc8(0x75);                        // jne new
    B(0x8B); B(0x3E); W(SA(log_top));                // mov di, [log_top]
    B(0x3B); B(0x3E); W(SA(log_end));                // cmp di, [log_end]
    uint8_t *f1 = jcc8(0x73);                        // jae full
    B(0x88); B(0x05);                                // mov [di], al
    B(0x47);                                         // inc di
    B(0x89); B(0x3E); W(SA(log_top));                // mov [log_top], di
    B(0xFE); B(0x47); B(0x02);                       // inc byte [bx + 2]
    uint8_t *out1 = jcc8(0xEB);                      // jmp out
    // (here, in reach of the short jumps)
    land8(f1);
    uint8_t *full = o;
    B(0x5F); B(0x59); B(0x5B); B(0x58);              // full: pop di, cx, bx, ax
    B(0xC6); B(0x06); W(SA(op)); B(DRC_H_SYNC);      // mov byte [op], SYNC (the log to VRAM)
    call_near(e->helper_off);                        // (keeps SI, DI, ES, the registers)
    B(0xEB); B((uint8_t) (retry - (o + 1)));         // jmp retry
    land8(n1); land8(n2); land8(n3); land8(n4);
    B(0x8B); B(0x3E); W(SA(log_top));                // new: mov di, [log_top]
    B(0x89); B(0xFB);                                // mov bx, di
    B(0x83); B(0xC3); B(0x04);                       // add bx, 4
    B(0x3B); B(0x1E); W(SA(log_end));                // cmp bx, [log_end]
    B(0x77); B((uint8_t) (full - (o + 1)));          // ja full
    B(0x89); B(0x35);                                // mov [di], si (address)
    B(0xC6); B(0x45); B(0x02); B(0x01);              // mov byte [di + 2], 1
    B(0x88); B(0x45); B(0x03);                       // mov [di + 3], al
    B(0x89); B(0x3E); W(SA(log_last));               // mov [log_last], di
    B(0x89); B(0x1E); W(SA(log_top));                // mov [log_top], bx
    land8(out1);
    // out: the next byte joins this entry (log_next), unless it is full
    B(0x8B); B(0x1E); W(SA(log_last));               // mov bx, [log_last]
    B(0x89); B(0xF7);                                // mov di, si
    B(0x47);                                         // inc di
    B(0x80); B(0x7F); B(0x02); B(0xFF);              // cmp byte [bx + 2], 255
    B(0x75); B(0x03);                                // jne +3
    B(0xBF); W(0xFFFF);                              // mov di, FFFFh
    B(0x89); B(0x3E); W(SA(log_next));               // mov [log_next], di
    B(0x5F); B(0x59); B(0x5B); B(0x58);              // pop di, cx, bx, ax
    B(0xC3);                                         // ret
    return (uint16_t) (o - o_start);
}

/*
 * Routine for OUTI / OUTI that write one whole entry of the name table
 * (at env->ntw_off): Sonic sends a column and a row of its map this way
 * on every frame it scrolls, 54 entries, and through the run routine and
 * the name table routine each took about 200 cycles. Port BEh, not CRAM,
 * an even VRAM address in a page of the live name table alone, the two
 * bytes inside one 8 KiB page: the entry stored and converted (the name
 * table routine), or nothing at all when it is the one already there (the
 * screen map is the one of VRAM as it is), then address, read buffer,
 * latch, HL, B as two OUTIs leave them; CF = 0, AH = the last byte (as
 * the run routine). The same for two bytes of the sprite table (a
 * sprite's X and tile: Super Monaco GP II sends its 64 this way on every
 * frame), stored with sprites_dirty set, or nothing when they are there
 * already. Anything else: CF = 1, nothing done (the caller uses the run
 * routine). Uses SI, DI, ES.
 */
uint16_t drc_emit_ntw_stub(const drc_env_t *e, uint8_t *out, uint16_t out_off, uint16_t *entry_a) {
    env = e;
    o = o_start = out; o_base = out_off;
    uint8_t *f[5]; uint8_t nf = 0;
    // (fail first, the jumps to it go backwards; the entry at + DRC_NTW_ENTRY)
    uint8_t *fail = o;
    B(0xF9);                                         // fail: stc
    B(0xC3);                                         // ret
#ifdef GGSWAN_HOST
#define NFAIL(op) do { if (o + 2 - fail > 128) { extern void abort(void); abort(); } \
        B(op); B((uint8_t) (fail - (o + 1))); } while (0)
#else
#define NFAIL(op) do { B(op); B((uint8_t) (fail - (o + 1))); } while (0)
#endif
    B(0x89); B(0xCE);                                // mov si, cx
    B(0x81); B(0xE6); W(0x00C1);                     // and si, C1h
    B(0x81); B(0xFE); W(0x0080);                     // cmp si, 80h (port BEh)
    NFAIL(0x75);                                      // jne fail
    B(0x80); B(0x3E); W(VA(code)); B(3);             // cmp byte [code], 3
    NFAIL(0x74);                                      // je fail (CRAM)
    B(0x8B); B(0x3E); W(VA(addr));                   // mov di, [addr]
    B(0x81); B(0xE7); W(0x3FFF);                     // and di, 3FFFh
    B(0xF7); B(0xC7); W(0x0001);                     // test di, 1
    NFAIL(0x75);                                      // jnz fail (not a whole entry)
    B(0x89); B(0xFE);                                // mov si, di
    B(0xC1); B(0xEE); B(0x08);                       // shr si, 8
    B(0x80); B(0xBC); W(SA(vram_class)); B(1);       // cmp byte [si + class], 1
    B(0x74); B(0x07);                                // je +7 (the name table)
    B(0x80); B(0xBC); W(SA(vram_class)); B(2);       // cmp byte [si + class], 2
    NFAIL(0x75);                                      // jne fail (not the sprite table either)
    B(0x89); B(0xDE);                                // mov si, bx
    B(0x81); B(0xE6); W(0x1FFF);                     // and si, 1FFFh
    B(0x81); B(0xFE); W(0x1FFE);                     // cmp si, 1FFEh
    NFAIL(0x77);                                      // ja fail (the page ends)
    B(0x89); B(0xDE);                                // mov si, bx
    B(0xC1); B(0xC6); B(0x04);                       // rol si, 4
    B(0x83); B(0xE6); B(0x0E);                       // and si, 14
    B(0x8E); B(0x84); W(SA(segtab));                 // mov es, [si + segtab]
    B(0x26); B(0x8B); B(0x37);                       // mov si, es:[bx] (the entry)
    B(0x83); B(0xC3); B(0x02);                       // add bx, 2
    B(0x80); B(0xED); B(0x02);                       // sub ch, 2
    B(0x26); B(0x8A); B(0x67); B(0xFF);              // mov ah, es:[bx - 1] (the last byte)
    B(0x88); B(0x26); W(VA(read_buffer));            // mov [read_buffer], ah
    B(0xC6); B(0x06); W(VA(latch_full)); B(0);       // mov byte [latch_full], 0
    uint8_t *tail = o;
    B(0x3B); B(0xB5); W(env->vram_addr);             // tail: cmp si, [di + vram]
    uint8_t *same = jcc8(0x74);                      // je same
    B(0x89); B(0xB5); W(env->vram_addr);             // mov [di + vram], si
    B(0x89); B(0xFE);                                // mov si, di
    B(0xC1); B(0xEE); B(0x08);                       // shr si, 8
    B(0x80); B(0xBC); W(SA(vram_class)); B(1);       // cmp byte [si + class], 1
    uint8_t *sat = jcc8(0x75);                       // jne sat
    B(0x89); B(0xFE);                                // mov si, di
    call_near(env->nt_stub_off);                     // (keeps the registers)
    uint8_t *same2 = jcc8(0xEB);                     // jmp same
    land8(sat);
    B(0xC6); B(0x06); W(SA(sprites_dirty)); B(1);    // sat: sprites_dirty = 1
    land8(same2);
    land8(same);
    B(0x83); B(0xC7); B(0x02);                       // add di, 2
    B(0x81); B(0xE7); W(0x3FFF);                     // and di, 3FFFh
    B(0x89); B(0x3E); W(VA(addr));                   // mov [addr], di
    B(0xF8);                                         // clc
    B(0xC3);                                         // ret
#undef NFAIL
    /*
     * The entry for OUTI / OUT (BEh),A: an entry whose tile comes from
     * (HL) and whose second byte is A. Columns draws its two wells this
     * way on every frame, six entries a row and some 200 a frame, the
     * same as the frame before but for the falling column; each took
     * about 250 cycles (the run routine for one byte, the name table
     * routine, the single write). The same conditions and results, AH =
     * the OUTI's byte.
     */
    *entry_a = (uint16_t) (o_base + (o - o_start));
    nf = 0;
    B(0x89); B(0xCE);                                // mov si, cx
    B(0x81); B(0xE6); W(0x00C1);                     // and si, C1h
    B(0x81); B(0xFE); W(0x0080);                     // cmp si, 80h (port BEh)
    f[nf++] = jcc8(0x75);                            // jne fail
    B(0x80); B(0x3E); W(VA(code)); B(3);             // cmp byte [code], 3
    f[nf++] = jcc8(0x74);                            // je fail (CRAM)
    B(0x8B); B(0x3E); W(VA(addr));                   // mov di, [addr]
    B(0x81); B(0xE7); W(0x3FFF);                     // and di, 3FFFh
    B(0xF7); B(0xC7); W(0x0001);                     // test di, 1
    f[nf++] = jcc8(0x75);                            // jnz fail (not a whole entry)
    B(0x89); B(0xFE);                                // mov si, di
    B(0xC1); B(0xEE); B(0x08);                       // shr si, 8
    B(0x80); B(0xBC); W(SA(vram_class)); B(1);       // cmp byte [si + class], 1
    B(0x74); B(0x07);                                // je +7 (the name table)
    B(0x80); B(0xBC); W(SA(vram_class)); B(2);       // cmp byte [si + class], 2
    f[nf++] = jcc8(0x75);                            // jne fail (not the sprite table either)
    B(0x89); B(0xDE);                                // mov si, bx
    B(0xC1); B(0xC6); B(0x04);                       // rol si, 4
    B(0x83); B(0xE6); B(0x0E);                       // and si, 14
    B(0x8E); B(0x84); W(SA(segtab));                 // mov es, [si + segtab]
    B(0x26); B(0x8A); B(0x27);                       // mov ah, es:[bx] (the tile)
    B(0x43);                                         // inc bx
    B(0xFE); B(0xCD);                                // dec ch
    B(0xA2); W(VA(read_buffer));                     // mov [read_buffer], al
    B(0xC6); B(0x06); W(VA(latch_full)); B(0);       // mov byte [latch_full], 0
    B(0x89); B(0xC6);                                // mov si, ax
    B(0xC1); B(0xC6); B(0x08);                       // rol si, 8 (A the second byte)
    B(0xE9); W((uint16_t) (tail - (o + 2)));         // jmp tail
    for (uint8_t i = 0; i < nf; i++) land8(f[i]);
    B(0xF9);                                         // stc
    B(0xC3);                                         // ret
    return (uint16_t) (o - o_start);
}

/*
 * Routine for OUTI / OUTI / OUTI / OUT (BEh),A (entered at env->trow_off):
 * a tile row whose fourth plane is A, the way Sonic (and Sega's other
 * games with 8-color sprites) upload their animated tiles; Sonic's own
 * are 192 rows in one frame, every nine or so. Through the run routine
 * and the single write each row made two entries in the log of deferred
 * tile bytes, of three bytes and of one, about 200 cycles. Port BEh, not
 * CRAM, the four VRAM bytes in deferred tile data, the three source
 * bytes inside one 8 KiB page, room in the log: the four bytes added to
 * the log's open entry when they follow it (up to 254 bytes an entry),
 * else as a new one, then address, read buffer, latch, HL, B as the four
 * instructions leave them; CF = 0, AH = the third byte (the last OUTI's:
 * the flags). Anything else: CF = 1, nothing done (the block runs the
 * instructions as usual). Uses SI, DI, ES.
 */
uint16_t drc_emit_trow_stub(const drc_env_t *e, uint8_t *out, uint16_t out_off) {
    env = e;
    o = o_start = out; o_base = out_off;
    uint8_t *fail = o;
    B(0xF9);                                         // fail: stc
    B(0xC3);                                         // ret
#ifdef GGSWAN_HOST
#define TFAIL(op) do { if (o + 2 - fail > 128) { extern void abort(void); abort(); } \
        B(op); B((uint8_t) (fail - (o + 1))); } while (0)
#else
#define TFAIL(op) do { B(op); B((uint8_t) (fail - (o + 1))); } while (0)
#endif
    B(0x89); B(0xCE);                                // mov si, cx
    B(0x81); B(0xE6); W(0x00C1);                     // and si, C1h
    B(0x81); B(0xFE); W(0x0080);                     // cmp si, 80h (port BEh)
    TFAIL(0x75);                                     // jne fail
    B(0x80); B(0x3E); W(VA(code)); B(3);             // cmp byte [code], 3
    TFAIL(0x74);                                     // je fail (CRAM)
    B(0x89); B(0xDE);                                // mov si, bx
    B(0x81); B(0xE6); W(0x1FFF);                     // and si, 1FFFh
    B(0x81); B(0xFE); W(0x1FFD);                     // cmp si, 1FFDh
    TFAIL(0x77);                                     // ja fail (the page ends)
    B(0x8B); B(0x36); W(VA(addr));                   // mov si, [addr]
    B(0x81); B(0xE6); W(0x3FFF);                     // and si, 3FFFh
    B(0x89); B(0xF7);                                // mov di, si
    B(0xC1); B(0xEF); B(0x08);                       // shr di, 8
    B(0x80); B(0xBD); W(SA(vram_class)); B(8);       // cmp byte [di + class], 8
    TFAIL(0x75);                                     // jne fail
    B(0x8D); B(0x7C); B(0x03);                       // lea di, [si + 3]
    B(0xC1); B(0xEF); B(0x08);                       // shr di, 8
    B(0x80); B(0xBD); W(SA(vram_class)); B(8);       // cmp byte [di + class], 8
    TFAIL(0x75);                                     // jne fail
    B(0x8B); B(0x3E); W(SA(log_top));                // mov di, [log_top]
    B(0x3B); B(0x36); W(SA(log_next));               // cmp si, [log_next]
    uint8_t *new1 = jcc8(0x75);                      // jne new
    B(0x8B); B(0x36); W(SA(log_last));               // mov si, [log_last]
    B(0x80); B(0x7C); B(0x02); B(250);               // cmp byte [si + 2], 250
    uint8_t *new2 = jcc8(0x77);                      // ja new (it would fill up)
    B(0x83); B(0xC7); B(0x04);                       // add di, 4
    B(0x3B); B(0x3E); W(SA(log_end));                // cmp di, [log_end]
    uint8_t *full = jcc8(0x77);                      // ja fail (no room)
    B(0x89); B(0x3E); W(SA(log_top));                // mov [log_top], di
    B(0x83); B(0xEF); B(0x04);                       // sub di, 4
    B(0x80); B(0x44); B(0x02); B(0x04);              // add byte [si + 2], 4 (length)
    B(0x83); B(0x06); W(SA(log_next)); B(0x04);      // add word [log_next], 4
    uint8_t *copy = jcc8(0xEB);                      // jmp copy
    // (a second fail, in reach of the short jumps from here on)
    land8(full);
    fail = o;
    B(0xF9);                                         // fail: stc
    B(0xC3);                                         // ret
    land8(new1); land8(new2);
    B(0x8B); B(0x36); W(VA(addr));                   // new: mov si, [addr]
    B(0x81); B(0xE6); W(0x3FFF);                     // and si, 3FFFh
    B(0x83); B(0xC7); B(0x07);                       // add di, 7
    B(0x3B); B(0x3E); W(SA(log_end));                // cmp di, [log_end]
    TFAIL(0x77);                                     // ja fail (no room)
    B(0x89); B(0x3E); W(SA(log_top));                // mov [log_top], di
    B(0x83); B(0xEF); B(0x07);                       // sub di, 7
    B(0x89); B(0x3E); W(SA(log_last));               // mov [log_last], di (this entry)
    B(0x89); B(0x35);                                // mov [di], si (VRAM address)
    B(0xC6); B(0x45); B(0x02); B(0x04);              // mov byte [di + 2], 4 (length)
    B(0x83); B(0xC7); B(0x03);                       // add di, 3
    B(0x83); B(0xC6); B(0x04);                       // add si, 4
    B(0x89); B(0x36); W(SA(log_next));               // mov [log_next], si
    land8(copy);
    B(0x89); B(0xDE);                                // copy: mov si, bx
    B(0xC1); B(0xC6); B(0x04);                       // rol si, 4
    B(0x83); B(0xE6); B(0x0E);                       // and si, 14
    B(0x8E); B(0x84); W(SA(segtab));                 // mov es, [si + segtab]
    B(0x26); B(0x8B); B(0x37);                       // mov si, es:[bx]
    B(0x89); B(0x35);                                // mov [di], si
    B(0x26); B(0x8A); B(0x67); B(0x02);              // mov ah, es:[bx + 2]
    B(0x88); B(0x65); B(0x02);                       // mov [di + 2], ah
    B(0x88); B(0x45); B(0x03);                       // mov [di + 3], al (A)
    B(0x83); B(0xC3); B(0x03);                       // add bx, 3
    B(0x80); B(0xED); B(0x03);                       // sub ch, 3
    B(0x8B); B(0x36); W(VA(addr));                   // mov si, [addr]
    B(0x83); B(0xC6); B(0x04);                       // add si, 4
    B(0x81); B(0xE6); W(0x3FFF);                     // and si, 3FFFh
    B(0x89); B(0x36); W(VA(addr));                   // mov [addr], si
    B(0xA2); W(VA(read_buffer));                     // mov [read_buffer], al
    B(0xC6); B(0x06); W(VA(latch_full)); B(0);       // mov byte [latch_full], 0
    B(0xF8);                                         // clc
    B(0xC3);                                         // ret
#undef TFAIL
    return (uint16_t) (o - o_start);
}

/*
 * Routine for a run of OUTI (dec: OUTD), shared by the blocks of a code
 * bank. In: DI = k (OUTIs), SI = Z80 address of the run. VRAM uploads
 * through the fast path; returns CF = 0 with the flags of the last OUTI
 * in AH. Anything else (CRAM, another port): CF = 1, with val = OUTIs
 * left, arg = address of the first one left, their cycles added back to
 * the counter (the block calls the STEP helper). Uses SI, DI, ES.
 */
#define OUTI_SHORT 4   // runs up to this long: a byte loop instead of REP MOVSB

uint16_t drc_emit_run_stub(const drc_env_t *e, uint8_t *out, uint16_t out_off, uint8_t dec, uint16_t *one) {
    env = e;
    o = o_start = out; o_base = out_off;
    uint8_t inc = dec ? 0xFF : 0x01;
    B(0x56);                                         // push si (address)
    B(0x57);                                         // push di (k)
    B(0x89); B(0xDE);                                // mov si, bx
    B(0xC1); B(0xC6); B(0x04);                       // rol si, 4
    B(0x83); B(0xE6); B(0x0E);                       // and si, 14
    B(0x8E); B(0x84); W(SA(segtab));                 // mov es, [si + segtab]
    B(0x89); B(0xCE);                                // mov si, cx
    B(0x81); B(0xE6); W(0x00C1);                     // and si, C1h
    B(0x81); B(0xFE); W(0x0080);                     // cmp si, 80h (port BEh)
    uint8_t *after_cmp = o;
    uint8_t *s0 = 0, *to_ctl = 0;
    if (!dec && !env->no_fast && env->ctl_off) {
        B(0x74); B(3);                               // je +3
        B(0xE9); W(0); to_ctl = o - 2;               // jmp CTL (at the end)
    } else
        s0 = jcc8(0x75);                             // jne slow
    uint8_t *to_fast = 0, *to_loop = 0;
    if (!dec && !env->no_fast) {
        B(0xE9); W(0); to_fast = o - 2;              // jmp FAST (at the end)
    } else {
        to_loop = jcc8(0xEB);                        // jmp loop
    }
    // slow (before the loop, in reach of its short jumps): the OUTIs
    // left (DI of them) for the interpreter
    uint8_t *slow1 = o;
    B(0x5F);                                         // pop di
    if (s0) land8(s0);
    uint8_t *slow0 = o;
    B(0x89); B(0x3E); W(SA(val));                    // mov [val], di (count)
    B(0x5E);                                         // pop si (k)
    B(0x29); B(0xFE);                                // sub si, di
    B(0xD1); B(0xE6);                                // shl si, 1
    B(0x89); B(0x36); W(SA(arg));                    // mov [arg], si
    B(0x5E);                                         // pop si (address)
    B(0x01); B(0x36); W(SA(arg));                    // add [arg], si
    B(0x89); B(0xFE);                                // mov si, di
    B(0xC1); B(0xE6); B(0x04);                       // shl si, 4
    B(0x01); B(0x36); W(SA(cycles));                 // add [cycles], si
    B(0xF9);                                         // stc
    B(0xC3);                                         // ret
    if (to_loop) land8(to_loop);
    uint8_t *loop = o;
    B(0x57);                                         // push di
    uint8_t *s1;
    B(0x26); B(0x8A); B(0x27);                       // mov ah, es:[bx]
    vram_fast(4 /* AH */, &s1, 0);   // (the routines: short)
    if (s1) *s1 = (uint8_t) (int8_t) (slow1 - (s1 + 1));   // (backwards)
    B(0x5F);                                         // pop di
    B(0xFE); B(0xCD);                                // dec ch
    B(0x8D); B(0x5F); B(inc);                        // lea bx, [bx +- 1]
    B(0xF7); B(0xC3); W(0x1FFF);                     // test bx, 1FFFh
    uint8_t *noseg = jcc8(0x75);                     // jnz: same ES
    B(0x89); B(0xDE);                                // mov si, bx
    B(0xC1); B(0xC6); B(0x04);                       // rol si, 4
    B(0x83); B(0xE6); B(0x0E);                       // and si, 14
    B(0x8E); B(0x84); W(SA(segtab));                 // mov es, [si + segtab]
    land8(noseg);
    B(0x4F);                                         // dec di
    uint8_t *end = jcc8(0x74);                       // jz end
    B(0xE9); W((uint16_t) (loop - (o + 2)));         // jmp loop
    land8(end);
    uint8_t *flags = o;
    // AH = the last byte: the caller makes the flags of the last OUTI from
    // it when they are live (run_flags), ~a third of the runs' fixed cost
    // in Aladdin's VRAM fill loop, whose flags are dead (OR A after)
    B(0x83); B(0xC4); B(0x04);                       // add sp, 4
    B(0xF8);                                         // clc
    B(0xC3);                                         // ret
#ifdef GGSWAN_HOST
    if (s1 && s1 - slow1 > 128) { extern void abort(void); abort(); }
#endif
    if (to_fast) {
        /*
         * FAST: the whole run to VRAM at once (rep movsb) when it is tile
         * data: not CRAM, no byte in the name table, no wrap at the end of
         * VRAM, the source inside one 8 KiB page. The same results as the
         * loop: VRAM, read buffer, address, latch, sprites to rebuild if it
         * touches the sprite table, HL, B, the flags of the last OUTI.
         */
        uint16_t fast_at = (uint16_t) (o - o_start);
        uint16_t rel = (uint16_t) (fast_at - (uint16_t) (to_fast + 2 - o_start));
        to_fast[0] = (uint8_t) rel;
        to_fast[1] = (uint8_t) (rel >> 8);
        B(0x80); B(0x3E); W(VA(code)); B(3);         // cmp byte [code], 3
        uint8_t *cram = 0;
        uint8_t *not_cram = jcc8(0x75);              // jne (VRAM)
#ifdef GGSWAN_SMS
        if (0) {   // (the Master System's CRAM, one byte a color: the loop)
#else
        if (!dec) {
#endif
            /*
             * CRAM (palette uploads, ~60 bytes a frame in Super Monaco GP):
             * the bytes here instead of one CRAM routine call each through
             * the loop. As put_cram (drc_rt.c): the even byte latched, the
             * odd one writes the color and its WS copy; read buffer,
             * address, latch, HL, B; AH = the last byte. The source inside
             * one 8 KiB page, else the loop.
             */
            B(0x52);                                 // push dx
            B(0x89); B(0xDA);                        // mov dx, bx
            B(0x81); B(0xE2); W(0x1FFF);             // and dx, 1FFFh
            B(0x01); B(0xFA);                        // add dx, di
            B(0x81); B(0xFA); W(0x2000);             // cmp dx, 2000h
            uint8_t *cr_ok = jcc8(0x76);             // jbe ok
            B(0x5A);                                 // pop dx
            B(0xE9); W((uint16_t) (loop - (o + 2))); // jmp loop
            land8(cr_ok);
            B(0x89); B(0xFA);                        // mov dx, di (k)
            B(0x28); B(0xD5);                        // sub ch, dl (B -= k)
            B(0x50);                                 // push ax (A)
            B(0x8B); B(0x16); W(VA(addr));           // mov dx, [addr]
            uint8_t *cr_loop = o;
            B(0x26); B(0x8A); B(0x07);               // mov al, es:[bx]
            B(0x43);                                 // inc bx
            B(0xA2); W(VA(read_buffer));             // mov [read_buffer], al
            B(0xF6); B(0xC2); B(0x01);               // test dl, 1
            uint8_t *cr_odd = jcc8(0x75);            // jnz odd
            B(0xA2); W(VA(cram_latch));              // mov [cram_latch], al
            uint8_t *cr_next = jcc8(0xEB);           // jmp next
            land8(cr_odd);
            B(0x24); B(0x0F);                        // and al, 0Fh
            B(0x89); B(0xD6);                        // mov si, dx
            B(0x83); B(0xE6); B(0x3F);               // and si, 3Fh
            B(0x88); B(0x84); W(VA(cram));           // mov [si + cram], al
            B(0x8A); B(0x26); W(VA(cram_latch));     // mov ah, [cram_latch]
            B(0x88); B(0xA4); W(VA(cram) - 1);       // mov [si + cram - 1], ah
            B(0x53);                                 // push bx
            B(0x88); B(0xE3);                        // mov bl, ah
            B(0x80); B(0xE3); B(0xF0);               // and bl, F0h (G)
            B(0x08); B(0xD8);                        // or al, bl (G B)
            B(0x80); B(0xE4); B(0x0F);               // and ah, 0Fh (R)
            B(0x5B);                                 // pop bx
            B(0x83); B(0xE6); B(0x3E);               // and si, 3Eh (color * 2)
#ifndef GGSWAN_SG1000
            // (not to the WS's palettes: with the frame, main.c pal_present)
            B(0x89); B(0x84); W(SA(pal));            // mov [si + pal], ax
            B(0xC6); B(0x06); W(SA(pal_dirty)); B(1);   // mov byte [pal_dirty], 1
#else
            // (sgswan: no color RAM, never run)
            B(0x89); B(0x84); W(0xFE00);             // mov [si + FE00h], ax
            B(0x83); B(0xFE); B(0x20);               // cmp si, 20h
            B(0x72); B(0x04);                        // jb +4
            B(0x89); B(0x84); W(0xFEE0);             // mov [si + FEE0h], ax
#endif
            land8(cr_next);
            B(0x42);                                 // inc dx
            B(0x81); B(0xE2); W(0x3FFF);             // and dx, 3FFFh
            B(0x4F);                                 // dec di
            B(0x75); B((uint8_t) (cr_loop - (o + 1)));   // jnz loop
            B(0x89); B(0x16); W(VA(addr));           // mov [addr], dx
            B(0xC6); B(0x06); W(VA(latch_full)); B(0);   // mov byte [latch_full], 0
            B(0x58);                                 // pop ax (A)
            B(0x8A); B(0x26); W(VA(read_buffer));    // mov ah, [read_buffer] (the last byte)
            B(0x5A);                                 // pop dx
            B(0x83); B(0xC4); B(0x04);               // add sp, 4
            B(0xF8);                                 // clc
            B(0xC3);                                 // ret
        } else {
            cram = jcc8(0xEB);                       // (OUTD: the loop)
        }
        land8(not_cram);
        B(0x52);                                     // push dx
        B(0x8B); B(0x36); W(VA(addr));               // mov si, [addr]
        B(0x81); B(0xE6); W(0x3FFF);                 // and si, 3FFFh (start)
        B(0x89); B(0xDA);                            // mov dx, bx
        B(0x81); B(0xE2); W(0x1FFF);                 // and dx, 1FFFh
        B(0x01); B(0xFA);                            // add dx, di
        B(0x81); B(0xFA); W(0x2000);                 // cmp dx, 2000h
        uint8_t *n2 = jcc8(0x77);                    // ja nofast (next page)
        // the classes of the first and last VRAM page (64: it wraps)
        B(0x57);                                     // push di (k)
        B(0x01); B(0xF7);                            // add di, si
        B(0x4F);                                     // dec di (last byte)
        B(0xC1); B(0xEF); B(0x08);                   // shr di, 8
        B(0x8A); B(0x95); W(SA(vram_class));         // mov dl, [di + class]
        B(0x89); B(0xF7);                            // mov di, si
        B(0xC1); B(0xEF); B(0x08);                   // shr di, 8
        B(0x0A); B(0x95); W(SA(vram_class));         // or dl, [di + class]
        B(0x5F);                                     // pop di
        uint8_t *n1, *n3 = 0, *n4 = 0, *logged = 0, *plain = 0;
        uint8_t *lbig = 0, *nbig = 0, *ntail = 0;   // (the long runs: at the end)
        if (e->cls_off) {
            /*
             * Deferred writes (class bit 3, main.c): the run goes to the
             * log instead of VRAM (the same results otherwise). Wrapping
             * at the end of VRAM, also touching the sprite table, or no
             * room in the log: the loop, byte by byte (the class routine).
             */
            B(0xF6); B(0xC2); B(0x10);               // test dl, 10h
            n1 = jcc8(0x75);                         // jnz nofast (wrap)
            B(0xF6); B(0xC2); B(0x08);               // test dl, 8
            uint8_t *live = jcc8(0x74);              // jz live
            B(0xF6); B(0xC2); B(0x03);               // test dl, 3
            n3 = jcc8(0x75);                         // jnz nofast (sprite table,
                                                     // or a run from tile data into
                                                     // the name table: the log copy
                                                     // does not update the map)
            B(0x89); B(0xFA);                        // mov dx, di (k)
            B(0x03); B(0x16); W(SA(log_top));        // add dx, [log_top]
            B(0x83); B(0xC2); B(0x03);               // add dx, 3
            B(0x3B); B(0x16); W(SA(log_end));        // cmp dx, [log_end]
            uint8_t *dolog = jcc8(0x76);             // jbe dolog
            land8(n1); land8(n2); land8(n3);
            uint8_t *nofast = o;
            B(0x5A);                                 // nofast: pop dx
            land8(cram);
            B(0xE9); W((uint16_t) (loop - (o + 2)));     // jmp loop
            land8(dolog);
            B(0x57);                                 // push di (k)
            B(0x8B); B(0x3E); W(SA(log_top));        // mov di, [log_top]
            B(0x89); B(0x16); W(SA(log_top));        // mov [log_top], dx
            B(0x89); B(0x3E); W(SA(log_last));       // mov [log_last], di (this entry)
            B(0xC7); B(0x06); W(SA(log_next)); W(0xFFFF);   // mov word [log_next], FFFFh
            B(0x89); B(0x35);                        // mov [di], si (VRAM address)
            B(0x5A);                                 // pop dx (k)
            B(0x88); B(0x55); B(0x02);               // mov [di + 2], dl (length)
            B(0x83); B(0xC7); B(0x03);               // add di, 3
            B(0x28); B(0xD5);                        // sub ch, dl (B -= k)
            // a short run (Shinobi's OUTI / OUTI): a byte loop, cheaper
            // than setting up REP MOVSB with DS and ES swapped
            B(0x80); B(0xFA); B(OUTI_SHORT);         // cmp dl, OUTI_SHORT
            B(0x76); B(3);                           // jbe small
            B(0xE9); W(0); lbig = o - 2;             // jmp big (at the end)
            B(0x01); B(0xD6);                        // add si, dx (VRAM end)
            B(0x81); B(0xE6); W(0x3FFF);             // and si, 3FFFh
            B(0x89); B(0x36); W(VA(addr));           // mov [addr], si
            uint8_t *lsl = o;
            B(0x26); B(0x8A); B(0x27);               // L: mov ah, es:[bx]
            B(0x43);                                 // inc bx
            B(0x88); B(0x25);                        // mov [di], ah
            B(0x47);                                 // inc di
            B(0xFE); B(0xCA);                        // dec dl
            B(0x75); B((uint8_t) (lsl - (o + 1)));   // jnz L
            B(0x88); B(0x26); W(VA(read_buffer));    // mov [read_buffer], ah
            B(0xC6); B(0x06); W(VA(latch_full)); B(0);   // mov byte [latch_full], 0
            B(0x5A);                                 // pop dx
            B(0xE9); W((uint16_t) (flags - (o + 2)));    // jmp flags
            land8(live);
            B(0xF6); B(0xC2); B(0x03);               // test dl, 3 (name or sprite table)
            B(0x75); B(3);                           // jnz +3
            B(0xE9); W(0);                           // jmp plain (tile data, not deferred)
            plain = o - 2;
            B(0xF6); B(0xC2); B(0x01);               // test dl, 1 (name table)
            B(0x75); B(3);                           // jnz +3
            B(0xE9); W(0);                           // jmp not_nt (sprite table only)
            uint8_t *not_nt = o - 2;
            B(0xF6); B(0xC2); B(0x02);               // test dl, 2 (both)
#ifdef GGSWAN_HOST
            if (o + 2 - nofast > 128) { extern void abort(void); abort(); }   // (short jump too far)
#endif
            B(0x75); B((uint8_t) (nofast - (o + 1)));    // jnz nofast (backwards)
            /*
             * Name table (live): the run to VRAM at once, then the name
             * table routine once for each word it touched (it converts the
             * whole word), instead of the loop's class and name table
             * routines for every byte. Sonic writes its map entries as
             * runs of 2 OUTIs, ~50 a frame.
             */
            /*
             * First: the bytes already there? Then nothing to store or
             * convert (the screen map is the one of VRAM as it is). Games
             * send whole parts of the name table again on every frame:
             * Super Monaco GP II's screen before the race some 380 bytes
             * a frame, all the same, the name table routine once for
             * every two of them.
             */
            B(0x56);                                 // push si
            B(0x57);                                 // push di
            B(0x51);                                 // push cx
            B(0x89); B(0xF9);                        // mov cx, di (k)
            B(0x8D); B(0xBC); W(env->vram_addr);     // lea di, [si + vram]
            B(0x89); B(0xDE);                        // mov si, bx
            B(0x1E); B(0x06); B(0x1F); B(0x07);      // push ds; push es; pop ds; pop es
            B(0xFC);                                 // cld
            B(0xF3); B(0xA6);                        // repe cmpsb
            B(0x8A); B(0x64); B(0xFF);               // mov ah, [si - 1] (the last one compared)
            B(0x06); B(0x1F);                        // push es; pop ds (DS, ES: data)
            B(0x59);                                 // pop cx
            B(0x5F);                                 // pop di
            B(0x5E);                                 // pop si
            uint8_t *differ = jcc8(0x75);            // jne differ
            B(0x01); B(0xFB);                        // add bx, di (HL + k)
            B(0x89); B(0xFA);                        // mov dx, di
            B(0x28); B(0xD5);                        // sub ch, dl (B -= k)
            B(0x01); B(0xFE);                        // add si, di
            B(0x81); B(0xE6); W(0x3FFF);             // and si, 3FFFh
            B(0x89); B(0x36); W(VA(addr));           // mov [addr], si
            B(0x88); B(0x26); W(VA(read_buffer));    // mov [read_buffer], ah
            B(0xC6); B(0x06); W(VA(latch_full)); B(0);   // mov byte [latch_full], 0
            B(0x5A);                                 // pop dx
            B(0xE9); W((uint16_t) (flags - (o + 2)));    // jmp flags
            land8(differ);
            B(0x56);                                 // push si (ES: the source again)
            B(0x89); B(0xDE);                        // mov si, bx
            B(0xC1); B(0xC6); B(0x04);               // rol si, 4
            B(0x83); B(0xE6); B(0x0E);               // and si, 14
            B(0x8E); B(0x84); W(SA(segtab));         // mov es, [si + segtab]
            B(0x5E);                                 // pop si
            B(0x89); B(0xFA);                        // mov dx, di (k)
            B(0x28); B(0xD5);                        // sub ch, dl (B -= k)
            B(0x51);                                 // push cx
            B(0x56);                                 // push si (VRAM start)
            // a short run (Sonic's map entries): a byte loop
            B(0x83); B(0xFF); B(OUTI_SHORT);         // cmp di, OUTI_SHORT
            B(0x76); B(3);                           // jbe small
            B(0xE9); W(0); nbig = o - 2;             // jmp big (at the end)
            uint8_t *nsl = o;
            B(0x26); B(0x8A); B(0x27);               // L: mov ah, es:[bx]
            B(0x43);                                 // inc bx
            B(0x88); B(0xA4); W(env->vram_addr);     // mov [si + vram], ah
            B(0x46);                                 // inc si
            B(0x4F);                                 // dec di
            B(0x75); B((uint8_t) (nsl - (o + 1)));   // jnz L
            B(0x88); B(0x26); W(VA(read_buffer));    // mov [read_buffer], ah
            B(0xC6); B(0x06); W(VA(latch_full)); B(0);   // mov byte [latch_full], 0
            B(0x81); B(0xE6); W(0x3FFF);             // and si, 3FFFh
            B(0x89); B(0x36); W(VA(addr));           // mov [addr], si
            ntail = o;
            B(0x5E);                                 // tail: pop si (VRAM start)
            B(0x01); B(0xF2);                        // add dx, si (end)
            B(0x83); B(0xE6); B(0xFE);               // and si, FFFEh (its word)
            uint8_t *ntl = o;
            call_near(e->nt_stub_off);               // the word at SI to the map
            B(0x83); B(0xC6); B(0x02);               // add si, 2
            B(0x39); B(0xD6);                        // cmp si, dx
            B(0x72); B((uint8_t) (ntl - (o + 1)));   // jb ntl
            B(0x59);                                 // pop cx
            B(0x5A);                                 // pop dx
            B(0xE9); W((uint16_t) (flags - (o + 2)));    // jmp flags
            { uint16_t r_ = (uint16_t) (o - (not_nt + 2)); not_nt[0] = (uint8_t) r_; not_nt[1] = (uint8_t) (r_ >> 8); }
            (void) n4; (void) logged;
        } else {
            B(0xF6); B(0xC2); B(0x01);               // test dl, 1
            n1 = jcc8(0x75);                         // jnz nofast (name table, wrap)
            uint8_t *go = jcc8(0xEB);                // jmp go
            land8(n1); land8(n2);
            B(0x5A);                                 // nofast: pop dx
            land8(cram);
            B(0xE9); W((uint16_t) (loop - (o + 2)));     // jmp loop
            land8(go);
        }
        // sprite table touched: rebuild the sprites
        B(0xF6); B(0xC2); B(0x02);                   // test dl, 2
        uint8_t *clean = jcc8(0x74);                 // jz clean
        B(0xC6); B(0x06); W(SA(sprites_dirty)); B(1);   // sprites_dirty = 1
        land8(clean);
        if (plain) { uint16_t r_ = (uint16_t) (o - (plain + 2)); plain[0] = (uint8_t) r_; plain[1] = (uint8_t) (r_ >> 8); }
        B(0x89); B(0xFA);                            // mov dx, di (k)
        B(0x28); B(0xD5);                            // sub ch, dl (B -= k)
        // a short run (Super Monaco GP's sprite table): a byte loop
        B(0x83); B(0xFF); B(OUTI_SHORT);             // cmp di, OUTI_SHORT
        uint8_t *pbig = jcc8(0x77);                  // ja big
        uint8_t *psl = o;
        B(0x26); B(0x8A); B(0x27);                   // L: mov ah, es:[bx]
        B(0x43);                                     // inc bx
        B(0x88); B(0xA4); W(env->vram_addr);         // mov [si + vram], ah
        B(0x46);                                     // inc si
        B(0x4F);                                     // dec di
        B(0x75); B((uint8_t) (psl - (o + 1)));       // jnz L
        B(0x88); B(0x26); W(VA(read_buffer));        // mov [read_buffer], ah
        B(0xC6); B(0x06); W(VA(latch_full)); B(0);   // mov byte [latch_full], 0
        B(0x81); B(0xE6); W(0x3FFF);                 // and si, 3FFFh
        B(0x89); B(0x36); W(VA(addr));               // mov [addr], si
        B(0x5A);                                     // pop dx
        B(0xE9); W((uint16_t) (flags - (o + 2)));    // jmp flags
        land8(pbig);
        B(0x51);                                     // big: push cx
        B(0x89); B(0xF9);                            // mov cx, di (k)
        B(0x8D); B(0xBC); W(env->vram_addr);         // lea di, [si + vram]
        B(0x89); B(0xDE);                            // mov si, bx
        B(0x1E); B(0x06); B(0x1F); B(0x07);          // push ds; push es; pop ds; pop es
        B(0xFC);                                     // cld (forwards, whatever came before)
        B(0xF3); B(0xA4);                            // rep movsb
        B(0x06); B(0x1F);                            // push es; pop ds (DS, ES: data)
        B(0x59);                                     // pop cx
        B(0x5A);                                     // pop dx
        B(0x89); B(0xF3);                            // mov bx, si (HL + k)
        B(0x8A); B(0x65); B(0xFF);                   // mov ah, [di - 1] (last byte)
        B(0x88); B(0x26); W(VA(read_buffer));        // mov [read_buffer], ah
        B(0xC6); B(0x06); W(VA(latch_full)); B(0);   // mov byte [latch_full], 0
        if (env->vram_addr & 0x3FFF) { B(0x81); B(0xEF); W(env->vram_addr); }   // sub di, vram
        B(0x81); B(0xE7); W(0x3FFF);                 // and di, 3FFFh (vram: a multiple of 4000h)
        B(0x89); B(0x3E); W(VA(addr));               // mov [addr], di
        B(0xE9); W((uint16_t) (flags - (o + 2)));    // jmp flags
        if (lbig) {
            // a long run to the log: REP MOVSB
            { uint16_t r_ = (uint16_t) (o - (lbig + 2)); lbig[0] = (uint8_t) r_; lbig[1] = (uint8_t) (r_ >> 8); }
            B(0x51);                                         // big: push cx
            B(0x89); B(0xD1);                                // mov cx, dx (k)
            B(0x56);                                         // push si (VRAM start)
            B(0x89); B(0xDE);                                // mov si, bx
            B(0x1E); B(0x06); B(0x1F); B(0x07);              // push ds; push es; pop ds; pop es
            B(0xFC);                                         // cld
            B(0xF3); B(0xA4);                                // rep movsb
            B(0x06); B(0x1F);                                // push es; pop ds (DS, ES: data)
            B(0x8A); B(0x65); B(0xFF);                       // mov ah, [di - 1] (last byte)
            B(0x88); B(0x26); W(VA(read_buffer));            // mov [read_buffer], ah
            B(0xC6); B(0x06); W(VA(latch_full)); B(0);       // mov byte [latch_full], 0
            B(0x5F);                                         // pop di (VRAM start)
            B(0x01); B(0xF7);                                // add di, si
            B(0x29); B(0xDF);                                // sub di, bx (+ k)
            B(0x81); B(0xE7); W(0x3FFF);                     // and di, 3FFFh
            B(0x89); B(0x3E); W(VA(addr));                   // mov [addr], di
            B(0x89); B(0xF3);                                // mov bx, si (HL + k)
            B(0x59);                                         // pop cx
            B(0x5A);                                         // pop dx
            B(0xE9); W((uint16_t) (flags - (o + 2)));        // jmp flags
        }
        if (nbig) {
            // a long run to the name table: REP MOVSB, then the tail
            { uint16_t r_ = (uint16_t) (o - (nbig + 2)); nbig[0] = (uint8_t) r_; nbig[1] = (uint8_t) (r_ >> 8); }
            B(0x89); B(0xF9);                                // big: mov cx, di (k)
            B(0x8D); B(0xBC); W(env->vram_addr);             // lea di, [si + vram]
            B(0x89); B(0xDE);                                // mov si, bx
            B(0x1E); B(0x06); B(0x1F); B(0x07);              // push ds; push es; pop ds; pop es
            B(0xFC);                                         // cld
            B(0xF3); B(0xA4);                                // rep movsb
            B(0x06); B(0x1F);                                // push es; pop ds (DS, ES: data)
            B(0x89); B(0xF3);                                // mov bx, si (HL + k)
            B(0x8A); B(0x65); B(0xFF);                       // mov ah, [di - 1] (last byte)
            B(0x88); B(0x26); W(VA(read_buffer));            // mov [read_buffer], ah
            B(0xC6); B(0x06); W(VA(latch_full)); B(0);       // mov byte [latch_full], 0
            if (env->vram_addr & 0x3FFF) { B(0x81); B(0xEF); W(env->vram_addr); }   // sub di, vram
            B(0x81); B(0xE7); W(0x3FFF);                     // and di, 3FFFh
            B(0x89); B(0x3E); W(VA(addr));                   // mov [addr], di
            B(0xE9); W((uint16_t) (ntail - (o + 2)));        // jmp tail
        }
    }
    if (to_ctl) {
        /*
         * CTL: to the VDP control port (Sailor Moon sends VRAM addresses
         * with OUTI / OUTI to BDh between its data pairs, ~7 rows a frame,
         * each a helper of ~2 WS lines): byte after byte through the OUT
         * (BFh),A routine; a byte it does not take (a register write C
         * must do) leaves the rest to the interpreter as the loop does.
         */
        uint16_t r_ = (uint16_t) (o - (to_ctl + 2));
        to_ctl[0] = (uint8_t) r_; to_ctl[1] = (uint8_t) (r_ >> 8);
        B(0x81); B(0xFE); W(0x0081);                 // cmp si, 81h (port BDh, BFh)
        B(0x74); B(3);                               // je +3
        B(0xE9); W((uint16_t) (slow0 - (o + 2)));    // jmp slow
        uint8_t *cl = o;
        B(0x57);                                     // L: push di
        B(0x50);                                     // push ax
        B(0x26); B(0x8A); B(0x07);                   // mov al, es:[bx]
        call_near(env->ctl_off);
        B(0xA2); W(SA(val));                         // mov [val], al (the last byte)
        B(0x58);                                     // pop ax
        B(0x73); B(3);                               // jnc +3
        B(0xE9); W((uint16_t) (slow1 - (o + 2)));    // jmp slow1 (this one and the rest)
        B(0x5F);                                     // pop di
        B(0xFE); B(0xCD);                            // dec ch
        B(0x43);                                     // inc bx
        B(0xF7); B(0xC3); W(0x1FFF);                 // test bx, 1FFFh
        uint8_t *nseg = jcc8(0x75);                  // jnz: same ES
        B(0x89); B(0xDE);                            // mov si, bx
        B(0xC1); B(0xC6); B(0x04);                   // rol si, 4
        B(0x83); B(0xE6); B(0x0E);                   // and si, 14
        B(0x8E); B(0x84); W(SA(segtab));             // mov es, [si + segtab]
        land8(nseg);
        B(0x4F);                                     // dec di
        B(0x75); B((uint8_t) (cl - (o + 1)));        // jnz L
        B(0x8A); B(0x26); W(SA(val));                // mov ah, [val]
        B(0xE9); W((uint16_t) (flags - (o + 2)));    // jmp flags
    }
    if (one) {
        /*
         * The entry for an OUTI alone (DI = 1): to port BEh straight to
         * the loop, a byte through the single write's code, which stores
         * it or logs it (added to the log's open entry) or calls the
         * class routine; the path above sets up a run first, classes of
         * its first and last page and a log entry of its own, and for
         * the name table converts the entry whatever it holds: about 130
         * instructions for the byte. Super Monaco GP II draws the screen
         * before the race with OUTIs one at a time, some 380 a frame.
         */
        *one = (uint16_t) (o_base + (o - o_start));
        B(0x56);                                     // push si (address)
        B(0x57);                                     // push di (k)
        B(0x89); B(0xDE);                            // mov si, bx
        B(0xC1); B(0xC6); B(0x04);                   // rol si, 4
        B(0x83); B(0xE6); B(0x0E);                   // and si, 14
        B(0x8E); B(0x84); W(SA(segtab));             // mov es, [si + segtab]
        B(0x89); B(0xCE);                            // mov si, cx
        B(0x81); B(0xE6); W(0x00C1);                 // and si, C1h
        B(0x81); B(0xFE); W(0x0080);                 // cmp si, 80h (port BEh)
        B(0x75); B(3);                               // jne +3
        B(0xE9); W((uint16_t) (loop - (o + 2)));     // jmp loop
        B(0xE9); W((uint16_t) (after_cmp - (o + 2)));    // jmp: the other ports, as above
    }
    return (uint16_t) (o - o_start);
}

/*
 * Routine for g groups of OUTI / OUTI / DEC L x 4 (Shinobi uploads its
 * tiles with that, unrolled: each pair of bytes from 2 below the one
 * before, to the next 2 VRAM bytes). In: DI = g, BX = HL, CX = BC. When
 * it is a plain VRAM upload (port BEh, not CRAM, L from 2 (g - 1) to FDh
 * so that no step crosses into H, a VRAM run of 2 g bytes in tile data,
 * the sprite table or the log, no wrap, room in the log): the bytes go
 * there in one go, as the OUTI routine would put them, with VRAM
 * address, read buffer, latch, B -= 2 g, L -= 2 g (H as it was), AH =
 * the last byte; returns CF = 0. Anything else: nothing changed, CF = 1
 * (the block runs the instructions one by one). The flags are left
 * undefined: the block calls it only where the next OUTI sets them all.
 */
uint16_t drc_emit_pairs_stub(const drc_env_t *e, uint8_t *out, uint16_t out_off) {
    env = e;
    o = o_start = out; o_base = out_off;
    // (fail first, so that the jumps to it go backwards; the entry is at
    // + DRC_PAIRS_ENTRY)
    uint8_t *fail = o;
    B(0x5A);                                         // fail: pop dx
    B(0xF9);                                         // stc
    B(0xC3);                                         // ret
#ifdef GGSWAN_HOST
#define FAIL(op) do { if (o + 2 - fail > 128) { extern void abort(void); abort(); } \
        B(op); B((uint8_t) (fail - (o + 1))); } while (0)
#else
#define FAIL(op) do { B(op); B((uint8_t) (fail - (o + 1))); } while (0)
#endif
    B(0x52);                                         // push dx
    B(0x89); B(0xCE);                                // mov si, cx
    B(0x81); B(0xE6); W(0x00C1);                     // and si, C1h
    B(0x81); B(0xFE); W(0x0080);                     // cmp si, 80h (port BEh)
    FAIL(0x75);                                      // jne fail
    B(0x80); B(0x3E); W(VA(code)); B(3);             // cmp byte [code], 3
    FAIL(0x74);                                      // je fail (CRAM)
    B(0x80); B(0xFB); B(0xFD);                       // cmp bl, FDh
    FAIL(0x77);                                      // ja fail
    B(0x89); B(0xFA);                                // mov dx, di
    B(0x4A);                                         // dec dx
    B(0xD1); B(0xE2);                                // shl dx, 1 (2 (g - 1))
    B(0x38); B(0xD3);                                // cmp bl, dl
    FAIL(0x72);                                      // jb fail
    B(0x89); B(0xDE);                                // mov si, bx
    B(0xC1); B(0xC6); B(0x04);                       // rol si, 4
    B(0x83); B(0xE6); B(0x0E);                       // and si, 14
    B(0x8E); B(0x84); W(SA(segtab));                 // mov es, [si + segtab]
    B(0x8B); B(0x36); W(VA(addr));                   // mov si, [addr]
    B(0x81); B(0xE6); W(0x3FFF);                     // and si, 3FFFh (start)
    // the classes of the first and last VRAM page (64: it wraps)
    B(0x57);                                         // push di (g)
    B(0xD1); B(0xE7);                                // shl di, 1 (2 g)
    B(0x01); B(0xF7);                                // add di, si
    B(0x4F);                                         // dec di (last byte)
    B(0xC1); B(0xEF); B(0x08);                       // shr di, 8
    B(0x8A); B(0x95); W(SA(vram_class));             // mov dl, [di + class]
    B(0x89); B(0xF7);                                // mov di, si
    B(0xC1); B(0xEF); B(0x08);                       // shr di, 8
    B(0x0A); B(0x95); W(SA(vram_class));             // or dl, [di + class]
    B(0x5F);                                         // pop di
    B(0xF6); B(0xC2); B(0x11);                       // test dl, 11h (wrap, name table)
    FAIL(0x75);                                      // jnz fail
    B(0xF6); B(0xC2); B(0x08);                       // test dl, 8
    uint8_t *plain = jcc8(0x74);                     // jz plain
    B(0xF6); B(0xC2); B(0x02);                       // test dl, 2
    FAIL(0x75);                                      // jnz fail (sprite table and log)
    // to the log: an entry of VRAM address, length, the bytes
    B(0x89); B(0xFA);                                // mov dx, di
    B(0xD1); B(0xE2);                                // shl dx, 1 (2 g)
    B(0x03); B(0x16); W(SA(log_top));                // add dx, [log_top]
    B(0x83); B(0xC2); B(0x03);                       // add dx, 3
    B(0x3B); B(0x16); W(SA(log_end));                // cmp dx, [log_end]
    FAIL(0x77);                                      // ja fail (no room)
    B(0x57);                                         // push di (g)
    B(0x8B); B(0x3E); W(SA(log_top));                // mov di, [log_top]
    B(0x89); B(0x16); W(SA(log_top));                // mov [log_top], dx
    B(0x89); B(0x3E); W(SA(log_last));               // mov [log_last], di (this entry)
    B(0xC7); B(0x06); W(SA(log_next)); W(0xFFFF);    // mov word [log_next], FFFFh
    B(0x89); B(0x35);                                // mov [di], si (VRAM address)
    B(0x5A);                                         // pop dx (g)
    B(0x88); B(0xD4);                                // mov ah, dl
    B(0xD0); B(0xE4);                                // shl ah, 1
    B(0x88); B(0x65); B(0x02);                       // mov [di + 2], ah (length)
    B(0x83); B(0xC7); B(0x03);                       // add di, 3
    uint8_t *copy = jcc8(0xEB);                      // jmp copy
    land8(plain);
    B(0xF6); B(0xC2); B(0x02);                       // test dl, 2
    uint8_t *clean = jcc8(0x74);                     // jz clean
    B(0xC6); B(0x06); W(SA(sprites_dirty)); B(1);    // sprites_dirty = 1
    land8(clean);
    B(0x89); B(0xFA);                                // mov dx, di (g)
    B(0x8D); B(0xBC); W(env->vram_addr);             // lea di, [si + vram]
    land8(copy);
    // DI: where the bytes go, DX = g, SI = VRAM address
    B(0x01); B(0xD6);                                // add si, dx
    B(0x01); B(0xD6);                                // add si, dx
    B(0x81); B(0xE6); W(0x3FFF);                     // and si, 3FFFh
    B(0x89); B(0x36); W(VA(addr));                   // mov [addr], si
    B(0x28); B(0xD5);                                // sub ch, dl
    B(0x28); B(0xD5);                                // sub ch, dl (B -= 2 g)
    uint8_t *loop = o;
    B(0x26); B(0x8A); B(0x27);                       // L: mov ah, es:[bx]
    B(0x88); B(0x25);                                // mov [di], ah
    B(0x26); B(0x8A); B(0x67); B(0x01);              // mov ah, es:[bx + 1]
    B(0x88); B(0x65); B(0x01);                       // mov [di + 1], ah
    B(0x83); B(0xC7); B(0x02);                       // add di, 2
    B(0x80); B(0xEB); B(0x02);                       // sub bl, 2 (+ 2 - 4: L only)
    B(0x4A);                                         // dec dx
    B(0x75); B((uint8_t) (loop - (o + 1)));          // jnz L
    B(0x88); B(0x26); W(VA(read_buffer));            // mov [read_buffer], ah
    B(0xC6); B(0x06); W(VA(latch_full)); B(0);       // mov byte [latch_full], 0
    B(0x5A);                                         // pop dx
    B(0xF8);                                         // clc
    B(0xC3);                                         // ret
#undef FAIL
    return (uint16_t) (o - o_start);
}

/*
 * Routine for a run of LDI (dec: LDD) copying to work RAM. In: DI = k,
 * SI = Z80 address of the run, AH = F. Returns CF = 0 with the flags of
 * the last LDI in AH; a destination outside work RAM: CF = 1 as for
 * OUTI (the LDIs left for the STEP helper). Uses SI, DI, ES.
 */
/*
 * sfswan's: everything is RAM, so the whole run at once wherever it goes,
 * unless a 16 bytes it would write have code in them (the map of
 * flat_write) or it runs past the end of the memory: then none of it,
 * CF = 1, and the STEP helper does it all, looking at every byte. LDI:
 * REP MOVSB with DS = ES for the copy; LDD: byte by byte (the direction
 * flag is never set here: an interrupt's C code would find it).
 */
static uint16_t flat_ldi_stub(uint8_t dec) {
    B(0x56); B(0x57); B(0x50);                       // push si (address); push di (k); push ax
    B(0x89); B(0xD6);                                // mov si, dx
    B(0x89); B(0xD8);                                // mov ax, bx
    uint8_t *w0, *w1, *bad, *loop, *flags_j = 0;
    if (!dec) {
        B(0x01); B(0xFE); w0 = jcc8(0x72);           // add si, di; jc slow (past FFFFh)
        B(0x01); B(0xF8); w1 = jcc8(0x72);           // add ax, di; jc slow
        B(0x4E);                                     // dec si (the last byte written)
        B(0xC1); B(0xEE); B(0x04);                   // shr si, 4
        B(0x89); B(0xD0);                            // mov ax, dx
        B(0xC1); B(0xE8); B(0x04);                   // shr ax, 4
        B(0x29); B(0xC6);                            // sub si, ax
        B(0x96);                                     // xchg ax, si (SI: the first 16, AX: how many more)
    } else {
        B(0x29); B(0xFE); w0 = jcc8(0x72);           // sub si, di; jc slow (below 0000h)
        B(0x29); B(0xF8); w1 = jcc8(0x72);           // sub ax, di; jc slow
        B(0x46);                                     // inc si (the lowest byte written)
        B(0xC1); B(0xEE); B(0x04);                   // shr si, 4
        B(0x89); B(0xD0);                            // mov ax, dx
        B(0xC1); B(0xE8); B(0x04);                   // shr ax, 4
        B(0x29); B(0xF0);                            // sub ax, si
    }
    loop = o;
    B(0x80); B(0xBC); W(FENV(cmap)); B(0);            // cmp byte [si + cmap], 0
    bad = jcc8(0x75);                                // jne slow (code there)
    B(0x46);                                         // inc si
    B(0x48);                                         // dec ax
    B(0x79); B((uint8_t) (loop - (o + 1)));          // jns loop
    if (!dec) {
        B(0x51);                                     // push cx
        B(0x89); B(0xF9);                            // mov cx, di (k)
        B(0x89); B(0xF8);                            // mov ax, di
        B(0x89); B(0xDE);                            // mov si, bx
        B(0x89); B(0xD7);                            // mov di, dx
        B(0x1E); B(0x06); B(0x1F);                   // push ds; push es; pop ds
        B(0xFC);                                     // cld
        B(0xF3); B(0xA4);                            // rep movsb
        B(0x1F);                                     // pop ds
        B(0x89); B(0xF3);                            // mov bx, si (HL + k)
        B(0x89); B(0xFA);                            // mov dx, di (DE + k)
        B(0x59);                                     // pop cx
        B(0x29); B(0xC1);                            // sub cx, ax (BC - k)
    } else {
        uint8_t *bl = o;
        B(0x26); B(0x8A); B(0x07);                   // mov al, es:[bx]
        B(0x89); B(0xD6);                            // mov si, dx
        B(0x26); B(0x88); B(0x04);                   // mov es:[si], al
        B(0x4B); B(0x4A); B(0x49);                   // dec bx; dec dx; dec cx
        B(0x4F);                                     // dec di
        B(0x75); B((uint8_t) (bl - (o + 1)));        // jnz
    }
    (void) flags_j;
    // flags: S Z C kept, H = N = 0, P/V = BC != 0
    B(0x58);                                         // pop ax
    B(0x80); B(0xE4); B(0xC1);                       // and ah, C1h
    B(0x85); B(0xC9);                                // test cx, cx
    B(0x74); B(0x03); B(0x80); B(0xCC); B(0x04);     // jz +3; or ah, 4
    B(0x83); B(0xC4); B(0x04);                       // add sp, 4
    B(0xF8);                                         // clc
    B(0xC3);                                         // ret
    land8(w0); land8(w1); land8(bad);
    B(0x58);                                         // pop ax
    B(0x89); B(0x3E); W(SA(val));                    // mov [val], di (all of them left)
    B(0x5E);                                         // pop si (k)
    B(0x29); B(0xFE);                                // sub si, di
    B(0xD1); B(0xE6);                                // shl si, 1
    B(0x89); B(0x36); W(SA(arg));                    // mov [arg], si
    B(0x5E);                                         // pop si (address)
    B(0x01); B(0x36); W(SA(arg));                    // add [arg], si
    B(0x89); B(0xFE);                                // mov si, di
    B(0xC1); B(0xE6); B(0x04);                       // shl si, 4
    B(0x01); B(0x36); W(SA(cycles));                 // add [cycles], si
    B(0xF9);                                         // stc
    B(0xC3);                                         // ret
    return (uint16_t) (o - o_start);
}

uint16_t drc_emit_ldi_stub(const drc_env_t *e, uint8_t *out, uint16_t out_off, uint8_t dec) {
    env = e;
    o = o_start = out; o_base = out_off;
    if (FLAT)
        return flat_ldi_stub(dec);
    B(0x56);                                         // push si (address)
    B(0x57);                                         // push di (k)
    B(0x50);                                         // push ax
    B(0x89); B(0xDE);                                // mov si, bx
    B(0xC1); B(0xC6); B(0x04);                       // rol si, 4
    B(0x83); B(0xE6); B(0x0E);                       // and si, 14
    B(0x8E); B(0x84); W(SA(segtab));                 // mov es, [si + segtab]
    uint8_t *to_fast = 0;
    if (!dec && !env->no_fast) {
        B(0xE9); W(0); to_fast = o - 2;              // jmp FAST (at the end)
    }
    uint8_t *loop = o;
    B(0x80); B(0xFE); B(0xC0); uint8_t *s0 = jcc8(0x72);   // cmp dh, C0h; jb slow
    B(0x80); B(0xFE); B(0xE0); uint8_t *s1 = jcc8(0x73);   // cmp dh, E0h; jae slow
    B(0x26); B(0x8A); B(0x27);                       // mov ah, es:[bx]
    B(0x89); B(0xD6);                                // mov si, dx
    B(0x88); B(0xA4); W(env->ram_delta);             // mov [si + ram], ah
    B(0x8D); B(0x5F); B(dec ? 0xFF : 0x01);          // lea bx, [bx +- 1]
    B(dec ? 0x4A : 0x42);                            // dec/inc dx
    B(0x49);                                         // dec cx
    B(0xF7); B(0xC3); W(0x1FFF);                     // test bx, 1FFFh
    uint8_t *noseg = jcc8(0x75);                     // jnz: same ES
    B(0x89); B(0xDE);                                // mov si, bx
    B(0xC1); B(0xC6); B(0x04);                       // rol si, 4
    B(0x83); B(0xE6); B(0x0E);                       // and si, 14
    B(0x8E); B(0x84); W(SA(segtab));                 // mov es, [si + segtab]
    land8(noseg);
    B(0x4F);                                         // dec di
    {
        int16_t back = (int16_t) (loop - (o + 2));
#ifdef GGSWAN_HOST
        if (back < -128) { extern void abort(void); abort(); }
#endif
        B(0x75); B((uint8_t) back);                  // jnz loop
    }
    // flags: S Z C kept, H = N = 0, P/V = BC != 0
    uint8_t *flags = o;
    B(0x58);                                         // pop ax
    B(0x80); B(0xE4); B(0xC1);                       // and ah, C1h
    B(0x85); B(0xC9);                                // test cx, cx
    B(0x74); B(0x03); B(0x80); B(0xCC); B(0x04);     // jz +3; or ah, 4
    B(0x83); B(0xC4); B(0x04);                       // add sp, 4
    B(0xF8);                                         // clc
    B(0xC3);                                         // ret
    land8(s0); land8(s1);
    B(0x58);                                         // pop ax
    B(0x89); B(0x3E); W(SA(val));                    // mov [val], di (count)
    B(0x5E);                                         // pop si (k)
    B(0x29); B(0xFE);                                // sub si, di
    B(0xD1); B(0xE6);                                // shl si, 1
    B(0x89); B(0x36); W(SA(arg));                    // mov [arg], si
    B(0x5E);                                         // pop si (address)
    B(0x01); B(0x36); W(SA(arg));                    // add [arg], si
    B(0x89); B(0xFE);                                // mov si, di
    B(0xC1); B(0xE6); B(0x04);                       // shl si, 4
    B(0x01); B(0x36); W(SA(cycles));                 // add [cycles], si
    B(0xF9);                                         // stc
    B(0xC3);                                         // ret
    if (to_fast) {
        /*
         * FAST: the whole run at once (rep movsb copies byte after byte
         * forwards, as the LDIs do, overlapping or not) when the
         * destination stays inside work RAM and the source inside one
         * 8 KiB page. HL, DE += k, BC -= k, flags as the loop's.
         */
        uint16_t fast_at = (uint16_t) (o - o_start);
        uint16_t rel = (uint16_t) (fast_at - (uint16_t) (to_fast + 2 - o_start));
        to_fast[0] = (uint8_t) rel;
        to_fast[1] = (uint8_t) (rel >> 8);
        B(0x80); B(0xFE); B(0xC0);                   // cmp dh, C0h
        uint8_t *n1 = jcc8(0x72);                    // jb loop (not work RAM)
        B(0x80); B(0xFE); B(0xE0);                   // cmp dh, E0h
        uint8_t *n0 = jcc8(0x73);                    // jae loop (mirror, mapper:
                                                     // DE + k could also wrap)
        B(0x89); B(0xD6);                            // mov si, dx
        B(0x01); B(0xFE);                            // add si, di
        B(0x81); B(0xFE); W(0xE000);                 // cmp si, E000h
        uint8_t *n2 = jcc8(0x77);                    // ja loop (leaves it)
        B(0x89); B(0xDE);                            // mov si, bx
        B(0x81); B(0xE6); W(0x1FFF);                 // and si, 1FFFh
        B(0x01); B(0xFE);                            // add si, di
        B(0x81); B(0xFE); W(0x2000);                 // cmp si, 2000h
        uint8_t *n3 = jcc8(0x77);                    // ja loop (next page)
        B(0x51);                                     // push cx
        B(0x89); B(0xF9);                            // mov cx, di (k)
        B(0x89); B(0xD7);                            // mov di, dx
        B(0x81); B(0xC7); W(env->ram_delta);         // add di, ram
        B(0x89); B(0xDE);                            // mov si, bx
        B(0x1E); B(0x06); B(0x1F); B(0x07);          // push ds; push es; pop ds; pop es
        B(0xFC);                                     // cld (forwards, whatever came before)
        B(0xF3); B(0xA4);                            // rep movsb
        B(0x06); B(0x1F);                            // push es; pop ds (DS, ES: data)
        B(0x89); B(0xF0);                            // mov ax, si
        B(0x29); B(0xD8);                            // sub ax, bx (k)
        B(0x89); B(0xF3);                            // mov bx, si (HL + k)
        B(0x01); B(0xC2);                            // add dx, ax (DE + k)
        B(0x59);                                     // pop cx
        B(0x29); B(0xC1);                            // sub cx, ax (BC - k)
        B(0xE9); W((uint16_t) (flags - (o + 2)));    // jmp flags
        land8(n0); land8(n1); land8(n2); land8(n3);
        B(0xE9); W((uint16_t) (loop - (o + 2)));     // jmp loop
    }
    return (uint16_t) (o - o_start);
}

/*
 * Routine for OUT (BFh),A: the VDP control port. AL = byte. The first byte
 * is latched; the second sets address and code (and for a read, the
 * prefetch). Register writes (code 2): CF = 1, the caller uses the helper.
 * Uses SI; AL, AH kept.
 */
/*
 * IN A,(7Eh) (the V counter; and its even mirrors 40h-7Eh): AL = the
 * counter as z80_in (gg_current_line: frame position / 228, at most 261;
 * past DAh, 6 less), the position as the CTL routine finds it (frame_given
 * - left_base - [cycles]); AH and the others kept. CF = 1: not plain (as
 * there: fl_off, frame_given past 16 bits), the caller uses the helper.
 * Sailor Moon S's loading screens read it ~100 times a frame.
 */
uint16_t drc_emit_vcnt_stub(const drc_env_t *e, uint8_t *out, uint16_t out_off) {
    env = e;
    o = o_start = out; o_base = out_off;
    B(0x80); B(0x3E); W(SA(fl_off)); B(0);           // cmp byte [fl_off], 0
    uint8_t *c1 = jcc8(0x75);                        // jne C
    B(0x83); B(0x3E); W(VA(frame_given) + 2); B(0);  // cmp word [frame_given + 2], 0
    uint8_t *go = jcc8(0x74);                        // je go
    land8(c1);
    B(0xF9); B(0xC3);                                // stc; ret
    land8(go);
    B(0x52); B(0x57); B(0x50);                       // push dx, di, ax
    B(0x8B); B(0x3E); W(SA(left_base));              // mov di, [left_base]
    B(0x03); B(0x3E); W(SA(cycles));                 // add di, [cycles] (left)
    B(0xA1); W(VA(frame_given));                     // mov ax, [frame_given]
    B(0x85); B(0xFF);                                // test di, di
    B(0x7E); B(0x08);                                // jle +8 (sub)
    B(0x39); B(0xC7);                                // cmp di, ax
    B(0x76); B(0x04);                                // jbe +4 (sub)
    B(0x31); B(0xC0);                                // xor ax, ax
    B(0xEB); B(0x02);                                // jmp +2
    B(0x29); B(0xF8);                                // sub ax, di (pos)
    B(0x31); B(0xD2);                                // xor dx, dx
    B(0xBF); W(GG_CYCLES_PER_LINE);                  // mov di, 228
    B(0xF7); B(0xF7);                                // div di (ax = line)
    B(0x3D); W(GG_LINES - 1);                        // cmp ax, 261
    B(0x76); B(0x03);                                // jbe +3
    B(0xB8); W(GG_LINES - 1);                        // mov ax, 261
#ifdef GGSWAN_SMS
    B(0x3B); B(0x06); W(VA(vcnt_max));               // cmp ax, [vcnt_max] (DAh or EAh)
#else
    B(0x3D); W(0xDA);                                // cmp ax, DAh
#endif
    B(0x76); B(0x03);                                // jbe +3
    B(0x2D); W(6);                                   // sub ax, 6
    B(0x88); B(0xC2);                                // mov dl, al
    B(0x58);                                         // pop ax
    B(0x88); B(0xD0);                                // mov al, dl
    B(0x5F); B(0x5A);                                // pop di, dx
    B(0xF8); B(0xC3);                                // clc; ret
    return (uint16_t) (o - o_start);
}

/*
 * Routine for IN A,(7Eh) / (CP n | AND A | OR A) / (JR|JP) cc back
 * (at env->vwait_off): skips in O(1) all T-cycle turns whose V counter
 * keeps the loop going, stopping before the first turn that changes the
 * condition or exhausts [cycles] (or would wrap 16-bit pos/left).
 * In: SI = n | (cc << 8) | (T << 10), where cc = 0..3 (NZ/Z/NC/C) and
 * T = cycles per taken turn (25, 27, 28, or 30).
 * Keeps AX, BX, CX, DX, BP, DS, ES; uses SI, DI and x86 flags.
 */
uint16_t drc_emit_vwait_stub(const drc_env_t *e, uint8_t *out, uint16_t out_off) {
    env = e;
    o = o_start = out; o_base = out_off;
#ifdef GGSWAN_HOST
#define VW_JBACK(op, dst) do { int16_t r_ = (int16_t) ((dst) - (o + 2)); \
        if (r_ < -128 || r_ > 127) { extern void abort(void); abort(); } \
        B(op); B((uint8_t) r_); } while (0)
#else
#define VW_JBACK(op, dst) do { B(op); B((uint8_t) ((dst) - (o + 1))); } while (0)
#endif
    B(0x80); B(0x3E); W(SA(fl_off)); B(0);           // cmp byte [fl_off], 0
    uint8_t *e1 = jcc8(0x75);                        // jne early_ret
    B(0x83); B(0x3E); W(VA(frame_given) + 2); B(0);  // cmp word [frame_given + 2], 0
    uint8_t *e2 = jcc8(0x75);                        // jne early_ret
    B(0x83); B(0x3E); W(SA(cycles)); B(0);           // cmp word [cycles], 0
    uint8_t *go = jcc8(0x7F);                        // jg go
    land8(e1); land8(e2);
    B(0xC3);                                         // early_ret: ret
    land8(go);

    B(0x50); B(0x53); B(0x51); B(0x52); B(0x55);     // push ax, bx, cx, dx, bp
    B(0x89); B(0xF3);                                // mov bx, si (BL = n, BH = cc | (T << 2))
    B(0x8B); B(0x0E); W(SA(cycles));                 // mov cx, [cycles] (cur_cyc > 0)
    B(0x8B); B(0x3E); W(SA(left_base));              // mov di, [left_base]
    B(0x01); B(0xCF);                                // add di, cx (DI = left = left_base + cur_cyc)
    B(0xA1); W(VA(frame_given));                     // mov ax, [frame_given] (fg)
    B(0x85); B(0xFF);                                // test di, di
    uint8_t *pos_ok = jcc8(0x7E);                    // jle pos_ok
    B(0x39); B(0xC7);                                // cmp di, ax
    uint8_t *bail0 = jcc8(0x77);                     // ja bail (di > 0 && di > fg: k = 0)
    land8(pos_ok);
    B(0x29); B(0xF8);                                // sub ax, di (AX = pos = fg - di)

    // BP = max_skip = min(cur_cyc - 1, (uint16_t)(di + 8000h), FFFFh - pos)
    B(0x89); B(0xCD);                                // mov bp, cx
    B(0x4D);                                         // dec bp (cur_cyc - 1)
    B(0x81); B(0xC7); W(0x8000);                     // add di, 8000h (di_room)
    B(0x39); B(0xFD);                                // cmp bp, di
    B(0x76); B(0x02); B(0x89); B(0xFD);              // jbe +2; mov bp, di
    B(0x89); B(0xC7); B(0xF7); B(0xD7);              // mov di, ax; not di (FFFFh - pos)
    B(0x39); B(0xFD);                                // cmp bp, di
    B(0x76); B(0x02); B(0x89); B(0xFD);              // jbe +2; mov bp, di

    // DI = pos, SI = line = min(pos / 228, 261), DX = vmax, AL = vcnt(line)
    B(0x89); B(0xC7);                                // mov di, ax (DI = pos)
    B(0x31); B(0xD2);                                // xor dx, dx
    B(0xBE); W(GG_CYCLES_PER_LINE);                  // mov si, 228
    B(0xF7); B(0xF6);                                // div si (AX = pos / 228)
    B(0x3D); W(GG_LINES - 1);                        // cmp ax, 261
    B(0x76); B(0x03); B(0xB8); W(GG_LINES - 1);      // jbe +3; mov ax, 261
    B(0x89); B(0xC6);                                // mov si, ax (SI = line)
#ifdef GGSWAN_SMS
    B(0x8B); B(0x16); W(VA(vcnt_max));               // mov dx, [vcnt_max] (DAh or EAh)
#else
    B(0xBA); W(0xDA);                                // mov dx, DAh
#endif
    B(0x39); B(0xD0);                                // cmp ax, dx
    B(0x76); B(0x03); B(0x2D); W(6);                 // jbe +3; sub ax, 6 (AL = v)

    // Dispatch on cc = BH & 3 (fast path: cc == 0 JR/JP NZ)
    B(0x88); B(0xFC);                                // mov ah, bh
    B(0x80); B(0xE4); B(0x03);                       // and ah, 3
    uint8_t *chk123 = jcc8(0x75);                    // jnz chk123
    // cc == 0 (NZ): taken iff v != n
    B(0x38); B(0xD8);                                // cmp al, bl
    uint8_t *bail1 = jcc8(0x74);                     // je bail (already stopped)
    B(0x88); B(0xD8);                                // mov al, bl
    B(0x30); B(0xE4);                                // xor ah, ah (AX = n)
    B(0x39); B(0xC6);                                // cmp si, ax (line < n ?)
    uint8_t *nz_s2 = jcc8(0x73);                     // jae nz_seg2
    B(0x39); B(0xD0);                                // cmp ax, dx (n <= vmax ?)
    uint8_t *hs0 = jcc8(0x76);                       // jbe have_stop
    land8(nz_s2);
    B(0x83); B(0xC0); B(6);                          // add ax, 6 (n + 6)
    B(0x39); B(0xD0);                                // cmp ax, dx (n + 6 > vmax ?)
    uint8_t *ns0 = jcc8(0x76);                       // jbe no_stop
    B(0x39); B(0xF0);                                // cmp ax, si (n + 6 > line ?)
    uint8_t *hs1 = jcc8(0x77);                       // ja have_stop
    uint8_t *ns1 = jcc8(0xEB);                       // jmp no_stop

    // bail (reachable by short jumps from prologue, cc == 0, and cc == 1..3)
    land8(bail0); land8(bail1);
    uint8_t *bail_lbl = o;
    B(0x5D); B(0x5A); B(0x59); B(0x5B); B(0x58);     // bail: pop bp, dx, cx, bx, ax
    B(0xC3);                                         // ret

    // cc == 1 (Z), 2 (NC), 3 (C)
    land8(chk123);
    B(0x80); B(0xFC); B(2);                          // cmp ah, 2
    uint8_t *chk_z = jcc8(0x72);                     // jb chk_z (cc == 1)
    uint8_t *chk_nc = jcc8(0x74);                    // je chk_nc (cc == 2)
    // cc == 3 (C): taken iff v < n
    B(0x38); B(0xD8);                                // cmp al, bl
    VW_JBACK(0x73, bail_lbl);                        // jae bail
    B(0x88); B(0xD8);                                // mov al, bl
    B(0x30); B(0xE4);                                // xor ah, ah (AX = n)
    B(0x39); B(0xD6);                                // cmp si, dx (line <= vmax ?)
    uint8_t *c_s2 = jcc8(0x77);                      // ja c_seg2
    B(0x39); B(0xD0);                                // cmp ax, dx (n <= vmax ?)
    uint8_t *hs2 = jcc8(0x76);                       // jbe have_stop
    land8(c_s2);
    B(0x83); B(0xC0); B(6);                          // add ax, 6
    uint8_t *hs3 = jcc8(0xEB);                       // jmp have_stop

    // cc == 2 (NC): taken iff v >= n
    land8(chk_nc);
    B(0x38); B(0xD8);                                // cmp al, bl
    VW_JBACK(0x72, bail_lbl);                        // jb bail
    B(0x39); B(0xD6);                                // cmp si, dx (line <= vmax ?)
    uint8_t *ns2 = jcc8(0x77);                       // ja no_stop
    B(0x89); B(0xD0);                                // mov ax, dx (vmax)
    B(0x83); B(0xE8); B(5);                          // sub ax, 5 (vmax - 5)
    B(0x38); B(0xD8);                                // cmp al, bl (vmax - 5 < n ?)
    uint8_t *ns3 = jcc8(0x73);                       // jae no_stop
    B(0x89); B(0xD0);                                // mov ax, dx
    B(0x40);                                         // inc ax (vmax + 1)
    uint8_t *hs4 = jcc8(0xEB);                       // jmp have_stop

    // cc == 1 (Z): taken iff v == n
    land8(chk_z);
    B(0x38); B(0xD8);                                // cmp al, bl
    VW_JBACK(0x75, bail_lbl);                        // jne bail
    B(0x89); B(0xF0);                                // mov ax, si
    B(0x40);                                         // inc ax (line + 1)
    B(0x3D); W(GG_LINES);                            // cmp ax, 262
    uint8_t *ns4 = jcc8(0x73);                       // jae no_stop

    // have_stop: AX = stop_line (1..261), DI = pos, BP = max_skip
    land8(hs0); land8(hs1); land8(hs2); land8(hs3); land8(hs4);
    B(0x6B); B(0xC0); B(57);                         // imul ax, ax, 57
    B(0xC1); B(0xE0); B(2);                          // shl ax, 2 (stop_line * 228)
    B(0x29); B(0xF8);                                // sub ax, di (delta = target_pos - pos)
    B(0x88); B(0xFB);                                // mov bl, bh
    B(0xC0); B(0xEB); B(2);                          // shr bl, 2
    B(0x30); B(0xFF);                                // xor bh, bh (BX = T)
    B(0x01); B(0xD8);                                // add ax, bx
    B(0x48);                                         // dec ax (u = delta + T - 1)
    B(0x89); B(0xC6);                                // mov si, ax (SI = u)
    B(0x31); B(0xD2);                                // xor dx, dx
    B(0xF7); B(0xF3);                                // div bx (DX = u % T)
    B(0x29); B(0xD6);                                // sub si, dx (SI = skip = k * T)
    B(0x39); B(0xEE);                                // cmp si, bp (skip <= max_skip ?)
    uint8_t *apply_j = jcc8(0x76);                   // jbe apply_skip
    uint8_t *clamp_j = jcc8(0xEB);                   // jmp clamp_with_bx

    // no_stop: unpack T into BX, then skip floor(max_skip / T) * T
    land8(ns0); land8(ns1); land8(ns2); land8(ns3); land8(ns4);
    B(0x88); B(0xFB);                                // mov bl, bh
    B(0xC0); B(0xEB); B(2);                          // shr bl, 2
    B(0x30); B(0xFF);                                // xor bh, bh (BX = T)
    land8(clamp_j);
    B(0x89); B(0xE8);                                // mov ax, bp (max_skip)
    B(0x31); B(0xD2);                                // xor dx, dx
    B(0xF7); B(0xF3);                                // div bx (DX = max_skip % T)
    B(0x89); B(0xEE);                                // mov si, bp
    B(0x29); B(0xD6);                                // sub si, dx (SI = floor(max_skip / T) * T)
    land8(apply_j);
    B(0x29); B(0xF1);                                // sub cx, si (cur_cyc -= skip)
    B(0x89); B(0x0E); W(SA(cycles));                 // mov [cycles], cx
    B(0x5D); B(0x5A); B(0x59); B(0x5B); B(0x58);     // pop bp, dx, cx, bx, ax
    B(0xC3);                                         // ret
#undef VW_JBACK
    return (uint16_t) (o - o_start);
}

#ifdef GGSWAN_SG1000
/*
 * sgswan: the TMS9918A's ports without the C helper (gg.c: vdp_write_data,
 * vdp_read_data, vdp_read_status, vdp_write_control). The VRAM is at
 * SG_VRAM_SEG:SG_VRAM_OFF (the RAM window, main.c); a byte that changes
 * marks what the screen is made from again (sg_video.inc). AL the byte in
 * or out; SI, DI, ES and the flags used, the rest kept.
 */
#define VS(field) ((uint16_t) (env->sg_vdp + offsetof(gg_vdp_t, field)))
// (sfswan: the VRAM is read through another segment, env->sf_vseg, a ROM
// window on its bank; ES is the memory's again afterwards)
static void sg_vram_es(void) {
    B(0xBF); W(FLAT ? FENV(sf_vseg) : SG_VRAM_SEG);   // mov di, the VRAM's segment
    B(0x8E); B(0xC7);                                // mov es, di
}
static void sg_vram_done(void) {
    if (!FLAT)
        return;
    B(0xBF); W(SG_VRAM_SEG);                         // mov di, the memory's segment
    B(0x8E); B(0xC7);                                // mov es, di (the flags kept)
}
// sfswan: the RAM window on a PSRAM bank (AX kept)
static void sf_ram_bank(uint16_t bank) {
    B(0x50);                                         // push ax
    B(0xB8); W(bank);                                // mov ax, bank
    B(0xE7); B(0xD0);                                // out D0h, ax
    B(0x58);                                         // pop ax
}
static void sg_addr_next(void) {
    B(0x46);                                         // inc si
    B(0x81); B(0xE6); W(0x3FFF);                     // and si, 3FFFh
    B(0x89); B(0x36); W(VS(addr));                   // mov [addr], si
}
uint16_t drc_emit_sg_stubs(const drc_env_t *e, uint8_t *out, uint16_t out_off, uint16_t *at) {
    env = e;
    o = o_start = out; o_base = out_off;
    // the data port, out: always VRAM
    at[0] = here();
    B(0x8B); B(0x36); W(VS(addr));                   // mov si, [addr]
    sg_vram_es();
    B(0x26); B(0x38); B(0x84); W(SG_VRAM_OFF);       // cmp es:[si + vram], al
    sg_vram_done();
    uint8_t *same = jcc8(0x74);                      // je same
    if (FLAT && FENV(sf_vbank) != 0xFFFF) {
        // (sfswan: written through the RAM window, moved to the VRAM's
        // bank for the byte: the same segment as the memory's)
        sf_ram_bank(FENV(sf_vbank));
        B(0x26); B(0x88); B(0x84); W(SG_VRAM_OFF);   // mov es:[si + vram], al
        sf_ram_bank(FENV(sf_rbank));
    } else if (FLAT) {
        // (the PC tools: no banks, the read segment can be written)
        sg_vram_es();
        B(0x26); B(0x88); B(0x84); W(SG_VRAM_OFF);
        sg_vram_done();
    } else {
    B(0x26); B(0x88); B(0x84); W(SG_VRAM_OFF);       // mov es:[si + vram], al
    }
    B(0x89); B(0xF7);                                // mov di, si
    B(0xC1); B(0xEF); B(0x04);                       // shr di, 4
    B(0xC6); B(0x85); W(SG_DIRTY_ADDR); B(1);        // mov byte [di + dirty], 1
    B(0xC1); B(0xEF); B(0x05);                       // shr di, 5
    B(0xC6); B(0x85); W(SG_PAGE_ADDR); B(1);         // mov byte [di + page], 1
    B(0xC6); B(0x06); W(SG_ANY_ADDR); B(1);          // mov byte [any], 1
    land8(same);
    B(0xA2); W(VS(read_buffer));                     // mov [read_buffer], al
    sg_addr_next();
    B(0xC6); B(0x06); W(VS(latch_full)); B(0);       // latch_full = 0
    B(0xC3);                                         // ret
    // the data port, in: the byte read ahead, the next one read
    at[1] = here();
    B(0x8B); B(0x36); W(VS(addr));                   // mov si, [addr]
    sg_vram_es();
    B(0x26); B(0x8A); B(0x84); W(SG_VRAM_OFF);       // mov al, es:[si + vram]
    sg_vram_done();
    B(0x86); B(0x06); W(VS(read_buffer));            // xchg al, [read_buffer]
    sg_addr_next();
    B(0xC6); B(0x06); W(VS(latch_full)); B(0);       // latch_full = 0
    B(0xC3);                                         // ret
    // the status: bits 0-4 are 1 without a fifth sprite; read, it clears
    at[2] = here();
    B(0xA0); W(VS(status));                          // mov al, [status]
    B(0xA8); B(0x40);                                // test al, 40h
    B(0x75); B(0x02);                                // jnz +2
    B(0x0C); B(0x1F);                                // or al, 1Fh
    B(0xC6); B(0x06); W(VS(status)); B(0);           // status = 0
    B(0xC6); B(0x06); W(VS(line_pending)); B(0);     // line_pending = 0
    B(0xC6); B(0x06); W(VS(latch_full)); B(0);       // latch_full = 0
    B(0xC6); B(0x06); W(ZA(irq)); B(0);              // (no request left)
    B(0xC3);                                         // ret
    // the control port: the first byte kept, the second an address (and
    // the byte read ahead) or a register (the helper)
    at[3] = here();
    B(0x80); B(0x3E); W(VS(latch_full)); B(0);       // cmp byte [latch_full], 0
    uint8_t *second = jcc8(0x75);                    // jne second
    B(0xA2); W(VS(latch));                           // mov [latch], al
    B(0xA2); W(VS(addr));                            // mov [addr], al (low byte)
    B(0xC6); B(0x06); W(VS(latch_full)); B(1);       // latch_full = 1
    B(0xF8); B(0xC3);                                // clc; ret
    land8(second);
    B(0xA8); B(0x80);                                // test al, 80h
    uint8_t *adr = jcc8(0x74);                       // jz address
    B(0xF9); B(0xC3);                                // stc; ret
    land8(adr);
    B(0x50);                                         // push ax
    B(0xC6); B(0x06); W(VS(latch_full)); B(0);       // latch_full = 0
    B(0x88); B(0xC4);                                // mov ah, al
    B(0xC0); B(0xEC); B(0x06);                       // shr ah, 6
    B(0x88); B(0x26); W(VS(code));                   // mov [code], ah
    B(0x88); B(0xC4);                                // mov ah, al
    B(0x80); B(0xE4); B(0x3F);                       // and ah, 3Fh
    B(0x88); B(0x26); W(VS(addr) + 1);               // mov [addr + 1], ah
    B(0x8A); B(0x26); W(VS(latch));                  // mov ah, [latch]
    B(0x88); B(0x26); W(VS(addr));                   // mov [addr], ah
    B(0xA8); B(0x40);                                // test al, 40h
    uint8_t *done = jcc8(0x75);                      // jnz done (to write)
    B(0x8B); B(0x36); W(VS(addr));                   // mov si, [addr]
    sg_vram_es();
    B(0x26); B(0x8A); B(0x84); W(SG_VRAM_OFF);       // mov al, es:[si + vram]
    sg_vram_done();
    B(0xA2); W(VS(read_buffer));                     // mov [read_buffer], al
    sg_addr_next();
    land8(done);
    B(0x58);                                         // pop ax
    B(0xF8); B(0xC3);                                // clc; ret
    return (uint16_t) (o - o_start);
}
#endif

uint16_t drc_emit_ctl_stub(const drc_env_t *e, uint8_t *out, uint16_t out_off) {
    env = e;
    o = o_start = out; o_base = out_off;
    B(0x80); B(0x3E); W(VA(latch_full)); B(0);       // cmp byte [latch_full], 0
    uint8_t *second = jcc8(0x75);                    // jne second
    B(0xA2); W(VA(latch));                           // mov [latch], al
    B(0xA2); W(VA(addr));                            // mov [addr], al (low byte)
    B(0xC6); B(0x06); W(VA(latch_full)); B(1);       // latch_full = 1
    B(0xF8); B(0xC3);                                // clc; ret
    uint8_t *slow = o;
    B(0xF9); B(0xC3);                                // stc; ret
    land8(second);
    B(0x89); B(0xC6);                                // mov si, ax
    B(0x81); B(0xE6); W(0x00C0);                     // and si, C0h
    B(0x81); B(0xFE); W(0x0080);                     // cmp si, 80h (register)
    uint8_t *regw = jcc8(0x74);                      // je reg
    B(0x50);                                         // push ax
    B(0xC6); B(0x06); W(VA(latch_full)); B(0);       // latch_full = 0
    B(0x88); B(0xC4);                                // mov ah, al
    B(0xC0); B(0xEC); B(0x06);                       // shr ah, 6
    B(0x88); B(0x26); W(VA(code));                   // mov [code], ah
    B(0x88); B(0xC4);                                // mov ah, al
    B(0x80); B(0xE4); B(0x3F);                       // and ah, 3Fh
    B(0x88); B(0x26); W(VA(addr) + 1);               // mov [addr + 1], ah
    B(0x8A); B(0x26); W(VA(latch));                  // mov ah, [latch]
    B(0x88); B(0x26); W(VA(addr));                   // mov [addr], ah
    B(0xA8); B(0xC0);                                // test al, C0h
    uint8_t *done = jcc8(0x75);                      // jnz done (not a read)
    if (e->cls_off) {
        // deferred tile writes (main.c): written first
        B(0x80); B(0x3E); W(SA(defer)); B(0);        // cmp byte [defer], 0
        uint8_t *nd = jcc8(0x74);                    // je
        B(0xC6); B(0x06); W(SA(op)); B(DRC_H_SYNC);  // mov byte [op], SYNC
        call_near(e->helper_off);
        land8(nd);
    }
    B(0x8B); B(0x36); W(VA(addr));                   // mov si, [addr]
    B(0x8A); B(0xA4); W(env->vram_addr);             // mov ah, [si + vram]
    B(0x88); B(0x26); W(VA(read_buffer));            // mov [read_buffer], ah
    B(0x46);                                         // inc si
    B(0x81); B(0xE6); W(0x3FFF);                     // and si, 3FFFh
    B(0x89); B(0x36); W(VA(addr));                   // mov [addr], si
    land8(done);
    B(0x58);                                         // pop ax
    B(0xF8); B(0xC3);                                // clc; ret
    // register write: register 8 (horizontal scroll) with the value of the
    // last split recorded changes nothing else: done here; others: C
    land8(regw);
    B(0x89); B(0xC6);                                // mov si, ax
    B(0x83); B(0xE6); B(0x0F);                       // and si, 0Fh
    B(0x50);                                         // push ax
    B(0x8A); B(0x26); W(VA(latch));                  // mov ah, [latch]
    B(0x83); B(0xFE); B(0x08);                       // cmp si, 8
    uint8_t *r8 = jcc8(0x74);                        // je reg8
    // another register with the value it has: nothing to do but the latch
    // (registers 0, 9, 10 are written again with the same value a few
    // times a frame by Aladdin, the Monaco games: a helper each before)
    B(0x83); B(0xFE); B(0x0B);                       // cmp si, 11
    uint8_t *ns1 = jcc8(0x73);                       // jae notsame
    B(0x3A); B(0xA4); W(VA(reg));                    // cmp ah, [si + reg]
    uint8_t *ns2 = jcc8(0x75);                       // jne notsame
    uint8_t *same_r = jcc8(0xEB);                    // jmp same
    land8(ns1); land8(ns2);
    B(0x58);                                         // pop ax
    {
        int16_t back = (int16_t) (slow - (o + 2));
#ifdef GGSWAN_HOST
        if (back < -128) { extern void abort(void); abort(); }
#endif
        B(0xEB); B((uint8_t) back);                  // jmp slow
    }
    land8(r8);
    B(0x3A); B(0x26); W(SA(hs_last));                // cmp ah, [hs_last]
    uint8_t *diff = jcc8(0x75);                      // jne differ
    B(0x88); B(0x26); W(VA(reg) + 8);                // mov [reg + 8], ah
    land8(same_r);
    B(0x88); B(0x26); W(VA(addr));                   // mov [addr], ah
    B(0x88); B(0xC4);                                // mov ah, al
    B(0x80); B(0xE4); B(0x3F);                       // and ah, 3Fh
    B(0x88); B(0x26); W(VA(addr) + 1);               // mov [addr + 1], ah
    B(0xC6); B(0x06); W(VA(code)); B(2);             // code = 2
    B(0xC6); B(0x06); W(VA(latch_full)); B(0);       // latch_full = 0
    B(0x58);                                         // pop ax
    B(0xF8); B(0xC3);                                // clc; ret
    land8(diff);
    if (!env->no_fast) {
        /*
         * A new value: the register write as above, then main.c's
         * hscroll_write here: the split is recorded at the line after the
         * current one (frame position = frame_given - left, left =
         * left_base + [cycles]; as gg_current_line). AH = the value.
         */
        // (C when the frame position is not plain: fl_off is set after a
        // remap, whose [cycles] = -30000 make it overflow, and the like)
        B(0x80); B(0x3E); W(SA(fl_off)); B(0);       // cmp byte [fl_off], 0
        uint8_t *c1 = jcc8(0x75);                    // jne C
        // (no room for another split: C, which makes some)
        B(0x80); B(0x3E); W(SA(rec_n)); B(DRC_MAX_SPLITS);   // cmp byte [rec_n], MAX
        uint8_t *c0 = jcc8(0x73);                    // jae C
        B(0x83); B(0x3E); W(VA(frame_given) + 2); B(0);   // cmp word [frame_given + 2], 0
        uint8_t *small = jcc8(0x74);                 // je go
        land8(c1); land8(c0);
        B(0x58);                                     // pop ax
        B(0xF9); B(0xC3);                            // stc; ret (C)
        land8(small);
        B(0x52);                                     // push dx
        B(0xB2); B(16);                              // mov dl, 16
        B(0x28); B(0xE2);                            // sub dl, ah (v = 16 - value)
        B(0x88); B(0x26); W(VA(reg) + 8);            // mov [reg + 8], ah
        B(0x88); B(0x26); W(VA(addr));               // mov [addr], ah
        B(0x88); B(0xC4);                            // mov ah, al
        B(0x80); B(0xE4); B(0x3F);                   // and ah, 3Fh
        B(0x88); B(0x26); W(VA(addr) + 1);           // mov [addr + 1], ah
        B(0xC6); B(0x06); W(VA(code)); B(2);         // code = 2
        B(0xC6); B(0x06); W(VA(latch_full)); B(0);   // latch_full = 0
        // (AX is free now: popped at the end)
        B(0xA0); W(SA(rec_n));                       // mov al, [rec_n]
        B(0x30); B(0xE4);                            // xor ah, ah
        B(0x89); B(0xC6);                            // mov si, ax (n)
        B(0x3A); B(0x94); W(SA(rec_val) - 1);        // cmp dl, [si + rec_val - 1]
        uint8_t *same = jcc8(0x74);                  // je done (the last split's value)
        // frame position (0 if left > frame_given: frame_pos)
        B(0x8B); B(0x3E); W(SA(left_base));          // mov di, [left_base]
        B(0x03); B(0x3E); W(SA(cycles));             // add di, [cycles] (left)
        B(0xA1); W(VA(frame_given));                 // mov ax, [frame_given]
        B(0x85); B(0xFF);                            // test di, di
        B(0x7E); B(0x08);                            // jle +8 (sub)
        B(0x39); B(0xC7);                            // cmp di, ax
        B(0x76); B(0x04);                            // jbe +4 (sub)
        B(0x31); B(0xC0);                            // xor ax, ax
        B(0xEB); B(0x02);                            // jmp +2
        B(0x29); B(0xF8);                            // sub ax, di (pos)
        B(0x52);                                     // push dx
        B(0x31); B(0xD2);                            // xor dx, dx
        B(0xBF); W(GG_CYCLES_PER_LINE);              // mov di, 228
        B(0xF7); B(0xF7);                            // div di (ax = line)
        B(0x5A);                                     // pop dx
        B(0x3D); W(GG_LINES - 1);                    // cmp ax, 261
        B(0x76); B(0x03);                            // jbe +3
        B(0xB8); W(GG_LINES - 1);                    // mov ax, 261
        B(0x40);                                     // inc ax (the next line)
        B(0x3D); W(GG_REC_Y);                        // cmp ax, VIEW_Y
        uint8_t *inview = jcc8(0x77);                // ja inview
        // before the view: the value for its first line
        B(0x88); B(0x16); W(SA(rec_val));            // mov [rec_val], dl
        B(0xC6); B(0x06); W(SA(rec_n)); B(1);        // rec_n = 1
        uint8_t *last1 = jcc8(0xEB);                 // jmp set_last
        land8(inview);
        B(0x2D); W(GG_REC_Y);                        // sub ax, VIEW_Y (y)
#ifdef GGSWAN_SMS
        B(0x3B); B(0x06); W(VA(active));             // cmp ax, [active] (192 or 224)
#else
        B(0x3D); W(GG_REC_H);                        // cmp ax, VIEW_H
#endif
        uint8_t *below = jcc8(0x73);                 // jae below
        B(0x83); B(0xFE); B(1);                      // cmp si, 1
        uint8_t *app1 = jcc8(0x76);                  // jbe append
        B(0x3A); B(0x84); W(SA(rec_line) - 1);       // cmp al, [si + rec_line - 1]
        uint8_t *app2 = jcc8(0x75);                  // jne append
        B(0x88); B(0x94); W(SA(rec_val) - 1);        // mov [si + rec_val - 1], dl (same line)
        uint8_t *last2 = jcc8(0xEB);                 // jmp set_last
        land8(app1); land8(app2);
        B(0x88); B(0x84); W(SA(rec_line));           // mov [si + rec_line], al
        B(0x88); B(0x94); W(SA(rec_val));            // mov [si + rec_val], dl
        B(0xFE); B(0x06); W(SA(rec_n));              // inc byte [rec_n]
        uint8_t *out = jcc8(0xEB);                   // (appended: jmp set_last)
        // below the view: after it (line 169 on, the frame was presented
        // at the start of line 168, main.c) the value for the next frame's
        // first line; before it, nothing
        land8(below);
#ifdef GGSWAN_SMS
        B(0x3B); B(0x06); W(VA(present_p1));         // cmp ax, [present + 1]
#else
        B(0x3D); W(GG_PRESENT_LINE + 1 - GG_REC_Y);  // cmp ax, 169 - VIEW_Y
#endif
        uint8_t *mid = jcc8(0x72);                   // jb done
        B(0x88); B(0x16); W(SA(rec_val));            // mov [rec_val], dl
        B(0xC6); B(0x06); W(SA(rec_n)); B(1);        // rec_n = 1
        land8(out);
        land8(last1); land8(last2);
        B(0x8A); B(0x26); W(VA(latch));              // mov ah, [latch] (value)
        B(0x88); B(0x26); W(SA(hs_last));            // mov [hs_last], ah
        land8(same); land8(mid);
        B(0x5A);                                     // pop dx
        B(0x58);                                     // pop ax
        B(0xF8); B(0xC3);                            // clc; ret
    }
    B(0x58);                                         // pop ax
    B(0xF9); B(0xC3);                                // stc; ret
    return (uint16_t) (o - o_start);
}

/* === Decoder === */

// the Z80 bytes, as z80_read: through a pointer kept for the span it
// covers (z80_read_span), not a far call each. On the WS z80_read was 13-17%
// of the translation time (the device build run in Unicorn, Shinobi's
// blocks), every byte read several times. Reset at each block.
static const uint8_t GGFAR *rd_ptr;
static uint16_t rd_base, rd_len;
// (a far call each was still 13% of the time on the WS, patch 77: inline,
// the refill out of line)
static __attribute__((noinline)) void rd_refill(uint16_t a) {
    rd_ptr = z80_read_span(a, &rd_len);
    rd_base = a;
}
static inline __attribute__((always_inline)) uint8_t rd(uint16_t a) {
    uint16_t k = a - rd_base;
    if (k >= rd_len) {
        rd_refill(a);
        k = 0;
    }
    return rd_ptr[k];
}

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
        case 0x47: return;                                          // LD I,A
        case 0x57: I->fwrite = FALL & ~FC; I->fread = FC; return;   // LD A,I
        case 0x5F:                                                  // LD A,R
            if (env->no_fast) { I->step = 1; return; }
            I->fwrite = FALL & ~FC; I->fread = FC; return;
        case 0x67: case 0x6F:                                       // RRD, RLD
            if (env->no_fast) { I->step = 1; return; }
            I->fwrite = FALL & ~FC; I->fread = FC; return;
        case 0x43: case 0x53: case 0x73:                            // LD (nn),rr
            I->nn = rd(pc + 2) | (rd(pc + 3) << 8); I->len = 4;
            if (!FLAT && (I->nn < 0xC000 || I->nn >= 0xDFFF)) I->step = 1;
            return;
        case 0x4B: case 0x5B: case 0x7B:                            // LD rr,(nn)
            I->nn = rd(pc + 2) | (rd(pc + 3) << 8); I->len = 4;
            if (op == 0x7B) I->term = T_END_AFTER;                  // (SP: sp_loaded_exit)
            return;
        case 0x45: case 0x4D: case 0x55: case 0x5D:
        case 0x65: case 0x6D: case 0x75: case 0x7D:                 // RETN, RETI
            I->term = T_END; return;
        }
        if ((op & 0xC7) == 0x42) {                                  // SBC/ADC HL,rr
            I->fwrite = FALL; I->fread = FC; return;
        }
#ifdef GGSWAN_SG1000
        if ((op == 0xA2 || op == 0xAA || op == 0xA3 || op == 0xAB) && env->sg_out_off && !FLAT) {
            // sgswan: a run of INI, IND, OUTI or OUTD, with the
            // TMS9918A's data port (sg_block_io)
            uint8_t k = 1;
            while (k < 64 && rd(pc + k * 2) == 0xED && rd(pc + k * 2 + 1) == op) k++;
            I->len = k * 2; I->count = k; I->fwrite = FALL;
            return;
        }
        if ((op == 0xB2 || op == 0xB3) && env->sg_out_off && !FLAT) {
            // INIR, OTIR: up to 64 bytes a turn
            I->term = T_END; I->fwrite = FALL;
            return;
        }
#endif
        if (op == 0xA3 && env->outi_off && !env->no_fast) {
            /*
             * OUTI / JR NZ back to it (or JP NZ): OTIR written as a loop,
             * in Sega's older games (Shinobi, Columns, Batman Returns, Alex
             * Kidd). As two instructions each turn was a block and a call
             * of the OUTI routine for one byte, about 126 cycles: Shinobi
             * sends 53 bytes a frame this way. Translated as OTIR (op B3h,
             * the routine with up to 32 bytes at a time) with the loop's
             * length and cycles: nn = those of a turn, d = how many less
             * the last one takes (the jump not taken).
             */
            uint8_t j = rd(pc + 2);
            if (j == 0x20 && rd(pc + 3) == 0xFC) {
                I->op = 0xB3; I->len = 4; I->nn = 16 + 12; I->d = 5;
                I->term = T_END; I->fwrite = FALL; return;
            }
            if (j == 0xC2 && (uint16_t) (rd(pc + 3) | (rd(pc + 4) << 8)) == pc) {
                I->op = 0xB3; I->len = 5; I->nn = 16 + 10; I->d = 0;
                I->term = T_END; I->fwrite = FALL; return;
            }
        }
        if (op == 0xA0 || op == 0xA8 || op == 0xA3 || op == 0xAB) {
            // LDI LDD OUTI OUTD: games unroll long runs of them (VRAM
            // uploads); a run is one interpreted step of n instructions
            uint8_t k = 1;
            while (k < 64 && rd(pc + k * 2) == 0xED && rd(pc + k * 2 + 1) == op) k++;
            I->len = k * 2; I->count = k;
            if ((op == 0xA3 || op == 0xAB) && env->vdp_addr)
                I->fwrite = FALL;   // native loop (VRAM uploads), see emit
            else if ((op == 0xA0 || op == 0xA8) && env->ldi_off)
                I->fwrite = FH | FP | FN;   // native loop (to work RAM)
            else
                I->step = 1;
            return;
        }
        if ((op & 0xC7) == 0x41 && op != 0x71) return;                      // OUT (C),r
        if ((op == 0xB0 || op == 0xB8) && env->ldi_off) {
            // LDIR, LDDR to work RAM: native, 64 bytes per turn
            I->term = T_END; I->fwrite = FH | FP | FN; return;
        }
        if (op == 0xB3 && env->outi_off && !env->no_fast) {
            // OTIR: the OUTI routine, 64 bytes per turn
            I->nn = 21; I->d = 5;
            I->term = T_END; I->fwrite = FALL; return;
        }
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
            if (!FLAT && op2 == 0x22 && (I->nn < 0xC000 || I->nn >= 0xDFFF)) I->step = 1;
            return;
        case 0x23: case 0x2B: case 0xE1: case 0xE5: case 0xF9: case 0xE3:
            if (op2 == 0xF9) I->term = T_END_AFTER;                   // LD SP,IX (sp_loaded_exit)
            return;
        case 0xE9: I->term = T_END; return;                           // JP (IX)
        case 0x09: case 0x19: case 0x29: case 0x39:                   // ADD IX,rr
            I->fwrite = FH | FN | FC; I->fread = FS | FZ | FP; return;
        case 0x36:                                                    // LD (IX+d),n
            I->d = (int8_t) rd(pc + 2); I->nn = rd(pc + 3); I->len = 4; return;
        case 0x34: case 0x35:                                         // INC/DEC (IX+d)
            I->d = (int8_t) rd(pc + 2); I->len = 3;
            I->fwrite = FS | FZ | FH | FP | FN; I->fread = FC; return;   // (as INC/DEC (HL))
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
        // the undocumented halves IXH/IXL (IYH/IYL): H and L of a plain
        // opcode read as them. Aladdin's palette fades loop on LD IXL,A and
        // OR IXL: each ended its block, then the interpreter between blocks
        // (~20 dispatches a frame). LD r,IXh / LD IXh,r / ALU A,IXh native
        // (drc_emit), INC/DEC/LD n and the rest stepped in the block.
        {
            uint8_t x2 = op2 >> 6, y2 = (op2 >> 3) & 7, z2 = op2 & 7;
            uint8_t hy = y2 == 4 || y2 == 5, hz = z2 == 4 || z2 == 5;
            if (x2 == 1 && op2 != 0x76 && (hy || hz) && y2 != 6 && z2 != 6) {
                if (hy && hz) I->step = 1;                         // LD IXh,IXl
                return;                                            // LD r,IXh / LD IXh,r
            }
            if (x2 == 2 && hz) {                                   // ALU A,IXh
                I->fwrite = FALL;
                if (y2 == 1 || y2 == 3) I->fread = FC;
                return;
            }
            if (x2 == 0 && hy && (z2 == 4 || z2 == 5 || z2 == 6)) {   // INC/DEC/LD IXh(,n)
                I->step = 1;
                if (z2 == 6) I->len = 3;
                return;
            }
        }
        // other DD/FD forms, or a prefix on a plain opcode: left to the
        // interpreter, between blocks
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
            I->nn = rd(pc + 1) | (rd(pc + 2) << 8); I->len = 3;                      // LD rr,nn
            if (y == 6 && !SP_IN_RAM(I->nn)) I->term = T_END_AFTER;                  // (SP: sp_loaded_exit)
            return;
        case 2:
            if (y >= 4) { I->nn = rd(pc + 1) | (rd(pc + 2) << 8); I->len = 3;
                          if (y == 4 || y == 6) {
                              // LD (nn),HL / LD (nn),A: a mapper write ends the block
                              if (!env->codemasters && I->nn >= 0xFFFC - (y == 4)) I->term = T_END_AFTER;
#ifdef GGSWAN_SMS
                              // (the Codemasters mapper's: 0000h, 4000h, 8000h)
                              if (env->codemasters && !(I->nn & 0x3FFF) && I->nn < 0xC000) I->term = T_END_AFTER;
#endif
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
                // (S, Z, P go through unchanged: live after = live before;
                // emit reads them from AH only when they are live)
                I->fwrite = FH | FN | FC; I->fread = y >= 2 ? FC : 0; return;
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
            if (y == 7) { I->term = T_END_AFTER; return; }       // LD SP,HL (sp_loaded_exit)
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
            case 2: case 3: I->nn = rd(pc + 1); I->len = 2;           // OUT (n),A / IN A,(n)
#ifndef GGSWAN_SG1000
                // (a read of the VDP status: does the game test the
                // sprite collision flag after it? gg.c)
                if (y == 3 && (I->nn & 0xC1) == 0x81)
                    gg_status_read_at(pc + 2);
#endif
                return;
            case 4: case 5: return;                                   // EX (SP),HL / EX DE,HL
            case 6: return;                                           // DI
            case 7: I->fread = FALL; return;   // EI (flags all live: the block may exit there)
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


/*
 * Wait loops: a block that only reads memory into registers and tests it,
 * then jumps back to its start (waiting for an interrupt to change a
 * variable, or JR $). Every turn does the same as the one before until
 * an interrupt, and interrupts only arrive between runs: the loop can end
 * the run at once, as if it had spun until then.
 */

static int idle_body(const ins_t *I) {
    uint8_t op = I->op;
    if (I->step) return 0;   // (interpreted: not known to be harmless)
    if (I->pre == 0xCB) return op >= 0x40 && op < 0x80;          // BIT b,r / (HL)
    if (I->pre == 0xDD || I->pre == 0xFD) {
        // BIT b,(IX+d) (Sonic waits for its frame interrupt with
        // BIT 0,(IY+0) / JR Z: a quarter of its time on the title screen,
        // 7% in the game); LD r,(IX+d), r not H or L
        if (I->cb) return (op & 0xC0) == 0x40;
        return op == 0x7E || op == 0x46 || op == 0x4E || op == 0x56 || op == 0x5E;
    }
    if (I->pre) return 0;
    return op == 0x00                                            // NOP
        || op == 0xFB                                            // EI (Spinball: EI / LD A,(nn) / CP B / JR NZ)
        || op == 0x3A || op == 0x7E || op == 0x0A || op == 0x1A  // LD A,(nn) (HL) (BC) (DE)
        || op == 0x46 || op == 0x4E || op == 0x56 || op == 0x5E  // LD B/C/D/E,(HL)
        || (op >= 0xA0 && op <= 0xA7) || op == 0xE6              // AND (x & m twice = once)
        || (op >= 0xB0 && op <= 0xBF) || op == 0xF6 || op == 0xFE;   // OR, CP
}

static int idle_rest(int n);
static int is_idle(int n, uint16_t pc, uint8_t ram) {
    const ins_t *L = &ins[n - 1];
    uint16_t target;
    if (L->pre) return 0;
    if (L->op == 0x20 || L->op == 0x28 || L->op == 0x30 || L->op == 0x38 || (L->op & 0xC7) == 0xC2) {
        // the loop's way out, the way back after it: LD A,(nn) / OR A /
        // JR Z,out / JR back (Batman Returns, a quarter of its time): the
        // block ends at the JR Z, its "not taken" exit is the wait
        uint16_t next = L->pc + ((L->op & 0xC7) == 0xC2 ? 3 : 2);
        if (!ram && (next >> 14) == (pc >> 14) && next < 0xC000
                && ((rd(next) == 0x18 && (uint16_t) (next + 2 + (int8_t) rd(next + 1)) == pc)
                    || (rd(next) == 0xC3 && (rd(next + 1) | (rd(next + 2) << 8)) == pc))
                && idle_rest(n - 1)) {
            idle_target = next;
            return 3;
        }
    }
    if (L->op == 0x18 || (L->op >= 0x20 && L->op <= 0x38 && !(L->op & 7)))
        target = (uint16_t) (L->pc + 2 + (int8_t) L->nn);       // JR, JR cc
    else if (L->op == 0xC3 || (L->op & 0xC7) == 0xC2)
        target = L->nn;                                          // JP, JP cc
    else
        return 0;
    if (target != pc) {
        // a loop through EI / JP pc (or JR back) in the same ROM slot, the
        // body here starting with DI, Super Tennis' (Agassi) wait for its
        // frame interrupt: DI / LD A,(nn) / CP n / JR NZ,x ... x: EI / JP
        // (three quarters of its time)
        if (ram || (target >> 14) != (pc >> 14) || target >= 0xC000
                || ins[0].pre || ins[0].op != 0xF3 || rd(target) != 0xFB)
            return 0;
        uint8_t j = rd(target + 1);
        if (!((j == 0xC3 && (rd(target + 2) | (rd(target + 3) << 8)) == pc)
              || (j == 0x18 && (uint16_t) (target + 3 + (int8_t) rd(target + 2)) == pc)))
            return 0;
        for (int i = 1; i < n - 1; i++)
            if (!idle_body(&ins[i])) return 0;
        idle_target = target;
        return 2;
    }
    return idle_rest(n - 1);
}

// the first n instructions only read and test
static int idle_rest(int n) {
    uint16_t pc = ins[0].pc;
    (void) pc;
    for (int i = 0; i < n; i++) {
        const ins_t *I = &ins[i];
        // a conditional branch out of the loop (its end): Batman Returns'
        // LD A,(nn) / OR A / JR Z,out / JR back (a third of its time)
        if (!I->pre && (I->op == 0x20 || I->op == 0x28 || I->op == 0x30 || I->op == 0x38
                        || (I->op & 0xC7) == 0xC2))
            continue;
        if (!idle_body(I)) {
#ifdef GGSWAN_HOST
            extern char *getenv(const char *);
            extern int printf(const char *, ...);
            if (getenv("GGSWAN_IDLELOG")) printf("NOIDLE %04X n %d at %d: %02X %02X\n", pc, n, i, I->pre, I->op);
#endif
            return 0;
        }
    }
#ifdef GGSWAN_HOST
    {
        extern char *getenv(const char *);
        extern int printf(const char *, ...);
        if (getenv("GGSWAN_IDLELOG")) printf("IDLE %04X n %d\n", pc, n + 1);
    }
#endif
    return 1;
}

// wait loop: end the run (cycles left: none)
static void idle_exit(uint16_t target, uint16_t extra) {
    materialize();
    sub_cycles(cyc + extra);
    if (idle_block == 2) {
        // DI ... EI / JP: an interrupt already waiting is taken right after
        // the EI's next instruction, not at the next event (ChatGPT's
        // review of 89-98)
        B(0x7E); B(0x0D);                     // jle +13
        B(0x80); B(0x3E); W(ZA(irq)); B(0);   // cmp byte [irq], 0
        B(0x75); B(0x06);                     // jne +6
    } else {
        B(0x7E); B(0x06);                     // jle +6
    }
    B(0xC7); B(0x06); W(SA(cycles)); W(0);    // mov word [cycles], 0
    B(0xC7); B(0x06); W(ZA(pc)); W(target);
    set_link(0);
    jmp_near(env->exit_off);
}

// Taken branch to target: loop back inside the block when possible
static void branch_to(uint16_t target, uint16_t extra) {
    if ((idle_block == 1 && target == block_pc) || (idle_block == 2 && target == idle_target)) {
        idle_exit(target, extra);
        return;
    }
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

/*
 * After an instruction that loaded SP (BP here). The translated code
 * reaches the stack in place, [bp + ram_delta]: the work RAM, C000-DFFF,
 * in IRAM. With SP anywhere else (the RAM's mirror at E000h, a table in
 * ROM read with POPs, the cartridge RAM) those were other bytes of IRAM:
 * the font, the tiles. So the block ends here, and with SP outside
 * C002h-E000h at an exit that is never linked: the runtime goes on with
 * the interpreter until SP is back (drc_rt.c, SP_OK). bad: known when
 * translating (LD SP,nn).
 */
static void sp_loaded_exit(uint16_t next, uint8_t bad) {
    if (FLAT) {   // (the stack is reached wherever it is)
        exit_const(next, 0);
        return;
    }
    if (!bad) {
        materialize();
        B(0x8D); B(0xB6); W((uint16_t) (0u - 0xC002u));   // lea si, [bp - C002h]
        B(0x81); B(0xFE); W(0x1FFE);                      // cmp si, 1FFEh
        uint8_t *out = jcc8(0x77);                        // ja out
        exit_const(next, 0);
        land8(out);
    }
    force_nolink = 1;
    exit_const(next, 0);
}

static void push_imm(uint16_t value) {
    B(0x8D); B(0x6E); B(0xFE);                                  // lea bp, [bp-2]
    FLAT_ES(); B(0xC7); B(0x86); W(env->ram_delta); W(value);              // mov word [bp+d], imm
}

// After a call to the OUTI/OUTD run routine (CF = 1: the rest to the
// helper, which makes the flags; CF = 0: AH = the last byte): the flags of
// the last OUTI, S and Z from B, N from bit 7 of the byte, only when live
static void run_flags(uint8_t h, int live) {
    if (!live && COLD_FREE()) {
        cold_add(jcc8(0x72), here(), COLD_HELPER, h, 0, 0, 0);   // jc cold
        hl_es = 0;                                       // (as after the helper)
        return;
    }
    if (!live) {
        uint8_t *done = jcc8(0x73);                      // jnc done
        helper(h);                                       // the OUTIs left (CRAM, other ports)
        land8(done);
        return;
    }
    uint8_t *slow = jcc8(0x72);                          // jc slow
    B(0x88); B(0x26); W(SA(val));                        // mov [val], ah
    B(0x84); B(0xED);                                    // test ch, ch
    B(0x9F);                                             // lahf
    B(0x80); B(0xE4); B(0xC0);                           // and ah, C0h
    B(0xF6); B(0x06); W(SA(val)); B(0x80);               // test byte [val], 80h
    B(0x74); B(0x03); B(0x80); B(0xCC); B(0x02);         // jz +3; or ah, 2
    if (COLD_FREE()) {
        // the OUTIs left (rare): the helper in the cold tail
        cold_add(slow, here(), COLD_HELPER, h, 0, 0, 0);
        hl_es = 0;
        return;
    }
    uint8_t *done = jcc8(0xEB);                          // jmp done
    land8(slow);
    helper(h);                                           // the OUTIs left (CRAM, other ports)
    land8(done);
}

#ifdef GGSWAN_SG1000
/*
 * sgswan: INI, IND, OUTI, OUTD (a run of them) and INIR, OTIR with the
 * TMS9918A's data port (C: 80h-BEh, even), a loop around the port's
 * routine (drc_emit_sg_stubs): the way games fill the VRAM and read it
 * back (Doki Doki Penguin Land: some 110 of them a frame, each an
 * interpreted step before). INI's bytes go to the work RAM in place.
 * Another port, or INI to somewhere else: the interpreter, as before.
 * Flags as the Game Gear's runs (run_flags): S and Z of B, N bit 7 of the
 * last byte. INIR and OTIR: up to 64 bytes, then the instruction again
 * (the cycles checked in between, as OTIR's on the Game Gear).
 */
static int sg_block_io(int i) {
    ins_t *I = &ins[i];
    uint8_t op = I->op;
    uint8_t in = !(op & 1), rep_ = (op & 0x10) != 0, down = (op & 0x08) != 0;
    uint16_t last = in ? SA(val) : VS(read_buffer);
    uint8_t *slow0, *slow1 = 0, *slow2 = 0;
    if (rep_) {
        materialize();
        B(0x89); B(0xCF);                                // mov di, cx
        B(0xC1); B(0xEF); B(0x08);                       // shr di, 8 (B)
        B(0x4F);                                         // dec di
        B(0x83); B(0xFF); B(63);                         // cmp di, 63
        B(0x72); B(0x03); B(0xBF); W(63);                // jb +3; mov di, 63
        B(0x47);                                         // inc di (B = 0: 64)
    } else {
        clobber(0);
        B(0xBF); W(I->count);                            // mov di, k
    }
    B(0x89); B(0xCE);                                    // mov si, cx
    B(0x81); B(0xE6); W(0x00C1);                         // and si, C1h
    B(0x81); B(0xFE); W(0x0080);                         // cmp si, 80h (the data port)
    slow0 = jcc8(0x75);                                  // jne slow
    if (in) {
        // the bytes' place: all of them in C000h-DFFFh
        B(0x8D); B(0xB7); W(0x4000);                     // lea si, [bx + 4000h] (HL - C000h)
        B(0x81); B(0xFE); W(0x2000);                     // cmp si, 2000h
        slow1 = jcc8(0x73);                              // jae slow
        if (down) {
            B(0x46);                                     // inc si
            B(0x39); B(0xFE);                            // cmp si, di
            slow2 = jcc8(0x72);                          // jb slow (below C000h)
        } else {
            B(0x01); B(0xFE);                            // add si, di
            B(0x81); B(0xFE); W(0x2000);                 // cmp si, 2000h
            slow2 = jcc8(0x77);                          // ja slow (past DFFFh)
        }
    }
    if (rep_)
        B(0x57);                                         // push di (the turns, for the cycles)
    B(0x89); B(0x3E); W(SA(arg));                        // mov [arg], di
    B(0x50);                                             // push ax (A)
    uint8_t *loop = o;
    if (in) {
        call_near(env->sg_in_off);
        FLAT_ES(); B(0x88); B(0x87); W(env->ram_delta);             // mov [bx + ram], al
    } else {
        B(0x89); B(0xDE);                                // mov si, bx
        B(0xC1); B(0xC6); B(0x04);                       // rol si, 4
        B(0x83); B(0xE6); B(0x0E);                       // and si, 14
        B(0x8E); B(0x84); W(SA(segtab));                 // mov es, [si + segtab]
        B(0x26); B(0x8A); B(0x07);                       // mov al, es:[bx]
        call_near(env->sg_out_off);
    }
    B(down ? 0x4B : 0x43);                               // dec / inc bx
    B(0xFE); B(0xCD);                                    // dec ch
    B(0xFF); B(0x0E); W(SA(arg));                        // dec word [arg]
    B(0x75); B((uint8_t) (loop - (o + 1)));              // jnz loop
    if (in) {
        B(0xA2); W(SA(val));                             // mov [val], al (the last byte)
    }
    B(0x58);                                             // pop ax
    B(0x84); B(0xED);                                    // test ch, ch
    B(0x9F);                                             // lahf
    B(0x80); B(0xE4); B(0xC0);                           // and ah, C0h
    B(0xF6); B(0x06); W(last); B(0x80);                  // test byte [last], 80h
    B(0x74); B(0x03); B(0x80); B(0xCC); B(0x02);         // jz +3; or ah, 2
    fkind = K_AH; hl_es = 0;
    if (!rep_) {
        uint8_t *done = jcc8(0xEB);                      // jmp done
        land8(slow0);
        if (slow1) { land8(slow1); land8(slow2); }
        // (the helper counts their cycles: not twice). Out, to another
        // port (the control port, the PSG): in C as the Game Gear's runs,
        // not as a step of the interpreter, which leaves the interrupt
        // request as it was before (a register written may raise it)
        B(0x81); B(0x06); W(SA(cycles)); W(16 * I->count);   // add word [cycles], 16 k
        B(0xC6); B(0x06); W(SA(val)); B(I->count);
        if (in) {
            B(0xC7); B(0x06); W(SA(arg)); W(I->pc);
            helper(DRC_H_STEP);
        } else {
            helper(down ? DRC_H_OUTD_RUN : DRC_H_OUTI_RUN);
        }
        land8(done);
        cyc += 16 * I->count;
        return 1;
    }
    B(0x5E);                                             // pop si (the turns)
    B(0x6B); B(0xF6); B(21);                             // imul si, si, 21
    B(0x29); B(0x36); W(SA(cycles));                     // sub [cycles], si
    B(0x84); B(0xED);                                    // test ch, ch
    uint8_t *fin = jcc8(0x74);                           // jz fin
    exit_const(I->pc, 0);                                // more to do
    land8(fin);
    B(0x83); B(0x06); W(SA(cycles)); B(5);               // add word [cycles], 5 (the last: 16)
    exit_const(I->pc + 2, 0);
    land8(slow0);
    if (slow1) { land8(slow1); land8(slow2); }
    if (!in) {
        // OTIR to another port (The Castle sets the registers this way):
        // this turn's bytes in C (see above), then as the other way
        B(0x57);                                         // push di (the turns)
        B(0x97);                                         // xchg ax, di
        B(0xA2); W(SA(val));                             // mov [val], al
        B(0x97);                                         // xchg ax, di
        B(0xC1); B(0xE7); B(0x04);                       // shl di, 4
        B(0x01); B(0x3E); W(SA(cycles));                 // add [cycles], di (the helper takes 16 each)
        helper(DRC_H_OUTI_RUN);
        fkind = K_AH;
        B(0x5E);                                         // pop si (the turns)
        B(0x6B); B(0xF6); B(21);                         // imul si, si, 21
        B(0x29); B(0x36); W(SA(cycles));                 // sub [cycles], si
        B(0x84); B(0xED);                                // test ch, ch
        uint8_t *fin2 = jcc8(0x74);                      // jz fin
        exit_const(I->pc, 0);
        land8(fin2);
        B(0x83); B(0x06); W(SA(cycles)); B(5);           // add word [cycles], 5
        exit_const(I->pc + 2, 0);
        return 1;
    }
    // INIR: all of it by the interpreter (it repeats the instruction itself)
    B(0xC7); B(0x06); W(SA(arg)); W(I->pc);
    B(0xC6); B(0x06); W(SA(val)); B(1);
    helper(DRC_H_STEP);
    fkind = K_AH;
    exit_const(I->pc + 2, 0);
    return 1;
}
#endif

// Returns 0 if the instruction could not be emitted (block ends before it)
static int emit1(int i);

static int emit(int i) {
    // (ES for HL - 1, after INC HL: for the next instruction only, if it
    // reads (HL); one that changes L would hide the step into the next
    // 8 KiB from seg_hl's test)
    uint8_t pend = hl_es == 2;
    int r = emit1(i);
    // (LD (HL),r and LD (HL),n use no ES and leave HL alone)
    if (pend && hl_es == 2 && !(ins[i].pre == 0 && (ins[i].op == 0x36 || (ins[i].op >= 0x70 && ins[i].op <= 0x77))))
        hl_es = 0;
    return r;
}

static int emit1(int i) {
    ins_t *I = &ins[i];
    // flags whose current value is still needed: read by this instruction,
    // or needed later and not rewritten by it
    uint8_t live = (live_out[i] & ~I->fwrite) | I->fread;
    uint8_t op = I->op;
    uint16_t next = I->pc + I->len;
#ifdef GGSWAN_SF7000
    flat_next = next;
#endif

    if (I->trace) {
        if (I->trace == 1) {   // the block goes on at the target
            cyc += op == 0x18 ? 12 : 10;
            return 1;
        }
        if (I->trace == 2) {   // CALL nn, routine inline: push the return address
            push_imm(next);
            cyc += 17;
            return 1;
        }
        // I->trace == 3: its RET: go on inline if it returns to the CALL
        materialize();
        FLAT_ES(); B(0x8B); B(0xB6); W(env->ram_delta);       // mov si, [bp+d]
        B(0x8D); B(0x6E); B(0x02);                 // lea bp, [bp+2]
        B(0x81); B(0xFE); W(I->nn);                // cmp si, return address
        uint8_t *same = jcc8(0x74);                // je: inline
        exit_reg(X_SI, 10);                        // (the stack was changed)
        land8(same);
        fkind = K_AH;
        cyc += 10;
        return 1;
    }

    if (I->step) {
        materialize();
        B(0xC7); B(0x06); W(SA(arg)); W(I->pc);
        B(0xC6); B(0x06); W(SA(val)); B(I->count);   // instructions
        helper(DRC_H_STEP);
        fkind = K_AH;
        if (I->dyn == 2) {
            // (sfswan, an instruction that gets written: on with the
            // block only if it left the Z80 at the next one)
            B(0x81); B(0x3E); W(ZA(pc)); W(next);        // cmp word [pc], next
            uint8_t *on = jcc8(0x74);                    // je on
            sub_cycles(cyc);
            set_link(0);
            jmp_near(env->exit_off);
            land8(on);
        }
        if (I->term == T_END_AFTER) { force_nolink = 1; exit_const(next, 0); }
        return 1;
    }

    if (!I->pre)
        goto unprefixed;

    if (I->pre == 0xCB) {
        uint8_t r = op & 7, b = (op >> 3) & 7;
        uint8_t xr = r8x[r];
        if (r == 4) hl_es = 0;   // H changes (except BIT: harmless; L: the same slot)
        if (op < 0x40) {
            uint8_t y = op >> 3;
            if (y <= 3) {
                // RLC RRC RL RR: rol ror rcl rcr
                if (y >= 2) {
                    if (fkind == K_AH) B(0x9E);   // sahf: CF = C
                }
                B(0xD0); B(0xC0 | (y << 3) | xr);
                if (!env->no_fast && !(live_out[i] & (FS | FZ | FP | FH | FN))) {
                    // only C is read after it (a chain of rotates through
                    // carry, 16/32-bit shifts): it stays in x86 CF
                    fkind = K_CARRY; cf_valid = 1;
                    cyc += 8;
                    return 1;
                }
                B(0x19); B(0xF6);                 // sbb si, si
                B(0x84); B(0xC0 | (xr << 3) | xr);// test r, r
                B(0x9F);                          // lahf
                B(0x80); B(0xE4); B(0xC4);        // and ah, C4h
                B(0xD1); B(0xEE);                 // shr si, 1
                B(0x80); B(0xD4); B(0x00);        // adc ah, 0
            } else {
                static const uint8_t sh[8] = { 0, 0, 0, 0, 4, 7, 0, 5 };   // SLA SRA - SRL
                B(0xD0); B(0xC0 | (sh[y] << 3) | xr);
                if (env->no_fast) {
                    B(0x9F); B(0x80); B(0xE4); B(0xC5);   // lahf; and ah, C5h
                    fkind = K_AH;
                    cyc += 8;
                    return 1;
                }
                // x86 shl/sar/shr by 1 set S, Z, P, C as the Z80 (H = N
                // = 0): the flags of an OR, kept in the x86 flags
                fkind = K_OR; cf_valid = 1;
                cyc += 8;
                return 1;
            }
            fkind = K_AH;
            cyc += 8;
            return 1;
        }
        if (op < 0x80) {
            // BIT b,r / BIT b,(HL)
            if (!env->no_fast && !(live_out[i] & (FALL & ~FZ))) {
                // only Z is read after it (BIT + JR/JP Z/NZ): TEST sets
                // x86 ZF = Z80 Z directly, no AH bit-twiddling or branch
                clobber(0);
                if (r == 6) seg_hl(0);
                if (r == 6) { B(0x26); B(0xF6); B(0x07); B(1 << b); }
                else { B(0xF6); B(0xC0 | xr); B(1 << b); }
                fkind = K_OR; cf_valid = 0;
                cyc += r == 6 ? 12 : 8;
                return 1;
            }
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
        uint8_t *j2, *j1 = mem_check(X_BX, &j2);
        FLAT_ES(); B(0x80); B(0x87 | (set ? 0x08 : 0x20)); W(env->ram_delta); B(imm);
        B(0x83); B(0x2E); W(SA(cycles)); B(15);
        if (!step_cold(j1, j2, I->pc)) {
            uint8_t *j3 = jcc8(0xEB);
            land8(j1); land8(j2);
            B(0xC7); B(0x06); W(SA(arg)); W(I->pc);
            B(0xC6); B(0x06); W(SA(val)); B(1);
            helper(DRC_H_STEP);
            land8(j3);
        }
        hl_es = 0;
        return 1;
    }

    if (I->pre == 0xED) {
#ifdef GGSWAN_SG1000
        if (env->sg_out_off && !FLAT && (op & 0xE6) == 0xA2)   // INI, IND, OUTI, OUTD, INIR, INDR, OTIR, OTDR
            if (op == 0xA2 || op == 0xAA || op == 0xA3 || op == 0xAB || op == 0xB2 || op == 0xB3)
                return sg_block_io(i);
#endif
        switch (op) {
        case 0x45: case 0x4D: case 0x55: case 0x5D:
        case 0x65: case 0x6D: case 0x75: case 0x7D:   // RETN, RETI: IFF1 = IFF2, return
            B(0x50);                                  // push ax
            B(0xA0); W(ZA(iff2));                     // mov al, [iff2]
            B(0xA2); W(ZA(iff1));                     // mov [iff1], al
            B(0x58);                                  // pop ax
            FLAT_ES(); B(0x8B); B(0xB6); W(env->ram_delta);      // mov si, [bp + d]
            B(0x8D); B(0x6E); B(0x02);                // lea bp, [bp + 2]
            cyc += 14;
            // back to the runtime: an interrupt may be pending now
            materialize();
            sub_cycles(cyc);
            B(0x89); B(0x36); W(ZA(pc));              // mov [pc], si
            set_link(0);
            jmp_near(env->exit_off);
            return 1;
        case 0x67: case 0x6F: {
            // RRD / RLD: the 12 bits A low, (HL) rotated by a nibble
            // (Sailor Moon S unpacks its graphics with them: 160 a frame,
            // each an interpreted step before). Work RAM directly, anything
            // else with the interpreter, as SET/RES (HL). Flags: S Z P of
            // the new A, H = N = 0, C kept; in AH.
            materialize();
            uint8_t *j2, *j1 = mem_check(X_BX, &j2);
            B(0x89); B(0xC7);                         // mov di, ax (F, A)
            if (op == 0x67) {
                // AX = 0 : A low : (HL), rotated right: AL = A low : (HL) high
                // (the new (HL)), AH high nibble = (HL) low (the new A low)
                B(0x88); B(0xC4);                     // mov ah, al
                B(0x80); B(0xE4); B(0x0F);            // and ah, 0Fh
                FLAT_ES(); B(0x8A); B(0x87); W(env->ram_delta);  // mov al, [bx + ram]
                B(0xC1); B(0xC8); B(0x04);            // ror ax, 4
                FLAT_ES(); B(0x88); B(0x87); W(env->ram_delta);  // mov [bx + ram], al
                B(0x88); B(0xE0);                     // mov al, ah
                B(0xC0); B(0xE8); B(0x04);            // shr al, 4
            } else {
                // AX = (HL) : A low : 0, rotated left: AH = (HL) low : A low
                // (the new (HL)), AL low nibble = (HL) high (the new A low)
                FLAT_ES(); B(0x8A); B(0xA7); W(env->ram_delta);  // mov ah, [bx + ram]
                B(0xC0); B(0xE0); B(0x04);            // shl al, 4
                B(0xC1); B(0xC0); B(0x04);            // rol ax, 4
                FLAT_ES(); B(0x88); B(0xA7); W(env->ram_delta);  // mov [bx + ram], ah
                B(0x24); B(0x0F);                     // and al, 0Fh
            }
            if (!(live_out[i] & FALL)) {
                // flags dead after it (Sailor Moon S: RRD / OR C): A only
                B(0x81); B(0xE7); W(0x00F0);          // and di, 00F0h (A high)
                B(0x09); B(0xF8);                     // or ax, di (AH: stale flags)
                B(0x83); B(0x2E); W(SA(cycles)); B(18);   // sub word [cycles], 18
                uint8_t *j4 = jcc8(0xEB);
                land8(j1); land8(j2);
                B(0xC7); B(0x06); W(SA(arg)); W(I->pc);
                B(0xC6); B(0x06); W(SA(val)); B(1);
                helper(DRC_H_STEP);
                land8(j4);
                hl_es = 0;
                fkind = K_AH;
                return 1;
            }
            B(0x89); B(0xFE);                         // mov si, di
            B(0x81); B(0xE6); W(0x00F0);              // and si, 00F0h (A high)
            B(0x09); B(0xF0);                         // or ax, si
            B(0x89); B(0xFE);                         // mov si, di
            B(0x81); B(0xE6); W(0x0100);              // and si, 0100h (C)
            B(0x08); B(0xC0);                         // or al, al (S Z P)
            B(0x9F);                                  // lahf
            B(0x80); B(0xE4); B(0xC4);                // and ah, C4h (S Z P)
            B(0x09); B(0xF0);                         // or ax, si (C)
            B(0x83); B(0x2E); W(SA(cycles)); B(18);   // sub word [cycles], 18
            uint8_t *j3 = jcc8(0xEB);
            land8(j1); land8(j2);
            B(0xC7); B(0x06); W(SA(arg)); W(I->pc);
            B(0xC6); B(0x06); W(SA(val)); B(1);
            helper(DRC_H_STEP);
            land8(j3);
            hl_es = 0;
            fkind = K_AH;
            return 1;
        }
        case 0x47:   // LD I,A (Wonder Boy III keeps a value in I)
            B(0xA2); W(ZA(i));                        // mov [i], al
            cyc += 9;
            return 1;
        case 0x57:   // LD A,I: S Z (and bits 5, 3) from I, P/V = IFF2, C kept
            clobber(live);
            B(0xA0); W(ZA(i));                        // mov al, [i]
            B(0x80); B(0xE4); B(0x01);                // and ah, 1 (C)
            B(0x89); B(0xC6);                         // mov si, ax
            B(0x81); B(0xE6); W(0x00A8);              // and si, 0A8h (S, 5, 3)
            B(0xC1); B(0xE6); B(0x08);                // shl si, 8
            B(0x09); B(0xF0);                         // or ax, si
            B(0x84); B(0xC0);                         // test al, al
            B(0x75); B(0x03);                         // jnz +3
            B(0x80); B(0xCC); B(0x40);                // or ah, 40h (Z)
            B(0x80); B(0x3E); W(ZA(iff2)); B(0);      // cmp byte [iff2], 0
            B(0x74); B(0x03);                         // je +3
            B(0x80); B(0xCC); B(0x04);                // or ah, 4 (P/V)
            fkind = K_AH;
            cyc += 9;
            return 1;
        case 0x40: case 0x48: case 0x50: case 0x58: case 0x60: case 0x68: case 0x78:
            if (I->d == 1) {
                // IN r,(C), C the VDP data port (known: see the decoder):
                // the prefetched byte as IN A,(BEh), into r; S Z P from it,
                // H N 0, C kept
                uint8_t dst = r8x[(op >> 3) & 7];
                clobber(live);
                B(0x80); B(0xE4); B(0x01);                      // and ah, 1 (C)
                B(0x50);                                        // push ax
                if (env->cls_off) {
                    B(0x80); B(0x3E); W(SA(defer)); B(0);       // cmp byte [defer], 0
                    uint8_t *nd = jcc8(0x74);                   // je
                    B(0xC6); B(0x06); W(SA(op)); B(DRC_H_SYNC); // mov byte [op], SYNC
                    call_near(env->helper_off);
                    land8(nd);
                    hl_es = 0;
                }
                B(0x8B); B(0x36); W(VA(addr));                  // mov si, [addr]
                B(0x8A); B(0x84); W(env->vram_addr);            // mov al, [si + vram]
                B(0x86); B(0x06); W(VA(read_buffer));           // xchg al, [read_buffer]
                B(0x46);                                        // inc si
                B(0x81); B(0xE6); W(0x3FFF);                    // and si, 3FFFh
                B(0x89); B(0x36); W(VA(addr));                  // mov [addr], si
                B(0xC6); B(0x06); W(VA(latch_full)); B(0);      // latch_full = 0
                if (dst) { B(0x88); B(0xC0 | dst); }            // mov r, al
                B(0x84); B(0xC0);                               // test al, al
                B(0x9F);                                        // lahf (S Z P: as the Z80's)
                B(0x80); B(0xE4); B(0xC4);                      // and ah, C4h
                B(0x89); B(0xE6);                               // mov si, sp
                B(0x36); B(0x0A); B(0x64); B(0x01);             // or ah, ss:[si + 1] (C)
                if (dst) { B(0x36); B(0x8A); B(0x04); }         // mov al, ss:[si] (A)
                B(0x5E);                                        // pop si
                if (dst == X_BX || dst == 7) hl_es = 0;
                fkind = K_AH;
                cyc += 12;
                return 1;
            }
            return 0;
        case 0x5F:   // LD A,R: as the interpreter (the step it was, ~1800
                     // a minute in Aladdin): R + 2 (the two opcode
                     // fetches, bit 7 kept), A = R, flags as LD A,I. The
                     // translated code does not count R otherwise: the
                     // cycles left in the run (their low byte) stand for
                     // the instructions since the last time, or R would
                     // go 2, 4, 6... for a game that draws its random
                     // numbers from it.
            clobber(live);
            B(0x50);                                  // push ax
            B(0xA0); W(ZA(r));                        // mov al, [r]
            B(0x88); B(0xC4);                         // mov ah, al
            B(0x80); B(0xE4); B(0x80);                // and ah, 80h
            B(0x04); B(0x02);                         // add al, 2
            B(0x2A); B(0x06); W(SA(cycles));          // sub al, [cycles]
            B(0x24); B(0x7F);                         // and al, 7Fh
            B(0x08); B(0xE0);                         // or al, ah
            B(0xA2); W(ZA(r));                        // mov [r], al
            B(0x58);                                  // pop ax
            B(0xA0); W(ZA(r));                        // mov al, [r]
            B(0x80); B(0xE4); B(0x01);                // and ah, 1 (C)
            B(0x89); B(0xC6);                         // mov si, ax
            B(0x81); B(0xE6); W(0x00A8);              // and si, 0A8h (S, 5, 3)
            B(0xC1); B(0xE6); B(0x08);                // shl si, 8
            B(0x09); B(0xF0);                         // or ax, si
            B(0x84); B(0xC0);                         // test al, al
            B(0x75); B(0x03);                         // jnz +3
            B(0x80); B(0xCC); B(0x40);                // or ah, 40h (Z)
            B(0x80); B(0x3E); W(ZA(iff2)); B(0);      // cmp byte [iff2], 0
            B(0x74); B(0x03);                         // je +3
            B(0x80); B(0xCC); B(0x04);                // or ah, 4 (P/V)
            fkind = K_AH;
            cyc += 9;
            return 1;
        case 0x44:   // NEG
            B(0xF6); B(0xD8);
            fkind = K_SUB; cf_valid = 1; cyc += 8;
            return 1;
        case 0x41: case 0x49: case 0x51: case 0x59: case 0x61: case 0x69: case 0x79: {   // OUT (C),r
            clobber(live);
            uint8_t src = r8x[(op >> 3) & 7];
            uint8_t *s0 = 0, *s1 = 0, *s2 = 0, *done = 0, *done2 = 0;
#ifdef GGSWAN_SG1000
            if (env->sg_out_off) {
                // the TMS9918A's ports (80h-BFh: even data, odd control)
                B(0x89); B(0xCE);                        // mov si, cx
                B(0x81); B(0xE6); W(0x00C1);             // and si, C1h
                B(0x81); B(0xFE); W(0x0080);             // cmp si, 80h
                s0 = jcc8(0x75);
                if (src) { B(0x50); B(0x88); B(0xC0 | (src << 3)); }   // push ax; mov al, r
                call_near(env->sg_out_off);
                if (src) B(0x58);                        // pop ax
                done = jcc8(0xEB);
                land8(s0);
                B(0x81); B(0xFE); W(0x0081);             // cmp si, 81h
                s2 = jcc8(0x75);
                if (src) { B(0x50); B(0x88); B(0xC0 | (src << 3)); }   // push ax; mov al, r
                call_near(env->sg_ctl_off);
                if (src) B(0x58);                        // pop ax
                done2 = jcc8(0x73);                      // jnc done
                land8(s2);
                hl_es = 0;
            }
#endif
            if (env->vdp_addr) {
                B(0x89); B(0xCE);                        // mov si, cx
                B(0x81); B(0xE6); W(0x00C1);             // and si, C1h
                B(0x81); B(0xFE); W(0x0080);             // cmp si, 80h (VDP data)
                s0 = jcc8(0x75);
                vram_fast(src, &s1, 1);
                done = jcc8(0xEB);
                land8(s0);
                if (env->ctl_off) {
                    B(0x81); B(0xFE); W(0x0081);         // cmp si, 81h (VDP control)
                    s2 = jcc8(0x75);
                    if (src) { B(0x50); B(0x88); B(0xC0 | (src << 3)); }   // push ax; mov al, r
                    call_near(env->ctl_off);
                    if (src) B(0x58);                    // pop ax
                    done2 = jcc8(0x73);                  // jnc done
                    land8(s2);
                }
                land8(s1);
            }
            B(0x88); B(0x06 | (src << 3)); W(SA(val));
            B(0x88); B(0x0E); W(SA(arg));
            helper(DRC_H_OUT);
            if (done) land8(done);
            if (done2) land8(done2);
            cyc += 12;
            return 1;
        }
        case 0x42: case 0x52: case 0x62: case 0x72:     // SBC HL,rr
        case 0x4A: case 0x5A: case 0x6A: case 0x7A: {   // ADC HL,rr
            uint8_t rr = (op >> 4) & 3, sbc = !(op & 8);
            need_cf();                                       // (CF = C)
            if (!env->no_fast && !(live_out[i] & FH)) {
                // H not read: x86 SBB / ADC gives S, Z, overflow (P/V), C
                // as the Z80's; the flags stay in x86 flags (Gemini)
                B(sbc ? 0x19 : 0x11); B(0xC3 | (r16x[rr] << 3));  // sbb/adc bx, rr
                fkind = sbc ? K_SUB : K_ADD; cf_valid = 1; hl_es = 0;
                cyc += 15;
                return 1;
            }
            if (rr != 2) { B(0x89); B(0xDE); }               // mov si, bx
            B(sbc ? 0x19 : 0x11); B(0xC3 | (r16x[rr] << 3));  // sbb/adc bx, rr
            {
                uint8_t mask = sbc ? 0xC3 : 0xC1;            // (lahf: bit 1 = 1, N = sbc)
                B(0x9F);                                     // lahf (S Z C)
                if ((live_out[i] & FP) && COLD_FREE()) {
                    uint8_t *j = jcc8(0x70);                 // jo cold
                    B(0x80); B(0xE4); B(mask);               // and ah, mask (V = 0)
                    cold_add(j, here(), COLD_PV, mask, 0, 0, 0);
                } else if (live_out[i] & FP) {
                    B(0x70); B(0x05);                        // jo +5
                    B(0x80); B(0xE4); B(mask);               // and ah, mask (V = 0)
                    B(0xEB); B(0x06);                        // jmp +6
                    B(0x80); B(0xE4); B(mask);               // and ah, mask
                    B(0x80); B(0xCC); B(0x04);               // or ah, 4 (V = 1)
                } else {
                    B(0x80); B(0xE4); B(mask);               // and ah, mask (V not read)
                }
            }
            if (rr == 2) {
                B(0xF7); B(0xC3); W(0x1000);                 // test bx, 1000h
            } else {
                B(0x31); B(0xC6 | (r16x[rr] << 3));          // xor si, rr
                B(0x31); B(0xDE);                            // xor si, bx
                B(0xF7); B(0xC6); W(0x1000);                 // test si, 1000h
            }
            B(0x74); B(0x03); B(0x80); B(0xCC); B(0x10);     // jz +3; or ah, 10h (H)
            fkind = K_AH; hl_es = 0;
            cyc += 15;
            return 1;
        }
        case 0xA0: case 0xA8: {   // a run of LDI / LDD (routine in the stubs)
            if (I->count == 1 && !env->no_fast) {
                /*
                 * One alone, to work RAM: here. Through the routine it
                 * was some 55 instructions for the byte (the registers
                 * saved, the run's checks, REP MOVSB set up, the flags):
                 * Sonic's map loop has four of them in each turn, about
                 * 60 a frame while playing. S, Z, C kept, H = N = 0,
                 * P/V = BC != 0, only if a flag is read afterwards (AH is
                 * then the byte's way from ROM to RAM).
                 */
                uint8_t after = live_out[i] & FALL;
                clobber(live);
                seg_hl(0);
                uint8_t *s1, *s0 = mem_check(X_DX, &s1);
                B(0x89); B(0xD6);                            // mov si, dx
                if (after) {
                    B(0x50);                                 // push ax
                    B(0x26); B(0x8A); B(0x07);               // mov al, es:[bx]
                    FLAT_ES(); B(0x88); B(0x84); W(env->ram_delta);     // mov [si + ram], al
                    B(0x58);                                 // pop ax
                } else {
                    B(0x26); B(0x8A); B(0x27);               // mov ah, es:[bx]
                    FLAT_ES(); B(0x88); B(0xA4); W(env->ram_delta);     // mov [si + ram], ah
                }
                B(op == 0xA0 ? 0x43 : 0x4B);                 // inc / dec bx
                B(op == 0xA0 ? 0x42 : 0x4A);                 // inc / dec dx
                if (after) { B(0x80); B(0xE4); B(0xC1); }    // and ah, C1h
                B(0x49);                                     // dec cx
                if (after) { B(0x74); B(0x03); B(0x80); B(0xCC); B(0x04); }   // jz +3; or ah, 4
                uint8_t *fin = jcc8(0xEB);                   // jmp fin
                // (the destination elsewhere: the routine, which leaves
                // it to the interpreter)
                land8(s0); land8(s1);
                B(0xBF); W(1);                               // mov di, 1
                B(0xBE); W(I->pc);                           // mov si, pc
                call_near(op == 0xA0 ? env->ldi_off : env->ldi_off + DRC_LDD_DELTA);
                uint8_t *done = jcc8(0x73);                  // jnc done
                helper(DRC_H_STEP);
                land8(done);
                land8(fin);
                fkind = K_AH; hl_es = 0;
                cyc += 16;
                return 1;
            }
            materialize();
            B(0xBF); W(I->count);                            // mov di, k
            B(0xBE); W(I->pc);                               // mov si, pc
            call_near(op == 0xA0 ? env->ldi_off : env->ldi_off + DRC_LDD_DELTA);
            if (COLD_FREE()) {
                // the LDIs left (not work RAM: rare): the cold tail
                cold_add(jcc8(0x72), here(), COLD_HELPER, DRC_H_STEP, 0, 0, 0);
            } else {
                uint8_t *done = jcc8(0x73);                  // jnc done
                helper(DRC_H_STEP);                          // the LDIs left
                land8(done);
            }
            fkind = K_AH; hl_es = 0;
            cyc += 16 * I->count;
            return 1;
        }
        case 0xB0: case 0xB8: {   // LDIR / LDDR (the LDI routine, up to 64 per turn)
            materialize();
            B(0x89); B(0xCF);                                // mov di, cx
            B(0x4F);                                         // dec di
            B(0x83); B(0xFF); B(63);                         // cmp di, 63
            B(0x72); B(0x03); B(0xBF); W(63);                // jb +3; mov di, 63
            B(0x47);                                         // inc di (BC = 0: 64)
            B(0x57);                                         // push di (k)
            B(0xBE); W(I->pc);                               // mov si, pc
            call_near(op == 0xB0 ? env->ldi_off : env->ldi_off + DRC_LDD_DELTA);
            B(0x5E);                                         // pop si (k)
            uint8_t *slow = jcc8(0x72);                      // jc slow
            B(0x6B); B(0xF6); B(21);                         // imul si, si, 21
            B(0x29); B(0x36); W(SA(cycles));                 // sub [cycles], si
            fkind = K_AH;
            B(0x85); B(0xC9);                                // test cx, cx
            uint8_t *fin = jcc8(0x74);                       // jz fin
            exit_const(I->pc, 0);                            // more to copy
            land8(fin);
            B(0x83); B(0x06); W(SA(cycles)); B(5);           // add [cycles], 5 (the last: 16)
            exit_const(I->pc + 2, 0);
            // destination outside work RAM: the ones done, then the rest
            // with the interpreter
            land8(slow);
            // (val is a byte, the one after it the low byte of arg, where
            // the routine has put an address: read as a word, the cycles
            // were off by 21 * 256 times that byte, up or down: a game
            // lost or gained most of a frame at every such LDIR)
            B(0x8B); B(0x3E); W(SA(val));                    // mov di, [val]
            B(0x81); B(0xE7); W(0x00FF);                     // and di, FFh (the ones left)
            B(0x29); B(0xFE);                                // sub si, di (done)
            B(0x6B); B(0xF6); B(21);                         // imul si, si, 21
            B(0xC1); B(0xE7); B(0x04);                       // shl di, 4
            B(0x01); B(0xFE);                                // add si, di (undo the routine's)
            B(0x29); B(0x36); W(SA(cycles));                 // sub [cycles], si
            B(0xC7); B(0x06); W(SA(val)); W(1);              // val = 1
            B(0xC7); B(0x06); W(SA(arg)); W(I->pc);          // arg = pc
            helper(DRC_H_STEP);
            fkind = K_AH;
            // (the step is not one turn: the interpreter goes on until
            // the instruction is over, BC = 0. The block went back to the
            // instruction, which found BC = 0 and copied 64 KiB more,
            // through all of the memory: any LDIR or LDDR to the
            // cartridge RAM or to ROM, in all three builds up to patch
            // 143 (sgswan's The Castle fills its cartridge RAM this way,
            // and stopped for a third of a minute with its memory
            // overwritten: how it was found). mkldirtest.py)
            exit_const(I->pc + 2, 0);
            return 1;
        }
        case 0xB3: {   // OTIR (the OUTI routine, up to 64 per turn)
            // (also OUTI / JR NZ or JP NZ back to it, see decode: up to
            // 32 per turn, their cycles in nn and d)
            uint8_t most = I->len == 2 ? 64 : 32;
            materialize();
            B(0x89); B(0xCF);                                // mov di, cx
            B(0xC1); B(0xEF); B(0x08);                       // shr di, 8 (B)
            B(0x4F);                                         // dec di
            B(0x83); B(0xFF); B(most - 1);                   // cmp di, 63
            B(0x72); B(0x03); B(0xBF); W(most - 1);          // jb +3; mov di, 63
            B(0x47);                                         // inc di (B = 0: 64)
            if (I->len != 2) {
                // the loop: no more turns than the cycles left pay for
                // (cycles / 32 + 1, their 28 or 26 each: it ends less
                // than a turn past them, as the block for each turn did;
                // an interrupt is taken where it was)
                B(0x8B); B(0x36); W(SA(cycles));             // mov si, [cycles]
                if (cyc) { B(0x81); B(0xEE); W(cyc); }       // sub si, the block's so far
                B(0xC1); B(0xFE); B(0x05);                   // sar si, 5
                B(0x46);                                     // inc si
                B(0x83); B(0xFE); B(0x01);                   // cmp si, 1
                B(0x7D); B(0x03); B(0xBE); W(1);             // jge +3; mov si, 1
                B(0x39); B(0xF7);                            // cmp di, si
                B(0x76); B(0x02); B(0x89); B(0xF7);          // jbe +2; mov di, si
            }
            B(0x57);                                         // push di (k)
            B(0xBE); W(I->pc);                               // mov si, pc
            call_near(env->outi_off);
            run_flags(DRC_H_OUTI_RUN, 1);
            B(0x5E);                                         // pop si (k)
            B(0x6B); B(0xF6); B((uint8_t) I->nn);            // imul si, si, 21
            B(0x29); B(0x36); W(SA(cycles));                 // sub [cycles], si
            fkind = K_AH; hl_es = 0;
            B(0x84); B(0xED);                                // test ch, ch
            uint8_t *fin = jcc8(0x74);                       // jz fin
            exit_const(I->pc, 0);                            // more to send
            land8(fin);
            if (I->d) {
                B(0x83); B(0x06); W(SA(cycles)); B((uint8_t) I->d);   // add [cycles], 5 (the last: 16)
            }
            exit_const(I->pc + I->len, 0);
            return 1;
        }
        case 0xA3: case 0xAB: {   // a run of OUTI / OUTD (routine in the stubs)
            clobber(0);
            uint8_t *ntw = 0;
            if (op == 0xA3 && I->count == 2 && env->ntw_off) {
                // two: a name table entry, most often (its own routine)
                call_near(env->ntw_off);
                ntw = jcc8(0x73);                            // jnc done
            }
            B(0xBF); W(I->count);                            // mov di, k
            B(0xBE); W(I->pc);                               // mov si, pc
            call_near(op != 0xA3 ? env->outi_off + DRC_OUTD_DELTA
                : I->count == 1 && env->outi1_off ? env->outi1_off : env->outi_off);
            land8(ntw);   // (CF = 0, as from the run routine)
            run_flags(op == 0xA3 ? DRC_H_OUTI_RUN : DRC_H_OUTD_RUN, live_out[i] & FALL);
            fkind = K_AH; hl_es = 0;
            cyc += 16 * I->count;
            return 1;
        }
        case 0x43: case 0x53: case 0x73: {   // LD (nn),rr
            uint8_t xr = r16x[(op >> 4) & 3];
            if (FLAT) {
                flat_write16(I->nn, xr, live);
                cyc += 20;
                return 1;
            }
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
            if (op == 0x7B) sp_loaded_exit(next, 0);
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
                uint8_t z_only = !env->no_fast && !(live_out[i] & (FALL & ~FZ));
                if (z_only) {
                    clobber(0);
                } else {
                    need_ah(live);
                    B(0x80); B(0xE4); B(0x01);               // and ah, 1
                    B(0x80); B(0xCC); B(0x10);               // or ah, 10h
                }
                if (env->no_fast || FLAT) {
                    ix_addr(I->pre, I->d);
                    seg_si(0);
                    B(0x26); B(0xF6); B(0x04); B(1 << b);
                } else if (!cold_off) {
                    uint8_t *j = ix_ram_cold(I->pre, 0);
                    B(0xF6); B(0x84); W((uint16_t) (I->d + (int16_t) env->ram_delta)); B(1 << b);
                    cold_add(j, here(), COLD_IX_BIT, b, 0, I->d, 0);
                } else {
                    uint8_t *f = ix_ram(I->pre, 0);
                    ix_far(I->d);
                    B(0x26); B(0xF6); B(0x04); B(1 << b);    // test byte es:[si], mask
                    B(0x07);                                 // pop es
                    uint8_t *done = jcc8(0xEB);
                    land8(f);
                    FLAT_ES(); B(0xF6); B(0x84); W((uint16_t) (I->d + (int16_t) env->ram_delta)); B(1 << b);
                    land8(done);
                }
                if (z_only) {
                    fkind = K_OR; cf_valid = 0;
                    cyc += 20;
                    return 1;
                }
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
            uint8_t *j2, *j1 = mem_check(X_SI, &j2);
            FLAT_ES(); B(0x80); B(0x84 | (set ? 0x08 : 0x20)); W(env->ram_delta); B(imm);
            B(0x83); B(0x2E); W(SA(cycles)); B(23);
            if (!step_cold(j1, j2, I->pc)) {
                uint8_t *j3 = jcc8(0xEB);
                land8(j1); land8(j2);
                B(0xC7); B(0x06); W(SA(arg)); W(I->pc);
                B(0xC6); B(0x06); W(SA(val)); B(1);
                helper(DRC_H_STEP);
                land8(j3);
            }
            return 1;
        }
        switch (op) {
        case 0x21: B(0xC7); B(0x06); W(ixa); W(I->nn); cyc += 14; return 1;
        case 0x2A: read_const16(I->nn, X_SI); B(0x89); B(0x36); W(ixa); cyc += 20; return 1;
        case 0x22:
            if (FLAT) {
                clobber(live);
                B(0x8B); B(0x36); W(ixa);
                flat_write16(I->nn, X_SI, 0);
                cyc += 20;
                return 1;
            }
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
            FLAT_ES(); B(0x89); B(0xB6); W(env->ram_delta);
            cyc += 15;
            return 1;
        case 0xE1:   // POP IX
            FLAT_ES(); B(0x8B); B(0xB6); W(env->ram_delta);
            B(0x8D); B(0x6E); B(0x02);
            B(0x89); B(0x36); W(ixa);
            cyc += 14;
            return 1;
        case 0xE3:   // EX (SP),IX
            B(0x8B); B(0x36); W(ixa);
            FLAT_ES(); B(0x87); B(0xB6); W(env->ram_delta);
            B(0x89); B(0x36); W(ixa);
            cyc += 23;
            return 1;
        case 0xF9: B(0x8B); B(0x2E); W(ixa); cyc += 10; sp_loaded_exit(next, 0); return 1;   // LD SP,IX
        case 0xE9:   // JP (IX)
            B(0x8B); B(0x36); W(ixa);
            cyc += 8;
            exit_reg(X_SI, 0);
            return 1;
        case 0x09: case 0x19: case 0x29: case 0x39: {   // ADD IX,rr
            uint8_t rr = (op >> 4) & 3;
            if (!env->no_fast && !(live_out[i] & (FS | FZ | FP | FH | FN))) {
                // only C read afterwards: in place (Gemini)
                clobber(0);
                if (rr == 2) { B(0xD1); B(0x26); W(ixa); }                // shl word [ix], 1
                else { B(0x01); B(0x06 | (r16x[rr] << 3)); W(ixa); }      // add [ix], rr
                fkind = K_CARRY; cf_valid = 1;
                cyc += 15;
                return 1;
            }
            uint8_t hl = (live_out[i] & FH) != 0;        // (H read afterwards)
            need_ah(live);
            B(0x8B); B(0x3E); W(ixa);                    // mov di, [ix]
            B(0x80); B(0xE4); B(0xC4);                   // and ah, C4h
            if (hl) { B(0x89); B(0xFE); }                // mov si, di
            if (rr == 2) { B(0x01); B(0xFF); }           // add di, di
            else { B(0x01); B(0xC7 | (r16x[rr] << 3)); } // add di, rr
            B(0x80); B(0xD4); B(0x00);                   // adc ah, 0
            if (hl) {
                if (rr == 2) {
                    B(0xF7); B(0xC6); W(0x0800);         // test si, 0800h
                } else {
                    B(0x31); B(0xC6 | (r16x[rr] << 3));  // xor si, rr
                    B(0x31); B(0xFE);                    // xor si, di
                    B(0xF7); B(0xC6); W(0x1000);
                }
                B(0x74); B(0x03); B(0x80); B(0xCC); B(0x10);
            }
            B(0x89); B(0x3E); W(ixa);
            fkind = K_AH;
            cyc += 15;
            return 1;
        }
        case 0x34: case 0x35: {   // INC/DEC (IX+d): work RAM directly
            uint8_t dec = op == 0x35;
            materialize();
            ix_addr(I->pre, I->d);
            uint8_t *j2, *j1 = mem_check(X_SI, &j2);
            B(0x9E);                                              // sahf
            FLAT_ES(); B(0xFE); B(0x84 | (dec ? 0x08 : 0)); W(env->ram_delta);   // inc/dec byte [si+d]
            fkind = dec ? K_DEC : K_INC; cf_valid = 1;
            materialize();
            B(0x83); B(0x2E); W(SA(cycles)); B(23);
            if (!step_cold(j1, j2, I->pc)) {
                uint8_t *j3 = jcc8(0xEB);
                land8(j1); land8(j2);
                B(0xC7); B(0x06); W(SA(arg)); W(I->pc);
                B(0xC6); B(0x06); W(SA(val)); B(1);
                helper(DRC_H_STEP);
                land8(j3);
            }
            fkind = K_AH;
            return 1;
        }
        case 0x36:   // LD (IX+d),n
            if (env->no_fast) {
                ix_addr(I->pre, I->d);
                write_byte(X_SI, 0xFF, I->nn, live);
            } else
                ix_write(I->pre, I->d, 0xFF, I->nn, live);
            cyc += 19;
            return 1;
        }
        if (op >= 0x70 && op <= 0x77) {   // LD (IX+d),r
            if (env->no_fast) {
                ix_addr(I->pre, I->d);
                write_byte(X_SI, r8x[op & 7], 0, live);
            } else
                ix_write(I->pre, I->d, r8x[op & 7], 0, live);
            cyc += 19;
            return 1;
        }
        if ((op & 7) == 6 && op >= 0x46 && op <= 0x7E) {   // LD r,(IX+d)
            uint8_t dst = r8x[(op >> 3) & 7];
            if (env->no_fast || FLAT) {
                ix_addr(I->pre, I->d);
                seg_si(live);
                B(0x26); B(0x8A); B(0x04 | (dst << 3));
            } else if (!cold_off) {
                uint8_t *j = ix_ram_cold(I->pre, live);
                B(0x8A); B(0x84 | (dst << 3)); W((uint16_t) (I->d + (int16_t) env->ram_delta));
                cold_add(j, here(), COLD_IX_LD, dst, 0, I->d, 0);
            } else {
                uint8_t *f = ix_ram(I->pre, live);
                ix_far(I->d);
                B(0x26); B(0x8A); B(0x04 | (dst << 3));      // mov r, es:[si]
                B(0x07);                                     // pop es
                uint8_t *done = jcc8(0xEB);
                land8(f);
                FLAT_ES(); B(0x8A); B(0x84 | (dst << 3)); W((uint16_t) (I->d + (int16_t) env->ram_delta));
                land8(done);
            }
            if (dst == X_BX || dst == 7) hl_es = 0;
            cyc += 19;
            return 1;
        }
        {
            // IXH/IXL (decode): the byte in z80_t (ixh after ixl)
            uint8_t x2 = op >> 6, y2 = (op >> 3) & 7, z2 = op & 7;
            uint8_t hy = y2 == 4 || y2 == 5, hz = z2 == 4 || z2 == 5;
            if (x2 == 1 && (hy || hz) && !(hy && hz) && y2 != 6 && z2 != 6) {
                if (hz) {   // LD r,IXh
                    B(0x8A); B(0x06 | (r8x[y2] << 3)); W(ixa + (z2 == 4));
                } else {    // LD IXh,r
                    B(0x88); B(0x06 | (r8x[z2] << 3)); W(ixa + (y2 == 4));
                }
                cyc += 8;
                return 1;
            }
            if (x2 == 2 && hz) {   // ALU A,IXh
                if (y2 == 1 || y2 == 3) need_cf();
                B(alu_x86[y2] + 2); B(0x06); W(ixa + (z2 == 4));
                alu_done(y2);
                cyc += 8;
                return 1;
            }
        }
        if (op >= 0x86 && op <= 0xBE) {   // ALU A,(IX+d)
            uint8_t y = (op >> 3) & 7;
            if (env->no_fast || FLAT) {
                ix_addr(I->pre, I->d);
                seg_si(live);
                if (y == 1 || y == 3) need_cf();
                B(0x26); B(alu_x86[y] + 2); B(0x04);
            } else if (!cold_off) {
                uint8_t *j = ix_ram_cold(I->pre, live);
                if (y == 1 || y == 3) need_cf();
                B(alu_x86[y] + 2); B(0x84); W((uint16_t) (I->d + (int16_t) env->ram_delta));
                cold_add(j, here(), COLD_IX_ALU, y, 0, I->d, 0);
            } else {
                // (POP ES and the jump leave the operation's flags alone)
                uint8_t *f = ix_ram(I->pre, live);
                ix_far(I->d);
                if (y == 1 || y == 3) need_cf();
                B(0x26); B(alu_x86[y] + 2); B(0x04);         // op al, es:[si]
                B(0x07);                                     // pop es
                uint8_t *done = jcc8(0xEB);
                land8(f);
                if (y == 1 || y == 3) need_cf();
                FLAT_ES(); B(alu_x86[y] + 2); B(0x84); W((uint16_t) (I->d + (int16_t) env->ram_delta));
                land8(done);
            }
            alu_done(y);
            cyc += 19;
            return 1;
        }
        return 0;
    }

unprefixed:;
    uint8_t x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    switch (x) {
    case 1: {   // LD r,r'
        if (z == 6) {
            seg_hl(live);
            B(0x26); B(0x8A); B(0x07 | (r8x[y] << 3));
            if (y == 4) hl_es = 0;   // (L: the same slot)
            cyc += 7;
        } else if (y == 6) {
            write_byte(X_BX, r8x[z], 0, live);
            cyc += 7;
        } else {
            if (y != z) { B(0x8A); B(0xC0 | (r8x[y] << 3) | r8x[z]); }
            if (y == 4) hl_es = 0;   // (L: the same slot)
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
                // (three MOVs: 3 cycles, XCHG with memory takes 5)
                B(0x8B); B(0x36); W(ZA(a_));         // mov si, [af']
                B(0xA3); W(ZA(a_));                  // mov [af'], ax
                B(0x89); B(0xF0);                    // mov ax, si
                cyc += 4;
                return 1;
            }
            {
                uint16_t target = next + (int8_t) I->nn;
                if (y == 2 && I->nn == 0xFE && !env->no_fast) {
                    /*
                     * DJNZ $: a delay of B steps (Sailor Moon S times its
                     * voice samples with it, ~570 a frame): in one go,
                     * 13 (B - 1) + 8 cycles, B = 0 (B = 0 at the start:
                     * 256 steps), the flags as they were
                     */
                    clobber(live);
                    B(0x89); B(0xCE);                    // mov si, cx
                    B(0xC1); B(0xEE); B(0x08);           // shr si, 8 (B)
                    B(0x4E);                             // dec si
                    B(0x81); B(0xE6); W(0x00FF);         // and si, FFh (B - 1, 0: 255)
                    B(0x6B); B(0xF6); B(13);             // imul si, si, 13
                    B(0x29); B(0x36); W(SA(cycles));     // sub [cycles], si
                    B(0xB5); B(0x00);                    // mov ch, 0
                    cf_valid = 0;
                    exit_const(next, 8);
                    return 1;
                }
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
                if (target == block_pc) {
                    // a loop: going on (the common case) falls through
                    uint8_t *j = jcond((y - 4) ^ 1);
                    uint8_t save_kind = fkind;
                    branch_to(target, 12);
                    land8(j);
                    fkind = save_kind;
                    exit_const(next, 7);
                    return 1;
                }
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
                uint8_t hl = (live_out[i] & FH) != 0;      // (H read afterwards)
                need_ah(live);
                B(0x80); B(0xE4); B(0xC4);
                if (hl) { B(0x89); B(0xDE); }              // mov si, bx
                B(0x01); B(0xC3 | (r16x[rr] << 3));        // add bx, rr
                B(0x80); B(0xD4); B(0x00);
                if (hl) {
                    if (rr == 2) {
                        B(0xF7); B(0xC6); W(0x0800);
                    } else {
                        B(0x31); B(0xC6 | (r16x[rr] << 3));
                        B(0x31); B(0xDE);
                        B(0xF7); B(0xC6); W(0x1000);
                    }
                    B(0x74); B(0x03); B(0x80); B(0xCC); B(0x10);
                }
                fkind = K_AH;
                cyc += 11;
                return 1;
            }
            B(0xB8 + r16x[y >> 1]); W(I->nn);   // LD rr,nn
            if (y == 4) hl_es = 0;
            cyc += 10;
            if (I->term == T_END_AFTER) sp_loaded_exit(next, 1);   // (SP out of the work RAM)
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
                if (FLAT) {
                    flat_write16(I->nn, X_BX, live);
                } else if (I->nn >= 0xC000 && I->nn < 0xDFFF) {
                    B(0x89); B(0x1E); W(I->nn + env->ram_delta);
                } else {
                    write_const(I->nn, X_BX, live);
                    write_const(I->nn + 1, 7, live);
                }
                cyc += 16;
                if (I->term == T_END_AFTER) {
                    force_nolink = !(map_write_safe(I->nn) && map_write_safe(I->nn + 1));
                    exit_const(next, 0);
                }
                return 1;
            case 5:   // LD HL,(nn)
                read_const16(I->nn, X_BX);
                hl_es = 0;
                cyc += 16;
                return 1;
            case 6:   // LD (nn),A
                write_const(I->nn, X_AX, live);
                cyc += 13;
                if (I->term == T_END_AFTER) { force_nolink = !map_write_safe(I->nn); exit_const(next, 0); }
                return 1;
            case 7:   // LD A,(nn)
                if (I->dyn) {
                    // (self-modifying code in RAM: the address as it is now)
                    FLAT_ES(); B(0x8B); B(0x36); W((uint16_t) (I->pc + 1 + env->ram_delta));   // mov si, [operand]
                    seg_si(live);
                    B(0x26); B(0x8A); B(0x04);                    // mov al, es:[si]
                } else
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
                if (xr == X_BX) hl_es = (!dec && hl_es == 1 && !env->no_fast) ? 2 : 0;
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
                uint8_t *j2, *j1 = mem_check(X_BX, &j2);
                B(0x9E);                                              // sahf
                FLAT_ES(); B(0xFE); B(0x87 | (dec ? 0x08 : 0)); W(env->ram_delta);
                fkind = dec ? K_DEC : K_INC; cf_valid = 1;
                materialize();
                B(0x83); B(0x2E); W(SA(cycles)); B(11);
                if (!step_cold(j1, j2, I->pc)) {
                    uint8_t *j3 = jcc8(0xEB);
                    land8(j1); land8(j2);
                    B(0xC7); B(0x06); W(SA(arg)); W(I->pc);
                    B(0xC6); B(0x06); W(SA(val)); B(1);
                    helper(DRC_H_STEP);
                    land8(j3);
                }
                fkind = K_AH;
                return 1;
            }
            inc_dec_flags(dec, live_out[i] & FC);
            B(0xFE); B((dec ? 0xC8 : 0xC0) | r8x[y]);
            fkind = dec ? K_DEC : K_INC;
            if (y == 4) hl_es = 0;   // (L: the same slot)
            cyc += 4;
            return 1;
        }
        case 6:   // LD r,n
            if (y == 6) { write_byte(X_BX, 0xFF, I->nn, live); cyc += 10; return 1; }
            B(0xB0 + r8x[y]); B(I->nn);
            if (y == 4) hl_es = 0;   // (L: the same slot)
            cyc += 7;
            return 1;
        case 7:
            switch (y) {
            case 0: case 1: case 2: case 3: {
                static const uint8_t rot[4] = { 0xC0, 0xC8, 0xD0, 0xD8 };   // rol ror rcl rcr al
                if (!env->no_fast && !(live_out[i] & (FS | FZ | FP | FH | FN))) {
                    // only C is read after it: it stays in x86 CF
                    if (y >= 2) need_cf();
                    B(0xD0); B(rot[y]);
                    fkind = K_CARRY; cf_valid = 1;
                    cyc += 4;
                    return 1;
                }
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
            FLAT_ES(); B(0x8B); B(0xB6); W(env->ram_delta);   // mov si, [bp+d]
            B(0x8D); B(0x6E); B(0x02);
            exit_reg(X_SI, 11);
            return 1;
        }
        case 1:
            if (y == 1) {   // RET
                FLAT_ES(); B(0x8B); B(0xB6); W(env->ram_delta);
                B(0x8D); B(0x6E); B(0x02);
                exit_reg(X_SI, 10);
                return 1;
            }
            if (y == 5) { exit_reg(X_BX, 4); return 1; }   // JP (HL)
            if (y == 3) {   // EXX
                // (MOVs: 9 cycles in all, three XCHGs with memory 15)
                B(0x8B); B(0x36); W(ZA(c_)); B(0x89); B(0x0E); W(ZA(c_)); B(0x89); B(0xF1);   // bc'
                B(0x8B); B(0x36); W(ZA(e_)); B(0x89); B(0x16); W(ZA(e_)); B(0x89); B(0xF2);   // de'
                B(0x8B); B(0x36); W(ZA(l_)); B(0x89); B(0x1E); W(ZA(l_)); B(0x89); B(0xF3);   // hl'
                hl_es = 0;
                cyc += 4;
                return 1;
            }
            if (y == 7) { B(0x89); B(0xDD); cyc += 6; sp_loaded_exit(next, 0); return 1; }   // LD SP,HL
            // POP
            if (y == 6) {
                FLAT_ES(); B(0x8B); B(0x86); W(env->ram_delta);   // mov ax, [bp+d]
                B(0x86); B(0xC4);                      // xchg al, ah
                fkind = K_AH;
            } else {
                uint8_t xr = r16x[y >> 1];
                FLAT_ES(); B(0x8B); B(0x86 | (xr << 3)); W(env->ram_delta);
                if (xr == X_BX) hl_es = 0;
            }
            B(0x8D); B(0x6E); B(0x02);
            cyc += 10;
            return 1;
        case 2: {   // JP cc,nn
            if (I->nn == block_pc) {
                // a loop: going on (the common case) falls through
                uint8_t *j = jcond(y ^ 1);
                uint8_t save_kind = fkind;
                branch_to(I->nn, 10);
                land8(j);
                fkind = save_kind;
                exit_const(next, 10);
                return 1;
            }
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
            case 2: {   // OUT (n),A
#ifdef GGSWAN_SMS
                // (smsswan: the even ports, 3E; the odd ones are 3F, kept)
                if (env->ctl_off && I->nn < 0x40 && !(I->nn & 1)) {
#else
                if (env->ctl_off && I->nn < 0x40 && I->nn != 0x06) {
#endif
                    cyc += 11;   // (ports 00-3F but 06: nothing to do, see z80_out)
                    return 1;
                }
                clobber(live);
                if (env->psg_queue && I->nn >= 0x40 && I->nn < 0x80) {
                    // PSG: queued (main.c writes the queue at the end of the
                    // frame, or here when full): Sailor Moon S plays samples
                    // with hundreds of volume writes a frame, each a helper
                    B(0x8B); B(0x36); W(SA(psg_n));      // mov si, [psg_n]
                    B(0x88); B(0x84); W(SA(psg_q));      // mov [si + psg_q], al
                    B(0x46);                             // inc si
                    B(0x89); B(0x36); W(SA(psg_n));      // mov [psg_n], si
                    B(0x81); B(0xFE); W(DRC_PSG_Q);      // cmp si, DRC_PSG_Q
                    if (COLD_FREE()) {
                        // full (once in DRC_PSG_Q writes): the helper in
                        // the cold tail
                        uint8_t *full = jcc8(0x73);      // jae cold
                        cold_add(full, here(), COLD_PSG, 0, 0, 0, 0);
                        hl_es = 0;                       // (as after helper)
                    } else {
                        uint8_t *room = jcc8(0x72);      // jb room
                        helper(DRC_H_PSG);
                        land8(room);
                    }
                    cyc += 11;
                    return 1;
                }
                uint8_t *s1 = 0, *done = 0;
#ifdef GGSWAN_SG1000
                if (env->sg_out_off && (I->nn & 0xC1) == 0x80) {
                    call_near(env->sg_out_off);           // TMS9918A data port
                    hl_es = 0;
                    cyc += 11;
                    return 1;
                }
                if (env->sg_ctl_off && (I->nn & 0xC1) == 0x81) {
                    call_near(env->sg_ctl_off);           // TMS9918A control port
                    hl_es = 0;
                    done = jcc8(0x73);                    // jnc done
                }
#endif
                if (env->vdp_addr && (I->nn & 0xC1) == 0x80) {
                    vram_fast(X_AX, &s1, 3);
                    if (!s1) {
                        // (no slow path: the helper below is never reached;
                        // the routines may call helpers: ES as after one)
                        hl_es = 0;
                        cyc += 11;
                        return 1;
                    }
                    done = jcc8(0xEB);
                    land8(s1);
                } else if (env->ctl_off && (I->nn & 0xC1) == 0x81) {
                    call_near(env->ctl_off);              // VDP control port
                    done = jcc8(0x73);                    // jnc done
                }
                B(0xA2); W(SA(val));
                B(0xC7); B(0x06); W(SA(arg)); W(I->nn);
                helper(DRC_H_OUT);
                if (done) land8(done);
                cyc += 11;
                return 1;
            }
            case 3:   // IN A,(n)
                if (FLAT && I->nn == 0xE4) {
                    // sfswan: port A of the SF-7000's 8255 (the disk's
                    // index hole, the controller's interrupt). Disk
                    // BASIC polls it in loops of thousands of turns
                    // (the disk's speed, each time the motor starts):
                    // the value from drc_state_t while the runtime says
                    // that time alone can not change it (keys_in[0]: for
                    // the cycles the block was given; drc_rt.c), else
                    // the helper
                    clobber(live);
                    B(0x80); B(0x3E); W(SA(keys_in)); B(0);                // cmp byte [keys_in], 0
                    uint8_t *slow = jcc8(0x74);                            // je slow
                    B(0xA0); W(SA(keys_in) + 1);                           // mov al, [keys_in + 1]
                    uint8_t *done = jcc8(0xEB);
                    land8(slow);
                    B(0xC7); B(0x06); W(SA(arg)); W(I->nn);
                    helper(DRC_H_IN);
                    B(0xA0); W(SA(val));                                   // mov al, [val]
                    land8(done);
                    cyc += 11;
                    return 1;
                }
#if defined(GGSWAN_SMS) || defined(GGSWAN_SG1000)
                // smsswan: DC only (DD has the TH lines: z80_in); 00-3F FFh
                // (sgswan: the same joypad port; the rest by the helper)
                if (env->pad_in && I->nn >= 0xC0 && !(I->nn & 1)) {
                    B(0xA0); W(SA(keys_in) + 1);                           // mov al, [keys_in + 1]
                    cyc += 11;
                    return 1;
                }
                if (env->ctl_off && I->nn < 0x40) {
                    B(0xB0); B(0xFF);                                      // mov al, FFh
                    cyc += 11;
                    return 1;
                }
                if (0) {
#else
                if (env->pad_in && (I->nn == 0x00 || I->nn >= 0xC0)) {
#endif
                    // Start, the joypad (and its odd mirrors: FFh), as
                    // z80_in, from drc_state_t: no helper
                    if (I->nn & 1) { B(0xB0); B(0xFF); }                 // mov al, FFh
                    else { B(0xA0); W(SA(keys_in) + (I->nn != 0x00)); }     // mov al, [keys_in + n]
                    cyc += 11;
                    return 1;
                }
                if (env->ctl_off && I->nn >= 0x01 && I->nn < 0x40) {
                    // fixed values (z80_in): 01 7F, 03 and 05 00, the others FF
                    uint8_t n = I->nn;
                    B(0xB0); B(n == 1 ? 0x7F : (n == 3 || n == 5) ? 0x00 : 0xFF);   // mov al, value
                    cyc += 11;
                    return 1;
                }
                clobber(live);
#ifdef GGSWAN_SG1000
                if (env->sg_in_off && (I->nn & 0xC0) == 0x80) {
                    // the TMS9918A's data port, or its status
                    call_near((I->nn & 1) ? env->sg_st_off : env->sg_in_off);
                    hl_es = 0;
                    cyc += 11;
                    return 1;
                }
#endif
                if (env->vcnt_off && (I->nn & 0xC0) == 0x40) {
                    // (after clobber: the routine's DIV and compares)
                    if (I->nn & 1) {
                        B(0xB0); B(0x80);                    // H counter: mov al, 80h (z80_in)
                    } else {
                        // the V counter: the routine, or the helper
                        call_near(env->vcnt_off);
                        uint8_t *ok = jcc8(0x73);            // jnc done
                        B(0xC7); B(0x06); W(SA(arg)); W(I->nn);
                        helper(DRC_H_IN);
                        B(0xA0); W(SA(val));                 // mov al, [val]
                        land8(ok);
                    }
                    cyc += 11;
                    return 1;
                }
                if (env->vdp_addr && env->ctl_off && (I->nn & 0xC1) == 0x80) {
                    // VDP data port: the prefetched byte, prefetch the next
                    if (env->cls_off) {
                        // deferred tile writes (main.c): written first
                        B(0x80); B(0x3E); W(SA(defer)); B(0);       // cmp byte [defer], 0
                        uint8_t *nd = jcc8(0x74);                   // je
                        B(0xC6); B(0x06); W(SA(op)); B(DRC_H_SYNC); // mov byte [op], SYNC
                        call_near(env->helper_off);
                        land8(nd);
                        hl_es = 0;
                    }
                    B(0x8B); B(0x36); W(VA(addr));                  // mov si, [addr]
                    B(0x8A); B(0x84); W(env->vram_addr);            // mov al, [si + vram]
                    B(0x86); B(0x06); W(VA(read_buffer));           // xchg al, [read_buffer]
                    B(0x46);                                        // inc si
                    B(0x81); B(0xE6); W(0x3FFF);                    // and si, 3FFFh
                    B(0x89); B(0x36); W(VA(addr));                  // mov [addr], si
                    B(0xC6); B(0x06); W(VA(latch_full)); B(0);      // latch_full = 0
                    cyc += 11;
                    return 1;
                }
                if (env->vdp_addr && env->ctl_off && (I->nn & 0xC1) == 0x81) {
                    // VDP status: read it, then the interrupt requests clear
                    B(0xA0); W(VA(status));                         // mov al, [status]
                    B(0x0C); B(0x1F);                               // or al, 1Fh
                    B(0xC6); B(0x06); W(VA(status)); B(0);          // status = 0
                    B(0xC6); B(0x06); W(VA(line_pending)); B(0);    // line_pending = 0
                    B(0xC6); B(0x06); W(VA(latch_full)); B(0);      // latch_full = 0
                    B(0xC6); B(0x06); W(ZA(irq)); B(0);             // (no request left)
                    cyc += 11;
                    return 1;
                }
                B(0xC7); B(0x06); W(SA(arg)); W(I->nn);
                helper(DRC_H_IN);
                B(0xA0); W(SA(val));
                cyc += 11;
                return 1;
            case 6:   // DI
                B(0xC7); B(0x06); W(ZA(iff1)); W(0);   // iff1 = iff2 = 0
                cyc += 4;
                return 1;
            case 7: {   // EI
                // an interrupt waiting: exit, the runtime takes it after the
                // next instruction; otherwise none can arrive before the
                // end of the block (only between runs): go on
                B(0xC7); B(0x06); W(ZA(iff1)); W(0x0101);
                cyc += 4;
                materialize();
                B(0x80); B(0x3E); W(ZA(irq)); B(0);       // cmp byte [irq], 0
                if (COLD_FREE() && !(idle_block == 3 && next == idle_target)) {
                    // (no interrupt waiting: the common case falls through)
                    cold_add(jcc8(0x75), next, COLD_EI_EXIT,
                             (uint8_t) cyc, (uint8_t) (cyc >> 8), 0, 0);
                } else {
                    uint8_t *go = jcc8(0x74);             // je go
                    B(0xC6); B(0x06); W(ZA(ei_pending)); B(1);
                    force_nolink = 1;
                    exit_const(next, 0);
                    force_nolink = 0;
                    land8(go);
                }
                fkind = K_AH;
                return 1;
            }
            case 4:   // EX (SP),HL
                FLAT_ES(); B(0x87); B(0x9E); W(env->ram_delta);
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
                FLAT_ES(); B(0x89); B(0x86); W(env->ram_delta);
                B(0x86); B(0xC4);
            } else {
                FLAT_ES(); B(0x89); B(0x86 | (r16x[y >> 1] << 3)); W(env->ram_delta);
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

/*
 * Code in RAM. A constant write into the block's own bytes is allowed only
 * for the address of a later LD A,(nn) (read at run time then, see emit);
 * anything else: not translated. The other bytes are checked every time the
 * block is entered.
 */
#define RAM_BLOCK_MAX 24
#define CALL_INLINE_MAX 12   // instructions of an inline routine (RET included)
#ifdef __ia16__
#define ram_mask ((uint8_t*) (0xE000 + MAX_INSTR * (sizeof(ins_t) + 1)))
#else
static uint8_t ram_mask[RAM_BLOCK_MAX];   // 1: byte not checked
#endif

static int ram_smc(int n, uint16_t pc, uint16_t end) {
    memset(ram_mask, 0, RAM_BLOCK_MAX);
    for (int i = 0; i < n; i++) {
        const ins_t *I = &ins[i];
        uint16_t t; uint8_t w;
        if (I->pre == 0 && I->op == 0x32) { t = I->nn; w = 1; }          // LD (nn),A
        else if (I->pre == 0 && I->op == 0x22) { t = I->nn; w = 2; }     // LD (nn),HL
        else if ((I->pre == 0xED && (I->op & 0xC7) == 0x43)
                 || ((I->pre == 0xDD || I->pre == 0xFD) && !I->cb && I->op == 0x22)) { t = I->nn; w = 2; }
        else continue;
        for (uint8_t k = 0; k < w; k++) {
            uint16_t a = t + k;
            if (a < pc || a >= end) continue;
            int j;
            for (j = i + 1; j < n; j++)
                if (a >= ins[j].pc && a < ins[j].pc + ins[j].len) break;
            if (j == n || ins[j].pre || ins[j].op != 0x3A || a == ins[j].pc)
                return 0;
            ins[j].dyn = 1;
            ram_mask[a - pc] = 1;
        }
    }
    return 1;
}

// at the start of a block in RAM: are its bytes still the translated ones?
static void ram_check(uint16_t pc, uint16_t end) {
    uint8_t *fail[RAM_BLOCK_MAX];
    int nf = 0;
    if (end - pc > RAM_BLOCK_MAX) {
        // a block of OUTIs only (translate): all words ED A3
        B(0x50);                                         // push ax
        B(0x51);                                         // push cx
        B(0x1E); B(0x07);                                // push ds; pop es
        B(0xBF); W((uint16_t) (pc + env->ram_delta));    // mov di, start
        B(0xB9); W((uint16_t) ((end - pc) / 2));         // mov cx, words
        B(0xB8); W(0xA3ED);                              // mov ax, A3EDh (ED A3)
        B(0xFC);                                         // cld
        B(0xF3); B(0xAF);                                // repe scasw
        B(0x59);                                         // pop cx
        B(0x58);                                         // pop ax
        fail[nf++] = jcc8(0x75);                         // jne fail
        hl_es = 0;
        goto checked;
    }
    uint16_t a = pc;
    while (a < end) {
        uint8_t k = a - pc;
        uint16_t ia = (uint16_t) (a + env->ram_delta);
        if (a + 1 < end && !ram_mask[k] && !ram_mask[k + 1]) {
            B(0x81); B(0x3E); W(ia); B(rd(a)); B(rd(a + 1));    // cmp word [ia], bytes
            a += 2;
        } else {
            if (!ram_mask[k]) { B(0x80); B(0x3E); W(ia); B(rd(a)); }   // cmp byte [ia], byte
            else { a++; continue; }
            a++;
        }
        fail[nf++] = jcc8(0x75);                                 // jne fail
    }
checked:;
    uint8_t *body = jcc8(0xEB);
    for (int i = 0; i < nf; i++) land8(fail[i]);
    // changed: back to the runtime, which leaves this address to the interpreter
    B(0xC6); B(0x06); W(SA(ram_fail)); B(1);
    B(0xC7); B(0x06); W(ZA(pc)); W(pc);
    set_link(0);
    jmp_near(env->exit_off);
    land8(body);
}

/*
 * OUTI / OUTI / DEC L x 4, again and again (Shinobi's tile uploads,
 * 0720-0950h): returns how many of these groups starting at ins[i] go to
 * the pairs routine (0: none). The last group of the row is left to the
 * usual code: the flags it leaves are the ones that count, and those of
 * the groups before are overwritten by its OUTIs.
 */
static int is_group(int j) {
    if (ins[j].pre != 0xED || ins[j].op != 0xA3 || ins[j].count != 2 || ins[j].dyn || ins[j].trace)
        return 0;
    for (int k = 1; k <= 4; k++)
        if (ins[j + k].pre || ins[j + k].op != 0x2D || ins[j + k].term || ins[j + k].trace)
            return 0;
    return 1;
}

static int pairs_at(int i, int n) {
    if (!env->pairs_off || env->no_fast)
        return 0;
    int g = 0;
    while (i + 5 * g + 5 <= n && g < 60 && is_group(i + 5 * g))
        g++;
    return g >= 3 ? g - 1 : 0;
}

static int emit(int i);

// the g groups at ins[i]: the pairs routine, or (CF = 1) the instructions
// one by one; both leave the same state (flags dead: the next OUTI)
static int emit_pairs(int i, int g) {
    clobber(0);
    B(0xBF); W(g);                                   // mov di, g
    call_near(env->pairs_off);
    B(0x72); B(3);                                   // jc +3
    B(0xE9); W(0);                                   // jmp done
    uint8_t *done = o - 2;
    uint16_t cyc0 = cyc;
    for (int j = i; j < i + 5 * g; j++)
        if (!emit(j))
            return 0;
    clobber(0);
    uint16_t r = (uint16_t) (o - (done + 2));
    done[0] = (uint8_t) r; done[1] = (uint8_t) (r >> 8);
    fkind = K_AH; cf_valid = 0; hl_es = 0;
    cyc = cyc0 + 48 * g;
    return 1;
}

// OUTI x 3 / OUT (BEh),A at ins[i]: a tile row for the row routine; OUTI
// / OUT (BEh),A: a name table entry for its routine (returns the routine,
// 0: neither)
static uint16_t row_at(int i, int n) {
    if (env->no_fast || i + 1 >= n)
        return 0;
    const ins_t *a = &ins[i], *b = &ins[i + 1];
    if (!(a->pre == 0xED && a->op == 0xA3 && !a->dyn && !a->trace && !a->term
          && b->pre == 0 && b->op == 0xD3 && (b->nn & 0xC1) == 0x80 && !b->dyn && !b->trace && !b->term))
        return 0;
    return a->count == 3 ? env->trow_off : a->count == 1 ? env->ntwa_off : 0;
}

// the routine (at off), or (CF = 1) the two instructions as usual; both
// leave the same state: the OUTIs' flags in AH if they are read
// afterwards (OUT changes none)
static int emit_row(int i, uint16_t off) {
    clobber(0);
    call_near(off);
    uint8_t *usual = jcc8(0x72);                     // jc usual
    if (live_out[i] & FALL) {
        // (as run_flags: S and Z of B, N of the last byte)
        B(0x88); B(0x26); W(SA(val));                // mov [val], ah
        B(0x84); B(0xED);                            // test ch, ch
        B(0x9F);                                     // lahf
        B(0x80); B(0xE4); B(0xC0);                   // and ah, C0h
        B(0xF6); B(0x06); W(SA(val)); B(0x80);       // test byte [val], 80h
        B(0x74); B(0x03); B(0x80); B(0xCC); B(0x02); // jz +3; or ah, 2
    }
    B(0xE9); W(0);                                   // jmp done
    uint8_t *done = o - 2;
    land8(usual);
    if (!emit(i) || !emit(i + 1))
        return 0;
    uint16_t r = (uint16_t) (o - (done + 2));
    done[0] = (uint8_t) r; done[1] = (uint8_t) (r >> 8);
    return 1;
}

/*
 * Consecutive LD A,(rr) / OUT (BEh),A / INC rr steps (with optional
 * PUSH rr2 / POP rr2 delay pairs in between): Sonic GG's compressed tile
 * loader (04C0, 048E, 04B9, ~23% of the frame) writes 4-byte tile rows
 * this way, taking ~420 cycles per row via 4 separate vram_fast + seg_hl
 * / seg_si + 3 PUSH/POP delays.
 *
 * Returns how many instructions from ins[i] form a run of k >= 3 bytes
 * (0: none), filling *out_k (bytes, 3..8), *out_src (x86 16-bit reg:
 * X_BX/X_CX/X_DX), *out_push_reg (0xFF if none, else X_CX/X_DX/X_BX of
 * the last PUSH/POP pair), *out_push_delta (how much src advanced after
 * that last PUSH if push_reg == src, else 0), and *out_np (count of
 * PUSH/POP pairs, for exact Z80 cycle accounting).
 */
static int vcopy_at(int i, int n, uint8_t *out_k, uint8_t *out_src,
                    uint8_t *out_push_reg, uint8_t *out_push_delta, uint8_t *out_np) {
    if (FLAT || env->no_fast || !env->vdp_addr || !env->cls_off || i + 9 > n)
        return 0;
    const ins_t *I = &ins[i];
    if (I->pre || I->dyn || I->trace || I->step || I->term)
        return 0;
    uint8_t ld_op = I->op, inc_op, src_reg;
    if (ld_op == 0x7E) { inc_op = 0x23; src_reg = X_BX; }        // HL
    else if (ld_op == 0x0A) { inc_op = 0x03; src_reg = X_CX; }   // BC
    else if (ld_op == 0x1A) { inc_op = 0x13; src_reg = X_DX; }   // DE
    else return 0;

    int j = i;
    uint8_t k = 0, np = 0, push_reg = 0xFF, last_push_step = 0;
    while (k < 8 && j + 3 <= n) {
        const ins_t *a = &ins[j], *b = &ins[j + 1], *c = &ins[j + 2];
        if (a->pre || a->op != ld_op || a->dyn || a->trace || a->step || a->term)
            break;
        if (b->pre || b->op != 0xD3 || (b->nn & 0xC1) != 0x80
            || b->dyn || b->trace || b->step || b->term)
            break;
        if (c->pre || c->op != inc_op || c->dyn || c->trace || c->step || c->term)
            break;
        j += 3;
        k++;
        // Optional PUSH rr2 / POP rr2 delay pair (BC, DE, or HL)
        if (j + 2 <= n) {
            const ins_t *p = &ins[j], *q = &ins[j + 1];
            if (!p->pre && !q->pre && !p->dyn && !q->dyn && !p->trace && !q->trace
                && !p->step && !q->step && !p->term && !q->term
                && ((p->op == 0xC5 && q->op == 0xC1)
                    || (p->op == 0xD5 && q->op == 0xD1)
                    || (p->op == 0xE5 && q->op == 0xE1))) {
                push_reg = r16x[(p->op >> 4) & 3];
                last_push_step = k;   // src has been incremented k times here
                np++;
                j += 2;
            }
        }
    }
    if (k < 3)
        return 0;
    *out_k = k;
    *out_src = src_reg;
    *out_push_reg = push_reg;
    *out_push_delta = (push_reg == src_reg) ? (uint8_t) (k - last_push_step) : 0;
    *out_np = np;
    return j - i;
}

static int emit_vcopy(int i, int span, uint8_t k, uint8_t src_reg,
                      uint8_t push_reg, uint8_t push_delta, uint8_t np) {
    // None of LD A,(rr), OUT (BE),A, INC rr, PUSH/POP rr2 alters Z80 flags:
    // materialize into AH if any flag is live after the run, then keep AH untouched.
    clobber(live_out[i + span - 1]);
    hl_es = 0;

    uint8_t *f[6];
    uint8_t nf = 0;
    // 1. Not CRAM
    B(0x80); B(0x3E); W(VA(code)); B(3);                 // cmp byte [code], 3
    f[nf++] = jcc8(0x74);                                // je fail
    // 2. Source [src .. src + k - 1] stays inside one 8 KiB slice
    B(0x89); B(0xC6 | (src_reg << 3));                   // mov si, src
    B(0x81); B(0xE6); W(0x1FFF);                         // and si, 1FFFh
    B(0x81); B(0xFE); W((uint16_t) (0x2000 - k));        // cmp si, 2000h - k
    f[nf++] = jcc8(0x77);                                // ja fail
    // 3. If PUSH/POP present, ensure [src .. src + k - 1] does not overlap
    //    [SP - 2 .. SP - 1] modulo 8 KiB (covers the E000-FFFF work RAM mirror)
    if (push_reg != 0xFF) {
        B(0x8D); B(0x76); B(0xFF);                       // lea si, [bp - 1]
        B(0x29); B(0xC6 | (src_reg << 3));               // sub si, src
        B(0x81); B(0xE6); W(0x1FFF);                     // and si, 1FFFh
        B(0x83); B(0xFE); B(k);                          // cmp si, k
        f[nf++] = jcc8(0x76);                            // jbe fail
    }
    // 4. VRAM address and page classes of first and last byte (AL is dead: overwritten by LD A,(rr))
    B(0x8B); B(0x36); W(VA(addr));                       // mov si, [addr]
    B(0x81); B(0xE6); W(0x3FFF);                         // and si, 3FFFh
    B(0x89); B(0xF7);                                    // mov di, si
    B(0xC1); B(0xEF); B(8);                              // shr di, 8
    B(0x8A); B(0x85); W(SA(vram_class));                 // mov al, [di + class]
    B(0x8D); B(0x7C); B((uint8_t) (k - 1));              // lea di, [si + k - 1]
    B(0xC1); B(0xEF); B(8);                              // shr di, 8
    B(0x0A); B(0x85); W(SA(vram_class));                 // or al, [di + class]
    B(0xA8); B(0x13);                                    // test al, 13h (wrap, NT, SAT)
    f[nf++] = jcc8(0x75);                                // jnz fail
    // 5. Deferred log (bit 3 set) vs plain VRAM (class == 0)
    B(0xA8); B(0x08);                                    // test al, 8
    uint8_t *plain = jcc8(0x74);                         // jz plain_vram
    // Deferred: try appending to open log entry (log_next)
    B(0x8B); B(0x3E); W(SA(log_top));                    // mov di, [log_top]
    B(0x3B); B(0x36); W(SA(log_next));                   // cmp si, [log_next]
    uint8_t *new1 = jcc8(0x75);                          // jne new_entry
    B(0x8B); B(0x36); W(SA(log_last));                   // mov si, [log_last]
    B(0x80); B(0x7C); B(0x02); B((uint8_t) (254 - k));   // cmp byte [si + 2], 254 - k
    uint8_t *new2 = jcc8(0x77);                          // ja new_entry
    B(0x83); B(0xC7); B(k);                              // add di, k
    B(0x3B); B(0x3E); W(SA(log_end));                    // cmp di, [log_end]
    f[nf++] = jcc8(0x77);                                // ja fail (log full -> fallback)
    B(0x89); B(0x3E); W(SA(log_top));                    // mov [log_top], di
    B(0x83); B(0xEF); B(k);                              // sub di, k (DI = dst in log)
    B(0x80); B(0x44); B(0x02); B(k);                     // add byte [si + 2], k
    B(0x83); B(0x06); W(SA(log_next)); B(k);             // add word [log_next], k
    uint8_t *copy1 = jcc8(0xEB);                         // jmp copy_bytes

    // Mid-point trampoline to fallback (reachable by short jumps both forward and backward)
    for (uint8_t m = 0; m < nf; m++) land8(f[m]);
    uint8_t *fail_tramp = o;
    B(0xE9); W(0);                                       // jmp fallback
    uint8_t *to_fallback = o - 2;

    // New log entry
    land8(new1); land8(new2);
    B(0x8B); B(0x36); W(VA(addr));                       // mov si, [addr]
    B(0x81); B(0xE6); W(0x3FFF);                         // and si, 3FFFh
    B(0x83); B(0xC7); B((uint8_t) (k + 3));              // add di, k + 3
    B(0x3B); B(0x3E); W(SA(log_end));                    // cmp di, [log_end]
    B(0x77); B((uint8_t) (fail_tramp - (o + 1)));        // ja fail_tramp (backwards)
    B(0x89); B(0x3E); W(SA(log_top));                    // mov [log_top], di
    B(0x83); B(0xEF); B((uint8_t) (k + 3));              // sub di, k + 3
    B(0x89); B(0x3E); W(SA(log_last));                   // mov [log_last], di
    B(0x89); B(0x35);                                    // mov [di], si (VRAM addr)
    B(0xC6); B(0x45); B(0x02); B(k);                     // mov byte [di + 2], k
    B(0x83); B(0xC7); B(0x03);                           // add di, 3 (DI = dst in log)
    B(0x83); B(0xC6); B(k);                              // add si, k
    B(0x89); B(0x36); W(SA(log_next));                   // mov [log_next], si
    uint8_t *copy2 = jcc8(0xEB);                         // jmp copy_bytes

    // Plain VRAM (not deferred)
    land8(plain);
    B(0x8D); B(0xBC); W(env->vram_addr);                 // lea di, [si + vram]

    // Copy k bytes from ES:[src] to DS:[di], advance src += k, set AL = last byte
    land8(copy1); land8(copy2);
    if (src_reg != X_BX) { B(0x87); B(0xC3 | (src_reg << 3)); }   // xchg bx, src
    B(0x89); B(0xDE);                                    // mov si, bx
    B(0xC1); B(0xC6); B(4);                              // rol si, 4
    B(0x83); B(0xE6); B(14);                             // and si, 14
    B(0x8E); B(0x84); W(SA(segtab));                     // mov es, [si + segtab]
    for (uint8_t m = 0; m + 1 < k; m += 2) {
        if (m == 0) {
            B(0x26); B(0x8B); B(0x37);                   // mov si, es:[bx]
            B(0x89); B(0x35);                            // mov [di], si
        } else {
            B(0x26); B(0x8B); B(0x77); B(m);             // mov si, es:[bx + m]
            B(0x89); B(0x75); B(m);                      // mov [di + m], si
        }
    }
    if (k & 1) {
        B(0x26); B(0x8A); B(0x47); B((uint8_t) (k - 1)); // mov al, es:[bx + k - 1]
        B(0x88); B(0x45); B((uint8_t) (k - 1));          // mov [di + k - 1], al
    } else {
        B(0x26); B(0x8A); B(0x47); B((uint8_t) (k - 1)); // mov al, es:[bx + k - 1]
    }
    B(0x83); B(0xC3); B(k);                              // add bx, k
    if (src_reg != X_BX) { B(0x87); B(0xC3 | (src_reg << 3)); }   // xchg bx, src
    // Update VDP addr, read_buffer, latch_full
    B(0x8B); B(0x36); W(VA(addr));                       // mov si, [addr]
    B(0x83); B(0xC6); B(k);                              // add si, k
    B(0x81); B(0xE6); W(0x3FFF);                         // and si, 3FFFh
    B(0x89); B(0x36); W(VA(addr));                       // mov [addr], si
    B(0xA2); W(VA(read_buffer));                         // mov [read_buffer], al
    B(0xC6); B(0x06); W(VA(latch_full)); B(0);           // mov byte [latch_full], 0
    // Observable stack write of the last PUSH rr2 / POP rr2 delay pair
    if (push_reg != 0xFF) {
        uint16_t sp_disp = (uint16_t) (env->ram_delta - 2);
        if (push_delta == 0) {
            B(0x89); B(0x86 | (push_reg << 3)); W(sp_disp);       // mov [bp + ram - 2], rr2
        } else if (push_reg == X_BX) {
            B(0x8D); B(0x77); B((uint8_t) (0u - push_delta));     // lea si, [bx - delta]
            B(0x89); B(0xB6); W(sp_disp);                         // mov [bp + ram - 2], si
        } else {
            B(0x89); B(0xC6 | (push_reg << 3));                   // mov si, rr2
            B(0x83); B(0xEE); B(push_delta);                      // sub si, delta
            B(0x89); B(0xB6); W(sp_disp);                         // mov [bp + ram - 2], si
        }
    }
    B(0xE9); W(0);                                       // jmp done
    uint8_t *to_done = o - 2;

    // Fallback: emit the original instructions (inline cold paths to avoid
    // blowing the block's rel8 cold tail on a never-taken fallback)
    {
        uint16_t r_ = (uint16_t) (o - (to_fallback + 2));
        to_fallback[0] = (uint8_t) r_;
        to_fallback[1] = (uint8_t) (r_ >> 8);
    }
    uint8_t saved_cold = cold_off;
    uint16_t cyc0 = cyc;
    cold_off = 1;
    fkind = K_AH; cf_valid = 0; hl_es = 0;
    for (int j = i; j < i + span; j++)
        if (!emit(j)) {
            cold_off = saved_cold;
            return 0;
        }
    clobber(live_out[i + span - 1]);
    cold_off = saved_cold;
    {
        uint16_t r_ = (uint16_t) (o - (to_done + 2));
        to_done[0] = (uint8_t) r_;
        to_done[1] = (uint8_t) (r_ >> 8);
    }
    fkind = K_AH; cf_valid = 0; hl_es = 0;
    cyc = (uint16_t) (cyc0 + k * 24 + np * 21);
    return 1;
}

int drc_translate(const drc_env_t *e, uint16_t pc, uint8_t *out,
                  uint16_t out_off, uint16_t out_max, drc_result_t *res) {
    env = e;
    rd_len = 0;   // (the mapping may have changed since the last block)
    // code in work RAM (C000-DFFF): short blocks that check their own bytes
    // when entered (see ram_check)
    // (sfswan: all of it is RAM, and none of it is "a block in RAM": no
    // check of its bytes at the start, the writes are looked at instead)
    uint8_t ram = pc >= 0xC000 && !FLAT;
    uint8_t long_run = 0;   // a block in RAM of OUTIs only, longer than RAM_BLOCK_MAX
    if (ram && (!env->ram_blocks || pc >= 0xDFC0)) return 0;
    // decode
    int n = 0;
    uint16_t p = pc;
    // every instruction fits in 96 bytes of x86 code: stop early enough
    int max_n;
    if (out_max >= 128 + MAX_INSTR * 96) {
        max_n = MAX_INSTR;
    } else if (out_max <= 128) {
        max_n = 0;
    } else {
        uint16_t q = (uint16_t) (out_max - 128) >> 5;
        max_n = (int) ((uint16_t) (q * 171u) >> 9);
    }
    uint8_t inlined = 0;
    int c_known = -1;   // C as set by LD C,n earlier in the block (-1: not known)
    ins_t *K = ins;
    while (n < max_n) {
        // stay inside the block's 16 KiB slot (or slot 0, through a jump)
        if (n && ((p ^ pc) & 0xC000) && !(p < 0x4000 && !ram)) break;
        if (ram && p - pc > RAM_BLOCK_MAX - 4) break;
        // a long block stops where one already translated starts, and links
        // to it: Shinobi uploads tiles with an unrolled OUTI / OUTI / DEC L
        // x4 routine (0720-0950h) entered at 221 different points, each a
        // new block of 48 instructions: 60% of all it translated
        if (n >= 8 && !ram && env->known_start && env->known_start(p)) break;
        decode(p, K);
        K->dyn = 0;
        K->trace = 0;
#ifdef GGSWAN_SF7000
        if (env->smc && env->smc(p, K->len)) {
            // sfswan: a byte of it gets written by the program: the
            // interpreter's, in the block if it is a plain one (each LDI
            // or OUTI of a run one step), else between blocks
            if (K->term != T_NONE)
                break;
            K->step = 1;
            K->dyn = 2;
        }
#endif
        if (ram) {
            if (n == 0 && K->pre == 0xED && K->op == 0xA3 && K->count > RAM_BLOCK_MAX / 2
                && !env->no_fast) {
                // a block in RAM made only of a run of OUTIs (Aladdin's
                // unrolled VRAM upload at C6FF-C7F8, entered at any of them):
                // the whole run, its bytes checked by one REPE SCASW
                // (ram_check) instead of one compare per OUTI; the block ends
                // after it. The same as the blocks of 12 OUTIs it replaces
                // (translate): whole pieces of 12 unless the run ends here (64:
                // more may follow), none starting from DFC0 (interpreted)
                uint8_t k = K->count;
                if (k == 64) k = 60;
                uint16_t pieces = (uint16_t) ((0xDFC0 - pc + 23) / 24);
                if (pieces < 6 && k > 12 * pieces) k = (uint8_t) (12 * pieces);
                K->count = k;
                K->len = k * 2;
                long_run = k > RAM_BLOCK_MAX / 2;
                n = 1;
                p += K->len;
                break;
            }
            if (p + K->len - pc > RAM_BLOCK_MAX) {
                // a run of LDI/OUTI... (up to 128 bytes) longer than a block
                // in RAM may be (ram_check checks at most RAM_BLOCK_MAX
                // bytes): only the part that fits, the rest in the next block
                uint8_t room = (uint8_t) (RAM_BLOCK_MAX - (p - pc));
                if (K->count > 1 && room >= 2) {
                    K->count = room / 2;
                    K->len = K->count * 2;
                } else {
                    break;
                }
            }
        }
        // IN r,(C) with C known (LD C,n, then only instructions that
        // leave C alone): the VDP data port natively, as IN A,(n)
        // (Aladdin reads VRAM with LD C,BEh / IN C,(C): a step each,
        // ~3 a frame). d = 1 marks it (emit).
        if (c_known >= 0) {
            if (K->pre == 0xED && (K->op & 0xC7) == 0x40 && K->op != 0x70
                && (c_known & 0xC1) == 0x80 && env->vdp_addr && env->ctl_off && !env->no_fast) {
                K->step = 0; K->count = 0; K->nn = (uint16_t) c_known; K->d = 1;
                K->fwrite = FALL & ~FC; K->fread = FC;
            }
            if (K->pre != 0)
                c_known = -1;
            else if (K->op == 0x0E) c_known = K->nn;
            else if (K->op == 0x01) c_known = K->nn & 0xFF;
            else if (K->op != 0x00 && K->op != 0xF3 && K->op != 0xD5 && K->op != 0xE5
                     && K->op != 0xF5 && K->op != 0xC5 && K->op != 0xD1 && K->op != 0xE1
                     && K->op != 0xF1)
                c_known = -1;
        } else if (K->pre == 0 && (K->op == 0x0E || K->op == 0x01)) {
            c_known = K->nn & 0xFF;
        }
        if (K->term == T_STOP) break;
        p += K->len;
        n++;
        if (K->term == T_NONE) {
            K++;
            continue;
        }
        if (K->term == T_END_AFTER) break;
        // K->term == T_END
        if (!ram && K->pre == 0) {
            if (!env->no_trace && (K->op == 0xC3 || K->op == 0x18) && n < max_n - 1) {
                // JP nn / JR e: go on at the target, unless it leaves slot 0 and
                // the block's slot, or is already in the block (a loop)
                uint16_t t = K->op == 0xC3 ? K->nn : (uint16_t) (K->pc + 2 + (int8_t) K->nn);
                int seen = t >= 0xC000 || ((t ^ pc) & 0xC000 && t >= 0x4000);
                if (!seen) {
                    for (const ins_t *S = ins; S <= K; S++)
                        if (S->pc == t) { seen = 1; break; }
                }
                if (!seen) {
                    K->term = T_NONE;
                    K->trace = 1;
                    p = t;
                    K++;
                    continue;
                }
            }
            if (!env->no_inline && K->op == 0xCD && !inlined && n + CALL_INLINE_MAX + 2 < max_n
                && !(((K->nn ^ pc) & 0xC000) && K->nn >= 0x4000) && K->nn < 0xC000) {
                // CALL nn to a short routine (no jumps out of it, ends with RET):
                // its instructions follow inline (once per block)
                uint16_t q = K->nn;
                ins_t *C = K + 1;
                int k, ok = 0;
                for (k = 0; k <= CALL_INLINE_MAX; k++, C++) {
                    if (((q ^ pc) & 0xC000) && q >= 0x4000) break;
                    decode(q, C);
                    C->dyn = 0;
                    C->trace = 0;
                    if (C->pre == 0 && C->op == 0xC9) { ok = 1; break; }
                    if (C->term != T_NONE || C->step) break;
                    q += C->len;
                }
                if (ok) {
                    inlined = 1;
                    K->term = T_NONE;
                    K->trace = 2;
                    C->term = T_NONE;
                    C->trace = 3;
                    C->nn = K->pc + 3;
                    C->fread = FALL;   // (the flags are needed if it does not return here)
                    n += k + 1;
                    p = K->pc + 3;
                    K = C + 1;
                    continue;
                }
            }
        }
        break;
    }
    if (n == 0) return 0;
    uint16_t end = p;
    if (ram && !ram_smc(n, pc, end)) return 0;

    // liveness of the flags (all live at the end of the block)
    uint8_t live = FALL;
    for (int i = n - 1; i >= 0; i--) {
        live_out[i] = live;
        if (ins[i].term == T_END) live = FALL;
        live = (live & ~ins[i].fwrite) | ins[i].fread;
        if (ins[i].step) live = FALL;
    }

    // generate
    idle_block = is_idle(n, pc, ram);
    cold_off = env->no_fast;
retry_gen:
    o = o_start = out; o_base = out_off; o_limit = out + out_max;
    block_start_off = out_off; block_pc = pc;
    fkind = K_AH; cf_valid = 0; hl_es = 0; cyc = 0;
    force_nolink = 0;
    cold_n = 0; cold_fail = 0; cold_emitting = 0;
    if (ram) ram_check(pc, end);
    if (long_run) {
        // the blocks of 12 OUTIs this one replaces would each have ended the
        // run when the cycles ran out: so only when the cycles of all the
        // pieces but the last leave some; else the first 12, then the
        // block after them (the same again)
        uint8_t k = ins[0].count, r = (uint8_t) (k - 12 * ((k - 1) / 12));
        B(0x81); B(0x3E); W(SA(cycles)); W(16 * (k - r));   // cmp word [cycles], cycles
        uint8_t *whole = jcc8(0x7F);                         // jg whole
        ins[0].count = 12; ins[0].len = 24;
        if (!emit(0))
            return 0;
        exit_const(pc + 24, 0);
        land8(whole);
        fkind = K_AH; cf_valid = 0; hl_es = 0; cyc = 0;
        ins[0].count = k; ins[0].len = k * 2;
    }
    int i;
    // IN A,(7Eh) / (CP n | AND A | OR A) / (JR|JP) cc back here: waiting for
    // a line (Sonic Spinball, Micro Machines C049): skip the turns whose V
    // counter keeps the loop going, then run the final turn as usual
    if (n == 3 && env->vcnt_off && !env->no_fast && ins[0].pre == 0 && ins[0].op == 0xDB
        && (ins[0].nn & 0xC1) == 0x40
        && ins[1].pre == 0 && (ins[1].op == 0xFE || ins[1].op == 0xA7 || ins[1].op == 0xB7)
        && ins[2].pre == 0 && ((ins[2].op & 0xE7) == 0x20 || (ins[2].op & 0xE7) == 0xC2)) {
        uint8_t is_jr = (ins[2].op & 0xE7) == 0x20;
        uint16_t tgt = is_jr ? (uint16_t) (ins[2].pc + 2 + (int8_t) ins[2].nn) : ins[2].nn;
        if (tgt == ins[0].pc) {
            uint8_t is_cp = ins[1].op == 0xFE;
            uint8_t step = (uint8_t) (11 + (is_cp ? 7 : 4) + (is_jr ? 12 : 10));
            uint16_t varg = (uint16_t) (is_cp ? (ins[1].nn & 0xFF) : 0)
                          | (uint16_t) (((ins[2].op >> 3) & 3) << 8);
            if (env->vwait_off) {
                B(0xBE); W((uint16_t) (varg | ((uint16_t) step << 10)));   // mov si, n | (cc << 8) | (step << 10)
                call_near(env->vwait_off);
            } else if (step == 30) {
                B(0xC7); B(0x06); W(SA(arg)); W(varg);
                helper(DRC_H_VWAIT);
            }
        }
    }
    // DEC r / JR NZ (or JP NZ) back here: exact delay loop (Castle of
    // Illusion 06E8: DEC A / JR NZ, 255 turns/frame). Skip k = min((uint8_t)(r - 1),
    // floor((cycles - 1) / T)) full taken turns so the final turn runs normally
    // with exact register value, flags, and interrupt timing.
    if (n == 2 && !env->no_fast && !ins[0].step && !ins[1].step
        && ins[0].pre == 0 && (ins[0].op & 0xC7) == 0x05 && ins[0].op != 0x35
        && ins[1].pre == 0 && (ins[1].op == 0x20 || ins[1].op == 0xC2)) {
        uint8_t is_jr = ins[1].op == 0x20;
        uint16_t tgt = is_jr ? (uint16_t) (ins[1].pc + 2 + (int8_t) ins[1].nn) : ins[1].nn;
        if (tgt == ins[0].pc) {
            uint8_t xr = r8x[(ins[0].op >> 3) & 7];
            uint8_t r16 = xr & 3;
            uint8_t hi = xr >= 4;
            if (is_jr) {
                // T = 16 cycles/turn (power of two: shift only, no DIV)
                B(0x8B); B(0x3E); W(SA(cycles));             // mov di, [cycles]
                B(0x83); B(0xFF); B(16);                     // cmp di, 16
                uint8_t *done = jcc8(0x7E);                  // jle done (k_max = 0)
                B(0x4F);                                     // dec di (cycles - 1 >= 16)
                B(0xC1); B(0xEF); B(4);                      // shr di, 4 (k_max)
                B(0x89); B(0xC6 | (r16 << 3));               // mov si, r16
                if (hi) { B(0xC1); B(0xEE); B(8); }          // shr si, 8
                B(0x4E);                                     // dec si
                B(0x81); B(0xE6); W(0x00FF);                 // and si, FFh (M = (uint8_t)(r - 1))
                B(0x39); B(0xFE);                            // cmp si, di
                B(0x76); B(0x02); B(0x89); B(0xFE);          // jbe +2; mov si, di (k = min(M, k_max))
                B(0x89); B(0x36); W(SA(arg));                // mov [arg], si
                B(0x2A); B(0x06 | (xr << 3)); W(SA(arg));    // sub xr, [arg]
                B(0xC1); B(0xE6); B(4);                      // shl si, 4 (k * 16)
                B(0x29); B(0x36); W(SA(cycles));             // sub [cycles], si
                land8(done);
            } else {
                // T = 14 cycles/turn: fast path without DIV when M * 14 < cycles
                B(0x8B); B(0x3E); W(SA(cycles));             // mov di, [cycles]
                B(0x83); B(0xFF); B(14);                     // cmp di, 14
                uint8_t *done = jcc8(0x7E);                  // jle done (k_max = 0)
                B(0x89); B(0xC6 | (r16 << 3));               // mov si, r16
                if (hi) { B(0xC1); B(0xEE); B(8); }          // shr si, 8
                B(0x4E);                                     // dec si
                B(0x81); B(0xE6); W(0x00FF);                 // and si, FFh (M)
                B(0x6B); B(0xFE); B(14);                     // imul di, si, 14 (M * 14 <= 3570)
                B(0x39); B(0x3E); W(SA(cycles));             // cmp [cycles], di
                uint8_t *apply = jcc8(0x7F);                 // jg apply (M * 14 < cycles: k = M)
                B(0x50); B(0x52);                            // push ax; push dx
                B(0xA1); W(SA(cycles));                      // mov ax, [cycles]
                B(0x48);                                     // dec ax (cycles - 1 >= 14)
                B(0x31); B(0xD2);                            // xor dx, dx
                B(0xBE); W(14);                              // mov si, 14
                B(0xF7); B(0xF6);                            // div si (AX = k_max)
                B(0x89); B(0xC6);                            // mov si, ax (k = k_max)
                B(0x6B); B(0xFE); B(14);                     // imul di, si, 14 (k * 14)
                B(0x5A); B(0x58);                            // pop dx; pop ax
                land8(apply);
                B(0x89); B(0x36); W(SA(arg));                // mov [arg], si
                B(0x2A); B(0x06 | (xr << 3)); W(SA(arg));    // sub xr, [arg]
                B(0x29); B(0x3E); W(SA(cycles));             // sub [cycles], di
                land8(done);
            }
        }
    }
    {
        const ins_t *I = ins;
        for (i = 0; i < n; i++, I++) {
            if (I->pre == 0xED && I->op == 0xA3) {
                uint16_t row = row_at(i, n);
                if (row) {
                    if (!emit_row(i, row))
                        return 0;
                    i++;
                    I++;
                    continue;
                }
                int g = pairs_at(i, n);
                if (g) {
                    if (!emit_pairs(i, g))
                        return 0;
                    i += 5 * g - 1;
                    I += 5 * g - 1;
                    continue;
                }
            } else if (I->pre == 0 && (I->op == 0x7E || I->op == 0x0A || I->op == 0x1A)) {
                uint8_t vk, vsrc, vpr, vpd, vnp;
                int vspan = vcopy_at(i, n, &vk, &vsrc, &vpr, &vpd, &vnp);
                if (vspan) {
                    if (!emit_vcopy(i, vspan, vk, vsrc, vpr, vpd, vnp))
                        return 0;
                    i += vspan - 1;
                    I += vspan - 1;
                    continue;
                }
            }
            // emit() can only refuse an instruction that decode() accepted by
            // mistake: the liveness analysis assumed it, so give up entirely
            if (!emit(i))
                return 0;
            if (I->term == T_END || I->term == T_END_AFTER) { i++; goto done; }
        }
    }
    if (i == 0) return 0;
    // fell off the end: continue at the next instruction (or the target
    // of a followed jump)
    {
        const ins_t *L = &ins[i - 1];
        uint16_t t = L->pc + L->len;
        if (L->trace == 1) t = L->op == 0xC3 ? L->nn : (uint16_t) (L->pc + 2 + (int8_t) L->nn);
        if (L->trace == 3) t = L->nn;   // (after an inline routine)
        exit_const(t, 0);
    }
done:
    if (cold_n && !cold_fail)
        cold_emit_all();
    if (cold_fail) {
        if (!cold_off) {
            cold_off = 1;
            goto retry_gen;
        }
        return 0;
    }
#ifdef GGSWAN_SF7000
    // (what the interpreter runs where it is, is not translated code)
    if (env->mark)
        for (int k = 0; k < i; k++)
            if (!ins[k].step)
                env->mark(ins[k].pc, ins[k].len);
#endif
    res->length = (uint16_t) (o - o_start);
    res->z80_instr = i;
    res->z80_end = ins[i - 1].pc + ins[i - 1].len;
    return 1;
}
