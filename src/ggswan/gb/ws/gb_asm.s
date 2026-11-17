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
#define SPR_ORDER 0xCB00        /* 40 bytes: the sprites in last frame's order, as offsets into the OAM; CB2Eh: the word below the keys */
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
    // The sprites on the screen, each put in its place among those
    // before it: by X, then by its place in the OAM (the Game Boy's
    // order of drawing). They are taken in the order they had a frame
    // ago, so few change places. The ones not on the screen are not
    // sorted at all (patch 193): until then they were, last, and a
    // sprite that came onto the screen had to be moved past every one of
    // them - Castlevania II, which has four or five of its forty on the
    // screen and moves them round the OAM, made 165 moves a frame, half
    // this routine's time. SPR_ORDER for the next frame: the ones off the
    // screen first, as they come (written behind the place being read),
    // then the ones on it, sorted.
    mov word ptr [SPR_KEYS - 2], 0      // (a key below every other: the moves stop there)
    xor si, si
    xor bp, bp                  // twice the sprites on the screen so far
    xor cx, cx                  // the ones off it so far
1:
    mov bh, [gbv_oam_hi]        // (FFh: 9D00h, the OAM as it was before a copy made in the middle of the frame)
    mov bl, [SPR_ORDER + si]
    mov ax, [0x9E00 + bx]       // AL = Y, AH = X
    mov dl, bl
    test al, al
    jz 5f
    cmp al, 160
    jae 5f
    test ah, ah
    jz 5f
    cmp ah, 168
    jae 5f
    mov bl, al
    xor bh, bh
    cmp byte ptr [bx + SPR_TALL], 0
    jnz 6f
    cmp al, 8
    jbe 5f                      // (8 x 8, above the screen: not seen)
6:
    mov al, dl
    mov di, bp
    add bp, 2
    jmp 2f
    .balign 2
4:
    mov [SPR_KEYS + di], dx
    sub di, 2
2:
    mov dx, [SPR_KEYS - 2 + di]
    cmp dx, ax
    ja 4b
    mov [SPR_KEYS + di], ax
3:
    inc si
    cmp si, 40
    jb 1b
    jmp 7f
5:
    mov bx, cx
    mov [SPR_ORDER + bx], dl
    inc cx
    jmp 3b
7:
    // (the order kept: the ones on the screen after the others)
    xor si, si
    mov bx, cx
    jmp 9f
8:
    mov al, [SPR_KEYS + si]
    mov [SPR_ORDER + bx], al
    inc bx
    add si, 2
9:
    cmp si, bp
    jb 8b

    mov di, 0x5800
    xor dx, dx                  // WS sprites written
    xor si, si
    test bp, bp
    jz 9f
6:
    mov bx, [SPR_KEYS + si]
    mov bh, [gbv_oam_hi]
    mov ax, [0x9E02 + bx]       // AL = tile, AH = flags
    push si
    mov si, ax
    shr si, 12                  // the flags' high half
    mov ah, cs:[spr_attr + si]
    pop si
    mov cx, [0x9E00 + bx]       // CL = Y, CH = X
    mov bl, cl
    xor bh, bh
    sub cl, 16
    add ch, 24
    cmp byte ptr [bx + SPR_TALL], 0
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
    cmp si, bp
    jb 6b
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

/*
 * uint8_t gbd_scan_fast(void): scan_block's loop (gb_drc.c) for the
 * instructions it has nothing of its own to do for - op_inf's second
 * byte without bit 5, and without bit 6 once something is known of BC,
 * DE, HL (scan_anyk; what i_dir and i_cls then are for each is in
 * scan_cdir and scan_ccls). From scan_pc, n_ins, scan_total; back with
 * them: 0, the next instruction is scan_block's (or the block is full, or
 * its bytes are not in tbuf); 1, the block ends with the last one; 2, a
 * block may already start at scan_pc (DRC_HAS: scan_block looks).
 */
    .global gbd_scan_fast
gbd_scan_fast:
    push si
    push di
    mov si, [scan_pc]
    xor ax, ax
    mov al, [n_ins]
    mov di, ax
    mov cl, [scan_total]
    mov ch, [scan_anyk]
scf_loop:
    cmp di, 40
    jae scf_slow
    cmp cl, 90
    jae scf_slow
    mov bx, si
    sub bx, [tbase]
    cmp bx, [scan_tl1]
    jae scf_slow
    mov bl, [tbuf + bx]
    xor bh, bh
    shl bx, 1
    mov ax, [op_inf + bx]       // AL: the flags (F_END, F_OUT), AH: length, cycles, bits 5-7
    test ah, 0x60
    jz scf_plain
    test ah, 0x20
    jnz scf_slow
    test ch, ch
    jnz scf_slow
scf_plain:
    mov bx, di
    shl bx, 1
    test ch, ch
    jz scf_put
    cmp byte ptr [scan_cok], 0
    je scf_want
    mov dx, [scan_cdir]
    mov [i_dir + bx], dx
    mov dx, [scan_cdir + 2]
    mov [i_dir + 80 + bx], dx
    mov dx, [scan_cdir + 4]
    mov [i_dir + 160 + bx], dx
    mov dl, [scan_ccls]
    mov [i_cls + di], dl
    mov dl, [scan_ccls + 1]
    mov [i_cls + 40 + di], dl
    mov dl, [scan_ccls + 2]
    mov [i_cls + 80 + di], dl
scf_put:
    mov dl, ah
    shr dl, 2
    and dl, 7
    add dl, 2
    add cl, dl
    mov [i_pc + bx], si
    mov [i_inf + di], ah
    mov [i_fa + di], al
    inc di
    mov bl, ah
    and bx, 3
    add si, bx
    test al, 0x40               // F_END
    jnz scf_end
    cmp byte ptr [in_ram], 0
    jne scf_ram
    cmp di, 6
    jb scf_loop
    cmp si, 0x8000
    jae scf_loop
    mov bx, si
    shr bx, 6
    xor bx, si
    cmp si, 0x4000
    jb scf_h0
    xor bx, [drc_hashx]
    jmp scf_hd
scf_h0:
    xor bx, [drc_hash0]
scf_hd:
    and bx, 8191
    mov dx, bx
    shr bx, 3
    mov al, [0x9800 + bx]       // DRC_HAS
    mov dh, cl
    mov cl, dl
    and cl, 7
    shr al, cl
    mov cl, dh
    test al, 1
    jz scf_loop
    mov al, 2
    jmp scf_out
scf_ram:
    mov bx, si
    sub bx, [blk_pc]
    cmp bx, 48
    jbe scf_loop
scf_end:
    mov al, 1
    jmp scf_out
scf_want:
    mov al, 3
    jmp scf_out
scf_slow:
    xor al, al
scf_out:
    mov [scan_pc], si
    mov bx, di
    mov [n_ins], bl
    mov [scan_total], cl
    pop di
    pop si
    IA16_RET

/*
 * void gbd_scan_live(void): the flags needed before each instruction of
 * the block (i_live), from what each does with them (i_fa: bits 0-2 the
 * flags it sets, 3-5 those it reads, bit 7 it may leave the block), last
 * instruction first; after the block all three are needed.
 */
    .global gbd_scan_live
gbd_scan_live:
    xor bx, bx
    mov bl, [n_ins]
    mov al, 7
scl_loop:
    dec bx
    js scl_done
    mov ah, [i_fa + bx]
    test ah, 0x80
    jnz scl_all
    mov dl, ah
    and dl, 7
    not dl
    and al, dl
    shr ah, 3
    and ah, 7
    or al, ah
    mov [i_live + bx], al
    jmp scl_loop
scl_all:
    mov al, 7
    mov [i_live + bx], al
    jmp scl_loop
scl_done:
    IA16_RET

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

/*
 * The patterns a frame late (gb_video.c).
 */
#define TQ_IDX  0xCC30
#define TQ_ADR  0xCDB0
#define TQ_PIDX 0xCE70
#define TQ_DAT  0xF800
#define TQ_S    32
// void gbv_tq_new(uint16_t a): a copy for the tile at the Game Boy's
// address a in the set being made, with what the tile will be then
    .global gbv_tq_new
gbv_tq_new:
    push si
    push di
    push es
    mov si, ax
    and si, 0xFFF0
    cmp byte ptr ss:[gbv_tq_on], 0
    je 9f
    mov bl, ss:[gbv_tq_n]
    cmp bl, TQ_S
    jb 1f
    IA16_CALL gbv_tq_full
    jmp 9f
1:
    xor bh, bh
    mov di, bx
    shl di, 4
    add di, ss:[gbv_tq_base]
    add di, 16                  // the copy
    mov ss:[gbv_tq_lastp], di
    mov ss:[gbv_tq_last], si
    mov ss:[gbv_pw_last], si
    mov al, ss:[gbv_tq_cur]
    xor ah, ah
    shl ax, 5
    add ax, bx
    shl ax, 1
    xchg ax, bx                 // (AL: tiles so far)
    lea cx, [si - 0x6000]       // the tile
    mov ss:[bx + TQ_ADR], cx
    inc al
    mov ss:[gbv_tq_n], al
    mov bx, si
    shr bx, 4
    mov ss:[bx + TQ_IDX - 0x800], al
    mov bl, ss:[bx + TQ_PIDX - 0x800]
    mov si, cx
    test bl, bl
    jz 2f
    xor bh, bh
    dec bx
    shl bx, 4
    lea si, [bx + TQ_DAT]
2:
    push ss
    pop es
    cld
    mov cx, 8
    rep movsw
9:
    pop es
    pop di
    pop si
    IA16_RET

// void gbv_tq_copy_asm(const uint16_t *adr, const uint8_t *dat, uint16_t n):
// n copies at dat to the tiles whose addresses are at adr; a tile whose
// newest copy waiting this is has none waiting any more
    .global gbv_tq_copy_asm
gbv_tq_copy_asm:
    push si
    push di
    push es
    push ss
    pop es
    cld
    mov bx, ax
    mov si, dx
    jcxz 9f
1:
    mov di, ss:[bx]
    add bx, 2
    push bx
    mov ax, si
    sub ax, TQ_DAT
    shr ax, 4
    inc ax
    mov bx, di
    shr bx, 4
    cmp ss:[bx + TQ_PIDX - 0x200], al
    jne 2f
    mov byte ptr ss:[bx + TQ_PIDX - 0x200], 0
2:
    pop bx
    push cx
    mov cx, 8
    rep movsw
    pop cx
    loop 1b
9:
    pop es
    pop di
    pop si
    IA16_RET

/*
 * uint8_t gbf_sched_asm(void): gbf_hw.c's schedule(), the next event
 * after now, instruction for instruction (it was a tenth to a sixth of
 * the time of a program with an interrupt at every line); 0 and nothing
 * done while a serial transfer is on (C does that).
 * SI the frame's cycle now, DI the next event's, BX "from".
 */
#define LINE_MC 114
#define FRAME_MC 17556
#define VBL_MC (144 * 114 + 1)
    .global gbf_sched_asm
gbf_sched_asm:
    xor al, al
    cmp byte ptr ss:[gbf_ser_on], 0
    je 1f
    IA16_RET
1:
    // the commonest case by itself: the LCD on, LYC's interrupt the only
    // one of STAT's asked for, the timer off
    mov al, ss:[0x9F41]
    and al, 0x78
    cmp al, 0x40
    jne sched_any
    test byte ptr ss:[0x9F07], 4
    jnz sched_any
    cmp byte ptr ss:[gb + 12], 0
    je sched_any
    push si
    mov si, ss:[gbf_ev_pos]
    sub si, ss:[gbf_cyc]
    mov dx, FRAME_MC
    cmp byte ptr ss:[gbf_vbl_done], 0
    jne 1f
    mov dx, VBL_MC
1:
    mov bx, ss:[gbf_stat_done]
    cmp bx, si
    jb 1f
    mov bx, si
1:
    mov al, ss:[0x9F45]
    cmp al, 154
    jae 2f
    mov cx, 153 * LINE_MC + 3
    test al, al
    jz 1f
    mov ah, LINE_MC
    mul ah
    inc ax
    mov cx, ax
1:
    cmp cx, bx
    jbe 2f
    cmp cx, dx
    jae 2f
    mov dx, cx
2:
    cmp dx, bx
    ja 1f
    mov dx, bx
    inc dx
1:
    mov ss:[gbf_ev_pos], dx
    sub dx, si                          // (zero or less: the event is now)
    mov ss:[gbf_cyc], dx
    pop si
    mov al, 1
    IA16_RET
sched_any:
    push si
    push di
    mov si, ss:[gbf_ev_pos]
    sub si, ss:[gbf_cyc]
    mov di, FRAME_MC
    cmp byte ptr ss:[gb + 12], 0        // gb.lcd_on (gb_drc.h: GB_LCD_ON)
    jne 1f
    jmp sched_timer
1:
    cmp byte ptr ss:[gbf_vbl_done], 0
    jne 1f
    mov di, VBL_MC
1:
    mov al, ss:[0x9F41]                 // STAT
    test al, 0x78
    jnz 1f
    jmp sched_timer
1:
    mov bx, ss:[gbf_stat_done]
    cmp bx, si
    jb 1f
    mov bx, si
1:
    test al, 0x28
    jz sched_m1
    // the line `from` is in (CL), its first cycle (DX), the cycle in it (CH)
    push ax
    mov ax, bx
    mov cl, LINE_MC
    div cl
    mov cx, ax
    mov ah, LINE_MC
    mul ah
    mov dx, ax
    pop ax
    test al, 0x08
    jz sched_m2
    push ax
    mov ax, FRAME_MC
    cmp cl, 144
    jae 2f
    cmp ch, 63
    jae 1f
    mov ax, dx
    add ax, 63
    jmp 2f
1:
    cmp cl, 143
    jae 2f
    mov ax, dx
    add ax, LINE_MC + 63
2:
    cmp ax, di
    jae 1f
    mov di, ax
1:
    pop ax
sched_m2:
    test al, 0x20
    jz sched_m1
    push ax
    cmp cl, 143
    jae 1f
    mov ax, dx
    add ax, LINE_MC
    jmp 2f
1:
    mov ax, VBL_MC
    cmp cl, 144
    jb 2f
    cmp bx, VBL_MC
    jb 2f
    mov ax, FRAME_MC
2:
    cmp ax, di
    jae 1f
    mov di, ax
1:
    pop ax
sched_m1:
    test al, 0x30
    jz 1f
    cmp bx, VBL_MC
    jae 1f
    cmp di, VBL_MC
    jbe 1f
    mov di, VBL_MC
1:
    test al, 0x40
    jz 2f
    mov al, ss:[0x9F45]                 // LYC
    cmp al, 154
    jae 2f
    mov dx, 153 * LINE_MC + 3
    test al, al
    jz 1f
    mov ah, LINE_MC
    mul ah
    inc ax
    mov dx, ax
1:
    cmp dx, bx
    jbe 2f
    cmp dx, di
    jae 2f
    mov di, dx
2:
    cmp di, bx
    ja sched_timer
    mov di, bx
    inc di
sched_timer:
    cmp di, si
    ja 1f
    // (already passed: now)
    mov ss:[gbf_ev_pos], di
    sub di, si
    mov ss:[gbf_cyc], di
    jmp 9f
1:
    sub di, si                          // the cycles left
    mov al, ss:[0x9F07]                 // TAC
    test al, 4
    jz 2f
    mov cl, al
    and cl, 3
    shl cl, 1                           // TAC's rates: 256, 4, 16, 64 cycles
    jnz 3f
    mov cl, 8
3:
    mov ax, 1
    shl ax, cl                          // the period
    mov dx, ss:[gbf_div_off]
    add dx, si
    dec ax
    and dx, ax
    inc ax
    sub ax, dx                          // to the first edge
    mov dl, ss:[0x9F05]                 // TIMA
    not dl
    xor dh, dh
    shl dx, cl                          // the edges after it, to the overflow
    cmp dx, di
    jae 2f
    add ax, dx
    cmp ax, di
    jae 2f
    mov di, ax
2:
    test di, di
    jnz 1f
    inc di
1:
    mov ax, si
    add ax, di
    mov ss:[gbf_ev_pos], ax
    mov ss:[gbf_cyc], di
9:
    pop di
    pop si
    mov al, 1
    IA16_RET

/*
 * uint8_t gbf_event_asm(void): gbf_hw.c's event() when there is nothing
 * in it but the timer and LYC's interrupt, which is what an event is in
 * the middle of a frame for most programs; then schedule(), above.
 * 0 and nothing done otherwise.
 */
    .global gbf_event_asm
gbf_event_asm:
    xor al, al
    cmp byte ptr ss:[gbf_ser_on], 0
    jne 9f
    mov dx, ss:[gbf_ev_pos]
    sub dx, ss:[gbf_cyc]                // the frame's cycle now
    cmp dx, FRAME_MC
    jae 9f
    cmp byte ptr ss:[gb + 12], 0        // gb.lcd_on
    je 1f
    test byte ptr ss:[0x9F41], 0x38
    jnz 9f
    cmp byte ptr ss:[gbf_vbl_done], 0
    jne 1f
    cmp dx, VBL_MC
    jb 1f
9:
    IA16_RET
1:
    // timer_sync
    add dx, ss:[gbf_div_off]            // DIV / 4 now
    mov al, ss:[0x9F07]                 // TAC
    test al, 4
    jz 4f
    mov cl, al
    and cl, 3
    shl cl, 1
    jnz 2f
    mov cl, 8
2:
    mov ax, dx
    shr ax, cl
    mov bx, ss:[gbf_tim_last]
    shr bx, cl
    sub ax, bx
    mov bx, 0xFFFF
    shr bx, cl
    and ax, bx                          // the timer's steps since
    jz 4f
    xor bh, bh
    mov bl, ss:[0x9F05]                 // TIMA
    add ax, bx
2:
    cmp ax, 0xFF
    jbe 3f
    sub ax, 0x100
    mov bl, ss:[0x9F06]                 // TMA
    add ax, bx
    or byte ptr ss:[0x9F0F], 4
    jmp 2b
3:
    mov ss:[0x9F05], al
4:
    mov ss:[gbf_tim_last], dx
    // LYC's interrupt: at the one cycle its signal rises
    mov bx, ss:[gbf_ev_pos]
    cmp byte ptr ss:[gb + 12], 0
    je 5f
    test byte ptr ss:[0x9F41], 0x40
    jz 5f
    test bx, bx
    jz 5f
    cmp bx, FRAME_MC
    jae 5f
    mov al, ss:[0x9F45]
    cmp al, 154
    jae 5f
    mov cx, 153 * LINE_MC + 3
    test al, al
    jz 2f
    mov ah, LINE_MC
    mul ah
    inc ax
    mov cx, ax
2:
    cmp bx, cx
    jne 5f
    or byte ptr ss:[0x9F0F], 2
5:
    mov ss:[gbf_stat_done], bx
    jmp gbf_sched_asm
