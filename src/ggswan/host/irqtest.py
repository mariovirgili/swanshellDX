#!/usr/bin/env python3
"""The line interrupt's short path (drc_asm.s line_irq_entry) against the C
handler (line_irq_slow -> main.c line_irq), in Unicorn, on the WS build:
random lists of scroll splits, a random split being served, the line
counter moving as the handler works. Both must write the same values to
the same ports in the same order and leave the same state, and return with
every register as it was.
usage: irqtest.py build/ggswan.elf [cases]   (also build/smsswan.elf)
The line read from port 02h does not follow the cycles (the two handlers
take different times): it is the starting line plus k1 for every scroll
value written and k2 for every match line set, so that a handler finds
itself late in the same places whichever path it takes."""
import sys, random
from elftools.elf.elffile import ELFFile
from unicorn import *
from unicorn.x86_const import *

ELF = sys.argv[1]
CASES = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
f = ELFFile(open(ELF, 'rb'))
uc = Uc(UC_ARCH_X86, UC_MODE_16)
uc.mem_map(0, 0x110000)
image = []
for s in f.iter_sections():
    if s['sh_flags'] & 2 and s['sh_size'] and s['sh_type'] == 'SHT_PROGBITS':
        image.append((s['sh_addr'] & 0xFFFFF, s.data()))
syms = {}
for y in f.get_section_by_name('.symtab').iter_symbols():
    syms.setdefault(y.name, y['st_value'] & 0xFFFFF)
SMS = 'act_sq' in syms
VIEW_H, LINES = 144, 159
PRIO = syms['drc_st'] + 506
SENT = 0x50000   # a HLT to return to
STATE = ['act_i', 'act_n', 'frame_waiting', 'pend_n', 'act_vy']
N = (syms['act_val'] - syms['act_line']) if syms['act_val'] > syms['act_line'] else 64

st = {}
def h_in(uc, port, size, ud):
    if port == 0x02:
        st['reads'] += 1
        return (st['base'] + st['k1'] * st['w10'] + st['k2'] * st['w03']) % LINES
    return st['ports'][port]
def h_out(uc, port, size, value, ud):
    value &= 0xFF
    st['ports'][port] = value
    st['trace'].append((port, value))
    if port == 0x10: st['w10'] += 1
    if port == 0x03: st['w03'] += 1
def h_code(uc, addr, size, ud):
    st['ins'] += 1
uc.hook_add(UC_HOOK_INSN, h_in, None, 1, 0, UC_X86_INS_IN)
uc.hook_add(UC_HOOK_INSN, h_out, None, 1, 0, UC_X86_INS_OUT)
uc.hook_add(UC_HOOK_CODE, h_code)

REGS = [UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX, UC_X86_REG_SI, UC_X86_REG_DI,
        UC_X86_REG_BP, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SP, UC_X86_REG_SS]

def run(entry, case):
    uc.mem_write(0, bytes(0x110000))
    for a, d in image:
        uc.mem_write(a, d)
    uc.mem_write(SENT, b'\xF4')
    n, lines, vals, ys, i, prio, sq, base, k1, k2, irq_line, pend = case
    uc.mem_write(syms['act_line'], bytes(lines)); uc.mem_write(syms['act_val'], bytes(vals))
    uc.mem_write(syms['act_n'], bytes([n])); uc.mem_write(syms['act_i'], bytes([i]))
    uc.mem_write(PRIO, bytes([prio]))
    if SMS:
        uc.mem_write(syms['act_y'], bytes(ys)); uc.mem_write(syms['act_sq'], bytes([sq]))
    if pend:
        pn, pl, pv = pend
        uc.mem_write(syms['pend_line'], bytes(pl)); uc.mem_write(syms['pend_val'], bytes(pv))
        uc.mem_write(syms['pend_n'], bytes([pn]))
    st.update(ports=bytearray(256), trace=[], w10=0, w03=0, reads=0, ins=0, base=base, k1=k1, k2=k2)
    st['ports'][0x03] = irq_line
    rnd = random.Random(1234)
    regs = [rnd.randrange(0x10000) for _ in range(7)] + [0x6000, 0x7000, 0x3E00, 0]
    for r, v in zip(REGS, regs):
        uc.reg_write(r, v)
    # the interrupt's frame: flags, CS, IP
    sp = 0x3E00 - 6
    uc.mem_write(sp, (0).to_bytes(2, 'little') + (SENT >> 4).to_bytes(2, 'little') + (0x0202).to_bytes(2, 'little'))
    uc.reg_write(UC_X86_REG_SP, sp)
    uc.reg_write(UC_X86_REG_EFLAGS, 0x0002)   # (interrupts off, as in a handler)
    uc.reg_write(UC_X86_REG_CS, 0xF000)
    try:
        uc.emu_start(0xF0000 + (entry & 0xFFFF), 0xFFFFFFFF, count=200000)
    except UcError as e:
        return ('error %s at %04X:%04X' % (e, uc.reg_read(UC_X86_REG_CS), uc.reg_read(UC_X86_REG_IP)),)
    cs, ip = uc.reg_read(UC_X86_REG_CS), uc.reg_read(UC_X86_REG_IP)
    if (cs << 4) + ip != SENT + 1:
        return ('did not return: %04X:%04X' % (cs, ip),)
    after = [uc.reg_read(r) for r in REGS]
    if after != regs:
        return ('registers changed: %s -> %s' % (regs, after),)
    state = tuple(bytes(uc.mem_read(syms[k], 1))[0] for k in STATE)
    lists = bytes(uc.mem_read(syms['act_line'], N)) + bytes(uc.mem_read(syms['act_val'], N))
    return (tuple(st['trace']), state, lists, st['ins'])

random.seed(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
bad = 0; fast_ins = []; slow_ins = []; fast_only = 0
for c in range(CASES):
    n = random.choice([1, 2, 3, 8, 24, random.randrange(1, N + 1)])
    # lines of the splits: rising, often one after the other (a road)
    lines = [0] * N; y = random.randrange(1, 100)
    for k in range(1, n):
        lines[k] = min(y, 200) if random.random() > 0.02 else random.choice([0, 143, 144, 150, 255])
        y += 1 if random.random() < 0.6 else random.randrange(1, 12)
    vals = [random.randrange(256) for _ in range(N)]
    ys = [random.randrange(256) for _ in range(N)]
    kind = random.random()
    pend = None
    if kind < 0.1 or n == 1:
        i, irq_line = random.randrange(0, n + 1), VIEW_H   # the end of the screen
        base = random.choice([144, 145, 150, 158, 0, 3])
        if random.random() < 0.5:
            pn = random.randrange(1, N + 1)
            pend = (pn, sorted(random.randrange(1, 144) for _ in range(N)), [random.randrange(256) for _ in range(N)])
    else:
        i = random.randrange(1, n)
        irq_line = (lines[i] - 1) & 0xFF
        if irq_line > VIEW_H: irq_line = VIEW_H
        if random.random() < 0.03: i = 0
        base = (irq_line + random.choice([0, 0, 0, 0, 1, 1, 2, 5, 30])) % LINES
    k1 = random.choice([0, 0, 0, 1, 1, 2]); k2 = random.choice([0, 0, 0, 0, 1, 3])
    case = (n, lines, vals, ys, i, random.random() < 0.3, int(SMS and random.random() < 0.4), base, k1, k2, irq_line, pend)
    a = run(syms['line_irq_entry'], case)
    b = run(syms['line_irq_slow'], case)
    if len(a) == 1 or len(b) == 1 or a[:3] != b[:3]:
        bad += 1
        if bad <= 5:
            print('case %d differs: n %d i %d irq line %d base %d k1 %d k2 %d prio %d sq %d' % (c, n, i, irq_line, base, k1, k2, case[5], case[6]))
            print('  lines', lines[:n]); print('  short:', a[0] if len(a) == 1 else a[0][:12], a[1:2]); print('  C:    ', b[0] if len(b) == 1 else b[0][:12], b[1:2])
    else:
        slow_ins.append(b[3])
        if a[3] < 40:
            fast_only += 1; fast_ins.append(a[3])
print('%d cases, %d differ; served by the short path alone: %d (%.1f instructions, the C handler %.1f)' % (
    CASES, bad, fast_only, sum(fast_ins) / max(len(fast_ins), 1), sum(slow_ins) / max(len(slow_ins), 1)))
sys.exit(1 if bad else 0)
