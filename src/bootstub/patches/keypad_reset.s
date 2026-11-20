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

/* START + Y1 in a game: back to the menu (SwanShellDX).
 *
 * Games built with Bandai's library read the buttons with
 *     mov al, 40h / out 0B5h, al / ... / nop / in al, 0B5h
 * or with a delay loop (push cx / mov cx, n / loop / pop cx) before the IN. The
 * last NOP and the IN (or the loop) become "call near keypad_reset_hook",
 * and the hook (copied into free space of the same 64 KB bank) does the IN for the
 * game. If START is down, it also reads the Y group: with Y1 down too it
 * resets the cartridge into the menu, like nile_soft_reset().
 *
 * The game is patched only when its button read is found in one of these
 * forms once or twice in the last 16 banks (the 1 MB the code runs from),
 * and only where its bank has free space for the hook; otherwise it is
 * left as it is. Only the copy in PSRAM changes, never the file. (Written
 * in assembly: the bootstub's stack is just below 0x2000, right after it.) */

#include <wonderful.h>
#include <ws.h>

    .arch   i186
    .code16
    .intel_syntax noprefix

    /* === the hook, copied into the game: position independent === */
    .section .text.s.keypad_reset_hook, "ax"
    .global keypad_reset_hook
    .global keypad_reset_hook_end
keypad_reset_hook:
    pushf
    in al, 0xB5             // the game's read: START, A, B (group 40h)
    test al, 0x02           // START?
    jnz 1f
    popf
    ret
1:
    push ax
    push cx
    mov al, 0x10            // the Y group
    out 0xB5, al
    mov cx, 24
2:  loop 2b
    in al, 0xB5
    mov ah, al
    mov al, 0x40            // back to the group the game chose
    out 0xB5, al
    mov cx, 24
3:  loop 3b
    test ah, 0x01           // Y1?
    jnz 4f
    pop cx
    pop ax
    popf
    ret
4:
    // as nile_soft_reset(): the flash was kept awake by the bootstub
    cli
    xor ax, ax
    out 0xB7, al            // no NMI
    mov al, 0xDD            // unlock the nileswan registers
    out 0xE2, al
    xor ax, ax
    out 0x00, ax            // hide the display
    dec ax
    push ax                 // 0xFFFF:0x0000, read by the far jump below
    inc ax
    push ax
    dec ax
    // code on the stack (works without prefetch, as in nileswan-medem):
    // out 0C0h, al / out 0E4h, ax / nop / jmp far 0FFFFh:0000h
    push 0xEA90
    push 0xE4E7
    push 0xC0E6
    mov cx, sp
    push ss
    push cx
    retf
keypad_reset_hook_end:

    /* === search and patch, run by the bootstub (ES = RAM window) === */
    .section .text, "ax"

#define HOOK_SIZE (keypad_reset_hook_end - keypad_reset_hook)

    // bool patch_apply_keypad_reset(uint16_t total_banks)
    // ax = banks of the ROM in PSRAM; returns ax = 1 if patched
    .global patch_apply_keypad_reset
patch_apply_keypad_reset:
    pusha
    push es
    mov bp, sp
    mov word ptr [bp + 16], 0   // the result, in the saved AX
    push 0x1000
    pop es
    mov bx, ax
    mov al, WS_CART_BANK_FLASH_ENABLE
    out WS_CART_BANK_FLASH_PORT, al
    mov dx, bx                  // dx = bank: the last 16
    sub dx, 16
    jnc 1f
    xor dx, dx
1:
    xor cx, cx                  // cx = reads found (bank, offset on the stack)
2:
    cmp dx, bx
    jae 5f
    mov ax, dx
    out WS_CART_EXTBANK_RAM_PORT, ax
    mov ax, 8
3:
    call keypad_reset_find
    test ax, ax
    jz 4f
    cmp cx, 2
    jae 9f                      // more than two: not the classic routine
    push dx
    push ax
    inc cx
    cmp ax, 0xFFF0
    jae 4f
    add ax, 2
    jmp 3b
4:
    inc dx
    jmp 2b
5:
    jcxz 9f
6:
    pop si                      // the IN
    pop ax                      // its bank
    out WS_CART_EXTBANK_RAM_PORT, ax
    push cx
    mov ax, HOOK_SIZE
    call keypad_reset_free
    test ax, ax
    jz 8f                       // no room: this read stays as it is
    mov dx, ax                  // dx = the hook
    mov di, ax
    push si
    mov si, offset keypad_reset_hook
    mov cx, HOOK_SIZE
    cld
    rep movsb
    pop si
    lea di, [si - 1]
    cmp byte ptr es:[di], 0x90
    je 7f
    // "loop $ / pop cx / in al, 0B5h" -> "pop cx / call near hook / nop":
    // no more loop: the hook waits itself
    lea di, [si - 3]
    mov byte ptr es:[di], 0x59
    inc di
    mov byte ptr es:[di + 3], 0x90
7:
    // the last NOP and "in al, 0B5h" (or the above) -> "call near hook"
    mov byte ptr es:[di], 0xE8
    mov ax, dx
    sub ax, di
    sub ax, 3
    mov es:[di + 1], ax
    mov word ptr [bp + 16], 1
8:
    pop cx
    loop 6b
9:
    mov sp, bp
    pop es
    popa
    IA16_RET

    // ax = offset to start from (>= 8)
    // returns ax = offset of the "in al, 0B5h" of the next button read in
    // the mapped bank, 0 if none. A button read is
    //     mov al, 40h / out 0B5h, al / (up to 96 bytes without another
    //     OUT to 0B5h) / in al, 0B5h
    // where the IN follows a NOP, or the end of a delay loop
    //     push cx / mov cx, n / loop $ (or a loop over one or two NOPs:
    //     nothing else, as the loop goes) / pop cx
    // (the caller tells them apart by the byte before the IN).
    .global keypad_reset_find
keypad_reset_find:
    push cx
    push dx
    push si
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
    repne scasb             // mov al, ...
    jne 8f
    cmp word ptr es:[di], 0xE640
    jne 1b
    cmp byte ptr es:[di + 2], 0xB5
    jne 1b
    lea si, [di + 3]        // after "out 0B5h, al"
    mov cx, 96
2:
    cmp byte ptr es:[si], 0xB5
    jne 3f
    mov al, es:[si - 1]
    cmp al, 0xE4
    je 4f                   // in al, 0B5h
    cmp al, 0xE6
    je 1b                   // another OUT first: not this form
    cmp al, 0xE7
    je 1b
3:
    inc si
    loop 2b
    jmp 1b
4:
    lea dx, [si - 1]        // dx = the IN
    mov si, dx
    mov al, es:[si - 1]
    cmp al, 0x90
    je 5f
    cmp al, 0x59            // pop cx
    jne 1b
    cmp byte ptr es:[si - 3], 0xE2  // loop ...
    jne 1b
    mov al, es:[si - 2]
    cmp al, 0xFE            // loop $
    je 5f
    cmp al, 0xFD            // loop over one NOP
    jne 6f
    cmp byte ptr es:[si - 4], 0x90
    je 5f
    jmp 1b
6:
    cmp al, 0xFC            // loop over two NOPs
    jne 1b
    cmp word ptr es:[si - 5], 0x9090
    jne 1b
5:
    mov ax, dx
    jmp 9f
8:
    xor ax, ax
9:
    pop es
    pop di
    pop si
    pop dx
    pop cx
    IA16_RET

    // ax = bytes needed
    // returns ax = offset of a free place in the mapped bank, 0 if none:
    // the last run of at least 256 FFh bytes (and 128 more than needed)
    // that reaches 0xFF00 or is at least 1 KB long, with the hook 64 bytes
    // before its end. The padding at the end of the bank's code is the
    // likeliest to be really unused; the first run found used to be inside
    // graphics or tables, and the games crashed.
    .global keypad_reset_free
keypad_reset_free:
    push bx
    push cx
    push dx
    push si
    push di
    push es
    push ax                 // the size, for the end
    push 0x1000
    pop es
    cld
    mov bx, ax
    add bx, 128             // run length wanted: 64 before, 64 after
    cmp bx, 256
    jae 0f
    mov bx, 256
0:
    xor si, si              // si = end of the last run long enough
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
    jbe 7f
    repe scasb              // its end
    jne 2f
    inc di                  // ran to the limit: all FFh
2:
    dec di                  // di = first byte after the run
    mov ax, di
    sub ax, dx
    cmp ax, bx
    jb 1b
    cmp di, 0xFF00          // padding up to the end of the bank
    je 3f
    cmp ax, 1024            // elsewhere only a long run: shorter ones were
    jb 1b                   // blank tiles or tables (Tetsuman, Anchorz Field)
3:
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
