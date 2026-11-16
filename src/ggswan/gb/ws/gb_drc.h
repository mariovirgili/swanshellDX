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
#define GB_IME 18
#define GB_HALTED 20
#define GB_FRAME_DONE 51

#ifndef __ASSEMBLER__
#include <stdint.h>
void gbd_init(void);
void gbd_lin_open(uint16_t seg);    // writes to a segment of the linear area: through the RAM window
void gbd_lin_close(void);
void gbd_lin_put(uint16_t seg, uint16_t off, const void __far *src, uint16_t n);
extern uint16_t drc_lin_hi, drc_lin_mask;
void gbd_flush(void);           // everything translated forgotten
void gbd_run_frame(void);
void gbd_enter(uint16_t off, uint16_t seg);     // gb_drc_asm.s: runs translated code until it leaves
extern uint8_t drc_ah, drc_n, drc_tmp, drc_ei, drc_interp;
extern uint16_t drc_link, drc_link_seg, drc_segtab[4];
extern uint16_t drc_key0, drc_keyx;     // the table's key of the ROM bank at 0000h and at 4000h
extern uint16_t drc_hash0, drc_hashx, drc_tseg;     // the keys' part of the table's hash; the table's segment
extern uint8_t drc_must, drc_ram_ok;
extern uint8_t drc_mbc_and, drc_mbc_z1, drc_p1_tab[4];
extern uint16_t drc_mbc_top, drc_mbc_lo, drc_mbc_or, drc_mbc_hi, drc_bank_mask;
extern uint8_t drc_ramg;
void gbd_keys(void);            // the keys changed
extern uint16_t drc_ram_seg;
void gbd_write(uint16_t a, uint8_t v);
// the routines copied to the start of every bank of code (gb_drc_asm.s)
extern const uint8_t __far drc_stubs[];
extern const uint16_t __far drc_stubs_size;
extern const uint16_t __far drc_stub_offs[];
#endif

/* where the routines are in a bank of code: a jump each, 3 bytes apart */
/* The small tables s_exit_dyn looks in first (gb_drc_asm.s): 512 places
   of 8 bytes as the blocks' table's, in the internal RAM */
#define DRC_RC0 0xD800
#define DRC_RCX 0xE800

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
#define ST_DAA       42     /* DAA: A in AL, the flags in AH (N from drc_n) */
#define ST_STAT      45     /* AL = STAT */
#define ST_LY        48     /* AL = LY */
#define ST_WAIT_STAT 51     /* AL = STAT, in a loop that goes round while the mode is in the byte after the call: at the first cycle it is not */
#define ST_DELAY_BC  54     /* DEC BC / LD A, B / OR C / JR NZ: all the turns there is time for but the last */
#define ST_EXIT_DYN_F 57    /* ST_EXIT_DYN straight after the cycles were taken off gbf_cyc: the flags are that subtraction's */
#define ST_COPY0     60     /* the copying loops (gb_drc_asm.s: s_copy0-4): all the turns but the last at once */
#define ST_COPY1     63
#define ST_COPY2     66
#define ST_COPY3     69
#define ST_COPY4     72
#define ST_FILL0     75     /* LD (HL+), A / DEC B / JR NZ, as the copying loops */
#define ST_SKIP_HL   78     /* a loop on the byte at HL: LY, to the next line; RAM, to the next event */
#define ST_VREG      81     /* LDH (n), A to SCY, SCX, BGP, WY or WX: the register and the frame's list of changes */
#define ST_BANKX     84     /* LD (nn), A to the second ROM area's bank register */
#define ST_BANK0     87     /* the same, from a block in the first ROM area */
#define ST_WRV       90     /* a write the block knows is to the video memory */
#define ST_FAR       93     /* a byte copied from another ROM bank and the bank set back (gb_drc_asm.s, s_far) */
#define ST_FILL1     96     /* s_fill0 with D for the count */
#define ST_COUNT     33

#endif
