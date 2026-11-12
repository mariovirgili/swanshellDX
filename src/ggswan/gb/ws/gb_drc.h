/**
 * gbswan - the recompiler (SM83 -> V30MZ): what its parts share. GB-ABI.md
 * says how the translated code keeps the Game Boy's registers and flags.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GB_DRC_H
#define GB_DRC_H

#define DRC_TABLE_SEG 0x4000u   /* the blocks' table (a PSRAM bank through the linear area) */
#define DRC_CODE_SEG  0x5000u   /* the first bank of translated code */
#define DRC_CODE_SEGS 8         /* 5000h-C000h */

/* gb_t's first fields (gb.h; checked in gb_drc.c) */
#define GB_A  0
#define GB_BC 2
#define GB_DE 4
#define GB_HL 6
#define GB_SP 8
#define GB_PC 10
#define GB_LCD_ON 12
#define GB_KEYS 13
#define GB_BANK_LO 14
#define GB_ROMX 16

#ifndef __ASSEMBLER__
#include <stdint.h>
void gbd_init(void);
void gbd_run_frame(void);
void gbd_enter(uint16_t off, uint16_t seg);     // gb_drc_asm.s: runs translated code until it leaves
extern uint8_t drc_ah, drc_n, drc_tmp, drc_ei, drc_interp;
extern uint16_t drc_link, drc_link_seg, drc_segtab[4];
extern uint16_t drc_key0, drc_keyx;     // the table's key of the ROM bank at 0000h and at 4000h
extern uint16_t drc_jmp[2];
extern uint8_t drc_must, drc_ram_ok;
extern uint8_t drc_mbc_and, drc_mbc_z1, drc_p1_tab[4];
extern uint16_t drc_mbc_top, drc_mbc_hi, drc_bank_mask;
void gbd_keys(void);            // the keys changed
extern uint16_t drc_ram_seg;
void gbd_write(uint16_t a, uint8_t v);
extern uint8_t gbv_tiles_8000;
// the routines copied to the start of every bank of code (gb_drc_asm.s)
extern const uint8_t __far drc_stubs[];
extern const uint16_t __far drc_stubs_size;
#endif

/* where the routines are in a bank of code: a jump each, 3 bytes apart */
#define ST_EXIT      0      /* leave: gb.pc is set */
#define ST_EXIT_DYN  3      /* leave to the address in SI */
#define ST_RD        6      /* AL = the byte at SI */
#define ST_RDT       9      /* drc_tmp = the byte at SI */
#define ST_WRT       12     /* the byte at SI = drc_tmp; the next instruction's address follows the call */
#define ST_PUSH      15     /* push SI */
#define ST_POP       18     /* SI = pop */
#define ST_PUSH_AF   21
#define ST_POP_AF    24
#define ST_INC_HL    27     /* INC (HL): flags in AH on entry and exit; the next address follows */
#define ST_DEC_HL    30
#define ST_SKIP_LINE 33     /* a loop that waits on LY: the time to the next line passed */
#define ST_SKIP_MODE 36     /* ... on STAT: to the next change of mode */
#define ST_P1        39     /* P1 = AL (the keys' rows selected) */
#define ST_COUNT     14

#endif
