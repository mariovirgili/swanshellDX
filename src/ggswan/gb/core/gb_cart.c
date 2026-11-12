/**
 * gbswan - the Game Boy's cartridges: the header, and the registers of no
 * mapper, MBC1, MBC2, MBC3 (its clock as Peanut-GB's, MIT, by M. Koshkouei, has it: gb_rtc_*) and MBC5, which say what banks
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
    gb.has_rtc = type == 0x0F || type == 0x10;
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
        case 2: gb.bank_hi = v & 3; gb.rtc_sel = gb.has_rtc && v >= 8 && v <= 12 ? v : 0; break;
        default:    // the clock's copy is taken when 0 then 1 is written
            if ((v & 1) && !gb.rtc_latch) memcpy(gb.rtc_lat, gb.rtc, 5);
            gb.rtc_latch = v & 1;
            break;
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


/* MBC3's clock. It counts the Game Boy's own time, a frame at a time, and
   only while the game runs: nothing keeps it between one run and the next
   (the program finds the time it left in its RAM and a clock at zero).
   The counters are the chip's: 6, 6 and 5 bits that wrap at 60, 60 and 24
   only when they get there by counting (a 61 written counts to 63 and
   then to 0 with no carry). */
void gb_rtc_frame(void) {
    if (!gb.has_rtc || (gb.rtc[4] & 0x40)) return;
    gb.rtc_sub += 17556;
    if (gb.rtc_sub < 1048576L) return;
    gb.rtc_sub -= 1048576L;
    gb.rtc[0] = (gb.rtc[0] + 1) & 0x3F;
    if (gb.rtc[0] != 60) return;
    gb.rtc[0] = 0;
    gb.rtc[1] = (gb.rtc[1] + 1) & 0x3F;
    if (gb.rtc[1] != 60) return;
    gb.rtc[1] = 0;
    gb.rtc[2] = (gb.rtc[2] + 1) & 0x1F;
    if (gb.rtc[2] != 24) return;
    gb.rtc[2] = 0;
    if (++gb.rtc[3]) return;
    if (gb.rtc[4] & 1) gb.rtc[4] |= 0x80;
    gb.rtc[4] ^= 1;
}

uint8_t gb_rtc_read(void) {
    return gb.rtc_lat[gb.rtc_sel - 8];
}

void gb_rtc_write(uint8_t v) {
    static const uint8_t GB_FAR masks[5] = { 0x3F, 0x3F, 0x1F, 0xFF, 0xC1 };
    uint8_t r = gb.rtc_sel - 8;
    gb.rtc[r] = v & masks[r];
    if (!r) gb.rtc_sub = 0;     // (the seconds written: the second starts again)
}
