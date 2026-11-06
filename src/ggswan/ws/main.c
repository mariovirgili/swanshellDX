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

/* === IRAM layout (color mode) ===
 * $4000-$7FFF  Game Gear VRAM = WS 4bpp tiles 0-511 (same tile format)
 * $8000-$9FFF  Game Gear work RAM
 * $A000-       debug font tiles (768+)
 * $BFC0/$BFE0  tile 1022 (opaque, border mask) / 1023 (blank)
 * screen maps, sprite table, wave table: aligned arrays below $4000
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
#define MAP_SIZE MAP_SIZE

static FATFS fs;
static char rom_path[0x400 - 0xE0 + 1];
#endif

#include "font.inc"

#ifndef GGSWAN_HOST_VISIBLE
#define GGSWAN_HOST_VISIBLE static
#endif

#ifndef GGSWAN_HOST
/* === Debug text (on screen 2, before the emulation starts) === */

static uint8_t text_y;

static void text_init(void) {
    // font: color 1 of palette FONT_PALETTE
    for (uint8_t g = 0; g < sizeof(font_chars) - 1; g++) {
        uint8_t *t = TILE_PTR(FONT_TILE + g);
        for (uint8_t r = 0; r < 8; r++) {
            t[r * 4] = font_data[g * 8 + r];
            t[r * 4 + 1] = 0;
            t[r * 4 + 2] = 0;
            t[r * 4 + 3] = 0;
        }
    }
    memset(map2, 0, MAP_SIZE); // tile 0 of a cleared VRAM: blank
    uint16_t m1 = IRAM_OFF(map1), m2 = IRAM_OFF(map2);
    outportb(WS_SCR_BASE_PORT, ((m1 >> 11) & 0x0F) | (((m2 >> 11) & 0x0F) << 4));
    WS_DISPLAY_COLOR_MEM(FONT_PALETTE)[1] = 0xFFF;
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
    if (text_y < 17)
        text_y++;
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
    text_print("ERROR");
    text_hex(what, code);
    while (1) {}
}

#endif

/* === ROM banks, cartridge RAM === */

const uint8_t __far *gg_platform_bank(uint8_t slot, uint16_t bank) {
    uint16_t ws_bank = GG_PSRAM_FIRST_BANK + (bank >> 2);
    uint16_t offset = (bank & 3) << 14;
    switch (slot) {
    case 0:
        outportw(WS_CART_EXTBANK_ROM0_PORT, ws_bank);
        return MK_FP(0x2000, offset);
    case 1:
        outportw(WS_CART_EXTBANK_ROM1_PORT, ws_bank);
        return MK_FP(0x3000, offset);
    default:
        // slot 2: the RAM window, on PSRAM
        outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
        outportw(WS_CART_EXTBANK_RAM_PORT, ws_bank);
        return MK_FP(0x1000, offset);
    }
}

uint8_t __far *gg_platform_cart_ram(uint8_t page) {
    // the RAM window, on SRAM (first 32 KiB)
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, 0);
    return MK_FP(0x1000, (uint16_t) page << 14);
}

/* === Video: VDP to WS === */

static uint16_t nt_base = 0x3800;
static uint16_t sat_base = 0x3F00;
static uint8_t map_top;        // first "virtual" Game Gear row in map1
static bool sprites_dirty = true;
static uint8_t backdrop_palette;

/*
 * Horizontal scroll can change on any line (status bars, parallax). The
 * changes seen while emulating a frame are recorded as "splits" (WS line,
 * screen 1 X scroll); at the end of the frame they are handed to a line
 * interrupt that replays them while the WS draws the screen.
 */
#define MAX_SPLITS 24
static uint8_t rec_line[MAX_SPLITS], rec_val[MAX_SPLITS], rec_n;
GGSWAN_HOST_VISIBLE volatile uint8_t act_line[MAX_SPLITS], act_val[MAX_SPLITS], act_n = 1;
static volatile uint8_t act_i;

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
    for (uint8_t w = 0; w < 32; w++) {
        uint8_t v = top + ((w - top) & 31);
        uint8_t r = v % 28;
        for (uint8_t c = 0; c < 32; c++)
            map1[w * 32 + c] = conv_entry(gg_entry(r, c));
    }
}

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
        sprites_dirty = true;
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
        map_relayout(map_top);
        break;
    case 5:
        sat_base = (v & 0x7E) << 7;
        sprites_dirty = true;
        break;
    case 0: case 1: case 6:
        sprites_dirty = true;
        break;
    }
}

void gg_platform_line(uint16_t line) {
#ifdef GGSWAN_HOST
    host_line_hook(line);
#endif
    uint8_t y = line - GG_VIEW_Y;
    if (y >= GG_VIEW_H)
        return;
    // WS x shows Game Gear picture column x + 16
    uint8_t v = 16 - gg.vdp.reg[8];
    if (y == 0) {
        rec_val[0] = v;
        rec_n = 1;
    } else if (v != rec_val[rec_n - 1] && rec_n < MAX_SPLITS) {
        rec_line[rec_n] = y;
        rec_val[rec_n] = v;
        rec_n++;
    }
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
    sprites_dirty = false;
}

GGSWAN_HOST_VISIBLE void video_init(void) {
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
    if (!(gg.frame & 7))
        update_backdrop();

    if (sprites_dirty)
        build_sprites();

    if (reg[1] & 0x40) {
        outportb(0x01, backdrop_palette << 4);        // backdrop: color 0
        outportb(0x00, 0x01 | 0x02 | 0x04 | 0x10 | 0x20);
    } else {
        outportb(0x01, 0x10 | (reg[7] & 0x0F));       // backdrop color
        outportb(0x00, 0x02 | 0x10 | 0x20);
    }
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
    if (_fmemcmp(MK_FP(0x2000, (uint16_t) (done - 0x2000)), buf, 0x2000))
        fail("PSRAM MAP", done);

    uint16_t banks16 = (size + 0x3FFF) >> 14;
    return banks16;
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
        while (src[i] && i < sizeof(rom_path) - 1) {
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

    // give time to read the text
    for (uint16_t i = 0; i < 60; i++) {
        while (inportb(WS_DISPLAY_LINE_PORT) != 144) {}
        while (inportb(WS_DISPLAY_LINE_PORT) == 144) {}
    }

    memset(gg_ram, 0, 0x2000);
    video_init();
    sound_init();
    gg_init(banks);

    while (1) {
        read_keys();
        gg_run_frame();
        video_present();
    }
}
#endif
