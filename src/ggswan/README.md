# ggswan: Game Gear on the WonderSwan Color

ggswan is SwanShellDX's Game Gear plugin. It runs Sega Game Gear games
(`.gg` files) on a WonderSwan Color with a nileswan cartridge. The Z80 is
emulated by a dynamic recompiler (Z80 to V30MZ code), with the original
interpreter kept for the parts that need it. The Game Gear video memory is
mapped onto the WonderSwan's own tile and screen hardware.

Status: fairly stable (patch 256). Tested in the nileswan-medem emulator,
with the host test tools and, since 2026-11-16, on a WonderSwan (a
SwanCrystal with a nileswan): see "On the console" below.

## Installation and use

1. Copy `PLUG_GG.BIN` to the `NILESWAN` folder of the microSD card
   (`/NILESWAN/PLUG_GG.BIN`). The SwanShellDX build puts it in
   `dist/NILESWAN/`.
2. Put the `.gg` files anywhere on the card.
3. Choose a `.gg` file in the SwanShellDX menu: the menu loads the
   plugin, which loads the game (up to 1 MiB) and starts it.

The start screen shows a few lines: the game's name and size, the card,
the ROM's banks, the CPU mode, and the start-up times in milliseconds
(`SD` the card's mount, `ROM` the ROM's read, `INIT` the rest). It stays
while a key is held (hold one at the launch to read it); otherwise the
game starts at once. `PATCH` and a number at the top right is the
plugin's patch number.

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
| START + A, held half a second | **The state kept** (patch 187): the game as it is at that moment, in memory and in a file next to the ROM (`.st0`); the sound stops for the second or two the file takes. `SAVE` in the left border when the numbers are shown |
| START + B, held half a second | **The state brought back** (`LOAD`; `NONE` when none has been kept), also after the console has been off. What was translated is translated again: a pause of a moment |
| START + Y1 (Y up), held for a second | **Back to the menu** (the cartridge RAM is then saved: see below). |
| START + Y3 (Y down) | **The on-screen menu** (see below): the game stops while it is open. |
| Y4 (Y left) held, then START | **The numbers in the borders**, off and on (patch 151). (Y4 first: START alone is the game's Start button.) |

## The on-screen menu (START + Y3 (Y down))

In ggswan, smsswan, sgswan and sfswan (gbswan has its own, below). X1 (X
up) and X3 (X down) choose a line, A opens a page, A, X2 (X right) or X4
(X left) change a value, B goes back and closes the menu (START + Y3
closes it too). The game stops while the menu is open.

| Page | Lines |
|---|---|
| VIDEO | FRAMESKIP (OFF, AUTO, 1, 2, 3, 5); STRETCH (smsswan, sgswan, sfswan: the whole picture squeezed into the 144 lines); PROFILER (OFF, FPS, ON: the numbers); SPRITE LIMIT (sgswan, sfswan: four sprites to a line, as the chip); GAME FPS, the game's frames in the last second before the menu opened (patch 254) |
| AUDIO | VOLUME (0-8), SOUND (on, off) |
| STATES | SAVE, LOAD (not in sfswan) |
| OPTIONS | SETTINGS FOR: ALL GAMES or THIS GAME; RESET TO DEFAULTS |
| ABOUT | The plugin and its patch number |

The settings are saved when the menu closes, if something changed: for
all games in `/NILESWAN/PLUG_GG.CFG` (`PLUG_SMS.CFG`, `PLUG_SG.CFG`,
`PLUG_SF.CFG`), or for one game in a `.cfg` file next to its ROM. At the
start a game's own file is used if there is one, else the plugin's.

## Saves

Games that save to their cartridge RAM (battery saves) are saved as
`.sav` files next to the `.gg` file, as SwanShellDX does for WonderSwan
games. When a `.gg` file is launched, the menu loads its `.sav` (32 KiB,
created empty the first time) into the cartridge's SRAM, where the
plugin keeps the Game Gear cartridge RAM. When the menu starts again
(START + Y1 (Y up) held for a second in the game, or the console turned
off and on), it writes the SRAM back to the `.sav` file, with the
backups set in the menu's settings.

A state (START + A, patch 187) is something else: the plugin's whole
memory at that moment, in a `.st0` file next to the ROM. It holds the
cartridge's RAM as it was, so bringing a state back undoes a save the
game made after it. A `.st0` file is that build's of the plugin only:
another patch number does not read it. ggswan, smsswan and sgswan have
states; sfswan has not.

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
- Background priority only with Y1 (Y up) at the start (see above);
  smsswan has it always, except while the numbers are shown.
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

## gbswan: the Game Boy plugin

`PLUG_GB.BIN` (`make -f Makefile.ggswan GB=1`), `.gb` files, sources of
its own in `gb/`. The documents above say how it is made; here, what
the keys do.

| Keys | Function |
|---|---|
| X1-X4, A, B | The Game Boy's pad, A and B |
| START | START. It reaches the game a tenth of a second after it is pressed (pressed and let go sooner, it is pressed for the game then): pressed with another key within that time it is not the game's |
| Y3 (Y down), Y2 (Y right) | SELECT |
| START + A, held half a second | **The state kept**: the game as it is at that moment (`SAVE` at the top of the left border) |
| START + B, held half a second | **The state brought back** (`LOAD`; `NONE` when none has been kept). A pause of a moment follows, and others as the game goes on: what was translated is translated again |
| START + Y1 (Y up), held for a second | Back to the menu |
| START + Y4 (Y left) | The numbers in the left border, on and off |
| START + Y3 (Y down) | The on-screen menu (patch 229): VIDEO (FRAMESKIP, PROFILER), AUDIO, STATES, OPTIONS (settings for all games, `/NILESWAN/PLUG_GB.CFG`, or for this game), ABOUT |

The state is kept in the cartridge's PSRAM and, since patch 182, in a
file next to the ROM with `.st0` for its extension, written when the
state is kept (a second or so, without sound; `FILE` in the border if
it could not be written - the state is then in memory only) and read
when the game is started: START + B works after the console has been
off. Neither is the `.sav` file or the SRAM the menu writes to it: the
game's own saves are as they were. A state holds the cartridge's RAM as
it was: bringing one back undoes a save the game made after it.

A `.st0` file is the plugin's own memory, so it is that build's of the
plugin: with another `PLUG_GB.BIN` it is passed over (`NONE`).

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
| START + A, held half a second | **The state kept** (patch 187): the game as it is at that moment, in memory and in a file next to the ROM (`.st0`); the sound stops for the second or two the file takes. `SAVE` over the side of the picture when the numbers are shown |
| START + B, held half a second | **The state brought back** (`LOAD`; `NONE` when none has been kept), also after the console has been off. What was translated is translated again: a pause of a moment |
| START + Y1 (Y up), held for a second | Back to the menu |
| START + Y4 (Y left) | **The numbers** (FPS, the recompiler's, the profile) over the sides of the picture, on and off (patch 151). They use screen 2, as the background's priority does: with the numbers on (also PROFILER FPS in the menu) every sprite is in front of the background (Micro Machines' boat over the bridge). To read the FPS with the priority on, see GAME FPS in the menu's VIDEO page |
| START + Y3 (Y down) | **The on-screen menu** (above). The **whole picture** (an experiment, VIDEO > STRETCH): the 192 lines squeezed into the 144 of the screen, one line in four left out. It takes about a quarter of the CPU, small text loses lines, and sprites keep their height (the pieces of a tall figure overlap). Sideways the view is still 224 columns. |
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
| START + A, held half a second | **The state kept** (patch 188), as ggswan's: in memory and in a `.st0` file next to the ROM |
| START + B, held half a second | **The state brought back** |
| START + Y1 (Y up), held for a second | Back to the menu |
| START + Y4 (Y left) | **The numbers** over the sides of the picture, on and off (patch 151; as Y1 (Y up) held at the start) |
| START + Y3 (Y down) | **The on-screen menu** (above). The **whole picture** is its VIDEO > STRETCH (patch 150), as smsswan's: the 192 lines squeezed into the 144 of the screen, one line in four left out; sprites keep their height (the pieces of a tall figure overlap by a quarter). The view still moves sideways. A screen of more different characters than there are tiles for (558) shows some of them empty |
| START + Y2 (Y right) | **Four sprites to a line**, on and off (off at the start; also the menu's VIDEO > SPRITE LIMIT; it was START + Y3 (Y down) until patch 147). The SG-1000 draws four sprites on a line and leaves the others out, which is the flicker of its games; the WonderSwan can draw them all, and does. On: as the SG-1000 |
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
| START + Y3 (Y down) | The on-screen menu; the whole picture squeezed is its VIDEO > STRETCH, as sgswan's (not in text mode: Disk BASIC's screen stays a view) |
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

### On the console

Until 2026-11-16 everything was run in nileswan-medem and in the PC
tools. On a SwanCrystal with a nileswan (Gio's), measured with the test
in gbswan (`make -f Makefile.ggswan GB=1 GBX=-DGB_HWTEST`: five pages
before the game, START for the next) and with ggswan's X3 (X down):

- A write to the linear area (segments 4000h-F000h) does nothing. PSRAM is
  written through the RAM window (segment 1000h) with port CEh bit 0 set,
  at bank ((port C0h << 4) | segment's top digit) & the ROM banks' mask
  (port E4h: C0h was 3Fh, the mask 01Fh, so segment 4000h is bank 14h).
- The tile bank bit of a screen entry works in the 2 bits a pixel color
  mode. A program is started in the mono mode (port 60h read 0Ah, or 00h
  with the menu's "fast SRAM" on); the first screen drawn right after
  setting port 60h to 80h showed its tiles read another way, the second
  was right. Set at the start of the vertical blank and again two frames
  later, the first is right too.
- Code runs exactly as fast from PSRAM (the linear area) as from the
  internal RAM, in all fifteen tests.
- What things cost, in cycles (a line of the display is 256): a NOP 1;
  `mov al, [bx]` and `mov ax, [bx]` 2; `mov ax, [bx + 1]` (a word at an
  odd address) 3.5; `mov al, es:[bx]` in the cartridge's RAM 3.5, and 2.5
  with its wait off (port 60h bit 1); PUSH AX and POP AX 3 the pair;
  `shl ax, 4` 3; a short jump 4 to an even address and 5 to an odd one, a
  near jump 5 and 6; a near call, its return and a short jump 20; `in ax,
  D2h` and `out D2h, ax` 10, and 8 with the ports' wait off (port 60h bit
  3); sixteen bytes like translated code (`add bx, dx` / `mov al, [bx]` /
  `mov [m], al` / `sub word [m], 5` / `jle` / `mov ah, al`) 15. So: an
  instruction costs the larger of what it takes to run and half a cycle a
  byte of its code, plus a cycle for each access to data (two for a word
  at an odd address, one more in the cartridge's RAM with its wait on),
  plus 3 for a jump taken (4 to an odd address). The tables of cycles the
  emulators and `gbrun.py`/`wsrun.py` use (an instruction 1 to 3, a jump
  4) are about a third short of this for translated code.
- Frames a second, ggswan, Sailor Moon S: title 45 (59 in nileswan-medem),
  in the game 37 (50). The menu's "fast SRAM" (port 60h bits 1 and 3
  clear) made no difference to be seen there and nothing went wrong with
  it on.
- From patch 185 `gbrun.py` and `wsrun.py` count these costs
  (`GBRUN_HW=1`, `WSRUN_HW=1`). The same scenes come to 1.34 (Super Mario
  Land 2) to 1.41 (Donkey Kong Land, X) times the cycles of the old count,
  which is the console's three quarters of the emulator's speed; ZAS in
  play is 36 frames a second there and 32-39 in Gio's photographs. Where
  the time goes does not change much (the console is slower nearly evenly):
  what the new count shows is what it goes on - in the translated code of
  Super Mario Land 2, 17% unconditional jumps, 16% instructions longer to
  fetch than to run, 14% accesses to data, 7.5% counting the Game Boy's
  cycles at each block's end, 5% storing N, 3% jumps to odd addresses.

### Patches 90-193

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
| 171 | gbswan: leaving a block without the dispatcher. What most leavings are for (the event that is due, an interrupt to take, HALT's wait, on to the next block) is done by a routine in each code segment (`s_exit`), not in C; HALT is translated; a write to a sound register through a pointer is a byte moved; the dispatcher fills the two small tables; `gbf_hw.c` works out less for a STAT interrupt on LYC alone (asked for in patch 170's report: Batman's water, X's title at 8 frames a second, Castlevania II). Cycles a Game Boy frame in `gbrun.py`: X's title 343,316 to 232,860; Batman 58,191 to 51,752, Castlevania II 56,998 to 51,351, X 92,045 to 86,054, Donkey Kong Land 78,192 to 74,851, the other four 3 to 6% less. Not run on a WonderSwan or in nileswan-medem |
| 172 | gbswan: Donkey Kong Land's Kong no longer broken up. The game draws his next pose into a second set of tiles while the first is still on the WonderSwan's screen (which shows the patterns as they are, a frame behind the program); when a program is seen to write patterns of sprites on the screen while the picture is drawn, those writes now reach the tiles with the picture of the frame after (`gb_video.c`, "the patterns a frame late"): of 186 pictures in a row with Kong moving, 121 wrong before, none now; that scene 5% slower. `schedule()` in assembly and LYC written without C, for programs with an interrupt at a line or at every line (asked for in patch 171's report: Batman's and Castlevania II's water, X's fades): X's title 232,860 cycles a frame to 202,664, its fade 234,546 to 208,785. Not run on a WonderSwan or in nileswan-medem |
| 173 | gbswan: two more things out of C. The event in the middle of a frame (the timer's and LYC's: `gbf_event_asm`), with a short way through `schedule()` for LYC's interrupt alone; and code in the work RAM or the high RAM entered from the routine that looks blocks up (`s_lookup_ram`: its bytes compared there), not through the dispatcher. Cycles a Game Boy frame in `gbrun.py`: X's title 202,664 to 178,085, its fade 208,785 to 188,889; Donkey Kong Land 78,869 to 73,897, R-Type II 31,723 to 29,652, Batman 51,612 to 49,933, Race Drivin' 102,821 to 100,014, the other four 1 to 1.6% less; the same memory everywhere. The translator profiled by function and by instruction: no routine in it to convert, its time is spread evenly over the two long functions. Not run on a WonderSwan or in nileswan-medem |
| 174 | gbswan: the translator itself, a tenth to a fifth faster, the code it makes unchanged (the same blocks at the same places, the same memory in every scene). Profiled line by line (`gbrun.py` `GBRUN_FUNCDUMP`, the object linked with `-g`): three fifths of `translate_block`'s own time was what it does before each instruction. One table by opcode (flags, length, cycles, "the interpreter's") read once in the first pass and kept for the second, for three far tables and a switch of seventeen compares; a block put together in the console's RAM and copied to its segment once; tests and reads done once. Cycles a block: `translate_block` 5,682 to 4,548 at Race Drivin's start, 4,784 to 3,771 in Batman, 4,530 to 3,622 in Donkey Kong Land, 4,193 to 3,286 in X; `scan_block` 5,517 to 5,121, 5,378 to 5,005, 4,351 to 4,034, 3,924 to 3,590. Race Drivin's start, cycles a frame: 696,375 to 628,159. Not run on a WonderSwan or in nileswan-medem |
| 175 | gbswan: what the hottest blocks of the slow scenes showed. Super Mario Land 2's map (37 frames a second on Gio's screen) reads its data through a routine that sets the ROM bank and sets it back, 160 times a frame, each through the general write routine: `LD (nn), A` to the bank register now calls a routine of its own (`s_bankx`, `s_bank0`), and writes to the cartridge's RAM are looked for sooner: 84,014 cycles a frame to 74,163. ZAS: `ADD HL, rr` was seven instructions to keep flags nothing read; `add bx, rr` when H and C are set again first: 64,404 to 60,174. `s_wrt` tries the patterns first, then the maps. The translator asks the table whether a block starts at the next instruction only when a bit says one may (`DRC_HAS`): Race Drivin's start 628,159 to 603,032. Race Drivin' 99,125 to 93,065, X 86,072 to 82,479, Donkey Kong Land 72,329 to 69,631, Batman 49,528 to 48,029. Not run on a WonderSwan or in nileswan-medem |
| 176 | gbswan: the work RAM's echo put right, and four of the second opinion's small proposals. The code made took C000h-FDFFh as one piece at `[address - 6000h]`: E000h-FDFFh, the echo, was the console's 8000h-9DFFh, where the translator has had its buffer and its bitmap since 174 and 175; the range in line now ends at DFFFh and the routines do the echo (a stack in the echo is still not supported: the stack's own path is left as it was for the games whose stack is in the high RAM). A write the block knows is to the video memory calls `s_wrv`, with nothing after the call (11 bytes for 37); INC and DEC of BC, DE, HL are the V30MZ's one-byte ones when the flags are in AH or dead; INC and DEC (HL) work in place when HL is known to be in the work RAM; `ADD HL, rr` with only C needed is three instructions. Under 1% in the scenes (Batman and ZAS -0.7%, Race Drivin' -0.3%), the same memory everywhere; 24 tests pass. Not run on a WonderSwan or in nileswan-medem; no test ROM for the echo. |
| 177 | gbswan: Donkey Kong Land's Kong still broken up in some frames, most when he jumps (Gio, on patch 176). The game keeps two sets of tiles for him: it draws the next picture in the set that is not shown and then points his sprites at it. The patterns were kept a frame late (patch 172) only when the last one written was a tile of a sprite of the last frame's picture, which with two sets happens only now and then, and the keeping ended after 120 frames. Now the tile is also looked for among the sprites the program has just set (the OAM), and once on, the patterns stay late until the screen is turned off or a frame writes more tiles than can be kept. In a longer walk with jumps: 27 wrong pictures of 756 to 3 of 760 (the first picture drawn after the level's start, one Game Boy frame). The lines of the text shown at the start have a row between them (they touched). The other scenes within 0.3%. Not run on a WonderSwan or in nileswan-medem. |
| 178 | gbswan: the translator again, by its source lines' cycles. In the pass that writes the code, three fifths of the time were what is made ready for every instruction before anything is written: its three bytes read through a function, what is known of BC, DE, HL fetched, the EI before it looked for. In the pass that looks the block over, a quarter were compares for a few rare instructions made for all of them. Now: the bytes straight from the buffer; the rare ones marked in the opcodes' table and passed over with one test; what is known of the pointers neither cleared nor fetched before the block's first `LD rr, nn`; the hash for "a block already starts here" with the key's part ready. Race Drivin's start 605,626 cycles a frame to 522,307 (-13.8%), Batman's level 127,900 to 118,142, the same blocks and the same memory. In the game: Donkey Kong Land -2.7%, Race Drivin' -1.1%. Also measured, and not done: code interpreted the first time it is met (the numbers are in `GB-IMPLEMENTATION.md`, patch 177). Not run on a WonderSwan or in nileswan-medem. |
| 179 | gbswan: sprites in pieces for a frame when their picture changes - the coin that turns in Donkey Kong Land's "Select Game", its enemies (Gio, on patch 178 in nileswan-medem). Two things, neither seen by `gbrun.py`, which drew the sprites from the table as it was at the picture's end: the WonderSwan takes the table at line 142, for the next frame (`GBRUN_SPRLATCH=1` now does the same). The game copies its sprites to the OAM at line 128 of every frame, and the frame's sprites were set from the OAM at the frame's end: the next frame's sprites on this frame's patterns. What the OAM was before a copy made while the picture is drawn is now kept and used. And a frame that ended just before line 142 had its table half taken, or taken a frame after its list: such a frame now waits for the vertical blank. The coin against the reference's poses: 25 pictures of 131 wrong, then none of 172. Donkey Kong Land 0.9% slower in the game, the others the same. Not run on a WonderSwan or in nileswan-medem. |
| 180 | gbswan: the translator's first pass in assembly. `scan_block`'s loop, for the instructions it has nothing of its own to do for (most), is `gbd_scan_fast` in `gb_asm.s`; the rare ones, and those that change what is known of BC, DE, HL, are still the C's, one at a time; the flags needed before each instruction are `gbd_scan_live`. The code made is the same. Race Drivin's start 522,307 cycles a frame to 489,794 (-6.2%; 605,626 in 177), Batman's level 118,142 to 112,967. Tried and taken out: the same for the second pass's simplest instructions (LD r, r, the ALU with a register, LD r, n) - right, and slower (499,003): going from the C to the assembly and back for each run of them cost more than they saved. Not run on a WonderSwan or in nileswan-medem. |
| 181 | gbswan: a state kept and brought back, START + A and START + B held half a second (Gio's request): the whole internal RAM and the cartridge's RAM copied to two PSRAM banks the recompiler does not use, between two frames; on the way back the ROM's banks are set again and everything translated is forgotten. In memory only for now (lost on leaving the game); the `.sav` file is not touched. START reaches the game a tenth of a second late, so that a key pressed with it does not pause the game first. Super Mario Land 2 slow at times (Gio, on 180: 41 frames a second): its levels and its map are read a byte at a time through three routines that set the ROM bank, copy a byte and set the bank back, some 190 times a frame where a level scrolls; the seven instructions are now one routine (`s_far`) that moves no bank unless the byte is in the other bank, and then only the ROM window for the read: the slow stretch 70,387 cycles a frame to 63,279, the map 73,857 to 64,143. INC and DEC of an 8-bit register whose Z and H are set again before they are read are the V30MZ's and nothing else. Not run on a WonderSwan or in nileswan-medem. |
| 182 | gbswan: the state as a file on the card. START + A also writes it next to the ROM, `.st0` for the extension (16 bytes of header, the internal RAM from 0040h, the cartridge's 32 KiB: 98,256 bytes), and the game's start reads it if it is there and is this build's: a state survives the console being turned off. A fault of 181: a state kept in one game was still in the PSRAM when another was started, and START + B there brought the first game's memory back; the mark is now cleared at the start. In `gbrun.py`, which stands in for the card: kept in one run, brought back from the title screen of a fresh one with the same checksums, and the game goes on. Not tried with a card: nileswan-medem or the console. |
| 183 | gbswan: Super Mario Land 2's slow stretches again. After 181 the next thing in them was a loop that clears a row of the level in the cartridge's RAM, `LD (HL+), A / DEC D / JR NZ`, each byte the general write. The routine there was for that loop with B (`s_fill0`) is also for D (`s_fill1`), and both also fill the cartridge's RAM when it is plain RAM. The slow stretch 63,279 cycles a frame to 59,021 (70,387 in 180: 73% of full speed to 87%), the map 64,143 to 57,760 (73,857 in 180), the same memory; the four slowest games as they were. Not run on a WonderSwan or in nileswan-medem. |
| 184 | gbswan runs on the console. First run on a WonderSwan (Gio's SwanCrystal, 2026-11-16): the other four plugins ran, gbswan showed a screen of wrong tiles and then stopped. Two things the emulator and `gbrun.py` had let through: a write to the linear area (where the translated code and its table are read and run) does nothing on the cartridge, and the 2 bits a pixel mode set coming from the menu was not in effect for the first screen. Now every such write (the table, the code, the links between blocks, a state kept) goes through the RAM window with the PSRAM mapped there, as ggswan's always did, and the mode is set at the vertical blank, twice. `gbrun.py` stops at a write to the linear area. A test of the hardware in the plugin (`GBX=-DGB_HWTEST`): what works, and what instructions cost. Measured: code runs as fast from PSRAM as from the internal RAM; the console is at about three quarters of the emulator's frames a second (Sailor Moon S: 37 against 50), because an instruction costs at least half a cycle a byte of its code and a cycle for each access to data, a taken jump 3 more (4 to an odd address), a call and its return about 16, a port 8-10: "On the console" below. Also: the slow ways of PUSH and POP out of line (ZAS -2.4%, X -2.4%, Race Drivin' -0.7%, Super Mario Land 2 +0.2%, Donkey Kong Land +1.9%, in `gbrun.py`'s old cycles); ggswan's X3 (X down) test on a screen of its own. On the console: the games start, a state is kept and brought back (Gio). |
| 185 | The console's cycles in the PC tools, and the first things they showed. `gbrun.py GBRUN_HW=1` (and `wsrun.py WSRUN_HW=1`) count an instruction as measured on the SwanCrystal in 184; `GBRUN_KIND=1` says what the translated code's cycles go on. Then, each measured alone with that count: (1) a block joined to one a conditional jump can reach is "jg there; jmp short slow" in place of "jle slow; jmp near there" (four cycles for six); (2) every block starts at an even address (a jump to an odd one is a cycle more); (3) N is not stored before a jump to code that writes it before reading it (`n_dead`: three cycles and a half at most exits), nor before a write through a pointer that stays in the work RAM; (4) a program that keeps its variables in the cartridge's RAM and goes to them through pointers (Super Mario Land 2: 135 reads and 58 writes a frame through `s_rd` and `s_wrt`, 57 cycles each) gets that access in its blocks' own code (30 cycles), once the routines have counted 3000 such accesses in 32 frames - everything is then translated again, once; (5) POP with the stack pointer in the ROM (Donkey Kong Land draws from tables so, 64 a frame) reads its word from the ROM's segment, not through two calls of `s_rd`, and POP AF has the work RAM's case in itself; (6) the slow ways of PUSH and POP that 184 put apart go after the jump a following access already has, not after it under a jump of their own (a read after a PUSH ended with two jumps, ten cycles for four). Cycles a Game Boy frame, the console's count, 184 -> 185: Super Mario Land 2 62,726 -> 56,177 (-10.4%), Donkey Kong Land 97,300 -> 91,629 (-5.8%), ZAS 80,504 -> 77,203 (-4.1%), X 113,720 -> 108,368 (-4.7%), Race Drivin' 124,696 -> 122,135 (-2.1%); the Game Boy's memory the same in all five, the 24 tests pass. Tried and left out: the read's and the write's slow ways apart, and the routine's call before the ROM's read (each worse in Super Mario Land 2, by 0.7% to 2.2%). Not run on the console yet. |
| 186 | PUSH AF and POP AF in the block's own code. Each was a routine (`s_push_af`, `s_pop_af`: F made from AH and `drc_n`, or back) that then went through the push's or the pop's: about 54 and 48 cycles on the console, half of them the call, the jump between the two and the return. ZAS and X do some ninety a frame (their interrupt routines and their drawing): the routines were 7-8% of a frame there. Now the pop or the push is the block's usual one and the dozen instructions that turn the flags are after or before it; N is a constant there when the block knows it. Also the sprites' sorting loop tighter (a key below all the others ends the moves, one jump a move, no segment prefixes): ZAS -0.5%. Looked at and left: the Game Boy's cycle count in a register (DI is the temporary of nearly every routine: one to three cycles more at each of their calls for three less at each block's end); the sprites' order of two frames ago (ZAS moves its sprites round the OAM every frame, 195 moves a frame to sort them, and that order is no better); skipping the sprites when the OAM has not changed (never in ZAS; 35% of the frames in Super Mario Land 2, where the sprites are 5% of a frame). Cycles a Game Boy frame, the console's count, 185 -> 186: ZAS 77,203 -> 71,649 (-7.2%), X 108,368 -> 100,884 (-6.9%), Donkey Kong Land 91,629 -> 90,960 (-0.7%), Super Mario Land 2 56,177 -> 55,939 (-0.4%), Race Drivin' 122,135 -> 119,916 (-1.8%); the Game Boy's memory the same, the 24 tests pass. Not run on the console yet. |
| 187 | ggswan and smsswan: a state kept and brought back, as gbswan's (Gio's request): START + A and START + B held half a second. The whole internal RAM (the Z80's memory, the video memory, the screens, every variable) and the 64 KiB of SRAM behind the RAM window go to the two PSRAM banks after the block table's (linear segments B000h and C000h), and to a file next to the ROM, `.st0` (131,072 bytes), read at the start if it is this build's and this plugin's. Coming back: everything but the stack, the ROM's banks set again, everything translated forgotten, the sound's registers written again. On the Game Gear START reaches the game after eight frames of it alone, or as a short press when let go sooner (it is the game's Start, and was sent as soon as pressed: with A or B a moment later the game would have paused first); on the Master System START with A or B is not a Pause when let go. In `wsrun.py`, which now stands in for the file: Sonic (Game Gear) kept at frame 338 and brought back at 558 has at frame 700 the work RAM of a plain run at 480, and so has a Master System game; kept in one run and brought back at frame 138 of a fresh one, the work RAM at 300 is the plain run's at 500. Not sgswan (its screen's tables are in a PSRAM bank) nor sfswan (its memory is in PSRAM): next. Not run on the console or in nileswan-medem; the note on the screen was not looked at. |
| 188 | A fault of 187, and sgswan's states. 187 copied the SRAM and read the state's file through 2 KiB of the internal RAM at E000h, taken for free between frames: the table of the Game Gear's colors is at E600h and the sound's samples at E400h. Keeping a state put them back; the file read when the game starts did not, so with a `.st0` file there and not brought back the colors could be wrong for the whole game. Now the copies go through 512 bytes of the plugin's own, and FatFs's volume is left out of what is brought back rather than saved on the stack. sgswan keeps a state too: its second bank is the bank of the screen's tables and the ROM's bank from 8000h (the RAM of a cartridge that has some, the TMS9918A's video memory), and the RAM window is put back as it was. In `wsrun.py` (`WSRUN_STCMP=1`): right after a state is brought back, the internal RAM but the stack, both PSRAM banks and the SRAM are as they were when it was kept; ggswan's and smsswan's games then go on as a plain run does (the same checksums as 187's); sgswan's Gulkave does not - but there a single flush of the code cache at the same frame, in a plain run, changes its course as much (`WSRUN_POKE1`): something of sgswan's timing depends on how the code is cut into blocks, to be looked at. sfswan: no states - written (a third bank for its 64 KiB of RAM), its first try ran over its 4 KiB of stack, and Gio can do without. Not run on the console. |
| 189 | gbswan, the three things ChatGPT proposed for the video, each measured (the console's count). (1) and (3), not rebuilding the sprites when the OAM has not changed: `GBRUN_OAMSAME=1` counts it - never in ZAS in 160 frames, 11 times in Donkey Kong Land, 42 in 120 in Super Mario Land 2's scene. Knowing it costs either a compare and a copy of the OAM every frame (about +1% in Super Mario Land 2, -0.6% in ZAS) or a flag set wherever the OAM can be written (the DMA, the write routine, the C side, the copy and fill loops, addresses the block knows, pushes), with stale sprites for the one missed: left out. (2), the patterns kept for the next frame (`gbv_tq_*`): in Donkey Kong Land the cost is not in `gbv_tq_new` (1.3% of a frame) or `gbv_tq_copy_asm` (0.7%) but in the write of each byte, 125 a frame, through `s_wrt`, `wrt_pat_late`, `wrt_tq` and back - some 70 cycles. The tile being the one written last, as it is fifteen times in sixteen, is now done at `s_wrt`'s head: Donkey Kong Land 90,960 -> 89,530 (-1.6%), X 100,884 -> 100,745 (-0.1%); the Game Boy's memory the same, the 24 tests pass. Not run on the console. |
| 190 | gbswan, the Game Boy's cycles left to the next event in a register while translated code runs. Gio asked for another profile now that the console's costs are known: in ZAS a block's end, `sub word [gbf_cyc], n`, was run 2,457 times a frame at four cycles and a half - 13% of the translated code's time. Now `sub di, n` (one and a half): DI holds the count from `gbd_enter` on; `gbf_cyc` is written before a routine that needs it and DI taken back after. The routines called most leave DI alone (`s_rd`, `s_wrt`, `s_wrv`, `s_pop`, `s_rdt`, `s_bank0`, DAA, P1, INC/DEC (HL); LY and STAT are worked out from DI) and write `gbf_cyc` only on their long ways, so nothing is added around a read or a write; the exit to an address not known (`s_exit_dyn`) tests DI and keeps it in ES while it looks in the tables. PUSH AF and ADD HL, which had DI as their scratch, use SI. Console count, a Game Boy frame: ZAS 71,582 -> 70,562 (-1.4%), Super Mario Land 2 55,939 -> 55,044 (-1.6%), X 100,745 -> 99,347 (-1.4%), Donkey Kong Land 89,530 -> 89,415 (-0.1%), Race Drivin' 119,916 -> 119,800 (-0.1%); the memory at the end of each scene the same as before, 24 tests of 24. Less than the instruction counts promised (I had told Gio 8% for ZAS): the translated code is 1,836 cycles a frame shorter there, and some of it comes back in the routines - three cycles more at each exit through the tables to park DI, and the long ways of a write now pay the store and the load that the block paid. `gbrun.py`: `GBRUN_KIND` knows the new instructions; `GBRUN_STUBS=1` (how many times each routine is entered) is there but too slow to be of use. Not run on the console. |
| 191 | gbswan, the cartridge's RAM taken for open. After 190 Gio asked whether to go on this way at all; the answer was to measure every game there is here (two of twelve at full speed, five between 83% and 93%) and to try something larger, traces - but the profile for that showed first where Super Mario Land 2's time is: it keeps its variables in the cartridge's RAM, and every access there was `cmp byte [drc_ram_ok], 1` / `jne` / `mov es, [drc_ram_seg]` / the access / `jmp` over the other way, some 14 cycles for three; with the tests around an access through a pointer, half the translated code's time. Now a block is translated taking that RAM for what it is at the time, plain and enabled (`cart_sure`): no test and, for a known address, no other way; and ES is loaded once for accesses in a row (`es_ram`). The first time `gbf_plat_ram` finds the RAM otherwise, everything translated is forgotten and nothing is taken for sure again (`drc_cart_unsure`): a program that closes its RAM after every use pays one flush and runs as before. Console count, a Game Boy frame: Super Mario Land 2 55,044 -> 46,981 (-14.6%), under the 51,430 of full speed; the others as they were (ZAS 70,533, Donkey Kong Land 89,446, X 99,314, Race Drivin' 119,825: none of them goes to that RAM that way); the memory at each scene's end the same, 24 tests of 24. The forgetting itself is gone through by Mooneye's `mbc1/bits_ramg` and `mbc1/ram_64kb` (one each, and they pass); no game here closes its RAM while it runs. Not run on the console. |
| 192 | gbswan, an exit to an address found as the program runs (RET, JP (HL)) remembers where it went first. The larger attempt Gio agreed to after 190, measured before written: in Castlevania II the jumps between blocks that traces would remove are 4% of a frame, and `s_lookup`, which finds the block a RET goes to, 12.3% - 182 times a frame at 42 cycles on the console; and four exits in five go where that same exit went the first time (`GBRUN_DYNSITE=1`: 82% in Castlevania II and in Super Mario Land 2). So the exit is now `cmp si, (that address)` / `jne` / `jmp far (its block)`, some 12 cycles, with the tables for any other address; from a block in the first ROM area the second area's bank is compared too. The address, the bank's key and the jump are written by the dispatcher the first time the exit is taken (`s_dyn_first`, `drc_dlink`); the Game Boy's time is counted exactly as before. Console count, a Game Boy frame: Castlevania II 61,878 -> 57,844 (-6.5%), Super Mario Land 2 46,981 -> 44,596 (-5.1%), Super Mario Land 35,158 -> 32,334 (-8.0%), X 99,314 -> 95,005 (-4.3%), R-Type 58,978 -> 57,304 (-2.8%), Yoshi 58,206 -> 56,566 (-2.8%), Donkey Kong Land 89,446 -> 87,005 (-2.7%), R-Type II 35,697 -> 34,785 (-2.6%), ZAS 70,533 -> 69,193 (-1.9%), Batman 61,357 -> 60,387 (-1.6%), Race Drivin' 119,825 -> 118,276 (-1.3%), Balloon Kid 69,281 -> 68,659 (-0.9%). The memory at each scene's end the same, 24 tests of 24. Not run on the console. |
| 193 | gbswan, the sprites off the screen are no longer sorted. After 192 Gio asked what could still matter and proposed asking ChatGPT (reasoning High; the brief and its answer are in GB-IMPLEMENTATION.md). Its first proposal, the next block put right after the one that jumps to it, measured before anything was written: those jumps are 215 cycles of Castlevania II's 57,806, 0.4% - not done. (And a fault of mine found on the way: `gbrun.py`'s counts of how many times an instruction runs, the `xN` of `GBRUN_JMPS`, come out some nine times too many; the cycles are right. The estimates made from them - 190's above all - were wrong for that.) What the routines' profile showed instead: `gbv_sprites_asm` is 8.5% of that frame, and half of it the insertion sort's moves, 165 a frame - the game has four or five of its forty sprites on the screen and moves them round the OAM, and each one that came onto the screen was moved past all those that were not, which were sorted too, last. Now only the ones on the screen are sorted; the others are written to next frame's order as they come. The WonderSwan's sprite table is the same at every frame (`GBRUN_SPRHASH=1`: one checksum of all of them, equal for the two builds in three games). Console count, a Game Boy frame: Castlevania II 57,806 -> 54,689 (-5.4%), Super Mario Land 32,334 -> 31,288 (-3.2%), R-Type II 34,785 -> 33,844 (-2.7%), Super Mario Land 2 44,550 -> 43,609 (-2.1%), R-Type 57,304 -> 56,498 (-1.4%), Batman 60,387 -> 59,591 (-1.3%), Yoshi 56,566 -> 55,882 (-1.2%), Balloon Kid 68,659 -> 67,897 (-1.1%), ZAS 69,193 -> 68,626 (-0.8%), Donkey Kong Land 87,005 -> 86,522 (-0.6%), Race Drivin' 118,276 -> 117,951 (-0.3%), X 95,005 -> 94,870 (-0.1%). 24 tests of 24, the memory at each scene's end the same. Not run on the console. |

### After patch 193

One line per commit, oldest first (the subject of each; the commits
say more). Some have no patch number in their subject.

- menu: the thumbnail comes up and goes away faster
- menu: the thumbnail after a quarter of a second
- menu: the wallpaper under the thumbnail comes back from the PSRAM
- menu: START + Y1 in a WonderSwan game goes back to the menu
- menu: the START+Y1 choice is kept in the settings file
- menu: START+Y1 only in Settings > System > Advanced
- menu: the game's sound is silenced when the menu starts
- bootstub: START+Y1 hook only in real padding; safer delay-loop check
- bootstub: START+Y1 hook reached by a far call through the bank's segment
- bootstub: flash kept awake whenever START+Y1 is on
- menu: START+Y1 patches from /NILESWAN/PATCHES, recognised by the footer
- menu: START+Y1 automatic patch moved from the bootstub to the menu, more forms
- menu: START+Y1 second read reuses the hook already placed
- menu: START+Y1 banked read without room calls a hook in a linear bank
- ggswan/smsswan/sgswan: cartridge windows back after the card is used
- menu: "Show save files" also hides the plugins' .sav and .st0
- gbswan: sound frame faster (Gemini GB-1, fixed)
- gbswan: s_wrt without dead code, DI kept on the quick paths (Gemini GB-4)
- gbswan: line and cycle from a remembered line, wait for STAT on DI (Gemini GB-3)
- gbswan: small cuts - ALU A,(HL) in the cartridge's RAM, one keypad scan, IME first (Gemini GB-6)
- gbswan: flags built only when read (Gemini GB-5)
- gbswan: a found exit jumps short when it can; ROM0 -> ROMX constant exits (Gemini GB-2)
- gbswan: the cartridge's RAM through HL tested first when HL points there (WIP-194 finished)
- ggswan: links within 127 bytes as "jg rel8", no needless relinking (Gemini Z-1)
- ggswan: BIT b,r / BIT b,(HL) as one TEST when only Z is read after it (Gemini Z-3)
- smsswan: Codemasters bank writes through the map routine (Gemini Z-4)
- ggswan: RET / JP (HL) inline cache without [z80.pc] on a hit, slot 0 -> slot 1 too (Gemini Z-5)
- sgswan: sprite collisions in 16 bits for magnification 1, no div in sg_line_entry (Gemini Z-6)
- sgswan: sprite cache without thrashing, sg_dirty skips non-BG pages (Gemini Z-7)
- sgswan: TMS9918A tile conversion in V30MZ assembly (Gemini Z-8)
- sgswan: name table update in V30MZ assembly (Gemini Z-9)
- ggswan: slow ways of write_byte, ix_ram, ix_write at the block's end; BIT b,(IX+d) for Z only (Gemini Z-10)
- ggswan: P/V overflow set in the cold tail, no taken jump when there is no overflow
- ggswan: rare paths in the cold tail (segment after INC HL, CRAM, full PSG queue, RMW outside work RAM), loops fall through (ChatGPT)
- ggswan: SBC/ADC HL and ADD IX with lazy flags, H only when read, more rare paths in the cold tail (Gemini)
- sgswan: dirty tiles of Graphics I/II in one V30MZ pass (Gemini G1, 4 fixes)
- ggswan/smsswan: V counter wait loop through an emitted routine, not the C helper (Gemini G2)
- ggswan: dispatcher hot path in V30MZ (quick re-entry, lookup, block setup and accounting) (Gemini G3)
- ggswan: name table rows and the backdrop's tile count in V30MZ, no 32-bit multiply in event_step (Gemini G4)
- ggswan: patch number 218 on the screen
- ggswan: translation with fewer checks per instruction, emitted code unchanged (Gemini G5a, G5b)
- ggswan/smsswan/sgswan/sfswan: on-screen menu, START + Y3 (first step: VIDEO, STATES, ABOUT)
- ggswan/smsswan/sgswan/sfswan: menu fixes after the first console test (patch 221)
- ggswan/smsswan: menu, the profiler switch moved screen 2 (patch 222)
- ggswan/smsswan/sgswan/sfswan: menu VIDEO, frameskip (patch 223)
- ggswan/smsswan/sgswan/sfswan: profiler, the frameskip under the FPS (patch 224)
- ggswan/smsswan/sgswan/sfswan: menu AUDIO, volume and sound on/off (patch 225)
- ggswan/smsswan/sgswan/sfswan: menu OPTIONS, settings saved per plugin or per game (patch 226)
- ggswan/smsswan/sgswan/sfswan: menu PROFILER OFF / FPS / ON (patch 227)
- plugins: start screen, the game's name and the size in KB (patch 228)
- gbswan: the on-screen menu, START + Y3; start screen title lines together (patch 229)
- gbswan: menu VIDEO frameskip; the backdrop back after the menu (patch 230)
- menu: "Last directory" also puts the cursor on the last file opened (patch 231)
- ggswan: event_step without divisions on the usual positions, next_event inline (Gemini G6a, patch 232)
- ggswan: gg_run_frame reads z80.cycles directly, pos_from_left inline (Gemini G6b, patch 233)
- ggswan: video_present, the diagnostics' timing only in diagnostic mode, no nested interrupt saves (Gemini G6c, patch 234)
- ggswan: the keys read once a frame (Gemini G6d, adapted to the menu, patch 235)
- ggswan/smsswan/sgswan/sfswan: the menu leaves the line interrupt on (patch 236)
- ggswan/smsswan/sgswan/sfswan: menu, the front tiles' switch kept across a change (patch 237)
- ggswan/smsswan/sgswan/sfswan: settings file, the cartridge windows back from the plugin's values (patch 238)
- gbswan: settings file, the cartridge windows back from the Game Boy's banks (patch 239)
- ggswan: event_step, a short path for the four usual events, the rest out of line (Gemini G7a, patch 240)
- ggswan: gg_run_frame compact, the per-line test mode out of line; gg_line_event tests the position first (Gemini G7b, patch 241)
- ggswan, gbswan: read the ROM straight into PSRAM; no fixed wait at start (patch 242)
- ggswan: event_step fast paths for line IRQ on, mid-VBlank stops, GG present-to-VBL (Gemini G8a, patch 243)
- ggswan, gbswan: back to the 8 KiB ROM loader through IRAM (patch 244)
- ggswan: show the start-up times on the start screen (patch 245)
- ggswan, gbswan: read the card at the fast SPI clock (patch 246)
- Repository text in English only, before publishing (patch 247)
- README: SwanShellDX, not swanshell's (patch 248)
- README: screenshots of the menu, the plugins and the tools (patch 249)
- Plugins: patch number at the top, scope option on one row (patch 250)
- Menu: VGM player no longer runs out of stack; PCM-heavy tracks keep the keys (patch 251)
- README: the VGM player (patch 252)
- ggswan: vwait in O(1), also for AND A / OR A and JP cc (Gemini G10a, patch 253)
- Plugins: the game's FPS on the menu's VIDEO page (patch 254)
- .gitignore: the PC test libraries and local plugin copies (patch 255)
- ggswan README and the plugins' help pages up to date (patch 256)
