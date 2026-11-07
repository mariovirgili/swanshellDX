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
    gg_platform_vram_sync();   // (tile writes the platform deferred)
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
        gg_platform_vram_sync();
        gg.vdp.read_buffer = gg_vram[gg.vdp.addr];
        gg.vdp.addr = (gg.vdp.addr + 1) & 0x3FFF;
        break;
    case 2: {
        uint8_t r = v & 0x0F;
        if (r < 11) {
            uint8_t old = gg.vdp.reg[r];
            gg.vdp.reg[r] = gg.vdp.latch;
            gg_platform_vdp_reg(r, gg.vdp.latch);
            // display turned on before the end of the view: shown this
            // frame even if off again at the end (Sonic, slow frames: off
            // at line 0 and at the end, on in between; see video_present)
            if (r == 1 && (gg.vdp.latch & 0x40) && gg_current_line() < GG_VIEW_Y + GG_VIEW_H)
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
 * interrupt at line 193, a line interrupt, the end of the frame); the line
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

static uint8_t vdp_vcounter(void) {
    // NTSC 192-line mode: 0x00-0xDA, then 0xD5-0xFF
    uint16_t l = gg_current_line();
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
        for (uint16_t i = 0; i < 0x400; i++)
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
    if (line == 0) {
        gg.vdp.vscroll_frame = gg.vdp.reg[9];
        gg.vdp.reg1_frame = gg.vdp.reg[1];
    }

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
    if (line == GG_ACTIVE_LINES + 1) {
        gg.vdp.status |= 0x80;
        gg_diag_vbl();
    }

    update_irq();
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
    uint16_t a_end = to < GG_ACTIVE_LINES + 1 ? to : GG_ACTIVE_LINES + 1;
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
    if (to > GG_ACTIVE_LINES + 1)
        c = r;
    gg.vdp.line_counter = c;
    // frame interrupt
    if (from <= GG_ACTIVE_LINES + 1 && to > GG_ACTIVE_LINES + 1) {
        gg.vdp.status |= 0x80;
        gg_diag_vbl();
    }
    gg.vdp.vcounter = to - 1;
    update_irq();
}

// the next line (from l, the first one not started yet) whose start must
// happen at its exact time
static uint16_t next_event(uint16_t l) {
    if (gg_every_line)
        return l;
    uint16_t e = GG_LINES;
    if (l <= GG_ACTIVE_LINES + 1)
        e = GG_ACTIVE_LINES + 1;   // frame interrupt
    if ((gg.vdp.reg[0] & 0x10) && l <= GG_ACTIVE_LINES) {
        // the line counter reaches zero at the start of line l + counter
        uint16_t u = l + gg.vdp.line_counter;
        if (u < e)
            e = u;
    }
    return e;
}

#define started gg.vdp.started   // lines of this frame started

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
    vdp_lines(started, reached);
    if (reached > started) set_started(reached);
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
    // only the lines of the picture (the frame interrupt, the lines after
    // it and the end of the frame stay in gg_run_frame)
    if (gg_every_line || pos >= (uint16_t) (GG_ACTIVE_LINES * (unsigned) GG_CYCLES_PER_LINE))
        return -1;
    return event_step(pos);
}

int16_t gg_fast_line(int16_t left) {
    // the event waited for is the start of line e = started, reached with
    // frame_given = e * 228 exactly (left: 0 down to -227), where the line
    // counter (0) reloads and requests the interrupt; the next one is r + 1
    // lines later (r: register 10), still inside the picture (and r below
    // 128, so that the cycles fit the 16-bit run budget)
    uint16_t e = started;
    uint8_t r = gg.vdp.reg[10];
    if (gg_every_line || left > 0 || left <= -GG_CYCLES_PER_LINE
        || e == 0 || r >= 128 || e + r + 1 >= GG_ACTIVE_LINES - 1
        || gg.vdp.line_counter || !(gg.vdp.reg[0] & 0x10)
        || frame_given != (int32_t) (uint16_t) (e * (unsigned) GG_CYCLES_PER_LINE))
        return 0;
    // vdp_lines(e, e + 1) for this case, then next_event(e + 1) = e + 1 + r
    gg.vdp.line_pending = 1;
    gg.vdp.line_counter = r;
    gg.vdp.vcounter = e;
    update_irq();
    set_started(e + 1);
    int16_t want = (int16_t) ((r + 1u) * GG_CYCLES_PER_LINE);   // (r < 128: fits)
    frame_given += want;
    return want;
}

void gg_run_frame(void) {
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
    gg.frame++;
}
