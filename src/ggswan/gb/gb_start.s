/**
 * gbswan - the stack below the tiles.
 * The Game Boy's video memory is put at 2000h-3FFFh of the internal RAM
 * (the WonderSwan Color's first bank of 2 bpp tiles), where the C library
 * has the stack (its top is 4000h). gb_run moves it under 2000h, above the
 * C data (the Makefile checks that this ends below 1C00h), and calls
 * gb_main, which never
 * comes back. Interrupts: off, as the C start-up leaves them (none is
 * used yet).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <wonderful.h>
#include <ws.h>

    .arch   i186
    .code16
    .intel_syntax noprefix

    .section .text, "ax"

    .global gb_run
gb_run:
    cli
    mov sp, 0x2000
    IA16_CALL gb_main
1:
    hlt
    jmp 1b
