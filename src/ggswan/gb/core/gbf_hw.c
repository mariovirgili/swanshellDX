/**
 * gbswan - the plugin's machine: what is around the CPU, by events.
 *
 * Time is counted in machine cycles (4 clock ticks) from the start of the
 * LCD's frame: a frame is 17556 of them, a line 114. The CPU takes cycles
 * off gbf_cyc; when that reaches zero an event is due (the vertical blank,
 * the frame's end, a line or a mode the STAT interrupt is asked at, the
 * timer's overflow, the end of a serial transfer). LY, STAT, DIV and TIMA
 * are worked out from the time when they are read.
 *
 * Next to the reference (gb_bus.c, gb_ppu.c): the same registers and
 * rules, to the cycle at the events but not inside an instruction (a
 * register is read or written as if at the instruction's last cycle... or
 * a cycle or two before); the OAM DMA is done at once; the video memory
 * and the OAM are never refused; the timer's faults on writes to DIV and
 * TAC are not there.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include "gb.h"
#include "gbv.h"

#define LINE_MC 114
#define FRAME_MC 17556
#define VBL_MC (144 * LINE_MC + 1)

#ifdef GB_WS
gb_t gb;
#endif
int16_t gbf_cyc;
uint16_t gbf_ev_pos;            // the frame's cycle the next event is at
#define ev_pos gbf_ev_pos
static uint16_t div_off;        // DIV's 16 bits / 4 = div_off + the frame's cycle
static uint16_t tim_last;       // (DIV / 4) the timer was brought up to
static uint16_t ser_end;        // (DIV / 4) the serial transfer ends at
static uint8_t ser_on;
static uint8_t vbl_done;        // this frame's vertical blank was begun

#define POS() ((uint16_t) (ev_pos - gbf_cyc))
#define DIV_MC() ((uint16_t) (div_off + POS()))

/* ---- the LCD's place in the frame ---- */

uint8_t gbf_line(void) {
    return POS() / LINE_MC;
}
uint8_t gbf_draw_line(void) {
    // (a line is drawn from its 21st cycle: what changes later is for the next)
    if (!gb.lcd_on) return 0;
    uint16_t pos = POS();
    uint8_t line = pos / LINE_MC;
    if (line >= 144) return 0;      // (the vertical blank: the next frame's first)
    return pos % LINE_MC < 21 ? line : line + 1;
}

static uint8_t ppu_mode(uint8_t line, uint8_t d) {
    if (line >= 144) return line == 144 && d == 0 ? 0 : 1;
    if (d < 1) return 0;
    if (d < 21) return 2;
    if (d < 64) return 3;
    return 0;
}
static uint8_t lyc_match(uint8_t line, uint8_t d) {
    uint8_t cmp;
    if (line == 153) cmp = d == 0 ? 0xFF : d == 1 ? 153 : d == 2 ? 0xFF : 0;
    else cmp = d == 0 && line != 0 ? 0xFF : line;
    return cmp == gb_io[GB_LYC];
}

// the STAT interrupt's signal at a cycle of the frame (the LCD on)
static uint8_t stat_sig(uint16_t pos) {
    uint8_t stat = gb_io[GB_STAT];
    uint8_t line = pos / LINE_MC, d = pos % LINE_MC;
    uint8_t mode = ppu_mode(line, d);
    // (mode 2's is asked at the line's first cycle, and at the vertical
    // blank's; mode 0's a cycle before the mode shows)
    uint8_t m2 = mode == 2 || (line < 144 && d == 0) || (line == 144 && d == 1);
    uint8_t m0 = mode == 0 || (line < 144 && d == 63);
    return ((stat & 0x40) && lyc_match(line, d)) || ((stat & 0x08) && m0)
        || ((stat & 0x10) && mode == 1) || ((stat & 0x20) && m2);
}
static uint8_t stat_now(void) {
    if (!gb.lcd_on) return (gb_io[GB_STAT] & 0x40) && gb.lyc_match;
    return stat_sig(POS());
}
// the signal was `before`; an interrupt if it is up now
static void stat_rise(uint8_t before) {
    if (!before && stat_now()) gb_io[GB_IF] |= GB_INT_STAT;
}

/* ---- the timer and the serial port ---- */

static const uint8_t GB_FAR tim_shift[4] = { 8, 2, 4, 6 };     // TAC's rates, in cycles: 256, 4, 16, 64

// TIMA brought up to now
static void timer_sync(void) {
    uint16_t now = DIV_MC();
    if (gb_io[GB_TAC] & 4) {
        uint8_t sh = tim_shift[gb_io[GB_TAC] & 3];
        uint16_t n = ((now >> sh) - (tim_last >> sh)) & (0xFFFF >> sh);
        if (n) {
            uint16_t t = gb_io[GB_TIMA] + n;
            while (t > 0xFF) {
                t = t - 0x100 + gb_io[GB_TMA];
                gb_io[GB_IF] |= GB_INT_TIMER;
            }
            gb_io[GB_TIMA] = t;
        }
    }
    tim_last = now;
}

static void serial_sync(void) {
    if (ser_on && (int16_t) (DIV_MC() - ser_end) >= 0) {
        ser_on = 0;
        gb_io[GB_SB] = 0xFF;
        gb_io[GB_SC] &= 0x7F;
        gb_io[GB_IF] |= GB_INT_SERIAL;
    }
}

/* ---- events ---- */

static uint16_t stat_done;      // the frame's cycle up to which the STAT interrupt's signal has been looked at

// the next event after now
static void schedule(void) {
    uint16_t pos = POS();
    uint16_t next = FRAME_MC;
    if (gb.lcd_on) {
        if (!vbl_done) next = VBL_MC;
        // the cycles the STAT interrupt's signal can rise at, by what is
        // enabled: where the drawing ends (mode 0), a line's start (mode
        // 2, and mode 0 after the vertical blank), the vertical blank's
        // second cycle (modes 1 and 2), LYC's line
        uint8_t stat = gb_io[GB_STAT];
        if (stat & 0x78) {
            // (from the last cycle the signal was looked at, not from now:
            // the recompiled code runs a block to its end, past an event's
            // cycle, and a rise between that cycle and now would not be
            // found: an event set for the past is done at once. Super
            // Mario Land's LYC interrupt was lost that way on the frames
            // its timer's interrupt came a few cycles before it.)
            uint16_t from = stat_done < pos ? stat_done : pos;
            uint8_t line = from / LINE_MC, d = from - (uint16_t) line * LINE_MC;
            uint16_t start = (uint16_t) line * LINE_MC, t;
            if (stat & 0x08) {
                t = line < 144 ? (d < 63 ? start + 63 : line < 143 ? start + LINE_MC + 63 : FRAME_MC) : FRAME_MC;
                if (t < next) next = t;
            }
            if (stat & 0x20) {
                t = line < 143 ? start + LINE_MC : line < 144 ? VBL_MC : (from < VBL_MC ? VBL_MC : FRAME_MC);
                if (t < next) next = t;
            }
            if ((stat & 0x30) && from < VBL_MC && VBL_MC < next) next = VBL_MC;
            if (stat & 0x40) {
                uint8_t lyc = gb_io[GB_LYC];
                if (lyc < 154) {
                    t = lyc ? (uint16_t) lyc * LINE_MC + 1 : 153 * LINE_MC + 3;
                    if (t > from && t < next) next = t;
                }
            }
            if (next <= from) next = from + 1;
        }
    }
    if (next <= pos) {
        // (already passed: now)
        ev_pos = next;
        gbf_cyc = (int16_t) (next - pos);
        return;
    }
    uint16_t left = next - pos;
    if (gb_io[GB_TAC] & 4) {
        uint8_t sh = tim_shift[gb_io[GB_TAC] & 3];
        uint16_t period = 1 << sh;
        uint16_t now = div_off + pos;
        uint16_t first = period - (now & (period - 1));
        uint16_t more = (uint16_t) (0xFF - gb_io[GB_TIMA]) << sh;   // (the edges after the first)
        // (tim_last may be behind now: timer_sync is run before this)
        if (more < left && first + more < left) left = first + more;
    }
    if (ser_on) {
        uint16_t t = ser_end - (div_off + pos);
        if ((int16_t) t <= 0) t = 1;
        if (t < left) left = t;
    }
    if (left == 0) left = 1;
    ev_pos = pos + left;
    gbf_cyc = left;
}

// the hardware brought up to now, and the next event
static void sync(void) {
    timer_sync();
    serial_sync();
    schedule();
}

static void event(void) {
    timer_sync();
    serial_sync();
    uint16_t pos = POS();
    // the STAT interrupt: did its signal rise at the cycle this event was
    // set for? (the CPU may be a cycle or two past it)
    if (gb.lcd_on && (gb_io[GB_STAT] & 0x78) && ev_pos && ev_pos < FRAME_MC
        && stat_sig(ev_pos) && !stat_sig(ev_pos - 1))
        gb_io[GB_IF] |= GB_INT_STAT;
    if (gb.lcd_on && !vbl_done && pos >= VBL_MC) {
        vbl_done = 1;
        gb_io[GB_IF] |= GB_INT_VBLANK;
        gbv_frame();
        gb.frame_done = 1;
        gb.frames++;
        gb_rtc_frame();
    }
    if (pos >= FRAME_MC) {
        ev_pos -= FRAME_MC;
        div_off += FRAME_MC;
        vbl_done = 0;
        // (line 0's first cycle: mode 0 after the vertical blank's mode 1)
        if (gb.lcd_on && (gb_io[GB_STAT] & 0x28) && !(gb_io[GB_STAT] & 0x10)) gb_io[GB_IF] |= GB_INT_STAT;
        if (!gb.lcd_on) {
            gbv_frame();
            gb.frame_done = 1;
            gb.frames++;
            gb_rtc_frame();
        }
    }
    stat_done = ev_pos;
    schedule();
}

// (for what steps the CPU itself: the PC's tools, the recompiler)
void gbf_host_event(void) {
    if (gbf_cyc > 0) gbf_cyc = 0;
    event();
}

void gbf_run_frame(void) {
    gb.frame_done = 0;
    while (!gb.frame_done) {
        while (gbf_cyc > 0) {
            if (gb.halted && !(gb_ie & gb_io[GB_IF] & 0x1F)) {
                gbf_cyc = 0;        // nothing to do until the next event
                break;
            }
            gb_step();
        }
        event();
    }
}

/* ---- the keys ---- */

// (gb_io[GB_P1] is kept as it reads: the selection written and the keys)
static void p1_set(uint8_t sel) {
    uint8_t before = gb_io[GB_P1], low = 15;
    sel &= 0x30;
    if (!(sel & 0x10)) low &= ~(gb.keys & 15);
    if (!(sel & 0x20)) low &= ~(gb.keys >> 4);
    gb_io[GB_P1] = 0xC0 | sel | low;
    if (before & ~gb_io[GB_P1] & 15) gb_io[GB_IF] |= GB_INT_JOYPAD;
}
void gb_set_keys(uint8_t keys) {
    gb.keys = keys;
    p1_set(gb_io[GB_P1]);
}

/* ---- memory ---- */

static const uint8_t GB_FAR snd_mask[0x20] = {
    0x80, 0x3F, 0x00, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF, 0x9F, 0xFF, 0xBF, 0xFF,
    0xFF, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x70, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

uint8_t gbf_io_read(uint8_t r) {
    switch (r) {
    case GB_P1: return gb_io[GB_P1];
    case GB_SB: return gb_io[GB_SB];
    case GB_SC: serial_sync(); return gb_io[GB_SC] | 0x7E;
    case GB_DIV: return DIV_MC() >> 6;
    case GB_TIMA: timer_sync(); return gb_io[r];
    case GB_TMA: return gb_io[r];
    case GB_TAC: return gb_io[r] | 0xF8;
    case GB_IF: return gb_io[r] | 0xE0;
    case GB_STAT: {
        if (!gb.lcd_on) return 0x80 | (gb_io[r] & 0x78) | (gb.lyc_match ? 4 : 0);
        uint16_t pos = POS();
        uint8_t line = pos / LINE_MC, d = pos % LINE_MC;
        return 0x80 | (gb_io[r] & 0x78) | (lyc_match(line, d) ? 4 : 0) | ppu_mode(line, d);
    }
    case GB_LY: {
        if (!gb.lcd_on) return 0;
        uint16_t pos = POS();
        return pos >= 153 * LINE_MC + 1 ? 0 : pos / LINE_MC;
    }
    default:
        if (r >= 0x10 && r < 0x30) return gb_io[r] | snd_mask[r - 0x10];
        if (r >= 0x30 && r < 0x40) return gb_io[r];
        if (r >= 0x40 && r <= 0x4B) return gb_io[r];
        return 0xFF;
    }
}

uint8_t gbf_trig;       // bit n: channel n + 1 was started (NRx4 written with bit 7) since the sound last looked

void gbf_io_write(uint8_t r, uint8_t v) {
    switch (r) {
    case GB_P1:
        p1_set(v);
        break;
    case GB_SB:
        gb_io[r] = v;
        break;
    case GB_SC:
        gb_io[r] = v & 0x81;
        ser_on = (v & 0x81) == 0x81;
#ifdef GB_HOST
        {
            extern void (*gb_serial_hook)(uint8_t);
            if (ser_on && gb_serial_hook) gb_serial_hook(gb_io[GB_SB]);
        }
#endif
        if (ser_on) {
            // (8 bits, one every 128 cycles of DIV's clock)
            ser_end = (DIV_MC() & ~127) + 8 * 128;
            sync();
        }
        break;
    case GB_DIV:
        timer_sync();
        div_off = -POS();
        tim_last = 0;
        if (ser_on) ser_on = 0, gb_io[GB_SC] &= 0x7F;      // (a transfer under way is dropped: not the hardware's way)
        schedule();
        break;
    case GB_TIMA:
        timer_sync();
        gb_io[r] = v;
        schedule();
        break;
    case GB_TMA:
        timer_sync();
        gb_io[r] = v;
        break;
    case GB_TAC:
        timer_sync();
        gb_io[r] = v & 7;
        schedule();
        break;
    case GB_IF:
        gb_io[r] = v & 0x1F;
        break;
    case GB_LCDC:
        gbv_reg(r, v);
        if ((v & 0x80) && !gb.lcd_on) {
            // on: the frame starts again, a cycle into line 0
            timer_sync();
            uint8_t was = stat_now();
            uint16_t pos = POS();
            div_off += pos - 1;
            ev_pos = gbf_cyc + 1;
            gb.lcd_on = 1;
            vbl_done = 0;
            gb_io[r] = v;
            stat_rise(was);
            stat_done = POS();
            schedule();
        } else if (!(v & 0x80) && gb.lcd_on) {
            // off: LYC's comparison stays as it is
            uint16_t pos = POS();
            gb.lyc_match = lyc_match(pos / LINE_MC, pos % LINE_MC);
            gb.lcd_on = 0;
            gb_io[r] = v;
            schedule();
        } else {
            gb_io[r] = v;
        }
        break;
    case GB_STAT: {
        // (the DMG's fault: for a moment every source is enabled)
        uint8_t was = stat_now();
        gb_io[r] = 0x78;
        stat_rise(was);
        was = stat_now();
        gb_io[r] = v & 0x78;
        stat_rise(was);
        stat_done = POS();
        schedule();
        break;
    }
    case GB_LY:
        break;
    case GB_LYC:
        if (gb.lcd_on) {
            uint8_t was = stat_now();
            gb_io[r] = v;
            stat_rise(was);
            stat_done = POS();
            schedule();
        } else {
            gb_io[r] = v;
        }
        break;
    case GB_DMA: {
        gb_io[r] = v;
        gbv_oam();
        uint16_t src = (uint16_t) v << 8;
        if (src >= 0xE000) src -= 0x2000;
        if (src >= 0xC000) {
            memcpy(gb_oam, gb_wram + (src & 0x1FFF), 0xA0);     // (the usual: from the work RAM)
        } else if (src >= 0xA000 && gb.ram_banks && (gb.ram_on || !gb.mbc) && gb.mbc != 2 && !(gb.mbc == 3 && gb.rtc_sel)) {
            // (from the cartridge's RAM, in one copy: Super Mario Land 2
            // does this every frame)
#ifdef GB_WS
            _fmemcpy(gb_oam, &GBF_RAM(src & 0x1FFF), 0xA0);
#else
            memcpy(gb_oam, &GBF_RAM(src & 0x1FFF), 0xA0);
#endif
        } else {
            for (uint8_t i = 0; i < 0xA0; i++)
                gb_oam[i] = gbf_read(src + i);
        }
        break;
    }
    default:
        if (r >= 0x40 && r <= 0x4B) {
            if (gb_io[r] != v) gbv_reg(r, v);
            gb_io[r] = v;
        } else if (r >= 0x10 && r < 0x40) {
            gb_io[r] = v;
            // (a channel started: kept for the sound, which looks once a
            // frame, by when the program may have written the register
            // again without the bit)
            if (v & 0x80) {
                if (r == 0x14) gbf_trig |= 1;
                else if (r == 0x19) gbf_trig |= 2;
                else if (r == 0x1E) gbf_trig |= 4;
                else if (r == 0x23) gbf_trig |= 8;
            }
        }
        break;
    }
}

static uint8_t ram_read(uint16_t a) {
    if (!gb.ram_on && gb.mbc) return 0xFF;
    if (gb.mbc == 2) return GBF_RAM(a & 0x1FF) | 0xF0;
    if (gb.mbc == 3 && gb.rtc_sel) return gb_rtc_read();
    if (!gb.ram_banks) return 0xFF;
    return GBF_RAM(a & 0x1FFF);
}

static void ram_write(uint16_t a, uint8_t v) {
    if (!gb.ram_on && gb.mbc) return;
    if (gb.mbc == 2) {
        GBF_RAM(a & 0x1FF) = v & 15;
    } else if (gb.mbc == 3 && gb.rtc_sel) {
        gb_rtc_write(v);
        return;
    } else if (gb.ram_banks) {
        GBF_RAM(a & 0x1FFF) = v;
    } else {
        return;
    }
    gb.ram_dirty = 1;
}

uint8_t gbf_read(uint16_t a) {
    if (a < 0x4000) return GBF_ROM0(a);
    if (a < 0x8000) return GBF_ROMX(a);
    if (a < 0xA000) return gb_vram[a - 0x8000];
    if (a < 0xC000) return ram_read(a);
    if (a < 0xFE00) return gb_wram[a & 0x1FFF];
    if (a >= 0xFF80) return a == 0xFFFF ? gb_ie : gb_hram[a - 0xFF80];
    if (a >= 0xFF00) return gbf_io_read(a & 0xFF);
    if (a < 0xFEA0) return gb_oam[a - 0xFE00];
    return 0;
}

void gbf_write(uint16_t a, uint8_t v) {
    if (a >= 0xC000) {
        if (a < 0xFE00) {
            gb_wram[a & 0x1FFF] = v;
        } else if (a >= 0xFF80) {
            if (a == 0xFFFF) gb_ie = v;
            else gb_hram[a - 0xFF80] = v;
        } else if (a >= 0xFF00) {
            gbf_io_write(a & 0xFF, v);
        } else if (a < 0xFEA0) {
            if (gb_oam[a - 0xFE00] != v) {
                gbv_oam();
                gb_oam[a - 0xFE00] = v;
            }
        }
    } else if (a >= 0xA000) {
        ram_write(a, v);
    } else if (a >= 0x8000) {
        if (gb_vram[a - 0x8000] != v) {
            gbv_vram(a - 0x8000, v);
            gb_vram[a - 0x8000] = v;
        }
    } else {
        // (the windows and what the recompiled code keeps of all this are
        // set again only when something has changed: programs write the
        // same bank, or "RAM on", over and over; Race Drivin' and X spent
        // a twelfth of their time there),
        // and only the RAM's part when only that has
        uint16_t r0 = gb.rom0_bank, rx = gb.romx_bank;
        uint8_t rb = gb.ram_bank, on = gb.ram_on, rs = gb.rtc_sel, hi = gb.bank_hi, mo = gb.mbc1_mode;
        gb_cart_write(a, v);
        if (r0 != gb.rom0_bank || rx != gb.romx_bank || mo != gb.mbc1_mode || (hi != gb.bank_hi && gb.mbc == 1)) {
            gbf_plat_map();
        } else if (rb != gb.ram_bank || on != gb.ram_on || rs != gb.rtc_sel) {
            gbf_plat_ram();     // (X turns the RAM on and off around every use of it)
        }
    }
}

/* ---- start ---- */

uint8_t gbf_load(const uint8_t GB_FAR *header, uint32_t size) {
    uint8_t err = gb_cart_header(header, size);
    if (err) return err;
    gbf_reset();
    return 0;
}

// what the DMG's boot ROM leaves when it jumps to 0100h (as gb_reset)
void gbf_reset(void) {
    uint8_t mbc = gb.mbc, battery = gb.has_battery, ram_banks = gb.ram_banks, rtc = gb.has_rtc;
    uint16_t rom_banks = gb.rom_banks;
    memset(&gb, 0, sizeof(gb));
    gb.mbc = mbc;
    gb.has_battery = battery;
    gb.has_rtc = rtc;
    gb.ram_banks = ram_banks;
    gb.rom_banks = rom_banks;
    gb.bank_lo = 1;
    gb_cart_map();
    gbf_plat_map();
    memset(gb_vram, 0, 0x2000);
    memset(gb_wram, 0, 0x2000);
#ifdef GB_WS
    memset((uint8_t*) 0x8000, 0, 0x2000);       // (the OAM, the I/O registers, the high RAM)
#endif
    gb.a = 0x01; gb.f = 0xB0; gb.b = 0x00; gb.c = 0x13;
    gb.d = 0x00; gb.e = 0xD8; gb.h = 0x01; gb.l = 0x4D;
    gb.sp = 0xFFFE;
    gb.pc = 0x0100;
    gb_io[GB_IF] = 0x01;
    gb_io[GB_P1] = 0xCF;
    static const uint8_t GB_FAR snd[0x17] = { 0x80, 0xBF, 0xF3, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF,
        0x9F, 0xFF, 0xBF, 0xFF, 0xFF, 0x00, 0x00, 0xBF, 0x77, 0xF3, 0xF1 };
    for (uint8_t i = 0; i < sizeof(snd); i++) gb_io[0x10 + i] = snd[i];
    gb_io[GB_DMA] = 0xFF;
    gb_io[GB_LCDC] = 0x91;
    gb_io[GB_BGP] = 0xFC;
    gb_io[GB_OBP0] = 0xFF;
    gb_io[GB_OBP1] = 0xFF;
    gb.lcd_on = 1;
    // (in the last line of the vertical blank, as the reference)
    ev_pos = 153 * LINE_MC + 100;
    gbf_cyc = 0;
    vbl_done = 1;
    div_off = (0xABC8 >> 2) - ev_pos;
    tim_last = 0xABC8 >> 2;
    ser_on = 0;
    gbv_reset();
    stat_done = POS();
    schedule();
}
