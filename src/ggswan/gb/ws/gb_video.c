/**
 * gbswan - the Game Boy's picture with the WonderSwan Color's hardware
 * (GB-MEMORY-MAP.md), in its color mode with 2 bits a pixel:
 *
 * - the patterns are the Game Boy's own video memory, where it is;
 * - screen 2, seen only inside 160 x 144 in the middle, is the background
 *   and the window: each of the Game Boy's two maps has a WonderSwan map
 *   kept next to it (an entry rewritten when its byte is), and the scroll,
 *   the map and what is shown are changed at the lines the program changed
 *   them at, or where the window begins, by the line interrupt;
 * - color 0 of the background is clear: behind it are the sprites the
 *   Game Boy has "behind the background", then the backdrop, which has
 *   color 0's shade;
 * - screen 1 is the black at the two sides;
 * - the sprites are the Game Boy's 40 (an 8 x 16 one is two), leftmost
 *   first, as the DMG puts them over one another.
 *
 * A window that does not start at the left edge (rare: wipes, panels at
 * the side) has no hardware of its own: on its lines screen 2, cut at the
 * window's edge, stays the background, and screen 1 becomes the window.
 * Screen 1 cannot be cut: on those lines the black at the sides is lost
 * (the columns of the window's map that a Game Boy never shows are there
 * instead), and so is what is to the left of the edge behind the
 * background's color 0.
 *
 * Not done yet: the palettes changed in the middle of the picture (the
 * last ones are the frame's); the 10 sprites to a line.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include <wonderful.h>
#include <ws.h>
#include "gb.h"
#include "gbv.h"
#include "gb_ws.h"

#define SCR1        ((uint16_t*) 0x4000)
#define MAPS        ((uint16_t*) 0x4800)    // of the Game Boy's 9800h, then (5000h) of its 9C00h
#define SPR_TABLE   ((uint8_t*) 0x5800)
#define BASE_9800   0x98                    // port 07h: screen 1 at 4000h, screen 2 at 4800h
#define BASE_9C00   0xA8                    // ... screen 2 at 5000h
#define GB_X0 32

#define CTRL_BASE (WS_DISPLAY_CTRL_SCR1_ENABLE | WS_DISPLAY_CTRL_SCR2_WIN_INSIDE)

/* The changes of the frame being run, as the Game Boy's registers. */
typedef struct {
    uint8_t line, lcdc, scx, scy, wy, wx;
} raw_t;
// (a program may change something on every line: room for all of them,
// in the internal RAM above the Game Boy's: GB-MEMORY-MAP.md)
#define RAW_MAX 146
#define raw ((raw_t*) 0xA000)
static uint8_t raw_n;

/* What the interrupts do: 8 bytes a change (gb_asm.s). Two lists: the one
   being shown and the one being made. */
#define OUT_MAX (RAW_MAX * 2 + 2)
#define list_a ((uint8_t*) 0xA400)
#define list_b ((uint8_t*) 0xB000)
static uint8_t *volatile list_shown = list_a;
static uint8_t *list_made = list_b;
static volatile uint8_t list_ready;
uint8_t *gbv_split_ptr;

static uint8_t tiles_8000;      // LCDC bit 4 the maps were made with
static volatile uint16_t frame_credit;
volatile uint16_t gbv_vblanks;

// every WonderSwan frame (75.47 a second) earns 5973, a Game Boy frame
// (59.73) costs 7547
#define FRAME_EARN 5973
#define FRAME_COST 7547

void gb_vblank_irq(void) {
    gbv_vblanks++;
    if (frame_credit < 3 * FRAME_COST) frame_credit += FRAME_EARN;
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
    gbv_split_ptr = (uint8_t*) l + 8;
    outportb(WS_DISPLAY_LINE_IRQ_PORT, l[8]);
    ws_int_ack(WS_INT_ACK_VBLANK | WS_INT_ACK_LINE_MATCH);
}

static inline uint16_t map_entry(uint8_t n) {
    return (tiles_8000 || n >= 128 ? n : 256 + n) | (4 << 9);
}

static void rebuild_maps(void) {
    const uint8_t *src = gb_vram + 0x1800;
    for (uint16_t i = 0; i < 0x800; i++)
        MAPS[i] = map_entry(src[i]);
}

void gbv_vram(uint16_t off, uint8_t v) {
    if (off >= 0x1800) MAPS[off - 0x1800] = map_entry(v);
}

void gbv_oam(void) {
}

static void raw_first(void) {
    raw[0].line = 0;
    raw[0].lcdc = gb.io[GB_LCDC];
    raw[0].scx = gb.io[GB_SCX];
    raw[0].scy = gb.io[GB_SCY];
    raw[0].wy = gb.io[GB_WY];
    raw[0].wx = gb.io[GB_WX];
    raw_n = 1;
}

void gbv_reg(uint8_t reg, uint8_t v) {
    if (reg > GB_SCX && reg < GB_WY) return;    // (LY, LYC, DMA, the palettes: at the frame's end)
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
    default: r->wx = v; break;
    }
}

#define BASE_BORDER 0x08        // (port 07h's low half: screen 1 at 4000h)
static uint8_t *out;
// what: 0 the background, 1 the window from the left edge, 2 the window
// from further right (the background to its left)
static void emit(uint8_t line, const raw_t *s, uint8_t what, uint8_t win_y) {
    uint8_t *o = out;
    // (the same line twice: the later one)
    if (o != list_made && o[-8] == (uint8_t) (line - 1)) o -= 8;
    uint8_t ctrl = CTRL_BASE;
    if (s->lcdc & 0x80) {
        if (s->lcdc & 1) ctrl |= WS_DISPLAY_CTRL_SCR2_ENABLE;
        if (s->lcdc & 2) ctrl |= WS_DISPLAY_CTRL_SPR_ENABLE | WS_DISPLAY_CTRL_SPR_WIN_ENABLE;
    }
    uint8_t bg_base = (s->lcdc & 0x08) ? 0xA0 : 0x90, win_base = (s->lcdc & 0x40) ? 0xA0 : 0x90;
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
    out = o + 8;
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

static const uint16_t __far shades[4] = { 0xFFF, 0xAAA, 0x555, 0x000 };

static void palettes(void) {
    uint8_t p = gb.io[GB_BGP];
    WS_DISPLAY_COLOR_MEM(0)[0] = shades[p & 3];
    for (uint8_t i = 1; i < 4; i++)
        WS_DISPLAY_COLOR_MEM(4)[i] = shades[(p >> (i * 2)) & 3];
    p = gb.io[GB_OBP0];
    for (uint8_t i = 1; i < 4; i++)
        WS_DISPLAY_COLOR_MEM(12)[i] = shades[(p >> (i * 2)) & 3];
    p = gb.io[GB_OBP1];
    for (uint8_t i = 1; i < 4; i++)
        WS_DISPLAY_COLOR_MEM(13)[i] = shades[(p >> (i * 2)) & 3];
}

// the Game Boy's sprites as the WonderSwan's: leftmost first (the same X:
// OAM order), which is who covers whom on the DMG
static void sprites(void) {
    uint8_t order[40], n = 0;
    uint8_t tall = gb.io[GB_LCDC] & 4;
    for (uint8_t i = 0; i < 40; i++) {
        const uint8_t *s = gb.oam + i * 4;
        if (s[0] == 0 || s[0] >= 160 || s[1] == 0 || s[1] >= 168) continue;
        if (!tall && s[0] <= 8) continue;
        uint8_t x = s[1], j = n++;
        while (j > 0 && gb.oam[order[j - 1] * 4 + 1] > x) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    uint8_t *o = SPR_TABLE;
    uint8_t count = 0;
    for (uint8_t k = 0; k < n; k++) {
        const uint8_t *s = gb.oam + order[k] * 4;
        uint8_t f = s[3];
        // palette 12 or 13 (4 or 5 of the sprites'), over screen 2 unless
        // "behind", the flips
        uint8_t hi = ((f & 0x10) ? 5 << 1 : 4 << 1) | ((f & 0x80) ? 0 : 0x20)
            | ((f & 0x20) ? 0x40 : 0) | ((f & 0x40) ? 0x80 : 0);
        uint8_t y = s[0] - 16, x = s[1] + (GB_X0 - 8);
        if (tall) {
            uint8_t top = s[2] & 0xFE, bottom = s[2] | 1;
            if (f & 0x40) { top |= 1; bottom &= 0xFE; }
            o[0] = top; o[1] = hi; o[2] = y; o[3] = x;
            o[4] = bottom; o[5] = hi; o[6] = y + 8; o[7] = x;
            o += 8;
            count += 2;
        } else {
            o[0] = s[2]; o[1] = hi; o[2] = y; o[3] = x;
            o += 4;
            count++;
        }
    }
    outportb(WS_SPR_COUNT_PORT, count);
}

void gbv_frame(void) {
    if ((gb.io[GB_LCDC] >> 4 & 1) != tiles_8000) {
        tiles_8000 = gb.io[GB_LCDC] >> 4 & 1;
        rebuild_maps();
    }
    make_list();
    sprites();
    palettes();
    // the frame's time (none to wait for when the plugin is slow)
    while (frame_credit < FRAME_COST)
        ia16_halt();
    ia16_disable_irq();
    frame_credit -= FRAME_COST;
    list_ready = 1;
    ia16_enable_irq();
    raw_first();
    gb_frame_keys();
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
        WS_DISPLAY_COLOR_MEM(1)[i] = 0x000;
        WS_DISPLAY_COLOR_MEM(4)[i] = 0xFFF;
    }
    tiles_8000 = gb.io[GB_LCDC] >> 4 & 1;
    rebuild_maps();
    memset(SPR_TABLE, 0, 0x200);
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
    frame_credit = FRAME_COST;
    ws_int_set_handler(WS_INT_VBLANK, (ia16_int_handler_t) gb_vblank_entry);
    ws_int_set_handler(WS_INT_LINE_MATCH, (ia16_int_handler_t) gb_line_entry);
    outportb(WS_DISPLAY_LINE_IRQ_PORT, 0xFF);
    ws_int_enable(WS_INT_ENABLE_VBLANK | WS_INT_ENABLE_LINE_MATCH);
    ia16_enable_irq();
}
