// Test library for the recompiler: a Game Gear-like memory map (Sega
// mapper, 8 KiB RAM mirrored at C000-FFFF), the Z80 interpreter, the
// translator and the C side of the helper routine. Driven by drctest.py.
#include <stdint.h>
#include <string.h>
#include "../core/z80.h"
#include "../drc/drc.h"

uint8_t rom[512 * 1024];
uint8_t ram[0x2000];
uint8_t cart_ram[0x8000];
uint8_t mapper[4];
uint32_t rom_mask = sizeof(rom) - 1;
drc_state_t st;          // C copy of the state block (cycles, op, arg, val)
uint8_t in_value = 0xFF;
uint32_t out_log[256];
int out_count;

uint8_t z80_read(uint16_t a) {
    if (a >= 0xC000) return ram[a & 0x1FFF];
    if (a < 0x400) return rom[a];
    if (a >= 0x8000 && (mapper[0] & 8)) return cart_ram[(a & 0x3FFF) | ((mapper[0] & 4) << 12)];
    uint32_t bank = mapper[1 + (a >> 14)];
    return rom[((bank << 14) | (a & 0x3FFF)) & rom_mask];
}
void z80_write(uint16_t a, uint8_t v) {
    if (a >= 0xC000) {
        ram[a & 0x1FFF] = v;
        if (a >= 0xFFFC) mapper[a - 0xFFFC] = v;
        return;
    }
    if (a >= 0x8000 && (mapper[0] & 8)) cart_ram[(a & 0x3FFF) | ((mapper[0] & 4) << 12)] = v;
}
uint8_t z80_in(uint8_t p) { (void) p; return in_value; }
void z80_out(uint8_t p, uint8_t v) { if (out_count < 256) out_log[out_count++] = (p << 8) | v; }

int t_peek(uint16_t a) { return z80_read(a); }
z80_t *t_z80(void) { return &z80; }
drc_state_t *t_st(void) { return &st; }
void t_init(void) { z80_init(); out_count = 0; }

// one instruction with the interpreter; returns cycles
int t_step(void) {
    z80.cycles = 0;
    uint8_t irq = z80.irq; z80.irq = 0;
    int32_t r = z80_run(1);
    z80.irq = irq;
    return 1 - r;
}

// the helper routine's C side (registers already in z80)
void t_helper(void) {
    switch (st.op) {
    case DRC_H_WRITE: z80_write(st.arg, st.val); break;
    case DRC_H_OUT: z80_out(st.arg, st.val); break;
    case DRC_H_IN: st.val = z80_in(st.arg); break;
    case DRC_H_STEP:
        uint8_t n = st.val ? st.val : 1;
        z80.pc = st.arg;
        while (n--) {
            uint16_t p = z80.pc;
            do { st.cycles -= t_step(); } while (z80.pc == p);   // (repeating ones)
        }
        break;
    }
}

int t_translate(uint16_t z80_addr, uint16_t state_addr, uint16_t exit_off,
                uint16_t helper_off, int16_t ram_delta,
                uint16_t pc, uint8_t *out, uint16_t out_off, uint16_t out_max,
                drc_result_t *res) {
    drc_env_t env = { z80_addr, state_addr, exit_off, helper_off, ram_delta };
    return drc_translate(&env, pc, out, out_off, out_max, res);
}
