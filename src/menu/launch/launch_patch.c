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

/*
 * On-the-fly IPS/BPS patching: the ROM is loaded into PSRAM, patched there
 * and launched through the bootstub "already in PSRAM" path (the same one
 * used by XMODEM uploads). Nothing on the storage card is modified.
 *
 * PSRAM layout (64 KiB banks, accessed through the cartridge RAM window in
 * flash mode for writing and through the ROM0 window for reading):
 * - IPS: ROM at bank 0, patched in place (it may grow).
 * - BPS: target (patched ROM) built at bank 0, source (original ROM)
 *        loaded right after it, at the first bank boundary past the target.
 * Everything must stay below the asset heap (fonts, menu music).
 *
 * BPS checksums are not verified: CRC32 over a multi-megabyte ROM would take
 * many seconds on the WonderSwan CPU. The source size is checked instead.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wonderful.h>
#include <ws.h>
#include <nile.h>
#include <nilefs.h>
#include "bootstub.h"
#include "cart/mcu.h"
#include "cart/status.h"
#include "errors.h"
#include "lang.h"
#include "launch.h"
#include "launch_patch.h"
#include "plugin/plugin.h"
#include "strings.h"
#include "ui/ui.h"
#include "util/asset_heap.h"
#include "util/file.h"
#include "util/ini.h"
#include "settings.h"
#include <stdio.h>

#define PSRAM_BANK_SIZE 0x10000UL

/* === Progress in the status bar ===
 * Redrawn only when the percentage or the stage changes. The bank registers
 * are saved around drawing, since this runs in the middle of PSRAM work. */

static uint16_t progress_lk = 0xFFFF;
static uint8_t progress_pct = 0xFF;
static char progress_text[48];

static void progress_show(uint16_t lk, uint32_t step, uint32_t max) {
    uint8_t pct = (max == 0 || step >= max) ? 100 : (uint8_t) ((step * 100) / max);
    if (lk == progress_lk && pct == progress_pct)
        return;
    progress_lk = lk;
    progress_pct = pct;

    uint16_t ram = inportw(WS_CART_EXTBANK_RAM_PORT);
    uint16_t rom0 = inportw(WS_CART_EXTBANK_ROM0_PORT);
    uint16_t rom1 = inportw(WS_CART_EXTBANK_ROM1_PORT);
    snprintf(progress_text, sizeof(progress_text), lang_keys[lk], pct);
    ui_draw_statusbar(progress_text);
    outportw(WS_CART_EXTBANK_ROM1_PORT, rom1);
    outportw(WS_CART_EXTBANK_ROM0_PORT, rom0);
    outportw(WS_CART_EXTBANK_RAM_PORT, ram);
}

static void progress_load_cb(void *userdata, uint32_t step, uint32_t max) {
    progress_show(LK_UI_STATUS_PATCH_LOADING, step, max);
}

/* === PSRAM access ===
 * Callers enable flash mode (WS_CART_BANK_FLASH_ENABLE) for writes; the
 * RAM and ROM0 bank registers are saved/restored by launch_patched_rom(). */

static void psram_write(uint32_t addr, const uint8_t *src, uint16_t len) {
    while (len) {
        uint16_t ofs = addr & 0xFFFF;
        uint32_t room = PSRAM_BANK_SIZE - ofs;
        uint16_t n = room < len ? (uint16_t) room : len;
        outportw(WS_CART_EXTBANK_RAM_PORT, addr >> 16);
        _fmemcpy(MK_FP(0x1000, ofs), src, n);
        src += n;
        addr += n;
        len -= n;
    }
}

static void psram_fill(uint32_t addr, uint8_t value, uint32_t len) {
    uint32_t total = len;
    bool report = len >= 0x40000;
    while (len) {
        if (report)
            progress_show(LK_UI_STATUS_PATCH_EXPANDING, total - len, total);
        uint16_t ofs = addr & 0xFFFF;
        uint32_t n = PSRAM_BANK_SIZE - ofs;
        if (n > len) n = len;
        outportw(WS_CART_EXTBANK_RAM_PORT, addr >> 16);
        uint8_t __far *d = MK_FP(0x1000, ofs);
        for (uint32_t i = 0; i < n; i++)
            *(d++) = value;
        addr += n;
        len -= n;
    }
}

static inline uint8_t psram_read_byte(uint32_t addr) {
    outportw(WS_CART_EXTBANK_ROM0_PORT, addr >> 16);
    return *((const uint8_t __far*) MK_FP(WS_ROM0_SEGMENT, (uint16_t) addr));
}

// Copy within PSRAM. Overlapping ranges with dst > src are copied byte by
// byte, front to back, as BPS TargetCopy requires (it may repeat a pattern).
static void psram_copy(uint32_t dst, uint32_t src, uint32_t len) {
    if (dst > src && dst < src + len) {
        while (len--) {
            uint8_t b = psram_read_byte(src++);
            outportw(WS_CART_EXTBANK_RAM_PORT, dst >> 16);
            *((uint8_t __far*) MK_FP(0x1000, (uint16_t) dst)) = b;
            dst++;
        }
        return;
    }
    while (len) {
        uint16_t so = src & 0xFFFF;
        uint16_t dof = dst & 0xFFFF;
        uint32_t n = PSRAM_BANK_SIZE - so;
        if (PSRAM_BANK_SIZE - dof < n) n = PSRAM_BANK_SIZE - dof;
        if (n > len) n = len;
        if (n > 0x8000) n = 0x8000;
        outportw(WS_CART_EXTBANK_ROM0_PORT, src >> 16);
        outportw(WS_CART_EXTBANK_RAM_PORT, dst >> 16);
        _fmemcpy(MK_FP(0x1000, dof), MK_FP(WS_ROM0_SEGMENT, so), (uint16_t) n);
        src += n;
        dst += n;
        len -= n;
    }
}

/* === Buffered patch file reader === */

typedef struct {
    FIL fp;
    uint16_t pos, len;
    bool error;
    uint8_t buf[256];
} patch_reader_t;

static patch_reader_t reader;

static bool reader_fill(patch_reader_t *r) {
    unsigned int br;
    if (f_read(&r->fp, r->buf, sizeof(r->buf), &br) != FR_OK || br == 0) {
        r->error = true;
        return false;
    }
    r->pos = 0;
    r->len = br;
    return true;
}

static uint8_t reader_byte(patch_reader_t *r) {
    if (r->pos >= r->len && !reader_fill(r))
        return 0;
    return r->buf[r->pos++];
}

static uint32_t reader_be(patch_reader_t *r, uint8_t bytes) {
    uint32_t v = 0;
    while (bytes--)
        v = (v << 8) | reader_byte(r);
    return v;
}

static uint32_t reader_tell(patch_reader_t *r) {
    return f_tell(&r->fp) - (r->len - r->pos);
}

// Copy len bytes from the patch file to PSRAM at addr.
static void reader_to_psram(patch_reader_t *r, uint32_t addr, uint32_t len) {
    while (len && !r->error) {
        if (r->pos >= r->len && !reader_fill(r))
            return;
        uint16_t n = r->len - r->pos;
        if (n > len) n = len;
        psram_write(addr, r->buf + r->pos, n);
        r->pos += n;
        addr += n;
        len -= n;
    }
}

static bool reader_magic(patch_reader_t *r, const char *magic) {
    while (*magic)
        if (reader_byte(r) != (uint8_t) *(magic++))
            return false;
    return !r->error;
}

// BPS variable-length number
static uint32_t reader_bps_number(patch_reader_t *r) {
    uint32_t data = 0, shift = 1;
    for (uint8_t i = 0; i < 5; i++) {
        uint8_t x = reader_byte(r);
        data += (x & 0x7F) * shift;
        if (x & 0x80)
            return data;
        shift <<= 7;
        data += shift;
    }
    r->error = true; // longer than 32 bits: not a sane ROM patch
    return 0;
}

/* === IPS === */

static int16_t apply_ips(patch_reader_t *r, uint32_t *size, uint32_t max_size, uint32_t patch_size) {
    if (!reader_magic(r, "PATCH"))
        return ERR_FILE_FORMAT_INVALID;

    uint32_t end = *size;
    while (true) {
        progress_show(LK_UI_STATUS_PATCH_APPLYING, reader_tell(r), patch_size);
        uint32_t ofs = reader_be(r, 3);
        if (r->error)
            return ERR_FILE_FORMAT_INVALID;
        if (ofs == 0x454F46) { // "EOF"
            // optional 24-bit truncation size
            uint32_t truncate = reader_be(r, 3);
            if (!r->error && truncate > 0 && truncate < end)
                end = truncate;
            break;
        }

        uint16_t len = reader_be(r, 2);
        bool rle = len == 0;
        uint8_t value = 0;
        if (rle) {
            len = reader_be(r, 2);
            value = reader_byte(r);
        }
        if (r->error)
            return ERR_FILE_FORMAT_INVALID;
        if (ofs + len > max_size)
            return ERR_FILE_TOO_LARGE;

        // writing past the end grows the ROM: zero the gap, like PC tools
        if (ofs > end)
            psram_fill(end, 0x00, ofs - end);

        if (rle)
            psram_fill(ofs, value, len);
        else
            reader_to_psram(r, ofs, len);
        if (r->error)
            return ERR_FILE_FORMAT_INVALID;

        if (ofs + len > end)
            end = ofs + len;
    }

    *size = end;
    return FR_OK;
}

/* === BPS === */

typedef struct {
    uint32_t source_size;
    uint32_t target_size;
} bps_header_t;

static int16_t read_bps_header(patch_reader_t *r, bps_header_t *hdr) {
    if (!reader_magic(r, "BPS1"))
        return ERR_FILE_FORMAT_INVALID;
    hdr->source_size = reader_bps_number(r);
    hdr->target_size = reader_bps_number(r);
    uint32_t metadata_size = reader_bps_number(r);
    while (metadata_size-- && !r->error)
        reader_byte(r);
    return r->error ? ERR_FILE_FORMAT_INVALID : FR_OK;
}

// target at PSRAM 0, source at source_base
static int16_t apply_bps(patch_reader_t *r, const bps_header_t *hdr, uint32_t source_base, uint32_t patch_size) {
    uint32_t actions_end = patch_size - 12; // footer: 3x CRC32
    uint32_t out = 0;
    int32_t source_rel = 0, target_rel = 0;

    while (reader_tell(r) < actions_end) {
        progress_show(LK_UI_STATUS_PATCH_APPLYING, out, hdr->target_size);
        uint32_t data = reader_bps_number(r);
        uint8_t command = data & 3;
        uint32_t len = (data >> 2) + 1;
        if (r->error || out + len > hdr->target_size)
            return ERR_FILE_FORMAT_INVALID;

        switch (command) {
        case 0: // SourceRead
            if (out + len > hdr->source_size)
                return ERR_FILE_FORMAT_INVALID;
            psram_copy(out, source_base + out, len);
            break;
        case 1: // TargetRead
            reader_to_psram(r, out, len);
            break;
        case 2: { // SourceCopy
            uint32_t d = reader_bps_number(r);
            source_rel += (d & 1) ? -(int32_t) (d >> 1) : (int32_t) (d >> 1);
            if (source_rel < 0 || (uint32_t) source_rel + len > hdr->source_size)
                return ERR_FILE_FORMAT_INVALID;
            psram_copy(out, source_base + source_rel, len);
            source_rel += len;
        } break;
        case 3: { // TargetCopy
            uint32_t d = reader_bps_number(r);
            target_rel += (d & 1) ? -(int32_t) (d >> 1) : (int32_t) (d >> 1);
            if (target_rel < 0 || (uint32_t) target_rel >= out)
                return ERR_FILE_FORMAT_INVALID;
            psram_copy(out, target_rel, len);
            target_rel += len;
        } break;
        }
        if (r->error)
            return ERR_FILE_FORMAT_INVALID;
        out += len;
    }

    return out == hdr->target_size ? FR_OK : ERR_FILE_FORMAT_INVALID;
}

/* === Launch === */

static char rom_path[FF_LFN_BUF + 8];
static char save_path[FF_LFN_BUF + 8];

// Last '.' in a near string (strrchr() returns a far pointer here).
static char *last_dot(char *s) {
    char *dot = NULL;
    for (; *s; s++)
        if (*s == '.')
            dot = s;
    return dot;
}

static bool file_exists(const char *path) {
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK && !(fno.fattrib & AM_DIR);
}

// "Game.bps" -> "Game.wsc" / "Game.ws" / "Game.pc2"; "Game.wsc.bps" -> "Game.wsc"
static bool find_rom_for_patch(const char *patch_path) {
    strncpy(rom_path, patch_path, sizeof(rom_path) - 5);
    rom_path[sizeof(rom_path) - 5] = 0;
    char *dot = last_dot(rom_path);
    if (dot == NULL)
        return false;
    *dot = 0;

    const char *inner = last_dot(rom_path);
    if (inner != NULL && (!strcasecmp(inner, s_file_ext_wsc) || !strcasecmp(inner, s_file_ext_ws)
        || !strcasecmp(inner, s_file_ext_pc2)) && file_exists(rom_path))
        return true;

    strcpy(dot, s_file_ext_wsc);
    if (file_exists(rom_path)) return true;
    strcpy(dot, s_file_ext_ws);
    if (file_exists(rom_path)) return true;
    strcpy(dot, s_file_ext_pc2);
    if (file_exists(rom_path)) return true;
    return false;
}

static int16_t load_rom_to_psram(const char *path, uint16_t bank, uint32_t *size) {
    FIL fp;
    int16_t result = f_open(&fp, path, FA_OPEN_EXISTING | FA_READ);
    if (result != FR_OK)
        return result;
    *size = f_size(&fp);
    result = f_read_rom_banked(&fp, bank, *size, progress_load_cb, NULL);
    f_close(&fp);
    return result;
}

int16_t launch_patched_rom(const char *patch_path) {
    launch_rom_metadata_t file_meta, meta;
    uint32_t rom_size = 0, size = 0;
    int16_t result;

    if (!find_rom_for_patch(patch_path))
        return FR_NO_FILE;

    // Validates the original ROM; also gives its type (WS/WSC/PCv2/Freya)
    result = launch_get_rom_metadata(rom_path, &file_meta);
    if (result != FR_OK)
        return result;
    if (file_meta.rom_type == ROM_TYPE_FREYA)
        return ERR_FILE_NOT_EXECUTABLE;

    memset(&reader, 0, sizeof(reader));
    result = f_open(&reader.fp, patch_path, FA_OPEN_EXISTING | FA_READ);
    if (result != FR_OK)
        return result;
    uint32_t patch_id = reader.fp.obj.sclust; // save ID, distinct from the plain ROM
    uint32_t patch_size = f_size(&reader.fp);
    uint32_t max_size = (uint32_t) asset_heap_get_free_first_banks() << 16;
    // rom_path holds a copy of patch_path's directory/name; reuse save_path
    // as scratch to look at the patch extension without strrchr()
    strncpy(save_path, patch_path, sizeof(save_path) - 1);
    save_path[sizeof(save_path) - 1] = 0;
    const char *patch_ext = last_dot(save_path);
    bool is_bps = patch_ext != NULL && !strcasecmp(patch_ext, s_file_ext_bps);

    ui_draw_titlebar_filename(patch_path);
    progress_lk = 0xFFFF;
    progress_show(LK_UI_STATUS_PATCH_LOADING, 0, 1);

    uint16_t prev_ram = inportw(WS_CART_EXTBANK_RAM_PORT);
    uint16_t prev_rom0 = inportw(WS_CART_EXTBANK_ROM0_PORT);
    uint8_t prev_flash = inportb(WS_CART_BANK_FLASH_PORT);

    if (is_bps) {
        bps_header_t hdr;
        result = read_bps_header(&reader, &hdr);
        if (result == FR_OK && patch_size < 12)
            result = ERR_FILE_FORMAT_INVALID;
        uint32_t source_base = (hdr.target_size + PSRAM_BANK_SIZE - 1) & ~(PSRAM_BANK_SIZE - 1);
        if (result == FR_OK && source_base + hdr.source_size > max_size)
            result = ERR_FILE_TOO_LARGE;
        if (result == FR_OK)
            result = load_rom_to_psram(rom_path, source_base >> 16, &rom_size);
        if (result == FR_OK && rom_size != hdr.source_size)
            result = ERR_FILE_FORMAT_INVALID; // patch made for a different ROM
        if (result == FR_OK) {
            outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
            result = apply_bps(&reader, &hdr, source_base, patch_size);
            size = hdr.target_size;
        }
    } else {
        result = load_rom_to_psram(rom_path, 0, &rom_size);
        if (result == FR_OK && rom_size > max_size)
            result = ERR_FILE_TOO_LARGE;
        if (result == FR_OK) {
            size = rom_size;
            outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
            result = apply_ips(&reader, &size, max_size, patch_size);
        }
    }

    outportb(WS_CART_BANK_FLASH_PORT, prev_flash);
    outportw(WS_CART_EXTBANK_ROM0_PORT, prev_rom0);
    outportw(WS_CART_EXTBANK_RAM_PORT, prev_ram);
    f_close(&reader.fp);
    if (result != FR_OK)
        return result;

    ui_draw_statusbar(lang_keys[LK_UI_STATUS_PATCH_STARTING]);

    // Metadata of the patched image (the patch may change size or save type)
    bootstub_data->prog.size = size;
    result = launch_get_rom_metadata_psram(&meta);
    if (result != FR_OK)
        return result;
    meta.id = patch_id;
    meta.rom_type = file_meta.rom_type;

    // Saves go next to the patch: "Game.bps" -> "Game.bps.ram"
    // (launch_restore_save_data() replaces the last extension)
    size_t len = strlen(patch_path);
    if (len + 3 > sizeof(save_path))
        return ERR_FILE_TOO_LARGE;
    memcpy(save_path, patch_path, len);
    save_path[len] = '.';
    save_path[len + 1] = 'x';
    save_path[len + 2] = 0;

    result = launch_restore_save_data(save_path, &meta);
    if (result == ERR_MCU_COMM_FAILED) {
        if (!launch_ui_handle_mcu_comm_error(&meta))
            goto exit_no_launch;
        result = FR_OK;
    } else if (result == FR_OK && launch_is_battery_required(&meta)) {
        if (cart_status_mcu_info_valid() && !cart_status_mcu_battery_ok()) {
            if (!launch_ui_handle_battery_missing_error(&meta))
                goto exit_no_launch;
        }
    }

    if (result == FR_OK) {
        bootstub_data->prog.cluster = BOOTSTUB_CLUSTER_AT_PSRAM;
        result = launch_rom_via_bootstub(&meta);
    }

exit_no_launch:
    mcu_reset_if_not_native();
    return result;
}

/* === START + Y1 patches (/NILESWAN/PATCHES) ===
 *
 * With the option on, every WonderSwan ROM is loaded into PSRAM by the menu
 * and launched from there: patched by its .Y1P variant if it has one, else
 * by the automatic patch above (if its button read has a known form).
 *
 * One text file per game, named after the ROM footer (the last 16 bytes,
 * read at once, while a CRC32 of a 4-16 MB ROM would take seconds here):
 *     <publisher><game id><version><checksum><banks>.Y1P
 * in hex, 2+2+2+4+4 digits: "01230045A10010.Y1P". A file may hold several
 * variants (the original and its translations often share the footer):
 *     ; comment
 *     [GunPey (J)]
 *     crc=A1656BBB                    (for people; not checked)
 *     p=0EEE31:9090909090:9A76FE00E0  (file offset : original : new, hex)
 *     p=...
 * The first variant whose original bytes all match the ROM file is
 * applied, after loading the ROM into PSRAM. Records hold up to 160 bytes
 * (the hook is 74). */

bool launch_keypad_patched;

#define KP_MAX_BYTES 160
#define KP_LINE_SIZE (16 + KP_MAX_BYTES * 4 + 8)

static int8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// "offset:orig:new": offset, and with which = 1 the original bytes, 2 the
// new ones, into buf. Returns the byte count, 0 if malformed.
static uint16_t kp_record(const char *v, uint32_t *ofs, uint8_t which, uint8_t *buf) {
    uint32_t o = 0;
    int8_t n;
    while ((n = hex_nibble(*v)) >= 0) {
        o = (o << 4) | n;
        v++;
    }
    if (*v != ':')
        return 0;
    *ofs = o;
    for (uint8_t field = 1; field <= 2; field++) {
        uint16_t count = 0;
        v++;
        while (true) {
            int8_t hi = hex_nibble(v[0]);
            if (hi < 0)
                break;
            int8_t lo = hex_nibble(v[1]);
            if (lo < 0 || count >= KP_MAX_BYTES)
                return 0;
            if (field == which)
                buf[count] = (hi << 4) | lo;
            count++;
            v += 2;
        }
        if (field == which)
            return count;
        if (*v != ':')
            return 0;
    }
    return 0;
}

/* === START + Y1: automatic patch ===
 * For ROMs without a .Y1P variant. The same steps as y1p.py's "auto" (its
 * output is compared byte for byte with this code's): the game's button
 * read "mov al, 40h / out 0B5h, al / ... / in al, 0B5h" in the last 16
 * banks (one or two of them; more: left alone) becomes a call to kr_hook,
 * copied into padding. The ROM is in PSRAM from bank 0; the RAM window
 * (flash mode on) shows the bank being worked on. */

uint16_t keypad_reset_find(uint16_t from);
uint16_t keypad_reset_free(uint16_t size);

// The hook. Entry 00h: selects the buttons group (out 0B5h, 40h) and waits;
// entry 0Bh: in al, 0B5h for the game (flags and every other register
// kept); with START and Y1 down, the soft reset of nile_soft_reset() (the
// bootstub keeps the flash awake for IPL0). RETF at 13h and 34h, RET there
// for a near call.
static const uint8_t __far kr_hook[] = {
    0x51, 0xB0, 0x40, 0xE6, 0xB5, 0xB9, 0x18, 0x00, 0xE2, 0xFE, 0x59, 0x9C,
    0xE4, 0xB5, 0xA8, 0x02, 0x75, 0x02, 0x9D, 0xCB, 0x50, 0x51, 0xB0, 0x10,
    0xE6, 0xB5, 0xB9, 0x18, 0x00, 0xE2, 0xFE, 0xE4, 0xB5, 0x88, 0xC4, 0xB0,
    0x40, 0xE6, 0xB5, 0xB9, 0x18, 0x00, 0xE2, 0xFE, 0xF6, 0xC4, 0x01, 0x75,
    0x04, 0x59, 0x58, 0x9D, 0xCB, 0xFA, 0x31, 0xC0, 0xE6, 0xB7, 0xB0, 0xDD,
    0xE6, 0xE2, 0x31, 0xC0, 0xE7, 0x00, 0x48, 0x50, 0x40, 0x50, 0x48, 0x68,
    0x90, 0xEA, 0x68, 0xE7, 0xE4, 0x68, 0xE6, 0xC0, 0x89, 0xE1, 0x16, 0x51,
    0xCB
};
#define KR_HOOK_SIZE  sizeof(kr_hook)
#define KR_HOOK_PLAIN 0x0B
#define KR_HOOK_RET1  0x13
#define KR_HOOK_RET2  0x34

enum { KR_PLAIN, KR_PLAIN_CX0, KR_SELECT, KR_SELECT_CX0, KR_LOOP };

typedef struct {
    uint16_t bank, start, in;
    uint8_t form;
} kr_site_t;

#define KR_BYTE(ofs) (((const uint8_t __far*) MK_FP(0x1000, 0))[(uint16_t) (ofs)])

// The button read whose "mov al, 40h" is at i (bank mapped):
//   loop:       ... mov cx,n / (nop) / loop / pop cx / in   -> from the mov cx
//   select:     out / nop... / in                           -> from the out
//   select-cx0: out / mov cx,n / loop $ / nop... / in       -> from the out
//   plain-cx0:  out / code / mov cx,n / loop $ / nop... / in -> from the mov cx
//   plain:      out / other code / nop... / in              -> from the NOPs
static bool kr_read_site(uint16_t i, kr_site_t *site) {
    uint16_t si = i + 4, in = 0;
    for (uint8_t k = 0; k < 96; k++, si++) {
        if (si < i)
            return false; // past the end of the bank
        if (KR_BYTE(si) == 0xB5) {
            uint8_t p = KR_BYTE(si - 1);
            if (p == 0xE4) {
                in = si - 1;
                break;
            }
            if (p == 0xE6 || p == 0xE7)
                return false;
        }
    }
    if (!in)
        return false;
    site->in = in;
    if (KR_BYTE(in - 1) == 0x59) {
        if (KR_BYTE(in - 3) != 0xE2)
            return false;
        uint8_t d = KR_BYTE(in - 2);
        site->form = KR_LOOP;
        if (d == 0xFE && KR_BYTE(in - 6) == 0xB9)
            site->start = in - 6;
        else if (d == 0xFD && KR_BYTE(in - 4) == 0x90 && KR_BYTE(in - 7) == 0xB9)
            site->start = in - 7;
        else if (d == 0xFC && KR_BYTE(in - 5) == 0x90 && KR_BYTE(in - 4) == 0x90 && KR_BYTE(in - 8) == 0xB9)
            site->start = in - 8;
        else
            return false;
        return true;
    }
    uint8_t n = 0;
    while (n < 255 && KR_BYTE(in - 1 - n) == 0x90)
        n++;
    uint16_t k = in - n; // first NOP (or the IN)
    if (k == i + 4) {
        site->form = KR_SELECT;
        site->start = i + 2;
        return true;
    }
    if (k - 5 >= i + 4 && KR_BYTE(k - 5) == 0xB9 && KR_BYTE(k - 2) == 0xE2 && KR_BYTE(k - 1) == 0xFE) {
        // "mov cx, n / loop $" right before the NOPs
        if (k - 5 == i + 4) {
            site->form = KR_SELECT_CX0;
            site->start = i + 2;
        } else {
            site->form = KR_PLAIN_CX0;
            site->start = k - 5;
        }
        return true;
    }
    if (n >= 1) {
        site->form = KR_PLAIN;
        site->start = k;
        return true;
    }
    return false;
}

// Place for the hook in the mapped bank, 0 if none: see keypad_reset_free();
// else, but not in the ROM's last bank (header), 00h padding up to the end
// of the bank.
static uint16_t kr_free(bool last_bank) {
    uint16_t place = keypad_reset_free(KR_HOOK_SIZE);
    if (place || last_bank)
        return place;
    uint16_t need = KR_HOOK_SIZE + 128 < 256 ? 256 : KR_HOOK_SIZE + 128;
    for (uint16_t ofs = 0xFF00 - need; ofs != 0; ofs++) // up to 0xFFFF
        if (KR_BYTE(ofs) != 0x00)
            return 0;
    return 0xFF00 - KR_HOOK_SIZE - 64;
}

static inline bool kr_linear(uint16_t bank, uint16_t banks) {
    // seen in the linear window (segments 4-F)
    return banks < 16 || (bank >= banks - 16 && ((bank - banks) & 0x0F) >= 4);
}

static void kr_map(uint16_t bank) {
    outportw(WS_CART_EXTBANK_RAM_PORT, bank);
}

// Flash mode must be on. Returns true if at least one read was patched.
typedef struct {
    uint16_t bank[2], hook[2];
    bool far[2];
    uint8_t count;
} kr_placed_t;

// A hook this call can reach: one already copied (any far one for a far
// call, one in the same bank for a near call), else a new copy in the
// read's bank (near) or in a linear bank (far). Returns its offset (0: none)
// and its bank in *hb.
static uint16_t kr_get_hook(kr_placed_t *pl, uint16_t sbank, bool far, uint16_t banks, uint16_t first, uint16_t *hb) {
    for (uint8_t j = 0; j < pl->count; j++) {
        if (pl->far[j] == far && (far || pl->bank[j] == sbank)) {
            *hb = pl->bank[j];
            return pl->hook[j];
        }
    }
    uint16_t b = sbank, hook = 0;
    if (!far || kr_linear(b, banks)) {
        kr_map(b);
        hook = kr_free(b == banks - 1);
    }
    if (!hook && far) {
        for (b = banks - 1; ; b--) {
            if (b != sbank && kr_linear(b, banks)) {
                kr_map(b);
                if ((hook = kr_free(b == banks - 1)))
                    break;
            }
            if (b == first)
                break;
        }
    }
    if (!hook || pl->count == 2)
        return 0;
    uint8_t __far *bank = MK_FP(0x1000, 0);
    _fmemcpy(bank + hook, kr_hook, KR_HOOK_SIZE);
    if (!far)
        bank[hook + KR_HOOK_RET1] = bank[hook + KR_HOOK_RET2] = 0xC3;
    pl->bank[pl->count] = b;
    pl->hook[pl->count] = hook;
    pl->far[pl->count++] = far;
    *hb = b;
    return hook;
}

static bool keypad_auto_patch(uint16_t banks) {
    kr_site_t sites[2];
    uint8_t count = 0;
    uint16_t first = banks > 16 ? banks - 16 : 0;

    for (uint16_t b = first; b < banks; b++) {
        kr_map(b);
        uint16_t i = keypad_reset_find(8);
        while (i) {
            kr_site_t s;
            if (kr_read_site(i, &s)) {
                if (count == 2)
                    return false; // not the classic single routine
                s.bank = b;
                sites[count++] = s;
            }
            i = keypad_reset_find(i + 1);
        }
    }

    bool done = false;
    // hooks already copied (a second read reuses one it can reach: the
    // padding may not hold two)
    kr_placed_t placed = {0};
    while (count) {
        kr_site_t *s = &sites[--count];
        uint16_t room = s->in + 2 - s->start;
        uint8_t prefix = s->form == KR_LOOP ? 1 : 0;
        uint8_t suffix = (s->form == KR_SELECT_CX0 || s->form == KR_PLAIN_CX0) ? 3 : 0;

        // linear bank: far call. Banked window (2000h/3000h): near call to
        // a hook in the same bank, else far call to a hook in a linear bank
        // (the linear window does not move while the game runs).
        uint16_t hb, hook = 0;
        bool far = kr_linear(s->bank, banks);
        if (!far && room >= prefix + 3 + suffix)
            hook = kr_get_hook(&placed, s->bank, false, banks, first, &hb);
        if (!hook && room >= prefix + 5 + suffix) {
            far = true;
            hook = kr_get_hook(&placed, s->bank, true, banks, first, &hb);
        }
        if (!hook)
            continue;
        uint16_t entry = hook + ((s->form == KR_SELECT || s->form == KR_SELECT_CX0) ? 0 : KR_HOOK_PLAIN);

        kr_map(s->bank);
        uint8_t __far *bank = MK_FP(0x1000, 0); // (the window, now on the read's bank)
        uint16_t p = s->start;
        if (prefix)
            bank[p++] = 0x59; // pop cx (the loop goes)
        if (far) {
            uint16_t seg = ((hb - banks) & 0x0F) << 12;
            bank[p] = 0x9A;
            bank[p + 1] = entry;
            bank[p + 2] = entry >> 8;
            bank[p + 3] = seg;
            bank[p + 4] = seg >> 8;
            p += 5;
        } else {
            uint16_t rel = entry - (p + 3);
            bank[p] = 0xE8;
            bank[p + 1] = rel;
            bank[p + 2] = rel >> 8;
            p += 3;
        }
        if (suffix) {
            bank[p] = 0xB9; // mov cx, 0: as after the game's loop
            bank[p + 1] = 0;
            bank[p + 2] = 0;
            p += 3;
        }
        while (p != s->in + 2)
            bank[p++] = 0x90;
        done = true;
    }
    return done;
}

int16_t launch_keypad_patch(const char *path, const launch_rom_metadata_t *meta) {
    FIL pf, rf;
    char line[KP_LINE_SIZE];
    uint8_t buf[KP_MAX_BYTES], rom_buf[KP_MAX_BYTES];
    char *key, *value;
    ini_next_result_t r;
    int16_t result;

    launch_keypad_patched = false;
    if (!(settings.prog.flags & SETTING_PROG_FLAG_KEYPAD_RESET) || meta->rom_type != ROM_TYPE_WS)
        return FR_NO_FILE;

    snprintf(line, sizeof(line), "/NILESWAN/PATCHES/%02X%02X%02X%04X%04X.Y1P",
        meta->footer.publisher_id, meta->footer.game_id, meta->footer.game_version,
        meta->footer.checksum, meta->rom_banks);
    bool have_file = f_open(&pf, line, FA_OPEN_EXISTING | FA_READ) == FR_OK;
    if (f_open(&rf, path, FA_OPEN_EXISTING | FA_READ) != FR_OK) {
        if (have_file)
            f_close(&pf);
        return FR_NO_FILE;
    }
    uint32_t rom_size = f_size(&rf);

    // Find the first variant whose original bytes are all in the ROM
    uint32_t start = 0, found = 0xFFFFFFFF;
    bool ok = true;
    uint16_t records = 0;
    while (have_file) {
        r = ini_next(&pf, line, sizeof(line), &key, &value);
        if (r == INI_NEXT_ERROR)
            break;
        if (r == INI_NEXT_FINISHED || r == INI_NEXT_CATEGORY) {
            if (ok && records) {
                found = start;
                break;
            }
            if (r == INI_NEXT_FINISHED)
                break;
            start = f_tell(&pf);
            ok = true;
            records = 0;
            continue;
        }
        if (!ok || strcmp(key, "p"))
            continue;
        uint32_t ofs;
        uint16_t n = kp_record(value, &ofs, 1, buf);
        UINT br;
        if (n == 0 || ofs + n > rom_size || f_lseek(&rf, ofs) != FR_OK
            || f_read(&rf, rom_buf, n, &br) != FR_OK || br != n || memcmp(buf, rom_buf, n))
            ok = false;
        else
            records++;
    }
    f_close(&rf);

    // Load the ROM; then the variant's new bytes, or the automatic patch
    uint32_t max_size = (uint32_t) asset_heap_get_free_first_banks() << 16;
    if (rom_size > max_size) {
        if (have_file)
            f_close(&pf);
        return FR_NO_FILE;
    }
    uint16_t prev_ram = inportw(WS_CART_EXTBANK_RAM_PORT);
    uint16_t prev_rom0 = inportw(WS_CART_EXTBANK_ROM0_PORT);
    uint8_t prev_flash = inportb(WS_CART_BANK_FLASH_PORT);
    progress_lk = 0xFFFF;
    progress_show(LK_UI_STATUS_PATCH_LOADING, 0, 1);
    uint32_t size;
    bool patched = false;
    result = load_rom_to_psram(path, 0, &size);
    if (result == FR_OK) {
        outportb(WS_CART_BANK_FLASH_PORT, WS_CART_BANK_FLASH_ENABLE);
        if (found != 0xFFFFFFFF) {
            result = f_lseek(&pf, found);
            while (result == FR_OK
                && (r = ini_next(&pf, line, sizeof(line), &key, &value)) == INI_NEXT_KEY_VALUE) {
                if (strcmp(key, "p"))
                    continue;
                uint32_t ofs;
                uint16_t n = kp_record(value, &ofs, 2, buf);
                if (n)
                    psram_write(ofs, buf, n);
            }
            patched = result == FR_OK;
        } else {
            ui_draw_statusbar(lang_keys[LK_UI_STATUS_PATCH_STARTING]);
            patched = keypad_auto_patch((size + 0xFFFF) >> 16);
        }
    }
    outportb(WS_CART_BANK_FLASH_PORT, prev_flash);
    outportw(WS_CART_EXTBANK_ROM0_PORT, prev_rom0);
    outportw(WS_CART_EXTBANK_RAM_PORT, prev_ram);
    if (have_file)
        f_close(&pf);
    if (result != FR_OK)
        return FR_NO_FILE; // launched the usual way instead

    // launched from PSRAM, patched or not (no second load)
    bootstub_data->prog.size = size;
    bootstub_data->prog.cluster = BOOTSTUB_CLUSTER_AT_PSRAM;
    launch_keypad_patched = patched;
    return FR_OK;
}
