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

## Where it is (patch 167)

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

| scene (keys in `gbrun.py`'s notes below) | 162 | 163 | 165 | 166 |
|---|---|---|---|---|
| Yoshi, in play | 349,589 | 53,446 | 52,490 | 50,548 |
| Super Mario Land 2, walking in the first level | 82,174 | 59,132 | 55,454 | 53,013 |
| Balloon Kid, flying right in stage 1 | | | 62,267 | 58,809 |
| Castlevania II, Crystal Castle: walking, the whip, a jump | | | 63,426 | 60,906 |
| Batman, the first level: walking, a jump, a punch | | | 63,630 | 61,353 |
| R-Type, the first level: moving and firing | | | 54,691 | 53,042 |

(349,589 is 8.8 frames a second: what he saw.) Yoshi did its sums
with DAA and shifts through HL, which went to the interpreter one
instruction at a time, and all through pointers into the high RAM;
Super Mario Land 2 keeps its variables and its stack in the
cartridge's RAM. **Only Yoshi is under a frame's time**; the others
are at 84 to 97% of full speed, by this count. Where their time goes
(patch 165's profiles): Castlevania II, 13% in the lookup of returns
(about 175 a frame, small routines that change the ROM bank);
Batman and Balloon Kid, a fifth to a quarter translating the blocks
those scenes keep meeting (some 9,000 cycles a block in 165, about
6,000 now); R-Type, 17% waiting for the horizontal blank.

The keys, by Game Boy frame (`frame:keys` as `ws_keypad_scan`'s bits),
and the frames measured:
- Castlevania II `240:200,256:0,360:200,376:0,480:200,496:0,1050:200,1066:0,1900:20,2020:820,2030:20,2080:420,2100:20`, 2000-2160;
- Batman `240:200,256:0,360:200,376:0,480:200,496:0,600:200,616:0,960:20,1040:420,1060:20,1100:820,1110:20`, 1000-1160;
- R-Type `240:200,256:0,360:200,376:0,480:400,496:0,560:200,576:0,640:20,700:420,760:410,820:440,880:420,940:410,1000:440`, 860-1020;
- Yoshi `240:200,256:0,360:200,376:0,480:400,496:0`, 600-720;
- Super Mario Land 2 `240:200,256:0,400:40,460:0,700:400,716:0,900:400,916:0,1100:400,1116:0,1300:20`, 1320-1440;
- Balloon Kid `240:200,256:0,360:200,376:0,480:400,496:0,560:200,576:0,640:20,700:420,720:20,760:420,780:20,820:420,840:20,900:420,920:20,980:420,1000:20`, 900-1060.

**Patch 167: another set.** Gio ran 166 in nileswan-medem: Super Mario
Land, Bubble Ghost, R-Type, Yoshi and Balloon Kid at 52-60 frames a
second, so they are no longer measured; Super Mario Land 2 58 falling
to 43 with enemies about and 38 on the map, Batman 45-60, Castlevania
II 21 at its title and 28-51 in play. And five more ROMs of his:
Donkey Kong Land 8-11 frames a second, Race Drivin' 13-14, X 27-28,
ZAS 44, R-Type II 48-58 with its status bar wrong at times. The set
is now these eight, in play (`gbplay2.sh` in the session's notes; the
keys below). Cycles a Game Boy frame, 51,430 being full speed; "before"
is patch 166 for the first three and patch 167's work as it stood
before the five were looked at for the others:

| scene | before | 167 |
|---|---|---|
| Castlevania II, Crystal Castle | 60,906 | 58,256 |
| Batman, the first level | 61,353 | 58,734 |
| Super Mario Land 2, the first level | 53,013 | 51,606 |
| R-Type II, the first level | 45,756 | 35,563 |
| ZAS, in play | 67,393 | 67,813 |
| Donkey Kong Land, the first level | 143,178 | 90,499 |
| Race Drivin', driving | 151,752 | 142,393 |
| X, in the tunnel | 111,375 | 114,516 |

(Castlevania II, Batman, R-Type II and ZAS are of the build before the
last change, to the writes of the cartridge's registers; Super Mario
Land 2 and Donkey Kong Land, measured again after it, moved by 64 and
62 cycles.) **Only R-Type II is under a frame's time**, Super Mario
Land 2 at it. **X is 3% slower than before**: see below. What was found:
- R-Type II, Donkey Kong Land and Race Drivin' change code they keep
  in RAM, and every change was a block translated again: 5,214 blocks
  in R-Type II's scene (601 now), the table and the code's room
  forgotten once or twice a scene in the other two. Each form is now
  kept (`GB-ABI.md`, "Blocks"); Donkey Kong Land's, which are made
  anew for every use (`LD HL, nn / CALL`, `LD SP, nn / JP` with the
  operands written just before), go to the interpreter after six.
- Donkey Kong Land forgot everything with 1,247 blocks in a table of
  8,192: a block's eight places were the hash's and the seven after
  it, and the blocks of one routine have neighbouring hashes. The
  places are now spread (twelve of them).
- Race Drivin' writes the cartridge's registers all the time with
  what is already there: the windows are now set again only when
  something has changed (148,656 to 142,393), and only the RAM's part
  when only that has. X's writes do change something every time
  (which register is not known yet), so it pays for the comparison and
  saves nothing: 110,909 before the change, 114,516 after. Its path
  through C for those writes (a seventh of its time) is the next
  thing to look at.
- What is left in Donkey Kong Land (a quarter still translating and
  looking blocks up), Race Drivin' and X (a third in the translated
  code itself: they draw their pictures by program; the real Game Boy
  shows a few of them a second), and ZAS has not been looked at.

The keys: R-Type II `240:200,256:0,400:200,416:0,1150:200,1166:0,1300:200,1316:0,1500:420`,
1500-1660; ZAS `240:200,256:0,400:200,416:0,560:400,576:0,800:420`,
900-1060; Donkey Kong Land `240:200,256:0,400:200,416:0,560:400,576:0,720:200,736:0,880:400,896:0,1000:20,1150:420,1170:20`,
1040-1200; Race Drivin' `240:200,256:0,400:200,416:0,560:400,576:0,720:200,736:0,880:400,896:0,1000:420`,
1100-1260; X: START at 240, then A every 160 frames from 400 to 2160,
2300-2460.

In play the plugin's picture is no longer the PC machine's pixel for
pixel at a given frame, as it is in the old eight scenes: an interrupt
is taken at a block's end, a few cycles late, and a game that reads
the keys or the time can come to be a frame apart. The end pictures
were looked at beside `gbftest`'s (R-Type II's, Race Drivin's and X's
are one of its frames to the pixel; the others show the same scene a
little further on or back).

He also saw, in 166, Super Mario Land's whole picture jump sideways
for an instant now and then. Its status bar is a STAT interrupt at a
line (LYC) that sets the scroll; when a timer interrupt came a few
cycles before it, the block running went on past the line's cycle,
the timer's event set the next event from the time *now*, and the
line's interrupt, already behind, was never raised: a frame drawn
with the status bar's scroll all the way down. Events that are due
are now done before the next is looked for (`gbf_hw.c`: `stat_done`),
and before an I/O register is written. With his keys the scroll at
each frame goes up evenly over 2,400 frames. R-Type II's status bar
is probably the same fault (it is right in every picture here, but
it was not seen wrong here before either).

He also saw Super Mario Land's scroll move in jerks. That was a fault
in how a frame's list reaches the screen, put right in patch 164
(`gb_video.c`: gbv_frame, gbv_wait): `GBRUN_SCROLL=1` prints the scroll
shown at each WonderSwan frame, which now goes up a step at a time,
one frame in four or five shown twice (that much is the two rates:
59.7 pictures on a screen of 75.5 a second).

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
