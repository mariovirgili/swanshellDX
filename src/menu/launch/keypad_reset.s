/**
 * swanshell is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * swanshell is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with swanshell. If not, see <https://www.gnu.org/licenses/>.
 */

/* SwanShellDX, START + Y1 back to the menu: the two byte scans of the
 * automatic patch (launch_patch.c, keypad_auto_patch()), on the PSRAM bank
 * mapped in the RAM window (segment 1000h, flash mode on). A scan of up to
 * 16 banks in C would take seconds; REPNE SCASB takes a fraction. The same
 * steps, in Python: y1p.py. */

#include <wonderful.h>
#include <ws.h>

    .arch   i186
    .code16
    .intel_syntax noprefix

    .section .fartext.s.keypad_reset, "ax"

    // uint16_t keypad_reset_find(uint16_t from)
    // ax = offset (>= 8): the next "mov al, 40h / out 0B5h, al"
    // (B0 40 E6 B5) at or after it, below FF00h; ax = 0 if none
    .global keypad_reset_find
keypad_reset_find:
    push cx
    push di
    push es
    push 0x1000
    pop es
    cld
    mov di, ax
1:
    mov cx, 0xFF00
    sub cx, di
    jbe 8f
    mov al, 0xB0
    repne scasb
    jne 8f
    cmp word ptr es:[di], 0xE640
    jne 1b
    cmp byte ptr es:[di + 2], 0xB5
    jne 1b
    lea ax, [di - 1]
    jmp 9f
8:
    xor ax, ax
9:
    pop es
    pop di
    pop cx
    IA16_RET

    // uint16_t keypad_reset_free(uint16_t size)
    // ax = bytes needed: the place of the hook, 64 bytes before the end of
    // the last run of FFh below FF00h that is at least max(size + 128, 256)
    // long and reaches FF00h or is 1 KB or more; ax = 0 if none
    .global keypad_reset_free
keypad_reset_free:
    push bx
    push cx
    push dx
    push si
    push di
    push es
    push ax
    push 0x1000
    pop es
    cld
    mov bx, ax
    add bx, 128
    cmp bx, 256
    jae 0f
    mov bx, 256
0:
    xor si, si              // si = end of the last run that qualifies
    xor di, di
1:
    mov cx, 0xFF00
    sub cx, di
    jbe 7f
    mov al, 0xFF
    repne scasb             // the start of a run
    jne 7f
    mov dx, di
    dec dx                  // dx = first FFh
    mov cx, 0xFF00
    sub cx, di
    jbe 6f
    repe scasb              // its end
    jne 2f
    inc di                  // ran to the limit: all FFh
2:
    dec di                  // di = first byte after the run
    jmp 3f
6:
    mov di, 0xFF00          // a single FFh right at the limit
3:
    mov ax, di
    sub ax, dx
    cmp ax, bx
    jb 1b
    cmp di, 0xFF00
    je 4f
    cmp ax, 1024
    jb 1b
4:
    mov si, di
    jmp 1b
7:
    pop ax
    test si, si
    jz 8f
    xchg ax, si             // ax = end, si = size
    sub ax, si
    sub ax, 64
    jmp 9f
8:
    xor ax, ax
9:
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    IA16_RET
