/**
 * gbswan - the Game Boy's picture with the WonderSwan Color's hardware
 * (GB-MEMORY-MAP.md), in its color mode with 2 bits a pixel:
 *
 * - the patterns are the Game Boy's own video memory, where it is;
 * - screen 2, seen only inside 160 x 144 in the middle, is the background
 *   and the window: each of the Game Boy's two maps has two WonderSwan
 *   maps kept next to it (their entries rewritten when its byte is), one
 *   for each place the patterns can be taken from (LCDC bit 4), and the
 *   scroll, the map and what is shown are changed at the lines the
 *   program changed them at, or where the window begins, by the line
 *   interrupt;
 * - color 0 of the background is clear: behind it are the sprites the
 *   Game Boy has "behind the background", then the backdrop, which has
 *   color 0's shade;
 * - screen 1 is the black at the two sides;
 * - the sprites are the Game Boy's 40 (an 8 x 16 one is two: the size is
 *   the one set at the sprite's top line), leftmost first, as the DMG
 *   puts them over one another.
 *
 * A window that does not start at the left edge (rare: wipes, panels at
 * the side) has no hardware of its own: on its lines screen 2, cut at the
 * window's edge, stays the background, and screen 1 becomes the window.
 * Screen 1 cannot be cut: on those lines the black at the sides is lost
 * (the columns of the window's map that a Game Boy never shows are there
 * instead), and so is what is to the left of the edge behind the
 * background's color 0.
 *
 * The background's palette is set at the lines too. Not done yet: the
 * sprites' palettes changed in the middle of the picture (the last ones
 * are the frame's). Not done, and not planned: the 10 sprites to a line
 * (the eleventh is shown: counting them by line costs more than it is
 * worth, and a game is better with it than without).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include <wonderful.h>
#include <ws.h>
#include "gb.h"
#include "gbv.h"
#include "gb_ws.h"
#include "gb_drc.h"
#include "../../ws/version.h"

#define SCR1        ((uint16_t*) 0x4000)
// The Game Boy's two maps (9800h, 9C00h) each as two WS maps: with the
// patterns from 8000h (LCDC bit 4 set: tile n is WS tile n) and with
// those from 8800h (clear: 0-127 are WS tiles 256-383). A program may
// change the bit in the middle of the picture (one set of patterns for
// the game, another for the status bar): the line's list names the map.
#define MAPS        ((uint16_t*) 0x4800)    // LCDC bit 4 set: 9800h's, then (5000h) 9C00h's
#define MAPS_88     ((uint16_t*) 0x1000)    // LCDC bit 4 clear: 9800h's, then (1800h) 9C00h's
#define SPR_TABLE   ((uint8_t*) 0x5800)
#define BASE_9800   0x98                    // port 07h: screen 1 at 4000h, screen 2 at 4800h
#define BASE_9C00   0xA8                    // ... screen 2 at 5000h
#define GB_X0 32

#define CTRL_BASE (WS_DISPLAY_CTRL_SCR1_ENABLE | WS_DISPLAY_CTRL_SCR2_WIN_INSIDE)

/* The changes of the frame being run, as the Game Boy's registers. */
typedef struct {
    uint8_t line, lcdc, scx, scy, wy, wx, bgp, pad;
} raw_t;
// (a program may change something on every line: room for all of them,
// in the internal RAM above the Game Boy's: GB-MEMORY-MAP.md)
#define RAW_MAX 146
#define raw ((raw_t*) 0xA000)        // (146 of 8 bytes: to A48Fh)
static uint8_t raw_n;

/* What the interrupts do: 16 bytes a change (gb_asm.s). Two lists: the one
   being shown and the one being made. */
#define OUT_MAX (RAW_MAX * 2 + 2)
#define list_a ((uint8_t*) 0xA500)     // (294 of 16 bytes: to B75Fh; the other to CA5Fh)
#define list_b ((uint8_t*) 0xB800)
static uint8_t *volatile list_shown = list_a;
static uint8_t *list_made = list_b;
static volatile uint8_t list_ready;
uint8_t *gbv_split_ptr;

volatile uint16_t gbv_vblanks;

/* What the plugin is doing, sampled at every vertical blank of the
   WonderSwan (75 a second): for the figures on the screen. */
volatile uint8_t gb_phase;
static volatile uint16_t phase_n[GB_PHASES];
static uint8_t stats_on;
static uint16_t stats_frames;       // Game Boy frames since the figures were last shown

/* The Game Boy's frames are started 200.91 WonderSwan lines apart (159
   lines 75.47 times a second; 59.73 Game Boy frames a second), by the
   line counter. (Until patch 163 the time was counted in whole
   WonderSwan frames, so every Game Boy frame started at a vertical blank
   and, being most of a frame long, ended near the next one: a little
   more work and its picture missed it, shown a frame late and the one
   after not at all. Evenly spaced they miss only when the two rates
   drift past each other, which is the one frame in five shown twice.) */
static volatile uint16_t line_base;     // lines to the last vertical blank interrupt
static uint16_t next_at, next_frac;     // when the next Game Boy frame may start (lines, 65536ths)
__attribute__((noinline)) uint16_t gbv_now_lines(void) {
    uint16_t b;
    uint8_t l;
    do {
        b = line_base;
        l = inportb(WS_DISPLAY_LINE_PORT);
    } while (b != line_base);
    return b + (l >= 144 ? l - 144 : l + 15);
}

void gb_vblank_irq(void) {
    gbv_vblanks++;
    phase_n[gb_phase]++;
    line_base += 159;
    if (list_ready) {
        uint8_t *t = list_shown;
        list_shown = list_made;
        list_made = t;
        list_ready = 0;
    }
    const uint8_t *l = list_shown;
    outportb(0x12, l[1]);
    outportb(0x13, l[2]);
    outportb(0x07, l[3]);
    outportb(0x00, l[4]);
    outportb(0x10, l[5]);
    outportb(0x11, l[6]);
    outportb(0x0A, l[7]);
    WS_DISPLAY_COLOR_MEM(0)[0] = *(const uint16_t*) (l + 8);
    WS_DISPLAY_COLOR_MEM(4)[1] = *(const uint16_t*) (l + 10);
    WS_DISPLAY_COLOR_MEM(4)[2] = *(const uint16_t*) (l + 12);
    WS_DISPLAY_COLOR_MEM(4)[3] = *(const uint16_t*) (l + 14);
    gbv_split_ptr = (uint8_t*) l + 16;
    outportb(WS_DISPLAY_LINE_IRQ_PORT, l[16]);
    ws_int_ack(WS_INT_ACK_VBLANK | WS_INT_ACK_LINE_MATCH);
}

// (gb_drc_asm.s, s_wrt, writes the two entries too)
void gbv_vram(uint16_t off, uint8_t v) {
    if (off >= 0x1800) {
        MAPS[off - 0x1800] = v | (4 << 9);
        MAPS_88[off - 0x1800] = (v >= 128 ? v : 256 + v) | (4 << 9);
    }
}

static void rebuild_maps(void) {
    for (uint16_t i = 0x1800; i < 0x2000; i++)
        gbv_vram(i, gb_vram[i]);
}

void gbv_oam(void) {
}

static void raw_first(void) {
    raw[0].line = 0;
    raw[0].lcdc = gb_io[GB_LCDC];
    raw[0].scx = gb_io[GB_SCX];
    raw[0].scy = gb_io[GB_SCY];
    raw[0].wy = gb_io[GB_WY];
    raw[0].wx = gb_io[GB_WX];
    raw[0].bgp = gb_io[GB_BGP];
    raw_n = 1;
}

void gbv_reg(uint8_t reg, uint8_t v) {
    if (reg > GB_SCX && reg < GB_WY && reg != GB_BGP) return;   // (LY, LYC, DMA, the sprites' palettes: at the frame's end)
    uint8_t line = gbf_draw_line();
    if (line >= 144) return;                    // (after the last line: the next frame's start has it)
    raw_t *r = &raw[raw_n - 1];
    if (r->line != line && raw_n < RAW_MAX) {
        r[1] = r[0];
        r++;
        r->line = line;
        raw_n++;
    }
    switch (reg) {
    case GB_LCDC: r->lcdc = v; break;
    case GB_SCX: r->scx = v; break;
    case GB_SCY: r->scy = v; break;
    case GB_WY: r->wy = v; break;
    case GB_BGP: r->bgp = v; break;
    default: r->wx = v; break;
    }
}

static const uint16_t __far shades[4] = { 0xFFF, 0xAAA, 0x555, 0x000 };

#define BASE_BORDER 0x08        // (port 07h's low half: screen 1 at 4000h)
static uint8_t *out;
// what: 0 the background, 1 the window from the left edge, 2 the window
// from further right (the background to its left)
static void emit(uint8_t line, const raw_t *s, uint8_t what, uint8_t win_y) {
    uint8_t *o = out;
    // (the same line twice: the later one)
    if (o != list_made && o[-16] == (uint8_t) (line - 1)) o -= 16;
    uint8_t ctrl = CTRL_BASE;
    if (s->lcdc & 0x80) {
        if (s->lcdc & 1) ctrl |= WS_DISPLAY_CTRL_SCR2_ENABLE;
        if (s->lcdc & 2) ctrl |= WS_DISPLAY_CTRL_SPR_ENABLE | WS_DISPLAY_CTRL_SPR_WIN_ENABLE;
    }
    // (port 07h's high half: the map at 4800h, 5000h, 1000h or 1800h)
    uint8_t bg_base = (s->lcdc & 0x08) ? 0xA0 : 0x90, win_base = (s->lcdc & 0x40) ? 0xA0 : 0x90;
    if (!(s->lcdc & 0x10)) {
        bg_base -= 0x70;
        win_base -= 0x70;
    }
    o[0] = line - 1;        // (set a line early: it shows from the next)
    o[5] = 0;
    o[6] = 0;
    o[7] = GB_X0 + 159;
    if (what == 1) {
        o[1] = (uint8_t) (7 - GB_X0) - s->wx;
        o[2] = win_y - line;
        o[3] = win_base | BASE_BORDER;
    } else {
        o[1] = s->scx - GB_X0;
        o[2] = s->scy;
        o[3] = bg_base | BASE_BORDER;
        if (what == 2) {
            o[3] = bg_base | win_base >> 4;
            o[5] = (uint8_t) (7 - GB_X0) - s->wx;
            o[6] = win_y - line;
            o[7] = GB_X0 + s->wx - 8;
        }
    }
    o[4] = ctrl;
    // the background's palette from this line: color 0's shade (the
    // backdrop's), then colors 1-3
    uint16_t *c = (uint16_t*) (o + 8);
    for (uint8_t i = 0; i < 4; i++)
        c[i] = shades[(s->bgp >> (i * 2)) & 3];
    out = o + 16;
}

// the frame's changes as the interrupts' list
static void make_list(void) {
    out = list_made;
    uint8_t win_at = 0xFF;      // the line WY was met at
    uint8_t win_y = 0;          // the window's own line counter
    for (uint8_t i = 0; i < raw_n; i++) {
        const raw_t *s = &raw[i];
        uint8_t from = s->line, to = i + 1 < raw_n ? raw[i + 1].line : 144;
        if (win_at == 0xFF && s->wy >= from && s->wy < to) win_at = s->wy;
        bool shown = (s->lcdc & 0x21) == 0x21 && s->wx < 167 && win_at != 0xFF;
        if (!shown) {
            emit(from, s, 0, 0);
        } else {
            uint8_t w = win_at > from ? win_at : from;
            if (w > from) emit(from, s, 0, 0);
            emit(w, s, s->wx > 7 ? 2 : 1, win_y);
            win_y += to - w;
        }
    }
    out[0] = 0xFF;
}

// the sprites' palettes (the background's is in the lists: it may change
// at a line)
static void palettes(void) {
    uint8_t p;
    p = gb_io[GB_OBP0];
    for (uint8_t i = 1; i < 4; i++)
        WS_DISPLAY_COLOR_MEM(12)[i] = shades[(p >> (i * 2)) & 3];
    p = gb_io[GB_OBP1];
    for (uint8_t i = 1; i < 4; i++)
        WS_DISPLAY_COLOR_MEM(13)[i] = shades[(p >> (i * 2)) & 3];
}

// the Game Boy's sprites as the WonderSwan's: leftmost first (the same X:
// OAM order), which is who covers whom on the DMG. In assembly
// (gb_asm.s: gbv_sprites_asm; in C it took a fifth of a frame's time)
// The sprites' size (LCDC bit 2) may change in the middle of the picture
// (dmg-acid2's mouth): each sprite has the size set at its top line, from
// a table by its Y (the line + 16), made from the frame's changes
#define SPR_TALL ((uint8_t*) 0xCB80)
static uint8_t tall_made = 0xFF;    // what the table is all filled with, FEh: not one value
static void sprites(void) {
    uint8_t t = raw[raw_n - 1].lcdc & 4, mixed = 0;
    for (uint8_t i = 0; i + 1 < raw_n; i++)
        if ((raw[i].lcdc & 4) != t) mixed = 1;
    if (!mixed) {
        if (tall_made != t) {
            memset(SPR_TALL, t, 176);
            tall_made = t;
        }
    } else {
        memset(SPR_TALL, raw[0].lcdc & 4, 16);
        for (uint8_t i = 0; i < raw_n; i++) {
            uint8_t from = raw[i].line, to = i + 1 < raw_n ? raw[i + 1].line : 160;
            if (to > from) memset(SPR_TALL + 16 + from, raw[i].lcdc & 4, to - from);
        }
        tall_made = 0xFE;
    }
    outportb(WS_SPR_COUNT_PORT, gbv_sprites_asm());
}

/* ---- the figures (START + Y4): in the black at the left, 4 characters
   wide, once a second: frames shown a second, then the share of the time
   (in hundredths) spent in translated code and its routines, in the
   dispatcher, translating, on the picture, waiting for the frame's time;
   blocks translated (hex) and times the code was all forgotten ---- */

#define FONT_TILE 928
static const char __far font_set[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.:/-_()";

static void stat_put(uint8_t row, const char __far *t) {
    for (uint8_t c = 0; c < 4; c++) {
        uint8_t k = 0;
        if (*t) {
            for (uint8_t i = 0; font_set[i]; i++)
                if (font_set[i] == *t) { k = i; break; }
            t++;
        }
        SCR1[row * 32 + c] = 0x2000 | ((FONT_TILE + k) & 511) | (1 << 9);
    }
}
static void stat_num(uint8_t row, uint16_t v, uint8_t hex) {
    char b[5];
    for (int8_t i = 3; i >= 0; i--) {
        uint8_t d = hex ? v & 15 : v % 10;
        b[i] = d < 10 ? '0' + d : 'A' + d - 10;
        v = hex ? v >> 4 : v / 10;
    }
    b[4] = 0;
    stat_put(row, b);
}

void gbv_stats_toggle(void) {
    stats_on ^= 1;
    for (uint8_t r = 0; r < 18; r++)
        for (uint8_t c = 0; c < 4; c++)
            SCR1[r * 32 + c] = 0x2000 | 511 | (1 << 9);
    ia16_disable_irq();
    gbv_vblanks = 0;
    for (uint8_t i = 0; i < GB_PHASES; i++) phase_n[i] = 0;
    ia16_enable_irq();
    stats_frames = 0;
}

static void stats_show(void) {
    static const char __far names[GB_PHASES][4] = { "RUN", "DSP", "TRN", "VID", "IDL" };
    uint16_t n[GB_PHASES], total;
    ia16_disable_irq();
    total = gbv_vblanks;
    gbv_vblanks = 0;
    for (uint8_t i = 0; i < GB_PHASES; i++) { n[i] = phase_n[i]; phase_n[i] = 0; }
    ia16_enable_irq();
    stat_put(0, "FPS");
    stat_num(1, (uint32_t) stats_frames * 7547 / ((uint32_t) total * 100), 0);
    stats_frames = 0;
    for (uint8_t i = 0; i < GB_PHASES; i++) {
        stat_put(2 + i * 2, names[i]);
        stat_num(3 + i * 2, n[i] * 100 / total, 0);
    }
    stat_put(12, "BLK");
    stat_num(13, drc_blocks, 1);
    stat_put(14, "FLS");
    stat_num(15, drc_flushes, 0);
    stat_put(16, "PAT");                // the patch this build is
    stat_num(17, GGSWAN_PATCH, 0);
}

// wait for the next Game Boy frame's time (gbrun.py leaves this function's
// cycles out of what a frame costs)
__attribute__((noinline)) void gbv_wait(void) {
    uint16_t f = next_frac;
    next_frac += 59775u;
    next_at += 200 + (next_frac < f);
    int16_t late = gbv_now_lines() - next_at;
    if (late > 400) next_at += late;        // (slow: the time lost is not made up)
    else while ((int16_t) (gbv_now_lines() - next_at) < 0) { }
}

void gbv_frame(void) {
    uint8_t was = gb_phase;
    gb_phase = GB_PHASE_VIDEO;
    // The list for the vertical blank interrupt, which shows it and gives
    // back the other one. Ready as soon as it is made, and not ready while
    // it is being made: until patch 163 it was marked ready after the wait
    // below, by when the interrupt (served during the wait) could already
    // have taken it; the mark then stood for the other list, an old
    // frame's, which the next interrupt showed: a scrolling picture went
    // a step back every few frames.
    // (a list not shown yet is not made again over: its frame would never
    // be seen, and the scroll would jump two steps. It is shown at the next
    // vertical blank; this only waits when two frames end inside one
    // WonderSwan frame)
    while (list_ready)
        ia16_halt();
    make_list();
    list_ready = 1;
    sprites();
    palettes();
    gb_sound_frame();
    if (stats_on) {
        stats_frames++;
        if (gbv_vblanks >= 75) stats_show();
    }
    // the frame's time (none to wait for when the plugin is slow)
    gb_phase = GB_PHASE_IDLE;
    gbv_wait();
    gb_phase = GB_PHASE_VIDEO;
    raw_first();
    gb_frame_keys();
    gb_phase = was;
}

void gbv_reset(void) {
    ia16_disable_irq();
    outportb(WS_DISPLAY_CTRL_PORT, 0);
    // tile 1023: color 0. Screen 1: clear in the middle (palette 4), black
    // at the sides (palette 1, where color 0 is seen and is black)
    memset((uint8_t*) 0x5FF0, 0, 16);
    for (uint8_t r = 0; r < 32; r++)
        for (uint8_t c = 0; c < 32; c++)
            SCR1[r * 32 + c] = 0x2000 | 511 | ((c >= 4 && c < 24 ? 4 : 1) << 9);
    for (uint8_t i = 0; i < 4; i++) {
        WS_DISPLAY_COLOR_MEM(1)[i] = i == 1 ? 0xFFF : 0x000;    // (color 1: the figures' white)
        WS_DISPLAY_COLOR_MEM(4)[i] = 0xFFF;
    }
    rebuild_maps();
    memset(SPR_TABLE, 0, 0x200);
    for (uint8_t i = 0; i < 40; i++)        // (gb_asm.s: the sprites' order, to start with the OAM's)
        ((uint8_t*) 0xCB00)[i] = i * 4;
    outportb(WS_SPR_BASE_PORT, 0x5800 >> 9);
    outportb(WS_SPR_FIRST_PORT, 0);
    outportb(WS_SPR_COUNT_PORT, 0);
    outportb(0x10, 0);
    outportb(0x11, 0);
    outportb(0x01, 0);      // the backdrop: palette 0's color 0
    outportb(WS_SCR2_WIN_X1_PORT, GB_X0); outportb(WS_SCR2_WIN_Y1_PORT, 0);
    outportb(WS_SCR2_WIN_X2_PORT, GB_X0 + 159); outportb(WS_SCR2_WIN_Y2_PORT, 143);
    outportb(WS_SPR_WIN_X1_PORT, GB_X0); outportb(WS_SPR_WIN_Y1_PORT, 0);
    outportb(WS_SPR_WIN_X2_PORT, GB_X0 + 159); outportb(WS_SPR_WIN_Y2_PORT, 143);
    palettes();
    raw_first();
    make_list();
    list_ready = 1;
    next_at = gbv_now_lines();
    ws_int_set_handler(WS_INT_VBLANK, (ia16_int_handler_t) gb_vblank_entry);
    ws_int_set_handler(WS_INT_LINE_MATCH, (ia16_int_handler_t) gb_line_entry);
    outportb(WS_DISPLAY_LINE_IRQ_PORT, 0xFF);
    ws_int_enable(WS_INT_ENABLE_VBLANK | WS_INT_ENABLE_LINE_MATCH);
    ia16_enable_irq();
}
