/**
 * gbswan - assembly: the stack moved below the Game Boy's video memory,
 * and the WonderSwan's two interrupts (the vertical blank's, which calls
 * C; the line's, which changes the scroll in the middle of the picture).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <wonderful.h>
#include <ws.h>

    .arch   i186
    .code16
    .intel_syntax noprefix

    .section .text, "ax"

/*
 * The Game Boy's video memory is put at 2000h-3FFFh of the internal RAM
 * (the WonderSwan Color's first bank of 2 bpp tiles), where the C library
 * has the stack (its top is 4000h). gb_run moves it under 2000h, above the
 * C data (the Makefile checks that this ends below 1C00h), and calls
 * gb_emulate, which never comes back.
 */
    .global gb_run
gb_run:
    cli
    mov sp, 0x2000
    IA16_CALL gb_emulate
1:
    hlt
    jmp 1b

    .global gb_vblank_entry
gb_vblank_entry:
    pusha
    push ds
    push es
    push ss
    pop ds
    cld
    IA16_CALL gb_vblank_irq
    pop es
    pop ds
    popa
    iret

/*
 * The line interrupt: the next entry of the list of changes (8 bytes: the
 * WonderSwan line to do it at or FFh for no more, screen 2's X and Y
 * scroll, the screens' maps, the display control, screen 1's X and Y
 * scroll, the right edge of what is seen of screen 2), which shows from
 * the line after. Then the interrupt is set for the entry after.
 */
    .global gb_line_entry
gb_line_entry:
    push ax
    push bx
    mov bx, ss:[gbv_split_ptr]
    mov al, ss:[bx + 1]
    out 0x12, al
    mov al, ss:[bx + 2]
    out 0x13, al
    mov al, ss:[bx + 3]
    out 0x07, al
    mov al, ss:[bx + 4]
    out 0x00, al
    mov al, ss:[bx + 5]
    out 0x10, al
    mov al, ss:[bx + 6]
    out 0x11, al
    mov al, ss:[bx + 7]
    out 0x0A, al
    add bx, 8
    mov ss:[gbv_split_ptr], bx
    mov al, ss:[bx]
    out 0x03, al
    mov al, 0x10
    out 0xB6, al
    pop bx
    pop ax
    iret
