/**
 * Copyright (c) 2025, 2026 Adrian Siekierka
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

#include <string.h>
#include <ws.h>
#include <nilefs.h>
#include "lang.h"
#include "lang_gen.h"
#include "strings.h"
#include "ui/bitmap.h"
#include "ui/ui.h"
#include "util/input.h"
#include "util/util.h"
#include "ui_help.h"
#include "../main.h"

/*
 * Help (Gio's request, 2026-11-16): the entry after Settings in the START
 * menu. What it shows goes by the file selected: the keys of the plugin
 * that runs it (.gg, .sms, .sg, .sf, .gb), or the menu's own. The texts
 * are here, in English, a page a string (as many lines as fit the screen
 * in either layout: twelve short ones).
 */

static const char __far s_help_menu_1[] =
    "SwanShellDX: the file list\n\n"
    "X pad: move, change page\n"
    "A: open the folder, start the game\n"
    "B: back to the folder above\n"
    "START: this menu (File, Tools, Settings, Help)\n"
    "Y1 (Y up): add to the favorites, or remove\n"
    "Y2, Y4 (Y right, Y left): next, previous initial letter\n"
    "Y3 (Y down): the game's pictures; A there starts it";
static const char __far s_help_menu_2[] =
    "SwanShellDX: more\n\n"
    "Y1 (Y up) held when the menu starts: the last game played again.\n"
    "Saves: a game's save is written to its .sav file when the menu starts again.\n"
    "Help on a .gg, .sms, .sg, .sf or .gb file: that plugin's keys.";

static const char __far s_help_gg_1[] =
    "Game Gear (ggswan)\n\n"
    "X pad: the D-pad\n"
    "B: button 1   A: button 2\n"
    "START: Start (a short press, or held)\n\n"
    "START + A, half a second: keep the state\n"
    "START + B, half a second: bring it back\n"
    "START + Y1 (Y up), a second: back to the menu\n"
    "START + Y3 (Y down): the menu (video, sound, states, options)";
static const char __far s_help_gg_2[] =
    "Game Gear: more keys\n\n"
    "Y4 (Y left) held, then START: the numbers in the borders, off and on\n"
    "Y2 + Y4 (Y right + Y left): pause; Y3 (Y down) then runs one frame\n\n"
    "Held when the game starts:\n"
    "Y1 (Y up): background priority\n"
    "Y4 (Y left): diagnostics";
static const char __far s_help_gg_3[] =
    "Game Gear: saves\n\n"
    "A game's own save is its .sav file, written when the menu starts again.\n"
    "A state is a .st0 file next to the ROM; it is read when the game starts, "
    "and only by the same version of the plugin.";

static const char __far s_help_sms_1[] =
    "Master System (smsswan)\n\n"
    "X pad: the D-pad\n"
    "B: button 1   A: button 2\n"
    "START: Pause, when let go\n"
    "Y1, Y2, Y3, Y4: move the view over the picture\n\n"
    "START + A, half a second: keep the state\n"
    "START + B, half a second: bring it back\n"
    "START + Y1 (Y up), a second: back to the menu\n"
    "START + Y3 (Y down): the menu (video, sound, states, options)";
static const char __far s_help_sms_2[] =
    "Master System: more keys\n\n"
    "START + Y4 (Y left): the numbers, on and off (the sprites then in front of everything)\n"
    "Y2 + Y4 (Y right + Y left): pause; Y3 (Y down) then runs one frame\n\n"
    "Y1 (Y up) held when the game starts: diagnostics";

static const char __far s_help_sg_1[] =
    "SG-1000 (sgswan)\n\n"
    "X pad: the joystick\n"
    "B: button 1   A: button 2\n"
    "START: Pause, when let go\n"
    "Y1, Y2, Y3, Y4: move the view over the picture\n\n"
    "START + A, half a second: keep the state\n"
    "START + B, half a second: bring it back\n"
    "START + Y1 (Y up), a second: back to the menu\n"
    "START + Y3 (Y down): the menu (video, sound, states, options)";
static const char __far s_help_sg_2[] =
    "SG-1000: more keys\n\n"
    "START + Y2 (Y right): four sprites to a line, as the SG-1000, on and off\n"
    "START + Y4 (Y left): the numbers, on and off\n"
    "Y2 + Y4 (Y right + Y left): pause; Y3 (Y down) then runs one frame";

static const char __far s_help_sf_1[] =
    "SC-3000 with SF-7000 (sfswan)\n\n"
    "X pad: the joystick   B, A: its buttons\n"
    "START: the keyboard on the screen, on and off\n"
    "Y1, Y2, Y3, Y4: move the view over the picture\n\n"
    "START + Y1 (Y up), a second: back to the menu\n"
    "START + Y2 (Y right): four sprites to a line, on and off\n"
    "START + Y3 (Y down): the menu (video, sound, options)\n"
    "START + Y4 (Y left): the numbers, on and off";
static const char __far s_help_sf_2[] =
    "SF-7000: disks\n\n"
    "A disk that does not start by itself needs the system disk: "
    "/NILESWAN/SF7000.SF.\n"
    "What a program writes to the disk is lost when the plugin is left.\n"
    "No states in this plugin.";

static const char __far s_help_gb_1[] =
    "Game Boy (gbswan)\n\n"
    "X pad: the D-pad\n"
    "A: A   B: B\n"
    "START: Start\n"
    "Y3, Y2 (Y down, Y right): Select\n\n"
    "START + A, half a second: keep the state\n"
    "START + B, half a second: bring it back\n"
    "START + Y1 (Y up), a second: back to the menu\n"
    "START + Y3 (Y down): the plugin's menu\n"
    "START + Y4 (Y left): the numbers, on and off";
static const char __far s_help_gb_2[] =
    "Game Boy: saves\n\n"
    "A game with a battery keeps its RAM in its .sav file, written when the menu starts again.\n"
    "A state is a .st0 file next to the ROM; it is read when the game starts, "
    "and only by the same version of the plugin.\n"
    "Game Boy Color games are not run yet.";

#define HELP_MAX_PAGES 3
typedef struct {
    const char __far *ext;
    const char __far *page[HELP_MAX_PAGES];
} help_topic_t;

static const help_topic_t __far help_topics[] = {
    { s_file_ext_gg, { s_help_gg_1, s_help_gg_2, s_help_gg_3 } },
    { s_file_ext_sms, { s_help_sms_1, s_help_sms_2, NULL } },
    { s_file_ext_sg, { s_help_sg_1, s_help_sg_2, NULL } },
    { s_file_ext_sf, { s_help_sf_1, s_help_sf_2, NULL } },
    { s_file_ext_gb, { s_help_gb_1, s_help_gb_2, NULL } },
    { NULL, { s_help_menu_1, s_help_menu_2, NULL } },
};

static const char __far s_help_status[] = "A: next   B: back";

void ui_help(const char __far *filename, uint8_t attrib) {
    // the topic: by the file's extension
    const help_topic_t __far *topic = help_topics;
    const char __far *ext = NULL;
    if (filename != NULL && !(attrib & AM_DIR)) {
        for (const char __far *p = filename; *p; p++)
            if (*p == '.') ext = p;
    }
    while (topic->ext != NULL && (ext == NULL || strcasecmp(ext, topic->ext)))
        topic++;
    uint8_t pages = 0;
    while (pages < HELP_MAX_PAGES && topic->page[pages] != NULL)
        pages++;

    // black on white whatever the theme, and no wallpaper behind: screen 1
    // off, the list's layer cleared to its color 0 (the display's own
    // color, set to white), the text's color black
    uint8_t ctrl = inportb(WS_DISPLAY_CTRL_PORT);
    uint16_t pal0 = 0, pal1 = 0;
    bool color = ws_system_is_color_active();
    if (color) {
        pal0 = WS_DISPLAY_COLOR_MEM(0)[0];
        pal1 = WS_DISPLAY_COLOR_MEM(0)[1];
    }
    outportb(WS_DISPLAY_CTRL_PORT, ctrl & ~WS_DISPLAY_CTRL_SCR1_ENABLE);

    uint8_t page = 0;
    bool redraw = true;
    input_wait_clear();
    while (true) {
        if (redraw) {
            ui_layout_clear(0);
            memset(ui_bitmap.start, 0, (WS_DISPLAY_WIDTH_PIXELS / 8 * WS_DISPLAY_HEIGHT_PIXELS) * ui_bitmap.bpp);
            if (color) {
                WS_DISPLAY_COLOR_MEM(0)[0] = 0xFFF;
                WS_DISPLAY_COLOR_MEM(0)[1] = 0x000;
            }
            bitmapfont_set_active_font(font8_bitmap);
            bitmapfont_draw_string_box(&ui_bitmap, 4, 3, topic->page[page], screen_width - 8, 2, 0);
            {
                char num[8];
                num[0] = '1' + page; num[1] = '/'; num[2] = '0' + pages; num[3] = 0;
                bitmapfont_draw_string(&ui_bitmap, 4, screen_height - 10, s_help_status, screen_width - 40);
                bitmapfont_draw_string(&ui_bitmap, screen_width - 4 - bitmapfont_get_string_width(num, 65535),
                    screen_height - 10, num, 65535);
            }
            redraw = false;
        }
        idle_until_vblank();
        input_update();
        if (input_pressed & (WS_KEY_B | WS_KEY_START))
            break;
        if (input_pressed & (WS_KEY_A | KEY_RIGHT | KEY_DOWN)) {
            if (pages > 1) {
                page = page + 1 < pages ? page + 1 : 0;
                redraw = true;
            } else if (input_pressed & WS_KEY_A) {
                break;
            }
        }
        if ((input_pressed & (KEY_LEFT | KEY_UP)) && pages > 1) {
            page = page ? page - 1 : pages - 1;
            redraw = true;
        }
    }
    input_wait_clear();

    if (color) {
        WS_DISPLAY_COLOR_MEM(0)[0] = pal0;
        WS_DISPLAY_COLOR_MEM(0)[1] = pal1;
    }
    outportb(WS_DISPLAY_CTRL_PORT, ctrl);
}
