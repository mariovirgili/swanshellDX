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

#ifndef UI_GAME_IMAGES_H_
#define UI_GAME_IMAGES_H_

#include <stdbool.h>
#include <stdint.h>
#include <wonderful.h>

/**
 * Pictures of a game: up to GAME_IMAGES_MAX valid wallpaper-format BMP files
 * (224x144, 16 colors) in /NILESWAN/IMAGES/<name>/, where <name> is the file
 * name without its extension ("Game.wsc" -> "Game"; for "Game.wsc.bps" also
 * "Game.wsc" then "Game"). Shown in alphabetical order.
 */

#define GAME_IMAGES_MAX 6
#define GAME_IMAGES_NAME_MAX 48
#define GAME_IMAGES_PATH_MAX 300

typedef struct {
    uint8_t count;
    char dir[GAME_IMAGES_PATH_MAX];      ///< /NILESWAN/IMAGES/<name>
    char name[GAME_IMAGES_MAX][GAME_IMAGES_NAME_MAX];
    char tmp[GAME_IMAGES_PATH_MAX];      ///< work buffer
} game_images_t;

/// The (single) game_images_t, kept in the upper half of the sector buffer
/// (internal RAM is nearly full; the lower half is used by name scrolling).
/// Color mode only.
game_images_t *game_images_buffer(void);

/// Find the pictures of a file (name only, no folder). Returns the count.
uint8_t game_images_find(const char __far *file_name, game_images_t *images);

/// Full path of picture i in buf (GAME_IMAGES_PATH_MAX bytes).
bool game_images_path(const game_images_t *images, uint8_t i, char *buf);

/**
 * Full-screen viewer. Left/Right: picture, Up: hide/show the bars,
 * A: play (returns true), B/Start: back (returns false).
 * The wallpaper is restored on exit.
 */
bool game_images_view(game_images_t *images, const char __far *title);

#endif /* UI_GAME_IMAGES_H_ */
