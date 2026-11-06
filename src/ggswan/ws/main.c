/**
 * ggswan - Game Gear emulator for the WonderSwan (nileswan)
 * WonderSwan side: loader, VDP translation to WS screens/sprites, PSG to
 * WS sound, input. Phase 2a: interpreter, no frame timing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The PSG part follows the SN76489 driver of swanshell's VGM player
 * (Copyright (c) 2022, 2025 Adrian "asie" Siekierka, zlib license).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#ifdef GGSWAN_HOST
// PC build of this file, to test the video translation (host/wssim.c)
#include "wsshim.h"
#else
#include <ws.h>
#include <nile.h>
#include <nilefs.h>
#define IRAM(a) ((uint8_t*) (a))
#define IRAM_OFF(p) ((uint16_t) (p))
#endif
#include "gg.h"
#include "z80.h"
#include "drc_rt.h"

/* === IRAM layout (color mode) ===
 * $4000-$7FFF  Game Gear VRAM = WS 4bpp tiles 0-511 (same tile format)
 * $8000-$9FFF  Game Gear work RAM
 * $A000-       debug font tiles (768+)
 * $BFC0/$BFE0  tile 1022 (opaque, border mask) / 1023 (blank)
 * $C000-$FDFF  free in color mode; used by the plugin:
 *   $D000-$DFFF  recompiler: L0 cache of RET / JP (HL) targets (drc_rt.c)
 *   $E000-$E347  recompiler: decoded instructions, flag liveness (drc.c)
 *   $E800-$EB20  ROM path
 *   $EC00-$EFFF  first KiB of the Game Gear ROM (gg.c)
 *   $F000-$F1FF  Z80 flag tables (z80.c)
 * screen maps, sprite table, wave table: aligned arrays below $4000 (the
 * hardware needs them there); the C data and the stack share that area
 * (the stack gets the largest gap left by the linker), so big buffers
 * that do not need to be there live above $C000
 */
#define GG_VRAM_ADDR 0x4000
#define GG_RAM_ADDR  0x8000
#define FONT_TILE    768
#define MASK_TILE    1022
#define TILE_ADDR(t) (0x4000 + (uint16_t) (t) * 32)
#define TILE_ENTRY(t) (((t) & 0x1FF) | (((t) >> 9) << 13))
#define PAL_ENTRY(p) ((uint16_t) (p) << 9)
#define FONT_PALETTE 14
#define MASK_PALETTE 15

// PSRAM: Game Gear ROM in banks 0.. (64 KiB each); see wfconfig.toml
#define GG_PSRAM_FIRST_BANK 0
#define GG_ROM_MAX (512UL * 1024)

#define TILE_PTR(t) IRAM(TILE_ADDR(t))

#ifdef GGSWAN_HOST
uint8_t *gg_ram = iram + GG_RAM_ADDR;
uint8_t *gg_vram = iram + GG_VRAM_ADDR;
#define map1 ((uint16_t*) (iram + 0x2000))
#define map2 ((uint16_t*) (iram + 0x2800))
#define sprite_table (iram + 0x3000)
#define wave_table (iram + 0x3200)
#define MAP_SIZE 2048
#else
uint8_t *gg_ram = (uint8_t*) GG_RAM_ADDR;
uint8_t *gg_vram = (uint8_t*) GG_VRAM_ADDR;

static uint16_t map1[32 * 32] __attribute__((aligned(2048)));
static uint16_t map2[32 * 32] __attribute__((aligned(2048)));
static uint8_t sprite_table[128 * 4] __attribute__((aligned(512)));
static uint8_t wave_table[64] __attribute__((aligned(64)));
#define MAP_SIZE 2048

static FATFS fs;
#define ROM_PATH_SIZE (0x400 - 0xE0 + 1)
#ifndef GGSWAN_HOST
// (in the free internal RAM above E000: below 4000 there is little room
// left for the stack; drc.c uses E000-E4FF)
#define rom_path ((char*) 0xE800)
#else
static char rom_path[ROM_PATH_SIZE];
#endif
#endif

#include "font.inc"

#ifndef GGSWAN_HOST_VISIBLE
#define GGSWAN_HOST_VISIBLE static
#endif

#ifndef GGSWAN_HOST
/* === Debug text (on screen 2, before the emulation starts) === */

static uint8_t text_y;
static uint8_t text_step = 2;   // rows per line (1 on the crowded screens)

static void text_init(void) {
    // font: color 1 of palette FONT_PALETTE
    for (uint8_t g = 0; g < sizeof(font_chars) - 1; g++) {
        uint8_t *t = TILE_PTR(FONT_TILE + g);
        for (uint8_t r = 0; r < 8; r++) {
            t[r * 4] = font_data[g * 8 + r];
            t[r * 4 + 1] = ~font_data[g * 8 + r];   // opaque background (color 2)
            t[r * 4 + 2] = 0;
            t[r * 4 + 3] = 0;
        }
    }
    memset(map2, 0, MAP_SIZE); // tile 0 of a cleared VRAM: blank
    uint16_t m1 = IRAM_OFF(map1), m2 = IRAM_OFF(map2);
    outportb(WS_SCR_BASE_PORT, ((m1 >> 11) & 0x0F) | (((m2 >> 11) & 0x0F) << 4));
    WS_DISPLAY_COLOR_MEM(FONT_PALETTE)[1] = 0xFFF;
    WS_DISPLAY_COLOR_MEM(FONT_PALETTE)[2] = 0x000;
    WS_DISPLAY_COLOR_MEM(0)[0] = 0x000;
    outportb(0x01, 0x00);                 // backdrop: palette 0 color 0
    outportb(0x13, 0);                    // screen 2 scroll
    outportb(0x12, 0);
    outportb(0x00, 0x02);                 // screen 2 only
    text_y = 1;
}

static void text_print(const char __far *s) {
    uint8_t x = 1;
    while (*s && x < 27) {
        char c = *(s++);
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        uint8_t g = 0;
        for (uint8_t i = 0; font_chars[i]; i++) {
            if (font_chars[i] == c) {
                g = i;
                break;
            }
        }
        map2[text_y * 32 + x] = TILE_ENTRY(FONT_TILE + g) | PAL_ENTRY(FONT_PALETTE);
        x++;
    }
    text_y += text_step;
    if (text_y > 17)
        text_y = 17;   // (the last row is overwritten)
}

static void text_hex(const char __far *label, uint32_t v) {
    char buf[28];
    uint8_t n = 0;
    while (*label && n < 18)
        buf[n++] = *(label++);
    buf[n++] = ' ';
    for (int8_t s = 28; s >= 0; s -= 4)
        buf[n++] = "0123456789ABCDEF"[(v >> s) & 0xF];
    buf[n] = 0;
    text_print(buf);
}

static void fail(const char __far *what, uint32_t code) {
    if (text_y > 15) text_y = 15;   // (both lines on screen)
    text_print("ERROR");
    text_hex(what, code);
    while (1) {}
}

#endif

/* === ROM banks, cartridge RAM === */

// the RAM window as Game Gear slot 2 needs it (the ports may not read back)
static uint16_t ram_window_bank;
static uint8_t ram_window_flash;

static void ram_window_set(uint8_t flash, uint16_t bank) {
    ram_window_flash = flash;
    ram_window_bank = bank;
    outportb(WS_CART_BANK_FLASH_PORT, flash);
    outportw(WS_CART_EXTBANK_RAM_PORT, bank);
}

void ws_ram_window_restore(void) {
    outportb(WS_CART_BANK_FLASH_PORT, ram_window_flash);
    outportw(WS_CART_EXTBANK_RAM_PORT, ram_window_bank);
}

const uint8_t __far *gg_platform_bank(uint8_t slot, uint16_t bank) {
    uint16_t ws_bank = GG_PSRAM_FIRST_BANK + (bank >> 2);
    uint16_t offset = (bank & 3) << 14;
    switch (slot) {
    // recompiled code reaches Z80 address X as ES:X with ES = segtab[X >> 13]
    case 0: {
        // blocks may jump straight into slot 0: when it changes, start over
        static uint16_t slot0_bank = 0xFFFF;
        if (bank != slot0_bank) {
            if (slot0_bank != 0xFFFF)
                drc_flush_request = 1;
            slot0_bank = bank;
        }
        }
        outportw(WS_CART_EXTBANK_ROM0_PORT, ws_bank);
        drc_st.keys[1] = bank;
        drc_st.segtab[0] = drc_st.segtab[1] = 0x2000 + (offset >> 4);
        return MK_FP(0x2000, offset);
    case 1:
        outportw(WS_CART_EXTBANK_ROM1_PORT, ws_bank);
        drc_st.keys[2] = bank;
        drc_st.segtab[2] = drc_st.segtab[3] = 0x3000 + (offset >> 4) - 0x400;
        return MK_FP(0x3000, offset);
    default:
        // slot 2: the RAM window, on PSRAM
        ram_window_set(WS_CART_BANK_FLASH_ENABLE, ws_bank);
        drc_st.keys[3] = bank;
        drc_st.keys[0] = 0;
        drc_st.segtab[4] = drc_st.segtab[5] = 0x1000 + (offset >> 4) - 0x800;
        return MK_FP(0x1000, offset);
    }
}

uint8_t __far *gg_platform_cart_ram(uint8_t page) {
    // the RAM window, on SRAM (first 32 KiB)
    ram_window_set(WS_CART_BANK_FLASH_DISABLE, 0);
    drc_st.keys[0] = 0x08;
    drc_st.segtab[4] = drc_st.segtab[5] = 0x1000 + ((uint16_t) page << 10) - 0x800;
    return MK_FP(0x1000, (uint16_t) page << 14);
}

/* === Video: VDP to WS === */

static uint16_t nt_base = 0x3800;
static uint16_t sat_base = 0x3F00;
static uint8_t map_top;        // first "virtual" Game Gear row in map1
static uint8_t backdrop_palette;

/*
 * Horizontal scroll can change on any line (status bars, parallax). The
 * changes seen while emulating a frame are recorded as "splits" (WS line,
 * screen 1 X scroll); at the end of the frame they are handed to a line
 * interrupt that replays them while the WS draws the screen.
 */
#define MAX_SPLITS 24
static uint8_t rec_line[MAX_SPLITS], rec_val[MAX_SPLITS], rec_n = 1;
static void hscroll_write(uint8_t value);
GGSWAN_HOST_VISIBLE volatile uint8_t act_line[MAX_SPLITS], act_val[MAX_SPLITS], act_n = 1;
static volatile uint8_t act_i;

uint16_t ws_map1_iram(void) {
    return IRAM_OFF(map1);
}

// Game Gear name table entry -> WS screen entry
static inline uint16_t conv_entry(uint16_t e) {
    uint16_t w = e & 0x1FF;
    if (e & 0x800) w |= PAL_ENTRY(1);     // sprite palette
    if (e & 0x200) w |= 0x4000;           // horizontal flip
    if (e & 0x400) w |= 0x8000;           // vertical flip
    return w;
}

static inline uint16_t gg_entry(uint8_t row, uint8_t col) {
    uint16_t a = (nt_base + row * 64 + col * 2) & 0x3FFF;
    return gg_vram[a] | (gg_vram[a + 1] << 8);
}

/*
 * The Game Gear map has 28 rows, the WS map 32. map1 holds the 32
 * consecutive Game Gear rows map_top .. map_top + 31 (wrapping at 28): WS
 * row (v & 31) shows Game Gear row (v % 28) for v in that range. When the
 * view leaves it, the map is laid out again.
 */
static void map_relayout(uint8_t top) {
    map_top = top;
    drc_st.map_top = top;
    for (uint8_t w = 0; w < 32; w++) {
        uint8_t v = top + ((w - top) & 31);
        uint8_t r = v % 28;
        for (uint8_t c = 0; c < 32; c++)
            map1[w * 32 + c] = conv_entry(gg_entry(r, c));
    }
}

#ifdef GGSWAN_HOST
// tests: map1 entries that differ from the Game Gear name table
uint16_t host_map_check(void) {
    uint16_t bad = 0;
    for (uint8_t w = 0; w < 32; w++) {
        uint8_t v = map_top + ((w - map_top) & 31);
        uint8_t r = v % 28;
        for (uint8_t c = 0; c < 32; c++)
            if (map1[w * 32 + c] != conv_entry(gg_entry(r, c))) bad++;
    }
    return bad;
}
#endif

static void map_put(uint8_t row, uint8_t col, uint16_t w) {
    for (uint8_t v = row; v < map_top + 32; v += 28)
        if (v >= map_top)
            map1[(v & 31) * 32 + col] = w;
}

void gg_platform_vram_write(uint16_t a, uint8_t value) {
    (void) value;
    uint16_t off = a - nt_base;
    if (off < 0x700) {
        map_put(off >> 6, (off >> 1) & 31, conv_entry(gg_entry(off >> 6, (off >> 1) & 31)));
    } else if ((uint16_t) (a - sat_base) < 0x100) {
        drc_st.sprites_dirty = true;
    }
}

void gg_platform_cram_write(uint8_t i) {
    uint16_t c = gg_cram[i * 2] | (gg_cram[i * 2 + 1] << 8);
    // Game Gear: ----BBBBGGGGRRRR; WS: ----RRRRGGGGBBBB
    uint16_t ws = ((c & 0x00F) << 8) | (c & 0x0F0) | ((c >> 8) & 0x00F);
    if (i < 16) {
        WS_DISPLAY_COLOR_MEM(0)[i] = ws;
    } else {
        WS_DISPLAY_COLOR_MEM(1)[i - 16] = ws;
        WS_DISPLAY_COLOR_MEM(8)[i - 16] = ws;
    }
}

void gg_platform_vdp_reg(uint8_t r, uint8_t v) {
    switch (r) {
    case 2:
        nt_base = (v & 0x0E) << 10;
        drc_st.nt_base = nt_base;
        map_relayout(map_top);
        break;
    case 5:
        sat_base = (v & 0x7E) << 7;
        drc_st.sat_base = sat_base;
        drc_st.sprites_dirty = true;
        break;
    case 0: case 1: case 6:
        drc_st.sprites_dirty = true;
        break;
    case 8:
        hscroll_write(v);
        break;
    }
}

void gg_platform_line(uint16_t line) {
    // (only called on the PC tests, which stop at every line)
#ifdef GGSWAN_HOST
    host_line_hook(line);
#else
    (void) line;
#endif
}

// A write to the horizontal scroll register: it takes effect from the next
// line. WS x shows Game Gear picture column x + 16.
static void hscroll_write(uint8_t value) {
    uint8_t v = 16 - value;
    // the same value as the last split: nothing changes (and the line, which
    // takes some work to find, is not needed)
    if (v == rec_val[rec_n - 1])
        return;
    uint16_t line = gg_current_line() + 1;
    if (line <= GG_VIEW_Y) {
        rec_val[0] = v;   // before the view: the value for its first line
        rec_n = 1;
        drc_st.hs_last = value;
        return;
    }
    uint16_t y = line - GG_VIEW_Y;
    if (y >= GG_VIEW_H || v == rec_val[rec_n - 1])
        return;
    if (rec_n > 1 && rec_line[rec_n - 1] == y) {
        rec_val[rec_n - 1] = v;
    } else if (rec_n < MAX_SPLITS) {
        rec_line[rec_n] = y;
        rec_val[rec_n] = v;
        rec_n++;
    } else {
        return;
    }
    drc_st.hs_last = value;
}

#ifndef GGSWAN_HOST
static void __attribute__((interrupt, assume_ss_data)) line_irq(void) {
    uint8_t i = act_i;
    if (inportb(WS_DISPLAY_LINE_IRQ_PORT) >= GG_VIEW_H) {
        // end of the screen: value for the first line of the next one
        outportb(WS_SCR1_SCRL_X_PORT, act_val[0]);
        i = 1;
    } else {
        outportb(WS_SCR1_SCRL_X_PORT, act_val[i]);
        i++;
    }
    act_i = i;
    // interrupt on the line before each split
    outportb(WS_DISPLAY_LINE_IRQ_PORT, i < act_n ? act_line[i] - 1 : GG_VIEW_H);
    ws_int_ack(WS_INT_ACK_LINE_MATCH);
}
#endif

static void splits_commit(void) {
#ifndef GGSWAN_HOST
    ia16_disable_irq();
#endif
    for (uint8_t i = 0; i < rec_n; i++) {
        act_line[i] = rec_line[i];
        act_val[i] = rec_val[i];
    }
    act_n = rec_n;
    if (act_i > act_n)
        act_i = act_n;
#ifndef GGSWAN_HOST
    ia16_enable_irq();
#endif
}

/*
 * WS screens show color 0 as transparent (the backdrop); on the Game Gear it
 * is color 0 of the tile's palette. Use as backdrop color 0 of the palette
 * most of the visible tiles use.
 */
static void update_backdrop(void) {
    uint8_t top = (GG_VIEW_Y + gg.vdp.vscroll_frame) % 224 >> 3;
    uint8_t left = (GG_VIEW_X - gg.vdp.reg[8]) >> 3;
    uint16_t sprite_pal = 0;
    for (uint8_t r = 0; r < 18; r += 2) {
        uint8_t row = (top + r) % 28;
        for (uint8_t c = 0; c < 20; c += 2)
            if (gg_entry(row, (left + c) & 31) & 0x800)
                sprite_pal++;
    }
    backdrop_palette = sprite_pal > 45 ? 1 : 0;
}

static void put_sprite(uint8_t n, uint16_t tile, uint8_t x, uint8_t y) {
    uint8_t *s = sprite_table + n * 4;
    s[0] = tile & 0xFF;
    // bit 8 of the tile; palette 0 (= WS palette 8); behind screen 2
    s[1] = (tile >> 8) & 1;
    s[2] = y;
    s[3] = x;
}

static void build_sprites(void) {
    const uint8_t *reg = gg.vdp.reg;
    uint16_t tbase = (reg[6] & 0x04) ? 0x100 : 0;
    bool tall = reg[1] & 0x02;
    uint8_t xoff = (reg[0] & 0x08) ? 8 : 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < 64 && n < 127; i++) {
        uint8_t y = gg_vram[sat_base + i];
        if (y == 0xD0)
            break;
        uint8_t x = gg_vram[sat_base + 0x80 + i * 2];
        uint16_t t = tbase | gg_vram[sat_base + 0x81 + i * 2];
        // Game Gear (x, y + 1) in the 256x192 picture; the view starts at
        // (48, 24) and is shown at WS (32, 0)
        uint8_t wy = y + 1 - GG_VIEW_Y;
        uint8_t wx = x - xoff - (GG_VIEW_X - 32);
        if (tall) {
            put_sprite(n++, t & ~1, wx, wy);
            put_sprite(n++, t | 1, wx, wy + 8);
        } else {
            put_sprite(n++, t, wx, wy);
        }
    }
    outportb(0x05, 0);        // first sprite
    outportb(0x06, n);        // sprite count
    drc_st.sprites_dirty = false;
}

GGSWAN_HOST_VISIBLE void video_init(void) {
    drc_st.sprites_dirty = 1;
    drc_st.nt_base = nt_base;
    drc_st.sat_base = sat_base;
    memset(gg_vram, 0, 0x4000);
    memset(map1, 0, MAP_SIZE);
    // border mask: tile 1022 opaque (color 1 of MASK_PALETTE = black)
    uint8_t *t = TILE_PTR(MASK_TILE);
    for (uint8_t r = 0; r < 8; r++) {
        t[r * 4] = 0xFF;
        t[r * 4 + 1] = t[r * 4 + 2] = t[r * 4 + 3] = 0;
    }
    memset(TILE_PTR(1023), 0, 32);
    for (uint16_t i = 0; i < 32 * 32; i++)
        map2[i] = TILE_ENTRY(MASK_TILE) | PAL_ENTRY(MASK_PALETTE);
    WS_DISPLAY_COLOR_MEM(MASK_PALETTE)[1] = 0x000;
    memset(sprite_table, 0, 512);

    uint16_t m1 = IRAM_OFF(map1), m2 = IRAM_OFF(map2);
    outportb(WS_SCR_BASE_PORT, ((m1 >> 11) & 0x0F) | (((m2 >> 11) & 0x0F) << 4));
    outportb(0x04, (IRAM_OFF(sprite_table) >> 9) & 0x3F);
    // screen 2 (the mask) only outside the 160x144 window
    outportb(0x08, 32);
    outportb(0x09, 0);
    outportb(0x0A, 32 + GG_VIEW_W - 1);
    outportb(0x0B, GG_VIEW_H - 1);
    outportb(0x12, 0);
    outportb(0x13, 0);
    outportb(0x06, 0);
    map_relayout(0);
#ifndef GGSWAN_HOST
    ia16_disable_irq();
    ws_int_set_handler(WS_INT_LINE_MATCH, (ia16_int_handler_t) line_irq);
    outportb(WS_DISPLAY_LINE_IRQ_PORT, GG_VIEW_H);
    ws_int_enable(WS_INT_ENABLE_LINE_MATCH);
    ia16_enable_irq();
#endif
}

GGSWAN_HOST_VISIBLE void video_present(void) {
    prof_phase = PROF_VIDEO;
    const uint8_t *reg = gg.vdp.reg;

    // vertical: the Game Gear view starts at row (24 + vscroll) mod 224
    uint16_t s = (GG_VIEW_Y + gg.vdp.vscroll_frame) % 224;
    uint16_t vy = s;
    if (vy < map_top * 8u)
        vy += 224;
    if (vy > map_top * 8u + (256 - GG_VIEW_H - 8)) {
        map_relayout(s >> 3);
        vy = s;
    }
    outportb(0x11, vy & 0xFF);
    splits_commit();
    // record the next frame: it starts with the current value
    rec_val[0] = 16 - gg.vdp.reg[8];
    rec_n = 1;
    drc_st.hs_last = gg.vdp.reg[8];
    if (!(gg.frame & 7))
        update_backdrop();

    if (drc_st.sprites_dirty)
        build_sprites();

    if (reg[1] & 0x40) {
        outportb(0x01, backdrop_palette << 4);        // backdrop: color 0
        outportb(0x00, 0x01 | 0x02 | 0x04 | 0x10 | 0x20);
    } else {
        outportb(0x01, 0x10 | (reg[7] & 0x0F));       // backdrop color
        outportb(0x00, 0x02 | 0x10 | 0x20);
    }
    prof_phase = PROF_OTHER;
}

/* === Sound: SN76489 to WS === */

static const uint8_t __far psg_volume_table[16] = {
    15, 12, 10, 8, 6, 4, 3, 2, 2, 2, 1, 1, 1, 1, 0, 0
};
static uint8_t psg_latch, psg_noise, psg_stereo = 0xFF;
static uint8_t psg_vol[4];
static uint16_t psg_tone[3];

static void psg_volume(uint8_t ch) {
    outportb(0x88 + ch, psg_vol[ch] * ((psg_stereo >> ch) & 0x11));
}

static void psg_noise_freq(void) {
    uint16_t tone;
    switch (psg_noise & 3) {
    case 0: tone = 0x10; break;
    case 1: tone = 0x20; break;
    case 2: tone = 0x40; break;
    default: tone = psg_tone[2]; break;
    }
    // 3072000 * tone / (3579545 >> 5)
    uint16_t divider = ((uint32_t) tone * 28118) >> 10;
    outportw(0x80 + 3 * 2, -divider);
    outportb(WS_SOUND_CH_CTRL_PORT, (psg_noise & 4) ? 0x8F : 0x0F);
}

void gg_platform_psg(uint8_t data) {
    uint8_t target;
    if (data & 0x80) {
        target = data;
        psg_latch = data;
    } else {
        target = psg_latch;
    }
    uint8_t ch = (target >> 5) & 3;
    switch ((target >> 4) & 7) {
    case 1: case 3: case 5: case 7:
        psg_vol[ch] = psg_volume_table[data & 0x0F];
        psg_volume(ch);
        break;
    case 6:
        psg_noise = data & 7;
        psg_noise_freq();
        break;
    default:
        if (data & 0x80)
            psg_tone[ch] = (psg_tone[ch] & 0x3F0) | (data & 0x0F);
        else
            psg_tone[ch] = (psg_tone[ch] & 0x00F) | ((data & 0x3F) << 4);
        // 3072000 * tone / (3579545 >> 1): the wave holds two periods
        outportw(0x80 + ch * 2, -(uint16_t) (((uint32_t) psg_tone[ch] * 1758) >> 10));
        if (ch == 2 && (psg_noise & 3) == 3)
            psg_noise_freq();
        break;
    }
}

void gg_platform_stereo(uint8_t v) {
    psg_stereo = v;
    for (uint8_t ch = 0; ch < 4; ch++)
        psg_volume(ch);
}

GGSWAN_HOST_VISIBLE void sound_init(void) {
    outportb(WS_SOUND_WAVE_BASE_PORT, IRAM_OFF(wave_table) >> 6);
    for (uint8_t ch = 0; ch < 3; ch++)
        for (uint8_t i = 0; i < 16; i++)
            wave_table[ch * 16 + i] = (i & 4) ? 0xFF : 0x00;
    for (uint8_t i = 0; i < 16; i++)
        wave_table[48 + i] = (i & 7) ? 0x00 : 0x0F;
    for (uint8_t ch = 0; ch < 4; ch++)
        outportb(0x88 + ch, 0);
    outportb(WS_SOUND_CH_CTRL_PORT, 0x0F);
    outportb(WS_SOUND_OUT_CTRL_PORT, WS_SOUND_OUT_CTRL_HEADPHONE_ENABLE
        | WS_SOUND_OUT_CTRL_SPEAKER_ENABLE | WS_SOUND_OUT_CTRL_SPEAKER_VOLUME_100);
    outportb(WS_SOUND_NOISE_CTRL_PORT, WS_SOUND_NOISE_CTRL_LENGTH_1953
        | WS_SOUND_NOISE_CTRL_ENABLE | WS_SOUND_NOISE_CTRL_RESET);
}

/* === Input === */

GGSWAN_HOST_VISIBLE void read_keys(void) {
    uint16_t k = ws_keypad_scan();
    uint8_t g = 0;
    if (k & WS_KEY_X1) g |= GG_KEY_UP;
    if (k & WS_KEY_X3) g |= GG_KEY_DOWN;
    if (k & WS_KEY_X4) g |= GG_KEY_LEFT;
    if (k & WS_KEY_X2) g |= GG_KEY_RIGHT;
    if (k & WS_KEY_B) g |= GG_KEY_1;
    if (k & WS_KEY_A) g |= GG_KEY_2;
    if (k & WS_KEY_START) g |= GG_KEY_START;
    gg.keys = g;
}

#ifndef GGSWAN_HOST
/* === Loading === */

static FIL fp;

static uint16_t load_rom(void) {
    int16_t result = f_open(&fp, rom_path, FA_READ | FA_OPEN_EXISTING);
    if (result != FR_OK)
        fail("OPEN", result);
    uint32_t size = f_size(&fp);
    text_hex("SIZE", size);
    if (size > GG_ROM_MAX || size < 0x400)
        fail("ROM SIZE", size);

    // FatFs must run with the RAM window on SRAM (it uses the IPC bank),
    // so read 8 KiB at a time into IRAM ($8000, the future Game Gear RAM)
    // and copy it to PSRAM through the RAM window.
    uint8_t *buf = (uint8_t*) GG_RAM_ADDR;
    uint32_t done = 0;
    uint32_t total = (size + 0xFFFF) & ~0xFFFFUL;
    while (done < total) {
        unsigned int br = 0;
        if (done < size) {
            result = f_read(&fp, buf, 0x2000, &br);
            if (result != FR_OK)
                fail("READ", result);
        }
        if (br < 0x2000)
            memset(buf + br, 0xFF, 0x2000 - br);
        outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
        outportw(WS_CART_EXTBANK_RAM_PORT, GG_PSRAM_FIRST_BANK + (done >> 16));
        _fmemcpy(MK_FP(0x1000, (uint16_t) done), buf, 0x2000);
        outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
        done += 0x2000;
    }
    f_close(&fp);

    // check: the last 8 KiB read must be visible through ROM window 0
    outportw(WS_CART_EXTBANK_ROM0_PORT, GG_PSRAM_FIRST_BANK + ((done - 0x2000) >> 16));
    {
        const uint8_t __far *chk = MK_FP(0x2000, (uint16_t) (done - 0x2000));
        for (uint16_t i = 0; i < 0x2000; i++)
            if (chk[i] != buf[i])
                fail("PSRAM MAP", done);
    }

    uint16_t banks16 = (size + 0x3FFF) >> 14;
    return banks16;
}

static void hex_line(const char __far *label, const uint16_t *v, uint8_t n) {
    char buf[29];
    uint8_t k = 0;
    while (*label && k < 4) buf[k++] = *(label++);
    for (uint8_t i = 0; i < n && k < 24; i++) {
        buf[k++] = ' ';
        for (int8_t s = 12; s >= 0; s -= 4)
            buf[k++] = "0123456789ABCDEF"[(v[i] >> s) & 15];
    }
    buf[k] = 0;
    text_print(buf);
}

static void regs_lines(const char __far *label, const z80_t *z) {
    uint16_t v[4];
    v[0] = (z->a << 8) | z->f; v[1] = (z->b << 8) | z->c;
    v[2] = (z->d << 8) | z->e; v[3] = (z->h << 8) | z->l;
    hex_line(label, v, 4);
    v[0] = z->sp; v[1] = z->pc; v[2] = (z->ixh << 8) | z->ixl; v[3] = (z->iyh << 8) | z->iyl;
    hex_line("", v, 4);
}

void drc_check_failed(uint16_t pc, const z80_t *before, const z80_t *drc,
                      const z80_t *ref, int16_t used, int16_t used_ref,
                      uint16_t sum_drc, uint16_t sum_ref) {
    ia16_disable_irq();
    outportb(WS_DISPLAY_CTRL_PORT, 0);
    text_init();
    text_step = 1;   // (more lines than spaced rows)
    text_print("SELF-CHECK: DIFFERENCE");
    uint16_t v[6];
    v[0] = pc;
    hex_line("PC", v, 1);
    for (uint8_t i = 0; i < 6; i++)
        v[i] = (z80_read(pc + i * 2) << 8) | z80_read(pc + i * 2 + 1);
    hex_line("Z80", v, 4);
    hex_line("", v + 4, 2);
    text_print("AF BC DE HL / SP PC IX IY");
    regs_lines("BEF", before);
    regs_lines("DRC", drc);
    regs_lines("REF", ref);
    v[0] = used; v[1] = used_ref; v[2] = sum_drc; v[3] = sum_ref;
    hex_line("CYC", v, 2);
    hex_line("RAM", v + 2, 2);
    v[0] = gg.mapper[0]; v[1] = gg.mapper[1]; v[2] = gg.mapper[2]; v[3] = gg.mapper[3];
    hex_line("MAP", v, 4);
    while (1) {}
}

// the interpreter, marked for the profiler
static int32_t interp_run(int32_t cycles) {
    prof_phase = PROF_INTERP;
    int32_t r = z80_run(cycles);
    prof_phase = PROF_OTHER;
    return r;
}

static volatile uint8_t vblanks;
// frame limiter: the Game Gear shows 59.92 frames a second, the WS 75.47:
// every WS frame earns 5992, a Game Gear frame costs 7547
#define FRAME_EARN 5992
#define FRAME_COST 7547
static volatile uint16_t frame_credit;
static volatile uint16_t prof_count[PROF_COUNT];
static volatile uint16_t prof_total;

static void __attribute__((interrupt, assume_ss_data)) vblank_irq(void) {
    vblanks++;
    if (frame_credit < 3 * FRAME_COST)   // (no catching up after slow parts)
        frame_credit += FRAME_EARN;
    // profiler: one sample of what the plugin is doing, 75 times a second
    prof_count[prof_phase]++;
    prof_total++;
    ws_int_ack(WS_INT_ACK_VBLANK);
}

static uint16_t font_entry(char c) {
    uint8_t g = 0;
    for (uint8_t i = 0; font_chars[i]; i++)
        if (font_chars[i] == c) { g = i; break; }
    return TILE_ENTRY(FONT_TILE + g) | PAL_ENTRY(FONT_PALETTE);
}

// Diagnostics in the borders (screen 2, outside the Game Gear view):
// left columns 0-3, right columns 24-27, one empty row between lines.
static void put_text(uint8_t row, uint8_t col, const char __far *t) {
    while (*t)
        map2[row * 32 + col++] = font_entry(*(t++));
}

static void put_value(uint8_t row, uint8_t col, char label, uint32_t v) {
    char t[5];
    if (v > 999) v = 999;
    t[0] = label;
    t[1] = v >= 100 ? '0' + v / 100 : ' ';
    t[2] = v >= 10 ? '0' + (v / 10) % 10 : ' ';
    t[3] = '0' + v % 10;
    t[4] = 0;
    put_text(row, col, t);
}

static void put_hex(uint8_t row, uint8_t col, uint16_t v) {
    char t[5];
    for (uint8_t i = 0; i < 4; i++)
        t[i] = "0123456789ABCDEF"[(v >> (12 - i * 4)) & 15];
    t[4] = 0;
    put_text(row, col, t);
}

static bool show_stats;

// Profile: share of the samples (percent) in the right border, every 8
// seconds: CD translated code, DI dispatcher, PO helpers (ports, memory),
// ST interpreted inside blocks, IN interpreter, TR translating,
// VI end of frame video, AL the rest
static void show_profile(void) {
    static const char __far labels[] = "ALCDDIPOSTINTRVI";   // (enum order)
    uint16_t total = prof_total - prof_count[PROF_IDLE];   // (time spent busy)
    if (prof_total < 600)
        return;
    if (total == 0)
        total = 1;
    for (uint8_t i = 0; i < PROF_IDLE; i++) {
        uint32_t pct = (uint32_t) prof_count[i] * 100 / total;
        if (pct > 99) pct = 99;
        char t[5];
        t[0] = labels[i * 2];
        t[1] = labels[i * 2 + 1];
        t[2] = '0' + pct / 10;
        t[3] = '0' + pct % 10;
        t[4] = 0;
        // order on screen: CD DI PO ST IN TR VI AL (rows 1, 3, ... 15)
        static const uint8_t row_of[PROF_COUNT] = { 15, 1, 3, 5, 7, 9, 11, 13 };
        put_text(row_of[i], 24, t);
    }
    ia16_disable_irq();
    for (uint8_t i = 0; i < PROF_COUNT; i++)
        prof_count[i] = 0;
    prof_total = 0;
    ia16_enable_irq();
}

static void show_fps(uint8_t fps) {
    put_text(1, 0, "FPS");
    put_value(2, 0, ' ', fps);
    if (!show_stats)
        return;
    // per second: T translations; D returns to the dispatcher, helpers
    // W (write) O (out) I (in) S (interpreted step), N interpreted
    // instructions, Q interrupts taken, all in hundreds; X cache flushes
    // (total); bottom right: last instruction interpreted (not interrupts)
    static uint32_t l_blocks, l_disp, l_help[5], l_interp, l_irq;
    put_value(4, 0, 'T', drc_stat_blocks - l_blocks);
    put_value(6, 0, 'D', (drc_stat_dispatch - l_disp) / 100);
    put_value(8, 0, 'W', (drc_stat_helper[1] - l_help[1]) / 100);
    put_value(10, 0, 'O', (drc_stat_helper[2] - l_help[2]) / 100);
    put_value(12, 0, 'I', (drc_stat_helper[3] - l_help[3]) / 100);
    put_value(14, 0, 'S', (drc_stat_helper[4] - l_help[4]) / 100);
    put_value(16, 0, 'N', (drc_stat_interp - l_interp) / 100);
    put_value(17, 0, 'Q', (drc_stat_irq - l_irq) / 100);
    put_hex(17, 24, drc_last_interp_pc);
    put_value(0, 0, 'X', drc_stat_flushes);
    l_blocks = drc_stat_blocks; l_disp = drc_stat_dispatch; l_interp = drc_stat_interp; l_irq = drc_stat_irq;
    for (uint8_t i = 0; i < 5; i++) l_help[i] = drc_stat_helper[i];
}

int main(void) {
    // The bootstub hides the nileswan registers and powers the TF card off
    // before starting a plugin: turn them back on. Bank masks: keep the ROM
    // mask (the linear area, where this code runs, always uses it), but do
    // not apply masks to the ROM0, ROM1 and RAM windows, so that PSRAM banks,
    // SRAM, the IPC bank and the SPI buffers can all be reached.
    nile_io_unlock();
    uint16_t old_mask = inportw(IO_NILE_SEG_MASK);
    outportb(IO_NILE_POW_CNT, NILE_POW_CLOCK | NILE_POW_IO_NILE | NILE_POW_IO_2001
        | NILE_POW_IO_2003 | NILE_POW_SRAM);
    outportw(IO_NILE_SEG_MASK, (old_mask & NILE_SEG_MASK_ROM_MASK) | (0xF << NILE_SEG_MASK_RAM_SHIFT));

    ws_system_set_mode(WS_MODE_COLOR_4BPP);
    outportb(WS_DISPLAY_CTRL_PORT, 0);
    memset(gg_vram, 0, 0x4000);
    text_init();
    text_print("GGSWAN 0.1 - GAME GEAR");
    text_hex("MASK", inportw(IO_NILE_SEG_MASK));

    // path of the .gg file, left by the menu in the IPC area
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, NILE_SEG_RAM_IPC);
    {
        const char __far *src = MK_FP(0x1000, 0x00E0);
        uint16_t i = 0;
        while (src[i] && i < ROM_PATH_SIZE - 1) {
            rom_path[i] = src[i];
            i++;
        }
        rom_path[i] = 0;
    }
    text_print(rom_path);
    if (!rom_path[0])
        fail("NO PATH", 0);

    int16_t result = f_mount(&fs, "", 1);
    if (result != FR_OK)
        fail("MOUNT", result);
    text_print("SD OK");

    uint16_t banks = load_rom();
    text_hex("BANKS", banks);

    // CPU: the recompiler, or the interpreter if Y1 is held
    if (ws_keypad_scan() & WS_KEY_Y1) {
        text_print("CPU: INTERPRETER");
        gg_cpu_run = interp_run;
    } else {
        // the code cache: the PSRAM bank behind linear segment DRC_CODE_SEG
        uint16_t code_bank = (((uint16_t) inportb(0xC0 /* linear bank */) << 4)
            | (DRC_CODE_SEG >> 12)) & (old_mask & NILE_SEG_MASK_ROM_MASK);
        text_hex("CODE BANK", code_bank);
        if (code_bank < GG_PSRAM_FIRST_BANK + 8)
            fail("CODE BANK", code_bank);
        drc_rt_init(code_bank);
        // the stubs written through the RAM window must be visible there
        // (the EXIT stub starts with mov byte [reason], 0: C6h)
        if (*((const uint8_t __far*) MK_FP(DRC_CODE_SEG, 0)) != 0xC6)
            fail("CODE READ", code_bank);
        uint8_t bad = drc_rt_test_banks();
        if (bad)
            fail("BANK TEST", bad);
        gg_cpu_run = drc_z80_run;
        gg_cpu_left = drc_cycles_left;
        show_stats = true;
        if (ws_keypad_scan() & WS_KEY_Y3) {
            drc_no_links = 1;
            drc_check = 1;
            text_print("CPU: RECOMPILER, SELF-CHECK");
        } else if (ws_keypad_scan() & WS_KEY_Y2) {
            drc_no_links = 1;
            text_print("CPU: RECOMPILER, NO LINKS");
        } else {
            text_print("CPU: RECOMPILER");
        }
    }

    // give time to read the text
    for (uint16_t i = 0; i < 60; i++) {
        while (inportb(WS_DISPLAY_LINE_PORT) != 144) {}
        while (inportb(WS_DISPLAY_LINE_PORT) == 144) {}
    }

    memset(gg_ram, 0, 0x2000);
    drc_st.hs_last = 16;   // (rec_val[0] = 0: register value 16)
    video_init();
    sound_init();
    gg_init(banks);

    ia16_disable_irq();
    ws_int_set_handler(WS_INT_VBLANK, (ia16_int_handler_t) vblank_irq);
    ws_int_enable(WS_INT_ENABLE_VBLANK);
    ia16_enable_irq();

    uint8_t frames = 0;
    while (1) {
        // never faster than the Game Gear (waiting time is not in the profile)
        prof_phase = PROF_IDLE;
        while (frame_credit < FRAME_COST)
            __asm__ volatile ("hlt");
        ia16_disable_irq();
        frame_credit -= FRAME_COST;
        ia16_enable_irq();
        prof_phase = PROF_OTHER;
        read_keys();
        gg_run_frame();
        video_present();
        frames++;
        // frames per second, in the left border (the WS shows 75.47 Hz)
        if (vblanks >= 75) {
            vblanks -= 75;
            show_fps(frames);
            show_profile();
            frames = 0;
        }
    }
}
#endif
