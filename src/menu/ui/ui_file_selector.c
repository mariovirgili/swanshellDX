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
#include "ui_dialog.h"
#include "ui_selector.h"
#include "plugin/plugin.h"
#include "../../../build/menu/assets/menu/icons.h"
#include "lang.h"

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

bool ui_file_selector_default_predicate(const FILINFO __far *fno, const char __far* ext) {
    // the ROM index file is never listed
    if (!strcasecmp(fno->fname, s_file_index_name))
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
    for (int i = 0; i < file_count; i++)
        FILE_SELECTOR_INDEXES[i] = i;
    ui_file_selector_qsort(file_count, compare_filenames, (void*) settings.file_sort);
}

int16_t ui_file_selector_scan_directory(const char *path, filinfo_predicate_t predicate, uint16_t *count) {
    DIR dir;
    uint16_t file_count = 0;
    *count = 0;

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

void ui_file_selector_draw_icon(uint16_t x, uint16_t y, uint16_t icon_idx, uint16_t style) {
    uint16_t tile_idx = bitmap_rotation ? ((y >> 3) + ((x >> 3) * 18)) : ((((WS_DISPLAY_WIDTH_TILES - (style == UI_SELECTOR_STYLE_16 ? 2 : 1)) - (y >> 3)) * 18) + (x >> 3));
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

static char display_name[FF_LFN_BUF + 1];

// Name as shown in the list: without the extension when the "Show
// extensions" setting asks for it. Directories always keep their name.
static const char __far *ui_file_selector_display_name(file_selector_entry_t __far *fno) {
    uint8_t mode = settings.file_show_ext;
    if (mode == SETTING_FILE_SHOW_EXT_ALL || mode > SETTING_FILE_SHOW_EXT_NONE
        || (fno->fno.fattrib & AM_DIR) || !FILE_SELECTOR_ENTRY_HAS_EXTENSION(fno)
        || fno->extension_loc == 0)
        return fno->fno.fname;
    if (mode == SETTING_FILE_SHOW_EXT_EXCEPT_ROMS) {
        const char __far *ext = FILE_SELECTOR_ENTRY_GET_EXTENSION(fno);
        if (strcasecmp(ext, s_file_ext_ws) && strcasecmp(ext, s_file_ext_wsc) && strcasecmp(ext, s_file_ext_pc2))
            return fno->fno.fname;
    }
    uint8_t len = fno->extension_loc;
    for (uint8_t i = 0; i < len; i++)
        display_name[i] = fno->fno.fname[i];
    display_name[len] = 0;
    return display_name;
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
    if (scroll_tick) {
        int width = bitmapfont_get_string_width(s, 65535);
        if (width <= max_width)
            scroll_tick = 0;
        else {
            int width_cropped = width;
            while (width_cropped > max_width) {
                width_cropped -= bitmapfont_get_char_width(wsx_utf8_decode_next(&s)) + CONFIG_FONT_CHAR_GAP;
            }
            int tick_count = (s - name) + 2;
            // pause at the end of the name before restarting (in scroll steps)
            scroll_tick %= (tick_count + 12);
            if (scroll_tick > tick_count) scroll_tick = 0;

            s = name;
            while (scroll_tick--)
                wsx_utf8_decode_next(&s);

            // The text starts a few pixels below the top of the row, so a
            // font-height rectangle can reach past the row: on the last row it
            // would overwrite the top line of the status bar. Clip it there.
            uint16_t clear_height = bitmapfont_get_font_height();
            if (y + clear_height > screen_height - 8)
                clear_height = screen_height - 8 - y;
            while (inportb(WS_DISPLAY_LINE_PORT) != (y + clear_height - 1));
            // Only the selected row scrolls: clear it with the selection
            // colour. With a wallpaper the bitmap is 4bpp, so all four planes
            // must be written (colour 0 would be transparent and the white
            // selection text would vanish over the wallpaper).
            bitmap_rect_fill(&ui_bitmap, x_offset & ~7, y, screen_width - (x_offset & ~7), clear_height,
                ui_has_wallpaper() ? BITMAP_COLOR_4BPP(2) : BITMAP_COLOR_2BPP(2));
        }
    }

    uint16_t x = x_offset + bitmapfont_draw_string(&ui_bitmap, x_offset, y, s, max_width);
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

static bool ui_file_selector_status_text(struct ui_selector_config *config, uint16_t idx, char *buf, uint16_t buf_len) {
    file_selector_entry_t __far *fno = ui_file_selector_open_fno(idx);
    if ((fno->fno.fattrib & AM_DIR) || !FILE_SELECTOR_ENTRY_HAS_EXTENSION(fno))
        return false;
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
    status_append(buf, p, end, lang_keys[(meta.footer.flags & 1) ? LK_ROM_INFO_VERTICAL : LK_ROM_INFO_HORIZONTAL]);
    return true;
}

/* === ROM index (SWANSHELL.IDX) ===
 *
 * Header, then the entries exactly as stored in SRAM by the directory scan
 * (one slot of 1 << FILE_SELECTOR_ENTRY_SHIFT bytes each, unsorted), so that
 * loading is a single sequential read. Sorting is applied after loading.
 */

#define FILE_INDEX_VERSION 1
#define FILE_INDEX_FILTER_FLAGS (SETTING_FILE_SHOW_HIDDEN | SETTING_FILE_SHOW_SAVES)

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

// Width of "...", in pixels.
#define DOT3_WIDTH 6

void ui_file_selector(void) {
    char strbuf[FF_LFN_BUF + 1];

    ui_selector_config_t config = {0};
    bool reinit_ui = false;
    bool reinit_dirs = true;

    uint16_t path_depth[CONFIG_FILESELECT_PATH_MEMORY_DEPTH] = {0};
    uint8_t path_depth_pos = 255;

rescan_directory:
    config.draw = ui_file_selector_draw;
    config.status_text = ui_file_selector_status_text;
    config.key_mask = WS_KEY_A | WS_KEY_B | WS_KEY_START;
    config.style = settings.file_view;

    if (reinit_ui) {
        ui_layout_bars();
    }
    ui_draw_titlebar(NULL);
    ui_draw_statusbar(lang_keys[LK_UI_STATUS_LOADING]);
    ui_show();
    if (!reinit_ui && ui_has_wallpaper()) {
        ui_layout_bars();
    }
    if (reinit_ui || reinit_dirs) {
        char *path = strbuf + 3;
        f_getcwd(path, sizeof(strbuf) - 4);

        // "/" = depth 0
        // "/a" = depth 1
        // "/a/b" = depth 2
        path_depth_pos = (path[0] != 0 && path[1] != 0) ? 1 : 0;

        bitmapfont_set_active_font(font8_bitmap);
        char *draw_path = path;
        int16_t draw_path_len = bitmapfont_get_string_width(draw_path, screen_width + DOT3_WIDTH) - DOT3_WIDTH;
        char *p = path + 1;
        while (*p) {
            if (*p == '/') {
                if (draw_path_len > screen_width - 4 - DOT3_WIDTH) {
                    draw_path = p;
                    draw_path_len = bitmapfont_get_string_width(draw_path, screen_width);
                }
                path_depth_pos++;
            }
            p++;
        }
        while (draw_path_len > screen_width - 4 - DOT3_WIDTH) {
            draw_path++;
            draw_path_len = bitmapfont_get_string_width(draw_path, screen_width);
        }
        if (draw_path != path) {
            *(--draw_path) = '.';
            *(--draw_path) = '.';
            *(--draw_path) = '.';
        }
        ui_draw_titlebar(draw_path);
    }
    if (reinit_dirs) {
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
    }
    if (listing_from_index)
        ui_draw_titlebar_right(lang_keys[LK_UI_FILE_SELECTOR_INDEX_TAG]);
    reinit_ui = false;
    reinit_dirs = false;

    while (true) {
        uint16_t keys_pressed = ui_selector(&config);

        if (keys_pressed == UI_SELECTOR_RELOAD_REQUESTED) {
            reinit_ui = true;
            reinit_dirs = true;
            path_depth[path_depth_pos] = config.offset;
            goto rescan_directory;
        }
        if ((keys_pressed & WS_KEY_A) && config.count) {
            path_depth[path_depth_pos] = config.offset;

            file_selector_entry_t __far *fno = ui_file_selector_open_fno(config.offset);
            strncpy(strbuf, fno->fno.fname, sizeof(fno->fno.fname));
            if (fno->fno.fattrib & AM_DIR) {
                path_depth[++path_depth_pos] = 0;
                f_chdir(strbuf);
                reinit_ui = true;
                reinit_dirs = true;
                goto rescan_directory;
            } else {
                ui_selector_clear_selection(&config);
                const char *ext = strbuf + fno->extension_loc;
                if (!strcasecmp(ext, s_file_ext_bfb)) {
                    ui_selector_clear_selection(&config);
                    int option = ui_file_selector_actions_bfb();
                    if (option == 0) {
                        ui_dialog_error_check(launch_bfb(strbuf), NULL, 0);
                    }
                    reinit_ui = true;
                    goto rescan_directory;
                } else {
                    int16_t result = launch_router_file(strbuf, ext, 0);
                    // VGM player: right/left skips to the next/previous track
                    while (result == PLUGIN_RESULT_NEXT_FILE || result == PLUGIN_RESULT_PREV_FILE) {
                        uint16_t track = ui_file_selector_find_track(config.offset, config.count,
                            result == PLUGIN_RESULT_NEXT_FILE);
                        if (track == 0xFFFF) {
                            result = LAUNCH_ROUTER_OK_SRAM_UNCHANGED;
                            break;
                        }
                        config.offset = track;
                        path_depth[path_depth_pos] = track;
                        fno = ui_file_selector_open_fno(track);
                        strncpy(strbuf, fno->fno.fname, sizeof(fno->fno.fname));
                        ext = strbuf + fno->extension_loc;
                        result = launch_router_file(strbuf, ext, 0);
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
            path_depth[path_depth_pos] = config.offset;

            file_selector_entry_t __far *fno = ui_file_selector_open_fno(config.offset);

            ui_selector_clear_selection(&config);
            if (ui_file_selector_options(fno->fno.fname, fno->fno.fattrib)) {
                reinit_dirs = true;
            }
            reinit_ui = true;
            goto rescan_directory;
        }
    }
}
