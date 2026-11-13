# ggswan: Game Gear on the WonderSwan Color

ggswan is SwanShellDX's Game Gear plugin. It runs Sega Game Gear games
(`.gg` files) on a WonderSwan Color with a nileswan cartridge. The Z80 is
emulated by a dynamic recompiler (Z80 to V30MZ code), with the original
interpreter kept for the parts that need it. The Game Gear video memory is
mapped onto the WonderSwan's own tile and screen hardware.

Status: fairly stable (patch 170). It has been tested mostly in the
nileswan-medem emulator and with the host test tools; tests on real
hardware are still to come.

## Installation and use

1. Copy `PLUG_GG.BIN` to the `NILESWAN` folder of the microSD card
   (`/NILESWAN/PLUG_GG.BIN`). The SwanShellDX build puts it in
   `dist/NILESWAN/`.
2. Put the `.gg` files anywhere on the card.
3. Choose a `.gg` file in the SwanShellDX menu: the menu loads the
   plugin, which loads the game (up to 1 MiB) and starts it.

The start screen shows a few lines (SD card, banks, CPU mode) for about a
second, then the game starts. The number at the top right of the screen
(`P` and a number) is the plugin's patch number.

## Controls

| Game Gear | WonderSwan |
|---|---|
| D-pad up | X1 (X up) |
| D-pad right | X2 (X right) |
| D-pad down | X3 (X down) |
| D-pad left | X4 (X left) |
| Button 1 | B |
| Button 2 | A |
| Start | START |

The Y buttons are not Game Gear buttons; the plugin uses some of them
(see below).

## Keys held when the game starts

Hold the key while the plugin starts (from choosing the `.gg` file until
the game appears). They can be combined, except Y3, Y2, X1 and X2: of
these only one counts (in that order).

| Key | Function |
|---|---|
| Y1 (Y up) | **Background priority**: background tiles marked as in front of the sprites hide them (Sonic Spinball's tubes, for example). The WonderSwan has no priority per tile; its second screen layer is used for these tiles, so the side borders show the level instead of black and the diagnostics are not shown. |
| X3 (X down) | **IRAM/PSRAM benchmark**: before the game, the same code is timed from internal RAM and from the PSRAM code cache. Six values in WS lines (hexadecimal) are shown; press A to go on. They are also kept in IRAM at E7E0h after the marker `BENC`, for a debugger. Meaningful only on real hardware. |
| Y4 (Y left) | Diagnostics column on the right instead of the profile (see below). |
| X4 (X left) | Recompiler without fast paths (everything through the C helpers): for comparing when a game misbehaves. |
| Y3 (Y down) | Recompiler with self-check (slow): each block is checked against the interpreter. |
| Y2 (Y right) | Recompiler without links between blocks. |
| X1 (X up) | Recompiler without inline CALLs. |
| X2 (X right) | Recompiler test option (JP/JR followed). |

X4, Y3, Y2, X1 and X2 are test modes: slower, for finding out which part
of the recompiler is behind a problem.

## Keys during the game

| Keys | Function |
|---|---|
| Y2 (Y right) + Y4 (Y left) together | **Pause** (sound off) and resume: frame by frame, like Mednafen's Alt+A. |
| Y3 (Y down), while paused | Runs **one frame**, then pauses again. While paused, `F` in the left border shows the Game Gear frame number (last 3 digits). |
| START + Y1 (Y up), held for a second | **Back to the menu** (the cartridge RAM is then saved: see below). |
| Y4 (Y left) held, then START | **The numbers in the borders**, off and on (patch 151). (Y4 first: START alone is the game's Start button.) |

## Saves

Games that save to their cartridge RAM (battery saves) are saved as
`.sav` files next to the `.gg` file, as SwanShellDX does for WonderSwan
games. When a `.gg` file is launched, the menu loads its `.sav` (32 KiB,
created empty the first time) into the cartridge's SRAM, where the
plugin keeps the Game Gear cartridge RAM. When the menu starts again
(START + Y1 (Y up) held for a second in the game, or the console turned
off and on), it writes the SRAM back to the `.sav` file, with the
backups set in the menu's settings.

## The numbers in the borders

Without Y1 (Y up) at the start, the black borders show diagnostics,
updated every second.

Left column:

| Label | Meaning |
|---|---|
| X | Code cache flushes (total) |
| FPS | Game Gear frames shown in the last second (the Game Gear runs at about 60) |
| T | Blocks translated in the last second |
| D | Returns to the dispatcher, hundreds a second |
| W, O, I | Memory writes, port outputs, port inputs through the C helpers, hundreds a second |
| S, N | Instructions run by the interpreter: inside blocks, outside them (hundreds a second) |
| Q | Interrupts taken, hundreds a second |
| Z | Only if it happens: the game mapped this ROM bank in slot 0 (under the FPS). The first KiB of the ROM stays visible there on a real console; translated code reads those addresses from the bank mapped. No game known to do it and read data there: worth reporting |

Right column (profile: percentage of the busy time):

| Label | Meaning |
|---|---|
| P | Patch number |
| CD | Translated code |
| DI | Dispatcher (finding the next block) |
| PO | Helpers in C (ports, memory, VRAM synchronization) |
| ST | Instructions interpreted inside blocks |
| IN | Interpreter (interrupts, code in RAM) |
| TR | Translating new code |
| VI | End of frame: scroll, sprites, colors, video memory |
| AL | Everything else (main loop, timing) |
| (hex, bottom) | The last block translated in the last second, or else the last address interpreted |

With Y4 (Y left) at the start, the right column shows instead: K (lines
taken by the video memory writes at the end of the screen), M (most
scroll values in a frame), O, I, Y, U (WS lines a second in the port
output, port input, VRAM sync and PSG queue helpers), S and V (lines in
the sprite building and in the whole end of frame), G (Game Gear
interrupts a second).

## Features

- Recompiler with block linking, inline caches for RET and JP (HL),
  wait-loop detection, and dedicated routines for the common VDP and copy
  instructions (OUTI/OTIR, LDI/LDIR, VDP control and data ports, the V
  counter).
- Video: background, sprites (8x8 and 8x16), per-line horizontal scroll,
  vertical scroll, Game Gear palette; tile and palette writes are applied
  at the end of the WonderSwan screen so a frame is never shown half
  updated.
- Sound: the three tone channels and the noise channel on the WonderSwan
  sound channels, stereo; samples played through the volume registers
  (Sailor Moon S's voices) through the WonderSwan Color sound DMA.
- Frame rate limited to the Game Gear's (59.92 frames a second).

## Limitations

- The Game Gear view is 160x144; the WonderSwan screen is 224x144, so the
  game is centered with borders on both sides.
- Background priority only with Y1 (Y up) at the start (see above).
- Some games run below full speed in their heaviest parts.
- Requires a WonderSwan Color (or SwanCrystal) for the color modes and
  the sound DMA.

## Source layout

- `core/`: Game Gear machine (Z80 interpreter, VDP, memory mapper).
- `drc/`: the Z80 to V30MZ recompiler.
- `ws/`: the WonderSwan side: runtime, video, sound, main loop.
- `host/`: PC test tools (comparison against the interpreter and a
  reference renderer, cycle profiling in an x86 emulator).
- `SG1000.md`: sgswan, the SG-1000 plugin being made from these sources
  (`-DGGSWAN_SG1000`): the plan, the memory map, what is done.
- `SF7000.md`: the SC-3000 with the SF-7000 and its disks
  (`-DGGSWAN_SF7000`): the machine, which runs on the PC only so far, the
  built-in IPL, what the plugin still needs.
- `GB-MEMORY-MAP.md`: gbswan, the Game Boy plugin being started (its own
  sources, in `gb/`): the memory map and the video test of step GB0
- `GB-ACCURACY.md`: the Game Boy's reference on the PC and the test ROMs
  it passes
- `GB-IMPLEMENTATION.md`: how gbswan is made and where it is
- `GB-ABI.md`: the code gbswan's recompiler writes

## smsswan: the Master System plugin

`PLUG_SMS.BIN` is built from the same sources with `-DGGSWAN_SMS`
(`make -f Makefile.ggswan SMS=1`; the main build makes both plugins).
Copy it to `/NILESWAN/PLUG_SMS.BIN`: the menu starts it for `.sms`
files, with the same `.sav` saves. What is written above for the Game
Gear holds, with these differences.

The Master System shows a 256x192 picture; the WonderSwan shows 224x144
of it on its whole screen (no borders), and the view can be moved over
the picture while playing. The background priority is always on.

| Keys | Function |
|---|---|
| X1-X4, B, A | D-pad, button 1, button 2 (joypad 1; there is no second joypad) |
| START | The **Pause** button (sent to the game when START is let go) |
| Y1 (Y up), Y3 (Y down), Y4 (Y left), Y2 (Y right) | **Move the view** over the picture, 2 pixels a frame (X 0-32, Y 0-48; it starts in the middle). Not into the first 8 columns while the game blanks them. |
| Y2 (Y right) + Y4 (Y left) together | Pause and resume, frame by frame with Y3 (Y down), as on the Game Gear |
| START + Y1 (Y up), held for a second | Back to the menu |
| START + Y4 (Y left) | **The numbers** (FPS, the recompiler's, the profile) over the sides of the picture, on and off (patch 151). They use screen 2, as the background's priority does: with the numbers on, no priority (as with Y1 (Y up) held at the start) |
| START + Y3 (Y down) | **Whole picture**, on and off (an experiment): the 192 lines squeezed into the 144 of the screen, one line in four left out. It takes about a quarter of the CPU, small text loses lines, and sprites keep their height (the pieces of a tall figure overlap). Sideways the view is still 224 columns. |
| Y1 (Y up) held at the start | **Diagnostics** over the sides of the picture (the same numbers as in the Game Gear's borders) instead of the background priority |

Mappers: Sega's, and Codemasters' (found from the cartridge header).

The 224-line mode (Codemasters' games) is there: the picture is then
256x224, the view moves over Y 0-80, and it starts in the middle.

Not done: PAL timing (313 lines, 50 Hz: Codemasters' games were made
for it; Micro Machines reaches its menus with the 262 lines here),
register 0 bit 7 (the right columns not scrolled up and down), the
sprite overflow flag, the limit of 8 sprites a line, FM sound, a second
joypad.

The sprite collision flag is worked out only for games seen to test
it right after reading the VDP status (it takes time: about 8% of a
frame in Sonic, which does not use it).

## sgswan: the SG-1000 plugin

`PLUG_SG.BIN` is built from the same sources with `-DGGSWAN_SG1000`
(`make -f Makefile.ggswan SG=1`; the main build makes all four
plugins). Copy it to `/NILESWAN/PLUG_SG.BIN`: the menu starts it for
`.sg` files (up to 48 KiB, no mapper). New in patch 143 and tried only
in the PC tools so far: see `SG1000.md` for how it is made and what was
measured.

The SG-1000 shows a 256x192 picture; the WonderSwan shows 224x144 of it
on its whole screen, and the view can be moved over the picture while
playing, as smsswan's.

| Keys | Function |
|---|---|
| X1-X4, B, A | D-pad, button 1, button 2 (joypad 1; there is no second joypad) |
| START | The **Pause** button (sent to the game when START is let go) |
| Y1 (Y up), Y3 (Y down), Y4 (Y left), Y2 (Y right) | **Move the view** over the picture, 2 pixels a frame (X 0-32, Y 0-48; it starts in the middle) |
| Y2 (Y right) + Y4 (Y left) together | Pause and resume, frame by frame with Y3 (Y down), as on the Game Gear |
| START + Y1 (Y up), held for a second | Back to the menu |
| START + Y4 (Y left) | **The numbers** over the sides of the picture, on and off (patch 151; as Y1 (Y up) held at the start) |
| START + Y3 (Y down) | **Whole picture**, on and off (patch 150), as smsswan's: the 192 lines squeezed into the 144 of the screen, one line in four left out; sprites keep their height (the pieces of a tall figure overlap by a quarter). The view still moves sideways. A screen of more different characters than there are tiles for (558) shows some of them empty |
| START + Y2 (Y right) | **Four sprites to a line**, on and off (off at the start; it was START + Y3 (Y down) until patch 147). The SG-1000 draws four sprites on a line and leaves the others out, which is the flicker of its games; the WonderSwan can draw them all, and does. On: as the SG-1000 |
| Y1 (Y up) held at the start | **Diagnostics** over the sides of the picture (FPS and the recompiler's numbers; on the right, with Y4 (Y left) held too: K the tiles drawn in the last second, M the background tiles in use) |

There: Graphics I and II, multicolor, sprites of 8x8 and 16x16 pixels,
magnified too, the backdrop color, the sprite flags of the status
register (fifth sprite, overlap), the three sound channels and the noise.
Cartridges with their own RAM at 8000h (The Castle) and the games made
for the 8 KiB RAM adapter (TwinBee, Bomberman Special) run. No saves:
the SG-1000 has none.

Not done: a second joypad, the SC-3000's keyboard (`.sc` files are not for
this; the SF-7000's disks are sfswan's, below).

## sfswan: the SC-3000 with the SF-7000's disks

`PLUG_SF.BIN` is built from the same sources with `-DGGSWAN_SG1000
-DGGSWAN_SF7000` (`make -f Makefile.ggswan SF=1`; the main build makes
all four plugins). Copy it to `/NILESWAN/PLUG_SF.BIN`: the menu starts it
for `.sf` files, disk images of 163,840 bytes. New in patch 148 and run
only in the PC tools so far: `SF7000.md` says how it is made, what was
measured and what is missing.

It needs a system disk: **a copy of a disk that has Disk BASIC on it, as
`/NILESWAN/SF7000.SF`** (The Complete Arcade Pack Collection's image is
one). A disk with no system of its own is started from that one and is
in the drive when Disk BASIC's prompt comes; so is any disk with B held
at the start. A disk whose system is not Disk BASIC (Kamikaze) is started
from it too, and `BOOT` is typed for it.

| Keys | Function |
|---|---|
| X1-X4, B, A | The joypad: D-pad, button 1, button 2 |
| START | **The keyboard** on the screen, on and off (when START is let go). With it: X1-X4 choose a key, A presses it, B is CR; the last row is SHIFT, BREAK, INS/DEL, space, CR and four lines typed for you (`FILES`, `LOAD "`, `RUN`, `BOOT`) |
| Y1 (Y up), Y3 (Y down), Y4 (Y left), Y2 (Y right) | Move the view over the picture, as sgswan's |
| START + Y1 (Y up), held for a second | Back to the menu |
| START + Y4 (Y left) | **The numbers** over the sides of the picture, on and off (patch 151; as Y1 (Y up) held at the start) |
| START + Y3 (Y down) | The whole picture squeezed, on and off, as sgswan's (not in text mode: Disk BASIC's screen stays a view) |
| START + Y2 (Y right) | Four sprites to a line, on and off, as sgswan's |
| B held at the start | Start from the system disk |

Slow: about half the SF-7000's speed at Disk BASIC's prompt, less while
it reads the disk. What a program writes to the disk is lost when the
plugin is left.

## Changelog

One line per commit, oldest first. The patch number (shown as `P` at
the top right while playing) exists from patch 90; the commits before
it are listed in order without one.

### Before patch 90

- Game Gear emulator plugin (experimental)
- Z80 -> V30MZ recompiler, first part (PC only, not built yet)
- fix the build (MAP_SIZE), no far memcmp
- recompiler runtime, the recompiler is now the default CPU
- recompiler: do not read the bank ports back; diagnostics
- recompiler diagnostics: self-check mode, PSRAM bank test
- recompiler: fix the work RAM segments with 16-bit ints
- faster timing, VRAM fast path, dynamic block lookup; clearer diagnostics
- sampling profiler in the right border
- 6 code cache banks, RETI/RETN translated, flush counter shown
- shared OUTI routine, wait loops end the run, EI without the interpreter
- native LDI runs, SBC/ADC HL, INC/DEC (IX+d); repeated instructions without the dispatcher
- far links between code banks, guarded links between slots, helpers without leaving the block, VDP ports, native LDIR
- shorter linked exits and RAM writes through (HL)
- L0 cache in IRAM for RET / JP (HL) lookups; fix the start-up checks
- fix the WS build of the recompiler (drc_tramp declared after use)
- translate code in work RAM, with self-modifying LD A,(nn)
- start-up text on every other row
- start the lines reached by the CPU in one pass (AL time)
- OUTI runs that leave the VRAM path finish in C, not instruction by instruction
- link into blocks in work RAM, and find them through DYN
- longer blocks through JP/JR and short CALLed routines
- fix the 16-bit overflow warnings in gg_run_frame
- VDP status reads and the fixed ports without the C helper
- frame limiter at 59.92 FPS; scroll register writes that change nothing stay in the code
- move big buffers out of the internal RAM below 4000h (stack room)
- diagnostics, switch off JP/JR following or CALL inlining at start
- turn off following JP/JR inside blocks by default
- lighter dispatcher (L0 lookup in C, self-check sums only when on)
- take IM 1 interrupts in the dispatcher
- start the next line from the dispatcher, not the frame loop
- line interrupts handled without leaving drc_enter
- OUTI and LDI runs copied with rep movsb
- rotates and shifts keep their flags in x86 flags when they can
- name table routine with a row table
- cld before the rep movsb of the OUTI and LDI routines
- diagnostics, X4 at start turns off patches 29-32
- LDI rep movsb path, refuse destinations in the RAM mirror
- OTIR translated (the OUTI routine, 64 bytes per turn)
- line events at the block exits, in the generated code
- horizontal scroll splits recorded by the VDP control routine
- name table routine, palette and flip bits from a table
- OUTI routine, one page-class lookup instead of three range checks
- keep the WS screens on when the display was on at the frame start
- single VRAM writes find the name and sprite tables from the page class
- LD A,I and LD I,A translated
- block copies to PSRAM with _fmemcpy, table entries in one window switch
- VDP data writes to CRAM through a generated routine, not the helper
- port helpers without the mapper snapshot, PSG tone divider from a table
- flags in AH only as far as they are read later
- runs of LDI/OUTI in RAM code cut to the RAM block size
- sprite tile writes in the frame interrupt deferred to the end of the frame
- Sonic, screen shown when the display goes on during the frame
- vertical scroll set at the end of the WS screen, like the horizontal one
- sprites outside the view not given to the WS
- scroll splits the WS line interrupt reaches late are applied at once
- defer all VRAM tile/name-table writes to the end of the WS screen
- defer only tile data; scroll before the VRAM commit; no vblank spin
- diagnostics for the end-of-screen interrupt (L, K, E)
- tile log copied with REP MOVSW; diagnostics M, Y, J
- diagnostics for Sonic's device-only scroll split (G, registers, RAM)
- diagnostics column (Y4 at the start): where Sonic's frame interrupt is taken
- diagnostics: the frame position when a scroll split is recorded
- cycles left after a mapping change: 32-bit and the real count
- map relaid with room around the view; two sprite tables
- interrupt entries in assembler that load DS and ES
- present a Game Gear frame only after the last one was shown
- a VRAM read applies only the byte it reads, not the whole log
- Sailor Moon S: RRD/RLD translated, PSG writes queued, frame shown at line 193
- declare video_present before gg_platform_frame_end
- no split dropped at the bottom when the list is full; SYNC profiled as PO
- same-value bank and VDP register writes without a helper; video_present timing
- line interrupt, sprite table and PSG queue in assembly
- psg_st not static on the WS
- line interrupt back to the full entry
- slot 1/2 bank changes and joypad reads without helpers
- VRAM reads with a long tile log, helper time in the Y4 column
- V counter without the helper, no deferring after many VRAM reads
- translator reads Z80 bytes through a kept pointer; wsprof.py
- translator byte reads inline
- diagnostics show the last block translated
- mapper routines write the WS bank ports; wsrun.py
- IXH/IXL translated; quick redispatch from L0 in drc_fast_reentry
- LD A,R and IN r,(C) on the VDP data port translated
- optional niledebug poll in the frame loop (NILEDBG=1); ggsyms.py
- cheaper OUTI/OUTD runs (flags in the caller, only when live)
- Game Gear ROMs up to 1 MiB
- long OUTI-only blocks in RAM, checked with one REPE SCASW
- VRAM reads outside the deferred pages skip the log scan
- lighter DYN lookup and class routine for deferred tile bytes
- OUTI runs to CRAM handled in the run routine

### Patches 90-170

| Patch | Change |
|---|---|
| 90 | patch number on the start screen; profiling options |
| 91 | patch number also in the top right corner while playing |
| 92 | present the frame at line 168, after the last line shown |
| 93 | VRAM reads outside the tile log do not turn deferring off |
| 94 | tile log and sprite table handed to the WS together, off the last lines |
| 95 | wait loops on (IX+d)/(IY+d) skipped; HL's segment kept when only L changes |
| 96 | OUTI runs into the name table at once, one map conversion a word |
| 97 | wait loops with EI, and DI loops that go through EI / JP back |
| 98 | wait loops that leave with a conditional branch and come back with JR |
| 99 | fixes from ChatGPT's review of patches 89-98 |
| 100 | translator compiled with -O3 -fno-tree-vrp -fno-tree-pre |
| 101 | long blocks stop where a translated block starts |
| 102 | the profile column every second |
| 103 | IRAM/PSRAM benchmark (X3 at start), stub and link profiling |
| 104 | OUTI runs up to 4 bytes copied with a byte loop |
| 105 | inline cache at the DYN exits (RET, JP (HL)) |
| 106 | one routine for Shinobi's OUTI / OUTI / DEC L x 4 rows |
| 107 | OUTI runs to the VDP control port without the helper |
| 108 | samples played through the PSG volumes (Sailor Moon S voices) |
| 109 | background tile priority, faster DJNZ $ delays, longer PSG queue |
| 110 | background priority off by default, Y1 switches it |
| 111 | Y1 at the start selects the background priority |
| 112 | frame by frame (Y2 + Y4 stop, Y3 one frame) |
| 113 | V counter wait loops skipped at once (Sonic Spinball) |
| 114 | RRD / RLD without flags when nothing reads them |
| 115 | tile bytes appended to the log inside the block |
| 116 | START + Y1 held for a second goes back to the menu; cartridge RAM saved to .sav by the menu |
| 117 | frame by frame fixed (the pause waited for a line it never saw), frame number F while paused; cartridge reset before the soft reset to the menu |
| 118 | smsswan, step 1: PLUG_SMS.BIN from the same sources (-DGGSWAN_SMS), the 224x144 view over the whole WS screen, priority always on, .sms in the menu |
| 119 | line interrupts taken at their line (Super Monaco GP II: the status bar's digits torn, its scroll set lines late) |
| 120 | smsswan, step 2: the Master System palette and ports |
| 121 | a frame's scroll list starts at the end of the WS screen, with its tiles and sprites (half the screen jerking sideways for an instant) |
| 122 | a scroll split's line interrupt no longer lost when its line starts while it is being set (half the screen jerking sideways for an instant) |
| 123 | the map laid out again after the end of the WS screen, ahead of the raster (Sonic: a few rows jumping when the camera moves up or down) |
| 124 | smsswan, step 3: the Pause button (START, a non-maskable interrupt) |
| 125 | smsswan, steps 5 and 6: the view moved with the Y buttons, diagnostics with Y1 at the start, the Codemasters mapper |
| 126 | smsswan: the whole picture squeezed vertically (START + Y3), an experiment; the scroll list prepared with interrupts on |
| 127 | smsswan: the first 16 lines not scrolled sideways (register 0 bit 6: Wonder Boy III's status bar) |
| 128 | smsswan: palette writes in the recompiler without the C helper |
| 129 | smsswan: the 224-line mode (Micro Machines and the other Codemasters games) |
| 130 | smsswan: the sprite collision flag, for the games that test it |
| 131 | line interrupts every few lines taken without going back to C (Sonic Spinball: about a fifth less time a frame) |
| 132 | the loop OUTI / JR NZ (OTIR written as a loop: Shinobi, Columns, Alex Kidd) sent to VRAM in one go |
| 133 | Sonic while playing: name table entries from two OUTIs in their own routine, nothing done for bytes already there, a single LDI and sprite table bytes without a routine call (about 14% less time a frame) |
| 134 | routines for a tile row sent as OUTI x 3 + OUT (Sonic's own tiles) and for a name table entry sent as OUTI + OUT (Columns: its wells, 59 k cycles a frame to 37 k) |
| 135 | an OUTI alone through the single write's code, sprite table words in the entry routine, name table runs already in VRAM skipped (Super Monaco GP II); wsrun counts cycles as Mednafen does |
| 136 | translated code: (IX+d) in work RAM in place, RAM writes through HL, DE, BC shorter and without losing the segment of HL, INC HL keeps it too, EXX with MOVs; the scroll splits' merge stops at the first pair one apart |
| 137 | more scroll splits than fit: thinned out a quarter at a time with a threshold for the frame, looking only at the ones added since the last pass (Super Monaco GP's road in a bend spent 18% of its time dropping them one by one) |
| 138 | 64 scroll splits a frame instead of 24 (the road of Super Monaco GP: its edges in steps of 4 pixels), paid for by a short path in assembly for the line interrupt of a split; irqtest.py compares it with the C handler |
| 139 | Shinobi: a block in RAM that finds its code changed empties its own L0 entry, not the whole cache (6000 cycles every other frame, and the line interrupt late in nileswan-medem: the fence broken in two); long copies in pieces; the backdrop from the tiles that show color 0 (the map screen's brown bar); wsrun lists the long REPs, wssim probes a pixel |
| 140 | from a review of smsswan: the stack outside the work RAM (a table in ROM read with POPs, the RAM's mirror) goes to the interpreter instead of other bytes of IRAM; smsswan starts with the VDP registers as the BIOS leaves them; PSG: tone 0 as 1, noise that follows tone 2 below the WS's slowest rate; the collision flag also for a status kept and tested later; LD A,R moves with the cycles; Codemasters cartridges without the Sega mapper's routines; mksptest.py |
| 141 | diagnostics: Z and the bank, under the FPS, when a game maps a ROM bank other than 0 in slot 0 (the one case left from the review of patch 140) |
| 142 | sgswan (the SG-1000), step 1, nothing on the WonderSwan yet: the machine in `core/` under `-DGGSWAN_SG1000` (ports, the TMS9918A's registers and status, the sprites' flags worked out once a frame), `host/sgtest` with a reference renderer of the four screen modes and the sprites, the test ROMs of `mksgtest.py`; the plan and the memory map in `SG1000.md`. `PLUG_GG.BIN` and `PLUG_SMS.BIN` are the same bytes as patch 141 but for this number |
| 143 | sgswan, the SG-1000 plugin: `PLUG_SG.BIN` (`SG=1`), `.sg` in the menu. The TMS9918A's screen on the WonderSwan (`ws/sg_video.inc`: patterns turned into tiles when a character in the view shows them, sprites 8x8, 16x16 and magnified, the 224x144 view), its ports and block I/O in the recompiler, RAM at 8000h after a short ROM (The Castle) and at 2000h where the ROM is empty (the 8 KiB RAM adapter's games); `wssim_sg`, `libdrcsys_sg.so`, `wsshot.py`. Not yet tried on a device or in nileswan-medem. `PLUG_GG.BIN` and `PLUG_SMS.BIN`: the same bytes as patch 142 but for this number |
| 144 | LDIR and LDDR to somewhere outside the work RAM (the cartridge RAM, ROM), in all three plugins: after the interpreter had done the copy, the block went back to the instruction with BC = 0 and copied 64 KiB more, over all of the memory; and its cycles were counted with a byte too many, most of a frame lost or gained each time. Found with sgswan's The Castle; `mkldirtest.py` |
| 145 | sgswan: the four sprites to a line of the TMS9918A, on and off with START + Y3 (Y down), off at the start (all the sprites shown, as before). The lines of a sprite the chip leaves out are empty in a set of tiles made for it; the picture is then the reference's to the pixel in Star Force, TwinBee and Zoom 909. The sprites' tiles were not made again when a game changed the sprites' size and wrote the table in the same frame. `PLUG_GG.BIN` and `PLUG_SMS.BIN`: the same bytes as patch 144 but for this number |
| 146 | The SF-7000 (the SC-3000's disk unit), step 1, nothing on the WonderSwan yet: the machine in `core/` under `-DGGSWAN_SF7000` (64 KiB of RAM with the IPL over the first 16, the two 8255s, the keyboard matrix, the uPD765 and its disk with the index hole Disk BASIC times), an IPL of its own in place of Sega's (`host/mksfipl.py`), `host/sftest` (keys typed, the disk changed) and `host/sf_unit` (56 checks through the ports). On the PC the three disks tried start: Disk BASIC 1.0p and Astro from The Complete Arcade Pack, Kamikaze with `BOOT`, Poseidon Wars. `SF7000.md`. `PLUG_GG.BIN`, `PLUG_SMS.BIN` and `PLUG_SG.BIN` are the same bytes as patch 145 but for this number |
| 147 | The palette with the frame, in ggswan and smsswan: a color the game writes goes to the WonderSwan's palettes at the end of the WS screen, with the scroll, the tiles and the sprites of the frame it belongs to, and no longer at once, in the middle of the screen being drawn and a WS screen or two before the rest. Shinobi moves the bars of its fence by turning four colors round: the fence was broken in two, one part two pixels ahead of the other. In `wssim` the WS picture of Sonic, Shinobi, Super Monaco GP, Aladdin, Columns, Batman Returns and Sailor Moon S differs from the Game Gear's by 0.0-1.4% of the pixels on average, from 1.0-9.0%. `drcsys.py --pal`. The menu: Settings > System > Save data management, first in the list and on by default; off, no save data is loaded into the cartridge at launch and none is written back to the card when the menu starts again (for where the save memory is not the physical cartridge's). `PLUG_SG.BIN`: the same bytes as patch 146 but for this number |
| 148 | sfswan, the SC-3000 with the SF-7000's disks: `PLUG_SF.BIN` (`SF=1`), `.sf` in the menu. The recompiler with all of the memory RAM (one WonderSwan segment, a map of where code was translated from, code that writes over code run by the interpreter where it is), the disk and a system disk (`/NILESWAN/SF7000.SF`) in PSRAM, the TMS9918A's text mode (in sgswan too), a keyboard on the screen (START), the built-in IPL's long parts done by the machine. Run in the PC tools only, the device build in `wsrun.py` too (to Disk BASIC's prompt and `FILES` through the keyboard on the screen); about half the SF-7000's speed at the prompt. `SF7000.md`. sgswan and sfswan: the four sprites to a line are START + Y2 (Y right) now, START + Y3 (Y down) is kept in every plugin for the whole picture squeezed. ggswan, smsswan, sgswan: the translator writes the same bytes as patch 147 (`drcsys.py --same-code`); `wsrun.py` with the WonderSwan's keys (`WSRUN_WSKEYS`) and a second file (`WSRUN_SYS`) |
| 149 | ggswan: the sprite collision flag (VDP status bit 5), which only smsswan had: Batman Returns reads it for its hits (`IN A,(BFh)` / `AND 20h`) and walked through everything. As smsswan's: worked out once a frame, only for a game found to test the flag (of seven games tried only Batman Returns), and it costs: Batman Returns in its first level goes from about 70 k to 84-98 k cycles a frame in `wsrun.py`. In both plugins a sprite with no pixel is left out early (a sprite table never written, 64 empty sprites in one place, took 1.4 million cycles a frame); the flag is the same as a count of the sprites on each pixel in every frame tried (`GGSWAN_COLL=1` in `wssim`, now for ggswan too). The menu: the game icon for `.gg`, `.sms`, `.sg` and `.sf` files. `drcsys.py` moved the windows only for ports C0h-CEh: with the bank ports at D0h-D5h since patch 148, games with a mapper ran wrong in it (patch 148's `--same-code` check was repeated with this corrected: the same result). The sgswan and sfswan binaries are not the same from one build to the next (their size changes by a few bytes with the same sources: to be looked at) |
| 150 | sgswan, sfswan: the whole picture squeezed, START + Y3 (Y down), as smsswan's: the view is all 24 rows and the line interrupt adds 1 to the Y scroll every three lines (`sg_line_entry` in assembly, 47 a WS screen); sprites placed on the squeezed picture, their height kept; not in text mode. `sgirqtest.py` (the interrupt in Unicorn, served late too), `GGSWAN_SQUEEZE=1` in `wssim_sg` with the reference squeezed. Not on a device: `wsrun.py` has no interrupts. sfswan: a disk whose system is not Disk BASIC (Kamikaze, to be started with `BOOT`: by itself its screen is mirrored) is started from the system disk and `BOOT` typed for it |
| 151 | START + Y4 (Y left) shows and hides the numbers while playing, in the four plugins (ggswan: Y4 first; smsswan: the numbers or the background's priority). The sprite collision flag: the pairs compared sideways first, from a table of the X positions (measured in `wsrun.py`: no gain, the routine is 9-15% of Batman Returns' frame all the same: 100 pairs looked at, 2 compared by rows; the C of the loop is what costs). From the list of smsswan optimizations reviewed with measurements (`GGSWAN_COLLSTAT=1` in `wssim`): keeping the flag's result between frames is not worth it (what it is made from is the same as the frame before in a tenth of Batman Returns' frames in the game, half of Castle of Illusion's with its title); the squeezed picture's Y-only events and the sprite builder's tests would save 3 or 4 instructions each: left as they are |
| 152 | The sprite collision flag's walk of the sprite table in assembly (`coll_scan`, `drc_asm.s`; the rows of a pair that overlaps stay in C, `coll_pair`), and the sprites with a tile found to have no pixel passed over at once. Batman Returns in `wsrun.py`: 96/86/83 k cycles a frame in its first level before, 93/83/79 k now; before the game 94/31/59/30 k, now 87/22/50/22 k (85/20/48/19 k with no flag at all, patch 148). `colltest.py`: the device's routine in Unicorn on 3,102 states saved by `wssim` (`GGSWAN_COLLDUMP`), the same flag as the C in all of them |
| 153 | gbswan, a Game Boy plugin, step GB0: nothing of the Game Boy yet. `PLUG_GB.BIN` (`GB=1`, sources of its own in `gb/`), `.gb` in the menu; it shows a test of the video planned: the WonderSwan Color's 2 bits a pixel color mode, whose tiles are the Game Boy's byte for byte, the Game Boy's video memory at 2000h-3FFFh of the internal RAM so that background and sprites use the patterns where the program writes them, the C data and stack below 2000h (checked at build), screen 1 for color 0's shade, screen 2 in a 160 x 144 window, sprites over and behind it cut by the sprite window. `GB-MEMORY-MAP.md`; `host/gbrun.py` runs the device build in Unicorn and draws that mode. Not run on a WonderSwan. The other four plugins: the same sources |
| 154 | gbswan: START + Y1 (Y up) stopped the emulator instead of going back to the menu (Gio, nileswan-medem; the rest of step GB0's test was as expected there). The plugin now sets the nileswan up as the other plugins do before anything else (clock, I/O and SRAM on, the RAM window's mask): the difference found with their way out, not confirmed as the cause |
| 155 | gbswan, step GB1, on the PC only: the Game Boy itself as a reference, in `gb/core` (the SM83 with every bus access a machine cycle, the bus, MBC1, MBC2, MBC3 without its clock, MBC5, the timer, the serial port, the keys, the OAM DMA, and the LCD with a picture drawn a line at a time); `gb/host/gbtest` runs a ROM (trace, keys, picture) and `gb/host/gbsuite.py` the test ROMs: Blargg's `cpu_instrs`, `instr_timing`, `mem_timing`, `mem_timing-2`, `halt_bug`, the 54 of Mooneye's acceptance tests for the DMG that are not about the LCD, 27 of 28 of its cartridge tests, dmg-acid2; 5 of 12 of Mooneye's LCD tests. `GB-ACCURACY.md`. The plugin on the WonderSwan is still the video test (its sources moved to `gb/ws`) |
| 156 | gbswan, step GB2, on the PC only: the LCD's timing as the test ROMs measure it (when STAT shows each mode and when its interrupts are asked, the video memory and the OAM refused to reads and writes at their own dots, the line after the LCD is turned on, LYC's comparison kept with the LCD off, what the sprites add to the drawing of a line). Mooneye's `acceptance/ppu`: 12 of 12 (5 before); dmg-acid2 the same picture as its reference; 33 of 47 of the older LCD tests of wilbertpol's fork, run by `gbsuite.py` from the text they leave on the screen (`gbtest -s`). `GB-ACCURACY.md` |
| 157 | gbswan, steps GB3-GB5: the plugin runs Game Boy programs, far too slowly to play (about a frame a second: the CPU is the C interpreter). `gb/core/gbf_hw.c`, the machine by events (cycles counted to the next thing due; LY, STAT, DIV, TIMA worked out when read), with the reference's CPU and mappers; on the PC with the reference's renderer (`gbftest`) it shows the same pictures as the reference in the eight games tried, and passes the tests that are not about timing. `gb/ws`: the ROM read into PSRAM and mapped by the cartridge's windows (1 MiB at most), the picture with the WonderSwan's hardware (the video memory as the tiles, a WonderSwan map kept for each Game Boy map, screen 2 for background and window switched at their lines by the line interrupt, the sprites sorted as the DMG covers them), the keys. `host/gbrun.py` now runs the plugin with a ROM and the interrupts; `host/gbcmp.py`: the plugin's first screens are the PC's in six games of eight, pixel for pixel. `GB-IMPLEMENTATION.md`. Not run on a WonderSwan |
| 158 | gbswan, step GB7 first: a recompiler for the Game Boy's CPU (`gb/ws/gb_drc.c`, `gb_drc_asm.s`, `GB-ABI.md`): A, BC, DE, HL, SP in AL, CX, DX, BX, BP; Z, H, C the V30MZ's own flags, kept in AH only when something would lose them (a pass over the block finds which are still wanted); everything from 8000h up but the cartridge's RAM 6000h lower in the internal RAM, the high RAM and the I/O registers too; blocks joined by jumps, returns looked up by an assembly routine; code in RAM translated with a copy of its bytes to compare; loops that wait on memory, LY or STAT given their time at once; what is not translated left to the interpreter an instruction at a time. In `gbrun.py`: Blargg's 11 CPU test ROMs pass in the plugin, the first screens are as before, and a frame costs 31-62 thousand V30MZ cycles in Batman, Super Mario Land and Balloon Kid (51,430 is full speed), 110 thousand in R-Type, half a million and more in Castlevania II, Yoshi and Super Mario Land 2 (`GB-IMPLEMENTATION.md`). Figures on the screen with START + Y4 (Y left). Not run on a WonderSwan |
| 159 | gbswan: sound (`gb/ws/gb_sound.c`: the two squares, the wave with the Game Boy's own samples and the noise on the WonderSwan's four channels, lengths, envelopes and sweep stepped once a frame); a cartridge with a battery saved to `.sav` by the menu as the Game Gear's are (the menu's part only compiled); and speed. The STAT interrupt's events only where its signal can rise; P1, the ROM bank register, the cartridge's RAM and a stack in it by the assembly routines; writes to the scroll, palettes and DMA registers that stay in the block; the sprite table in assembly; a fault of 158 fixed (a loop that jumps back to its start taken for one that waits whichever of its jumps did it: Balloon Kid's water a frame out). In `gbrun.py`, cycles a frame with sound (51,430 is full speed): Castlevania II 27 thousand (470 before), Balloon Kid 30, Super Mario Land 31, Bubble Ghost 34, Batman 48, Yoshi 55 (510), R-Type 65 (110), Super Mario Land 2 70 (895). `gbrun.py`: a profile by routine and by block, the sound registers as a WAV, HLT told from a stop elsewhere. Not run on a WonderSwan |
| 160 | gbswan: faster, and a fault of 159. The fault: the table's key for the second ROM area's bank was no longer set when the bank changed through C (a line lost in 159: MBC1 cartridges over 512 KiB, MBC5's ninth bit), so a return could find the wrong bank's block; 13 of Mooneye's cartridge tests now run in the plugin. Faster: after a write to an I/O register the block goes on unless an interrupt can be taken or the time is up (it left for the dispatcher every time); a loop that waits for a STAT mode is one routine call that passes the time and reads STAT; returns jump through the blocks' table; the sprites sorted from last frame's order. The background's palette is now set at the lines the program changes it at. `gbrun.py` can count frames, keys and the profile in Game Boy frames (`GBRUN_GBTIME`), which is what makes two builds comparable: 159's table was not. Cycles a frame, 159 then 160 (51,430 is full speed): Batman 68,590 to 45,927, R-Type 69,048 to 58,492, Castlevania II 72,209 to 64,653, Yoshi 54,982 to 53,694, the other four 17-33 thousand as before. With keys, at frame 840, five games' pictures are the PC's to a few pixels. Not run on a WonderSwan |
| 161 | gbswan: MBC3's clock, and faster again. The clock (the registers' behaviour as Peanut-GB, MIT, has it) counts the Game Boy's frames while the game runs and is not kept between runs; rtc3test's basic and range tests pass but "Tick". Faster: PUSH AF and POP AF with the stack in the high RAM went through C for every byte (Castlevania II); a wait for a STAT mode is found anywhere in a block, not only at its start, and where it ends comes from a table; a constant written to P1 is one byte moved (Yoshi reads the keys two hundred times a frame). Cycles a frame, 160 then 161 (51,430 is full speed): Castlevania II 64,653 to 45,478, R-Type 58,492 to 50,425, Yoshi 54,034 to 46,035, Bubble Ghost's title 33,228 to 16,602, the other four within 5%: all eight under a frame's time in the scenes measured. `gbrun.py` counted a Game Boy frame twice now and then (keys a frame off in some runs): corrected, and it prints checksums of the Game Boy's memory to tell two builds apart. Not run on a WonderSwan |
| 162 | gbswan, the picture: the patterns' place (LCDC bit 4) and the sprites' size (bit 2) changed in the middle of the frame. Each of the Game Boy's two maps is now kept as two WS maps, one for each place the patterns can come from, and the line's list names the one to show (before, the whole frame had the bit's last value: a status bar with its own patterns was wrong); a sprite is 8 x 16 if the bit was set at its top line. For the two more maps (they must be below 8000h) the stack went to D000h-D7FFh and the sound's waves to 0FC0h. dmg-acid2 in the plugin: right but for the "!" (an eleventh sprite, shown) and the lines of a window starting in the middle of the screen. `gbrun.py` drew its pictures a line early at every split and with the last palettes: with that corrected the eight games' pictures at frame 860, with keys, are the PC's pixel for pixel (the few to a thousand pixels of difference in the notes of 159-161 were the tool's). Cycles a frame within 1% of 161. Not run on a WonderSwan |
| 163 | gbswan after its first run in nileswan-medem (Gio, patch 161): the pictures right, the games in play far slower than the scenes measured here said (Yoshi 8 frames a second, Super Mario Land 2 27-35). In the recompiler now: DAA; RLC, RL, SLA, SWAP, SRL, RES, SET and the like on (HL) (they went to the interpreter, an instruction at a time); the high RAM through HL, BC, DE with no routine; a pointer loaded with a constant in the block (LD HL, nn, then INC and DEC) is that address, with nothing checked; a constant address in the cartridge's RAM and a stack there, in line. Yoshi in play 349,589 cycles a frame to 53,446, Super Mario Land 2 in its first level 82,174 to 59,132 (51,430 is full speed: both still over). START + Y4 (Y left) shows the patch number too (PAT). Not run on a WonderSwan |
| 164 | gbswan: a scrolling picture went a step back every few frames (Gio: Super Mario Land's scroll in jerks). Two faults in how a frame's list of scroll and map changes reaches the screen. The list was marked ready after the wait for the frame's time, by when the vertical blank interrupt, served during the wait, could have shown it already: the mark then stood for the other list, an older frame's, shown next. And the frames' time was counted in whole WonderSwan frames, so every Game Boy frame started at a vertical blank and ended near the next: a little more work and its picture missed it. Now the list is ready when made, one not shown yet is not made over, and the frames start 200.91 WonderSwan lines apart by the line counter. In `gbrun.py`, walking right in Super Mario Land, screen 2's scroll at each WonderSwan frame: before 252 252 254 255 0 0 255 1 (back and forth), now every value once and in order, one frame in four or five shown twice (59.7 pictures a second on a 75.5 Hz screen). Not run on a WonderSwan |
| 165 | gbswan: the blocks' table has 8,192 places (4,096: with about 3,000 blocks a new one found no place in 8 tries and everything was forgotten and translated again, which is the pause Gio saw in Castlevania II with FLS at 1); the translator reads a block's bytes once, from where they are; a pointer the block loads into the cartridge's RAM is accessed in line; the OAM copied from the cartridge's RAM in one move (Super Mario Land 2, every frame). Super Mario Land 2 in its first level 59,132 cycles a frame to 55,454, Yoshi in play 53,446 to 52,490 (51,430 is full speed). Balloon Kid in play measured for the first time: 62,267, of which about 17,000 are the translation of the two new blocks a frame that scene meets (some 9,000 cycles a block: the translator is slow, and that is what a game does when something new comes on the screen). Not run on a WonderSwan |
| 166 | gbswan: six scenes in play are now what is measured (Castlevania II's Crystal Castle, Batman's and R-Type's first levels, Yoshi, Super Mario Land 2's first level, Balloon Kid's stage 1: `GB-IMPLEMENTATION.md`). The translated code calls its routines where they are (it went through a table of jumps: 4 cycles a call, thousands of calls a frame); a return's lookup starts from the flags of the cycles' subtraction; a loop that only counts BC down is passed over to its last turn; the translator's first pass does less for each instruction. Cycles a frame in play, 165 then 166 (51,430 is full speed): Castlevania II 63,426 to 60,906, Batman 63,630 to 61,353, R-Type 54,691 to 53,042, Super Mario Land 2 55,454 to 53,013, Balloon Kid 62,267 to 58,809, Yoshi 52,490 to 50,548: only Yoshi under a frame's time. Not run on a WonderSwan |
| 167 | gbswan: Super Mario Land's picture jumping sideways for an instant (Gio, patch 166 in nileswan-medem): its status bar's STAT interrupt was lost when a timer interrupt came just before it; events that are due are now done before the next is looked for. Speed, on a new set of eight scenes in play (the three slow ones of the old set and five ROMs of his: `GB-IMPLEMENTATION.md`): returns look their block up in two small tables in the console's RAM first; loops that only copy or fill are done at once; reads from ROM through a pointer are in line; `CP (HL) / JR` waits are passed over; code in RAM that changes is kept in each of its forms instead of being translated again every time (R-Type II 45,756 cycles a frame to 35,563, Donkey Kong Land 143,178 to 90,499), or run by the interpreter when it is made anew for every use; a block's places in the table are spread (Donkey Kong Land forgot all its blocks with the table an eighth full); cartridge register writes that change nothing cost little. Castlevania II 60,906 to 58,256, Batman 61,353 to 58,734, Super Mario Land 2 53,013 to 51,606, Race Drivin' 151,752 to 142,393, X 111,375 to 114,516 (slower: see there), ZAS 67,813 (51,430 is full speed): only R-Type II is under a frame's time. Not run on a WonderSwan |
| 168 | gbswan: R-Type II's status bar jumping sideways (Gio, patch 167 in nileswan-medem; not the fault 167 guessed): a wait for the horizontal blank begun in the vertical blank was passed over in one step, to the next frame's first line, and the vertical blank's interrupt taken only there; every dozen frames the game had not set its line interrupt in time. No wait is now passed over beyond the next event (`GB-ABI.md`, "Time"): the bar is right in 60 frames of 60 in `gbrun.py`. Code in RAM that is made anew for every use goes straight to the interpreter (Donkey Kong Land 90,561 cycles a frame to 89,045). The others cost up to 2% more (X 116,665, Race Drivin' 143,685, ZAS 68,549, Castlevania II 58,583, Batman 58,986). Not found: R-Type's top row of tiles wrong at a point of its first level, Donkey Kong Land's Kong broken at times (`GB-IMPLEMENTATION.md`). Not run on a WonderSwan |
| 169 | gbswan: R-Type II's status bar, the rest of it (Gio, patch 168: the bottom still flickering): RETI went on by the return cache without a look at the interrupts waiting, so the STAT interrupt behind the vertical blank's, where the game sets its line interrupt, was at times taken a frame late. RETI now leaves through the dispatcher when one waits. And speed for the slow games, cycles a Game Boy frame in `gbrun.py`: ZAS's logo 159,543 to 72,387 (a line that changes only the scroll is a short list entry; SCY, SCX, BGP, WY and WX written by a routine of the recompiler's), Race Drivin' 143,685 to 109,938 (operands of code in RAM that the game changes read at run time, not a new form each time; "RAM on" written again with the same value returns at once), X 116,665 to 92,239 (the quick ROM bank write also for MBC2), Donkey Kong Land 89,045 to 85,404. Not run on a WonderSwan or in nileswan-medem |
| 170 | gbswan, the slow games again. Race Drivin' has more code than the recompiler has room for (1,737 blocks, 460,000 bytes, in its first twenty seconds; then everything is forgotten and translated again, five seconds at a frame or two a second): a block now ends where another already starts (from its sixth instruction), not with a second translation of the same code: 357,000 bytes at the same point. Code in RAM compared in one pass of the plugin's own routine, not by the C library first; `op_flags` from a table; a pointer whose high byte the block sets (`LD H, n`) read and written with nothing checked. Cycles a Game Boy frame in `gbrun.py`: Donkey Kong Land 85,404 to 78,192, Race Drivin' 109,938 to 104,873, the rest 0 to 2% less. Tried and left out: the tiles written in line, a compact translation after the room runs out (17% slower). Not run on a WonderSwan or in nileswan-medem |
