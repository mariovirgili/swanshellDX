/**
 * gbswan - Game Boy (DMG) plugin for swanshell (nileswan). Step GB0: no
 * Game Boy yet, the video it is planned on, on the screen to be looked at
 * (GB-MEMORY-MAP.md):
 *
 * - the WonderSwan Color's 2 bpp color mode, whose tiles are the Game
 *   Boy's byte for byte. The Game Boy's video memory (8000h-9FFFh) is the
 *   internal RAM's 2000h-3FFFh: its 384 patterns are WS tiles 0-383, the
 *   first tile bank, the only one sprites can take, so background and
 *   sprites both use what the program writes, with nothing converted or
 *   copied;
 * - screen 1: the shade of the background's color 0, and black borders;
 * - screen 2, inside a window of 160 x 144 in the middle: the Game Boy's
 *   background, with a palette whose color 0 shows screen 1 through;
 * - sprites over screen 2 or between the two screens (the Game Boy's
 *   "behind the background": seen where the background is color 0), cut
 *   at the sides of the 160 x 144 by the sprite window.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>
#include <wonderful.h>
#include <ws.h>
#include <nile.h>

/* The internal RAM (GB-MEMORY-MAP.md). C data and the stack: below 2000h. */
#define GB_VRAM     ((uint8_t*) 0x2000)     // the Game Boy's 8000h-9FFFh: WS tiles 0-383, then its two maps
#define SCR1        ((uint16_t*) 0x4000)    // WS screen 1, 32 x 32 entries
#define SCR2        ((uint16_t*) 0x4800)    // WS screen 2
#define SPR_TABLE   ((uint8_t*) 0x5000)     // WS sprites, 4 bytes each
#define TILE_BLANK  1023                    // a WS tile of color 0 (5FF0h, second bank)

#define GB_X0 32                            // the Game Boy's 160 columns: WS 32-191
// Game Boy pattern n (0-383: 8000h + n * 16) in a WS screen or sprite entry
#define ENTRY_GB(n, pal)   ((n) | ((uint16_t) (pal) << 9))
#define ENTRY_BLANK(pal)   (0x2000 | (TILE_BLANK & 511) | ((uint16_t) (pal) << 9))
// the background's pattern for the byte in the Game Boy's map, by LCDC bit
// 4: set, 8000h + n * 16; clear, 9000h + n * 16 for 0-127 and 8800h on for
// 128-255
#define BG_PATTERN(n, lcdc4)  ((lcdc4) || (n) >= 128 ? (n) : 256 + (n))

static void pattern(uint8_t *to, const uint8_t __far *rows) {
    for (uint8_t i = 0; i < 16; i++)
        to[i] = rows[i];
}

// Game Boy patterns, as a Game Boy program would write them at 8000h+:
// two bytes a row, the low bits of the 8 pixels then the high ones
static const uint8_t __far pat_stripes[16] = {   // colors 0 0 1 1 2 2 3 3
    0x33, 0x0F, 0x33, 0x0F, 0x33, 0x0F, 0x33, 0x0F, 0x33, 0x0F, 0x33, 0x0F, 0x33, 0x0F, 0x33, 0x0F };
static const uint8_t __far pat_check[16] = {     // color 1 and color 0, 2 x 2
    0xCC, 0x00, 0xCC, 0x00, 0x33, 0x00, 0x33, 0x00, 0xCC, 0x00, 0xCC, 0x00, 0x33, 0x00, 0x33, 0x00 };
static const uint8_t __far pat_arrow[16] = {     // an "F" of color 3 on color 1, a corner of color 0
    0xFF, 0xFE, 0xFF, 0x80, 0xFF, 0x80, 0xFF, 0xF8, 0xFF, 0x80, 0xFF, 0x80, 0xFE, 0x80, 0xFC, 0x00 };

static void wait_vblank(void) {
    while (inportb(WS_DISPLAY_LINE_PORT) == 144) {}
    while (inportb(WS_DISPLAY_LINE_PORT) != 144) {}
}

static void sprite(uint8_t i, uint16_t attr, uint8_t y, uint8_t x) {
    SPR_TABLE[i * 4] = attr & 0xFF;
    SPR_TABLE[i * 4 + 1] = attr >> 8;
    SPR_TABLE[i * 4 + 2] = y;
    SPR_TABLE[i * 4 + 3] = x;
}

// back to the menu, as the other plugins (START + Y1 (Y up) for a second)
static void leave(void) {
    ia16_disable_irq();
    outportb(WS_SOUND_CH_CTRL_PORT, 0);
    outportb(WS_DISPLAY_CTRL_PORT, 0);
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, 0);
    outportw(WS_CART_EXTBANK_ROM0_PORT, 0xFFFF);
    outportw(WS_CART_EXTBANK_ROM1_PORT, 0xFFFF);
    outportw(IO_NILE_SPI_CNT, NILE_SPI_CLOCK_CART | NILE_SPI_DEV_NONE);
    outportb(IO_NILE_EMU_CNT, 0);
    nile_soft_reset();
}

void gb_main(void) {
    nile_io_unlock();
    outportb(WS_DISPLAY_CTRL_PORT, 0);
    ws_system_set_mode(WS_MODE_COLOR);          // color, 2 bits a pixel

    memset(GB_VRAM, 0, 0x2000);
    memset((uint8_t*) 0x2000 + TILE_BLANK * 16, 0, 16);

    // Game Boy patterns 1-6 (8010h-806Fh) and 257 (9010h: the background's
    // number 1 when LCDC bit 4 is clear)
    pattern(GB_VRAM + 1 * 16, pat_stripes);
    pattern(GB_VRAM + 257 * 16, pat_check);
    for (uint8_t r = 0; r < 8; r++) {
        GB_VRAM[2 * 16 + r * 2] = 0xFF;                          // 2: color 1
        GB_VRAM[3 * 16 + r * 2 + 1] = 0xFF;                      // 3: color 2
        GB_VRAM[4 * 16 + r * 2] = GB_VRAM[4 * 16 + r * 2 + 1] = 0xFF;   // 4: color 3
    }
    pattern(GB_VRAM + 5 * 16, pat_check);
    pattern(GB_VRAM + 6 * 16, pat_arrow);

    // palettes: 0 for screen 1 (color 0 shown), 1 for its black borders
    // (the same blank tile: color 0 black),
    // 4 for screen 2 (color 0 lets screen 1 through), 12 and 13 for the
    // sprites. Four grays; the second sprite palette the other way round,
    // to tell it
    static const uint16_t __far gray[4] = { 0xFFF, 0xAAA, 0x555, 0x000 };
    for (uint8_t i = 0; i < 4; i++) {
        WS_DISPLAY_COLOR_MEM(0)[i] = gray[i];
        WS_DISPLAY_COLOR_MEM(4)[i] = gray[i];
        WS_DISPLAY_COLOR_MEM(12)[i] = gray[i];
        WS_DISPLAY_COLOR_MEM(13)[i] = gray[3 - i];
        WS_DISPLAY_COLOR_MEM(1)[i] = 0x000;
    }

    // screen 1: color 0's shade behind the Game Boy's picture, black beside
    for (uint8_t r = 0; r < 32; r++)
        for (uint8_t c = 0; c < 32; c++)
            SCR1[r * 32 + c] = ENTRY_BLANK(c >= 4 && c < 24 ? 0 : 1);
    // screen 2: the Game Boy's background, all 32 x 32 of it (what is
    // outside the 160 x 144 must not be seen)
    for (uint8_t r = 0; r < 32; r++) {
        static const uint8_t __far rows[18] = { 1, 1, 2, 3, 4, 0, 0, 1, 1, 1, 5, 5, 0, 5, 1, 2, 3, 4 };
        uint8_t n = r < 18 ? rows[r] : 5;
        // (the last two rows of the 144 lines: number 1 with LCDC bit 4
        // clear, the pattern at 9010h and not the one at 8010h)
        uint16_t t = r == 16 || r == 17 ? BG_PATTERN(1, 0) : BG_PATTERN(n, 1);
        for (uint8_t c = 0; c < 32; c++)
            SCR2[r * 32 + c] = ENTRY_GB(t, 4);
    }
    // sprites (attribute: tile, palette - 8 in bits 9-11, bit 13 over
    // screen 2, bits 14 and 15 flipped). Rows 7-9 are stripes:
    sprite(0, ENTRY_GB(6, 4) | 0x2000, 60, GB_X0 + 16);           // over the background
    sprite(1, ENTRY_GB(6, 5), 60, GB_X0 + 48);                    // behind it: seen on its color 0 only
    sprite(2, ENTRY_GB(6, 4) | 0x2000 | 0x4000 | 0x8000, 60, GB_X0 + 80);   // turned both ways
    sprite(3, ENTRY_GB(6, 4) | 0x2000, 60, GB_X0 - 4);            // half out on the left: cut
    sprite(4, ENTRY_GB(6, 4) | 0x2000, 60, GB_X0 + 156);          // half out on the right: cut
    sprite(5, ENTRY_GB(6, 4) | 0x2000, 44, GB_X0 + 16);           // over color 0 (rows 5-6): on screen 1's shade

    outportb(WS_SCR_BASE_PORT, (0x4000 >> 11) | ((0x4800 >> 11) << 4));
    outportb(WS_SPR_BASE_PORT, 0x5000 >> 9);
    outportb(WS_SPR_FIRST_PORT, 0);
    outportb(WS_SPR_COUNT_PORT, 6);
    outportb(0x10, 0); outportb(0x11, 0);                        // screen 1 scroll
    uint8_t scx = 0, scy = 0;
    outportb(0x12, (uint8_t) (scx - GB_X0)); outportb(0x13, scy);   // screen 2: the Game Boy's SCX, SCY
    outportb(WS_SCR2_WIN_X1_PORT, GB_X0); outportb(WS_SCR2_WIN_Y1_PORT, 0);
    outportb(WS_SCR2_WIN_X2_PORT, GB_X0 + 159); outportb(WS_SCR2_WIN_Y2_PORT, 143);
    outportb(WS_SPR_WIN_X1_PORT, GB_X0); outportb(WS_SPR_WIN_Y1_PORT, 0);
    outportb(WS_SPR_WIN_X2_PORT, GB_X0 + 159); outportb(WS_SPR_WIN_Y2_PORT, 143);
    outportb(0x01, 0);
    outportb(WS_DISPLAY_CTRL_PORT, WS_DISPLAY_CTRL_SCR1_ENABLE | WS_DISPLAY_CTRL_SCR2_ENABLE
        | WS_DISPLAY_CTRL_SPR_ENABLE | WS_DISPLAY_CTRL_SPR_WIN_ENABLE | WS_DISPLAY_CTRL_SCR2_WIN_INSIDE);

    // X1-X4: the background scrolls (the Game Boy's SCX and SCY, by the
    // WS's own scroll); A: the background's palette turned (BGP: no tile
    // is written); START + Y1 (Y up) for a second: back to the menu
    uint8_t held = 0, was_a = 0, bgp = 0;
    for (;;) {
        wait_vblank();
        uint16_t k = ws_keypad_scan();
        if (k & WS_KEY_X2) scx++;
        if (k & WS_KEY_X4) scx--;
        if (k & WS_KEY_X3) scy++;
        if (k & WS_KEY_X1) scy--;
        outportb(0x12, (uint8_t) (scx - GB_X0));
        outportb(0x13, scy);
        if ((k & WS_KEY_A) && !was_a) {
            bgp++;
            for (uint8_t i = 0; i < 4; i++) {
                WS_DISPLAY_COLOR_MEM(0)[i] = gray[(i + bgp) & 3];
                WS_DISPLAY_COLOR_MEM(4)[i] = gray[(i + bgp) & 3];
            }
        }
        was_a = (k & WS_KEY_A) != 0;
        if ((k & (WS_KEY_START | WS_KEY_Y1)) == (WS_KEY_START | WS_KEY_Y1)) {
            if (++held >= 60)
                leave();
        } else {
            held = 0;
        }
    }
}

void gb_run(void);
int main(void) {
    gb_run();
    return 0;
}
