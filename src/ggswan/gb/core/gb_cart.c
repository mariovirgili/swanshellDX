/**
 * gbswan - the Game Boy's cartridges: the header, and the registers of no
 * mapper, MBC1, MBC2, MBC3 (no clock yet) and MBC5, which say what banks
 * 0000h, 4000h and A000h show. Shared by the reference and by the plugin.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include "gb.h"

void gb_cart_map(void) {
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

// The cartridge's description from its header (rom: its first 150h bytes
// at least): 0, or 1 a size the header does not say or more than the file
// has, 2 a cartridge type not done, 3 for the Game Boy Color only
uint8_t gb_cart_header(const uint8_t GB_FAR *rom, uint32_t size) {
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
    static const uint8_t GB_FAR ram_sizes[6] = { 0, 1, 1, 4, 16, 8 };
    if (rom[0x149] > 5) return 1;
    memset(&gb, 0, sizeof(gb));
    gb.mbc = mbc;
    gb.has_battery = battery;
    gb.rom_banks = banks;
    gb.ram_banks = mbc == 2 ? 0 : ram_sizes[rom[0x149]];
    if (!gb.ram_banks && mbc != 2) gb.has_battery = 0;
    return 0;
}

uint32_t gb_ram_size(void) {
    if (!gb.has_battery) return 0;
    return gb.mbc == 2 ? 0x200 : (uint32_t) gb.ram_banks * 0x2000;
}

void gb_cart_write(uint16_t a, uint8_t v) {
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
    gb_cart_map();
}

