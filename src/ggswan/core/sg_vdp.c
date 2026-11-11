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

// the screen pixels of a pattern's row (16 of them, bit 15 the leftmost),
// bit 31 the leftmost: each bit twice when the sprites are magnified
static uint32_t row_bits(uint16_t v, uint8_t mag) {
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
 * it is at the end of the picture, and as little work as it takes (this
 * runs on every frame of every game):
 *   - the table copied once (the VRAM may be far);
 *   - up to four sprites: no fifth, all of them drawn. More: how many
 *     are on each line (a count up where each starts, down where it ends,
 *     then the sum line after line); the first line with five has the
 *     chip's fifth sprite, the fifth of the table on that line;
 *   - every pair of sprites whose rectangles meet: the rows of their
 *     patterns, on the lines both are drawn on (a line with more than
 *     four: only the first four of the table are). A sprite's rows are
 *     read once, when the first pair needs them, and one with nothing in
 *     its pattern meets nobody. "Is it one of the four on this line" is
 *     a count of the sprites before it, asked a few times; after that,
 *     worked out for every sprite and line at once (The Castle keeps its
 *     32 sprites on top of each other for a while: 496 pairs of 16 rows,
 *     each row a count, was 85,000 cycles a frame).
 */
static uint8_t sg_sprite_status_now(void);
uint8_t sg_sprite_status(void) {

    // (none with the display off, nor in text mode)
    if (!(gg.vdp.reg[1] & 0x40) || sg_mode() == SG_MODE_TEXT)
        return 0;
    // nothing written to the table or the patterns since the last time,
    // the same registers: the same answer (most frames of most games)
    {
        static uint8_t last, r0, r1, r5, r6, valid;
        const uint8_t *reg = gg.vdp.reg;
        uint8_t same = valid && reg[0] == r0 && reg[1] == r1 && reg[5] == r5 && reg[6] == r6;
        if (!gg_platform_sprites_written() && same)
            return last;
        r0 = reg[0]; r1 = reg[1]; r5 = reg[5]; r6 = reg[6];
        valid = 1;
        last = sg_sprite_status_now();
        return last;
    }
}

static uint8_t sg_sprite_status_now(void) {
    static uint8_t sat[128];
    static int16_t sx[32], sy[32];
    static uint8_t si[32];   // (their numbers in the table)
    static uint16_t rows[32][16];
    static uint8_t have[32];   // (rows: 0 not read yet, 1 read, 2 all empty)
    static int8_t cnt[GG_ACTIVE_LINES + 1];
    // (with more than four on a line: the lines of each sprite, from its
    // first, where it is not one of the four; all of its lines)
    static uint32_t hid[32];
    static uint8_t dead[32];
    uint8_t n = 0, status = 0, crowded = 0, scans = 0, hid_done = 0;
    uint8_t mag = sg_sprite_mag();
    int16_t side = sg_sprite_size() * mag;
#ifdef __ia16__
    _fmemcpy(sat, gg_vram + (gg_sat_base() & 0x3FFF), 128);
#else
    memcpy(sat, gg_vram + (gg_sat_base() & 0x3FFF), 128);
#endif
    // (the ones on some line of the picture: games park the rest below it)
    for (uint8_t i = 0; i < 32; i++) {
        const uint8_t *e = sat + i * 4;
        if (e[0] == 0xD0)
            break;
        int16_t y = (e[0] > 0xE0 ? (int16_t) e[0] - 256 : e[0]) + 1;
        if (y >= GG_ACTIVE_LINES || y + side <= 0)
            continue;
        sy[n] = y;
        sx[n] = (int16_t) e[1] - ((e[3] & 0x80) ? 32 : 0);
        si[n++] = i;
    }
    if (n < 2)
        return 0;
    memset(have, 0, n);
    uint8_t top = GG_ACTIVE_LINES, bottom = 0;   // (the lines with sprites)
    if (n > 4) {
        memset(cnt, 0, sizeof(cnt));
        for (uint8_t i = 0; i < n; i++) {
            int16_t y0 = sy[i], y1 = y0 + side;
            if (y0 < 0) y0 = 0;
            if (y1 > GG_ACTIVE_LINES) y1 = GG_ACTIVE_LINES;
            if (y0 < y1) {
                cnt[y0]++;
                cnt[y1]--;
                if (y0 < top) top = (uint8_t) y0;
                if (y1 > bottom) bottom = (uint8_t) y1;
            }
        }
        int8_t c = 0;
        for (uint8_t y = top; y < bottom; y++) {
            c += cnt[y];
            cnt[y] = c;
            if (c > 4 && !crowded) {
                // the fifth of the table on this line
                uint8_t k = 0;
                crowded = 1;
                for (uint8_t i = 0; i < n; i++)
                    if (y >= sy[i] && y < sy[i] + side && ++k == 5) {
                        status = 0x40 | si[i];
                        break;
                    }
            }
        }
    }
    for (uint8_t i = 0; i + 1 < n; i++) {
        int16_t yi = sy[i], xi = sx[i];
        if (hid_done && dead[i])
            continue;
        for (uint8_t j = i + 1; j < n; j++) {
            if (hid_done && dead[j])
                continue;
            int16_t dy = sy[j] - yi, dx = sx[j] - xi;
            if (dy <= -side || dy >= side || dx <= -side || dx >= side)
                continue;
            // their rows
            for (uint8_t q = 0; q < 2; q++) {
                uint8_t k = q ? j : i;
                if (have[k])
                    continue;
                uint8_t pat[32], big = (gg.vdp.reg[1] & 0x02) != 0;
                uint16_t p = (((gg.vdp.reg[6] & 0x07) << 11)
                    + (uint16_t) (big ? sat[si[k] * 4 + 2] & 0xFC : sat[si[k] * 4 + 2]) * 8) & 0x3FFF;
                if (p > 0x3FE0) {
                    // (the last patterns of the VRAM: it goes on at 0000h)
                    for (uint8_t r = 0; r < 32; r++)
                        pat[r] = gg_vram[(p + r) & 0x3FFF];
                } else {
#ifdef __ia16__
                    _fmemcpy(pat, gg_vram + p, 32);
#else
                    memcpy(pat, gg_vram + p, 32);
#endif
                }
                uint16_t any = 0;
                for (uint8_t r = 0; r < 16; r++) {
                    uint16_t v = 0;
                    if (big)
                        v = ((uint16_t) pat[r] << 8) | pat[r + 16];
                    else if (r < 8)
                        v = (uint16_t) pat[r] << 8;
                    rows[k][r] = v;
                    any |= v;
                }
                have[k] = any ? 1 : 2;
            }
            if (have[i] == 2 || have[j] == 2)
                continue;
            // a: the one on the left
            uint8_t ia = i, ib = j;
            if (dx < 0) {
                ia = j; ib = i;
                dx = -dx;
            }
            // the columns of a that are in the picture (0-255)
            int16_t ax = sx[ia];
            uint32_t clip = 0xFFFFFFFFul;
            if (ax < 0)
                clip >>= -ax;
            if (ax + 32 > 256)
                clip &= ax >= 256 ? 0 : 0xFFFFFFFFul << (ax + 32 - 256);
            int16_t y0 = yi > sy[j] ? yi : sy[j];
            int16_t y1 = (yi < sy[j] ? yi : sy[j]) + side;
            if (y0 < 0) y0 = 0;
            if (y1 > GG_ACTIVE_LINES) y1 = GG_ACTIVE_LINES;
            for (int16_t y = y0; y < y1; y++) {
                if (crowded && cnt[y] > 4) {
                    // (the later of the two is drawn if it is one of the
                    // first four of the table on this line: then both are)
                    if (!hid_done && ++scans > 16) {
                        // asked too often (sprites on top of each other on
                        // lines with more than four): all the answers at
                        // once, and the sprites drawn on no line left out
                        // of the pairs from here on
                        for (uint8_t q = 0; q < n; q++)
                            hid[q] = 0;
                        for (uint8_t l = top; l < bottom; l++) {
                            if (cnt[l] <= 4)
                                continue;
                            uint8_t k = 0;
                            for (uint8_t q = 0; q < n; q++)
                                if (l >= sy[q] && l < sy[q] + side && ++k > 4)
                                    hid[q] |= (uint32_t) 1 << (l - sy[q]);
                        }
                        for (uint8_t q = 0; q < n; q++) {
                            // (its rows that are lines of the picture)
                            int16_t lo = sy[q] < 0 ? -sy[q] : 0;
                            int16_t hi = sy[q] + side > GG_ACTIVE_LINES ? GG_ACTIVE_LINES - sy[q] : side;
                            uint32_t on = (hi >= 32 ? 0xFFFFFFFFul : ((uint32_t) 1 << hi) - 1)
                                & ~(((uint32_t) 1 << lo) - 1);
                            dead[q] = (hid[q] & on) == on;
                        }
                        hid_done = 1;
                    }
                    if (hid_done) {
                        if (dead[i] || dead[j])
                            break;
                        if (((hid[i] >> (y - yi)) | (hid[j] >> (y - sy[j]))) & 1)
                            continue;
                    } else {
                        uint8_t k = 0;
                        for (uint8_t q = 0; q < j; q++)
                            if (y >= sy[q] && y < sy[q] + side)
                                k++;
                        if (k >= 4)
                            continue;
                    }
                }
                uint8_t ra = (uint8_t) (y - sy[ia]) / mag, rb = (uint8_t) (y - sy[ib]) / mag;
                if (row_bits(rows[ia][ra], mag) & (row_bits(rows[ib][rb], mag) >> dx) & clip)
                    return status | 0x20;
            }
        }
    }
    return status;
}
#endif
