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

#ifndef UI_ICONS_H_
#define UI_ICONS_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * File icon sets: /NILESWAN/ICONS/<name>.ICN, chosen in Settings > Theme >
 * Icons; without a valid set the built-in icons are used.
 *
 * .ICN file (4832 bytes, little endian):
 *    0  "WSIC", version (1), 3 reserved bytes
 *    8  12 colors (12-bit, 0x0RGB) for the color indexes 4..15 of the icon
 *       palettes (0: unused, 1: text, 2: row background, 3: ink - fixed)
 *   32  tile data, same format and order as the built-in icons:
 *       16color, 16color_rot (10 x 128 bytes each), 8color, 8color_rot
 *       (10 x 32), 16mono, 16mono_rot (10 x 64), 8mono, 8mono_rot (10 x 16).
 *       16x16 icons: tiles top-left, bottom-left, top-right, bottom-right.
 * The set is kept in one bank of the PSRAM asset heap.
 */

#define UI_ICONS_FILE_SIZE 4832
#define UI_ICONS_PATH_MAX 64

/// Allocate the PSRAM bank and load the set chosen in settings.icons.
/// Call once at startup, before anything else uses the asset heap later.
void ui_icons_init(void);
/// Load a set by name ("" = built-in); an invalid set falls back to the
/// built-in icons (returns false). Also applies its palette.
bool ui_icons_load(const char __far *name);
/// Is a set from the memory card in use? (checks that its memory is intact;
/// if not, falls back to the built-in icons)
bool ui_icons_custom(void);
/// Load the set again if its data was overwritten (call before redrawing
/// the file list).
void ui_icons_recover(void);
/// Write the colors 4..15 of the icon palettes (0 and 1) for the set in use.
void ui_icons_apply_palette(void);
/// Copy tile data of the set in use (offset in the .ICN file).
void ui_icons_read(void __far *dest, uint16_t offset, uint16_t length);

/// Settings > Theme > Icons. Uses the file selector's listing memory: the
/// caller must rescan the directory afterwards.
void ui_icons_picker(void);

#endif /* UI_ICONS_H_ */
