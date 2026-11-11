#!/usr/bin/env python3
"""sgswan's line interrupt with the whole picture on the screen
(drc_asm.s sg_line_entry, sg_video.inc line_irq), in Unicorn, on the WS
build: WS screens line by line, the handler called when the line counter
reaches the match it set, some of the times a few lines late. On every
line not reached late the Y scroll must be line / 3; a match must never be
lost (the end of the screen comes in every screen, frame_waiting is
cleared); the registers come back as they were.
usage: sgirqtest.py build/sgswan.elf [screens]   (also build/sfswan.elf)"""
import sys, random
from elftools.elf.elffile import ELFFile
from unicorn import *
from unicorn.x86_const import *

ELF = sys.argv[1]
SCREENS = int(sys.argv[2]) if len(sys.argv) > 2 else 300
f = ELFFile(open(ELF, 'rb'))
uc = Uc(UC_ARCH_X86, UC_MODE_16)
uc.mem_map(0, 0x110000)
for s in f.iter_sections():
    if s['sh_flags'] & 2 and s['sh_size'] and s['sh_type'] == 'SHT_PROGBITS':
        uc.mem_write(s['sh_addr'] & 0xFFFFF, s.data())
syms = {}
for y in f.get_section_by_name('.symtab').iter_symbols():
    syms.setdefault(y.name, y['st_value'] & 0xFFFFF)
LINES, SENT = 159, 0x50000
uc.mem_write(SENT, b'\xF4')
st = {'line': 0, 'ins': 0, 'ports': [0] * 256}
def h_in(uc, port, size, ud):
    if port == 0x02:
        # (the line moves on while the handler works: one every 40 instructions)
        return (st['line'] + st['ins'] // 40) % LINES
    return st['ports'][port]
def h_out(uc, port, size, value, ud):
    st['ports'][port] = value & 0xFF
def h_code(uc, addr, size, ud):
    st['ins'] += 1
uc.hook_add(UC_HOOK_INSN, h_in, None, 1, 0, UC_X86_INS_IN)
uc.hook_add(UC_HOOK_INSN, h_out, None, 1, 0, UC_X86_INS_OUT)
uc.hook_add(UC_HOOK_CODE, h_code)
REGS = [UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX, UC_X86_REG_SI, UC_X86_REG_DI,
        UC_X86_REG_BP, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SP, UC_X86_REG_SS]
ENTRY = syms['sg_line_entry']
def irq():
    vals = [random.randrange(0x10000) for _ in range(7)] + [0x6000, 0x7000, 0x3E00, 0]
    for r, v in zip(REGS, vals): uc.reg_write(r, v)
    # the interrupt's frame: flags, CS, IP of the HLT
    uc.mem_write(0x3E00, (0).to_bytes(2, 'little') + (SENT >> 4).to_bytes(2, 'little') + (0x0202).to_bytes(2, 'little'))
    st['ins'] = 0
    uc.reg_write(UC_X86_REG_EFLAGS, 0x0002)
    uc.reg_write(UC_X86_REG_CS, 0xF000)
    uc.emu_start(ENTRY, SENT + 1, count=20000)
    got = [uc.reg_read(r) for r in REGS]
    vals[9] = 0x3E06
    assert got == vals, ('registers', [hex(a) for a in got], [hex(b) for b in vals])
    return st['ins'] // 40

random.seed(1)
bad = late_n = calls = 0
uc.mem_write(syms['sg_sq_pend'], b'\x01'); uc.mem_write(syms['sg_sq_act'], b'\x01')
uc.mem_write(syms['sg_vy'], b'\x00')
st['ports'][0x03] = 144
for scr in range(SCREENS):
    late_until = -1; ends = 0
    uc.mem_write(syms['frame_waiting'], b'\x01')
    pending = None
    line = 0
    while line < LINES:
        st['line'] = line
        if st['ports'][0x03] == line and pending is None:
            pending = line + (random.choice([0, 0, 0, 0, 1, 2, 5]) if scr % 3 else 0)
            late_until = max(late_until, pending)
        if pending is not None and line >= pending:
            if line > st['ports'][0x03]: late_n += 1
            was = line
            took = irq(); calls += 1
            pending = None
            if was >= 143: ends += 1
            late_until = max(late_until, line + took + 1) if took else late_until
            line += took
            st['line'] = line
        # the scroll the WS takes for the next line
        nxt = line + 1
        if scr and nxt < 144 and nxt > late_until and st['ports'][0x11] != nxt // 3:
            bad += 1
            if bad <= 8: print('screen %d line %d: scroll %d, not %d' % (scr, nxt, st['ports'][0x11], nxt // 3))
        line += 1
    if scr and ends != 1 or uc.mem_read(syms['frame_waiting'], 1)[0]:
        bad += 1
        if bad <= 8: print('screen %d: the end of the screen served %d times, frame_waiting %d' % (scr, ends, uc.mem_read(syms['frame_waiting'], 1)[0]))
print('%d screens, %d calls (%d of them late): %d wrong' % (SCREENS, calls, late_n, bad))
