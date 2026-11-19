/**
 * Copyright (c) 2024 Adrian Siekierka
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

#include <nile/fpga.h>
#include <nilefs/ff.h>
#include <wonderful.h>
#include <ws.h>
#include <ws/display.h>
#include <ws/util.h>
#include <wsx/planar_unpack.h>
#include "cart/irq.h"
#include "cart/status.h"
#include "errors.h"
#include "fs.h"
#include "lang.h"
#include "lang_gen.h"
#include "main.h"
#include "cart/mcu.h"
#include "launch/launch.h"
#include "settings.h"
#include "ui/bitmap.h"
#include "ui/ui.h"
#include "ui/ui_dialog.h"
#include "ui/ui_file_selector.h"
#include "ui/ui_popup_dialog.h"
#include "ui/ui_settings.h"
#include "util/input.h"
#include "shell/shell.h"
#include "ui/ui_icons.h"
#include "menu_music.h"
#include "strings.h"

volatile uint16_t vbl_ticks;
static uint8_t last_signaled_icon = 0;

__attribute__((assume_ss_data, interrupt))
void __far vblank_int_handler(void) {
	ws_int_ack(WS_INT_ACK_VBLANK);
	ia16_enable_irq();
	vbl_ticks++;
	vblank_input_update();
}

// The console's own battery (the AA cell): the hardware says only "low",
// with an NMI, when bit 4 of port B7h is set. Asked once, at start: if the
// NMI comes in the next 50 ms a warning is shown for a second. (Not known:
// whether the NMI comes when the battery is already low as the bit is
// set, or only as it becomes so - the documentation does not say, and it
// has not been tried with a spent cell. If it is the second, this never
// shows anything.)
static volatile uint8_t console_battery_low;

__attribute__((assume_ss_data, interrupt))
void __far low_battery_nmi_handler(void) {
	outportb(WS_INT_NMI_CTRL_PORT, 0);
	console_battery_low = 1;
}

static void console_battery_check(void) {
	// (the NMI is the CPU's interrupt 2: its vector at 0000:0008)
	*(void __far * __far *) MK_FP(0x0000, 2 * 4) = (void __far *) low_battery_nmi_handler;
	outportb(WS_INT_NMI_CTRL_PORT, WS_INT_NMI_CTRL_LOW_BATTERY);
	ws_delay_ms(50);
	outportb(WS_INT_NMI_CTRL_PORT, 0);
	if (!console_battery_low) return;

	ui_popup_dialog_config_t dlg = {0};
	dlg.title = lang_keys[LK_WARNING_CONSOLE_BATTERY_LOW];
	ui_popup_dialog_draw(&dlg);
	ui_show();
	ws_delay_ms(1000);
	ui_layout_clear(0);
}

void signal_display_orientation(uint8_t orientation) {
    outportb(WS_LCD_ICON_PORT, (inportb(WS_LCD_ICON_PORT) & ~(WS_LCD_ICON_ORIENT_H | WS_LCD_ICON_ORIENT_V)) | orientation);
    last_signaled_icon = orientation;
}

bool idle_until_vblank(void) {
	uint16_t vbl_ticks_last = vbl_ticks;
	bool refresh_view = false;

	switch (vbl_ticks & 63) {
	    case 31: {
			cart_irq_update();
		} break;
		case 63: {
			cart_status_update();
			ui_icon_update();

			uint8_t next_signaled_icon = bitmap_rotation ? WS_LCD_ICON_ORIENT_H : WS_LCD_ICON_ORIENT_V;
			if (last_signaled_icon != next_signaled_icon) {
				outportb(WS_LCD_ICON_PORT, (inportb(WS_LCD_ICON_PORT) & ~(WS_LCD_ICON_ORIENT_H | WS_LCD_ICON_ORIENT_V)) | next_signaled_icon);
				last_signaled_icon = next_signaled_icon;
			} else {
				outportb(WS_LCD_ICON_PORT, inportb(WS_LCD_ICON_PORT) & ~(WS_LCD_ICON_ORIENT_H | WS_LCD_ICON_ORIENT_V));
			}
		} break;
		default: {
			refresh_view = shell_tick();
		} break;
	}
	while (vbl_ticks == vbl_ticks_last) {
		ia16_halt();
	}

	return refresh_view;
}

void wait_for_vblank(void) {
	uint16_t vbl_ticks_last = vbl_ticks;

	while (vbl_ticks == vbl_ticks_last) {
		ia16_halt();
	}
}

void factory_reset(void) {
    f_unlink_far(s_path_save_ini);
    f_unlink_far(s_path_config_ini);
    nile_soft_reset();
}

void main(void) {
	ia16_disable_irq();
	// after START + Y1 in a game the game's sound may still be playing
	menu_sound_reset();

	bool is_safe_mode = (inportb(IO_NILE_SPI_CNT+1) & (NILE_SPI_CLOCK_CART >> 8)) != 0;

	settings_reset();

	ws_int_set_handler(WS_INT_VBLANK, vblank_int_handler);
	ws_int_enable(WS_INT_ENABLE_VBLANK);
    ws_int_ack_all();

	ia16_enable_irq();

	ui_init();
	ui_layout_clear(0);

	console_battery_check();

	{
		const char __far *title = lang_keys[LK_NAME];
		bitmapfont_draw_string(&ui_bitmap,
			(screen_width - bitmapfont_get_string_width(title, 255)) >> 1,
			(screen_height - 16) >> 1,
			title, 255);
	}
	ui_show();

	fs_init();
	outportw(WS_CART_EXTBANK_RAM_PORT, 0);

	bitmap_set_screen_force_horizontal(false);

	{
		int16_t mcu_reset_result = mcu_reset(true);
		int16_t result = cart_status_init(is_safe_mode, !mcu_reset_result);
		if (mcu_reset_result && mcu_reset_result != ERR_MCU_COMM_FAILED) {
			ui_dialog_error_check(mcu_reset_result, lang_keys[LK_ERROR_TITLE_MCU_INIT], 0);
		}
		if (result) {
			ui_popup_dialog_config_t dlg = {0};
			char error_name_buffer[ERROR_TO_POINTER_BUFFER_SIZE];

			dlg.title = error_to_pointer(result, error_name_buffer, sizeof(error_name_buffer));
			dlg.description = lang_keys[LK_PROMPT_FUNCTIONALITY_UNAVAILABLE];
			dlg.buttons[0] = LK_OK;

			ui_popup_dialog_draw(&dlg);
			ui_show();

			ui_popup_dialog_action(&dlg, 0);
		}
	}

	{
		int16_t result = settings_load();
		if (result == FR_NO_FILE || result == FR_NO_PATH) {
			ui_popup_dialog_config_t cfg = {0};
			cfg.title = lang_keys_en[LK_DIALOG_SETTINGS_CREATING_NEW];
			ui_popup_dialog_draw(&cfg);
			ui_show();
			ws_delay_ms(500);
			settings.language = SETTINGS_LANG_EN;
			settings.language = ui_settings_selector(&setting_language, settings.language);
			settings_language_update();
			ui_layout_clear(0);
			settings_save();
		} else {
			ui_dialog_error_check(result, lang_keys[LK_ERROR_TITLE_SETTINGS_LOAD], 0);
		}
	}

	ui_dialog_error_check(launch_backup_save_data(), lang_keys[LK_ERROR_TITLE_SAVE_STORE], 0);

	shell_init();
	// before the menu music: it frees and allocates asset heap banks later
	ui_icons_init();
	menu_music_init();

	// Run ui_show() again to intiialize wallpaper
	ui_layout_bars();
	ui_show();
	ui_file_selector();

	while(1);
}
