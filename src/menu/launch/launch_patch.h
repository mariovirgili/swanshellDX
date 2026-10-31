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

#endif /* LAUNCH_PATCH_H_ */
