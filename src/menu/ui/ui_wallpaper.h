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

#ifndef UI_WALLPAPER_H_
#define UI_WALLPAPER_H_

/**
 * Settings > Interface > Theme > Wallpaper: choose the wallpaper among the
 * valid BMP files in /NILESWAN/WALLPAPERS (plus "None" and
 * /NILESWAN/WALLPAPER.BMP), with a live preview. Uses the file selector's
 * listing memory: the caller must rescan the directory afterwards.
 */
void ui_wallpaper_picker(void);

#endif /* UI_WALLPAPER_H_ */
