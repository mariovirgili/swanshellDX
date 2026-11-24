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

void gbf_plat_ram(void) {
    outportw(WS_CART_EXTBANK_RAM_PORT, gb.ram_bank >> 3);
    gbf_ram_off = (gb.ram_bank & 7) << 13;
    // the cartridge's RAM for the recompiled code: 1 plain RAM that can
    // be reached, 2 none or disabled (reads FFh, writes lost), 0 ask C
    // (MBC2's half bytes, MBC3's clock)
    drc_ram_ok = gb.mbc == 2 || (gb.mbc == 3 && gb.rtc_sel) ? 0 : gb.ram_banks && (gb.ram_on || !gb.mbc) ? 1 : 2;
    drc_ram_seg = 0x1000 + (gbf_ram_off >> 4) - 0xA00;
    // (blocks translated taking it for plain RAM that is enabled: gb_drc.c, cart_sure)
    if (drc_cart_assumed && drc_ram_ok != 1) drc_cart_break = 1;
    // (gb_drc_asm.s, s_wrt: 0 no "RAM on" register at 0000h-1FFFh or not a plain one, 1 off, 2 on)
    drc_ramg = gb.mbc == 1 || gb.mbc == 3 || gb.mbc == 5 ? 1 + (gb.ram_on != 0) : 0;
}

void gbf_plat_map(void) {
    outportw(WS_CART_EXTBANK_ROM0_PORT, gb.rom0_bank >> 2);
    gbf_rom0_off = (gb.rom0_bank & 3) << 14;
    outportw(WS_CART_EXTBANK_ROM1_PORT, gb.romx_bank >> 2);
    gbf_romx_off = (gb.romx_bank & 3) << 14;
    // (the recompiled code's way to the two ROM areas: segments the Game
    // Boy's address is the offset in)
    drc_segtab[0] = 0x2000 + (gbf_rom0_off >> 4);
    drc_segtab[1] = 0x3000 + (gbf_romx_off >> 4) - 0x400;
    // (the first ROM area's bank has changed: MBC1's mode. s_exit_dyn's
    // small table for that area has no bank in it)
    if (drc_key0 != gb.rom0_bank + 1) {
        memset((uint8_t*) DRC_RC0, 0xFF, 0x1000);
        // (and the blocks' own jumps to addresses there - the joined
        // exits, and from patch 192 the exits that remember where they
        // went - have no bank in them either: everything translated is
        // forgotten, gbd_run_frame)
        drc_key0_break = 1;
    }
    drc_key0 = gb.rom0_bank + 1;
    drc_keyx = gb.romx_bank + 1;
    drc_hash0 = drc_key0 << 5;      // (their part of the table's hash)
    drc_hashx = drc_keyx << 5;
    gbf_plat_ram();
    // the write that changes the second ROM area's bank (gb_drc_asm.s:
    // s_wrt): 2000h up to drc_mbc_top (its high byte), the bits of the
    // value that count, whether 0 means 1, the other register's bits
    // (MBC2: 0000h-3FFFh with the address' bit 8 set, four bits. X
    // changes its bank some 75 times a frame; until patch 168 an MBC2's
    // went through C, a seventh of X's time)
    drc_mbc_top = gb.mbc == 1 || gb.mbc == 2 || gb.mbc == 3 ? 0x40 : gb.mbc == 5 ? 0x30 : 0;
    drc_mbc_lo = gb.mbc == 2 ? 0 : 0x20;
    drc_mbc_or = gb.mbc != 2;
    drc_mbc_and = gb.mbc == 1 ? 0x1F : gb.mbc == 2 ? 0x0F : gb.mbc == 3 ? 0x7F : 0xFF;
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

static void wait_vblank(void);
static void text_init(void) {
    outportb(WS_DISPLAY_CTRL_PORT, 0);
    // The 2 bits a pixel mode, coming from the menu's 4: on the console
    // the first screen was drawn with the tiles still read as 4 bits a
    // pixel ones (pieces of the menu's picture), and the second one, set
    // up the same way after a key, was right. Set at the start of the
    // vertical blank and again two frames later, it is right from the
    // first (hw_test 2, 2026-11-16). Which of the two it takes is not
    // known.
    wait_vblank();
    ws_system_set_mode(WS_MODE_COLOR);
    wait_vblank();
    wait_vblank();
    ws_system_set_mode(WS_MODE_COLOR);
    // (the first 44 glyphs, as before: the 4 added for the plugins' menu
    // would go past the tiles gbswan keeps for its font)
    for (uint8_t i = 0; i < 44; i++) {
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
    if (text_row < 17) { text_put(text_row, 1, s); text_row += 2; }
}
static void text_hex(const char __far *label, uint32_t v) {
    static const char __far digits[] = "0123456789ABCDEF";
    char buf[9];
    for (uint8_t i = 0; i < 8; i++)
        buf[i] = digits[(v >> ((7 - i) * 4)) & 15];
    buf[8] = 0;
    if (text_row < 17) {
        text_put(text_row, 1, label);
        text_put(text_row, 16, buf);
        text_row += 2;     // (a row left empty between the lines: the letters fill their eight)
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

#ifdef GB_HWTEST
/* ---- a test of the hardware (first run on a WonderSwan, 2026-11-16: the
   text was other tiles and the game did not start). Text with tiles below
   512 and the mono palettes set too, so that it reads whatever the answers
   are. ---- */
static void hw_put(uint8_t row, uint8_t col, const char __far *s) {
    uint16_t *map = (uint16_t*) 0x4800;
    for (; *s && col < 28; s++, col++) {
        char c = *s;
        uint8_t k = 0;
        for (uint8_t i = 0; font_chars[i]; i++)
            if (font_chars[i] == c) { k = i; break; }
        map[row * 32 + col] = 384 + k;
    }
}
static void hw_hex(uint8_t row, const char __far *label, uint16_t v) {
    static const char __far digits[] = "0123456789ABCDEF";
    char buf[5];
    for (uint8_t i = 0; i < 4; i++)
        buf[i] = digits[(v >> ((3 - i) * 4)) & 15];
    buf[4] = 0;
    hw_put(row, 1, label);
    hw_put(row, 14, buf);
}
/* The same code timed in the internal RAM and in the PSRAM (the linear
   area, where the translated code runs), in lines of the display (256
   cycles each), as ggswan's X3 (X down) test, and then what translated
   code does with the cartridge: its RAM read and written, a bank port
   written. First run on a WonderSwan: the plugins at about half the
   emulator's frames a second. */
#define BENCH_IRAM 0x9100u
#define BENCH_SEG  0xC000u      // (the last code segment: nothing is translated yet)
#define BENCH_OFF  0xF000u
static const uint8_t bn_mix[] = { 0x01, 0xD3, 0x8A, 0x07, 0xA2, 0x20, 0x90,       // add bx, dx; mov al, [bx]; mov [9020h], al
    0x83, 0x2E, 0x22, 0x90, 0x05, 0x7E, 0x00, 0x88, 0xC4 };                         // sub word [9022h], 5; jle $+2; mov ah, al
static const uint8_t bn_jmp8[] = { 0xEB, 0x00 };
static const uint8_t bn_jmp16[] = { 0xE9, 0x00, 0x00 };
static const uint8_t bn_rd_iram[] = { 0x8A, 0x07 };                                // mov al, [bx]
static const uint8_t bn_rd_sram[] = { 0x26, 0x8A, 0x07 };                          // mov al, es:[bx]
static const uint8_t bn_wr_sram[] = { 0x26, 0x88, 0x07 };                          // mov es:[bx], al
static const uint8_t bn_out[] = { 0xE7, 0xD2 };                                    // out D2h, ax (the ROM0 window's bank)
static const uint8_t bn_nop[] = { 0x90 };
static const uint8_t bn_pushpop[] = { 0x50, 0x58 };                                // push ax; pop ax
static const uint8_t bn_callret[] = { 0xE8, 0x02, 0x00, 0xEB, 0x01, 0xC3 };        // call f; jmp short over; f: ret; over:
static const uint8_t bn_in[] = { 0xE5, 0xD2 };                                     // in ax, D2h
static const uint8_t bn_rdw[] = { 0x8B, 0x07 };                                    // mov ax, [bx]
static const uint8_t bn_rdwo[] = { 0x8B, 0x47, 0x01 };                             // mov ax, [bx + 1] (a word at an odd address)
static const uint8_t bn_shl4[] = { 0xC1, 0xE0, 0x04 };                             // shl ax, 4
static const uint8_t bn_call[] = { 0xE8, 0x00, 0x00, 0x83, 0xC4, 0x02 };           // call $+3; add sp, 2
static uint16_t bn_build(uint8_t *b, const uint8_t __far *body, uint8_t len, uint8_t copies, uint16_t n) {
    uint8_t *p = b;
    *p++ = 0x50; *p++ = 0x53; *p++ = 0x51; *p++ = 0x52; *p++ = 0x06;   // push ax, bx, cx, dx, es
    *p++ = 0xB8; *p++ = 0x00; *p++ = 0x10; *p++ = 0x8E; *p++ = 0xC0;   // mov ax, 1000h; mov es, ax
    *p++ = 0xBB; *p++ = 0x00; *p++ = 0x40;                             // mov bx, 4000h
    *p++ = 0x31; *p++ = 0xD2;                                          // xor dx, dx
    *p++ = 0xE5; *p++ = 0xD2;                                          // in ax, D2h
    *p++ = 0xB9; *p++ = n; *p++ = n >> 8;                              // mov cx, n
    uint8_t *loop = p;
    for (uint8_t i = 0; i < copies; i++)
        for (uint8_t j = 0; j < len; j++)
            *p++ = body[j];
    *p++ = 0x49;                                                       // dec cx
    *p++ = 0x74; *p++ = 3;                                             // jz done
    uint16_t rel = (uint16_t) (loop - (p + 3));
    *p++ = 0xE9; *p++ = rel; *p++ = rel >> 8;                          // jmp loop
    *p++ = 0x07; *p++ = 0x5A; *p++ = 0x59; *p++ = 0x5B; *p++ = 0x58;   // done: pop...
    *p++ = 0xCB;                                                       // retf
    return (uint16_t) (p - b);
}
static uint16_t bn_time(uint16_t seg, uint16_t off) {
    uint16_t fp2[2] = { off, seg };
    outportb(WS_TIMER_CTRL_PORT, WS_TIMER_CTRL_HBL_DISABLE);
    outportw(WS_TIMER_HBL_RELOAD_PORT, 0xFFFF);
    outportb(WS_TIMER_CTRL_PORT, WS_TIMER_CTRL_HBL_ONESHOT);
    __asm__ volatile ("lcall *%0" : : "m" (fp2) : "ax", "bx", "cx", "dx", "memory");
    uint16_t left = inportw(WS_TIMER_HBL_COUNTER_PORT);
    outportb(WS_TIMER_CTRL_PORT, WS_TIMER_CTRL_HBL_DISABLE);
    return 0xFFFF - left;
}
static void hw_hex2(uint8_t row, const char __far *label, uint16_t a, uint16_t b) {
    static const char __far digits[] = "0123456789ABCDEF";
    char buf[5];
    buf[4] = 0;
    hw_put(row, 1, label);
    for (uint8_t i = 0; i < 4; i++) buf[i] = digits[(a >> ((3 - i) * 4)) & 15];
    hw_put(row, 12, buf);
    for (uint8_t i = 0; i < 4; i++) buf[i] = digits[(b >> ((3 - i) * 4)) & 15];
    hw_put(row, 18, buf);
}
#define BN_TESTS 15
static void hw_bench(void) {
    struct { const uint8_t __far *body; uint8_t len, copies; const char __far *name; } t[BN_TESTS] = {
        { bn_mix, sizeof(bn_mix), 16, "MIX" },
        { bn_jmp8, sizeof(bn_jmp8), 64, "JMP8" },
        { bn_jmp16, sizeof(bn_jmp16), 64, "JMP16" },
        { bn_call, sizeof(bn_call), 32, "CALL" },
        { bn_rd_iram, sizeof(bn_rd_iram), 64, "RD IRAM" },
        { bn_rd_sram, sizeof(bn_rd_sram), 64, "RD SRAM" },
        { bn_wr_sram, sizeof(bn_wr_sram), 64, "WR SRAM" },
        { bn_out, sizeof(bn_out), 64, "OUT D2" },
        { bn_nop, sizeof(bn_nop), 128, "NOP" },
        { bn_pushpop, sizeof(bn_pushpop), 64, "PUSHPOP" },
        { bn_callret, sizeof(bn_callret), 32, "CALLRET" },
        { bn_in, sizeof(bn_in), 64, "IN D2" },
        { bn_rdw, sizeof(bn_rdw), 64, "RD WORD" },
        { bn_rdwo, sizeof(bn_rdwo), 64, "RD W ODD" },
        { bn_shl4, sizeof(bn_shl4), 64, "SHL 4" },
    };
    // the pages: a header and up to seven tests each, on every other row
    // from the second (test | 80h: with the cartridge's waits off, port 60h
    // bits 1 and 3 clear - what the menu's "fast SRAM" setting does)
    static const uint8_t pages[3][7] = {
        { 0, 1, 2, 3, 4, 5, 0xFF },
        { 6, 7, 0x84, 0x85, 0x86, 0x87, 0xFF },
        { 8, 9, 10, 11, 12, 13, 14 },
    };
    static const char __far names[3][7] = { "BENCH1", "BENCH2", "BENCH3" };
    uint16_t *map = (uint16_t*) 0x4800;
    uint8_t ie = inportb(WS_INT_ENABLE_PORT);
    outportb(WS_INT_ENABLE_PORT, 0);
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, 0);
    uint8_t *b = (uint8_t*) BENCH_IRAM;
    uint8_t m = inportb(0x60);
    for (uint8_t pg = 0; pg < 3; pg++) {
        for (uint16_t i = 0; i < 32 * 32; i++) map[i] = 384;
        hw_hex2(1, names[pg], m, inportb(0xA0));
        for (uint8_t k = 0; k < 7; k++) {
            uint8_t i = pages[pg][k];
            if (i == 0xFF) break;
            uint8_t fast = i & 0x80;
            i &= 0x7F;
            if (fast) outportb(0x60, m & ~0x0A);
            uint16_t len = bn_build(b, t[i].body, t[i].len, t[i].copies, 4000);
            gbd_lin_put(BENCH_SEG, BENCH_OFF, b, len);
            uint16_t a = bn_time(0x0000, BENCH_IRAM);
            uint16_t c = bn_time(BENCH_SEG, BENCH_OFF);
            outportb(0x60, m);
            hw_hex2(3 + k * 2, t[i].name, a, c);
            if (fast) hw_put(3 + k * 2, 10, "F");
        }
        hw_put(1, 24, "ST");
        while (!(ws_keypad_scan() & WS_KEY_START)) wait_vblank();
        while (ws_keypad_scan()) wait_vblank();
    }
    outportb(WS_INT_ENABLE_PORT, ie);
}

static void hw_test(uint16_t old_mask) {
    outportb(WS_DISPLAY_CTRL_PORT, 0);
    uint8_t mode_before = inportb(0x60);
    // (test 1 on the console: the first screen was drawn with other tiles,
    // the second, after a key, was right. So here: the mode set at the
    // start of the vertical blank, and set again two frames later)
    wait_vblank();
    ws_system_set_mode(WS_MODE_COLOR);
    wait_vblank();
    wait_vblank();
    ws_system_set_mode(WS_MODE_COLOR);
    // is the internal RAM above 4000h there at once? (written right after
    // the mode is set, read after a few frames)
    volatile uint8_t *early = (volatile uint8_t*) 0x9010;
    early[0] = 0x11; early[1] = 0x22;
    uint8_t early_now = early[0];
    for (uint8_t i = 0; i < 4; i++) wait_vblank();
    uint8_t early_later = early[0];
    // the font at tiles 384-427 (3800h), the same again shifted by one
    // letter at 896-939 (5800h): "BANK A" is drawn with tile 384 + 'A' and
    // the bank bit: an A if the bit is not used, a B if it is
    for (uint8_t i = 0; i < sizeof(font_data) / 8; i++) {
        uint8_t *t = (uint8_t*) 0x3800 + (uint16_t) i * 16;
        uint8_t *u = (uint8_t*) 0x5800 + (uint16_t) i * 16;
        uint8_t j = i + 1 < sizeof(font_data) / 8 ? i + 1 : i;
        for (uint8_t r = 0; r < 8; r++) {
            t[r * 2] = font_data[i * 8 + r]; t[r * 2 + 1] = 0;
            u[r * 2] = font_data[j * 8 + r]; u[r * 2 + 1] = 0;
        }
    }
    uint16_t *map = (uint16_t*) 0x4800;
    for (uint16_t i = 0; i < 32 * 32; i++) map[i] = 384;
    WS_DISPLAY_COLOR_MEM(0)[0] = 0xFFF;
    WS_DISPLAY_COLOR_MEM(0)[1] = 0x000;
    outportb(0x1C, 0x00); outportb(0x1D, 0x00); outportb(0x1E, 0xF0); outportb(0x1F, 0xFF);
    outportw(0x20, 0x0070);     // mono palette 0: color 0 white, color 1 black
    outportb(WS_SCR_BASE_PORT, (0x4800 >> 11) << 4 | (0x4800 >> 11));
    outportb(0x12, 0); outportb(0x13, 0); outportb(0x01, 0);
    outportb(WS_DISPLAY_CTRL_PORT, WS_DISPLAY_CTRL_SCR2_ENABLE);

    // (every other row, from the second to the sixteenth: the first row
    // is cut on Gio's console and rows next to each other run together)
    hw_put(1, 1, "GBSWAN HW TEST 3");
    hw_hex(3, "BEFORE 60", mode_before);
    hw_hex(5, "MODE 60", inportb(0x60));
    hw_hex(7, "EARLY", (uint16_t) early_now << 8 | early_later);
    hw_put(9, 1, "BANK");
    {   // 'A' is font_chars[11]
        map[9 * 32 + 14] = 0x2000 | (384 + 11);
    }
    hw_hex(11, "LIN C0", inportb(0xC0));
    hw_hex(13, "MASK", old_mask);
    // at least three seconds, then START
    hw_put(15, 1, "WAIT");
    for (uint8_t i = 0; i < 225; i++) wait_vblank();
    hw_put(15, 1, "PRESS START");
    while (!(ws_keypad_scan() & WS_KEY_START)) wait_vblank();
    while (ws_keypad_scan()) wait_vblank();

    for (uint16_t i = 0; i < 32 * 32; i++) map[i] = 384;
    hw_put(1, 1, "GBSWAN HW TEST 3B");
    // a place read through the linear area (segment 4000h), as the
    // recompiler's (written there, it read back unchanged on the console,
    // test 2; not written any more: gbrun.py stops at such a write)
    volatile uint8_t __far *p = MK_FP(0x4000, 0x0123);
    uint8_t a0 = *p;
    hw_hex(3, "LIN READ", a0);
    // the same place written through the RAM window (flash on), bank as
    // ggswan works it out, and read through the linear area
    uint16_t bank = (((uint16_t) inportb(0xC0) << 4) | 4) & (old_mask & NILE_SEG_MASK_ROM_MASK);
    hw_hex(5, "BANK OF 4000", bank);
    uint8_t b0 = *p;
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, bank);
    *(volatile uint8_t __far*) MK_FP(0x1000, 0x0123) = (uint8_t) (b0 ^ 0x5A);
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, 0);
    uint8_t b1 = *p;
    hw_hex(7, "RAM WRITE", (uint16_t) b0 << 8 | b1);        // works: they differ by 5Ah
    // the internal RAM above 4000h
    volatile uint8_t *q = (volatile uint8_t*) 0x9000;
    *q = 0xA5;
    hw_hex(9, "IRAM 9000", *q);
    hw_put(15, 1, "PRESS START");
    while (!(ws_keypad_scan() & WS_KEY_START)) wait_vblank();
    while (ws_keypad_scan()) wait_vblank();
    hw_bench();
}
#endif

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

/* ---- the state kept and brought back ----
 * START + A held half a second keeps the machine as it is, START + B
 * brings it back. What is kept: the whole internal RAM but the vectors
 * (the Game Boy's registers, memory, video memory and what the plugin
 * keeps of them: GB-MEMORY-MAP.md), in the PSRAM bank behind segment
 * D000h, and the cartridge's RAM (32 KiB) in the one behind E000h - two
 * banks of the linear area the recompiler does not use. Not the SRAM:
 * that is the cartridge's RAM, which the menu writes to the .sav file.
 * Kept and brought back between two frames, in gb_emulate, where
 * nothing translated is being run; what was translated is then
 * forgotten (it is made again as the game goes on). */
#define ST_SEG  0xD000u
#define ST_SEG2 0xE000u
#define ST_MARK ((const uint32_t __far*) MK_FP(ST_SEG2, 0x8000))   // (read here, written by st_mark)
static void st_mark(uint32_t v) {
    gbd_lin_put(ST_SEG2, 0x8000, &v, 4);
}
#define ST_MAGIC 0x54534247ul           // "GBST"
#define ST_KEEP ((const uint8_t __far*) MK_FP(ST_SEG2, 0x9000))
static uint8_t st_req;                  // asked for: 1 keep, 2 bring back
static uint8_t st_wait;                 // nothing to the game until the keys are let go
static uint8_t st_held;
static uint16_t st_t0;
static uint8_t start_n, start_tap, start_used;

// The state as a file on the card, next to the ROM, with .st0 for its
// extension (rom_path, once the ROM has been read): 16 bytes - "GBST",
// the patch, and the build's date and time, since the state is the
// plugin's own memory and another build's is not this one's - then the
// internal RAM from 0040h and the cartridge's RAM as they are kept.
// Written when a state is kept, read when the game starts.
#define ST_BUF ((uint8_t*) 0x9000)      // 2 KiB of internal RAM nothing uses while the game runs
static const char st_build[] = __DATE__ __TIME__;
static void state_head(uint8_t *h) {
    h[0] = 'G'; h[1] = 'B'; h[2] = 'S'; h[3] = 'T';
    h[4] = (uint8_t) GGSWAN_PATCH; h[5] = (uint8_t) (GGSWAN_PATCH >> 8);
    uint8_t x = 0, y = 0;
    for (uint8_t i = 0; st_build[i]; i++) { x += st_build[i]; y += x; }
    h[6] = x; h[7] = y;
    for (uint8_t i = 8; i < 16; i++) h[i] = st_build[i + 3];
}
static uint8_t state_file_write(void) {
    unsigned int bw;
    if (f_open(&fp, rom_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return 1;
    uint8_t bad = 0;
    state_head(ST_BUF);
    if (f_write(&fp, ST_BUF, 16, &bw) != FR_OK || bw != 16) bad = 1;
    for (uint8_t part = 0; part < 2 && !bad; part++) {
        uint16_t off = part ? 0 : 0x0040;
        uint16_t seg = part ? ST_SEG2 : ST_SEG;
        uint32_t left = part ? 0x8000ul : 0xFFC0ul;
        while (left && !bad) {
            uint16_t n = left > 0x800 ? 0x800 : (uint16_t) left;
            _fmemcpy(ST_BUF, MK_FP(seg, off), n);
            if (f_write(&fp, ST_BUF, n, &bw) != FR_OK || bw != n) bad = 1;
            off += n;
            left -= n;
        }
    }
    if (f_close(&fp) != FR_OK) bad = 1;
    return bad;
}
// (before the game: the internal RAM at 6000h is free, as for the ROM)
static void state_file_read(void) {
    unsigned int br;
    uint8_t *buf = (uint8_t*) 0x6000;
    uint8_t h[16];
    st_mark(0);                         // (the PSRAM may still hold another game's)
    if (f_open(&fp, rom_path, FA_READ | FA_OPEN_EXISTING) != FR_OK) return;
    state_head(h);
    uint8_t bad = f_read(&fp, buf, 16, &br) != FR_OK || br != 16;
    for (uint8_t i = 0; i < 16 && !bad; i++)
        if (buf[i] != h[i]) bad = 1;
    for (uint8_t part = 0; part < 2 && !bad; part++) {
        uint16_t off = part ? 0 : 0x0040;
        uint16_t seg = part ? ST_SEG2 : ST_SEG;
        uint32_t left = part ? 0x8000ul : 0xFFC0ul;
        while (left && !bad) {
            uint16_t n = left > 0x2000 ? 0x2000 : (uint16_t) left;
            if (f_read(&fp, buf, n, &br) != FR_OK || br != n) bad = 1;
            gbd_lin_put(seg, off, buf, n);
            off += n;
            left -= n;
        }
    }
    f_close(&fp);
    if (!bad) st_mark(ST_MAGIC);
}
// rom_path's extension made .st0
static void state_path(void) {
    uint16_t n = 0, dot = 0;
    while (rom_path[n]) {
        if (rom_path[n] == '.') dot = n;
        else if (rom_path[n] == '/') dot = 0;
        n++;
    }
    if (!dot) dot = n;
    if (dot + 5 > PATH_SIZE) dot = PATH_SIZE - 5;
    rom_path[dot] = '.'; rom_path[dot + 1] = 's'; rom_path[dot + 2] = 't'; rom_path[dot + 3] = '0';
    rom_path[dot + 4] = 0;
}

static void state_save(void) {
    ia16_disable_irq();
    gbd_lin_put(ST_SEG, 0x0040, MK_FP(0x0000, 0x0040), 0xFFC0);
    // the cartridge's RAM and the PSRAM are both reached through the RAM
    // window: 2 KiB at a time through the internal RAM (ST_BUF: as it was,
    // it is in the copy just made)
    for (uint16_t off = 0; off < 0x8000; off += 0x800) {
        outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
        outportw(WS_CART_EXTBANK_RAM_PORT, 0);
        _fmemcpy(ST_BUF, MK_FP(0x1000, off), 0x800);
        gbd_lin_put(ST_SEG2, off, ST_BUF, 0x800);
    }
    _fmemcpy(ST_BUF, MK_FP(ST_SEG, (uint16_t) ST_BUF), 0x800);
    gbf_plat_ram();
    st_mark(ST_MAGIC);
    ia16_enable_irq();
    gbv_note("SAVE");
    // the file (a second or so: the WonderSwan's sound off meanwhile)
    uint8_t snd = inportb(WS_SOUND_CH_CTRL_PORT);
    outportb(WS_SOUND_CH_CTRL_PORT, 0);
    if (state_file_write()) gbv_note("FILE");       // (kept in memory, not on the card)
    outportb(WS_SOUND_CH_CTRL_PORT, snd);
}

static void state_load(void) {
    if (*ST_MARK != ST_MAGIC) {
        gbv_note("NONE");
        return;
    }
    ia16_disable_irq();
    // (FatFs's volume is the card's as it is now, not as it was)
    gbd_lin_put(ST_SEG2, 0x9000, &fs, sizeof(fs));
    // (the stack, D000h-D7FFh, is this function's)
    _fmemcpy(MK_FP(0x0000, 0x0040), MK_FP(ST_SEG, 0x0040), 0xD000 - 0x0040);
    _fmemcpy(MK_FP(0x0000, 0xD800), MK_FP(ST_SEG, 0xD800), 0x2800);
    _fmemcpy(&fs, ST_KEEP, sizeof(fs));
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, 0);
    _fmemcpy(MK_FP(0x1000, 0), MK_FP(ST_SEG2, 0), 0x8000);
    gbf_plat_map();
    gbd_flush();
    gb_sound_resync();          // (its record of the sound ports is the saved one: write them all again)
    st_req = 0;
    st_held = 2;
    st_wait = 1;
    start_n = start_tap = 0;
    start_used = 1;
    exit_held = 0;
    ia16_enable_irq();
    gbv_note("LOAD");
}

void gb_frame_keys(void) {
    uint16_t k0 = ws_keypad_scan(), k = k0;
    if (st_wait) {
        if (k & (WS_KEY_START | WS_KEY_A | WS_KEY_B)) k &= ~(WS_KEY_START | WS_KEY_A | WS_KEY_B);
        else st_wait = 0;
    }
    if ((k & (WS_KEY_START | WS_KEY_Y1)) == (WS_KEY_START | WS_KEY_Y1)) {
        if (++exit_held >= 60) leave();
        start_used = 1;
        return;
    }
    exit_held = 0;
    // START + Y4 (Y left): the figures on the screen, on and off
    static uint8_t stats_held;
    if ((k & (WS_KEY_START | WS_KEY_Y4)) == (WS_KEY_START | WS_KEY_Y4)) {
        if (!stats_held) gbv_stats_toggle();
        stats_held = 1;
        start_used = 1;
        return;
    }
    if (!(k & WS_KEY_Y4)) stats_held = 0;
    else if (stats_held) return;       // (START let go first: not the game's)
    // START + A, START + B, held half a second (5,960 WonderSwan lines):
    // the state kept, brought back. Neither key is the game's meanwhile
    if ((k & WS_KEY_START) && (k & (WS_KEY_A | WS_KEY_B))) {
        if (!st_held) {
            st_held = 1;
            st_t0 = gbv_now_lines();
        } else if (st_held == 1 && (uint16_t) (gbv_now_lines() - st_t0) >= 5960) {
            st_req = (k & WS_KEY_A) ? 1 : 2;
            st_held = 2;
            st_wait = 1;
        }
        start_used = 1;
        k &= ~(WS_KEY_START | WS_KEY_A | WS_KEY_B);
    } else {
        st_held = 0;
    }
    // START reaches the game after 8 frames of it alone (pressed with A
    // or B a moment later it would have paused the game first); let go
    // sooner, it is pressed for the game for 6 frames then
    if (k & WS_KEY_START) {
        if (start_n < 8) {
            start_n++;
            k &= ~WS_KEY_START;
        }
    } else {
        if (start_n && start_n < 8 && !start_used) start_tap = 6;
        if (!(k0 & WS_KEY_START)) { start_n = 0; start_used = 0; }
        if (start_tap) {
            start_tap--;
            k |= WS_KEY_START;
        }
    }
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
    for (;;) {
        gbd_run_frame();
        if (st_req) {
            uint8_t r = st_req;
            st_req = 0;
            if (r == 1) state_save();
            else state_load();
        }
    }
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

    drc_lin_hi = (uint16_t) inportb(0xC0 /* the linear area's bank */) << 4;
    drc_lin_mask = old_mask & NILE_SEG_MASK_ROM_MASK;
#ifdef GB_HWTEST
    hw_test(old_mask);
#endif
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
    state_path();
    state_file_read();

    gb_run();
    return 0;
}
