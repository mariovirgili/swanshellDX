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
    mov ax, [z80]
    mov cx, [z80 + 2]
    mov dx, [z80 + 4]
    mov bx, [z80 + 6]
    mov bp, [z80 + 20]
    .byte 0xFF, 0x1E        // call far [drc_entry]
    .word drc_entry

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
