# gbcswan, Gate 0: what gbswan's HEAD really is

The Game Boy Color technical guide (a working document, not in the
repository; sections 0.1, 4, 5 and 12) asks, before any Game Boy Color code: rebuild the real HEAD, find
every structure and fixed address, compare with the names in Argon's
report, and say whether the memory map it proposes can be had. This is
that, on commit `289f030` (gbswan patch 189, 2026-11-17), from the sources in
`src/ggswan/gb` and from `build/gbswan.elf` (`nm`, `size`). Nothing of
gbswan was changed for it.

## 1. Argon's names

Looked for, as whole words, in `gb/ws` and `gb/core`:

| name in the report | references in HEAD |
|---|---|
| `GBV_TQ_SEG`, `GBV_TQ_CAP`, `gbv_tq_meta`, `gbv_tq_data` | 0 |
| `gbv_vram_Wait`, `gbv_scx` | 0 |
| `DRC_R16`, `DRC_R23`, `DRC_CB`, `DRC_B0` | 0 |
| `s_ev_ram` | 0 |

None exists, at this commit or in any patch since 153 (the plugin's
first). The report describes a tree that is not this one; the snapshot the
guide quotes (`RAW_MAX 146`, `raw` at A000h, `list_a` at A500h,
`list_b` at B800h, `TQ_S 32`, `TQ_IDX` CC30h, `TQ_ADR` CDB0h,
`TQ_PIDX` CE70h, `TQ_DAT` F800h) is HEAD's, to the byte. So: **the
lists of the frame's events and the patterns kept for the next frame are
in the internal RAM, not in PSRAM**, and nothing of the report's PSRAM
layout exists.

## 2. What is where (HEAD)

Internal RAM, from the sources' fixed addresses (`grep` of every
`#define NAME ((type*) 0x....)`) and the linker (`__wf_heap_start` =
0BD3h: text 77,287 bytes, data 527, bss 2,221):

| internal RAM | bytes | what | defined in | lives |
|---|---:|---|---|---|
| 0040h-0BD2h | 2,963 | C data and bss (room to 0FBFh) | linker | always |
| 0FC0h-0FFFh | 64 | `WAVES`, the WS's sound waves | `gb_sound.c` | always |
| 1000h-1FFFh | 4,096 | `MAPS_88`: two WS maps kept (LCDC bit 4 clear) | `gb_video.c` | always |
| 2000h-37FFh | 6,144 | `gb_vram`: the patterns = WS tiles 0-383 (2 bits a pixel) | `core/gb.h` | always |
| 3800h-3FFFh | 2,048 | `gb_vram`: the Game Boy's two maps as written | `core/gb.h` | always |
| 4000h-47FFh | 2,048 | `SCR1` | `gb_video.c` | always |
| 4800h-57FFh | 4,096 | `MAPS`: two WS maps kept (LCDC bit 4 set); `TEXT_MAP` before the game | `gb_video.c`, `gb_main.c` | always |
| 5800h-59FFh | 512 | `SPR_TABLE` | `gb_video.c` | always |
| 5A00h-5FFFh | 1,536 | the font's tiles, tile 1023 | `gb_main.c` | always |
| 6000h-7FFFh | 8,192 | `gb_wram` | `core/gb.h` | always |
| 8000h-8FFFh | 4,096 | `DRC_EBUF`: a block being put together | `gb_drc.c` | while translating |
| 9000h-97FFh | 2,048 | `COLD_SCR` (while translating); `ST_BUF` (a state's file) | `gb_drc.c`, `gb_main.c` | transient |
| 9800h-9BFFh | 1,024 | `DRC_HAS`: a bit a place of the blocks' table | `gb_drc.c` | always |
| 9D00h-9D9Fh | 160 | `OAM_KEPT` | `gb_video.c` | a frame |
| 9E00h-9FFFh | 512 | `gb_oam`, `gb_io`, `gb_hram` | `core/gb.h` | always |
| A000h-A48Fh | 1,168 | `raw`: 146 changes a frame | `gb_video.c` | a frame |
| A500h-CA5Fh | 9,408 | `list_a`, `list_b`: 294 entries of 16 bytes each | `gb_video.c` | two frames |
| CB00h-CC2Fh | 304 | the sprites' order and keys, `SPR_TALL` | `gb_asm.s`, `gb_video.c` | always |
| CC30h-CFEFh | 960 | `TQ_IDX`, `TQ_ADR`, `TQ_PIDX` | `gb_video.c`, `gb_asm.s` | always |
| D000h-D7FFh | 2,048 | the stack while the game runs | `gb_asm.s` | always |
| D800h-F7FFh | 8,192 | `DRC_RC0`, `DRC_RCX`: 512 places of 8 bytes each | `gb_drc.h` | always |
| F800h-FDFFh | 1,536 | `TQ_DAT`: 3 sets of 32 tiles | `gb_video.c` | always |
| FE00h-FFFFh | 512 | the WS's palettes | hardware | always |

Free: 0BD3h-0FBFh (1,005 bytes), 9C00h-9CFFh, 9DA0h-9DFFh, CFF0h-CFFFh.
The internal RAM is full.

PSRAM (64 KiB banks; on the console read and run through the linear
area, written through the RAM window - patch 184):

| linear segment | bank on the console | what |
|---|---|---|
| (windows) | 0-15 | the ROM, 1 MiB at most (`ROM_MAX_BANKS 64`) |
| 4000h | 14h | the blocks' table, 8192 places of 8 bytes (`DRC_TABLE_SEG`) |
| 5000h-C000h | 15h-1Ch | translated code, 8 banks (`DRC_CODE_SEG`, `DRC_CODE_SEGS`) |
| D000h | 1Dh | a state: the internal RAM (`ST_SEG`) |
| E000h | 1Eh | a state: the cartridge's RAM, its mark, FatFs's volume (`ST_SEG2`); the plugin's first bytes are in this bank's top (77 KiB: from about ECC0h) |
| F000h | 1Fh | the plugin |

The cartridge's RAM is the SRAM (32 KiB) behind the RAM window. The bank
mask read on the console is 01Fh: **32 banks, 2 MiB, all of them spoken
for** (16 the ROM, 9 the recompiler, 2 a state, 2 the plugin; 3 free:
10h-13h less none - banks 10h, 11h, 12h, 13h are free only because the
ROM stops at 1 MiB).

## 3. The candidate map against this

The guide's section 4 wants, for 4 bits a pixel: tiles at 4000h-BFFFh
(32 KiB: that is where the hardware has them, and it is right), two WS
screens and the sprite table below 4000h, the work RAM's two banks at
C000h-DFFFh, `DRC_EBUF` at E000h, the stack at F600h-FDFFh, caches of
2 + 4 KiB at 2800h-3FFFh. Against HEAD:

- 4000h-BFFFh is today `SCR1`, `MAPS`, the sprite table, the font, the
  work RAM, `DRC_EBUF`, `DRC_HAS`, the OAM and I/O, `raw` and most of
  the two lists. **All of it has to go somewhere else.**
- What gbswan keeps in the internal RAM outside the tiles is 2,963 (C) +
  64 + 4,096 + 2,048 (the maps as written) + 2,048 + 4,096 + 512 + 1,536 +
  8,192 + 4,096 + 2,048 + 1,024 + 160 + 512 + 1,168 + 9,408 + 304 + 960 +
  2,048 + 8,192 + 1,536 + 512 = **57,560 bytes**. With 32 KiB of tiles the
  Color has 0040h-3FFFh and C000h-FDFFh for everything else: **31,680
  bytes**. And the Color needs more than the DMG: a second bank of maps
  with their attributes (2 KiB), 64 bytes of palettes kept, a C side with
  more registers.
- So the candidate map does not fit by some 26 KiB, before anything new.
  The guide's own STOP ("the map collides with the video lists") applies.

What can move, and what it costs on the console (section "On the
console" of the README: PSRAM is as fast as the internal RAM to read and
to run, but every write is through the RAM window, a bank switch around
it):

| structure | bytes | written | could be |
|---|---:|---|---|
| `list_a`, `list_b` | 9,408 | once a frame, by C, in one go; read by the line interrupt | PSRAM, written through the window between frames; the interrupt reads through the linear area. The cheapest 9 KiB there is |
| `MAPS`, `MAPS_88` (four WS maps kept) | 8,192 | at every write to a map, by the write routine | the Color needs them anyway as the two screens; gbswan's trick of four is DMG's (LCDC bit 4 flipping in a frame) and may be dropped to two |
| `DRC_RC0`, `DRC_RCX` | 8,192 | by the recompiler's routines, often | 6 KiB as the guide says, after measuring (its Gate B) |
| `DRC_EBUF` | 4,096 | while translating | stays in the internal RAM (the translator writes a byte at a time) |
| `gb_wram` | 8,192 | by the game, all the time, in line | the internal RAM, both banks in view; the other six in PSRAM, 4 KiB copied each way at a change of SVBK through the window |
| `TQ_*` | 2,496 | at pattern writes | to be thought again for 4 bits a pixel and two banks: not a move |
| the video memory as the CPU reads it | 16,384 | by the game | the patterns are the tiles (interleaved, guide 3.3); the two banks of maps, 4 KiB, need a place |

## 4. What this means for the plan

1. The report's PSRAM layout and its internal RAM map are **not**
   descriptions of this code: to be read as proposals only, as the guide
   already says.
2. A Color map has to be drawn from section 2 above, not from the
   report's. The two lists in PSRAM and two WS maps in place of four get
   back about 13 KiB; the caches at 6 KiB, 2 more; that is still some 11
   KiB short, and the rest has to come from the design of the video (what
   is kept of the maps, where the attributes live), which is Gate C's
   question.
3. PSRAM: with a ROM over 1 MiB the banks run out (2 MiB in all on this
   cartridge's mask). Color ROMs of 2 MiB and more do not fit whatever is
   done; 1 MiB ones leave four banks for the six work RAM banks (24 KiB),
   the lists and a state.
4. Speed: gbswan's five slow scenes are at 56,000 to 120,000 cycles a
   frame on the console's count for 51,430 to be had. A Color game in
   double speed asks for twice the CPU. Only games in normal speed, and
   light ones, can be expected to run at full speed.
5. The fork point, when it comes: gbswan patch 189 or later
   (`GBCSWAN-DMG-SYNC.md`).

Gate 0 stops here. Nothing of gbcswan is to be written before Gio has
read this and the next gate is chosen.
