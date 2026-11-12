/**
 * gbswan - the Game Boy (DMG): the machine. This is the reference: an
 * interpreter that moves the rest of the hardware (timer, serial, OAM
 * DMA, the LCD) by one machine cycle (4 clock ticks) before each access
 * to the bus, as the hardware does, so that the test ROMs that time
 * things by the cycle can be passed. Speed is not its job.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GB_H
#define GB_H

#include <stdint.h>
#include <stdbool.h>

#define GB_FLAG_Z 0x80
#define GB_FLAG_N 0x40
#define GB_FLAG_H 0x20
#define GB_FLAG_C 0x10

// interrupts (bits of IF and IE; vectors 40h + 8 * bit)
#define GB_INT_VBLANK 0x01
#define GB_INT_STAT   0x02
#define GB_INT_TIMER  0x04
#define GB_INT_SERIAL 0x08
#define GB_INT_JOYPAD 0x10

// the I/O registers kept in gb.io[] (index: address - FF00h)
#define GB_P1   0x00
#define GB_SB   0x01
#define GB_SC   0x02
#define GB_DIV  0x04
#define GB_TIMA 0x05
#define GB_TMA  0x06
#define GB_TAC  0x07
#define GB_IF   0x0F
#define GB_LCDC 0x40
#define GB_STAT 0x41
#define GB_SCY  0x42
#define GB_SCX  0x43
#define GB_LY   0x44
#define GB_LYC  0x45
#define GB_DMA  0x46
#define GB_BGP  0x47
#define GB_OBP0 0x48
#define GB_OBP1 0x49
#define GB_WY   0x4A
#define GB_WX   0x4B

// keys (gb.keys: set = pressed)
#define GB_KEY_RIGHT  0x01
#define GB_KEY_LEFT   0x02
#define GB_KEY_UP     0x04
#define GB_KEY_DOWN   0x08
#define GB_KEY_A      0x10
#define GB_KEY_B      0x20
#define GB_KEY_SELECT 0x40
#define GB_KEY_START  0x80

#define GB_FRAME_TICKS 70224UL      // 154 lines of 456

typedef struct {
    // CPU
    uint8_t a, f, b, c, d, e, h, l;
    uint16_t sp, pc;
    uint8_t ime;            // interrupts enabled
    uint8_t ei_delay;       // EI: enabled after the next instruction
    uint8_t halted;
    uint8_t halt_bug;       // the byte after HALT is read twice
    uint8_t stopped;
    uint8_t locked;         // an opcode that does not exist: the CPU hangs
    uint32_t clock;         // clock ticks since reset (4 a machine cycle)

    // timer and serial
    uint16_t div;           // the 16 bits DIV is the top 8 of
    uint8_t tima_state;     // 0, 1: overflowed, reads 0, reload next cycle; 2: the cycle of the reload
    uint8_t serial_bits;    // bits left of a transfer with the internal clock

    // OAM DMA
    uint8_t dma_wait;       // cycles before a (re)start: 2, 1
    uint8_t dma_active;
    uint8_t dma_pos;        // next byte, 0-159
    uint8_t dma_page;       // of the running copy (gb.io[GB_DMA] is the last written)
    uint8_t dma_next;       // the page of the one waiting to start

    // LCD
    uint16_t dot;           // 0-455 in the line
    uint8_t line;           // 0-153 (not what LY reads on line 153)
    uint8_t mode;           // what STAT's bits 0-1 read
    uint8_t stat_line;      // the STAT interrupt's signal (an interrupt on its rise)
    uint8_t lyc_match;      // STAT bit 2
    uint8_t ly_cmp;         // the value LYC is compared with, or FFh: none
    uint16_t mode3_end;     // the dot mode 3 ends at, this line
    uint8_t lcd_on;
    uint8_t first_line;     // the line after the LCD is turned on: no mode 2
    uint8_t win_line;       // the window's own line counter
    uint8_t win_on_frame;   // WY was met this frame
    uint8_t frame_done;     // set at the start of the vertical blank
    uint32_t frames;

    // cartridge
    uint8_t mbc;            // 0 none, 1, 2, 3, 5
    uint8_t ram_on;
    uint8_t mbc1_mode;
    uint8_t bank_lo, bank_hi;       // the registers as written
    uint8_t rtc_sel;
    uint16_t rom_banks;     // of 16 KiB (a power of two)
    uint8_t ram_banks;      // of 8 KiB (MBC2: 0, its 512 half bytes apart)
    uint16_t rom0_bank, romx_bank;  // what 0000h and 4000h show
    uint8_t ram_bank;
    uint8_t has_battery;
    uint8_t ram_dirty;

    uint8_t keys;
    uint8_t ie;
    uint8_t io[0x80];
    uint8_t hram[0x7F];
    uint8_t oam[0xA0];
} gb_t;

extern gb_t gb;

/* The memory: on the PC arrays; (later) on the WonderSwan fixed places. */
extern uint8_t gb_vram[0x2000];
extern uint8_t gb_wram[0x2000];
extern const uint8_t *gb_rom;       // rom_banks * 4000h
extern uint8_t *gb_cart_ram;        // ram_banks * 2000h (MBC2: 200h)

/* gb_bus.c */
// rom: the whole file; ram: 32 KiB at least (kept by the caller). Returns 0,
// or why not: 1 too short or a size the header does not say, 2 a cartridge
// type not done, 3 for the Game Boy Color only
uint8_t gb_load(const uint8_t *rom, uint32_t size, uint8_t *ram);
uint32_t gb_ram_size(void);         // bytes of cartridge RAM to keep in a file (0: none)
void gb_reset(void);                // the state the DMG's boot ROM leaves
uint8_t gb_read(uint16_t a);        // what the CPU reads (not a cycle)
void gb_write(uint16_t a, uint8_t v);
uint8_t gb_peek(uint16_t a);        // the memory as it is, whatever the LCD or the DMA are doing
void gb_tick(void);                 // one machine cycle of everything but the CPU
void gb_set_keys(uint8_t keys);

/* gb_cpu.c */
void gb_step(void);                 // one instruction, or one interrupt taken, or one cycle halted
void gb_run_frame(void);            // to the next start of a vertical blank (or 70224 ticks with the LCD off)

/* gb_ppu.c */
void gb_ppu_reset(void);
void gb_ppu_tick(void);             // 4 dots
uint8_t gb_ppu_read(uint8_t reg);
void gb_ppu_write(uint8_t reg, uint8_t v);
bool gb_ppu_vram_blocked(bool write);
bool gb_ppu_oam_blocked(bool write);
// the picture: 160 x 144 of shades 0 (lightest) to 3, drawn a line at a time
extern uint8_t gb_screen[144][160];

#endif
