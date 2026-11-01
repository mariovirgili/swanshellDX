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

#ifndef UI_FAVORITES_H_
#define UI_FAVORITES_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * Favorites and recently launched files.
 *
 * Both lists are plain text files, one absolute path per line:
 * /NILESWAN/FAVORITE.TXT (in the order they were added) and
 * /NILESWAN/RECENT.TXT (most recent first, at most FAVORITES_RECENT_MAX).
 *
 * Game statistics: /NILESWAN/STATS.TXT, one line per launched ROM or patch,
 * sorted by launch count (highest first; on a tie, the latest launched
 * first):
 *   "0012 2026-11-01 /ROM/Game.wsc"
 * launch count (4 digits), date of the last launch from the cartridge RTC
 * ("----------" if unknown), absolute path. The first FAVORITES_PLAYED_MAX
 * lines are the "Most played" list.
 */

#define FAVORITES_LIST_NONE      0
#define FAVORITES_LIST_FAVORITES 1
#define FAVORITES_LIST_RECENT    2
#define FAVORITES_LIST_PLAYED    3

#define FAVORITES_RECENT_MAX 10
#define FAVORITES_PLAYED_MAX 20

#define FAVORITES_STATS_COUNT_MAX 9999
#define FAVORITES_STATS_DATE_LEN 10 // "YYYY-MM-DD"

/**
 * Build the absolute path of a file in the current directory.
 * Returns false if it does not fit in buf.
 */
bool favorites_full_path(char *buf, uint16_t buf_len, const char __far *name);

/**
 * Add the path to the favorites, or remove it if it is already there.
 * *added tells which of the two happened.
 */
int16_t favorites_toggle(const char *path, bool *added);

/**
 * Is the path in the favorites?
 */
bool favorites_contains(const char *path);

/**
 * Put the path at the top of the recent list.
 */
int16_t favorites_add_recent(const char *path);

/**
 * Count one more launch of the path (and the date, if the RTC is set).
 */
int16_t favorites_note_played(const char *path);

/**
 * Statistics of the path: launch count (0 if never launched) and date of
 * the last launch (date: FAVORITES_STATS_DATE_LEN + 1 bytes, "" if unknown).
 */
uint16_t favorites_get_stats(const char *path, char *date);

/**
 * Load a list into the file selector's listing (SRAM). Entries whose file no
 * longer exists are skipped; each entry's fname holds the absolute path.
 */
int16_t favorites_load(uint8_t list, uint16_t *count);

#endif /* UI_FAVORITES_H_ */
