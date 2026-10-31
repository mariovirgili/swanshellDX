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

#endif /* MENU_MUSIC_H_ */
