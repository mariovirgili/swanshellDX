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
#include <stdio.h>
#include <string.h>
#include <ws.h>
#include <ws/display.h>
#include <nilefs.h>
#include "config.h"
#include "lang.h"
#include "lang_gen.h"
#include "main.h"
#include "strings.h"
#include "ui.h"
#include "ui_game_images.h"
#include "util/bmp.h"
#include "util/file.h"
#include "util/input.h"

#ifdef CONFIG_ENABLE_WALLPAPER

_Static_assert(GAME_IMAGES_BUFFER_OFFSET + sizeof(game_images_t) <= CONFIG_MEMLAYOUT_SECTOR_BUFFER_SIZE,
    "game_images_t does not fit in the sector buffer");

game_images_t *game_images_buffer(void) {
    return (game_images_t*) (sector_buffer + GAME_IMAGES_BUFFER_OFFSET);
}

static bool picture_ok(const char *path) {
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

// Collect the (up to GAME_IMAGES_MAX, alphabetically first) valid pictures
// of images->dir.
static uint8_t collect(game_images_t *images) {
    DIR dir;
    FILINFO fno;
    char *path = images->tmp;

    images->count = 0;
    if (f_opendir(&dir, images->dir) != FR_OK)
        return 0;

    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fattrib & (AM_DIR | AM_HID | AM_SYS))
            continue;
        uint16_t len = strlen(fno.fname);
        if (len < 5 || len >= GAME_IMAGES_NAME_MAX || strcasecmp(fno.fname + len - 4, s_file_ext_bmp))
            continue;
        // the thumbnail (112x72) is not one of the pictures
        if (!strcasecmp(fno.fname, s_thumb_bmp)) {
            images->has_thumb = true;
            continue;
        }
        // position in the sorted list; skip if after the last kept one
        uint8_t pos = images->count;
        while (pos > 0 && strcasecmp(fno.fname, images->name[pos - 1]) < 0)
            pos--;
        if (pos >= GAME_IMAGES_MAX)
            continue;

        uint16_t dlen = strlen(images->dir);
        if (dlen + 1 + len >= GAME_IMAGES_PATH_MAX)
            continue;
        memcpy(path, images->dir, dlen);
        path[dlen] = '/';
        memcpy(path + dlen + 1, fno.fname, len + 1);
        if (!picture_ok(path))
            continue;

        uint8_t last = images->count < GAME_IMAGES_MAX ? images->count : GAME_IMAGES_MAX - 1;
        for (uint8_t i = last; i > pos; i--)
            memcpy(images->name[i], images->name[i - 1], GAME_IMAGES_NAME_MAX);
        memcpy(images->name[pos], fno.fname, len + 1);
        if (images->count < GAME_IMAGES_MAX)
            images->count++;
    }
    f_closedir(&dir);
    return images->count;
}

uint8_t game_images_find(const char __far *file_name, game_images_t *images) {
    images->count = 0;
    images->has_thumb = false;
    strcpy(images->dir, s_path_images_dir);
    uint16_t n = strlen(images->dir);
    images->dir[n++] = '/';
    uint16_t base = n;
    while (*file_name && n < GAME_IMAGES_PATH_MAX - GAME_IMAGES_NAME_MAX - 2)
        images->dir[n++] = *(file_name++);
    images->dir[n] = 0;

    // strip one extension at a time: "Game.wsc.bps" -> "Game.wsc" -> "Game"
    for (uint8_t tries = 0; tries < 2; tries++) {
        char *dot = NULL;
        for (char *p = images->dir + base; *p; p++)
            if (*p == '.')
                dot = p;
        if (dot == NULL || dot == images->dir + base) {
            if (tries == 0)
                collect(images); // no extension: the name as it is
            break;
        }
        *dot = 0;
        if (collect(images) || images->has_thumb)
            break;
    }
    return images->count;
}

bool game_images_thumb_path(const game_images_t *images, char *buf) {
    if (!images->has_thumb)
        return false;
    uint16_t dlen = strlen(images->dir);
    uint16_t len = strlen(s_thumb_bmp);
    if (dlen + 1 + len >= GAME_IMAGES_PATH_MAX)
        return false;
    memcpy(buf, images->dir, dlen);
    buf[dlen] = '/';
    strcpy(buf + dlen + 1, s_thumb_bmp);
    return true;
}

bool game_images_path(const game_images_t *images, uint8_t i, char *buf) {
    if (i >= images->count)
        return false;
    uint16_t dlen = strlen(images->dir);
    uint16_t len = strlen(images->name[i]);
    if (dlen + 1 + len >= GAME_IMAGES_PATH_MAX)
        return false;
    memcpy(buf, images->dir, dlen);
    buf[dlen] = '/';
    memcpy(buf + dlen + 1, images->name[i], len + 1);
    return true;
}

static void view_draw_bars(const game_images_t *images, const char __far *title, uint8_t i) {
    char pos[8];
    snprintf(pos, sizeof(pos), "%u/%u", i + 1, images->count);
    ui_draw_titlebar(title);
    ui_draw_titlebar_right(pos);
    ui_draw_statusbar(lang_keys[bitmap_rotation ? LK_UI_IMAGES_CONTROLS : LK_UI_IMAGES_CONTROLS_SHORT]);
}

bool game_images_view(game_images_t *images, const char __far *title) {
    char *path = images->tmp;
    uint8_t i = 0;
    bool bars = true;
    bool play = false;

    input_wait_clear();
    while (true) {
        if (game_images_path(images, i, path))
            ui_load_background(path);
        // the picture fills the screen: the list area is transparent
        ui_layout_bars();
        view_draw_bars(images, title, i);
        ui_show();
        if (!bars)
            outportb(WS_DISPLAY_CTRL_PORT, inportb(WS_DISPLAY_CTRL_PORT) & ~WS_DISPLAY_CTRL_SCR2_ENABLE);

        uint8_t next = i;
        while (next == i) {
            idle_until_vblank();
            input_update();
            uint16_t keys = input_pressed;
            if (keys & KEY_RIGHT)
                next = (i + 1) % images->count;
            else if (keys & KEY_LEFT)
                next = i ? i - 1 : images->count - 1;
            else if (keys & KEY_UP) {
                bars = !bars;
                uint8_t ctrl = inportb(WS_DISPLAY_CTRL_PORT);
                outportb(WS_DISPLAY_CTRL_PORT, bars ? (ctrl | WS_DISPLAY_CTRL_SCR2_ENABLE) : (ctrl & ~WS_DISPLAY_CTRL_SCR2_ENABLE));
            } else if (keys & WS_KEY_A) {
                play = true;
                goto done;
            } else if (keys & (WS_KEY_B | WS_KEY_START)) {
                goto done;
            }
            if (images->count < 2 && next != i)
                next = i; // only one picture
        }
        i = next;
    }

done:
    outportb(WS_DISPLAY_CTRL_PORT, inportb(WS_DISPLAY_CTRL_PORT) | WS_DISPLAY_CTRL_SCR2_ENABLE);
    ui_reload_wallpaper();
    input_wait_clear();
    return play;
}

#else

uint8_t game_images_find(const char __far *file_name, game_images_t *images) {
    images->count = 0;
    images->has_thumb = false;
    return 0;
}
bool game_images_path(const game_images_t *images, uint8_t i, char *buf) { return false; }
bool game_images_thumb_path(const game_images_t *images, char *buf) { return false; }
bool game_images_view(game_images_t *images, const char __far *title) { return false; }
game_images_t *game_images_buffer(void) { return NULL; }

#endif /* CONFIG_ENABLE_WALLPAPER */
