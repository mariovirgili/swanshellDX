/**
 * gbswan - Game Boy (DMG) plugin for swanshell (nileswan).
 *
 * The menu leaves the .gb file's path in the IPC area and starts this. The
 * ROM is read from the SD card into the nileswan's PSRAM (64 KiB banks
 * from 0: four of the Game Boy's 16 KiB banks each), which the Game Boy's
 * 0000h and 4000h reach through the cartridge's ROM windows; its
 * cartridge RAM is the SRAM behind the RAM window. The machine is
 * core/gbf_hw.c with the interpreter of core/gb_cpu.c; the picture is
 * gb_video.c. GB-MEMORY-MAP.md has where everything is.
 *
 * Keys: the X pad is the Game Boy's; A and B; START; Y3 (Y down) or Y2
 * (Y right) is SELECT. START + Y1 (Y up) held for a second: back to the
 * menu. START + Y4 (Y left): figures on the screen (gb_video.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>
#include <wonderful.h>
#include <ws.h>
#include <nile.h>
#include <nilefs.h>
#include "gb.h"
#include "gbv.h"
#include "gb_ws.h"
#include "gb_drc.h"
#include "../../ws/version.h"

#define ROM_MAX_BANKS 64        // of 16 KiB: 1 MiB (PSRAM banks 0-15)
#define PATH_SIZE 260

uint16_t gbf_rom0_off, gbf_romx_off, gbf_ram_off;

void gbf_plat_map(void) {
    outportw(WS_CART_EXTBANK_ROM0_PORT, gb.rom0_bank >> 2);
    gbf_rom0_off = (gb.rom0_bank & 3) << 14;
    outportw(WS_CART_EXTBANK_ROM1_PORT, gb.romx_bank >> 2);
    gbf_romx_off = (gb.romx_bank & 3) << 14;
    outportw(WS_CART_EXTBANK_RAM_PORT, gb.ram_bank >> 3);
    gbf_ram_off = (gb.ram_bank & 7) << 13;
    // (the recompiled code's way to the two ROM areas: segments the Game
    // Boy's address is the offset in)
    drc_segtab[0] = 0x2000 + (gbf_rom0_off >> 4);
    drc_segtab[1] = 0x3000 + (gbf_romx_off >> 4) - 0x400;
    drc_key0 = gb.rom0_bank + 1;
    drc_keyx = gb.romx_bank + 1;
    drc_hash0 = drc_key0 << 5;      // (their part of the table's hash)
    drc_hashx = drc_keyx << 5;
    // the cartridge's RAM for the recompiled code: 1 plain RAM that can
    // be reached, 2 none or disabled (reads FFh, writes lost), 0 ask C
    // (MBC2's half bytes, MBC3's clock)
    drc_ram_ok = gb.mbc == 2 || (gb.mbc == 3 && gb.rtc_sel) ? 0 : gb.ram_banks && (gb.ram_on || !gb.mbc) ? 1 : 2;
    drc_ram_seg = 0x1000 + (gbf_ram_off >> 4) - 0xA00;
    // the write that changes the second ROM area's bank (gb_drc_asm.s:
    // s_wrt): 2000h up to drc_mbc_top (its high byte), the bits of the
    // value that count, whether 0 means 1, the other register's bits
    drc_mbc_top = gb.mbc == 1 || gb.mbc == 3 ? 0x40 : gb.mbc == 5 ? 0x30 : 0;
    drc_mbc_and = gb.mbc == 1 ? 0x1F : gb.mbc == 3 ? 0x7F : 0xFF;
    drc_mbc_z1 = gb.mbc != 5;
    drc_mbc_hi = gb.mbc == 1 ? (uint16_t) gb.bank_hi << 5 : gb.mbc == 5 ? (uint16_t) (gb.mbc1_mode & 1) << 8 : 0;
    drc_bank_mask = gb.rom_banks - 1;
}

/* ---- text (before the game, and when something is wrong): screen 2 with
   the font of the other plugins at tiles 928 and up (5A00h) ---- */

#include "../../ws/font.inc"
#define TEXT_MAP ((uint16_t*) 0x4800)
#define FONT_TILE 928
static uint8_t text_row;

static void text_init(void) {
    outportb(WS_DISPLAY_CTRL_PORT, 0);
    ws_system_set_mode(WS_MODE_COLOR);
    for (uint8_t i = 0; i < sizeof(font_data) / 8; i++) {
        uint8_t *t = (uint8_t*) 0x2000 + (uint16_t) (FONT_TILE + i) * 16;
        for (uint8_t r = 0; r < 8; r++) {
            t[r * 2] = font_data[i * 8 + r];
            t[r * 2 + 1] = 0;
        }
    }
    for (uint16_t i = 0; i < 32 * 32; i++)
        TEXT_MAP[i] = 0x2000 | (FONT_TILE & 511);
    WS_DISPLAY_COLOR_MEM(0)[0] = 0xFFF;
    WS_DISPLAY_COLOR_MEM(0)[1] = 0x000;
    outportb(WS_SCR_BASE_PORT, (0x4800 >> 11) << 4 | (0x4800 >> 11));
    outportb(0x12, 0);
    outportb(0x13, 0);
    outportb(0x01, 0);
    outportb(WS_DISPLAY_CTRL_PORT, WS_DISPLAY_CTRL_SCR2_ENABLE);
    text_row = 1;
}

static void text_put(uint8_t row, uint8_t col, const char __far *s) {
    for (; *s && col < 28; s++, col++) {
        char c = *s;
        if (c >= 'a' && c <= 'z') c -= 32;
        uint8_t k = 0;
        for (uint8_t i = 0; font_chars[i]; i++)
            if (font_chars[i] == c) { k = i; break; }
        TEXT_MAP[row * 32 + col] = 0x2000 | ((FONT_TILE + k) & 511);
    }
}
static void text_print(const char __far *s) {
    if (text_row < 17) text_put(text_row++, 1, s);
}
static void text_hex(const char __far *label, uint32_t v) {
    static const char __far digits[] = "0123456789ABCDEF";
    char buf[9];
    for (uint8_t i = 0; i < 8; i++)
        buf[i] = digits[(v >> ((7 - i) * 4)) & 15];
    buf[8] = 0;
    if (text_row < 17) {
        text_put(text_row, 1, label);
        text_put(text_row++, 16, buf);
    }
}

// back to the menu, as the other plugins
static void leave(void) {
    ia16_disable_irq();
    outportb(WS_INT_ENABLE_PORT, 0);
    outportb(WS_SDMA_CTRL_PORT, 0);
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

static void wait_vblank(void) {
    while (inportb(WS_DISPLAY_LINE_PORT) == 144) {}
    while (inportb(WS_DISPLAY_LINE_PORT) != 144) {}
}

// the reason, then any key: back to the menu
static void fail(const char __far *what, uint32_t v) {
    text_hex(what, v);
    text_print("PRESS A KEY");
    while (ws_keypad_scan()) wait_vblank();
    while (!ws_keypad_scan()) wait_vblank();
    leave();
}

/* ---- the ROM ---- */

static FATFS fs;
static FIL fp;
static char rom_path[PATH_SIZE];

static void load_rom(void) {
    int16_t result = f_open(&fp, rom_path, FA_READ | FA_OPEN_EXISTING);
    if (result != FR_OK) fail("OPEN", result);
    uint32_t size = f_size(&fp);
    text_hex("SIZE", size);

    // FatFs must run with the RAM window on SRAM (it uses the IPC bank):
    // 8 KiB at a time into the internal RAM (6000h, the Game Boy's work
    // RAM later), then into the PSRAM through the RAM window
    uint8_t *buf = (uint8_t*) 0x6000;
    uint32_t done = 0, total = 0x8000;
    while (done < total) {
        unsigned int br = 0;
        if (done < size) {
            result = f_read(&fp, buf, 0x2000, &br);
            if (result != FR_OK) fail("READ", result);
        }
        if (br < 0x2000) memset(buf + br, 0xFF, 0x2000 - br);
        if (done == 0) {
            // the header: what cartridge it is
            uint8_t err = gb_cart_header(buf, size);
            if (err == 1) fail("ROM SIZE", size);
            if (err == 2) fail("CARTRIDGE TYPE", buf[0x147]);
            if (err == 3) fail("GAME BOY COLOR ONLY", buf[0x143]);
            if (gb.rom_banks > ROM_MAX_BANKS) fail("ROM OVER 1 MB", size);
            if (gb.ram_banks > 4) fail("RAM OVER 32 KB", gb.ram_banks);
            total = (uint32_t) gb.rom_banks << 14;
            text_hex("CARTRIDGE", buf[0x147]);
        }
        outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
        outportw(WS_CART_EXTBANK_RAM_PORT, done >> 16);
        _fmemcpy(MK_FP(0x1000, (uint16_t) done), buf, 0x2000);
        outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
        outportw(WS_CART_EXTBANK_RAM_PORT, 0);
        done += 0x2000;
    }
    f_close(&fp);

    // check: the last 8 KiB read must be seen through ROM window 0
    outportw(WS_CART_EXTBANK_ROM0_PORT, (done - 0x2000) >> 16);
    const uint8_t __far *chk = MK_FP(0x2000, (uint16_t) (done - 0x2000));
    for (uint16_t i = 0; i < 0x2000; i++)
        if (chk[i] != buf[i]) fail("PSRAM MAP", done);
}

/* ---- the keys ---- */

static uint8_t exit_held;

void gb_frame_keys(void) {
    uint16_t k = ws_keypad_scan();
    if ((k & (WS_KEY_START | WS_KEY_Y1)) == (WS_KEY_START | WS_KEY_Y1)) {
        if (++exit_held >= 60) leave();
        return;
    }
    exit_held = 0;
    // START + Y4 (Y left): the figures on the screen, on and off
    static uint8_t stats_held;
    if ((k & (WS_KEY_START | WS_KEY_Y4)) == (WS_KEY_START | WS_KEY_Y4)) {
        if (!stats_held) gbv_stats_toggle();
        stats_held = 1;
        return;
    }
    if (!(k & WS_KEY_Y4)) stats_held = 0;
    else if (stats_held) return;       // (START let go first: not the game's)
    uint8_t g = 0;
    if (k & WS_KEY_X2) g |= GB_KEY_RIGHT;
    if (k & WS_KEY_X4) g |= GB_KEY_LEFT;
    if (k & WS_KEY_X1) g |= GB_KEY_UP;
    if (k & WS_KEY_X3) g |= GB_KEY_DOWN;
    if (k & WS_KEY_A) g |= GB_KEY_A;
    if (k & WS_KEY_B) g |= GB_KEY_B;
    if (k & (WS_KEY_Y3 | WS_KEY_Y2)) g |= GB_KEY_SELECT;
    if (k & WS_KEY_START) g |= GB_KEY_START;
    if (g != gb.keys) {
        gb_set_keys(g);
        gbd_keys();
    }
}

/* ---- start ---- */

// (on the stack below 2000h: gb_asm.s)
void gb_emulate(void) {
    gbf_reset();
    gbd_init();
    gb_sound_init();
    gb_phase = GB_PHASE_DISP;
    for (;;)
        gbd_run_frame();
}

int main(void) {
    // the nileswan as the other plugins set it: its registers unlocked,
    // clock, I/O and SRAM on (the way back to the menu sends the flash a
    // command through the SPI), no mask on the RAM window
    nile_io_unlock();
    uint16_t old_mask = inportw(IO_NILE_SEG_MASK);
    outportb(IO_NILE_POW_CNT, NILE_POW_CLOCK | NILE_POW_IO_NILE | NILE_POW_IO_2001
        | NILE_POW_IO_2003 | NILE_POW_SRAM);
    outportw(IO_NILE_SEG_MASK, (old_mask & NILE_SEG_MASK_ROM_MASK) | (0xF << NILE_SEG_MASK_RAM_SHIFT));

    text_init();
    text_print("GBSWAN");
    text_hex("PATCH", GGSWAN_PATCH);

    // the path of the .gb file, left by the menu in the IPC area
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, NILE_SEG_RAM_IPC);
    {
        const char __far *src = MK_FP(0x1000, 0x00E0);
        uint16_t i = 0;
        while (src[i] && i < PATH_SIZE - 1) {
            rom_path[i] = src[i];
            i++;
        }
        rom_path[i] = 0;
    }
    if (!rom_path[0]) fail("NO PATH", 0);
    int16_t result = f_mount(&fs, "", 1);
    if (result != FR_OK) fail("MOUNT", result);
    load_rom();

    gb_run();
    return 0;
}
