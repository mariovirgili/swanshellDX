/**
 * gbswan - the picture of the plugin's machine on the PC: the reference's
 * line renderer, run for the lines not drawn yet before anything they are
 * made of changes.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include "gb.h"
#include "gbv.h"

static uint8_t drawn;

static void catch_up(uint8_t upto) {
    if (!gb.lcd_on) return;
    while (drawn < upto && drawn < 144)
        gb_ppu_draw_line(drawn++);
}

void gbv_reset(void) {
    drawn = 0;
    memset(gb_screen, 0, sizeof(gb_screen));
}
void gbv_reg(uint8_t reg, uint8_t v) {
    catch_up(gbf_draw_line());
    if (reg == GB_LCDC && !(v & 0x80) && gb.lcd_on) {
        memset(gb_screen, 0, sizeof(gb_screen));
        drawn = 0;
    }
}
void gbv_vram(uint16_t off, uint8_t v) {
    catch_up(gbf_draw_line());
}
void gbv_oam(void) {
    catch_up(gbf_draw_line());
}
void gbv_frame(void) {
    catch_up(144);
    drawn = 0;
}
