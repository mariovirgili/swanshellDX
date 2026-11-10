# ggswan: Game Gear on the WonderSwan Color

ggswan is SwanShellDX's Game Gear plugin. It runs Sega Game Gear games
(`.gg` files) on a WonderSwan Color with a nileswan cartridge. The Z80 is
emulated by a dynamic recompiler (Z80 to V30MZ code), with the original
interpreter kept for the parts that need it. The Game Gear video memory is
mapped onto the WonderSwan's own tile and screen hardware.

Status: fairly stable (patch 115). It has been tested mostly in the
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
| Y3 (Y down), while paused | Runs **one frame**, then pauses again. |

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
- No saving of the cartridge RAM (battery saves) to the SD card yet.
- Some games run below full speed in their heaviest parts.
- Requires a WonderSwan Color (or SwanCrystal) for the color modes and
  the sound DMA.

## Source layout

- `core/`: Game Gear machine (Z80 interpreter, VDP, memory mapper).
- `drc/`: the Z80 to V30MZ recompiler.
- `ws/`: the WonderSwan side: runtime, video, sound, main loop.
- `host/`: PC test tools (comparison against the interpreter and a
  reference renderer, cycle profiling in an x86 emulator).
