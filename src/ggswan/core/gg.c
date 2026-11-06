/**
 * ggswan - Game Gear emulator for the WonderSwan (nileswan)
 * The Game Gear machine.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>
#include "gg.h"
#include "z80.h"

gg_t gg;
uint8_t gg_cram[64];
const uint8_t GGFAR *gg_slot[3];
static uint8_t GGFAR *cart_ram;   // at $8000 when enabled, else NULL
// the first KiB of the ROM (always visible at $0000: interrupt vectors)
static uint8_t rom_first_kb[0x400];

/* === Memory === */

static void map_slot(uint8_t slot) {
    // on some platforms slot 2 and the cartridge RAM share one window: while
    // the cartridge RAM is enabled, slot 2 is not mapped
    if (slot == 2 && (gg.mapper[0] & 0x08))
        return;
    uint16_t bank = gg.mapper[1 + slot];
    if (gg.rom_banks)
        bank %= gg.rom_banks;
    gg_slot[slot] = gg_platform_bank(slot, bank);
}

static void map_cart_ram(void) {
    // $FFFC bit 3: cartridge RAM at $8000 (bit 2: which 16 KiB page)
    if (gg.mapper[0] & 0x08) {
        cart_ram = gg_platform_cart_ram((gg.mapper[0] >> 2) & 1);
    } else {
        cart_ram = 0;
        map_slot(2);
    }
}

uint8_t z80_read(uint16_t a) {
    if (a >= 0xC000)
        return gg_ram[a & 0x1FFF];
    if (a < 0x0400) {
        // the first KiB is always bank 0 (interrupt vectors)
        return rom_first_kb[a];
    }
    if (a >= 0x8000 && cart_ram)
        return cart_ram[a & 0x3FFF];
    return gg_slot[a >> 14][a & 0x3FFF];
}

void z80_write(uint16_t a, uint8_t v) {
    if (a >= 0xC000) {
        gg_ram[a & 0x1FFF] = v;
        if (a >= 0xFFFC) {
            uint8_t r = a - 0xFFFC;
            gg.mapper[r] = v;
            if (r == 0)
                map_cart_ram();
            else
                map_slot(r - 1);
        }
        return;
    }
    if (a >= 0x8000 && cart_ram)
        cart_ram[a & 0x3FFF] = v;
    // writes to ROM are ignored
}

/// Recompute the slot pointers from the mapper registers (after a
/// restore, or when the platform needs the mapping again).
void gg_remap(void) {
    for (uint8_t s = 0; s < 3; s++)
        map_slot(s);
    map_cart_ram();
}

/* === VDP === */

static inline void update_irq(void) {
    z80.irq = ((gg.vdp.status & 0x80) && (gg.vdp.reg[1] & 0x20))
        || (gg.vdp.line_pending && (gg.vdp.reg[0] & 0x10));
}

static uint8_t vdp_read_data(void) {
    uint8_t v = gg.vdp.read_buffer;
    gg.vdp.read_buffer = gg_vram[gg.vdp.addr & 0x3FFF];
    gg.vdp.addr = (gg.vdp.addr + 1) & 0x3FFF;
    gg.vdp.latch_full = 0;
    return v;
}

static void vdp_write_data(uint8_t v) {
    gg.vdp.latch_full = 0;
    if (gg.vdp.code == 3) {
        // CRAM: 32 colors of 2 bytes; the even byte is latched, the odd
        // byte writes both
        uint8_t ca = gg.vdp.addr & 0x3F;
        if (ca & 1) {
            gg_cram[ca & 0x3E] = gg.vdp.cram_latch;
            gg_cram[ca] = v & 0x0F;
            gg_platform_cram_write(ca >> 1);
        } else {
            gg.vdp.cram_latch = v;
        }
    } else {
        uint16_t va = gg.vdp.addr & 0x3FFF;
        gg_vram[va] = v;
        gg_platform_vram_write(va, v);
    }
    gg.vdp.read_buffer = v;
    gg.vdp.addr = (gg.vdp.addr + 1) & 0x3FFF;
}

static uint8_t vdp_read_status(void) {
    uint8_t v = gg.vdp.status | 0x1F;
    gg.vdp.status = 0;
    gg.vdp.line_pending = 0;
    gg.vdp.latch_full = 0;
    update_irq();
    return v;
}

static void vdp_write_control(uint8_t v) {
    if (!gg.vdp.latch_full) {
        gg.vdp.latch = v;
        gg.vdp.addr = (gg.vdp.addr & 0x3F00) | v;
        gg.vdp.latch_full = 1;
        return;
    }
    gg.vdp.latch_full = 0;
    gg.vdp.code = v >> 6;
    gg.vdp.addr = ((v & 0x3F) << 8) | gg.vdp.latch;
    switch (gg.vdp.code) {
    case 0:
        // VRAM read: prefetch
        gg.vdp.read_buffer = gg_vram[gg.vdp.addr];
        gg.vdp.addr = (gg.vdp.addr + 1) & 0x3FFF;
        break;
    case 2: {
        uint8_t r = v & 0x0F;
        if (r < 11) {
            gg.vdp.reg[r] = gg.vdp.latch;
            gg_platform_vdp_reg(r, gg.vdp.latch);
            update_irq();
        }
        break;
    }
    default:
        break;
    }
}

static uint8_t vdp_vcounter(void) {
    // NTSC 192-line mode: 0x00-0xDA, then 0xD5-0xFF
    uint16_t l = gg.vdp.vcounter;
    return l <= 0xDA ? l : l - 6;
}

/* === I/O ports === */

uint8_t z80_in(uint8_t port) {
    if (port < 0x40) {
        switch (port) {
        case 0x00:
            // bit 7: Start (0 = pressed), bit 6: 1 = export (not Japan)
            return ((gg.keys & GG_KEY_START) ? 0x00 : 0x80) | 0x40;
        case 0x01: return 0x7F;
        case 0x02: return 0xFF;
        case 0x03: return 0x00;
        case 0x04: return 0xFF;
        case 0x05: return 0x00;
        case 0x06: return 0xFF;
        default: return 0xFF;
        }
    }
    if (port < 0x80)
        return (port & 1) ? 0x80 /* H counter: approximation */ : vdp_vcounter();
    if (port < 0xC0)
        return (port & 1) ? vdp_read_status() : vdp_read_data();
    // $DC (and mirrors): joypad 1, active low
    if (!(port & 1))
        return ~(gg.keys & 0x3F);
    return 0xFF;
}

void z80_out(uint8_t port, uint8_t v) {
    if (port < 0x40) {
        if (port == 0x06)
            gg_platform_stereo(v);
        return;
    }
    if (port < 0x80) {
        gg_platform_psg(v);
        return;
    }
    if (port < 0xC0) {
        if (port & 1)
            vdp_write_control(v);
        else
            vdp_write_data(v);
    }
}

/* === Machine === */

void gg_init(uint16_t rom_banks) {
    gg.rom_banks = rom_banks;
    z80_init();
    gg_reset();
}

void gg_reset(void) {
    memset(&gg.vdp, 0, sizeof(gg.vdp));
    memset(gg_cram, 0, sizeof(gg_cram));
    gg.mapper[0] = 0;
    gg.mapper[1] = 0;
    gg.mapper[2] = 1;
    gg.mapper[3] = 2;
    {
        const uint8_t GGFAR *b0 = gg_platform_bank(0, 0);
        for (uint16_t i = 0; i < sizeof(rom_first_kb); i++)
            rom_first_kb[i] = b0[i];
    }
    for (uint8_t s = 0; s < 3; s++)
        map_slot(s);
    map_cart_ram();
    gg.frame = 0;
    z80_reset();
}

static void vdp_line(uint16_t line) {
    gg.vdp.vcounter = line;
    if (line == 0)
        gg.vdp.vscroll_frame = gg.vdp.reg[9];

    // line counter: counts down on lines 0-192, reloaded on the others
    if (line <= GG_ACTIVE_LINES) {
        if (gg.vdp.line_counter == 0) {
            gg.vdp.line_counter = gg.vdp.reg[10];
            gg.vdp.line_pending = 1;
        } else {
            gg.vdp.line_counter--;
        }
    } else {
        gg.vdp.line_counter = gg.vdp.reg[10];
    }

    // frame interrupt
    if (line == GG_ACTIVE_LINES + 1)
        gg.vdp.status |= 0x80;

    update_irq();
}

void gg_run_frame(void) {
    for (uint16_t line = 0; line < GG_LINES; line++) {
        vdp_line(line);
        gg_platform_line(line);
        z80_run(GG_CYCLES_PER_LINE);
    }
    gg.frame++;
}
