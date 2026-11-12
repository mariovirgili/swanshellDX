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

/*
 * uint8_t gbv_sprites_asm(uint8_t tall): the WonderSwan's sprite table
 * (5800h) from the Game Boy's OAM (9E00h): the sprites on the screen,
 * leftmost first and, with the same X, first in the OAM first (who covers
 * whom on the DMG; the WS puts the first of its table on top). An 8 x 16
 * sprite (tall) is two. Returns how many WS sprites.
 * A WS sprite: tile, attributes (palette 12 or 13, over screen 2 unless
 * the Game Boy has it behind the background, the flips), Y, X.
 */
#define SPR_ORDER 0xB940        /* 40 bytes: the sprites in order, as offsets into the OAM */
    .global gbv_sprites_asm
gbv_sprites_asm:
    push si
    push di
    push bp
    push es
    push ss
    pop es
    xor ah, ah
    mov bp, ax                  // tall
    xor cx, cx                  // sprites found
    mov si, 0x9E00
1:
    mov ax, ss:[si]             // AL = Y, AH = X
    test al, al
    jz 5f
    cmp al, 160
    jae 5f
    test ah, ah
    jz 5f
    cmp ah, 168
    jae 5f
    test bp, bp
    jnz 2f
    cmp al, 8
    jbe 5f                      // (8 x 8, above the screen)
2:
    // its place: after the last one not to its right
    mov bx, cx
3:
    test bx, bx
    jz 4f
    mov dl, ss:[SPR_ORDER - 1 + bx]
    xor dh, dh
    mov di, dx
    cmp ss:[0x9E01 + di], ah
    jbe 4f
    mov ss:[SPR_ORDER + bx], dl
    dec bx
    jmp 3b
4:
    mov dx, si
    mov ss:[SPR_ORDER + bx], dl     // (the low byte: 00h-9Ch)
    inc cx
5:
    add si, 4
    cmp si, 0x9EA0
    jb 1b

    mov di, 0x5800
    xor dx, dx                  // WS sprites written
    xor si, si
    jcxz 9f
6:
    xor bx, bx
    mov bl, ss:[SPR_ORDER + si]
    mov ax, ss:[0x9E02 + bx]    // AL = tile, AH = flags
    push si
    mov si, ax
    shr si, 12                  // the flags' high half
    mov ah, cs:[spr_attr + si]
    pop si
    push cx
    mov cx, ss:[0x9E00 + bx]    // CL = Y, CH = X
    sub cl, 16
    add ch, 24
    test bp, bp
    jnz 7f
    stosw
    mov ax, cx
    stosw
    inc dx
    jmp 8f
7:
    // 8 x 16: the even tile on top, the odd one below; the other way
    // round when flipped upside down
    and al, 0xFE
    test ah, 0x80
    jz 1f
    or al, 1
1:
    stosw
    xchg ax, cx
    stosw
    xchg ax, cx
    xor al, 1
    stosw
    mov ax, cx
    add al, 8
    stosw
    add dx, 2
8:
    pop cx
    inc si
    loop 6b
9:
    mov ax, dx
    pop es
    pop bp
    pop di
    pop si
    IA16_RET
// the WS attribute byte for the Game Boy's flags' high half (bit 3
// behind the background, 2 upside down, 1 mirrored, 0 the second palette)
spr_attr:
    .byte 0x28, 0x2A, 0x68, 0x6A, 0xA8, 0xAA, 0xE8, 0xEA
    .byte 0x08, 0x0A, 0x48, 0x4A, 0x88, 0x8A, 0xC8, 0xCA
