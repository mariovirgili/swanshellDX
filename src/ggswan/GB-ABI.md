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
  goes to a routine: some programs point SP at tables in ROM). PUSH AF
  and POP AF always go through the routines, which do the work RAM and
  the high RAM themselves.
- **(HL), (BC), (DE)**: two comparisons, then `[BX - 6000h]` for
  C000h-DFFFh (to FDFFh until patch 176, which made the echo the
  console's 8000h-9DFFh); everything else through a routine (`s_rd`, `s_wrt`):
  ROM through `drc_segtab` (a segment for each ROM area in which the
  Game Boy's address is the offset), the video memory with the
  WonderSwan map entry kept next to a map byte, the echo, the OAM, the
  high RAM, and C for the I/O registers and the cartridge's RAM and
  registers. LY and STAT are read by the routine itself, from the time.
- The high RAM through a pointer is in line too (a third comparison,
  off the work RAM's path).
- **A pointer the block itself loads** (`LD HL, nn`, `LD DE, nn`,
  `LD BC, nn`, then only INC, DEC, `(HL+)`, `(HL-)`: `i_dir`) into the
  work RAM or the high RAM is that address: the access is one
  instruction on it, nothing checked, and cannot leave the block. Into
  the cartridge's RAM: through its segment after the test of
  `drc_ram_ok` (reads, writes, the operations on A).
- **A pointer whose high byte the block itself sets** (`LD H, n`,
  `LD D, n`, `LD B, n`, `LD rr, nn`, then INC and DEC of that byte or
  of the pair: `i_cls`, the range the byte can be in) and that stays
  within one part of the memory: the work RAM, read and written at
  `[BX - 6000h]` with nothing checked; the video memory, read so (its
  writes go through `s_wrt`: see "Time", the patterns); one of the ROM areas, read through its
  segment. A table read by `LD H, n` / `LD L, A` / `LD r, (HL)` is one
  instruction, not six and 54 bytes.
- **A constant address** is sorted out when the block is translated; in
  the cartridge's RAM it is an access through the RAM's segment after
  one test that it is plain RAM and enabled (`drc_ram_ok`).
- **A stack in the cartridge's RAM** (where SP is when the block is
  translated): PUSH and POP in line the same way.
- A write to an I/O register or to the cartridge's registers may not
  come back: when an interrupt can now be taken, the time to the next
  event has run out or the code's own ROM bank has changed
  (`drc_must`), the block is left for the next instruction through the
  dispatcher. The word after such a call is that instruction's
  address. Otherwise the block goes on.

## Time

`gbf_cyc` is the count of machine cycles to the next event
(`gb/core/gbf_hw.c`). Every exit takes the block's cycles so far off it
and goes on to the next block only while it is above zero. Before an
I/O register is read or written the cycles so far are taken off, so
that the time it is read at is right to the instruction. A write
through HL to an I/O register or to the cartridge's registers leaves the
block without its cycles so far: the CPU is that much ahead (a few
cycles), the one known shortcut in the count.

A loop that only counts BC down (`DEC BC / LD A, B / OR C / JR NZ`, a
program's delay) is passed over to its last turn, or as far as the
time to the next event goes (`s_delay_bc`).

A loop that only loads A from memory or an I/O register, tests it and
jumps back is a wait: nothing can change what it reads before the next
event (memory: only an interrupt writes it) or the next line or mode
(LY, STAT), so that time is taken off at once. A loop on STAT whose
going round depends on the mode alone (worked out when it is
translated: `wait_modes`) has its wait done by its read of STAT
(`s_wait_stat`: the time passed to the first cycle of a mode that ends
it, from a table for the lines of the picture), and need not be a
block's start: `LDH A, (41h)`, tests, a conditional jump back to the
read, anywhere in a block.

A write to SCY, SCX, BGP, WY or WX by `LDH (n), A` is done by a
routine (`s_vreg`): the register and the frame's list of changes
(`gb_video.c`, `gbv_reg`), without C. The second ROM area's bank
register is written without C for MBC1, MBC3, MBC5 and MBC2 (0000h-
3FFFh with the address's bit 8 set, four bits); "RAM on" (0000h-1FFFh
of MBC1, 3, 5) costs nothing when it writes what is already so.

No wait is passed over beyond the next event (`SKIP_CYC` in
`gb_drc_asm.s`; `s_wait_stat` then reads STAT as it is and the loop
goes round, to the dispatcher), and none of it when an event is
already due: an interrupt must be taken where it falls, not at the
wait's end. (Until patch 167 the whole wait was taken first: a wait
for the horizontal blank begun in the vertical blank went to the next
frame's first line, and the vertical blank's interrupt was taken
there, ten lines late.)

A loop that only copies or fills is done at once when a block starts
with it (`s_copy0`-`s_copy4`, `s_fill0`): `LD A, (HL+) / LD (DE), A /
INC DE / DEC B / JR NZ`, the same with BC counted by `LD A, B / OR C`,
their mirrors from (DE) to (HL+), `LD A, (BC) / INC BC / LD (HL+), A /
DEC D / JR NZ`, and `LD (HL+), A / DEC B / JR NZ`. As many turns as
fit: the source wholly in ROM, tiles, work RAM or high RAM, the
destination in the tiles (8000h-97FFh), work RAM or high RAM, and the
time to the next event; registers, flags and cycles are left as the
loop's turns would have. What does not fit (the maps, OAM, I/O,
cartridge RAM, a bank's edge) runs as translated, and the jump back
goes to after the routine's call, not to it again.

A loop of `CP (HL) / JR cc` on itself waits for what is at HL to
change (`s_skip_hl`): with HL = FF44h and the jump taken while LY is
not A, the time to the line before the one waited for is taken off at
once and the rest by lines; with HL in RAM the time to the next event
(only an interrupt can change it).

`LD (nn), A` where nn is the register of the second ROM area's bank
(the cartridge's kind says which addresses: the translator knows)
calls `s_bankx`, A in AL and the next instruction's address after the
call as for `s_wrt`, or, from a block in the first ROM area, `s_bank0`,
which comes straight back (patch 175): `wrt_bank` without its looking
at the address.

`ADD HL, rr` whose H and C are set again before anything reads them is
`add bx, rr` and nothing else (patch 175; by `i_live`, the flags needed
before each instruction, which until then only decided whether the
V30MZ's flags had to be saved).

A write through a pointer the block loaded itself and that is in
the video memory (`i_cls` 2) calls `s_wrv` (patch 176): the byte in
`drc_tmp`, the address in SI, nothing after the call, the registers
where they are. A pattern, with the late patterns off, is written and
`s_wrv` returns; for anything else it becomes `s_wrt`.

INC and DEC of BC, DE, HL are `inc`/`dec` of the register when the
Game Boy's flags are in AH or are not read before they are next set
(patch 176); `lea` otherwise. INC and DEC (HL) with HL known to be in
the work RAM are done on `[bx - 6000h]`. `ADD HL, rr` with only C
read after it is `add bx, rr / lahf / and ah, 1`.

A write to a pattern (8000h-97FFh) goes through `s_wrt`, whatever
the block knows of the pointer (until patch 172 a pointer whose high
byte the block had set wrote there unchecked), and the copying loops
below leave the patterns alone while `gbv_tq_on` is set: `gb_video.c`
may be keeping the patterns a frame late ("the patterns a frame late"
there: the byte then goes to the tile's copy, `wrt_tq`, the last tile's
copy remembered, a new one made by `gbv_tq_new` in `gb_asm.s`).

LYC written (`LDH (45h), A`) with the LCD on, no interrupt asked for
at a mode, no event due and no serial transfer is `gbf_io_write`'s
case in the routine (`wrt_lyc`): the place in the frame, what LY is
compared as there, the interrupt if the signal rises, the next event
(`gbf_sched_asm`, `gb_asm.s`: `gbf_hw.c`'s `schedule()` in assembly,
which C calls too), and the dispatcher only if an interrupt is to be
taken or the event is now.

A write through a pointer that turns out to be at an I/O register
(`s_wrt`): FF10h-FF3Fh, the sound's, is the byte stored and, for
NR14, NR24, NR34, NR44 with bit 7 set, the channel's bit in
`gbf_trig`; SCY, SCX, BGP, OBP0, OBP1, WY, WX written with the value
they have is nothing; the rest is `gbf_io_write`.

A read through HL, BC or DE from below 8000h is in line: the ROM's
segment for that area and the byte, no routine.

A write of a constant to P1 (`LD A, n` before it in the block and A
left alone since) is the table's byte for those rows of keys, moved
with no routine.

## Blocks

- A block ends at a jump, call or return, after the instruction that
  follows EI, or after 40 instructions or about 90 cycles.
- From its sixth instruction on, a block in ROM also ends where
  another block already starts: it goes on into that one by a joined
  jump, and the same code is not translated twice (a program that
  enters a row of instructions at many places, as Race Drivin' does
  its unrolled loops, had a third of its translated code twice or
  more). The first five are translated anyway: a boundary costs a
  dozen cycles.
- The table (PSRAM, segment 4000h, 8192 places of 8 bytes) is keyed by
  address and ROM bank + 1 (FFFFh for code in RAM). A block is at the
  hash's place or at one of eleven others far from it (`PLACE` in
  `gb_drc.c`); when all twelve are taken everything is forgotten. The code is in
  segments 5000h-C000h; when the last is full everything is forgotten.
- An exit to a constant address in the same ROM area, or in the first
  one, is a jump the dispatcher joins to the block there. The others
  (returns, JP HL, calls from the first ROM area into the second) look
  the block up in the table themselves (`s_exit_dyn`) and go through
  the dispatcher only when it is not at its first place there.
  Before the table they look in two smaller ones in the console's own
  RAM (D800h for the first ROM area, E800h for the second: 512 places
  of 8 bytes, the address's low 9 bits; the address, the block's
  offset and segment and, in the second, the bank's key): a hit is a
  near compare or two and a far jump, with no segment register loaded.
  A miss that the big table answers fills the place, and so does the
  dispatcher with the block in ROM it goes to (patch 171). Both are emptied
  when the blocks are forgotten, the first also when the first area's
  bank changes (`gb_main.c`).
- Code in RAM has a copy of its bytes before it, compared at every
  entry from the dispatcher (`gbd_ram_cmp`, `gb_asm.s`: one `repe
  cmpsb`, which goes on past a byte that differs when that byte is one
  read as the code runs); it is joined to nothing but itself (its
  jumps to its own start go there at once). Before the copy, a header
  (`RAM_HEAD`, 34 bytes): the bytes that could be read as the code
  runs (16 bytes, a bit for each of the block's first 128), those
  that are, "are there any", the count of forms. When the bytes are
  no longer those and all that changed are operands of `LD r, n`, `LD
  rr, nn`, `JP nn`, `JR e` or `CALL nn` in a block wholly in the work
  RAM or the high RAM, the block is translated once more with those
  operands read from the RAM as it runs (`mov r, [n's place]`; the
  jumps through `s_exit_dyn`) and left out of the comparison
  (`gbd_ram_cmp` in `gb_asm.s`): a routine that writes its own
  operands before every use (Race Drivin's line filler, the n of an
  `LD D, n` and the e of a `JR`; Donkey Kong Land's `LD HL, nn /
  CALL`) is translated twice and then runs as it is. Otherwise the
  form now there is looked up, and translated if new, under a key of
  its own, 8000h + a 15-bit sum of its first bytes (as many as the
  first form has, 12 at most; bit 0 clear, so never FFFFh), and
  compared in full as well. The first form counts the others met
  (a byte before its copy of the bytes); from the seventh on the code
  there is run by the interpreter and no longer looked up: as far as
  the first form went, to a jump taken or to an interrupt due. A program that keeps changing a routine in
  RAM (an operand, a jump's target) has each form translated once.
  (Until patch 167 the changed block was translated again every time:
  Donkey Kong Land, R-Type II and Race Drivin' filled the table and
  the code's room over and over.)
- What the translator does not do is run by the interpreter, one
  instruction, and the next block starts after it: STOP,
  LD (nn), SP, ADD HL, SP, ADD SP, e, LD HL, SP + e. (Until patch 163
  DAA and the CB operations on (HL) other than BIT too: a program that
  does its sums with them, as Yoshi does, went through the dispatcher
  hundreds of times a frame.)
- HALT ends its block (patch 171). With an interrupt already asked
  for (IE and IF) it is left to the interpreter, as is a HALT just
  after EI; otherwise a cycle, `gb.halted` set, and `s_exit`
  ("Interrupts"), which passes the time to each event until one asks
  for an interrupt.

## Interrupts

They are taken between blocks: when the count runs out and the event
raises one, or after a write to an I/O register. Since patch 171 a
block that leaves (its time has run out, or the block to go to is not
in the small tables) goes to `s_exit`, in its own code segment, which
does what `gbd_run_frame` does, in its order: `drc_ei` or an
instruction for the interpreter: the dispatcher; the count at zero or
below: the event (`gbf_event_asm` in `gb_asm.s` when it is the
timer's and LYC's alone, as in the middle of a frame it mostly is;
else `gbf_host_event`, C; AX, BX, CX, DX saved around it), and the
dispatcher if that ended the frame; a jump to be joined: the
dispatcher; halted: the event at once if no interrupt is asked for,
otherwise on, a cycle later; an interrupt asked for, enabled, and IME
set: PC written at `[BP - 6002h]`, SP down two, IME cleared, the bit
cleared in IF, 5 cycles, PC = 40h + 8n (SP outside C002h-E000h and
below FF82h: the dispatcher, whose `gb_cpu_interrupt` writes
anywhere); then the block at PC by the small tables and the big one's
first place, and the dispatcher when it is not there. Code in the
work RAM or the high RAM (patch 173, `s_lookup_ram`): its first form,
if it is at its first place in the table and the bytes there are
still the ones translated (compared in the routine as `gbd_ram_cmp`
does, the bytes read as the code runs left out); else the dispatcher.
The dispatcher does the same things for what reaches it. RETI, with an interrupt enabled and waiting, leaves
by the dispatcher, which takes it there and then (until patch 168 it
went on by the table and the interrupt waited for the next event: a
STAT interrupt raised with the vertical blank's was taken ten lines
late). EI sets `drc_ei`; the block ends after the next
instruction, and the dispatcher then enables them.
