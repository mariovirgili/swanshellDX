/**
 * ggswan - recompiler runtime: entering translated code.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <wonderful.h>
#include <ws.h>

    .arch   i186
    .code16
    .intel_syntax noprefix

    .section .text, "ax"

    // void drc_enter(uint16_t offset);
    //
    // Loads the Z80 registers from z80 (AL = A, AH = F, CX = BC, DX = DE,
    // BX = HL, BP = SP) and far-calls DRC_CODE_SEG:offset. The block ends
    // in the EXIT or HELPER stub, which store the registers back and RETF.
    // z80_t: a f c b e d l h (0-7) ... sp at offset 20.
    .global drc_enter
drc_enter:
    push si
    push di
    push bp
    push es
    push bx
    push cx
    push dx

    mov [drc_entry], ax
    // far address of drc_helper_tramp, for the helper stub
    mov word ptr [drc_tramp], offset drc_helper_tramp
    mov word ptr [drc_tramp + 2], cs
.Ldrc_again:
    mov ax, [z80]
    mov cx, [z80 + 2]
    mov dx, [z80 + 4]
    mov bx, [z80 + 6]
    mov bp, [z80 + 20]
    .byte 0xFF, 0x1E        // call far [drc_entry]
    .word drc_entry

    // a line event with an interrupt taken at once: on with the block at
    // 0038h (drc_fast_reentry sets drc_entry), else back to the dispatcher
    push ds
    pop es
    IA16_CALL drc_fast_reentry
    test al, al
    jnz .Ldrc_again

    pop dx
    pop cx
    pop bx
    pop es
    pop bp
    pop di
    pop si
    IA16_RET

    // Far-called by the helper stub of a code bank (registers already
    // stored in z80): runs the helper in C and returns to the stub.
    // ES is whatever the translated code had: C may expect it equal to DS.
drc_helper_tramp:
    push es
    push ds
    pop es
    IA16_CALL drc_helper_direct
    pop es
    retf

    // Interrupt entries (main.c's handlers are ordinary C functions).
    //
    // A WS interrupt can come while translated code runs with DS (and ES)
    // elsewhere: the OUTI and LDI routines copy with DS = the Z80 source
    // (a ROM bank, the work RAM). gcc-ia16's interrupt attribute saves the
    // registers but does not load DS, so the handlers' variables (act_i,
    // the scroll values, the frame counters) were read from and written to
    // that segment: wrong scroll values now and then (Super Monaco GP's
    // status bar), bytes of the ROM copy overwritten (Aladdin's sprites).
    // Here: every register saved, DS = ES = SS (the data segment), DF
    // clear, then the C handler, then IRET.
    .macro IRQ_ENTRY name, handler
    .global \name
\name:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es
    push ss
    pop ds
    push ss
    pop es
    cld
    IA16_CALL \handler
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    iret
    .endm

    // (line_irq_entry: patch 71 served the splits inside the picture with
    // a short path in assembly, without C; Super Monaco GP II got worse on
    // the emulator, the whole picture flickering now and then: back to the
    // full entry for every line interrupt)
    IRQ_ENTRY line_irq_entry, line_irq
    IRQ_ENTRY vblank_irq_entry, vblank_irq

    // void log_copy(void);
    //
    // Writes the deferred VRAM writes from log_copy_start to log_copy_end
    // (main.c: entries of GG VRAM address, length, bytes; tile data only)
    // to the Game Gear VRAM (IRAM 4000h), with REP MOVSW. DS = the data
    // segment (IRAM).
    .global log_copy
log_copy:
    push si
    push di
    push es
    push bx
    push cx
    push dx
    push ds
    pop es
    cld
    mov si, [log_copy_start]
    mov dx, [log_copy_end]
    xor cx, cx
.Llog_next:
    cmp si, dx
    jae .Llog_done
    lodsw
    mov di, ax
    add di, 0x4000
    lodsb
    mov cl, al
    shr cx, 1
    rep movsw
    jnc .Llog_next
    movsb
    jmp .Llog_next
.Llog_done:
    pop dx
    pop cx
    pop bx
    pop es
    pop di
    pop si
    IA16_RET

    // uint8_t *spr_build(const uint8_t *sat, uint8_t *table): the WS
    // sprite table from the Game Gear's (main.c build_sprites, which
    // computes spr_par: the tile bank byte, 8x16 or not, the X offset, the
    // hidden rows). The same as the C loop it replaces, there ~35 cycles a
    // sprite (Sailor Moon S: 9 lines a frame). Returns the end of the
    // table. GG_VIEW_X/Y/W/H: 48, 24, 160, 144.
    //   spr_par: +0 tile bank byte, +1 8x16, +2 X offset + 48 - 32,
    //            +3 hidden rows (113 - sprite height)
    .global spr_build
spr_build:
    push bx
    push cx
    push si
    push di
    push bp
    mov si, ax                  // Y bytes
    mov di, dx                  // the table
    lea bp, [si + 0x80]         // X, tile pairs (SS = DS)
    lea bx, [di + 127 * 4]      // (room for 127 WS sprites)
    mov cx, word ptr [spr_par]  // CL tile bank, CH 8x16
    mov dx, word ptr [spr_par + 2]  // DL X offset, DH hidden rows
    mov byte ptr [spr_cnt], 64
.Lspr_loop:
    cmp di, bx
    jae .Lspr_done
    lodsb                       // y
    cmp al, 0xD0
    je .Lspr_done
    add al, 1 - 24              // the WS row
    mov ah, al
    sub ah, 144
    cmp ah, dh
    jb .Lspr_skip               // outside the 144 rows
    mov ah, byte ptr [bp]
    sub ah, dl                  // the WS column
    push ax
    sub ah, 32 + 160
    cmp ah, 89
    pop ax
    jb .Lspr_skip               // outside the 160 columns
    test ch, ch
    jnz .Lspr_tall
    mov ch, byte ptr [bp + 1]   // tile
    mov byte ptr [di], ch
    xor ch, ch
    mov byte ptr [di + 1], cl
    mov word ptr [di + 2], ax   // row, column
    add di, 4
    jmp .Lspr_skip
.Lspr_tall:
    mov ch, byte ptr [bp + 1]
    and ch, 0xFE
    mov byte ptr [di], ch
    or ch, 1
    mov byte ptr [di + 4], ch
    mov ch, 1
    mov byte ptr [di + 1], cl
    mov byte ptr [di + 5], cl
    mov word ptr [di + 2], ax
    add al, 8
    mov word ptr [di + 6], ax
    add di, 8
.Lspr_skip:
    add bp, 2
    dec byte ptr [spr_cnt]
    jnz .Lspr_loop
.Lspr_done:
    mov ax, di
    pop bp
    pop di
    pop si
    pop cx
    pop bx
    IA16_RET

    // void psg_run(const uint8_t *q, uint8_t n): n (1-255) writes to the
    // SN76489, into psg_st (main.c: latch, noise, dirty, -, vol[4],
    // tone[3]); the same as main.c's psg_apply on each, there ~45 cycles a
    // write (Sailor Moon S: a sample through the volume, hundreds a frame).
    .global psg_run
psg_run:
    push bx
    push cx
    push si
    push di
    mov si, ax
    mov di, dx
    and di, 0xFF
    mov ah, byte ptr [psg_st]       // the latched register
    mov ch, byte ptr [psg_st + 2]   // dirty
    cmp byte ptr [pcm_on], 0
    jne .Lpcm_loop
.Lpsg_loop:
    lodsb
.Lpsg_byte:
    test al, al
    jns .Lpsg_data
    mov ah, al                  // a latch byte
.Lpsg_data:
    mov bl, ah
    shr bl, 5
    and bx, 3                   // the channel
    test ah, 0x10
    jz .Lpsg_tone
    and al, 0x0F                // volume
    mov byte ptr [psg_st + 4 + bx], al
    mov cl, bl
    mov al, 1
    shl al, cl
    or ch, al
    // a sample (main.c, pcm_buf): channel 2's volume, the three without tone
    cmp bl, 2
    jne .Lpsg_next
    cmp byte ptr [pcm_on], 0
    je .Lpsg_next
    mov bx, word ptr [pcm_n]
    cmp bx, 640                 // (PCM_MAX)
    jae .Lpsg_next
    push bx
    xor bx, bx
    mov bl, byte ptr [psg_st + 4]
    mov dl, byte ptr [pcm_level + bx]
    mov bl, byte ptr [psg_st + 5]
    add dl, byte ptr [pcm_level + bx]
    mov bl, byte ptr [psg_st + 6]
    add dl, byte ptr [pcm_level + bx]
    pop bx
    mov byte ptr [pcm_buf + bx], dl
    inc bx
    mov word ptr [pcm_n], bx
    jmp .Lpsg_next
    // samples (pcm_on: the three tones 0-1): nearly all the bytes are
    // volume latches, taken here in fewer steps; anything else goes to
    // the loop above (for the rest of the queue)
.Lpcm_loop:
    lodsb
    mov bl, al
    and bl, 0x90
    cmp bl, 0x90
    jne .Lpsg_byte
    mov ah, al                  // (the latch)
    mov bl, al
    shr bl, 5
    and bx, 3
    and al, 0x0F
    mov byte ptr [psg_st + 4 + bx], al
    cmp bl, 2
    jne .Lpcm_next
    mov bx, word ptr [pcm_n]
    cmp bx, 640                 // (PCM_MAX)
    jae .Lpcm_next
    push bx
    xor bx, bx
    mov bl, byte ptr [psg_st + 4]
    mov dl, byte ptr [pcm_level + bx]
    mov bl, byte ptr [psg_st + 5]
    add dl, byte ptr [pcm_level + bx]
    mov bl, al
    add dl, byte ptr [pcm_level + bx]
    pop bx
    mov byte ptr [pcm_buf + bx], dl
    inc bx
    mov word ptr [pcm_n], bx
.Lpcm_next:
    or ch, 0x07                 // (volumes to write when the samples end)
    dec di
    jnz .Lpcm_loop
    jmp .Lpsg_end
.Lpsg_tone:
    cmp bl, 3
    je .Lpsg_noise
    shl bx, 1
    test al, al
    js .Lpsg_low
    and ax, 0xFF3F              // (AH, the latch, kept)
    push ax
    xor ah, ah
    shl ax, 4                   // bits 4-9
    and word ptr [psg_st + 8 + bx], 0x000F
    or word ptr [psg_st + 8 + bx], ax
    pop ax
    jmp .Lpsg_tone_dirty
.Lpsg_low:
    and al, 0x0F                // bits 0-3
    and word ptr [psg_st + 8 + bx], 0x03F0
    or byte ptr [psg_st + 8 + bx], al
.Lpsg_tone_dirty:
    shr bx, 1
    mov cl, bl
    mov al, 0x10
    shl al, cl
    or ch, al
    cmp bl, 2
    jne .Lpsg_next
    mov al, byte ptr [psg_st + 1]
    and al, 3
    cmp al, 3
    jne .Lpsg_next
    or ch, 0x80                 // the noise follows tone 2
    jmp .Lpsg_next
.Lpsg_noise:
    and al, 7
    mov byte ptr [psg_st + 1], al
    or ch, 0x80
.Lpsg_next:
    dec di
    jnz .Lpsg_loop
.Lpsg_end:
    mov byte ptr [psg_st], ah
    mov byte ptr [psg_st + 2], ch
    pop di
    pop si
    pop cx
    pop bx
    IA16_RET
