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

## Where it is (patch 177)

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

**Patch 168.** Gio ran 167 in nileswan-medem: Super Mario Land's
scroll is right; Batman 48-59 frames a second, Castlevania II 46-49,
R-Type II 58-59, Donkey Kong Land 32-42 (it was 8-11) but slow in
Kong's roll and with Kong's picture broken at times, Super Mario Land
2 50 in the level, 39-41 with a Koopa or its shell about and 37 on
the map, ZAS 45-55 in play and 14 at its waving logo (a scroll change
every line: the video's part, 29% there), X 25-28 as before, Race
Drivin' 11-20 with half its time between blocks and translating. And
two pictures wrong: **R-Type II's status bar still jumping** (so it
was not Super Mario Land's fault, as guessed below), and R-Type's top
row of tiles wrong at one point of its first level.

R-Type II's bar was the waits. It waits for the horizontal blank
before every byte it writes to the video memory (`PUSH AF / LDH A,
(41h) / AND 3 / JR Z / LDH A, (41h) / AND 3 / JR NZ`); in the vertical
blank the second loop lasts to the next frame's first line. The plugin
took that wait in one step and only then the vertical blank's
interrupt, ten lines late and with its routine still to run, so every
dozen frames the game had not set LYC for its bar in time. Found with
`GBRUN_WATCH=9F43` (now with the frame's cycle and line of each
write): the write of SCX at line 128 was missing in frames 1523 and
1535; with the STAT waits off it was there in all 60; `gbftest`'s bar
never moves. No wait is now passed over beyond the next event
(`GB-ABI.md`, "Time"): 60 of 60. That costs a little everywhere
(more turns through the dispatcher): Castlevania II 58,256 to 58,583,
Batman 58,734 to 58,986, ZAS 67,813 to 68,549, X 114,516 to 116,665,
Race Drivin' 142,393 to 143,685; R-Type II 35,596, Super Mario Land 2
51,693. Donkey Kong Land 90,561 to 89,045: its ever-changing code in
RAM now goes straight to the interpreter once it is known for that.

Not found: R-Type's top row (the run by keys here dies long before
that place; a build without the copy loops gives the same memory over
5,000 frames, so it is not those; it may be the waits too, R-Type
writes its video memory the same way). Donkey Kong Land's Kong: in
`gbrun.py`'s end picture the only pixels unlike `gbftest`'s are in
Kong's box. The game draws Kong into the tiles' memory, which the
WonderSwan shows as it is being written, at whatever moment its own
frame falls: probably that, not looked into further.

**Patch 169.** Gio on 168: the bar's top right, its bottom still
flickering, "all to the side as before"; R-Type II 48 frames a second
in a heavy scene. And a plan of speed work to try on the slow games
only (Donkey Kong Land, X, Race Drivin', ZAS).

The bar's second cause was RETI. The game sets LYC = 80h for its bar
from the STAT interrupt of line 144, which waits behind the vertical
blank's; the Game Boy takes it right after that one's RETI, at line
150. RETI went on to its return address by the return cache, with no
look at what was waiting, and the plugin took it at the next block's
end that looked: every hundred frames or so, at the next frame's line
0, after the game had needed it (`GBRUN_ENTER`, and `gbftest` made to
print its writes of LYC and SCX beside the plugin's). RETI now looks
(`IE & IF & 1Fh`) and leaves through the dispatcher when something
waits. Over 1,100 frames with keys the scroll at line 128 is the bar's
in all but four, which are the two blank screens after a death. First
tried with RETI always through the dispatcher: Batman 3.9% slower, so
the look is in the block. A jump of a block in RAM to its own start is
also no longer linked through the table (it could have run another
form of the block unverified): not the bar's cause, kept.

Speed, cycles a Game Boy frame, 168 to 169 (same scenes and keys;
memory and pictures the same as the build before each step, where a
step does not change the time the game sees):

| scene | 168 | 169 | |
|---|---|---|---|
| ZAS, logo (frames 330-385) | 159,543 | 72,387 | -55% |
| Race Drivin', driving | 143,685 | 109,938 | -23% |
| X, tunnel | 116,665 | 92,239 | -21% |
| Donkey Kong Land | 89,045 | 85,404 | -4% |
| Castlevania II | 58,583 | 57,178 | -2% |
| R-Type II | 35,596 | 34,777 | -2% |
| Super Mario Land 2 | 51,693 | 51,181 | -1% |
| ZAS, play | 68,549 | 68,038 | -1% |
| Batman | 58,986 | 58,893 | 0 |

- ZAS's logo changes SCX on every line. A line of the list that
  differs from the one before only by its scroll is now a short entry
  (byte 9 = FFh: `GB-MEMORY-MAP.md`) for which the line interrupt
  writes two ports and returns (159,543 to 112,806), and a write of
  SCY, SCX, BGP, WY or WX is a routine of the recompiler's, `s_vreg`,
  not a call of the C one (to 72,285).
- Race Drivin's inner loop is code in RAM that the game changes as it
  goes (the `n` of an `LD D, n`, the `e` of a `JR` into a row of
  `LD (HL+), A`): six forms, then the interpreter. A block in RAM whose
  changed bytes are all operands the translator can read at run time
  (`LD r, n`, `LD rr, nn`, `JP nn`, `JR e`, `CALL nn`) is now
  translated once more with those read from RAM, and compared without
  them (`GB-ABI.md`): 143,670 to 137,487, Donkey Kong Land 89,222 to
  84,903.
- X is an MBC2 cartridge and changes its ROM bank 74 times a frame;
  the quick bank write left MBC2 out, so each went through C: 115,552
  to 91,918.
- Race Drivin' writes the "RAM on" register 68 times a frame, nearly
  always the 0 already there: such a write now returns at once:
  137,487 to 109,938.

Of the plan, not done: counters in the plugin, the translator's own
speed (what Donkey Kong Land still translates in the scene measured is
ROM code met for the first time, some 200 blocks, 11 in RAM), the
return cache's misses, the OAM. Not measured: Super Mario Land 2's map
and its Koopa scenes (no run by keys gets there yet). Not looked at
again: Kong's picture, R-Type's top row (Gio: it seems right in 168).
All of it only in `gbrun.py`; `gbreg` 24 of 24, the eight older scenes
the same memory as 168's and the same pictures.

**Patch 170.** More of the same search, on Race Drivin', X and Donkey
Kong Land only. What was found first is what makes Race Drivin' as
slow as Gio saw it (11-20 frames a second, "FLS 1", translating up to
45% of the time): **it has more code than there is room for**. At
frame 1,254 of the run here its 1,737 blocks, 276 bytes each, had
filled seven of the eight segments and half the last; a little later
all is forgotten and the 800 blocks it is using are translated again,
five seconds at one or two frames a second (the same five seconds as
at the start of the race, when they are translated the first time: a
block costs some 12,000 cycles, 880 a Game Boy instruction, spread
evenly over `scan_block` and `translate_block`). Frames 1,500-1,700,
with that in them: 163,303 cycles each; without: 95,789.

Kept:
- **A block ends where another already starts**, from its sixth
  instruction on (`GB-ABI.md`, "Blocks"). Race Drivin' enters its rows
  of unrolled instructions at many places, and each entry was a block
  of everything from there on: 34,779 bytes of Game Boy code
  translated for 22,102 there are. Its code at frame 1,254: 460,000
  bytes to 357,000 (from every first instruction: 338,000, but X came
  out 1% slower; from the sixth, no slower). Donkey Kong Land's scene, where a fifth
  of the time was translating code met for the first time: 83,148 to
  78,192.
- **The compare of code in RAM** was the C library's `_fmemcmp` and
  then, for a block with operands read as it runs, the plugin's own
  compare: 7 to 10% of Race Drivin's time while driving. Now only the
  second, which does both.
- **`op_flags`** from a table: a tenth of the translator's time.
- **A pointer whose high byte the block sets** (`GB-ABI.md`,
  "Memory"): under 1% of anything, 3% of the code's room.

Tried and left out:
- the tiles' patterns written in line, without `s_wrt`: X 92,239 to
  88,985, but 16 bytes more at every write, and Race Drivin' ran out
  of room inside the scene measured (185,966);
- a "compact" translation once the room has run out (every access to
  the memory a call of `s_rd` / `s_wrt`, 5 to 11 bytes for 40 to 54):
  after it Race Drivin' ran 17% slower than with the usual code.

Cycles a Game Boy frame, 169 to 170: Donkey Kong Land 85,404 to
78,192, Race Drivin' 109,938 to 104,873, X 92,239 to 92,045, Batman
58,893 to 58,191, Castlevania II 57,178 to 56,998, Super Mario Land 2
51,181 to 50,250, R-Type II 34,777 to 34,061, ZAS 68,038 to 68,203
(its logo 72,387 to 72,242). Race Drivin' and X are not the same
frames as before to the byte: an event is seen at a block's end, and
the blocks end elsewhere now (their work RAM's sum differs; X's end
picture is the same, Race Drivin's differs in one tile, 30 pixels).
The whole runs, in WonderSwan frames: Race Drivin' 2,869 to 2,729,
Donkey Kong Land 2,210 to 2,113, X 6,351 to 6,319. The other six
scenes and the eight older ones: the same memory as 169's; `gbreg` 24
of 24; R-Type II's bar as in 169 (1,100 frames, the four after a
death apart); ZAS's logo the same picture.

Seen and not done:
- Race Drivin' still runs out of room, later. The pause at the start
  of the race and after each forgetting is the translator's speed.
- X's title, 9 frames a second (343,316 cycles a frame): an LYC
  interrupt on every line, its routine writing SCX and the next LYC.
  Some 2,300 cycles a line, no part of it large: the event (`event`,
  `stat_sig` twice, `schedule`), the interrupt, the write of LYC
  through three C functions and `stat_sig` and `schedule` again.
- The dispatcher does not put a block it finds into the two small
  tables: a return to a block that is not at its first place in the
  big one goes through C every time.

**Patch 171.** Gio ran 170 in nileswan-medem ("PAT 0170" on its
screen) and wrote: Batman slows down a great deal when the water
effect is on (39-42 frames a second, the "DSP" figure 18-25%);
Castlevania II always a little slow (41-48); Donkey Kong Land 42, and
Kong's sprite still breaks up; R-Type and R-Type II all right; Race
Drivin' always slow (25-26, DSP 28%, translating 17%); Super Mario
Land "seems to skip a frame every three" (58 frames a second, idle
47%); Super Mario Land 2 slow on the map (37) and when new enemies
come (39-45); X slow in its fades, its title 8 frames a second (DSP
41%), 31-34 in the game; ZAS's logo much faster, the game slow.

Batman's water, X's title and fades and (less) Castlevania II are one
thing: **leaving a block for the dispatcher**. A game that has an
interrupt on every line or two leaves that way hundreds of times a
frame, and each leaving was all registers stored, a far return to C,
`gbd_run_frame`'s loop, the event, the interrupt by `gb_cpu_interrupt`
(two calls of `gbf_write`), the big table searched from C, all
registers loaded. Done:

- **`s_exit`** (`gb_drc_asm.s`; `GB-ABI.md`, "Interrupts"): the event
  that is due (C, called with four registers saved), the interrupt
  (the address pushed where the stack is), HALT's wait and the jump to
  the next block by the tables, in the code segment. The dispatcher
  is left what is rare.
- **HALT translated**, so that its wait is `s_exit`'s too.
- **A write to a sound register through a pointer** (`LD (HL), A`
  with HL at FF10h-FF3Fh: Batman's and Castlevania II's music
  routines, 9 to 16 a frame) is a byte moved and a bit set in
  `gbf_trig`, as `LDH (n), A` already was; SCY, SCX, BGP, OBP0, OBP1,
  WY, WX written that way with the value they have: nothing. The
  other I/O registers go to `gbf_io_write` at once, not by `gbf_write`.
- **The dispatcher fills the two small tables** with the block it
  goes to (see "Seen and not done" above).
- **`gbf_hw.c`, STAT on LYC alone** (no mode interrupt enabled: X's
  title, most status bars): `stat_sig` answers from the line and LYC
  without working out the mode; `schedule` divides only when a mode's
  interrupt is enabled; the event at LYC's line raises the interrupt
  by where it is, not by two calls of `stat_sig`; a write to LYC works
  out the position once. The PC model (`gbftest`) is the same code:
  its pictures of the sixteen scenes are the same as before.

A fault of my own on the way, never in a patch: the event called
from `s_exit` could end the frame, and the dispatcher's loop, which
only looked at the count, went on with the frame marked done: every
leaving then went to C and the pictures came late (found with
`GBRUN_SLOW=from-to`, which prints why a block left for the
dispatcher). The loop now stops at the frame's end.

X's title (frames 170-200 with no key): 343,316 cycles a Game Boy
frame in 170; 312,673 with `s_exit` and the trims of `stat_sig` and
`schedule`; 268,497 with HALT translated and the fault above put
right; 232,860 with the event and the write of LYC by position. The
same picture. What is left there is mostly `schedule` (18%, twice a
line).

Cycles a Game Boy frame, 170 to 171 (full speed is 51,430):

| scene | 170 | 171 | |
|---|---|---|---|
| Batman | 58,191 | 51,752 | -11.1% |
| Castlevania II | 56,998 | 51,351 | -9.9% |
| X, the tunnel | 92,045 | 86,054 | -6.5% |
| R-Type II | 34,061 | 32,008 | -6.0% |
| ZAS | 68,203 | 65,308 | -4.2% |
| Donkey Kong Land | 78,192 | 74,851 | -4.3% |
| Super Mario Land 2 | 50,250 | 48,196 | -4.1% |
| Race Drivin' | 104,873 | 102,161 | -2.6% |
| X, the title | 343,316 | 232,860 | -32.2% |
| ZAS, the logo | 72,242 | 71,599 | -0.9% |

The eight older scenes (cycles a WonderSwan frame, lower is less
work): Balloon Kid 22,401 to 20,848, Batman 36,377 to 33,611, Bubble
Ghost 16,028 to 15,125, Castlevania II 38,362 to 36,316, R-Type
44,050 to 42,622, Super Mario Land 27,083 to 24,228, Super Mario Land
2 12,260 to 11,515, Yoshi 40,462 to 40,625; all the same memory as
170's and the same pictures against `gbftest`. `gbreg` 24 of 24. Of
the eight newer scenes seven have the same memory as 170's to the
byte; Batman's differs (HALT's wait ends at the event now, not a
block later) and its picture is the reference's a frame apart, as
before (901 pixels at one frame, the scroll). R-Type II's bar as in
170 (the four frames after a death apart); ZAS's logo the same
picture. Batman's water scene itself was not measured: no run by
keys gets there.

Super Mario Land "skipping a frame every three": not found as a
fault. With `GBRUN_CYCLES=1` (without it `gbrun.py` runs 80
instructions a line and the pace is not the console's) the scroll
goes up one a Game Boy frame and the frames reach the screen four in
every five WonderSwan frames, evenly (`GBRUN_PACE=from-to` prints
it): that is 59.7 on 75.5, and it will be so on the console. In
nileswan-medem the 75.5 are then shown on a monitor at 60, and the
two beats can fall well or badly. To be looked at on a WonderSwan.

Not touched: Kong's sprite; Race Drivin's pauses (the translator's
speed); Super Mario Land 2's map and ZAS in the game (94% of the
time in translated code: only better code would help).

**Patch 172.** Gio on 171 (nileswan-medem, "PAT 0171"): Batman still
slows down when the water rises (29 frames a second, 81% of the time
in translated code and its routines, the dispatcher 2%), the rest of
it fine; X's fade still slow (12 frames a second, 92%), the game "not
satisfying yet" (35); Castlevania II slows down a little over the
water. The water is at the start of the first level in both.

**Kong.** Donkey Kong Land keeps two sets of tiles for Kong (232-243
and 244-255, 38-51 and 52-65). It writes his next pose into the set
the sprites are not using, a tile a line in the horizontal blanks from
line 130 and the rest in the vertical blank, over a frame or two, and
then moves the sprites to it: on a Game Boy nothing of this is ever
seen. Here the picture of frame N (its list and its sprites) is on
the WonderSwan's screen while frame N + 1 runs, and the tiles are
shown as they are: the set frame N's sprites use is the one frame
N + 1 is already writing into. `gbrun.py` can now tell
(`GBRUN_TRUTH=1`: each picture drawn again with the patterns as the
Game Boy had them at line 0 of the frame it shows): of 186 pictures
in a row with Kong walking, rolling and jumping, 121 had him wrong.
(The reference's pictures are no use for this: the plugin's game is
not at the same pose at the same frame as `gbftest`'s.)

The WonderSwan has no room for a second copy of the patterns (the
tiles of its 2 bits a pixel mode are exactly the Game Boy's video
memory), so the writes are kept back instead (`gb_video.c`, "the
patterns a frame late"; `GB-ACCURACY.md`): by tile, in sets by the
frame they were made in (line 0 to line 0), a set copied to the tiles
by the vertical blank interrupt that first shows the picture of the
frame after. Three sets of 32 tiles (Kong: 24 a frame at most). It is
on for 120 frames after a frame in which the last pattern written
since line 0 was one of a sprite on the screen: with every pattern
write of any frame counted it was on in Batman, R-Type II, ZAS and
Race Drivin' too, which load their sprites' patterns in the vertical
blank and need none of it (Batman 3% slower, Race Drivin' 2%). Now it
is on in Donkey Kong Land alone of the sixteen scenes. With it: 0 of
the 186 pictures wrong.

What it costs: the copying loops leave the patterns alone while it is
on and a pointer whose high byte the block sets no longer writes the
patterns unchecked (`GB-ABI.md`), so every pattern byte goes through
`s_wrt`; a copy to make for each tile and to put back. Donkey Kong
Land's scene: 74,851 cycles a frame to 78,869 (the routines for the
copies were first in C: 81,637).

**An interrupt at a line.** Batman's water is LYC written again and
again by the interrupt's routine (lines 128, 132, 136, 140, 143 where
the run here stands; the part where it rises was not reached: by keys
alone Batman stops at a pipe), Castlevania II's the same with other
lines, X's title and fades an interrupt at every line. X's fade
(frames 590-640 of the run with the keys
`240:200,256:0,400:200,416:0,560:400,576:0`): 234,546 cycles a frame,
of them `schedule` 14.5%, the write of LYC through `gbd_write` and
`gbf_io_write` 8%. Done: `schedule()` in assembly (`gb_asm.s`,
`gbf_sched_asm`, the same sums in the same order; C calls it, and
keeps the case of a serial transfer), and LYC written in `s_wrt`
(`wrt_lyc`) when no interrupt is asked for at a mode. The fade:
208,785, the same memory; the title (frames 170-200): 232,860 to
202,664, the same picture. What is left there is a little of
everything: the routine's own code, `s_wrt` (the fade is a dissolve:
each pattern byte read, masked and written back after a wait for the
horizontal blank), the WonderSwan's own line interrupt (a change of
scroll at every line), `event` in C.

Cycles a Game Boy frame, 171 to 172: Castlevania II 51,351 to 50,913,
Batman 51,752 to 51,612, R-Type II 32,008 to 31,723, Super Mario Land
2 48,196 to 48,394, ZAS 65,308 to 65,426, Race Drivin' 102,161 to
102,821, X 86,054 to 86,824 (the patterns' writes all through `s_wrt`
now), Donkey Kong Land 74,851 to 78,869. The eight scenes' memory is
171's to the byte but for Donkey Kong Land's video memory (patterns
waiting); their end pictures against `gbftest` as before. The whole
regression (`gbreg` 24 of 24, the eight older scenes the same memory
and pictures, R-Type II's bar, ZAS's logo the same picture and 71,691
for 71,599) was run on the build before the last change, the one that
made the test "since line 0"; after it, the eight scenes and the 186
pictures again.

Not done: Batman's and Castlevania II's water were not measured (not
reached); X in the game; Race Drivin's pauses; Super Mario Land 2's
map; ZAS in the game.

**Patch 173.** Asked by Gio after 172: which hot code is still C and
could be assembly; then to do the first two and to profile the
translator by its commonest calls (Batman, Race Drivin', X, Donkey
Kong Land).

- **The event** (`gbf_hw.c`, `event()`; 8.5% of X's title, with
  `timer_sync`). In the middle of a frame it is the timer and LYC's
  interrupt and nothing else: that much is `gbf_event_asm`
  (`gb_asm.s`), which ends in `gbf_sched_asm`; the vertical blank, the
  frame's end, an interrupt at a mode and a serial transfer stay in C
  (the routine answers 0 and has done nothing). `s_exit` calls it
  before `gbf_host_event`. `gbf_sched_asm` has a short way of its own
  for the LCD on, LYC's interrupt the only one of STAT's, the timer
  off.
- **Code in RAM** (`gbd_run_frame`, `find`, `gbd_ram_cmp`: a tenth of
  Donkey Kong Land's scene). `s_lookup` sent every address from 8000h
  up to the dispatcher. `s_lookup_ram` (`GB-ABI.md`, "Interrupts")
  now finds the first form of a block in the work RAM or the high RAM
  and compares its bytes itself; the other forms, a block not at its
  first place, a change in the bytes: the dispatcher, as before.

A fault of my own on the way, never in a patch: the label I put in
`s_exit_event` for "the event is done" was a `1:`, and the `jnz 1f` of
the halted case just above, which meant the `1:` further down, went to
mine: four words popped that were never pushed. The regression caught
it (Blargg's 02-interrupts failed, Batman's run stopped). A name now;
and no more numbers for labels put into a routine that has its own.

Cycles a Game Boy frame, 172 to 173 (full speed is 51,430), the same
memory in every one:

| scene | 172 | 173 | |
|---|---|---|---|
| X, the title | 202,664 | 178,085 | -12.1% |
| X, the fade | 208,785 | 188,889 | -9.5% |
| R-Type II | 31,723 | 29,652 | -6.5% |
| Donkey Kong Land | 78,869 | 73,897 | -6.3% |
| Batman | 51,612 | 49,933 | -3.3% |
| Race Drivin' | 102,821 | 100,014 | -2.7% |
| Castlevania II | 50,913 | 50,110 | -1.6% |
| Super Mario Land 2 | 48,394 | 47,817 | -1.2% |
| ZAS | 65,426 | 64,713 | -1.1% |
| X, the tunnel | 86,824 | 86,123 | -0.8% |

`gbreg` 24 of 24; the eight older scenes the same memory and the same
pictures against `gbftest` (cycles a WonderSwan frame: Batman 33,745
to 33,211, Castlevania II 36,217 to 35,762, R-Type 42,785 to 42,088,
Super Mario Land 2 11,618 to 11,128, Bubble Ghost 15,208 to 15,318);
R-Type II's bar and ZAS's logo as before. The title's end picture is
one dot of its dissolve away from 172's: the picture is taken at a
WonderSwan frame, and the program is further on by then.

**The translator, profiled** (`GBRUN_CALLS=1`, new: every function by
its own cycles and the times it is entered; `GBRUN_FUNC` for the
instructions of one). Race Drivin's start (frames 754-772: 684 blocks,
10,550 Game Boy instructions, 696,375 cycles a frame): `translate_block`
30.9% (5,682 cycles a block, its own), `scan_block` 30.1% (5,517),
`find` 4.0% (7,693 calls of 65), `exit_to` 2.0%, `_fmemcpy` 2.2%,
nothing else over 1.6%. Donkey Kong Land (frames 900-1100, 326
blocks): 9.2% and 8.8%, `find` 2.0%. Inside the two functions no
instruction is over 1.8% of its function and the forty heaviest are
30% of it: some 130 V30MZ instructions for each Game Boy instruction
in each of the two passes (360 cycles), all of them run once. There is
no call to convert; what can be seen to be saved is small (the
seventeen compares `interp_op` is compiled to, a byte read again for
"after EI", the chains of tests on the opcode in `scan_block`): a
tenth of the translator's time perhaps. More than that is another
translator (tables by opcode, assembly). Not done here. X's and
Batman's figures were not taken: the frames chosen for X translated
nothing, and Batman's run was the one the fault above stopped.

**Patch 174.** Gio: the small things in the translator first, the
rewrite after. Four windows (Batman's
first level loading, frames 885-925; Race Drivin's start, 754-772;
Donkey Kong Land 900-1100; X 240-600), each with `GBRUN_CALLS=1`.

What I had guessed from 173's profile was wrong: that most of
`translate_block` was the bytes going out one at a time through a far
pointer (`E()`: the segment register loaded for every byte). Putting
the block together in the console's own RAM (`DRC_EBUF`, 8000h, the
room the work RAM's echo would have) and copying it to the code
segment once took 8% off the function, not half (and 4 KiB off the
plugin). So, lines and not guesses: `GBRUN_FUNCDUMP=file` writes a
function's cycles by instruction, and `addr2line` on the plugin linked
with `gb_drc.c`'s object compiled with `-g` (the same code) gives the
source line of each. Three fifths of `translate_block`'s own time was
the dozen lines before each instruction's `switch`: the three bytes
read (13%), which of BC, DE, HL the access is through (9%), the
opcode's fields (6%), `interp_op` (a switch of seventeen compares),
the cycles from a far table, the wait's masks.

Done, all of it leaving the code made as it was:
- `op_inf`: by opcode, the flags, the length, the cycles and "the
  interpreter's" in two bytes, near (the three tables were far: a
  segment register each time). `scan_block` reads it once for an
  instruction and keeps the second byte (`i_inf`) for
  `translate_block`, which then reads no table.
- the instruction after EI known from the instruction before, not by
  reading its byte again; the wait's masks looked for only in a block
  that has a wait; the five copying loops compared only when the block
  starts with one of their three first opcodes.
- `E()` and `EW()` into the near buffer, one `_fmemcpy` a block.

Tried and taken out: leaving the tests on what is known of BC, DE, HL
aside for the opcodes they cannot be about (40h-BFh with nothing
known): nothing at Race Drivin's start, where something is nearly
always known.

Cycles a block, their own (173 to 174), and a Game Boy frame of the
window:

| window | `scan_block` | `translate_block` | cycles a frame |
|---|---|---|---|
| Race Drivin's start | 5,517 to 5,121 | 5,682 to 4,548 | 696,375 to 628,159 (-9.8%) |
| Batman, level loading | 5,378 to 5,005 | 4,784 to 3,771 | 139,728 to 130,696 (-6.5%) |
| Donkey Kong Land | 4,351 to 4,034 | 4,530 to 3,622 | 80,644 to 78,184 (-3.1%) |
| X | 3,924 to 3,590 | 4,193 to 3,286 | 83,549 to 82,161 (-1.7%) |

The same code? The table of blocks is the same to the byte in all
four (every block at the same place with the same length) and so is
the Game Boy's memory; the code segments differ only where a C
variable's address is written into the code (the variables moved:
checked on X, every differing run one or two bytes long). The scenes
(173 to 174; the translator is 5 to 12% of them): Donkey Kong Land
73,897 to 72,329, Race Drivin' 100,014 to 99,125, R-Type II 29,652 to
29,257, Batman 49,933 to 49,528, Castlevania II 50,110 to 49,804, ZAS
64,713 to 64,404, Super Mario Land 2 47,817 to 47,606, X 86,123 to
86,072; the same memory in each. `gbreg` 24 of 24, the eight older
scenes the same memory and pictures, R-Type II's bar, ZAS's logo the
same picture.

What is left in it: `scan_block` is now the larger half (its test for
a block already at the next instruction, the tables it clears for
each instruction, the chains of tests on what is known of BC, DE,
HL); `find`, eleven calls a block, 4%.

**Patch 175.** Gio: go on by yourself, `scan_block` first, then the
routine that writes and the code made for Super Mario Land 2's map
and for ZAS.

*The translator.* `scan_block` by line again (`fprof.sh` here: the
tree's function by source line at Race Drivin's start): its test for
a block already at the next instruction, and `find` behind it, were a
fifth of it. `DRC_HAS` (9800h, 1 KiB): a bit for each first place in
the table, set when a block in ROM is made, cleared with the table;
`find` is called only where the bit is set: 7,692 calls to 1,508 in
the window, 628,159 cycles a frame to 609,648, the same blocks. The
rest of `scan_block` is fifteen to twenty-five cycles a line for each
instruction, on every line: nothing more to take there without
writing it again.

*Super Mario Land 2's map* (frames 540-700 with the keys
`240:200,256:0,400:40,460:0`: 84,014 cycles a frame, the 37 frames a
second of Gio's screen). A third of it was `s_wrt`. `GBRUN_WRT=1`:
160 writes a frame to 3000h, the ROM bank, and 266 to the cartridge's
RAM through a pointer. The block at 3E34h is a routine that copies a
byte from one bank to the cartridge's RAM: bank set, byte read, byte
written, bank set back, eighty times a frame. Done: `LD (nn), A` with
nn the bank's register (the translator knows by the cartridge's kind)
calls `s_bankx`, which is `wrt_bank` from where the address has been
looked at, and from a block in the first ROM area `s_bank0`, which
has no address after the call and returns at once; the cartridge's
RAM is the second thing `s_wrt` looks for above 9800h. 74,163, the
same memory. What is left of it: the bank's change itself (some forty
instructions and the port, 8,500 cycles a frame), the writes to the
cartridge's RAM through a pointer (the block cannot know where HL
points).

*ZAS in the game* (63,895 with the profile on): its three hottest
blocks work an address out with `ADD HL, HL` five times and `ADD HL,
BC`. Each was seven V30MZ instructions - A and the flags aside, the
low bytes added, the high with carry, the flags taken, H and C kept,
Z put back - for an H and a C that the next `ADD HL` sets again.
`i_live` (the flags needed before each instruction) was there and
used only to decide whether the V30MZ's flags needed saving: an `ADD
HL, rr` whose H and C are not needed before they are next set is now
`add bx, rr`. 64,404 to 60,174, the same memory.

*`s_wrt`.* A pattern is the first thing looked for (two compares),
then the work RAM and above, the cartridge's RAM, the maps. With the
patterns first and the maps left where they were, Yoshi, which writes
maps, was 2.8% slower (40,208 cycles a WonderSwan frame to 41,343):
the maps now go straight to their code (40,384).

Cycles a Game Boy frame, 174 to 175, the same memory in every scene:

| scene | 174 | 175 | |
|---|---|---|---|
| Super Mario Land 2, the map | 84,014 | 74,163 | -11.7% |
| ZAS | 64,404 | 60,174 | -6.6% |
| Race Drivin' | 99,125 | 93,065 | -6.1% |
| X, the tunnel | 86,072 | 82,479 | -4.2% |
| Race Drivin's start (translating) | 628,159 | 603,032 | -4.0% |
| Donkey Kong Land | 72,329 | 69,631 | -3.7% |
| Batman | 49,528 | 48,029 | -3.0% |
| Castlevania II | 49,804 | 48,841 | -1.9% |
| Super Mario Land 2, the level | 47,606 | 46,761 | -1.8% |
| R-Type II | 29,257 | 28,855 | -1.4% |

The whole regression (`gbreg` 24 of 24, the sixteen scenes the same
memory, pictures, R-Type II's bar, ZAS's logo) on the build before
the maps' change in `s_wrt`; after it `gbreg`, the eight older scenes
and four of the newer again. The map's and the start's figures, ZAS's
and three more of the table are of that earlier build.

Looked at and left: PUSH AF and POP AF (6.5% of ZAS between them: F
built from AH and back, then the general push): a table would save a
dozen cycles of sixty.

**A second opinion.** Gio had the sources read by another model
(GPT-6). What it found: the code made treats C000h-FDFFh as one piece
of memory at `[address - 6000h]`, so the echo of the work RAM,
E000h-FDFFh, is not the work RAM but 8000h-9DFFh of the console's -
where since 174 and 175 the translator has its buffer and `DRC_HAS`.
Known as a thing not done, now also a way for a
program to write into the translator's own memory; to be put right by
making the block's own range end at DFFFh (the routines do the echo).
What it proposes, in its order: a routine of its own for a write the
block knows is to the video memory (`i_cls` 2), with no address after
the call and the flags saved only if needed; INC and DEC of BC, DE,
HL as the V30MZ's own when the flags are in AH or dead (BC and DE are
five bytes now), and `ADD HL, rr` with only C needed; the translator
by tables - each instruction decoded once and kept, fifteen to
twenty-five families of opcodes, the place written to in a register -
for under 200 cycles an instruction; code met for the first time
interpreted, translated the second time (Race Drivin'); INC and DEC
(HL) in place where the part of the memory is known; a guess of the
target at the RETs that have one. Against: a table by page at every
access, a second stack of return addresses, longer blocks, the
patterns written in line (tried for 170: Race Drivin's code no longer
fitted).

**Patch 176.** The echo, and the small ones of the second opinion.

*The echo.* `range_check` ends the range in line at DFFFh (`cmp bh,
0E0h`; `cmp si, 0E000h`), and a pointer the block loads is "in the
work RAM" (`i_cls` 1) only with its high byte up to DFh. E000h-FDFFh
goes to `s_rd` and `s_wrt`, which have always done the echo. It costs
nothing: the compare was there with another number. The stack's own
path (PUSH, POP, CALL, RET in line) is left as it was: a limit above
would send to the routines every game whose stack is in the high RAM,
and they are many; a stack in the echo is not supported
(`GB-ACCURACY.md`).

*Writes known to be to the video memory* (`i_cls` 2: the block loaded
the pointer and it is in 8000h-9FFFh). They called `s_wrt` with the
flags saved, the registers put back in their places and the next
instruction's address after the call. `s_wrv` (`ST_WRV`): the value
in `drc_tmp`, the address in SI, nothing after the call; a pattern
with the late patterns off is written there and it returns; anything
else (a map, a late pattern) takes two off its return address and
falls into `s_wrt`, which returns two bytes on, where the address of
the next instruction would have ended: that address is used only for
a write to the cartridge's registers or to an I/O register, which
this is not. Eleven bytes
a write for thirty-seven.

*INC and DEC of BC, DE, HL* change no flag on the Game Boy and all
but C on the V30MZ. They were `lea` for HL and `mov si, cx / lea /
mov cx, si` for the others. When the flags are in AH already, or
nothing reads them before they are set again, they are `inc cx`, one
byte.

*INC and DEC (HL)* with HL known to be in the work RAM: `inc byte ptr
[bx - 6000h]` (with the carry kept as the general one keeps it), not
read, changed, written.

*`ADD HL, rr` with only C read after it*: `add bx, rr / lahf / and
ah, 1`.

Cycles a Game Boy frame, 175 to 176, the same memory in every scene:

| scene | 175 | 176 | |
|---|---|---|---|
| Batman | 48,029 | 47,670 | -0.7% |
| ZAS | 60,174 | 59,780 | -0.7% |
| Super Mario Land 2, the map | 74,163 | 73,857 | -0.4% |
| Race Drivin' | 93,065 | 92,759 | -0.3% |
| X, the tunnel | 82,479 | 82,360 | -0.1% |
| Castlevania II | 48,841 | 48,813 | -0.1% |
| Super Mario Land 2 | 46,761 | 46,725 | -0.1% |
| R-Type II | 28,855 | 28,834 | -0.1% |
| Donkey Kong Land | 69,631 | 69,641 | 0.0% |
| Race Drivin's start (translating) | 603,032 | 605,626 | +0.4% |

X's title 175,675 and its fade 180,657 (178,085 and 188,889 in 173;
not measured for 174 and 175). The eight older scenes: Super Mario
Land -1.2%, Balloon Kid -0.5%, R-Type -0.4%, Castlevania II -0.3%,
the rest the same. Much less than the 3 to 9% the second opinion
gave for these together: the writes to the video memory that a block
knows of are few beside those through a pointer it does not know,
and the 16-bit INC and DEC were mostly of HL, which was one `lea`
already. Right for what they are, and the code is shorter; the time
is elsewhere (the translator, the events).

Tests: the 24 of `gbreg`, the eight scenes and the eight older ones
with the same memory as 175, the pictures as before, R-Type II's bar,
ZAS's logo. Not covered: no test ROM reads or writes the echo - the
change is two constants and was read, not run on a program that uses
it; nothing on a WonderSwan or in nileswan-medem.

**Patch 177.** Gio, on 176 in nileswan-medem: Donkey Kong Land "still
looks broken in some frames, most of all Kong when he jumps"; and the
lines of the text at the start touch each other.

The check of 172 (`GBRUN_TRUTH`: each picture against itself with the
patterns as the Game Boy had them at line 0 of the frame shown) had
found nothing wrong in its 186 pictures: a short stretch, Kong walking.
On 815 WonderSwan frames more, with jumps and a level's start: 27
pictures of 756 wrong, all in the half second after the level's
fade-in. `GBRUN_TRUTHQ=1` (the last pattern written, the sprites'
tiles, whether the patterns are late, at every frame's end): Kong has
two sets of twelve tiles (232-243 and 244-255, and others); the game
draws the next picture in the set not shown and then moves his
sprites to it. So the last pattern written is a tile of a sprite *to
be* shown, not of one shown, and 172's rule (a tile of a sprite of
the last frame's list) held only when a picture happened to be drawn
over the set on the screen: the patterns went late 25 frames after
the first broken picture, and 120 frames after the last such chance
they stopped being late.

Changed (`gb_video.c`, `tq_frame`): the tile is also looked for among
the sprites the program has set by the end of the frame (the Game
Boy's OAM: the tile and the one beside it, for a sprite of 16 lines;
a sprite off the screen's lines does not count); and once the
patterns are late they stay so until the screen is turned off or a
frame writes more tiles than there are copies for (`gbv_tq_full`), no
longer for a count of frames - with a count, the first picture drawn
after its end was broken. The same walk: 3 pictures of 760 wrong, one
Game Boy frame, the first picture of Kong drawn after the level's
start (the frame in which the patterns are found to need it). The
186 of 172: none wrong.

The text at the start (`gb_main.c`): a row left empty after each
line; the font's letters fill their eight lines.

Tests: the 24 of `gbreg`; seven of the eight scenes with the same
memory as 176 (X was not waited for: Gio asked for a shorter check on
small changes - the tests and a few scenes - and the full one only
for changes to the code made). Cycles a Game Boy frame, 176 to 177:
R-Type II 28,834 to 28,904 and ZAS 59,780 to 59,930 (their patterns
are now late at times), Donkey Kong Land 69,641 to 69,667, the others
the same. Not run: the pictures against the reference, the eight
older scenes; nothing on a WonderSwan or in nileswan-medem.

`GBRUN_ONCE=1` (`gbrun.py`): how many blocks were entered once,
twice... Made to decide on "interpret a piece of code the first time,
translate it the second" (the second opinion's fourth): the
interpreter is 358 V30MZ cycles a Game Boy instruction all told (159
`gb_step` itself, 112 the loop around it, the rest its reads), the
translator about 640. Blocks entered once: Race Drivin' 194 of 1,603,
X 139 of 640, Batman 131 of 635. A saving of some 280 cycles an
instruction on an eighth to a fifth of the code, 358 more on the
rest: not done.

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
