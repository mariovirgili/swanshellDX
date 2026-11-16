# gbswan: the memory map

gbswan is the Game Boy (DMG) plugin: `make -f Makefile.ggswan GB=1`,
`dist/NILESWAN/PLUG_GB.BIN`, `.gb` in the menu, sources of its own in
`src/ggswan/gb` (`ws/` the plugin, `core/` the Game Boy, `host/` the PC tools) (none of ggswan's is compiled in, and none of gbswan's in
the other four plugins).

This is where things are in the plugin as it is (patch 162), read from
the linked `build/gbswan.elf` and from the sources, and checked by
running the plugin in Unicorn (`host/gbrun.py`). Gio ran the video test
of patches 153-154 in nileswan-medem: the 2 bits a pixel color mode
behaved there as `gbrun.py` draws it. Not run on a WonderSwan by me.

## Why sources and a layout of its own

The WonderSwan Color has a color mode with tiles of 2 bits a pixel
(port 60h = 80h) whose 16 bytes a tile are the Game Boy's, byte for byte:
8 rows of two bytes, the low bits of the 8 pixels and then the high ones,
the leftmost pixel in bit 7. A Game Boy program's patterns can be shown
as they are written.

In that mode the first 512 tiles are the internal RAM's 2000h-3FFFh and
the next 512 are 4000h-5FFFh; sprites can only name the first 512. In the
other plugins that is where the C data and the stack are:

| `build/ggswan.elf` (patch 152) | |
|---|---|
| C data and bss | 0040h-2650h (`__wf_heap_start` = 2651h) |
| stack | down from 4000h (`__wf_heap_top`, the C start-up's `mov sp, 4000h`) |

So gbswan cannot be one more `-D` on those sources.

## The internal RAM

| internal RAM | what | |
|---|---|---|
| 0000h-003Fh | interrupt vectors | |
| 0040h-0FBFh | C data and bss: 3,968 bytes | used: 0040h-05B3h (the Game Boy's registers and what the machine keeps, FatFs's volume and file, the path, the recompiler's and the picture's variables). The Makefile stops the build if `__wf_heap_start` is over 0FC0h |
| 0FC0h-0FFFh | the WS's four sound waves | (the WS wants them below 4000h) |
| 1000h-17FFh | the WS map kept for the Game Boy's map at 9800h, LCDC bit 4 clear | tiles 0-127 are WS tiles 256-383 (the patterns from 9000h). A WS map must be below 8000h |
| 1800h-1FFFh | the same for the map at 9C00h | |
| 2000h-37FFh | the Game Boy's patterns, its 8000h-97FFh | WS tiles 0-383 |
| 3800h-3FFFh | the Game Boy's two maps, its 9800h-9FFFh, as it writes them | (WS tiles 384-511: never named) |
| 4000h-47FFh | WS screen 1's map | black at the sides, clear in the middle |
| 4800h-4FFFh | the WS map kept for the Game Boy's map at 9800h, LCDC bit 4 set | tile n is WS tile n. An entry (in both maps) is rewritten when its byte is; the line's list says which map is shown |
| 5000h-57FFh | the same for the map at 9C00h | |
| 5800h-59FFh | WS sprite table (128 sprites) | |
| 5A00h-5CBFh | WS tiles 928-971: the font of the text before the game | |
| 5FF0h-5FFFh | WS tile 1023: all color 0 | screen 1's only tile |
| 6000h-7FFFh | the Game Boy's work RAM, its C000h-DFFFh | (and the buffer the ROM is read through, before the game) |
| 9E00h-9E9Fh | the Game Boy's OAM, its FE00h | |
| 9F00h-9F7Fh | its I/O registers, as written (FF00h) | P1 as it reads |
| 9F80h-9FFEh, 9FFFh | its high RAM and IE | |
| A000h-A48Fh | the frame's changes of scroll, window, LCDC and BGP, by line | 146 of 8 bytes |
| A500h-B75Fh, B800h-CA5Fh | the two lists the line interrupt works from (shown, being made) | 294 of 16 bytes; an entry whose byte 9 is FFh is a short one: only the line and screen 2's scroll (bytes 0-2) |
| CB00h-CB7Fh | the sprites' order (kept from frame to frame) and the keys being sorted | |
| CB80h-CC2Fh | by a sprite's Y: is it 8 x 16 (LCDC bit 2 at its top line) | |
| CC30h-CFEFh | the patterns kept a frame late (`gb_video.c`): by tile, its copy in the set being made (384 bytes); where each copy's tile is (3 sets of 32 words); by tile, its newest copy waiting (384 bytes) | only while a program is seen to rewrite the patterns of sprites on the screen |
| D000h-D7FFh | stack, 2 KiB | `gb_run` (`gb/ws/gb_asm.s`) sets SP to D800h before the game. Before that (the ROM being read: FatFs) the stack is the C library's, down from 4000h |
| D800h-E7FFh, E800h-F7FFh | where the blocks of the first and of the second ROM area are, by address (512 places of 8 bytes each: `GB-ABI.md`, "Blocks") | all FFh when the blocks are forgotten; the first also when the first area's bank changes |
| F800h-FDFFh | the copies of those tiles: 3 sets of 32, 16 bytes each | |
| FE00h-FFFFh | the WS's palettes | 0: the backdrop, color 0's shade; 1: black (and the figures' white); 4: the background; 12, 13: the sprites |

8000h-9DFFh, where the echo of the work RAM would fall, is where the
translator puts a block together before copying it to its code segment
(patch 174; `gb_drc.c`, `DRC_EBUF`): used only while a block is being
translated, 4 KiB of it at most. 9800h-9BFFh: a bit for each first place
in the blocks' table, set when a block in ROM has it (patch 175;
`gb_drc.c`, `DRC_HAS`). Since patch 176 the code made does not reach
them through the echo's addresses (only a stack put in the echo would). 9D00h-9D9Fh: the OAM as it was before a copy made
while the picture is drawn (patch 179; `gb_video.c`, `OAM_KEPT`).
9000h-97FFh, 9C00h-9CFFh, 9DA0h-9DFFh and CFF0h-CFFFh are free.

## The cartridge

The ROM is in the nileswan's PSRAM, 64 KiB banks from 0 (1 MiB at most
for now: banks 0-15): four of the Game Boy's 16 KiB banks in each. The
Game Boy's 0000h-3FFFh is read through the ROM0 window (segment 2000h)
and its 4000h-7FFFh through the ROM1 window (segment 3000h), each set to
the PSRAM bank the Game Boy's bank is in, at the offset it has there. The
cartridge's RAM (32 KiB at most for now) is the SRAM behind the RAM
window (segment 1000h). It is not saved to a file yet.

### The Game Boy's video memory at 2000h, not at 4000h

The guide this plugin follows put the Game Boy's 8000h-9FFFh at
4000h-5FFFh (WS tiles 512-895) and noted that sprites, which cannot name
those tiles, would need their patterns copied to the first bank. With the
video memory at 2000h-3FFFh instead there is nothing to copy:

- a sprite's pattern n (always at 8000h + n × 16) is WS tile n;
- the background's pattern for the byte n in the map is WS tile n when
  LCDC bit 4 is set (8000h + n × 16), and when it is clear WS tile
  256 + n for n below 128 (9000h on) and n for the others (8800h on):
  `map_entry` in `gb/ws/gb_video.c`.

Both are in the first bank. The second bank (4000h-5FFFh) is left for the
WS's own maps and sprite table, which have to be below 8000h (their base
addresses are 4 and 6 bits). The Game Boy's address and the internal
RAM's differ by 6000h in 8000h-9FFFh and from C000h up (the echo apart).

## A state kept (patch 181)

The PSRAM banks behind segments D000h and E000h of the linear area
(1Dh, 1Eh). D000h: the internal RAM from 0040h, at the same offsets.
E000h: the cartridge's RAM at 0000h-7FFFh, `GBST` at 8000h when a
state has been kept, FatFs's volume at 9000h while a state is brought
back. `gb_main.c`. Internal RAM 9000h-97FFh: the buffer the state's
file is written through (patch 182), only while it is written.

## The recompiler's memory

PSRAM read and run through the linear area (segment n000h is the PSRAM
bank 10h + n here, above the ROM's banks 0-15): the blocks' table in
segment 4000h (8192 places of 8 bytes), translated code in segments
5000h-C000h, each starting with a copy of the routines the code calls.
`GB-ABI.md`. Written through the RAM window (segment 1000h) with the
PSRAM mapped there (port CEh), at the bank ((port C0h << 4) | n) & the
ROM banks' mask: a write to the linear area itself does nothing on the
cartridge (patch 184; `gbd_lin_put` in `gb_drc.c`). While that is done
the cartridge's RAM is not at 1000h. Internal RAM 9000h-97FFh is also
where a slow way kept for later is put together while a block is
translated (`COLD_SCR`), and 9100h on where the hardware test builds its
code.
