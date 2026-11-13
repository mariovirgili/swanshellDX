/**
 * gbswan - the picture's backend of the plugin's machine (gbf_hw.c): told
 * before anything the picture is made of changes. On the WonderSwan it
 * keeps the WonderSwan's screens, sprites and palettes (ws/gb_video.c); on
 * the PC it draws the lines not drawn yet with the reference's renderer
 * (host/gbv_host.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GBV_H
#define GBV_H

#include "gb.h"

void gbv_reset(void);
// an LCD register (GB_LCDC ... GB_WX) is about to become v
void gbv_reg(uint8_t reg, uint8_t v);
// a byte of the video memory (off: 0-1FFFh) is about to become v
void gbv_vram(uint16_t off, uint8_t v);
// the OAM is about to change
void gbv_oam(void);
// the frame is over (the start of the vertical blank; with the LCD off,
// every frame's time): show it, wait for its time, read the keys
void gbv_frame(void);

#ifdef __ia16__
// (the WonderSwan only, gb_video.c: the patterns of sprites a frame late)
#define GBV_PATTERNS_LATE
extern uint8_t gbv_tq_on;
void gbv_tq_put(uint16_t a, uint8_t v);
void gbv_line0(void);
#endif

#endif
