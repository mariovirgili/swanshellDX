/**
 * ggswan - Game Gear emulator for the WonderSwan (nileswan)
 * The Game Gear machine: memory map (Sega mapper), I/O ports, VDP state.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GGSWAN_GG_H_
#define GGSWAN_GG_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __ia16__
#define GGFAR __far
#else
#define GGFAR
#endif

#define GG_LINES 262
#define GG_CYCLES_PER_LINE 228
#define GG_ACTIVE_LINES 192
// the Game Gear shows a 160x144 window of the 256x192 picture
#define GG_VIEW_X 48
#define GG_VIEW_Y 24
#define GG_VIEW_W 160
#define GG_VIEW_H 144

// Joypad bits (1 = pressed)
#define GG_KEY_UP    0x01
#define GG_KEY_DOWN  0x02
#define GG_KEY_LEFT  0x04
#define GG_KEY_RIGHT 0x08
#define GG_KEY_1     0x10
#define GG_KEY_2     0x20
#define GG_KEY_START 0x80

typedef struct {
    uint8_t reg[16];
    uint16_t addr;
    uint8_t code;
    uint8_t latch;          // first byte of a control write
    uint8_t latch_full;     // second byte expected
    uint8_t read_buffer;
    uint8_t status;         // bit 7: frame interrupt, 6: overflow, 5: collision
    uint8_t line_pending;
    uint8_t line_counter;
    uint8_t cram_latch;
    uint16_t vcounter;      // current line
    uint8_t vscroll_frame;  // vertical scroll latched at the start of the frame
} gg_vdp_t;

typedef struct {
    gg_vdp_t vdp;
    uint8_t mapper[4];      // $FFFC-$FFFF
    uint8_t keys;           // GG_KEY_* currently pressed
    uint16_t rom_banks;     // 16 KiB banks in the ROM
    uint32_t frame;
} gg_t;

extern gg_t gg;
// 8 KiB of work RAM, 16 KiB of VRAM, 64 bytes of CRAM (32 colors, 12 bits)
extern uint8_t *gg_ram;
extern uint8_t *gg_vram;
extern uint8_t gg_cram[64];
// the three 16 KiB ROM slots ($0000, $4000, $8000)
extern const uint8_t GGFAR *gg_slot[3];

/* Provided by the platform */

/// Pointer to ROM bank "bank" (16 KiB) for slot 0..2.
const uint8_t GGFAR *gg_platform_bank(uint8_t slot, uint16_t bank);
/// Cartridge RAM (32 KiB), mapped at $8000 when enabled.
uint8_t GGFAR *gg_platform_cart_ram(uint8_t page);
/// Called at the start of each line, before the CPU runs it.
void gg_platform_line(uint16_t line);
/// A write to the PSG (port $7F) or the stereo register (port $06).
void gg_platform_psg(uint8_t value);
void gg_platform_stereo(uint8_t value);
/// VDP writes the platform may need to mirror (VRAM, CRAM, registers).
void gg_platform_vram_write(uint16_t addr, uint8_t value);
void gg_platform_cram_write(uint8_t index);
void gg_platform_vdp_reg(uint8_t reg, uint8_t value);

/* Machine */

void gg_init(uint16_t rom_banks);
void gg_reset(void);
/// Run one frame (262 lines).
void gg_run_frame(void);

/* VDP helpers */

static inline uint16_t gg_name_table_base(void) {
    return (gg.vdp.reg[2] & 0x0E) << 10;
}
static inline uint16_t gg_sat_base(void) {
    return (gg.vdp.reg[5] & 0x7E) << 7;
}
static inline bool gg_display_enabled(void) {
    return gg.vdp.reg[1] & 0x40;
}

#endif
