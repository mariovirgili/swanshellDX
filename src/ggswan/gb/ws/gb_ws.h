/**
 * gbswan - the plugin's parts, to one another.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GB_WS_H
#define GB_WS_H

#include <stdint.h>

void gb_run(void);              // gb_asm.s: the stack moved, then gb_emulate
void gb_emulate(void);
void gb_vblank_entry(void);
void gb_line_entry(void);
void gb_vblank_irq(void);
void gb_frame_keys(void);       // gb_main.c: the keys, once a frame (and the way out)
extern volatile uint16_t gbv_vblanks;

#endif
