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

    // void log_commit_tramp(void);
    //
    // Called by the WS line interrupt (main.c, end of the screen): writes
    // the waiting log of deferred VRAM writes (log_commit_pending, C) with
    // DS = ES = SS: the interrupted code may have had DS elsewhere (the
    // OUTI and LDI routines copy with DS = the Z80 source).
    .global log_commit_tramp
log_commit_tramp:
    push ds
    push es
    push ss
    pop ds
    push ss
    pop es
    cld
    IA16_CALL log_commit_pending
    pop es
    pop ds
    IA16_RET

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
