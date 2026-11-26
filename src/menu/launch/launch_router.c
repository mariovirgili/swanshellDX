#include "launch/launch_router.h"
#include <string.h>
#include <wonderful.h>
#include <nilefs.h>
#include "cart/mcu.h"
#include "cart/status.h"
#include "lang.h"
#include "launch.h"
#include "../errors.h"
#include "../strings.h"
#include "launch/launch_athena.h"
#include "launch/launch_patch.h"
#include "plugin/plugin.h"
#include "shell/shell.h"
#include "menu_music.h"

int16_t launch_router_file(char *path, const char *ext, uint16_t flags) {
    int16_t code = FR_OK;

    // Programs and players take over the sound hardware, HBL timer and PSRAM.
    menu_music_stop();

    if (ext != NULL) {
        if (!strcasecmp(ext, s_file_ext_ws) || !strcasecmp(ext, s_file_ext_wsc) || !strcasecmp(ext, s_file_ext_pc2)) {
            launch_rom_metadata_t meta;
            int16_t result = launch_get_rom_metadata(path, &meta);
            if (result == FR_OK) {
                result = launch_restore_save_data(path, &meta);
                if (result == ERR_MCU_COMM_FAILED) {
                    if (flags & LAUNCH_ROUTER_USE_SERIAL_OUT) {
                        shell_print_error(result);
                        shell_print_newline();
                        result = FR_OK;
                    } else {
                        if (launch_ui_handle_mcu_comm_error(&meta))
                            result = FR_OK;
                        else
                            goto exit_no_launch;
                    }
                } else {
                    if (launch_is_battery_required(&meta)) {
                        if (cart_status_mcu_info_valid() && !cart_status_mcu_battery_ok()) {
                            if (flags & LAUNCH_ROUTER_USE_SERIAL_OUT) {
                                nile_mcu_native_cdc_write_string(lang_keys[LK_PROMPT_NO_BATTERY_TITLE]);
                                shell_print_newline();
                            } else {
                                if (!launch_ui_handle_battery_missing_error(&meta))
                                    goto exit_no_launch;
                            }
                        }
                    }
                }
                if (result == FR_OK) {
                    // START + Y1: the game's patch from /NILESWAN/PATCHES,
                    // if there is one (the ROM is then already in PSRAM)
                    if (!(flags & LAUNCH_ROUTER_USE_SERIAL_OUT)
                        && launch_keypad_patch(path, &meta) == FR_OK) {
                        result = launch_rom_via_bootstub(&meta);
                    } else {
                        result = launch_set_bootstub_file_entry(path, (bootstub_file_entry_t*) &bootstub_data->prog);
                        if (result == FR_OK) {
                            result = launch_rom_via_bootstub(&meta);
                        }
                    }
                }
            }

exit_no_launch:
            mcu_reset_if_not_native();
            return result;
        } else if (!strcasecmp(ext, s_file_ext_ips) || !strcasecmp(ext, s_file_ext_bps)) {
            // ROM patch: launch the matching ROM with the patch applied in PSRAM
            if (flags & LAUNCH_ROUTER_USE_SERIAL_OUT) return ERR_FILE_NOT_EXECUTABLE;
            return launch_patched_rom(path);
        } else if (!strcasecmp(ext, s_file_ext_fx) || !strcasecmp(ext, s_file_ext_bin)) {
            // TODO: Do some checks on the .bin file
            return launch_athena_boot_curdir_as_rom_wip(path);
        } else if (!strcasecmp(ext, s_file_ext_vgm) || !strcasecmp(ext, s_file_ext_vgz)) {
            if (flags & LAUNCH_ROUTER_USE_SERIAL_OUT) return ERR_FILE_NOT_EXECUTABLE;
            code = ui_vgmplay(path);
        } else if (!strcasecmp(ext, s_file_ext_wav)) {
            if (flags & LAUNCH_ROUTER_USE_SERIAL_OUT) return ERR_FILE_NOT_EXECUTABLE;
            code = ui_wavplay(path);
        } else if (!strcasecmp(ext, s_file_ext_bmp)) {
            // SRAM corrupted by this operation
            if (flags & LAUNCH_ROUTER_USE_SERIAL_OUT) return ERR_FILE_NOT_EXECUTABLE;
            return ui_bmpview(path);
        } else if (!strcasecmp(ext, s_file_ext_bfb)) {
            return launch_bfb(path);
        } else if (!strcasecmp(ext, s_file_ext_com)) {
            return launch_com(path);
        } else if (!strcasecmp(ext, s_file_ext_rom)) {
            return launch_plugin_via_ipc(s_path_plugin_uxn, path);
        } else if (!strcasecmp(ext, s_file_ext_sg)) {
            // SG-1000 ROM: sgswan, from ggswan's sources (no cartridge RAM
            // to save)
            if (flags & LAUNCH_ROUTER_USE_SERIAL_OUT) return ERR_FILE_NOT_EXECUTABLE;
            return launch_plugin_via_ipc(s_path_plugin_sg, path);
        } else if (!strcasecmp(ext, s_file_ext_sf)) {
            // SF-7000 disk image (163,840 bytes): sfswan, from ggswan's
            // sources. A disk with no system on it is started from the
            // system disk /NILESWAN/SF7000.SF (a copy of a disk that has
            // Disk BASIC), which the plugin looks for. What the program
            // writes to the disk is not kept
            if (flags & LAUNCH_ROUTER_USE_SERIAL_OUT) return ERR_FILE_NOT_EXECUTABLE;
            return launch_plugin_via_ipc(s_path_plugin_sf, path);
        } else if (!strcasecmp(ext, s_file_ext_gb)) {
            // Game Boy ROM: gbswan (src/ggswan/gb). A cartridge with a
            // battery (its type, at 0147h of the ROM) has its RAM, up to
            // 32 KiB, in the first 32 KiB of the SRAM: saved as for a WS
            // game, game.sav next to the .gb, loaded now and written back
            // by the menu when it starts again (the plugin's START + Y1
            // goes back to the menu)
            if (flags & LAUNCH_ROUTER_USE_SERIAL_OUT) return ERR_FILE_NOT_EXECUTABLE;
            {
                launch_rom_metadata_t gb_meta;
                memset(&gb_meta, 0, sizeof(gb_meta));
                FIL f;
                uint8_t type = 0;
                unsigned int br = 0;
                int16_t result = f_open(&f, path, FA_OPEN_EXISTING | FA_READ);
                if (result != FR_OK)
                    return result;
                gb_meta.id = f.obj.sclust;
                if (f_lseek(&f, 0x147) == FR_OK)
                    f_read(&f, &type, 1, &br);
                f_close(&f);
                if (br == 1 && (type == 0x03 || type == 0x06 || type == 0x09 || type == 0x0F || type == 0x10
                        || type == 0x13 || type == 0x1B || type == 0x1E)) {
                    gb_meta.sram_size = 32 * 1024L;
                    result = launch_restore_save_data(path, &gb_meta);
                    if (result == ERR_MCU_COMM_FAILED) {
                        if (!launch_ui_handle_mcu_comm_error(&gb_meta))
                            return result;
                    } else if (result != FR_OK) {
                        return result;
                    }
                }
            }
            return launch_plugin_via_ipc(s_path_plugin_gb, path);
        } else if (!strcasecmp(ext, s_file_ext_gg) || !strcasecmp(ext, s_file_ext_sms)) {
            // Game Gear ROM: ggswan emulator plugin; Master System ROM:
            // smsswan, built from the same sources
            if (flags & LAUNCH_ROUTER_USE_SERIAL_OUT) return ERR_FILE_NOT_EXECUTABLE;
            // its cartridge RAM (up to 32 KiB) is the first 32 KiB of the
            // SRAM: saved as for a WS game, game.sav next to the .gg (or
            // .sms), loaded now and written back by the menu when it
            // starts again (SAVE.INI; the plugin's START + Y1 goes back to
            // the menu)
            {
                launch_rom_metadata_t gg_meta;
                memset(&gg_meta, 0, sizeof(gg_meta));
                FIL f;
                int16_t result = f_open(&f, path, FA_OPEN_EXISTING | FA_READ);
                if (result != FR_OK)
                    return result;
                gg_meta.id = f.obj.sclust;
                f_close(&f);
                gg_meta.sram_size = 32 * 1024L;
                result = launch_restore_save_data(path, &gg_meta);
                if (result == ERR_MCU_COMM_FAILED) {
                    if (!launch_ui_handle_mcu_comm_error(&gg_meta))
                        return result;
                } else if (result != FR_OK) {
                    return result;
                }
            }
            return launch_plugin_via_ipc(strcasecmp(ext, s_file_ext_sms) ? s_path_plugin_gg : s_path_plugin_sms, path);
        } else {
            goto txtview;
        }
    } else {
txtview:
        if (flags & LAUNCH_ROUTER_USE_SERIAL_OUT) {
            return ERR_FILE_NOT_EXECUTABLE;
        } else {
            code = ui_txtview(path);
        }
    }

    return !code ? LAUNCH_ROUTER_OK_SRAM_UNCHANGED : code;
}
