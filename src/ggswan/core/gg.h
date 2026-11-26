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
/// cycles of a frame (unsigned: past the 16-bit int range)
#define GG_FRAME_CYCLES ((uint16_t) (GG_LINES * 228u))
#define GG_ACTIVE_LINES 192
#if defined(GGSWAN_SMS) && defined(GGSWAN_SG1000)
#error "GGSWAN_SMS and GGSWAN_SG1000: one machine a build"
#endif
#if defined(GGSWAN_COLECO) && (!defined(GGSWAN_SG1000) || defined(GGSWAN_SF7000))
#error "GGSWAN_COLECO requires GGSWAN_SG1000 and excludes GGSWAN_SF7000"
#endif
/*
 * What differs with the machine, here and in gg.c:
 *   GG_VBL_LINE   the line whose start sets the frame interrupt flag
 *   GG_LINE_IRQ   line interrupts enabled (mode 4's register 0 bit 4)
 */
#ifdef GGSWAN_SG1000
// sgswan (PLUG_SG.BIN: these sources built with -DGGSWAN_SG1000): the
// SG-1000, a TMS9918A (see SG1000.md): no mapper, no line interrupts, 8
// registers, a fixed palette. The work RAM is the Mark III's, 8 KiB (the
// SG-1000's own is 1 KiB, there again every 400h: its games run on a Mark
// III, and some later ones need the 8). The picture is 256x192, the WS
// shows 224x144 of it: a view, as smsswan's.
extern uint8_t gg_view_x, gg_view_y;
#define GG_ACTIVE GG_ACTIVE_LINES
#define GG_VIEW_X gg_view_x
#define GG_VIEW_Y gg_view_y
#define GG_VIEW_X_MAX 32
#define GG_VIEW_Y_MAX (GG_ACTIVE - GG_VIEW_H)
#define GG_VIEW_W 224
#define GG_VIEW_H 144
#define WS_VIEW_ORIGIN_X 0
#define GG_REC_Y 0
#define GG_REC_H GG_ACTIVE
#define GG_VBL_LINE GG_ACTIVE
#define GG_LINE_IRQ 0
#else
#define GG_VBL_LINE (GG_ACTIVE + 1)
#define GG_LINE_IRQ (gg.vdp.reg[0] & 0x10)
#endif
#ifdef GGSWAN_SG1000
#elif defined(GGSWAN_SMS)
// smsswan (PLUG_SMS.BIN: these sources built with -DGGSWAN_SMS): the
// Master System shows all of the 256x192 picture, the WS 224x144 of it,
// on its whole screen
// on its whole screen: a view that can be moved over the picture (X 0-32,
// Y 0-48; 16, 24 at the start: the middle)
extern uint8_t gg_view_x, gg_view_y;
#define GG_VIEW_X gg_view_x
#define GG_VIEW_Y gg_view_y
#define GG_VIEW_X_MAX 32
#define GG_VIEW_Y_MAX (GG_ACTIVE - GG_VIEW_H)   // (48; 80 with 224 lines)
#define GG_VIEW_W 224
#define GG_VIEW_H 144
#define WS_VIEW_ORIGIN_X 0      // the WS column where the view starts
// scroll splits are recorded over the whole picture (the view moves)
#define GG_REC_Y 0
#define GG_REC_H GG_ACTIVE
// the lines of the picture: 192, or 224 (mode 4 with M1 and M2 set,
// registers 1 and 0: Codemasters' games), kept in gg.vdp (gg.c
// vdp_mode_update) with the values the generated routines compare with
#define GG_ACTIVE gg.vdp.active
#else
#define GG_ACTIVE GG_ACTIVE_LINES
// the Game Gear shows a 160x144 window of the 256x192 picture
#define GG_VIEW_X 48
#define GG_VIEW_Y 24
#define GG_VIEW_W 160
#define GG_VIEW_H 144
#define WS_VIEW_ORIGIN_X 32     // the WS column where the view starts
// scroll splits are recorded over the lines of the view
#define GG_REC_Y GG_VIEW_Y
#define GG_REC_H GG_VIEW_H
// WS column x shows picture column x + 16 (the horizontal scroll is 16 -
// register 8: main.c hscroll_write, the CTL routine in drc.c; the
// sprites' X offset; smsswan: the same with its view in the middle, the
// rest added when the frame is presented)
#if GG_VIEW_X - WS_VIEW_ORIGIN_X != 16
#error "the view must start 16 columns to the right of its WS column"
#endif
#endif
// the frame is presented at the start of this line, right after the last
// line the Game Gear shows, 168 (gg_platform_frame_end); smsswan: 192,
// the end of the picture, wherever the view is
#define GG_PRESENT_LINE (GG_REC_Y + GG_REC_H)

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
    uint8_t reg1_frame;     // register 1 (display on) at the start of the frame
    // frame timing (gg.c; here so that the recompiler's line routine can
    // reach it): lines of the frame started, started * 228, cycles given
    // to the CPU in the frame
    uint16_t started;
    uint16_t line_pos;
    int32_t frame_given;
    // 32 colors of 2 bytes, 12 bits (in gg.vdp: the recompiler's CRAM
    // routine reaches it at a known address)
    uint8_t cram[64];
    // cycles not counted when a line interrupt was taken late (gg.c
    // event_step): taken back below the view, in the same frame
    uint16_t irq_debt;
#ifdef GGSWAN_SMS
    uint16_t active;        // lines of the picture: 192 or 224
    uint16_t vcnt_max;      // the V counter jumps back by 6 after this line
    uint16_t present_m1;    // GG_PRESENT_LINE - 1 and + 1 (the recompiler's
    uint16_t present_p1;    // LINE and CTL routines)
#endif
} gg_vdp_t;

typedef struct {
    gg_vdp_t vdp;
    uint8_t mapper[4];      // $FFFC-$FFFF
    uint8_t keys;           // GG_KEY_* currently pressed
    uint16_t rom_banks;     // 16 KiB banks in the ROM
    uint32_t frame;
    uint8_t io_ctrl;        // smsswan: port 3Fh (I/O control: TH/TR direction and level)
    uint8_t codemasters;    // smsswan: the Codemasters mapper (banks written at 0000h, 4000h, 8000h)
    uint8_t coll_used;      // the game looks at the sprite collision flag (gg_status_read_at)
} gg_t;

extern gg_t gg;
// 8 KiB of work RAM, 16 KiB of VRAM, 64 bytes of CRAM (32 colors, 12 bits)
// (sgswan: the VRAM is the TMS9918A's own 16 KiB, not WS tiles, and may
// be outside the internal RAM)
extern uint8_t *gg_ram;
#ifdef GGSWAN_SG1000
extern uint8_t GGFAR *gg_vram;
#else
extern uint8_t *gg_vram;
#endif
#define gg_cram gg.vdp.cram
// color i (0-31) as ----BBBBGGGGRRRR: the Game Gear's two CRAM bytes;
// smsswan: one byte --BBGGRR, each 2-bit value times 5 (0, 5, 10, 15)
// the name table's height in lines and the end of the sprite list (a Y of
// D0h), both only with 192 lines
#ifdef GGSWAN_SMS
#define GG_NT_WRAP (gg.vdp.active == 224 ? 256 : 224)
#define GG_SPR_END(y) ((y) == 0xD0 && gg.vdp.active != 224)
#else
#define GG_NT_WRAP 224
#define GG_SPR_END(y) ((y) == 0xD0)
#endif
#ifdef GGSWAN_SG1000
// the TMS9918A's 16 colors (0: transparent, shown as black here), 12 bits
extern const uint16_t sg_palette[16];
static inline uint16_t gg_color12(uint8_t i) { return sg_palette[i & 15]; }
/// The screen mode from registers 0 and 1: SG_MODE_*.
enum { SG_MODE_G1, SG_MODE_G2, SG_MODE_MC, SG_MODE_TEXT };
static inline uint8_t sg_mode(void);
/// Status bits for the sprites as they are in VRAM now: 40h and the number
/// of the first fifth sprite of a line, if any; 20h: two of them overlap.
/// (Worked out again only when gg_platform_sprites_written() says so, or
/// registers 0, 1, 5, 6 changed.)
uint8_t sg_sprite_status(void);
/// Platform: was the sprite table, or a sprite's pattern, written since
/// the frame was last presented (gg_platform_frame_end)? 1 when unknown.
uint8_t gg_platform_sprites_written(void);
uint8_t sg_sprite_hidden(const uint8_t *sat, uint32_t *hid);
/// One sprite of the table as the screen has it. sg_sprite() gives 0 at
/// the end of the table (Y = D0h).
typedef struct {
    int16_t x, y;       // of its top left pixel, in the picture
    uint16_t pattern;   // where its rows are in VRAM (16x16: 16 more bytes
                        // for the right half, 16 further on)
    uint8_t color;      // 0-15 (0: nothing to see, but it counts)
} sg_sprite_t;
uint8_t sg_sprite(uint8_t i, sg_sprite_t *s);
/// The side of the sprites in pattern pixels (8, 16) and how many screen
/// pixels each is (1, 2), from register 1.
static inline uint8_t sg_sprite_size(void) { return (gg.vdp.reg[1] & 0x02) ? 16 : 8; }
static inline uint8_t sg_sprite_mag(void) { return (gg.vdp.reg[1] & 0x01) ? 2 : 1; }
/// The 16 pixels of a row of a sprite's pattern, bit 15 the leftmost.
uint16_t sg_sprite_row(const sg_sprite_t *s, uint8_t row);
#else
static inline uint16_t gg_color12(uint8_t i) {
#ifdef GGSWAN_SMS
    uint8_t v = gg_cram[i];
    return (uint16_t) ((v & 3) * 5) | (uint16_t) (((v >> 2) & 3) * 5) << 4
        | (uint16_t) (((v >> 4) & 3) * 5) << 8;
#else
    return gg_cram[i * 2] | (gg_cram[i * 2 + 1] << 8);
#endif
}
#endif
// the three 16 KiB ROM slots ($0000, $4000, $8000)
// a byte into the VRAM. sfswan on the WonderSwan: gg_vram is for reading
// (a ROM window on the VRAM's PSRAM bank); a write moves the RAM window
// there and back (main.c)
#if defined(GGSWAN_SF7000) && defined(__ia16__)
void gg_platform_vram_store(uint16_t a, uint8_t v);
#define GG_VRAM_STORE(a, v) gg_platform_vram_store(a, v)
#else
#define GG_VRAM_STORE(a, v) (gg_vram[a] = (v))
#endif
extern const uint8_t GGFAR *gg_slot[3];
const uint8_t GGFAR *z80_read_span(uint16_t a, uint16_t *len);

/* Provided by the platform */

/// Pointer to ROM bank "bank" (16 KiB) for slot 0..2.
const uint8_t GGFAR *gg_platform_bank(uint8_t slot, uint16_t bank);
/// Cartridge RAM (32 KiB), mapped at $8000 when enabled.
uint8_t GGFAR *gg_platform_cart_ram(uint8_t page);
/// Called at the start of each line, before the CPU runs it (only with
/// gg_every_line).
void gg_platform_line(uint16_t line);
/// A write to the PSG (port $7F) or the stereo register (port $06).
void gg_platform_psg(uint8_t value);
/// the end of the view (the start of line 168, after the last line the
/// Game Gear shows): the platform shows the frame as the Game Gear has
/// drawn it
void gg_platform_frame_end(void);
/// the PSG writes the recompiler queued (drc_state_t), in order
void gg_platform_psg_flush(void);
void gg_platform_stereo(uint8_t value);
/// smsswan: VDP status bits for the sprites as they are now (20h: two of
/// them overlap on the picture).
uint8_t gg_platform_sprite_status(void);
/// smsswan: the code at pc follows a read of the VDP status (the
/// interpreter's, or an IN the recompiler translates): if it tests bit 5,
/// the collision flag is worked out from now on (it costs time).
void gg_status_read_at(uint16_t pc);
/// VDP writes the platform may need to mirror (VRAM, CRAM, registers).
void gg_platform_vram_write(uint16_t addr, uint8_t value);
void gg_platform_cram_write(uint8_t index);
void gg_platform_vram_sync(void);
// the deferred writes to one VRAM address only (a read of it)
void gg_platform_vram_sync_at(uint16_t a);
uint8_t gg_platform_vram_defer(uint16_t addr, uint8_t value);
void gg_platform_vdp_reg(uint8_t reg, uint8_t value);

/* Machine */

void gg_init(uint16_t rom_banks);
void gg_reset(void);
/// The CPU core used by gg_run_frame (default: z80_run).
extern int32_t (*gg_cpu_run)(int32_t cycles);
/// Cycles left in the current CPU run, if not z80.cycles (recompiler).
extern int32_t (*gg_cpu_left)(void);
/// Stop the CPU at every line and call gg_platform_line (PC tests);
/// otherwise the CPU only stops where timing matters.
extern uint8_t gg_every_line;
/// The line being drawn (0-261), from the cycles run in this frame.
uint16_t gg_current_line(void);
/// Recompute the slot mapping from gg.mapper.
void gg_remap(void);
/// Run one frame (262 lines).
void gg_run_frame(void);
/// For a CPU core that ran out of cycles inside a frame (left: its cycles
/// left, 0 or less): start the lines reached and return the cycles to the
/// next event, exactly as the frame loop would (without returning to it);
/// -1 near or at the end of the frame (the CPU core must then return).
int32_t gg_line_event(int32_t left);
/// The most common line event at once (left: the CPU's cycles left, 0 or
/// less): the line interrupt, inside the picture, with the next event
/// being the next line interrupt. Returns the cycles to run up to that one
/// (the line started) or 0 if it was not the case (nothing changed: use
/// gg_line_event).
int16_t gg_fast_line(int16_t left);

/* VDP helpers */

static inline uint16_t gg_name_table_base(void) {
#ifdef GGSWAN_SG1000
    return (gg.vdp.reg[2] & 0x0F) << 10;
#endif
#ifdef GGSWAN_SMS
    if (gg.vdp.active == 224)
        return ((gg.vdp.reg[2] & 0x0C) << 10) | 0x700;
#endif
    return (gg.vdp.reg[2] & 0x0E) << 10;
}
static inline uint16_t gg_sat_base(void) {
#ifdef GGSWAN_SG1000
    return (gg.vdp.reg[5] & 0x7F) << 7;
#else
    return (gg.vdp.reg[5] & 0x7E) << 7;
#endif
}
#ifdef GGSWAN_SG1000
static inline uint8_t sg_mode(void) {
    if (gg.vdp.reg[0] & 0x02) return SG_MODE_G2;     // M3
    if (gg.vdp.reg[1] & 0x10) return SG_MODE_TEXT;   // M1
    if (gg.vdp.reg[1] & 0x08) return SG_MODE_MC;     // M2
    return SG_MODE_G1;
}
#endif
static inline bool gg_display_enabled(void) {
    return gg.vdp.reg[1] & 0x40;
}

#endif
