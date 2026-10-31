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
#include "util/puff/puff.h"
#include "plugin/vgm/vgm.h"

// Largest MENU.VGM/VGZ accepted, in 64 KiB banks (uncompressed size).
#define MENU_MUSIC_MAX_BANKS 2
// Wavetable RAM: last row of the screen 1 map (unused, 18 visible rows).
#define MENU_MUSIC_WAVE_BASE 0x3FC0

#define MENU_MUSIC_NOT_LOADED 0xFF

DEFINE_STRING_LOCAL(s_path_menu_vgm, "/NILESWAN/MENU.VGM");
DEFINE_STRING_LOCAL(s_path_menu_vgz, "/NILESWAN/MENU.VGZ");

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

// Decompress a gzip file already in PSRAM (src_bank) into dest_bank.
static bool menu_music_gunzip(uint8_t src_bank, uint32_t size, uint8_t dest_bank) {
    bool ok = false;
    uint16_t prev_rom0 = inportw(WS_CART_EXTBANK_ROM0_PORT);
    uint16_t prev_rom1 = inportw(WS_CART_EXTBANK_ROM1_PORT);
    uint16_t prev_ram = inportw(WS_CART_EXTBANK_RAM_PORT);

    outportw(WS_CART_EXTBANK_ROM0_PORT, src_bank);
    outportw(WS_CART_EXTBANK_ROM1_PORT, src_bank + 1);
    const uint8_t __far *h = MK_FP(WS_ROM0_SEGMENT, 0x0000);

    // gzip header: DEFLATE only, no reserved flags
    if (h[2] == 8 && !(h[3] & 0xE0)) {
        uint16_t offset = 10;
        if (h[3] & 0x04) offset += *((const uint16_t __far*) (h + offset)) + 2; // FEXTRA
        if (h[3] & 0x08) { while (h[offset++]); }                                 // FNAME
        if (h[3] & 0x10) { while (h[offset++]); }                                 // FCOMMENT
        if (h[3] & 0x02) offset += 2;                                            // FHCRC

        if (offset < size) {
            ws_bank_with_flash(WS_CART_BANK_FLASH_ENABLE, {
                outportw(WS_CART_EXTBANK_RAM_PORT, dest_bank);
                ok = puff((unsigned char __far*) MK_FP(0x1000, 0x0000),
                    (uint32_t) MENU_MUSIC_MAX_BANKS << 16,
                    (const unsigned char __far*) MK_FP(WS_ROM0_SEGMENT, offset),
                    size - offset) == 0;
            });
        }
    }

    outportw(WS_CART_EXTBANK_RAM_PORT, prev_ram);
    outportw(WS_CART_EXTBANK_ROM1_PORT, prev_rom1);
    outportw(WS_CART_EXTBANK_ROM0_PORT, prev_rom0);
    return ok;
}

static inline uint8_t menu_music_peek(uint32_t pos) {
    // ROM0 and ROM1 are contiguous (0x20000-0x3FFFF), so up to 128 KiB can
    // be addressed linearly once ROM1 = ROM0 + 1.
    return *((const uint8_t __far*) MK_FP(WS_ROM0_SEGMENT + (uint16_t) (pos >> 4), (uint16_t) pos & 0xF));
}

static inline uint32_t menu_music_peek32(uint16_t pos) {
    return *((const uint32_t __far*) MK_FP(WS_ROM0_SEGMENT, pos));
}

// Max player interrupts per second for background music. Normal tracks need
// tens to a few hundred; sample playback through register writes needs
// thousands and would leave no CPU time for the menu.
#define MENU_MUSIC_MAX_EVENTS_PER_SEC 1500

// Check a VGM loaded at bank before using it as background music:
// only commands of the chips the player supports, WonderSwan writes limited
// to the sound registers, no PCM data blocks/streams (PCM would be copied
// over the menu's tile memory), no Ch2 "voice" sample playback, and not too
// many player events per second. A command the player does not know would
// make it lose its place in the file and write to arbitrary ports.
static bool menu_music_validate(uint8_t bank, uint32_t size) {
    bool ok = false;
    uint16_t prev_rom0 = inportw(WS_CART_EXTBANK_ROM0_PORT);
    uint16_t prev_rom1 = inportw(WS_CART_EXTBANK_ROM1_PORT);
    outportw(WS_CART_EXTBANK_ROM0_PORT, bank);
    outportw(WS_CART_EXTBANK_ROM1_PORT, bank + 1);

    uint32_t end = menu_music_peek32(0x04) + 4;
    uint32_t version = menu_music_peek32(0x08);
    uint32_t pos = 0x40;
    if (version >= 0x150 && menu_music_peek32(0x34) != 0)
        pos = menu_music_peek32(0x34) + 0x34;
    if (end > size)
        end = size;

    uint32_t total_samples = 0;
    uint32_t wait_count = 0;

    while (pos < end) {
        uint8_t cmd = menu_music_peek(pos);
        if (cmd == 0x66) {             // end of data
            // each wait is one player interrupt: limit the average rate
            ok = total_samples > 0 && wait_count * (44100 / MENU_MUSIC_MAX_EVENTS_PER_SEC) <= total_samples;
            break;
        }
        if (cmd >= 0x70 && cmd <= 0x7F) { // short wait
            total_samples += (cmd & 0x0F) + 1;
            wait_count++;
            pos += 1;
            continue;
        }
        switch (cmd) {
        case 0x62: total_samples += 735; wait_count++; pos += 1; break;
        case 0x63: total_samples += 882; wait_count++; pos += 1; break;
        case 0x61:                              // wait n samples
            total_samples += menu_music_peek(pos + 1) | ((uint16_t) menu_music_peek(pos + 2) << 8);
            wait_count++;
            pos += 3;
            break;
        case 0x4F: case 0x50: pos += 2; break; // SN76489
        case 0xB3: pos += 3; break;            // Game Boy DMG
        case 0xBC:                             // WonderSwan register write
            if (menu_music_peek(pos + 1) >= 0x20)
                goto done;
            // channel control with Ch2 voice (sample) mode
            if (menu_music_peek(pos + 1) == 0x10 && (menu_music_peek(pos + 2) & 0x20))
                goto done;
            pos += 3;
            break;
        case 0xC6: pos += 4; break;            // WonderSwan wavetable write
        default:                               // PCM (0x67, 0x68, 0x80-0x8F,
            goto done;                         // 0x90-0x95) or unsupported
        }
    }

done:
    outportw(WS_CART_EXTBANK_ROM1_PORT, prev_rom1);
    outportw(WS_CART_EXTBANK_ROM0_PORT, prev_rom0);
    return ok;
}

// Load a .vgm or gzip-compressed .vgz (detected by content, not extension).
static bool menu_music_load_file(const char __far *path) {
    FIL fp;
    uint8_t magic[2];
    unsigned int br;
    bool ok = false;

    if (f_open_far(&fp, path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
        return false;

    uint32_t size = f_size(&fp);
    uint16_t src_banks = (size + 65535L) >> 16;
    if (size < 0x20 || src_banks > MENU_MUSIC_MAX_BANKS)
        goto end;
    if (f_read(&fp, magic, 2, &br) != FR_OK || br != 2 || f_lseek(&fp, 0) != FR_OK)
        goto end;

    if (magic[0] == 0x1F && magic[1] == 0x8B) {
        // asset heap grows downwards: destination first, so it stays
        // allocated after the temporary source banks are freed
        uint8_t dest_bank = asset_heap_alloc_banks(MENU_MUSIC_MAX_BANKS);
        uint8_t src_bank = asset_heap_alloc_banks(src_banks);
        if (f_read_rom_banked(&fp, src_bank, size, NULL, NULL) == FR_OK)
            ok = menu_music_gunzip(src_bank, size, dest_bank);
        asset_heap_free_last_banks(src_banks);
        if (ok)
            ok = menu_music_validate(dest_bank, (uint32_t) MENU_MUSIC_MAX_BANKS << 16);
        if (ok)
            menu_music_bank = dest_bank;
        else
            asset_heap_free_last_banks(MENU_MUSIC_MAX_BANKS);
    } else {
        uint8_t bank = asset_heap_alloc_banks(src_banks);
        if (f_read_rom_banked(&fp, bank, size, NULL, NULL) == FR_OK
            && menu_music_validate(bank, size)) {
            menu_music_bank = bank;
            ok = true;
        } else {
            asset_heap_free_last_banks(src_banks);
        }
    }

end:
    f_close(&fp);
    if (ok)
        menu_music_is_default = false;
    return ok;
}

void menu_music_init(void) {
    if (menu_music_bank != MENU_MUSIC_NOT_LOADED)
        return;

    if (!menu_music_load_file(s_path_menu_vgm))
        menu_music_load_file(s_path_menu_vgz);
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
    // vgm_init() clears the state: set background mode afterwards.
    menu_music_state.flags |= VGM_FLAG_NO_PCM;

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
