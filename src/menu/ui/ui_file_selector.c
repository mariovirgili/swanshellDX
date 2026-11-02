/*
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
#include <string.h>
#include <wonderful.h>
#include <ws.h>
#include <nilefs.h>
#include <wsx/utf8.h>
#include "bitmap.h"
#include "cart/mcu.h"
#include "cart/status.h"
#include "config.h"
#include "errors.h"
#include "lang_gen.h"
#include "launch/launch.h"
#include "launch/launch_athena.h"
#include "launch/launch_router.h"
#include "plugin/plugin.h"
#include "settings.h"
#include "strings.h"
#include "ui.h"
#include "ui/ui_file_selector.h"
#include "ui_favorites.h"
#include "ui_save_backup.h"
#include "ui_game_images.h"
#include "ui_icons.h"
#include "menu_music.h"
#include "ui_dialog.h"
#include "ui_selector.h"
#include "util/input.h"
#include "plugin/plugin.h"
#include "../../../build/menu/assets/menu/icons.h"
#include "lang.h"
#include "main.h"

#define COMPARE(a, b) (((a) == (b)) ? 0 : (((a) < (b)) ? -1 : 1))

static int compare_filenames(const file_selector_entry_t __far* a, const file_selector_entry_t __far* b, void *userdata) {
    uint8_t mode = (uint8_t) userdata;
    bool a_dir = a->fno.fattrib & AM_DIR;
    bool b_dir = b->fno.fattrib & AM_DIR;
    if (a_dir && !b_dir) {
        return -1;
    } else if (!a_dir && b_dir) {
        return 1;
    } else switch(mode) {
        case SETTING_FILE_SORT_NAME_ASC:
            return strcasecmp(a->fno.fname, b->fno.fname);
        case SETTING_FILE_SORT_NAME_DESC:
            return strcasecmp(b->fno.fname, a->fno.fname);
        case SETTING_FILE_SORT_DATE_ASC:
            return COMPARE(a->fno.fdate, b->fno.fdate) || COMPARE(a->fno.ftime, b->fno.ftime);
        case SETTING_FILE_SORT_DATE_DESC:
            return COMPARE(b->fno.fdate, a->fno.fdate) || COMPARE(b->fno.ftime, a->fno.ftime);
        case SETTING_FILE_SORT_SIZE_ASC:
            return COMPARE(a->fno.fsize, b->fno.fsize);
        case SETTING_FILE_SORT_SIZE_DESC:
            return COMPARE(b->fno.fsize, a->fno.fsize);
        default:
            return 0;
    }
}

// Settings > System > Hide NILESWAN folder: leave /NILESWAN out of the
// root folder's listing (set by ui_file_selector_scan_directory()).
static bool scan_hide_nileswan;

bool ui_file_selector_default_predicate(const FILINFO __far *fno, const char __far* ext) {
    // the ROM index file is never listed
    if (!strcasecmp(fno->fname, s_file_index_name))
        return false;

    if (scan_hide_nileswan && (fno->fattrib & AM_DIR) && !strcasecmp(fno->fname, s_nileswan_dir_name))
        return false;

    if (!(settings.file_flags & SETTING_FILE_SHOW_HIDDEN)) {
        if (fno->fattrib & AM_HID)
            return false;
    }

    if (!(settings.file_flags & SETTING_FILE_SHOW_SAVES)) {
        if (ext != NULL) {
            if (!strcasecmp(ext, s_file_ext_sram)) return false;
            if (!strcasecmp(ext, s_file_ext_eeprom)) return false;
            if (!strcasecmp(ext, s_file_ext_flash)) return false;
            if (!strcasecmp(ext, s_file_ext_rtc)) return false;
        }
    }

    return true;
}

static void ui_file_selector_sort_entries(uint16_t file_count) {
    outportw(WS_CART_EXTBANK_RAM_PORT, FILE_SELECTOR_INDEX_BANK);
    FILE_SELECTOR_MEMORY_BARRIER();
    for (int i = 0; i < file_count; i++)
        FILE_SELECTOR_INDEXES[i] = i;
    ui_file_selector_qsort(file_count, compare_filenames, (void*) settings.file_sort);
}

int16_t ui_file_selector_scan_directory(const char *path, filinfo_predicate_t predicate, uint16_t *count) {
    DIR dir;
    uint16_t file_count = 0;
    *count = 0;

    // listing the root folder? ("/", or "." while in it)
    scan_hide_nileswan = false;
    if (settings.file_flags & SETTING_FILE_HIDE_NILESWAN) {
        char cwd[4];
        if (path[0] == '/' && !path[1])
            scan_hide_nileswan = true;
        else if (path[0] == '.' && !path[1] && f_getcwd(cwd, sizeof(cwd)) == FR_OK && cwd[0] == '/' && !cwd[1])
            scan_hide_nileswan = true;
    }

    uint8_t result = f_opendir(&dir, path);
	if (result != FR_OK)
		return result;
	while (true) {
        file_selector_entry_t __far* fno = ui_file_selector_open_fno_direct(file_count);
		result = f_readdir(&dir, &fno->fno);

        // Invalid/empty result?
		if (result != FR_OK) {
            f_closedir(&dir);
            return result;
        }
		if (fno->fno.fname[0] == 0)
			break;

  		const char __far* ext_loc = (fno->fno.fattrib & AM_DIR) ? NULL : strrchr(fno->fno.fname, '.');
  		if (!predicate(&fno->fno, ext_loc))
            continue;

        // Cache extension location
        fno->extension_loc = ext_loc == NULL ? 255 : ext_loc - fno->fno.fname;

        file_count++;
        if (file_count >= FILE_SELECTOR_MAX_FILES)
            break;
	}
	f_closedir(&dir);

    ui_file_selector_sort_entries(file_count);

    *count = file_count;
    return FR_OK;
}

int ui_file_selector_get_file_icon_idx(const char __far *ext) {
    if (ext != NULL) {
        if (!strcasecmp(ext, s_file_ext_ws) || !strcasecmp(ext, s_file_ext_wsc) || !strcasecmp(ext, s_file_ext_pc2)) {
            return 2;
        } else if (!strcasecmp(ext, s_file_ext_fx)) {
            return 9;
        } else if (!strcasecmp(ext, s_file_ext_zip)) {
            return 8;
        } else if (!strcasecmp(ext, s_file_ext_fr) || !strcasecmp(ext, s_file_ext_il)) {
            return 7;
        } else if (!strcasecmp(ext, s_file_ext_wav) || !strcasecmp(ext, s_file_ext_vgm) || !strcasecmp(ext, s_file_ext_vgz)) {
            return 4;
        } else if (!strcasecmp(ext, s_file_ext_bmp)) {
            return 3;
        } else if (!strcasecmp(ext, s_file_ext_bfb)) {
            return 5;
        } else if (!strcasecmp(ext, s_file_ext_com)) {
            return 6;
        }
    }
    return 1;
}

// Offsets of the tile data in an .ICN file (see ui_icons.h)
#define ICN_16COLOR 32
#define ICN_8COLOR  (ICN_16COLOR + 2 * 1280)
#define ICN_16MONO  (ICN_8COLOR + 2 * 320)
#define ICN_8MONO   (ICN_16MONO + 2 * 640)

void ui_file_selector_draw_icon(uint16_t x, uint16_t y, uint16_t icon_idx, uint16_t style) {
    uint16_t tile_idx = bitmap_rotation ? ((y >> 3) + ((x >> 3) * 18)) : ((((WS_DISPLAY_WIDTH_TILES - (style == UI_SELECTOR_STYLE_16 ? 2 : 1)) - (y >> 3)) * 18) + (x >> 3));
    if (ui_icons_custom()) {
        // set from the memory card (PSRAM): same data layout as the built-in one
        bool color = ws_system_is_color_active();
        uint16_t tile_size = color ? 32 : 16;
        void __far *dest = color ? (void __far *) WS_TILE_4BPP_MEM(tile_idx) : (void __far *) WS_TILE_MEM(tile_idx);
        if (style == UI_SELECTOR_STYLE_16) {
            uint16_t offset = (color ? ICN_16COLOR : ICN_16MONO) + (bitmap_rotation ? 0 : 10 * 4 * tile_size) + icon_idx * 4 * tile_size;
            void __far *dest2 = color ? (void __far *) WS_TILE_4BPP_MEM(tile_idx + WS_DISPLAY_HEIGHT_TILES)
                : (void __far *) WS_TILE_MEM(tile_idx + WS_DISPLAY_HEIGHT_TILES);
            ui_icons_read(dest, offset, 2 * tile_size);
            ui_icons_read(dest2, offset + 2 * tile_size, 2 * tile_size);
        } else {
            uint16_t offset = (color ? ICN_8COLOR : ICN_8MONO) + (bitmap_rotation ? 0 : 10 * tile_size) + icon_idx * tile_size;
            ui_icons_read(dest, offset, tile_size);
        }
        return;
    }
    if (style == UI_SELECTOR_STYLE_16) {
        if (ws_system_is_color_active()) {
            const uint8_t __far* src = bitmap_rotation ? gfx_icons_16color : gfx_icons_16color_rot;
            ws_gdma_copy(WS_TILE_4BPP_MEM(tile_idx), src + (icon_idx * 128), 64);
            ws_gdma_copy(WS_TILE_4BPP_MEM(tile_idx + WS_DISPLAY_HEIGHT_TILES), src + (icon_idx * 128) + 64, 64);
        } else {
            const uint8_t __far* src = bitmap_rotation ? gfx_icons_16mono : gfx_icons_16mono_rot;
            memcpy(WS_TILE_MEM(tile_idx), src + (icon_idx * 64), 32);
            memcpy(WS_TILE_MEM(tile_idx + WS_DISPLAY_HEIGHT_TILES), src + (icon_idx * 64) + 32, 32);
        }
    } else {
        if (ws_system_is_color_active()) {
            const uint8_t __far* src = bitmap_rotation ? gfx_icons_8color : gfx_icons_8color_rot;
            ws_gdma_copy(WS_TILE_4BPP_MEM(tile_idx), src + (icon_idx * 32), 32);
        } else {
            const uint8_t __far* src = bitmap_rotation ? gfx_icons_8mono : gfx_icons_8mono_rot;
            memcpy(WS_TILE_MEM(tile_idx), src + (icon_idx * 16), 16);
        }
    }
}

/* === Favorites / Recent ===
 * listing_list != FAVORITES_LIST_NONE: the listing is one of the two lists;
 * each entry's fname is then a whole path and the current directory is
 * changed to the entry's own directory before it is opened. The directory
 * the list was opened from is restored when leaving it with B. */

static uint8_t listing_list = FAVORITES_LIST_NONE;
static uint16_t listing_list_offset = 0;
// (the folder to return to is kept in FILE_SELECTOR_SCRATCH_SLOT)
// "File list" mode: slot of the "Favorites" entry added to the root folder
// ("Recent" is the next one), or 0xFFFF
static uint16_t listing_special_slot = 0xFFFF;

// Name of an entry without its directory (lists store whole paths).
static const char __far *ui_file_selector_entry_name(file_selector_entry_t __far *fno) {
    const char __far *name = fno->fno.fname;
    if (listing_list != FAVORITES_LIST_NONE) {
        for (const char __far *p = name; *p; p++)
            if (*p == '/')
                name = p + 1;
    }
    return name;
}

static char display_name[FF_LFN_BUF + 1];

// Name as shown in the list: without the extension when the "Show
// extensions" setting asks for it. Directories always keep their name.
static const char __far *ui_file_selector_display_name(file_selector_entry_t __far *fno) {
    uint8_t mode = settings.file_show_ext;
    const char __far *name = ui_file_selector_entry_name(fno);
    uint8_t base = name - fno->fno.fname;
    if (mode == SETTING_FILE_SHOW_EXT_ALL || mode > SETTING_FILE_SHOW_EXT_NONE
        || (fno->fno.fattrib & AM_DIR) || !FILE_SELECTOR_ENTRY_HAS_EXTENSION(fno)
        || fno->extension_loc <= base)
        return name;
    if (mode == SETTING_FILE_SHOW_EXT_EXCEPT_ROMS) {
        const char __far *ext = FILE_SELECTOR_ENTRY_GET_EXTENSION(fno);
        if (strcasecmp(ext, s_file_ext_ws) && strcasecmp(ext, s_file_ext_wsc) && strcasecmp(ext, s_file_ext_pc2))
            return name;
    }
    uint8_t len = fno->extension_loc - base;
    for (uint8_t i = 0; i < len; i++)
        display_name[i] = name[i];
    display_name[len] = 0;
    return display_name;
}

/* Long name scrolling, pixel by pixel. scroll_tick from ui_selector() is
 * the number of scroll steps since scrolling started: one pixel per step
 * (a step every 2 frames). */
// extra pixels scrolled past the end of the name
#define SCROLL_END_MARGIN 4
// steps to hold at the end (~0.8 s), then at the start (~0.4 s)
#define SCROLL_HOLD_END   30
#define SCROLL_HOLD_START 15

static uint16_t scroll_last_px = 0xFFFF;
static uint16_t scroll_last_offset;

// Wait until the LCD has just drawn line y_bottom - 1 (within 48 lines).
// Bounded, so it can never hang.
static void scroll_wait_beam_below(uint16_t y_bottom) {
    int16_t lines = inportb(WS_LCD_VTOTAL_PORT) + 1;
    for (uint16_t tries = 0; tries < 50000; tries++) {
        int16_t d = (int16_t) inportb(WS_DISPLAY_LINE_PORT) - (int16_t) y_bottom;
        if (d < 0)
            d += lines;
        if (d < 48)
            return;
    }
}

// Vertical layout: a list row is a band of physical tile columns (the
// bitmap is rotated). The row is drawn into a scratch bitmap of those
// columns (+1 for glyphs reaching below the row) and copied during the
// vertical blank, skipping the icon area (the first physical tile rows).
static void ui_file_selector_draw_scrolled_vertical(file_selector_entry_t __far *fno, uint16_t y, uint8_t row_tiles,
    int x_offset, int x_start, const char __far *s) {

    uint16_t tile_top = y & ~7;
    uint16_t row_h = row_tiles << 3;
    // rotated drawing maps logical y to physical x = width*8 - h - y: with
    // width = row_tiles + 1, the row is in scratch columns 1..row_tiles and
    // anything below it falls into column 0
    bitmap_t tmp = BITMAP(sector_buffer, row_tiles + 1, ui_bitmap.height, ui_bitmap.bpp);
    uint16_t ty = y - tile_top;
    uint16_t color = ui_has_wallpaper() ? BITMAP_COLOR_4BPP(2) : BITMAP_COLOR_2BPP(2);

    bitmap_rect_fill(&tmp, 0, 0, screen_width, (row_tiles + 1) << 3, color);
    uint16_t x = x_start + bitmapfont_draw_string(&tmp, x_start, ty, s, screen_width - x_start);
    if ((fno->fno.fattrib & AM_DIR) && x < (screen_width - 6))
        bitmapfont_draw_char(&tmp, x + CONFIG_FONT_CHAR_GAP, ty, '/');
    if (x_start < x_offset && (x_offset & 7))
        bitmap_rect_fill(&tmp, x_offset & ~7, 0, x_offset & 7, (row_tiles + 1) << 3, color);

    // physical column of the row on screen; logical x = physical line
    uint16_t dst_col = ((ui_bitmap.width << 3) - row_h - tile_top) >> 3;
    uint16_t first_line = x_offset & ~7;
    uint16_t lines = (ui_bitmap.height << 3) - first_line;
    uint16_t bytes = lines * ui_bitmap.y_pitch;
    uint8_t *src = ((uint8_t*) tmp.start) + tmp.x_pitch + first_line * ui_bitmap.y_pitch;
    uint8_t *dst = ((uint8_t*) ui_bitmap.start) + dst_col * ui_bitmap.x_pitch + first_line * ui_bitmap.y_pitch;

    // the band spans every LCD line: copy during the vertical blank
    for (uint16_t tries = 0; tries < 50000 && inportb(WS_DISPLAY_LINE_PORT) < (ui_bitmap.height << 3); tries++);
    for (uint8_t c = 0; c < row_tiles; c++) {
        memcpy(dst, src, bytes);
        src += tmp.x_pitch;
        dst += ui_bitmap.x_pitch;
    }
}

// Flicker-free scrolling (color mode): the row is drawn into a scratch
// bitmap in the sector buffer, then copied to the screen tiles right after
// the LCD has displayed it, so the visible row never shows a cleared or
// half-drawn state. The icon columns are not copied.
static void ui_file_selector_draw_scrolled(file_selector_entry_t __far *fno, uint16_t y, uint8_t row_tiles,
    int x_offset, int x_start, const char __far *s) {

    if (!bitmap_rotation) {
        ui_file_selector_draw_scrolled_vertical(fno, y, row_tiles, x_offset, x_start, s);
        return;
    }

    uint16_t tile_top = y & ~7;
    // one extra tile row: glyphs may reach a few pixels below the row
    bitmap_t tmp = BITMAP(sector_buffer, ui_bitmap.width, row_tiles + 1, ui_bitmap.bpp);
    uint16_t ty = y - tile_top;
    uint16_t color = ui_has_wallpaper() ? BITMAP_COLOR_4BPP(2) : BITMAP_COLOR_2BPP(2);

    bitmap_rect_fill(&tmp, 0, 0, screen_width, (row_tiles + 1) << 3, color);
    uint16_t x = x_start + bitmapfont_draw_string(&tmp, x_start, ty, s, screen_width - x_start);
    if ((fno->fno.fattrib & AM_DIR) && x < (screen_width - 6))
        bitmapfont_draw_char(&tmp, x + CONFIG_FONT_CHAR_GAP, ty, '/');
    // a character partially scrolled out reaches left of the text area:
    // clear the gap between icon and text (the icon columns are not copied)
    if (x_start < x_offset && (x_offset & 7))
        bitmap_rect_fill(&tmp, x_offset & ~7, 0, x_offset & 7, (row_tiles + 1) << 3, color);

    uint16_t first_col = x_offset >> 3;
    uint16_t bytes = (row_tiles << 3) * ui_bitmap.y_pitch;
    uint8_t *src = ((uint8_t*) tmp.start) + first_col * tmp.x_pitch;
    uint8_t *dst = ((uint8_t*) ui_bitmap.start) + first_col * ui_bitmap.x_pitch + tile_top * ui_bitmap.y_pitch;
    uint16_t cols = (screen_width >> 3) - first_col;

    scroll_wait_beam_below(tile_top + (row_tiles << 3));
    for (uint16_t c = 0; c < cols; c++) {
        memcpy(dst, src, bytes);
        src += tmp.x_pitch;
        dst += ui_bitmap.x_pitch;
    }
}

static void ui_file_selector_draw(struct ui_selector_config *config, uint16_t offset, uint16_t y, uint16_t scroll_tick) {
    int x_offset;
    if (settings.file_flags & SETTING_FILE_HIDE_ICONS)
        x_offset = 2;
    else
        x_offset = config->style == UI_SELECTOR_STYLE_16 ? 16 : 10;

    file_selector_entry_t __far *fno = ui_file_selector_open_fno(offset);

    const char __far *name = ui_file_selector_display_name(fno);
    const char __far *s = name;
    int max_width = screen_width - x_offset;
    int x_start = x_offset;
    uint16_t clear_y = 0, clear_height = 0;
    if (!scroll_tick) {
        scroll_last_px = 0xFFFF;
    } else {
        // scroll_tick: steps since scrolling started = pixels scrolled
        int width = bitmapfont_get_string_width(s, 65535);
        if (width <= max_width)
            return; // fits: already drawn in full, nothing to scroll

        // scroll until the end of the name is visible, hold, jump back to
        // the start, hold, repeat
        // (Normal: one pixel per step, Fast: two)
        uint16_t speed = settings.scroll_speed == SETTING_SCROLL_SPEED_FAST ? 2 : 1;
        uint16_t travel = width - max_width + SCROLL_END_MARGIN;
        uint16_t travel_steps = (travel + speed - 1) / speed;
        uint16_t step = scroll_tick % (travel_steps + SCROLL_HOLD_END + SCROLL_HOLD_START);
        uint16_t px;
        if (step < travel_steps)
            px = step * speed;
        else
            px = step < travel_steps + SCROLL_HOLD_END ? travel : 0;
        if (px > travel)
            px = travel;
        if (px == scroll_last_px && offset == scroll_last_offset)
            return; // holding: nothing changed on screen
        scroll_last_px = px;
        scroll_last_offset = offset;

        // skip the characters scrolled out entirely; the next one starts
        // left of the text area by `skip` pixels
        uint16_t skip = px;
        while (true) {
            const char __far *prev = s;
            uint32_t ch = wsx_utf8_decode_next(&s);
            if (!ch) {
                s = prev;
                skip = 0;
                break;
            }
            uint16_t cw = bitmapfont_get_char_width(ch) + CONFIG_FONT_CHAR_GAP;
            if (skip < cw) {
                if (skip > x_offset || (skip && !bitmap_rotation && !sector_buffer_is_active())) {
                    // no room left of the text to draw it partially (icons
                    // hidden), or vertical layout drawn in place (mono: the
                    // icon would be covered): start with the next character
                    x_start = x_offset - skip + cw;
                } else {
                    s = prev;
                    x_start = x_offset - skip;
                }
                break;
            }
            skip -= cw;
        }
        max_width = screen_width - x_start;

        if (sector_buffer_is_active()) {
            ui_file_selector_draw_scrolled(fno, y, config->style == UI_SELECTOR_STYLE_16 ? 2 : 1,
                x_offset, x_start, s);
            return;
        }

        // Otherwise (mono mode, vertical layout) draw in place. Clear
        // exactly the row (the text starts a few pixels below its top, so a
        // font-height rectangle would reach into the next row and leave a
        // strip of the selection colour there).
        clear_y = y & ~7;
        clear_height = config->style == UI_SELECTOR_STYLE_16 ? 16 : 8;
        // redraw right after the LCD has displayed the row (horizontal
        // layout only: in the vertical one rows are not LCD lines)
        if (bitmap_rotation)
            scroll_wait_beam_below(clear_y + clear_height);
        // Only the selected row scrolls: clear it with the selection
        // colour. With a wallpaper the bitmap is 4bpp, so all four planes
        // must be written (colour 0 would be transparent and the white
        // selection text would vanish over the wallpaper).
        bitmap_rect_fill(&ui_bitmap, x_offset & ~7, clear_y, screen_width - (x_offset & ~7), clear_height,
            ui_has_wallpaper() ? BITMAP_COLOR_4BPP(2) : BITMAP_COLOR_2BPP(2));
    }

    uint16_t x = x_start + bitmapfont_draw_string(&ui_bitmap, x_start, y, s, max_width);
    // a character partially scrolled out was drawn left of the text area:
    // clear the gap between icon and text (the icon is redrawn below)
    if (x_start < x_offset && (x_offset & 7)) {
        bitmap_rect_fill(&ui_bitmap, x_offset & ~7, clear_y, x_offset & 7, clear_height,
            ui_has_wallpaper() ? BITMAP_COLOR_4BPP(2) : BITMAP_COLOR_2BPP(2));
    }
    uint8_t icon_idx = 1;
    if (fno->fno.fattrib & AM_DIR) {
        icon_idx = 0;
        if (x < (screen_width - 6)) {
            bitmapfont_draw_char(&ui_bitmap, x + CONFIG_FONT_CHAR_GAP, y, '/');
        }
    } else if (FILE_SELECTOR_ENTRY_HAS_EXTENSION(fno)) {
        icon_idx = ui_file_selector_get_file_icon_idx(FILE_SELECTOR_ENTRY_GET_EXTENSION(fno));
    }

    if (!(settings.file_flags & SETTING_FILE_HIDE_ICONS)) {
        ui_file_selector_draw_icon(0, y, icon_idx, config->style);
        if (config->style == UI_SELECTOR_STYLE_16) {
            // the 16x16 icons' background reaches the text (x = 16): give
            // their last, always empty, column the row's background
            bool selected = offset == config->offset;
            bitmap_rect_fill(&ui_bitmap, 15, y & ~7, 1, 16,
                (ui_has_wallpaper() && !selected) ? BITMAP_COLOR_4BPP(0) : BITMAP_COLOR_4BPP(2));
        }
    }
}

static bool ui_file_selector_is_track(file_selector_entry_t __far *fno) {
    if ((fno->fno.fattrib & AM_DIR) || !FILE_SELECTOR_ENTRY_HAS_EXTENSION(fno))
        return false;
    const char __far *ext = FILE_SELECTOR_ENTRY_GET_EXTENSION(fno);
    return !strcasecmp(ext, s_file_ext_vgm) || !strcasecmp(ext, s_file_ext_vgz);
}

// Next/previous VGM track in the current listing (wrapping around), or
// 0xFFFF if there is no other track.
static uint16_t ui_file_selector_find_track(uint16_t offset, uint16_t count, bool forward) {
    for (uint16_t i = 1; i < count; i++) {
        uint16_t candidate = forward ? (offset + i) % count : (offset + count - i) % count;
        if (ui_file_selector_is_track(ui_file_selector_open_fno(candidate)))
            return candidate;
    }
    return 0xFFFF;
}

// Position of a track among the tracks of the listing (1-based) and
// their number, for the player's "3/12".
static void ui_file_selector_track_position(uint16_t offset, uint16_t count) {
    uint16_t index = 0, total = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (ui_file_selector_is_track(ui_file_selector_open_fno(i))) {
            total++;
            if (i == offset)
                index = total;
        }
    }
    ui_vgmplay_set_position(index, index ? total : 0);
}

// A random track other than the current one, or 0xFFFF if there is none.
static uint16_t ui_file_selector_random_track(uint16_t offset, uint16_t count) {
    static uint16_t seed;
    uint16_t total = 0;
    for (uint16_t i = 0; i < count; i++)
        if (i != offset && ui_file_selector_is_track(ui_file_selector_open_fno(i)))
            total++;
    if (!total)
        return 0xFFFF;

    // xorshift, stirred with the frame counter and the LCD line
    seed ^= vbl_ticks ^ ((uint16_t) inportb(WS_DISPLAY_LINE_PORT) << 8);
    if (!seed) seed = 0xACE1;
    seed ^= seed << 7;
    seed ^= seed >> 9;
    seed ^= seed << 8;
    uint16_t pick = seed % total;

    for (uint16_t i = 0; i < count; i++)
        if (i != offset && ui_file_selector_is_track(ui_file_selector_open_fno(i)) && !pick--)
            return i;
    return 0xFFFF;
}

/* === ROM details in the status bar === */

static char status_path[FF_LFN_BUF + 1];

// Append a field to buf, with two spaces between fields.
static char *status_append(char *buf, char *p, char *end, const char __far *s) {
    if (p != buf && p < end - 3) {
        *(p++) = ' ';
        *(p++) = ' ';
    }
    while (*s && p < end - 1)
        *(p++) = *(s++);
    *p = 0;
    return p;
}

// Launch statistics of an entry (0: never launched, or statistics off).
// Leaves the whole path in status_path.
static uint16_t ui_file_selector_entry_stats(file_selector_entry_t __far *fno, char *date) {
    date[0] = 0;
    if (settings.favorites_mode == SETTING_FAVORITES_OFF)
        return 0;
    if (listing_list != FAVORITES_LIST_NONE) {
        // lists: fname is already the whole path
        strncpy(status_path, fno->fno.fname, sizeof(status_path) - 1);
        status_path[sizeof(status_path) - 1] = 0;
    } else if (!favorites_full_path(status_path, sizeof(status_path), fno->fno.fname)) {
        return 0;
    }
    return favorites_get_stats(status_path, date);
}

static bool ui_file_selector_status_text(struct ui_selector_config *config, uint16_t idx, char *buf, uint16_t buf_len) {
    file_selector_entry_t __far *fno = ui_file_selector_open_fno(idx);
    if ((fno->fno.fattrib & AM_DIR) || !FILE_SELECTOR_ENTRY_HAS_EXTENSION(fno))
        return false;

    char date[FAVORITES_STATS_DATE_LEN + 1];
    uint16_t played = 0;
    if (listing_list == FAVORITES_LIST_RECENT || listing_list == FAVORITES_LIST_PLAYED) {
        // Recent and Most played: the launch statistics instead of the ROM
        // details ("Played 12x  2026-11-01")
        played = ui_file_selector_entry_stats(fno, date);
        if (played) {
            snprintf(buf, buf_len, lang_keys[LK_UI_STATUS_PLAYED], played);
            if (date[0])
                status_append(buf, buf + strlen(buf), buf + buf_len, date);
            return true;
        }
    }

    const char __far *ext = FILE_SELECTOR_ENTRY_GET_EXTENSION(fno);
    if (strcasecmp(ext, s_file_ext_ws) && strcasecmp(ext, s_file_ext_wsc) && strcasecmp(ext, s_file_ext_pc2))
        return false;

    strncpy(status_path, fno->fno.fname, sizeof(status_path) - 1);
    status_path[sizeof(status_path) - 1] = 0;
    launch_rom_metadata_t meta;
    if (launch_get_rom_metadata(status_path, &meta) != FR_OK)
        return false;

    char num[16];
    char *p = buf, *end = buf + buf_len;
    *p = 0;

    // system
    if (meta.rom_type == ROM_TYPE_PCV2)
        p = status_append(buf, p, end, s_rom_info_pcv2);
    else
        p = status_append(buf, p, end, lang_keys[(meta.footer.color & 1) ? LK_ROM_INFO_COLOR : LK_ROM_INFO_MONO]);

    // ROM size, as declared in the header
    if (meta.rom_banks >= 16 && !(meta.rom_banks & 15))
        snprintf(num, sizeof(num), s_rom_info_mb_tpl, meta.rom_banks >> 4);
    else if (meta.rom_banks)
        snprintf(num, sizeof(num), s_rom_info_kb_tpl, meta.rom_banks * 64);
    else
        num[0] = 0;
    if (num[0])
        p = status_append(buf, p, end, num);

    // save type
    if (meta.sram_size)
        snprintf(num, sizeof(num), s_rom_info_sram_tpl, (uint16_t) (meta.sram_size >> 10));
    else if (meta.eeprom_size >= 1024)
        snprintf(num, sizeof(num), s_rom_info_eeprom_kb_tpl, (uint16_t) (meta.eeprom_size >> 10));
    else if (meta.eeprom_size)
        snprintf(num, sizeof(num), s_rom_info_eeprom_b_tpl, (uint16_t) meta.eeprom_size);
    else
        num[0] = 0;
    p = status_append(buf, p, end, num[0] ? (const char __far*) num : lang_keys[LK_ROM_INFO_NO_SAVE]);

    // orientation
    // (vertical layout, 144 pixels: just "H" / "V")
    if (bitmap_rotation)
        p = status_append(buf, p, end, lang_keys[(meta.footer.flags & 1) ? LK_ROM_INFO_VERTICAL : LK_ROM_INFO_HORIZONTAL]);
    else
        p = status_append(buf, p, end, lang_keys[(meta.footer.flags & 1) ? LK_ROM_INFO_VERTICAL_SHORT : LK_ROM_INFO_HORIZONTAL_SHORT]);

    // launch count ("12x"), in the file list only (the lists above show it
    // already, or the game was never launched)
    if (listing_list == FAVORITES_LIST_NONE) {
        played = ui_file_selector_entry_stats(fno, date);
        if (played) {
            snprintf(num, sizeof(num), s_stats_count_tpl, played);
            status_append(buf, p, end, num);
        }
    }
    return true;
}

/* === ROM index (SWANSHELL.IDX) ===
 *
 * Header, then the entries exactly as stored in SRAM by the directory scan
 * (one slot of 1 << FILE_SELECTOR_ENTRY_SHIFT bytes each, unsorted), so that
 * loading is a single sequential read. Sorting is applied after loading.
 */

#define FILE_INDEX_VERSION 1
#define FILE_INDEX_FILTER_FLAGS (SETTING_FILE_SHOW_HIDDEN | SETTING_FILE_SHOW_SAVES | SETTING_FILE_HIDE_NILESWAN)

typedef struct __attribute__((packed)) {
    char magic[4];         // "SSIX"
    uint16_t version;
    uint16_t entry_size;   // sizeof(file_selector_entry_t)
    uint16_t entry_stride; // 1 << FILE_SELECTOR_ENTRY_SHIFT
    uint16_t count;
    uint8_t filter_flags;  // settings.file_flags & FILE_INDEX_FILTER_FLAGS
    uint8_t reserved[5];
} file_index_header_t;

static bool listing_from_index = false;

static void ui_file_selector_index_header(file_index_header_t *hdr, uint16_t count) {
    memset(hdr, 0, sizeof(file_index_header_t));
    hdr->magic[0] = 'S'; hdr->magic[1] = 'S'; hdr->magic[2] = 'I'; hdr->magic[3] = 'X';
    hdr->version = FILE_INDEX_VERSION;
    hdr->entry_size = sizeof(file_selector_entry_t);
    hdr->entry_stride = 1 << FILE_SELECTOR_ENTRY_SHIFT;
    hdr->count = count;
    hdr->filter_flags = settings.file_flags & FILE_INDEX_FILTER_FLAGS;
}

// Load the listing from SWANSHELL.IDX in the current directory.
// Returns FR_OK only if the index exists, is valid and matches the settings.
static int16_t ui_file_selector_load_index(uint16_t *count) {
    FIL fp;
    file_index_header_t hdr, expected;
    unsigned int br;

    *count = 0;
    int16_t result = f_open_far(&fp, s_file_index_name, FA_READ | FA_OPEN_EXISTING);
    if (result != FR_OK)
        return result;

    result = f_read(&fp, &hdr, sizeof(hdr), &br);
    if (result == FR_OK) {
        ui_file_selector_index_header(&expected, hdr.count);
        if (br != sizeof(hdr) || memcmp(&hdr, &expected, sizeof(hdr))
            || hdr.count > FILE_SELECTOR_MAX_FILES
            || f_size(&fp) != sizeof(hdr) + ((uint32_t) hdr.count << FILE_SELECTOR_ENTRY_SHIFT))
            result = FR_NO_FILE; // stale or foreign: fall back to scanning
    }
    if (result == FR_OK && hdr.count > 0) {
        result = f_read_sram_banked(&fp, FILE_SELECTOR_RAM_BANK_OFFSET,
            (uint32_t) hdr.count << FILE_SELECTOR_ENTRY_SHIFT, NULL, NULL);
    }
    f_close(&fp);

    if (result != FR_OK)
        return result;

    ui_file_selector_sort_entries(hdr.count);
    *count = hdr.count;
    return FR_OK;
}

int16_t ui_file_selector_scrape_directory(void) {
    FIL fp;
    file_index_header_t hdr;
    unsigned int bw;
    uint16_t count;
    char path[4];

    strcpy(path, s_dot);
    int16_t result = ui_file_selector_scan_directory(path, ui_file_selector_default_predicate, &count);
    if (result != FR_OK)
        return result;

    result = f_open_far(&fp, s_file_index_name, FA_WRITE | FA_CREATE_ALWAYS);
    if (result != FR_OK)
        return result;

    ui_file_selector_index_header(&hdr, count);
    result = f_write(&fp, &hdr, sizeof(hdr), &bw);
    if (result == FR_OK && bw != sizeof(hdr))
        result = FR_DISK_ERR;
    if (result == FR_OK && count > 0) {
        result = f_write_sram_banked(&fp, FILE_SELECTOR_RAM_BANK_OFFSET,
            (uint32_t) count << FILE_SELECTOR_ENTRY_SHIFT, NULL, NULL, false);
    }
    int16_t close_result = f_close(&fp);
    if (result == FR_OK)
        result = close_result;

    if (result != FR_OK)
        ui_file_selector_remove_index(); // never leave a half-written index
    return result;
}

int16_t ui_file_selector_remove_index(void) {
    char name[16];
    strcpy(name, s_file_index_name);
    int16_t result = f_unlink(name);
    return result == FR_NO_FILE ? FR_OK : result;
}

/* === Jump by initial letter (Y2 / Y4) ===
 * Works on groups of consecutive entries with the same initial, in the
 * current sort order. */

static uint8_t ui_file_selector_initial(uint16_t idx) {
    file_selector_entry_t __far *fno = ui_file_selector_open_fno(idx);
    uint8_t c = ui_file_selector_entry_name(fno)[0];
    if (c >= 'a' && c <= 'z')
        c -= 'a' - 'A';
    return c;
}

static uint16_t ui_file_selector_group_start(uint16_t idx) {
    uint8_t c = ui_file_selector_initial(idx);
    while (idx > 0 && ui_file_selector_initial(idx - 1) == c)
        idx--;
    return idx;
}

static uint16_t ui_file_selector_letter_jump(uint16_t offset, uint16_t count, bool forward) {
    if (forward) {
        // first entry with a different initial; past the last group, wrap
        uint8_t c = ui_file_selector_initial(offset);
        for (uint16_t i = offset + 1; i < count; i++)
            if (ui_file_selector_initial(i) != c)
                return i;
        return 0;
    }
    // start of the current group first, then start of the previous one
    uint16_t start = ui_file_selector_group_start(offset);
    if (start != offset)
        return start;
    return ui_file_selector_group_start(start > 0 ? start - 1 : count - 1);
}

/* === Favorites / Recent: listing helpers === */

// Title of a list (FAVORITES_LIST_*).
static const char __far *ui_file_selector_list_title(uint8_t list) {
    if (list == FAVORITES_LIST_PLAYED)
        return lang_keys[LK_UI_FILE_SELECTOR_PLAYED];
    return lang_keys[list == FAVORITES_LIST_RECENT ? LK_UI_FILE_SELECTOR_RECENT : LK_UI_FILE_SELECTOR_FAVORITES];
}

#define SPECIAL_ENTRY_COUNT 3

// "File list" mode: add "Favorites", "Recent" and "Most played" as the
// first entries of the root folder's listing.
static void ui_file_selector_add_special_entries(uint16_t *count) {
    listing_special_slot = 0xFFFF;
    uint16_t n = *count;
    if (n + SPECIAL_ENTRY_COUNT > FILE_SELECTOR_MAX_FILES)
        return;

    for (uint8_t i = 0; i < SPECIAL_ENTRY_COUNT; i++) {
        file_selector_entry_t __far *fno = ui_file_selector_open_fno_direct(n + i);
        // (slot n + i opens list FAVORITES_LIST_FAVORITES + i)
        const char __far *src = ui_file_selector_list_title(FAVORITES_LIST_FAVORITES + i);
        uint8_t j = 0;
        while (src[j] && j < sizeof(fno->fno.fname) - 1) {
            fno->fno.fname[j] = src[j];
            j++;
        }
        fno->fno.fname[j] = 0;
        fno->fno.altname[0] = 0;
        fno->fno.fattrib = AM_DIR;
        fno->fno.fsize = 0;
        fno->fno.fdate = 0;
        fno->fno.ftime = 0;
        fno->extension_loc = 255;
    }

    outportw(WS_CART_EXTBANK_RAM_PORT, FILE_SELECTOR_INDEX_BANK);
    FILE_SELECTOR_MEMORY_BARRIER();
    for (uint16_t j = n; j > 0; j--)
        FILE_SELECTOR_INDEXES[j - 1 + SPECIAL_ENTRY_COUNT] = FILE_SELECTOR_INDEXES[j - 1];
    for (uint8_t i = 0; i < SPECIAL_ENTRY_COUNT; i++)
        FILE_SELECTOR_INDEXES[i] = n + i;

    listing_special_slot = n;
    *count = n + SPECIAL_ENTRY_COUNT;
}

// Which list a root folder entry opens (FAVORITES_LIST_NONE: a real entry).
static uint8_t ui_file_selector_special_list(uint16_t offset) {
    if (listing_special_slot == 0xFFFF || listing_list != FAVORITES_LIST_NONE)
        return FAVORITES_LIST_NONE;
    outportw(WS_CART_EXTBANK_RAM_PORT, FILE_SELECTOR_INDEX_BANK);
    FILE_SELECTOR_MEMORY_BARRIER();
    uint16_t slot = FILE_SELECTOR_INDEXES[offset];
    if (slot >= listing_special_slot && slot < listing_special_slot + SPECIAL_ENTRY_COUNT)
        return FAVORITES_LIST_FAVORITES + (slot - listing_special_slot);
    return FAVORITES_LIST_NONE;
}

// buf: a FF_LFN_BUF + 1 bytes work buffer
static void ui_file_selector_open_list(uint8_t list, char *buf) {
    if (listing_list == FAVORITES_LIST_NONE) {
        if (f_getcwd(buf, FF_LFN_BUF + 1) != FR_OK) {
            buf[0] = '/';
            buf[1] = 0;
        }
        char __far *dst = (char __far*) ui_file_selector_open_fno_direct(FILE_SELECTOR_SCRATCH_SLOT);
        uint16_t i = 0;
        do {
            dst[i] = buf[i];
        } while (buf[i++]);
    }
    listing_list = list;
    listing_list_offset = 0;
}

static void ui_file_selector_close_list(char *buf) {
    if (listing_list != FAVORITES_LIST_NONE) {
        listing_list = FAVORITES_LIST_NONE;
        const char __far *src = (const char __far*) ui_file_selector_open_fno_direct(FILE_SELECTOR_SCRATCH_SLOT);
        uint16_t i = 0;
        do {
            buf[i] = src[i];
        } while (src[i++] && i < FF_LFN_BUF);
        buf[FF_LFN_BUF] = 0;
        f_chdir(buf);
    }
}

// Copy an entry's name to buf for opening it, and return its extension (or
// NULL). In a list, first change to the entry's directory, so that the
// file is opened (and its saves found) exactly as from the file list.
static const char *ui_file_selector_prepare_entry(char *buf, file_selector_entry_t __far *fno) {
    strncpy(buf, fno->fno.fname, FF_LFN_BUF);
    buf[FF_LFN_BUF] = 0;
    uint8_t ext_loc = fno->extension_loc;

    if (listing_list != FAVORITES_LIST_NONE) {
        // (no strrchr(): it returns a far pointer in this toolchain)
        char *slash = NULL;
        for (char *p = buf; *p; p++)
            if (*p == '/')
                slash = p;
        if (slash != NULL) {
            // cut after the slash for the root directory, else at it
            char *cut = (slash == buf) ? slash + 1 : slash;
            char saved = *cut;
            *cut = 0;
            f_chdir(buf);
            *cut = saved;
            uint8_t base = slash + 1 - buf;
            memmove(buf, slash + 1, strlen(slash + 1) + 1);
            if (ext_loc != 255)
                ext_loc -= base;
        }
    }
    return ext_loc == 255 ? NULL : buf + ext_loc;
}

static bool ui_file_selector_is_game(const char *ext) {
    return ext != NULL && (!strcasecmp(ext, s_file_ext_ws) || !strcasecmp(ext, s_file_ext_wsc)
        || !strcasecmp(ext, s_file_ext_pc2) || !strcasecmp(ext, s_file_ext_ips)
        || !strcasecmp(ext, s_file_ext_bps));
}

// Remember a ROM or patch that is about to be launched (name in the
// current directory): Recent list and statistics.
static void ui_file_selector_note_recent(const char *name, const char *ext) {
    if (settings.favorites_mode == SETTING_FAVORITES_OFF || !ui_file_selector_is_game(ext))
        return;
    if (favorites_full_path(status_path, sizeof(status_path), name)) {
        favorites_add_recent(status_path);
        favorites_note_played(status_path);
    }
}

// Settings > System > Choose save at launch: a game whose save has backups
// asks which save to use. False: cancelled (back to the list).
static bool ui_file_selector_choose_save(const char *name, const char *ext) {
    if (!(settings.save_flags & SETTING_SAVE_ASK_BACKUP) || !ui_file_selector_is_game(ext))
        return true;
    if (!save_backup_find(status_path, sizeof(status_path), name))
        return true;
    return ui_save_backup_choose(status_path, true);
}

// Width of "...", in pixels.
#define DOT3_WIDTH 6

/* === Title bar: path on the left, "NileSwan" in the middle, the number of
 * game images and the IDX tag on the right === */

static uint16_t ui_file_selector_title_center_width(void) {
    bitmapfont_set_active_font(font8_bitmap);
    return bitmapfont_get_string_width(s_title_nileswan, screen_width);
}

// room for the path, left of the centered title
static int16_t ui_file_selector_title_left_max(void) {
    return ((screen_width - ui_file_selector_title_center_width()) >> 1) - 4;
}

static void ui_file_selector_draw_title_center(void) {
    uint16_t w = ui_file_selector_title_center_width();
    uint16_t x = (screen_width - w) >> 1;
    bitmap_rect_fill(&ui_bitmap, x - 2, 0, w + 4, 8, BITMAP_COLOR_2BPP(2));
    bitmapfont_draw_string(&ui_bitmap, x, 0, s_title_nileswan, w + 1);
}

/* Long folder names scroll in the title bar (color mode, "scroll long
 * names" on), with the speed and pauses of the file names. The text is
 * ".../<current folder>" (or "/<folder>"), kept in the sector buffer above
 * the game images; it is drawn into a scratch bitmap with an 8-pixel left
 * margin (so a character partly scrolled out is drawn correctly) and the
 * columns left of "NileSwan" are copied to the screen. */
#define TITLE_SCROLL_OFFSET 6144
#define TITLE_SCROLL_MAX    (CONFIG_MEMLAYOUT_SECTOR_BUFFER_SIZE - TITLE_SCROLL_OFFSET)
#define TITLE_SCROLL_TEXT   ((char *) (sector_buffer + TITLE_SCROLL_OFFSET))
_Static_assert(GAME_IMAGES_BUFFER_END <= TITLE_SCROLL_OFFSET, "title text overlaps the game images");

static bool title_scroll_active;
static uint16_t title_scroll_width;   // text width, pixels
static uint16_t title_scroll_area;    // pixels copied (whole tiles, left of the centre)
static uint16_t title_scroll_frames;
static uint16_t title_scroll_last_px;
static uint8_t title_scroll_check;    // sum of the text: detects a reused buffer

static uint8_t title_scroll_sum(void) {
    uint8_t sum = 0;
    for (const char *c = TITLE_SCROLL_TEXT; *c; c++)
        sum += (uint8_t) *c;
    return sum;
}

static void ui_file_selector_draw_title_scrolled(uint16_t px) {
    const char __far *s = TITLE_SCROLL_TEXT;
    // the title font: the list may have left its 16-pixel font active, and
    // the widths below must be those the text was measured with
    bitmapfont_set_active_font(font8_bitmap);
    // skip the characters scrolled out entirely
    uint16_t skip = px;
    while (true) {
        const char __far *prev = s;
        uint32_t ch = wsx_utf8_decode_next(&s);
        if (!ch) {
            s = prev;
            skip = 0;
            break;
        }
        uint16_t cw = bitmapfont_get_char_width(ch) + CONFIG_FONT_CHAR_GAP;
        if (skip < cw) {
            s = prev;
            break;
        }
        skip -= cw;
    }

    uint16_t cols = title_scroll_area >> 3;
    uint16_t color = BITMAP_COLOR_2BPP(2);
    if (bitmap_rotation) {
        // (one tile more on the right: a character cut by the edge is drawn)
        bitmap_t tmp = BITMAP(sector_buffer, cols + 2, 1, ui_bitmap.bpp);
        bitmap_rect_fill(&tmp, 0, 0, (cols + 2) << 3, 8, color);
        bitmapfont_draw_string(&tmp, 8 + 2 - skip, 0, s, title_scroll_area + skip + 6);
        uint8_t *src = ((uint8_t*) tmp.start) + tmp.x_pitch;
        uint8_t *dst = (uint8_t*) ui_bitmap.start;
        uint16_t bytes = 8 * ui_bitmap.y_pitch;
        // right after the LCD has drawn the title bar (lines 0-7)
        scroll_wait_beam_below(8);
        for (uint16_t c = 0; c < cols; c++) {
            memcpy(dst, src, bytes);
            src += tmp.x_pitch;
            dst += ui_bitmap.x_pitch;
        }
    } else {
        // vertical: the title bar is the last physical tile column; logical
        // x = physical line (the scratch bitmap has one more tile of lines
        // for the margin)
        bitmap_t tmp = BITMAP(sector_buffer, 2, ui_bitmap.height + 1, ui_bitmap.bpp);
        bitmap_rect_fill(&tmp, 0, 0, (cols + 2) << 3, 16, color);
        bitmapfont_draw_string(&tmp, 8 + 2 - skip, 0, s, title_scroll_area + skip + 6);
        uint16_t dst_col = ui_bitmap.width - 1;
        uint8_t *src = ((uint8_t*) tmp.start) + tmp.x_pitch + 8 * ui_bitmap.y_pitch;
        uint8_t *dst = ((uint8_t*) ui_bitmap.start) + dst_col * ui_bitmap.x_pitch;
        // the column spans every LCD line: copy during the vertical blank
        for (uint16_t tries = 0; tries < 50000 && inportb(WS_DISPLAY_LINE_PORT) < (ui_bitmap.height << 3); tries++);
        memcpy(dst, src, title_scroll_area * ui_bitmap.y_pitch);
    }
}

// path: the current directory. Decides whether the title scrolls and, if
// so, draws its first frame (call after the normal title was drawn).
static void ui_file_selector_title_scroll_setup(const char *path, int16_t title_max) {
    title_scroll_active = false;
    if (!sector_buffer_is_active() || !(settings.file_flags & SETTING_THEME_SCROLL_LONG_NAMES) || title_max < 16)
        return;
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            last = p;
    char *text = TITLE_SCROLL_TEXT;
    uint16_t n = 0;
    if (last != path) {
        text[n++] = '.'; text[n++] = '.'; text[n++] = '.';
    }
    while (*last && n < TITLE_SCROLL_MAX - 1)
        text[n++] = *(last++);
    text[n] = 0;

    bitmapfont_set_active_font(font8_bitmap);
    title_scroll_area = title_max & ~7;
    title_scroll_width = bitmapfont_get_string_width(text, 65535);
    if (title_scroll_area < 16 || title_scroll_width + 2 <= title_scroll_area)
        return; // fits (or no room): the normal title stays
    title_scroll_check = title_scroll_sum();
    title_scroll_frames = 0;
    title_scroll_last_px = 0;
    title_scroll_active = true;
}

// Called every frame while the list is idle.
static void ui_file_selector_title_scroll_tick(void) {
    if (!title_scroll_active)
        return;
    if (title_scroll_sum() != title_scroll_check) {
        title_scroll_active = false; // the buffer was reused: stop
        return;
    }
    title_scroll_frames++;
    if (title_scroll_frames & 1)
        return; // a step every 2 frames, like the file names
    uint16_t steps = title_scroll_frames >> 1;
    uint16_t speed = settings.scroll_speed == SETTING_SCROLL_SPEED_FAST ? 2 : 1;
    uint16_t travel = title_scroll_width + 2 - title_scroll_area + SCROLL_END_MARGIN;
    uint16_t travel_steps = (travel + speed - 1) / speed;
    // start with the pause at the beginning
    uint16_t step = (steps + travel_steps + SCROLL_HOLD_END) % (travel_steps + SCROLL_HOLD_END + SCROLL_HOLD_START);
    uint16_t px;
    if (step < travel_steps)
        px = step * speed;
    else
        px = step < travel_steps + SCROLL_HOLD_END ? travel : 0;
    if (px > travel)
        px = travel;
    if (px == title_scroll_last_px)
        return;
    title_scroll_last_px = px;
    ui_file_selector_draw_title_scrolled(px);
}

static void ui_file_selector_draw_images_indicator(uint8_t count) {
    uint16_t x0 = ((screen_width + ui_file_selector_title_center_width()) >> 1) + 4;
    // fixed place, never at the right edge: that is where IDX goes (at
    // least five characters, or the IDX tag if wider, are kept free)
    uint16_t idx_width = bitmapfont_get_string_width(lang_keys[LK_UI_FILE_SELECTOR_INDEX_TAG], screen_width);
    uint16_t reserve = idx_width;
    // horizontal: at least five characters; vertical (144 pixels): just IDX
    if (bitmap_rotation) {
        uint16_t five = bitmapfont_get_string_width(s_title_right_reserve, screen_width);
        if (five > reserve)
            reserve = five;
    }
    uint16_t x1 = screen_width - 2 - reserve - (bitmap_rotation ? 8 : 4);
    if (x1 <= x0)
        return;
    bitmap_rect_fill(&ui_bitmap, x0, 0, x1 - x0, 8, BITMAP_COLOR_2BPP(2));
    if (count) {
        char buf[12];
        snprintf(buf, sizeof(buf), bitmap_rotation ? s_images_indicator_tpl : s_images_indicator_short_tpl, count);
        uint16_t w = bitmapfont_get_string_width(buf, screen_width);
        if (x0 + w <= x1)
            bitmapfont_draw_string(&ui_bitmap, x1 - w, 0, buf, w + 1);
    }
}

/* === Game images: indicator, preview behind the list, viewer === */

#define IMAGES_INDICATOR_TICKS 10  // ~0.13 s after the selection stops
#define IMAGES_PREVIEW_TICKS   38  // ~0.5 s

static uint8_t images_shown_count;  // indicator currently shown
static bool images_preview_active;  // a game picture replaces the wallpaper

static bool ui_file_selector_images_available(void) {
#ifdef CONFIG_ENABLE_WALLPAPER
    return sector_buffer_is_active() && ws_system_get_mode() == WS_MODE_COLOR_4BPP;
#else
    return false;
#endif
}

// Find the pictures of entry idx (none for folders and the root entries).
static uint8_t ui_file_selector_find_images(uint16_t idx, game_images_t *images);
static uint8_t ui_file_selector_special_list(uint16_t offset);

// The VGM player took its track as menu music (already in the music area):
// save it as /NILESWAN/MENU.VGM now, with the player off the stack, and
// turn the music on.
static void ui_file_selector_apply_menu_music(const char *name) {
    (void) name;
    if (!menu_music_take_install_request())
        return;
    int16_t result = menu_music_save();
    if (result == FR_OK) {
        settings.file_flags |= SETTING_FILE_MENU_MUSIC;
        settings_save();
    } else {
        ui_dialog_error_check(result, lang_keys[LK_UI_VGMPLAY_MENU_MUSIC_FAILED], 0);
    }
}

// Close the thumbnail popup (before anything else is drawn over the list).
static void ui_file_selector_end_thumbnail(void) {
#ifdef CONFIG_ENABLE_WALLPAPER
    ui_hide_thumbnail();
#endif
}

static void ui_file_selector_end_preview(void) {
    if (images_preview_active) {
        images_preview_active = false;
        ui_reload_wallpaper();
    }
}

static bool ui_file_selector_idle(struct ui_selector_config *config, uint16_t idx, uint16_t ticks) {
    ui_file_selector_title_scroll_tick();
    if (!ui_file_selector_images_available())
        return false;

    if (ticks == 1)
        ui_file_selector_end_thumbnail(); // the selection moved
    if (ticks == 1 && images_shown_count) {
        images_shown_count = 0;
        ui_file_selector_draw_images_indicator(0);
    } else if (ticks == IMAGES_PREVIEW_TICKS && settings.images_preview == SETTING_IMAGES_PREVIEW_THUMBNAIL) {
        // thumb.bmp of the game in a popup (found again: loading the menu
        // music may have reused the buffer)
        game_images_t *images = game_images_buffer();
        ui_file_selector_find_images(idx, images);
#ifdef CONFIG_ENABLE_WALLPAPER
        if (game_images_thumb_path(images, images->tmp))
            ui_show_thumbnail(images->tmp);
#endif
    } else if (ticks == IMAGES_INDICATOR_TICKS) {
        images_shown_count = ui_file_selector_find_images(idx, game_images_buffer());
        if (images_shown_count)
            ui_file_selector_draw_images_indicator(images_shown_count);
    } else if (ticks == IMAGES_PREVIEW_TICKS && settings.images_preview == SETTING_IMAGES_PREVIEW_BACKGROUND) {
        // (found again: loading the menu music may have reused the buffer)
        game_images_t *images = game_images_buffer();
        if (images_shown_count && ui_file_selector_find_images(idx, images)
            && game_images_path(images, 0, images->tmp)) {
            // the first picture behind the list (the rows are redrawn)
            if (ui_load_background(images->tmp)) {
                images_preview_active = true;
            } else {
                images_preview_active = false;
                ui_reload_wallpaper();
            }
            return true;
        } else if (images_preview_active) {
            ui_file_selector_end_preview();
            return true;
        }
    }
    return false;
}

static uint8_t ui_file_selector_find_images(uint16_t idx, game_images_t *images) {
    images->count = 0;
    images->has_thumb = false; // (folders and the root entries return early)
    if (ui_file_selector_special_list(idx) != FAVORITES_LIST_NONE)
        return 0;
    file_selector_entry_t __far *fno = ui_file_selector_open_fno(idx);
    if (fno->fno.fattrib & AM_DIR)
        return 0;
    // copy the name first: reading pictures remaps the RAM window
    // (game_images_find() copies it to images->dir before using tmp)
    const char __far *name = ui_file_selector_entry_name(fno);
    uint16_t n = 0;
    while (name[n] && n < sizeof(images->tmp) - 1) {
        images->tmp[n] = name[n];
        n++;
    }
    images->tmp[n] = 0;
    return game_images_find(images->tmp, images);
}

// Y3 / Start > Images. Returns true if the user chose to play the game.
static bool ui_file_selector_view_images(uint16_t idx, char *title_buf) {
    game_images_t *images = game_images_buffer();
    if (!ui_file_selector_images_available() || !ui_file_selector_find_images(idx, images)) {
        ui_draw_statusbar(lang_keys[LK_UI_STATUS_NO_IMAGES]);
        for (uint8_t i = 0; i < 60; i++)
            wait_for_vblank();
        return false;
    }
    // the title: the name as shown in the list (copied: SRAM gets remapped)
    file_selector_entry_t __far *fno = ui_file_selector_open_fno(idx);
    const char __far *name = ui_file_selector_display_name(fno);
    strncpy(title_buf, name, FF_LFN_BUF);
    title_buf[FF_LFN_BUF] = 0;
    images_preview_active = false; // the viewer restores the wallpaper
    return game_images_view(images, title_buf);
}

void ui_file_selector(void) {
    char strbuf[FF_LFN_BUF + 1];

    ui_selector_config_t config = {0};
    bool reinit_ui = false;
    bool reinit_dirs = true;

    uint16_t path_depth[CONFIG_FILESELECT_PATH_MEMORY_DEPTH] = {0};
    uint8_t path_depth_pos = 255;

// The selection to come back to: per list, or per directory depth.
#define REMEMBER_OFFSET() \
    if (listing_list != FAVORITES_LIST_NONE) \
        listing_list_offset = config.offset; \
    else if (path_depth_pos < CONFIG_FILESELECT_PATH_MEMORY_DEPTH) \
        path_depth[path_depth_pos] = config.offset

rescan_directory:
    ui_icons_recover();
    config.draw = ui_file_selector_draw;
    config.status_text = ui_file_selector_status_text;
    config.idle = ui_file_selector_idle;
    config.key_mask = WS_KEY_A | WS_KEY_B | WS_KEY_START | KEY_ALEFT | KEY_ARIGHT | KEY_ADOWN;
    // a game picture shown behind the list: back to the wallpaper
    ui_file_selector_end_thumbnail();
    ui_file_selector_end_preview();
    images_shown_count = 0;
    if (settings.favorites_mode != SETTING_FAVORITES_OFF)
        config.key_mask |= KEY_AUP;
    else
        ui_file_selector_close_list(strbuf);
    config.style = settings.file_view;
    // the listing (and SWANSHELL.IDX loading) uses SRAM, not PSRAM
    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);

    if (reinit_ui) {
        ui_layout_bars();
    }
    title_scroll_active = false;
    ui_draw_titlebar(NULL);
    ui_draw_statusbar(lang_keys[LK_UI_STATUS_LOADING]);
    ui_show();
    if (!reinit_ui && ui_has_wallpaper()) {
        ui_layout_bars();
    }
    if ((reinit_ui || reinit_dirs) && listing_list != FAVORITES_LIST_NONE) {
        ui_draw_titlebar(ui_file_selector_list_title(listing_list));
        ui_file_selector_draw_title_center();
    } else if (reinit_ui || reinit_dirs) {
        char *path = strbuf + 3;
        f_getcwd(path, sizeof(strbuf) - 4);

        // "/" = depth 0
        // "/a" = depth 1
        // "/a/b" = depth 2
        path_depth_pos = (path[0] != 0 && path[1] != 0) ? 1 : 0;

        int16_t title_max = ui_file_selector_title_left_max();
        bool title_scroll = sector_buffer_is_active() && (settings.file_flags & SETTING_THEME_SCROLL_LONG_NAMES);
        if (title_scroll) {
            // (copied before the path below gets its "..." written into it)
            ui_file_selector_title_scroll_setup(path, title_max);
        }
        bitmapfont_set_active_font(font8_bitmap);
        char *draw_path = path;
        int16_t draw_path_len = bitmapfont_get_string_width(draw_path, screen_width + DOT3_WIDTH) - DOT3_WIDTH;
        char *p = path + 1;
        while (*p) {
            if (*p == '/') {
                if (draw_path_len > title_max - DOT3_WIDTH) {
                    draw_path = p;
                    draw_path_len = bitmapfont_get_string_width(draw_path, screen_width);
                }
                path_depth_pos++;
            }
            p++;
        }
        while (draw_path_len > title_max - DOT3_WIDTH) {
            draw_path++;
            draw_path_len = bitmapfont_get_string_width(draw_path, screen_width);
        }
        if (draw_path != path) {
            *(--draw_path) = '.';
            *(--draw_path) = '.';
            *(--draw_path) = '.';
        }
        ui_draw_titlebar(draw_path);
        ui_file_selector_draw_title_center();
        if (title_scroll_active) {
            // the scrolled text covers whole tiles: clear the few pixels
            // between them and "NileSwan" (the fixed title drew there)
            int16_t gap_end = ((screen_width - ui_file_selector_title_center_width()) >> 1) - 2;
            if (gap_end > (int16_t) title_scroll_area)
                bitmap_rect_fill(&ui_bitmap, title_scroll_area, 0, gap_end - title_scroll_area, 8, BITMAP_COLOR_2BPP(2));
            ui_file_selector_draw_title_scrolled(0);
        }
    }
    if (reinit_dirs && listing_list != FAVORITES_LIST_NONE) {
        listing_from_index = false;
        listing_special_slot = 0xFFFF;
        int16_t result = favorites_load(listing_list, &config.count);
        if (ui_dialog_error_check(result, NULL, 0)) {
            ui_file_selector_close_list(strbuf);
            reinit_ui = true;
            goto rescan_directory;
        }
        config.offset = listing_list_offset;
        if (config.offset >= config.count)
            config.offset = config.count ? config.count - 1 : 0;
    } else if (reinit_dirs) {
        config.offset = path_depth_pos >= CONFIG_FILESELECT_PATH_MEMORY_DEPTH ? 0 : path_depth[path_depth_pos];
        int16_t result = FR_OK;
        // Use the ROM index (Tools > Scrape ROMs) if present, else scan.
        listing_from_index = ui_file_selector_load_index(&config.count) == FR_OK;
        if (!listing_from_index) {
            strcpy(strbuf, s_dot);
            result = ui_file_selector_scan_directory(strbuf, ui_file_selector_default_predicate, &config.count);
        }
        if (ui_dialog_error_check(result, NULL, 0)) {
            strcpy(strbuf, s_dotdot);
            f_chdir(strbuf);
            if (path_depth_pos) path_depth_pos--;
            goto rescan_directory;
        }
        listing_special_slot = 0xFFFF;
        // (the remembered selection already counts the special entries)
        if (path_depth_pos == 0 && settings.favorites_mode == SETTING_FAVORITES_FILE_LIST)
            ui_file_selector_add_special_entries(&config.count);
    }
    if (listing_from_index)
        ui_draw_titlebar_right(lang_keys[LK_UI_FILE_SELECTOR_INDEX_TAG]);
    reinit_ui = false;
    reinit_dirs = false;

    while (true) {
        uint16_t keys_pressed = ui_selector(&config);
        // a key for an action (or a reload): the popup would hide part of
        // what comes next
        ui_file_selector_end_thumbnail();

        if (keys_pressed == UI_SELECTOR_RELOAD_REQUESTED) {
            reinit_ui = true;
            reinit_dirs = true;
            REMEMBER_OFFSET();
            goto rescan_directory;
        }
        if ((keys_pressed & KEY_ADOWN) && config.count) {
            // Y3: pictures of the game; A in the viewer plays it
            REMEMBER_OFFSET();
            if (ui_file_selector_view_images(config.offset, strbuf)) {
                keys_pressed = WS_KEY_A;
            } else {
                reinit_ui = true;
                goto rescan_directory;
            }
        }
handle_a:
        if ((keys_pressed & WS_KEY_A) && config.count) {
            REMEMBER_OFFSET();

            uint8_t special = ui_file_selector_special_list(config.offset);
            if (special != FAVORITES_LIST_NONE) {
                ui_file_selector_open_list(special, strbuf);
                reinit_ui = true;
                reinit_dirs = true;
                goto rescan_directory;
            }

            file_selector_entry_t __far *fno = ui_file_selector_open_fno(config.offset);
            if (fno->fno.fattrib & AM_DIR) {
                strncpy(strbuf, fno->fno.fname, sizeof(fno->fno.fname));
                path_depth[++path_depth_pos] = 0;
                f_chdir(strbuf);
                reinit_ui = true;
                reinit_dirs = true;
                goto rescan_directory;
            } else {
                ui_selector_clear_selection(&config);
                const char *ext = ui_file_selector_prepare_entry(strbuf, fno);
                if (!ui_file_selector_choose_save(strbuf, ext)) {
                    reinit_ui = true;
                    goto rescan_directory;
                }
                ui_file_selector_note_recent(strbuf, ext);
                if (ext != NULL && !strcasecmp(ext, s_file_ext_bfb)) {
                    ui_selector_clear_selection(&config);
                    int option = ui_file_selector_actions_bfb();
                    if (option == 0) {
                        ui_dialog_error_check(launch_bfb(strbuf), NULL, 0);
                    }
                    reinit_ui = true;
                    goto rescan_directory;
                } else {
                    ui_file_selector_track_position(config.offset, config.count);
                    int16_t result = launch_router_file(strbuf, ext, 0);
                    ui_file_selector_apply_menu_music(strbuf);
                    // VGM player: next/previous/random track (keys or end of track)
                    while (result == PLUGIN_RESULT_NEXT_FILE || result == PLUGIN_RESULT_PREV_FILE
                        || result == PLUGIN_RESULT_RANDOM_FILE) {
                        uint16_t track = result == PLUGIN_RESULT_RANDOM_FILE
                            ? ui_file_selector_random_track(config.offset, config.count)
                            : ui_file_selector_find_track(config.offset, config.count,
                                result == PLUGIN_RESULT_NEXT_FILE);
                        if (track == 0xFFFF) {
                            result = LAUNCH_ROUTER_OK_SRAM_UNCHANGED;
                            break;
                        }
                        config.offset = track;
                        REMEMBER_OFFSET();
                        ui_file_selector_track_position(track, config.count);
                        fno = ui_file_selector_open_fno(track);
                        ext = ui_file_selector_prepare_entry(strbuf, fno);
                        result = launch_router_file(strbuf, ext, 0);
                        ui_file_selector_apply_menu_music(strbuf);
                    }
                    if (result && result != LAUNCH_ROUTER_OK_SRAM_UNCHANGED) {
                        ui_dialog_error_check(result, NULL, 0);
                    }
                    reinit_ui = true;
                    reinit_dirs = result != LAUNCH_ROUTER_OK_SRAM_UNCHANGED;
                    goto rescan_directory;
                }
            }
        }
        if ((keys_pressed & (KEY_ALEFT | KEY_ARIGHT)) && config.count) {
            // Y2/Y4: next/previous initial letter; the list on screen is
            // unchanged, so only redraw what the jump changes
            config.redraw_from = config.offset + 1;
            config.offset = ui_file_selector_letter_jump(config.offset, config.count,
                (keys_pressed & KEY_ARIGHT) != 0);
            REMEMBER_OFFSET();
            continue;
        }
        if ((keys_pressed & KEY_AUP) && config.count) {
            // Y1: add the selected file to the favorites, or remove it
            file_selector_entry_t __far *fno = ui_file_selector_open_fno(config.offset);
            if (!(fno->fno.fattrib & AM_DIR)) {
                bool ok;
                if (listing_list != FAVORITES_LIST_NONE) {
                    strncpy(status_path, fno->fno.fname, sizeof(status_path) - 1);
                    status_path[sizeof(status_path) - 1] = 0;
                    ok = true;
                } else {
                    ok = favorites_full_path(status_path, sizeof(status_path), fno->fno.fname);
                }
                bool added = false;
                int16_t result = ok ? favorites_toggle(status_path, &added) : FR_INVALID_NAME;
                if (!ui_dialog_error_check(result, NULL, 0)) {
                    ui_draw_statusbar(lang_keys[added ? LK_UI_STATUS_FAVORITE_ADDED : LK_UI_STATUS_FAVORITE_REMOVED]);
                    for (uint8_t i = 0; i < 45; i++)
                        wait_for_vblank();
                }
                REMEMBER_OFFSET();
                if (listing_list == FAVORITES_LIST_FAVORITES || result != FR_OK) {
                    // the entry left the list (or a dialog covered it)
                    reinit_ui = result != FR_OK;
                    reinit_dirs = true;
                    goto rescan_directory;
                }
                config.redraw_from = config.offset + 1;
            }
            continue;
        }
        if ((keys_pressed & WS_KEY_B) && listing_list != FAVORITES_LIST_NONE) {
            ui_file_selector_close_list(strbuf);
            reinit_ui = true;
            reinit_dirs = true;
            goto rescan_directory;
        }
        if (keys_pressed & WS_KEY_B) {
            strcpy(strbuf, s_dotdot);
            f_chdir(strbuf);
            if (path_depth_pos) path_depth_pos--;
            reinit_ui = true;
            reinit_dirs = true;
            goto rescan_directory;
        }

        // TODO: temporary
        if ((keys_pressed & WS_KEY_START) && config.count) {
            REMEMBER_OFFSET();

            file_selector_entry_t __far *fno = ui_file_selector_open_fno(config.offset);
            uint8_t attrib = fno->fno.fattrib;
            ui_file_selector_prepare_entry(strbuf, fno);

            ui_selector_clear_selection(&config);
            uint8_t open_list = FAVORITES_LIST_NONE;
            if (ui_file_selector_options(strbuf, attrib, &open_list)) {
                reinit_dirs = true;
            }
            if (open_list == FILE_SELECTOR_OPTIONS_IMAGES) {
                if (ui_file_selector_view_images(config.offset, strbuf)) {
                    keys_pressed = WS_KEY_A;
                    goto handle_a;
                }
                open_list = FAVORITES_LIST_NONE;
            }
            if (open_list != FAVORITES_LIST_NONE) {
                if (listing_list != FAVORITES_LIST_NONE && open_list != listing_list)
                    ui_file_selector_close_list(strbuf); // switch lists: back to where we came from first
                if (listing_list != open_list)
                    ui_file_selector_open_list(open_list, strbuf);
                reinit_dirs = true;
            }
            reinit_ui = true;
            goto rescan_directory;
        }
    }
}
