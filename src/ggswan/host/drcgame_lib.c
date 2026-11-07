// Recompiler test on real games: the Game Gear machine (gg.c) with a host
// platform, plus snapshot/restore so that the same block can be run by the
// recompiler and by the interpreter. Driven by drctest.py --rom.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gg.h"
#include "../core/z80.h"
#include "../drc/drc.h"

uint8_t rom[512 * 1024];
uint32_t rom_size;
uint8_t ram_buf[0x2000], vram_buf[0x4000], cart_buf[0x8000];
uint8_t *gg_ram = ram_buf;
uint8_t *gg_vram = vram_buf;
drc_state_t st;

const uint8_t *gg_platform_bank(uint8_t slot, uint16_t bank) { (void) slot; return rom + (uint32_t) bank * 0x4000; }
uint8_t *gg_platform_cart_ram(uint8_t page) { return cart_buf + page * 0x4000; }
void gg_platform_line(uint16_t line) { (void) line; }
void gg_platform_psg(uint8_t v) { (void) v; }
void gg_platform_stereo(uint8_t v) { (void) v; }
void gg_platform_vram_write(uint16_t a, uint8_t v) { (void) a; (void) v; }
void gg_platform_cram_write(uint8_t i) { (void) i; }
void gg_platform_vdp_reg(uint8_t r, uint8_t v) { (void) r; (void) v; }
void gg_platform_vram_sync(void) {}
void gg_platform_vram_sync_at(uint16_t a) { (void) a; }
uint8_t gg_platform_vram_defer(uint16_t a, uint8_t v) { (void) a; (void) v; return 0; }

int t_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    rom_size = fread(rom, 1, sizeof(rom), f);
    fclose(f);
    uint32_t banks = (rom_size + 0x3FFF) / 0x4000;
    if (banks < 4) banks = 4;
    gg_init(banks);
    return banks;
}
void t_frames(int n) { while (n--) gg_run_frame(); }
// run the interpreter for about n cycles (with interrupts, like the game)
void t_run(int n) { z80_break = 0; z80_run(n); }

static struct { gg_t gg; z80_t z; uint8_t ram[0x2000], vram[0x4000], cart[0x8000], cram[64]; } snap;
void t_snapshot(void) {
    snap.gg = gg; snap.z = z80;
    memcpy(snap.ram, ram_buf, sizeof(ram_buf)); memcpy(snap.vram, vram_buf, sizeof(vram_buf));
    memcpy(snap.cart, cart_buf, sizeof(cart_buf)); memcpy(snap.cram, gg_cram, 64);
}
void t_restore(void) {
    gg = snap.gg; z80 = snap.z;
    memcpy(ram_buf, snap.ram, sizeof(ram_buf)); memcpy(vram_buf, snap.vram, sizeof(vram_buf));
    memcpy(cart_buf, snap.cart, sizeof(cart_buf)); memcpy(gg_cram, snap.cram, 64);
    gg_remap();
}
// a digest of the machine state that registers do not cover
uint32_t t_digest(void) {
    uint32_t h = 2166136261u;
    #define HB(b) h = (h ^ (b)) * 16777619u
    for (int i = 0; i < 0x4000; i++) HB(vram_buf[i]);
    for (int i = 0; i < 64; i++) HB(gg_cram[i]);
    for (int i = 0; i < 16; i++) HB(gg.vdp.reg[i]);
    HB(gg.vdp.addr); HB(gg.vdp.addr >> 8); HB(gg.vdp.code); HB(gg.vdp.latch_full);
    return h;
}

uint8_t *t_ram(void) { return ram_buf; }
uint8_t *t_rom(void) { return rom; }
uint8_t *t_cart(void) { return cart_buf; }
uint8_t *t_mapper(void) { return gg.mapper; }
int t_rom_banks(void) { return gg.rom_banks; }
int t_peek(uint16_t a) { return z80_read(a); }
z80_t *t_z80(void) { return &z80; }
drc_state_t *t_st(void) { return &st; }

int t_step(void) {
    z80.cycles = 0;
    uint8_t irq = z80.irq; z80.irq = 0;
    z80_step();
    z80.irq = irq;
    return (int) -z80.cycles;
}
void t_helper(void) {
    switch (st.op) {
    case DRC_H_WRITE: case DRC_H_WRITE_MAP: z80_write(st.arg, st.val); break;
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
    drc_env_t env = { z80_addr, state_addr, exit_off, helper_off, ram_delta, 0, 0, 0, 0 };
    return drc_translate(&env, pc, out, out_off, out_max, res);
}
void t_keys(int k) { gg.keys = k; }
