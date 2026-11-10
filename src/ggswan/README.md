# ggswan: Game Gear on the WonderSwan Color

ggswan is SwanShellDX's Game Gear plugin. It runs Sega Game Gear games
(`.gg` files) on a WonderSwan Color with a nileswan cartridge. The Z80 is
emulated by a dynamic recompiler (Z80 to V30MZ code), with the original
interpreter kept for the parts that need it. The Game Gear video memory is
mapped onto the WonderSwan's own tile and screen hardware.

Status: fairly stable (patch 118). It has been tested mostly in the
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
  vertical scroll, Game Gear palette; tile writes are applied at the end
  of the WonderSwan screen so a frame is never shown half updated.
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

## smsswan: the Master System plugin (in progress)

`PLUG_SMS.BIN` is built from the same sources with `-DGGSWAN_SMS`
(`make -f Makefile.ggswan SMS=1`; the main build makes both plugins).
Copy it to `/NILESWAN/PLUG_SMS.BIN`: the menu starts it for `.sms`
files, with the same `.sav` saves and the same START + Y1 (Y up) exit.

The Master System shows the whole 256x192 picture; the WonderSwan shows
224x144 of it (columns 16-239, lines 24-167) on its whole screen, with
the background priority always on (screen 2 holds the priority tiles,
so there are no borders and no diagnostics on screen).

Done (patch 118): two plugins from one source tree (the Game Gear one
unchanged: built with the patch number 117 it is identical, byte for
byte, to patch 117's), the Master System view, sprites and screen 2
over the whole WonderSwan screen, `.sms` in the menu, the host tools
(`make sms` in `host/`). Still to do, in this order: the Master System
palette (32 colors of 6 bits) and ports (3E, 3F, DC, DD), the Pause
button (an NMI, from START), register 0's fixed scroll areas and the
sprite status flags, moving the view over the picture and diagnostics
on sprites, the Codemasters mapper. Until the palette is done the
colors are wrong.

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

### Patches 90-118

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
