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
void gbv_stats_toggle(void);
// what the plugin is doing (the figures on the screen)
#define GB_PHASE_RUN   0    // translated code, its routines, what they call
#define GB_PHASE_DISP  1    // the dispatcher, the events, the interpreter's steps
#define GB_PHASE_TRANS 2    // translating
#define GB_PHASE_VIDEO 3    // the frame's end: the lists, the sprites, the keys
#define GB_PHASE_IDLE  4    // waiting for the frame's time
#define GB_PHASES 5
extern volatile uint8_t gb_phase;
extern uint16_t drc_blocks, drc_flushes;

#endif
