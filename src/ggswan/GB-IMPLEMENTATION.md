# gbswan: how it is made

`PLUG_GB.BIN` (`make -f Makefile.ggswan GB=1`) is the Game Boy (DMG)
plugin; the menu starts it for `.gb` files. Its sources are in
`src/ggswan/gb`, apart from ggswan's:

- `core/`: the Game Boy.
  - `gb_cpu.c`: the SM83 as an interpreter, every bus access a machine
    cycle. One source for two machines.
  - `gb_cart.c`: the header and the mappers (none, MBC1, MBC2, MBC3
    without its clock, MBC5).
  - `gb_bus.c`, `gb_ppu.c`: **the reference**, for the PC only. The rest
    of the hardware is moved by one machine cycle before each bus
    access; the LCD's timing is the one the test ROMs measure
    (`GB-ACCURACY.md`); the picture is drawn a line at a time.
  - `gbf_hw.c`: **the plugin's machine**. The same registers and rules,
    but by events: time is a count of machine cycles from the frame's
    start (17556 a frame, 114 a line), the CPU takes cycles off a
    counter, and when it reaches zero something is due (the vertical
    blank, a line the STAT interrupt is asked at, the timer's overflow).
    LY, STAT, DIV and TIMA are worked out when read. The picture is a
    backend's (`gbv.h`).
- `ws/`: the plugin. `gb_main.c` reads the ROM into PSRAM and maps the
  cartridge's windows; `gb_video.c` is the picture with the WonderSwan's
  hardware; `gb_asm.s` the stack's move and the interrupts.
- `host/`: PC tools. `gbtest` runs the reference, `gbftest` the plugin's
  machine with the reference's line renderer as its backend
  (`gbv_host.c`: the lines not drawn yet are drawn before anything they
  are made of changes); `gbsuite.py` runs the test ROMs with either.

`src/ggswan/host/gbrun.py` runs the plugin itself (`build/gbswan.elf`) in
Unicorn with a ROM, the WonderSwan's interrupts and its 2 bits a pixel
color mode drawn line by line; `gbcmp.py` puts its picture next to
`gbftest`'s for the same number of Game Boy frames.

## The picture on the WonderSwan

See the head of `gb/ws/gb_video.c` and `GB-MEMORY-MAP.md`. In short: the
WonderSwan Color's 2 bits a pixel color mode has the Game Boy's tile
format, so the Game Boy's video memory is the WonderSwan's first tile
bank and nothing is converted; each of the Game Boy's two maps has a
WonderSwan map next to it; screen 2 (seen inside 160 x 144) is the
background and the window, switched at the right lines by the line
interrupt, with the scroll the program gave each line; the sprites are
the WonderSwan's.

## Where it is (patch 160)

The CPU is a recompiler (`gb/ws/gb_drc.c`, `gb_drc_asm.s`; `GB-ABI.md`),
there is sound (`gb/ws/gb_sound.c`: what it does and does not, at its
head) and a cartridge with a battery keeps its RAM in a `.sav` file,
through the menu, as the Game Gear's games do. Everything below is from
`gbrun.py` (the plugin in Unicorn, the cycles by Mednafen's table of
V30MZ costs): **nothing of gbswan since the video test has been run on a
WonderSwan or in nileswan-medem**, the sound has not been heard by
anybody, and the menu's part of the saves was only compiled.

Right, in the plugin:
- the 11 ROMs of Blargg's `cpu_instrs` say "Passed" (their code is run
  from RAM, so through the translator);
- 13 of Mooneye's cartridge tests say "Test OK" (MBC1: bits_bank1,
  bits_bank2, bits_mode, bits_ramg, ram_64kb, ram_256kb, rom_512kb,
  rom_1Mb; MBC2: bits_romb, rom_512kb, ram; MBC5: rom_512kb, rom_1Mb);
- after 713 Game Boy frames with no keys (`gbcmp.py`) the 160 x 144 is
  the PC's pixel for pixel in six games; 6 pixels differ in Bubble
  Ghost (not looked into), Castlevania II's title is moving;
- with keys, by Game Boy frames on both sides (`GBRUN_GBTIME`), at
  frame 840: Balloon Kid, Castlevania II and Yoshi the PC's pixel for
  pixel in play; Super Mario Land 3 pixels, Bubble Ghost 6, Super Mario
  Land 2 140; Batman in the middle of a fade and R-Type with its stars
  a frame apart (what the plugin shows is a frame behind for the scroll
  and the palette and not for the tiles and sprites: looked at, taken
  for that, not proved).

Fast enough? A Game Boy frame's time is 51,430 V30MZ cycles. Cycles a
Game Boy frame over Game Boy frames 700-860, the same keys at the same
Game Boy frames for every build, sound included:

| game | patch 159 | patch 160 | |
|---|---|---|---|
| Super Mario Land 2 (choosing a file) | 17,212 | 18,192 | full speed |
| Balloon Kid | 30,071 | 26,461 | full speed |
| Super Mario Land | 32,780 | 32,623 | full speed |
| Bubble Ghost (title) | 33,117 | 33,250 | full speed |
| Batman: Return of the Joker | 68,590 | 45,927 | full speed (was 75%) |
| Yoshi | 54,982 | 53,694 | 96% |
| R-Type | 69,048 | 58,492 | 88% (was 74%) |
| Castlevania II (first level) | 72,209 | 64,653 | 80% (was 71%) |

**The table patch 159 came with was not a fair comparison**: its keys
were at WonderSwan frames, so a faster or slower build was measured at
another place in the game, and one build against another meant
nothing. The 159 column above is 159 measured again this way; by it
Batman, R-Type and Castlevania II were slower in play than 159's table
said.

What is still slow, by the profile: R-Type and Castlevania II wait for
the horizontal blank before each byte they write to the video memory
(a tenth to a fifth of their time is that wait, already one routine
call a wait); Yoshi's time is in the translated code of a loop that
reads the keys all frame long.

START + Y4 (Y left) shows figures in the black at the left, once a
second: FPS (Game Boy frames shown), then hundredths of the time in
translated code and its routines (RUN), the dispatcher and events
(DSP), translating (TRN), the frame's end (VID), waiting for the
frame's time (IDL); blocks translated (BLK, hex), times the code was
forgotten (FLS).

Not done: the 10 sprites to a line, the sprites' palettes changed
inside a frame, MBC3's clock, ROMs over 1 MiB, cartridge RAM over
32 KiB.
