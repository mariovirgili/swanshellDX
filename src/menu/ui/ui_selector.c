/**
 * Copyright (c) 2024 Adrian Siekierka
 *
 * swanshell is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * swanshell is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with swanshell. If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdio.h>
#include <ws.h>
#include <ws/display.h>
#include <ws/timer.h>
#include "ui_selector.h"
#include "../main.h"
#include "cart/status.h"
#include "lang.h"
#include "lang_gen.h"
#include "settings.h"
#include "ui/bitmap.h"
#include "ui/ui.h"
#include "util/input.h"
#include "util/util.h"
#include "menu_music.h"

#define SELECTOR_Y_OFFSET 8

static inline void ui_selector_set_active_font(ui_selector_config_t *config) {
    bitmapfont_set_active_font(config->style == UI_SELECTOR_STYLE_16 ? font16_bitmap : font8_bitmap);
}

#define UI_SELECTOR_ROW_CONFIG() \
    uint16_t row_count, row_height, row_offset; \
    if (config->style == UI_SELECTOR_STYLE_16) { \
        row_height = 16; \
        row_offset = 2; \
    } else { \
        row_height = 8; \
        row_offset = 0; \
    } \
    row_count = (screen_height - 16) / row_height;

void ui_selector_clear_selection(ui_selector_config_t *config) {
    ui_screen_modify_tiles(bitmap_screen2, ~WS_SCREEN_ATTR_PALETTE_MASK, 0, 0, 1, screen_width>>3, (screen_height-16)>>3);

    if (ui_has_wallpaper()) {
        ui_selector_set_active_font(config);
        UI_SELECTOR_ROW_CONFIG();

        if (config->offset < config->count) {
            uint16_t prev_sel = (config->offset % row_count);
            bitmap_rect_fill(&ui_bitmap, 0, prev_sel * row_height + SELECTOR_Y_OFFSET, screen_width, row_height, BITMAP_COLOR_4BPP(0));
            config->draw(config, config->offset, prev_sel * row_height + SELECTOR_Y_OFFSET + row_offset, 0);
        }
    }
}

/* === Flicker-free redraws ("racing the beam") ===
 * The menu draws straight into the displayed bitmap, so clearing a row and
 * then drawing it can show a blank or half-drawn row. Instead, each band is
 * redrawn right after the LCD has scanned past it: there is then almost a
 * whole frame before the beam reaches it again, and the band switches from
 * the old to the new content between two frames. In the horizontal layout
 * bitmap rows are LCD lines; in the vertical one this is skipped. */

// Lines after the band's bottom in which a redraw may start: leaves
// (lines per frame - band height - 48) lines (~8 ms) to finish drawing.
#define BEAM_WINDOW_LINES 48

static void ui_selector_wait_beam_below(uint16_t y_bottom) {
    if (!bitmap_rotation)
        return;
    int16_t lines = inportb(WS_LCD_VTOTAL_PORT) + 1;
    // bounded: never hang if the line counter does not behave as expected
    for (uint16_t tries = 0; tries < 50000; tries++) {
        int16_t d = (int16_t) inportb(WS_DISPLAY_LINE_PORT) - (int16_t) y_bottom;
        if (d < 0)
            d += lines;
        if (d < BEAM_WINDOW_LINES)
            return;
    }
}

// Redraw list row `row` (on screen) showing entry `idx`: tile palette
// (selected or not), background and entry, all right after the beam.
static void ui_selector_redraw_row(ui_selector_config_t *config, uint16_t row, uint16_t row_height, uint16_t row_offset, uint16_t idx) {
    uint16_t y = row * row_height + SELECTOR_Y_OFFSET;
    bool selected = idx == config->offset;

    ui_selector_set_active_font(config);
    ui_selector_wait_beam_below(y + row_height);
    ui_screen_modify_tiles(bitmap_screen2, ~WS_SCREEN_ATTR_PALETTE_MASK,
        selected ? WS_SCREEN_ATTR_PALETTE(1) : 0, 0, y >> 3, screen_width >> 3, row_height >> 3);
    // without a wallpaper the palette does the highlighting (colour 2 is the
    // row background in both palettes); with one, unselected rows are
    // transparent and the selected one gets the selection colour
    bitmap_rect_fill(&ui_bitmap, 0, y, screen_width, row_height,
        (ui_has_wallpaper() && !selected) ? BITMAP_COLOR_4BPP(0) : BITMAP_COLOR_4BPP(2));
    if (idx < config->count)
        config->draw(config, idx, y + row_offset, 0);
}

// Only the palette of a row (no wallpaper: no need to redraw its contents).
static void ui_selector_set_row_palette(uint16_t row, uint16_t row_height, bool selected) {
    ui_screen_modify_tiles(bitmap_screen2, ~WS_SCREEN_ATTR_PALETTE_MASK,
        selected ? WS_SCREEN_ATTR_PALETTE(1) : 0, 0, (row * row_height + SELECTOR_Y_OFFSET) >> 3,
        screen_width >> 3, row_height >> 3);
}

static void ui_selector_draw_statusbar(const char __far *text, const char __far *right_text) {
    // the status bar is the last 8 lines: redraw once the beam is past them
    ui_selector_wait_beam_below(screen_height);
    ui_draw_statusbar_lr(text, right_text);
}

#define UI_SELECTOR_OFFSET_TO_BOUNDS() \
    if (config->count > 0 && config->offset >= config->count) \
        config->offset = config->count - 1

// Delay before a long name starts scrolling, in frames (~75 fps): ~0.5 s
#define SCROLL_TICKS_MIN 40
// Scroll by one character every (1 << SHIFT) frames: 8 frames (~9 chars/s)
#define SCROLL_TICKS_PACE_SHIFT 3
#define SCROLL_TICKS_PACE_MASK ((1 << (SCROLL_TICKS_PACE_SHIFT)) - 1)

// Frames the selection must stay on an entry before status_text() is asked
// (avoids storage card reads while scrolling quickly through the list).
#define STATUS_TEXT_DELAY 10

uint16_t ui_selector(ui_selector_config_t *config) {
    char sbuf[33];
    char status_buf[48];
    uint16_t status_ticks = 0;
    bool status_custom = false;
    uint16_t prev_offset = 0xFFFF;
    uint16_t scroll_ticks = 0;
    bool prev_row_dirty = false;

    UI_SELECTOR_ROW_CONFIG();

    // Caller says the list on screen is still the one drawn for
    // redraw_from - 1 (e.g. after a letter jump): only update what changed.
    if (config->redraw_from) {
        if (config->redraw_from - 1 < config->count) {
            prev_offset = config->redraw_from - 1;
            prev_row_dirty = true;  // it may have been mid-scroll
            status_custom = true;   // the status bar may show entry details
        }
        config->redraw_from = 0;
    }

    if (config->count == 0) {
        ui_layout_bars();
        ui_selector_set_active_font(config);
        ui_draw_statusbar_lr(lang_keys[LK_UI_FILE_SELECTOR_EMPTY], config->info_str);
        // (Re)start background music only once the screen is drawn, so it
        // does not compete with the directory scan and font loading.
        menu_music_start();

        while (true) {
            if (idle_until_vblank() || cart_status_apply_orientation_change())
                return UI_SELECTOR_RELOAD_REQUESTED;
            input_update();
            uint16_t keys_pressed = input_pressed;

            // User actions
            if (keys_pressed & config->key_mask)
                return keys_pressed & config->key_mask;
        }
    }

    while (true) {
        if (prev_offset != config->offset) {
            status_ticks = 0;
            if ((settings.file_flags & SETTING_THEME_SCROLL_LONG_NAMES) && scroll_ticks > SCROLL_TICKS_MIN)
                prev_row_dirty = true; // its name was scrolled: redraw from the start
            scroll_ticks = 0;

            uint16_t page_start = (config->offset / row_count) * row_count;
            uint16_t sel = config->offset % row_count;
            bool draw_filenames = prev_offset == 0xFFFF || ((prev_offset / row_count) != (config->offset / row_count));

            if (draw_filenames) {
                // new page: every row, top to bottom, each behind the beam
                for (uint16_t i = 0; i < row_count; i++)
                    ui_selector_redraw_row(config, i, row_height, row_offset, page_start + i);

                snprintf(sbuf, sizeof(sbuf), lang_keys[LK_UI_FILE_SELECTOR_PAGE_FORMAT], (config->offset / row_count) + 1, ((config->count + row_count - 1) / row_count));
                ui_selector_draw_statusbar(sbuf, config->info_str);
                status_custom = false;
            } else {
                // same page: only the previous and the new selected row
                uint16_t prev_sel = prev_offset % row_count;
                if (prev_sel != sel) {
                    if (ui_has_wallpaper() || prev_row_dirty)
                        ui_selector_redraw_row(config, prev_sel, row_height, row_offset, prev_offset);
                    else
                        ui_selector_set_row_palette(prev_sel, row_height, false);

                    if (ui_has_wallpaper())
                        ui_selector_redraw_row(config, sel, row_height, row_offset, config->offset);
                    else
                        ui_selector_set_row_palette(sel, row_height, true);
                }
            }

            prev_row_dirty = false;
            prev_offset = config->offset;

            // (Re)start background music only once the list is drawn, so it
            // does not compete with the directory scan and font loading.
            menu_music_start();
        }

        if (idle_until_vblank() || cart_status_apply_orientation_change())
            return UI_SELECTOR_RELOAD_REQUESTED;

        // Selection settled: ask for entry details for the status bar
        if (config->status_text != NULL && status_ticks < STATUS_TEXT_DELAY && ++status_ticks == STATUS_TEXT_DELAY) {
            if (config->offset < config->count
                && config->status_text(config, config->offset, status_buf, sizeof(status_buf))) {
                ui_selector_draw_statusbar(status_buf, config->info_str);
                status_custom = true;
            } else if (status_custom) {
                snprintf(sbuf, sizeof(sbuf), lang_keys[LK_UI_FILE_SELECTOR_PAGE_FORMAT], (config->offset / row_count) + 1, ((config->count + row_count - 1) / row_count));
                ui_selector_draw_statusbar(sbuf, config->info_str);
                status_custom = false;
            }
        }

        if (settings.file_flags & SETTING_THEME_SCROLL_LONG_NAMES) {
            scroll_ticks++;
            if (scroll_ticks > SCROLL_TICKS_MIN && !(scroll_ticks & SCROLL_TICKS_PACE_MASK)) {
                ui_selector_set_active_font(config);
                int y_offset = (config->offset % row_count) * row_height + SELECTOR_Y_OFFSET;
                config->draw(config, config->offset, y_offset + row_offset, (scroll_ticks - SCROLL_TICKS_MIN) >> SCROLL_TICKS_PACE_SHIFT);
            }
        }

    	input_update();
        uint16_t keys_pressed = input_pressed;

        // Page/entry movement
        if (keys_pressed & WS_KEY_X1) {
            do {
                if (config->offset == 0 || ((config->offset - 1) % row_count) == (row_count - 1))
                    config->offset = config->offset + (row_count - 1);
                else
                    config->offset = config->offset - 1;
                UI_SELECTOR_OFFSET_TO_BOUNDS();
            } while (config->can_select != NULL && !config->can_select(config, config->offset));
        }
        if (keys_pressed & WS_KEY_X3) {
            do {
                if ((config->offset + 1) == config->count)
                    config->offset = config->offset - (config->offset % row_count);
                else if (((config->offset + 1) % row_count) == 0)
                    config->offset = config->offset - (row_count - 1);
                else
                    config->offset = config->offset + 1;
                UI_SELECTOR_OFFSET_TO_BOUNDS();
            } while (config->can_select != NULL && !config->can_select(config, config->offset));
        }
        if (keys_pressed & WS_KEY_X2) {
            do {
                if ((config->offset - (config->offset % row_count) + row_count) < config->count)
                    config->offset += row_count;
                UI_SELECTOR_OFFSET_TO_BOUNDS();
            } while (config->can_select != NULL && !config->can_select(config, config->offset));
        }
        if (keys_pressed & WS_KEY_X4) {
            do {
                if (config->offset >= row_count)
                    config->offset -= row_count;
                UI_SELECTOR_OFFSET_TO_BOUNDS();
            } while (config->can_select != NULL && !config->can_select(config, config->offset));
        }
        UI_SELECTOR_OFFSET_TO_BOUNDS();

        // User actions
        if (keys_pressed & config->key_mask)
            return keys_pressed & config->key_mask;
    }
}
