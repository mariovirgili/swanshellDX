/**
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
#include <stdint.h>
#include <string.h>
#include <ws.h>
#include <nilefs.h>
#include "config.h"
#include "lang.h"
#include "lang_gen.h"
#include "main.h"
#include "settings.h"
#include "strings.h"
#include "ui.h"
#include "ui_file_selector.h"
#include "ui_wallpaper.h"
#include "util/bmp.h"
#include "util/input.h"

#ifdef CONFIG_ENABLE_WALLPAPER

// Entries: "None", "WALLPAPER.BMP" (if valid), then the valid files of
// /NILESWAN/WALLPAPERS (sorted, in the file selector's listing).
static uint16_t entry_count, file_count;
static bool has_default;

#define ROW_HEIGHT 8
#define LIST_Y 8
#define SETTLE_FRAMES 10

static bool wallpaper_file_ok(const char *path) {
    FIL fp;
    bmp_header_t hdr;
    unsigned int br;
    if (f_open(&fp, path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
        return false;
    bool ok = f_read(&fp, &hdr, sizeof(hdr), &br) == FR_OK && br == sizeof(hdr)
        && ui_wallpaper_header_ok(&hdr, f_size(&fp));
    f_close(&fp);
    return ok;
}

static bool wallpaper_predicate(const FILINFO __far *fno, const char __far *ext) {
    if (fno->fattrib & (AM_DIR | AM_HID | AM_SYS))
        return false;
    if (ext == NULL || strcasecmp(ext, s_file_ext_bmp))
        return false;
    if (fno->fsize > 65535 || strlen(fno->fname) >= SETTING_WALLPAPER_NAME_MAX)
        return false;
    return true;
}

// Scan the folder, then drop the files that are not valid wallpapers.
static void wallpaper_scan(char *buf) {
    uint16_t count = 0;
    strcpy(buf, s_path_wallpapers_dir);
    if (ui_file_selector_scan_directory(buf, wallpaper_predicate, &count) != FR_OK)
        count = 0;

    file_count = 0;
    for (uint16_t i = 0; i < count; i++) {
        file_selector_entry_t __far *fno = ui_file_selector_open_fno(i);
        strcpy(buf, s_path_wallpapers_dir);
        uint16_t n = strlen(buf);
        buf[n++] = '/';
        const char __far *src = fno->fno.fname;
        while (*src && n < UI_WALLPAPER_PATH_MAX - 1)
            buf[n++] = *(src++);
        buf[n] = 0;
        if (wallpaper_file_ok(buf)) {
            // keep it: move its index down over the dropped ones
            outportw(WS_CART_EXTBANK_RAM_PORT, FILE_SELECTOR_INDEX_BANK);
            FILE_SELECTOR_MEMORY_BARRIER();
            FILE_SELECTOR_INDEXES[file_count++] = FILE_SELECTOR_INDEXES[i];
        }
    }

    strcpy(buf, s_path_wallpaper_bmp);
    has_default = wallpaper_file_ok(buf);
    entry_count = 1 + (has_default ? 1 : 0) + file_count;
}

// The value stored in settings.wallpaper for an entry.
static void entry_value(uint16_t idx, char *buf) {
    if (idx == 0) {
        buf[0] = '-';
        buf[1] = 0;
    } else if (has_default && idx == 1) {
        buf[0] = 0;
    } else {
        file_selector_entry_t __far *fno = ui_file_selector_open_fno(idx - 1 - (has_default ? 1 : 0));
        strncpy(buf, fno->fno.fname, SETTING_WALLPAPER_NAME_MAX - 1);
        buf[SETTING_WALLPAPER_NAME_MAX - 1] = 0;
    }
}

static uint16_t entry_of_setting(char *buf) {
    for (uint16_t i = 0; i < entry_count; i++) {
        entry_value(i, buf);
        if (!strcasecmp(buf, settings.wallpaper))
            return i;
    }
    // not found (e.g. removed): "None" if it is "-", else the first entry
    return 0;
}

static void draw_entry_name(uint16_t idx, uint16_t x, uint16_t y, uint16_t max_width) {
    if (idx == 0) {
        bitmapfont_draw_string(&ui_bitmap, x, y, lang_keys[LK_UI_WALLPAPER_NONE], max_width);
    } else if (has_default && idx == 1) {
        bitmapfont_draw_string(&ui_bitmap, x, y, lang_keys[LK_UI_WALLPAPER_DEFAULT], max_width);
    } else {
        file_selector_entry_t __far *fno = ui_file_selector_open_fno(idx - 1 - (has_default ? 1 : 0));
        bitmapfont_draw_string(&ui_bitmap, x, y, fno->fno.fname, max_width);
    }
}

// The list on the left half; the wallpaper shows through on the right
// (or everywhere, with the list hidden).
static void draw_screen(uint16_t selected, bool list_hidden) {
    uint16_t panel_width = (screen_width >> 1) & ~7;
    uint16_t rows = (screen_height - 16) / ROW_HEIGHT;
    uint16_t page_start = (selected / rows) * rows;

    ui_layout_bars();
    ui_draw_titlebar(lang_keys[LK_SETTINGS_WALLPAPER_KEY]);
    ui_draw_statusbar(lang_keys[list_hidden ? LK_UI_WALLPAPER_CONTROLS_FULL
        : (bitmap_rotation ? LK_UI_WALLPAPER_CONTROLS : LK_UI_WALLPAPER_CONTROLS_SHORT)]);
    if (list_hidden)
        return;

    bitmap_rect_fill(&ui_bitmap, 0, LIST_Y, panel_width, screen_height - 16, BITMAP_COLOR_4BPP(2));
    bitmapfont_set_active_font(font8_bitmap);
    for (uint16_t r = 0; r < rows && page_start + r < entry_count; r++) {
        uint16_t idx = page_start + r;
        draw_entry_name(idx, 3, LIST_Y + r * ROW_HEIGHT, panel_width - 5);
        if (idx == selected) {
            // selected row: the "selected" tile palette, as in the file list
            ui_screen_modify_tiles(bitmap_screen2, ~WS_SCREEN_ATTR_PALETTE_MASK, WS_SCREEN_ATTR_PALETTE(1),
                0, (LIST_Y + r * ROW_HEIGHT) >> 3, panel_width >> 3, ROW_HEIGHT >> 3);
        }
    }
}

static void preview(uint16_t idx, char *buf) {
    entry_value(idx, buf);
    strcpy(settings.wallpaper, buf);
    ui_reload_wallpaper();
}

void ui_wallpaper_picker(void) {
    char buf[UI_WALLPAPER_PATH_MAX];
    char original[SETTING_WALLPAPER_NAME_MAX];
    memcpy(original, settings.wallpaper, sizeof(original));

    ui_draw_statusbar(lang_keys[LK_UI_STATUS_LOADING]);
    wallpaper_scan(buf);
    uint16_t selected = entry_of_setting(buf);
    uint16_t previewed = selected;
    uint16_t settle = 0;
    bool list_hidden = false;
    bool chosen = false;

    draw_screen(selected, list_hidden);
    input_wait_clear();

    while (true) {
        idle_until_vblank();
        input_update();
        uint16_t keys = input_pressed;
        uint16_t prev = selected;

        if (!list_hidden) {
            if ((keys & KEY_UP) && entry_count)
                selected = selected ? selected - 1 : entry_count - 1;
            if ((keys & KEY_DOWN) && entry_count)
                selected = selected + 1 < entry_count ? selected + 1 : 0;
        }
        if (keys & KEY_RIGHT) {
            list_hidden = true;
            draw_screen(selected, list_hidden);
        } else if (keys & KEY_LEFT) {
            list_hidden = false;
            draw_screen(selected, list_hidden);
        }
        if (keys & WS_KEY_A) {
            chosen = true;
            break;
        }
        if (keys & (WS_KEY_B | WS_KEY_START))
            break;

        if (selected != prev) {
            draw_screen(selected, list_hidden);
            settle = SETTLE_FRAMES;
        }
        // load the preview once the selection has stopped moving
        if (settle && !--settle && selected != previewed) {
            preview(selected, buf);
            previewed = selected;
            draw_screen(selected, list_hidden);
        }
    }

    if (chosen) {
        if (previewed != selected)
            preview(selected, buf);
    } else if (memcmp(original, settings.wallpaper, sizeof(original))) {
        memcpy(settings.wallpaper, original, sizeof(original));
        ui_reload_wallpaper();
    }
    input_wait_clear();
    ui_layout_bars();
}

#endif /* CONFIG_ENABLE_WALLPAPER */
