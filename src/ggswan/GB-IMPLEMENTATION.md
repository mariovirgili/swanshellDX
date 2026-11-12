# gbswan: how it is made

`PLUG_GB.BIN` (`make -f Makefile.ggswan GB=1`) is the Game Boy (DMG)
plugin; the menu starts it for `.gb` files. Its sources are in
`src/ggswan/gb`, apart from ggswan's:

- `core/`: the Game Boy.
  - `gb_cpu.c`: the SM83 as an interpreter, every bus access a machine
    cycle. One source for two machines.
  - `gb_cart.c`: the header and the mappers (none, MBC1, MBC2, MBC3
    with its clock, MBC5).
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

## Where it is (patch 163)

The CPU is a recompiler (`gb/ws/gb_drc.c`, `gb_drc_asm.s`; `GB-ABI.md`),
there is sound (`gb/ws/gb_sound.c`: what it does and does not, at its
head), a cartridge with a battery keeps its RAM in a `.sav` file,
through the menu, as the Game Gear's games do, and MBC3 has its clock
(`GB-ACCURACY.md`: it counts only while the game runs). Everything
below is from `gbrun.py` (the plugin in Unicorn, the cycles by
Mednafen's table of V30MZ costs): **nothing of gbswan since the video
test has been run on a WonderSwan or in nileswan-medem**, the sound has
not been heard by anybody, and the menu's part of the saves was only
compiled.

Right, in the plugin:
- the 11 ROMs of Blargg's `cpu_instrs` say "Passed" (their code is run
  from RAM, so through the translator);
- 13 of Mooneye's cartridge tests say "Test OK" (MBC1: bits_bank1,
  bits_bank2, bits_mode, bits_ramg, ram_64kb, ram_256kb, rom_512kb,
  rom_1Mb; MBC2: bits_romb, rom_512kb, ram; MBC5: rom_512kb, rom_1Mb);
- rtc3test's basic tests and range tests pass but "Tick" (1004.6 ms
  for 1000: the clock counts by frames);
- with keys, by Game Boy frames on both sides (`GBRUN_GBTIME`), the
  160 x 144 at frame 860 is, pixel for pixel, the same machine's on the
  PC (`gbftest`, its picture of one or two frames before: the plugin
  shows a frame late) in all eight games. The differences the notes of
  patches 159-161 had (Super Mario Land 3 pixels, Bubble Ghost 6, Super
  Mario Land 2 140, R-Type over a thousand) were `gbrun.py`'s, not the
  plugin's: it drew a line with what the line interrupt had already
  changed (on the WonderSwan that shows from the next line, which is
  why the list is set a line early) and with the palettes of the
  frame's end;
- dmg-acid2: `GB-ACCURACY.md` (right but for the eleventh sprite and
  the lines of a window that starts in the middle of the screen).

Fast enough? A Game Boy frame's time is 51,430 V30MZ cycles. Cycles a
Game Boy frame over Game Boy frames 700-860, the same keys at the same
Game Boy frames for every build, sound included:

| game | patch 159 | patch 160 | patch 161 | patch 162 |
|---|---|---|---|---|
| Super Mario Land 2 (choosing a file) | 17,212 | 18,192 | 18,341 | 18,406 |
| Bubble Ghost (title) | 33,117 | 33,228 | 16,602 | 16,616 |
| Balloon Kid | 30,071 | 26,461 | 25,271 | 25,492 |
| Super Mario Land | 32,780 | 32,623 | 32,003 | 32,071 |
| Batman: Return of the Joker | 68,590 | 45,927 | 44,572 | 44,873 |
| Castlevania II (first level) | 72,209 | 64,653 | 45,478 | 45,387 |
| Yoshi | 54,982 | 54,034 | 46,035 | 45,618 |
| R-Type | 69,048 | 58,492 | 50,425 | 50,428 |

**These scenes said too little.** Gio ran patch 161 in nileswan-medem
(the first run of the recompiler anywhere but `gbrun.py`): the
pictures were right, and the games in play were far slower than the
table: Yoshi 8 frames a second, Super Mario Land 2 27-35, Balloon Kid
48-51, Castlevania II 21-38, R-Type 42-49, Batman 42; only Super Mario
Land and Bubble Ghost at full speed. The table's scenes are titles,
menus and the first seconds of a level. Two of his scenes measured
here since (cycles a Game Boy frame, 162 then 163):

| scene | patch 162 | patch 163 |
|---|---|---|
| Yoshi, in play | 349,589 | 53,446 |
| Super Mario Land 2, walking in the first level | 82,174 | 59,132 |

(349,589 is 8.8 frames a second: what he saw.) Yoshi did its sums
with DAA and shifts through HL, which went to the interpreter one
instruction at a time, and all through pointers into the high RAM;
Super Mario Land 2 keeps its variables and its stack in the
cartridge's RAM. Both are still over a frame's time, and Balloon Kid,
Castlevania II, R-Type and Batman in play have not been measured here.

**The table patch 159 came with was not a fair comparison**: its keys
were at WonderSwan frames, so a faster or slower build was measured at
another place in the game. The 159 column above is 159 measured again
with the keys at Game Boy frames. And until patch 161 `gbrun.py` could
count a Game Boy frame twice (stopped for an interrupt at the first
instruction of `gbv_frame`, its hook was called again), which moved
the keys a frame in some runs: the 160 column is 160 measured again
without that (two of its figures moved by less than 1%); the 159
column was not measured again.

Two builds can now be told apart by more than the picture: `gbrun.py`
prints checksums of the Game Boy's memory at the last frame's end.
Between 160 and 161 they are the same in six games; in Castlevania II
and R-Type they are not, as expected (their waits for a STAT mode are
now passed over from the first read, a few cycles' difference in when
the loop ends).

START + Y4 (Y left) shows figures in the black at the left, once a
second: FPS (Game Boy frames shown), then hundredths of the time in
translated code and its routines (RUN), the dispatcher and events
(DSP), translating (TRN), the frame's end (VID), waiting for the
frame's time (IDL); blocks translated (BLK, hex), times the code was
forgotten (FLS).

Not done: the 10 sprites to a line, the sprites' palettes changed
inside a frame, the clock kept between runs, ROMs over 1 MiB,
cartridge RAM over 32 KiB.
