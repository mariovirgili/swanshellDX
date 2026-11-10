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
uint8_t *gg_vram = iram + GG_VRAM_ADDR;
#define map1 ((uint16_t*) (iram + 0x2000))
#define map2 ((uint16_t*) (iram + 0x2800))
#define sprite_table (iram + 0x3000)
#define wave_table (iram + 0x3200)
#define sprite_table_b (iram + 0x3400)
#define MAP_SIZE 2048
#else
uint8_t *gg_ram = (uint8_t*) GG_RAM_ADDR;
uint8_t *gg_vram = (uint8_t*) GG_VRAM_ADDR;

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

// s at the bottom right of the text screen (the patch number at start)
static void text_corner(const char __far *s) {
    uint8_t n = 0;
    while (s[n]) n++;
    uint8_t y = text_y;
    text_y = 17;
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
        uint8_t c = ((uint16_t) (a - nt_base) < 0x700 ? 1 : 0)
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
        if (off < 0x700 || (uint16_t) (a + n - 1 - nt_base) < 0x700) {
            // name table: the words touched
            for (uint16_t k = 0; k < n; k++) {
                uint16_t o = (uint16_t) (a + k - nt_base);
                if (o < 0x700 && (!(o & 1) || k == 0))
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
    uint16_t f = irq_save();
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
    irq_restore(f);
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
GGSWAN_HOST_VISIBLE volatile uint8_t act_line[MAX_SPLITS], act_val[MAX_SPLITS], act_n = 1;
// screen 1 Y scroll for the next WS frame (set by the line interrupt at the
// end of the screen, like act_val[0]: no tear while the WS draws)
static volatile uint8_t act_vy;
static volatile uint8_t act_i;
// the splits and the Y scroll of the frame just presented, waiting for the
// end of the WS screen (pend_n: 0 none). They replaced the active list at
// once, in the middle of the WS screen being drawn, whose line interrupts
// went on with the new list from where they were in the old one: the new
// values on the old list's lines (or the rest skipped, a shorter list),
// for one WS screen: the road and the status bar of Super Monaco GP
// jerking sideways now and then, half a screen of it.
#ifndef GGSWAN_HOST
static uint8_t pend_line[MAX_SPLITS], pend_val[MAX_SPLITS];
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
    for (uint8_t r = 0; r < 28; r++) {
        uint8_t k = 0;
        drc_st.nt_rows[r] = drc_st.nt_rows[28 + r] = 0xFFFF;
        for (uint8_t v = r; v < top + 32; v += 28)
            if (v >= top)
                drc_st.nt_rows[k++ * 28 + r] = (v & 31) * 64;
    }
    for (uint8_t k = 0; k < 32; k++) {
        uint8_t w = (first + k) & 31;
        uint8_t v = top + ((w - top) & 31);
        uint8_t r = v % 28;
        // (a name table row does not wrap in VRAM: 3800h + 27 * 64 at most)
        const uint8_t *p = gg_vram + nt_base + r * 64;
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

static void map_put(uint8_t row, uint8_t col, uint16_t e) {
    uint16_t w = conv_entry(e), w2 = prio_entry(e, w);
    for (uint8_t v = row; v < map_top + 32; v += 28)
        if (v >= map_top) {
            map1[(v & 31) * 32 + col] = w;
            if (drc_st.prio_on)
                map2[(v & 31) * 32 + col] = w2;
        }
}

void gg_platform_vram_write(uint16_t a, uint8_t value) {
    (void) value;
    uint16_t off = a - nt_base;
    if (off < 0x700) {
        map_put(off >> 6, (off >> 1) & 31, gg_entry(off >> 6, (off >> 1) & 31));
    } else if ((uint16_t) (a - sat_base) < 0x100) {
        drc_st.sprites_dirty = true;
    }
}

void gg_platform_cram_write(uint8_t i) {
    uint16_t c = gg_color12(i);
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
        // (tile bytes still in the logs may be in the new name table: to
        // VRAM first, so that map_relayout reads them; the log copy does
        // not update the map)
        if ((uint16_t) ((v & 0x0E) << 10) != nt_base)
            gg_platform_vram_sync();
        nt_base = (v & 0x0E) << 10;
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
// no room for another split: the one whose value is closest to the split
// before it is dropped (its lines take that value), so what comes after
// is still recorded with the least change. Before, the splits after the
// 24th were dropped: Super Monaco GP II (a road of about 70 lines, each
// with its value, then its status bar) had the status bar scrolling with
// the road's last value. (Dropping every other one instead lost too much
// of Super Monaco GP's road.)
static void splits_drop_one(void) {
    uint8_t best = 1, best_d = 0xFF;
    for (uint8_t i = 1; i < rec_n; i++) {
        uint8_t d = rec_val[i] - rec_val[i - 1];
        if ((int8_t) d < 0)
            d = -d;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    for (uint8_t i = best; i + 1 < rec_n; i++) {
        rec_line[i] = rec_line[i + 1];
        rec_val[i] = rec_val[i + 1];
    }
    rec_n--;
}

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
    if (line > GG_PRESENT_LINE) {
        // after the view (the frame was presented at line 168): the value
        // for the next frame's first line
        rec_val[0] = v;
        rec_n = 1;
        drc_st.hs_last = value;
        return;
    }
    uint16_t y = line - GG_VIEW_Y;
    if (y >= GG_VIEW_H || v == rec_val[rec_n - 1])
        return;
    // (diagnostics, as the generated routine)
    drc_st.diag_fg = (uint16_t) gg.vdp.frame_given;
    drc_st.diag_left = gg_cpu_left ? (int16_t) gg_cpu_left() : (int16_t) z80.cycles;
    if (rec_n > 1 && rec_line[rec_n - 1] == y) {
        rec_val[rec_n - 1] = v;
    } else {
        if (rec_n >= MAX_SPLITS)
            splits_drop_one();
        rec_line[rec_n] = y;
        rec_val[rec_n] = v;
        rec_n++;
    }
    drc_st.hs_last = value;
}

// diagnostics (X4 screen, right column): the end-of-screen interrupt's
// worst lateness and VRAM-write time in lines, and how many per second
#define WS_LINES 159
static volatile uint8_t diag_late, diag_commit, diag_ends, diag_splits, diag_split_line;
static volatile uint16_t diag_calls;
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
// (the interrupt itself: line_irq_entry, drc_asm.s)
void line_irq_entry(void);
void line_irq(void) {
    uint8_t i = act_i;
    uint8_t end = 0;   // (the end of the screen done in this interrupt)
    uint8_t l0 = inportb(WS_DISPLAY_LINE_PORT);
    diag_calls++;
    if (inportb(WS_DISPLAY_LINE_IRQ_PORT) >= GG_VIEW_H) {
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
                }
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

static void splits_commit(void) {
#ifndef GGSWAN_HOST
    uint16_t f = irq_save();   // (video_present calls it with interrupts off)
#endif
#ifndef GGSWAN_HOST
    // (to the line interrupt at the end of the WS screen)
    for (uint8_t i = 0; i < rec_n; i++) {
        pend_line[i] = rec_line[i];
        pend_val[i] = rec_val[i];
    }
    pend_n = rec_n;
#else
    for (uint8_t i = 0; i < rec_n; i++) {
        act_line[i] = rec_line[i];
        act_val[i] = rec_val[i];
    }
    act_n = rec_n;
    act_vy = next_vy;
#endif
    if (rec_n > diag_splits)
        diag_splits = rec_n;
    if (rec_n > 1) {
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
#ifndef GGSWAN_HOST
    irq_restore(f);
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
    // (every other tile of the view: 18 rows, 20 columns; smsswan 28)
    for (uint8_t r = 0; r < GG_VIEW_H / 8; r += 2) {
        uint8_t row = (top + r) % 28;
        for (uint8_t c = 0; c < GG_VIEW_W / 8; c += 2)
            if (gg_entry(row, (left + c) & 31) & 0x800)
                sprite_pal++;
    }
    backdrop_palette = sprite_pal > (GG_VIEW_H / 16) * (GG_VIEW_W / 16) / 2 ? 1 : 0;
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
uint8_t spr_par[4], spr_cnt;   // (spr_build's)
uint8_t *spr_build(const uint8_t *sat, uint8_t *table);
#endif
static uint8_t spr_port04, spr_count, spr_ready;
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
    uint8_t *s = spr_build(sat, table);
#else
    uint8_t *s = table, *end = table + 127 * 4;
    for (uint8_t i = 0; i < 64 && s < end; i++) {
        uint8_t y = sat[i];
        if (y == 0xD0)
            break;
        // Game Gear (x, y + 1) in the 256x192 picture; the view starts at
        // (48, 24) and is shown at WS (32, 0) (smsswan: (16, 24) at (0, 0))
        uint8_t wy = y + 1 - GG_VIEW_Y;
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
            s[4] = t | 1; s[5] = tbase >> 8; s[6] = wy + 8; s[7] = wx;
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

// the sprite table built to the WS (it copies it at the end of the screen)
static void sprites_publish(void) {
    if (!spr_ready)
        return;
    spr_ready = 0;
    uint16_t f = irq_save();
    outportb(0x04, spr_port04);
    outportb(0x05, 0);        // first sprite
    outportb(0x06, spr_count);   // sprite count
    irq_restore(f);
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
#define prio_wanted 1
#else
static uint8_t prio_wanted;   // (main: Y1 held; the PC tests: GGSWAN_PRIO)
#endif
static void screen2_mode(uint8_t prio) {
    drc_st.prio_on = prio;
    if (prio) {
        map_relayout(map_top, 0);   // (map1 and map2)
        outportb(0x12, inportb(0x10));
        outportb(0x13, inportb(0x11));
    } else {
        for (uint16_t i = 0; i < 32 * 32; i++)
            map2[i] = TILE_ENTRY(MASK_TILE) | PAL_ENTRY(MASK_PALETTE);
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
#if defined(GGSWAN_HOST) && !defined(GGSWAN_SMS)
    prio_wanted = getenv("GGSWAN_PRIO") != NULL;
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
void gg_platform_frame_end(void) {
    video_present();
}

static void pcm_frame(void);   // (the frame's samples: below)

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
    uint8_t vp_start = inportb(WS_DISPLAY_LINE_PORT);
    vp_mark = vp_start;
    gg_platform_psg_flush();   // (the frame's PSG writes, queued)
    pcm_frame();
    vp_lap(VP_PSG);

    // vertical: the Game Gear view starts at row (24 + vscroll) mod 224
    uint16_t s = (GG_VIEW_Y + gg.vdp.vscroll_frame) % 224;
    uint16_t vy = s;
    while (vy < map_top * 8u)
        vy += 224;   // (virtual rows: the same Game Gear row 28 later)
    next_vy = vy & 0xFF;
#ifndef GGSWAN_HOST
    uint8_t relay = 0, relay_top = 0;
#endif
    if (vy > map_top * 8u + (256 - GG_VIEW_H - 8)) {
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
        vy = s + 224;
        next_vy = vy & 0xFF;
#ifdef GGSWAN_HOST
        // (the PC tests have no WS raster: the map and its scroll now)
        map_relayout((s >> 3) + 28 - 6, 0);
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
        relay_top = (s >> 3) + 28 - 6;
#endif
    }
#ifdef GGSWAN_HOST
    outportb(0x11, next_vy);   // (the PC tests have no WS raster)
#endif
    vp_lap(VP_MAP);
    // the sprite table first (built, not given to the WS yet)
    if (drc_st.sprites_dirty)
        build_sprites();
    vp_lap(VP_SPR);
    // this frame's VRAM writes (at the end of the WS screen) and its
    // sprite table (the WS copies it there) together, both for the same WS
    // frame: never on the last lines of the screen, where the sprite table
    // missed the copy (line 144 on the WS, 142 in Mednafen) while the
    // VRAM writes still made the end-of-screen interrupt (patch 92: checked
    // only at the start of video_present, a map relayout or the sprite
    // table could still end there: Aladdin's new pose on the old sprites
    // now and then). Then the scroll (splits_commit)
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
    sprites_publish();
    splits_commit();
#endif
    // record the next frame: it starts with the current value
    rec_val[0] = 16 - gg.vdp.reg[8];
    rec_n = 1;
    drc_st.hs_last = gg.vdp.reg[8];
    vp_lap(VP_LOG);
    if (!(gg.frame & 7))
        update_backdrop();
    vp_lap(VP_BACK);

    // display on: at the start of the frame or now (games such as Sonic
    // turn it off in the frame interrupt while they fill the VRAM, and on
    // again before the next frame: at this point it can still be off)
    if ((reg[1] | gg.vdp.reg1_frame) & 0x40) {
        outportb(0x01, backdrop_palette << 4);        // backdrop: color 0
        // (screen 2: the tiles with priority inside the view, or the
        // borders' mask and diagnostics outside it)
        outportb(0x00, 0x01 | 0x02 | 0x04 | 0x20 | (drc_st.prio_on ? 0 : 0x10));
    } else {
        outportb(0x01, 0x10 | (reg[7] & 0x0F));       // backdrop color
        outportb(0x00, drc_st.prio_on ? 0 : 0x02 | 0x10 | 0x20);
    }
#ifdef GGSWAN_HOST
    frame_waiting = 1;
#endif
    vp_mark = vp_start;
    vp_lap(VP_ALL);
    prof_phase = PROF_OTHER;
}

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

static void psg_volume(uint8_t ch) {
    if (voice_on && ch < 3) {
        // (channel 2's volume port is the voice's sample now)
        if (ch != 1)
            outportb(0x88 + ch, 0);
        return;
    }
    outportb(0x88 + ch, psg_volume_table[psg_vol[ch]] * ((psg_stereo >> ch) & 0x11));
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
            // (a table: no 32-bit multiplication)
            outportw(0x80 + ch * 2, -psg_divider[psg_tone[ch]]);
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
        outportb(WS_SOUND_VOICE_VOL_PORT, WS_SOUND_VOICE_VOL_LEFT_FULL | WS_SOUND_VOICE_VOL_RIGHT_FULL);
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

GGSWAN_HOST_VISIBLE void read_keys(void) {
    uint16_t k = ws_keypad_scan();


    uint8_t g = 0;
    if (k & WS_KEY_X1) g |= GG_KEY_UP;
    if (k & WS_KEY_X3) g |= GG_KEY_DOWN;
    if (k & WS_KEY_X4) g |= GG_KEY_LEFT;
    if (k & WS_KEY_X2) g |= GG_KEY_RIGHT;
    if (k & WS_KEY_B) g |= GG_KEY_1;
    if (k & WS_KEY_A) g |= GG_KEY_2;
#ifdef GGSWAN_SMS
    // START is the Pause button, pressed for the game when it is let go
    // (one frame: gg_run_frame takes the edge) unless Y1 (Y up) was held
    // with it: START + Y1 goes back to the menu
    {
        static uint8_t start_held, start_y1;
        if (k & WS_KEY_START) {
            if (!start_held) { start_held = 1; start_y1 = 0; }
            if (k & WS_KEY_Y1) start_y1 = 1;
        } else {
            if (start_held && !start_y1) g |= GG_KEY_START;
            start_held = 0;
        }
    }
#else
    if (k & WS_KEY_START) g |= GG_KEY_START;
#endif
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

static volatile uint8_t vblanks;
// frame limiter: the Game Gear shows 59.92 frames a second, the WS 75.47:
// every WS frame earns 5992, a Game Gear frame costs 7547
#define FRAME_EARN 5992
#define FRAME_COST 7547
static volatile uint16_t frame_credit;
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
static uint16_t step_pressed(void) {
    uint16_t k = ws_keypad_scan();
    uint16_t p = k & ~step_keys;
    step_keys = k;
    return p;
}
#define STEP_PAUSE (WS_KEY_Y2 | WS_KEY_Y4)
// START + Y1 (Y up) held for a second: back to the menu (nileswan soft
// reset: the menu starts again and writes the cartridge RAM, in SRAM, to
// the game's .sav; the Game Gear has no Y buttons)
static uint8_t exit_held;
static void exit_check(void) {
    if ((ws_keypad_scan() & (WS_KEY_START | WS_KEY_Y1)) != (WS_KEY_START | WS_KEY_Y1)) {
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

static void frame_step(void) {
    exit_check();
    uint16_t p = step_pressed();
    if ((p & STEP_PAUSE) && (step_keys & STEP_PAUSE) == STEP_PAUSE)
        step_paused = 1;
    else if (!step_paused)
        return;
    outportb(WS_SOUND_CH_CTRL_PORT, 0);   // (sound off while stopped)
    for (;;) {
        // the Game Gear frame number (left border, F: last 3 digits)
        put_value(3, 0, 'F', gg.frame % 1000);
        // a WS frame (the vblank count: polling the line counter for 144
        // missed it, the line and vblank interrupts being served there)
        uint8_t v = vblanks;
        while (vblanks == v)
            __asm__ volatile ("hlt");
        p = step_pressed();
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
    put_value(0, 24, 'P', GGSWAN_PATCH);   // the patch number, top right
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
    memset(gg_vram, 0, 0x4000);
    text_init();
#ifdef GGSWAN_SMS
    text_print("SMSSWAN 0.1 - MASTER SYSTEM");
#else
    text_print("GGSWAN 0.1 - GAME GEAR");
#endif
    {
        // the patch number, bottom right
        char pb[12] = "PATCH ";
        uint8_t v = GGSWAN_PATCH, n = 6;
        if (v >= 100) pb[n++] = '0' + v / 100;
        if (v >= 10) pb[n++] = '0' + (v / 10) % 10;
        pb[n++] = '0' + v % 10;
        pb[n] = 0;
        text_corner(pb);
    }
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

#ifndef GGSWAN_SMS
    // Y1 (Y up) held: background priority on screen 2 (see screen2_mode;
    // it used to select the interpreter, no longer needed; smsswan: always)
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
        if (code_bank < GG_PSRAM_FIRST_BANK + ((banks + 3) >> 2))
            fail("CODE BANK", code_bank);
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
            text_print("BENCH IRAM/PSRAM LINES");
            text_hex("MIX IRAM", r[0]); text_hex("MIX PSRAM", r[1]);
            text_hex("JMP8 IRAM", r[2]); text_hex("JMP8 PSRAM", r[3]);
            text_hex("JMP16 IRAM", r[4]); text_hex("JMP16 PSRAM", r[5]);
            text_print("PRESS A");
            while (!(ws_keypad_scan() & WS_KEY_A)) {}
        }
        gg_cpu_run = drc_z80_run;
        gg_cpu_left = drc_cycles_left;
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

    // give time to read the text
    for (uint16_t i = 0; i < 60; i++) {
        while (inportb(WS_DISPLAY_LINE_PORT) != 144) {}
        while (inportb(WS_DISPLAY_LINE_PORT) == 144) {}
    }

    memset(gg_ram, 0, 0x2000);
    drc_st.hs_last = 16;   // (rec_val[0] = 0: register value 16)
    rec_n = 1;
    video_init();
    sound_init();
    gg_init(banks);
    if (gg_cpu_run == drc_z80_run)
        drc_rt_rom_banks(banks);

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
        frame_step();
        read_keys();
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
            show_fps(frames);
            if (!diag_mode)
                show_profile();
            frames = 0;
        }
    }
}
#endif
