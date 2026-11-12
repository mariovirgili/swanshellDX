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

## Where it is (patch 159)

The CPU is a recompiler (`gb/ws/gb_drc.c`, `gb_drc_asm.s`; `GB-ABI.md`),
there is sound (`gb/ws/gb_sound.c`: what it does and does not, at its
head) and a cartridge with a battery keeps its RAM in a `.sav` file,
through the menu, as the Game Gear's games do. Everything below is from
`gbrun.py` (the plugin in Unicorn, the cycles by Mednafen's table of
V30MZ costs): **nothing of gbswan since the video test has been run on a
WonderSwan or in nileswan-medem**, the sound has not been heard by
anybody, and the menu's part of the saves was only compiled.

Right: the 11 ROMs of Blargg's `cpu_instrs` say "Passed" run by the
plugin (their code is run from RAM, so through the translator). After
713 Game Boy frames with no keys (`gbcmp.py`) the plugin's 160 x 144 is
the PC's pixel for pixel in six games (Balloon Kid, Batman: Return of
the Joker, R-Type, Super Mario Land, Super Mario Land 2, Yoshi); 6
pixels differ in Bubble Ghost (not looked into), 27 in Castlevania II's
moving title. With keys the games go on into play; that is looked at,
not compared.

Fast enough? A Game Boy frame's time is 51,430 V30MZ cycles. Cycles a
Game Boy frame, WonderSwan frames 900-1100, after START and a few keys,
sound included (the screen each was at is in the third column):

| game | patch 158 | patch 159 | |
|---|---|---|---|
| Castlevania II | 469,996 | 27,178 | title: full speed |
| Balloon Kid | 62,238 | 30,062 | first level: full speed |
| Super Mario Land | 40,072 | 30,998 | first level: full speed |
| Bubble Ghost | ? | 33,780 | title: full speed |
| Batman: Return of the Joker | 30,988 | 48,058 | a later screen than in 158: full speed, just |
| Yoshi | 510,361 | 54,636 | game screen, paused: 94% |
| R-Type | 110,343 | 64,609 | first level: 80% |
| Super Mario Land 2 | 895,374 | 69,778 | the map: 74% |

What made the difference: the STAT interrupt's events only where its
signal can rise (one a frame for the vertical blank's, not four a
line); loops that wait told apart from loops that merely jump back to
their start (a fault of 158: Balloon Kid's water was a frame out); P1,
the ROM bank register, the cartridge's RAM and a stack kept in it
(Super Mario Land 2) done by the assembly routines, not C; writes to
the scroll, the palettes and the DMA register that go on in the block;
the sprite table made in assembly (in C it took 8-13 thousand cycles a
frame); the OAM DMA as one copy.

What is still slow, by the profile: R-Type waits for the horizontal
blank before each byte it writes to the video memory (a quarter of its
time is reading STAT); Yoshi's and Super Mario Land 2's time is spread
over the translated code and the memory routines.

START + Y4 (Y left) shows figures in the black at the left, once a
second: FPS (Game Boy frames shown), then hundredths of the time in
translated code and its routines (RUN), the dispatcher and events
(DSP), translating (TRN), the frame's end (VID), waiting for the
frame's time (IDL); blocks translated (BLK, hex), times the code was
forgotten (FLS).

Not done: the 10 sprites to a line, palettes changed inside a frame,
MBC3's clock, ROMs over 1 MiB, cartridge RAM over 32 KiB.
