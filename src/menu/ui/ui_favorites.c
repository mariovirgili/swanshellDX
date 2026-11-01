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
#include <string.h>
#include <ws.h>
#include <nilefs.h>
#include <ws/cart/rtc.h>
#include "cart/mcu.h"
#include "cart/rtc.h"
#include "cart/status.h"
#include "strings.h"
#include "ui_favorites.h"
#include "ui_file_selector.h"
#include "util/util.h"

/* === Line reader === */

typedef struct {
    FIL *fp;
    uint8_t pos, len;
    char buf[64];
} line_reader_t;

// Read one line (without CR/LF) into line. Lines that do not fit are
// returned empty. Returns false at the end of the file.
static bool read_line(line_reader_t *r, char *line, uint16_t line_len) {
    uint16_t n = 0;
    bool any = false, overflow = false;

    while (true) {
        if (r->pos >= r->len) {
            unsigned int br;
            if (f_read(r->fp, r->buf, sizeof(r->buf), &br) != FR_OK || br == 0)
                break;
            r->pos = 0;
            r->len = br;
        }
        char c = r->buf[r->pos++];
        any = true;
        if (c == '\n')
            break;
        if (c == '\r')
            continue;
        if (n < line_len - 1)
            line[n++] = c;
        else
            overflow = true;
    }
    line[overflow ? 0 : n] = 0;
    return any;
}

static int16_t write_line(FIL *fp, const char *line) {
    unsigned int bw;
    uint16_t len = strlen(line);
    int16_t result = f_write(fp, line, len, &bw);
    if (result == FR_OK && bw != len)
        result = FR_DISK_ERR;
    if (result == FR_OK) {
        char nl = '\n';
        result = f_write(fp, &nl, 1, &bw);
        if (result == FR_OK && bw != 1)
            result = FR_DISK_ERR;
    }
    return result;
}

static void list_file_name(char *buf, uint8_t list) {
    if (list == FAVORITES_LIST_PLAYED)
        strcpy(buf, s_path_stats_txt);
    else
        strcpy(buf, list == FAVORITES_LIST_RECENT ? s_path_recent_txt : s_path_favorites_txt);
}

// Replace the list file with the temporary file just written (or drop the
// temporary file if writing failed).
static int16_t list_commit(const char *name, const char *tmp_name, int16_t result) {
    if (result == FR_OK) {
        result = f_unlink(name);
        if (result == FR_NO_FILE)
            result = FR_OK;
        if (result == FR_OK)
            result = f_rename(tmp_name, name);
    } else {
        f_unlink(tmp_name);
    }
    return result;
}

/* === Paths === */

bool favorites_full_path(char *buf, uint16_t buf_len, const char __far *name) {
    if (f_getcwd(buf, buf_len) != FR_OK)
        return false;
    uint16_t len = strlen(buf);
    if (len == 0 || buf[len - 1] != '/') {
        if (len + 1 >= buf_len)
            return false;
        buf[len++] = '/';
    }
    while (*name) {
        if (len + 1 >= buf_len)
            return false;
        buf[len++] = *(name++);
    }
    buf[len] = 0;
    return true;
}

/* === Editing === */

#define REWRITE_TOGGLE     0
#define REWRITE_PUSH_FRONT 1

// Rewrite a list through a temporary file: drop the line equal to path
// (case-insensitive, as on FAT), then add it back at the front (recent) or,
// for a toggle, at the end only if it was not there.
static int16_t list_rewrite(uint8_t list, const char *path, uint8_t mode, bool *found) {
    FIL in, out;
    line_reader_t r;
    char line[FF_LFN_BUF + 1];
    char name[32], tmp_name[32];
    uint16_t lines = 0;

    *found = false;
    list_file_name(name, list);
    strcpy(tmp_name, s_path_list_tmp);

    int16_t result = f_open(&out, tmp_name, FA_WRITE | FA_CREATE_ALWAYS);
    if (result != FR_OK)
        return result;

    if (mode == REWRITE_PUSH_FRONT) {
        result = write_line(&out, path);
        lines++;
    }

    if (result == FR_OK && f_open(&in, name, FA_READ | FA_OPEN_EXISTING) == FR_OK) {
        memset(&r, 0, sizeof(r));
        r.fp = &in;
        while (result == FR_OK && read_line(&r, line, sizeof(line))) {
            if (!line[0])
                continue;
            if (!strcasecmp(line, path)) {
                *found = true;
                continue;
            }
            if (mode == REWRITE_PUSH_FRONT && lines >= FAVORITES_RECENT_MAX)
                break;
            result = write_line(&out, line);
            lines++;
        }
        f_close(&in);
    }

    if (result == FR_OK && mode == REWRITE_TOGGLE && !*found)
        result = write_line(&out, path);

    int16_t close_result = f_close(&out);
    if (result == FR_OK)
        result = close_result;

    return list_commit(name, tmp_name, result);
}

int16_t favorites_toggle(const char *path, bool *added) {
    bool found;
    int16_t result = list_rewrite(FAVORITES_LIST_FAVORITES, path, REWRITE_TOGGLE, &found);
    *added = !found;
    return result;
}

int16_t favorites_add_recent(const char *path) {
    bool found;
    return list_rewrite(FAVORITES_LIST_RECENT, path, REWRITE_PUSH_FRONT, &found);
}

bool favorites_contains(const char *path) {
    FIL in;
    line_reader_t r;
    char line[FF_LFN_BUF + 1];
    char name[32];
    bool found = false;

    list_file_name(name, FAVORITES_LIST_FAVORITES);
    if (f_open(&in, name, FA_READ | FA_OPEN_EXISTING) != FR_OK)
        return false;
    memset(&r, 0, sizeof(r));
    r.fp = &in;
    while (read_line(&r, line, sizeof(line))) {
        if (line[0] && !strcasecmp(line, path)) {
            found = true;
            break;
        }
    }
    f_close(&in);
    return found;
}

/* === Game statistics (STATS.TXT) === */

#define STATS_PREFIX_LEN (4 + 1 + FAVORITES_STATS_DATE_LEN + 1)

// Launch count of a STATS.TXT line, or -1 if the line is not valid.
static int16_t stats_line_count(const char *line) {
    if (strlen(line) <= STATS_PREFIX_LEN)
        return -1;
    int16_t count = 0;
    for (uint8_t i = 0; i < 4; i++) {
        if (line[i] < '0' || line[i] > '9')
            return -1;
        count = count * 10 + (line[i] - '0');
    }
    if (line[4] != ' ' || line[STATS_PREFIX_LEN - 1] != ' ' || line[STATS_PREFIX_LEN] != '/')
        return -1;
    return count;
}

static inline const char *stats_line_path(const char *line) {
    return line + STATS_PREFIX_LEN;
}

// Today's date from the cartridge RTC, "----------" if it is not available
// or not set.
static void stats_today(char *date) {
    strcpy(date, "----------");
    if (!(cart_status.present & CART_PRESENT_MCU))
        return;

    uint8_t status;
    ws_cart_rtc_datetime_t dt;
    mcu_native_start();
    bool ok = nile_mcu_native_rtc_transaction_sync(WS_CART_RTC_CTRL_CMD_READ_STATUS, NULL, 0, &status, 1) >= 1
        && !(status & WS_CART_RTC_STATUS_POWER_LOST)
        && nile_mcu_native_rtc_transaction_sync(WS_CART_RTC_CTRL_CMD_READ_DATETIME, NULL, 0, &dt, RTC_DATETIME_SIZE) >= RTC_DATETIME_SIZE;
    mcu_native_finish();
    if (!ok || !dt.date.month || !dt.date.day)
        return;

    // the RTC counts in BCD
    date[0] = '2';
    date[1] = '0';
    date[2] = util_hex_chars[dt.date.year >> 4];
    date[3] = util_hex_chars[dt.date.year & 0xF];
    date[5] = util_hex_chars[dt.date.month >> 4];
    date[6] = util_hex_chars[dt.date.month & 0xF];
    date[8] = util_hex_chars[dt.date.day >> 4];
    date[9] = util_hex_chars[dt.date.day & 0xF];
    date[4] = date[7] = '-';
}

uint16_t favorites_get_stats(const char *path, char *date) {
    FIL in;
    line_reader_t r;
    char line[STATS_PREFIX_LEN + FF_LFN_BUF + 1];
    char name[32];
    uint16_t count = 0;

    date[0] = 0;
    list_file_name(name, FAVORITES_LIST_PLAYED);
    if (f_open(&in, name, FA_READ | FA_OPEN_EXISTING) != FR_OK)
        return 0;
    memset(&r, 0, sizeof(r));
    r.fp = &in;
    while (read_line(&r, line, sizeof(line))) {
        int16_t c = stats_line_count(line);
        if (c >= 0 && !strcasecmp(stats_line_path(line), path)) {
            count = c;
            if (line[5] != '-') {
                memcpy(date, line + 5, FAVORITES_STATS_DATE_LEN);
                date[FAVORITES_STATS_DATE_LEN] = 0;
            }
            break;
        }
    }
    f_close(&in);
    return count;
}

// Write a STATS.TXT line (prefix + path) to fp.
static int16_t stats_write_line(FIL *fp, uint16_t count, const char *date, const char *path) {
    char prefix[STATS_PREFIX_LEN + 1];
    unsigned int bw;
    prefix[0] = '0' + (count / 1000) % 10;
    prefix[1] = '0' + (count / 100) % 10;
    prefix[2] = '0' + (count / 10) % 10;
    prefix[3] = '0' + count % 10;
    prefix[4] = ' ';
    memcpy(prefix + 5, date, FAVORITES_STATS_DATE_LEN);
    prefix[STATS_PREFIX_LEN - 1] = ' ';
    prefix[STATS_PREFIX_LEN] = 0;
    int16_t result = f_write(fp, prefix, STATS_PREFIX_LEN, &bw);
    if (result == FR_OK && bw != STATS_PREFIX_LEN)
        result = FR_DISK_ERR;
    if (result == FR_OK)
        result = write_line(fp, path);
    return result;
}

int16_t favorites_note_played(const char *path) {
    FIL in, out;
    line_reader_t r;
    char line[STATS_PREFIX_LEN + FF_LFN_BUF + 1];
    char date[FAVORITES_STATS_DATE_LEN + 1];
    char name[32], tmp_name[32];

    uint16_t count = favorites_get_stats(path, date);
    if (count < FAVORITES_STATS_COUNT_MAX)
        count++;
    stats_today(date);

    list_file_name(name, FAVORITES_LIST_PLAYED);
    strcpy(tmp_name, s_path_list_tmp);
    int16_t result = f_open(&out, tmp_name, FA_WRITE | FA_CREATE_ALWAYS);
    if (result != FR_OK)
        return result;

    // copy the lines, without the old one of path, and insert the new one
    // before the first line with the same or a lower count
    bool written = false;
    if (f_open(&in, name, FA_READ | FA_OPEN_EXISTING) == FR_OK) {
        memset(&r, 0, sizeof(r));
        r.fp = &in;
        while (result == FR_OK && read_line(&r, line, sizeof(line))) {
            int16_t c = stats_line_count(line);
            if (c < 0 || !strcasecmp(stats_line_path(line), path))
                continue;
            if (!written && (uint16_t) c <= count) {
                result = stats_write_line(&out, count, date, path);
                written = true;
                if (result != FR_OK)
                    break;
            }
            result = write_line(&out, line);
        }
        f_close(&in);
    }
    if (result == FR_OK && !written)
        result = stats_write_line(&out, count, date, path);

    int16_t close_result = f_close(&out);
    if (result == FR_OK)
        result = close_result;
    return list_commit(name, tmp_name, result);
}

/* === Loading into the file selector === */

int16_t favorites_load(uint8_t list, uint16_t *count) {
    FIL in;
    line_reader_t r;
    char line[STATS_PREFIX_LEN + FF_LFN_BUF + 1];
    char name[32];
    uint16_t n = 0;
    uint16_t max = list == FAVORITES_LIST_PLAYED ? FAVORITES_PLAYED_MAX : FILE_SELECTOR_MAX_FILES;

    *count = 0;
    list_file_name(name, list);
    int16_t result = f_open(&in, name, FA_READ | FA_OPEN_EXISTING);
    if (result == FR_NO_FILE || result == FR_NO_PATH)
        return FR_OK; // no list yet: empty
    if (result != FR_OK)
        return result;

    memset(&r, 0, sizeof(r));
    r.fp = &in;
    while (n < max && read_line(&r, line, sizeof(line))) {
        // STATS.TXT: the path follows the count and the date
        const char *path = line;
        if (list == FAVORITES_LIST_PLAYED) {
            if (stats_line_count(line) < 0)
                continue;
            path = stats_line_path(line);
        }
        if (path[0] != '/')
            continue;
        file_selector_entry_t __far *fno = ui_file_selector_open_fno_direct(n);
        // files that were moved or deleted are skipped (the list keeps them)
        if (f_stat(path, &fno->fno) != FR_OK || (fno->fno.fattrib & AM_DIR))
            continue;

        // fname holds the whole path; the extension is looked up in the
        // last path component only
        uint8_t ext_loc = 255;
        uint16_t i = 0;
        for (; path[i]; i++) {
            fno->fno.fname[i] = path[i];
            if (path[i] == '/')
                ext_loc = 255;
            else if (path[i] == '.')
                ext_loc = i;
        }
        fno->fno.fname[i] = 0;
        fno->extension_loc = ext_loc;
        n++;
    }
    f_close(&in);

    // keep the order of the file (no sorting)
    outportw(WS_CART_EXTBANK_RAM_PORT, FILE_SELECTOR_INDEX_BANK);
    FILE_SELECTOR_MEMORY_BARRIER();
    for (uint16_t i = 0; i < n; i++)
        FILE_SELECTOR_INDEXES[i] = i;

    *count = n;
    return FR_OK;
}
