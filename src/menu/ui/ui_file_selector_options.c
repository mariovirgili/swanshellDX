/**
 * Copyright (c) 2025 Adrian Siekierka
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

#include <nilefs/ff.h>
#include <string.h>
#include <wonderful.h>
#include <ws.h>
#include "bootstub.h"
#include "lang.h"
#include "lang_gen.h"
#include "launch/launch.h"
#include "settings.h"
#include "strings.h"
#include "ui/ui.h"
#include "ui_about.h"
#include "ui_dialog.h"
#include "ui_favorites.h"
#include "ui_file_selector.h"
#include "ui_fileops.h"
#include "ui_popup_list.h"
#include "ui_settings.h"
#include "../plugin/plugin.h"
#include "../ww/ww.h"
#include "../xmodem.h"

enum tristate {
    TRISTATE_FALSE,
    TRISTATE_TRUE,
    TRISTATE_NONE
};

int ui_file_selector_actions_bfb(void) {
    ui_popup_list_config_t lst = {0};
    lst.option[0] = lang_keys[LK_SUBMENU_OPTION_TEST_BFB];
    return ui_popup_list(&lst);
}

static enum tristate ui_file_selector_tools_witch(ui_popup_list_config_t *lst, const char __far *filename) {
    while (true) {
        memset(lst, 0, sizeof(ui_popup_list_config_t));
        uint8_t i = 0;
        uint8_t options[4];
        if (fileops_has_rom_contents(filename)) {
            options[i] = 0;
            lst->option[i++] = lang_keys[LK_SUBMENU_OPTION_WITCH_REPLACE_BIOS];
            options[i] = 1;
            lst->option[i++] = lang_keys[LK_SUBMENU_OPTION_WITCH_REPLACE_OS];
            options[i] = 2;
            lst->option[i++] = lang_keys[LK_SUBMENU_OPTION_WITCH_EXTRACT_BIOS_OS];
        } else if (fileops_has_extension(filename, s_file_ext_il)) {
            options[i] = 3;
            lst->option[i++] = lang_keys[LK_SUBMENU_OPTION_WITCH_ADD_GLOBAL_LIBRARY];
        }
        options[i] = 4;
        lst->option[i++] = lang_keys[LK_SUBMENU_OPTION_WITCH_CREATE_IMAGE];

        int result = ui_popup_list(lst);
        if (result >= 0) {
            result = options[result];
        }
        switch (result) {
        case UI_POPUP_ACTION_START:
            return TRISTATE_FALSE;
        default:
            ui_popup_list_clear(lst);
            return TRISTATE_NONE;
        case 0:
            ui_dialog_error_check(ww_ui_replace_component(filename, false), NULL, 0);
            return TRISTATE_TRUE;
        case 1:
            ui_dialog_error_check(ww_ui_replace_component(filename, true), NULL, 0);
            return TRISTATE_TRUE;
        case 2:
            ui_dialog_error_check(ww_ui_extract_from_rom(filename), NULL, 0);
            return TRISTATE_TRUE;
        case 3:
            ui_dialog_error_check(ww_ui_move_to_fbin(filename), NULL, 0);
            return TRISTATE_TRUE;
        case 4:
            ui_dialog_error_check(ww_ui_create_image(), NULL, 0);
            return TRISTATE_TRUE;
        }
    }
}

static enum tristate ui_file_selector_tools(ui_popup_list_config_t *lst, const char __far *filename) {
    while (true) {
        memset(lst, 0, sizeof(ui_popup_list_config_t));
        lst->option[0] = lang_keys[LK_SUBMENU_OPTION_TOOLS_LAUNCH_VIA_XMODEM];
        lst->option[1] = lang_keys[LK_SUBMENU_OPTION_WITCH];
        lst->option[2] = lang_keys[LK_SUBMENU_OPTION_CONTROLLER_MODE];
        lst->option[3] = lang_keys[LK_SUBMENU_OPTION_TOOLS_SCRAPE_ROMS];
        // only if this folder has a ROM index
        if (f_exists_far(s_file_index_name))
            lst->option[4] = lang_keys[LK_SUBMENU_OPTION_TOOLS_REMOVE_INDEX];

        switch (ui_popup_list(lst)) {
        case UI_POPUP_ACTION_START:
            return TRISTATE_FALSE;
        default:
            ui_popup_list_clear(lst);
            return TRISTATE_NONE;
        case 0: {
            uint32_t size = 0;
            int16_t result = xmodem_recv_to_psram(0, &size);
            if (result == FR_OK) {
                result = launch_in_psram(size);
            }

            ui_dialog_error_check(result, lang_keys[LK_SUBMENU_OPTION_TOOLS_LAUNCH_VIA_XMODEM], 0);
            return TRISTATE_TRUE;
        }
        case 1: {
            enum tristate result = ui_file_selector_tools_witch(lst, filename);
            if (result != TRISTATE_NONE)
                return result;
            break;
        }
        case 2:
            ui_hidctrl();
            return TRISTATE_TRUE;
        case 3:
            ui_popup_list_clear(lst);
            ui_draw_statusbar(lang_keys[LK_UI_STATUS_SCRAPING]);
            ui_dialog_error_check(ui_file_selector_scrape_directory(), lang_keys[LK_SUBMENU_OPTION_TOOLS_SCRAPE_ROMS], 0);
            return TRISTATE_TRUE;
        case 4:
            ui_dialog_error_check(ui_file_selector_remove_index(), lang_keys[LK_SUBMENU_OPTION_TOOLS_REMOVE_INDEX], 0);
            return TRISTATE_TRUE;
        }
    }
}

static enum tristate ui_file_selector_file(ui_popup_list_config_t *lst, const char __far *filename) {
    char path[FF_LFN_BUF + 1];
    bool has_path = false;
    // filename is in the current directory: make it a whole path
    if (settings.favorites_mode != SETTING_FAVORITES_OFF)
        has_path = favorites_full_path(path, sizeof(path), filename);

    while (true) {
        uint8_t options[4];
        int i = 0;
        memset(lst, 0, sizeof(ui_popup_list_config_t));
        if (has_path) {
            options[i] = 2;
            lst->option[i++] = lang_keys[favorites_contains(path) ? LK_SUBMENU_OPTION_FILE_REMOVE_FAVORITE : LK_SUBMENU_OPTION_FILE_ADD_FAVORITE];
        }
        if (fileops_is_rom(filename)) {
            options[i] = 1;
            lst->option[i++] = lang_keys[LK_SUBMENU_OPTION_FILE_ERASE_SAVE];
        }
        options[i] = 0;
        lst->option[i++] = lang_keys[LK_SUBMENU_OPTION_FILE_DELETE];

        int result = ui_popup_list(lst);
        if (result >= 0) {
            result = options[result];
        }
        switch (result) {
        case UI_POPUP_ACTION_START:
            return TRISTATE_FALSE;
        default:
            ui_popup_list_clear(lst);
            return TRISTATE_NONE;
        case 0: {
            int16_t result = ui_fileops_check_file_delete_by_user(filename);
            // the ROM index would still list the deleted file: drop it
            if (result == FR_OK)
                ui_file_selector_remove_index();
            ui_dialog_error_check(result, lang_keys[LK_SUBMENU_OPTION_FILE_DELETE], 0);
            return TRISTATE_TRUE;
        }
        case 1: {
            int16_t result = ui_fileops_check_file_erase_savedata(filename);
            ui_dialog_error_check(result, lang_keys[LK_SUBMENU_OPTION_FILE_ERASE_SAVE], 0);
            return TRISTATE_TRUE;
        }
        case 2: {
            bool added;
            ui_dialog_error_check(favorites_toggle(path, &added), NULL, 0);
            return TRISTATE_TRUE;
        }
        }
    }
}

#define OPTION_OPEN_FAVORITES 3
#define OPTION_OPEN_RECENT    4
#define OPTION_IMAGES         5

bool ui_file_selector_options(const char __far *filename, uint8_t attrib, uint8_t *open_list) {
    ui_popup_list_config_t lst;

    while (true) {
        memset(&lst, 0, sizeof(ui_popup_list_config_t));
        uint8_t options[6];
        int i = 0;
        if (settings.favorites_mode == SETTING_FAVORITES_START_MENU) {
            options[i] = OPTION_OPEN_FAVORITES;
            lst.option[i++] = lang_keys[LK_UI_FILE_SELECTOR_FAVORITES];
            options[i] = OPTION_OPEN_RECENT;
            lst.option[i++] = lang_keys[LK_UI_FILE_SELECTOR_RECENT];
        }
        if (!(attrib & AM_DIR)) {
            options[i] = 0;
            lst.option[i++] = lang_keys[LK_SUBMENU_OPTION_FILE];
#ifdef CONFIG_ENABLE_WALLPAPER
            if (ws_system_is_color_active()) {
                options[i] = OPTION_IMAGES;
                lst.option[i++] = lang_keys[LK_SUBMENU_OPTION_IMAGES];
            }
#endif
        }
        options[i] = 1;
        lst.option[i++] = lang_keys[LK_SUBMENU_OPTION_TOOLS];
        options[i] = 2;
        lst.option[i++] = lang_keys[LK_SUBMENU_OPTION_SETTINGS];

        int option = ui_popup_list(&lst);
        if (option >= 0) option = options[option];

        enum tristate result = TRISTATE_NONE;
        switch (option) {
        default:
            result = TRISTATE_FALSE;
            break;
        case 0:
            result = ui_file_selector_file(&lst, filename);
            break;
        case 1:
            result = ui_file_selector_tools(&lst, filename);
            break;
        case 2:
            ui_settings(&settings_root);
            result = TRISTATE_TRUE;
            break;
        case OPTION_OPEN_FAVORITES:
            *open_list = FAVORITES_LIST_FAVORITES;
            result = TRISTATE_TRUE;
            break;
        case OPTION_OPEN_RECENT:
            *open_list = FAVORITES_LIST_RECENT;
            result = TRISTATE_TRUE;
            break;
        case OPTION_IMAGES:
            *open_list = FILE_SELECTOR_OPTIONS_IMAGES;
            result = TRISTATE_FALSE;
            break;
        }

        if (result != TRISTATE_NONE)
            return result != TRISTATE_FALSE;
    }
}
