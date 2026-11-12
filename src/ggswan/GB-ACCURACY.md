# gbswan: how faithful, measured

The Game Boy in `gb/core` is the reference: an interpreter that moves the
timer, the serial port, the OAM DMA and the LCD by one machine cycle
before each access to the bus. It is run on the PC by `gb/host/gbtest`
and checked with test ROMs by `gb/host/gbsuite.py` (the ROMs are not in
the repository: game-boy-test-roms v7.0, which collects Blargg's,
Mooneye's and dmg-acid2). What runs on the WonderSwan will be compared
with it.

The machine: a DMG (the boot ROM's revisions A, B, C), from the state its
boot ROM leaves at 0100h (the boot ROM itself is not run and not needed;
the logo it leaves in the video memory is not there).

## Results (patch 155)

| tests | passed |
|---|---|
| Blargg `cpu_instrs` (all 11) | yes |
| Blargg `instr_timing` | yes |
| Blargg `mem_timing`, `mem_timing-2` | yes |
| Blargg `halt_bug` | yes (its picture read, then by hash) |
| Blargg `interrupt_time` | not run: the ROM is for the Game Boy Color only |
| Mooneye acceptance, the tests for the DMG: bits 3, instr 1, interrupts 1, oam_dma 3, serial 1, timer 13, and the 32 in the directory itself (the boot state, the instructions' and interrupts' timing, HALT, EI, DI) | 54 of 54 |
| Mooneye acceptance/ppu | 5 of 12 (the LCD's timing is the next step) |
| Mooneye emulator-only: mbc1 | 12 of 13 (not `multicart_rom_8Mb`: the cartridges with several games are not told apart) |
| Mooneye emulator-only: mbc2, mbc5 | 7 of 7, 8 of 8 |
| dmg-acid2 | yes (the same picture as its reference) |

## Known differences

- The picture is drawn a line at a time, at the start of the line's
  drawing: what a program changes in the middle of a line is seen from
  the next line.
- While the OAM DMA runs only the reads of the OAM are refused (FFh); the
  rest of the bus is as usual.
- STOP: the CPU waits for a key; DIV is cleared.
- MBC3: no clock (its registers read 0). No MBC1 cartridges with several
  games, no camera, rumble, HuC or MMM01.
- The serial port has no other end: a transfer with the Game Boy's own
  clock ends after 8 bits with FFh received, one with the other's clock
  never does.
- No sound yet (the registers keep what is written).
