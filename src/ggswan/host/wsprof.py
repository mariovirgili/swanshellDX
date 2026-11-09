#!/usr/bin/env python3
"""Profiles the WS build's translator. Runs it (ggswan.elf, gcc-ia16) in Unicorn: drc_rt_init,
gg_init, then translate() for each block of a host BLK log; counts x86
instructions and estimated V30MZ cycles per function.
usage: wsprof.py build/ggswan.elf rom.gg blk.txt [n]
blk.txt: the blocks to translate, "BLK pc bank len n" lines (as drcsys.c
prints them with a GGSWAN_BLK hook; only pc and bank are used here).
Buffer and cache offsets as drc_rt.c (BUF_ADDR A600h, CACHE_START 0AE0h).
Cycles: a rough V30MZ estimate, for comparing, not exact."""
import sys, os, bisect, collections
from elftools.elf.elffile import ELFFile
from unicorn import *
from unicorn.x86_const import *
import capstone

ELF, ROM, BLK = sys.argv[1:4]
NMAX = int(sys.argv[4]) if len(sys.argv) > 4 else 99999
f = ELFFile(open(ELF, 'rb'))
uc = Uc(UC_ARCH_X86, UC_MODE_16)
uc.mem_map(0, 0x100000)
syms = {}
funcs = []
for s in f.iter_sections():
    if s['sh_flags'] & 2 and s['sh_size'] and s['sh_type'] == 'SHT_PROGBITS':
        uc.mem_write(s['sh_addr'] & 0xFFFFF, s.data())
st = f.get_section_by_name('.symtab')
for y in st.iter_symbols():
    a = y['st_value'] & 0xFFFFF
    if y.name == 'env' and y['st_size'] > 2:
        syms['env_rt'] = a   # (drc_rt.c's drc_env_t; drc.c has a pointer of the same name)
    syms.setdefault(y.name, a)
    if y['st_info']['type'] == 'STT_FUNC' and y['st_size']:
        funcs.append((a, a + y['st_size'], y.name))
funcs.sort()
starts = [x[0] for x in funcs]
def fname(a):
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0 and funcs[i][0] <= a < funcs[i][1]:
        return funcs[i][2]
    return '?%05X' % a

# PSRAM (32 banks) behind the windows 10000h (C1, RAM), 20000h (C2), 30000h (C3)
psram = [bytearray(0x10000) for _ in range(32)]
rom = open(ROM, 'rb').read()
for i in range(0, len(rom), 0x10000):
    psram[i >> 16][:len(rom[i:i + 0x10000])] = rom[i:i + 0x10000]
ports = bytearray(256)
win = {0x10000: None, 0x20000: None, 0x30000: None}
def set_win(base, bank):
    if win[base] == bank: return
    if win[base] is not None:
        psram[win[base]][:] = uc.mem_read(base, 0x10000)
    uc.mem_write(base, bytes(psram[bank]))
    win[base] = bank
def remap():
    set_win(0x10000, (ports[0xC1] | ports[0xC2] << 8) & 31)
    set_win(0x20000, (ports[0xC2] | ports[0xC3] << 8) & 31)
    set_win(0x30000, (ports[0xC3] | ports[0xC4] << 8) & 31)
def h_out(uc, port, size, value, ud):
    for i in range(size):
        ports[(port + i) & 255] = (value >> (8 * i)) & 255
    if 0xC0 <= port <= 0xCE: remap()
def h_in(uc, port, size, ud):
    return ports[port] if size == 1 else ports[port] | ports[port + 1] << 8
uc.hook_add(UC_HOOK_INSN, h_out, None, 1, 0, UC_X86_INS_OUT)
uc.hook_add(UC_HOOK_INSN, h_in, None, 1, 0, UC_X86_INS_IN)
remap()

# V30MZ cycle estimate (rough): 1 + memory operands, more for control flow
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
cost_cache = {}
def cost(code):
    c = cost_cache.get(code)
    if c is not None: return c
    try:
        i = next(md.disasm(code, 0))
    except StopIteration:
        return 1
    m = i.mnemonic; ops = i.op_str
    c = 1 + ops.count('[')
    if m in ('lcall',) or (m == 'call' and ':' in ops): c = 10
    elif m == 'call': c = 5
    elif m in ('retf', 'lret'): c = 8
    elif m == 'ret': c = 5
    elif m.startswith('j') or m.startswith('loop'): c = 3
    elif m in ('push', 'pop'): c = 2 + ops.count('[')
    elif m in ('div', 'idiv'): c = 16
    elif m in ('mul', 'imul'): c = 3
    elif m.startswith('rep'): c = 3
    elif m in ('movsw', 'movsb', 'stosw', 'stosb', 'lodsb', 'lodsw'): c = 2
    cost_cache[code] = c
    return c

calls = collections.Counter()
stats_n = collections.Counter(); stats_c = collections.Counter(); addr_c = collections.Counter()
cur = {'n': 0, 'c': 0, 'prof': False}
SENT = 0x3F800   # (HLT: the return address)
uc.mem_write(SENT, b'\xF4')
last = collections.deque(maxlen=8)
def h_code(uc, addr, size, ud):
    last.append('%05X %s' % (addr, fname(addr)))
    if not cur['prof']: return
    code = bytes(uc.mem_read(addr, size))
    c = cost(code)
    n = fname(addr)
    stats_n[n] += 1; stats_c[n] += c
    addr_c[addr] += c
    if addr in entry_set: calls[fname(addr)] += 1
    cur['n'] += 1; cur['c'] += c
uc.hook_add(UC_HOOK_CODE, h_code)

def call(name, ax=0, dx=0, cx=0, stack=()):
    a = syms[name]
    sp = 0x3F00
    for v in reversed(stack):
        sp -= 2; uc.mem_write(sp, (v & 0xFFFF).to_bytes(2, 'little'))
    sp -= 4; uc.mem_write(sp, bytes([0, 0, (SENT >> 4) & 255, SENT >> 12]))
    for r, v in ((UC_X86_REG_SS, 0), (UC_X86_REG_DS, 0), (UC_X86_REG_ES, 0), (UC_X86_REG_SP, sp),
                 (UC_X86_REG_AX, ax), (UC_X86_REG_DX, dx), (UC_X86_REG_CX, cx)):
        uc.reg_write(r, v)
    uc.reg_write(UC_X86_REG_CS, a >> 4)
    try:
        uc.emu_start(a, SENT + 1, count=50_000_000)
    except UcError as e:
        cs = uc.reg_read(UC_X86_REG_CS); ip = uc.reg_read(UC_X86_REG_IP)
        raise SystemExit('%s: %s at %04X:%04X (%s) last %s' % (name, e, cs, ip, fname((cs << 4) + ip), last))
    pc = (uc.reg_read(UC_X86_REG_CS) << 4) + uc.reg_read(UC_X86_REG_IP)
    if pc not in (SENT, SENT + 1):
        raise SystemExit('%s stopped at %05X (%s)' % (name, pc, fname(pc)))
    return uc.reg_read(UC_X86_REG_AX)

entry_set = set(x[0] for x in funcs)
banks = len(rom) // 0x4000
call('drc_rt_init', ax=20)
call('gg_init', ax=banks)
call('gg_reset')
call('drc_rt_rom_banks', ax=banks)
blocks = [l.split() for l in open(BLK) if l.startswith('BLK')][:NMAX]
per = []
for k, b in enumerate(blocks):
    pc = int(b[1], 16); bank = int(b[2])
    slot = pc >> 14
    if slot:
        call('z80_write', ax=0xFFFD + slot, dx=bank)
    cur.update(n=0, c=0, prof=True)
    ok = call('drc_translate', ax=syms['env_rt'], dx=pc, cx=0xA600, stack=(0x0B28, 0x1900, 0x3E00))
    ln = int.from_bytes(uc.mem_read(0x3E00, 2), 'little')
    if ok & 0xFF:
        call('psram_write', ax=0, dx=0x0B28, cx=0xA600, stack=(ln,))
    if k < 5: print('pc %04X ok %d len %d (host %s)' % (pc, ok & 0xFF, ln, b[3]))
    cur['prof'] = False
    per.append((cur['n'], cur['c'], int(b[4]), int(b[3])))
tn = sum(p[0] for p in per); tc = sum(p[1] for p in per); ti = sum(p[2] for p in per)
print('blocks %d, x86 instructions %d (%.0f a block), est. cycles %d (%.0f a block, %.0f a Z80 instruction)'
      % (len(per), tn, tn / len(per), tc, tc / len(per), tc / max(ti, 1)))
for name, c in stats_c.most_common(30):
    print('%-28s %6.1f%%  instr %8d  cycles %9d' % (name, 100 * c / tc, stats_n[name], c))

if not os.environ.get("WSPROF_LINES"): raise SystemExit
import subprocess
full = {}
for y in st.iter_symbols():
    if y['st_info']['type'] == 'STT_FUNC' and y['st_size']:
        full[y['st_value'] & 0xFFFFF] = y['st_value']
fs = {x[2]: x for x in funcs}
hi = 0xFFF00000
addrs = sorted(addr_c)
out = subprocess.run(['addr2line', '-e', ELF] + ['0x%x' % (hi | a) for a in addrs], capture_output=True, text=True).stdout.split('\n')
line_c = collections.Counter()
for a, l in zip(addrs, out):
    line_c[l.split('/')[-1] + ' ' + fname(a)] += addr_c[a]
print('--- calls a block:', ', '.join('%s %.1f' % (k, v / len(per)) for k, v in calls.most_common(15)))
print('--- hot blocks')
import pickle; pickle.dump((dict(addr_c), tc), open('addr_c.pkl','wb'))
print('--- lines')
for l, c in line_c.most_common(45):
    print('%5.1f%% %s' % (100 * c / tc, l))
