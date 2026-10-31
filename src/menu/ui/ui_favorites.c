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
#include "strings.h"
#include "ui_favorites.h"
#include "ui_file_selector.h"

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
    strcpy(buf, list == FAVORITES_LIST_RECENT ? s_path_recent_txt : s_path_favorites_txt);
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

/* === Loading into the file selector === */

int16_t favorites_load(uint8_t list, uint16_t *count) {
    FIL in;
    line_reader_t r;
    char line[FF_LFN_BUF + 1];
    char name[32];
    uint16_t n = 0;

    *count = 0;
    list_file_name(name, list);
    int16_t result = f_open(&in, name, FA_READ | FA_OPEN_EXISTING);
    if (result == FR_NO_FILE || result == FR_NO_PATH)
        return FR_OK; // no list yet: empty
    if (result != FR_OK)
        return result;

    memset(&r, 0, sizeof(r));
    r.fp = &in;
    while (n < FILE_SELECTOR_MAX_FILES && read_line(&r, line, sizeof(line))) {
        if (line[0] != '/')
            continue;
        file_selector_entry_t __far *fno = ui_file_selector_open_fno_direct(n);
        // files that were moved or deleted are skipped (the list keeps them)
        if (f_stat(line, &fno->fno) != FR_OK || (fno->fno.fattrib & AM_DIR))
            continue;

        // fname holds the whole path; the extension is looked up in the
        // last path component only
        uint8_t ext_loc = 255;
        uint16_t i = 0;
        for (; line[i]; i++) {
            fno->fno.fname[i] = line[i];
            if (line[i] == '/')
                ext_loc = 255;
            else if (line[i] == '.')
                ext_loc = i;
        }
        fno->fno.fname[i] = 0;
        fno->extension_loc = ext_loc;
        n++;
    }
    f_close(&in);

    // keep the order of the file (no sorting)
    outportw(WS_CART_EXTBANK_RAM_PORT, FILE_SELECTOR_INDEX_BANK);
    for (uint16_t i = 0; i < n; i++)
        FILE_SELECTOR_INDEXES[i] = i;

    *count = n;
    return FR_OK;
}
