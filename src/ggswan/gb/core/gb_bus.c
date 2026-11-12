/**
 * gbswan - the Game Boy's bus: the memory map, the cartridge (none, MBC1,
 * MBC2, MBC3 without its clock yet, MBC5), the I/O registers, the timer,
 * the serial port (no other Game Boy: its own clock sends, FFh comes in),
 * the keys, the OAM DMA.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include "gb.h"

gb_t gb;
uint8_t gb_vram[0x2000];
uint8_t gb_wram[0x2000];
const uint8_t *gb_rom;
uint8_t *gb_cart_ram;
#ifdef GB_HOST
void (*gb_serial_hook)(uint8_t);
#endif

/* ---- cartridge ---- */

static void map_banks(void) {
    uint16_t mask = gb.rom_banks - 1;
    uint8_t rmask = gb.ram_banks ? gb.ram_banks - 1 : 0;
    switch (gb.mbc) {
    case 1:
        gb.rom0_bank = gb.mbc1_mode ? ((uint16_t) gb.bank_hi << 5) & mask : 0;
        gb.romx_bank = ((uint16_t) gb.bank_hi << 5 | (gb.bank_lo ? gb.bank_lo : 1)) & mask;
        gb.ram_bank = gb.mbc1_mode ? gb.bank_hi & rmask : 0;
        break;
    case 2:
        gb.romx_bank = (gb.bank_lo ? gb.bank_lo : 1) & mask;
        break;
    case 3:
        gb.romx_bank = (gb.bank_lo ? gb.bank_lo : 1) & mask;
        gb.ram_bank = gb.bank_hi & rmask;
        break;
    case 5:
        gb.romx_bank = ((uint16_t) (gb.mbc1_mode & 1) << 8 | gb.bank_lo) & mask;
        gb.ram_bank = gb.bank_hi & rmask;
        break;
    default:
        gb.romx_bank = 1 & mask;
        break;
    }
}

uint8_t gb_load(const uint8_t *rom, uint32_t size, uint8_t *ram) {
    if (size < 0x8000) return 1;
    if (rom[0x143] == 0xC0) return 3;
    uint8_t type = rom[0x147], battery = 0, mbc;
    switch (type) {
    case 0x00: case 0x08: mbc = 0; break;
    case 0x09: mbc = 0; battery = 1; break;
    case 0x01: case 0x02: mbc = 1; break;
    case 0x03: mbc = 1; battery = 1; break;
    case 0x05: mbc = 2; break;
    case 0x06: mbc = 2; battery = 1; break;
    case 0x11: case 0x12: mbc = 3; break;
    case 0x0F: case 0x10: case 0x13: mbc = 3; battery = 1; break;
    case 0x19: case 0x1A: case 0x1C: case 0x1D: mbc = 5; break;
    case 0x1B: case 0x1E: mbc = 5; battery = 1; break;
    default: return 2;
    }
    if (rom[0x148] > 8) return 1;
    uint16_t banks = 2 << rom[0x148];
    if (size < (uint32_t) banks * 0x4000) return 1;
    static const uint8_t ram_sizes[6] = { 0, 1, 1, 4, 16, 8 };
    if (rom[0x149] > 5) return 1;
    memset(&gb, 0, sizeof(gb));
    gb.mbc = mbc;
    gb.has_battery = battery;
    gb.rom_banks = banks;
    gb.ram_banks = mbc == 2 ? 0 : ram_sizes[rom[0x149]];
    if (!gb.ram_banks && mbc != 2) gb.has_battery = 0;
    gb_rom = rom;
    gb_cart_ram = ram;
    gb_reset();
    return 0;
}

uint32_t gb_ram_size(void) {
    if (!gb.has_battery) return 0;
    return gb.mbc == 2 ? 0x200 : (uint32_t) gb.ram_banks * 0x2000;
}

static void cart_write(uint16_t a, uint8_t v) {
    switch (gb.mbc) {
    case 1:
        switch (a >> 13) {
        case 0: gb.ram_on = (v & 15) == 10; break;
        case 1: gb.bank_lo = v & 0x1F; break;
        case 2: gb.bank_hi = v & 3; break;
        default: gb.mbc1_mode = v & 1; break;
        }
        break;
    case 2:
        if (a < 0x4000) {
            if (a & 0x100) gb.bank_lo = v & 15;
            else gb.ram_on = (v & 15) == 10;
        }
        break;
    case 3:
        switch (a >> 13) {
        case 0: gb.ram_on = (v & 15) == 10; break;
        case 1: gb.bank_lo = v & 0x7F; break;
        case 2: gb.bank_hi = v & 3; gb.rtc_sel = v >= 8 && v <= 12 ? v : 0; break;
        default: break;     // (the clock's latch: no clock yet)
        }
        break;
    case 5:
        switch (a >> 12) {
        case 0: case 1: gb.ram_on = (v & 15) == 10; break;
        case 2: gb.bank_lo = v; break;
        case 3: gb.mbc1_mode = v & 1; break;    // (the bank's ninth bit)
        case 4: case 5: gb.bank_hi = v & 15; break;
        default: break;
        }
        break;
    default:
        return;
    }
    map_banks();
}

static uint8_t ram_read(uint16_t a) {
    if (!gb.ram_on && gb.mbc) return 0xFF;
    if (gb.mbc == 2) return gb_cart_ram[a & 0x1FF] | 0xF0;
    if (gb.mbc == 3 && gb.rtc_sel) return 0;
    if (!gb.ram_banks) return 0xFF;
    return gb_cart_ram[(uint32_t) gb.ram_bank << 13 | (a & 0x1FFF)];
}

static void ram_write(uint16_t a, uint8_t v) {
    if (!gb.ram_on && gb.mbc) return;
    if (gb.mbc == 2) {
        gb_cart_ram[a & 0x1FF] = v & 15;
    } else if (gb.mbc == 3 && gb.rtc_sel) {
        return;
    } else if (gb.ram_banks) {
        gb_cart_ram[(uint32_t) gb.ram_bank << 13 | (a & 0x1FFF)] = v;
    } else {
        return;
    }
    gb.ram_dirty = 1;
}

/* ---- timer, serial, keys ---- */

static bool timer_signal(void) {
    static const uint8_t bit[4] = { 9, 3, 5, 7 };
    return (gb.io[GB_TAC] & 4) && (gb.div >> bit[gb.io[GB_TAC] & 3] & 1);
}
static void tima_inc(void) {
    if (++gb.io[GB_TIMA] == 0)
        gb.tima_state = 1;   // reads 0 for a cycle, then TMA and the interrupt
}
static void serial_clock(void) {
    if (!gb.serial_bits) return;
    gb.io[GB_SB] = gb.io[GB_SB] << 1 | 1;
    if (--gb.serial_bits == 0) {
        gb.io[GB_SC] &= 0x7F;
        gb.io[GB_IF] |= GB_INT_SERIAL;
    }
}
// DIV's 16 bits change: the timer counts on the fall of one of its bits
// (with the timer on), the serial port on the fall of bit 8
static void set_div(uint16_t v) {
    bool was = timer_signal();
    uint16_t old = gb.div;
    gb.div = v;
    if (was && !timer_signal()) tima_inc();
    if (((old + 4) & 0x100) && !((v + 4) & 0x100)) serial_clock();
}

static uint8_t p1_read(void) {
    uint8_t sel = gb.io[GB_P1] & 0x30, low = 15;
    if (!(sel & 0x10)) low &= ~(gb.keys & 15);
    if (!(sel & 0x20)) low &= ~(gb.keys >> 4);
    return 0xC0 | sel | low;
}
static void p1_changed(uint8_t before) {
    if (before & ~p1_read() & 15)
        gb.io[GB_IF] |= GB_INT_JOYPAD;
}
void gb_set_keys(uint8_t keys) {
    uint8_t before = p1_read();
    gb.keys = keys;
    p1_changed(before);
}

/* ---- the machine cycle ---- */

void gb_tick(void) {
    gb.clock += 4;
    if (gb.tima_state == 2) {
        gb.tima_state = 0;
    } else if (gb.tima_state == 1) {
        gb.io[GB_TIMA] = gb.io[GB_TMA];
        gb.io[GB_IF] |= GB_INT_TIMER;
        gb.tima_state = 2;
    }
    set_div(gb.div + 4);

    if (gb.dma_active == 2) gb.dma_active = 0;
    if (gb.dma_wait && --gb.dma_wait == 0) {
        gb.dma_active = 1;
        gb.dma_pos = 0;
        gb.dma_page = gb.dma_next;
    }
    if (gb.dma_active == 1) {
        uint16_t src = (uint16_t) gb.dma_page << 8 | gb.dma_pos;
        if (src >= 0xE000) src -= 0x2000;
        gb.oam[gb.dma_pos] = gb_peek(src);
        if (++gb.dma_pos == 0xA0) gb.dma_active = 2;
    }
    gb_ppu_tick();
}

/* ---- memory ---- */

// what the sound registers read as 1 whatever was written (FF10h-FF2Fh)
static const uint8_t snd_mask[0x20] = {
    0x80, 0x3F, 0x00, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF, 0x9F, 0xFF, 0xBF, 0xFF,
    0xFF, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x70, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static uint8_t io_read(uint8_t r) {
    switch (r) {
    case GB_P1: return p1_read();
    case GB_SB: return gb.io[GB_SB];
    case GB_SC: return gb.io[GB_SC] | 0x7E;
    case GB_DIV: return gb.div >> 8;
    case GB_TIMA: case GB_TMA: return gb.io[r];
    case GB_TAC: return gb.io[r] | 0xF8;
    case GB_IF: return gb.io[r] | 0xE0;
    case GB_DMA: return gb.io[r];
    default:
        if (r >= 0x10 && r < 0x30) return gb.io[r] | snd_mask[r - 0x10];
        if (r >= 0x30 && r < 0x40) return gb.io[r];
        if (r >= 0x40 && r <= 0x4B) return gb_ppu_read(r);
        return 0xFF;
    }
}

static void io_write(uint8_t r, uint8_t v) {
    uint8_t before;
    bool was;
    switch (r) {
    case GB_P1:
        before = p1_read();
        gb.io[r] = v & 0x30;
        p1_changed(before);
        break;
    case GB_SB:
        gb.io[r] = v;
        break;
    case GB_SC:
        gb.io[r] = v & 0x81;
        gb.serial_bits = (v & 0x81) == 0x81 ? 8 : 0;
#ifdef GB_HOST
        if ((v & 0x81) == 0x81 && gb_serial_hook) gb_serial_hook(gb.io[GB_SB]);
#endif
        break;
    case GB_DIV:
        set_div(0);
        break;
    case GB_TIMA:
        // (in the cycle of the reload the write is lost; in the one before,
        // it takes the reload's and the interrupt's place)
        if (gb.tima_state == 2) break;
        gb.tima_state = 0;
        gb.io[r] = v;
        break;
    case GB_TMA:
        gb.io[r] = v;
        if (gb.tima_state == 2) gb.io[GB_TIMA] = v;
        break;
    case GB_TAC:
        was = timer_signal();
        before = gb.io[r];
        gb.io[r] = v & 7;
        if (was && !timer_signal()) {
            tima_inc();
        } else if (!(before & 4) && (v & 4)) {
            // the timer turned on in the cycle its bit of DIV falls: the
            // write comes first, the fall is counted (Mooneye's rapid_toggle)
            static const uint16_t period[4] = { 1024, 16, 64, 256 };
            if (!(gb.div & (period[v & 3] - 1))) tima_inc();
        }
        break;
    case GB_IF:
        gb.io[r] = v & 0x1F;
        break;
    case GB_DMA:
        gb.io[r] = v;
        gb.dma_next = v;
        gb.dma_wait = 2;
        break;
    default:
        if (r >= 0x40 && r <= 0x4B) gb_ppu_write(r, v);
        else if (r >= 0x10 && r < 0x40) gb.io[r] = v;   // (the sound: registers only, for now)
        break;
    }
}

uint8_t gb_peek(uint16_t a) {
    switch (a >> 13) {
    case 0: case 1: return gb_rom[(uint32_t) gb.rom0_bank << 14 | a];
    case 2: case 3: return gb_rom[(uint32_t) gb.romx_bank << 14 | (a & 0x3FFF)];
    case 4: return gb_vram[a & 0x1FFF];
    case 5: return ram_read(a);
    case 6: return gb_wram[a & 0x1FFF];
    default:
        if (a < 0xFE00) return gb_wram[a & 0x1FFF];
        if (a < 0xFEA0) return gb.oam[a - 0xFE00];
        if (a < 0xFF00) return 0;
        if (a < 0xFF80) return io_read(a & 0xFF);
        if (a < 0xFFFF) return gb.hram[a - 0xFF80];
        return gb.ie;
    }
}

uint8_t gb_read(uint16_t a) {
    if (a >= 0x8000 && a < 0xA000) {
        if (gb_ppu_vram_blocked()) return 0xFF;
    } else if (a >= 0xFE00 && a < 0xFF00) {
        if (gb.dma_active || gb_ppu_oam_blocked()) return 0xFF;
    }
    return gb_peek(a);
}

void gb_write(uint16_t a, uint8_t v) {
    switch (a >> 13) {
    case 0: case 1: case 2: case 3:
        cart_write(a, v);
        break;
    case 4:
        if (!gb_ppu_vram_blocked()) gb_vram[a & 0x1FFF] = v;
        break;
    case 5:
        ram_write(a, v);
        break;
    case 6:
        gb_wram[a & 0x1FFF] = v;
        break;
    default:
        if (a < 0xFE00) {
            gb_wram[a & 0x1FFF] = v;
        } else if (a < 0xFEA0) {
            if (!gb.dma_active && !gb_ppu_oam_blocked()) gb.oam[a - 0xFE00] = v;
        } else if (a < 0xFF00) {
        } else if (a < 0xFF80) {
            io_write(a & 0xFF, v);
        } else if (a < 0xFFFF) {
            gb.hram[a - 0xFF80] = v;
        } else {
            gb.ie = v;
        }
        break;
    }
}

/* ---- reset: what the DMG's boot ROM leaves (DMG-ABC) when it jumps to 0100h ---- */

void gb_reset(void) {
    // (the cartridge's description, set by gb_load, is kept)
    uint8_t mbc = gb.mbc, battery = gb.has_battery, ram_banks = gb.ram_banks;
    uint16_t rom_banks = gb.rom_banks;
    memset(&gb, 0, sizeof(gb));
    gb.mbc = mbc;
    gb.has_battery = battery;
    gb.ram_banks = ram_banks;
    gb.rom_banks = rom_banks;
    gb.bank_lo = 1;
    map_banks();
    memset(gb_vram, 0, sizeof(gb_vram));
    memset(gb_wram, 0, sizeof(gb_wram));
    gb.a = 0x01; gb.f = 0xB0; gb.b = 0x00; gb.c = 0x13;
    gb.d = 0x00; gb.e = 0xD8; gb.h = 0x01; gb.l = 0x4D;
    gb.sp = 0xFFFE;
    gb.pc = 0x0100;
    gb.div = 0xABC8;
    gb.io[GB_P1] = 0x00;
    gb.io[GB_IF] = 0x01;
    static const uint8_t snd[0x17] = { 0x80, 0xBF, 0xF3, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF,
        0x9F, 0xFF, 0xBF, 0xFF, 0xFF, 0x00, 0x00, 0xBF, 0x77, 0xF3, 0xF1 };
    memcpy(gb.io + 0x10, snd, sizeof(snd));
    gb.io[GB_DMA] = 0xFF;
    gb_ppu_reset();
}
