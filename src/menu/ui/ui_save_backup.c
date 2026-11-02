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

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ws.h>
#include <ws/cart/rtc.h>
#include <nilefs.h>
#include "cart/mcu.h"
#include "cart/rtc.h"
#include "cart/status.h"
#include "lang.h"
#include "lang_gen.h"
#include "main.h"
#include "settings.h"
#include "strings.h"
#include "ui/ui.h"
#include "ui/ui_dialog.h"
#include "ui/ui_popup_list.h"
#include "ui_save_backup.h"

// The copy goes through PSRAM bank 0, the scratch area. At startup the
// "flash" saves (the ROM image in PSRAM) are written back before the SRAM
// and EEPROM ones (SAVE.INI lists them first), so nothing is lost there.
#define SAVE_BACKUP_PSRAM_BANK 0
#define SAVE_BACKUP_COPY_CHUNK 0x8000

#define STAMP_LEN 17 // "2026-11-02_232410"
#define LABEL_MAX 20

DEFINE_STRING_LOCAL(s_backup_number_tpl, "backup-%04u");
DEFINE_STRING_LOCAL(s_backup_prefix, "backup-");

static const uint8_t __far keep_counts[] = {0, 1, 3, 5};

uint8_t save_backup_keep_count(void) {
    return settings.save_backups < sizeof(keep_counts) ? keep_counts[settings.save_backups] : 0;
}

/* === Paths === */

// Last path component.
static const char *path_name(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            name = p + 1;
    return name;
}

// Extension of a file name (from the last '.'), or NULL.
static const char *name_ext(const char *name) {
    const char *ext = NULL;
    for (const char *p = name; *p; p++)
        if (*p == '.')
            ext = p;
    return ext;
}

// "/NILESWAN/SAVES/<save file name without extension>" into buf (returns its
// length, 0 if it does not fit); *ext = extension of the save file.
static uint16_t backup_dir(char *buf, uint16_t buf_len, const char *save_path, const char **ext) {
    const char *name = path_name(save_path);
    const char *e = name_ext(name);
    if (e == NULL || e == name)
        return 0;
    strcpy(buf, s_path_saves_dir);
    uint16_t n = strlen(buf);
    uint16_t base_len = e - name;
    // room for "/<name>/<stamp><ext>"
    if (n + 1 + base_len + 1 + SAVE_BACKUP_NAME_MAX >= buf_len)
        return 0;
    buf[n++] = '/';
    memcpy(buf + n, name, base_len);
    n += base_len;
    buf[n] = 0;
    *ext = e;
    return n;
}

/* === Listing === */

typedef struct {
    uint8_t count;  // entries in list
    uint8_t total;  // backups in the folder
    uint16_t next_number; // for names without a date
    char oldest[SAVE_BACKUP_NAME_MAX];
    char list[SAVE_BACKUP_LIST_MAX][SAVE_BACKUP_NAME_MAX]; // newest first
} backup_scan_t;

static bool is_backup_name(const char *fname, const char *ext) {
    uint16_t len = strlen(fname);
    uint16_t ext_len = strlen(ext);
    return len > ext_len && len < SAVE_BACKUP_NAME_MAX && !strcasecmp(fname + len - ext_len, ext);
}

// "backup-0012.ram" -> 12, else 0
static uint16_t backup_number(const char *fname) {
    for (uint8_t i = 0; s_backup_prefix[i]; i++) {
        char c = fname[i];
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c != s_backup_prefix[i])
            return 0;
    }
    uint16_t value = 0;
    for (const char *p = fname + 7; *p >= '0' && *p <= '9'; p++)
        value = value * 10 + (*p - '0');
    return value;
}

// Names sort by age: "2026-11-02_232410" by date and time; "backup-0003"
// (no clock) after all of them, by number.
static void backup_scan(const char *dir, const char *ext, backup_scan_t *s) {
    DIR d;
    FILINFO fno;

    memset(s, 0, sizeof(*s));
    s->next_number = 1;
    if (f_opendir(&d, dir) != FR_OK)
        return;
    while (f_readdir(&d, &fno) == FR_OK && fno.fname[0]) {
        if ((fno.fattrib & AM_DIR) || !is_backup_name(fno.fname, ext))
            continue;
        if (s->total < 255)
            s->total++;
        if (!s->oldest[0] || strcasecmp(fno.fname, s->oldest) < 0)
            strcpy(s->oldest, fno.fname);
        uint16_t number = backup_number(fno.fname);
        if (number >= s->next_number)
            s->next_number = number + 1;

        // newest first
        uint8_t pos = s->count;
        while (pos > 0 && strcasecmp(fno.fname, s->list[pos - 1]) > 0)
            pos--;
        if (pos >= SAVE_BACKUP_LIST_MAX)
            continue;
        uint8_t last = s->count < SAVE_BACKUP_LIST_MAX ? s->count : SAVE_BACKUP_LIST_MAX - 1;
        for (uint8_t i = last; i > pos; i--)
            strcpy(s->list[i], s->list[i - 1]);
        strcpy(s->list[pos], fno.fname);
        if (s->count < SAVE_BACKUP_LIST_MAX)
            s->count++;
    }
    f_closedir(&d);
}

/* === Copying === */

static int16_t copy_file(const char *src, const char *dst) {
    FIL in, out;
    int16_t result = f_open(&in, src, FA_READ | FA_OPEN_EXISTING);
    if (result != FR_OK)
        return result;
    if (!f_size(&in)) {
        // nothing to keep (or to put back)
        f_close(&in);
        return FR_NO_FILE;
    }
    result = f_open(&out, dst, FA_WRITE | FA_CREATE_ALWAYS);
    if (result != FR_OK) {
        f_close(&in);
        return result;
    }

    // (no return inside: the flash mode and RAM bank must be restored)
    ws_bank_with_flash(WS_CART_BANK_FLASH_ENABLE, {
        ws_bank_with_ram(SAVE_BACKUP_PSRAM_BANK, {
            while (result == FR_OK) {
                unsigned int br, bw;
                result = f_read(&in, MK_FP(0x1000, 0x0000), SAVE_BACKUP_COPY_CHUNK, &br);
                if (result != FR_OK || br == 0)
                    break;
                result = f_write(&out, MK_FP(0x1000, 0x0000), br, &bw);
                if (result == FR_OK && bw != br)
                    result = FR_DENIED; // card full
            }
        });
    });

    f_close(&in);
    int16_t close_result = f_close(&out);
    if (result == FR_OK)
        result = close_result;
    if (result != FR_OK)
        f_unlink(dst); // no half-written copies
    return result;
}

/* === Creating === */

// Date and time from the cartridge RTC: "2026-11-02_232410". False if the
// clock is not available or not set.
static bool backup_stamp(char *stamp) {
    if (!(cart_status.present & CART_PRESENT_MCU))
        return false;

    uint8_t status;
    ws_cart_rtc_datetime_t dt;
    mcu_native_start();
    bool ok = nile_mcu_native_rtc_transaction_sync(WS_CART_RTC_CTRL_CMD_READ_STATUS, NULL, 0, &status, 1) >= 1
        && !(status & WS_CART_RTC_STATUS_POWER_LOST)
        && nile_mcu_native_rtc_transaction_sync(WS_CART_RTC_CTRL_CMD_READ_DATETIME, NULL, 0, &dt, RTC_DATETIME_SIZE) >= RTC_DATETIME_SIZE;
    mcu_native_finish();
    if (!ok || !dt.date.month || !dt.date.day)
        return false;

    char text[RTC_DATETIME_STRING_LEN + 1];
    rtc_datetime_to_string(text, &dt); // "2026-11-02 23:24:10"
    memcpy(stamp, text, 10);
    stamp[10] = '_';
    stamp[11] = text[11];
    stamp[12] = text[12];
    stamp[13] = text[14];
    stamp[14] = text[15];
    stamp[15] = text[17];
    stamp[16] = text[18];
    stamp[STAMP_LEN] = 0;
    return true;
}

// Delete the oldest backups until at most keep are left.
static void backup_prune(char *path, uint16_t dir_len, const char *ext, uint8_t keep) {
    backup_scan_t s;
    while (true) {
        path[dir_len] = 0;
        backup_scan(path, ext, &s);
        if (s.total <= keep || !s.oldest[0])
            break;
        path[dir_len] = '/';
        strcpy(path + dir_len + 1, s.oldest);
        if (f_unlink(path) != FR_OK)
            break;
    }
    path[dir_len] = 0;
}

// path: a FF_LFN_BUF + 1 bytes work buffer
static int16_t backup_create_keep(const char *save_path, uint8_t keep, char *path) {
    const char *ext;
    uint16_t dir_len = backup_dir(path, FF_LFN_BUF + 1, save_path, &ext);
    if (!dir_len)
        return FR_OK;

    // /NILESWAN/SAVES, then the game's folder (errors: already there)
    uint16_t saves_len = path_name(path) - path - 1;
    path[saves_len] = 0;
    f_mkdir(path);
    path[saves_len] = '/';
    f_mkdir(path);

    char stamp[STAMP_LEN + 1];
    if (!backup_stamp(stamp)) {
        backup_scan_t s;
        backup_scan(path, ext, &s);
        snprintf(stamp, sizeof(stamp), s_backup_number_tpl, s.next_number);
    }
    path[dir_len] = '/';
    strcpy(path + dir_len + 1, stamp);
    strcat(path, ext);

    int16_t result = copy_file(save_path, path);
    if (result == FR_NO_FILE)
        result = FR_OK; // no save yet: nothing to back up

    backup_prune(path, dir_len, ext, keep);
    return result;
}

int16_t save_backup_create(const char *save_path) {
    char path[FF_LFN_BUF + 1];
    uint8_t keep = save_backup_keep_count();
    if (!keep)
        return FR_OK;
    return backup_create_keep(save_path, keep, path);
}

/* === Finding and choosing === */

// Save file name of a game file: "Game.wsc" -> "Game<save_ext>", patches
// "Game.bps" -> "Game.bps<save_ext>" (as launch_restore_save_data() does).
static bool save_name(char *buf, uint16_t buf_len, const char __far *game_name, const char __far *save_ext) {
    uint16_t len = 0, dot = 0xFFFF;
    for (; game_name[len]; len++) {
        if (len + 1 >= buf_len)
            return false;
        buf[len] = game_name[len];
        if (buf[len] == '.')
            dot = len;
        else if (buf[len] == '/')
            dot = 0xFFFF;
    }
    buf[len] = 0;
    // ROMs: the save replaces the extension; patches keep theirs
    if (dot != 0xFFFF && strcasecmp(buf + dot, s_file_ext_ips) && strcasecmp(buf + dot, s_file_ext_bps))
        len = dot;
    while (*save_ext) {
        if (len + 1 >= buf_len)
            return false;
        buf[len++] = *(save_ext++);
    }
    buf[len] = 0;
    return true;
}

static bool has_backups(const char *save_path) {
    char path[FF_LFN_BUF + 1];
    const char *ext;
    backup_scan_t s;
    if (!backup_dir(path, sizeof(path), save_path, &ext))
        return false;
    backup_scan(path, ext, &s);
    return s.count != 0;
}

bool save_backup_find(char *save_path, uint16_t save_path_len, const char __far *game_name) {
    if (save_name(save_path, save_path_len, game_name, s_file_ext_sram) && has_backups(save_path))
        return true;
    if (save_name(save_path, save_path_len, game_name, s_file_ext_eeprom) && has_backups(save_path))
        return true;
    return false;
}

// "2026-11-02_232410.ram" -> "2026-11-02 23:24"; else the name without
// its extension
static void backup_label(char *label, const char *name, const char *ext) {
    if (strlen(name) >= STAMP_LEN && name[4] == '-' && name[7] == '-' && name[10] == '_') {
        memcpy(label, name, 10);
        label[10] = ' ';
        label[11] = name[11];
        label[12] = name[12];
        label[13] = ':';
        label[14] = name[13];
        label[15] = name[14];
        label[16] = 0;
    } else {
        uint16_t len = strlen(name) - strlen(ext);
        if (len > LABEL_MAX - 1)
            len = LABEL_MAX - 1;
        memcpy(label, name, len);
        label[len] = 0;
    }
}

// Put a backup back: the current save is backed up first (keeping one more
// than the setting, so that the chosen backup is not deleted yet).
// path: a FF_LFN_BUF + 1 bytes work buffer
static int16_t backup_restore(const char *save_path, const char *backup_name, char *path) {
    uint8_t keep = save_backup_keep_count();
    int16_t result = backup_create_keep(save_path, (keep ? keep : 1) + 1, path);
    if (result != FR_OK)
        return result;

    const char *ext;
    uint16_t dir_len = backup_dir(path, FF_LFN_BUF + 1, save_path, &ext);
    if (!dir_len)
        return FR_INVALID_NAME;
    path[dir_len] = '/';
    strcpy(path + dir_len + 1, backup_name);
    return copy_file(path, save_path);
}

bool ui_save_backup_choose(const char *save_path, bool at_launch) {
    char path[FF_LFN_BUF + 1];
    char labels[SAVE_BACKUP_LIST_MAX][LABEL_MAX];
    const char *ext;
    backup_scan_t s;

    if (!backup_dir(path, sizeof(path), save_path, &ext))
        return true;
    backup_scan(path, ext, &s);
    if (!s.count)
        return true;

    ui_popup_list_config_t lst;
    memset(&lst, 0, sizeof(lst));
    lst.option[0] = lang_keys[LK_UI_SAVE_BACKUP_CURRENT];
    for (uint8_t i = 0; i < s.count; i++) {
        backup_label(labels[i], s.list[i], ext);
        lst.option[i + 1] = labels[i];
    }

    int result = ui_popup_list(&lst);
    ui_popup_list_clear(&lst);
    if (result < 0)
        return false; // B / Start: back to the list
    if (result == 0)
        return true;  // the current save

    ui_draw_statusbar(lang_keys[LK_UI_STATUS_LOADING]);
    int16_t restore_result = backup_restore(save_path, s.list[result - 1], path);
    if (ui_dialog_error_check(restore_result, lang_keys[LK_UI_SAVE_BACKUP_TITLE], 0))
        return !at_launch; // do not start a game with a half-restored save
    if (!at_launch) {
        ui_draw_statusbar(lang_keys[LK_UI_STATUS_SAVE_RESTORED]);
        for (uint8_t i = 0; i < 45; i++)
            wait_for_vblank();
    }
    return true;
}
