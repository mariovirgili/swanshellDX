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
#include "ui_icons.h"
#include "ui_selector.h"
#include "util/asset_heap.h"
#include "util/file.h"
#include "util/input.h"
#include "../../../build/menu/assets/menu/icons.h"

#define ICONS_PALETTE_OFFSET 8
#define ICONS_PALETTE_COLORS 12

static uint8_t icons_bank = 0;      // PSRAM bank of the set (0: none allocated)
static uint8_t icons_bank_check = 0xA5; // icons_bank ^ 0xA5: detects a damaged variable
static bool icons_custom = false;   // a set from the card is loaded in it
static bool icons_data_lost = false; // the set's data was overwritten: reload it

static bool icons_magic_ok(void);

// Is a set from the card in use? Checked before each use: if its memory
// was damaged, the built-in icons are used instead of garbage.
bool ui_icons_custom(void) {
    if (!icons_custom)
        return false;
    if ((uint8_t) (icons_bank ^ 0xA5) == icons_bank_check) {
        if (icons_magic_ok())
            return true;
        icons_data_lost = true;
    }
    icons_custom = false;
    ui_icons_apply_palette();
    return false;
}

// After the data was overwritten: load the set again (the bank is fine).
void ui_icons_recover(void) {
    if (!icons_custom && icons_data_lost && settings.icons[0]) {
        icons_data_lost = false;
        ui_icons_load(settings.icons);
    }
}

void ui_icons_read(void __far *dest, uint16_t offset, uint16_t length) {
    uint16_t prev_bank = inportw(WS_CART_EXTBANK_RAM_PORT);
    FILE_SELECTOR_MEMORY_BARRIER();
    ws_bank_with_flash(WS_CART_BANK_FLASH_ENABLE, {
        outportw(WS_CART_EXTBANK_RAM_PORT, icons_bank);
        FILE_SELECTOR_MEMORY_BARRIER();
        _fmemcpy(dest, MK_FP(0x1000, offset), length);
        FILE_SELECTOR_MEMORY_BARRIER();
        outportw(WS_CART_EXTBANK_RAM_PORT, prev_bank);
    });
    FILE_SELECTOR_MEMORY_BARRIER();
}

static bool icons_magic_ok(void) {
    char magic[4];
    ui_icons_read(magic, 0, 4);
    return magic[0] == 'W' && magic[1] == 'S' && magic[2] == 'I' && magic[3] == 'C';
}

void ui_icons_apply_palette(void) {
    if (!ws_system_is_color_active())
        return;
    if (icons_custom) {
        uint16_t pal[ICONS_PALETTE_COLORS];
        ui_icons_read(pal, ICONS_PALETTE_OFFSET, sizeof(pal));
        for (uint8_t i = 0; i < ICONS_PALETTE_COLORS; i++) {
            WS_DISPLAY_COLOR_MEM(0)[4 + i] = pal[i] & 0xFFF;
            WS_DISPLAY_COLOR_MEM(1)[4 + i] = pal[i] & 0xFFF;
        }
    } else {
        // the built-in colors, as set up by ui_init()
        ws_gdma_copyp(WS_DISPLAY_COLOR_MEM(0) + 4, gfx_icons_palcolor + 8, gfx_icons_palcolor_size - 8);
        ws_gdma_copyp(WS_DISPLAY_COLOR_MEM(1) + 4, gfx_icons_palcolor + 8, gfx_icons_palcolor_size - 8);
    }
}

// "/NILESWAN/ICONS/<name>"
static void icons_path(char *buf, const char __far *name) {
    strcpy(buf, s_path_icons_dir);
    uint16_t n = strlen(buf);
    buf[n++] = '/';
    while (*name && n < UI_ICONS_PATH_MAX - 1)
        buf[n++] = *(name++);
    buf[n] = 0;
}

static bool icons_file_ok(FIL *fp) {
    uint8_t hdr[8];
    unsigned int br;
    return f_size(fp) == UI_ICONS_FILE_SIZE
        && f_read(fp, hdr, sizeof(hdr), &br) == FR_OK && br == sizeof(hdr)
        && hdr[0] == 'W' && hdr[1] == 'S' && hdr[2] == 'I' && hdr[3] == 'C' && hdr[4] == 1;
}

static bool icons_path_ok(const char *path) {
    FIL fp;
    if (f_open(&fp, path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
        return false;
    bool ok = icons_file_ok(&fp);
    f_close(&fp);
    return ok;
}

bool ui_icons_load(const char __far *name) {
    bool ok = !name[0];
    icons_custom = false;
    if (name[0] && icons_bank && (uint8_t) (icons_bank ^ 0xA5) == icons_bank_check) {
        char path[UI_ICONS_PATH_MAX];
        FIL fp;
        icons_path(path, name);
        if (f_open(&fp, path, FA_READ | FA_OPEN_EXISTING) == FR_OK) {
            if (icons_file_ok(&fp) && f_lseek(&fp, 0) == FR_OK
                && f_read_rom_banked(&fp, icons_bank, UI_ICONS_FILE_SIZE, NULL, NULL) == FR_OK) {
                icons_custom = true;
                ok = true;
            }
            f_close(&fp);
        }
    }
    ui_icons_apply_palette();
    return ok;
}

void ui_icons_init(void) {
    if (!icons_bank) {
        icons_bank = asset_heap_alloc_banks(1);
        icons_bank_check = icons_bank ^ 0xA5;
    }
    ui_icons_load(settings.icons);
}

/* === Picker === */

// Entries: the built-in icons, then the valid sets of /NILESWAN/ICONS
// (sorted, in the file selector's listing).
static uint16_t entry_count;

#define ROW_HEIGHT 8
#define LIST_Y 8
#define SETTLE_FRAMES 10

static bool icons_predicate(const FILINFO __far *fno, const char __far *ext) {
    if (fno->fattrib & (AM_DIR | AM_HID | AM_SYS))
        return false;
    if (ext == NULL || strcasecmp(ext, s_file_ext_icn))
        return false;
    return fno->fsize == UI_ICONS_FILE_SIZE && strlen(fno->fname) < SETTING_ICONS_NAME_MAX;
}

static void icons_scan(char *buf) {
    uint16_t count = 0, kept = 0;
    strcpy(buf, s_path_icons_dir);
    if (ui_file_selector_scan_directory(buf, icons_predicate, &count) != FR_OK)
        count = 0;
    for (uint16_t i = 0; i < count; i++) {
        file_selector_entry_t __far *fno = ui_file_selector_open_fno(i);
        icons_path(buf, fno->fno.fname);
        if (icons_path_ok(buf)) {
            outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
            outportw(WS_CART_EXTBANK_RAM_PORT, FILE_SELECTOR_INDEX_BANK);
            FILE_SELECTOR_MEMORY_BARRIER();
            FILE_SELECTOR_INDEXES[kept++] = FILE_SELECTOR_INDEXES[i];
        }
    }
    entry_count = 1 + kept;
}

// The value stored in settings.icons for an entry ("" = built-in).
static void entry_value(uint16_t idx, char *buf) {
    buf[0] = 0;
    if (idx > 0) {
        file_selector_entry_t __far *fno = ui_file_selector_open_fno(idx - 1);
        strncpy(buf, fno->fno.fname, SETTING_ICONS_NAME_MAX - 1);
        buf[SETTING_ICONS_NAME_MAX - 1] = 0;
    }
}

static uint16_t entry_of_setting(char *buf) {
    for (uint16_t i = 1; i < entry_count; i++) {
        entry_value(i, buf);
        if (!strcasecmp(buf, settings.icons))
            return i;
    }
    return 0;
}

static void draw_entry_name(uint16_t idx, uint16_t x, uint16_t y, uint16_t max_width) {
    if (idx == 0) {
        bitmapfont_draw_string(&ui_bitmap, x, y, lang_keys[LK_UI_ICONS_BUILTIN], max_width);
    } else {
        // the name without ".icn"
        file_selector_entry_t __far *fno = ui_file_selector_open_fno(idx - 1);
        char name[SETTING_ICONS_NAME_MAX];
        uint16_t n = 0;
        while (fno->fno.fname[n] && n < SETTING_ICONS_NAME_MAX - 1) {
            name[n] = fno->fno.fname[n];
            n++;
        }
        name[n] = 0;
        if (n > 4 && name[n - 4] == '.')
            name[n - 4] = 0;
        bitmapfont_draw_string(&ui_bitmap, x, y, name, max_width);
    }
}

// The 10 icons of the set in use: 16x16, 16x16 on a selected row, 8x8.
static void draw_preview(uint16_t x0, uint16_t y0, uint16_t width, uint16_t height) {
    uint16_t cols16 = width / 24, cols8 = width / 16;
    if (!cols16) return;
    uint16_t rows16 = (10 + cols16 - 1) / cols16;
    uint16_t y = y0 + 8;

    bitmap_rect_fill(&ui_bitmap, x0, y0, width, height, BITMAP_COLOR_4BPP(2));
    for (uint16_t i = 0; i < 10; i++)
        ui_file_selector_draw_icon(x0 + 8 + (i % cols16) * 24, y + (i / cols16) * 24, i, UI_SELECTOR_STYLE_16);
    y += rows16 * 24;

    // as on the selected row of the list
    if (y + 24 <= y0 + height) {
        for (uint16_t i = 0; i < cols16; i++)
            ui_file_selector_draw_icon(x0 + 8 + i * 24, y, i, UI_SELECTOR_STYLE_16);
        ui_screen_modify_tiles(bitmap_screen2, ~WS_SCREEN_ATTR_PALETTE_MASK, WS_SCREEN_ATTR_PALETTE(1),
            x0 >> 3, y >> 3, width >> 3, 2);
        y += 24;
    }

    for (uint16_t i = 0; i < 10 && cols8; i++) {
        uint16_t iy = y + (i / cols8) * 16;
        if (iy + 8 > y0 + height) break;
        ui_file_selector_draw_icon(x0 + 8 + (i % cols8) * 16, iy, i, UI_SELECTOR_STYLE_8);
    }
}

static void draw_screen(uint16_t selected) {
    uint16_t panel_width = (screen_width >> 1) & ~7;
    uint16_t rows = (screen_height - 16) / ROW_HEIGHT;
    uint16_t page_start = (selected / rows) * rows;

    ui_layout_bars();
    ui_draw_titlebar(lang_keys[LK_SETTINGS_ICONS_KEY]);
    ui_draw_statusbar(lang_keys[bitmap_rotation ? LK_UI_ICONS_CONTROLS : LK_UI_ICONS_CONTROLS_SHORT]);

    bitmap_rect_fill(&ui_bitmap, 0, LIST_Y, panel_width, screen_height - 16, BITMAP_COLOR_4BPP(2));
    bitmapfont_set_active_font(font8_bitmap);
    for (uint16_t r = 0; r < rows && page_start + r < entry_count; r++) {
        uint16_t idx = page_start + r;
        draw_entry_name(idx, 3, LIST_Y + r * ROW_HEIGHT, panel_width - 5);
        if (idx == selected) {
            ui_screen_modify_tiles(bitmap_screen2, ~WS_SCREEN_ATTR_PALETTE_MASK, WS_SCREEN_ATTR_PALETTE(1),
                0, (LIST_Y + r * ROW_HEIGHT) >> 3, panel_width >> 3, ROW_HEIGHT >> 3);
        }
    }
    draw_preview(panel_width, LIST_Y, screen_width - panel_width, screen_height - 16);
}

static void preview(uint16_t idx, char *buf) {
    entry_value(idx, buf);
    ui_icons_load(buf);
}

void ui_icons_picker(void) {
    char buf[UI_ICONS_PATH_MAX];

    ui_draw_statusbar(lang_keys[LK_UI_STATUS_LOADING]);
    icons_scan(buf);
    uint16_t selected = entry_of_setting(buf);
    uint16_t previewed = 0xFFFF;
    uint16_t settle = 1;
    bool chosen = false;

    draw_screen(selected);
    input_wait_clear();

    while (true) {
        idle_until_vblank();
        input_update();
        uint16_t keys = input_pressed;
        uint16_t prev = selected;

        if (keys & KEY_UP)
            selected = selected ? selected - 1 : entry_count - 1;
        if (keys & KEY_DOWN)
            selected = selected + 1 < entry_count ? selected + 1 : 0;
        if (keys & WS_KEY_A) {
            chosen = true;
            break;
        }
        if (keys & (WS_KEY_B | WS_KEY_START))
            break;

        if (selected != prev) {
            draw_screen(selected);
            settle = SETTLE_FRAMES;
        }
        // load the set once the selection has stopped moving
        if (settle && !--settle && selected != previewed) {
            preview(selected, buf);
            previewed = selected;
            draw_screen(selected);
        }
    }

    if (chosen) {
        entry_value(selected, buf);
        strcpy(settings.icons, buf);
    }
    // the chosen set, or back to the one in use
    if (!ui_icons_load(settings.icons))
        settings.icons[0] = 0;
    input_wait_clear();
    ui_layout_bars();
}
