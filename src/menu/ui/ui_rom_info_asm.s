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

#include <wonderful.h>
#include <ws.h>

    .arch   i186
    .code16
    .intel_syntax noprefix

    .section .text, "ax"

    // uint32_t ui_rom_info_sum_words(uint16_t words);
    //
    // For the checksum of ROM info: adds up the AX words at 1000:0000 (the
    // RAM window). Returns AX = sum of the words, DX = sum of their high
    // bytes (both modulo 65536); the sum of all the bytes is then
    // AX - 255 * DX (modulo 65536).
    .global ui_rom_info_sum_words
ui_rom_info_sum_words:
    push si
    push di
    push bx
    push ds

    mov cx, ax
    mov ax, 0x1000
    mov ds, ax
    xor si, si
    xor di, di          // sum of the words
    xor bx, bx          // sum of the high bytes
    cld

    mov dx, cx
    shr cx, 3           // 8 words per pass
    jz 2f
1:
    .rept 8
    lodsw
    add di, ax
    add bl, ah
    adc bh, 0
    .endr
    loop 1b
2:
    mov cx, dx
    and cx, 7           // the remaining words
    jz 4f
3:
    lodsw
    add di, ax
    add bl, ah
    adc bh, 0
    loop 3b
4:
    mov ax, di
    mov dx, bx

    pop ds
    pop bx
    pop di
    pop si
    IA16_RET
