/**
 * sgswan host tests: the reference renderer of the TMS9918A (sgtest,
 * wssim_sg, libdrcsys_sg): a line of the picture from the VRAM and the
 * registers as they are, Graphics I, Graphics II with the masks of
 * registers 3 and 4, multicolor, text, the sprites with their four to a
 * line. Included in one C file of each tool.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include "../core/gg.h"

static inline uint8_t vr(uint16_t a) { return gg_vram[a & 0x3FFF]; }

// (the most sprites the chip found on a line, the fifth counted)
static uint8_t sg_ref_max_line;

// the sprites of a line: the first four of the table that are on it.
// Returns how many; *fifth: the number of the fifth, or -1.
static uint8_t sg_ref_line_sprites(uint16_t line, sg_sprite_t *out, uint8_t *index, int *fifth) {
    uint8_t n = 0, side = sg_sprite_size() * sg_sprite_mag();
    sg_sprite_t s;
    *fifth = -1;
    for (uint8_t i = 0; i < 32 && sg_sprite(i, &s); i++) {
        if ((int) line < s.y || (int) line >= s.y + side)
            continue;
        if (n == 4) {
            *fifth = i;
            break;
        }
        index[n] = i;
        out[n++] = s;
    }
    return n;
}

// the sprites of a line over color[] (or nowhere: color = NULL). Returns
// 1 if two of them overlap on it; *fifth as sg_ref_line_sprites.
static uint8_t sg_ref_sprite_line(uint16_t line, uint8_t *color, int *fifth) {
    sg_sprite_t s[4];
    uint8_t index[4];
    uint8_t n = sg_ref_line_sprites(line, s, index, fifth);
    uint8_t mag = sg_sprite_mag(), side = sg_sprite_size() * mag;
    uint8_t taken[256], hit = 0;
    memset(taken, 0, sizeof(taken));
    for (uint8_t i = 0; i < n; i++) {
        uint16_t bits = sg_sprite_row(&s[i], (line - s[i].y) / mag);
        for (uint8_t k = 0; k < side; k++) {
            int x = s[i].x + k;
            if (x < 0 || x > 255 || !((bits << (k / mag)) & 0x8000))
                continue;
            // (two sprites there, whatever their colors: the flag. The
            // first of the table with a color is in front: one of color
            // 0 is a plane with nothing on it, the next shows through)
            if (taken[x])
                hit = 1;
            if (!(taken[x] & 2) && color && s[i].color) {
                color[x] = s[i].color;
                taken[x] |= 2;
            }
            taken[x] |= 1;
        }
    }
    if (n + (*fifth >= 0) > sg_ref_max_line)
        sg_ref_max_line = n + (*fifth >= 0);
    return hit;
}

// one line: color[] gets the TMS color (0-15) of each pixel, colour 0
// being the backdrop's already. Returns 1 if two sprites overlap on it.
static uint8_t sg_ref_line(uint16_t line, uint8_t *color, int *fifth) {
    const uint8_t *reg = gg.vdp.reg;
    uint8_t backdrop = reg[7] & 0x0F;
    uint8_t mode = sg_mode();
    *fifth = -1;
    if (!(reg[1] & 0x40)) {
        memset(color, backdrop, 256);
        return 0;
    }
    uint16_t nt = gg_name_table_base();
    uint8_t row = line >> 3, y = line & 7;
    if (mode == SG_MODE_TEXT) {
        // 40 columns of 6 pixels, 8 pixels of backdrop on each side; the
        // two colors of register 7
        uint8_t fg = reg[7] >> 4, bg = backdrop;
        uint16_t pg = (reg[4] & 7) << 11;
        memset(color, backdrop, 256);
        for (uint16_t x = 0; x < 240; x++) {
            uint8_t name = vr(nt + row * 40 + x / 6);
            uint8_t p = vr(pg + name * 8 + y);
            color[8 + x] = ((p << (x % 6)) & 0x80) ? fg : bg;
        }
    } else {
        for (uint16_t x = 0; x < 256; x++) {
            uint16_t name = vr(nt + row * 32 + (x >> 3));
            uint8_t bit = 0x80 >> (x & 7), c;
            if (mode == SG_MODE_G2) {
                // 768 patterns and 8 colors bytes each: the tables are
                // at 0000h or 2000h, with masks from registers 3 and 4
                uint16_t cmask = ((reg[3] & 0x7F) << 3) | 7;
                uint16_t pmask = ((reg[4] & 3) << 8) | (cmask & 0xFF);
                uint16_t ch = name + (row >> 3) * 256;
                uint8_t p = vr(((reg[4] & 4) << 11) + ((ch & pmask) << 3) + y);
                uint8_t k = vr(((reg[3] & 0x80) << 6) + ((ch & cmask) << 3) + y);
                c = (p & bit) ? k >> 4 : k & 15;
            } else if (mode == SG_MODE_MC) {
                // blocks of 4x4 pixels: two colors a byte, two bytes of
                // the pattern a row of characters
                uint8_t k = vr(((reg[4] & 7) << 11) + name * 8 + ((row & 3) << 1) + (y >> 2));
                c = (x & 4) ? k & 15 : k >> 4;
            } else {
                uint8_t p = vr(((reg[4] & 7) << 11) + name * 8 + y);
                uint8_t k = vr((reg[3] << 6) + (name >> 3));
                c = (p & bit) ? k >> 4 : k & 15;
            }
            color[x] = c;
        }
    }
    for (uint16_t x = 0; x < 256; x++)
        if (!color[x])
            color[x] = backdrop;
    if (mode == SG_MODE_TEXT)
        return 0;   // (no sprites)
    return sg_ref_sprite_line(line, color, fifth);
}
