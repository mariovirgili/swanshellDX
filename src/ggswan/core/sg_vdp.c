/**
 * sgswan - SG-1000 emulator for the WonderSwan (nileswan)
 * The TMS9918A: what neither the machine (gg.c: ports, registers, the
 * frame interrupt) nor the screen (the platform) has to know alone: the
 * colors, the sprites of the table, and what the status register says of
 * them.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef GGSWAN_SG1000
#include <stdint.h>
#include <string.h>
#include "gg.h"

// 0RGB, 4 bits each (the values everybody uses for the chip's colors,
// rounded). Color 0 is "transparent": black here, the platform puts the
// backdrop (register 7) there.
const uint16_t sg_palette[16] = {
    0x000, 0x000, 0x2C4, 0x6D7, 0x55E, 0x77F, 0xC55, 0x4EE,
    0xF55, 0xF77, 0xCB5, 0xEC8, 0x2A3, 0xC5B, 0xCCC, 0xFFF
};

uint8_t sg_sprite(uint8_t i, sg_sprite_t *s) {
    const uint8_t GGFAR *e = gg_vram + ((gg_sat_base() + i * 4) & 0x3FFF);
    uint8_t y = e[0];
    if (y == 0xD0)
        return 0;
    // (the row after the one in the table; E1h-FFh: above the top)
    s->y = (y > 0xE0 ? (int16_t) y - 256 : y) + 1;
    // (bit 7 of the color byte: 32 pixels to the left, "early clock")
    s->x = (int16_t) e[1] - ((e[3] & 0x80) ? 32 : 0);
    s->pattern = (((gg.vdp.reg[6] & 0x07) << 11)
        + (uint16_t) ((gg.vdp.reg[1] & 0x02) ? e[2] & 0xFC : e[2]) * 8) & 0x3FFF;
    s->color = e[3] & 0x0F;
    return 1;
}

uint16_t sg_sprite_row(const sg_sprite_t *s, uint8_t row) {
    uint16_t a = s->pattern + row;
    uint16_t v = (uint16_t) gg_vram[a & 0x3FFF] << 8;
    if (gg.vdp.reg[1] & 0x02)
        v |= gg_vram[(a + 16) & 0x3FFF];
    return v;
}

// the screen pixels of a row, bit 31 the leftmost: each bit twice when the
// sprites are magnified
static uint32_t row_bits(const sg_sprite_t *s, uint8_t row, uint8_t mag) {
    uint16_t v = sg_sprite_row(s, row);
    if (mag == 1)
        return (uint32_t) v << 16;
    uint32_t w = 0;
    for (uint8_t k = 0; k < 16; k++, v <<= 1)
        w = (w << 2) | ((v & 0x8000) ? 3 : 0);
    return w;
}

/*
 * The chip finds these as it draws: on every line it takes the first four
 * sprites of the table that are there (the fifth sets 5S with its number,
 * and is not drawn, nor are the ones after it), and sets C when two of
 * the four have a pixel of their patterns at the same place of the
 * picture (whatever their colors). Here, once a frame, with the table as
 * it is at the end of the picture:
 *   - the lines of each sprite where it is one of the four (shown[], a
 *     bit a line of the sprite), counting the sprites on every line in
 *     the order of the table; the fifth sprite of the first line that has
 *     one is the chip's (it stops at the first)
 *   - then every pair of sprites whose rectangles meet: the rows of their
 *     patterns, on the lines where both are shown
 * (some 30 cycles a sprite and line, and a pair costs when its rectangles
 * meet: nothing like the chip's 192 lines of 32 sprites)
 */
uint8_t sg_sprite_status(void) {
    static sg_sprite_t sp[32];
    static uint32_t shown[32];
    static uint8_t count[GG_ACTIVE_LINES];
    uint8_t n = 0, status = 0;
    uint8_t mag = sg_sprite_mag(), side = sg_sprite_size() * mag;
    int16_t fifth_line = GG_ACTIVE_LINES;

    // (none with the display off, nor in text mode)
    if (!(gg.vdp.reg[1] & 0x40) || sg_mode() == SG_MODE_TEXT)
        return 0;
    while (n < 32 && sg_sprite(n, &sp[n]))
        n++;
    if (!n)
        return 0;
    memset(count, 0, sizeof(count));
    for (uint8_t i = 0; i < n; i++) {
        uint32_t m = 0;
        int16_t y = sp[i].y;
        for (uint8_t r = 0; r < side; r++, y++) {
            if (y < 0 || y >= GG_ACTIVE_LINES)
                continue;
            if (count[y] < 4) {
                count[y]++;
                m |= (uint32_t) 1 << r;
            } else if (y < fifth_line) {
                fifth_line = y;
                status = (status & 0x20) | 0x40 | i;
            }
        }
        shown[i] = m;
    }
    for (uint8_t i = 0; i + 1 < n; i++) {
        if (!shown[i])
            continue;
        for (uint8_t j = i + 1; j < n; j++) {
            // a: the one on the left
            const sg_sprite_t *a = &sp[i], *b = &sp[j];
            int16_t dy = b->y - a->y, dx = b->x - a->x;
            if (dy <= -(int16_t) side || dy >= side || dx <= -(int16_t) side || dx >= side || !shown[j])
                continue;
            if (dx < 0) {
                a = &sp[j]; b = &sp[i];
                dx = -dx;
            }
            // the columns of a that are in the picture (0-255)
            uint32_t clip = 0xFFFFFFFFul;
            if (a->x < 0)
                clip >>= -a->x;
            if (a->x + 32 > 256)
                clip &= a->x >= 256 ? 0 : 0xFFFFFFFFul << (a->x + 32 - 256);
            int16_t y0 = sp[i].y > sp[j].y ? sp[i].y : sp[j].y;
            int16_t y1 = (sp[i].y < sp[j].y ? sp[i].y : sp[j].y) + side;
            for (int16_t y = y0; y < y1; y++) {
                uint8_t ri = y - sp[i].y, rj = y - sp[j].y;
                if (!((shown[i] >> ri) & 1) || !((shown[j] >> rj) & 1))
                    continue;
                uint8_t ra = (uint8_t) (y - a->y) / mag, rb = (uint8_t) (y - b->y) / mag;
                if (row_bits(a, ra, mag) & (row_bits(b, rb, mag) >> dx) & clip)
                    return status | 0x20;
            }
        }
    }
    return status;
}
#endif
