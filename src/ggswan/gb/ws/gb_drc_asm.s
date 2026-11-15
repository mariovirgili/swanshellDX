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
    .byte 0xE9
    .word s_daa - . - 2
    .byte 0xE9
    .word s_stat_e - . - 2
    .byte 0xE9
    .word s_ly_e - . - 2
    .byte 0xE9
    .word s_wait_stat - . - 2
    .byte 0xE9
    .word s_delay_bc - . - 2
    .byte 0xE9
    .word s_exit_dyn_f - . - 2
    .byte 0xE9
    .word s_copy0 - . - 2
    .byte 0xE9
    .word s_copy1 - . - 2
    .byte 0xE9
    .word s_copy2 - . - 2
    .byte 0xE9
    .word s_copy3 - . - 2
    .byte 0xE9
    .word s_copy4 - . - 2
    .byte 0xE9
    .word s_fill0 - . - 2
    .byte 0xE9
    .word s_skip_hl - . - 2
    .byte 0xE9
    .word s_vreg - . - 2

// leave for the address in SI. When there are cycles left and the
// address is in ROM, the block is looked for here: the table's first
// place for it (gb_drc.c: hash), and if it is there the jump is made
// through the table, without the dispatcher. (After EI the translator
// does not come here: gb_drc.c, after_ei)
s_exit_dyn:
    cmp word ptr [gbf_cyc], 0
s_exit_dyn_f:
    jle s_exit_time
s_lookup:
    // first the two small tables in the internal RAM (512 places each,
    // by the address' low bits; filled below): the first ROM area's has
    // no bank to compare (it is emptied if that bank ever changes)
    cmp si, 0x4000
    jae 4f
    mov di, si
    shl di, 3
    and di, 0x0FF8
    cmp [di + DRC_RC0], si
    jne 5f
    jmp far ptr [di + DRC_RC0 + 4]
4:
    test si, si
    js s_lookup_ram
    mov di, si
    shl di, 3
    and di, 0x0FF8
    cmp [di + DRC_RCX], si
    jne 5f
    mov si, [di + DRC_RCX + 2]
    cmp si, [drc_keyx]
    mov si, [di + DRC_RCX]
    jne 5f
    jmp far ptr [di + DRC_RCX + 4]
5:
    // the blocks' table: its first place for the address
    mov di, si
    shr di, 6
    xor di, si
    mov es, [drc_tseg]
    cmp si, 0x4000
    jb 1f
    xor di, [drc_hashx]
    and di, 8191
    shl di, 3
    cmp es:[di], si
    jne 9f
    push ax
    mov ax, [drc_keyx]
    cmp es:[di + 2], ax
    jne 8f
    push bx
    mov bx, si
    shl bx, 3
    and bx, 0x0FF8
    add bx, DRC_RCX
    jmp 2f
1:
    xor di, [drc_hash0]
    and di, 8191
    shl di, 3
    cmp es:[di], si
    jne 9f
    push ax
    mov ax, [drc_key0]
    cmp es:[di + 2], ax
    jne 8f
    push bx
    mov bx, si
    shl bx, 3
    and bx, 0x0FF8
    add bx, DRC_RC0
2:
    // found: into the small table, and there
    mov [bx], si
    mov [bx + 2], ax
    mov ax, es:[di + 4]
    mov [bx + 4], ax
    mov ax, es:[di + 6]
    mov [bx + 6], ax
    pop bx
    pop ax
    jmp far ptr es:[di + 4]
8:
    pop ax
9:
    mov [gb + GB_PC], si
    jmp s_exit_slow
// Code in the work RAM or the high RAM: its first form, when that is at
// its first place in the table and the bytes there are still the ones
// translated (gb_drc.c, ram_same; gb_asm.s, gbd_ram_cmp: the bytes read
// as the code runs are left out). Anything else is the dispatcher's.
// (Donkey Kong Land, R-Type II and Race Drivin' go through code in RAM
// tens of times a frame; each was a round through C.)
s_lookup_ram:
    cmp si, 0xC000
    jb 9b
    mov di, si
    shr di, 6
    xor di, si
    xor di, 0xFFE0              // (the key FFFFh << 5)
    and di, 8191
    shl di, 3
    mov es, [drc_tseg]
    cmp es:[di], si
    jne 9b
    cmp word ptr es:[di + 2], 0xFFFF
    jne 9b
    push ax
    push bx
    push cx
    push si
    les bx, es:[di + 4]         // the block
    mov [drc_ram_jmp], bx
    mov [drc_ram_jmp + 2], es
    xor cx, cx
    mov cl, es:[bx - 1]         // how many bytes
    mov ax, si
    add ax, cx
    jc 7f
    cmp si, 0xFF80
    jae 1f
    cmp ax, 0xE000
    ja 7f
1:
    mov di, bx
    dec di
    sub di, cx                  // the copy of the bytes translated
    mov bx, di
    sub si, 0x6000
    cld
2:
    repe cmpsb
    je 3f
    mov ax, di
    dec ax
    sub ax, bx                  // which byte differs: one read as the code runs?
    push cx
    mov cl, al
    and cl, 7
    shr ax, 3
    push bx
    add bx, ax
    mov al, es:[bx - 18]
    pop bx
    shr al, cl
    pop cx
    test al, 1
    jz 7f
    jcxz 3f
    jmp 2b
3:
    pop si
    pop cx
    pop bx
    pop ax
    .byte 0xFF, 0x2E            // jmp far [drc_ram_jmp]
    .word drc_ram_jmp
7:
    pop si
    pop cx
    pop bx
    pop ax
    jmp 9b
s_exit_time:
    mov [gb + GB_PC], si
// leave: gb.pc is set. What most leavings are for is done here, without
// the dispatcher (gb_drc.c, gbd_run_frame, whose order this follows): the
// event that is due, an interrupt to take (the address pushed where the
// stack is, in the work RAM or the high RAM), and on to the block at
// gb.pc by the tables above; HALT's wait too. Anything else (EI just done, an
// instruction for the interpreter, a jump to be joined, the frame's end,
// code in RAM, a block not found) is the dispatcher's.
s_exit:
    cmp byte ptr [drc_ei], 0
    jne s_exit_slow
    cmp byte ptr [drc_interp], 0
    jne s_exit_slow
s_exit_top:
    cmp word ptr [gbf_cyc], 0
    jle s_exit_event
    cmp word ptr [drc_link], 0
    jne s_exit_slow
    cmp byte ptr [gb + GB_HALTED], 0
    je s_exit_irq
    // halted: nothing until an interrupt is asked for (the time to the
    // next event passed over), then on, a cycle later
    mov di, ax
    mov al, [0x9FFF]
    and al, [0x9F0F]
    test al, 0x1F
    mov ax, di
    jnz 1f
    mov word ptr [gbf_cyc], 0
s_exit_event:
    push ax
    push bx
    push cx
    push dx
    IA16_CALL gbf_event_asm     // (gb_asm.s: the timer's and LYC's)
    test al, al
    jnz s_exit_evdone
    IA16_CALL gbf_host_event    // (the rest: C)
s_exit_evdone:
    pop dx
    pop cx
    pop bx
    pop ax
    cmp byte ptr [gb + GB_FRAME_DONE], 0
    jne s_exit_slow
    // (a jump that would have been joined, had the time not run out: the
    // next time)
    mov word ptr [drc_link], 0
    jmp s_exit_top
1:
    mov byte ptr [gb + GB_HALTED], 0
    dec word ptr [gbf_cyc]
s_exit_irq:
    mov di, ax
    mov al, [0x9FFF]
    and al, [0x9F0F]
    and al, 0x1F
    jz s_exit_none
    cmp byte ptr [gb + GB_IME], 0
    je s_exit_none
    cmp bp, 0xC002
    jb s_exit_c
    cmp bp, 0xE000
    jbe 1f
    cmp bp, 0xFF82
    jb s_exit_c
1:
    mov si, [gb + GB_PC]
    mov [bp - 0x6002], si
    sub bp, 2
    mov byte ptr [gb + GB_IME], 0
    mov si, 0x40
    mov ah, 1
2:
    test al, ah
    jnz 3f
    shl ah, 1
    add si, 8
    jmp 2b
3:
    not ah
    and [0x9F0F], ah
    mov [gb + GB_PC], si
    mov ax, di
    sub word ptr [gbf_cyc], 5
    jle s_exit_slow
    jmp s_lookup
s_exit_none:
    mov ax, di
    mov si, [gb + GB_PC]
    jmp s_lookup
s_exit_c:
    mov ax, di
s_exit_slow:
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
    cmp si, 0x4000
    jae 0f
    mov es, [drc_segtab]
    mov al, es:[si]
    ret
0:
    mov es, [drc_segtab + 2]
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

// (LDH A, (41h) and LDH A, (44h) come straight here)
s_stat_e:
    mov si, 0xFF41
    cmp byte ptr [gb + GB_LCD_ON], 0
    jne s_stat
    jmp s_rd
s_ly_e:
    mov si, 0xFF44
    cmp byte ptr [gb + GB_LCD_ON], 0
    jne s_ly
    jmp s_rd
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
    // the usual case: a line's cycle from 1 on, not line 153: the mode
    // from a table, LYC compared with the line
    test ah, ah
    jz 7f
    cmp al, 153
    je 7f
    mov dh, 1
    cmp al, 144
    jae 1f
    push bx
    mov bl, ah
    xor bh, bh
    mov dh, cs:[bx + stat_modes - drc_stubs]
    pop bx
1:
    mov dl, al
    jmp 5f
7:
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
// the mode at each cycle of a line of the picture
stat_modes:
    .byte 0
    .fill 20, 1, 2
    .fill 43, 1, 3
    .fill 50, 1, 0

// The time a waiting loop is passed over is taken off the count, but not
// past the next event (and none of it when an event is already due): the
// loop goes round again after the event. (Until patch 167 the whole wait
// was taken at once. R-Type II waits for the horizontal blank before
// every byte it writes to the video memory; in the vertical blank that
// wait lasts to the next frame's first line, the plugin went there in
// one step and only then took the vertical blank's interrupt, ten lines
// late, with the interrupt's routine still to run: every dozen frames
// the game had not set its line interrupt in time, and the status bar
// was drawn with the playfield's scroll.)
.macro SKIP_CYC r
    sub [gbf_cyc], \r
    jg 77f
    add \r, [gbf_cyc]           // (what it was)
    jle 76f
    xor \r, \r
76:
    mov [gbf_cyc], \r
77:
.endm

// s_wait_stat: by the modes the loop goes round in (the rows) and the part
// of the line (cycle 0, 1-20, 21-63, 64-113), the cycle the wait ends at,
// 114 and more in the next line; FEh here, FFh in no mode of a line
wait_ends:
    .byte 0xFE, 0xFE, 0xFE, 0xFE
    .byte 0x01, 0xFE, 0xFE, 0x73
    .byte 0xFE, 0xFE, 0xFE, 0xFE
    .byte 0x01, 0xFE, 0xFE, 0x73
    .byte 0xFE, 0x15, 0xFE, 0xFE
    .byte 0x15, 0x15, 0xFE, 0x87
    .byte 0xFE, 0x15, 0xFE, 0xFE
    .byte 0x15, 0x15, 0xFE, 0x87
    .byte 0xFE, 0xFE, 0x40, 0xFE
    .byte 0x01, 0xFE, 0x73, 0x73
    .byte 0xFE, 0xFE, 0x40, 0xFE
    .byte 0x01, 0xFE, 0x73, 0x73
    .byte 0xFE, 0x40, 0x40, 0xFE
    .byte 0xFF, 0xFF, 0xFF, 0xFF
    .byte 0xFE, 0x40, 0x40, 0xFE
    .byte 0xFF, 0xFF, 0xFF, 0xFF

// AL = STAT, read by a loop that only tests the mode and goes round while
// it is one of those in the byte after the call (bit n: mode n): the
// time to the first cycle of another mode is taken off the count first,
// so the loop goes round once. The modes of a line: 0 at its first
// cycle, 2 from 1, 3 from 21, 0 from 64; the vertical blank is mode 1.
s_wait_stat:
    pop di
    inc di
    push di
    cmp byte ptr [gb + GB_LCD_ON], 0
    jne 1f
    mov si, 0xFF41
    jmp s_rd
1:
    push bx
    push cx
    push dx
    mov bl, cs:[di - 1]         // the modes it goes round in
    mov di, ax
    mov ax, [gbf_ev_pos]
    sub ax, [gbf_cyc]
    mov si, ax                  // the frame's cycle
    mov dl, 114
    div dl                      // AL = the line, AH = the cycle in it
    // In a line of the picture but the last: where the wait ends is in
    // a table, by the modes and the part of the line this is
    cmp al, 143
    jae 0f
    mov cl, bl
    xor bh, bh
    shl bx, 2
    cmp ah, 1
    jb 1f
    inc bx
    cmp ah, 21
    jb 1f
    inc bx
    cmp ah, 64
    jb 1f
    inc bx
1:
    mov dh, cs:[bx + wait_ends - drc_stubs]
    cmp dh, 0xFE
    jb 1f
    mov bl, cl
    jne 0f                      // (FFh: no mode of a line ends it)
    mov dh, ah                  // (FEh: it ends here)
1:
    mov bl, dh
    sub bl, ah
    xor bh, bh
    cmp word ptr [gbf_cyc], 0
    jle wst_plain
    cmp bx, [gbf_cyc]
    jl 1f
    mov word ptr [gbf_cyc], 0
    jmp wst_plain
1:
    sub [gbf_cyc], bx
    cmp dh, 114
    jb 1f
    sub dh, 114
    inc al
1:
    mov dl, al
    mov bl, dh
    test dh, dh
    jnz 1f
    test al, al
    jz 1f
    mov dl, 0xFF
1:
    mov dh, cs:[bx + stat_modes - drc_stubs]
    jmp 8f
0:
    xor cx, cx                  // cycles passed over
    cmp al, 144
    jae 4f
2:
    // the mode here; if the loop goes on in it, on to where it changes
    push bx
    mov bl, ah
    xor bh, bh
    mov dh, cs:[bx + stat_modes - drc_stubs]
    pop bx
    push cx
    mov cl, dh
    mov ch, 1
    shl ch, cl
    test bl, ch
    pop cx
    jz 5f
    mov dl, 1
    cmp ah, 1
    jb 3f
    mov dl, 21
    cmp ah, 21
    jb 3f
    mov dl, 64
    cmp ah, 64
    jb 3f
    // the next line's second cycle (or the vertical blank)
    mov dl, 115
    sub dl, ah
    xor dh, dh
    add cx, dx
    inc al
    mov ah, 1
    cmp al, 144
    jb 2b
    mov dh, 1                   // (line 144, cycle 1: the vertical blank)
    test bl, 2
    jz 5f
    mov si, 144 * 114 + 1
    jmp 6f
3:
    sub dl, ah
    add ah, dl
    xor dh, dh
    add cx, dx
    jmp 2b
4:
    // the vertical blank: mode 1 (line 144's first cycle: 0)
    mov dh, 1
    cmp al, 144
    jne 7f
    test ah, ah
    jnz 7f
    xor dh, dh
    jmp 5f
7:
    test bl, 2
    jz 5f
6:
    // (the loop goes on through the vertical blank: to the next frame's
    // first cycle, mode 0, and on from there)
    mov ax, 17556
    sub ax, si
    add cx, ax
    xor ax, ax
    jmp 2b
5:
    // (not past the next event: see SKIP_CYC. The loop then reads STAT
    // as it is and goes round, to the dispatcher)
    cmp word ptr [gbf_cyc], 0
    jle wst_plain
    cmp cx, [gbf_cyc]
    jl wst_skip
    mov word ptr [gbf_cyc], 0
wst_plain:
    mov ax, di
    pop dx
    pop cx
    pop bx
    jmp s_stat
wst_skip:
    sub [gbf_cyc], cx
    // DL = what LYC is compared with there
    mov dl, al
    cmp al, 153
    jne 7f
    mov dl, 0xFF
    cmp ah, 1
    jb 8f
    mov dl, 153
    je 8f
    mov dl, 0xFF
    cmp ah, 2
    je 8f
    xor dl, dl
    jmp 8f
7:
    test ah, ah
    jnz 8f
    test al, al
    jz 8f
    mov dl, 0xFF
8:
    mov al, [0x9F41]
    and al, 0x78
    or al, 0x80
    or al, dh
    cmp dl, [0x9F45]
    jne 9f
    or al, 4
9:
    and di, 0xFF00
    xor ah, ah
    or ax, di
    pop dx
    pop cx
    pop bx
    ret

// A loop that reads STAT and goes round while the mode is one of those in
// the byte after the call (bit n: mode n), about to go round: the time
// to the first cycle of another mode is taken off the count. In the
// picture's lines only (in the vertical blank the loop just goes round)
s_wait_mode:
    pop di
    inc di
    push di
    cmp byte ptr [gb + GB_LCD_ON], 0
    je 9f
    push ax
    push bx
    push dx
    mov bl, cs:[di - 1]         // the modes that keep it going round
    mov ax, [gbf_ev_pos]
    sub ax, [gbf_cyc]
    mov dl, 114
    div dl                      // AL = the line, AH = the cycle in it
    cmp al, 143
    jae 8f
    // the changes of mode from here on: cycle 1 (mode 2), 21 (3), 64
    // (0), and the next line's 1, 21, 64
    cmp ah, 64
    jae 5f
    cmp ah, 21
    jae 4f
    mov dx, 0x0201
    call 1f
    mov dx, 0x0315
    call 1f
4:
    mov dx, 0x0040
    call 1f
5:
    mov dx, 0x0273
    call 1f
    mov dx, 0x0387
    call 1f
    mov dx, 0x00B2
    call 1f
8:
    pop dx
    pop bx
    pop ax
9:
    ret
1:
    // (DL = a cycle, DH = the mode from it: the one, if it is ahead and
    // the loop does not go on in that mode)
    cmp dl, ah
    jbe 2f
    push cx
    mov cl, dh
    mov ch, 1
    shl ch, cl
    test bl, ch
    pop cx
    jnz 2f
    sub dl, ah
    xor dh, dh
    SKIP_CYC dx
    pop dx                      // (out of the list: not back to it)
    jmp 8b
2:
    ret

// AL to SCY, SCX, BGP, WY or WX (the register's low byte and its place in
// an entry of the frame's list of changes after the call): what
// gbf_io_write and gbv_reg (gb_video.c) do, without the way through C. A
// program that changes the scroll at every line (ZAS's logo, its
// playfield) spent a quarter of that screen's time there.
s_vreg:
    pop di
    add di, 2
    push di
    push bx
    mov bl, cs:[di - 2]
    mov bh, 0x9F
    cmp al, [bx]
    je 9f
    mov [bx], al
    push ax
    push dx
    // the line it shows from (gbf_draw_line): the one being drawn until
    // its 21st cycle, then the next; the first with the LCD off or in
    // the vertical blank; nothing after the last
    xor dl, dl
    cmp byte ptr [gb + GB_LCD_ON], 0
    je 1f
    mov ax, [gbf_ev_pos]
    sub ax, [gbf_cyc]
    mov dl, 114
    div dl
    mov dl, 0
    cmp al, 144
    jae 1f
    mov dl, al
    cmp ah, 21
    jb 1f
    inc dl
    cmp dl, 144
    jae 8f
1:
    mov bl, [gbv_raw_n]
    xor bh, bh
    shl bx, 3
    add bx, 0xA000 - 8          // the list's last entry
    cmp [bx], dl
    je 2f
    cmp byte ptr [gbv_raw_n], 146
    jae 2f
    mov ax, [bx]
    mov [bx + 8], ax
    mov ax, [bx + 2]
    mov [bx + 10], ax
    mov ax, [bx + 4]
    mov [bx + 12], ax
    mov ax, [bx + 6]
    mov [bx + 14], ax
    add bx, 8
    mov [bx], dl
    inc byte ptr [gbv_raw_n]
2:
    mov al, cs:[di - 1]
    xor ah, ah
    add bx, ax
    pop dx
    pop ax
    mov [bx], al
    pop bx
    ret
8:
    pop dx
    pop ax
9:
    pop bx
    ret

// A loop that only reads LY and tests it: nothing changes before the next
// line, so the time to it is taken off the count at once (to the line's
// second cycle, where LY has changed in every line)
// a loop that only compares A with the byte at HL, about to go round: HL
// at LY, as s_skip_line; in the work RAM or the high RAM (only an
// interrupt writes it), to the next event; anything else, nothing
s_skip_hl:
    cmp bx, 0xFF44
    je 3f
    cmp bx, 0xC000
    jb 2f
    cmp bx, 0xFE00
    jb 1f
    cmp bx, 0xFF80
    jb 2f
1:
    mov word ptr [gbf_cyc], 0
2:
    ret
3:
    // LY: A is the line waited for. More than a line away: to the second
    // cycle of the line before it (or as far as the time to the next
    // event goes) at once; the last line and a bit by s_skip_line, a line
    // a turn, so that what LY reads around the line's start is its own
    cmp byte ptr [gb + GB_LCD_ON], 0
    je 2b
    cmp al, 154
    jae s_skip_line
    push ax
    push cx
    push dx
    mov cl, al
    mov ax, [gbf_ev_pos]
    sub ax, [gbf_cyc]
    mov dl, 114
    div dl                      // AL = the line, AH = the cycle in it
    mov ch, ah
    dec cl
    sub cl, al                  // lines to the one before the line wanted
    jns 4f
    add cl, 154
4:
    cmp cl, 2
    jb 6f
    mov al, cl
    mul dl                      // AX = those lines' cycles
    mov dl, ch
    xor dh, dh
    sub ax, dx
    inc ax
    mov dx, [gbf_cyc]
    dec dx
    jle 6f
    cmp ax, dx
    jbe 5f
    mov ax, dx
5:
    sub [gbf_cyc], ax
    pop dx
    pop cx
    pop ax
    ret
6:
    pop dx
    pop cx
    pop ax
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
    SKIP_CYC ax
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
    SKIP_CYC ax
    pop dx
    pop ax
1:
    ret

/*
 * The loops that copy bytes (LD A, (HL+) / LD (DE), A / INC DE / DEC B /
 * JR NZ and its like: gb_drc.c, copy_loops). At the loop's start: when
 * all of what is still to copy but the last byte, or as much as there is
 * time for before the next event, comes from the ROM, the video memory,
 * the work RAM or the high RAM and goes to the patterns (8000h-97FFh),
 * the work RAM or the high RAM, those bytes are
 * moved at once, and the pointers, the count and the time are as after
 * those turns. The block's own code then does a turn as ever (A, the
 * flags and the way out are its). Anything else (the video memory, the
 * registers, the cartridge's RAM): nothing here, the loop as before.
 */
// AX = the turns to do here: the most (SI: the count less one) or what the
// time allows at DI cycles a turn; ZF if none
copy_k:
    mov ax, [gbf_cyc]
    dec ax
    jle 1f
    push dx
    xor dx, dx
    div di
    pop dx
    cmp ax, si
    jbe 2f
    mov ax, si
2:
    test ax, ax
    ret
1:
    xor ax, ax
    ret
// AX bytes from SI to DI (the Game Boy's addresses); CF if they are not
// all where this can be done (nothing copied then). BX, CX, DX, AX kept
copy_do:
    push cx
    push ax
    mov cx, ax
    mov ax, di
    add ax, cx                  // (the end)
    jc 8f
    cmp di, 0x8000
    jb 8f
    cmp ax, 0x9800              // (the patterns: as they are; the maps
    ja 6f                       // have the WonderSwan's to keep too: not here)
    mov [gbv_pw_last], di       // (gb_video.c: see wrt_pat; byte by byte
    cmp byte ptr [gbv_tq_on], 0 // when the patterns are shown a frame late)
    jne 8f
    jmp 1f
6:
    cmp di, 0xC000
    jb 8f
    cmp ax, 0xE000
    jbe 1f
    cmp di, 0xFF80
    jb 8f
    cmp ax, 0xFFFF
    ja 8f
1:
    sub di, 0x6000
    mov ax, si
    add ax, cx
    jc 8f
    cmp si, 0x8000
    jb 3f
    cmp ax, 0xA000              // (the video memory, all of it)
    jbe 2f
    cmp si, 0xC000
    jb 8f
    cmp ax, 0xE000
    jbe 2f
    cmp si, 0xFF80
    jb 8f
    cmp ax, 0xFFFF
    ja 8f
2:
    sub si, 0x6000
    push ss
    pop es
    cld
    rep movsb
    jmp 7f
3:
    // from the ROM: one of its two areas, through its segment
    cmp si, 0x4000
    jae 4f
    cmp ax, 0x4000
    ja 8f
    mov ax, [drc_segtab]
    jmp 5f
4:
    cmp ax, 0x8000
    ja 8f
    mov ax, [drc_segtab + 2]
5:
    push ss
    pop es
    push ds
    mov ds, ax
    cld
    rep movsb
    pop ds
7:
    pop ax
    pop cx
    clc
    ret
8:
    pop ax
    pop cx
    stc
    ret

// src, dst: the pointers' registers; cnt: the count's (8 bits: 0 is 256;
// 16 bits: 0 is 65536); cyc: the Game Boy's cycles a turn
.macro COPY_LOOP src, dst, cnt, wide, cyc
    push ax
.if \wide
    mov si, \cnt
    dec si
.else
    mov al, \cnt
    xor ah, ah
    dec al
    mov si, ax
.endif
    mov di, \cyc
    call copy_k
    jz 9f
    mov si, \src
    mov di, \dst
    call copy_do
    jc 9f
    add \src, ax
    add \dst, ax
.if \wide
    sub \cnt, ax
.else
    sub \cnt, al
.endif
    push dx
    mov di, \cyc
    mul di
    pop dx
    sub [gbf_cyc], ax
9:
    pop ax
    ret
.endm
s_copy0:    // LD A, (HL+) / LD (DE), A / INC DE / DEC B / JR NZ
    COPY_LOOP bx, dx, ch, 0, 10
s_copy1:    // LD A, (DE) / LD (HL+), A / INC DE / DEC B / JR NZ
    COPY_LOOP dx, bx, ch, 0, 10
s_copy2:    // LD A, (HL+) / LD (DE), A / INC DE / DEC BC / LD A, B / OR C / JR NZ
    COPY_LOOP bx, dx, cx, 1, 13
s_copy3:    // LD A, (DE) / LD (HL+), A / INC DE / DEC BC / LD A, B / OR C / JR NZ
    COPY_LOOP dx, bx, cx, 1, 13
s_copy4:    // LD A, (BC) / INC BC / LD (HL+), A / DEC D / JR NZ
    COPY_LOOP cx, bx, dh, 0, 10

s_fill0:    // LD (HL+), A / DEC B / JR NZ: into the patterns, the work RAM or the high RAM
    push ax
    mov al, ch
    xor ah, ah
    dec al
    mov si, ax
    mov di, 6
    call copy_k
    jz 9f
    mov si, bx
    add si, ax
    jc 9f
    cmp bx, 0x8000
    jb 9f
    cmp si, 0x9800
    ja 6f
    mov [gbv_pw_last], bx       // (as in copy_do)
    cmp byte ptr [gbv_tq_on], 0
    jne 9f
    jmp 1f
6:
    cmp bx, 0xC000
    jb 9f
    cmp si, 0xE000
    jbe 1f
    cmp bx, 0xFF80
    jb 9f
    cmp si, 0xFFFF
    ja 9f
1:
    push cx
    push ax
    mov cx, ax
    mov di, bx
    sub di, 0x6000
    push ss
    pop es
    mov si, sp
    mov al, ss:[si + 4]         // (A, as it was pushed)
    cld
    rep stosb
    pop ax
    pop cx
    add bx, ax
    sub ch, al
    mov si, ax
    shl ax, 1
    add ax, si
    shl ax, 1                   // 6 cycles a turn
    sub [gbf_cyc], ax
9:
    pop ax
    ret

// A loop that only counts BC down (DEC BC / LD A, B / OR C / JR NZ, 7
// cycles a turn: a program's way of waiting). At its start: as many turns
// as BC and the time to the next event allow, less the last one, are taken
// off BC and off the time at once; the block's own code then does a turn
// as ever (so the flags, A and the leaving are its).
s_delay_bc:
    push ax
    push dx
    mov ax, [gbf_cyc]
    dec ax
    jle 9f
    xor dx, dx
    mov si, 7
    div si                      // AX = the turns there is time for
    mov si, cx
    dec si                      // BC - 1 (BC = 0 counts 65536)
    cmp ax, si
    jbe 1f
    mov ax, si
1:
    sub cx, ax
    mov si, ax
    shl ax, 3
    sub ax, si
    sub [gbf_cyc], ax
9:
    pop dx
    pop ax
    ret

// DAA. AL = A, AH = the flags (Z 40h, H 10h, C 01h), N in drc_n. After an
// addition: 60h more if C or A over 99h (C set), 6 more if H or the low
// digit over 9. After a subtraction: 60h less if C, 6 less if H. H
// cleared, C set or kept, Z from the result.
s_daa:
    push bx
    mov bl, ah
    and bl, 1                   // BL = C after
    test byte ptr [drc_n], 0x40
    jnz 5f
    test ah, 1
    jnz 1f
    cmp al, 0x99
    jbe 2f
1:
    add al, 0x60
    mov bl, 1
2:
    test ah, 0x10
    jnz 3f
    mov bh, al
    and bh, 0x0F
    cmp bh, 9
    jbe 8f
3:
    add al, 6
    jmp 8f
5:
    test ah, 1
    jz 6f
    sub al, 0x60
6:
    test ah, 0x10
    jz 8f
    sub al, 6
8:
    mov ah, bl
    test al, al
    jnz 9f
    or ah, 0x40
9:
    pop bx
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

// the byte at SI = drc_tmp, SI in the video memory (the translator knows:
// gb_drc.c, write_from). Nothing after the call. A pattern with nothing
// kept late is written here; the rest is s_wrt's, made to come back to
// the same place
s_wrv:
    cmp si, 0x9800
    jae wrv_other
    cmp byte ptr [gbv_tq_on], 0
    jne wrv_other
    mov [gbv_pw_last], si
    mov di, ax
    mov al, [drc_tmp]
    mov [si - 0x6000], al
    mov ax, di
    ret
wrv_other:
    mov di, sp
    sub word ptr [di], 2        // (s_wrt returns two bytes on)
    // (falls into s_wrt)

// the byte at SI = drc_tmp. After the call: the Game Boy address of the
// next instruction (a word), where the run goes on through the dispatcher
// when the write was to the cartridge's registers or to an I/O register
s_wrt:
    // (the patterns first: what comes here most, from a program that
    // draws - the work RAM is written by the block itself)
    cmp si, 0x9800
    jae wrt_rest
    cmp si, 0x8000
    jb wrt_rest
    cmp byte ptr [gbv_tq_on], 0
    jne wrt_pat_late
    mov [gbv_pw_last], si
    mov di, ax
    mov al, [drc_tmp]
    mov [si - 0x6000], al
    mov ax, di
    pop di
    add di, 2
    jmp di
wrt_pat_late:
    mov di, ax
    mov al, [drc_tmp]
    jmp wrt_tq
wrt_rest:
    cmp si, 0xC000
    jae wrt_high
    cmp si, 0xA000
    jae wrt_cart                // (a program with its variables in the cartridge's RAM)
    cmp si, 0x8000
    jb 1f
    mov di, ax                  // (9800h-9FFFh: the maps)
    mov al, [drc_tmp]
    jmp wrt_map
wrt_high:
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
    cmp si, 0x9800
    jb wrt_pat
wrt_map:
    mov [si - 0x6000], al
    mov ah, 0x08                // palette 4
    sub si, 0x9800
    shl si, 1
    mov [si + 0x4800], ax       // (the map for LCDC bit 4 set)
    cmp al, 0x80
    jae 7f
    inc ah                      // (bit 4 clear: 0-127 are the patterns from 9000h)
7:
    mov [si + 0x1000], ax
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
    // an I/O register written through a pointer: the sound's (FF10h-
    // FF3Fh) are kept as written, with a channel's start remembered
    // (gbf_hw.c: gbf_trig), and SCY, SCX, BGP, OBP0, OBP1, WY, WX
    // written with what they have are nothing: none of it for C.
    // (Batman's sound routine writes nine registers a frame this way,
    // some 200 cycles each through gbd_write.)
    cmp si, 0xFF10
    jb 6f
    cmp si, 0xFF40
    jae wrt_vsame
    mov di, ax
    mov al, [drc_tmp]
    mov [si - 0x6000], al
    test al, 0x80
    jz wrt_snd_done
    mov al, 1
    cmp si, 0xFF14
    je wrt_trig
    mov al, 2
    cmp si, 0xFF19
    je wrt_trig
    mov al, 4
    cmp si, 0xFF1E
    je wrt_trig
    mov al, 8
    cmp si, 0xFF23
    jne wrt_snd_done
wrt_trig:
    or [gbf_trig], al
wrt_snd_done:
    mov ax, di
    jmp 9b
// a pattern's byte (8000h-97FFh). gb_video.c is told where (gbv_pw_last:
// is the program rewriting the patterns of sprites on the screen?), and
// when it has found so the byte goes to the tile's copy that is shown a
// frame later (gb_video.c, "the patterns a frame late"), not to the tile
wrt_pat:
    cmp byte ptr [gbv_tq_on], 0
    jne wrt_tq
    mov [gbv_pw_last], si
    mov [si - 0x6000], al
    mov ax, di
    jmp 9b
wrt_tq:
    push si
    and si, 0xFFF0
    cmp si, [gbv_tq_last]       // (the tile written last: its copy is known)
    pop si
    jne wrt_tq_look
    and si, 15
    add si, [gbv_tq_lastp]
    mov [si], al
    mov ax, di
    jmp 9b
wrt_tq_look:
    push bx
    mov bx, si
    shr bx, 4
    mov bl, [bx + 0xCC30 - 0x800]   // (TQ_IDX: the tile's copy in this frame's set, + 1)
    xor bh, bh
    test bl, bl
    jz wrt_tq_new
    shl bx, 4
    add bx, [gbv_tq_base]
    mov [gbv_tq_lastp], bx
    mov bx, si
    and bx, 0xFFF0
    mov [gbv_tq_last], bx
    mov bx, [gbv_tq_lastp]
    and si, 15
    mov [bx + si], al
    pop bx
    mov ax, di
    jmp 9b
wrt_tq_new:
    pop bx
    mov ax, di
    push ax
    push bx
    push cx
    push dx
    mov ax, si
    IA16_CALL gbv_tq_new        // (a copy for the tile; or, none left, no more copies)
    pop dx
    pop cx
    pop bx
    pop ax
    mov di, ax
    mov al, [drc_tmp]
    jmp wrt_pat
// LYC with the LCD on and no interrupt asked for at a mode (its own
// only, or none), no event due and no serial transfer: gbf_hw.c's
// gbf_io_write, GB_LYC, here. (A program with an interrupt at every
// line - X's title and its fades - writes the next line here 144 times
// a frame.)
wrt_lyc:
    cmp byte ptr [gb + GB_LCD_ON], 0
    je 6f
    test byte ptr [0x9F41], 0x38
    jnz 6f
    cmp byte ptr [gbf_ser_on], 0
    jne 6f
    cmp word ptr [gbf_cyc], 0
    jle 6f
    push ax
    push bx
    push cx
    push dx
    mov ax, [gbf_ev_pos]
    sub ax, [gbf_cyc]           // the frame's cycle now
    mov [gbf_stat_done], ax
    mov cl, 114
    div cl                      // AL the line, AH the cycle in it
    // what LY is compared as, now (lyc_match)
    mov bl, al
    cmp al, 153
    jne 1f
    mov bl, 0xFF
    test ah, ah
    jz 3f
    cmp ah, 2
    je 3f
    mov bl, 153
    cmp ah, 1
    je 3f
    mov bl, 0
    jmp 3f
1:
    test ah, ah
    jnz 3f
    test al, al
    jz 3f
    mov bl, 0xFF
3:
    mov al, [drc_tmp]
    mov ah, [0x9F45]
    mov [0x9F45], al
    test byte ptr [0x9F41], 0x40
    jz 4f
    cmp ah, bl
    je 4f
    cmp al, bl
    jne 4f
    or byte ptr [0x9F0F], 2     // (the signal rose)
4:
    IA16_CALL gbf_sched_asm
    pop dx
    pop cx
    pop bx
    pop ax
    cmp word ptr [gbf_cyc], 0
    jle 2f
    cmp byte ptr [gb + GB_IME], 0
    je 9b
    mov di, ax
    mov al, [0x9FFF]
    and al, [0x9F0F]
    test al, 0x1F
    mov ax, di
    jz 9b
2:
    // (an interrupt to take, or the next event is now)
    mov di, sp
    mov di, [di]
    mov di, cs:[di]
    add sp, 2
    mov [gb + GB_PC], di
    jmp s_exit
wrt_vsame:
    cmp si, 0xFF45
    je wrt_lyc
    cmp si, 0xFF42
    jb 6f
    cmp si, 0xFF4C
    jae 6f
    cmp si, 0xFF44
    jb 1f
    cmp si, 0xFF47
    jb 6f
1:
    mov di, ax
    mov al, [drc_tmp]
    cmp al, [si - 0x6000]
    mov ax, di
    je 9b
6:
    // the write that changes the second ROM area's bank, here (2000h up
    // to MBC1's and MBC3's 3FFFh or MBC5's 2FFFh; gb_main.c: gbf_plat_map)
    mov di, si
    shr di, 8
    // (0000h-1FFFh of an MBC1, 3 or 5, "the RAM on or off", with what it
    // already is: nothing to do. Race Drivin' writes there 68 times a
    // frame, zeros mostly; a tenth of its time went through C for them)
    cmp di, 0x20
    jae wrt_bank
    cmp byte ptr [drc_ramg], 0
    je wrt_bank
    push ax
    mov al, [drc_tmp]
    and al, 15
    cmp al, 10
    mov al, 1
    jne 1f
    inc al
1:
    cmp al, [drc_ramg]
    pop ax
    je 9b
    jmp 6f
wrt_bank:
    cmp di, [drc_mbc_lo]
    jb 6f
    cmp di, [drc_mbc_top]
    jae 6f
    or di, [drc_mbc_or]         // (MBC2: only with the address' bit 8 set)
    test di, 1
    jz 6f
    push ax
    push dx
    mov al, [drc_tmp]
wrt_bank_al:
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
    shl dx, 5
    mov [drc_hashx], dx
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
wrt_cart:
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

// LD (nn), A where nn is the register of the second ROM area's bank
// (the translator knows: gb_drc.c), A in AL: wrt_bank without the looking
// at the address. After the call the next instruction's address, as for
// s_wrt. (Super Mario Land 2's map reads its data through a routine that
// sets the bank and sets it back: 160 times a frame, a fifth of the
// frame's time in s_wrt.)
s_bankx:
    push ax
    push dx
    jmp wrt_bank_al

// the same from a block in the first ROM area, which goes on whatever the
// second holds: no address after the call, back at once
s_bank0:
    push ax
    push dx
    and al, [drc_mbc_and]
    mov [gb + GB_BANK_LO], al
    jnz bank0_set
    cmp byte ptr [drc_mbc_z1], 0
    je bank0_set
    mov al, 1
bank0_set:
    xor ah, ah
    or ax, [drc_mbc_hi]
    and ax, [drc_bank_mask]
    cmp ax, [gb + GB_ROMX]
    je bank0_done
    mov [gb + GB_ROMX], ax
    mov dx, ax
    inc dx
    mov [drc_keyx], dx
    shl dx, 5
    mov [drc_hashx], dx
    mov dx, ax
    and dx, 3
    shl dx, 14
    mov [gbf_romx_off], dx
    shr dx, 4
    add dx, 0x2C00
    mov [drc_segtab + 2], dx
    shr ax, 2
    out 0xD4, ax                // the ROM1 window's bank
bank0_done:
    pop dx
    pop ax
    ret

// A byte fetched from another ROM bank: LD (nn), A / LD (bank), A / LD A,
// (HL) or LD A, (HL+) / LD (DE), A or LD (BC), A / LD A, n / LD (nn), A /
// LD (bank), A - nn in the cartridge's RAM, where the program keeps the
// bank's number - in a block of the first ROM area (Super Mario Land 2
// reads its levels and its map a byte at a time through three routines
// that are this and a RET: 190 times a frame where a level scrolls, each
// the general write twice, the bank's change twice, a read and a write
// through pointers). After the call: which of the three (0: (HL) to (DE),
// 1: (HL+) to (BC), 2: (HL) to (BC)), nn, n, the seven instructions'
// cycles. Back after those with the carry clear when it was done; set,
// and nothing done, when it is not the plain case (the cartridge's RAM
// not plain RAM switched on, HL in the echo, the OAM or the I/O
// registers, the place written not in the cartridge's RAM, the work RAM
// or the high RAM): the block then does the seven instructions.
s_far:
    pop si
    cmp byte ptr [drc_ram_ok], 1
    jne far_no
    cmp bh, 0xE0                // (read: anything below the echo, and the high RAM)
    jb far_src
    cmp bx, 0xFF80
    jb far_no
far_src:
    mov di, dx
    cmp byte ptr cs:[si], 0
    je far_dest
    mov di, cx
far_dest:
    cmp di, 0xA000
    jb far_no
    cmp di, 0xE000
    jb far_go
    cmp di, 0xFF80              // (written: the cartridge's RAM, the work RAM, the high RAM)
    jb far_no
    cmp di, 0xFFFF
    je far_no
far_go:
    push di
    mov es, [drc_ram_seg]
    mov di, cs:[si + 1]
    mov es:[di], al             // (the bank's number, where the program keeps it)
    // the byte. Only a read from the second ROM area sees the bank the
    // program asks for, and nothing else can see it before it is set
    // back (this is one piece: no interrupt between): the ROM1 window
    // alone is moved for the read, not the machine's bank
    cmp bh, 0x80
    jae far_mem
    cmp bh, 0x40
    jae far_romx
    mov es, [drc_segtab]
    mov al, es:[bx]
    jmp far_got
far_romx:
    push dx
    push ax
    and al, [drc_mbc_and]       // (the bank, as s_bank0 makes it)
    jnz far_bank
    cmp byte ptr [drc_mbc_z1], 0
    je far_bank
    mov al, 1
far_bank:
    xor ah, ah
    or ax, [drc_mbc_hi]
    and ax, [drc_bank_mask]
    mov dx, ax
    shr ax, 2
    out 0xD4, ax                // the ROM1 window's bank
    and dx, 3
    shl dx, 10
    add dx, 0x2C00
    mov es, dx
    mov dl, es:[bx]
    mov ax, [gb + GB_ROMX]
    shr ax, 2
    out 0xD4, ax
    pop ax                      // (AH: the flags)
    mov al, dl
    pop dx
    jmp far_got
far_mem:
    cmp bh, 0xA0
    jb far_plain
    cmp bh, 0xC0
    jae far_plain
    mov al, es:[bx]             // (the cartridge's RAM)
    jmp far_got
far_plain:
    mov al, [bx - 0x6000]       // (the video memory, the work RAM)
far_got:
    cmp byte ptr cs:[si], 1
    jne far_put
    inc bx
far_put:
    pop di
    cmp di, 0xC000
    jae far_work
    mov es, [drc_ram_seg]
    mov es:[di], al
    jmp far_back
far_work:
    mov [di - 0x6000], al
far_back:
    // A, the bank's number kept, the bank: n (the machine's bank moves
    // only if n is not what it was last set to)
    mov al, cs:[si + 3]
    mov es, [drc_ram_seg]
    mov di, cs:[si + 1]
    mov es:[di], al
    cmp al, [gb + GB_BANK_LO]
    je far_same
    call s_bank0
far_same:
    mov di, ax
    xor ax, ax
    mov al, cs:[si + 4]
    sub [gbf_cyc], ax
    mov ax, di
    add si, 5
    clc
    jmp si
far_no:
    add si, 5
    stc
    jmp si

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
    lea di, [bp - 2]
    // (a stack in the work RAM or the high RAM: PUSH AF comes here
    // whatever the stack)
    cmp di, 0xFF80
    jae 2f
    cmp di, 0xC000
    jb 0f
    cmp bp, 0xE000
    ja 0f
3:
    mov [di - 0x6000], si
    mov bp, di
    ret
2:
    cmp bp, di
    jae 3b
0:
    // (a stack in the cartridge's RAM, when that is plain RAM)
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
    cmp bp, 0xFF80
    jae 2f
    cmp bp, 0xC000
    jb 0f
    cmp bp, 0xDFFE
    ja 0f
3:
    mov si, [bp - 0x6000]
    lea bp, [bp + 2]
    ret
2:
    cmp bp, 0xFFFD
    jbe 3b
0:
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
// where each routine is in the copy, in the table's order: the translator
// calls them there (through the table's jump each call was 4 cycles more)
    .global drc_stub_offs
drc_stub_offs:
    .word s_exit - drc_stubs
    .word s_exit_dyn - drc_stubs
    .word s_rd - drc_stubs
    .word s_rdt - drc_stubs
    .word s_wrt - drc_stubs
    .word s_push - drc_stubs
    .word s_pop - drc_stubs
    .word s_push_af - drc_stubs
    .word s_pop_af - drc_stubs
    .word s_inc_hl - drc_stubs
    .word s_dec_hl - drc_stubs
    .word s_skip_line - drc_stubs
    .word s_skip_mode - drc_stubs
    .word s_p1 - drc_stubs
    .word s_daa - drc_stubs
    .word s_stat_e - drc_stubs
    .word s_ly_e - drc_stubs
    .word s_wait_stat - drc_stubs
    .word s_delay_bc - drc_stubs
    .word s_exit_dyn_f - drc_stubs
    .word s_copy0 - drc_stubs
    .word s_copy1 - drc_stubs
    .word s_copy2 - drc_stubs
    .word s_copy3 - drc_stubs
    .word s_copy4 - drc_stubs
    .word s_fill0 - drc_stubs
    .word s_skip_hl - drc_stubs
    .word s_vreg - drc_stubs
    .word s_bankx - drc_stubs
    .word s_bank0 - drc_stubs
    .word s_wrv - drc_stubs
    .word s_far - drc_stubs
