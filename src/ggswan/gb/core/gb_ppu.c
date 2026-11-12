/**
 * gbswan - the Game Boy's LCD controller. The timing is kept to the
 * machine cycle (4 dots); the picture is drawn a whole line at a time
 * when the line's drawing begins (mode 3), so a register changed in the
 * middle of a line is seen from the next one.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include "gb.h"

uint8_t gb_screen[144][160];

// the sprites of the line being drawn: up to 10, in OAM order
static uint8_t line_spr[10], line_spr_n;

static void scan_sprites(void) {
    uint8_t h = (gb.io[GB_LCDC] & 4) ? 16 : 8;
    line_spr_n = 0;
    if (!(gb.io[GB_LCDC] & 2)) return;
    for (uint8_t i = 0; i < 40 && line_spr_n < 10; i++) {
        uint8_t dy = gb.line + 16 - gb.oam[i * 4];
        if (dy < h) line_spr[line_spr_n++] = i;
    }
}

// how long the drawing of this line takes: 172 dots, more for the scroll's
// fine part, for the window, and for each sprite (6, and what is left of
// the background tile it starts in, once a tile)
static uint16_t mode3_length(void) {
    uint16_t len = 0;
    uint8_t seen[32];
    memset(seen, 0, sizeof(seen));
    // (leftmost first; the same X: OAM order)
    uint8_t order[10];
    for (uint8_t i = 0; i < line_spr_n; i++) order[i] = line_spr[i];
    for (uint8_t i = 1; i < line_spr_n; i++)
        for (uint8_t j = i; j > 0 && gb.oam[order[j] * 4 + 1] < gb.oam[order[j - 1] * 4 + 1]; j--) {
            uint8_t t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    for (uint8_t i = 0; i < line_spr_n; i++) {
        uint8_t x = gb.oam[order[i] * 4 + 1];
        if (x >= 168) continue;
        uint8_t bx = x + gb.io[GB_SCX];
        if (!seen[bx >> 3]) {
            seen[bx >> 3] = 1;
            uint8_t left = 7 - (bx & 7);
            if (left > 2) len += left - 2;
        }
        len += 6;
    }
    // (what the sprites add is seen by the CPU in whole cycles, rounded
    // down: Mooneye's intr_2_mode0_timing_sprites)
    return 172 + (gb.io[GB_SCX] & 7) + (len & ~3);
}

static void render_line(void) {
    uint8_t y = gb.line, lcdc = gb.io[GB_LCDC];
    uint8_t bg[160];        // the background's and the window's color numbers
    uint8_t *out = gb_screen[y];
    if (lcdc & 1) {
        uint8_t sy = y + gb.io[GB_SCY];
        const uint8_t *map = gb_vram + ((lcdc & 8) ? 0x1C00 : 0x1800) + (sy >> 3) * 32;
        uint8_t sx = gb.io[GB_SCX];
        for (uint8_t x = 0; x < 160; x++, sx++) {
            uint8_t n = map[sx >> 3];
            const uint8_t *t = gb_vram + (((lcdc & 0x10) || n >= 128) ? n * 16 : 0x1000 + n * 16) + (sy & 7) * 2;
            uint8_t b = 7 - (sx & 7);
            bg[x] = (t[0] >> b & 1) | (t[1] >> b & 1) << 1;
        }
        if ((lcdc & 0x20) && gb.win_on_frame && gb.io[GB_WX] < 167) {
            uint8_t wy = gb.win_line++;
            map = gb_vram + ((lcdc & 0x40) ? 0x1C00 : 0x1800) + (wy >> 3) * 32;
            uint8_t wx = 0;
            int16_t x0 = (int16_t) gb.io[GB_WX] - 7;
            if (x0 < 0) { wx = -x0; x0 = 0; }
            for (uint8_t x = x0; x < 160; x++, wx++) {
                uint8_t n = map[wx >> 3];
                const uint8_t *t = gb_vram + (((lcdc & 0x10) || n >= 128) ? n * 16 : 0x1000 + n * 16) + (wy & 7) * 2;
                uint8_t b = 7 - (wx & 7);
                bg[x] = (t[0] >> b & 1) | (t[1] >> b & 1) << 1;
            }
        }
        for (uint8_t x = 0; x < 160; x++)
            out[x] = gb.io[GB_BGP] >> (bg[x] * 2) & 3;
    } else {
        memset(bg, 0, sizeof(bg));
        memset(out, 0, 160);
    }
    if (!line_spr_n) return;
    // sprites: for each pixel the leftmost sprite with a color there wins
    // (the same X: the first in OAM); if it is "behind", the background's
    // colors 1-3 cover it
    uint8_t h = (lcdc & 4) ? 16 : 8;
    uint8_t best_x[160];
    memset(best_x, 0xFF, sizeof(best_x));
    for (uint8_t i = 0; i < line_spr_n; i++) {
        const uint8_t *s = gb.oam + line_spr[i] * 4;
        uint8_t row = y + 16 - s[0], n = s[2];
        if (s[3] & 0x40) row = h - 1 - row;
        if (h == 16) n &= 0xFE;
        const uint8_t *t = gb_vram + n * 16 + row * 2;
        for (uint8_t c = 0; c < 8; c++) {
            int16_t x = (int16_t) s[1] - 8 + c;
            if (x < 0 || x >= 160) continue;
            uint8_t b = (s[3] & 0x20) ? c : 7 - c;
            uint8_t px = (t[0] >> b & 1) | (t[1] >> b & 1) << 1;
            if (!px) continue;
            if (best_x[x] != 0xFF && best_x[x] <= s[1]) continue;   // (an earlier one, not to the right of this)
            best_x[x] = s[1];
            if ((s[3] & 0x80) && bg[x])
                out[x] = gb.io[GB_BGP] >> (bg[x] * 2) & 3;
            else
                out[x] = gb.io[(s[3] & 0x10) ? GB_OBP1 : GB_OBP0] >> (px * 2) & 3;
        }
    }
}

// the state at the dot reached: the mode STAT shows, what LYC is compared
// with, the interrupt's signal
static void update(void) {
    uint8_t mode, pulse = 0;
    uint8_t stat = gb.io[GB_STAT];
    if (!gb.lcd_on) {
        // (off: mode 0, and LYC's comparison stays as it was, whatever is
        // written to LYC, until the LCD is on again)
        gb.mode = 0;
        uint8_t s = (stat & 0x40) && gb.lyc_match;
        if (s && !gb.stat_line) gb.io[GB_IF] |= GB_INT_STAT;
        gb.stat_line = s;
        return;
    }
    if (gb.line < 144) {
        if (gb.dot < 4) {
            mode = 0;
            pulse = gb.line != 0 && !gb.first_line;     // mode 2's interrupt, 4 dots before the mode reads 2
        } else if (gb.dot < 84) {
            mode = gb.first_line ? 0 : 2;
            pulse = gb.line == 0 && gb.dot < 8 && !gb.first_line;
        } else if (gb.dot < gb.mode3_end) {
            mode = 3;
        } else {
            mode = 0;
        }
        gb.ly_cmp = gb.dot < 4 && gb.line != 0 ? 0xFF : gb.line;
    } else {
        mode = gb.line == 144 && gb.dot < 4 ? 0 : 1;
        // (mode 2's interrupt is asked at the start of the vertical blank too)
        pulse = gb.line == 144 && gb.dot >= 4 && gb.dot < 8;
        if (gb.line == 153) gb.ly_cmp = gb.dot < 4 ? 0xFF : gb.dot < 8 ? 153 : gb.dot < 12 ? 0xFF : 0;
        else gb.ly_cmp = gb.dot < 4 ? 0xFF : gb.line;
    }
    gb.mode = mode;
    gb.lyc_match = gb.ly_cmp == gb.io[GB_LYC];
    // (the interrupts of modes 2 and 0 are asked 4 dots before STAT shows
    // the mode: the pulse above, and here)
    uint8_t irq0 = mode == 0 || (gb.line < 144 && gb.dot >= 84 && gb.dot + 4 >= gb.mode3_end);
    uint8_t sig = ((stat & 0x40) && gb.lyc_match) || ((stat & 0x08) && irq0)
        || ((stat & 0x10) && mode == 1) || ((stat & 0x20) && (mode == 2 || pulse));
    if (sig && !gb.stat_line) gb.io[GB_IF] |= GB_INT_STAT;
    gb.stat_line = sig;
}

void gb_ppu_tick(void) {
    if (!gb.lcd_on) return;
    gb.dot += 4;
    if (gb.dot >= 456) {
        gb.dot = 0;
        gb.first_line = 0;
        if (++gb.line == 154) {
            gb.line = 0;
            gb.win_line = 0;
            gb.win_on_frame = 0;
        }
    }
    if (gb.line < 144) {
        if (gb.dot == 4 && gb.line == gb.io[GB_WY]) gb.win_on_frame = 1;
        if (gb.dot == 80) {
            if (gb.line == gb.io[GB_WY]) gb.win_on_frame = 1;
            scan_sprites();
            gb.mode3_end = 84 + mode3_length();
            render_line();
        }
    } else if (gb.line == 144 && gb.dot == 4) {
        gb.io[GB_IF] |= GB_INT_VBLANK;
        gb.frame_done = 1;
        gb.frames++;
    }
    update();
}

uint8_t gb_ppu_read(uint8_t r) {
    switch (r) {
    case GB_STAT: return 0x80 | (gb.io[r] & 0x78) | (gb.lyc_match ? 4 : 0) | gb.mode;
    case GB_LY: return gb.line == 153 && gb.dot >= 4 ? 0 : gb.line;
    default: return gb.io[r];
    }
}

void gb_ppu_write(uint8_t r, uint8_t v) {
    switch (r) {
    case GB_LCDC:
        gb.io[r] = v;
        if ((v & 0x80) && !gb.lcd_on) {
            gb.lcd_on = 1;
            gb.line = 0;
            gb.dot = 4;
            gb.first_line = 1;
            gb.mode3_end = 84 + 172;
            gb.win_line = 0;
            gb.win_on_frame = 0;
            update();
        } else if (!(v & 0x80) && gb.lcd_on) {
            gb.lcd_on = 0;
            gb.line = 0;
            gb.dot = 0;
            memset(gb_screen, 0, sizeof(gb_screen));
            update();
        }
        break;
    case GB_STAT:
        // (the DMG's fault: for a moment every source is enabled)
        gb.io[r] = 0x78;
        update();
        gb.io[r] = v & 0x78;
        update();
        break;
    case GB_LY:
        break;
    case GB_LYC:
        gb.io[r] = v;
        update();
        break;
    default:
        gb.io[r] = v;
        break;
    }
}

// The video memory and the OAM cannot be reached while the LCD uses
// them; reads and writes are refused at slightly different dots (Mooneye's
// lcdon_timing and lcdon_write_timing): the video memory is not read from
// dot 80, 4 dots before STAT shows mode 3, and not written from 84; the
// OAM is not read from the line's start and not written from dot 4, and
// can be written at dot 80. The line after the LCD is turned on has no
// mode 2.
bool gb_ppu_vram_blocked(bool write) {
    if (!gb.lcd_on || gb.line >= 144 || gb.dot >= gb.mode3_end) return false;
    return gb.dot >= (write || gb.first_line ? 84 : 80);
}
bool gb_ppu_oam_blocked(bool write) {
    if (!gb.lcd_on || gb.line >= 144 || gb.dot >= gb.mode3_end) return false;
    if (gb.first_line) return gb.dot >= 84;
    if (write) return gb.dot >= 4 && gb.dot != 80;
    return true;
}

void gb_ppu_reset(void) {
    gb.io[GB_LCDC] = 0x91;
    gb.io[GB_STAT] = 0x00;
    gb.io[GB_BGP] = 0xFC;
    gb.io[GB_OBP0] = 0xFF;
    gb.io[GB_OBP1] = 0xFF;
    gb.lcd_on = 1;
    // the boot ROM jumps to the cartridge in the last line of the vertical
    // blank (LY reads 0 there, STAT 85h)
    gb.line = 153;
    gb.dot = 400;
    gb.mode3_end = 84 + 172;
    memset(gb_screen, 0, sizeof(gb_screen));
    update();
}
