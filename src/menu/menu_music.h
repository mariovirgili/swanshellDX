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

#ifndef MENU_MUSIC_H_
#define MENU_MUSIC_H_

#include <stdbool.h>
#include <stdint.h>
#include <wonderful.h>

// Built-in background track (VGM 1.71, WonderSwan), see menu_music_data.c
extern const uint8_t __far menu_music_default_vgm[];
extern const uint16_t menu_music_default_vgm_size;

/**
 * Load the menu music into the PSRAM asset heap.
 * Uses /NILESWAN/MENU.VGM or MENU.VGZ if present and valid, otherwise the
 * built-in track.
 * Safe to call more than once.
 */
void menu_music_init(void);

/**
 * Start background music, if enabled in settings and not already playing.
 */
/// Silence everything a game may have left playing (the console is not
/// reset by a soft reset into the menu): the sound channels and their
/// volumes, sound DMA and HyperVoice.
void menu_sound_reset(void);

void menu_music_start(void);

/**
 * Stop background music. Must be called before anything that takes over
 * the sound hardware, the HBL timer or PSRAM (launching programs, players).
 */
void menu_music_stop(void);

bool menu_music_is_playing(void);

/**
 * Apply settings.menu_music_volume. Takes effect from the next note.
 */
void menu_music_apply_volume(void);

/// Can the VGM in PSRAM at bank (uncompressed) be the menu music? (size,
/// chips, no PCM, not too many events per second)
bool menu_music_track_ok(uint16_t bank);

/// "Use as menu music": check the uncompressed VGM in PSRAM at bank and copy
/// it into the menu music area (false: not usable). The music plays it from
/// the next start; the file is written by menu_music_save().
bool menu_music_take_from(uint16_t bank);

/// Write the track taken from the player to /NILESWAN/MENU.VGM (MENU.VGZ is
/// removed).
int16_t menu_music_save(void);

/// The player took its track as menu music: the file list saves it
/// (menu_music_save()) when the player returns.
void menu_music_request_install(void);
bool menu_music_take_install_request(void);

#endif /* MENU_MUSIC_H_ */
