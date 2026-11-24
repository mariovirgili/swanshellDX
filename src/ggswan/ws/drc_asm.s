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
#ifdef GGSWAN_SF7000
    // sfswan: ES is the Z80's memory in translated code (drc.c: FLAT);
    // drc_st starts with the segment table, all of it that segment
    mov es, [drc_st]
#endif
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

    IRQ_ENTRY line_irq_slow, line_irq

#ifdef GGSWAN_SG1000
    // sgswan, sfswan: the line interrupt. With the whole picture on the
    // screen (sg_sq_act, sg_video.inc) a step of the Y scroll inside the
    // picture is served here: at line l (the match was set on it), k =
    // (l + 1) / 3 is the scroll from line l + 1 on, and the next match is
    // line 3 k + 2, or the end of the screen (144) from line 143. That
    // line there already (this was served late): again. Anything else
    // (no whole picture, the end of the screen): C, line_irq.
    .global sg_line_entry
sg_line_entry:
    cmp byte ptr ss:[sg_sq_act], 0
    je line_irq_slow
    push ax
    push cx
1:
    in al, 0x02                         // (the line being drawn)
    cmp al, 143
    jae 9f
    // k = (l + 1) / 3 == ((l + 2) * 85) >> 8, exact for l in 0..142
    add al, 2
    mov cl, 85
    mul cl
    mov al, ah                          // al = k
    out 0x11, al
    mov ah, al
    add al, al
    add al, ah
    add al, 2                           // 3 k + 2
    cmp al, 143
    jb 2f
    mov al, 144
2:
    out 0x03, al
    mov ah, al
    mov al, 0x10                        // WS_INT_ACK_LINE_MATCH
    out 0xB6, al
    cmp ah, 144
    je 3f
    in al, 0x02
    cmp al, ah
    jae 1b
3:
    pop cx
    pop ax
    iret
9:
    pop cx
    pop ax
    jmp line_irq_slow

    // void sg_conv_g2_asm(uint16_t dst, uint16_t pg_off)
    // Converts an 8x8 Graphics II tile directly from sg_vr (0x2000:pg_off
    // and 0x2000:[sg_conv_ct]) into WS 4bpp tile IRAM at DS:dst. Caches the
    // last color byte across consecutive rows.
    .global sg_conv_g2_asm
sg_conv_g2_asm:
    push bx
    push cx
    push si
    push di
    push bp
    push es
    mov di, ax
    mov si, dx
    mov bx, word ptr [sg_conv_ct]
    mov ax, 0x2000
    mov es, ax
    mov byte ptr [sg_conv_cnt], 8
    mov al, byte ptr es:[bx]
    jmp .Lg2_newcol
.Lg2_loop:
    mov al, byte ptr es:[bx]
    cmp al, byte ptr [sg_conv_lk]
    je .Lg2_samecol
.Lg2_newcol:
    mov byte ptr [sg_conv_lk], al
    push bx
    xor ah, ah
    mov bx, ax
    and bx, 0x000F
    shl bx, 2
    mov dx, word ptr [sg_plane4 + bx]       // DX = bl
    mov bp, word ptr [sg_plane4 + bx + 2]   // BP = bh
    mov bx, ax
    and bx, 0x00F0
    shr bx, 2
    mov cx, word ptr [sg_plane4 + bx]       // CX = fl
    xor cx, dx                              // CX = fl ^ bl
    mov ax, word ptr [sg_plane4 + bx + 2]   // AX = fh
    xor ax, bp                              // AX = fh ^ bh
    mov word ptr [sg_conv_xhi], ax
    pop bx
.Lg2_samecol:
    inc bx
    mov al, byte ptr es:[si]
    inc si
    mov ah, al
    push ax
    and ax, cx
    xor ax, dx
    mov word ptr [di], ax
    pop ax
    and ax, word ptr [sg_conv_xhi]
    xor ax, bp
    mov word ptr [di + 2], ax
    add di, 4
    dec byte ptr [sg_conv_cnt]
    jnz .Lg2_loop
    pop es
    pop bp
    pop di
    pop si
    pop cx
    pop bx
    IA16_RET

    // void sg_conv_g1_asm(uint16_t dst, uint16_t pg_off)
    // Graphics I tile: single color byte in [sg_conv_lk] for all 8 rows.
    .global sg_conv_g1_asm
sg_conv_g1_asm:
    push bx
    push cx
    push si
    push di
    push bp
    push es
    mov di, ax
    mov si, dx
    xor ax, ax
    mov al, byte ptr [sg_conv_lk]
    mov bx, ax
    and bx, 0x000F
    shl bx, 2
    mov dx, word ptr [sg_plane4 + bx]       // DX = bl
    mov bp, word ptr [sg_plane4 + bx + 2]   // BP = bh
    mov bx, ax
    and bx, 0x00F0
    shr bx, 2
    mov cx, word ptr [sg_plane4 + bx]       // CX = fl
    xor cx, dx                              // CX = fl ^ bl
    mov ax, word ptr [sg_plane4 + bx + 2]   // AX = fh
    xor ax, bp                              // AX = fh ^ bh
    mov word ptr [sg_conv_xhi], ax
    mov ax, 0x2000
    mov es, ax
    mov bl, 8
.Lg1_loop:
    mov al, byte ptr es:[si]
    inc si
    mov ah, al
    push ax
    and ax, cx
    xor ax, dx
    mov word ptr [di], ax
    pop ax
    and ax, word ptr [sg_conv_xhi]
    xor ax, bp
    mov word ptr [di + 2], ax
    add di, 4
    dec bl
    jnz .Lg1_loop
    pop es
    pop bp
    pop di
    pop si
    pop cx
    pop bx
    IA16_RET

    // uint16_t sg_kind_conv_asm(uint16_t dst, uint16_t src_off)
    // Converts 1 (8x8) or 4 (16x16) unmagnified sprite pieces directly from
    // 0x2000:src_off into WS tile IRAM, returning the empty-tile bitmask.
    .global sg_kind_conv_asm
sg_kind_conv_asm:
    push bx
    push cx
    push si
    push di
    push bp
    push es
    mov di, ax
    mov si, dx
    xor bx, bx
    mov bl, byte ptr [sg_conv_lk]
    shl bx, 2
    mov cx, word ptr [sg_plane4 + bx]       // CX = lo
    mov dx, word ptr [sg_plane4 + bx + 2]   // DX = hi
    mov ax, 0x2000
    mov es, ax
    xor bx, bx                              // BL = empty mask, BH = piece bit
    mov bh, 1
.Lsk_piece:
    push bx
    xor bl, bl                              // BL = OR of the 8 pattern bytes
    mov bh, 8
.Lsk_row:
    mov al, byte ptr es:[si]
    inc si
    or bl, al
    mov ah, al
    mov bp, ax
    and ax, cx
    mov word ptr [di], ax
    and bp, dx
    mov word ptr [di + 2], bp
    add di, 4
    dec bh
    jnz .Lsk_row
    test bl, bl
    pop bx
    jnz .Lsk_notempty
    or bl, bh
.Lsk_notempty:
    shl bh, 1
    dec byte ptr [sg_conv_cnt]
    jnz .Lsk_piece
    xor ah, ah
    mov al, bl
    pop es
    pop bp
    pop di
    pop si
    pop cx
    pop bx
    IA16_RET
#endif

#ifndef GGSWAN_SG1000   // (sgswan: sg_line_entry and line_irq_slow, sg_video.inc)
    // The line interrupt. A scroll split inside the picture is served here
    // (main.c line_irq does the same, and everything else: this is its
    // loop for i > 0, with the same order of port writes):
    //   the value of split act_i to the scroll register(s), act_i + 1;
    //   next = the line before the next split, or the end of the screen;
    //   below the picture already: the end of the screen now (C, told
    //   by act_i = 0);
    //   that line being drawn already: its value now, again;
    //   else the match on that line, the acknowledge, and the line read
    //   once more: there already (the match was missed): C serves it.
    // Variables through SS (the data segment): DS is whatever the
    // interrupted code had, a ROM bank in the OUTI and LDI routines.
    // (Patch 71 had a short path too, taken out because Super Monaco GP II
    // flickered with it: it came before the interrupts loaded DS at all,
    // and before the lost matches were found, patches 121-123.)
#define DRC_ST_PRIO_ON 506
#define LI_VIEW_H 144   // (GG_VIEW_H)
    .global line_irq_entry
line_irq_entry:
    push ax
    push bx
    in al, 0x03                         // (the line it was set for)
    cmp al, LI_VIEW_H
    jae .Lli_slow                       // the end of the screen
    xor bx, bx
    mov bl, ss:[act_i]
    test bl, bl
    jz .Lli_slow
    inc word ptr ss:[diag_calls]
.Lli_again:
    mov al, ss:[act_val + bx]
    out 0x10, al                        // screen 1 X scroll
    cmp byte ptr ss:[drc_st + DRC_ST_PRIO_ON], 0
    jz 1f
    out 0x12, al                        // screen 2 with it
1:
#ifdef GGSWAN_SMS
    cmp byte ptr ss:[act_sq], 0
    jz 1f
    mov al, ss:[act_y + bx]             // the picture squeezed: Y too
    out 0x11, al
    cmp byte ptr ss:[drc_st + DRC_ST_PRIO_ON], 0
    jz 1f
    out 0x13, al
1:
#endif
    inc bx
    mov ah, LI_VIEW_H
    cmp bl, ss:[act_n]
    jae 2f
    mov ah, ss:[act_line + bx]
    dec ah
    cmp ah, LI_VIEW_H
    jbe 2f
    mov ah, LI_VIEW_H
2:
    in al, 0x02                         // the line being drawn
    cmp al, LI_VIEW_H
    jae .Lli_end
    cmp ah, al
    jbe .Lli_again                      // late: its value now
    mov ss:[act_i], bl
    mov al, ah
    out 0x03, al
    mov al, 0x10                        // WS_INT_ACK_LINE_MATCH
    out 0xB6, al
    in al, 0x02
    cmp al, ah
    jae .Lli_slow                       // there already
    pop bx
    pop ax
    iret
.Lli_end:
    mov byte ptr ss:[act_i], 0          // (C: the end of the screen)
.Lli_slow:
    pop bx
    pop ax
    jmp line_irq_slow
#endif
    IRQ_ENTRY vblank_irq_entry, vblank_irq

#ifndef GGSWAN_SG1000   // (the Game Gear's video: main.c)

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
    // table. GG_VIEW_X/Y/W/H: 48, 24, 160, 144, at WS column 32 (smsswan:
    // 16, 24, 224, 144 at column 0).
    //   spr_par: +0 tile bank byte, +1 8x16, +2 X offset + 16 (GG_VIEW_X -
    //            the WS column), +3 hidden rows (113 - sprite height)
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
#ifdef GGSWAN_SMS
    jne .Lspr_go
    cmp byte ptr [spr_224], 0   // (224 lines: D0h does not end the list)
    je .Lspr_done
.Lspr_go:
#else
    je .Lspr_done
#endif
#ifdef GGSWAN_SMS
    inc al
    cmp byte ptr [spr_sq], 0
    je .Lspr_view
    cmp al, 192                 // the whole picture squeezed: the row
    jae .Lspr_skip              // scaled by 3/4 (none above line 0)
    mov ah, al
    shr ah, 2
    sub al, ah
    jmp .Lspr_row
.Lspr_view:
    sub al, byte ptr [gg_view_y]   // the WS row (the view moves)
.Lspr_row:
#else
    add al, 1 - 24              // the WS row
#endif
    mov ah, al
    sub ah, 144
    cmp ah, dh
    jb .Lspr_skip               // outside the 144 rows
    mov ah, byte ptr [bp]
    sub ah, dl                  // the WS column
    push ax
#ifdef GGSWAN_SMS
    sub ah, 0 + 224             // (main.c: WS_VIEW_ORIGIN_X + GG_VIEW_W,
    cmp ah, 256 - 224 - 7       // 256 - GG_VIEW_W - 7)
    pop ax
    jb .Lspr_skip               // outside the 224 columns
#else
    sub ah, 32 + 160
    cmp ah, 89
    pop ax
    jb .Lspr_skip               // outside the 160 columns
#endif
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
#ifdef GGSWAN_SMS
    add al, byte ptr [spr_dy]   // (6 when squeezed)
#else
    add al, 8
#endif
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

#endif

    // void psg_run(const uint8_t *q, uint8_t n): n (1-255) writes to the
    // SN76489, into psg_st (main.c: latch, noise, dirty, -, vol[4],
    // tone[3]); the same as main.c's psg_apply on each, there ~45 cycles a
    // write (Sailor Moon S: a sample through the volume, hundreds a frame).
#ifndef GGSWAN_SG1000
    // uint8_t coll_scan(const uint8_t *sat): the sprite collision flag's
    // walk of the sprite table (main.c, gg_platform_sprite_status, which
    // has it in C for the PC tests: the same, step by step). For each
    // sprite on the lines shown, the earlier ones in its band of 16
    // lines and the two beside it (coll_head, coll_nxt) are looked at:
    // overlapping sideways (coll_sx), then in height (coll_sy), then
    // coll_pair(i, j) in C for their rows. 20h: two pixels overlap.
    // CL the sprite, DL its X, DH its Y + 1, BX the band, AH bands left,
    // DI the other sprite, SI the table.
    .global coll_scan
coll_scan:
    push bx
    push cx
    push si
    push di
    mov si, ax
    xor cx, cx
.Lco_loop:
    mov bx, cx
    mov al, byte ptr [si + bx]
    cmp al, 0xD0
    jne 1f
    cmp byte ptr [coll_noend], 0
    je .Lco_none
1:
    inc al
    cmp al, byte ptr [coll_lines]
    jae .Lco_next
    mov dh, al
    shl bx, 1
    cmp byte ptr [coll_emp_has], 0      // (a tile known to have no pixel)
    je 1f
    mov dl, byte ptr [si + bx + 0x81]
    cmp dl, byte ptr [coll_emp_raw]
    je .Lco_next
1:
    mov dl, byte ptr [si + bx + 0x80]
    xor bx, bx
    mov bl, al
    shr bl, 4
    push bx                             // (its band, for the lists)
    mov ah, 3
    test bl, bl
    jz 1f
    dec bx
    jmp .Lco_band
1:
    dec ah
.Lco_band:
    mov al, byte ptr [coll_head + bx]
.Lco_walk:
    cmp al, 0xFF
    je .Lco_band_end
    mov di, ax
    and di, 0x00FF                      // DI = j (AH: the bands left)
    mov al, dl
    sub al, byte ptr [coll_sx + di]
    add al, 7
    cmp al, 14
    ja .Lco_other
    mov al, dh
    sub al, byte ptr [coll_sy + di]
    add al, byte ptr [coll_h1]
    cmp al, byte ptr [coll_hh]
    ja .Lco_other
    push ax
    push bx
    push cx
    push dx
    push di
    mov ax, cx
    mov dx, di
    IA16_CALL coll_pair
    pop di
    pop dx
    pop cx
    pop bx
    cmp al, 1
    je .Lco_hit
    cmp al, 2
    pop ax
    je .Lco_listed
.Lco_other:
    mov al, byte ptr [coll_nxt + di]
    jmp .Lco_walk
.Lco_band_end:
    inc bx
    dec ah
    jnz .Lco_band
.Lco_listed:
    pop bx
    mov di, cx
    mov byte ptr [coll_sx + di], dl
    mov byte ptr [coll_sy + di], dh
    mov al, byte ptr [coll_head + bx]
    mov byte ptr [coll_nxt + di], al
    mov byte ptr [coll_head + bx], cl
.Lco_next:
    inc cx
    cmp cl, 64
    jb .Lco_loop
.Lco_none:
    xor ax, ax
    jmp .Lco_ret
.Lco_hit:
    pop ax
    pop bx
    mov ax, 0x20
.Lco_ret:
    pop di
    pop si
    pop cx
    pop bx
    IA16_RET
#endif

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
