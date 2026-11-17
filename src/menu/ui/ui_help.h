/**
 * Copyright (c) 2025, 2026 Adrian Siekierka
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

#ifndef UI_HELP_H_
#define UI_HELP_H_

#include <stdint.h>
#include <wonderful.h>

/// The help page for what is selected in the file list: a plugin's keys
/// when the file is one of its ROMs, the menu's otherwise. Black on white,
/// no wallpaper; A / X2 next page, X4 previous, B back.
void ui_help(const char __far *filename, uint8_t attrib);

#endif /* UI_HELP_H_ */
