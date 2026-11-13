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
 * has the stack (its top is 4000h). gb_run moves it to D000h-D7FFh, above
 * the Game Boy's memory and the picture's lists (GB-MEMORY-MAP.md), and
 * calls gb_emulate, which never comes back.
 */
    .global gb_run
gb_run:
    cli
    mov sp, 0xD800
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
 * The line interrupt: the next entry of the list of changes (16 bytes:
 * the WonderSwan line to do it at or FFh for no more, screen 2's X and Y
 * scroll, the screens' maps, the display control, screen 1's X and Y
 * scroll, the right edge of what is seen of screen 2, then the
 * background's four colors: the backdrop and palette 4's 1-3), which
 * shows from the line after. Then the interrupt is set for the entry
 * after.
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
    cmp byte ptr ss:[bx + 9], 0xFF      // (a short entry: only the scroll. gb_video.c, make_list)
    je 1f
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
    mov ax, ss:[bx + 8]
    mov ss:[0xFE00], ax
    mov ax, ss:[bx + 10]
    mov ss:[0xFE82], ax
    mov ax, ss:[bx + 12]
    mov ss:[0xFE84], ax
    mov ax, ss:[bx + 14]
    mov ss:[0xFE86], ax
1:
    add bx, 16
    mov ss:[gbv_split_ptr], bx
    mov al, ss:[bx]
    out 0x03, al
    mov al, 0x10
    out 0xB6, al
    pop bx
    pop ax
    iret

/*
 * uint8_t gbv_sprites_asm(void): the WonderSwan's sprite table
 * (5800h) from the Game Boy's OAM (9E00h): the sprites on the screen,
 * leftmost first and, with the same X, first in the OAM first (who covers
 * whom on the DMG; the WS puts the first of its table on top). An 8 x 16
 * sprite is two: the size is LCDC bit 2 at the sprite's top line (the
 * table at CB80h, made by gb_video.c). Returns how many WS sprites.
 * A WS sprite: tile, attributes (palette 12 or 13, over screen 2 unless
 * the Game Boy has it behind the background, the flips), Y, X.
 */
#define SPR_ORDER 0xCB00        /* 40 bytes: the sprites in last frame's order, as offsets into the OAM */
#define SPR_TALL  0xCB80        /* 176 bytes, by a sprite's Y: 8 x 16 where its top line is (LCDC bit 2 there) */
#define SPR_KEYS  0xCB30        /* 40 words being sorted: X (FFh: not on the screen), offset */
    .global gbv_sprites_asm
gbv_sprites_asm:
    push si
    push di
    push bp
    push es
    push ss
    pop es
    // the 40 sprites, in the order they had a frame ago (few change
    // places from one frame to the next), each put in its place among
    // those before it: by X, then by its place in the OAM; the ones not
    // on the screen last
    xor si, si
1:
    xor bx, bx
    mov bl, ss:[SPR_ORDER + si]
    mov ax, ss:[0x9E00 + bx]    // AL = Y, AH = X
    test al, al
    jz 5f
    cmp al, 160
    jae 5f
    test ah, ah
    jz 5f
    cmp ah, 168
    jae 5f
    mov bp, ax
    and bp, 0xFF
    cmp byte ptr [bp + SPR_TALL], 0
    jnz 6f
    cmp al, 8
    ja 6f                       // (8 x 8, above the screen: not seen)
5:
    mov ah, 0xFF
6:
    mov al, bl
    mov di, si
    shl di, 1
2:
    test di, di
    jz 3f
    mov dx, ss:[SPR_KEYS - 2 + di]
    cmp dx, ax
    jbe 3f
    mov ss:[SPR_KEYS + di], dx
    sub di, 2
    jmp 2b
3:
    mov ss:[SPR_KEYS + di], ax
    inc si
    cmp si, 40
    jb 1b

    mov di, 0x5800
    xor dx, dx                  // WS sprites written
    xor si, si
6:
    mov bx, ss:[SPR_KEYS + si]
    shr si, 1
    mov ss:[SPR_ORDER + si], bl
    shl si, 1
    cmp bh, 0xFF
    je 8f
    xor bh, bh
    mov ax, ss:[0x9E02 + bx]    // AL = tile, AH = flags
    push si
    mov si, ax
    shr si, 12                  // the flags' high half
    mov ah, cs:[spr_attr + si]
    pop si
    mov cx, ss:[0x9E00 + bx]    // CL = Y, CH = X
    mov bp, cx
    and bp, 0xFF
    sub cl, 16
    add ch, 24
    cmp byte ptr [bp + SPR_TALL], 0
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
    add si, 2
    cmp si, 80
    jb 6b
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

/*
 * uint8_t gbd_ram_cmp(void): a block of code in RAM against the copy of
 * its bytes kept before its translation (gb_drc.c, ram_same), leaving out
 * the bytes that are read as the code runs (a bit each, in the 16 bytes
 * that end 18 before the copy). 1: the same.
 */
    .global gbd_ram_cmp
gbd_ram_cmp:
    push si
    push di
    push es
    les di, ss:[drc_cmp_p]
    mov si, ss:[drc_cmp_ram]
    mov bx, di
    xor cx, cx
    mov cl, ss:[drc_cmp_len]
    cld
1:
    repe cmpsb
    je 8f
    mov ax, di
    dec ax
    sub ax, bx                  // which byte
    mov dx, ax
    shr ax, 3
    push bx
    add bx, ax
    mov al, es:[bx - 18]
    pop bx
    push cx
    mov cl, dl
    and cl, 7
    shr al, cl
    pop cx
    test al, 1
    jz 9f
    jcxz 8f
    jmp 1b
8:
    mov al, 1
    jmp 7f
9:
    xor al, al
7:
    pop es
    pop di
    pop si
    IA16_RET
