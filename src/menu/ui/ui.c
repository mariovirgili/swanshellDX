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
#include <string.h>
#include <ws.h>
#include <ws/display.h>
#include <wsx/planar_convert.h>
#include "bitmap.h"
#include "cart/status.h"
#include "strings.h"
#include "ui.h"
#include "util/bmp.h"
#include "util/file.h"
#include "fs.h"
#include "settings.h"
#include "../../../build/menu/assets/menu/bar_icons.h"
#include "ui_icons.h"
#include "../../../build/menu/assets/menu/icons.h"
#include "../../../build/menu/assets/menu/screen_patterns.h"
#include "lang.h"
#include "config.h"

__attribute__((section(".iramx_1b88")))
uint16_t bitmap_screen2[32 * 18 - 4];

__attribute__((section(".iramx_2000")))
ws_display_tile_t bitmap_tiles[512];
#define bitmap_screen1 ((uint16_t ws_iram*) 0x3800)

#ifdef CONFIG_ENABLE_WALLPAPER
static bool thumb_active = false; // the thumbnail popup is shown
static bool thumb_saved = false;  // the wallpaper under it is in the scratch bank
#endif

__attribute__((section(".iramCx_4000"), retain))
ws_display_tile_4bpp_t bitmap_tiles_c1[512];
__attribute__((section(".iramCx_8000"), retain))
ws_display_tile_4bpp_t bitmap_tiles_c2[512];

bitmap_t ui_bitmap;
static uint8_t icons_visible;

void ui_draw_titlebar(const char __far* text) {
    bitmap_rect_fill(&ui_bitmap, 0, 0, screen_width, 8, BITMAP_COLOR_2BPP(2));
    if (text != NULL) {
        bitmapfont_set_active_font(font8_bitmap);
        bitmapfont_draw_string(&ui_bitmap, 2, 0, text, screen_width - 4);
    }
}

void ui_draw_titlebar_right(const char __far* text) {
    bitmapfont_set_active_font(font8_bitmap);
    uint16_t width = bitmapfont_get_string_width(text, screen_width);
    // clear behind the text, in case a long path runs underneath
    bitmap_rect_fill(&ui_bitmap, screen_width - width - 6, 0, width + 6, 8, BITMAP_COLOR_2BPP(2));
    bitmapfont_draw_string(&ui_bitmap, screen_width - 2 - width, 0, text, screen_width);
}

static void ui_draw_icon(int x, int idx) {
    const uint8_t __far* src;
    uint8_t *dest;
    if (!bitmap_rotation) {
        src = &gfx_bar_icons_rot[idx * 8];
        dest = ((uint8_t*) ui_bitmap.start) + (ui_bitmap.y_pitch * x * 8);
    } else {
        src = &gfx_bar_icons[idx * 8];
        dest = ((uint8_t*) ui_bitmap.start) + (ui_bitmap.x_pitch * x) + (ui_bitmap.y_pitch * (screen_height - 8));
    }
    if (ui_bitmap.bpp == 4) {
        dest[0x00] = src[0x0];
        dest[0x04] = src[0x1];
        dest[0x08] = src[0x2];
        dest[0x0c] = src[0x3];
        dest[0x10] = src[0x4];
        dest[0x14] = src[0x5];
        dest[0x18] = src[0x6];
        dest[0x1c] = src[0x7];
    } else {
        dest[0x00] = src[0x0];
        dest[0x02] = src[0x1];
        dest[0x04] = src[0x2];
        dest[0x06] = src[0x3];
        dest[0x08] = src[0x4];
        dest[0x0a] = src[0x5];
        dest[0x0c] = src[0x6];
        dest[0x0e] = src[0x7];
    }
}

uint16_t ui_icon_update(void) {
    if (!icons_visible) return screen_width;
    uint16_t prev_icon_pos = icons_visible;
    uint16_t icon_pos = (screen_width >> 3);
    bool mcu_present = cart_status.present & CART_PRESENT_MCU;
    bool mcu_info_ok = cart_status.present & CART_PRESENT_MCU_INFO_OK;
    bool mcu_data_valid = mcu_present && cart_status.version >= CART_FW_VERSION_1_1_0 && mcu_info_ok;

    if (!mcu_present) {
        ui_draw_icon(--icon_pos, UI_BAR_ICON_MCU_ERROR);
    }

    if (mcu_data_valid) {
        if (cart_status.mcu_info.status & NILE_MCU_NATIVE_INFO_BATTERY_OK) {
            ui_draw_icon(--icon_pos, UI_BAR_ICON_BATTERY_3);
        } else {
            ui_draw_icon(--icon_pos, UI_BAR_ICON_BATTERY_NONE);
        }
    }

    if (!(inportb(WS_SYSTEM_CTRL_PORT) & WS_SYSTEM_CTRL_IPL_LOCK))
        ui_draw_icon(--icon_pos, UI_BAR_ICON_BOOTROM_UNLOCK);

    if (mcu_data_valid) {
        if (cart_status.mcu_info.status & NILE_MCU_NATIVE_INFO_USB_CONNECT) {
            ui_draw_icon(--icon_pos, UI_BAR_ICON_USB_CONNECT);
        } else if (cart_status.mcu_info.status & NILE_MCU_NATIVE_INFO_USB_DETECT) {
            ui_draw_icon(--icon_pos, UI_BAR_ICON_USB_DETECT);
        }
    } else if (cart_status.present & CART_PRESENT_MCU_INFO_ERROR) {
        ui_draw_icon(--icon_pos, UI_BAR_ICON_MCU_ERROR);
    }

    if (prev_icon_pos < icon_pos) {
        for (int i = prev_icon_pos; i < icon_pos; i++)
            ui_draw_icon(i, UI_BAR_ICON_NONE);
    }
    icons_visible = icon_pos;

    return icon_pos * 8;
}

void ui_hide_icons(void) {
    icons_visible = 0;
}

void ui_draw_statusbar_lr(const char __far* text, const char __far* right_text) {
    icons_visible = right_text == NULL ? (screen_width >> 3) : 0;
    uint16_t icon_end = ui_icon_update();
    bitmap_rect_fill(&ui_bitmap, 0, screen_height-8, icon_end, 8, BITMAP_COLOR_2BPP(2));
    if (icon_end < screen_width) {
        bitmap_rect_fill(&ui_bitmap, icon_end, screen_height-8, screen_width-icon_end, 8, BITMAP_COLOR(2, 2, BITMAP_COLOR_MODE_STORE));
    }
    bitmapfont_set_active_font(font8_bitmap);
    if (text != NULL) {
        bitmapfont_draw_string(&ui_bitmap, 2, screen_height-8, text, icon_end - 4);
    }
    if (right_text != NULL) {
        bitmapfont_draw_string(&ui_bitmap, screen_width - 2 - bitmapfont_get_string_width(right_text, screen_width), screen_height-8, right_text, screen_width);
    }
}

void ui_draw_statusbar(const char __far* text) {
    ui_draw_statusbar_lr(text, NULL);
}

#define INIT_SCREEN_PATTERN(screen_loc, pal, ofs) \
    { \
        int ip = (ofs); \
        for (int ix = 0; ix < WS_DISPLAY_WIDTH_TILES; ix++) { \
            for (int iy = 0; iy < WS_DISPLAY_HEIGHT_TILES; iy++) { \
                ws_screen_put_tile(screen_loc, (pal) | (ip++), ix, iy); \
            } \
        } \
    }

void ui_init(void) {
    ui_hide();
#ifdef CONFIG_ENABLE_WALLPAPER
    thumb_active = false; // (ui_hide() turned the window off)
    thumb_saved = false;
#endif
    icons_visible = 0;

    // initialize palettes
#ifdef CONFIG_DEBUG_FORCE_MONO
    if (0) {
#else
    if (ws_system_is_color_model()) {
#endif
        ws_system_set_mode(WS_MODE_COLOR_4BPP);
        ui_bitmap = BITMAP(WS_TILE_4BPP_MEM(0), WS_DISPLAY_WIDTH_TILES, WS_DISPLAY_HEIGHT_TILES, 4);

        // palette 0 - icon palette
        WS_DISPLAY_COLOR_MEM(0)[0] = 0xFFF;
        ws_gdma_copyp(WS_DISPLAY_COLOR_MEM(0) + 2, gfx_icons_palcolor + 4, gfx_icons_palcolor_size - 4);
        WS_DISPLAY_COLOR_MEM(0)[1] = 0x000;

        // palette 1 - icon palette (selected)
        ws_gdma_copyp(WS_DISPLAY_COLOR_MEM(1) + 2, gfx_icons_palcolor + 4, gfx_icons_palcolor_size - 4);
        WS_DISPLAY_COLOR_MEM(1)[1] = 0xFFF;
        WS_DISPLAY_COLOR_MEM(1)[2] ^= 0xFFF;
        WS_DISPLAY_COLOR_MEM(1)[3] ^= 0xFFF;
        // colors 4..15: those of the icon set in use
        ui_icons_apply_palette();

        // palette 2 - titlebar palette
        WS_DISPLAY_COLOR_MEM(2)[0] = 0xFFF;
        WS_DISPLAY_COLOR_MEM(2)[1] = 0x000;
        WS_DISPLAY_COLOR_MEM(2)[2] = SETTING_THEME_ACCENT_COLOR_DEFAULT;
        WS_DISPLAY_COLOR_MEM(2)[3] = 0xFFF;
    } else {
        ui_bitmap = BITMAP(WS_TILE_MEM(0), WS_DISPLAY_WIDTH_TILES, WS_DISPLAY_HEIGHT_TILES, 2);

        // TODO: Add gamma configuration option.
        //
        // STN (color correction):
        // WS_DISPLAY_SHADE_LUT(0, 5, 8, 11, 12, 13, 14, 15)
        //
        // IPS (no color correction):
        // WS_DISPLAY_SHADE_LUT(0, 2, 4, 6, 9, 11, 13, 15)
        ws_display_set_shade_lut(WS_DISPLAY_SHADE_LUT(0, 3, 5, 7, 9, 11, 13, 15));
        // palette 0 - icon palette
        outportw(WS_SCR_PAL_0_PORT, 0x7052);
        // palette 1 - icon palette (selected)
        outportw(WS_SCR_PAL_1_PORT, 0x0725);
        // palette 2 - titlebar palette
        outportw(WS_SCR_PAL_2_PORT, 0x0370);
    }

    outportb(WS_SCR_BASE_PORT, WS_SCR_BASE_ADDR1(0x3800) | WS_SCR_BASE_ADDR2(bitmap_screen2));
    outportw(WS_SCR2_SCRL_X_PORT, ((14*8) << 8) + (4*8));
}

void ui_hide(void) {
    outportw(WS_DISPLAY_CTRL_PORT, 0);
}

#ifdef CONFIG_ENABLE_WALLPAPER
// 0 - not checked, 2 - checked, not found; 1 - checked, found
static uint8_t wallpaper_status = 0;

bool ui_has_wallpaper(void) {
    return wallpaper_status == 1;
}
#endif

__attribute__((always_inline))
static inline void ui_show_inner(void) {
    uint8_t ctrl = inportb(WS_DISPLAY_CTRL_PORT) & ~0x3;
#ifdef CONFIG_ENABLE_WALLPAPER
    outportb(WS_DISPLAY_CTRL_PORT, ctrl | WS_DISPLAY_CTRL_SCR2_ENABLE | (wallpaper_status & 1));
#else
    outportb(WS_DISPLAY_CTRL_PORT, ctrl | WS_DISPLAY_CTRL_SCR2_ENABLE);
#endif
}

#ifdef CONFIG_ENABLE_WALLPAPER
bool ui_wallpaper_header_ok(const bmp_header_t __far *bmp, uint32_t file_size) {
    if (file_size > 65535 || file_size < sizeof(bmp_header_t))
        return false;
    if (bmp->magic != BMP_MAGIC || bmp->header_size < BMP_MIN_HEADER_SIZE ||
        // the wallpaper layer is not rotated: always the LCD size
        bmp->width != WS_DISPLAY_WIDTH_PIXELS || bmp->height != WS_DISPLAY_HEIGHT_PIXELS ||
        bmp->compression != 0)
        return false;
    // 16 colors, or 256 colors in the native tile-palette layout
    uint16_t palette_bytes;
    if (bmp->bpp == 4)
        palette_bytes = 16 * 4;
    else if (bmp->bpp == 8 && bmp->res0 == BMP_WS_NATIVE_RES0 && bmp->res1 == BMP_WS_NATIVE_RES1)
        palette_bytes = 256 * 4;
    else
        return false;
    // palette and pixel data inside the file
    uint32_t pitch = (((uint32_t) bmp->width * bmp->bpp + 31) / 32) << 2;
    return 14 + bmp->header_size + palette_bytes <= bmp->data_start
        && bmp->data_start + pitch * bmp->height <= file_size;
}

// Path of the chosen wallpaper (settings.wallpaper), or false for none.
bool ui_wallpaper_path(char *buf, const char __far *name) {
    if (name[0] == '-')
        return false;
    if (!name[0]) {
        strcpy(buf, s_path_wallpaper_bmp);
        return true;
    }
    strcpy(buf, s_path_wallpapers_dir);
    uint16_t n = strlen(buf);
    buf[n++] = '/';
    while (*name && n < UI_WALLPAPER_PATH_MAX - 1)
        buf[n++] = *(name++);
    buf[n] = 0;
    return true;
}

static inline uint16_t bmp_color(const uint8_t __far *entry) {
    return WS_RGB(entry[2] >> 4, entry[1] >> 4, entry[0] >> 4);
}

// Native 256-color layout: palettes 3-15 and one palette per tile.
static void load_native_data(bmp_header_t __far *bmp) {
    const uint8_t __far *palette = MK_FP(0x1000, 14 + bmp->header_size);
    for (uint8_t p = 3; p < 16; p++)
        for (uint8_t c = 0; c < 16; c++)
            WS_DISPLAY_COLOR_MEM(p)[c] = bmp_color(palette + ((p << 4) + c) * 4);
    WS_DISPLAY_COLOR_MEM(0)[0] = WS_DISPLAY_COLOR_MEM(3)[0];

    const uint8_t __far *data = MK_FP(0x1000, bmp->data_start);
    uint16_t pitch = (((uint16_t) bmp->width * 8) + 31) / 32 * 4;
    uint8_t height = bmp->height;
    for (uint8_t tx = 0; tx < WS_DISPLAY_WIDTH_TILES; tx++) {
        for (uint8_t ty = 0; ty < WS_DISPLAY_HEIGHT_TILES; ty++) {
            uint16_t tile = tx * WS_DISPLAY_HEIGHT_TILES + ty;
            uint32_t *dst = (uint32_t*) (0x8000 + tile * 32);
            // rows are stored bottom-up
            const uint8_t __far *src = data + (uint16_t) (height - 1 - (ty << 3)) * pitch + (tx << 3);
            uint8_t pal = src[0] >> 4;
            if (pal < 3)
                pal = 3;
            for (uint8_t r = 0; r < 8; r++, src -= pitch) {
                uint8_t packed[4];
                for (uint8_t k = 0; k < 4; k++)
                    packed[k] = ((src[k << 1] & 15) << 4) | (src[(k << 1) + 1] & 15);
                dst[r] = wsx_planar_convert_4bpp_packed_row(*((uint32_t*) packed));
            }
            ws_screen_put_tile(bitmap_screen1, WS_SCREEN_ATTR_PALETTE(pal) | WS_SCREEN_ATTR_BANK(1) | tile, tx, ty);
        }
    }
}

static bool load_wallpaper_data(FIL *fp) {
    unsigned int br;
    int16_t result = f_read(fp, MK_FP(0x1000, 0x0000), f_size(fp), &br);
    if (result != FR_OK)
        return false;

    bmp_header_t __far* bmp = MK_FP(0x1000, 0x0000);
    if (!ui_wallpaper_header_ok(bmp, f_size(fp)))
        return false;

    if (bmp->bpp == 8) {
        load_native_data(bmp);
        return true;
    }

    uint8_t __far *palette = MK_FP(0x1000, 14 + bmp->header_size);
    for (int i = 0; i < 16; i++) {
        uint8_t b = *(palette++);
        uint8_t g = *(palette++);
        uint8_t r = *(palette++);
        palette++;
        WS_DISPLAY_COLOR_MEM(3)[i] = WS_RGB(r >> 4, g >> 4, b >> 4);
    }
    WS_DISPLAY_COLOR_MEM(0)[0] = WS_DISPLAY_COLOR_MEM(3)[0];

    uint8_t __far *data = MK_FP(0x1000, bmp->data_start);
    uint16_t pitch = (((bmp->width * bmp->bpp) + 31) / 32) << 2;
    for (uint8_t y = 0; y < bmp->height; y++, data += pitch) {
        uint32_t __far *line_src = (uint32_t __far*) data;
        uint32_t *line_dst = (uint32_t*) (0x8000 + (((uint16_t)bmp->height - 1 - y) * 4));
        for (uint8_t x = 0; x < bmp->width; x += 8, line_src++, line_dst += WS_DISPLAY_HEIGHT_TILES * 8) {
            *line_dst = wsx_planar_convert_4bpp_packed_row(*line_src);
        }
    }
    return true;
}

bool ui_load_background(const char *path) {
    FIL fp;

    wallpaper_status = 2;
    if (ws_system_get_mode() != WS_MODE_COLOR_4BPP) return false;

    int16_t result = f_open(&fp, path, FA_READ | FA_OPEN_EXISTING);
    if (result != FR_OK) return false;
    if (f_size(&fp) > 65535) {
        f_close(&fp);
        return false;
    }

    INIT_SCREEN_PATTERN(bitmap_screen1, WS_SCREEN_ATTR_PALETTE(3), WS_SCREEN_ATTR_BANK(1));
    // memset(WS_TILE_4BPP_MEM(512), 0, 28 * 18 * 32);

    wallpaper_status = 1;
    ui_show_inner();
    wallpaper_status = 2;

    bool ok;
    // (no return inside: the previous flash mode must be restored)
    // The picture is read into PSRAM bank 0, the scratch area (also used by
    // the text viewer and the VGM player): not into whatever bank is mapped,
    // which could be part of the asset heap (fonts, music, icon set).
    ws_bank_with_flash(WS_CART_BANK_FLASH_ENABLE, {
        ws_bank_with_ram(UI_SCRATCH_PSRAM_BANK, {
            ok = load_wallpaper_data(&fp);
        });
    });
    f_close(&fp);
    if (!ok) {
        WS_DISPLAY_COLOR_MEM(0)[0] = (settings.accent_color_high & SETTING_THEME_DARK_MODE) ? 0x000 : 0xFFF;
        return false;
    }

    wallpaper_status = 1;
    return true;
}

static inline void load_wallpaper(void) {
    char path[UI_WALLPAPER_PATH_MAX];

    wallpaper_status = 2;
    if (ws_system_get_mode() != WS_MODE_COLOR_4BPP) return;
    if (!ui_wallpaper_path(path, settings.wallpaper)) return;
    ui_load_background(path);
}

void ui_reload_wallpaper(void) {
    ui_unload_wallpaper();
    ui_show();
}

void ui_unload_wallpaper(void) {
    wallpaper_status = 0;
    WS_DISPLAY_COLOR_MEM(0)[0] = (settings.accent_color_high & SETTING_THEME_DARK_MODE) ? 0x000 : 0xFFF;
    ui_show_inner();
}

/* === Thumbnail popup ===
 * thumb.bmp (112x72, 16 colors) is shown in a framed box over the file list:
 * (color 0 is transparent and color 15 is the frame: pictures made for it
 * use colors 1-14, see NileSwanImages)
 * the right half of the screen in the horizontal layout, the lower half
 * (turned clockwise, so it reads upright) in the vertical one. It is drawn
 * into the wallpaper layer (screen 1: the tiles of those screen positions,
 * palette 15; a 2-pixel frame in the accent color over its edges), and the list layer
 * (screen 2) is hidden inside the box with its hardware window. Without a
 * wallpaper, screen 1 is turned on just for the box (the list layer is
 * opaque everywhere else). */
#define THUMB_W 112
#define THUMB_H 72
#define THUMB_PALETTE 15
#define THUMB_FRAME 2         // frame drawn over the outer pixels of the picture
#define THUMB_FRAME_COLOR 15  // its color index (set to the accent color)

static void thumb_geometry(uint8_t *x0, uint8_t *y0, uint8_t *w, uint8_t *h) {
    if (bitmap_rotation) {
        // horizontal: right half, vertically centered
        *x0 = 13; *y0 = 4; *w = THUMB_W / 8; *h = THUMB_H / 8;
    } else {
        // vertical: the lower half as the console is held = the left half
        // of the LCD; the picture is 72 x 112 pixels there
        *x0 = 2; *y0 = 2; *w = THUMB_H / 8; *h = THUMB_W / 8;
    }
}

bool ui_thumbnail_active(void) {
    return thumb_active;
}

static bool thumb_header_ok(const bmp_header_t __far *bmp, uint32_t file_size) {
    if (file_size > 65535 || file_size < sizeof(bmp_header_t))
        return false;
    if (bmp->magic != BMP_MAGIC || bmp->header_size < BMP_MIN_HEADER_SIZE
        || bmp->width != THUMB_W || bmp->height != THUMB_H || bmp->bpp != 4 || bmp->compression != 0)
        return false;
    uint32_t pitch = ((uint32_t) THUMB_W * 4 + 31) / 32 * 4;
    return 14 + bmp->header_size + 16 * 4 <= bmp->data_start
        && bmp->data_start + pitch * THUMB_H <= file_size;
}

// Pixel (x, y) of the picture (x < 112, y < 72; rows stored bottom-up).
static inline uint8_t thumb_pixel(const uint8_t __far *data, uint16_t pitch, uint8_t x, uint8_t y) {
    if (x < THUMB_FRAME || y < THUMB_FRAME || x >= THUMB_W - THUMB_FRAME || y >= THUMB_H - THUMB_FRAME)
        return THUMB_FRAME_COLOR;
    uint8_t b = data[(uint16_t) (THUMB_H - 1 - y) * pitch + (x >> 1)];
    return (x & 1) ? (b & 15) : (b >> 4);
}

/* What the box covers of the wallpaper (its tiles, their screen cells and
 * palette 15) is kept in the PSRAM scratch bank, after the picture's file,
 * and put back when the box closes: reading the wallpaper again from the
 * card and converting it took longer than anything else when the selection
 * moved. */
#define THUMB_FILE_MAX   0x8000                  // the picture's file below this
#define THUMB_SAVE_TILES 0x8000                  // w * h * 32 bytes (4032)
#define THUMB_SAVE_CELLS (THUMB_SAVE_TILES + 0x1000)
#define THUMB_SAVE_PAL   (THUMB_SAVE_CELLS + 0x0200)

static uint8_t thumb_x0, thumb_y0, thumb_w, thumb_h;

static inline uint16_t thumb_tile(uint8_t x, uint8_t y) {
    return x * WS_DISPLAY_HEIGHT_TILES + y;
}

// (the scratch bank is mapped) wallpaper under the box -> scratch bank, or back
static void thumb_save_restore(bool restore) {
    uint16_t __far *tiles = MK_FP(0x1000, THUMB_SAVE_TILES);
    uint16_t __far *cells = MK_FP(0x1000, THUMB_SAVE_CELLS);
    uint16_t __far *pal = MK_FP(0x1000, THUMB_SAVE_PAL);
    for (uint8_t ix = 0; ix < thumb_w; ix++) {
        // the tiles of a column of the box follow each other
        uint16_t *vram = (uint16_t*) (0x8000 + thumb_tile(thumb_x0 + ix, thumb_y0) * 32);
        for (uint16_t i = 0; i < thumb_h * 16; i++, tiles++, vram++) {
            if (restore) *vram = *tiles; else *tiles = *vram;
        }
        for (uint8_t iy = 0; iy < thumb_h; iy++, cells++) {
            uint16_t ws_iram *cell = bitmap_screen1 + (thumb_y0 + iy) * 32 + thumb_x0 + ix;
            if (restore) *cell = *cells; else *cells = *cell;
        }
    }
    for (uint8_t c = 0; c < 16; c++, pal++) {
        if (restore) WS_DISPLAY_COLOR_MEM(THUMB_PALETTE)[c] = *pal;
        else *pal = WS_DISPLAY_COLOR_MEM(THUMB_PALETTE)[c];
    }
}

// The picture is in the RAM window (PSRAM scratch bank).
static void thumb_draw(uint8_t x0, uint8_t y0, uint8_t w, uint8_t h) {
    bmp_header_t __far *bmp = MK_FP(0x1000, 0x0000);
    const uint8_t __far *palette = MK_FP(0x1000, 14 + bmp->header_size);
    for (uint8_t c = 0; c < 16; c++)
        WS_DISPLAY_COLOR_MEM(THUMB_PALETTE)[c] = bmp_color(palette + c * 4);
    // the frame: the accent color of the title bar
    WS_DISPLAY_COLOR_MEM(THUMB_PALETTE)[THUMB_FRAME_COLOR] = WS_DISPLAY_COLOR_MEM(2)[2];

    const uint8_t __far *data = MK_FP(0x1000, bmp->data_start);
    const uint16_t pitch = (THUMB_W * 4 + 31) / 32 * 4;
    for (uint8_t ix = 0; ix < w; ix++) {
        for (uint8_t iy = 0; iy < h; iy++) {
            uint16_t tile = (x0 + ix) * WS_DISPLAY_HEIGHT_TILES + (y0 + iy);
            uint32_t *dst = (uint32_t*) (0x8000 + tile * 32);
            if (bitmap_rotation && ix && iy && ix < w - 1 && iy < h - 1) {
                // inside the frame, not turned: a row of the tile is 4
                // bytes of the file, in the same order (left pixel in the
                // high nibble)
                for (uint8_t r = 0; r < 8; r++) {
                    const uint32_t __far *src = (const uint32_t __far*) (data
                        + (uint16_t) (THUMB_H - 1 - (iy * 8 + r)) * pitch + ix * 4);
                    dst[r] = wsx_planar_convert_4bpp_packed_row(*src);
                }
            } else for (uint8_t r = 0; r < 8; r++) {
                uint8_t packed[4];
                for (uint8_t k = 0; k < 8; k++) {
                    uint8_t nib;
                    if (bitmap_rotation)
                        nib = thumb_pixel(data, pitch, ix * 8 + k, iy * 8 + r);
                    else
                        // turned clockwise: LCD row R shows picture column R,
                        // LCD column C shows picture row 71 - C
                        nib = thumb_pixel(data, pitch, iy * 8 + r, THUMB_H - 1 - (ix * 8 + k));
                    if (k & 1)
                        packed[k >> 1] |= nib;
                    else
                        packed[k >> 1] = nib << 4;
                }
                dst[r] = wsx_planar_convert_4bpp_packed_row(*((uint32_t*) packed));
            }
            ws_screen_put_tile(bitmap_screen1, WS_SCREEN_ATTR_PALETTE(THUMB_PALETTE) | WS_SCREEN_ATTR_BANK(1) | tile, x0 + ix, y0 + iy);
        }
    }
}

bool ui_show_thumbnail(const char *path) {
    if (ws_system_get_mode() != WS_MODE_COLOR_4BPP)
        return false;
    FIL fp;
    if (f_open(&fp, path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
        return false;
    uint32_t size = f_size(&fp);
    if (size > THUMB_FILE_MAX) {
        f_close(&fp);
        return false;
    }

    uint8_t x0, y0, w, h;
    thumb_geometry(&x0, &y0, &w, &h);
    thumb_x0 = x0; thumb_y0 = y0; thumb_w = w; thumb_h = h;
    thumb_saved = false;
    bool ok = false;
    // read into the PSRAM scratch bank, then convert (no return inside:
    // the previous flash mode and RAM bank must be restored)
    ws_bank_with_flash(WS_CART_BANK_FLASH_ENABLE, {
        ws_bank_with_ram(UI_SCRATCH_PSRAM_BANK, {
            unsigned int br;
            if (f_read(&fp, MK_FP(0x1000, 0x0000), size, &br) == FR_OK && br == size
                && thumb_header_ok(MK_FP(0x1000, 0x0000), size)) {
                if (wallpaper_status == 1) {
                    thumb_save_restore(false);
                    thumb_saved = true;
                }
                // wait for the vertical blank: the tiles change on screen
                while (inportb(WS_DISPLAY_LINE_PORT) < WS_DISPLAY_HEIGHT_PIXELS);
                thumb_draw(x0, y0, w, h);
                ok = true;
            }
        });
    });
    f_close(&fp);
    if (!ok)
        return false;

    // hide the list inside the box, show screen 1
    ws_display_set_screen2_window_corners(x0 * 8, y0 * 8, (x0 + w) * 8 - 1, (y0 + h) * 8 - 1);
    outportb(WS_DISPLAY_CTRL_PORT, (inportb(WS_DISPLAY_CTRL_PORT) & ~WS_DISPLAY_CTRL_SCR2_WIN_OUTSIDE)
        | WS_DISPLAY_CTRL_SCR2_WIN_OUTSIDE | WS_DISPLAY_CTRL_SCR1_ENABLE);
    thumb_active = true;
    return true;
}

void ui_hide_thumbnail(void) {
    if (!thumb_active)
        return;
    thumb_active = false;
    if (thumb_saved && wallpaper_status == 1) {
        // put back what the box covered, then show the list there again
        thumb_saved = false;
        while (inportb(WS_DISPLAY_LINE_PORT) < WS_DISPLAY_HEIGHT_PIXELS);
        // (the RAM window shows the PSRAM only with port CEh's flash bit
        // set: without it, it is the cartridge's save RAM)
        ws_bank_with_flash(WS_CART_BANK_FLASH_ENABLE, {
            ws_bank_with_ram(UI_SCRATCH_PSRAM_BANK, {
                thumb_save_restore(true);
            });
        });
        outportb(WS_DISPLAY_CTRL_PORT, inportb(WS_DISPLAY_CTRL_PORT) & ~WS_DISPLAY_CTRL_SCR2_WIN_OUTSIDE);
        return;
    }
    outportb(WS_DISPLAY_CTRL_PORT, inportb(WS_DISPLAY_CTRL_PORT) & ~WS_DISPLAY_CTRL_SCR2_WIN_OUTSIDE);
    if (wallpaper_status == 1) {
        // its tiles and palette 15 were used by the box
        ui_reload_wallpaper();
    } else {
        ui_show_inner(); // screen 1 off again
    }
}
#endif

void ui_show(void) {
#ifdef CONFIG_ENABLE_WALLPAPER
    if (!wallpaper_status && fs_initialized()) {
        load_wallpaper();
    }
#endif
    ui_show_inner();
}

void ui_layout_clear(uint16_t pal) {
    icons_visible = 0;
    if (pal == 0 && !ui_has_wallpaper()) {
        // bitmap_rect_fill(&ui_bitmap, 0, 0, screen_width, screen_height, BITMAP_COLOR_4BPP(2));
        if (ui_bitmap.bpp == 4) {
            ws_memset32(ui_bitmap.start, 0x0000FF00, WS_DISPLAY_WIDTH_PIXELS / 2 * WS_DISPLAY_HEIGHT_PIXELS);
        } else {
            ws_memset16(ui_bitmap.start, 0xFF00, WS_DISPLAY_WIDTH_PIXELS / 4 * WS_DISPLAY_HEIGHT_PIXELS);
        }
    } else {
        // bitmap_rect_clear(&ui_bitmap, 0, 0, screen_width, screen_height);
        memset(ui_bitmap.start, 0, (WS_DISPLAY_WIDTH_PIXELS / 8 * WS_DISPLAY_HEIGHT_PIXELS) * ui_bitmap.bpp);
    }

#ifdef CONFIG_UI_PRECOMPUTED_SCREEN_PATTERNS
    memcpy(bitmap_screen2, ui_layout_clear_data, sizeof(ui_layout_clear_data));
#else
    INIT_SCREEN_PATTERN(bitmap_screen2, pal, 0);
#endif
}

void ui_layout_bars_pattern(void) {
#ifdef CONFIG_UI_PRECOMPUTED_SCREEN_PATTERNS
    memcpy(bitmap_screen2, !bitmap_rotation ? ui_layout_bars_v_data : ui_layout_bars_h_data, sizeof(ui_layout_clear_data));
#else
    INIT_SCREEN_PATTERN(bitmap_screen2, (!bitmap_rotation ? (ix == 0 || ix == 27) : (iy == 0 || iy == 17)) ? WS_SCREEN_ATTR_PALETTE(2) : 0, 0);
#endif
}

void ui_layout_bars(void) {
    if (!ui_has_wallpaper()) {
        bitmap_rect_fill(&ui_bitmap, 0, 8, screen_width, screen_height - 16, BITMAP_COLOR_4BPP(2));
    } else {
        bitmap_rect_clear(&ui_bitmap, 0, 8, screen_width, screen_height - 16);
    }
    ui_layout_bars_pattern();
}

void ui_screen_modify_tiles(void ws_iram *dest, uint16_t mask, uint16_t value, uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    if (!bitmap_rotation) {
        ws_screen_modify_tiles(dest, mask, value, WS_DISPLAY_WIDTH_TILES - height - y, x, height, width);
    } else {
        ws_screen_modify_tiles(dest, mask, value, x, y, width, height);
    }
}
