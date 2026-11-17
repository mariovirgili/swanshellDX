# gbcswan: what of gbswan's CPU and recompiler work it has

gbcswan does not exist yet (Gate 0 only: `GBCSWAN-AUDIT-HEAD.md`). When
it is forked from gbswan, the patch it is forked at is written here, and
from then on every gbswan patch that touches `gb_drc.c`, `gb_drc_asm.s`,
`gb_asm.s`, `core/gbf_hw.c` or the ABI gets a row: taken or not, what had
to change, the tests, the cycles. A patch that makes gbcswan worse is not
taken, and the reason is written.

Fork point: none yet.

| gbswan patch | what it does to the CPU side | for gbcswan? | changes needed | tests | cycles | state |
|---|---|---|---|---|---|---|
| 185 | near links as `jg`, even block starts, `n_dead`, N at the routine's call, cold pieces after a jump | yes, all (nothing of the memory's layout in them) | none expected | - | - | in the fork point if it is 185 or later |
| 185 | the cartridge's RAM through pointers in line (`cartp_open`) | yes, with care | the test "A000h-BFFFh" is the same; `drc_ram_seg` by the MBC; MBC5's banks | - | - | as above |
| 185 | POP from the ROM (`s_pop`) | yes | the ROM's segments if the ROM is over 1 MiB | - | - | as above |
| 186 | PUSH AF / POP AF in line | yes (the flags are the same) | none | - | - | as above |
| 186 | the sprites' sorting loop | no: on the Color the sprites' order is the OAM's, not X | - | - | - | not to be taken |
| 189 | `s_wrt`'s short way for a pattern kept | depends on how gbcswan keeps patterns (4 bits a pixel, two banks) | to be written again | - | - | to decide at Gate E |
