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
#include "menu_music.h"
#include "settings.h"
#include "strings.h"
#include "util/asset_heap.h"
#include "util/file.h"
#include "plugin/vgm/vgm.h"

// Largest MENU.VGM accepted, in 64 KiB banks (uncompressed .vgm only).
#define MENU_MUSIC_MAX_BANKS 2
// Wavetable RAM: last row of the screen 1 map (unused, 18 visible rows).
#define MENU_MUSIC_WAVE_BASE 0x3FC0

#define MENU_MUSIC_NOT_LOADED 0xFF

DEFINE_STRING_LOCAL(s_path_menu_vgm, "/NILESWAN/MENU.VGM");

static vgm_state_t menu_music_state;
static uint8_t menu_music_bank = MENU_MUSIC_NOT_LOADED;
static bool menu_music_is_default = false;
static volatile bool menu_music_playing = false;

void __attribute__((interrupt, assume_ss_data)) menu_music_interrupt_handler(void) {
    // vgm_play() restores ROM0, but leaves ROM1 pointing at the music data;
    // the menu may be using ROM1 when this interrupt fires.
    uint16_t rom1_backup = inportw(WS_CART_EXTBANK_ROM1_PORT);

    while (true) {
        outportw(WS_TIMER_HBL_RELOAD_PORT, 65535);
        uint16_t result = vgm_play(&menu_music_state);
        uint16_t ticks_elapsed = inportw(WS_TIMER_HBL_COUNTER_PORT) ^ 65535;
        if (result == VGM_PLAYBACK_FINISHED) {
            outportb(WS_TIMER_CTRL_PORT, 0);
            menu_music_playing = false;
        } else if (result > (ticks_elapsed + 1)) {
            outportw(WS_TIMER_HBL_RELOAD_PORT, result - ticks_elapsed);
        } else {
            continue;
        }
        break;
    }

    outportw(WS_CART_EXTBANK_ROM1_PORT, rom1_backup);
    ws_int_ack(WS_INT_ACK_HBL_TIMER);
}

static void menu_music_load_default(void) {
    uint8_t bank = asset_heap_alloc_banks(1);
    uint16_t prev_ram_bank = inportw(WS_CART_EXTBANK_RAM_PORT);

    ws_bank_with_flash(WS_CART_BANK_FLASH_ENABLE, {
        outportw(WS_CART_EXTBANK_RAM_PORT, bank);
        _fmemcpy(MK_FP(0x1000, 0x0000), menu_music_default_vgm, menu_music_default_vgm_size);
        outportw(WS_CART_EXTBANK_RAM_PORT, prev_ram_bank);
    });

    menu_music_bank = bank;
    menu_music_is_default = true;
}

static void menu_music_load_file(void) {
    FIL fp;

    if (f_open_far(&fp, s_path_menu_vgm, FA_READ | FA_OPEN_EXISTING) != FR_OK)
        return;

    uint32_t size = f_size(&fp);
    uint16_t banks = (size + 65535L) >> 16;
    if (size >= 0x100 && banks <= MENU_MUSIC_MAX_BANKS) {
        uint8_t bank = asset_heap_alloc_banks(banks);
        if (f_read_rom_banked(&fp, bank, size, NULL, NULL) == FR_OK) {
            menu_music_bank = bank;
            menu_music_is_default = false;
        } else {
            asset_heap_free_last_banks(banks);
        }
    }

    f_close(&fp);
}

void menu_music_init(void) {
    if (menu_music_bank != MENU_MUSIC_NOT_LOADED)
        return;

    menu_music_load_file();
    if (menu_music_bank == MENU_MUSIC_NOT_LOADED)
        menu_music_load_default();
}

bool menu_music_is_playing(void) {
    return menu_music_playing;
}

// settings.menu_music_volume (1..10) -> driver scale (0..16)
static uint8_t menu_music_volume_scale(void) {
    uint8_t level = settings.menu_music_volume;
    if (level < SETTING_MENU_MUSIC_VOLUME_MIN || level > SETTING_MENU_MUSIC_VOLUME_MAX)
        level = SETTING_MENU_MUSIC_VOLUME_DEFAULT;
    return (level * 16 + 5) / 10;
}

void menu_music_apply_volume(void) {
    if (menu_music_playing)
        vgm_ws_volume_scale = menu_music_volume_scale();
}

void menu_music_start(void) {
    if (menu_music_playing || menu_music_bank == MENU_MUSIC_NOT_LOADED)
        return;
    if (!(settings.file_flags & SETTING_FILE_MENU_MUSIC))
        return;

    ws_sound_reset();
    outportb(WS_SOUND_WAVE_BASE_PORT, WS_SOUND_WAVE_BASE_ADDR(MENU_MUSIC_WAVE_BASE));
    vgm_ws_volume_scale = menu_music_volume_scale();

    if (!vgm_init(&menu_music_state, menu_music_bank, 0)) {
        // MENU.VGM not supported (not a WonderSwan VGM?): fall back to the
        // built-in track once.
        if (menu_music_is_default)
            goto start_failed;
        menu_music_load_default();
        if (!vgm_init(&menu_music_state, menu_music_bank, 0))
            goto start_failed;
    }

    outportb(WS_SOUND_OUT_CTRL_PORT, WS_SOUND_OUT_CTRL_SPEAKER_ENABLE | WS_SOUND_OUT_CTRL_HEADPHONE_ENABLE | WS_SOUND_OUT_CTRL_SPEAKER_VOLUME_100);

    ia16_disable_irq();
    ws_int_set_handler(WS_INT_HBL_TIMER, (ia16_int_handler_t) menu_music_interrupt_handler);
    outportw(WS_TIMER_HBL_RELOAD_PORT, 2);
    outportb(WS_TIMER_CTRL_PORT, WS_TIMER_CTRL_HBL_REPEAT);
    ws_int_enable(WS_INT_ENABLE_HBL_TIMER);
    menu_music_playing = true;
    ia16_enable_irq();
    return;

start_failed:
    vgm_ws_volume_scale = 16;
    ws_sound_reset();
    menu_music_bank = MENU_MUSIC_NOT_LOADED;
}

void menu_music_stop(void) {
    ia16_disable_irq();
    if (menu_music_playing) {
        ws_int_disable(WS_INT_ENABLE_HBL_TIMER);
        outportb(WS_TIMER_CTRL_PORT, 0);
        ws_int_ack(WS_INT_ACK_HBL_TIMER);
        menu_music_playing = false;
        ia16_enable_irq();
        ws_sound_reset();
        // the VGM player plugin shares the driver: play files at full volume
        vgm_ws_volume_scale = 16;
        return;
    }
    ia16_enable_irq();
}
