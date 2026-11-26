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
// (sprites on each line: a count up where each starts, down where it
// ends, then summed; both routines below use it, one at a time)
static int8_t cnt[GG_ACTIVE_LINES + 2];

/*
 * What the chip does not draw: on a line with more than four sprites, the
 * fifth of the table and the ones after it (whatever their colors and
 * wherever they are from left to right). For the platform, which draws a
 * sprite whole: hid[i], for sprite i of the table (sat: the 128 bytes of
 * it), bit r set when its line r, from its top, is one of those (lines of
 * the picture only). Returns 0, hid not written, when no line has more
 * than four: most frames.
 */
uint8_t sg_sprite_hidden(const uint8_t *sat, uint32_t *hid) {
    int16_t sy[32];
    uint8_t n = 0, any = 0;
    int16_t side = sg_sprite_size() * sg_sprite_mag();
    for (; n < 32; n++) {
        uint8_t y = sat[n * 4];
        if (y == 0xD0)
            break;
        sy[n] = (y > 0xE0 ? (int16_t) y - 256 : y) + 1;
    }
    if (n <= 4)
        return 0;
    memset(cnt, 0, sizeof(cnt));
    uint8_t top = GG_ACTIVE_LINES, bottom = 0;
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
        if (c <= 4)
            continue;
        if (!any) {
            memset(hid, 0, 32 * sizeof(uint32_t));
            any = 1;
        }
        uint8_t k = 0;
        for (uint8_t i = 0; i < n; i++)
            if (y >= sy[i] && y < sy[i] + side && ++k > 4)
                hid[i] |= (uint32_t) 1 << (y - sy[i]);
    }
    return any;
}

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

static uint8_t sat[128];
static int16_t sx[32], sy[32];
static uint8_t sp_idx[32];   // (their numbers in the table)
static uint16_t rows[32][16];
static uint8_t have[32];     // (rows: 0 not read yet, 1 read, 2 all empty)
// (with more than four on a line: the lines of each sprite, from its
// first, where it is not one of the four; u16 for mag == 1, u32 for mag == 2)
static union { uint32_t u32[32]; uint16_t u16[32]; } hid;
static uint8_t dead[32];
// vertical bands of width `side` (8 -> 25, 16 -> 13, 32 -> 7); entries store j*2 (FFh = end)
static uint8_t bhead[25], bnxt2[64];
static uint8_t sp_n, sp_status, sp_crowded, sp_scans, sp_hid_done;
static uint8_t sp_mag, sp_big, sp_top, sp_bottom, sp_shift, sp_nband;
static int16_t sp_side;
static uint16_t sp_side2;
#ifdef __ia16__
static uint8_t cand[32], sp_cur_i, sp_cur_b, sp_b1;
static int16_t sp_xlo;
#endif

static void sg_load_pat(uint8_t k) {
    uint8_t pat[32];
    uint8_t len = sp_big ? 32 : 8;
    uint16_t p = (((gg.vdp.reg[6] & 0x07) << 11)
        + (uint16_t) (sp_big ? (sat[sp_idx[k] * 4 + 2] & 0xFC) : sat[sp_idx[k] * 4 + 2]) * 8) & 0x3FFF;
    if (p > (uint16_t) (0x4000u - len)) {
        // (the last patterns of the VRAM: it goes on at 0000h)
        for (uint8_t r = 0; r < len; r++)
            pat[r] = gg_vram[(p + r) & 0x3FFF];
    } else {
#ifdef __ia16__
        _fmemcpy(pat, gg_vram + p, len);
#else
        memcpy(pat, gg_vram + p, len);
#endif
    }
    uint16_t any = 0;
    if (sp_big) {
        for (uint8_t r = 0; r < 16; r++) {
            uint16_t v = ((uint16_t) pat[r] << 8) | pat[r + 16];
            rows[k][r] = v;
            any |= v;
        }
    } else {
        for (uint8_t r = 0; r < 8; r++) {
            uint16_t v = (uint16_t) pat[r] << 8;
            rows[k][r] = v;
            any |= v;
        }
    }
    have[k] = any ? 1 : 2;
}

static void sg_build_hid(void) {
    uint8_t n = sp_n;
    int16_t side = sp_side;
    if (sp_mag == 1) {
        for (uint8_t q = 0; q < n; q++)
            hid.u16[q] = 0;
        for (uint8_t l = sp_top; l < sp_bottom; l++) {
            if (cnt[l] <= 4)
                continue;
            uint8_t k = 0;
            for (uint8_t q = 0; q < n; q++)
                if (l >= sy[q] && l < sy[q] + side && ++k > 4)
                    hid.u16[q] |= (uint16_t) 1u << (uint8_t) (l - sy[q]);
        }
        for (uint8_t q = 0; q < n; q++) {
            int16_t lo = sy[q] < 0 ? -sy[q] : 0;
            int16_t hi = sy[q] + side > GG_ACTIVE_LINES ? GG_ACTIVE_LINES - sy[q] : side;
            uint16_t on = (uint16_t) ((hi >= 16 ? 0xFFFFu : (((uint16_t) 1u << hi) - 1u))
                & ~(((uint16_t) 1u << lo) - 1u));
            dead[q] = (hid.u16[q] & on) == on;
        }
    } else {
        for (uint8_t q = 0; q < n; q++)
            hid.u32[q] = 0;
        for (uint8_t l = sp_top; l < sp_bottom; l++) {
            if (cnt[l] <= 4)
                continue;
            uint8_t k = 0;
            for (uint8_t q = 0; q < n; q++)
                if (l >= sy[q] && l < sy[q] + side && ++k > 4)
                    hid.u32[q] |= (uint32_t) 1 << (l - sy[q]);
        }
        for (uint8_t q = 0; q < n; q++) {
            int16_t lo = sy[q] < 0 ? -sy[q] : 0;
            int16_t hi = sy[q] + side > GG_ACTIVE_LINES ? GG_ACTIVE_LINES - sy[q] : side;
            uint32_t on = (hi >= 32 ? 0xFFFFFFFFul : ((uint32_t) 1 << hi) - 1)
                & ~(((uint32_t) 1 << lo) - 1);
            dead[q] = (hid.u32[q] & on) == on;
        }
    }
    sp_hid_done = 1;
}

// Pair (i, j) with j < i whose bounding rectangles overlap:
// returns 1 on pixel collision, 2 if sprite i cannot collide with any sprite, 0 otherwise
static uint8_t sg_check_pair(uint8_t i, uint8_t j) {
    if (have[j] == 2 || (sp_hid_done && dead[j]))
        return 0;
    if (!have[i])
        sg_load_pat(i);
    if (have[i] == 2)
        return 2;
    if (!have[j])
        sg_load_pat(j);
    if (have[j] == 2)
        return 0;
    int16_t yi = sy[i], yj = sy[j];
    int16_t y0 = yi > yj ? yi : yj;
    int16_t y1 = (yi < yj ? yi : yj) + sp_side;
    if (y0 < 0) y0 = 0;
    if (y1 > GG_ACTIVE_LINES) y1 = GG_ACTIVE_LINES;
    if (y0 >= y1)
        return 0;
    // a: the one on the left
    int16_t dx = sx[j] - sx[i];
    uint8_t ia = i, ib = j;
    if (dx < 0) {
        ia = j; ib = i;
        dx = -dx;
    }
    int16_t ax = sx[ia];
    if (sp_mag == 1) {
        if (ax + 16 <= 0)
            return 0;
        uint16_t clip16 = 0xFFFFu;
        if (ax < 0)
            clip16 >>= -ax;
        if (ax > 240)
            clip16 &= (uint16_t) (0xFFFFu << (ax - 240));
        const uint16_t *ra = rows[ia] + (y0 - sy[ia]);
        const uint16_t *rb = rows[ib] + (y0 - sy[ib]);
        uint8_t sh = (uint8_t) dx;
        if (!sp_crowded) {
            for (uint8_t n_rows = (uint8_t) (y1 - y0); n_rows; n_rows--)
                if ((*ra++ & (*rb++ >> sh)) & clip16)
                    return 1;
            return 0;
        }
        for (int16_t y = y0; y < y1; y++, ra++, rb++) {
            if (cnt[y] > 4) {
                // j < i, so both are drawn on line y iff i is among the first four
                if (!sp_hid_done && ++sp_scans > 16)
                    sg_build_hid();
                if (sp_hid_done) {
                    if (dead[i])
                        return 2;
                    if (dead[j])
                        return 0;
                    if ((hid.u16[i] >> (uint8_t) (y - yi)) & 1)
                        continue;
                } else {
                    uint8_t k = 0;
                    for (uint8_t q = 0; q < i; q++)
                        if (y >= sy[q] && y < sy[q] + sp_side && ++k == 4)
                            goto skip16;
                }
            }
            if ((*ra & (*rb >> sh)) & clip16)
                return 1;
        skip16:;
        }
        return 0;
    }
    if (ax + 32 <= 0)
        return 0;
    uint32_t clip32 = 0xFFFFFFFFul;
    if (ax < 0)
        clip32 >>= -ax;
    if (ax > 224)
        clip32 &= 0xFFFFFFFFul << (ax - 224);
    for (int16_t y = y0; y < y1; y++) {
        if (sp_crowded && cnt[y] > 4) {
            if (!sp_hid_done && ++sp_scans > 16)
                sg_build_hid();
            if (sp_hid_done) {
                if (dead[i])
                    return 2;
                if (dead[j])
                    return 0;
                if ((hid.u32[i] >> (y - yi)) & 1)
                    continue;
            } else {
                uint8_t k = 0;
                for (uint8_t q = 0; q < i; q++)
                    if (y >= sy[q] && y < sy[q] + sp_side && ++k == 4)
                        goto skip32;
            }
        }
        uint8_t ra = (uint8_t) (y - sy[ia]) >> 1, rb = (uint8_t) (y - sy[ib]) >> 1;
        if (row_bits(rows[ia][ra], 2) & (row_bits(rows[ib][rb], 2) >> dx) & clip32)
            return 1;
    skip32:;
    }
    return 0;
}

#ifdef __ia16__
// V30MZ: parse SAT into sy/sx/sp_idx, clear have[], and (when n > 4) build
// line counts cnt[top..bottom-1] and the 5th-sprite status byte.
static __attribute__((noinline)) void sg_prep_asm(void) {
    __asm__ volatile (
        "pushw %%bp\n\t"
        "pushw %%es\n\t"
        "pushw %%ds\n\t"
        "popw %%es\n\t"
        "cld\n\t"
        "movw $%c[sat], %%si\n\t"
        "movw %c[side], %%bp\n\t"
        "xorw %%bx, %%bx\n\t"
        "xorw %%cx, %%cx\n\t"
    "1:\n\t"
        "movb (%%si), %%al\n\t"
        "cmpb $0xD0, %%al\n\t"
        "je 3f\n\t"
        "xorb %%ah, %%ah\n\t"
        "cmpb $0xE0, %%al\n\t"
        "jbe 2f\n\t"
        "decb %%ah\n\t"
    "2:\n\t"
        "incw %%ax\n\t"
        "cmpw $192, %%ax\n\t"
        "jge 4f\n\t"
        "movw %%ax, %%dx\n\t"
        "addw %%bp, %%dx\n\t"
        "testw %%dx, %%dx\n\t"
        "jle 4f\n\t"
        "movw %%ax, %c[sy](%%bx)\n\t"
        "movb 1(%%si), %%al\n\t"
        "xorb %%ah, %%ah\n\t"
        "testb $0x80, 3(%%si)\n\t"
        "jz 5f\n\t"
        "subw $32, %%ax\n\t"
    "5:\n\t"
        "movw %%ax, %c[sx](%%bx)\n\t"
        "movw %%bx, %%di\n\t"
        "shrw $1, %%di\n\t"
        "movb %%cl, %c[idx](%%di)\n\t"
        "addw $2, %%bx\n\t"
    "4:\n\t"
        "addw $4, %%si\n\t"
        "incb %%cl\n\t"
        "cmpb $32, %%cl\n\t"
        "jb 1b\n\t"
    "3:\n\t"
        "shrw $1, %%bx\n\t"
        "movb %%bl, %c[sp_n]\n\t"
        "movb $0, %c[sp_st]\n\t"
        "movb $0, %c[sp_cr]\n\t"
        "cmpw $2, %%bx\n\t"
        "jb 99f\n\t"
        "movw %%bx, %%cx\n\t"
        "movw $%c[have], %%di\n\t"
        "xorb %%al, %%al\n\t"
        "rep stosb\n\t"
        "cmpw $4, %%bx\n\t"
        "jbe 99f\n\t"
        "movw $%c[cnt], %%di\n\t"
        "movw $97, %%cx\n\t"
        "xorw %%ax, %%ax\n\t"
        "rep stosw\n\t"
        "movw $192, %%si\n\t"
        "xorw %%di, %%di\n\t"
        "movw %%bx, %%cx\n\t"
        "xorw %%bx, %%bx\n\t"
    "6:\n\t"
        "movw %c[sy](%%bx), %%ax\n\t"
        "movw %%ax, %%dx\n\t"
        "addw %%bp, %%dx\n\t"
        "testw %%ax, %%ax\n\t"
        "jge 7f\n\t"
        "xorw %%ax, %%ax\n\t"
    "7:\n\t"
        "cmpw $192, %%dx\n\t"
        "jle 8f\n\t"
        "movw $192, %%dx\n\t"
    "8:\n\t"
        "cmpw %%dx, %%ax\n\t"
        "jge 11f\n\t"
        "pushw %%bx\n\t"
        "movw %%ax, %%bx\n\t"
        "incb %c[cnt](%%bx)\n\t"
        "movw %%dx, %%bx\n\t"
        "decb %c[cnt](%%bx)\n\t"
        "popw %%bx\n\t"
        "cmpw %%si, %%ax\n\t"
        "jae 9f\n\t"
        "movw %%ax, %%si\n\t"
    "9:\n\t"
        "cmpw %%di, %%dx\n\t"
        "jbe 11f\n\t"
        "movw %%dx, %%di\n\t"
    "11:\n\t"
        "addw $2, %%bx\n\t"
        "loop 6b\n\t"
        "movw %%si, %%ax\n\t"
        "movb %%al, %c[sp_top]\n\t"
        "movw %%di, %%ax\n\t"
        "movb %%al, %c[sp_bot]\n\t"
        "cmpw %%di, %%si\n\t"
        "jae 99f\n\t"
        "xorb %%dl, %%dl\n\t"
        "xorb %%dh, %%dh\n\t"
    "12:\n\t"
        "addb %c[cnt](%%si), %%dl\n\t"
        "movb %%dl, %c[cnt](%%si)\n\t"
        "cmpb $4, %%dl\n\t"
        "jle 16f\n\t"
        "testb %%dh, %%dh\n\t"
        "jnz 16f\n\t"
        "movb $1, %%dh\n\t"
        "movb $1, %c[sp_cr]\n\t"
        "pushw %%dx\n\t"
        "xorb %%dl, %%dl\n\t"
        "xorw %%bx, %%bx\n\t"
        "movb %c[sp_n], %%cl\n\t"
        "xorb %%ch, %%ch\n\t"
    "13:\n\t"
        "movw %c[sy](%%bx), %%ax\n\t"
        "cmpw %%ax, %%si\n\t"
        "jl 14f\n\t"
        "addw %%bp, %%ax\n\t"
        "cmpw %%ax, %%si\n\t"
        "jge 14f\n\t"
        "incb %%dl\n\t"
        "cmpb $5, %%dl\n\t"
        "jne 14f\n\t"
        "shrw $1, %%bx\n\t"
        "movb %c[idx](%%bx), %%al\n\t"
        "orb $0x40, %%al\n\t"
        "movb %%al, %c[sp_st]\n\t"
        "jmp 15f\n\t"
    "14:\n\t"
        "addw $2, %%bx\n\t"
        "loop 13b\n\t"
    "15:\n\t"
        "popw %%dx\n\t"
    "16:\n\t"
        "incw %%si\n\t"
        "cmpw %%di, %%si\n\t"
        "jb 12b\n\t"
    "99:\n\t"
        "popw %%es\n\t"
        "popw %%bp\n\t"
        :
        : [sat] "i" (sat),
          [side] "i" (&sp_side),
          [sy] "i" (sy),
          [sx] "i" (sx),
          [idx] "i" (sp_idx),
          [sp_n] "i" (&sp_n),
          [sp_st] "i" (&sp_status),
          [sp_cr] "i" (&sp_crowded),
          [have] "i" (have),
          [cnt] "i" (cnt),
          [sp_top] "i" (&sp_top),
          [sp_bot] "i" (&sp_bottom)
        : "ax", "bx", "cx", "dx", "si", "di", "cc", "memory"
    );
}

// V30MZ: scan sprites from start_i through vertical bands of width `side`.
// Inserts sprites with no overlapping earlier sprite directly; stops on the
// first sprite with candidates in cand[0..nc-1] and returns (i << 8) | nc,
// or 0 when all sprites have been scanned.
static __attribute__((noinline)) uint16_t sg_scan_step_asm(uint16_t start_i) {
    uint16_t ret = start_i;
    __asm__ volatile (
        "pushw %%bp\n\t"
        "movb %%al, %c[cur_i]\n\t"
        "movw %c[side2], %%bp\n\t"
    "1:\n\t"
        "movb %c[cur_i], %%bl\n\t"
        "cmpb %c[sp_n], %%bl\n\t"
        "jae 90f\n\t"
        "xorb %%bh, %%bh\n\t"
        "cmpb $0, %c[hdone]\n\t"
        "je 2f\n\t"
        "cmpb $0, %c[dead](%%bx)\n\t"
        "jne 8f\n\t"
    "2:\n\t"
        "shlw $1, %%bx\n\t"
        "movw %c[sx](%%bx), %%ax\n\t"
        "movw %%ax, %%dx\n\t"
        "addw %c[side], %%dx\n\t"
        "testw %%dx, %%dx\n\t"
        "jle 8f\n\t"
        "subw %c[side], %%ax\n\t"
        "incw %%ax\n\t"
        "movw %%ax, %c[xlo]\n\t"
        "movw %c[sy](%%bx), %%dx\n\t"
        "movw %%dx, %%ax\n\t"
        "addw %c[side], %%ax\n\t"
        "movb %c[shift], %%cl\n\t"
        "shrw %%cl, %%ax\n\t"
        "movb %%al, %c[cur_b]\n\t"
        "subw %c[side], %%dx\n\t"
        "incw %%dx\n\t"
        "xorb %%ah, %%ah\n\t"
        "movw %%ax, %%si\n\t"
        "testw %%si, %%si\n\t"
        "jz 3f\n\t"
        "decw %%si\n\t"
    "3:\n\t"
        "incb %%al\n\t"
        "cmpb %c[nband], %%al\n\t"
        "jb 4f\n\t"
        "movb %c[nband], %%al\n\t"
        "decb %%al\n\t"
    "4:\n\t"
        "movb %%al, %c[b1]\n\t"
        "movw $%c[cand], %%di\n\t"
        "movw %c[xlo], %%cx\n\t"
    "5:\n\t"
        "movb %c[bhead](%%si), %%bl\n\t"
        "cmpb $0xFF, %%bl\n\t"
        "je 7f\n\t"
        "xorb %%bh, %%bh\n\t"
    "6:\n\t"
        "movw %c[sy](%%bx), %%ax\n\t"
        "subw %%dx, %%ax\n\t"
        "cmpw %%bp, %%ax\n\t"
        "ja 61f\n\t"
        "movw %c[sx](%%bx), %%ax\n\t"
        "subw %%cx, %%ax\n\t"
        "cmpw %%bp, %%ax\n\t"
        "ja 61f\n\t"
        "movw %%bx, %%ax\n\t"
        "shrw $1, %%ax\n\t"
        "movb %%al, (%%di)\n\t"
        "incw %%di\n\t"
    "61:\n\t"
        "movb %c[bnxt2](%%bx), %%bl\n\t"
        "cmpb $0xFF, %%bl\n\t"
        "jne 6b\n\t"
    "7:\n\t"
        "incw %%si\n\t"
        "movw %%si, %%ax\n\t"
        "cmpb %c[b1], %%al\n\t"
        "jbe 5b\n\t"
        "subw $%c[cand], %%di\n\t"
        "jnz 91f\n\t"
        "movb %c[cur_b], %%bl\n\t"
        "xorb %%bh, %%bh\n\t"
        "movb %c[bhead](%%bx), %%al\n\t"
        "movb %c[cur_i], %%dl\n\t"
        "shlb $1, %%dl\n\t"
        "movb %%dl, %c[bhead](%%bx)\n\t"
        "movb %%dl, %%bl\n\t"
        "movb %%al, %c[bnxt2](%%bx)\n\t"
    "8:\n\t"
        "incb %c[cur_i]\n\t"
        "jmp 1b\n\t"
    "90:\n\t"
        "xorw %%ax, %%ax\n\t"
        "jmp 99f\n\t"
    "91:\n\t"
        "movw %%di, %%ax\n\t"
        "movb %c[cur_i], %%ah\n\t"
    "99:\n\t"
        "popw %%bp\n\t"
        : "+a" (ret)
        : [cur_i] "i" (&sp_cur_i),
          [sp_n] "i" (&sp_n),
          [hdone] "i" (&sp_hid_done),
          [dead] "i" (dead),
          [sx] "i" (sx),
          [sy] "i" (sy),
          [side] "i" (&sp_side),
          [side2] "i" (&sp_side2),
          [xlo] "i" (&sp_xlo),
          [shift] "i" (&sp_shift),
          [cur_b] "i" (&sp_cur_b),
          [nband] "i" (&sp_nband),
          [b1] "i" (&sp_b1),
          [cand] "i" (cand),
          [bhead] "i" (bhead),
          [bnxt2] "i" (bnxt2)
        : "bx", "cx", "dx", "si", "di", "cc", "memory"
    );
    return ret;
}
#endif

static uint8_t sg_sprite_status_now(void) {
    sp_mag = sg_sprite_mag();
    sp_big = (gg.vdp.reg[1] & 0x02) != 0;
    sp_side = (int16_t) ((sp_big ? 16 : 8) * sp_mag);
    sp_side2 = (uint16_t) (2 * sp_side - 2);
    sp_shift = sp_side == 8 ? 3 : sp_side == 16 ? 4 : 5;
    sp_nband = sp_side == 8 ? 25 : sp_side == 16 ? 13 : 7;
    sp_scans = 0;
    sp_hid_done = 0;
#ifdef __ia16__
    _fmemcpy(sat, gg_vram + (gg_sat_base() & 0x3FFF), 128);
    sg_prep_asm();
    if (sp_n < 2)
        return 0;
    memset(bhead, 0xFF, sp_nband);
    uint8_t cur = 0;
    for (;;) {
        uint16_t res = sg_scan_step_asm(cur);
        uint8_t nc = (uint8_t) res;
        if (!nc)
            break;
        uint8_t i = (uint8_t) (res >> 8);
        uint8_t drop = 0;
        for (uint8_t k = 0; k < nc; k++) {
            uint8_t r = sg_check_pair(i, cand[k]);
            if (r == 1)
                return sp_status | 0x20;
            if (r == 2) {
                drop = 1;
                break;
            }
        }
        if (!drop) {
            uint8_t i2 = (uint8_t) (i << 1);
            bnxt2[i2] = bhead[sp_cur_b];
            bhead[sp_cur_b] = i2;
        }
        cur = i + 1;
    }
    return sp_status;
#else
    memcpy(sat, gg_vram + (gg_sat_base() & 0x3FFF), 128);
    uint8_t n = 0;
    sp_status = 0;
    sp_crowded = 0;
    for (uint8_t i = 0; i < 32; i++) {
        const uint8_t *e = sat + i * 4;
        if (e[0] == 0xD0)
            break;
        int16_t y = (e[0] > 0xE0 ? (int16_t) e[0] - 256 : e[0]) + 1;
        if (y >= GG_ACTIVE_LINES || y + sp_side <= 0)
            continue;
        sy[n] = y;
        sx[n] = (int16_t) e[1] - ((e[3] & 0x80) ? 32 : 0);
        sp_idx[n++] = i;
    }
    sp_n = n;
    if (n < 2)
        return 0;
    memset(have, 0, n);
    sp_top = GG_ACTIVE_LINES;
    sp_bottom = 0;
    if (n > 4) {
        memset(cnt, 0, sizeof(cnt));
        for (uint8_t i = 0; i < n; i++) {
            int16_t y0 = sy[i], y1 = y0 + sp_side;
            if (y0 < 0) y0 = 0;
            if (y1 > GG_ACTIVE_LINES) y1 = GG_ACTIVE_LINES;
            if (y0 < y1) {
                cnt[y0]++;
                cnt[y1]--;
                if (y0 < sp_top) sp_top = (uint8_t) y0;
                if (y1 > sp_bottom) sp_bottom = (uint8_t) y1;
            }
        }
        int8_t c = 0;
        for (uint8_t y = sp_top; y < sp_bottom; y++) {
            c += cnt[y];
            cnt[y] = c;
            if (c > 4 && !sp_crowded) {
                uint8_t k = 0;
                sp_crowded = 1;
                for (uint8_t i = 0; i < n; i++)
                    if (y >= sy[i] && y < sy[i] + sp_side && ++k == 5) {
                        sp_status = 0x40 | sp_idx[i];
                        break;
                    }
            }
        }
    }
    memset(bhead, 0xFF, sp_nband);
    for (uint8_t i = 0; i < n; i++) {
        if (sp_hid_done && dead[i])
            continue;
        if (sx[i] + sp_side <= 0)
            continue;
        int16_t y_lo = sy[i] - (sp_side - 1);
        int16_t x_lo = sx[i] - (sp_side - 1);
        uint8_t b = (uint8_t) ((uint16_t) (sy[i] + sp_side) >> sp_shift);
        uint8_t b0 = b ? b - 1 : 0;
        uint8_t b1 = b + 1 < sp_nband ? b + 1 : sp_nband - 1;
        uint8_t drop = 0;
        for (uint8_t bb = b0; bb <= b1 && !drop; bb++) {
            for (uint8_t j2 = bhead[bb]; j2 != 0xFF; j2 = bnxt2[j2]) {
                uint8_t j = j2 >> 1;
                if ((uint16_t) (sy[j] - y_lo) > sp_side2)
                    continue;
                if ((uint16_t) (sx[j] - x_lo) > sp_side2)
                    continue;
                uint8_t r = sg_check_pair(i, j);
                if (r == 1)
                    return sp_status | 0x20;
                if (r == 2) {
                    drop = 1;
                    break;
                }
            }
        }
        if (!drop) {
            uint8_t i2 = (uint8_t) (i << 1);
            bnxt2[i2] = bhead[b];
            bhead[b] = i2;
        }
    }
    return sp_status;
#endif
}
#endif
