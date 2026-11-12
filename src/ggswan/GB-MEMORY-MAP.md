# gbswan: the memory map

gbswan is the Game Boy (DMG) plugin: `make -f Makefile.ggswan GB=1`,
`dist/NILESWAN/PLUG_GB.BIN`, `.gb` in the menu, sources of its own in
`src/ggswan/gb` (`ws/` the plugin, `core/` the Game Boy, `host/` the PC tools) (none of ggswan's is compiled in, and none of gbswan's in
the other four plugins).

**Step GB0 (patch 153): there is no Game Boy yet.** The plugin shows a
test of the video the rest is planned on. What is below under "now" is
read from the linked `build/gbswan.elf` and checked by running it in
Unicorn (`host/gbrun.py`); what is under "planned" is a plan. Gio ran patch 153's
test in nileswan-medem: all as expected but the way back to the menu,
which patch 154 changes. Not run on a WonderSwan yet.

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

## Now (GB0)

| internal RAM | what | |
|---|---|---|
| 0000h-003Fh | interrupt vectors | |
| 0040h-1BFFh | C data and bss: 7,104 bytes | used now: 2 bytes (0040h-0041h). The Makefile stops the build if `__wf_heap_start` is over 1C00h |
| 1C00h-1FFFh | stack, 1 KiB | `gb_run` (`gb/ws/gb_start.s`) sets SP to 2000h before `gb_main`. Lowest SP seen in `gbrun.py`: 1FD6h |
| 2000h-37FFh | the Game Boy's patterns, its 8000h-97FFh | WS tiles 0-383 |
| 3800h-3FFFh | the Game Boy's two maps, its 9800h-9FFFh, as it writes them | (WS tiles 384-511: never named) |
| 4000h-47FFh | WS screen 1 | the shade of the background's color 0; black at the sides |
| 4800h-4FFFh | WS screen 2 | the Game Boy's background (later the window too) |
| 5000h-51FFh | WS sprite table (128 sprites) | |
| 5200h-5FEFh | free | |
| 5FF0h-5FFFh | WS tile 1023: all color 0 | screen 1's only tile |
| 6000h-FDFFh | free | |
| FE00h-FFFFh | the WS's palettes | 0: screen 1's shade; 1: black; 4: the background; 12, 13: the sprites |

The plugin's code and constants are in the cartridge's segment (1,566
bytes now), as in the other plugins.

### The Game Boy's video memory at 2000h, not at 4000h

The guide this plugin follows put the Game Boy's 8000h-9FFFh at
4000h-5FFFh (WS tiles 512-895) and noted that sprites, which cannot name
those tiles, would need their patterns copied to the first bank. With the
video memory at 2000h-3FFFh instead there is nothing to copy:

- a sprite's pattern n (always at 8000h + n × 16) is WS tile n;
- the background's pattern for the byte n in the map is WS tile n when
  LCDC bit 4 is set (8000h + n × 16), and when it is clear WS tile
  256 + n for n below 128 (9000h on) and n for the others (8800h on):
  `BG_PATTERN` in `gb/ws/gb_main.c`.

Both are in the first bank. The second bank (4000h-5FFFh) is left for the
WS's own two screens and sprite table, which have to be below 8000h
(their base addresses are 4 and 6 bits). The Game Boy's address and the
internal RAM's differ by 6000h in all of 8000h-9FFFh.

### What the test shows

`gb_main` writes six patterns at the Game Boy's 8010h-806Fh and one at
9010h, as a Game Boy program would, fills the background with them and
puts six sprites of pattern 6 on it. Expected (the picture is
`gbrun.py`'s):

- black at the two sides, 32 columns each, and 160 × 144 in the middle;
- rows of stripes in the four shades, of each shade, and of a small
  checker; the last two rows are the checker of 9010h (the background's
  number 1 with LCDC bit 4 clear), the first two the stripes of 8010h
  (number 1 with it set);
- two rows where the background is color 0: screen 1's shade (white)
  shows through, and the "F" sprite on them is whole;
- on the stripes, from the left: an "F" cut in half by the left edge of
  the 160 columns, a whole "F" over the background, an "F" behind the
  background (seen only on the stripes of color 0, in the reversed
  palette), an "F" turned both ways, an "F" cut by the right edge;
- X1-X4 scroll the background (the WS's own scroll: the Game Boy's SCX
  and SCY), and nothing outside the 160 × 144 may move or show;
- A turns the four shades of the background and of screen 1 by one (the
  Game Boy's BGP, with no tile written); the black sides stay black;
- START + Y1 (Y up) held for a second: back to the menu.

`host/gbrun.py build/gbswan.elf frames out.ppm [keys]` runs the device
build from its entry point and draws this.

## Planned (not done, not measured)

| internal RAM | what |
|---|---|
| 0040h-1BFFh | C data: 7 KiB is all there is. Tables go in the cartridge's segment (`__far`) or above 6000h, by address |
| 5200h-53FFh | a second sprite table, to change tables at a line |
| 5400h-5FEFh | the sound's wave table and what else must be below 8000h |
| 6000h-7FFFh | the Game Boy's work RAM, its C000h-DFFFh: again 6000h less, so one subtraction serves 8000h-9FFFh and C000h-DFFFh |
| 9E00h-9FFFh | OAM, the I/O registers' copies and the high RAM (FE00h-FFFFh), with the same 6000h |
| 8000h-9DFFh, A000h-FDFFh | free: 31 KiB, for the recompiler's tables |

The cartridge's ROM and RAM stay in the nileswan's PSRAM and SRAM,
through the windows, as in ggswan. The registers and the calling rules
of the recompiled code will be in `GB-ABI.md` when there is one (step
GB7); this table's rows from 6000h up are to be settled then.
