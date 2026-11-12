# gbswan: the recompiled code

The plugin's CPU is a recompiler (`gb/ws/gb_drc.c`, `gb_drc_asm.s`): the
Game Boy's instructions are translated, a block at a time, into V30MZ
code that is kept and run in their place. This is how that code holds the
Game Boy's state, for whoever changes the translator or its routines.

## Registers

| V30MZ | Game Boy |
|---|---|
| AL | A |
| AH | the flags, when they are not in the V30MZ's own (below) |
| CX (CH, CL) | BC |
| DX (DH, DL) | DE |
| BX (BH, BL) | HL |
| BP | SP |
| SI, DI, ES | free: addresses, values on their way |
| DS = SS = 0 | the internal RAM |

`gb.a`, `gb.c`/`gb.b`, `gb.e`/`gb.d`, `gb.l`/`gb.h`, `gb.sp`, `gb.pc`
(`gb_t`, `gb/core/gb.h`: its first 12 bytes, checked when `gb_drc.c` is
compiled) are where they are between runs of translated code; `gbd_enter`
loads them and the routine every block leaves through stores them.

## Flags

Z, H and C are the V30MZ's ZF, AF and CF, at the bits `LAHF` puts them
(40h, 10h, 01h). They are either in AH or, straight after an instruction
that sets them, still in the V30MZ's flags; the translator knows which
(`fl`), and writes `LAHF` only before something that would lose them and
`SAHF` (or `SHR AH, 1` when only C is wanted, for ADC, RLA, INC and the
like) only when an instruction needs them back. Where the V30MZ's AF is
not the Game Boy's H (AND sets it, OR and the shifts clear it) the
translator remembers what to do when it writes `LAHF` (`hfix`). A pass
over the block before it is translated finds, for each instruction,
which flags are still wanted after it, so that nothing is saved for
nobody.

N is not in a register: every instruction leaves it 0 or 1 whatever its
operands, so the translator knows it, and writes it to `drc_n` (0 or
40h) when the block is left or before PUSH AF. POP AF sets it from the
stack.

At every exit of a block the flags are in AH and N in `drc_n`. Outside
translated code AH is `drc_ah`; `gb.f` has the Game Boy's own byte only
around a step of the interpreter.

## Memory

On the WonderSwan a Game Boy address from 8000h up is the internal RAM
6000h lower: the video memory, the work RAM, the OAM (9E00h), the I/O
registers as they were written (9F00h), the high RAM (9F80h), IE
(9FFFh). The cartridge's RAM (A000h-BFFFh) is not: it is behind the RAM
window.

- **The stack**: `[BP - 6000h]`, after one comparison (SP below C000h
  goes to a routine: some programs point SP at tables in ROM).
- **(HL), (BC), (DE)**: two comparisons, then `[BX - 6000h]` for
  C000h-FDFFh; everything else through a routine (`s_rd`, `s_wrt`):
  ROM through `drc_segtab` (a segment for each ROM area in which the
  Game Boy's address is the offset), the video memory with the
  WonderSwan map entry kept next to a map byte, the echo, the OAM, the
  high RAM, and C for the I/O registers and the cartridge's RAM and
  registers. LY and STAT are read by the routine itself, from the time.
- **A constant address** is sorted out when the block is translated.
- A write that ends in C for an I/O register or the cartridge's
  registers does not come back: the block is left for the next
  instruction through the dispatcher, which sees what the write
  changed (an interrupt due, another ROM bank). The word after such a
  call is that instruction's address.

## Time

`gbf_cyc` is the count of machine cycles to the next event
(`gb/core/gbf_hw.c`). Every exit takes the block's cycles so far off it
and goes on to the next block only while it is above zero. Before an
I/O register is read or written the cycles so far are taken off, so
that the time it is read at is right to the instruction. A write
through HL to an I/O register or to the cartridge's registers leaves the
block without its cycles so far: the CPU is that much ahead (a few
cycles), the one known shortcut in the count.

A loop that only loads A from memory or an I/O register, tests it and
jumps back is a wait: nothing can change what it reads before the next
event (memory: only an interrupt writes it) or the next line or mode
(LY, STAT), so that time is taken off at once.

## Blocks

- A block ends at a jump, call or return, at a write to an I/O register
  with a constant address, after the instruction that follows EI, or
  after 40 instructions or about 90 cycles.
- The table (PSRAM, segment 4000h, 4096 places of 8 bytes) is keyed by
  address and ROM bank + 1 (FFFFh for code in RAM). The code is in
  segments 5000h-C000h; when the last is full everything is forgotten.
- An exit to a constant address in the same ROM area, or in the first
  one, is a jump the dispatcher joins to the block there. The others
  (returns, JP HL, calls from the first ROM area into the second) look
  the block up in the table themselves (`s_exit_dyn`) and go through
  the dispatcher only when it is not at its first place there.
- Code in RAM has a copy of its bytes before it, compared at every
  entry from the dispatcher; it is joined to nothing but itself.
- What the translator does not do is run by the interpreter, one
  instruction, and the next block starts after it: HALT, STOP, DAA,
  LD (nn), SP, ADD HL, SP, ADD SP, e, LD HL, SP + e, and the CB
  operations on (HL) other than BIT.

## Interrupts

They are taken by the dispatcher (`gbd_run_frame`), between blocks:
when the count runs out and the event raises one, or after a write to
an I/O register. EI sets `drc_ei`; the block ends after the next
instruction, and the dispatcher then enables them.
