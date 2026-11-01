/**
 * Copyright (c) 2024, 2025 Adrian Siekierka
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
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ws.h>
#include <nilefs.h>
#include <ws/memory.h>
#include <ws/system.h>
#include "errors.h"
#include "lang.h"
#include "../ui/ui.h"
#include "../util/file.h"
#include "../util/input.h"
#include "../util/memops.h"
#include "../util/util.h"
#include "plugin.h"
#include "settings.h"
#include "ui/bitmap.h"
#include "util/asset_heap.h"
#include "vgm/vgm.h"

static vgm_state_t *vgm_state;

static uint16_t track_index, track_count;

void ui_vgmplay_set_position(uint16_t index, uint16_t count) {
    track_index = index;
    track_count = count;
}

// The LCD draws 12000 lines per second, whatever the frame length: time is
// measured in lines, by polling the line counter.
#define LINES_PER_SECOND 12000

// Length of a track before moving on, in seconds, from the VGM header:
// total length (one pass, including one loop) plus one more loop.
// 0: unknown.
static uint16_t vgm_play_seconds(uint16_t vgm_bank) {
    uint16_t old_bank = ws_bank_rom0_save(vgm_bank);
    uint32_t total = *((uint32_t __far*) MK_FP(WS_ROM0_SEGMENT, 0x18));
    uint32_t loop_ofs = *((uint32_t __far*) MK_FP(WS_ROM0_SEGMENT, 0x1C));
    uint32_t loop = *((uint32_t __far*) MK_FP(WS_ROM0_SEGMENT, 0x20));
    ws_bank_rom0_set(old_bank);

    if (loop_ofs != 0 && !(loop_ofs & 0xFF000000))
        total += loop;
    // 44100 samples per second; at most ~18 hours
    uint32_t seconds = (total + 44099) / 44100;
    return seconds > 65535 ? 65535 : seconds;
}

static uint16_t time_text_width;

// "3/12  1:23 / 3:45" on the right of the title bar
static void draw_track_status(uint16_t elapsed, uint16_t length) {
    char buf[40];
    char *p = buf;
    if (track_count)
        p += sprintf(p, "%u/%u  ", track_index, track_count);
    p += sprintf(p, "%u:%02u", elapsed / 60, elapsed % 60);
    if (length)
        sprintf(p, " / %u:%02u", length / 60, length % 60);

    // redraw while the LCD is below the title bar, so it never shows half
    // drawn; clear what a longer previous text left behind
    while (inportb(WS_DISPLAY_LINE_PORT) < 8 || inportb(WS_DISPLAY_LINE_PORT) > 100);
    bitmapfont_set_active_font(font8_bitmap);
    uint16_t width = bitmapfont_get_string_width(buf, screen_width);
    if (time_text_width > width)
        bitmap_rect_fill(&ui_bitmap, screen_width - time_text_width - 6, 0, time_text_width - width, 8, BITMAP_COLOR_2BPP(2));
    time_text_width = width;
    ui_draw_titlebar_right(buf);
}

void  __attribute__((interrupt, assume_ss_data)) vgm_interrupt_handler(void) {
    while (true) {
        outportw(WS_TIMER_HBL_RELOAD_PORT, 65535);
        uint16_t result = vgm_play(vgm_state);
        uint16_t ticks_elapsed = inportw(WS_TIMER_HBL_COUNTER_PORT) ^ 65535;
        if (result == VGM_PLAYBACK_FINISHED) {
            vgm_state->bank = 0xFF;
            outportb(WS_TIMER_CTRL_PORT, 0);
        } else if (result > (ticks_elapsed + 1)) {
            outportw(WS_TIMER_HBL_RELOAD_PORT, result - ticks_elapsed);
        } else {
            continue;
        }

        ws_int_ack(WS_INT_ACK_HBL_TIMER);
        return;
    }
}

static int strlen16(const uint16_t __far* text) {
    int i = 0;
    while (*(text++)) i++;
    return i;
}

#define MAX_GD3_TEXT_LEN 127

__attribute__((noinline))
static void print_gd3_metadata(uint16_t vgm_bank) {
    uint16_t old_bank = ws_bank_rom0_save(vgm_bank);
    uint16_t text16[MAX_GD3_TEXT_LEN+1];

    uint32_t gd3_offset = *((uint32_t __far*) MK_FP(WS_ROM0_SEGMENT, 0x0014));
    if (gd3_offset != 0) {
        gd3_offset += 0x20;

        const uint16_t __far* gd3_data = MK_FP(WS_ROM0_SEGMENT + ((gd3_offset & 0xFFF0) >> 4), (gd3_offset & 0xF));

        int x = 8;
        int y = 16;
        for (int i = 0; i < 8; i++) {
            ws_bank_rom0_set(vgm_bank + (gd3_offset >> 16));
            ws_bank_rom1_set(vgm_bank + (gd3_offset >> 16) + 1);

            int len = strlen16(gd3_data);

            if (len > 0 && (i == 0 || i == 2 || i == 4 || i == 6)) {
                // Print text
                memcpy(text16, gd3_data, MIN(len, MAX_GD3_TEXT_LEN) * 2);
                text16[MIN(len, MAX_GD3_TEXT_LEN)] = 0;

                bitmapfont_set_active_font(i == 0 ? font16_bitmap : font8_bitmap);
                bitmapfont_draw_string16(&ui_bitmap, x, y, text16, 248);
                y += i == 0 ? 16 : 12;
            }

            gd3_data += len + 1;
        }
    }

    ws_bank_rom0_set(old_bank);
}

int ui_vgmplay(const char *path) {
    vgm_state_t local_vgm_state;
    FIL fp;

    uint8_t vtotal_initial = inportb(WS_LCD_VTOTAL_PORT);

    ui_layout_bars();

    uint8_t result = f_open(&fp, path, FA_READ);
    if (result != FR_OK) {
        return result;
    }

    ui_draw_titlebar_filename(path);
    ui_draw_statusbar(lang_keys[LK_UI_STATUS_LOADING]);

    uint32_t size = f_size(&fp);
    if ((size >> 16) > asset_heap_get_free_first_banks()) {
        return ERR_FILE_TOO_LARGE;
    }

    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
    result = f_read_rom_banked(&fp, 0, size, NULL, NULL);

    f_close(&fp);
    if (result != FR_OK) {
        return result;
    }

    uint16_t vgm_bank = 0;
    uint16_t vgm_banks_used = (size + 65535L) >> 16;
    memops_unpack_psram_data_if_gzip(&vgm_bank, vgm_banks_used);

    // Controls hint: right/left skip to the next/previous track, any other
    // key exits (aligned right, like the page counter in the file list)
    ui_draw_statusbar_lr(lang_keys[LK_UI_VGMPLAY_CONTROLS], lang_keys[LK_UI_VGMPLAY_EXIT]);

    vgm_state = &local_vgm_state;

    ws_sound_reset();
    outportb(WS_SOUND_WAVE_BASE_PORT, WS_SOUND_WAVE_BASE_ADDR(0x3FC0));

    ui_unload_wallpaper();

    if (!vgm_init(&local_vgm_state, vgm_bank, 0)) {
        ws_sound_reset();
        return ERR_FILE_FORMAT_INVALID;
    }

    print_gd3_metadata(vgm_bank);

    uint8_t play_mode = settings.vgm_play_mode;
    uint16_t length = vgm_play_seconds(vgm_bank);
    uint16_t elapsed = 0;
    uint16_t line_acc = 0;
    uint8_t prev_line = inportb(WS_DISPLAY_LINE_PORT);
    time_text_width = 0;
    draw_track_status(0, length);

    input_wait_clear();
    outportb(WS_SOUND_OUT_CTRL_PORT, WS_SOUND_OUT_CTRL_SPEAKER_ENABLE | WS_SOUND_OUT_CTRL_HEADPHONE_ENABLE | WS_SOUND_OUT_CTRL_SPEAKER_VOLUME_100);

    outportw(WS_TIMER_HBL_RELOAD_PORT, 2);
    outportb(WS_TIMER_CTRL_PORT, WS_TIMER_CTRL_HBL_REPEAT);

    ws_int_set_handler(WS_INT_HBL_TIMER, (ia16_int_handler_t) vgm_interrupt_handler);
    ws_int_enable(WS_INT_ENABLE_HBL_TIMER);

    int exit_code = 0;
    while (vgm_state->bank != 0xFF) {
        // elapsed time
        uint8_t line = inportb(WS_DISPLAY_LINE_PORT);
        int16_t d = (int16_t) line - (int16_t) prev_line;
        if (d < 0)
            d += inportb(WS_LCD_VTOTAL_PORT) + 1;
        prev_line = line;
        line_acc += d;
        if (line_acc >= LINES_PER_SECOND) {
            line_acc -= LINES_PER_SECOND;
            elapsed++;
            draw_track_status(elapsed, length);
            // end of the track: next one (unless repeating)
            if (length && elapsed >= length && play_mode != SETTING_VGM_PLAY_REPEAT) {
                exit_code = play_mode == SETTING_VGM_PLAY_SHUFFLE ? PLUGIN_RESULT_RANDOM_FILE : PLUGIN_RESULT_NEXT_FILE;
                break;
            }
        }

        input_update();
        if (input_pressed) {
            // Right/left: next/previous track in the same directory
            // (Right picks one at random in shuffle mode)
            if (input_pressed & KEY_RIGHT)
                exit_code = play_mode == SETTING_VGM_PLAY_SHUFFLE ? PLUGIN_RESULT_RANDOM_FILE : PLUGIN_RESULT_NEXT_FILE;
            else if (input_pressed & KEY_LEFT)
                exit_code = PLUGIN_RESULT_PREV_FILE;
            break;
        }
    }
    // the position is set again before each track
    track_count = 0;

    ws_int_disable(WS_INT_ENABLE_HBL_TIMER | WS_INT_ENABLE_LINE_MATCH);
    outportb(WS_TIMER_CTRL_PORT, 0);
    ws_sound_reset();

    if (inportb(WS_LCD_VTOTAL_PORT) != vtotal_initial) {
        lcd_set_vtotal(vtotal_initial);
    }

    outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_DISABLE);
    ui_init();
    settings_load();

    return exit_code;
}
