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

#ifndef LAUNCH_PATCH_H_
#define LAUNCH_PATCH_H_

#include <stdbool.h>
#include <stdint.h>
#include "launch.h"

#include <stdint.h>

/**
 * Launch a ROM with an IPS or BPS patch applied on the fly.
 *
 * patch_path is the selected .ips/.bps file. The ROM is looked up next to it:
 * "Game.bps" -> "Game.wsc", "Game.ws", "Game.pc2" (or "Game.wsc.bps" ->
 * "Game.wsc"). The ROM is loaded into PSRAM, patched there and launched;
 * the files on the storage card are not modified. Save data is kept in
 * "<patch file name>.ram" (etc.), separate from the unpatched ROM's.
 *
 * Only returns on error.
 */
int16_t launch_patched_rom(const char *patch_path);

/**
 * SwanShellDX, START + Y1 back to the menu: load the ROM into PSRAM and
 * patch it, with its variant in /NILESWAN/PATCHES (named after the ROM
 * footer, see the .c file) or else the automatic patch, and leave it there
 * for launch_rom_via_bootstub() (prog.cluster/size set; launch_keypad_patched
 * true if patched).
 *
 * Returns FR_OK when the ROM is in PSRAM (patched or not), FR_NO_FILE when
 * the caller should launch it as usual (option off, not a WonderSwan ROM,
 * too big, read error).
 */
int16_t launch_keypad_patch(const char *path, const launch_rom_metadata_t *meta);

/// Set by launch_keypad_patch(); read and cleared by launch_rom_via_bootstub().
extern bool launch_keypad_patched;

#endif /* LAUNCH_PATCH_H_ */
