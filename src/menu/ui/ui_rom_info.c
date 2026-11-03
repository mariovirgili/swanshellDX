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
#include <nilefs.h>
#include "errors.h"
#include "lang.h"
#include "lang_gen.h"
#include "launch/launch.h"
#include "main.h"
#include "menu_music.h"
#include "strings.h"
#include "ui/bitmap.h"
#include "ui/ui.h"
#include "ui/ui_dialog.h"
#include "ui/ui_popup_dialog.h"
#include "util/input.h"
#include "ui_rom_info.h"

/*
 * Start > File > ROM info: the ROM header (the last 16 bytes of the file,
 * see ws.nesdev.org/wiki/ROM_header) and, on request (A), the checksum
 * computed over the whole file.
 */

#define ROM_INFO_PSRAM_BANK 0  // scratch bank for the checksum reads
#define ROM_INFO_CHUNK 0x8000
#define ROW_X 4
#define ROW_Y 10
#define VALUE_X 62

DEFINE_STRING_LOCAL(s_hex2_tpl, "%02X");
DEFINE_STRING_LOCAL(s_close_paren, ")");
DEFINE_STRING_LOCAL(s_hex4_tpl, "%04X");
DEFINE_STRING_LOCAL(s_publisher_tpl, "%02X ");
DEFINE_STRING_LOCAL(s_game_id_tpl, "%02X  v%u");
DEFINE_STRING_LOCAL(s_size_mb_tpl, "%u MB");
DEFINE_STRING_LOCAL(s_size_kb_tpl, "%u KB");
DEFINE_STRING_LOCAL(s_size_b_tpl, "%lu B");
DEFINE_STRING_LOCAL(s_mapper_2001, "Bandai 2001");
DEFINE_STRING_LOCAL(s_mapper_2003, "Bandai 2003 (RTC)");
DEFINE_STRING_LOCAL(s_mapper_tpl, "%02X");
DEFINE_STRING_LOCAL(s_bus_tpl, "%u-bit, +%u wait");
DEFINE_STRING_LOCAL(s_checksum_bad_tpl, "%04X (");

// Developer/publisher IDs (ws.nesdev.org/wiki/ROM_header): the IDs, and
// "code name" strings in the same order, one after the other. (Plain byte
// arrays: a table of structs did not work.)
static const uint8_t __far publisher_ids[] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x10, 0x11, 0x12, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2B, 0x2D, 0x2E, 0x2F, 0x31, 0x32, 0x33, 0x35, 0x36
};

static const char __far publisher_names[] =
    "BAN Bandai\0"
    "TAT Taito\0"
    "TMY Tomy\0"
    "KEX Koei\0"
    "DTE Data East\0"
    "AAE Asmik Ace\0"
    "MDE Media Entertainment\0"
    "NHB Nichibutsu\0"
    "CCJ Coconuts Japan\0"
    "SUM Sammy\0"
    "SUN Sunsoft\0"
    "PAW Mebius\0"
    "BPR Banpresto\0"
    "JLC Jaleco\0"
    "MGA Imagineer\0"
    "KNM Konami\0"
    "KBS Kobunsha\0"
    "BTM Bottom Up\0"
    "KGT Kaga Tech\0"
    "SRV Sunrise\0"
    "CFT Cyber Front\0"
    "MGH Megahouse\0"
    "BEC Interbec\0"
    "NAP Nihon Application\0"
    "BVL Bandai Visual\0"
    "ATN Athena\0"
    "KDX KID\0"
    "HAL HAL Corporation\0"
    "YKE Yuki Enterprise\0"
    "OMN Omega Micott\0"
    "LAY Layup\0"
    "KDK Kadokawa Shoten\0"
    "SHL Shall Luck\0"
    "SQR Squaresoft\0"
    "TMC Tom Create\0"
    "NMC Namco\0"
    "SES Soeishinsha\0"
    "HTR Hearty Robin\0"
    "VGD Vanguard\0"
    "MGT Megatron\0"
    "WIZ Wiz\0"
    "TAN Tanita\0"
    "CPC Capcom\0";

// ROM sizes (header byte $A), in KB
static const uint16_t __far rom_size_kb[] = {
    128, 256, 512, 1024, 2048, 3072, 4096, 6144, 8192, 16384, 32768, 65535
};

static void format_size(char *buf, uint16_t buf_len, uint32_t bytes) {
    if (bytes >= 1024L * 1024L && !(bytes & 0xFFFFF))
        snprintf(buf, buf_len, s_size_mb_tpl, (uint16_t) (bytes >> 20));
    else if (bytes >= 1024 && !(bytes & 0x3FF))
        snprintf(buf, buf_len, s_size_kb_tpl, (uint16_t) (bytes >> 10));
    else
        snprintf(buf, buf_len, s_size_b_tpl, bytes);
}

// Append a far string to buf (no printf "%s": near/far pointers).
static void append(char *buf, uint16_t buf_len, const char __far *s) {
    uint16_t n = strlen(buf);
    while (*s && n + 1 < buf_len)
        buf[n++] = *(s++);
    buf[n] = 0;
}

static uint16_t row_y;

static void draw_row(uint16_t label, const char __far *value) {
    bitmapfont_draw_string(&ui_bitmap, ROW_X, row_y, lang_keys[label], VALUE_X - ROW_X - 2);
    bitmapfont_draw_string(&ui_bitmap, VALUE_X, row_y, value, screen_width - VALUE_X - 2);
    row_y += bitmapfont_get_font_height();
}

// ui_rom_info_asm.s: AX = sum of the words at 1000:0000, DX = sum of their
// high bytes
extern uint32_t ui_rom_info_sum_words(uint16_t words);

// Sum of all the bytes of the file but the last two (the checksum).
// Returns FR_OK, an error, or ERR_USER_EXIT_REQUESTED (B pressed).
static int16_t compute_checksum(const char *path, uint16_t *sum_out) {
    FIL fp;
    int16_t result = f_open(&fp, path, FA_READ | FA_OPEN_EXISTING);
    if (result != FR_OK)
        return result;
    uint32_t left = f_size(&fp);
    if (left < 16) {
        f_close(&fp);
        return ERR_FILE_FORMAT_INVALID;
    }
    left -= 2;

    ui_popup_dialog_config_t dlg = {0};
    dlg.title = lang_keys[LK_UI_ROM_INFO_VERIFYING];
    dlg.progress_max = (left >> 15) + 1;
    ui_popup_dialog_draw(&dlg);

    uint16_t word_sum = 0, high_sum = 0, odd_sum = 0;
    // (no return inside: the flash mode and RAM bank must be restored)
    ws_bank_with_flash(WS_CART_BANK_FLASH_ENABLE, {
        ws_bank_with_ram(ROM_INFO_PSRAM_BANK, {
            while (left && result == FR_OK) {
                unsigned int br;
                uint16_t want = left > ROM_INFO_CHUNK ? ROM_INFO_CHUNK : (uint16_t) left;
                result = f_read(&fp, MK_FP(0x1000, 0x0000), want, &br);
                if (result != FR_OK)
                    break;
                if (br != want) {
                    result = FR_INT_ERR;
                    break;
                }
                uint32_t sums = ui_rom_info_sum_words(want >> 1);
                word_sum += (uint16_t) sums;
                high_sum += (uint16_t) (sums >> 16);
                if (want & 1) {
                    // (only the last block can be odd)
                    const uint8_t __far *p = MK_FP(0x1000, 0x0000);
                    odd_sum += p[want - 1];
                }
                left -= want;

                dlg.progress_step++;
                ui_popup_dialog_draw_update(&dlg);
                input_update();
                if (input_pressed & WS_KEY_B)
                    result = ERR_USER_EXIT_REQUESTED;
            }
        });
    });

    f_close(&fp);
    ui_popup_dialog_clear(&dlg);
    // sum of the bytes = words - 255 * high bytes (low + high = word - 255 * high)
    *sum_out = word_sum - 255 * high_sum + odd_sum;
    return result;
}

void ui_rom_info(const char __far *filename) {
    char path[FF_LFN_BUF + 1];
    char value[48];
    launch_rom_metadata_t meta;

    strncpy(path, filename, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;

    int16_t result = launch_get_rom_metadata(path, &meta);
    if (result != FR_OK) {
        ui_dialog_error_check(result, lang_keys[LK_UI_ROM_INFO_TITLE], 0);
        return;
    }
    uint32_t file_size = 0;
    {
        FILINFO fno;
        if (f_stat(path, &fno) == FR_OK)
            file_size = fno.fsize;
    }

    bool checked = false;
    uint16_t computed = 0;

    // plain background: the text is hard to read over a wallpaper (the file
    // list loads it again when it is redrawn)
    ui_unload_wallpaper();

    while (true) {
        ui_layout_bars();
        ui_draw_titlebar(lang_keys[LK_UI_ROM_INFO_TITLE]);
        ui_draw_statusbar(lang_keys[checked ? LK_UI_ROM_INFO_CONTROLS_DONE : LK_UI_ROM_INFO_CONTROLS]);
        bitmapfont_set_active_font(font8_bitmap);
        row_y = ROW_Y;

        draw_row(LK_UI_ROM_INFO_FILE, filename);

        if (meta.rom_type == ROM_TYPE_PCV2)
            draw_row(LK_UI_ROM_INFO_SYSTEM, s_rom_info_pcv2);
        else
            draw_row(LK_UI_ROM_INFO_SYSTEM, lang_keys[(meta.footer.color & 1) ? LK_ROM_INFO_COLOR : LK_ROM_INFO_MONO]);

        snprintf(value, sizeof(value), s_publisher_tpl, meta.footer.publisher_id);
        {
            const char __far *name = publisher_names;
            for (uint8_t i = 0; i < sizeof(publisher_ids); i++) {
                if (publisher_ids[i] == meta.footer.publisher_id) {
                    append(value, sizeof(value), name);
                    break;
                }
                while (*name)
                    name++;
                name++; // the next "code name"
            }
        }
        draw_row(LK_UI_ROM_INFO_PUBLISHER, value);

        snprintf(value, sizeof(value), s_game_id_tpl, meta.footer.game_id, meta.footer.game_version & 0x7F);
        draw_row(LK_UI_ROM_INFO_GAME_ID, value);

        // declared size, and the file's if different
        if (meta.footer.rom_size < sizeof(rom_size_kb) / sizeof(uint16_t)) {
            format_size(value, sizeof(value), (uint32_t) rom_size_kb[meta.footer.rom_size] * 1024);
        } else {
            snprintf(value, sizeof(value), s_hex2_tpl, meta.footer.rom_size);
        }
        draw_row(LK_UI_ROM_INFO_ROM_SIZE, value);
        if (meta.footer.rom_size >= sizeof(rom_size_kb) / sizeof(uint16_t)
            || (uint32_t) rom_size_kb[meta.footer.rom_size] * 1024 != file_size) {
            format_size(value, sizeof(value), file_size);
            draw_row(LK_UI_ROM_INFO_FILE_SIZE, value);
        }

        if (meta.sram_size)
            snprintf(value, sizeof(value), s_rom_info_sram_tpl, (uint16_t) (meta.sram_size >> 10));
        else if (meta.eeprom_size >= 1024)
            snprintf(value, sizeof(value), s_rom_info_eeprom_kb_tpl, (uint16_t) (meta.eeprom_size >> 10));
        else if (meta.eeprom_size)
            snprintf(value, sizeof(value), s_rom_info_eeprom_b_tpl, (uint16_t) meta.eeprom_size);
        else
            value[0] = 0;
        draw_row(LK_UI_ROM_INFO_SAVE, value[0] ? (const char __far*) value : lang_keys[LK_ROM_INFO_NO_SAVE]);

        if ((meta.footer.mapper & 0x0F) == 0)
            draw_row(LK_UI_ROM_INFO_MAPPER, s_mapper_2001);
        else if ((meta.footer.mapper & 0x0F) == 1)
            draw_row(LK_UI_ROM_INFO_MAPPER, s_mapper_2003);
        else {
            snprintf(value, sizeof(value), s_mapper_tpl, meta.footer.mapper);
            draw_row(LK_UI_ROM_INFO_MAPPER, value);
        }

        draw_row(LK_UI_ROM_INFO_ORIENTATION, lang_keys[(meta.footer.flags & 1) ? LK_ROM_INFO_VERTICAL : LK_ROM_INFO_HORIZONTAL]);

        snprintf(value, sizeof(value), s_bus_tpl, (meta.footer.flags & 4) ? 16 : 8, (meta.footer.flags & 8) ? 1 : 0);
        draw_row(LK_UI_ROM_INFO_BUS, value);

        snprintf(value, sizeof(value), s_hex4_tpl, meta.footer.checksum);
        draw_row(LK_UI_ROM_INFO_CHECKSUM, value);

        if (checked) {
            if (computed == meta.footer.checksum) {
                draw_row(LK_UI_ROM_INFO_COMPUTED, lang_keys[LK_UI_ROM_INFO_CHECKSUM_OK]);
            } else {
                // "1A2C (wrong)"
                snprintf(value, sizeof(value), s_checksum_bad_tpl, computed);
                append(value, sizeof(value), lang_keys[LK_UI_ROM_INFO_CHECKSUM_WRONG]);
                append(value, sizeof(value), s_close_paren);
                draw_row(LK_UI_ROM_INFO_COMPUTED, value);
            }
        }

        input_wait_clear();
        while (true) {
            idle_until_vblank();
            input_update();
            if (input_pressed & (WS_KEY_B | WS_KEY_START))
                return;
            if ((input_pressed & WS_KEY_A) && !checked)
                break;
        }

        // the menu music plays from PSRAM in an interrupt: pause it while
        // the file is read through the PSRAM window, and resume it after
        bool music = menu_music_is_playing();
        if (music)
            menu_music_stop();
        result = compute_checksum(path, &computed);
        if (music)
            menu_music_start();
        if (result == ERR_USER_EXIT_REQUESTED)
            continue;
        if (ui_dialog_error_check(result, lang_keys[LK_UI_ROM_INFO_TITLE], 0))
            continue;
        checked = true;
    }
}
