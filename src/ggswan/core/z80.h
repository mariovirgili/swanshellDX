/**
 * ggswan - Game Gear emulator for the WonderSwan (nileswan)
 * Z80 interpreter core. Portable C: builds with host gcc (tests) and with
 * gcc-ia16 (Wonderful toolchain).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GGSWAN_Z80_H_
#define GGSWAN_Z80_H_

#include <stdint.h>

typedef struct {
    uint8_t a, f, b, c, d, e, h, l;
    uint8_t a_, f_, b_, c_, d_, e_, h_, l_;
    uint8_t ixh, ixl, iyh, iyl;
    uint16_t sp, pc;
    uint8_t i, r;
    uint8_t iff1, iff2, im;
    uint8_t halted;
    uint8_t ei_pending;  // EI: interrupts accepted after the next instruction
    uint8_t irq;         // level of the INT line (set by the machine)
    int32_t cycles;      // cycles left in the current z80_run() slice
} z80_t;

extern z80_t z80;

#define Z80_FLAG_C 0x01
#define Z80_FLAG_N 0x02
#define Z80_FLAG_P 0x04
#define Z80_FLAG_V 0x04
#define Z80_FLAG_3 0x08
#define Z80_FLAG_H 0x10
#define Z80_FLAG_5 0x20
#define Z80_FLAG_Z 0x40
#define Z80_FLAG_S 0x80

void z80_init(void);
void z80_reset(void);
/// Run for (about) the given number of cycles; returns the overrun (<= 0
/// means not all of them were used, > 0 means more were used).
int32_t z80_run(int32_t cycles);
/// Non-maskable interrupt.
void z80_nmi(void);

// Provided by the machine:
uint8_t z80_read(uint16_t addr);
void z80_write(uint16_t addr, uint8_t value);
uint8_t z80_in(uint8_t port);
void z80_out(uint8_t port, uint8_t value);

#endif
