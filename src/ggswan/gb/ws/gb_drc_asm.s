/**
 * gbswan - the recompiler's assembly: how translated code is entered and
 * left, and the routines it calls (copied to the start of every bank of
 * translated code, so that the calls are near ones). GB-ABI.md.
 *
 * In translated code: AL = A, AH = the flags when they are not in the
 * V30MZ's own (ZF = Z, AF = H, CF = C at LAHF's bits; N is drc_n, 0 or
 * 40h), CX = BC, DX = DE, BX = HL, BP = SP; SI, DI, ES are free; DS = SS =
 * 0. A Game Boy address from 8000h up, but for the cartridge's RAM, is
 * the internal RAM 6000h lower.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <wonderful.h>
#include <ws.h>
#include "gb_drc.h"

    .arch   i186
    .code16
    .intel_syntax noprefix

    .section .text, "ax"

// void gbd_enter(uint16_t off, uint16_t seg): AX, DX
    .global gbd_enter
gbd_enter:
    push bp
    push si
    push di
    push es
    push dx
    push ax
    mov al, ss:[gb + GB_A]
    mov ah, ss:[drc_ah]
    mov cx, ss:[gb + GB_BC]
    mov dx, ss:[gb + GB_DE]
    mov bx, ss:[gb + GB_HL]
    mov bp, ss:[gb + GB_SP]
    retf
    .global gbd_leave
gbd_leave:
    pop es
    pop di
    pop si
    pop bp
    IA16_RET

/* ---- the routines: position independent, entered by the jumps at the
   start (gb_drc.h: ST_*) ---- */

    .section .fartext.gbdrc, "ax"
    .global drc_stubs
drc_stubs:
    .byte 0xE9
    .word s_exit - . - 2
    .byte 0xE9
    .word s_exit_dyn - . - 2
    .byte 0xE9
    .word s_rd - . - 2
    .byte 0xE9
    .word s_rdt - . - 2
    .byte 0xE9
    .word s_wrt - . - 2
    .byte 0xE9
    .word s_push - . - 2
    .byte 0xE9
    .word s_pop - . - 2
    .byte 0xE9
    .word s_push_af - . - 2
    .byte 0xE9
    .word s_pop_af - . - 2
    .byte 0xE9
    .word s_inc_hl - . - 2
    .byte 0xE9
    .word s_dec_hl - . - 2
    .byte 0xE9
    .word s_skip_line - . - 2
    .byte 0xE9
    .word s_skip_mode - . - 2
    .byte 0xE9
    .word s_p1 - . - 2

// leave for the address in SI. When nothing is due (cycles left, no EI
// waiting) and the address is in ROM, the block is looked for here: the
// table's first place for it (gb_drc.c: hash), and if it is there the jump
// is made without the dispatcher
s_exit_dyn:
    cmp word ptr [gbf_cyc], 0
    jle 9f
    cmp byte ptr [drc_ei], 0
    jne 9f
    test si, si
    js 9f
    mov di, [drc_key0]
    cmp si, 0x4000
    jb 1f
    mov di, [drc_keyx]
1:
    push bx
    mov bx, di
    shl bx, 5
    xor bx, si
    push si
    shr si, 6
    xor bx, si
    pop si
    and bx, 4095
    shl bx, 3
    push DRC_TABLE_SEG
    pop es
    cmp es:[bx], si
    jne 8f
    cmp es:[bx + 2], di
    jne 8f
    mov di, es:[bx + 4]
    mov [drc_jmp], di
    mov di, es:[bx + 6]
    mov [drc_jmp + 2], di
    pop bx
    .byte 0xFF, 0x2E            // jmp far [drc_jmp]
    .word drc_jmp
8:
    pop bx
9:
    mov [gb + GB_PC], si
s_exit:
    mov [gb + GB_A], al
    mov [drc_ah], ah
    mov [gb + GB_BC], cx
    mov [gb + GB_DE], dx
    mov [gb + GB_HL], bx
    mov [gb + GB_SP], bp
    IA16_JMP gbd_leave

// AL = the byte at SI (AH and the rest kept; the V30MZ's flags not)
s_rd:
    cmp si, 0x8000
    jae 1f
    mov di, si
    rol di, 2
    and di, 3
    shl di, 1
    mov es, [drc_segtab + di]
    mov al, es:[si]
    ret
1:
    cmp si, 0xFE00
    jae 3f
    cmp si, 0xC000
    jae 2f
    cmp si, 0xA000
    jae 7f
    mov al, [si - 0x6000]       // the video memory
    ret
7:
    // the cartridge's RAM: behind the RAM window when it is plain RAM
    // and enabled (gb_main.c: gbf_plat_map), or C
    cmp byte ptr [drc_ram_ok], 1
    jb 4f
    mov al, 0xFF
    ja 6f                       // (none, or disabled)
    mov es, [drc_ram_seg]
    mov al, es:[si]
6:
    ret
2:
    cmp si, 0xE000
    jb 5f
    sub si, 0x2000              // (the work RAM's echo)
5:
    mov al, [si - 0x6000]
    ret
3:
    cmp si, 0xFF80
    jae 5b                      // the high RAM, IE
    cmp si, 0xFF00
    jb 5b                       // the OAM
    cmp byte ptr [gb + GB_LCD_ON], 0
    je 4f
    cmp si, 0xFF44
    je s_ly
    cmp si, 0xFF41
    je s_stat
4:
    // an I/O register or the cartridge's RAM: C
    mov [drc_ah], ah
    push bx
    push cx
    push dx
    mov ax, si
    IA16_CALL gbf_read
    pop dx
    pop cx
    pop bx
    mov ah, [drc_ah]
    ret

// LY and STAT from the time (gbf_hw.c: gbf_io_read), the LCD on: the
// frame's cycle now is gbf_ev_pos - gbf_cyc, 114 to a line
s_ly:
    mov di, ax
    mov ax, [gbf_ev_pos]
    sub ax, [gbf_cyc]
    push dx
    mov dl, 114
    div dl                      // AL = the line, AH = the cycle in it
    pop dx
    cmp al, 153
    jne 1f
    test ah, ah
    jz 1f
    xor al, al                  // (line 153 reads 0 after its first cycle)
1:
    and di, 0xFF00
    xor ah, ah
    or ax, di
    ret
s_stat:
    push dx
    mov di, ax
    mov ax, [gbf_ev_pos]
    sub ax, [gbf_cyc]
    mov dl, 114
    div dl                      // AL = the line, AH = the cycle in it
    // DL = what LYC is compared with
    mov dl, al
    cmp al, 153
    jne 1f
    mov dl, 0xFF
    cmp ah, 1
    jb 3f
    mov dl, 153
    je 3f
    mov dl, 0xFF
    cmp ah, 2
    je 3f
    xor dl, dl
    jmp 3f
1:
    test ah, ah
    jnz 3f
    test al, al
    jz 3f
    mov dl, 0xFF
3:
    // DH = the mode
    mov dh, 1
    cmp al, 144
    ja 5f
    jne 4f
    test ah, ah
    jnz 5f
    xor dh, dh
    jmp 5f
4:
    xor dh, dh
    cmp ah, 1
    jb 5f
    mov dh, 2
    cmp ah, 21
    jb 5f
    mov dh, 3
    cmp ah, 64
    jb 5f
    xor dh, dh
5:
    mov al, [0x9F41]
    and al, 0x78
    or al, 0x80
    or al, dh
    cmp dl, [0x9F45]
    jne 6f
    or al, 4
6:
    and di, 0xFF00
    xor ah, ah
    or ax, di
    pop dx
    ret

// A loop that only reads LY and tests it: nothing changes before the next
// line, so the time to it is taken off the count at once (to the line's
// second cycle, where LY has changed in every line)
s_skip_line:
    cmp byte ptr [gb + GB_LCD_ON], 0
    je 1f
    push ax
    push dx
    mov ax, [gbf_ev_pos]
    sub ax, [gbf_cyc]
    mov dl, 114
    div dl
    mov al, 115
    sub al, ah                  // 115 - the cycle in the line
    xor ah, ah
    sub [gbf_cyc], ax
    pop dx
    pop ax
1:
    ret
// ... that only reads STAT (and LY): to the next cycle its modes change
// at (1, 21, 64 of the line; the next line's 1)
s_skip_mode:
    cmp byte ptr [gb + GB_LCD_ON], 0
    je 1f
    push ax
    push dx
    mov ax, [gbf_ev_pos]
    sub ax, [gbf_cyc]
    mov dl, 114
    div dl
    mov al, 1
    cmp ah, 1
    jb 2f
    mov al, 21
    cmp ah, 21
    jb 2f
    mov al, 64
    cmp ah, 64
    jb 2f
    mov al, 115
2:
    sub al, ah
    xor ah, ah
    sub [gbf_cyc], ax
    pop dx
    pop ax
1:
    ret

// P1 = AL: the rows selected (bits 4 and 5, 0 selects); what it then
// reads is in a table made when the keys change (gb_drc.c: gbd_keys)
s_p1:
    push bx
    mov bl, al
    shr bl, 4
    and bx, 3
    mov bl, [drc_p1_tab + bx]
    mov [0x9F00], bl
    pop bx
    ret

s_rdt:
    push ax
    call s_rd
    mov [drc_tmp], al
    pop ax
    ret

// the byte at SI = drc_tmp. After the call: the Game Boy address of the
// next instruction (a word), where the run goes on through the dispatcher
// when the write was to the cartridge's registers or to an I/O register
s_wrt:
    cmp si, 0xC000
    jb 1f
    cmp si, 0xFE00
    jae 3f
    cmp si, 0xE000
    jb 2f
    sub si, 0x2000
2:
    mov di, ax
    mov al, [drc_tmp]
    mov [si - 0x6000], al
    mov ax, di
9:
    pop di
    add di, 2
    jmp di
1:
    cmp si, 0x8000
    jb 6f
    cmp si, 0xA000
    jae 5f
    // the video memory; from 9800h, the WonderSwan's map entry too
    mov di, ax
    mov al, [drc_tmp]
    mov [si - 0x6000], al
    cmp si, 0x9800
    jb 4f
    xor ah, ah
    cmp byte ptr [gbv_tiles_8000], 0
    jnz 7f
    cmp al, 0x80
    jae 7f
    inc ah                      // (LCDC bit 4 clear: 0-127 are the patterns from 9000h)
7:
    or ah, 0x08                 // palette 4
    sub si, 0x9800
    shl si, 1
    mov [si + 0x4800], ax
4:
    mov ax, di
    jmp 9b
3:
    cmp si, 0xFF80
    jb 8f
    cmp si, 0xFFFF
    jne 2b                      // the high RAM
    jmp 6f                      // IE: through the dispatcher (an interrupt may be due now)
8:
    cmp si, 0xFF00
    jb 2b                       // the OAM
6:
    // the write that changes the second ROM area's bank, here (2000h up
    // to MBC1's and MBC3's 3FFFh or MBC5's 2FFFh; gb_main.c: gbf_plat_map)
    mov di, si
    shr di, 8
    cmp di, 0x20
    jb 6f
    cmp di, [drc_mbc_top]
    jae 6f
    push ax
    push dx
    mov al, [drc_tmp]
    and al, [drc_mbc_and]
    mov [gb + GB_BANK_LO], al
    jnz 1f
    cmp byte ptr [drc_mbc_z1], 0
    je 1f
    mov al, 1
1:
    xor ah, ah
    or ax, [drc_mbc_hi]
    and ax, [drc_bank_mask]
    cmp ax, [gb + GB_ROMX]
    je 2f
    mov [gb + GB_ROMX], ax
    mov dx, ax
    inc dx
    mov [drc_keyx], dx
    mov dx, ax
    and dx, 3
    shl dx, 14
    mov [gbf_romx_off], dx
    shr dx, 4
    add dx, 0x2C00
    mov [drc_segtab + 2], dx
    shr ax, 2
    out 0xD4, ax                // the ROM1 window's bank
    pop dx
    pop ax
    // (code in the second area itself: through the dispatcher)
    mov di, sp
    mov di, [di]
    mov di, cs:[di]
    cmp di, 0x4000
    jb 9b
    add sp, 2
    mov [gb + GB_PC], di
    jmp s_exit
2:
    pop dx
    pop ax
    jmp 9b
6:
    // the cartridge's registers or an I/O register: C (gb_drc.c:
    // gbd_write), which says whether the dispatcher is needed: 1 yes (an
    // interrupt may be due, the time's events have changed, the first ROM
    // area has another bank), 2 if the code is in the second ROM area
    // (it has another bank), 0 no
    call s_cwrite
    cmp byte ptr [drc_must], 0
    je 9b
    mov di, sp
    mov di, [di]                // (where the call returns to: the word)
    mov di, cs:[di]             // the next instruction's address
    cmp byte ptr [drc_must], 2
    jne 7f
    cmp di, 0x4000
    jb 9b                       // (code in the first ROM area: on)
7:
    add sp, 2
    mov [gb + GB_PC], di
    jmp s_exit
5:
    // the cartridge's RAM
    cmp byte ptr [drc_ram_ok], 1
    jb 8f
    ja 9b                       // (none, or disabled: lost)
    mov es, [drc_ram_seg]
    mov di, ax
    mov al, [drc_tmp]
    mov es:[si], al
    mov ax, di
    jmp 9b
8:
    call s_cwrite
    jmp 9b

// gbf_write(SI, drc_tmp), the registers kept
s_cwrite:
    push ax
    push bx
    push cx
    push dx
    mov ax, si
    mov dl, [drc_tmp]
    IA16_CALL gbd_write
    pop dx
    pop cx
    pop bx
    pop ax
    ret

// push SI on the Game Boy's stack (anywhere: the fast case is inline)
s_push:
    // (a stack in the cartridge's RAM, when that is plain RAM)
    lea di, [bp - 2]
    cmp di, 0xA000
    jb 1f
    cmp bp, 0xC000
    ja 1f
    cmp byte ptr [drc_ram_ok], 1
    jne 1f
    mov es, [drc_ram_seg]
    mov es:[di], si
    mov bp, di
    ret
1:
    push ax
    mov ax, si
    mov [drc_tmp], ah
    lea si, [bp - 1]
    call s_wr_noexit
    mov [drc_tmp], al
    lea si, [bp - 2]
    call s_wr_noexit
    lea bp, [bp - 2]
    pop ax
    ret
// (a write that goes on in any case: gbf_write does it all)
s_wr_noexit:
    cmp si, 0xC000
    jb s_cwrite
    cmp si, 0xFE00
    jae s_cwrite
    cmp si, 0xE000
    jb 1f
    sub si, 0x2000
1:
    push ax
    mov al, [drc_tmp]
    mov [si - 0x6000], al
    pop ax
    ret

// SI = the word popped
s_pop:
    cmp bp, 0xA000
    jb 1f
    cmp bp, 0xBFFE
    ja 1f
    cmp byte ptr [drc_ram_ok], 1
    jne 1f
    mov es, [drc_ram_seg]
    mov si, es:[bp]
    lea bp, [bp + 2]
    ret
1:
    push ax
    mov si, bp
    call s_rd
    mov [drc_tmp], al
    lea si, [bp + 1]
    call s_rd
    mov ah, al
    mov al, [drc_tmp]
    mov si, ax
    lea bp, [bp + 2]
    pop ax
    ret

// PUSH AF: F from AH and drc_n as the Game Boy has it
s_push_af:
    mov di, ax
    mov al, ah
    shl al, 1
    and al, 0xA0                // Z (40h -> 80h), H (10h -> 20h)
    test ah, 1
    jz 1f
    or al, 0x10                 // C
1:
    or al, [drc_n]
    xor ah, ah                  // AX = F
    mov si, di
    xchg ax, si                 // AX = flags:A, SI = 00:F
    mov ah, al
    xor al, al                  // AX = A:00
    or si, ax                   // SI = A:F
    mov ax, di
    jmp s_push

// POP AF
s_pop_af:
    call s_pop                  // SI = A:F
    mov ax, si
    xchg al, ah                 // AL = A, AH = F
    mov di, ax
    and ah, 0x40
    mov [drc_n], ah
    mov ax, di
    shr ah, 1
    and ah, 0x50                // Z (80h -> 40h), H (20h -> 10h)
    test di, 0x1000
    jz 1f
    or ah, 1
1:
    ret

// INC (HL), DEC (HL): the flags in AH before and after (C kept). After the
// call: the next instruction's address, as s_wrt
s_inc_hl:
    mov di, ax                  // (A and the flags from before)
    mov si, bx
    call s_rd
    shr ah, 1                   // CF = C
    inc al
    jmp 1f
s_dec_hl:
    mov di, ax
    mov si, bx
    call s_rd
    shr ah, 1
    dec al
1:
    lahf                        // the new Z and H, the old C
    mov [drc_tmp], al
    and di, 0x00FF
    and ax, 0xFF00
    or ax, di
    mov si, bx
    jmp s_wrt

    .global drc_stubs_end
drc_stubs_end:

    .section .farrodata.gbdrc, "a"
    .global drc_stubs_size
drc_stubs_size:
    .word drc_stubs_end - drc_stubs
