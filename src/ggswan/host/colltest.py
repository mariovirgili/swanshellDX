#!/usr/bin/env python3
"""The sprite collision flag's routine of the WS build (drc_asm.s
coll_scan, main.c coll_pair) in Unicorn, on states saved by wssim
(GGSWAN_COLLDUMP=file: registers 1 and 6, the sprite table's address, the
lines, the flag as the PC's C found it, the VRAM). The flag must be the
same in every state, and the registers a C caller expects kept.
usage: colltest.py build/ggswan.elf file.cd"""
import sys
from elftools.elf.elffile import ELFFile
from unicorn import *
from unicorn.x86_const import *

f = ELFFile(open(sys.argv[1], 'rb'))
uc = Uc(UC_ARCH_X86, UC_MODE_16)
uc.mem_map(0, 0x110000)
for s in f.iter_sections():
    if s['sh_flags'] & 2 and s['sh_size'] and s['sh_type'] == 'SHT_PROGBITS':
        uc.mem_write(s['sh_addr'] & 0xFFFFF, s.data())
syms = {}
for y in f.get_section_by_name('.symtab').iter_symbols():
    syms.setdefault(y.name, y['st_value'] & 0xFFFFF)
SENT = 0x50000
uc.mem_write(SENT, b'\xF4')
vram = int.from_bytes(uc.mem_read(syms['gg_vram'], 2), 'little')
def w8(n, v): uc.mem_write(syms[n], bytes([v & 0xFF]))
def w16(n, v): uc.mem_write(syms[n], (v & 0xFFFF).to_bytes(2, 'little'))
data = open(sys.argv[2], 'rb').read()
n = bad = sets = ins = 0
def h_code(uc, addr, size, ud):
    global ins
    ins += 1
uc.hook_add(UC_HOOK_CODE, h_code)
for o in range(0, len(data), 8 + 0x4000):
    r1, r6, sl, sh, lines, want = data[o:o + 6]
    uc.mem_write(vram, data[o + 8:o + 8 + 0x4000])
    sat = vram + (sl | sh << 8)
    h = 16 if r1 & 2 else 8
    got = 0
    if r1 & 0x40:
        w16('coll_sat', sat); w8('coll_h', h); w8('coll_h1', h - 1); w8('coll_hh', 2 * h - 2)
        w16('coll_tb', 0x100 if r6 & 4 else 0); w8('coll_lines', lines)
        w8('coll_noend', 1 if lines == 224 else 0); w16('emp_tile', 0xFFFF); w8('coll_emp_has', 0)
        uc.mem_write(syms['coll_head'], b'\xFF' * 16); uc.mem_write(syms['emp'], bytes(64))
        regs = {UC_X86_REG_BX: 0x1234, UC_X86_REG_CX: 0x2345, UC_X86_REG_SI: 0x3456, UC_X86_REG_DI: 0x4567,
                UC_X86_REG_BP: 0x5678, UC_X86_REG_DS: 0, UC_X86_REG_SS: 0, UC_X86_REG_ES: 0x6000}
        for r, v in regs.items(): uc.reg_write(r, v)
        uc.reg_write(UC_X86_REG_AX, sat)
        uc.reg_write(UC_X86_REG_SP, 0x3E00)
        uc.mem_write(0x3E00, (0).to_bytes(2, 'little') + (SENT >> 4).to_bytes(2, 'little'))
        uc.reg_write(UC_X86_REG_CS, 0xF000)
        uc.emu_start(syms['coll_scan'], SENT + 1, count=2000000)
        got = uc.reg_read(UC_X86_REG_AX) & 0xFF
        for r, v in regs.items():
            if r != UC_X86_REG_ES and uc.reg_read(r) != v:
                bad += 1; print('state %d: a register changed (%04X, not %04X)' % (n, uc.reg_read(r), v)); break
        if uc.reg_read(UC_X86_REG_SP) != 0x3E04:
            bad += 1; print('state %d: SP %04X' % (n, uc.reg_read(UC_X86_REG_SP)))
    if got != want:
        bad += 1
        if bad < 9: print('state %d: flag %02X, the PC found %02X' % (n, got, want))
    sets += want != 0
    n += 1
print('%d states (the flag set in %d): %d wrong; %.0f instructions a state' % (n, sets, bad, ins / max(n, 1)))
