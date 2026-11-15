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
uint8_t gbv_raw_n;      // (gb_drc_asm.s, s_vreg, adds to the list too)
#define raw_n gbv_raw_n

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

/* ---- the patterns a frame late ----
   The WonderSwan shows the patterns as they are at the moment, and the
   picture of a Game Boy frame only once that frame is over: the sprites
   as frame N had them are on the screen while frame N + 1 runs. A program
   that draws a figure's next pose into patterns its sprites no longer use
   (Donkey Kong Land's Kong: two sets of tiles, written a few tiles a line
   in the last lines of the picture and in the vertical blank, the sprites
   moved to the finished set) is writing into the ones still on the
   WonderSwan's screen. When the program is seen to do that (gbv_frame:
   the last pattern written since line 0 is one of a sprite shown), the
   writes to the
   patterns that go through gb_drc_asm.s's s_wrt or gbf_write (the other
   ways are closed then) are kept by tile, in sets by the Game Boy frame
   they were made in (line 0 to line 0), and a set is copied to the
   patterns by the vertical blank interrupt that first shows the picture
   of the frame after: what the Game Boy would have shown with it.
   The program reading a pattern back gets the old one meanwhile. */
#define TQ_S 32                             // tiles a set
#define TQ_IDX ((uint8_t*) 0xCC30)          // by tile: its copy in the set being made, + 1 (384; gb_drc_asm.s)
#define TQ_ADR ((uint16_t*) 0xCDB0)         // 3 sets of TQ_S: where the tile is
#define TQ_PIDX ((uint8_t*) 0xCE70)         // by tile: its newest copy waiting, 1 + its place among all the copies, or 0 (384)
#define TQ_DAT ((uint8_t*) 0xF800)          // 3 sets of TQ_S tiles
uint8_t gbv_tq_on;                          // (gb_drc_asm.s)
uint16_t gbv_tq_base;                       // the set being made's tiles, less one
uint16_t gbv_tq_last, gbv_tq_lastp;         // the tile written last (its Game Boy address) and its copy (gb_drc_asm.s)
typedef struct { uint16_t w[8]; } tq_tile;
uint16_t gbv_pw_last;                       // the last pattern byte written (its Game Boy address), 0: none
uint8_t gbv_tq_n;                           // tiles in the set being made (gb_asm.s)
uint8_t gbv_tq_cur;                         // which set that is
#define tq_n gbv_tq_n
#define tq_cur gbv_tq_cur
static volatile uint8_t tq_pend;            // sets made and not copied yet (the oldest first, before tq_cur)
static uint8_t tq_cnt[3];
static volatile uint8_t tq_mark;            // how many of them the list that is ready goes with
#define TQ_FRAMES 1
static uint8_t tq_left;                     // frames the patterns are still kept late for

// (gb_asm.s) n tiles of a set to the patterns
void gbv_tq_copy_asm(const uint16_t *adr, const uint8_t *dat, uint16_t n);
static void tq_copy(uint8_t g, uint8_t n) {
    gbv_tq_copy_asm(TQ_ADR + g * TQ_S, TQ_DAT + g * (TQ_S * 16), n);
}
static inline uint8_t tq_before(uint8_t g) { return g ? g - 1 : 2; }
// (with the interrupts off, or in the interrupt)
static void tq_apply(uint8_t sets) {
    while (sets-- && tq_pend) {
        uint8_t g = tq_before(tq_cur);
        if (tq_pend == 2) g = tq_before(g);
        tq_copy(g, tq_cnt[g]);
        tq_pend--;
    }
}
static void tq_set_base(void) {
    gbv_tq_base = (uint16_t) (TQ_DAT + tq_cur * (TQ_S * 16)) - 16;
}
// everything kept, to the patterns now
static void tq_flush(void) {
    ia16_disable_irq();
    tq_apply(3);
    tq_mark = 0;
    tq_copy(tq_cur, tq_n);
    for (uint8_t i = 0; i < tq_n; i++)
        TQ_IDX[(TQ_ADR[tq_cur * TQ_S + i] - 0x2000) >> 4] = 0;
    tq_n = 0;
    gbv_tq_last = 0;
    ia16_enable_irq();
}
// no copy left in the set being made: no more copies, the patterns are
// written as they come (gb_asm.s, gbv_tq_new)
void gbv_tq_full(void) {
    tq_flush();
    gbv_tq_on = 0;
    tq_left = 0;
}
// (gb_asm.s) a copy for the tile at a (the Game Boy's address) in the set
// being made: what the tile will be when the set is copied (the newest
// copy of it waiting, or the tile); none if the set is full
void gbv_tq_new(uint16_t a);
// (gbf_write: what is not written by s_wrt)
void gbv_tq_put(uint16_t a, uint8_t v) {
    uint8_t k = TQ_IDX[(a - 0x8000) >> 4];
    if (!k) {
        gbv_tq_new(a);
        k = TQ_IDX[(a - 0x8000) >> 4];
        if (!k) { *(uint8_t*) (a - 0x6000) = v; return; }
    }
    *(uint8_t*) (gbv_tq_base + k * 16 + (a & 15)) = v;
}
// the Game Boy's line 0: the writes from here on are another frame's
void gbv_line0(void) {
    // (only what is written while the picture is drawn counts as "into
    // the sprites on the screen": gbv_frame looks at line 144. A program
    // that loads its sprites' patterns in the vertical blank, as most
    // do, is left alone)
    gbv_pw_last = 0;
    if (!gbv_tq_on || !tq_n) return;
    if (tq_pend == 2) {                     // (not met: the first is copied before the third is begun)
        tq_flush();
        return;
    }
    for (uint8_t i = 0; i < tq_n; i++) {
        uint16_t t = (TQ_ADR[tq_cur * TQ_S + i] - 0x2000) >> 4;
        TQ_IDX[t] = 0;
        TQ_PIDX[t] = tq_cur * TQ_S + i + 1;
    }
    tq_cnt[tq_cur] = tq_n;
    ia16_disable_irq();
    tq_cur = tq_cur == 2 ? 0 : tq_cur + 1;
    tq_pend++;
    ia16_enable_irq();
    tq_n = 0;
    gbv_tq_last = 0;
    tq_set_base();
}
// at a frame's end, before its list is ready and its sprites are set
static void tq_frame(void) {
    if (!gb.lcd_on) {
        if (gbv_tq_on) { tq_flush(); gbv_tq_on = 0; }
        tq_left = 0;
        gbv_pw_last = 0;
        return;
    }
    if (gbv_pw_last) {
        // is it a pattern of a sprite on the screen (the last frame's)?
        uint16_t t = (gbv_pw_last - 0x8000) >> 4;
        const uint16_t *s = (const uint16_t*) SPR_TABLE;
        for (uint8_t n = inportb(WS_SPR_COUNT_PORT); n; n--, s += 2)
            if ((*s & 0x1FF) == t) { tq_left = TQ_FRAMES; break; }
        // or of one the program has just put there? (Donkey Kong Land
        // draws Kong's next picture in the tiles that are not shown and
        // then points his sprites at them: by the last frame's sprites
        // alone the patterns were kept late only when a picture happened
        // to be drawn over the one shown - patch 177. Two tiles at a
        // time: a sprite of 16 lines is two.)
        if (t < 256) {
            const uint8_t *o = gb_oam;
            for (uint8_t n = 40; n; n--, o += 4)
                if (!((o[2] ^ (uint8_t) t) & 0xFE) && (uint8_t) (o[0] - 1) < 159) { tq_left = TQ_FRAMES; break; }
        }
        gbv_pw_last = 0;
    }
    if (tq_left) {
        if (!gbv_tq_on) { tq_set_base(); gbv_tq_on = 1; }
        // (and they stay late until the screen is turned off: with an end
        // after some frames, the first picture drawn after it was broken)
    }
    tq_mark = tq_pend;
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
        if (tq_mark) {
            tq_apply(tq_mark);
            tq_mark = 0;
        }
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
// The OAM copied over (DMA) while the picture is being drawn. The frame's
// sprites are set from the OAM at the frame's end: a program that copies
// its sprites there at line 128 of every frame (Donkey Kong Land) had the
// sprites of the frame to come on a picture whose patterns are this
// frame's - the coin of its "Select Game", turning, was in pieces for a
// frame at every step. What the OAM was before the frame's first such
// copy is kept (9D00h) and is what the sprites are set from.
#define OAM_KEPT ((uint8_t*) 0x9D00)
uint8_t gbv_oam_hi;             // (gb_asm.s, gbv_sprites_asm: FFh, the OAM kept; 0, the OAM)
static uint8_t oam_kept;
void gbv_oam_mid(void) {
    if (oam_kept) return;
    memcpy(OAM_KEPT, gb_oam, 0xA0);
    oam_kept = 1;
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
    bool plain = false;         // the entry before: the background alone
    for (uint8_t i = 0; i < raw_n; i++) {
        const raw_t *s = &raw[i];
        uint8_t from = s->line, to = i + 1 < raw_n ? raw[i + 1].line : 144;
        if (win_at == 0xFF && s->wy >= from && s->wy < to) win_at = s->wy;
        bool shown = (s->lcdc & 0x21) == 0x21 && s->wx < 167 && win_at != 0xFF;
        if (!shown) {
            if (plain && s->lcdc == s[-1].lcdc && s->bgp == s[-1].bgp) {
                // Only the scroll has changed (a wave: ZAS's logo, a
                // change at every line): a short entry, the line and
                // screen 2's scroll, marked by FFh in the first color's
                // high byte (a color is 0FFFh at most). The interrupt
                // then sets those two ports and nothing else. (Until
                // patch 168 every entry was a whole one: 144 of them
                // made each frame, eleven ports and colors set at each
                // line of every WonderSwan frame.)
                uint8_t *o = out;
                o[0] = from - 1;
                o[1] = s->scx - GB_X0;
                o[2] = s->scy;
                o[9] = 0xFF;
                out = o + 16;
                continue;
            }
            emit(from, s, 0, 0);
            plain = true;
        } else {
            plain = false;
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
    gbv_oam_hi = oam_kept ? 0xFF : 0;
    oam_kept = 0;
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

// a word at the top of the left border, for a second or so (gb_main.c:
// a state kept or brought back)
static uint8_t note_left;
void gbv_note(const char __far *t) {
    stat_put(0, t);
    note_left = 60;
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
    tq_frame();
    make_list();
    // The WonderSwan takes the sprites' table for the next frame at line
    // 142; the list is taken by the interrupt at line 144. A table still
    // being written at line 142 was half taken, and one written after it
    // was a frame behind its list (a figure whose patterns change, in
    // pieces for a frame now and then). So: the table is not begun when
    // it would not be done by line 142 (spr_lines: the most lines it has
    // taken, and two) - the frame then waits for the interrupt and is
    // shown a frame later, whole - and the list is ready after it.
    {
        static uint8_t spr_lines = 12;
        uint8_t l = inportb(0x02);
        if (l < 144 && (uint8_t) (l + spr_lines) >= 142) {
            uint16_t v = gbv_vblanks;
            while (gbv_vblanks == v)
                ia16_halt();
            l = inportb(0x02);
        }
        sprites();
        list_ready = 1;
        uint8_t e = inportb(0x02);
        if (e > l && (uint8_t) (e - l) < 60 && (uint8_t) (e - l + 2) > spr_lines) spr_lines = e - l + 2;
    }
    palettes();
    gb_sound_frame();
    if (note_left && !--note_left && !stats_on) stat_put(0, "");
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
    memset(TQ_IDX, 0, 384);
    memset(TQ_PIDX, 0, 384);
    gbv_tq_on = 0; gbv_tq_last = 0; tq_n = tq_cur = tq_pend = tq_mark = tq_left = 0; gbv_pw_last = 0;
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
