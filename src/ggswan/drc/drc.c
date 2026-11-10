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

/* === Flag state === */

enum { K_AH = 0, K_ADD, K_SUB, K_AND, K_OR, K_INC, K_DEC, K_CARRY };
// K_CARRY: x86 CF holds Z80 C, and C is the only flag still read (the
// others are dead: the liveness analysis says nobody reads them)
static uint8_t fkind;      // where the flags are (K_AH: in AH)
static uint8_t cf_valid;   // K_INC / K_DEC: x86 CF holds Z80 C
static uint8_t hl_es;      // ES is set up for the slot of HL

// The flags in AH; need: the ones read later (the others may be left
// wrong: nobody reads them before they are written again)
static void materialize_need(uint8_t need) {
    switch (fkind) {
    case K_ADD: case K_INC: case K_SUB: case K_DEC: {
        uint8_t mask = (fkind == K_SUB || fkind == K_DEC) ? 0xD3 : 0xD1;
        B(0x9F);                         // lahf
        if (need & FP) {
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

static uint8_t force_nolink;   // the next exit must return to the runtime
// (a helper that changes the mapping makes the runtime end the block
// without following links: see drc_rt.c)

static int linkable(uint16_t target) {
    // (blocks in work RAM check their bytes at entry: they can be linked)
    if (!force_nolink && env->ram_blocks && target >= 0xC000 && target < 0xDFC0) return 1;
    if (force_nolink || target >= 0xC000) return 0;
    return (target >> 14) == 0 || (target >> 14) == (block_pc >> 14);
}

// to another ROM slot: linked behind a check of the slot's bank
static int guarded(uint16_t target) {
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
         * recognizes these exits by the MOV BYTE [ic] at X + 6.
         */
        B(0x89); B(0x06 | (reg16 << 3)); W(ZA(pc));          // mov [pc], reg
        B(0x39); B(0xC0 | (reg16 << 3) | reg16);             // cmp reg, reg (later: cmp reg, target)
        B(0x90); B(0x90);
        uint8_t *d = jcc8(0x75);                             // jne D
        B(0x81); B(0x2E); W(SA(cycles)); W(cyc + extra);     // sub word [cycles], n
        B(0x7E); B(5);                                       // jle X
        uint16_t l = here();
        B(0xEB); B(3); B(0x90); B(0x90); B(0x90);            // L: jmp X until linked
        B(0xC7); B(0x06); W(SA(link)); W(l);                 // X: mov [link], L
        B(0xC6); B(0x06); W(SA(ic)); B(1 + (block_pc >> 14));   // mov byte [ic], slot + 1
        jmp_near(env->exit_off);
        land8(d);
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
    if (addr < 0xFFFC) return 1;
    uint8_t slot = addr == 0xFFFD ? 0 : addr == 0xFFFE ? 1 : 2;
    return slot != 0 && slot != (block_pc >> 14);
}

static void write_const(uint16_t addr, uint8_t src, uint8_t live) {
    if (addr >= 0xC000 && addr < 0xE000) {
        if (src == X_AX) { B(0xA2); }
        else { B(0x88); B(0x06 | (src << 3)); }
        W(addr + env->ram_delta);
        return;
    }
    clobber(live);
    uint8_t *same = 0;
    if (addr >= 0xFFFD && env->psg_queue) {
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
    }
    B(0x88); B(0x06 | (src << 3)); W(SA(val));
    uint8_t *mapped = 0;
    uint16_t map_off = addr == 0xFFFE ? env->map1_off : addr == 0xFFFF ? env->map2_off : 0;
    if (map_off && map_write_safe(addr)) {
        // another slot's bank (not slot 0, not the block's own): the
        // routine maps it (drc_rt.c), the C side later; carry: the helper
        call_near(map_off);
        mapped = jcc8(0x73);                         // jnc done
    }
    B(0xC7); B(0x06); W(SA(arg)); W(addr);
    helper(addr >= 0xFFFC && map_write_safe(addr) ? DRC_H_WRITE_MAP : DRC_H_WRITE);
    land8(mapped);
    land8(same);
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

/*
 * Fast path of a write to the VDP data port (VRAM; CRAM goes to *slow1).
 * The Game Gear VRAM is the WS tile memory: tile data needs nothing more;
 * name table writes also update the WS screen map (routine at
 * nt_stub_off), sprite table writes mark the sprites to be rebuilt.
 * src: x86 8-bit register. x86 flags destroyed; uses SI and DI.
 */
static void vram_fast(uint8_t src, uint8_t **slow1) {
    B(0x80); B(0x3E); W(VA(code)); B(3);                 // cmp byte [code], 3
    uint8_t *cram_end = 0;
    if (env->cram_off) {
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
        call_near(env->cls_off);
        uint8_t *done = jcc8(0xEB);
        land8(plain);
        B(0x88); B(0x84 | (src << 3)); W(env->vram_addr);    // mov [si + vram], src
        land8(done);
        B(0x46);                                         // inc si
        B(0x81); B(0xE6); W(0x3FFF);                     // and si, 3FFFh
        B(0x89); B(0x36); W(VA(addr));                   // mov [addr], si
        B(0xC6); B(0x06); W(VA(latch_full)); B(0);       // mov byte [latch_full], 0
        land8(cram_end);
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
    // live: stored, then the name table routine or sprites_dirty
    B(0x50);                                         // push ax
    B(0xA0); W(VA(read_buffer));                     // mov al, [read_buffer]
    B(0x88); B(0x84); W(e->vram_addr);               // mov [si + vram], al
    B(0x58);                                         // pop ax
    B(0xF6); B(0x85); W(SA(vram_class)); B(1);       // test byte [di + class], 1
    uint8_t *sat = jcc8(0x74);                       // jz sat
    B(0xE9); W((uint16_t) (e->nt_stub_off - (o_base + (uint16_t) (o - o_start) + 2)));   // jmp NT
    land8(sat);
    B(0xC6); B(0x06); W(SA(sprites_dirty)); B(1);    // sprites_dirty = 1
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
 * Routine for a run of OUTI (dec: OUTD), shared by the blocks of a code
 * bank. In: DI = k (OUTIs), SI = Z80 address of the run. VRAM uploads
 * through the fast path; returns CF = 0 with the flags of the last OUTI
 * in AH. Anything else (CRAM, another port): CF = 1, with val = OUTIs
 * left, arg = address of the first one left, their cycles added back to
 * the counter (the block calls the STEP helper). Uses SI, DI, ES.
 */
#define OUTI_SHORT 4   // runs up to this long: a byte loop instead of REP MOVSB

uint16_t drc_emit_run_stub(const drc_env_t *e, uint8_t *out, uint16_t out_off, uint8_t dec) {
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
    vram_fast(4 /* AH */, &s1);
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
        if (!dec) {
            /*
             * CRAM (palette uploads, ~60 bytes a frame in Super Monaco GP):
             * the bytes here instead of one CRAM routine call each through
             * the loop. As put_cram (drc_rt.c): the even byte latched, the
             * odd one writes the color and its WS copies; read buffer,
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
            B(0x89); B(0x84); W(0xFE00);             // mov [si + FE00h], ax
            B(0x83); B(0xFE); B(0x20);               // cmp si, 20h
            B(0x72); B(0x04);                        // jb +4
            B(0x89); B(0x84); W(0xFEE0);             // mov [si + FEE0h], ax (FF00h + color * 2 - 20h)
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
            plain = jcc8(0x74);                      // jz plain (the usual case)
            B(0xF6); B(0xC2); B(0x01);               // test dl, 1 (name table)
            uint8_t *not_nt = jcc8(0x74);            // jz (sprite table only)
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
            land8(not_nt);
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
        land8(plain);
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
uint16_t drc_emit_ldi_stub(const drc_env_t *e, uint8_t *out, uint16_t out_off, uint8_t dec) {
    env = e;
    o = o_start = out; o_base = out_off;
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
    B(0x3D); W(0xDA);                                // cmp ax, DAh
    B(0x76); B(0x03);                                // jbe +3
    B(0x2D); W(6);                                   // sub ax, 6
    B(0x88); B(0xC2);                                // mov dl, al
    B(0x58);                                         // pop ax
    B(0x88); B(0xD0);                                // mov al, dl
    B(0x5F); B(0x5A);                                // pop di, dx
    B(0xF8); B(0xC3);                                // clc; ret
    return (uint16_t) (o - o_start);
}

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
        B(0x3D); W(GG_VIEW_Y);                       // cmp ax, VIEW_Y
        uint8_t *inview = jcc8(0x77);                // ja inview
        // before the view: the value for its first line
        B(0x88); B(0x16); W(SA(rec_val));            // mov [rec_val], dl
        B(0xC6); B(0x06); W(SA(rec_n)); B(1);        // rec_n = 1
        uint8_t *last1 = jcc8(0xEB);                 // jmp set_last
        land8(inview);
        B(0x2D); W(GG_VIEW_Y);                       // sub ax, VIEW_Y (y)
        B(0x3D); W(GG_VIEW_H);                       // cmp ax, VIEW_H
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
        B(0x3D); W(GG_PRESENT_LINE + 1 - GG_VIEW_Y); // cmp ax, 169 - VIEW_Y
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
            if (I->nn < 0xC000 || I->nn >= 0xDFFF) I->step = 1;
            return;
        case 0x4B: case 0x5B: case 0x7B:                            // LD rr,(nn)
            I->nn = rd(pc + 2) | (rd(pc + 3) << 8); I->len = 4; return;
        case 0x45: case 0x4D: case 0x55: case 0x5D:
        case 0x65: case 0x6D: case 0x75: case 0x7D:                 // RETN, RETI
            I->term = T_END; return;
        }
        if ((op & 0xC7) == 0x42) {                                  // SBC/ADC HL,rr
            I->fwrite = FALL; I->fread = FC; return;
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
            I->d = (int8_t) rd(pc + 2); I->len = 3; return;
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

static void push_imm(uint16_t value) {
    B(0x8D); B(0x6E); B(0xFE);                                  // lea bp, [bp-2]
    B(0xC7); B(0x86); W(env->ram_delta); W(value);              // mov word [bp+d], imm
}

// After a call to the OUTI/OUTD run routine (CF = 1: the rest to the
// helper, which makes the flags; CF = 0: AH = the last byte): the flags of
// the last OUTI, S and Z from B, N from bit 7 of the byte, only when live
static void run_flags(uint8_t h, int live) {
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
    uint8_t *done = jcc8(0xEB);                          // jmp done
    land8(slow);
    helper(h);                                           // the OUTIs left (CRAM, other ports)
    land8(done);
}

// Returns 0 if the instruction could not be emitted (block ends before it)
static int emit(int i) {
    ins_t *I = &ins[i];
    // flags whose current value is still needed: read by this instruction,
    // or needed later and not rewritten by it
    uint8_t live = (live_out[i] & ~I->fwrite) | I->fread;
    uint8_t op = I->op;
    uint16_t next = I->pc + I->len;

    if (I->trace == 1) {   // the block goes on at the target
        cyc += op == 0x18 ? 12 : 10;
        return 1;
    }
    if (I->trace == 2) {   // CALL nn, routine inline: push the return address
        push_imm(next);
        cyc += 17;
        return 1;
    }
    if (I->trace == 3) {   // its RET: go on inline if it returns to the CALL
        materialize();
        B(0x8B); B(0xB6); W(env->ram_delta);       // mov si, [bp+d]
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
        if (I->term == T_END_AFTER) { force_nolink = 1; exit_const(next, 0); }
        return 1;
    }

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
        case 0x45: case 0x4D: case 0x55: case 0x5D:
        case 0x65: case 0x6D: case 0x75: case 0x7D:   // RETN, RETI: IFF1 = IFF2, return
            B(0x50);                                  // push ax
            B(0xA0); W(ZA(iff2));                     // mov al, [iff2]
            B(0xA2); W(ZA(iff1));                     // mov [iff1], al
            B(0x58);                                  // pop ax
            B(0x8B); B(0xB6); W(env->ram_delta);      // mov si, [bp + d]
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
            B(0x80); B(0xFF); B(0xC0); uint8_t *j1 = jcc8(0x72);   // cmp bh, C0h; jb slow
            B(0x80); B(0xFF); B(0xE0); uint8_t *j2 = jcc8(0x73);   // cmp bh, E0h; jae slow
            B(0x89); B(0xC7);                         // mov di, ax (F, A)
            if (op == 0x67) {
                // AX = 0 : A low : (HL), rotated right: AL = A low : (HL) high
                // (the new (HL)), AH high nibble = (HL) low (the new A low)
                B(0x88); B(0xC4);                     // mov ah, al
                B(0x80); B(0xE4); B(0x0F);            // and ah, 0Fh
                B(0x8A); B(0x87); W(env->ram_delta);  // mov al, [bx + ram]
                B(0xC1); B(0xC8); B(0x04);            // ror ax, 4
                B(0x88); B(0x87); W(env->ram_delta);  // mov [bx + ram], al
                B(0x88); B(0xE0);                     // mov al, ah
                B(0xC0); B(0xE8); B(0x04);            // shr al, 4
            } else {
                // AX = (HL) : A low : 0, rotated left: AH = (HL) low : A low
                // (the new (HL)), AL low nibble = (HL) high (the new A low)
                B(0x8A); B(0xA7); W(env->ram_delta);  // mov ah, [bx + ram]
                B(0xC0); B(0xE0); B(0x04);            // shl al, 4
                B(0xC1); B(0xC0); B(0x04);            // rol ax, 4
                B(0x88); B(0xA7); W(env->ram_delta);  // mov [bx + ram], ah
                B(0x24); B(0x0F);                     // and al, 0Fh
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
                     // fetches, bit 7 kept), A = R, flags as LD A,I. (The
                     // translated code does not count R otherwise.)
            clobber(live);
            B(0x50);                                  // push ax
            B(0xA0); W(ZA(r));                        // mov al, [r]
            B(0x88); B(0xC4);                         // mov ah, al
            B(0x80); B(0xE4); B(0x80);                // and ah, 80h
            B(0x04); B(0x02);                         // add al, 2
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
            if (env->vdp_addr) {
                B(0x89); B(0xCE);                        // mov si, cx
                B(0x81); B(0xE6); W(0x00C1);             // and si, C1h
                B(0x81); B(0xFE); W(0x0080);             // cmp si, 80h (VDP data)
                s0 = jcc8(0x75);
                vram_fast(src, &s1);
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
            materialize();
            B(0x9E);                                         // sahf (CF = C)
            B(0x89); B(0xDE);                                // mov si, bx
            B(sbc ? 0x19 : 0x11); B(0xC3 | (r16x[rr] << 3));  // sbb/adc bx, rr
            B(0x9F);                                         // lahf (S Z C)
            B(0x70); B(0x05);                                // jo +5
            B(0x80); B(0xE4); B(0xC1);                       // and ah, C1h (V = 0)
            B(0xEB); B(0x06);                                // jmp +6
            B(0x80); B(0xE4); B(0xC1);                       // and ah, C1h
            B(0x80); B(0xCC); B(0x04);                       // or ah, 4 (V = 1)
            if (sbc) { B(0x80); B(0xCC); B(0x02); }          // N
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
            materialize();
            B(0xBF); W(I->count);                            // mov di, k
            B(0xBE); W(I->pc);                               // mov si, pc
            call_near(op == 0xA0 ? env->ldi_off : env->ldi_off + DRC_LDD_DELTA);
            uint8_t *done = jcc8(0x73);                      // jnc done
            helper(DRC_H_STEP);                              // the LDIs left
            land8(done);
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
            // destination outside work RAM: the ones done, then one turn
            // with the interpreter, then this instruction again
            land8(slow);
            B(0x2B); B(0x36); W(SA(val));                    // sub si, [val] (done)
            B(0x6B); B(0xF6); B(21);                         // imul si, si, 21
            B(0x8B); B(0x3E); W(SA(val));                    // mov di, [val]
            B(0xC1); B(0xE7); B(0x04);                       // shl di, 4
            B(0x01); B(0xFE);                                // add si, di (undo the routine's)
            B(0x29); B(0x36); W(SA(cycles));                 // sub [cycles], si
            B(0xC7); B(0x06); W(SA(val)); W(1);              // val = 1
            B(0xC7); B(0x06); W(SA(arg)); W(I->pc);          // arg = pc
            helper(DRC_H_STEP);
            fkind = K_AH;
            exit_const(I->pc, 0);
            return 1;
        }
        case 0xB3: {   // OTIR (the OUTI routine, up to 64 per turn)
            materialize();
            B(0x89); B(0xCF);                                // mov di, cx
            B(0xC1); B(0xEF); B(0x08);                       // shr di, 8 (B)
            B(0x4F);                                         // dec di
            B(0x83); B(0xFF); B(63);                         // cmp di, 63
            B(0x72); B(0x03); B(0xBF); W(63);                // jb +3; mov di, 63
            B(0x47);                                         // inc di (B = 0: 64)
            B(0x57);                                         // push di (k)
            B(0xBE); W(I->pc);                               // mov si, pc
            call_near(env->outi_off);
            run_flags(DRC_H_OUTI_RUN, 1);
            B(0x5E);                                         // pop si (k)
            B(0x6B); B(0xF6); B(21);                         // imul si, si, 21
            B(0x29); B(0x36); W(SA(cycles));                 // sub [cycles], si
            fkind = K_AH; hl_es = 0;
            B(0x84); B(0xED);                                // test ch, ch
            uint8_t *fin = jcc8(0x74);                       // jz fin
            exit_const(I->pc, 0);                            // more to send
            land8(fin);
            B(0x83); B(0x06); W(SA(cycles)); B(5);           // add [cycles], 5 (the last: 16)
            exit_const(I->pc + 2, 0);
            return 1;
        }
        case 0xA3: case 0xAB: {   // a run of OUTI / OUTD (routine in the stubs)
            clobber(0);
            B(0xBF); W(I->count);                            // mov di, k
            B(0xBE); W(I->pc);                               // mov si, pc
            call_near(op == 0xA3 ? env->outi_off : env->outi_off + DRC_OUTD_DELTA);
            run_flags(op == 0xA3 ? DRC_H_OUTI_RUN : DRC_H_OUTD_RUN, live_out[i] & FALL);
            fkind = K_AH; hl_es = 0;
            cyc += 16 * I->count;
            return 1;
        }
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
        case 0x34: case 0x35: {   // INC/DEC (IX+d): work RAM directly
            uint8_t dec = op == 0x35;
            materialize();
            ix_addr(I->pre, I->d);
            B(0x81); B(0xFE); W(0xC000); uint8_t *j1 = jcc8(0x72);
            B(0x81); B(0xFE); W(0xE000); uint8_t *j2 = jcc8(0x73);
            B(0x9E);                                              // sahf
            B(0xFE); B(0x84 | (dec ? 0x08 : 0)); W(env->ram_delta);   // inc/dec byte [si+d]
            fkind = dec ? K_DEC : K_INC; cf_valid = 1;
            materialize();
            B(0x83); B(0x2E); W(SA(cycles)); B(23);
            uint8_t *j3 = jcc8(0xEB);
            land8(j1); land8(j2);
            B(0xC7); B(0x06); W(SA(arg)); W(I->pc);
            B(0xC6); B(0x06); W(SA(val)); B(1);
            helper(DRC_H_STEP);
            land8(j3);
            fkind = K_AH;
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
                B(0x87); B(0x06); W(ZA(a_));
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
                    B(0x8B); B(0x36); W((uint16_t) (I->pc + 1 + env->ram_delta));   // mov si, [operand]
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
            case 2: {   // OUT (n),A
                if (env->ctl_off && I->nn < 0x40 && I->nn != 0x06) {
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
                    uint8_t *room = jcc8(0x72);          // jb room
                    helper(DRC_H_PSG);
                    land8(room);
                    cyc += 11;
                    return 1;
                }
                uint8_t *s1 = 0, *done = 0;
                if (env->vdp_addr && (I->nn & 0xC1) == 0x80) {
                    vram_fast(X_AX, &s1);
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
                if (env->pad_in && (I->nn == 0x00 || I->nn >= 0xC0)) {
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
                uint8_t *go = jcc8(0x74);                 // je go
                B(0xC6); B(0x06); W(ZA(ei_pending)); B(1);
                force_nolink = 1;
                exit_const(next, 0);
                force_nolink = 0;
                land8(go);
                fkind = K_AH;
                return 1;
            }
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

int drc_translate(const drc_env_t *e, uint16_t pc, uint8_t *out,
                  uint16_t out_off, uint16_t out_max, drc_result_t *res) {
    env = e;
    rd_len = 0;   // (the mapping may have changed since the last block)
    // code in work RAM (C000-DFFF): short blocks that check their own bytes
    // when entered (see ram_check)
    uint8_t ram = pc >= 0xC000;
    uint8_t long_run = 0;   // a block in RAM of OUTIs only, longer than RAM_BLOCK_MAX
    if (ram && (!env->ram_blocks || pc >= 0xDFC0)) return 0;
    // decode
    int n = 0;
    uint16_t p = pc;
    // every instruction fits in 96 bytes of x86 code: stop early enough
    int max_n = (out_max - 128) / 96;
    if (max_n > MAX_INSTR) max_n = MAX_INSTR;
    uint8_t inlined = 0;
    int c_known = -1;   // C as set by LD C,n earlier in the block (-1: not known)
    while (n < max_n) {
        // stay inside the block's 16 KiB slot (or slot 0, through a jump)
        if (n && ((p ^ pc) & 0xC000) && !(p < 0x4000 && !ram)) break;
        if (ram && p - pc > RAM_BLOCK_MAX - 4) break;
        // a long block stops where one already translated starts, and links
        // to it: Shinobi uploads tiles with an unrolled OUTI / OUTI / DEC L
        // x4 routine (0720-0950h) entered at 221 different points, each a
        // new block of 48 instructions: 60% of all it translated
        if (n >= 8 && !ram && env->known_start && env->known_start(p)) break;
        decode(p, &ins[n]);
        ins[n].dyn = 0;
        ins[n].trace = 0;
        if (ram && n == 0 && ins[0].pre == 0xED && ins[0].op == 0xA3 && ins[0].count > RAM_BLOCK_MAX / 2
            && !env->no_fast) {
            // a block in RAM made only of a run of OUTIs (Aladdin's
            // unrolled VRAM upload at C6FF-C7F8, entered at any of them):
            // the whole run, its bytes checked by one REPE SCASW
            // (ram_check) instead of one compare per OUTI; the block ends
            // after it. The same as the blocks of 12 OUTIs it replaces
            // (translate): whole pieces of 12 unless the run ends here (64:
            // more may follow), none starting from DFC0 (interpreted)
            uint8_t k = ins[0].count;
            if (k == 64) k = 60;
            uint16_t pieces = (uint16_t) ((0xDFC0 - pc + 23) / 24);
            if (pieces < 6 && k > 12 * pieces) k = (uint8_t) (12 * pieces);
            ins[0].count = k;
            ins[0].len = k * 2;
            long_run = k > RAM_BLOCK_MAX / 2;
            n = 1;
            p += ins[0].len;
            break;
        }
        if (ram && p + ins[n].len - pc > RAM_BLOCK_MAX) {
            // a run of LDI/OUTI... (up to 128 bytes) longer than a block
            // in RAM may be (ram_check checks at most RAM_BLOCK_MAX
            // bytes): only the part that fits, the rest in the next block
            uint8_t room = (uint8_t) (RAM_BLOCK_MAX - (p - pc));
            if (ins[n].count > 1 && room >= 2) {
                ins[n].count = room / 2;
                ins[n].len = ins[n].count * 2;
            } else {
                break;
            }
        }
        {
            // IN r,(C) with C known (LD C,n, then only instructions that
            // leave C alone): the VDP data port natively, as IN A,(n)
            // (Aladdin reads VRAM with LD C,BEh / IN C,(C): a step each,
            // ~3 a frame). d = 1 marks it (emit).
            ins_t *K = &ins[n];
            if (K->pre == 0xED && (K->op & 0xC7) == 0x40 && K->op != 0x70 && c_known >= 0
                && (c_known & 0xC1) == 0x80 && env->vdp_addr && env->ctl_off && !env->no_fast) {
                K->step = 0; K->count = 0; K->nn = (uint16_t) c_known; K->d = 1;
                K->fwrite = FALL & ~FC; K->fread = FC;
            }
            if (K->pre == 0 && K->op == 0x0E) c_known = K->nn;
            else if (K->pre == 0 && K->op == 0x01) c_known = K->nn & 0xFF;
            else if (!(K->pre == 0 && (K->op == 0x00 || K->op == 0xF3 || K->op == 0xD5 || K->op == 0xE5
                                         || K->op == 0xF5 || K->op == 0xC5 || K->op == 0xD1 || K->op == 0xE1
                                         || K->op == 0xF1)))
                c_known = -1;
        }
        if (ins[n].term == T_STOP) break;
        p += ins[n].len;
        n++;
        const ins_t *J = &ins[n - 1];
        if (!ram && !env->no_trace && J->pre == 0 && (J->op == 0xC3 || J->op == 0x18) && n < max_n - 1) {
            // JP nn / JR e: go on at the target, unless it leaves slot 0 and
            // the block's slot, or is already in the block (a loop)
            uint16_t t = J->op == 0xC3 ? J->nn : (uint16_t) (J->pc + 2 + (int8_t) J->nn);
            int seen = t >= 0xC000 || ((t ^ pc) & 0xC000 && t >= 0x4000);
            for (int k = 0; k < n && !seen; k++)
                if (ins[k].pc == t) seen = 1;
            if (!seen) {
                ins[n - 1].term = T_NONE;
                ins[n - 1].trace = 1;
                p = t;
                continue;
            }
        }
        if (!ram && !env->no_inline && J->pre == 0 && J->op == 0xCD && !inlined && n + CALL_INLINE_MAX + 2 < max_n
            && !(((J->nn ^ pc) & 0xC000) && J->nn >= 0x4000) && J->nn < 0xC000) {
            // CALL nn to a short routine (no jumps out of it, ends with RET):
            // its instructions follow inline (once per block)
            uint16_t q = J->nn;
            int k, ok = 0;
            for (k = 0; k <= CALL_INLINE_MAX; k++) {
                if (((q ^ pc) & 0xC000) && q >= 0x4000) break;
                decode(q, &ins[n + k]);
                ins[n + k].dyn = 0;
                ins[n + k].trace = 0;
                if (ins[n + k].pre == 0 && ins[n + k].op == 0xC9) { ok = 1; break; }
                if (ins[n + k].term != T_NONE || ins[n + k].step) break;
                q += ins[n + k].len;
            }
            if (ok) {
                inlined = 1;
                ins[n - 1].term = T_NONE;
                ins[n - 1].trace = 2;
                ins_t *R = &ins[n + k];
                R->term = T_NONE;
                R->trace = 3;
                R->nn = J->pc + 3;
                R->fread = FALL;   // (the flags are needed if it does not return here)
                n += k + 1;
                p = J->pc + 3;
                continue;
            }
        }
        if (J->term == T_END || J->term == T_END_AFTER) break;
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
    o = o_start = out; o_base = out_off; o_limit = out + out_max;
    block_start_off = out_off; block_pc = pc;
    fkind = K_AH; cf_valid = 0; hl_es = 0; cyc = 0;
    force_nolink = 0;
    idle_block = is_idle(n, pc, ram);
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
    // IN A,(7Eh) / CP n / JR cc back here: waiting for a line (Sonic
    // Spinball, ~66 polls a frame): the runtime skips the polls whose
    // result changes nothing, then this one runs as usual
    if (n == 3 && env->vcnt_off && !env->no_fast && ins[0].pre == 0 && ins[0].op == 0xDB
        && (ins[0].nn & 0xC1) == 0x40 && ins[1].pre == 0 && ins[1].op == 0xFE
        && ins[2].pre == 0 && (ins[2].op & 0xE7) == 0x20
        && (uint16_t) (ins[2].pc + 2 + (int8_t) ins[2].nn) == ins[0].pc) {
        B(0xC7); B(0x06); W(SA(arg)); W((uint16_t) (ins[1].nn & 0xFF) | (((ins[2].op >> 3) & 3) << 8));
        helper(DRC_H_VWAIT);
    }
    for (i = 0; i < n; i++) {
        int g = pairs_at(i, n);
        if (g) {
            if (!emit_pairs(i, g))
                return 0;
            i += 5 * g - 1;
            continue;
        }
        // emit() can only refuse an instruction that decode() accepted by
        // mistake: the liveness analysis assumed it, so give up entirely
        if (!emit(i))
            return 0;
        if (ins[i].term == T_END || ins[i].term == T_END_AFTER) { i++; goto done; }
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
    res->length = (uint16_t) (o - o_start);
    res->z80_instr = i;
    res->z80_end = ins[i - 1].pc + ins[i - 1].len;
    return 1;
}
