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
#include <nile.h>

    .arch   i186
    .code16
    .intel_syntax noprefix

#ifdef __IA16_CMODEL_IS_FAR_TEXT
    .section .fartext.menu_music_irq, "ax"
#else
    .section .text.menu_music_irq, "ax"
#endif

// Entry of the menu music's timer interrupt.
//
// libnile reads the storage card through the ROM1 window (the SPI receive
// buffer, with DS = 0x3000) and leaves interrupts on meanwhile. The player
// maps the music's banks to ROM0/ROM1: on the cartridge, running it in the
// middle of a card read made the menu read wrong sectors ("Path not found",
// empty folder listings; not seen in the emulator). So while the interrupted
// code is in such a read, the player is not run: the interrupt is asked
// again two lines later.
    .align 2
    .global menu_music_irq_entry
menu_music_irq_entry:
    push ax
    mov ax, ds
    cmp ax, 0x3000
    je 1f
    in ax, WS_CART_EXTBANK_ROM1_PORT
    cmp ax, NILE_SEG_ROM_SPI_RX
    je 1f
    pop ax
    ljmp ss:[menu_music_irq_player]
1:
    mov ax, 2
    out WS_TIMER_HBL_RELOAD_PORT, ax
    mov al, WS_INT_ACK_HBL_TIMER
    out WS_INT_ACK_PORT, al
    pop ax
    iret
