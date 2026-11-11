/**
 * sgswan - SG-1000 emulator for the WonderSwan (nileswan)
 * The TMS9918A on the WonderSwan's screen 1 and sprites: main.c includes
 * this in place of the Game Gear's video (built with -DGGSWAN_SG1000). See
 * SG1000.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Nothing of the TMS9918A's VRAM can be shown as it is: its patterns are 1
 * bit a pixel, their colors elsewhere (the Game Gear's VRAM is the WS's
 * tiles). The VRAM is kept as the game writes it, outside the internal RAM
 * (the 16 KiB after the ROM in its PSRAM bank, through the RAM window:
 * gg_vram), and the screen is made from it at the end of every frame:
 *
 *   - a write only sets a bit, one for 8 bytes of VRAM (sg_dirty);
 *   - the background is screen 1, an entry a character. A pattern with
 *     its colors becomes a tile when a character in the view first shows
 *     it: a key (which pattern, which colors: the character's number, with
 *     its third of the screen in Graphics II) has a slot, a tile of the
 *     WS, as long as characters in the view show it; a slot nobody shows
 *     waits in a ring with its tile, and is taken for another key only
 *     when its turn comes (a game that scrolls finds most of its tiles
 *     there again);
 *   - at the end of the frame: the slots whose pattern or color bytes
 *     were written are drawn again, then the characters whose names were
 *     written get their slot;
 *   - sprites: WS sprites, of tiles drawn in the sprite's color, one set
 *     for each pattern and color on the screen ("kind").
 *
 * No line effects: the chip has none (no scrolling, no line interrupt).
 * Text mode (40 columns of 6 pixels) is not drawn. The four sprites to a
 * line are not enforced: the WS shows them all.
 */

/* === What the rest of main.c shares with the Game Gear's video === */

// interrupts off, and back as they were
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

// (diagnostics, as the Game Gear's: show_fps. Here K is the tiles drawn in
// the last second, M the background slots in use)
#define WS_LINES 159
#define SG_DIAG __attribute__((unused))
static volatile uint8_t diag_late SG_DIAG, diag_ends SG_DIAG, diag_split_line SG_DIAG;
static uint16_t diag_commit, diag_splits;
volatile uint16_t diag_calls;
static uint16_t diag_regs SG_DIAG, diag_ram SG_DIAG;
static uint16_t diag_irq_line SG_DIAG, diag_vbl_pc SG_DIAG, diag_fg SG_DIAG;
static int16_t diag_left SG_DIAG;
static uint8_t diag_vbl_st SG_DIAG;
static bool diag_mode SG_DIAG;
// Y1 (Y up) held at the start: the diagnostics (FPS, the recompiler's
// numbers) on screen 2, over the sides of the picture. Else nothing:
// main.c writes them only while drc_st.prio_on is 0 (the Game Gear's
// "screen 2 holds the background's priority tiles")
static uint8_t sg_diag;
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
// a frame presented and not shown yet: the line interrupt at the end of
// the WS screen clears it
static volatile uint8_t frame_waiting;
#ifdef GGSWAN_HOST
// (the PC tools written for the Game Gear's scroll splits: none here)
volatile uint8_t act_line[1], act_val[1], act_n = 1;
uint32_t host_spr_builds, host_relayouts, host_backdrops;
#endif

uint16_t ws_map1_iram(void) {
    return IRAM_OFF(map1);
}
uint16_t ws_map2_iram(void) {
    return IRAM_OFF(map2);
}

/* === Tables === */

// tiles 0-159: the sprites' (a kind takes 1, 4 or 16 of them); the
// background's slots: 160-511, then 816-1021 (after the font: where the
// Game Gear's builds have the recompiler's buffer, drc_rt.c)
#define SG_SPR_TILES 160
#define SG_BG_FIRST SG_SPR_TILES
#define SG_NSLOT_LO (512 - SG_BG_FIRST)
#define SG_BG_HIGH 816
#define SG_NSLOT (SG_NSLOT_LO + 1022 - SG_BG_HIGH)
#define SG_NONE 0xFFFF
#define SG_BLANK TILE_ENTRY(1023)        // (screen 1: nothing there)
static inline uint16_t sg_slot_tile(uint16_t s) {
    return s < SG_NSLOT_LO ? SG_BG_FIRST + s : SG_BG_HIGH - SG_NSLOT_LO + s;
}
static inline uint16_t sg_slot_entry(uint16_t s) {
    uint16_t t = sg_slot_tile(s);
    return TILE_ENTRY(t);
}
static inline uint16_t sg_entry_slot(uint16_t e) {
    return (e & 0x2000) ? (e & 0x1FF) + 512 - SG_BG_HIGH + SG_NSLOT_LO : e - SG_BG_FIRST;
}
_Static_assert(FONT_TILE + 48 <= SG_BG_HIGH && TILE_ADDR(SG_BG_HIGH) == 0xA600, "sgswan: tiles");
/*
 * The slots' tables are not in the internal RAM (there is no room left:
 * tiles, the work RAM, the recompiler's buffer and caches) but in the
 * PSRAM bank after the ROM's, through the RAM window: only inside
 * video_present, between sg_tab_open() and sg_tab_close(), where the
 * window is moved there. The VRAM (the ROM's bank, where the window
 * normally is) is then read through the ROM's first window (sg_vr): what
 * the frame's end needs of it is all reading.
 */
#define SG_TABLE_BANK (GG_PSRAM_FIRST_BANK + 1)
static uint16_t __far *sg_slot_of;     // [1024] key -> slot
static uint16_t __far *sg_slot_key;    // [SG_NSLOT] slot -> key
static uint16_t __far *sg_slot_ref;    // [SG_NSLOT] characters showing it
static uint16_t __far *sg_ring;        // [SG_NSLOT] the slots nobody shows
static uint8_t __far *sg_slot_fl;      // [SG_NSLOT] 1 in the ring, 2 drawn in this pass
static uint16_t __far *sg_done;        // [SG_DONE_MAX] the slots drawn in this pass
#define SG_DONE_MAX 256
static const uint8_t __far *sg_vr;     // the VRAM, to read while the tables are open
static inline void sg_tab_open(void) {
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
    outportw(WS_CART_EXTBANK_RAM_PORT, SG_TABLE_BANK);
}
static inline void sg_tab_close(void) {
    ws_ram_window_restore();
}
// what was written to the VRAM (drc.h: the recompiler's routine for the
// data port sets them too): a byte for 16 bytes of it, a byte for 512
#define sg_dirty ((uint8_t*) IRAM(SG_DIRTY_ADDR))
#define sg_page ((uint8_t*) IRAM(SG_PAGE_ADDR))
#define sg_any (*(uint8_t*) IRAM(SG_ANY_ADDR))

static uint16_t sg_ring_head, sg_ring_n;
static uint16_t sg_done_n;         // slots drawn in this pass
static uint8_t sg_starved;             // a character found no slot
// the tables as the screen was made with (sg_tables)
static uint8_t sg_m = 0xFF;            // SG_MODE_*, or 4: text mode
static uint16_t sg_nt, sg_pg, sg_ct, sg_pmask, sg_cmask, sg_kmask;
static uint16_t sg_spg, sg_sat;
static uint8_t sg_big, sg_mag;         // sprites of 16x16, magnified
// the view as the screen was made with, and its characters
static uint8_t sg_vx = 0xFF, sg_vy = 0xFF;
static uint8_t sg_c0, sg_c1, sg_r0, sg_r1;
static uint8_t sg_spr_dirty = 1;

// the four planes of a pixel's color, a byte each (FFh: the bit is set),
// as the two words of a tile's row
static const uint16_t sg_plane_lo[16] = {
    0x0000, 0x00FF, 0xFF00, 0xFFFF, 0x0000, 0x00FF, 0xFF00, 0xFFFF,
    0x0000, 0x00FF, 0xFF00, 0xFFFF, 0x0000, 0x00FF, 0xFF00, 0xFFFF
};
static const uint16_t sg_plane_hi[16] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0x00FF, 0x00FF, 0x00FF, 0x00FF,
    0xFF00, 0xFF00, 0xFF00, 0xFF00, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF
};
// 4 pixels twice as wide
static const uint8_t sg_wide[16] = {
    0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F,
    0xC0, 0xC3, 0xCC, 0xCF, 0xF0, 0xF3, 0xFC, 0xFF
};

void gg_platform_vram_write(uint16_t a, uint8_t value) {
    (void) value;
    sg_dirty[a >> 4] = 1;
    sg_page[a >> 9] = 1;
    sg_any = 1;
}

// (for sg_sprite_status, gg.h: the sprite table or the sprites' patterns
// written since the frame was presented, where these marks are cleared)
uint8_t gg_platform_sprites_written(void) {
    if (!sg_any)
        return 0;
    const uint8_t *d = sg_page + ((gg.vdp.reg[6] & 7) << 2);
    if (d[0] | d[1] | d[2] | d[3])
        return 1;
    d = sg_dirty + (gg_sat_base() >> 4);
    for (uint8_t i = 0; i < 8; i++)
        if (d[i])
            return 1;
    return 0;
}

void gg_platform_vdp_reg(uint8_t r, uint8_t v) {
    (void) r; (void) v;   // (looked at when the frame is presented: sg_tables)
}
void gg_platform_cram_write(uint8_t i) { (void) i; }
void gg_platform_vram_sync(void) {}
void gg_platform_vram_sync_at(uint16_t a) { (void) a; }
uint8_t gg_platform_vram_defer(uint16_t a, uint8_t value) { (void) a; (void) value; return 0; }
void log_commit_pending(void) {}
#ifdef GGSWAN_HOST
void host_late_commit(void) {}
#endif

void gg_platform_line(uint16_t line) {
    // (only called on the PC tests, which stop at every line)
#ifdef GGSWAN_HOST
    host_line_hook(line);
#else
    (void) line;
#endif
}

/* === Background === */

// the key of a character: SG_NONE when nothing is drawn (display off, text)
static inline uint16_t sg_key(uint8_t row, uint8_t name) {
    switch (sg_m) {
    case SG_MODE_G2: return (name | ((uint16_t) (row >> 3) << 8)) & sg_kmask;
    case SG_MODE_G1: return name;
    case SG_MODE_MC: return name | ((uint16_t) (row & 3) << 8);
    default: return SG_NONE;
    }
}

// the tile of a slot from the VRAM
static void sg_convert(uint16_t slot, uint16_t key) {
    uint16_t *t = (uint16_t*) TILE_PTR(sg_slot_tile(slot));
    uint8_t p[8], c[8];
    diag_commit++;
    if (sg_m == SG_MODE_G2) {
        _fmemcpy(p, sg_vr + sg_pg + ((key & sg_pmask) << 3), 8);
        _fmemcpy(c, sg_vr + sg_ct + ((key & sg_cmask) << 3), 8);
        for (uint8_t r = 0; r < 8; r++) {
            uint16_t pp = p[r] * 0x0101u;
            uint8_t k = c[r];
            *t++ = (pp & sg_plane_lo[k >> 4]) | (~pp & sg_plane_lo[k & 15]);
            *t++ = (pp & sg_plane_hi[k >> 4]) | (~pp & sg_plane_hi[k & 15]);
        }
    } else if (sg_m == SG_MODE_G1) {
        _fmemcpy(p, sg_vr + ((sg_pg + (key << 3)) & 0x3FFF), 8);
        uint8_t k = sg_vr[(sg_ct + (key >> 3)) & 0x3FFF];
        uint16_t fl = sg_plane_lo[k >> 4], fh = sg_plane_hi[k >> 4];
        uint16_t bl = sg_plane_lo[k & 15], bh = sg_plane_hi[k & 15];
        for (uint8_t r = 0; r < 8; r++) {
            uint16_t pp = p[r] * 0x0101u;
            *t++ = (pp & fl) | (~pp & bl);
            *t++ = (pp & fh) | (~pp & bh);
        }
    } else {
        // multicolor: two bytes of the pattern, 4 rows each, a color for
        // the left 4 pixels and one for the right
        _fmemcpy(p, sg_vr + ((sg_pg + ((key & 0xFF) << 3) + ((key >> 8) << 1)) & 0x3FFF), 2);
        for (uint8_t r = 0; r < 8; r++) {
            uint8_t k = p[r >> 2];
            *t++ = (sg_plane_lo[k >> 4] & 0xF0F0) | (sg_plane_lo[k & 15] & 0x0F0F);
            *t++ = (sg_plane_hi[k >> 4] & 0xF0F0) | (sg_plane_hi[k & 15] & 0x0F0F);
        }
    }
}

static void sg_pool_reset(void) {
    for (uint16_t k = 0; k < 1024; k++)
        sg_slot_of[k] = SG_NONE;
    for (uint16_t s = 0; s < SG_NSLOT; s++) {
        sg_slot_key[s] = SG_NONE;
        sg_slot_ref[s] = 0;
        sg_ring[s] = s;
        sg_slot_fl[s] = 1;
    }
    sg_ring_head = 0;
    sg_ring_n = SG_NSLOT;
    for (uint16_t i = 0; i < 32 * 32; i++)
        map1[i] = SG_BLANK;
    sg_starved = 0;
}

static inline void sg_unref(uint16_t s) {
    if (--sg_slot_ref[s] == 0 && !(sg_slot_fl[s] & 1)) {
        uint16_t at = sg_ring_head + sg_ring_n;
        if (at >= SG_NSLOT)
            at -= SG_NSLOT;
        sg_ring[at] = s;
        sg_ring_n++;
        sg_slot_fl[s] |= 1;
    }
}

// a slot for a key that has none: the one that waited longest
static uint16_t sg_alloc(uint16_t key) {
    while (sg_ring_n) {
        uint16_t s = sg_ring[sg_ring_head];
        if (++sg_ring_head == SG_NSLOT)
            sg_ring_head = 0;
        sg_ring_n--;
        sg_slot_fl[s] &= ~1;
        if (sg_slot_ref[s])
            continue;   // (shown again since it came here)
        if (sg_slot_key[s] != SG_NONE)
            sg_slot_of[sg_slot_key[s]] = SG_NONE;
        sg_slot_key[s] = key;
        sg_slot_of[key] = s;
        sg_convert(s, key);
        return s;
    }
    return SG_NONE;
}

// a character of the view: the slot of its name now
static void sg_cell(uint8_t row, uint8_t col, uint8_t name) {
    uint16_t *m = &map1[row * 32 + col];
    uint16_t old = *m, e = SG_BLANK;
    uint16_t key = sg_key(row, name);
    if (key != SG_NONE) {
        uint16_t s = sg_slot_of[key];
        if (s == SG_NONE)
            s = sg_alloc(key);
        if (s == SG_NONE) {
            sg_starved = 1;
        } else {
            e = sg_slot_entry(s);
            if (e == old)
                return;
            sg_slot_ref[s]++;
        }
    }
    if (e == old)
        return;
    *m = e;
    if (old != SG_BLANK)
        sg_unref(sg_entry_slot(old));
}

// the characters of the view, all of them (the view moved to other
// characters, the tables changed, a character had found no slot)
static void sg_relayout(void) {
    uint8_t names[32];
#ifdef GGSWAN_HOST
    host_relayouts++;
#endif
    sg_starved = 0;
    // (first the ones that left the view: their slots for the others)
    for (uint8_t row = 0; row < 24; row++) {
        uint16_t *m = &map1[row * 32];
        for (uint8_t col = 0; col < 32; col++, m++) {
            if (*m == SG_BLANK
                    || (row >= sg_r0 && row <= sg_r1 && col >= sg_c0 && col <= sg_c1))
                continue;
            sg_unref(sg_entry_slot(*m));
            *m = SG_BLANK;
        }
    }
    for (uint8_t row = sg_r0; row <= sg_r1; row++) {
        _fmemcpy(names, sg_vr + ((sg_nt + row * 32) & 0x3FFF), 32);
        for (uint8_t col = sg_c0; col <= sg_c1; col++)
            sg_cell(row, col, names[col]);
    }
}

// a slot's pattern or colors were written
static void sg_key_dirty(uint16_t key) {
    uint16_t s = sg_slot_of[key];
    if (s == SG_NONE || (sg_slot_fl[s] & 2))
        return;
    if (!sg_slot_ref[s]) {
        // (nobody shows it: it waits in the ring, with no tile now)
        sg_slot_of[key] = SG_NONE;
        sg_slot_key[s] = SG_NONE;
        return;
    }
    sg_convert(s, key);
    sg_slot_fl[s] |= 2;
    if (sg_done_n < SG_DONE_MAX)
        sg_done[sg_done_n] = s;
    sg_done_n++;
}

// 8 bytes of VRAM at a were written: the tiles made from them
static void sg_unit_tiles(uint16_t a) {
    uint16_t off;
    switch (sg_m) {
    case SG_MODE_G2:
        // (a character's pattern: the keys of the three thirds that the
        // masks send there, often one)
        off = a - sg_pg;
        if (off < 0x1800) {
            uint16_t ch = off >> 3;
            for (uint16_t t = 0; t < 0x300; t += 0x100) {
                uint16_t k = ((ch & 0xFF) | t) & sg_kmask;
                if ((k & sg_pmask) == ch)
                    sg_key_dirty(k);
            }
        }
        off = a - sg_ct;
        if (off < 0x1800) {
            uint16_t ch = off >> 3;
            for (uint16_t t = 0; t < 0x300; t += 0x100) {
                uint16_t k = ((ch & 0xFF) | t) & sg_kmask;
                if ((k & sg_cmask) == ch)
                    sg_key_dirty(k);
            }
        }
        break;
    case SG_MODE_G1:
        off = a - sg_pg;
        if (off < 0x800)
            sg_key_dirty(off >> 3);
        off = a - sg_ct;
        if (off < 32)   // (a color byte for 8 patterns)
            for (uint16_t k = off << 3; k < (off << 3) + 64 && k < 256; k++)
                sg_key_dirty(k);
        break;
    case SG_MODE_MC:
        off = a - sg_pg;
        if (off < 0x800)
            for (uint16_t r = 0; r < 0x400; r += 0x100)
                sg_key_dirty((off >> 3) | r);
        break;
    }
}

// the registers: what changed since the screen was made (1: the tiles are
// no longer what they were, everything again)
static uint8_t sg_tables(void) {
    const uint8_t *reg = gg.vdp.reg;
    uint8_t m = sg_mode();
    uint16_t nt = gg_name_table_base(), pg, ct, pm = 0x3FF, cm = 0x3FF;
    if (m == SG_MODE_TEXT)
        m = 4;
    if (m == SG_MODE_G2) {
        cm = ((reg[3] & 0x7F) << 3) | 7;
        pm = ((reg[4] & 3) << 8) | (cm & 0xFF);
        pg = (reg[4] & 4) << 11;
        ct = (reg[3] & 0x80) << 6;
    } else {
        pg = (reg[4] & 7) << 11;
        ct = (uint16_t) reg[3] << 6;
    }
    uint16_t spg = (reg[6] & 7) << 11, sat = gg_sat_base();
    uint8_t big = (reg[1] & 0x02) != 0, mag = (reg[1] & 0x01) != 0;
    if (spg != sg_spg || sat != sg_sat || big != sg_big || mag != sg_mag) {
        sg_spg = spg; sg_sat = sat; sg_big = big; sg_mag = mag;
        sg_spr_dirty = 2;   // (the kinds too)
    }
    if (m == sg_m && nt == sg_nt && pg == sg_pg && ct == sg_ct && pm == sg_pmask && cm == sg_cmask)
        return 0;
    // (a name table elsewhere with the same tiles: only the characters)
    uint8_t all = !(m == sg_m && pg == sg_pg && ct == sg_ct && pm == sg_pmask && cm == sg_cmask);
    sg_m = m; sg_nt = nt; sg_pg = pg; sg_ct = ct; sg_pmask = pm; sg_cmask = cm;
    sg_kmask = pm | cm;
    return 1 + all;
}

/* === Text mode === */

/*
 * 40 characters of 6 pixels to a line, 24 lines: 240 pixels in the middle
 * of the 256 (8 of the backdrop's color on each side), one color for all
 * the text (register 7's high half) on the backdrop's. A character does
 * not fall on a WonderSwan tile: a tile of the screen shows the end of
 * one and the start of the next, in one of three ways (the 8 pixels start
 * 0, 2 or 4 pixels into a character). So no pool of tiles shared by
 * what looks the same: every tile of the screen in the view has its own
 * (the view is 29 by 19 at most: 551 of the 558), found by its place (row
 * and column, modulo 19 and 29: a tile that leaves the view on one side
 * gives its place to the one that enters on the other, so that moving the
 * view draws only what comes in). The tables of the other modes keep,
 * for each of these tiles, which place of the screen it is drawing
 * (sg_slot_ref: + 1; 0: none) and the two characters it was made from
 * (sg_slot_key). No sprites in this mode.
 */
#define SG_TXT_COLS 29
#define SG_TXT_ROWS 19
_Static_assert(SG_TXT_COLS * SG_TXT_ROWS <= SG_NSLOT, "sgswan: a tile for each place of the view");
static uint8_t sg_txt_fg = 0xFF;       // the text's color as drawn

static void sg_text_cell(uint8_t row, uint8_t col, const uint8_t *names) {
    uint16_t at = (uint16_t) row * 32 + col;
    if (col == 0 || col == 31) {
        map1[at] = SG_BLANK;       // (the sides)
        return;
    }
    uint8_t px = (uint8_t) ((col - 1) * 8);   // its first pixel of the line's 240
    uint8_t k = px / 6, phase = px % 6;
    uint16_t want = names[k] | ((uint16_t) names[k + 1] << 8);
    uint16_t slot = (uint16_t) (row % SG_TXT_ROWS) * SG_TXT_COLS + col % SG_TXT_COLS;
    if (sg_slot_ref[slot] == at + 1 && sg_slot_key[slot] == want)
        return;
    sg_slot_ref[slot] = at + 1;
    sg_slot_key[slot] = want;
    // the 6 pixels of the first character and of the second in a row of
    // 12, the tile's 8 from where it starts
    uint8_t pa[8], pb[8];
    uint16_t *t = (uint16_t*) TILE_PTR(sg_slot_tile(slot));
    uint16_t fl = sg_plane_lo[sg_txt_fg], fh = sg_plane_hi[sg_txt_fg];
    _fmemcpy(pa, sg_vr + ((sg_pg + ((uint16_t) (want & 0xFF) << 3)) & 0x3FFF), 8);
    _fmemcpy(pb, sg_vr + ((sg_pg + ((want >> 8) << 3)) & 0x3FFF), 8);
    uint8_t sh = 4 - phase;
    for (uint8_t r = 0; r < 8; r++) {
        uint16_t w = ((uint16_t) (pa[r] & 0xFC) << 4) | (pb[r] >> 2);
        uint16_t pp = (uint8_t) (w >> sh) * 0x0101u;
        *t++ = pp & fl;
        *t++ = pp & fh;
    }
    diag_commit++;
    map1[at] = sg_slot_entry(slot);
}

// all: the mode was entered, the view shows other tiles, or (2) nothing
// drawn before is good
static void sg_text_present(uint8_t all) {
    uint8_t names[41];
    uint32_t rows = 0;   // the lines with characters written
    uint8_t fg = gg.vdp.reg[7] >> 4;
    if (fg != sg_txt_fg) {
        sg_txt_fg = fg;
        all = 2;
    }
    if (sg_any) {
        // the patterns written: everything again (a font being loaded)
        const uint8_t *d = sg_page + (sg_pg >> 9);
        if (d[0] | d[1] | d[2] | d[3])
            all = 2;
        d = sg_dirty + (sg_nt >> 4);
        for (uint8_t u = 0; u < 60; u++)
            if (d[u]) {
                // (16 characters from 16 * u: one line or two)
                rows |= (uint32_t) 1 << ((uint16_t) u * 16 / 40);
                rows |= (uint32_t) 1 << (((uint16_t) u * 16 + 15) / 40);
            }
    }
    if (all == 2)
        for (uint16_t i = 0; i < SG_TXT_COLS * SG_TXT_ROWS; i++)
            sg_slot_ref[i] = 0;
    if (all)
        rows = 0xFFFFFF;
    if (!rows)
        return;
    names[40] = 0;
    for (uint8_t row = sg_r0; row <= sg_r1; row++) {
        if (!((rows >> row) & 1))
            continue;
        _fmemcpy(names, sg_vr + sg_nt + (uint16_t) row * 40, 40);
        for (uint8_t col = sg_c0; col <= sg_c1; col++)
            sg_text_cell(row, col, names);
    }
}

/* === Sprites === */

static uint8_t spr_back = 1;   // the table to build next: 1 sprite_table_b
static uint8_t spr_port04, spr_count, spr_ready;
// the kinds: the tiles of a pattern in a color (pattern address / 8, the
// color in bits 12-15)
#define SG_KINDS 40
static uint16_t sg_kind_key[SG_KINDS];
// (with the four sprites to a line: the lines of it not drawn, bit r its
// line r; 0: all of it, the kind every sprite has without the limit)
static uint32_t sg_kind_mask[SG_KINDS];
// START + Y3 (Y down): the chip's four sprites to a line, the fifth and
// the ones after it not drawn there (the games' flicker), or all of them
// (the WonderSwan draws 32 to a line). The status register says the same
// either way (sg_vdp.c).
static uint8_t sg_limit;
static uint8_t sg_kind_live[SG_KINDS];   // on the screen in this frame
// its tiles with nothing in them, bit n the nth (not given to the
// WonderSwan: it has 128 sprites, and a magnified 16x16 one takes 16)
static uint16_t sg_kind_empty[SG_KINDS];
static uint8_t sg_kind_max;              // how many fit (the tiles each takes)

static void sg_kinds_reset(void) {
    uint8_t tiles = (sg_big ? 4 : 1) * (sg_mag ? 4 : 1);
    // (no more than 40: each sprite looks for its kind among them)
    sg_kind_max = SG_SPR_TILES / tiles > SG_KINDS ? SG_KINDS : SG_SPR_TILES / tiles;
    for (uint8_t k = 0; k < sg_kind_max; k++)
        sg_kind_key[k] = SG_NONE;
}

// the tiles of a kind: 8x8 pattern pieces top left, bottom left, top
// right, bottom right; magnified, each piece four tiles the same way
static void sg_kind_convert(uint8_t kind, uint16_t key, uint32_t mask) {
    uint8_t src[32];
    uint8_t pieces = sg_big ? 4 : 1;
    uint16_t lo = sg_plane_lo[key >> 12], hi = sg_plane_hi[key >> 12];
    uint16_t *t = (uint16_t*) TILE_PTR((uint16_t) kind * pieces * (sg_mag ? 4 : 1));
    uint16_t *t0 = t;
    _fmemcpy(src, sg_vr + (((key & 0x7FF) << 3) & 0x3FFF), pieces * 8);
    diag_commit += pieces;
    for (uint8_t q = 0; q < pieces; q++) {
        const uint8_t *s = src + q * 8;
        if (!sg_mag) {
            for (uint8_t r = 0; r < 8; r++) {
                uint16_t pp = s[r] * 0x0101u;
                *t++ = pp & lo;
                *t++ = pp & hi;
            }
            continue;
        }
        // (left half then right half: tiles 0, 1 the top, 2, 3 the bottom)
        for (uint8_t half = 0; half < 2; half++) {
            for (uint8_t side = 0; side < 2; side++) {
                for (uint8_t r = 0; r < 8; r++) {
                    uint8_t b = s[half * 4 + (r >> 1)];
                    uint16_t pp = sg_wide[side ? b & 15 : b >> 4] * 0x0101u;
                    *t++ = pp & lo;
                    *t++ = pp & hi;
                }
            }
        }
    }
    // the lines not drawn: nothing there (the tiles in the order above;
    // each one's first line of the sprite); then the tiles left empty
    uint8_t sub = sg_mag ? 4 : 1;
    uint16_t empty = 0, bit = 1;
    for (uint8_t q = 0; q < pieces; q++) {
        for (uint8_t u = 0; u < sub; u++, t0 += 16, bit <<= 1) {
            if (mask) {
                uint8_t l0 = sg_mag ? (q & 1) * 16 + (u >> 1) * 8 : (q & 1) * 8;
                uint8_t m = (uint8_t) (mask >> l0);
                for (uint8_t r = 0; m; r++, m >>= 1)
                    if (m & 1) {
                        t0[r * 2] = 0;
                        t0[r * 2 + 1] = 0;
                    }
            }
            uint16_t any = 0;
            for (uint8_t r = 0; r < 16; r++)
                any |= t0[r];
            if (!any)
                empty |= bit;
        }
    }
    sg_kind_empty[kind] = empty;
}

static void build_sprites(void) {
    uint8_t sat[128];
#ifdef GGSWAN_HOST
    host_spr_builds++;
#endif
    // (bit 1: the kinds too; bit 0 may have been set after it)
    if (sg_spr_dirty & 2)
        sg_kinds_reset();
    _fmemcpy(sat, sg_vr + sg_sat, 128);
    uint8_t *table = spr_back ? sprite_table_b : sprite_table;
    uint8_t *s = table, *end = table + 128 * 4;
    uint8_t pieces = sg_big ? 4 : 1, sub = sg_mag ? 4 : 1, step = sg_mag ? 16 : 8;
    int16_t side = (sg_big ? 16 : 8) * (sg_mag ? 2 : 1);
    memset(sg_kind_live, 0, sg_kind_max);
    uint32_t hid[32];
    uint8_t lim = sg_limit && sg_m != 4 && sg_sprite_hidden(sat, hid);
    // (no sprites with the display off or in text mode)
    for (uint8_t i = 0; i < 32 && sg_m != 4 && s < end; i++) {
        const uint8_t *e = sat + i * 4;
        if (e[0] == 0xD0)
            break;
        uint8_t color = e[3] & 0x0F;
        if (!color)
            continue;
        // (the top left pixel in the picture: sg_sprite, gg.h) in the view
        int16_t y = (e[0] > 0xE0 ? (int16_t) e[0] - 256 : e[0]) + 1 - sg_vy;
        int16_t x = (int16_t) e[1] - ((e[3] & 0x80) ? 32 : 0) - sg_vx;
        if (y <= -side || y >= GG_VIEW_H || x <= -side || x >= GG_VIEW_W)
            continue;
        uint16_t key = ((sg_spg >> 3) + (sg_big ? e[2] & 0xFC : e[2])) | ((uint16_t) color << 12);
        // the lines the chip leaves out (the fifth on them, or later):
        // all the ones it has in the picture: no sprite
        uint32_t mask = lim ? hid[i] : 0;
        if (mask) {
            int16_t py = y + sg_vy;
            int16_t lo = py < 0 ? -py : 0;
            int16_t hi = py + side > GG_ACTIVE_LINES ? GG_ACTIVE_LINES - py : side;
            uint32_t all = (hi >= 32 ? 0xFFFFFFFFul : ((uint32_t) 1 << hi) - 1)
                & ~(((uint32_t) 1 << lo) - 1);
            if (mask == all)
                continue;
        }
        // its kind: there already, or an empty place, or the place of
        // one not on the screen (so far)
        uint8_t k, spare, dead;
    find:
        k = 0; spare = 0xFF; dead = 0xFF;
        for (; k < sg_kind_max; k++) {
            if (sg_kind_key[k] == key && sg_kind_mask[k] == mask)
                break;
            if (sg_kind_key[k] == SG_NONE) {
                if (spare == 0xFF)
                    spare = k;
            } else if (dead == 0xFF && !sg_kind_live[k]) {
                dead = k;
            }
        }
        if (k == sg_kind_max) {
            if (spare == 0xFF)
                spare = dead;
            if (spare == 0xFF) {
                // more kinds than tiles for them: one with lines left
                // out is drawn whole, if that kind is there
                if (mask) {
#ifdef GGSWAN_HOST
                    if (getenv("GGSWAN_LIMIT_LOG")) fprintf(stderr, "frame %lu: sprite %d drawn whole (no kind left)\n", (unsigned long) gg.frame, i);
#endif
                    mask = 0;
                    goto find;
                }
                continue;
            }
            k = spare;
            sg_kind_key[k] = key;
            sg_kind_mask[k] = mask;
            sg_kind_convert(k, key, mask);
        }
        sg_kind_live[k] = 1;
        uint16_t tile = (uint16_t) k * pieces * sub;
        uint16_t empty = sg_kind_empty[k];
        for (uint8_t q = 0; q < pieces; q++) {
            int16_t qx = x + (q >> 1) * step, qy = y + (q & 1) * step;
            for (uint8_t u = 0; u < sub; u++, tile++, empty >>= 1) {
                int16_t wx = qx + (u & 1) * 8, wy = qy + (u >> 1) * 8;
                if ((empty & 1) || wy <= -8 || wy >= GG_VIEW_H || wx <= -8 || wx >= GG_VIEW_W || s >= end)
                    continue;
                // (palette 8, behind screen 2: the diagnostics' text)
                s[0] = (uint8_t) tile; s[1] = tile >> 8; s[2] = (uint8_t) wy; s[3] = (uint8_t) wx;
                s += 4;
            }
        }
    }
    spr_port04 = (IRAM_OFF(table) >> 9) & 0x3F;
    spr_count = (uint8_t) ((s - table) >> 2);
    spr_ready = 1;
    spr_back ^= 1;
    sg_spr_dirty = 0;
}

// the sprite table built to the WS (it copies it at the end of the screen)
static void sprites_publish(void) {
    if (!spr_ready)
        return;
    spr_ready = 0;
    outportb(0x04, spr_port04);
    outportb(0x05, 0);
    outportb(0x06, spr_count);
}

/* === The frame === */

#ifndef GGSWAN_HOST
// The line interrupt (line_irq_slow, drc_asm.s): the end of the WS screen
void line_irq_slow(void);
void line_irq(void) {
    outportb(WS_SCR1_SCRL_X_PORT, sg_vx);
    outportb(0x11, sg_vy);
    frame_waiting = 0;
    ws_int_ack(WS_INT_ACK_LINE_MATCH);
}
#endif

GGSWAN_HOST_VISIBLE void video_init(void) {
    // the TMS9918A's VRAM: the last 16 KiB of the ROM's PSRAM bank, where
    // slot 2 of the ROM keeps the RAM window anyway (gg_platform_bank)
#ifdef GGSWAN_SF7000
    // sfswan: the RAM window is on the SF-7000's RAM. The VRAM is the
    // last 16 KiB of the tables' bank: read through the ROM's first
    // window, which stays on it; written by gg_platform_vram_store
    _Static_assert(SF_VRAM_BANK == SG_TABLE_BANK, "sfswan: the VRAM in the tables' bank");
    ram_window_set(WS_CART_BANK_FLASH_ENABLE, SF_RAM_BANK);
    outportw(WS_CART_EXTBANK_ROM0_PORT, SG_TABLE_BANK);
    sg_tab_open();
    _fmemset(MK_FP(0x1000, 0xC000), 0, 0x4000);
    sg_tab_close();
    gg_vram = MK_FP(0x2000, 0xC000);
    sg_vr = gg_vram;
#else
    ram_window_set(WS_CART_BANK_FLASH_ENABLE, GG_PSRAM_FIRST_BANK);
    gg_vram = MK_FP(0x1000, 0xC000);
    _fmemset(gg_vram, 0, 0x4000);
    outportw(WS_CART_EXTBANK_ROM0_PORT, GG_PSRAM_FIRST_BANK);
    sg_vr = MK_FP(0x2000, 0xC000);
#endif
    memset(TILE_PTR(0), 0, 0x4000);
    memset(TILE_PTR(SG_BG_HIGH), 0, (1024 - SG_BG_HIGH) * 32);
    // the slots' tables (sg_tab_open)
    sg_tab_open();
    sg_slot_of = MK_FP(0x1000, 0x0000);
    sg_slot_key = MK_FP(0x1000, 0x0800);
    sg_slot_ref = MK_FP(0x1000, 0x1000);
    sg_ring = MK_FP(0x1000, 0x1800);
    sg_slot_fl = MK_FP(0x1000, 0x2000);
    sg_done = MK_FP(0x1000, 0x2800);
    memset(sg_dirty, 0, 1024);
    memset(sg_page, 0, 32);
    sg_any = 0;
    sg_m = 0xFF;
    sg_vx = sg_vy = 0xFF;
    sg_pool_reset();
    sg_tab_close();
    for (uint16_t i = 0; i < 32 * 32; i++)
        map2[i] = WS_PRIO_BLANK;
    // one palette, the chip's 16 colors: background (0) and sprites (8).
    // Color 0 is the WS's transparent one: the backdrop behind it
    for (uint8_t i = 0; i < 16; i++) {
        uint16_t c = i ? gg_color12(i) : 0;
        WS_DISPLAY_COLOR_MEM(0)[i] = c;
        WS_DISPLAY_COLOR_MEM(8)[i] = c;
    }
    memset(sprite_table, 0, 512);
    memset(sprite_table_b, 0, 512);
    spr_back = 1;
    sg_spr_dirty = 2;
    drc_st.prio_on = !sg_diag;   // (screen 2: the diagnostics' text, or nothing)

    uint16_t m1 = IRAM_OFF(map1), m2 = IRAM_OFF(map2);
    outportb(WS_SCR_BASE_PORT, ((m1 >> 11) & 0x0F) | (((m2 >> 11) & 0x0F) << 4));
    outportb(0x04, (IRAM_OFF(sprite_table) >> 9) & 0x3F);
    outportb(0x08, 0);
    outportb(0x09, 0);
    outportb(0x0A, GG_VIEW_W - 1);
    outportb(0x0B, GG_VIEW_H - 1);
    outportb(0x10, gg_view_x);
    outportb(0x11, gg_view_y);
    outportb(0x12, 0);
    outportb(0x13, 0);
    outportb(0x06, 0);
#ifndef GGSWAN_HOST
    ia16_disable_irq();
    ws_int_set_handler(WS_INT_LINE_MATCH, (ia16_int_handler_t) line_irq_slow);
    outportb(WS_DISPLAY_LINE_IRQ_PORT, GG_VIEW_H);
    ws_int_enable(WS_INT_ENABLE_LINE_MATCH);
    ia16_enable_irq();
#endif
}

// the frame is shown as it is at the end of the picture (line 192)
GGSWAN_HOST_VISIBLE void video_present(void);
void gg_platform_frame_end(void) {
    video_present();
}

static void pcm_frame(void);   // (the frame's samples: below)

GGSWAN_HOST_VISIBLE void video_present(void) {
#ifndef GGSWAN_HOST
    // the last frame not shown yet: wait for the end of the WS screen (as
    // the Game Gear's)
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
    gg_platform_psg_flush();
    pcm_frame();
    vp_lap(VP_PSG);

#ifdef GGSWAN_HOST
    // (the PC tests: GGSWAN_PAN=x,y for the view; GGSWAN_PANMOVE, below,
    // to move it all the time, as the Y buttons do)
    if (getenv("GGSWAN_LIMIT") && !sg_limit) {
        sg_limit = 1;
        sg_spr_dirty = 2;
    }
    if (getenv("GGSWAN_PAN")) {
        int px = 16, py = 24;
        sscanf(getenv("GGSWAN_PAN"), "%d,%d", &px, &py);
        gg_view_x = px; gg_view_y = py;
    }
#endif
    // the tables, the view
    uint8_t on = ((reg[1] | gg.vdp.reg1_frame) & 0x40) != 0;
    uint8_t redo = sg_tables();
    sg_tab_open();
    if (redo == 2)
        sg_pool_reset();
    if (gg_view_x != sg_vx || gg_view_y != sg_vy) {
        uint8_t c0 = gg_view_x >> 3, c1 = (uint8_t) ((gg_view_x + GG_VIEW_W - 1) >> 3);
        uint8_t r0 = gg_view_y >> 3, r1 = (uint8_t) ((gg_view_y + GG_VIEW_H - 1) >> 3);
        if (c1 > 31) c1 = 31;
        if (r1 > 23) r1 = 23;
        if (c0 != sg_c0 || c1 != sg_c1 || r0 != sg_r0 || r1 != sg_r1 || sg_vx == 0xFF)
            redo |= 1;
        sg_c0 = c0; sg_c1 = c1; sg_r0 = r0; sg_r1 = r1;
        sg_vx = gg_view_x;
        sg_vy = gg_view_y;
        if (!sg_spr_dirty)
            sg_spr_dirty = 1;
#ifdef GGSWAN_HOST
        outportb(0x10, sg_vx);   // (the PC tests have no WS raster)
        outportb(0x11, sg_vy);
#endif
    }
    if (sg_m == 4) {
        sg_text_present(redo);
    } else {
    if (sg_starved && sg_ring_n)
        redo |= 1;
    if (sg_any && redo != 2) {
        // the tiles written, then the names (a name's new slot is drawn
        // with the patterns as they are now)
        sg_done_n = 0;
        for (uint8_t pg = 0; pg < 32; pg++) {
            if (!sg_page[pg])
                continue;
            const uint8_t *d = sg_dirty + ((uint16_t) pg << 5);
            for (uint8_t u = 0; u < 32; u++)
                if (d[u]) {
                    uint16_t a = ((uint16_t) pg << 9) + ((uint16_t) u << 4);
                    sg_unit_tiles(a);
                    sg_unit_tiles(a + 8);
                }
        }
        if (sg_done_n > SG_DONE_MAX) {
            for (uint16_t i = 0; i < SG_NSLOT; i++)
                sg_slot_fl[i] &= ~2;
        } else {
            for (uint16_t i = 0; i < sg_done_n; i++)
                sg_slot_fl[sg_done[i]] &= ~2;
        }
    }
    if (redo) {
        sg_relayout();
    } else if (sg_any) {
        if (sg_m != 4) {
            uint8_t names[16];
            const uint8_t *d = sg_dirty + (sg_nt >> 4);
            for (uint8_t i = 0; i < 48; i++) {
                if (!d[i])
                    continue;
                uint16_t cell = (uint16_t) i << 4;
                uint8_t row = cell >> 5, col = cell & 31;
                if (row < sg_r0 || row > sg_r1 || col + 15 < sg_c0 || col > sg_c1)
                    continue;
                _fmemcpy(names, sg_vr + sg_nt + cell, 16);
                for (uint8_t k = 0; k < 16; k++)
                    if (col + k >= sg_c0 && col + k <= sg_c1)
                        sg_cell(row, col + k, names[k]);
            }
        }
    }
    }
    vp_lap(VP_MAP);

    if (sg_any) {
        // the sprite table, the sprites' patterns
        const uint8_t *d = sg_dirty + (sg_sat >> 4);
        for (uint8_t i = 0; i < 8; i++)
            if (d[i])
                sg_spr_dirty |= 1;
        d = sg_page + (sg_spg >> 9);
        if ((d[0] | d[1] | d[2] | d[3]) && sg_spr_dirty != 2) {
            // (the kinds made from those bytes: again when they are shown)
            uint8_t units = sg_big ? 4 : 1;
            for (uint8_t k = 0; k < sg_kind_max; k++) {
                uint16_t u = sg_kind_key[k] & 0x7FF;
                if (sg_kind_key[k] == SG_NONE)
                    continue;
                for (uint8_t n = 0; n < units; n++, u++) {
                    if (sg_dirty[(u & 0x7FF) >> 1]) {
                        sg_kind_key[k] = SG_NONE;
                        sg_spr_dirty |= 1;
                        break;
                    }
                }
            }
        }
        for (uint8_t pg = 0; pg < 32; pg++) {
            if (!sg_page[pg])
                continue;
            memset(sg_dirty + ((uint16_t) pg << 5), 0, 32);
            sg_page[pg] = 0;
        }
        sg_any = 0;
    }
    if (redo == 2 && !sg_spr_dirty)
        sg_spr_dirty = 1;   // (text mode, or out of it)
    if (sg_spr_dirty)
        build_sprites();
    diag_splits = SG_NSLOT - sg_ring_n;
    sg_tab_close();
    vp_lap(VP_SPR);

    // this frame's sprites to the WS, never on the last lines of its
    // screen (it copies the table there: as the Game Gear's)
#ifndef GGSWAN_HOST
    for (;;) {
        uint16_t f = irq_save();
        uint8_t l = inportb(WS_DISPLAY_LINE_PORT);
        if (l < GG_VIEW_H - 3 || l > GG_VIEW_H) {
            sprites_publish();
            frame_waiting = 1;
            irq_restore(f);
            break;
        }
        irq_restore(f);
    }
#else
    sprites_publish();
    frame_waiting = 1;
#endif
    vp_lap(VP_LOG);

    // the backdrop: register 7's color, behind color 0 of the tiles
    outportb(0x01, reg[7] & 0x0F);
    // (text mode: no sprites)
    outportb(0x00, !on ? 0x02 | 0x20 : sg_m != 4 ? 0x01 | 0x02 | 0x04 | 0x20 : 0x01 | 0x02 | 0x20);
#ifdef GGSWAN_HOST
    // (for the next frame: the reference draws it line by line)
    if (getenv("GGSWAN_PANMOVE")) {
        unsigned f = gg.frame / 2, x = f % 66, y = (f * 2 / 3) % 98;
        gg_view_x = x > 32 ? 65 - x : x;
        gg_view_y = y > 48 ? 97 - y : y;
        if (gg_view_x > 32) gg_view_x = 32;
        if (gg_view_y > 48) gg_view_y = 48;
    }
#endif
    vp_mark = vp_start;
    vp_lap(VP_ALL);
    prof_phase = PROF_OTHER;
}
