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
#if defined(GGSWAN_SMS) || defined(GGSWAN_SG1000)
uint8_t gg_view_x = 16, gg_view_y = 24;
#endif
// the CPU core: the interpreter, or the recompiler's z80_run equivalent
int32_t (*gg_cpu_run)(int32_t cycles) = z80_run;
const uint8_t GGFAR *gg_slot[3];
static uint8_t GGFAR *cart_ram;   // at $8000 when enabled, else NULL
// the first KiB of the ROM (always visible at $0000: interrupt vectors)
#ifdef __ia16__
// (WonderSwan: in the free internal RAM, see ws/main.c "IRAM layout")
#define rom_first_kb ((uint8_t*) 0xEC00)
#else
static uint8_t rom_first_kb[0x400];
#endif

/* === Memory === */

#ifdef GGSWAN_SG1000
// sgswan: 8 KiB of RAM at 2000h-3FFFh (the "8kB RAM adapter" some later
// games were sold with, in place of that part of the ROM), or NULL
static uint8_t GGFAR *sg_low_ram;
#endif

static void map_slot(uint8_t slot) {
    // on some platforms slot 2 and the cartridge RAM share one window: while
    // the cartridge RAM is enabled, slot 2 is not mapped
    if (slot == 2 && (gg.mapper[0] & 0x08))
        return;
    uint16_t bank = gg.mapper[1 + slot];
#ifndef GGSWAN_SG1000   // (no mapper: a short ROM is not there again)
    if (gg.rom_banks)
        bank %= gg.rom_banks;
#endif
    gg_slot[slot] = gg_platform_bank(slot, bank);
}

static void map_cart_ram(void) {
#ifdef GGSWAN_SG1000
    /*
     * sgswan: no mapper register. The memory after a ROM of 32 KiB or
     * less, 8000h-BFFFh, is RAM: where the cartridges with their own have
     * it (The Castle, 8 KiB). The platform's bank for slot 2 is memory
     * that can be written (the ROM's 64 KiB, FFh after the ROM), and the
     * recompiler reads it as it reads the ROM.
     */
    map_slot(2);
    cart_ram = gg.rom_banks <= 2 ? (uint8_t GGFAR*) gg_slot[2] : 0;
    return;
#endif
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
    if (a < 0x0400 && !gg.codemasters) {
        // the first KiB is always bank 0 (interrupt vectors)
        return rom_first_kb[a];
    }
    if (a >= 0x8000 && cart_ram)
        return cart_ram[a & 0x3FFF];
    return gg_slot[a >> 14][a & 0x3FFF];
}

// for the translator (drc.c): the bytes z80_read gives from a on, up to
// the end of what one pointer covers (*len bytes, at least 1)
const uint8_t GGFAR *z80_read_span(uint16_t a, uint16_t *len) {
    if (a >= 0xC000) {
        *len = 0x2000 - (a & 0x1FFF);
        return gg_ram + (a & 0x1FFF);
    }
    if (a < 0x0400 && !gg.codemasters) {
        *len = 0x0400 - a;
        return rom_first_kb + a;
    }
    *len = 0x4000 - (a & 0x3FFF);
    if (a >= 0x8000 && cart_ram)
        return cart_ram + (a & 0x3FFF);
    return gg_slot[a >> 14] + (a & 0x3FFF);
}

void z80_write(uint16_t a, uint8_t v) {
    if (a >= 0xC000) {
        gg_ram[a & 0x1FFF] = v;
#ifndef GGSWAN_SG1000   // (no mapper: plain RAM, like the rest)
        if (a >= 0xFFFC && !gg.codemasters) {
            uint8_t r = a - 0xFFFC;
            gg.mapper[r] = v;
            if (r == 0)
                map_cart_ram();
            else
                map_slot(r - 1);
        }
#endif
        return;
    }
#ifdef GGSWAN_SG1000
    if (sg_low_ram && (a & 0xE000) == 0x2000) {
        sg_low_ram[a & 0x1FFF] = v;
        return;
    }
#endif
#ifdef GGSWAN_SMS
    // the Codemasters mapper: the bank of each slot written at its start
    if (gg.codemasters && !(a & 0x3FFF)) {
        gg.mapper[1 + (a >> 14)] = v;
        map_slot(a >> 14);
        return;
    }
#endif
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

// smsswan: the sprite collision flag (status bit 5), worked out by the
// platform once a frame, when the frame interrupt is requested (the
// recompiler reads the status without coming to C)
#ifdef GGSWAN_SMS
// (only for the games that look at it: the check takes some 3500 cycles a
// frame in Sonic, 8% of it. Looked for in the 8 bytes after a status
// read: AND n with bit 5, or BIT 5,A)
#define SPRITE_STATUS() (gg.coll_used ? gg_platform_sprite_status() : 0)
// (bit 5 of A tested in the 8 bytes from a: AND n with it, or BIT 5,A)
static uint8_t coll_test_at(uint16_t a) {
    for (uint8_t k = 0; k < 8; k++) {
        uint8_t b = z80_read(a + k), n = z80_read(a + k + 1);
        if ((b == 0xE6 && (n & 0x20) && n != 0xFF) || (b == 0xCB && n == 0x6F))
            return 1;
    }
    return 0;
}
void gg_status_read_at(uint16_t pc) {
    if (gg.coll_used)
        return;
    if (coll_test_at(pc)) {
        gg.coll_used = 1;
        return;
    }
    // The status kept for later (LD (nn),A in those 8 bytes): tested
    // where it is read back? LD A,(nn) followed by the test, or LD HL,nn
    // and BIT 5,(HL), anywhere in the ROM as it is mapped now. Once for
    // each address (the scan takes about half a second on the WS).
    static uint16_t scanned;
    for (uint8_t k = 0; k < 8; k++) {
        if (z80_read(pc + k) != 0x32)
            continue;
        uint8_t lo = z80_read(pc + k + 1), hi = z80_read(pc + k + 2);
        uint16_t nn = lo | (hi << 8);
        if (nn < 0xC000 || nn == scanned)
            continue;
        scanned = nn;
        for (uint16_t a = 0; a < 0xBFF0; a++) {
            uint8_t b = z80_read(a);
            if ((b != 0x3A && b != 0x21) || z80_read(a + 1) != lo || z80_read(a + 2) != hi)
                continue;
            if (b == 0x3A ? coll_test_at(a + 3)
                          : (z80_read(a + 3) == 0xCB && z80_read(a + 4) == 0x6E)) {
                gg.coll_used = 1;
                return;
            }
        }
    }
}
#elif defined(GGSWAN_SG1000)
#define SPRITE_STATUS() sg_sprite_status()
#else
#define SPRITE_STATUS() 0
#endif

#ifdef GGSWAN_SMS
static void vdp_mode_update(void);
#endif

// the start of the vertical blank: the frame interrupt flag, and what the
// sprites of the frame did
#ifdef GGSWAN_SG1000
// (the fifth sprite's number stays the first one found until the status
// is read; the overlap flag adds up)
#define VBL_STATUS() do { \
        uint8_t s_ = SPRITE_STATUS(); \
        if (gg.vdp.status & 0x40) \
            s_ &= 0x20; \
        gg.vdp.status |= 0x80 | s_; \
    } while (0)
#else
#define VBL_STATUS() (gg.vdp.status |= 0x80 | SPRITE_STATUS())
#endif

static inline void update_irq(void) {
    z80.irq = ((gg.vdp.status & 0x80) && (gg.vdp.reg[1] & 0x20))
        || (gg.vdp.line_pending && GG_LINE_IRQ);
}

static uint8_t vdp_read_data(void) {
    gg_platform_vram_sync_at(gg.vdp.addr & 0x3FFF);   // (tile writes the platform deferred)
    uint8_t v = gg.vdp.read_buffer;
    gg.vdp.read_buffer = gg_vram[gg.vdp.addr & 0x3FFF];
    gg.vdp.addr = (gg.vdp.addr + 1) & 0x3FFF;
    gg.vdp.latch_full = 0;
    return v;
}

static void vdp_write_data(uint8_t v) {
    gg.vdp.latch_full = 0;
#ifdef GGSWAN_SG1000
    // TMS9918A: no color RAM, the byte goes to VRAM whatever the code. The
    // platform hears of the bytes that change (it marks what to draw again)
    {
        uint16_t va = gg.vdp.addr & 0x3FFF;
        if (gg_vram[va] != v) {
            gg_vram[va] = v;
            gg_platform_vram_write(va, v);
        }
        gg.vdp.read_buffer = v;
        gg.vdp.addr = (va + 1) & 0x3FFF;
        return;
    }
#endif
    if (gg.vdp.code == 3) {
#ifdef GGSWAN_SMS
        // CRAM: 32 colors of one byte (--BBGGRR), no latch
        uint8_t ci = gg.vdp.addr & 0x1F;
        gg_cram[ci] = v & 0x3F;
        gg_platform_cram_write(ci);
#else
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
#endif
    } else {
        uint16_t va = gg.vdp.addr & 0x3FFF;
        // (the platform may keep the byte for later: deferred tile writes)
        if (!gg_platform_vram_defer(va, v)) {
            gg_vram[va] = v;
            gg_platform_vram_write(va, v);
        }
    }
    gg.vdp.read_buffer = v;
    gg.vdp.addr = (gg.vdp.addr + 1) & 0x3FFF;
}

static uint8_t vdp_read_status(void) {
#ifdef GGSWAN_SG1000
    // (bits 0-4: the number of the fifth sprite, with bit 6; else 1Fh)
    uint8_t v = gg.vdp.status;
    if (!(v & 0x40))
        v |= 0x1F;
#else
    uint8_t v = gg.vdp.status | 0x1F;
#endif
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
#ifdef GGSWAN_SG1000
    if (gg.vdp.code == 3)
        gg.vdp.code = 2;   // (bit 7: a register, 8 of them)
#endif
    gg.vdp.addr = ((v & 0x3F) << 8) | gg.vdp.latch;
    switch (gg.vdp.code) {
    case 0:
        // VRAM read: prefetch
        gg_platform_vram_sync_at(gg.vdp.addr);
        gg.vdp.read_buffer = gg_vram[gg.vdp.addr];
        gg.vdp.addr = (gg.vdp.addr + 1) & 0x3FFF;
        break;
    case 2: {
#ifdef GGSWAN_SG1000
        uint8_t r = v & 0x07;
        {
#else
        uint8_t r = v & 0x0F;
        if (r < 11) {
#endif
            uint8_t old = gg.vdp.reg[r];
            gg.vdp.reg[r] = gg.vdp.latch;
            gg_platform_vdp_reg(r, gg.vdp.latch);
#ifdef GGSWAN_SMS
            if (r < 2)
                vdp_mode_update();
#endif
            // display turned on before the end of the view: shown this
            // frame even if off again at the end (Sonic, slow frames: off
            // at line 0 and at the end, on in between; see video_present)
            if (r == 1 && (gg.vdp.latch & 0x40) && gg_current_line() < GG_PRESENT_LINE)
                gg.vdp.reg1_frame |= 0x40;
            update_irq();
            // interrupt settings changed: plan the timing again (the same
            // value again changes nothing)
            if ((r == 0 || r == 1 || r == 10) && old != gg.vdp.latch)
                z80_break = 1;
        }
        break;
    }
    default:
        break;
    }
}

/* === Timing ===
 *
 * The CPU does not stop at every line: gg_run_frame() runs it up to the
 * next line where something must happen at an exact time (the frame
 * presented at line 168, the frame interrupt at line 193, a line
 * interrupt, the end of the frame); the line
 * being drawn is worked out from the cycles when the game asks for it.
 * Writes to VDP registers 0, 1 and 10 (interrupt settings) make the CPU
 * return early (z80_break) so that the next stop is computed again.
 */

int32_t (*gg_cpu_left)(void);   // cycles left in the current CPU run
uint8_t gg_every_line;          // stop at every line, call gg_platform_line
#define frame_given gg.vdp.frame_given   // cycles given to the CPU in this frame
static uint16_t frame_carry;    // cycles run past the end of the last frame

// diagnostics (main.c): where the CPU was when the frame interrupt was
// requested, and its state (bit 0 IFF1, 1 halted, 2 just after EI)
uint16_t gg_diag_vbl_pc;
uint8_t gg_diag_vbl_st;
static void gg_diag_vbl(void) {
    gg_diag_vbl_pc = z80.pc;
    gg_diag_vbl_st = (z80.iff1 ? 1 : 0) | (z80.halted ? 2 : 0) | (z80.ei_pending ? 4 : 0);
}

// cycles run since the start of the frame
static uint16_t frame_pos(void) {
    int32_t left = gg_cpu_left ? gg_cpu_left() : z80.cycles;
    int32_t p = frame_given - left;
    if (p < 0) return 0;
    if (p > 0xFFFF) return 0xFFFF;
    return (uint16_t) p;
}

uint16_t gg_current_line(void) {
    uint16_t l = frame_pos() / GG_CYCLES_PER_LINE;
    return l < GG_LINES ? l : GG_LINES - 1;
}

#ifdef GGSWAN_SMS
// the 192 or 224 lines of the picture (mode 4; with M1 and M2, register 1
// bit 4 and register 0 bit 1: 224 lines, the name table at x700h with 32
// rows, frame interrupt at line 225, V counter 00-EA then E5-FF)
static void vdp_mode_update(void) {
    uint16_t a = ((gg.vdp.reg[0] & 0x02) && (gg.vdp.reg[1] & 0x10)) ? 224 : 192;
    if (a == gg.vdp.active)
        return;
    gg.vdp.active = a;
    gg.vdp.vcnt_max = a == 224 ? 0xEA : 0xDA;
    gg.vdp.present_m1 = a - 1;
    gg.vdp.present_p1 = a + 1;
    z80_break = 1;                              // (the timing planned again)
    gg_platform_vdp_reg(2, gg.vdp.reg[2]);      // (the name table moved)
    gg_platform_vdp_reg(1, gg.vdp.reg[1]);      // (the sprites)
}
#define VCNT_MAX gg.vdp.vcnt_max
#else
#define VCNT_MAX 0xDA
#endif

static uint8_t vdp_vcounter(void) {
    // NTSC 192-line mode: 0x00-0xDA, then 0xD5-0xFF
    uint16_t l = gg_current_line();
    return l <= VCNT_MAX ? l : l - 6;
}

/* === I/O ports === */

uint8_t z80_in(uint8_t port) {
#ifdef GGSWAN_SG1000
    // SG-1000: 80-BF the TMS9918A (even: data, odd: status); C0-FF the
    // joypads, 0 = pressed (even: joypad 1 in bits 0-5, joypad 2's up and
    // down in 6-7; odd: the rest of joypad 2); nothing else to read (no
    // line or column counters)
    if (port < 0x80)
        return 0xFF;
    if (port < 0xC0)
        return (port & 1) ? vdp_read_status() : vdp_read_data();
    return (port & 1) ? 0xFF : ~(gg.keys & 0x3F);
#endif
#ifdef GGSWAN_SMS
    // Master System (export): nothing to read at 00-3F; DC (even, C0-FF):
    // joypad 1 in bits 0-5, joypad 2's up and down in bits 6-7 (not
    // pressed); DD (odd): the rest of joypad 2 (not pressed), Reset (bit
    // 4, not pressed), bits 6-7 the TH lines: high as inputs, the level
    // written to port 3F as outputs (how games tell an export console)
    if (port < 0x40)
        return 0xFF;
    if (port >= 0xC0) {
        if (!(port & 1))
            return ~(gg.keys & 0x3F);
        uint8_t v = 0xFF;
        if (!(gg.io_ctrl & 0x02) && !(gg.io_ctrl & 0x20)) v &= ~0x40;
        if (!(gg.io_ctrl & 0x08) && !(gg.io_ctrl & 0x80)) v &= ~0x80;
        return v;
    }
#endif
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
#ifdef GGSWAN_SMS
    {
        if (port & 1)
            gg_status_read_at(z80.pc);
    }
#endif
        return (port & 1) ? vdp_read_status() : vdp_read_data();
    // $DC (and mirrors): joypad 1, active low
    if (!(port & 1))
        return ~(gg.keys & 0x3F);
    return 0xFF;
}

void z80_out(uint8_t port, uint8_t v) {
    if (port < 0x40) {
#ifdef GGSWAN_SMS
        // Master System: even ports are 3E (memory control: the cartridge
        // is always on here), odd ports 3F (I/O control)
        if (port & 1)
            gg.io_ctrl = v;
#elif defined(GGSWAN_SG1000)
        // (nothing there)
#else
        if (port == 0x06)
            gg_platform_stereo(v);
#endif
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
    gg.codemasters = 0;
    gg.coll_used = 0;
#ifdef GGSWAN_SMS
    if (gg.rom_banks >= 2) {
        // a Codemasters cartridge: its header at 7FE0h has a checksum
        // (7FE6h) and, at 7FE8h, 10000h minus it; slots 0, 1, 0 at reset
        const uint8_t GGFAR *b1 = gg_platform_bank(1, 1);
        uint16_t sum = b1[0x3FE6] | (b1[0x3FE7] << 8), inv = b1[0x3FE8] | (b1[0x3FE9] << 8);
        if (sum && (uint16_t) (sum + inv) == 0) {
            gg.codemasters = 1;
            gg.mapper[3] = 0;
        }
    }
#endif
    {
        const uint8_t GGFAR *b0 = gg_platform_bank(0, 0);
        for (uint16_t i = 0; i < 0x400; i++)
            rom_first_kb[i] = b0[i];
    }
    for (uint8_t s = 0; s < 3; s++)
        map_slot(s);
    map_cart_ram();
#ifdef GGSWAN_SG1000
    {
        // 2000h-3FFFh of the ROM all FFh: RAM there (TwinBee, Bomberman
        // Special and the others made for the adapter have nothing else
        // in that part; a game that has no use for it never looks). In
        // the same 64 KiB as slot 2, 6000h before it
        const uint8_t GGFAR *b0 = gg_slot[0];
        uint16_t i = 0x2000;
        while (i < 0x4000 && b0[i] == 0xFF)
            i++;
        sg_low_ram = i == 0x4000 ? (uint8_t GGFAR*) gg_slot[2] - 0x6000 : 0;
    }
#endif
    gg.frame = 0;
#ifdef GGSWAN_SMS
    vdp_mode_update();   // (192 lines)
    {
        // The registers as the console's BIOS leaves them (there is none
        // here): mode 4 with the display off, the tables at the top of
        // VRAM, no line interrupts. A game that sets only some of them
        // (California Games II and Speedball need this, by Genesis Plus
        // GX's notes, whose values these are) found them all 0: the name
        // table and the sprite table at 0000h, on top of the tiles.
        static const uint8_t bios[11] = {
            0x36, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF
        };
        for (uint8_t r = 0; r < 11; r++) {
            gg.vdp.reg[r] = bios[r];
            gg_platform_vdp_reg(r, bios[r]);
        }
        vdp_mode_update();
    }
#endif
    gg.io_ctrl = 0xFF;   // (all inputs)
    z80_reset();
}

static void vdp_line(uint16_t line) {
    gg.vdp.vcounter = line;
    if (line == 0) {
        gg.vdp.vscroll_frame = gg.vdp.reg[9];
        gg.vdp.reg1_frame = gg.vdp.reg[1];
    }

    // line counter: counts down on lines 0-192, reloaded on the others
    if (line <= GG_ACTIVE) {
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
    if (line == GG_VBL_LINE) {
        VBL_STATUS();
        gg_diag_vbl();
    }

    update_irq();
    if (line == GG_PRESENT_LINE)
        gg_platform_frame_end();
}

// start lines from .. to - 1 at once (the same as vdp_line on each, in
// order): one pass instead of one call per line
static void vdp_lines(uint16_t from, uint16_t to) {
    if (from >= to) return;
    if (from == 0) {
        gg.vdp.vscroll_frame = gg.vdp.reg[9];
        gg.vdp.reg1_frame = gg.vdp.reg[1];
    }
    uint8_t c = gg.vdp.line_counter, r = gg.vdp.reg[10];
    // lines 0-192: the counter counts down, reloading (and requesting an
    // interrupt) on the line after it reached 0
    uint16_t a_end = to < GG_ACTIVE + 1 ? to : GG_ACTIVE + 1;
    if (from < a_end) {
        uint16_t k = a_end - from;
        if (k > c) {
            gg.vdp.line_pending = 1;
            k -= c + 1;   // lines after the reload
            c = r - (uint8_t) (k % ((uint16_t) r + 1));
        } else {
            c -= (uint8_t) k;
        }
    }
    // the other lines reload it
    if (to > GG_ACTIVE + 1)
        c = r;
    gg.vdp.line_counter = c;
    // frame interrupt
    uint8_t end = from <= GG_VBL_LINE && to > GG_VBL_LINE;
    if (end) {
        VBL_STATUS();
        gg_diag_vbl();
    }
    gg.vdp.vcounter = to - 1;
    update_irq();
    if (from <= GG_PRESENT_LINE && to > GG_PRESENT_LINE)
        gg_platform_frame_end();
}

// the next line (from l, the first one not started yet) whose start must
// happen at its exact time
static uint16_t next_event(uint16_t l) {
    if (gg_every_line)
        return l;
    uint16_t e = GG_LINES;
    if (l <= GG_PRESENT_LINE)
        e = GG_PRESENT_LINE;       // the frame presented
    else if (l <= GG_VBL_LINE)
        e = GG_VBL_LINE;   // frame interrupt
    if (GG_LINE_IRQ && l <= GG_ACTIVE) {
        // the line counter reaches zero at the start of line l + counter
        uint16_t u = l + gg.vdp.line_counter;
        if (u < e)
            e = u;
    }
    return e;
}

#define started gg.vdp.started   // lines of this frame started
#ifndef GG_IRQ_LATE
#define GG_IRQ_LATE 1
#endif

// (line_pos = started * 228 always: the recompiler's line routine)
static inline void set_started(uint16_t s) {
    started = s;
    gg.vdp.line_pos = s * (uint16_t) GG_CYCLES_PER_LINE;
}

// One step of the frame loop below without the per-line mode, at frame
// position pos: start the lines the CPU has reached, then the cycles to run
// up to the next event (or -1: the end of the frame, left to gg_run_frame)
static int32_t event_step(uint16_t pos) {
    uint16_t reached = pos / GG_CYCLES_PER_LINE + 1;
    if (reached > GG_LINES) reached = GG_LINES;
    /*
     * A line interrupt taken late (the recompiler stops only
     * between blocks: a long one, an LDIR or OUTI run in one go, ends lines
     * after the event it was given the cycles for). With an interrupt every
     * line (Super Monaco GP II's road, one scroll value a line, register 10
     * = 0 from line 120 to 160) the interrupts of the lines run over were
     * lost (one request for all of them), so the game's handler counted
     * fewer lines than the picture has and set the status bar's scroll up
     * to 7 lines late, or not at all in that frame: the bar's digits torn.
     * And an interrupt part of a line late put the handler's scroll write
     * on the line after its own. The frame position goes back to the start
     * of the interrupt's line instead (the cycles run over are not
     * counted, here and in gg_fast_line and the recompiler's LINE routine):
     * the handler runs at its line, and again for each of the next ones.
     * Those cycles are taken back later in the frame (irq_debt, below):
     * the CPU still gets a frame's worth. Only when the CPU would have taken
     * it (interrupts on, none waiting: a game that keeps them off loses
     * them on the Game Gear too).
     */
    if (GG_LINE_IRQ && z80.iff1 && !gg.vdp.line_pending
            && started <= GG_ACTIVE) {
        uint16_t l1 = started + gg.vdp.line_counter;   // the line that requests it
        uint16_t at = l1 * (uint16_t) GG_CYCLES_PER_LINE;
        if (l1 <= GG_ACTIVE && pos >= at && pos - at >= GG_IRQ_LATE) {
            frame_given -= pos - at;
            gg.vdp.irq_debt += pos - at;
            pos = at;
            reached = l1 + 1;
        }
    }
    vdp_lines(started, reached);
    if (reached > started) set_started(reached);
    // the cycles not counted above are taken back below the view, where
    // nothing is shown (after the frame was presented, up to line 192: the
    // frame interrupt stays on time); what is left, at the end of the frame
    if (gg.vdp.irq_debt && started > GG_PRESENT_LINE && started <= GG_ACTIVE) {
        uint16_t lim = GG_ACTIVE * (uint16_t) GG_CYCLES_PER_LINE;
        if (pos < lim) {
            uint16_t t = lim - pos;
            if (t > gg.vdp.irq_debt)
                t = gg.vdp.irq_debt;
            gg.vdp.irq_debt -= t;
            frame_given += t;
            pos += t;
            reached = pos / GG_CYCLES_PER_LINE + 1;
            vdp_lines(started, reached);
            if (reached > started) set_started(reached);
        }
    }
    if (started >= GG_LINES && pos >= GG_FRAME_CYCLES)
        return -1;
    uint16_t e = next_event(started);
    // (cycles already given but not run yet, after an early stop, count)
    int32_t want = (int32_t) e * GG_CYCLES_PER_LINE - frame_given;
    if (want < 0)
        want = 0;
    frame_given += want;
    return want;
}

int32_t gg_line_event(int32_t left) {
    // (frame_pos with the cycles left given)
    int32_t p = frame_given - left;
    uint16_t pos = p < 0 ? 0 : p > 0xFFFF ? 0xFFFF : (uint16_t) p;
    // only the lines of the view (the frame presented at line 168, the
    // frame interrupt and the end of the frame stay in gg_run_frame)
    if (gg_every_line || pos >= (uint16_t) (GG_PRESENT_LINE * (unsigned) GG_CYCLES_PER_LINE))
        return -1;
    return event_step(pos);
}

int16_t gg_fast_line(int16_t left) {
    // the event waited for is the start of line e = started + line_counter,
    // reached with frame_given = e * 228 exactly (left: 0 down to -227),
    // where the line counter reloads and requests the interrupt; the next
    // one is r + 1 lines later (r: register 10), still inside the picture
    // (and r below 128, so that the cycles fit the 16-bit run budget).
    // The lines before e are started here too (nothing happens on them):
    // a wait loop ends its run early, lines before the interrupt's
    // (Sonic Spinball, one every 8 lines: each went back to the
    // dispatcher and event_step, some 700 cycles)
    uint16_t e = started + gg.vdp.line_counter;
    uint8_t r = gg.vdp.reg[10];
    if (gg_every_line || left > 0 || left <= -GG_CYCLES_PER_LINE
        || started == 0 || r >= 128 || e + r + 1 >= GG_PRESENT_LINE - 1
        || !GG_LINE_IRQ
        || frame_given != (int32_t) (uint16_t) (e * (unsigned) GG_CYCLES_PER_LINE))
        return 0;
    // vdp_lines(started, e + 1) for this case, then next_event(e + 1) = e + 1 + r
    gg.vdp.line_pending = 1;
    gg.vdp.line_counter = r;
    gg.vdp.vcounter = e;
    update_irq();
    set_started(e + 1);
    int16_t want = (int16_t) ((r + 1u) * GG_CYCLES_PER_LINE);   // (r < 128: fits)
    frame_given += want;
    // (the cycles run past the start of the line, -left, are not counted:
    // the interrupt is taken at the start of its line; see event_step)
    gg.vdp.irq_debt -= left;
    return want - left;
}

void gg_run_frame(void) {
#if defined(GGSWAN_SMS) || defined(GGSWAN_SG1000)
    // the Pause button (GG_KEY_START in gg.keys): a non-maskable interrupt
    // when it is pressed, here between two frames (the CPU state is all
    // in z80, also with the recompiler: nothing to stop inside a block)
    {
        static uint8_t pause_was;
        uint8_t p = gg.keys & GG_KEY_START;
        if (p && !pause_was)
            z80_nmi();
        pause_was = p;
    }
#endif
    set_started(0);
    // (left = z80.cycles, so the frame starts frame_carry cycles in)
    frame_given = (int32_t) frame_carry + (gg_cpu_left ? gg_cpu_left() : z80.cycles);
    if (gg_every_line) {
        while (1) {
            uint16_t pos = frame_pos();
            // start the lines the CPU has reached
            while (started < GG_LINES && (uint16_t) (started * GG_CYCLES_PER_LINE) <= pos) {
                vdp_line(started);
                gg_platform_line(started);
                set_started(started + 1);
            }
            if (started >= GG_LINES && pos >= GG_FRAME_CYCLES)
                break;
            uint16_t e = next_event(started);
            int32_t want = (int32_t) e * GG_CYCLES_PER_LINE - frame_given;
            if (want < 0)
                want = 0;
            frame_given += want;
            z80_break = 0;
            gg_cpu_run(want);
        }
    } else {
        int32_t want;
        while ((want = event_step(frame_pos())) >= 0) {
            z80_break = 0;
            gg_cpu_run(want);
        }
    }
    frame_carry = frame_pos() - GG_FRAME_CYCLES;
    {
        // (at most 100 lines at once; the rest is forgiven)
        uint16_t d = gg.vdp.irq_debt;
        gg.vdp.irq_debt = 0;
        frame_carry += d > 100 * GG_CYCLES_PER_LINE ? 100 * GG_CYCLES_PER_LINE : d;
    }
    gg.frame++;
}
