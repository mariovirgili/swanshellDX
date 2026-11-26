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
#include <stdlib.h>
#include <stdio.h>
#else
#include <ws.h>
#include <nile.h>
#include <nilefs.h>
#define IRAM(a) ((uint8_t*) (a))
#define IRAM_OFF(p) ((uint16_t) (p))
#endif
#include "gg.h"
#include "z80.h"
#include "version.h"
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
// 1 MiB, the largest Game Gear ROMs: the loader writes them through the
// RAM window, whose bank mask (set in main) covers 16 banks of 64 KiB;
// the code cache must sit above the ROM's banks (checked in main)
#define GG_ROM_MAX (1024UL * 1024)

#define TILE_PTR(t) IRAM(TILE_ADDR(t))

#ifdef GGSWAN_HOST
uint8_t *gg_ram = iram + GG_RAM_ADDR;
#ifdef GGSWAN_SG1000
uint8_t *gg_vram;   // (the TMS9918A's: sg_video.inc)
#else
uint8_t *gg_vram = iram + GG_VRAM_ADDR;
#endif
#define map1 ((uint16_t*) (iram + 0x2000))
#define map2 ((uint16_t*) (iram + 0x2800))
#define sprite_table (iram + 0x3000)
#define wave_table (iram + 0x3200)
#define sprite_table_b (iram + 0x3400)
#define MAP_SIZE 2048
#else
uint8_t *gg_ram = (uint8_t*) GG_RAM_ADDR;
#ifdef GGSWAN_SG1000
uint8_t __far *gg_vram;   // (the TMS9918A's: sg_video.inc)
#else
uint8_t *gg_vram = (uint8_t*) GG_VRAM_ADDR;
#endif

static uint16_t map1[32 * 32] __attribute__((aligned(2048)));
static uint16_t map2[32 * 32] __attribute__((aligned(2048)));
static uint8_t sprite_table[128 * 4] __attribute__((aligned(512)));
static uint8_t sprite_table_b[128 * 4] __attribute__((aligned(512)));
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

// frameskip (the menu's VIDEO, patch 223): 1 for a frame whose end-of-frame
// video work is left out (gg_platform_frame_end)
#ifdef GGSWAN_HOST
static inline uint8_t frame_skip_now(void) { return 0; }   // (the PC tests: every frame)
#else
static uint8_t frame_skip_now(void);
#endif

#ifdef GGSWAN_SF7000
#include "sf7000.h"
#ifndef GGSWAN_HOST
#include "sf_ipl.h"
#endif
/*
 * sfswan: the PSRAM banks (64 KiB each) from GG_PSRAM_FIRST_BANK:
 *   + 0      the SF-7000's 64 KiB of RAM. The RAM window stays on it: Z80
 *            address X is 1000h:X, for the interpreter (sf_ram) and for
 *            translated code
 *   + 1      the screen's tables (sg_video.inc: SG_TABLE_BANK) and, in its
 *            last 16 KiB, the TMS9918A's VRAM: read through the ROM's
 *            first window, which stays there (gg_vram), written with the
 *            RAM window moved there for the byte (gg_platform_vram_store)
 *   + 2 - 4  the disk, 163,840 bytes: a sector read through the ROM's
 *            second window, written through the RAM window
 *   + 5 - 7  the system disk, when the disk asked for needs one
 * The code cache's banks come after them (checked at the start).
 */
// (the numbers: drc_rt.h)
_Static_assert(GG_PSRAM_FIRST_BANK == 0, "sfswan: PSRAM banks from 0");
#define LOAD_BANK SF_DISK_BANK
#else
#define LOAD_BANK GG_PSRAM_FIRST_BANK
#endif

#ifndef GGSWAN_HOST_VISIBLE
#define GGSWAN_HOST_VISIBLE static
#endif

#ifndef GGSWAN_HOST
/* === Debug text (on screen 2, before the emulation starts) === */

static uint8_t text_y;
static uint8_t text_step = 2;   // rows per line (1 on the crowded screens)

// font: color 1 of palette FONT_PALETTE
static void font_load(void) {
    for (uint8_t g = 0; g < sizeof(font_chars) - 1; g++) {
        uint8_t *t = TILE_PTR(FONT_TILE + g);
        for (uint8_t r = 0; r < 8; r++) {
            t[r * 4] = font_data[g * 8 + r];
            t[r * 4 + 1] = ~font_data[g * 8 + r];   // opaque background (color 2)
            t[r * 4 + 2] = 0;
            t[r * 4 + 3] = 0;
        }
    }
}

static void text_init(void) {
    font_load();
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

// s at the top right of the text screen (the patch number at start), on
// row 0, which the other lines leave free (on the last row the start-up
// times wrote over it)
static void text_corner(const char __far *s) {
    uint8_t n = 0;
    while (s[n]) n++;
    uint8_t y = text_y;
    text_y = 0;
    uint8_t x0 = (uint8_t) (27 - n), x = 1;
    // (text_print starts at column 1: pad with spaces)
    char buf[28];
    for (; x < x0 && x < 27; x++) buf[x - 1] = ' ';
    for (uint8_t k = 0; k < n && x < 27; k++, x++) buf[x - 1] = s[k];
    buf[x - 1] = 0;
    text_print(buf);
    text_y = y;
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

// the game's name: the file's, without the folders and the extension,
// on two lines at most (26 characters each)
static void text_title(const char *path) {
    const char *name = path;
    for (const char *q = path; *q; q++)
        if (*q == '/') name = q + 1;
    uint16_t len = 0, dot = 0;
    while (name[len]) {
        if (name[len] == '.') dot = len;
        len++;
    }
    if (dot) len = dot;
    char line[27];
    for (uint8_t l = 0; l < 2 && len; l++) {
        uint8_t n = len > 26 ? 26 : (uint8_t) len;
        for (uint8_t i = 0; i < n; i++) line[i] = name[i];
        line[n] = 0;
        uint8_t step = text_step;
        if (len > n)
            text_step = 1;   // (the second line right under the first)
        text_print(line);
        text_step = step;
        name += n;
        len -= n;
    }
}

// a size in KB (rounded up), in decimal
// Start-up timing: the VBlank timer counts the frames (75.47 a second)
// down from FFFFh with no interrupt; read without stopping it.
static uint16_t t_frames(void) {
    return 0xFFFF - inportw(WS_TIMER_VBL_COUNTER_PORT);
}

// "label n" with n in milliseconds (13.25 ms a frame), added to t at n
static uint8_t put_ms(char *t, uint8_t n, const char __far *label, uint16_t frames) {
    while (*label)
        t[n++] = *(label++);
    t[n++] = ' ';
    uint32_t ms = ((uint32_t) frames * 1325 + 50) / 100;
    char d[8];
    uint8_t k = 0;
    do {
        d[k++] = '0' + (uint8_t) (ms % 10);
        ms /= 10;
    } while (ms && k < 7);
    while (k)
        t[n++] = d[--k];
    t[n++] = ' ';
    return n;
}

static void text_kb(const char __far *label, uint32_t bytes) {
    char t[28];
    uint8_t n = 0;
    while (*label && n < 18)
        t[n++] = *(label++);
    t[n++] = ' ';
    uint32_t kb = (bytes + 1023) >> 10;
    char d[8];
    uint8_t k = 0;
    do {
        d[k++] = '0' + (uint8_t) (kb % 10);
        kb /= 10;
    } while (kb && k < 7);
    while (k)
        t[n++] = d[--k];
    t[n++] = ' ';
    t[n++] = 'K';
    t[n++] = 'B';
    t[n] = 0;
    text_print(t);
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

#ifdef GGSWAN_SF7000
#ifndef GGSWAN_HOST
void gg_platform_vram_store(uint16_t a, uint8_t v) {
    outportw(WS_CART_EXTBANK_RAM_PORT, SF_VRAM_BANK);
    *(uint8_t __far*) MK_FP(0x1000, 0xC000 + a) = v;
    outportw(WS_CART_EXTBANK_RAM_PORT, ram_window_bank);
}
#endif

// (a sector is 256 bytes: lba << 8 is where it is in the image)
// the disk in the drive: the one asked for, or first the system disk
// (main: when the one asked for does not start by itself), until its
// loader gives the memory to what it loaded
static uint16_t sf_disk_bank = SF_DISK_BANK;
static uint8_t sf_swap_pending;
static uint8_t sf_auto_boot;
static uint32_t sf_boot_at;
void gg_platform_disk_read(uint16_t lba, uint8_t *buf) {
    outportw(WS_CART_EXTBANK_ROM1_PORT, sf_disk_bank + (lba >> 8));
    _fmemcpy(buf, MK_FP(0x3000, lba << 8), SF_SECTOR_SIZE);
}

// written to the copy in PSRAM only: gone when the plugin is left
static uint8_t sf_disk_written;
void gg_platform_disk_write(uint16_t lba, const uint8_t *buf) {
    outportw(WS_CART_EXTBANK_RAM_PORT, sf_disk_bank + (lba >> 8));
    _fmemcpy(MK_FP(0x1000, lba << 8), buf, SF_SECTOR_SIZE);
    outportw(WS_CART_EXTBANK_RAM_PORT, ram_window_bank);
    sf_disk_written = 1;
}

// the IPL came or went at 0000h-3FFFh: what was translated from there is
// no longer what the CPU reads
void gg_platform_map_changed(void) {
    if (sf_swap_pending && !sf.rom_on) {
        sf_swap_pending = 0;
        sf_disk_bank = SF_DISK_BANK;
        // (a disk to be started with BOOT: typed when Disk BASIC is up,
        // six seconds from here; kb_frame)
        if (sf_auto_boot)
            sf_boot_at = gg.frame + 360;
    }
#ifndef GGSWAN_HOST
    drc_flush_request = 1;
#endif
}
#endif

void ws_ram_window_restore(void) {
    outportb(WS_CART_BANK_FLASH_PORT, ram_window_flash);
    outportw(WS_CART_EXTBANK_RAM_PORT, ram_window_bank);
}

// (diagnostics, "Z": the last bank other than 0 a game mapped in slot 0.
// The ROM's first KiB stays there whatever the bank; the interpreter
// knows, translated code reads those addresses from the bank mapped: wrong
// data for a game that does both. None known: this is to hear of one)
static uint8_t diag_slot0;

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
            if (bank && !gg.codemasters)   // (Codemasters: no fixed KiB)
                diag_slot0 = (uint8_t) bank;
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

#ifdef GGSWAN_SG1000
#include "sg_video.inc"
#else
static uint16_t nt_base = 0x3800;
static uint16_t sat_base = 0x3F00;
static uint8_t map_top;        // first "virtual" Game Gear row in map1
// the name table: 28 rows of 32 entries at (register 2 & 0Eh) << 10;
// smsswan with 224 lines: 32 rows at ((register 2 & 0Ch) << 10) + 700h
#ifdef GGSWAN_SMS
static uint8_t nt_h = 28;
#define NT_H nt_h
#define NT_P (nt_h * 8u)
#define NT_SIZE (nt_h * 64u)
#define NT_BASE_OF(v) ((uint16_t) (nt_h == 32 ? (((v) & 0x0C) << 10) | 0x700 : ((v) & 0x0E) << 10))
#else
#define NT_H 28
#define NT_P 224u
#define NT_SIZE 0x700
#define NT_BASE_OF(v) ((uint16_t) (((v) & 0x0E) << 10))
#endif
static uint8_t backdrop_palette;

/*
 * Horizontal scroll can change on any line (status bars, parallax). The
 * changes seen while emulating a frame are recorded as "splits" (WS line,
 * screen 1 X scroll); at the end of the frame they are handed to a line
 * interrupt that replays them while the WS draws the screen.
 */
#define MAX_SPLITS DRC_MAX_SPLITS
// (in drc_st: the VDP control routine records splits too)
#define rec_line drc_st.rec_line
#define rec_val drc_st.rec_val
#define rec_n drc_st.rec_n
static void hscroll_write(uint8_t value);

/*
 * The VRAM page classes (drc_st.vram_class, used by the generated VRAM
 * routines and by C): bit 0 the name table, bit 1 the sprite table (both
 * start on a page), bit 3 deferred (below), bit 4 past the end of VRAM
 * ([64]: a run that wraps).
 *
 * Deferred VRAM writes. The Game Gear VRAM is the WS tile memory and the
 * name table feeds the WS map, so whatever the game writes shows at once,
 * in the middle of the WS frame being drawn, while the scroll and the
 * sprites change only between WS frames; and one Game Gear frame takes
 * two or three WS frames to emulate. Pieces of two frames showed together
 * for a WS frame: Aladdin's new poses with the old sprite table, Sonic's
 * animated sunflowers torn in half.
 *
 * So every write to tile data (class bit 3; the name table and the
 * sprite table stay live: the name table applied from C was too slow in
 * the interrupt) goes to a log in IRAM instead: entries of VRAM address (word), length (byte),
 * the bytes; a byte that follows the last entry joins it. At the end of
 * the Game Gear frame (video_present) the log is closed and handed to the
 * WS line interrupt at the end of the screen (line 144, when the WS also
 * copies the sprite table and the new scroll is set): there it is written
 * to VRAM (after the scroll), all at once, for the next WS frame. Two logs
 * take turns (one filling, one waiting for the interrupt).
 *
 * A VRAM read (the game must read what it wrote) or a full log: the logs
 * are written at once (gg_platform_vram_sync). X4 writes everything at
 * once, as before (no class bit 3).
 */
#define LOG_A 0xC400        // (IRAM, free; the PC tests' mirrors end below)
#define LOG_B 0xF200
#define LOG_SIZE 0x0C00
static uint8_t vram_class_norm[64];
// the pages whose writes were ever deferred since the last full sync: a
// VRAM read elsewhere (Aladdin's name table reads in its game screens) has
// nothing in the logs to look for (gg_platform_vram_sync_at)
static uint8_t defer_seen[64];
static uint16_t log_start;                    // the log being filled
static volatile uint16_t pend_start, pend_top; // the log waiting (0: none)

static void vram_class_update(void) {
    for (uint8_t i = 0; i < 64; i++) {
        uint16_t a = (uint16_t) i << 8;
        uint8_t c = ((uint16_t) (a - nt_base) < NT_SIZE ? 1 : 0)
                  | ((uint16_t) (a - sat_base) < 0x100 ? 2 : 0);
        vram_class_norm[i] = c;
        drc_st.vram_class[i] = (drc_st.defer && !c) ? c | 8 : c;
        defer_seen[i] |= drc_st.vram_class[i] & 8;
    }
    drc_st.vram_class[64] = 0x11;
}

static void map_put(uint8_t row, uint8_t col, uint16_t w);
static inline uint16_t conv_entry(uint16_t e);
static inline uint16_t gg_entry(uint8_t row, uint8_t col);

// a log to VRAM, in order, and the name table entries it touched to the
// WS map (also called from the line interrupt, through
// line_irq_entry: drc_asm.s sets DS and ES)
#ifndef GGSWAN_HOST
uint16_t log_copy_start, log_copy_end;
void log_copy(void);   // drc_asm.s: the copies with REP MOVSW
#endif
static void log_apply(uint16_t start, uint16_t end) {
#ifndef GGSWAN_HOST
    // (only tile data is deferred: no name table entries to update; the
    // C loop below, kept for the PC tests, took about 30 lines on the
    // device for Sonic's and Aladdin's biggest frames)
    log_copy_start = start;
    log_copy_end = end;
    log_copy();
    return;
#endif
    const uint8_t *p = IRAM(start), *e = IRAM(end);
    while (p < e) {
        uint16_t a = p[0] | (p[1] << 8);
        uint8_t n = p[2];
        memcpy(gg_vram + a, p + 3, n);
        uint16_t off = a - nt_base;
        if (off < NT_SIZE || (uint16_t) (a + n - 1 - nt_base) < NT_SIZE) {
            // name table: the words touched
            for (uint16_t k = 0; k < n; k++) {
                uint16_t o = (uint16_t) (a + k - nt_base);
                if (o < NT_SIZE && (!(o & 1) || k == 0))
                    map_put(o >> 6, (o >> 1) & 31, gg_entry(o >> 6, (o >> 1) & 31));
            }
        }
        p += 3 + n;
    }
}

// interrupts off, and back as they were (not on: the callers may already
// run with them off)
static inline uint16_t irq_save(void) {
#ifndef GGSWAN_HOST
    uint16_t f;
    __asm__ volatile ("pushf\n\tpop %0\n\tcli" : "=r" (f) :: "memory");
    return f;
#else
    return 0;
#endif
}
static inline void irq_restore(uint16_t f) {
#ifndef GGSWAN_HOST
    __asm__ volatile ("push %0\n\tpopf" :: "r" (f) : "memory");
#else
    (void) f;
#endif
}

// the line interrupt at the end of the screen: the waiting log
void log_commit_pending(void) {
    if (pend_start) {
        log_apply(pend_start, pend_top);
        pend_start = 0;
    }
}

// the deferred writes to VRAM address a, in order (the waiting log, then
// the one filling): the byte a read must see, now in VRAM; the logs stay
// (their commit writes the same last value). Only that byte shows early:
// a whole sync at a read made a frame's tiles visible mid-screen, with the
// previous frame's sprites (Aladdin reads the VRAM in its frame interrupt
// every 8 frames: pieces of its sprites in the wrong places)
static void log_apply_at(uint16_t start, uint16_t end, uint16_t a) {
    const uint8_t *p = IRAM(start), *e = IRAM(end);
    while (p < e) {
        uint16_t ea = p[0] | (p[1] << 8);
        uint8_t n = p[2];
        if ((uint16_t) (a - ea) < n)
            gg_vram[a] = p[3 + (a - ea)];
        p += 3 + n;
    }
}
// (VRAM reads in this Game Gear frame: past a few, no more deferring in it)
static uint8_t sync_reads, defer_paused;
void gg_platform_vram_sync_at(uint16_t a) {
    if (!drc_st.defer)
        return;
    // (an entry holding a starts at most 254 bytes before it) a page never
    // deferred has nothing in the logs: no work, and it does not count
    // (Aladdin reads its name table 16-34 times a frame in its levels:
    // counted, they turned the deferring off for the rest of nearly every
    // frame, its sprite tiles written in the middle of the WS screen)
    if (!defer_seen[a >> 8] && !defer_seen[((a - 254) & 0x3FFF) >> 8])
        return;
    if (sync_reads >= 16) {
        // many reads (Sailor Moon S loads with ~250 a frame, Aladdin has
        // up to 6): scanning the whole log for each one took most of the
        // time, the screens black for seconds. All of it written now, and
        // the tile writes go straight to VRAM until the end of the frame
        // (log_present), so the reads need no helper (patch 75 wrote the
        // log each time: still ~1000 lines a second in the helper)
        gg_platform_vram_sync();
        drc_st.defer = 0;
        vram_class_update();
        defer_paused = 1;
        return;
    }
    sync_reads++;
    uint16_t f = irq_save();
    if (pend_start)
        log_apply_at(pend_start, pend_top, a);
    log_apply_at(log_start, drc_st.log_top, a);
    irq_restore(f);
}

// all the deferred writes to VRAM now (in order), the log empty again
void gg_platform_vram_sync(void) {
    if (!drc_st.defer)
        return;
    uint16_t f = irq_save();
#ifdef GGSWAN_HOST
    if (getenv("GGSWAN_SYNCSTAT") && drc_st.log_top != log_start)
        fprintf(stderr, "SYNC %u %u line %u\n", (unsigned) gg.frame,
                (unsigned) (drc_st.log_top - log_start), (unsigned) gg_current_line());
#endif
    log_commit_pending();
    log_apply(log_start, drc_st.log_top);
    drc_st.log_top = log_start;
    drc_st.log_last = 0;
    drc_st.log_next = 0xFFFF;
    // (both logs empty: only the pages deferred from now on)
    for (uint8_t i = 0; i < 64; i++)
        defer_seen[i] = drc_st.vram_class[i] & 8;
    irq_restore(f);
}

// the end of the Game Gear frame: the log waits for the end of the WS
// screen; the other one fills (if the last one still waits, as when two
// frames end in one WS frame: written now)
static void log_present(void) {
    sync_reads = 0;
    if (defer_paused) {
        defer_paused = 0;
        drc_st.defer = 1;
        vram_class_update();
    }
    if (!drc_st.defer)
        return;
    // (called from video_present with interrupts already off on the device)
    log_commit_pending();
    if (drc_st.log_top != log_start) {
        pend_top = drc_st.log_top;
        pend_start = log_start;
        log_start = log_start == LOG_A ? LOG_B : LOG_A;
        drc_st.log_top = log_start;
        drc_st.log_end = log_start + LOG_SIZE;
        drc_st.log_last = 0;
    drc_st.log_next = 0xFFFF;
#ifdef GGSWAN_HOST
        if (getenv("GGSWAN_LOGSTAT"))
            fprintf(stderr, "LOGSZ %u\n", (unsigned) (pend_top - pend_start));
#endif
    }
#ifdef GGSWAN_HOST
    // (no WS raster on the PC: at once; with GGSWAN_LATE_COMMIT the PC
    // tests commit it later, from drc_z80_run, as the interrupt would)
    if (!getenv("GGSWAN_LATE_COMMIT"))
        log_commit_pending();
#endif
}

#ifdef GGSWAN_HOST
// the PC tests: the line interrupt now and then (GGSWAN_LATE_COMMIT)
void host_late_commit(void) {
    static unsigned n;
    if (getenv("GGSWAN_LATE_COMMIT") && pend_start && (++n % 3) == 0)
        log_commit_pending();
}
#endif

static void log_init(void) {
    drc_st.defer = !drc_no_fast;
    log_start = LOG_A;
    drc_st.log_top = LOG_A;
    drc_st.log_end = LOG_A + LOG_SIZE;
    drc_st.log_last = 0;
    drc_st.log_next = 0xFFFF;
    pend_start = 0;
}

// a VRAM byte from C (helpers, interpreter: Aladdin uploads its tiles from
// code in RAM, run by the interpreter): to the log if its page is deferred
// (joined to the last entry when it follows it); 1: kept, not in VRAM yet
uint8_t gg_platform_vram_defer(uint16_t a, uint8_t value) {
    if (!(drc_st.vram_class[a >> 8] & 8))
        return 0;
    for (;;) {
        uint8_t *top = IRAM(drc_st.log_top);
        uint8_t *last = drc_st.log_last ? IRAM(drc_st.log_last) : 0;
        if (last && last[2] < 255 && last + 3 + last[2] == top
                && (uint16_t) (last[0] | (last[1] << 8)) + last[2] == a) {
            if (drc_st.log_top + 1 <= drc_st.log_end) {
                *top = value;
                last[2]++;
                drc_st.log_top++;
                drc_st.log_next = 0xFFFF;
                return 1;
            }
        } else if (drc_st.log_top + 4 <= drc_st.log_end) {
            top[0] = a;
            top[1] = a >> 8;
            top[2] = 1;
            top[3] = value;
            drc_st.log_last = drc_st.log_top;
            drc_st.log_next = 0xFFFF;
            drc_st.log_top += 4;
            return 1;
        }
        gg_platform_vram_sync();   // full: written now, then again
    }
}
#ifdef GGSWAN_SMS
/*
 * smsswan, the whole picture on the screen (START + Y3 (Y down) switches
 * it): the 192 lines squeezed into the 144 of the WS, one line in four
 * left out. The WS has nothing to scale with: the line interrupt adds 1
 * to the Y scroll every 3 WS lines (48 more interrupts a WS screen: about
 * a quarter of the CPU), in the same list as the scroll splits, whose
 * entries then carry a Y scroll too (act_y). Sprites keep their height:
 * only their rows are scaled, so the pieces of a tall figure overlap by a
 * quarter. Sideways the view is still 224 of the 256 columns.
 */
#define MAX_EVENTS (48 + MAX_SPLITS)   // (47 Y steps, the splits)
uint8_t gg_squeeze;                                 // wanted (read_keys)
// the view's Y as set with the Y buttons (gg_view_y is this, kept inside
// the picture: 192 or 224 lines, see video_present), and whether they
// were used (if not, a picture of 224 lines starts with the view in its
// middle too)
static uint8_t view_y_want = 24, view_moved;
volatile uint8_t act_y[MAX_EVENTS], act_sq;       // (and drc_asm.s)
uint8_t spr_sq, spr_dy = 8, spr_224;                // (spr_build's, drc_asm.s)
#else
#define MAX_EVENTS MAX_SPLITS
#endif
// (also read by the line interrupt's short path, drc_asm.s)
volatile uint8_t act_line[MAX_EVENTS], act_val[MAX_EVENTS], act_n = 1;
// screen 1 Y scroll for the next WS frame (set by the line interrupt at the
// end of the screen, like act_val[0]: no tear while the WS draws)
static volatile uint8_t act_vy;
volatile uint8_t act_i;
// the splits and the Y scroll of the frame just presented, waiting for the
// end of the WS screen (pend_n: 0 none). They replaced the active list at
// once, in the middle of the WS screen being drawn, whose line interrupts
// went on with the new list from where they were in the old one: the new
// values on the old list's lines (or the rest skipped, a shorter list),
// for one WS screen: the road and the status bar of Super Monaco GP
// jerking sideways now and then, half a screen of it.
#ifndef GGSWAN_HOST
static uint8_t pend_line[MAX_EVENTS], pend_val[MAX_EVENTS];
#ifdef GGSWAN_SMS
static uint8_t pend_y[MAX_EVENTS], pend_sq;
#endif
static volatile uint8_t pend_n;
#endif
static uint8_t next_vy;
// a Game Gear frame presented (video_present) and not shown yet: the line
// interrupt at the end of the WS screen clears it
static volatile uint8_t frame_waiting;

uint16_t ws_map1_iram(void) {
    return IRAM_OFF(map1);
}
uint16_t ws_map2_iram(void) {
    return IRAM_OFF(map2);
}

// screen 2 entry (drc_st.prio_on): a background tile with priority (bit
// 12) in front of the sprites, the others empty
static inline uint16_t prio_entry(uint16_t e, uint16_t w) {
    return (e & 0x1000) ? w : WS_PRIO_BLANK;
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

#ifndef GGSWAN_HOST
_Static_assert(WS_PRIO_BLANK == 0x21FF, "map_relayout_row_asm: WS_PRIO_BLANK");

// V30MZ inner row converter for map_relayout() (32 entries, DS = 0x0000).
// Uses drc_st.nt_flags[8] like put_nt(); no "m" operands, all clobbers explicit.
static __attribute__((noinline)) void map_relayout_row_prio_asm(const uint8_t *p, uint16_t off) {
    __asm__ volatile (
        "cld\n\t"
        "movw $32, %%cx\n\t"
    "1:\n\t"
        "lodsw\n\t"
        "movb %%ah, %%dl\n\t"
        "movb %%ah, %%bl\n\t"
        "xorb %%bh, %%bh\n\t"
        "andb $0x0E, %%bl\n\t"
        "andw $0x01FF, %%ax\n\t"
        "orw %c[ntf](%%bx), %%ax\n\t"
        "movw %%ax, %c[m1](%%di)\n\t"
        "testb $0x10, %%dl\n\t"
        "jnz 2f\n\t"
        "movw $0x21FF, %%ax\n\t"
    "2:\n\t"
        "movw %%ax, %c[m2](%%di)\n\t"
        "addw $2, %%di\n\t"
        "loop 1b\n\t"
        : "+S" (p), "+D" (off)
        : [ntf] "i" (drc_st.nt_flags), [m1] "i" (map1), [m2] "i" (map2)
        : "ax", "bx", "cx", "dx", "cc", "memory"
    );
}

static __attribute__((noinline)) void map_relayout_row_plain_asm(const uint8_t *p, uint16_t off) {
    __asm__ volatile (
        "cld\n\t"
        "movw $32, %%cx\n\t"
    "1:\n\t"
        "lodsw\n\t"
        "movb %%ah, %%bl\n\t"
        "xorb %%bh, %%bh\n\t"
        "andb $0x0E, %%bl\n\t"
        "andw $0x01FF, %%ax\n\t"
        "orw %c[ntf](%%bx), %%ax\n\t"
        "movw %%ax, %c[m1](%%di)\n\t"
        "addw $2, %%di\n\t"
        "loop 1b\n\t"
        : "+S" (p), "+D" (off)
        : [ntf] "i" (drc_st.nt_flags), [m1] "i" (map1)
        : "ax", "bx", "cx", "cc", "memory"
    );
}
#endif

/*
 * The Game Gear map has 28 rows, the WS map 32. map1 holds the 32
 * consecutive Game Gear rows map_top .. map_top + 31 (wrapping at 28): WS
 * row (v & 31) shows Game Gear row (v % 28) for v in that range. When the
 * view leaves it, the map is laid out again.
 */
// (first: the WS row written first, then the ones after it: video_present)
static void map_relayout(uint8_t top, uint8_t first) {
    map_top = top;
    drc_st.map_top = top;
    // where each row goes (as map_put), for the name table routine
    for (uint8_t r = 0; r < DRC_NT_ROWS; r++)
        drc_st.nt_rows[r] = drc_st.nt_rows[DRC_NT_ROWS + r] = 0xFFFF;
    for (uint8_t r = 0; r < NT_H; r++) {
        uint8_t k = 0;
        for (uint8_t v = r; v < top + 32; v += NT_H)
            if (v >= top)
                drc_st.nt_rows[k++ * DRC_NT_ROWS + r] = (v & 31) * 64;
    }
    for (uint8_t k = 0; k < 32; k++) {
        uint8_t w = (first + k) & 31;
        uint8_t v = top + ((w - top) & 31);
        uint8_t r = v % NT_H;
        // (a name table row does not wrap in VRAM: 3800h + 27 * 64 at most)
        const uint8_t *p = gg_vram + nt_base + r * 64;
#ifndef GGSWAN_HOST
        if (drc_st.prio_on)
            map_relayout_row_prio_asm(p, (uint16_t) w << 6);
        else
            map_relayout_row_plain_asm(p, (uint16_t) w << 6);
#else
        uint16_t *m = map1 + w * 32;
        if (drc_st.prio_on) {
            uint16_t *m2 = map2 + w * 32;
            for (uint8_t c = 0; c < 32; c++, p += 2) {
                uint16_t e = p[0] | (p[1] << 8), x = conv_entry(e);
                *m++ = x;
                *m2++ = prio_entry(e, x);
            }
        } else {
            for (uint8_t c = 0; c < 32; c++, p += 2)
                *m++ = conv_entry(p[0] | (p[1] << 8));
        }
#endif
    }
}

#ifdef GGSWAN_HOST
// tests: map1 entries that differ from the Game Gear name table
uint16_t host_map_check(void) {
    uint16_t bad = 0;
    for (uint8_t w = 0; w < 32; w++) {
        uint8_t v = map_top + ((w - map_top) & 31);
        uint8_t r = v % NT_H;
        for (uint8_t c = 0; c < 32; c++)
            if (map1[w * 32 + c] != conv_entry(gg_entry(r, c))) bad++;
    }
    return bad;
}
#endif

static void map_put(uint8_t row, uint8_t col, uint16_t e) {
    uint16_t w = conv_entry(e), w2 = prio_entry(e, w);
    for (uint8_t v = row; v < map_top + 32; v += NT_H)
        if (v >= map_top) {
            map1[(v & 31) * 32 + col] = w;
            if (drc_st.prio_on)
                map2[(v & 31) * 32 + col] = w2;
        }
}

void gg_platform_vram_write(uint16_t a, uint8_t value) {
    (void) value;
    uint16_t off = a - nt_base;
    if (off < NT_SIZE) {
        map_put(off >> 6, (off >> 1) & 31, gg_entry(off >> 6, (off >> 1) & 31));
    } else if ((uint16_t) (a - sat_base) < 0x100) {
        drc_st.sprites_dirty = true;
    }
}

/*
 * The palette with the frame. A color the game writes went to the WS's
 * palettes at once: in the middle of the WS screen being drawn, which
 * shows the frame presented before, with its scroll and its tiles. Shinobi
 * moves the bars of its fence by turning four colors round (the tile is
 * four stripes of them), every other frame, in its frame interrupt: the
 * bars moved two pixels in the part of the screen the WS had not drawn
 * yet, a WS screen or two before the rest of the picture followed, broken
 * in two where the WS was then, with a line of no bars between the two
 * writes.
 *
 * So the colors go to drc_st.pal (here, and from the recompiled code:
 * put_cram in drc_rt.c, the OUTI routine of drc.c); when a frame is
 * presented those written since the last one are copied aside
 * (pal_present, with the scroll and the tile log), and the line interrupt
 * at the end of the WS screen writes them to the WS's palettes
 * (pal_commit), before the first line that shows that frame.
 */
static uint16_t pal_pend[32];
static volatile uint8_t pal_wait;

void gg_platform_cram_write(uint8_t i) {
    uint16_t c = gg_color12(i);
    // Game Gear: ----BBBBGGGGRRRR; WS: ----RRRRGGGGBBBB
    drc_st.pal[i & 31] = ((c & 0x00F) << 8) | (c & 0x0F0) | ((c >> 8) & 0x00F);
    drc_st.pal_dirty = 1;
}

// the colors waiting to the WS: background palettes 0 and 1, and the
// second again as palette 8 for the sprites (also from the line interrupt)
static void pal_commit(void) {
    if (!pal_wait)
        return;
    memcpy(WS_DISPLAY_COLOR_MEM(0), pal_pend, 64);
    memcpy(WS_DISPLAY_COLOR_MEM(8), pal_pend + 16, 32);
    pal_wait = 0;
}

#ifdef GGSWAN_HOST
// tests: colors of drc_st.pal that are not the Game Gear's, or that the WS
// did not get with the last frame presented (drcsys.py --pal: the
// recompiled code writes drc_st.pal too)
uint16_t host_pal_check(void) {
    uint16_t bad = 0;
    for (uint8_t i = 0; i < 32; i++) {
        uint16_t c = gg_color12(i);
        if (drc_st.pal[i] != (uint16_t) (((c & 0x00F) << 8) | (c & 0x0F0) | ((c >> 8) & 0x00F)))
            bad++;
        if (!drc_st.pal_dirty && WS_DISPLAY_COLOR_MEM(i >> 4)[i & 15] != drc_st.pal[i])
            bad++;
        if (!drc_st.pal_dirty && i >= 16 && WS_DISPLAY_COLOR_MEM(8)[i - 16] != drc_st.pal[i])
            bad++;
    }
    return bad;
}
#endif

// a frame presented (interrupts off on the device): its colors wait for
// the end of the WS screen
static void pal_present(void) {
    if (!drc_st.pal_dirty)
        return;
    drc_st.pal_dirty = 0;
    memcpy(pal_pend, drc_st.pal, sizeof(pal_pend));
    pal_wait = 1;
#ifdef GGSWAN_HOST
    pal_commit();   // (no WS raster on the PC)
#endif
}

void gg_platform_vdp_reg(uint8_t r, uint8_t v) {
    switch (r) {
    case 2:
        // (tile bytes still in the logs may be in the new name table: to
        // VRAM first, so that map_relayout reads them; the log copy does
        // not update the map)
#ifdef GGSWAN_SMS
        // (224 lines: 32 rows at x700h; a change of mode comes here too)
        if (nt_h != (gg.vdp.active == 224 ? 32 : 28)) {
            gg_platform_vram_sync();
            nt_h = gg.vdp.active == 224 ? 32 : 28;
            map_top = 0;
        }
#endif
        if (NT_BASE_OF(v) != nt_base)
            gg_platform_vram_sync();
        nt_base = NT_BASE_OF(v);
        drc_st.nt_base = nt_base;
        vram_class_update();
        map_relayout(map_top, 0);
        break;
    case 5:
        if ((uint16_t) ((v & 0x7E) << 7) != sat_base)
            gg_platform_vram_sync();
        sat_base = (v & 0x7E) << 7;
        drc_st.sat_base = sat_base;
        vram_class_update();
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

/*
 * More splits than fit: they are thinned out, the ones that change the
 * scroll least first (their lines take the value of the split before), so
 * that what comes after is still recorded. (Dropping the splits after the
 * last that fits had Super Monaco GP II's status bar, below a road of about 70
 * lines each with its value, scrolling with the road's last value;
 * dropping every other one lost too much of Super Monaco GP's road.)
 *
 * The frame has a threshold t, the least difference between two splits in
 * a row when the list first fills: a pass drops up to a quarter of the
 * list, each split within t pixels of the one kept before it, so that its
 * lines are at most t pixels off and the next few splits find room without
 * coming here. The splits up to thin_f have been through it: the next
 * pass looks at the ones added since, and only when none of them goes is
 * the whole list scanned again for a new (larger) t. Dropping them one at
 * a time, the closest pair of the whole list each time, cost a scan of
 * the list for every split past the limit: Super Monaco GP's road in a
 * bend has one on most lines, 30 calls a frame and 18% of its time (4% in
 * Super Monaco GP II).
 */
static uint8_t thin_t, thin_f;   // (thin_f = 0: no threshold yet this frame)

static void splits_thin(void) {
    uint8_t n = rec_n;
    uint8_t f = thin_f, t = thin_t;
    if (f > n)
        f = 0;
    for (uint8_t round = 2; round; round--) {
        if (!f) {
            const uint8_t *p = rec_val;
            uint8_t prev = *p++;
            t = 0xFF;
            for (uint8_t i = n - 1; i; i--) {
                uint8_t cur = *p++;
                uint8_t d = cur - prev;
                if ((int8_t) d < 0)
                    d = -d;
                if (d < t) {
                    t = d;
                    // (two in a row differ: 1 is the least there is)
                    if (d <= 1)
                        break;
                }
                prev = cur;
            }
            f = 1;
        }
        const uint8_t *vi = rec_val + f, *li = rec_line + f;
        uint8_t *vo = rec_val + f, *lo = rec_line + f;
        uint8_t last = vi[-1], left = MAX_SPLITS / 4;
        uint8_t i = n - f;
        for (; i; i--) {
            uint8_t v = *vi++, l = *li++;
            uint8_t d = v - last;
            if ((int8_t) d < 0)
                d = -d;
            // (0: the value of the one kept before, no split there)
            if (!d)
                continue;
            if (d <= t) {
                if (--left)
                    continue;
                i--;
                break;
            }
            *vo++ = v;
            *lo++ = l;
            last = v;
        }
        f = (uint8_t) (vo - rec_val);
        // (a quarter gone: the rest as it is, for the next pass)
        for (; i; i--) {
            *vo++ = *vi++;
            *lo++ = *li++;
        }
        uint8_t m = (uint8_t) (vo - rec_val);
        if (m < n) {
            rec_n = m;
            thin_f = f;
            thin_t = t;
            return;
        }
        f = 0;   // none of the new ones within t: a new t
    }
    // (not reached: a scan of the whole list always finds one to drop)
    rec_n = MAX_SPLITS - 1;
    thin_f = 0;
}

static void hscroll_write(uint8_t value) {
    uint8_t v = 16 - value;
    // the same value as the last split: nothing changes (and the line, which
    // takes some work to find, is not needed)
    if (v == rec_val[rec_n - 1])
        return;
    uint16_t line = gg_current_line() + 1;
    if (line <= GG_REC_Y) {
        rec_val[0] = v;   // before the view: the value for its first line
        rec_n = 1;
        thin_f = 0;
        drc_st.hs_last = value;
        return;
    }
    if (line > GG_PRESENT_LINE) {
        // after the view (the frame was presented at line 168): the value
        // for the next frame's first line
        rec_val[0] = v;
        rec_n = 1;
        thin_f = 0;
        drc_st.hs_last = value;
        return;
    }
    uint16_t y = line - GG_REC_Y;
    if (y >= GG_REC_H || v == rec_val[rec_n - 1])
        return;
    // (diagnostics, as the generated routine)
    drc_st.diag_fg = (uint16_t) gg.vdp.frame_given;
    drc_st.diag_left = gg_cpu_left ? (int16_t) gg_cpu_left() : (int16_t) z80.cycles;
    if (rec_n > 1 && rec_line[rec_n - 1] == y) {
        rec_val[rec_n - 1] = v;
    } else {
        if (rec_n >= MAX_SPLITS)
            splits_thin();
        if (v != rec_val[rec_n - 1]) {   // (equal: the last one went)
            rec_line[rec_n] = y;
            rec_val[rec_n] = v;
            rec_n++;
        }
    }
    drc_st.hs_last = value;
}

// diagnostics (X4 screen, right column): the end-of-screen interrupt's
// worst lateness and VRAM-write time in lines, and how many per second
#define WS_LINES 159
static volatile uint8_t diag_late, diag_commit, diag_ends, diag_splits, diag_split_line;
volatile uint16_t diag_calls;   // (drc_asm.s too)
static uint16_t diag_regs, diag_ram;   // (at the last frame with a split)
static uint16_t diag_irq_line, diag_vbl_pc, diag_fg;
static int16_t diag_left;
static uint8_t diag_vbl_st;
static bool diag_mode;   // Y4 (Y left) held at the start: the right column
// diagnostics: WS lines spent per second in parts of video_present (PSG
// queue, map relayout, splits and tile log, backdrop guess, sprites, all)
enum { VP_PSG, VP_MAP, VP_LOG, VP_BACK, VP_SPR, VP_ALL, VP_N };
static uint16_t vp_lines[VP_N];
static uint8_t vp_mark;
static inline uint8_t vp_lap(uint8_t part) {
    uint8_t l = inportb(WS_DISPLAY_LINE_PORT);
    uint8_t d = l >= vp_mark ? l - vp_mark : l + WS_LINES - vp_mark;
    vp_lines[part] += d;
    vp_mark = l;
    return d;
}
extern uint16_t drc_diag_irq_line, gg_diag_vbl_pc;
extern uint8_t gg_diag_vbl_st;

#ifndef GGSWAN_HOST
/*
 * The line interrupt (line_irq_entry, drc_asm.s). A split inside the
 * picture is served there, in a few instructions: the scroll value, the
 * line of the next one. Everything else comes here (line_irq_slow, with
 * every register saved): the end of the screen, and a line that had
 * already started when its interrupt was set. With one split a line (the
 * road of Super Monaco GP) a WS line is about 256 cycles and this function
 * most of them: 24 splits took an eighth of the CPU, and no more than 24
 * could be afforded.
 */
_Static_assert(offsetof(drc_state_t, prio_on) == 506, "drc_asm.s: DRC_ST_PRIO_ON");
_Static_assert(GG_VIEW_H == 144, "drc_asm.s: LI_VIEW_H");
void line_irq_entry(void);
void line_irq(void) {
    uint8_t i = act_i;
    uint8_t end = 0;   // (the end of the screen done in this interrupt)
    uint8_t l0 = inportb(WS_DISPLAY_LINE_PORT);
    diag_calls++;
    // (i = 0: the short path found the line below the picture already)
    if (inportb(WS_DISPLAY_LINE_IRQ_PORT) >= GG_VIEW_H || !i) {
        i = 0;   // end of the screen
        // (diagnostics: how many lines after line 144 it is served)
        uint8_t d = l0 >= GG_VIEW_H ? l0 - GG_VIEW_H : l0 + (WS_LINES - GG_VIEW_H);
        if (d > diag_late)
            diag_late = d;
        diag_ends++;
    }
    for (;;) {
        if (i == 0) {
            // the list of the frame presented, if one waits
            if (pend_n) {
                for (uint8_t k = 0; k < pend_n; k++) {
                    act_line[k] = pend_line[k];
                    act_val[k] = pend_val[k];
#ifdef GGSWAN_SMS
                    act_y[k] = pend_y[k];
#endif
                }
#ifdef GGSWAN_SMS
                act_sq = pend_sq;
#endif
                act_n = pend_n;
                act_vy = next_vy;
                pend_n = 0;
            }
            // values for the first line of the next screen, first (the
            // scroll must not wait for the VRAM writes); then the VRAM
            // writes of the frame presented
            outportb(WS_SCR1_SCRL_X_PORT, act_val[0]);
            outportb(0x11, act_vy);                   // screen 1 Y scroll
            if (drc_st.prio_on) {                     // screen 2 with it
                outportb(0x12, act_val[0]);
                outportb(0x13, act_vy);
            }
            pal_commit();   // (the frame's colors: 48 words at most)
            if (pend_start) {
                uint8_t c0 = inportb(WS_DISPLAY_LINE_PORT);
                log_commit_pending();
                uint8_t c1 = inportb(WS_DISPLAY_LINE_PORT);
                // (diagnostics: lines the VRAM writes took)
                uint8_t dc = c1 >= c0 ? c1 - c0 : c1 + WS_LINES - c0;
                if (dc > diag_commit)
                    diag_commit = dc;
            }
            frame_waiting = 0;
            end = 1;
            i = 1;
        } else {
            outportb(WS_SCR1_SCRL_X_PORT, act_val[i]);
            if (drc_st.prio_on)
                outportb(0x12, act_val[i]);
#ifdef GGSWAN_SMS
            if (act_sq) {
                outportb(0x11, act_y[i]);
                if (drc_st.prio_on)
                    outportb(0x13, act_y[i]);
            }
#endif
            i++;
        }
        // interrupt on the line before the next split (or the end)
        uint8_t next = i < act_n ? act_line[i] - 1 : GG_VIEW_H;
        if (next > GG_VIEW_H)
            next = GG_VIEW_H;
        uint8_t cur = inportb(WS_DISPLAY_LINE_PORT);   // (the line being drawn)
        if (cur >= GG_VIEW_H) {
            // below the picture: after the end of the screen, every line
            // of the next one is still to come; before it (splits late
            // into the bottom border), the end of the screen now
            if (end) {
                act_i = i;
                outportb(WS_DISPLAY_LINE_IRQ_PORT, next);
                ws_int_ack(WS_INT_ACK_LINE_MATCH);
                // (see below; here only after VRAM writes long enough to
                // reach that line of the next screen)
                uint8_t now = inportb(WS_DISPLAY_LINE_PORT);
                if (now < GG_VIEW_H && now >= next)
                    continue;
                break;
            }
            i = 0;
            continue;
        }
        if (next > cur) {
            act_i = i;
            outportb(WS_DISPLAY_LINE_IRQ_PORT, next);
            /*
             * The line may have started between reading it and setting
             * the match (this interrupt served late: interrupts off in
             * the main code, a long REP there): no match on it then until
             * the next WS screen, so no more splits and no end of the
             * screen in this one, the rest of it and the top of the next
             * with the last scroll value written: half a screen jerking
             * sideways for an instant (Super Monaco GP and others). And
             * a match that came before the acknowledge at the end of the
             * handler was lost with it, the same way. So: acknowledge
             * right after setting the match, then look at the line again;
             * if it is there already, that split (or the end of the
             * screen) now. A match still pending from a line handled this
             * way is cleared by the acknowledge after the next one set.
             */
            ws_int_ack(WS_INT_ACK_LINE_MATCH);
            uint8_t now = inportb(WS_DISPLAY_LINE_PORT);
            if (now >= next) {
                if (next >= GG_VIEW_H)
                    i = 0;
                continue;
            }
            break;
        }
        // late (consecutive splits, Super Monaco GP's road: one a line, and
        // an interrupt a line late; or VRAM writes past the bottom border):
        // that line is already being drawn, a match on it would come only
        // in the next WS frame. Its value now.
    }
}
#endif

#ifdef GGSWAN_SMS
// smsswan: the splits were recorded over the whole picture (lines 0-191,
// with the view in the middle, X 16); here for the view where it is now:
// those above it give its first line's value, those below it are dropped
static uint8_t cv_line[MAX_EVENTS], cv_val[MAX_EVENTS], cv_y[MAX_EVENTS], cv_n, cv_sq;
static uint8_t view_sq;   // (the frame being presented: video_present)
static void splits_convert(void) {
    uint8_t dx = gg_view_x - 16, vy = gg_view_y, n = 1;
    // the list over the picture's lines: as recorded, or, with register 0
    // bit 6 (the first 16 lines not scrolled sideways: Wonder Boy III's
    // status bar at the top), scroll 0 from line 0 and the value that
    // holds at line 16 from there
    static uint8_t pl_line[MAX_SPLITS + 1], pl_val[MAX_SPLITS + 1];
    if (!view_sq && !(gg.vdp.reg[0] & 0x40)) {
        cv_line[0] = 0;
        cv_val[0] = rec_val[0] + dx;
        cv_sq = 0;
        for (uint8_t i = 1; i < rec_n; i++) {
            uint8_t y = rec_line[i];
            if (y <= vy) {
                cv_val[0] = rec_val[i] + dx;
            } else if ((uint8_t) (y - vy) < GG_VIEW_H) {
                cv_line[n] = y - vy;
                cv_val[n] = rec_val[i] + dx;
                n++;
            }
        }
        cv_n = n;
        return;
    }
    uint8_t pn = 1;
    pl_val[0] = rec_val[0];
    if (gg.vdp.reg[0] & 0x40) {
        uint8_t i = 1, cur = rec_val[0];
        while (i < rec_n && rec_line[i] <= 16)
            cur = rec_val[i++];
        pl_val[0] = 16;           // (16 - 0)
        pl_line[1] = 16;
        pl_val[1] = cur;
        pn = 2;
        for (; i < rec_n; i++) {
            pl_line[pn] = rec_line[i];
            pl_val[pn++] = rec_val[i];
        }
    } else {
        for (uint8_t i = 1; i < rec_n; i++) {
            pl_line[pn] = rec_line[i];
            pl_val[pn++] = rec_val[i];
        }
    }
    cv_line[0] = 0;
    cv_val[0] = pl_val[0] + dx;
    cv_sq = view_sq;
    if (view_sq) {
        // WS line L shows picture line L + L / 3: a split at picture line
        // p is at WS line ceil(3p / 4); the Y scroll one more every 3 lines
        uint8_t x = cv_val[0], j = 1;
        for (uint8_t l = 1; l < GG_VIEW_H; l++) {
            uint8_t ev = (l % 3) == 0;
            while (j < pn && (uint8_t) ((pl_line[j] * 3u + 3) >> 2) <= l) {
                x = pl_val[j++] + dx;
                ev = 1;
            }
            if (ev && n < MAX_EVENTS) {
                cv_line[n] = l;
                cv_val[n] = x;
                cv_y[n] = next_vy + l / 3;
                n++;
            }
        }
        cv_n = n;
        return;
    }
    for (uint8_t i = 1; i < pn; i++) {
        uint8_t y = pl_line[i];
        if (y <= vy) {
            cv_val[0] = pl_val[i] + dx;
        } else if ((uint8_t) (y - vy) < GG_VIEW_H) {
            cv_line[n] = y - vy;
            cv_val[n] = pl_val[i] + dx;
            n++;
        }
    }
    cv_n = n;
}
#define SPL_LINE cv_line
#define SPL_VAL cv_val
#define SPL_N cv_n
#else
#define SPL_LINE rec_line
#define SPL_VAL rec_val
#define SPL_N rec_n
#endif

// the frame's list written where the line interrupt will take it (the
// waiting list: the interrupt does not look at it while pend_n is 0, and
// video_present has waited for the last one to be taken), outside the
// stretch with interrupts off: with smsswan's squeezed picture it has up
// to 72 entries
static uint8_t prep_n;
static void splits_prepare(void) {
#ifdef GGSWAN_SMS
    splits_convert();
#endif
#ifndef GGSWAN_HOST
#ifdef GGSWAN_SMS
    if (cv_sq) {
        for (uint8_t i = 0; i < SPL_N; i++) {
            pend_line[i] = SPL_LINE[i];
            pend_val[i] = SPL_VAL[i];
            pend_y[i] = cv_y[i];
        }
    } else
#endif
    for (uint8_t i = 0; i < SPL_N; i++) {
        pend_line[i] = SPL_LINE[i];
        pend_val[i] = SPL_VAL[i];
    }
#endif
    prep_n = SPL_N;
    if (rec_n > diag_splits)
        diag_splits = rec_n;
    if (diag_mode && rec_n > 1) {
        diag_split_line = rec_line[1];
        diag_regs = (gg.vdp.reg[0] << 8) | gg.vdp.reg[10];
        // (Sonic: D2DC the line-interrupt line, D241 its state)
        diag_ram = (gg_ram[0x12DC] << 8) | gg_ram[0x1241];
        diag_irq_line = drc_diag_irq_line;
        diag_vbl_pc = gg_diag_vbl_pc;
        diag_vbl_st = gg_diag_vbl_st;
        diag_fg = drc_st.diag_fg;
        diag_left = drc_st.diag_left;
    }
}

// (video_present calls it with interrupts off)
static void splits_commit(void) {
#ifndef GGSWAN_HOST
    // (to the line interrupt at the end of the WS screen)
#ifdef GGSWAN_SMS
    pend_sq = cv_sq;
#endif
    pend_n = prep_n;
#else
    for (uint8_t i = 0; i < prep_n; i++) {
        act_line[i] = SPL_LINE[i];
        act_val[i] = SPL_VAL[i];
#ifdef GGSWAN_SMS
        act_y[i] = cv_y[i];
#endif
    }
#ifdef GGSWAN_SMS
    act_sq = cv_sq;
#endif
    act_n = prep_n;
    act_vy = next_vy;
#endif
}

/*
 * WS screens show color 0 as transparent (the backdrop); on the Game Gear it
 * is color 0 of the tile's palette. Use as backdrop color 0 of the palette
 * most of the visible tiles that have that color use: a tile without it
 * shows no backdrop, whatever its palette. (Shinobi's map screen is black
 * tiles of the sprite palette, none of them with color 0, and a few blank
 * ones of the other palette: with every tile counted the backdrop was the
 * sprite palette's color 0, brown, a bar of it at the bottom of the screen.)
 * Color 0 is looked for in two of the tile's eight rows (as it is in VRAM:
 * deferred writes not yet there are not seen, until the next time).
 */
#ifndef GGSWAN_HOST
_Static_assert(GG_VIEW_H == 144 && (GG_VIEW_W == 160 || GG_VIEW_W == 224),
               "update_backdrop_asm: view dimensions");

// V30MZ inner loops for update_backdrop() (9 rows x 10/14 cols, DS = 0x0000).
// No "m" operands; all modified registers in outputs or clobbers; ES/BP untouched.
static __attribute__((noinline)) void update_backdrop_asm(uint16_t top16, uint16_t left_off) {
    __asm__ volatile (
        /* In: AX = top (0..NT_H-1), DX = left_off ((left & 31) * 2, 0..62) */
        "pushw %%ax\n\t"                      /* [sp+2] = top for Pass 2 */
        "pushw %%dx\n\t"                      /* [sp+0] = left_off for Pass 2 */
        "movw %%dx, %%di\n\t"                 /* DI = left_off (constant in Pass 1) */
        "xorb %%dl, %%dl\n\t"                 /* DL = n1 = 0 */
        "movb $9, %%dh\n\t"                   /* DH = 9 rows */
    "1:\n\t"
        "movb %%al, %%cl\n\t"
        "xorb %%ch, %%ch\n\t"
        "shlw $6, %%cx\n\t"                   /* CX = row * 64 */
        "movw %c[ntb], %%si\n\t"
        "addw $0x4000, %%si\n\t"
        "addw %%cx, %%si\n\t"                 /* SI = 0x4000 + nt_base + row * 64 */
        "movw %%di, %%bx\n\t"                 /* BX = left_off */
        "movw $%c[cols], %%cx\n\t"
    "2:\n\t"
        "testb $0x08, 1(%%bx,%%si)\n\t"       /* (e >> 11) & 1 */
        "jz 3f\n\t"
        "incb %%dl\n\t"                       /* n1++ */
    "3:\n\t"
        "addw $4, %%bx\n\t"
        "andw $0x003E, %%bx\n\t"
        "loop 2b\n\t"
        "addb $2, %%al\n\t"                   /* row += 2 */
#ifdef GGSWAN_SMS
        "cmpb %c[nth], %%al\n\t"
        "jb 4f\n\t"
        "subb %c[nth], %%al\n\t"
#else
        "cmpb $28, %%al\n\t"
        "jb 4f\n\t"
        "subb $28, %%al\n\t"
#endif
    "4:\n\t"
        "decb %%dh\n\t"
        "jnz 1b\n\t"

        "testb %%dl, %%dl\n\t"
        "jz 10f\n\t"                          /* all pal 0 */
        "cmpb $%c[total], %%dl\n\t"
        "jne 20f\n\t"                         /* mixed palettes -> Pass 2 */
        "addw $4, %%sp\n\t"
        "movb $1, %c[bpal]\n\t"
        "jmp 99f\n\t"
    "10:\n\t"
        "addw $4, %%sp\n\t"
        "movb $0, %c[bpal]\n\t"
        "jmp 99f\n\t"

        /* Pass 2: count tiles that contain color 0 in row 1 (t[4..7]) or row 5 (t[20..23]) */
    "20:\n\t"
        "popw %%bx\n\t"                       /* BX = left_off */
        "popw %%ax\n\t"                       /* AL = top */
        "xorw %%dx, %%dx\n\t"                 /* DL = n[0] = 0, DH = n[1] = 0 */
        "movb $9, %%ah\n\t"                   /* AH = 9 rows */
    "12:\n\t"
        "pushw %%ax\n\t"                      /* save row (AL) and rows_left (AH) */
        "pushw %%bx\n\t"                      /* save left_off */
        "xorb %%ah, %%ah\n\t"
        "shlw $6, %%ax\n\t"                   /* AX = row * 64 */
        "addw %c[ntb], %%ax\n\t"
        "addw $0x4000, %%ax\n\t"
        "movw %%ax, %%si\n\t"                 /* SI = 0x4000 + nt_base + row * 64 */
        "movb $%c[cols], %%cl\n\t"
    "13:\n\t"
        "movw (%%bx,%%si), %%di\n\t"          /* DI = e */
        "movw %%di, %%ax\n\t"
        "movb %%ah, %%ch\n\t"                 /* CH = e >> 8 (bit 3 = pal) */
        "andw $0x01FF, %%di\n\t"
        "shlw $5, %%di\n\t"
        "addw $0x4000, %%di\n\t"              /* DI = gg_vram + (e & 0x1FF) * 32 */
        "movw 4(%%di), %%ax\n\t"
        "orw 6(%%di), %%ax\n\t"
        "orb %%ah, %%al\n\t"
        "incb %%al\n\t"
        "jnz 14f\n\t"                         /* row 1 has color 0 */
        "movw 20(%%di), %%ax\n\t"
        "orw 22(%%di), %%ax\n\t"
        "orb %%ah, %%al\n\t"
        "incb %%al\n\t"
        "jz 16f\n\t"                          /* neither row has color 0 */
    "14:\n\t"
        "testb $0x08, %%ch\n\t"
        "jnz 15f\n\t"
        "incb %%dl\n\t"                       /* n[0]++ */
        "jmp 16f\n\t"
    "15:\n\t"
        "incb %%dh\n\t"                       /* n[1]++ */
    "16:\n\t"
        "addw $4, %%bx\n\t"
        "andw $0x003E, %%bx\n\t"
        "decb %%cl\n\t"
        "jnz 13b\n\t"

        "popw %%bx\n\t"                       /* restore left_off */
        "popw %%ax\n\t"                       /* restore row (AL), rows_left (AH) */
        "addb $2, %%al\n\t"
#ifdef GGSWAN_SMS
        "cmpb %c[nth], %%al\n\t"
        "jb 17f\n\t"
        "subb %c[nth], %%al\n\t"
#else
        "cmpb $28, %%al\n\t"
        "jb 17f\n\t"
        "subb $28, %%al\n\t"
#endif
    "17:\n\t"
        "decb %%ah\n\t"
        "jnz 12b\n\t"

        "cmpb %%dl, %%dh\n\t"                 /* n[1] vs n[0] */
        "ja 18f\n\t"
        "jb 19f\n\t"
        "jmp 99f\n\t"
    "18:\n\t"
        "movb $1, %c[bpal]\n\t"
        "jmp 99f\n\t"
    "19:\n\t"
        "movb $0, %c[bpal]\n\t"
    "99:\n\t"
        : "+a" (top16), "+d" (left_off)
        : [ntb] "i" (&nt_base),
          [bpal] "i" (&backdrop_palette),
#ifdef GGSWAN_SMS
          [nth] "i" (&nt_h),
#endif
          [cols] "i" (GG_VIEW_W / 16),
          [total] "i" (9 * (GG_VIEW_W / 16))
        : "bx", "cx", "si", "di", "cc", "memory"
    );
}
#endif

static void update_backdrop(void) {
    uint16_t s = GG_VIEW_Y + gg.vdp.vscroll_frame;
    if (s >= NT_P) s -= NT_P;
    uint8_t top = (uint8_t) (s >> 3);
    uint8_t left = (GG_VIEW_X - gg.vdp.reg[8]) >> 3;
#ifndef GGSWAN_HOST
    update_backdrop_asm(top, (uint16_t) (left & 31) << 1);
    return;
#endif
    uint8_t n[2] = { 0, 0 };
    // (every other tile of the view: 18 rows, 20 columns; smsswan 28)
    // First their palettes alone: all of one (most screens), that one,
    // and the tiles need not be looked at
    for (uint8_t r = 0; r < GG_VIEW_H / 8; r += 2) {
        uint8_t row = (top + r) % NT_H;
        for (uint8_t c = 0; c < GG_VIEW_W / 8; c += 2)
            n[(gg_entry(row, (left + c) & 31) >> 11) & 1]++;
    }
    if (!n[0] || !n[1]) {
        backdrop_palette = n[1] != 0;
        return;
    }
    n[0] = n[1] = 0;
    for (uint8_t r = 0; r < GG_VIEW_H / 8; r += 2) {
        uint8_t row = (top + r) % NT_H;
        for (uint8_t c = 0; c < GG_VIEW_W / 8; c += 2) {
            uint16_t e = gg_entry(row, (left + c) & 31);
            const uint8_t *t = gg_vram + (e & 0x1FF) * 32;
            // (a pixel is color 0 where none of its row's four planes has it)
            if ((uint8_t) ~(t[4] | t[5] | t[6] | t[7])
                    || (uint8_t) ~(t[20] | t[21] | t[22] | t[23]))
                n[(e >> 11) & 1]++;
        }
    }
    // (as many of each, or none: as it was)
    if (n[1] > n[0])
        backdrop_palette = 1;
    else if (n[0] > n[1])
        backdrop_palette = 0;
}

static uint8_t spr_back = 1;   // the table to build next: 1 sprite_table_b
#ifdef GGSWAN_HOST
uint32_t host_spr_builds, host_relayouts, host_backdrops;
#endif
#ifdef GGSWAN_NILEDBG
#include "niledbg.h"
NILEDBG_API_TABLE;   // (filled in by niledebug's injector)
#endif

#ifndef GGSWAN_HOST
uint8_t spr_par[4], spr_cnt;   // (spr_build's; smsswan: gg_view_y too)
uint8_t *spr_build(const uint8_t *sat, uint8_t *table);
#endif
static uint8_t spr_port04, spr_count, spr_ready;
#ifdef GGSWAN_SMS
#define TALL_DY spr_dy
#else
#define TALL_DY 8
#endif
static void build_sprites(void) {
#ifdef GGSWAN_HOST
    host_spr_builds++;
#endif
    const uint8_t *reg = gg.vdp.reg;
    uint16_t tbase = (reg[6] & 0x04) ? 0x100 : 0;
    bool tall = reg[1] & 0x02;
    uint8_t xoff = (reg[0] & 0x08) ? 8 : 0;
    // hidden: a sprite outside the 160x144 window (WS 32-191 x 0-143, the
    // borders are screen 2, in front of the sprites; smsswan: 224x144, the
    // whole WS screen) is not given to the WS; games park the unused ones
    // out of the picture (Shinobi: most of its 64)
    uint8_t hide_y = 113 - (tall ? 16 : 8);
    const uint8_t *sat = gg_vram + sat_base;
    // two tables: this frame's sprites go to the one the WS is not using,
    // then the WS is pointed at it (port 04h). Built in place, the table
    // could be copied by the WS (line 144) half old, half new: Aladdin's
    // pieces of trousers away from his legs, for a frame now and then.
    uint8_t *table = spr_back ? sprite_table_b : sprite_table;
#ifndef GGSWAN_HOST
    // (the loop below in assembly: drc_asm.s)
    spr_par[0] = tbase >> 8;
    spr_par[1] = tall;
    spr_par[2] = xoff + (GG_VIEW_X - WS_VIEW_ORIGIN_X);
    spr_par[3] = hide_y;
#ifdef GGSWAN_SMS
    spr_sq = view_sq;
    spr_dy = view_sq ? 6 : 8;
    spr_224 = gg.vdp.active == 224;
#endif
    uint8_t *s = spr_build(sat, table);
#else
    uint8_t *s = table, *end = table + 127 * 4;
    for (uint8_t i = 0; i < 64 && s < end; i++) {
        uint8_t y = sat[i];
        if (GG_SPR_END(y))
            break;
        // Game Gear (x, y + 1) in the 256x192 picture; the view starts at
        // (48, 24) and is shown at WS (32, 0) (smsswan: (16, 24) at (0, 0))
        uint8_t wy = y + 1 - GG_VIEW_Y;
#ifdef GGSWAN_SMS
        if (view_sq) {
            // (the row scaled as the picture; none above its first line)
            wy = y + 1;
            if (wy >= GG_ACTIVE_LINES)
                continue;
            wy -= wy >> 2;
        }
#endif
        if ((uint8_t) (wy - GG_VIEW_H) < hide_y)
            continue;
        uint8_t wx = sat[0x80 + i * 2] - xoff - (GG_VIEW_X - WS_VIEW_ORIGIN_X);
        // (hidden: from the column after the view to 8 before its start)
        if ((uint8_t) (wx - (WS_VIEW_ORIGIN_X + GG_VIEW_W)) < 256 - GG_VIEW_W - 7)
            continue;
        uint8_t t = sat[0x81 + i * 2];
        // tile (bit 8: s[1] bit 0); palette 0 (= WS palette 8); behind screen 2
        if (tall) {
            s[0] = t & ~1; s[1] = tbase >> 8; s[2] = wy; s[3] = wx;
            s[4] = t | 1; s[5] = tbase >> 8; s[6] = wy + TALL_DY; s[7] = wx;
            s += 8;
        } else {
            s[0] = t; s[1] = tbase >> 8; s[2] = wy; s[3] = wx;
            s += 4;
        }
    }
#endif
    // (given to the WS by sprites_publish, with the frame's tile log)
    spr_port04 = (IRAM_OFF(table) >> 9) & 0x3F;
    spr_count = (uint8_t) ((s - table) >> 2);
    spr_ready = 1;
    spr_back ^= 1;
    drc_st.sprites_dirty = false;
}

#ifndef GGSWAN_SG1000   // (sgswan: sg_vdp.c has its own)
/*
 * The sprite collision flag (VDP status bit 5: two sprites with a pixel
 * of each on the same spot; Fantastic Dizzy, Taz-Mania, Ecco, Impossible
 * Mission, Master of Darkness, Cheese Cat-Astrophe use it for their own
 * collisions). Once a frame, from the sprite table as it is then: the
 * sprites on the picture go into lists by band of 16 lines, each one is
 * compared with those of its band and the two next to it (rectangles
 * first), and the pairs that overlap by the rows of their tiles: a row's
 * 8 pixels are the OR of its 4 bitplane bytes. Not the limit of 8
 * sprites a line (more are compared than the Master System shows), nor
 * sprites partly above the first line.
 */
static inline uint8_t spr_row_mask(uint16_t tile, uint8_t row) {
    const uint8_t *p = gg_vram + tile * 32u + row * 4u;
    return p[0] | p[1] | p[2] | p[3];
}
/*
 * In two parts since patch 152. coll_scan walks the table and looks for
 * the pairs whose rectangles overlap: that is where the time went (Batman
 * Returns: 33-42 sprites, about 100 pairs looked at a frame, 2 that
 * overlap), so on the device it is in assembly (drc_asm.s); the PC tests
 * have it in C here. coll_pair is called for an overlapping pair and
 * compares its rows.
 */
uint8_t coll_head[16], coll_nxt[64], coll_sy[64], coll_sx[64];
uint8_t coll_h1, coll_hh, coll_lines, coll_noend;   // h - 1, 2 h - 2, the lines, no D0h end
// the tile (as the table has it) of the last sprite found with no pixel,
// if any yet: coll_scan passes over the sprites with that one at once
uint8_t coll_emp_raw, coll_emp_has;
// (a sprite with no pixel hits nothing: 1 here once that is found, 2 if
// it has one. A table never written is 64 empty sprites in one place,
// 2016 pairs with all their rows to compare: 1.4 million cycles a frame
// in Batman Returns before its first screen)
static uint8_t emp[64];
static const uint8_t *coll_sat;
static uint16_t coll_tb, emp_tile;
static uint8_t coll_h, emp_is;
#ifdef GGSWAN_HOST
static unsigned long st_calls, st_spr, st_rect, st_pairs, st_rows;
#define ST(x) (x)
#else
#define ST(x) ((void) 0)
#endif

// sprites i and j (j earlier in the table) with rectangles that overlap:
// 1 two of their pixels do, 2 sprite i has no pixel (none of its pairs
// need looking at), 0 neither
uint8_t coll_pair(uint8_t i, uint8_t j) {
    const uint8_t *sat = coll_sat;
    uint8_t h = coll_h;
    int8_t oy = (int8_t) ((uint8_t) (sat[i] + 1) - coll_sy[j]);
    int8_t ox = (int8_t) (sat[0x80 + i * 2] - coll_sx[j]);
    uint16_t ti = sat[0x81 + i * 2] | coll_tb, tj = sat[0x81 + j * 2] | coll_tb;
    if (h == 16) {
        ti &= ~1;
        tj &= ~1;
    }
    if (!emp[i]) {
        // (the last tile looked at is remembered: the empty sprites of a
        // table are mostly one tile)
        if (ti != emp_tile) {
            uint8_t m = 0;
            for (uint8_t r = 0; r < h && !m; r++)
                m = spr_row_mask((ti + (r >> 3)) & 0x1FF, r & 7);
            emp_tile = ti;
            emp_is = m ? 2 : 1;
        }
        emp[i] = emp_is;
    }
    if (emp[i] == 1) {
        coll_emp_raw = sat[0x81 + i * 2];
        coll_emp_has = 1;
        return 2;
    }
    if (emp[j] == 1)
        return 0;
    // (rows: i's from ri, j's from rj, n of them)
    uint8_t ri = oy < 0 ? -oy : 0, rj = oy > 0 ? oy : 0;
    uint8_t n = h - (oy < 0 ? -oy : oy);
    ST(st_pairs++);
    for (; n; n--, ri++, rj++) {
        ST(st_rows++);
        uint8_t mi = spr_row_mask((ti + (ri >> 3)) & 0x1FF, ri & 7);
        uint8_t mj = spr_row_mask((tj + (rj >> 3)) & 0x1FF, rj & 7);
        if (ox >= 0 ? (mi & (uint8_t) (mj << ox)) : (mj & (uint8_t) (mi << -ox)))
            return 1;
    }
    return 0;
}

#ifdef GGSWAN_HOST
// (the device's is drc_asm.s: the same, step by step)
static uint8_t coll_scan(const uint8_t *sat) {
    for (uint8_t i = 0; i < 64; i++) {
        uint8_t y = sat[i];
        if (y == 0xD0 && !coll_noend)
            break;
        y++;
        if (y >= coll_lines)
            continue;
        if (coll_emp_has && sat[0x81 + i * 2] == coll_emp_raw)
            continue;
        uint8_t x = sat[0x80 + i * 2];
        uint8_t b = y >> 4;
        ST(st_spr++);
        for (uint8_t bb = b ? b - 1 : 0; bb <= b + 1; bb++) {
            for (uint8_t j = coll_head[bb]; j != 0xFF; j = coll_nxt[j]) {
                uint8_t dx = x - coll_sx[j] + 7;
                ST(st_rect++);
                if (dx > 14)
                    continue;
                uint8_t dy = y - coll_sy[j] + coll_h1;
                if (dy > coll_hh)
                    continue;
                uint8_t r = coll_pair(i, j);
                if (r == 1)
                    return 0x20;
                if (r == 2)
                    goto listed;
            }
        }
    listed:
        coll_sx[i] = x;
        coll_sy[i] = y;
        coll_nxt[i] = coll_head[b];
        coll_head[b] = i;
    }
    return 0;
}
#else
uint8_t coll_scan(const uint8_t *sat);
#endif

uint8_t gg_platform_sprite_status(void) {
    const uint8_t *reg = gg.vdp.reg;
    if (!(reg[1] & 0x40))
        return 0;
    coll_sat = gg_vram + sat_base;
    coll_h = (reg[1] & 0x02) ? 16 : 8;
    coll_h1 = coll_h - 1;
    coll_hh = 2 * coll_h - 2;
    coll_tb = (reg[6] & 0x04) ? 0x100 : 0;
    coll_lines = (uint8_t) GG_ACTIVE;
    coll_noend = !GG_SPR_END(0xD0);
    emp_tile = 0xFFFF;
    coll_emp_has = 0;
    memset(coll_head, 0xFF, sizeof(coll_head));
    memset(emp, 0, sizeof(emp));
#ifdef GGSWAN_HOST
    st_calls++;
    if (getenv("GGSWAN_COLLSTAT") && st_calls % 500 == 0)
        fprintf(stderr, "COLL after %lu calls: sprites %.1f, rectangle tests %.1f, pairs compared %.2f, rows %.1f a call\n",
                st_calls, (double) st_spr / st_calls, (double) st_rect / st_calls, (double) st_pairs / st_calls, (double) st_rows / st_calls);
#endif
    return coll_scan(coll_sat);
}
#endif

// the sprite table built to the WS (it copies it at the end of the screen)
static void sprites_publish(void) {
    if (!spr_ready)
        return;
    spr_ready = 0;
    // (called from video_present with interrupts already off on the device)
    outportb(0x04, spr_port04);
    outportb(0x05, 0);        // first sprite
    outportb(0x06, spr_count);   // sprite count
}

/*
 * Screen 2: either the Game Gear background tiles with priority (bit 12 of
 * the name table entry: in front of the sprites; Sonic Spinball's tubes
 * hide Sonic), inside the 160x144 view, scrolling with screen 1; or the
 * borders' black mask and the diagnostics, outside the view (the default;
 * Y1 (Y up) held at the start for the priority: the borders then show the
 * map around the view, no diagnostics). The WS has no priority per tile:
 * screen 2,
 * whose sprites are all behind it, is the only layer in front of them.
 * smsswan: always the priority (the view is the whole WS screen: no
 * borders), so no diagnostics there either.
 */
#ifdef GGSWAN_SMS
// (smsswan: Y1 held at the start turns it off, for the diagnostics, shown
// over the picture's sides: screen 2 empty but for the text)
static uint8_t prio_wanted = 1;
#define SCR2_OUTSIDE 0
#else
static uint8_t prio_wanted;   // (main: Y1 held; the PC tests: GGSWAN_PRIO)
#define SCR2_OUTSIDE 0x10     // (screen 2 shown outside its window)
#endif
static void screen2_mode(uint8_t prio) {
    drc_st.prio_on = prio;
    if (prio) {
        map_relayout(map_top, 0);   // (map1 and map2)
        outportb(0x12, inportb(0x10));
        outportb(0x13, inportb(0x11));
    } else {
        for (uint16_t i = 0; i < 32 * 32; i++)
#ifdef GGSWAN_SMS
            map2[i] = WS_PRIO_BLANK;
#else
            map2[i] = TILE_ENTRY(MASK_TILE) | PAL_ENTRY(MASK_PALETTE);
#endif
        outportb(0x12, 0);
        outportb(0x13, 0);
    }
}

GGSWAN_HOST_VISIBLE void video_init(void) {
    rec_n = 1;   // (no split yet: rec_val[0] only)
    drc_st.sprites_dirty = 1;
    drc_st.nt_base = nt_base;
    drc_st.sat_base = sat_base;
    log_init();
    vram_class_update();
    memset(gg_vram, 0, 0x4000);
    memset(map1, 0, MAP_SIZE);
    // border mask: tile 1022 opaque (color 1 of MASK_PALETTE = black)
    uint8_t *t = TILE_PTR(MASK_TILE);
    for (uint8_t r = 0; r < 8; r++) {
        t[r * 4] = 0xFF;
        t[r * 4 + 1] = t[r * 4 + 2] = t[r * 4 + 3] = 0;
    }
    memset(TILE_PTR(1023), 0, 32);
#ifdef GGSWAN_SMS
    // the WS color of each of the Master System's 64 (--BBGGRR, each
    // 2-bit value times 5), for the recompiler's CRAM routine
    for (uint8_t v = 0; v < 64; v++)
        ((uint16_t*) IRAM(WS_CRAM_TABLE))[v] = (uint16_t) ((v & 3) * 5) << 8
            | (uint16_t) (((v >> 2) & 3) * 5) << 4 | (uint16_t) (((v >> 4) & 3) * 5);
#endif
#ifdef GGSWAN_HOST
#ifdef GGSWAN_SMS
    // (the PC tests: GGSWAN_NOPRIO, and GGSWAN_PAN=x,y for the view)
    prio_wanted = getenv("GGSWAN_NOPRIO") == NULL;
    gg_squeeze = getenv("GGSWAN_SQUEEZE") != NULL;
    if (getenv("GGSWAN_PAN")) {
        int px = 16, py = 24;
        sscanf(getenv("GGSWAN_PAN"), "%d,%d", &px, &py);
        gg_view_x = px; gg_view_y = py;
        view_y_want = py; view_moved = 1;
    }
#else
    prio_wanted = getenv("GGSWAN_PRIO") != NULL;
#endif
#endif
    screen2_mode(prio_wanted);
    WS_DISPLAY_COLOR_MEM(MASK_PALETTE)[1] = 0x000;
    memset(sprite_table, 0, 512);
    memset(sprite_table_b, 0, 512);
    spr_back = 1;

    uint16_t m1 = IRAM_OFF(map1), m2 = IRAM_OFF(map2);
    outportb(WS_SCR_BASE_PORT, ((m1 >> 11) & 0x0F) | (((m2 >> 11) & 0x0F) << 4));
    outportb(0x04, (IRAM_OFF(sprite_table) >> 9) & 0x3F);
    // the screen 2 window: the 160x144 view (the mask outside it; the
    // priority tiles inside); smsswan: the whole WS screen, 224x144
    outportb(0x08, WS_VIEW_ORIGIN_X);
    outportb(0x09, 0);
    outportb(0x0A, WS_VIEW_ORIGIN_X + GG_VIEW_W - 1);
    outportb(0x0B, GG_VIEW_H - 1);
    outportb(0x12, 0);
    outportb(0x13, 0);
    outportb(0x06, 0);
    map_relayout(0, 0);
#ifndef GGSWAN_HOST
    ia16_disable_irq();
    ws_int_set_handler(WS_INT_LINE_MATCH, (ia16_int_handler_t) line_irq_entry);
    outportb(WS_DISPLAY_LINE_IRQ_PORT, GG_VIEW_H);
    ws_int_enable(WS_INT_ENABLE_LINE_MATCH);
    ia16_enable_irq();
#endif
}

// The frame is shown as the Game Gear drew it: right after the last line
// it shows (the start of line 168), not at the end of the frame (262) nor
// at the frame interrupt (193). What a game writes below the view is for
// the next frame: Sailor Moon S writes its sprite table at lines 194-208
// and the matching sprite tiles at lines 241-261 and 0-84 of the next
// frame (above its sprites on a Game Gear): shown at line 262 its sprites
// were half the old pose. The Addams Family writes its sprite table at
// lines 180-193 and Gomez's tiles at 198-241: shown at 193 (since the
// Sailor Moon S change), the last sprites still had the old table and
// Gomez the old tiles, in pieces.
GGSWAN_HOST_VISIBLE void video_present(void);
static void pcm_frame(void);   // (the frame's samples: below)
void gg_platform_frame_end(void) {
    if (frame_skip_now()) {
        // a frame left out: its sound, and the scroll splits recorded
        // again from the next frame. The rest waits for the next frame
        // shown: the VRAM log goes on filling (written at once if full),
        // the palette and the sprite table stay dirty, the scroll is the
        // next frame's
        prof_phase = PROF_VIDEO;
        gg_platform_psg_flush();
        pcm_frame();
        rec_val[0] = 16 - gg.vdp.reg[8];
        rec_n = 1;
        thin_f = 0;
        drc_st.hs_last = gg.vdp.reg[8];
        prof_phase = PROF_OTHER;
        return;
    }
    video_present();
}

GGSWAN_HOST_VISIBLE void video_present(void) {
#ifndef GGSWAN_HOST
    // the last frame not shown yet (two Game Gear frames ended in one WS
    // frame): wait for the end of the screen, then for the line after it
    // (the WS copies the sprite table on line 144). Presenting now changed
    // the scroll values, the sprite table and the VRAM (the waiting log,
    // written at once) in the middle of a WS frame that shows the frame
    // before: Aladdin's sprite pieces in the wrong places, Super Monaco
    // GP's status bar now and then.
    if (frame_waiting) {
        prof_phase = PROF_IDLE;
        while (frame_waiting)
            __asm__ volatile ("hlt");
        while (inportb(WS_DISPLAY_LINE_PORT) == GG_VIEW_H) {}
    }
#endif
    prof_phase = PROF_VIDEO;
    const uint8_t *reg = gg.vdp.reg;
    uint8_t vp_start = 0;
    if (diag_mode) {
        vp_start = inportb(WS_DISPLAY_LINE_PORT);
        vp_mark = vp_start;
    }
    gg_platform_psg_flush();   // (the frame's PSG writes, queued)
    pcm_frame();
    if (diag_mode) vp_lap(VP_PSG);

#ifdef GGSWAN_SMS
    {
        uint8_t y = view_y_want, max = GG_VIEW_Y_MAX;
        if (!view_moved)
            y = max / 2;        // (24, or 40 with 224 lines)
        if (y > max)
            y = max;
        if (y != gg_view_y) {
            gg_view_y = y;
            drc_st.sprites_dirty = 1;
        }
    }
    // register 0 bit 5: the picture's first 8 columns are blanked (games
    // that scroll sideways hide the column being redrawn): the view does
    // not go there
    if ((reg[0] & 0x20) && gg_view_x < 8) {
        gg_view_x = 8;
        drc_st.sprites_dirty = 1;
    }
#endif
#ifdef GGSWAN_SMS
    // (the whole picture squeezed: from its first line, 192 lines of map)
    if (view_sq != gg_squeeze) {
        view_sq = gg_squeeze;
        drc_st.sprites_dirty = 1;
    }
    #define VP_VIEW_Y (view_sq ? 0 : GG_VIEW_Y)
    #define VP_SLACK (view_sq ? 256 - GG_ACTIVE_LINES - 8 : 256 - GG_VIEW_H - 8)
    #define VP_ABOVE (view_sq ? 4 : 6)
#else
    #define VP_VIEW_Y GG_VIEW_Y
    #define VP_SLACK (256 - GG_VIEW_H - 8)
    #define VP_ABOVE 6
#endif
    // vertical: the Game Gear view starts at row (24 + vscroll) mod 224
    uint16_t s = VP_VIEW_Y + gg.vdp.vscroll_frame;
    if (s >= NT_P) s -= NT_P;
    uint16_t vy = s;
    while (vy < map_top * 8u)
        vy += NT_P;   // (virtual rows: the same Game Gear row 28 later)
    next_vy = vy & 0xFF;
#ifndef GGSWAN_HOST
    uint8_t relay = 0, relay_top = 0;
#endif
    if (vy > map_top * 8u + VP_SLACK) {
#ifdef GGSWAN_HOST
        if (getenv("GGSWAN_RELAYSTAT"))
            fprintf(stderr, "RELAYOUT %u s=%u top=%u\n", (unsigned) gg.frame, (unsigned) s, (unsigned) map_top);
#endif
        // the view 6 rows into the new map (virtual row (s >> 3) + 28):
        // room to move 48 pixels up and 56 down before the next relayout.
        // (With the view at the top, a camera going up relaid the whole
        // map every 8 pixels, every few frames: Sonic jumping, the
        // background jumping too while the map was rewritten.)
#ifdef GGSWAN_HOST
        host_relayouts++;
#endif
        vy = s + NT_P;
        next_vy = vy & 0xFF;
#ifdef GGSWAN_HOST
        // (the PC tests have no WS raster: the map and its scroll now)
        map_relayout((s >> 3) + NT_H - VP_ABOVE, 0);
        act_vy = next_vy;
#else
        /*
         * The map is laid out again below, after the end of the WS screen
         * that shows this frame's scroll, tiles and sprites for the first
         * time. It was done here, at once, with its scroll: the rows the
         * WS was drawing changed under it, some of them with the old
         * contents at the new place or the other way around for one WS
         * screen (Sonic: a few rows of the picture jumping now and then,
         * whenever the camera had moved some 50 pixels up or down).
         */
        relay = 1;
        relay_top = (s >> 3) + NT_H - VP_ABOVE;
#endif
    }
#ifdef GGSWAN_HOST
    outportb(0x11, next_vy);   // (the PC tests have no WS raster)
#endif
    if (diag_mode) vp_lap(VP_MAP);
    // the sprite table first (built, not given to the WS yet)
    if (drc_st.sprites_dirty)
        build_sprites();
    if (diag_mode) vp_lap(VP_SPR);
    // this frame's VRAM writes (at the end of the WS screen) and its
    // sprite table (the WS copies it there) together, both for the same WS
    // frame: never on the last lines of the screen, where the sprite table
    // missed the copy (line 144 on the WS, 142 in Mednafen) while the
    // VRAM writes still made the end-of-screen interrupt (patch 92: checked
    // only at the start of video_present, a map relayout or the sprite
    // table could still end there: Aladdin's new pose on the old sprites
    // now and then). Then the scroll (splits_commit)
    splits_prepare();
#ifndef GGSWAN_HOST
    for (;;) {
        uint16_t f = irq_save();
        uint8_t l = inportb(WS_DISPLAY_LINE_PORT);
        if (l < GG_VIEW_H - 3 || l > GG_VIEW_H) {
            // the scroll (splits) and frame_waiting in the same step: the
            // end-of-screen interrupt could otherwise come in between and
            // show the new tiles and sprites with the old scroll, and clear
            // frame_waiting before it was set (ChatGPT's review of 89-98)
            log_present();
            pal_present();
            sprites_publish();
            splits_commit();
            frame_waiting = 1;
            irq_restore(f);
            break;
        }
        irq_restore(f);   // (the end-of-screen interrupt)
    }
    if (relay) {
        // the end of the WS screen (its interrupt sets the new scroll for
        // the next one), then the rows in the order the WS draws them,
        // ahead of it: 15 lines of vertical blank to start with, a row of
        // 32 entries written in less time than the 8 lines it is shown for
        prof_phase = PROF_IDLE;
        while (frame_waiting)
            __asm__ volatile ("hlt");
        prof_phase = PROF_VIDEO;
        map_relayout(relay_top, (next_vy >> 3) & 31);
    }
#else
    log_present();
    pal_present();
    sprites_publish();
    splits_commit();
#endif
    // record the next frame: it starts with the current value
    rec_val[0] = 16 - gg.vdp.reg[8];
    rec_n = 1;
    thin_f = 0;
    drc_st.hs_last = gg.vdp.reg[8];
    if (diag_mode) vp_lap(VP_LOG);
    if (!(gg.frame & 7))
        update_backdrop();
    if (diag_mode) vp_lap(VP_BACK);

    // display on: at the start of the frame or now (games such as Sonic
    // turn it off in the frame interrupt while they fill the VRAM, and on
    // again before the next frame: at this point it can still be off)
    if ((reg[1] | gg.vdp.reg1_frame) & 0x40) {
        outportb(0x01, backdrop_palette << 4);        // backdrop: color 0
        // (screen 2: the tiles with priority inside the view, or the
        // borders' mask and diagnostics outside it)
        outportb(0x00, 0x01 | 0x02 | 0x04 | 0x20 | (drc_st.prio_on ? 0 : SCR2_OUTSIDE));
    } else {
        outportb(0x01, 0x10 | (reg[7] & 0x0F));       // backdrop color
        outportb(0x00, drc_st.prio_on ? 0 : 0x02 | SCR2_OUTSIDE | 0x20);
    }
#ifdef GGSWAN_HOST
    frame_waiting = 1;
#endif
    if (diag_mode) {
        vp_mark = vp_start;
        vp_lap(VP_ALL);
    }
    prof_phase = PROF_OTHER;
}
#endif   // (not GGSWAN_SG1000)

/* === Sound: SN76489 to WS === */

#include "psg_table.h"
static const uint8_t psg_volume_table[16] = {
    15, 12, 10, 8, 6, 4, 3, 2, 2, 2, 1, 1, 1, 1, 0, 0
};
// the SN76489's state (psg_run in drc_asm.s writes it too: the layout is
// fixed): the latched register, the noise control, the WS registers to
// update (psg_dirty: bits 0-3 volume of channel, 4-6 tone, 7 noise), the
// volumes (as written: 0 loudest, 15 off), the tones
struct psg_state {
    uint8_t latch, noise, dirty, pad;
    uint8_t vol[4];
    uint16_t tone[3];
};
struct psg_state psg_st;   // (not static: drc_asm.s)

/*
 * Samples. A game that plays them (Sailor Moon S's voices) sets the three
 * tone channels to 0-1 (no tone: a constant level) and writes their
 * volumes thousands of times a second (~11.4 kHz there, three writes a
 * sample). Those levels can not wait for the end of the frame like the
 * rest: each write to channel 2's volume while the three are without
 * tone queues the sum of the three levels in pcm_buf (psg_apply, psg_run
 * in drc_asm.s), and at the end of the frame (pcm_frame) the sound DMA
 * plays them on WS channel 2 as a voice, 200 samples at 12 kHz a frame.
 */
#define PCM_MAX 640
uint8_t pcm_on;               // (drc_asm.s too) the three tones are 0-1
uint16_t pcm_n;               // (drc_asm.s too) levels in pcm_buf
uint8_t pcm_buf[PCM_MAX];     // (drc_asm.s too)
// 2 dB a step, 0-15 (off): three of them add up to 0-255
const uint8_t pcm_level[16] = {   // (drc_asm.s too)
    85, 67, 54, 43, 34, 27, 21, 17, 13, 11, 8, 7, 5, 4, 3, 0
};
static uint8_t voice_on;      // WS channel 2 plays the samples
#ifndef WS_SDMA_CTRL_PORT   // (the host's older headers)
#define WS_SDMA_SOURCE_L_PORT 0x4A
#define WS_SDMA_SOURCE_H_PORT 0x4C
#define WS_SDMA_LENGTH_L_PORT 0x4E
#define WS_SDMA_LENGTH_H_PORT 0x50
#define WS_SDMA_CTRL_PORT 0x52
#define WS_SDMA_CTRL_ENABLE 0x80
#define WS_SDMA_CTRL_TARGET_CH2 0x00
#define WS_SDMA_CTRL_ONESHOT 0x00
#define WS_SDMA_CTRL_RATE_12000 0x02
#define WS_SOUND_VOICE_VOL_PORT 0x94
#define WS_SOUND_VOICE_VOL_LEFT_FULL 0x0C
#define WS_SOUND_VOICE_VOL_RIGHT_FULL 0x03
#endif
#define PCM_OUT_MAX 200
#ifndef GGSWAN_HOST
#define PCM_OUT ((uint8_t*) 0xE400)   // 2 x 200 (free IRAM E400h-E58Fh)
#else
static uint8_t pcm_out_host[2 * PCM_OUT_MAX];
#define PCM_OUT pcm_out_host
#endif
#define psg_latch psg_st.latch
#define psg_noise psg_st.noise
#define psg_dirty psg_st.dirty
#define psg_vol psg_st.vol
#define psg_tone psg_st.tone
static uint8_t psg_stereo = 0xFF;

/*
 * The menu's AUDIO (patch 225): snd_vol 0-8 (8: as the table, the
 * default), snd_mute. vol_table is psg_volume_table times snd_vol / 8,
 * 0 with the sound off; the voice (samples) has two steps, half and
 * full.
 */
static uint8_t snd_vol = 8, snd_mute;
static uint8_t vol_table[16] = {
    15, 12, 10, 8, 6, 4, 3, 2, 2, 2, 1, 1, 1, 1, 0, 0
};
static uint8_t voice_vol = WS_SOUND_VOICE_VOL_LEFT_FULL | WS_SOUND_VOICE_VOL_RIGHT_FULL;
static void psg_volume(uint8_t ch);
static __attribute__((unused)) void snd_volume_set(void) {
    uint8_t v = snd_mute ? 0 : snd_vol;
    for (uint8_t i = 0; i < 16; i++)
        vol_table[i] = (uint8_t) ((psg_volume_table[i] * v + 4) >> 3);
    voice_vol = !v ? 0 : v <= 5 ? 0x08 | 0x02 : WS_SOUND_VOICE_VOL_LEFT_FULL | WS_SOUND_VOICE_VOL_RIGHT_FULL;
}

static void psg_volume(uint8_t ch) {
    if (voice_on && ch < 3) {
        // (channel 2's volume port is the voice's sample now)
        if (ch != 1)
            outportb(0x88 + ch, 0);
        return;
    }
    outportb(0x88 + ch, vol_table[psg_vol[ch]] * ((psg_stereo >> ch) & 0x11));
}

static uint8_t noise_slow;   // channel 4's wave: one pulse in 32 samples
static void psg_noise_freq(void) {
    uint16_t tone;
    switch (psg_noise & 3) {
    case 0: tone = 0x10; break;
    case 1: tone = 0x20; break;
    case 2: tone = 0x40; break;
    default: tone = psg_tone[2]; break;
    }
    if (!tone)
        tone = 1;   // (as the tone channels: 0 counts as 1)
    // 3072000 * tone / (3579545 >> 5)
    uint32_t divider = ((uint32_t) tone * 28118) >> 10;
    /*
     * The WS divider has 11 bits: with the rate taken from tone 2 (basses
     * of periodic noise, low rumbles) anything over 74 did not fit, and
     * what was left of it gave some other pitch. Periodic noise (the
     * wave: two pulses in its 32 samples) goes on an octave lower with
     * one pulse, twice as wide, at twice the rate; below that, and for
     * white noise, the slowest there is.
     */
    uint8_t slow = 0;
    if (divider > 2047) {
        if (!(psg_noise & 4)) {
            slow = 1;
            divider >>= 1;
        }
        if (divider > 2047)
            divider = 2047;
    }
    if (slow != noise_slow) {
        noise_slow = slow;
        wave_table[48] = slow ? 0xFF : 0x0F;
        wave_table[56] = slow ? 0x00 : 0x0F;
    }
    outportw(0x80 + 3 * 2, -(uint16_t) divider);
    outportb(WS_SOUND_CH_CTRL_PORT, ((psg_noise & 4) ? 0x8F : 0x0F) | (voice_on ? 0x20 : 0));
}

// one write to the SN76489: its state only (psg_dirty: what to output)
static void psg_apply(uint8_t data) {
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
        psg_vol[ch] = data & 0x0F;
        psg_dirty |= 1 << ch;
        if (ch == 2 && pcm_on && pcm_n < PCM_MAX)
            pcm_buf[pcm_n++] = pcm_level[psg_vol[0]] + pcm_level[psg_vol[1]] + pcm_level[psg_vol[2]];
        break;
    case 6:
        psg_noise = data & 7;
        psg_dirty |= 0x80;
        break;
    default:
        if (data & 0x80)
            psg_tone[ch] = (psg_tone[ch] & 0x3F0) | (data & 0x0F);
        else
            psg_tone[ch] = (psg_tone[ch] & 0x00F) | ((data & 0x3F) << 4);
        psg_dirty |= 0x10 << ch;
        if (ch == 2 && (psg_noise & 3) == 3)
            psg_dirty |= 0x80;
        break;
    }
}

// the WS sound registers of what changed
static void psg_output(void) {
    uint8_t d = psg_dirty;
    psg_dirty = 0;
    for (uint8_t ch = 0; ch < 4; ch++)
        if (d & (1 << ch))
            psg_volume(ch);
    for (uint8_t ch = 0; ch < 3; ch++)
        if (d & (0x10 << ch))
            // 3072000 * tone / (3579545 >> 1): the wave holds two periods
            // (a table: no 32-bit multiplication). Tone 0 counts as 1, a
            // steady level (samples played through the volume): its
            // divider 0 was the slowest the WS has, a hum at 94 Hz
            outportw(0x80 + ch * 2, -psg_divider[psg_tone[ch] ? psg_tone[ch] : 1]);
    if (d & 0x70)
        pcm_on = psg_tone[0] <= 1 && psg_tone[1] <= 1 && psg_tone[2] <= 1;
    if (d & 0x80)
        psg_noise_freq();
}

// the writes the recompiler queued (OUT (n),A to ports 40-7F), in order,
// then the WS registers once: a sample played through the volume (Sailor
// Moon S, hundreds of writes a frame) costs a C loop, not a helper each
#ifndef GGSWAN_HOST
void psg_run(const uint8_t *q, uint8_t n);   // (drc_asm.s)
#endif
void gg_platform_psg_flush(void) {
    uint8_t n = (uint8_t) drc_st.psg_n;
    if (!n)
        return;
    drc_st.psg_n = 0;
#ifndef GGSWAN_HOST
    psg_run(drc_st.psg_q, n);   // (the loop below in assembly)
#else
    for (uint8_t i = 0; i < n; i++)
        psg_apply(drc_st.psg_q[i]);
#endif
    psg_output();
}

// the frame's samples to the sound DMA (see pcm_buf): a Game Gear frame
// lasts 200 samples at 12 kHz, whatever the frame rate (slower than 60
// frames a second: gaps between the frames' samples, but the voice at
// its speed and pitch)
#define PCM_FRAME 200
static uint8_t pcm_half;
static void pcm_frame(void) {
    uint16_t n = PCM_FRAME;
    if (!pcm_n) {
        if (voice_on) {
            outportb(WS_SDMA_CTRL_PORT, 0);
            voice_on = 0;
            psg_dirty |= 0xFF;   // the channels as they are
            psg_output();
        }
        return;
    }
    if (!voice_on) {
        voice_on = 1;
        outportb(WS_SOUND_VOICE_VOL_PORT, voice_vol);
        psg_dirty |= 0x87;   // tone channels quiet, the voice on
        psg_output();
    }
    uint8_t *out = PCM_OUT + (pcm_half ? PCM_OUT_MAX : 0);
    pcm_half ^= 1;
    // pcm_n levels over n samples (10.6 fixed point: pcm_n <= 640)
    uint16_t step = (pcm_n << 6) / n, pos = 0;
    for (uint16_t i = 0; i < n; i++, pos += step)
        out[i] = pcm_buf[pos >> 6];
    pcm_n = 0;
    outportb(WS_SDMA_CTRL_PORT, 0);
    outportw(WS_SDMA_SOURCE_L_PORT, (uint16_t) (uintptr_t) out);
    outportb(WS_SDMA_SOURCE_H_PORT, 0);
    outportw(WS_SDMA_LENGTH_L_PORT, n);
    outportb(WS_SDMA_LENGTH_H_PORT, 0);
    outportb(WS_SDMA_CTRL_PORT, WS_SDMA_CTRL_ENABLE | WS_SDMA_CTRL_RATE_12000
        | WS_SDMA_CTRL_TARGET_CH2 | WS_SDMA_CTRL_ONESHOT);
}

void gg_platform_psg(uint8_t data) {
    gg_platform_psg_flush();   // (the queued ones first)
    psg_apply(data);
    psg_output();
}

void gg_platform_stereo(uint8_t v) {
    gg_platform_psg_flush();
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
    for (uint8_t ch = 0; ch < 4; ch++) {
        psg_vol[ch] = 15;   // (off)
        outportb(0x88 + ch, 0);
    }
    outportb(WS_SOUND_CH_CTRL_PORT, 0x0F);
    outportb(WS_SOUND_OUT_CTRL_PORT, WS_SOUND_OUT_CTRL_HEADPHONE_ENABLE
        | WS_SOUND_OUT_CTRL_SPEAKER_ENABLE | WS_SOUND_OUT_CTRL_SPEAKER_VOLUME_100);
    outportb(WS_SOUND_NOISE_CTRL_PORT, WS_SOUND_NOISE_CTRL_LENGTH_1953
        | WS_SOUND_NOISE_CTRL_ENABLE | WS_SOUND_NOISE_CTRL_RESET);
}

/* === Input === */

#if defined(GGSWAN_SF7000) && !defined(GGSWAN_HOST)
/* === sfswan: the keyboard on the screen === */

/*
 * START shows and hides it, at the top of the screen (screen 2, over the
 * picture; the view is at the bottom of the TMS9918A's, where a text
 * screen is being written). Four rows of 12 keys, as on an SC-3000 but
 * for the keys with no character, and a row of nine things:
 *   SH  SHIFT for the next key          BK  BREAK
 *   DL  INS/DEL                         SP  the space bar
 *   CR  the CR key                      FI LO RU BO  typed for you:
 *       FILES + CR, LOAD ", RUN + CR, BOOT + CR
 * X1-X4 move over them, A is the finger (down while A is down), B is CR.
 * While it is shown the joypad is not the game's. The 48 characters of
 * the keys take the place of the diagnostics' font in its tiles (they are
 * the built-in IPL's font, the one Disk BASIC writes with), and the
 * diagnostics are not written.
 */
#define KB_PAL_SEL 15
#define KB_PAL_GAP 13
static const char __far kb_keys[] = "1234567890-^" "QWERTYUIOP@[" "ASDFGHJKL;:]" "ZXCVBNM,./\\_";
static const char __far kb_items[] = "SHBKDLSPCRFILORUBO";
static const char __far kb_typed[] = "FILES\r\0LOAD \"\0RUN\r\0BOOT\r\0";
static uint8_t kb_open, kb_row, kb_col, kb_shift, kb_was_a;
static uint16_t kb_prev;
static const char __far *kb_typing;
static uint8_t kb_typing_t;

static uint16_t kb_glyph(char ch, uint8_t pal) {
    for (uint8_t i = 0; i < 48; i++)
        if (kb_keys[i] == ch)
            return TILE_ENTRY(FONT_TILE + i) | PAL_ENTRY(pal);
    return TILE_ENTRY(FONT_TILE) | PAL_ENTRY(KB_PAL_GAP);
}

static void kb_draw(void) {
    for (uint8_t row = 0; row < 5; row++) {
        uint16_t *m = map2 + row * 32;
        for (uint8_t x = 0; x < 28; x++)
            m[x] = TILE_ENTRY(FONT_TILE) | PAL_ENTRY(KB_PAL_GAP);
        if (row < 4) {
            for (uint8_t c = 0; c < 12; c++)
                m[2 + c * 2] = kb_glyph(kb_keys[row * 12 + c],
                    row == kb_row && c == kb_col ? KB_PAL_SEL : FONT_PALETTE);
        } else {
            for (uint8_t c = 0; c < 9; c++) {
                uint8_t pal = (kb_row == 4 && c == kb_col) || (c == 0 && kb_shift) ? KB_PAL_SEL : FONT_PALETTE;
                m[c * 3] = kb_glyph(kb_items[c * 2], pal);
                m[c * 3 + 1] = kb_glyph(kb_items[c * 2 + 1], pal);
            }
        }
    }
}

static void kb_show(uint8_t on) {
    kb_open = on;
    if (!on) {
        for (uint8_t row = 0; row < 5; row++)
            for (uint8_t x = 0; x < 28; x++)
                map2[row * 32 + x] = WS_PRIO_BLANK;
        font_load();
        return;
    }
    // the keys' characters: the IPL's font, a pixel to the right
    for (uint8_t i = 0; i < 48; i++) {
        uint8_t ch = kb_keys[i] == '_' ? 0x5F : kb_keys[i];
        const uint8_t __far *g = sf_ipl_builtin + SF_IPL_FONT + (uint16_t) (ch - 0x20) * 8;
        uint8_t *t = TILE_PTR(FONT_TILE + i);
        for (uint8_t r = 0; r < 8; r++) {
            uint8_t v = g[r] >> 1;
            t[r * 4] = v;
            t[r * 4 + 1] = ~v;
            t[r * 4 + 2] = 0;
            t[r * 4 + 3] = 0;
        }
    }
    WS_DISPLAY_COLOR_MEM(FONT_PALETTE)[1] = 0xFFF;
    WS_DISPLAY_COLOR_MEM(FONT_PALETTE)[2] = 0x000;
    WS_DISPLAY_COLOR_MEM(KB_PAL_SEL)[1] = 0x000;
    WS_DISPLAY_COLOR_MEM(KB_PAL_SEL)[2] = 0xFE0;
    WS_DISPLAY_COLOR_MEM(KB_PAL_GAP)[1] = 0x000;
    WS_DISPLAY_COLOR_MEM(KB_PAL_GAP)[2] = 0x000;
    kb_draw();
}

// a key of the matrix down, with SHIFT if its character needs it
static void kb_key(uint8_t key) {
    if (key == 0xFF)
        return;
    sf_key_set(key & 0x7F, 1);
    if (key & SF_KEY_SHIFT_BIT)
        sf_key_set(SF_K_SHIFT, 1);
}

// once a frame: k the WonderSwan's keys. Returns the joypad for the game
static uint8_t kb_frame(uint16_t k) {
    uint16_t pressed = k & ~kb_prev;
    kb_prev = k;
    memset(sf_keys, 0, sizeof(sf_keys));
    if (sf_boot_at && gg.frame >= sf_boot_at) {
        // (BOOT and CR: the fourth of the lines typed for you)
        const char __far *t = kb_typed;
        for (uint8_t n = 3; n; n--) {
            while (*t) t++;
            t++;
        }
        kb_typing = t;
        kb_typing_t = 0;
        sf_boot_at = 0;
    }
    if (kb_typing) {
        // a key every 8 frames, down for 4
        char c = *kb_typing;
        if (!c) {
            kb_typing = 0;
        } else {
            if ((kb_typing_t & 7) < 4)
                kb_key(sf_char_key(c));
            if ((++kb_typing_t & 7) == 0)
                kb_typing++;
        }
        return 0;
    }
    if (!kb_open) {
        uint8_t g = 0;
        if (k & WS_KEY_X1) g |= GG_KEY_UP;
        if (k & WS_KEY_X3) g |= GG_KEY_DOWN;
        if (k & WS_KEY_X4) g |= GG_KEY_LEFT;
        if (k & WS_KEY_X2) g |= GG_KEY_RIGHT;
        if (k & WS_KEY_B) g |= GG_KEY_1;
        if (k & WS_KEY_A) g |= GG_KEY_2;
        return g;
    }
    uint8_t cols = kb_row == 4 ? 9 : 12, moved = 0;
    if (pressed & WS_KEY_X2) { kb_col = kb_col + 1 < cols ? kb_col + 1 : 0; moved = 1; }
    if (pressed & WS_KEY_X4) { kb_col = kb_col ? kb_col - 1 : cols - 1; moved = 1; }
    if (pressed & (WS_KEY_X1 | WS_KEY_X3)) {
        uint8_t row = (pressed & WS_KEY_X1) ? (kb_row ? kb_row - 1 : 4) : (kb_row < 4 ? kb_row + 1 : 0);
        // (the last row has 9 places for the others' 12)
        if (row == 4 && kb_row != 4) kb_col = (uint8_t) (kb_col * 3 / 4);
        else if (row != 4 && kb_row == 4) kb_col = (uint8_t) ((kb_col * 4 + 2) / 3);
        if (kb_col > 11) kb_col = 11;
        kb_row = row;
        moved = 1;
    }
    if (k & WS_KEY_B) {
        kb_key(SF_K_ENTER);
    } else if (k & WS_KEY_A) {
        uint8_t edge = !kb_was_a;
        if (kb_row < 4) {
            char ch = kb_keys[kb_row * 12 + kb_col];
            // (the last key of the last row is pi: no character for it)
            uint8_t key = ch == '_' ? SF_KEY(3, 5) : sf_char_key(ch);
            kb_key(key);
            if (kb_shift)
                sf_key_set(SF_K_SHIFT, 1);
        } else if (kb_col == 0) {
            if (edge) { kb_shift ^= 1; moved = 1; }
        } else if (kb_col == 1) {
            kb_key(SF_K_BREAK);
        } else if (kb_col == 2) {
            kb_key(SF_K_INSDEL);
        } else if (kb_col == 3) {
            kb_key(SF_K_SPACE);
        } else if (kb_col == 4) {
            kb_key(SF_K_ENTER);
        } else if (edge) {
            // typed for you: the nth of kb_typed
            const char __far *t = kb_typed;
            for (uint8_t n = kb_col - 5; n; n--) {
                while (*t) t++;
                t++;
            }
            kb_typing = t;
            kb_typing_t = 0;
        }
    } else if (kb_was_a && kb_row < 4 && kb_shift) {
        kb_shift = 0;   // (SHIFT was for that key)
        moved = 1;
    }
    kb_was_a = (k & WS_KEY_A) != 0;
    if (moved)
        kb_draw();
    return 0;
}
#elif defined(GGSWAN_COLECO) && !defined(GGSWAN_HOST)
/* === cvswan: the keypad on the screen === */

/*
 * START + Y2 (Y right) shows and hides it, in the bottom-right corner on
 * screen 2: 4 rows of 3 keys (1 2 3 / 4 5 6 / 7 8 9 / * 0 #), using the
 * plugin's own font tiles (with * and # in place of _ and ! in font.inc).
 * While open: X1-X4 move the selection, A holds the selected key down, B
 * (or START + Y2) closes it, and joystick/fires do not reach the game.
 * The last selected key stays highlighted when reopened.
 */
#define CV_KP_PAL_SEL 15
#define CV_KP_PAL_GAP 13
#define CV_KP_ROW0    14
#define CV_KP_COL0    21
static const char __far cv_kp_chars[] = "123456789*0#";
static const uint8_t __far cv_kp_vals[12] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11
};
static uint8_t cv_kp_open, cv_kp_row, cv_kp_col, cv_kp_b_wait;
static uint16_t cv_kp_prev;

static uint16_t cv_kp_tile(char ch, uint8_t pal) {
    uint8_t g = 0;
    for (uint8_t i = 0; font_chars[i]; i++)
        if (font_chars[i] == ch) { g = i; break; }
    return TILE_ENTRY(FONT_TILE + g) | PAL_ENTRY(pal);
}

static void cv_kp_draw(void) {
    uint16_t gap = TILE_ENTRY(FONT_TILE) | PAL_ENTRY(CV_KP_PAL_GAP);
    for (uint8_t r = 0; r < 4; r++) {
        uint16_t *m = map2 + (CV_KP_ROW0 + r) * 32 + CV_KP_COL0;
        for (uint8_t x = 0; x < 7; x++)
            m[x] = gap;
        for (uint8_t c = 0; c < 3; c++) {
            uint8_t pal = (r == cv_kp_row && c == cv_kp_col) ? CV_KP_PAL_SEL : FONT_PALETTE;
            m[1 + c * 2] = cv_kp_tile(cv_kp_chars[r * 3 + c], pal);
        }
    }
}

static void cv_kp_show(uint8_t on) {
    cv_kp_open = on;
    if (!on) {
        for (uint8_t r = 0; r < 4; r++)
            for (uint8_t x = 0; x < 7; x++)
                map2[(CV_KP_ROW0 + r) * 32 + CV_KP_COL0 + x] = WS_PRIO_BLANK;
        gg.cv_keypad = 0xFF;
        return;
    }
    cv_kp_prev = 0xFFFF;
    WS_DISPLAY_COLOR_MEM(FONT_PALETTE)[1] = 0xFFF;
    WS_DISPLAY_COLOR_MEM(FONT_PALETTE)[2] = 0x000;
    WS_DISPLAY_COLOR_MEM(CV_KP_PAL_SEL)[1] = 0x000;
    WS_DISPLAY_COLOR_MEM(CV_KP_PAL_SEL)[2] = 0xFE0;
    WS_DISPLAY_COLOR_MEM(CV_KP_PAL_GAP)[1] = 0x000;
    WS_DISPLAY_COLOR_MEM(CV_KP_PAL_GAP)[2] = 0x000;
    cv_kp_draw();
}

static void cv_kp_frame(uint16_t k) {
    uint16_t pressed = k & ~cv_kp_prev;
    cv_kp_prev = k;
    if (pressed & WS_KEY_B) {
        cv_kp_b_wait = 1;
        cv_kp_show(0);
        return;
    }
    uint8_t moved = 0;
    if (pressed & WS_KEY_X2) { cv_kp_col = cv_kp_col < 2 ? cv_kp_col + 1 : 0; moved = 1; }
    if (pressed & WS_KEY_X4) { cv_kp_col = cv_kp_col ? cv_kp_col - 1 : 2; moved = 1; }
    if (pressed & WS_KEY_X1) { cv_kp_row = cv_kp_row ? cv_kp_row - 1 : 3; moved = 1; }
    if (pressed & WS_KEY_X3) { cv_kp_row = cv_kp_row < 3 ? cv_kp_row + 1 : 0; moved = 1; }
    if (moved)
        cv_kp_draw();
    gg.cv_keypad = (k & WS_KEY_A) ? cv_kp_vals[cv_kp_row * 3 + cv_kp_col] : 0xFF;
}
#endif

#if (defined(GGSWAN_SMS) || defined(GGSWAN_SG1000)) && !defined(GGSWAN_HOST)
static uint8_t step_paused;   // (frame by frame: below)
#endif
/*
 * START + Y4 (Y left): the numbers (FPS, the recompiler's, the profile),
 * on and off while playing. ggswan shows them in its borders from the
 * start: off, the borders are empty. smsswan and sgswan have no borders:
 * the numbers go over the sides of the picture, on screen 2, which in
 * smsswan is otherwise the background's priority (so: numbers and no
 * priority, or priority; as Y1 (Y up) held at the start chooses).
 */
static uint8_t stats_hide __attribute__((unused));
// the profiler's FPS value only (the menu's PROFILER: FPS, patch 227):
// the FPS and the frameskip under it, nothing else
static uint8_t fps_only __attribute__((unused));
static __attribute__((unused)) void stats_toggle(void) {
#ifndef GGSWAN_HOST
#if defined(GGSWAN_SG1000)
    sg_diag ^= 1;
    drc_st.prio_on = !sg_diag;
    for (uint16_t i = 0; i < 32 * 32; i++)
        map2[i] = WS_PRIO_BLANK;
#ifdef GGSWAN_SF7000
    if (kb_open)
        kb_draw();
#elif defined(GGSWAN_COLECO)
    if (cv_kp_open)
        cv_kp_draw();
#endif
#elif defined(GGSWAN_SMS)
    prio_wanted ^= 1;
    screen2_mode(prio_wanted);
#else
    stats_hide ^= 1;
    if (!drc_st.prio_on)
        screen2_mode(0);   // (the borders' mask again: nothing written on it)
#endif
#endif
}

// (a state kept and brought back, below. Not sfswan: the code for it is
// there - ST_PARTS 3, st_src - and was not finished: its stack is under 4
// KiB, the first try ran over it, and Gio can do without)
#if !defined(GGSWAN_HOST) && !defined(GGSWAN_SF7000)
#define ST_STATES 1
static uint8_t st_req, st_held, st_wait, st_t0, st_note;
static uint8_t st_used;          // START was held with A or B: not a Start or a Pause for the game when it is let go
static volatile uint8_t vblanks;
#endif
// START + Y3 (Y down): the menu, opened by the main loop (menu_run)
static uint8_t menu_req __attribute__((unused));
#ifdef GGSWAN_HOST
GGSWAN_HOST_VISIBLE void read_keys(void) {
    uint16_t k = ws_keypad_scan();
#else
// k: the keys read once for the frame (main loop: frame_step), patch 235
static void read_keys(uint16_t k) {
#endif
    uint16_t raw_k __attribute__((unused)) = k;   // (k as read: masked below)
#if defined(ST_STATES) && !defined(GGSWAN_HOST)
    // START + A, START + B held half a second (38 of the WonderSwan's
    // frames): the state kept, brought back. None of the three keys is
    // the game's while they are held so, nor until all are let go
    static uint8_t st_combo;
    if (st_wait) {
        if (k & (WS_KEY_START | WS_KEY_A | WS_KEY_B)) k &= ~(WS_KEY_START | WS_KEY_A | WS_KEY_B);
        else st_wait = 0;
    }
    if ((k & WS_KEY_START) && (k & (WS_KEY_A | WS_KEY_B))) {
        if (!st_held) {
            st_held = 1;
            st_t0 = vblanks;
        } else if (st_held == 1 && (uint8_t) (vblanks - st_t0 + (vblanks < st_t0 ? 75 : 0)) >= 38) {
            st_req = (k & WS_KEY_A) ? 1 : 2;
            st_held = 2;
            st_wait = 1;
        }
        st_combo = 1;
        st_used = 1;
        k &= ~(WS_KEY_START | WS_KEY_A | WS_KEY_B);
    } else {
        st_held = 0;
        // (START still held after the other is let go: none of them the game's)
        if (st_combo) {
            if (k & WS_KEY_START) k &= ~(WS_KEY_START | WS_KEY_A | WS_KEY_B);
            else st_combo = 0;
        }
    }
#endif
#if defined(GGSWAN_SG1000) && !defined(GGSWAN_HOST)
    // sgswan: the Y buttons move the 224x144 view over the 256x192
    // picture, as smsswan's
    if (!(k & WS_KEY_START) && !step_paused
#ifdef GGSWAN_COLECO
            && !cv_kp_open
#endif
            && (k & (WS_KEY_Y2 | WS_KEY_Y4)) != (WS_KEY_Y2 | WS_KEY_Y4)) {
        if ((k & WS_KEY_Y1) && gg_view_y >= 2) gg_view_y -= 2;
        if ((k & WS_KEY_Y3) && gg_view_y <= GG_VIEW_Y_MAX - 2) gg_view_y += 2;
        if ((k & WS_KEY_Y4) && gg_view_x >= 2) gg_view_x -= 2;
        if ((k & WS_KEY_Y2) && gg_view_x <= GG_VIEW_X_MAX - 2) gg_view_x += 2;
    }
#endif
#if defined(GGSWAN_SMS) && !defined(GGSWAN_HOST)
    // smsswan: the Y buttons move the 224x144 view over the 256x192
    // picture, 2 pixels a frame (not with START held: START + Y1 is the
    // way out; not Y2 and Y4 together nor while stopped: frame by frame)
    if (!(k & WS_KEY_START) && !step_paused
            && (k & (WS_KEY_Y2 | WS_KEY_Y4)) != (WS_KEY_Y2 | WS_KEY_Y4)) {
        uint8_t x = gg_view_x, y = gg_view_y;
        // (register 0 bit 5: not into the first 8 columns, blanked; held
        // there, Y4 moved the view in and video_present moved it back,
        // the sprite table built again on every frame)
        uint8_t x_min = (gg.vdp.reg[0] & 0x20) ? 8 : 0;
        if ((k & WS_KEY_Y1) && y >= 2) y -= 2;
        if ((k & WS_KEY_Y3) && y <= GG_VIEW_Y_MAX - 2) y += 2;
        if ((k & WS_KEY_Y4) && x >= x_min + 2) x -= 2;
        if ((k & WS_KEY_Y2) && x <= GG_VIEW_X_MAX - 2) x += 2;
        if (x != gg_view_x || y != gg_view_y) {
            gg_view_x = x;
            gg_view_y = y;
            view_y_want = y;
            view_moved = 1;
            drc_st.sprites_dirty = 1;
        }
    }
#endif


    uint8_t g = 0;
    if (k & WS_KEY_X1) g |= GG_KEY_UP;
    if (k & WS_KEY_X3) g |= GG_KEY_DOWN;
    if (k & WS_KEY_X4) g |= GG_KEY_LEFT;
    if (k & WS_KEY_X2) g |= GG_KEY_RIGHT;
#ifdef GGSWAN_COLECO
    if (k & WS_KEY_A) g |= GG_KEY_1;   // left fire
    if (k & WS_KEY_B) g |= GG_KEY_2;   // right fire
#else
    if (k & WS_KEY_B) g |= GG_KEY_1;
    if (k & WS_KEY_A) g |= GG_KEY_2;
#endif
#if defined(GGSWAN_SF7000) && !defined(GGSWAN_HOST)
    // sfswan: START shows and hides the keyboard, when it is let go
    // (unless a Y button was held with it: START + Y1 (Y up) is the way
    // back to the menu, START + Y2 (Y right) the sprites' limit, START +
    // Y3 (Y down) kept for the whole picture squeezed); the
    // SC-3000's RESET key (GG_KEY_START in the machine) is not there
    {
        static uint8_t start_held, start_y, start_y3, start_sq, start_st;
        if (k & WS_KEY_START) {
            if (!start_held) { start_held = 1; start_y = 0; start_y3 = 0; start_sq = 0; start_st = 0; }
            if (k & WS_KEY_Y1) start_y = 1;
            if (k & WS_KEY_Y3) {
                if (!start_sq) menu_req = 1;   // (the squeeze: in the menu's VIDEO)
                start_sq = 1;
                start_y = 1;
            } else {
                start_sq = 0;
            }
            if (k & WS_KEY_Y4) {
                if (!start_st) stats_toggle();
                start_st = 1;
                start_y = 1;
            } else {
                start_st = 0;
            }
            if (k & WS_KEY_Y2) {
                if (!start_y3) {
                    sg_limit ^= 1;
                    sg_spr_dirty = 2;
                }
                start_y3 = 1;
                start_y = 1;
            } else {
                start_y3 = 0;
            }
        } else {
#ifdef ST_STATES
            if (st_used) start_y = 1;
            st_used = 0;
#endif
            if (start_held && !start_y)
                kb_show(!kb_open);
            start_held = 0;
        }
        g = kb_frame(k);
    }
#elif defined(GGSWAN_SF7000)
    // (the PC tools type on the SF-7000's keyboard themselves)
#elif defined(GGSWAN_SMS)
    // START is the Pause button, pressed for the game when it is let go
    // (one frame: gg_run_frame takes the edge) unless Y1 (Y up) was held
    // with it: START + Y1 goes back to the menu
    {
        static uint8_t start_held, start_y1, start_y3, start_st;
        if (k & WS_KEY_START) {
            if (!start_held) { start_held = 1; start_y1 = 0; start_y3 = 0; start_st = 0; }
            if (k & WS_KEY_Y1) start_y1 = 1;
            // START + Y3 (Y down): the menu (the whole picture squeezed,
            // or the view, is now in its VIDEO page)
            if (k & WS_KEY_Y3) {
                if (!start_y3) menu_req = 1;
                start_y3 = 1;
                start_y1 = 1;   // (no Pause for the game)
            } else {
                start_y3 = 0;
            }
            // START + Y4 (Y left): the numbers, or the priority
            if (k & WS_KEY_Y4) {
                if (!start_st) stats_toggle();
                start_st = 1;
                start_y1 = 1;
            } else {
                start_st = 0;
            }
        } else {
#ifdef ST_STATES
            if (st_used) start_y1 = 1;
            st_used = 0;
#endif
#ifndef GGSWAN_COLECO
            if (start_held && !start_y1) g |= GG_KEY_START;
#endif
            start_held = 0;
        }
    }
#elif defined(GGSWAN_SG1000)
    // START is the Pause button, pressed for the game when it is let go,
    // unless Y1 (Y up) was held with it (the way back to the menu)
    {
        static uint8_t start_held, start_y1, start_y3, start_sq, start_st;
        if (k & WS_KEY_START) {
            if (!start_held) { start_held = 1; start_y1 = 0; start_y3 = 0; start_sq = 0; start_st = 0; }
            if (k & WS_KEY_Y1) start_y1 = 1;
            // START + Y2 (Y right): four sprites to a line as the chip,
            // or all of them (sg_video.inc). (It was START + Y3 (Y down)
            // until patch 147: that one is kept, in every plugin, for the
            // whole picture squeezed, which is smsswan's only so far)
            if (k & WS_KEY_Y2) {
                if (!start_y3) {
#if defined(GGSWAN_COLECO) && !defined(GGSWAN_HOST)
                    cv_kp_show(!cv_kp_open);
#elif !defined(GGSWAN_COLECO)
                    sg_limit ^= 1;
                    sg_spr_dirty = 2;
#endif
                }
                start_y3 = 1;
                start_y1 = 1;   // (no Pause for the game)
            } else {
                start_y3 = 0;
            }
            // START + Y3 (Y down): the menu (the whole picture squeezed,
            // or the view, sg_video.inc, is now in its VIDEO page)
            if (k & WS_KEY_Y3) {
                if (!start_sq) menu_req = 1;
                start_sq = 1;
                start_y1 = 1;   // (no Pause for the game)
            } else {
                start_sq = 0;
            }
            // START + Y4 (Y left): the numbers over the sides of the picture
            if (k & WS_KEY_Y4) {
                if (!start_st) stats_toggle();
                start_st = 1;
                start_y1 = 1;
            } else {
                start_st = 0;
            }
        } else {
#ifdef ST_STATES
            if (st_used) start_y1 = 1;
            st_used = 0;
#endif
            if (start_held && !start_y1) g |= GG_KEY_START;
            start_held = 0;
        }
    }
#else
    // (ggswan: START is the game's Start, as it is held. With Y4 (Y left)
    // held first it is not sent: START + Y4 hides and shows the numbers)
    {
        static uint8_t start_st, start_menu;
        if ((k & (WS_KEY_START | WS_KEY_Y3)) == (WS_KEY_START | WS_KEY_Y3)) {
            // START + Y3 (Y down): the menu
            if (!start_menu) menu_req = 1;
            start_menu = 1;
#if defined(ST_STATES) && !defined(GGSWAN_HOST)
            st_used = 1;   // (no Start for the game when START is let go)
#endif
        } else if ((k & (WS_KEY_START | WS_KEY_Y4)) == (WS_KEY_START | WS_KEY_Y4)) {
            if (!start_st) stats_toggle();
            start_st = 1;
        } else {
            start_st = 0;
            start_menu = 0;
#if defined(ST_STATES) && !defined(GGSWAN_HOST)
            // START reaches the game after 8 frames of it alone (pressed
            // with A or B a moment later it would have paused the game
            // first); let go sooner, it is pressed for the game for 6
            // frames then (as gbswan)
            static uint8_t start_n, start_tap;
            if (k & WS_KEY_START) {
                if (start_n < 8) start_n++;
                else g |= GG_KEY_START;
            } else {
                if (start_n && start_n < 8 && !st_used) start_tap = 6;
                if (!(raw_k & WS_KEY_START)) { start_n = 0; st_used = 0; }
                if (start_tap) {
                    start_tap--;
                    g |= GG_KEY_START;
                }
            }
#else
            if (k & WS_KEY_START) g |= GG_KEY_START;
#endif
        }
    }
#endif
#if defined(GGSWAN_COLECO) && !defined(GGSWAN_HOST)
    if (cv_kp_open) {
        cv_kp_frame(k);
        g = 0;
    } else {
        gg.cv_keypad = 0xFF;
        if (cv_kp_b_wait) {
            if (k & WS_KEY_B) g &= ~GG_KEY_2;
            else cv_kp_b_wait = 0;
        }
    }
#endif
    gg.keys = g;
}

#ifndef GGSWAN_HOST
/* === Loading === */

static FIL fp;

// (sfswan: a disk image, into the PSRAM banks from load_bank)
#ifdef GGSWAN_SF7000
static uint16_t load_bank = SF_DISK_BANK;
#undef LOAD_BANK
#define LOAD_BANK load_bank
#endif
static uint16_t load_rom(const char *path) {
    int16_t result = f_open(&fp, path, FA_READ | FA_OPEN_EXISTING);
    if (result != FR_OK)
        fail("OPEN", result);
    uint32_t size = f_size(&fp);
    text_kb("SIZE", size);
    if (size > GG_ROM_MAX || size < 0x400)
        fail("ROM SIZE", size);
#ifdef GGSWAN_SF7000
    // (a disk: 40 tracks of 16 sectors of 256 bytes)
    if (size != SF_DISK_SIZE)
        fail("DISK SIZE", size);
#elif defined(GGSWAN_COLECO)
    // ColecoVision (CV1): standard cartridge up to 32 KiB at bank 0 4000h-BFFFh,
    // BIOS (8 KiB) at 0000h-1FFFh; C000h-FFFFh is reserved for TMS9918A VRAM
    if (size > 0x8000)
        fail("ROM SIZE", size);
    {
    uint8_t *buf = (uint8_t*) GG_RAM_ADDR;
    for (uint16_t done = 0; done < 0x8000; done += 0x2000) {
        unsigned int br = 0;
        if (done < size) {
            result = f_read(&fp, buf, 0x2000, &br);
            if (result != FR_OK)
                fail("READ", result);
        }
        if (br < 0x2000)
            memset(buf + br, 0xFF, 0x2000 - br);
        outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
        outportw(WS_CART_EXTBANK_RAM_PORT, LOAD_BANK);
        _fmemcpy(MK_FP(0x1000, 0x4000 + done), buf, 0x2000);
        outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
    }
    f_close(&fp);
    outportw(WS_CART_EXTBANK_ROM0_PORT, LOAD_BANK);
    {
        const uint8_t __far *chk = MK_FP(0x2000, 0xA000);
        for (uint16_t i = 0; i < 0x2000; i++)
            if (chk[i] != buf[i])
                fail("PSRAM MAP", 0xA000);
    }
    // Load the 8 KiB ColecoVision BIOS from SD into bank 0 0000h-1FFFh
    {
        static const char bios_path[] = "/NILESWAN/COLECO.ROM";
        result = f_open(&fp, bios_path, FA_READ | FA_OPEN_EXISTING);
        if (result != FR_OK) {
            text_step = 1;
            text_print("BIOS NOT FOUND");
            text_print("PUT /NILESWAN/COLECO.ROM");
            text_print("ON THE SD CARD (8 KB)");
            fail("BIOS", result);
        }
        uint32_t bsize = f_size(&fp);
        if (bsize != 0x2000) {
            f_close(&fp);
            text_step = 1;
            text_print("BAD /NILESWAN/COLECO.ROM");
            text_print("EXPECTED 8192 BYTES");
            fail("BIOS SIZE", bsize);
        }
        unsigned int br = 0;
        result = f_read(&fp, buf, 0x2000, &br);
        f_close(&fp);
        if (result != FR_OK || br != 0x2000)
            fail("BIOS READ", result);
        outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
        outportw(WS_CART_EXTBANK_RAM_PORT, LOAD_BANK);
        _fmemcpy(MK_FP(0x1000, 0x0000), buf, 0x2000);
        _fmemset(MK_FP(0x1000, 0x2000), 0xFF, 0x2000);
        outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
        outportw(WS_CART_EXTBANK_ROM0_PORT, LOAD_BANK);
        const uint8_t __far *chk = MK_FP(0x2000, 0x0000);
        for (uint16_t i = 0; i < 0x2000; i++)
            if (chk[i] != buf[i])
                fail("BIOS MAP", i);
    }
    return (size + 0x3FFF) >> 14;
    }
#elif defined(GGSWAN_SG1000)
    // (no mapper: 48 KiB at most; the rest of the bank is the VRAM)
    if (size > 0xC000)
        fail("ROM SIZE", size);
#endif

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
        outportw(WS_CART_EXTBANK_RAM_PORT, LOAD_BANK + (done >> 16));
        _fmemcpy(MK_FP(0x1000, (uint16_t) done), buf, 0x2000);
        outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
        done += 0x2000;
    }
    f_close(&fp);

    // check: the last 8 KiB read must be visible through ROM window 0
    outportw(WS_CART_EXTBANK_ROM0_PORT, LOAD_BANK + ((done - 0x2000) >> 16));
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

static volatile uint8_t vblanks;
// frame limiter: the Game Gear shows 59.92 frames a second, the WS 75.47:
// every WS frame earns 5992, a Game Gear frame costs 7547
#define FRAME_EARN 5992
#define FRAME_COST 7547
static volatile uint16_t frame_credit;

/*
 * Frameskip (the menu's VIDEO): fs_mode 0 off, 1 AUTO (a frame left out
 * when the next one is already due: the emulation behind the Game Gear's
 * time; never more than 5 in a row), 2-5: 1, 2, 3, 5 frames left out
 * after each one shown. The CPU and the sound run every frame; only the
 * end-of-frame video work (VI) is left out, so the game sees nothing.
 */
static uint8_t fs_mode, fs_run;
// (the most frames left out in a row since the profiler last showed it)
static uint8_t fs_max;
static const uint8_t fs_n[6] = { 0, 5, 1, 2, 3, 5 };
#ifndef GGSWAN_HOST
static uint8_t frame_skip_now(void) {
    if (fs_mode) {
        uint8_t skip = fs_run < fs_n[fs_mode];
        if (fs_mode == 1 && frame_credit < FRAME_COST)
            skip = 0;
        if (skip) {
            fs_run++;
            return 1;
        }
    }
    if (fs_run > fs_max)
        fs_max = fs_run;
    fs_run = 0;
    return 0;
}
#endif
static volatile uint16_t prof_count[PROF_COUNT];

#ifndef GGSWAN_HOST
/*
 * Frame by frame (for testing, as Mednafen's Alt+A): Y2 (Y right) and Y4
 * (Y left) pressed together stop the game (the sound off) and start it
 * again; while it is stopped, Y3 (Y down) runs one frame. The Game Gear
 * has no Y buttons: the game sees none of this.
 */
static void put_value(uint8_t row, uint8_t col, char label, uint32_t v);
static uint16_t step_keys = 0xFFFF;   // (keys held from the start: not presses)
static uint8_t step_paused;
static uint16_t step_pressed(uint16_t k) {
    uint16_t p = k & ~step_keys;
    step_keys = k;
    return p;
}
#define STEP_PAUSE (WS_KEY_Y2 | WS_KEY_Y4)
// START + Y1 (Y up) held for a second: back to the menu (nileswan soft
// reset: the menu starts again and writes the cartridge RAM, in SRAM, to
// the game's .sav; the Game Gear has no Y buttons)
static uint8_t exit_held;
static void exit_check(uint16_t k) {
    if ((k & (WS_KEY_START | WS_KEY_Y1)) != (WS_KEY_START | WS_KEY_Y1)) {
        exit_held = 0;
        return;
    }
    if (++exit_held < 60)
        return;
    ia16_disable_irq();
    outportb(WS_SDMA_CTRL_PORT, 0);
    outportb(WS_SOUND_CH_CTRL_PORT, 0);
    outportb(WS_DISPLAY_CTRL_PORT, 0);
    // the cartridge as the menu's own soft reset finds it: the RAM window
    // back on SRAM, no SPI device selected, cartridge features as at power
    // on (the plugin set them for the Game Gear)
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, 0);
    outportw(WS_CART_EXTBANK_ROM0_PORT, 0xFFFF);
    outportw(WS_CART_EXTBANK_ROM1_PORT, 0xFFFF);
    outportw(IO_NILE_SPI_CNT, NILE_SPI_CLOCK_CART | NILE_SPI_DEV_NONE);
    outportb(IO_NILE_EMU_CNT, 0);
    nile_soft_reset();
}

#ifdef ST_STATES
/*
 * A state kept and brought back (as gbswan's, patches 181-184): START + A
 * held half a second keeps it, START + B brings it back. What is kept is
 * the whole internal RAM (the Z80's memory, the video memory, the screens,
 * every variable of the machine and of the plugin) and the 64 KiB of SRAM
 * behind the RAM window (the cartridge's RAM), in the two PSRAM banks
 * after the block table's (linear segments B000h and C000h: read there,
 * written through the RAM window), and in a file next to the ROM,
 * ".st0", read at the start if it is this build's. Coming back: the
 * memory but the stack (main's, which is where this is called from,
 * between two frames), the ROM's banks as they were, everything
 * translated forgotten, the sound's registers written again.
 * sgswan (patch 188): the second bank is the screen's tables and the
 * ROM's bank from 8000h, where its RAM and video memory are; sfswan: a
 * bank more, its 64 KiB of RAM (st_src).
 */
#define ST_IRAM_BANK (DRC_CODE_BANKS + 1)
// (the banks after it, one for ggswan, smsswan and sgswan, two for sfswan: st_src)
#ifdef GGSWAN_SF7000
#define ST_PARTS 3
#else
#define ST_PARTS 2
#endif
#define ST_IRAM_SEG (DRC_TABLE_SEG + 0x1000u)
#define ST_CHUNK 512                    // (a piece at a time through st_buf: no internal RAM is free of something,
static uint8_t st_buf[ST_CHUNK];        // and sfswan's stack is 4 KiB: not there)
#define buf st_buf
#define ST_MAGIC (0x54530000ul | ((uint32_t) (GGSWAN_PATCH & 0xFF) << 8) | ST_KIND)
#if defined(GGSWAN_SF7000)
#define ST_KIND 'F'
#elif defined(GGSWAN_COLECO)
#define ST_KIND 'C'
#elif defined(GGSWAN_SG1000)
#define ST_KIND '1'
#elif defined(GGSWAN_SMS)
#define ST_KIND 'S'
#else
#define ST_KIND 'G'
#endif
#define ST_MARK ((const uint32_t __far*) MK_FP(ST_IRAM_SEG, 0))
extern char __wf_heap_start[];          // (the end of the C data: the stack is from there to 4000h)
static void put_text(uint8_t row, uint8_t col, const char __far *t);

static void st_mark(uint32_t v) {
    drc_psram_put(ST_IRAM_BANK, 0, (const uint8_t*) &v, 4);
}
// (with the numbers off nothing is written on the borders or over the picture:
// the sound stopping for the file's second is then the only sign)
static void st_show(const char __far *t) {
    if (stats_hide) return;
    put_text(3, 0, t);
    st_note = 3;
}
// the state's file: the ROM's path with ".st0" for its extension, put
// there and taken away again (the path is the game's)
static char st_ext[4];
static uint16_t st_dot;
static void st_path(uint8_t on) {
    if (on) {
        uint16_t n = 0, dot = 0;
        while (rom_path[n]) {
            if (rom_path[n] == '.') dot = n;
            if (rom_path[n] == '/') dot = 0;
            n++;
        }
        if (!dot || dot + 4 >= ROM_PATH_SIZE) { st_dot = 0; return; }
        st_dot = dot;
        for (uint8_t i = 0; i < 4; i++) st_ext[i] = rom_path[dot + 1 + i];
        rom_path[dot + 1] = 's'; rom_path[dot + 2] = 't'; rom_path[dot + 3] = '0'; rom_path[dot + 4] = 0;
    } else if (st_dot) {
        for (uint8_t i = 0; i < 4; i++) rom_path[st_dot + 1 + i] = st_ext[i];
    }
}
static FIL st_fp;
static uint8_t st_file_write(void) {
    uint8_t bad = 0;
    UINT bw;
    st_path(1);
    if (!st_dot) return 1;
    if (f_open(&st_fp, rom_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) { st_path(0); return 1; }
    for (uint8_t part = 0; part < ST_PARTS && !bad; part++) {
        uint16_t seg = ST_IRAM_SEG + part * 0x1000u;
        uint16_t off = 0;
        do {
            _fmemcpy(buf, MK_FP(seg, off), ST_CHUNK);
            if (f_write(&st_fp, buf, ST_CHUNK, &bw) != FR_OK || bw != ST_CHUNK) bad = 1;
            off += ST_CHUNK;
        } while (off && !bad);
    }
    if (f_close(&st_fp) != FR_OK) bad = 1;
    st_path(0);
    return bad;
}
// at the start: the file, if there is one and it is this build's
// The card's driver moves the cartridge windows: back to the ROM banks the
// game mapped (slots 0 and 1) and to slot 2's RAM window. Without this a
// game that keeps the banks it started with (Addams Family, Alex Kidd)
// ran on whatever the card left there: black or white screen.
static void st_windows_restore(void) {
    outportw(WS_CART_EXTBANK_ROM0_PORT, GG_PSRAM_FIRST_BANK + (drc_st.keys[1] >> 2));
    outportw(WS_CART_EXTBANK_ROM1_PORT, GG_PSRAM_FIRST_BANK + (drc_st.keys[2] >> 2));
    ws_ram_window_restore();
}

static void st_file_read(void) {
    uint8_t bad = 0;
    UINT br;
    st_mark(0);                         // (the PSRAM may still hold another game's)
    st_path(1);
    if (!st_dot) return;
    if (f_open(&st_fp, rom_path, FA_READ) != FR_OK) { st_path(0); return; }
    for (uint8_t part = 0; part < ST_PARTS && !bad; part++) {
        uint16_t off = 0;
        do {
            if (f_read(&st_fp, buf, ST_CHUNK, &br) != FR_OK || br != ST_CHUNK) { bad = 1; break; }
            if (!part && !off) {
                uint32_t m;
                memcpy(&m, buf, 4);
                if (m != ST_MAGIC) { bad = 1; break; }
            }
            drc_psram_put(ST_IRAM_BANK + part, off, buf, ST_CHUNK);
            off += ST_CHUNK;
        } while (off);
    }
    f_close(&st_fp);
    st_path(0);
    if (bad) st_mark(0);
}
// what is kept outside the internal RAM, 64 KiB of it, by its place in
// the state's second bank: the RAM window there (st_src). ggswan,
// smsswan: the SRAM (the cartridge's RAM). sgswan has nothing in the
// SRAM: the first half is the bank of the screen's tables (sg_video.inc:
// they end before 3000h), the second the ROM's bank from 8000h - the RAM
// of a cartridge that has some, then the TMS9918A's video memory
// (sfswan, two banks: the SF-7000's 64 KiB of RAM, then the bank of the
// screen's tables and the video memory. The disk is not kept: what a
// program wrote to it after the state stays written)
static void st_src(uint8_t part, uint16_t off) {
#if defined(GGSWAN_SF7000)
    (void) off;
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, GG_PSRAM_FIRST_BANK + part - 1);
#elif defined(GGSWAN_SG1000)
    (void) part;
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, off >= 0x8000 ? GG_PSRAM_FIRST_BANK : SG_TABLE_BANK);
#else
    (void) part; (void) off;
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, 0);
#endif
}
static void psg_output(void);
__attribute__((noinline)) static void state_save(void) {
    ia16_disable_irq();
    drc_psram_put(ST_IRAM_BANK, 0x0040, (const uint8_t*) 0x0040, 0xFFC0);
    // the SRAM and the PSRAM are both reached through the RAM window: 2 KiB
    // at a time through the internal RAM
    for (uint8_t part = 1; part < ST_PARTS; part++) {
        uint16_t off = 0;
        do {
            st_src(part, off);
            _fmemcpy(buf, MK_FP(0x1000, off), ST_CHUNK);
            drc_psram_put(ST_IRAM_BANK + part, off, buf, ST_CHUNK);
            off += ST_CHUNK;
        } while (off);
    }
    ws_ram_window_restore();
    st_mark(ST_MAGIC);
    ia16_enable_irq();
    st_show("SAVE");
    // the file (a second or two: the WonderSwan's sound off meanwhile)
    uint8_t snd = inportb(WS_SOUND_CH_CTRL_PORT);
    outportb(WS_SOUND_CH_CTRL_PORT, 0);
    if (st_file_write()) st_show("FILE");           // (kept in memory, not on the card)
    st_windows_restore();
    outportb(WS_SOUND_CH_CTRL_PORT, snd);
}
// (the internal RAM from a to b as it was kept)
static void st_back(uint16_t a, uint16_t b) {
    if (b > a) _fmemcpy(MK_FP(0x0000, a), MK_FP(ST_IRAM_SEG, a), b - a);
}
__attribute__((noinline)) static void state_load(void) {
    if (*ST_MARK != ST_MAGIC) {
        st_show("NONE");
        return;
    }
    ia16_disable_irq();
    for (uint8_t part = 1; part < ST_PARTS; part++) {
        uint16_t off = 0;
        do {
            _fmemcpy(buf, MK_FP(ST_IRAM_SEG + part * 0x1000u, off), ST_CHUNK);
            st_src(part, off);
            _fmemcpy(MK_FP(0x1000, off), buf, ST_CHUNK);
            off += ST_CHUNK;
        } while (off);
    }
    // (the stack, from the end of the C data to 4000h, is this function's and main's)
    // (and but FatFs's volume, which is the card's as it is now, and the
    // piece being copied through)
    {
        uint16_t a = (uint16_t) &fs, an = sizeof(fs), b = (uint16_t) st_buf, bn = sizeof(st_buf);
        if (a > b) { uint16_t t = a; a = b; b = t; t = an; an = bn; bn = t; }
        st_back(0x0040, a);
        st_back(a + an, b);
        st_back(b + bn, (uint16_t) __wf_heap_start);
    }
    _fmemcpy(MK_FP(0x0000, 0x4000), MK_FP(ST_IRAM_SEG, 0x4000), 0xC000);
    // (the ROM's windows by the mapper's registers; the RAM window as it
    // was when the state was kept - sgswan's is on the video memory, which
    // the mapper knows nothing of)
    {
        uint8_t wf = ram_window_flash;
        uint16_t wb = ram_window_bank;
        gg_remap();
        ram_window_set(wf, wb);
    }
    drc_flush_request = 1;
    psg_dirty = 0xFF;
    psg_output();
    st_req = 0;
    st_held = 2;
    st_wait = 1;
    exit_held = 0;
    ia16_enable_irq();
    st_show("LOAD");
}
#endif
#undef buf

// k: the keys read once for the frame; back: the keys for read_keys (k,
// or the last read when it was stopped)
static uint16_t frame_step(uint16_t k) {
    exit_check(k);
    uint16_t p = step_pressed(k);
    if ((p & STEP_PAUSE) && (step_keys & STEP_PAUSE) == STEP_PAUSE)
        step_paused = 1;
    else if (!step_paused)
        return k;
    outportb(WS_SOUND_CH_CTRL_PORT, 0);   // (sound off while stopped)
    for (;;) {
        // the Game Gear frame number (left border, F: last 3 digits)
        put_value(3, 0, 'F', gg.frame % 1000);
        // a WS frame (the vblank count: polling the line counter for 144
        // missed it, the line and vblank interrupts being served there)
        uint8_t v = vblanks;
        while (vblanks == v)
            __asm__ volatile ("hlt");
        p = step_pressed(ws_keypad_scan());
        if ((p & STEP_PAUSE) && (step_keys & STEP_PAUSE) == STEP_PAUSE) {
            step_paused = 0;   // go on
            break;
        }
        if (p & WS_KEY_Y3)
            break;             // one frame, then stopped again
    }
    psg_noise_freq();   // (the channels on again)
    ia16_disable_irq();
    frame_credit = FRAME_COST;   // (no catching up)
    ia16_enable_irq();
    return step_keys;
}

/*
 * The menu (START + Y3 (Y down), patch 219): the game stopped while it is
 * open - the CPU, the frames, the sound - and screen 2 alone on the
 * display, with the menu on it. Closed, screen 2 is made again as the
 * plugin has it (screen2_mode, or blank) and the game goes on from the
 * cycle it stopped at: nothing of the machine changes, only the settings.
 * X1 (X up) / X3 (X down): the line; X4 (X left) / X2 (X right) or A:
 * change it; A: into a page; B: back, or closed (also START + Y3 again).
 */
static uint16_t font_entry(char c);
#define MENU_W 28
enum { MP_MAIN, MP_VIDEO, MP_AUDIO, MP_STATES, MP_OPTIONS, MP_ABOUT };
enum { MI_NONE, MI_PAGE_VIDEO, MI_PAGE_AUDIO, MI_PAGE_STATES, MI_PAGE_OPTIONS, MI_PAGE_ABOUT,
       MI_FRAMESKIP, MI_STRETCH, MI_PROFILER, MI_SPRITES, MI_SAVE, MI_LOAD,
       MI_VOLUME, MI_SOUND, MI_SCOPE, MI_DEFAULTS };
// the settings saved per game (the ROM's .cfg) rather than for the
// plugin (OPTIONS; below, cfg_*)
static uint8_t cfg_game;
static void cfg_defaults(void);
// the game's frames in the last whole second before the menu opened (the
// main loop counts them with or without the profiler): the VIDEO page
// shows it, so the FPS can be read with the profiler off (smsswan: the
// profiler takes screen 2 from the background's priority)
static uint8_t last_fps;

static uint16_t menu_blank;
static void menu_text(uint8_t row, uint8_t col, const char __far *t) {
    while (*t && col < MENU_W)
        map2[row * 32 + col++] = font_entry(*(t++));
}
static void menu_clear(void) {
    for (uint16_t i = 0; i < 32 * 32; i++)
        map2[i] = menu_blank;
}

// the lines of a page (MI_*), 0 at the end
static uint8_t menu_items(uint8_t page, uint8_t *it) {
    uint8_t n = 0;
    switch (page) {
    case MP_MAIN:
        it[n++] = MI_PAGE_VIDEO;
        it[n++] = MI_PAGE_AUDIO;
#ifdef ST_STATES
        it[n++] = MI_PAGE_STATES;
#endif
        it[n++] = MI_PAGE_OPTIONS;
        it[n++] = MI_PAGE_ABOUT;
        break;
    case MP_OPTIONS:
        it[n++] = MI_SCOPE;
        it[n++] = MI_DEFAULTS;
        break;
    case MP_VIDEO:
        it[n++] = MI_FRAMESKIP;
#if defined(GGSWAN_SMS) || defined(GGSWAN_SG1000)
        it[n++] = MI_STRETCH;
#endif
        it[n++] = MI_PROFILER;
#ifdef GGSWAN_SG1000
        it[n++] = MI_SPRITES;
#endif
        break;
    case MP_AUDIO:
        it[n++] = MI_VOLUME;
        it[n++] = MI_SOUND;
        break;
#ifdef ST_STATES
    case MP_STATES:
        it[n++] = MI_SAVE;
        it[n++] = MI_LOAD;
        break;
#endif
    }
    return n;
}

static uint8_t menu_on(uint8_t item) {
    switch (item) {
#if defined(GGSWAN_SG1000)
    case MI_STRETCH: return sg_squeeze;
    case MI_PROFILER: return sg_diag;
    case MI_SPRITES: return sg_limit;
#elif defined(GGSWAN_SMS)
    case MI_STRETCH: return gg_squeeze;
    case MI_PROFILER: return !prio_wanted;
#else
    case MI_PROFILER: return !stats_hide;
#endif
    }
    return 0;
}

// the profiler: 0 off, 1 on (all of it), 2 the FPS only
static uint8_t prof_level(void) {
    return !menu_on(MI_PROFILER) ? 0 : fps_only ? 2 : 1;
}
static void prof_level_set(uint8_t l) {
    if (menu_on(MI_PROFILER))
        stats_toggle();
    fps_only = l == 2;
    if (l)
        stats_toggle();
}

static void menu_change(uint8_t item, uint8_t back) {
    switch (item) {
    case MI_FRAMESKIP:
        fs_mode = back ? (fs_mode ? fs_mode - 1 : 5) : (fs_mode < 5 ? fs_mode + 1 : 0);
        fs_run = 0;
        break;
    case MI_VOLUME:
        // (X2 (X right) and A louder, X4 (X left) softer: no wrapping)
        if (back) { if (snd_vol) snd_vol--; }
        else if (snd_vol < 8) snd_vol++;
        snd_volume_set();
        break;
    case MI_SOUND:
        snd_mute ^= 1;
        snd_volume_set();
        break;
    case MI_SCOPE:
        cfg_game ^= 1;
        break;
    case MI_DEFAULTS:
        cfg_defaults();
        break;
#if defined(GGSWAN_SG1000)
    case MI_STRETCH: sg_squeeze ^= 1; break;
    case MI_SPRITES: sg_limit ^= 1; sg_spr_dirty = 2; break;
#elif defined(GGSWAN_SMS)
    case MI_STRETCH: gg_squeeze ^= 1; break;
#endif
    case MI_PROFILER: {
        // OFF -> FPS -> ON -> OFF (X4 (X left) the other way); switched
        // off and on again so that screen 2 is cleared in between
        uint8_t l = prof_level();
        l = back ? (l == 0 ? 1 : l == 2 ? 0 : 2) : (l == 0 ? 2 : l == 2 ? 1 : 0);
        prof_level_set(l);
        break;
    }
    }
    // (stats_toggle -> screen2_mode sets the scroll of screen 2: the
    // game's with the front tiles on, which moved the menu's text)
    outportb(0x12, 0);
    outportb(0x13, 0);
}

static const char __far *menu_label(uint8_t item) {
    switch (item) {
    case MI_PAGE_VIDEO: return "VIDEO";
    case MI_PAGE_AUDIO: return "AUDIO";
    case MI_PAGE_OPTIONS: return "OPTIONS";
    case MI_SCOPE: return "SETTINGS FOR";
    case MI_DEFAULTS: return "RESET TO DEFAULTS";
    case MI_VOLUME: return "VOLUME";
    case MI_SOUND: return "SOUND";
    case MI_PAGE_STATES: return "STATES";
    case MI_PAGE_ABOUT: return "ABOUT";
    case MI_FRAMESKIP: return "FRAMESKIP";
    case MI_STRETCH: return "STRETCH";
    case MI_PROFILER: return "PROFILER";
    case MI_SPRITES: return "SPRITE LIMIT";
    case MI_SAVE: return "SAVE";
    case MI_LOAD: return "LOAD";
    }
    return "";
}

static void menu_draw(uint8_t page, uint8_t sel) {
    uint8_t it[8];
    menu_clear();
    menu_text(1, 2, page == MP_VIDEO ? "VIDEO" : page == MP_AUDIO ? "AUDIO" : page == MP_OPTIONS ? "OPTIONS" : page == MP_STATES ? "STATES" : page == MP_ABOUT ? "ABOUT" : "MENU");
    if (page == MP_ABOUT) {
#ifdef GGSWAN_SF7000
        menu_text(4, 2, "SFSWAN - SC-3000 + SF-7000");
#elif defined(GGSWAN_COLECO)
        menu_text(4, 2, "CVSWAN - COLECOVISION");
#elif defined(GGSWAN_SG1000)
        menu_text(4, 2, "SGSWAN - SG-1000");
#elif defined(GGSWAN_SMS)
        menu_text(4, 2, "SMSSWAN - MASTER SYSTEM");
#else
        menu_text(4, 2, "GGSWAN - GAME GEAR");
#endif
        char pb[10] = "PATCH ";
        uint16_t v = GGSWAN_PATCH;
        uint8_t k = 6;
        if (v >= 100) pb[k++] = '0' + v / 100;
        if (v >= 10) pb[k++] = '0' + (v / 10) % 10;
        pb[k++] = '0' + v % 10;
        pb[k] = 0;
        menu_text(6, 2, pb);
        menu_text(8, 2, "CREATED BY CLAUDE");
        menu_text(10, 2, "WITH GIO-74");
        menu_text(16, 2, "B: BACK");
        return;
    }
    uint8_t n = menu_items(page, it);
    for (uint8_t i = 0; i < n; i++) {
        uint8_t row = 4 + i * 2;
        if (i == sel)
            menu_text(row, 1, ">");
        menu_text(row, 3, menu_label(it[i]));
        if (it[i] == MI_FRAMESKIP) {
            static const char __far fs_names[] = "OFF\0AUTO\0001\0002\0003\0005";
            const char __far *v = fs_names;
            for (uint8_t k = fs_mode; k; k--)
                while (*v++) {}
            menu_text(row, 22, v);
        } else if (it[i] == MI_VOLUME) {
            char t[4] = { (char) ('0' + snd_vol), '/', '8', 0 };
            menu_text(row, 22, t);
        } else if (it[i] == MI_SCOPE) {
            // (on the same row: on the next one it touched the item below)
            menu_text(row, 17, cfg_game ? "THIS GAME" : "ALL GAMES");
        } else if (it[i] == MI_PROFILER) {
            uint8_t l = prof_level();
            menu_text(row, 22, l == 0 ? "OFF" : l == 2 ? "FPS" : "ON");
        } else if (it[i] == MI_SOUND) {
            menu_text(row, 22, snd_mute ? "OFF" : "ON");
        } else if (it[i] >= MI_STRETCH && it[i] <= MI_SPRITES)
            menu_text(row, 22, menu_on(it[i]) ? "ON" : "OFF");
    }
#ifdef GGSWAN_SMS
    // (smsswan: the profiler is written on screen 2, which otherwise
    // carries the game's tiles in front of the sprites: with it on,
    // those are not drawn)
    if (page == MP_VIDEO) {
        menu_text(11, 2, "PROFILER ON/FPS: SPRITES");
        menu_text(12, 2, "IN FRONT OF ALL THE TILES");
    }
#endif
    if (page == MP_VIDEO) {
        char t[4], *p = t;
        uint8_t v = last_fps;
        if (v >= 100) *p++ = '0' + v / 100;
        if (v >= 10) *p++ = '0' + (v / 10) % 10;
        *p++ = '0' + v % 10;
        *p = 0;
        menu_text(14, 3, "GAME FPS");
        menu_text(14, 22, t);
    }
    menu_text(16, 2, page == MP_MAIN ? "A: OPEN  B: CLOSE" : "A: CHANGE  B: BACK");
}

static void menu_wait_frame(void) {
    uint8_t v = vblanks;   // (a WS frame, as frame_step waits)
    while (vblanks == v)
        __asm__ volatile ("hlt");
}

// screen 2 as the plugin has it (the menu wrote all of it)
static void menu_screen2_back(void) {
#if defined(GGSWAN_SG1000)
    for (uint16_t i = 0; i < 32 * 32; i++)
        map2[i] = WS_PRIO_BLANK;
#ifdef GGSWAN_SF7000
    if (kb_open)
        kb_show(1);   // (its glyphs back too)
#elif defined(GGSWAN_COLECO)
    if (cv_kp_open)
        cv_kp_show(1);
#endif
#else
    screen2_mode(drc_st.prio_on);
#endif
}

/*
 * The menu's settings kept on the card (OPTIONS, patch 226): a file for
 * the plugin, /NILESWAN/PLUG_xx.CFG, or one for the game, its ROM's path
 * with ".cfg" (as .sav and .st0). At the start: the game's if there is
 * one (OPTIONS shows THIS GAME then), else the plugin's, else the
 * defaults. Written when the menu closes and something changed; going
 * back from THIS GAME to THE PLUGIN deletes the game's file. 16 bytes:
 * "GC", the version, then the values; another version, or values out of
 * range: the file is not used.
 */
#define CFG_VERSION 1
struct cfg {
    uint8_t magic[2], version;
    uint8_t frameskip, volume, mute, stretch, profiler, sprites;
    uint8_t pad[7];
};
_Static_assert(sizeof(struct cfg) == 16, "cfg: 16 bytes");
#if defined(GGSWAN_SF7000)
static char cfg_plugin_path[] = "/NILESWAN/PLUG_SF.CFG";
#elif defined(GGSWAN_COLECO)
static char cfg_plugin_path[] = "/NILESWAN/PLUG_CV.CFG";
#elif defined(GGSWAN_SG1000)
static char cfg_plugin_path[] = "/NILESWAN/PLUG_SG.CFG";
#elif defined(GGSWAN_SMS)
static char cfg_plugin_path[] = "/NILESWAN/PLUG_SMS.CFG";
#else
static char cfg_plugin_path[] = "/NILESWAN/PLUG_GG.CFG";
#endif
static struct cfg cfg_dflt, cfg_saved;   // (the defaults; as on the card)
static uint8_t cfg_game_saved;
static FIL cfg_fp;

static void cfg_get(struct cfg *c) {
    memset(c, 0, sizeof(*c));
    c->magic[0] = 'G';
    c->magic[1] = 'C';
    c->version = CFG_VERSION;
    c->frameskip = fs_mode;
    c->volume = snd_vol;
    c->mute = snd_mute;
    c->stretch = menu_on(MI_STRETCH);
    c->profiler = prof_level();
    c->sprites = menu_on(MI_SPRITES);
}
static void cfg_set(const struct cfg *c) {
    fs_mode = c->frameskip;
    fs_run = 0;
    snd_vol = c->volume;
    snd_mute = c->mute;
    snd_volume_set();
    // (the switches through the menu's own code: what they also do)
    if (menu_on(MI_STRETCH) != c->stretch) menu_change(MI_STRETCH, 0);
    if (prof_level() != c->profiler) prof_level_set(c->profiler);
    if (menu_on(MI_SPRITES) != c->sprites) menu_change(MI_SPRITES, 0);
}
static void cfg_defaults(void) {
    cfg_set(&cfg_dflt);
}
static uint8_t cfg_valid(const struct cfg *c) {
    return c->magic[0] == 'G' && c->magic[1] == 'C' && c->version == CFG_VERSION
        && c->frameskip <= 5 && c->volume <= 8 && c->mute <= 1
        && c->stretch <= 1 && c->profiler <= 2 && c->sprites <= 1;
}

// the game's file: the ROM's path with ".cfg", put there and taken away
// again (as st_path)
static char cfg_ext[4];
static uint16_t cfg_dot;
static uint8_t cfg_rom_path(uint8_t on) {
    if (on) {
        uint16_t n = 0, dot = 0;
        while (rom_path[n]) {
            if (rom_path[n] == '.') dot = n;
            if (rom_path[n] == '/') dot = 0;
            n++;
        }
        cfg_dot = 0;
        if (!dot || dot + 4 >= ROM_PATH_SIZE) return 0;
        cfg_dot = dot;
        for (uint8_t i = 0; i < 4; i++) cfg_ext[i] = rom_path[dot + 1 + i];
        rom_path[dot + 1] = 'c'; rom_path[dot + 2] = 'f'; rom_path[dot + 3] = 'g'; rom_path[dot + 4] = 0;
        return 1;
    }
    if (cfg_dot)
        for (uint8_t i = 0; i < 4; i++) rom_path[cfg_dot + 1 + i] = cfg_ext[i];
    return 0;
}

// the card's driver moves the cartridge windows: all of them back after,
// from what the plugin knows (as a state brought back does). Patches
// 226-236 read the ports before and wrote them back: the console does
// not read the flash/PSRAM port back (the comment at ram_window_set),
// and slot 2's ROM bank ended up on the SRAM after every save of the
// settings - wrong level data (Mickey through the floor), wrong
// graphics, games stopping (Gio's test, smsswan, Land of Illusion);
// the emulator reads it back, gbswan has it 0 anyway
static void cfg_io_begin(void) {
}
static void cfg_io_end(void) {
#ifdef GGSWAN_SF7000
    outportw(WS_CART_EXTBANK_ROM0_PORT, SG_TABLE_BANK);   // (the VRAM: sg_video.inc)
#else
    outportw(WS_CART_EXTBANK_ROM0_PORT, GG_PSRAM_FIRST_BANK + (drc_st.keys[1] >> 2));
    outportw(WS_CART_EXTBANK_ROM1_PORT, GG_PSRAM_FIRST_BANK + (drc_st.keys[2] >> 2));
#endif
    ws_ram_window_restore();
}

static uint8_t cfg_read_file(const char *path, struct cfg *c) {
    UINT br;
    if (f_open(&cfg_fp, path, FA_READ) != FR_OK)
        return 0;
    uint8_t ok = f_read(&cfg_fp, c, sizeof(*c), &br) == FR_OK && br == sizeof(*c);
    f_close(&cfg_fp);
    return ok && cfg_valid(c);
}
static uint8_t cfg_write_file(const char *path, const struct cfg *c) {
    UINT bw;
    if (f_open(&cfg_fp, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return 0;
    uint8_t ok = f_write(&cfg_fp, c, sizeof(*c), &bw) == FR_OK && bw == sizeof(*c);
    if (f_close(&cfg_fp) != FR_OK) ok = 0;
    return ok;
}

// at the start, before the first frame
static void cfg_load(void) {
    struct cfg c;
    cfg_get(&cfg_dflt);
    cfg_saved = cfg_dflt;
    cfg_io_begin();
    uint8_t found = 0;
    if (cfg_rom_path(1)) {
        found = cfg_read_file(rom_path, &c);
        cfg_rom_path(0);
    }
    if (found) {
        cfg_game = 1;
    } else {
        found = cfg_read_file(cfg_plugin_path, &c);
    }
    cfg_io_end();
    cfg_game_saved = cfg_game;
    if (found) {
        cfg_set(&c);
        cfg_get(&cfg_saved);
    }
}

// the menu closed: to the card if something changed
static void cfg_store(void) {
    struct cfg c;
    cfg_get(&c);
    if (!memcmp(&c, &cfg_saved, sizeof(c)) && cfg_game == cfg_game_saved)
        return;
    cfg_io_begin();
    uint8_t ok;
    if (cfg_game) {
        ok = cfg_rom_path(1) && cfg_write_file(rom_path, &c);
        cfg_rom_path(0);
    } else {
        ok = cfg_write_file(cfg_plugin_path, &c);
        if (cfg_game_saved && cfg_rom_path(1)) {
            f_unlink(rom_path);   // (the game's settings no more)
            cfg_rom_path(0);
        }
    }
    cfg_io_end();
    if (ok) {
        cfg_saved = c;
        cfg_game_saved = cfg_game;
    }
}

static void menu_run(void) {
    uint8_t ctrl = inportb(WS_DISPLAY_CTRL_PORT), back = inportb(0x01);
    uint8_t s2x = inportb(0x12), s2y = inportb(0x13);
    /*
     * The line interrupt writes the scroll of screen 2 too while the
     * game's front tiles are on (drc_st.prio_on: the splits, smsswan's
     * squeezed picture), which moved and squeezed the menu's text (Gio's
     * test of patch 220). Patches 221-235 turned the interrupt off while
     * the menu was open: on the console it did not come back, and its end
     * of the screen is what lets the next frame be presented
     * (frame_waiting): the game stopped after the menu (Gio's test,
     * ggswan, smsswan, sgswan; not gbswan, whose vertical blank sets it
     * up again every frame). Now it runs as always, with prio_on 0 while
     * the menu is open, so it leaves screen 2 alone.
     */
    uint8_t prio = drc_st.prio_on;
    drc_st.prio_on = 0;
    outportb(WS_SOUND_CH_CTRL_PORT, 0);           // (sound off while it is open)
    font_load();   // (sfswan's keyboard puts its own glyphs there)
    menu_blank = font_entry(' ');
    outportb(0x12, 0);
    outportb(0x13, 0);
    outportb(0x01, (FONT_PALETTE << 4) | 2);      // (the backdrop: the font's black)
    uint8_t page = MP_MAIN, sel = 0, main_sel = 0, it[8];
    menu_draw(page, sel);
    outportb(WS_DISPLAY_CTRL_PORT, 0x02);         // screen 2 alone
    uint16_t held = ws_keypad_scan();             // (START + Y3 still held: not presses)
    for (;;) {
        menu_wait_frame();
        uint16_t k = ws_keypad_scan(), p = k & ~held;
        held = k;
        if ((k & (WS_KEY_START | WS_KEY_Y3)) == (WS_KEY_START | WS_KEY_Y3) && (p & (WS_KEY_START | WS_KEY_Y3)))
            break;
        uint8_t n = menu_items(page, it);
        if (p & WS_KEY_B) {
            if (page == MP_MAIN)
                break;
            page = MP_MAIN;
            sel = main_sel;
        } else if ((p & WS_KEY_X1) && sel) {
            sel--;
        } else if ((p & WS_KEY_X3) && sel + 1 < n) {
            sel++;
        } else if (p & (WS_KEY_A | WS_KEY_X2 | WS_KEY_X4)) {
            if (page == MP_ABOUT || !n)
                continue;
            uint8_t item = it[sel];
            if (item >= MI_PAGE_VIDEO && item <= MI_PAGE_ABOUT) {
                if (!(p & WS_KEY_A))
                    continue;
                main_sel = sel;
                page = item == MI_PAGE_VIDEO ? MP_VIDEO : item == MI_PAGE_AUDIO ? MP_AUDIO : item == MI_PAGE_STATES ? MP_STATES : item == MI_PAGE_OPTIONS ? MP_OPTIONS : MP_ABOUT;
                sel = 0;
#ifdef ST_STATES
            } else if (item == MI_SAVE || item == MI_LOAD) {
                if (!(p & WS_KEY_A))
                    continue;
                st_req = item == MI_SAVE ? 1 : 2;   // (done by the main loop, the menu closed)
                break;
#endif
            } else {
                if (item == MI_DEFAULTS && !(p & WS_KEY_A))
                    continue;   // (an action: A only)
                // (the item sees prio_on as the plugin has it; the
                // profiler switches it: taken again after, then 0 again.
                // Patch 236 took it again without putting it back first:
                // any other item left it 0 for good - the profiler on by
                // itself in smsswan and stuck there, Gio's test)
                drc_st.prio_on = prio;
                menu_change(item, (p & WS_KEY_X4) != 0);
                prio = drc_st.prio_on;
                drc_st.prio_on = 0;
            }
        } else {
            continue;
        }
        menu_draw(page, sel);
    }
    // closed: the keys let go first (none of them reaches the game)
    while (ws_keypad_scan())
        menu_wait_frame();
    drc_st.prio_on = prio;
    menu_screen2_back();
#if defined(GGSWAN_SG1000)
    outportb(0x12, s2x);
    outportb(0x13, s2y);
#else
    // (screen2_mode set it: the game's scroll with the front tiles on, 0
    // without. The one saved at the opening was wrong after the profiler
    // switched in the menu: the profiler's text shifted, Gio's test)
    (void) s2x; (void) s2y;
#endif
    outportb(0x01, back);
    outportb(WS_DISPLAY_CTRL_PORT, ctrl);
    cfg_store();   // (the settings to the card, if changed)
    step_keys = 0xFFFF;
    psg_noise_freq();   // (the channels on again)
    for (uint8_t ch = 0; ch < 4; ch++)
        psg_volume(ch);   // (at the menu's volume)
    if (voice_on)
        outportb(WS_SOUND_VOICE_VOL_PORT, voice_vol);
    ia16_disable_irq();
    frame_credit = FRAME_COST;   // (no catching up)
    ia16_enable_irq();
}
#endif
static volatile uint16_t prof_total;

// (the interrupt itself: vblank_irq_entry, drc_asm.s)
void vblank_irq_entry(void);
void vblank_irq(void) {
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
    if (drc_st.prio_on)
        return;   // (screen 2 holds the background's priority tiles)
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

// Profile: share of the samples (percent) in the right border, every
// second (75 samples, ~1.3% each; it was every 8 seconds, and showed the
// translation of a scene start long after it, patch 102): CD translated code, DI dispatcher, PO helpers (ports, memory),
// ST interpreted inside blocks, IN interpreter, TR translating,
// VI end of frame video, AL the rest
static void show_profile(void) {
    static const char __far labels[] = "ALCDDIPOSTINTRVI";   // (enum order)
    uint16_t total = prof_total - prof_count[PROF_IDLE];   // (time spent busy)
    if (prof_total < 75)
        return;
    if (total == 0)
        total = 1;
    for (uint8_t i = 0; i < PROF_IDLE && !fps_only; i++) {   // (FPS only: the counts started again)
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
    if (!fps_only)
        put_value(0, 24, 'P', GGSWAN_PATCH);   // the patch number, top right
    if (diag_slot0)
        put_value(3, 0, 'Z', diag_slot0);  // (see diag_slot0)
#ifdef ST_STATES
    else if (st_note) {}                   // (the state saved / loaded note)
#endif
    else if (fs_mode) {
        // frameskip under the FPS: A (AUTO) or F (fixed), and the most
        // frames left out in a row in the last second
        put_value(3, 0, fs_mode == 1 ? 'A' : 'F', fs_max);
        fs_max = 0;
    } else {
        put_text(3, 0, "    ");
    }
    if (!show_stats || fps_only)
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
    put_value(0, 0, 'X', drc_stat_flushes);
    if (!diag_mode) {
        // the last block translated, when there were some in the last
        // second (which code keeps being translated), else the last pc
        // interpreted
        put_hex(17, 24, drc_stat_blocks != l_blocks ? drc_last_tr_pc : drc_last_interp_pc);
    } else {
        // the right column instead of the profile (Y4 held at the start),
        // one value a row: K lines the VRAM writes took at the end of the
        // screen, M most scroll values in a frame (1: no splits), the
        // parts of video_present (below), G Game Gear interrupts taken
        put_value(1, 24, 'K', diag_commit);
        put_value(3, 24, 'M', diag_splits);
        // WS lines a second (256 cycles each, interrupts included) in the
        // helpers: O OUT (and OUTI runs), I IN, Y the tile log written for
        // a VRAM read, U the PSG queue full; in video_present: S sprites,
        // V all of it
        put_value(5, 24, 'O', drc_stat_op_lines[DRC_H_OUT] + drc_stat_op_lines[DRC_H_OUTI_RUN]
                              + drc_stat_op_lines[DRC_H_OUTD_RUN]);
        put_value(7, 24, 'I', drc_stat_op_lines[DRC_H_IN]);
        put_value(9, 24, 'Y', drc_stat_op_lines[DRC_H_SYNC]);
        put_value(11, 24, 'U', drc_stat_op_lines[DRC_H_PSG]);
        put_value(13, 24, 'S', vp_lines[VP_SPR]);
        put_value(15, 24, 'V', vp_lines[VP_ALL]);
        put_value(17, 24, 'G', drc_stat_irq - l_irq);
    }
    for (uint8_t i = 0; i < VP_N; i++)
        vp_lines[i] = 0;
    for (uint8_t i = 0; i < 16; i++)
        drc_stat_op[i] = drc_stat_op_lines[i] = 0;
    diag_irq_line = 0;
    diag_fg = 0;
    diag_left = 0;
    diag_vbl_pc = 0;
    diag_vbl_st = 0;
    diag_regs = 0;
    diag_ram = 0;
    diag_splits = 0;
    diag_split_line = 0;
    diag_calls = 0;
    diag_late = 0;
    diag_commit = 0;
    diag_ends = 0;
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
#ifdef GGSWAN_SG1000
    memset(TILE_PTR(0), 0, 0x4000);
#else
    memset(gg_vram, 0, 0x4000);
#endif
    text_init();
#ifdef GGSWAN_SF7000
    text_print("SFSWAN 0.1 - SC-3000 + SF-7000");
#elif defined(GGSWAN_COLECO)
    text_print("CVSWAN 0.1 - COLECOVISION");
#elif defined(GGSWAN_SG1000)
    text_print("SGSWAN 0.1 - SG-1000");
#elif defined(GGSWAN_SMS)
    text_print("SMSSWAN 0.1 - MASTER SYSTEM");
#else
    text_print("GGSWAN 0.1 - GAME GEAR");
#endif
    {
        // the patch number, top right
        char pb[12] = "PATCH ";
        uint16_t v = GGSWAN_PATCH;
        uint8_t n = 6;
        if (v >= 100) pb[n++] = '0' + v / 100;
        if (v >= 10) pb[n++] = '0' + (v / 10) % 10;
        pb[n++] = '0' + v % 10;
        pb[n] = 0;
        text_corner(pb);
    }
    text_hex("MASK", inportw(IO_NILE_SEG_MASK));
    // start-up timing (shown above the wait, hold A at the launch to read it)
    outportb(WS_TIMER_CTRL_PORT, 0);
    outportw(WS_TIMER_VBL_RELOAD_PORT, 0xFFFF);   // (also sets the counter)
    outportb(WS_TIMER_CTRL_PORT, WS_TIMER_CTRL_VBL_REPEAT);
    uint16_t t_sd, t_rom;

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
    text_title(rom_path);
    if (!rom_path[0])
        fail("NO PATH", 0);

    // The card at the fast SPI clock: swanshell starts the plugin with the
    // cartridge clock (slow: about 40 KB/s, 6.5 s for a 256 KB ROM).
    // disk_initialize keeps the clock it finds once the card is set up.
    outportw(IO_NILE_SPI_CNT, NILE_SPI_CLOCK_FAST | NILE_SPI_DEV_NONE);
    int16_t result = f_mount(&fs, "", 1);
    if (result != FR_OK)
        fail("MOUNT", result);
    text_print("SD OK");
    t_sd = t_frames();

    uint16_t banks = load_rom(rom_path);
    t_rom = t_frames();
#ifdef GGSWAN_SF7000
    {
        // A disk with no system on it ("SYS:" at its start) does not start:
        // Disk BASIC comes from another one, /NILESWAN/SF7000.SF (any disk
        // that starts by itself), and this one goes in the drive when that
        // one's loader has finished. The same with B held now, for a disk
        // that is to be started from Disk BASIC with BOOT.
        // And a disk whose system is not Disk BASIC itself (the name after
        // "SYS:": Kamikaze's is "BASIC LEVEL IIIB+KAMIKAZE") is one made to
        // be started from Disk BASIC with BOOT, as its directory says:
        // started by itself its screen comes out mirrored. From the system
        // disk too, and BOOT typed for it when Disk BASIC is up.
        static const char __far basic_name[] = "SYS: disk BASIC";
        uint8_t first[15];
        outportw(WS_CART_EXTBANK_ROM1_PORT, SF_DISK_BANK);
        _fmemcpy(first, MK_FP(0x3000, 0), 15);
        uint8_t sys = first[0] == 'S' && first[1] == 'Y' && first[2] == 'S' && first[3] == ':';
        uint8_t is_basic = sys;
        for (uint8_t i = 4; i < 15 && is_basic; i++)
            if (first[i] != (uint8_t) basic_name[i])
                is_basic = 0;
        sf_auto_boot = sys && !is_basic;
        if (!is_basic || (ws_keypad_scan() & WS_KEY_B)) {
            static const char sys_path[] = "/NILESWAN/SF7000.SF";
            text_print(sf_auto_boot ? "A DISK TO START WITH BOOT"
                       : sys ? "B HELD: DISK BASIC FIRST" : "NO SYSTEM ON THIS DISK");
            text_print("STARTING FROM /NILESWAN/SF7000.SF");
            text_print("(A COPY OF A DISK WITH DISK BASIC)");
            load_bank = SF_SYS_BANK;
            load_rom(sys_path);
            sf_disk_bank = SF_SYS_BANK;
            sf_swap_pending = 1;
        }
    }
#endif
    text_hex("BANKS", banks);

#ifdef GGSWAN_SG1000
    // Y1 (Y up) held: the diagnostics, over the sides of the picture
    if (ws_keypad_scan() & WS_KEY_Y1) {
        sg_diag = 1;
        text_print("DIAGNOSTICS");
    }
#elif defined(GGSWAN_SMS)
    // Y1 (Y up) held: the diagnostics instead of the background priority
    // (both use screen 2)
    if (ws_keypad_scan() & WS_KEY_Y1) {
        prio_wanted = 0;
        text_print("DIAGNOSTICS, NO PRIORITY");
    }
#else
    // Y1 (Y up) held: background priority on screen 2 (see screen2_mode;
    // it used to select the interpreter, no longer needed)
    if (ws_keypad_scan() & WS_KEY_Y1) {
        prio_wanted = 1;
        text_print("BACKGROUND PRIORITY");
    }
#endif
    // CPU: the recompiler
    {
        // the code cache: the PSRAM bank behind linear segment DRC_CODE_SEG
        uint16_t code_bank = (((uint16_t) inportb(0xC0 /* linear bank */) << 4)
            | (DRC_CODE_SEG >> 12)) & (old_mask & NILE_SEG_MASK_ROM_MASK);
        text_hex("CODE BANK", code_bank);
        // the ROM takes 64 KiB banks from GG_PSRAM_FIRST_BANK up: the code
        // cache (segments 4000h-A000h) must start above them
#ifdef GGSWAN_SF7000
        if (code_bank < GG_PSRAM_FIRST_BANK + SF_BANKS)
            fail("CODE BANK", code_bank);
#else
        if (code_bank < GG_PSRAM_FIRST_BANK + ((banks + 3) >> 2))
            fail("CODE BANK", code_bank);
#endif
#ifdef GGSWAN_SG1000
        // (and above the bank of the screen's tables: sg_video.inc)
        if (code_bank <= SG_TABLE_BANK)
            fail("CODE BANK", code_bank);
#endif
        // X4 (X left) held: diagnostics, the recompiler without patches
        // 29-32 (see drc_rt.h)
        if (ws_keypad_scan() & WS_KEY_X4)
            drc_no_fast = 1;
        drc_rt_init(code_bank);
        // the stubs written through the RAM window must be visible there
        // (the EXIT stub starts with mov byte [reason], 0: C6h)
        if (*((const uint8_t __far*) MK_FP(DRC_CODE_SEG, 0)) != 0xC6)
            fail("CODE READ", code_bank);
        uint8_t bad = drc_rt_test_banks();
        if (bad)
            fail("BANK TEST", bad);
        // X3 (X down) held: time the same code in IRAM and in PSRAM
        if (ws_keypad_scan() & WS_KEY_X3) {
            uint16_t r[6];
            drc_rt_bench(r);
            // (on a screen of its own: after the lines already shown the
            // six values did not fit and were all written on the last
            // row - first run on a WonderSwan, 2026-11-16)
            memset(map2, 0, MAP_SIZE);
            text_y = 1;
            text_print("BENCH IRAM/PSRAM LINES");
            text_hex("MIX IRAM", r[0]); text_hex("MIX PSRAM", r[1]);
            text_hex("JMP8 IRAM", r[2]); text_hex("JMP8 PSRAM", r[3]);
            text_hex("JMP16 IRAM", r[4]); text_hex("JMP16 PSRAM", r[5]);
            text_print("PRESS A");
            while (!(ws_keypad_scan() & WS_KEY_A)) {}
        }
#ifndef GGSWAN_SF_INTERP   // (sfswan, a test build: the interpreter only)
        gg_cpu_run = drc_z80_run;
        gg_cpu_left = drc_cycles_left;
#endif
        show_stats = true;
        // Y4 (Y left) held: the diagnostics column instead of the profile
        if (ws_keypad_scan() & WS_KEY_Y4)
            diag_mode = true;

        if (ws_keypad_scan() & WS_KEY_Y3) {
            drc_rt_set_no_links(1);
            drc_check = 1;
            text_print("CPU: RECOMPILER, SELF-CHECK");
        } else if (ws_keypad_scan() & WS_KEY_Y2) {
            drc_rt_set_no_links(1);
            text_print("CPU: RECOMPILER, NO LINKS");
        } else if (ws_keypad_scan() & WS_KEY_X1) {
            drc_rt_options(1, 1);
            text_print("CPU: RECOMPILER, NO CALL INLINE");
        } else if (ws_keypad_scan() & WS_KEY_X2) {
            drc_rt_options(0, 0);
            text_print("CPU: RECOMPILER, JP/JR FOLLOW (TEST)");
        } else if (drc_no_fast) {
            text_print("CPU: RECOMPILER, NO FAST PATHS");
        } else {
            text_print("CPU: RECOMPILER");
        }
    }

    {
        // milliseconds from the plugin's start: mounting the card, reading
        // the ROM, the rest up to here
        uint16_t t_end = t_frames();
        outportb(WS_TIMER_CTRL_PORT, 0);
        char t[48];
        uint8_t n = put_ms(t, 0, "SD", t_sd);
        n = put_ms(t, n, "ROM", t_rom - t_sd);
        n = put_ms(t, n, "INIT", t_end - t_rom);
        t[n++] = 'M'; t[n++] = 'S';
        t[n < 28 ? n : 28] = 0;
        text_print(t);
    }

    // no fixed wait: the text stays while a key is held (the keys that
    // choose the CPU modes above, or any other one to read it)
    while (ws_keypad_scan()) {
        while (inportb(WS_DISPLAY_LINE_PORT) != 144) {}
        while (inportb(WS_DISPLAY_LINE_PORT) == 144) {}
    }

#ifdef GGSWAN_COLECO
    memset(gg_ram, 0, 0x400);
#else
    memset(gg_ram, 0, 0x2000);
#endif
#ifndef GGSWAN_SG1000
    drc_st.hs_last = 16;   // (rec_val[0] = 0: register value 16)
    rec_n = 1;
#endif
    video_init();
    sound_init();
#ifdef GGSWAN_SF7000
    // the SF-7000: its RAM (the RAM window is on it since video_init), the
    // built-in IPL, the disk loaded in the drive
    sf_ram = MK_FP(0x1000, 0x0000);
    sf_ipl = sf_ipl_builtin;
    sf_ipl_size = sizeof(sf_ipl_builtin);
    sf_ipl_fast = 1;
    sf_ipl_font = SF_IPL_FONT;
    sf.inserted = 1;
    gg_init(1);
#else
    gg_init(banks);
    if (gg_cpu_run == drc_z80_run)
        drc_rt_rom_banks(banks);
#endif

    cfg_load();   // (the menu's settings: the game's, the plugin's)
#ifdef ST_STATES
    st_file_read();
    st_windows_restore();
#endif
    ia16_disable_irq();
    ws_int_set_handler(WS_INT_VBLANK, (ia16_int_handler_t) vblank_irq_entry);
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
        read_keys(frame_step(ws_keypad_scan()));   // (the keys read once a frame)
        if (menu_req) {
            menu_req = 0;
            menu_run();
        }
#ifdef ST_STATES
        if (st_req) {
            uint8_t r = st_req;
            st_req = 0;
            if (r == 1) state_save();
            else state_load();
        }
#endif
        gg_run_frame();   // (video_present at its line 168)
#ifdef GGSWAN_NILEDBG
        // niledebug (USB debugger, build with NILEDBG=1, inject with
        // --hook-offset none --pow read): the link served here, once a
        // frame, where nothing else uses the cartridge's SPI (the VBlank
        // hook does not fit: vblank_irq's CS is not the bank's)
        niledbg_poll();
#endif
        frames++;
        // frames per second, in the left border (the WS shows 75.47 Hz)
        if (vblanks >= 75) {
            vblanks -= 75;
            last_fps = frames;
#ifdef ST_STATES
            if (st_note && !--st_note && !stats_hide) put_text(3, 0, "    ");
#endif
#ifdef GGSWAN_SF7000
            if (!kb_open)   // (the keyboard has the font's tiles)
#endif
            if (!stats_hide) {
            show_fps(frames);
            if (!diag_mode)
                show_profile();
            }
#ifdef GGSWAN_COLECO
            if (cv_kp_open)
                cv_kp_draw();
#endif
            frames = 0;
        }
    }
}
#endif
