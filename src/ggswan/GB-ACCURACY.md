# gbswan: how faithful, measured

The Game Boy in `gb/core` is the reference: an interpreter that moves the
timer, the serial port, the OAM DMA and the LCD by one machine cycle
before each access to the bus. It is run on the PC by `gb/host/gbtest`
and checked with test ROMs by `gb/host/gbsuite.py` (the ROMs are not in
the repository: game-boy-test-roms v7.0, which collects Blargg's,
Mooneye's and dmg-acid2). What runs on the WonderSwan will be compared
with it.

The machine: a DMG (the boot ROM's revisions A, B, C), from the state its
boot ROM leaves at 0100h (the boot ROM itself is not run and not needed;
the logo it leaves in the video memory is not there).

## Results (patch 156)

| tests | passed |
|---|---|
| Blargg `cpu_instrs` (all 11) | yes |
| Blargg `instr_timing` | yes |
| Blargg `mem_timing`, `mem_timing-2` | yes |
| Blargg `halt_bug` | yes (its picture read, then by hash) |
| Blargg `interrupt_time` | not run: the ROM is for the Game Boy Color only |
| Mooneye acceptance, the tests for the DMG: bits 3, instr 1, interrupts 1, oam_dma 3, serial 1, timer 13, and the 32 in the directory itself (the boot state, the instructions' and interrupts' timing, HALT, EI, DI) | 54 of 54 |
| Mooneye acceptance/ppu (the LCD's timing as the CPU sees it: the modes, the interrupts, LY, LYC, the LCD turned on, the sprites' cost) | 12 of 12 |
| The older Mooneye LCD tests in wilbertpol's fork (`acceptance/gpu`, those for the DMG; most were checked by their author on the Game Boy Pocket or Color, not on a DMG) | 33 of 47 |
| Mooneye emulator-only: mbc1 | 12 of 13 (not `multicart_rom_8Mb`: the cartridges with several games are not told apart) |
| Mooneye emulator-only: mbc2, mbc5 | 7 of 7, 8 of 8 |
| dmg-acid2 | yes (the same picture as its reference) |

What the LCD tests settled, as this model counts (dots from the line's
start, the CPU seeing them every 4):

- STAT shows mode 2 from dot 4, mode 3 from dot 84, mode 0 from
  256 + SCX's low 3 bits + what the sprites add (6 each, and once for
  each background tile a sprite starts in what is left of that tile less
  2; the sum in whole cycles);
- the interrupts of modes 2 and 0 are asked 4 dots before STAT shows the
  mode; the vertical blank's at dot 4 of line 144, where mode 2's is
  asked once more;
- LY reads 0 from dot 4 of line 153; LYC is compared with nothing in a
  line's first 4 dots, in line 153 with 153 for 4 dots, then with 0;
- the line after the LCD is turned on is 4 dots shorter and has no
  mode 2;
- the video memory cannot be read from dot 80 and written from dot 84;
  the OAM cannot be read from the line's start, nor written from dot 4,
  but can be written at dot 80;
- with the LCD off LYC's comparison stays as it was.

The 14 older tests not passed measure single dots with the scroll's fine
part and the sprites, or expect the vertical blank's interrupt a cycle
away from where the tests checked on a DMG put it.

## Known differences

- The picture is drawn a line at a time, at the start of the line's
  drawing: what a program changes in the middle of a line is seen from
  the next line.
- While the OAM DMA runs only the reads of the OAM are refused (FFh); the
  rest of the bus is as usual.
- STOP: the CPU waits for a key; DIV is cleared.
- MBC3's clock (`gb_cart.c`, the registers' behaviour as Peanut-GB has
  it) counts the Game Boy's time a frame at a time, and only while the
  program runs: nothing keeps it from one run to the next. Of rtc3test
  (keys by hand, the pictures looked at) the basic tests and the range
  tests pass but "Tick" (987.8 ms in the reference, 1004.6 ms in the plugin's
  machine and in the plugin, for 1000: the frame), and none of the
  sub-second writes do. No MBC1 cartridges with several
  games, no camera, rumble, HuC or MMM01.
- The echo of the work RAM (E000h-FDFFh) is the work RAM for every
  read and write through a pointer or an address (since patch 176: from
  174 the code made took it for memory of the translator's). A stack
  there is not supported: PUSH, POP, CALL and RET with SP in
  E000h-FDFFh do not reach the work RAM. No program known to do it.
- The sprites are one table a frame. It is the OAM at the frame's end,
  or, when the program copied the OAM over (DMA) while the picture was
  being drawn, the OAM as it was before that (patch 179): sprites
  changed in the middle of a picture are not seen changed below that
  line in the same picture.
- The serial port has no other end: a transfer with the Game Boy's own
  clock ends after 8 bits with FFh received, one with the other's clock
  never does.
- No sound in the reference (the registers keep what is written). The
  plugin's sound is `gb/ws/gb_sound.c`: once a frame, from the registers.

## The plugin's picture and dmg-acid2

The plugin does not draw: it sets the WonderSwan's hardware up
(`gb/ws/gb_video.c`). dmg-acid2 in the plugin under `gbrun.py` (patch
162; 686 of the 23,040 pixels are not the reference's):

- right: the footer and the eyes' lower halves (LCDC bit 4 changed in
  the middle of the picture: two WS maps for each of the Game Boy's),
  the mouth (8 x 16 sprites on some lines only: the size is the one at
  the sprite's top line), the hair's lines (the background off), the
  nose (the flips), the moles (who covers whom);
- wrong, and known: the "!" is covered (an eleventh sprite on its
  lines, which the plugin shows); the left half of the right eye is a
  white square and there are dots and a grey bar at the far left on the
  eyes' and the chin's lines. Those are the lines of a window that
  starts in the middle of the screen: the window is then screen 1, under
  the background, with nothing to cut it at its edge (what is to its
  left shows where the background has color 0), and a sprite "behind
  the background" is over all of it.

The WonderSwan shows the patterns as they are in the memory at the
moment, and a frame's picture only when the frame is over. A program
that writes the next pose of a figure into patterns its sprites have
just left is, here, writing into what is still on the screen (Donkey
Kong Land's Kong was broken up in two pictures out of three while he
moved). Since patch 172, when the last pattern a frame wrote is one of
a sprite on the screen - or, since 177, of a sprite the program has
set by the frame's end: Kong's next picture is drawn in tiles not
shown, and his sprites then moved to them - the writes to the patterns
are kept, until the screen is turned off, and reach the tiles with the picture of the frame after
the one they were made in (`gb_video.c`). Meanwhile: the program
reading such a pattern back gets the old bytes; 32 tiles a frame at
most are kept (more: all are written at once and it stops, as with
the LCD off); the background's tiles written the same way come a
frame late with the rest, as on the Game Boy.
