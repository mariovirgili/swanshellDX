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

#ifndef UI_SAVE_BACKUP_H_
#define UI_SAVE_BACKUP_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Save backups.
 *
 * Before the menu writes a game's save (SRAM or EEPROM) back to its file at
 * startup, the old file is copied to
 *   /NILESWAN/SAVES/<save file name without extension>/<stamp><extension>
 * e.g. /NILESWAN/SAVES/Game/2026-11-02_232410.ram, where <stamp> is the date
 * and time from the cartridge RTC, or "backup-0001"... if the clock is not
 * set. Only the newest settings.save_backups (Off/1/3/5) are kept. Saves of
 * the "flash" kind (as big as the ROM) are not backed up.
 *
 * A backup can be put back from Start > File > Save backups, or from the
 * popup shown when launching a game that has backups (if enabled); the
 * current save is backed up first, so nothing is lost.
 */

#define SAVE_BACKUP_LIST_MAX 6  // backups shown in the popup
#define SAVE_BACKUP_NAME_MAX 24 // "2026-11-02_232410.eeprom" + NUL

/// Number of backups kept (settings.save_backups is an index: Off/1/3/5).
uint8_t save_backup_keep_count(void);

/// Back up the save file at save_path (absolute or relative), then delete
/// the oldest backups beyond the setting. Nothing if the setting is Off,
/// or if the file does not exist or is empty.
int16_t save_backup_create(const char *save_path);

/// Save file name of a game file ("Game.wsc" -> "Game.ram"; patches:
/// "Game.bps" -> "Game.bps.ram") that has backups, SRAM first, then
/// EEPROM. False if neither has backups.
bool save_backup_find(char *save_path, uint16_t save_path_len, const char __far *game_name);

/// Back up the saves (SRAM, EEPROM) of a game file, as at startup (e.g.
/// before they are erased).
void save_backup_game(const char __far *game_name);

/// Delete all the save backups of a game file (and their folders).
int16_t save_backup_delete_all(const char __far *game_name);

/// Popup: "Current save" and the backups of save_path, newest first. A on a
/// backup puts it back (the current save is backed up first).
/// at_launch: shown before a game starts; B cancels the launch.
/// Returns false if cancelled with B (or Start).
bool ui_save_backup_choose(const char *save_path, bool at_launch);

#endif /* UI_SAVE_BACKUP_H_ */
