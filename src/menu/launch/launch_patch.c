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
#include "strings.h"
#include "ui/ui.h"
#include "util/asset_heap.h"
#include "util/file.h"

#define PSRAM_BANK_SIZE 0x10000UL

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
    while (len) {
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

static int16_t apply_ips(patch_reader_t *r, uint32_t *size, uint32_t max_size) {
    if (!reader_magic(r, "PATCH"))
        return ERR_FILE_FORMAT_INVALID;

    uint32_t end = *size;
    while (true) {
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

static bool file_exists(const char *path) {
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK && !(fno.fattrib & AM_DIR);
}

// "Game.bps" -> "Game.wsc" / "Game.ws" / "Game.pc2"; "Game.wsc.bps" -> "Game.wsc"
static bool find_rom_for_patch(const char *patch_path) {
    strncpy(rom_path, patch_path, sizeof(rom_path) - 5);
    rom_path[sizeof(rom_path) - 5] = 0;
    char *dot = strrchr(rom_path, '.');
    if (dot == NULL)
        return false;
    *dot = 0;

    const char *inner = strrchr(rom_path, '.');
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
    result = f_read_rom_banked(&fp, bank, *size, NULL, NULL);
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
    bool is_bps = !strcasecmp(strrchr(patch_path, '.'), s_file_ext_bps);

    ui_draw_statusbar(lang_keys[LK_UI_STATUS_PATCHING]);

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
            result = apply_ips(&reader, &size, max_size);
        }
    }

    outportb(WS_CART_BANK_FLASH_PORT, prev_flash);
    outportw(WS_CART_EXTBANK_ROM0_PORT, prev_rom0);
    outportw(WS_CART_EXTBANK_RAM_PORT, prev_ram);
    f_close(&reader.fp);
    if (result != FR_OK)
        return result;

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
