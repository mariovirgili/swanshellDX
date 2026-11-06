#!/usr/bin/env python3
"""Recompiler test on a real game.

Runs the game with the interpreter; at many random points it translates
the block at the current PC, runs it in Unicorn (x86 16-bit, WonderSwan
memory layout), then runs the interpreter from the same state and compares
registers, work RAM and VDP state. Also prints statistics: how often the
recompiler can not start a block, and how many x86 instructions run per
Z80 instruction.

usage: drcgame.py libdrcgame.so rom.gg [samples] [seed] [keyscript]
"""
import collections, ctypes, random, sys
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE, UcError
from unicorn.x86_const import *

lib = ctypes.CDLL(sys.argv[1])
ROMPATH = sys.argv[2]
N = int(sys.argv[3]) if len(sys.argv) > 3 else 2000
SEED = int(sys.argv[4]) if len(sys.argv) > 4 else 1
KEYS = sys.argv[5] if len(sys.argv) > 5 and not sys.argv[5].startswith('-') else "200:80,210:00,400:80,410:00,600:08"
VERBOSE = '-v' in sys.argv
SKIP = int(sys.argv[sys.argv.index('--skip') + 1]) if '--skip' in sys.argv else 100

class Z80(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint8) for n in
                "a f c b e d l h a_ f_ c_ b_ e_ d_ l_ h_ ixl ixh iyl iyh".split()] + \
               [("sp", ctypes.c_uint16), ("pc", ctypes.c_uint16)] + \
               [(n, ctypes.c_uint8) for n in "i r iff1 iff2 im halted ei_pending irq".split()] + \
               [("cycles", ctypes.c_int32)]
class St(ctypes.Structure):
    _fields_ = [("segtab", ctypes.c_uint16 * 8), ("cycles", ctypes.c_int16),
                ("op", ctypes.c_uint8), ("val", ctypes.c_uint8), ("arg", ctypes.c_uint16),
                ("resume", ctypes.c_uint16), ("reason", ctypes.c_uint8), ("pad", ctypes.c_uint8),
                ("link", ctypes.c_uint16), ("keys", ctypes.c_uint8 * 4), ("table_seg", ctypes.c_uint16),
                ("nolink", ctypes.c_uint8), ("pad2", ctypes.c_uint8)]
class Res(ctypes.Structure):
    _fields_ = [("length", ctypes.c_uint16), ("z80_instr", ctypes.c_uint16), ("z80_end", ctypes.c_uint16)]

lib.t_z80.restype = ctypes.POINTER(Z80)
lib.t_st.restype = ctypes.POINTER(St)
for fn in ("t_ram", "t_rom", "t_cart", "t_mapper"):
    getattr(lib, fn).restype = ctypes.POINTER(ctypes.c_uint8)
lib.t_digest.restype = ctypes.c_uint32
lib.t_load.argtypes = [ctypes.c_char_p]
banks = lib.t_load(ROMPATH.encode())
assert banks, "can not load " + ROMPATH
z = lib.t_z80().contents
st = lib.t_st().contents
RAMP = lib.t_ram(); ROMP = lib.t_rom(); CARTP = lib.t_cart(); MAPP = lib.t_mapper()

Z80_ADDR, ST_ADDR = 0xC000, 0xC100
RAM_IRAM = 0x8000
RAM_DELTA = RAM_IRAM - 0xC000
CODE_SEG, EXIT_OFF, HELPER_OFF, CODE_OFF = 0x8000, 0x0000, 0x0010, 0x0100
WIN = {0: 0x2000, 1: 0x3000, 2: 0x1000}

iram = ctypes.create_string_buffer(0x10000)
iram_addr = ctypes.addressof(iram)
uc = Uc(UC_ARCH_X86, UC_MODE_16)
uc.mem_map_ptr(0x00000, 0x10000, 7, iram_addr)
uc.mem_map_ptr(0x100000, 0x10000, 7, iram_addr)
uc.mem_map(0x10000, 0x30000)
uc.mem_map(CODE_SEG << 4, 0x10000)
uc.mem_write(CODE_SEG << 4, b'\xF4' + bytes(15) + b'\xC3')

def map_windows():
    segtab = []
    for slot in range(3):
        if slot == 2 and MAPP[0] & 8:
            page = (MAPP[0] >> 2) & 1
            uc.mem_write(0x10000, ctypes.string_at(CARTP, 0x8000) + bytes(0x8000))
            seg = 0x1000 + page * 0x400 - 0x800
        else:
            bank = MAPP[1 + slot] % banks
            uc.mem_write(WIN[slot] << 4, ctypes.string_at(ctypes.addressof(ROMP.contents) + (bank >> 2) * 0x10000, 0x10000))
            seg = WIN[slot] + (bank & 3) * 0x400 - slot * 0x400
        segtab += [seg & 0xFFFF] * 2
    segtab.append((RAM_IRAM - 0xC000) // 16 & 0xFFFF)
    segtab.append((RAM_IRAM - 0xE000) // 16 & 0xFFFF)
    return segtab

def load_uc():
    uc.reg_write(UC_X86_REG_AX, z.a | z.f << 8)
    uc.reg_write(UC_X86_REG_CX, z.c | z.b << 8)
    uc.reg_write(UC_X86_REG_DX, z.e | z.d << 8)
    uc.reg_write(UC_X86_REG_BX, z.l | z.h << 8)
    uc.reg_write(UC_X86_REG_BP, z.sp)
    ctypes.memmove(iram_addr + RAM_IRAM, RAMP, 0x2000)
    ctypes.memmove(iram_addr + Z80_ADDR, ctypes.addressof(z), ctypes.sizeof(Z80))
    for i, s in enumerate(map_windows()):
        st.segtab[i] = s
    ctypes.memmove(iram_addr + ST_ADDR, ctypes.addressof(st), ctypes.sizeof(St))

def store_lib():
    ctypes.memmove(ctypes.addressof(z), iram_addr + Z80_ADDR, ctypes.sizeof(Z80))
    ctypes.memmove(ctypes.addressof(st), iram_addr + ST_ADDR, ctypes.sizeof(St))
    ax = uc.reg_read(UC_X86_REG_AX); cx = uc.reg_read(UC_X86_REG_CX)
    dx = uc.reg_read(UC_X86_REG_DX); bx = uc.reg_read(UC_X86_REG_BX)
    z.a, z.f, z.c, z.b = ax & 255, ax >> 8, cx & 255, cx >> 8
    z.e, z.d, z.l, z.h = dx & 255, dx >> 8, bx & 255, bx >> 8
    z.sp = uc.reg_read(UC_X86_REG_BP)
    ctypes.memmove(RAMP, iram_addr + RAM_IRAM, 0x2000)

stats = collections.Counter()
helper_ops = collections.Counter()
step_ops = collections.Counter()
def hook_helper(uc_, address, size, user):
    op = iram.raw[ST_ADDR + 18]
    helper_ops[op] += 1
    if op == 4:
        a = iram.raw[ST_ADDR + 20] | iram.raw[ST_ADDR + 21] << 8
        o = lib.t_peek(a)
        k = '%02X' % o
        if o in (0xCB, 0xED, 0xDD, 0xFD): k += ' %02X' % lib.t_peek(a + 1)
        step_ops[k] += 1
    store_lib()
    lib.t_helper()
    load_uc()
x86_count = [0]
def hook_count(uc_, address, size, user):
    x86_count[0] += 1
uc.hook_add(UC_HOOK_CODE, hook_helper, begin=(CODE_SEG << 4) + HELPER_OFF, end=(CODE_SEG << 4) + HELPER_OFF)
uc.hook_add(UC_HOOK_CODE, hook_count, begin=(CODE_SEG << 4) + CODE_OFF, end=(CODE_SEG << 4) + 0xFFFF)

def state():
    return dict(a=z.a, f=z.f & 0xD7, bc=z.b << 8 | z.c, de=z.d << 8 | z.e, hl=z.h << 8 | z.l,
                sp=z.sp, ix=z.ixh << 8 | z.ixl, iy=z.iyh << 8 | z.iyl, pc=z.pc,
                af_=z.a_ << 8 | (z.f_ & 0xD7), bc_=z.b_ << 8 | z.c_, de_=z.d_ << 8 | z.e_,
                hl_=z.h_ << 8 | z.l_, iff=z.iff1, ram=ctypes.string_at(RAMP, 0x2000),
                mapper=ctypes.string_at(MAPP, 4), vdp=lib.t_digest())

out = (ctypes.c_uint8 * 0x4000)()
res = Res()
BUDGET = 228

def run_case(pc):
    lib.t_snapshot()
    if not lib.t_translate(Z80_ADDR, ST_ADDR, EXIT_OFF, HELPER_OFF, ctypes.c_int16(RAM_DELTA),
                           pc, out, CODE_OFF, 0x3000, ctypes.byref(res)):
        return None, None, 0, 0
    code = bytes(out[:res.length])
    uc.mem_write((CODE_SEG << 4) + CODE_OFF, code)
    uc.ctl_flush_tb()
    st.cycles = BUDGET
    load_uc()
    for r, v in ((UC_X86_REG_DS, 0), (UC_X86_REG_SS, 0), (UC_X86_REG_ES, 0x5555),
                 (UC_X86_REG_SP, 0x7F00), (UC_X86_REG_CS, CODE_SEG)):
        uc.reg_write(r, v)
    x86_count[0] = 0
    try:
        uc.emu_start((CODE_SEG << 4) + CODE_OFF, (CODE_SEG << 4) + EXIT_OFF, count=500000)
    except UcError as e:
        return ["x86 crash: %s" % e], code, 0, 0
    store_lib()
    used = BUDGET - st.cycles
    drc = state()
    xcount = x86_count[0]
    lib.t_restore()
    c = steps = 0
    while True:
        f0, op0 = z.f, lib.t_peek(z.pc)
        c += lib.t_step()
        steps += 1
        if z.f != f0 and op0 not in (0xF1, 0x08):
            z.f &= 0xD7
        if z.pc == drc['pc'] and c >= used:
            break
        if c > used + 100 or steps > 500000:
            break
    ref = state()
    diffs = []
    for k in drc:
        if drc[k] != ref[k]:
            if k == 'ram':
                d = [i for i in range(0x2000) if (drc[k][i] ^ ref[k][i]) & 0xD7]
                if d: diffs.append("ram[%d bytes, first C%03X: %02X/%02X]" % (len(d), d[0], drc[k][d[0]], ref[k][d[0]]))
            else:
                diffs.append("%s drc=%s ref=%s" % (k, drc[k], ref[k]))
    if c != used:
        diffs.append("cycles drc=%d ref=%d" % (used, c))
    return diffs, code, xcount, steps

def main():
    random.seed(SEED)
    keys = dict((int(k.split(':')[0]), int(k.split(':')[1], 16)) for k in KEYS.split(',')) if KEYS else {}
    frame = 0
    def frames(n):
        nonlocal frame
        for _ in range(n):
            if frame in keys:
                lib.t_keys(keys[frame])
            lib.t_frames(1)
            frame += 1
    frames(SKIP)
    fails = collections.OrderedDict()
    not_translated = collections.Counter()
    tot_x86 = tot_z80 = 0
    blocks = 0
    # the game runs ~ 60000 cycles per frame; sample every few thousand cycles
    for t in range(N):
        frames(random.randrange(0, 3))
        lib.t_run(random.randrange(1, 228))
        if z.halted:
            continue
        pc = z.pc
        diffs, code, xc, steps = run_case(pc)
        if diffs is None:
            op = lib.t_peek(pc)
            k = '%02X' % op
            if op in (0xCB, 0xED, 0xDD, 0xFD):
                k += ' %02X' % lib.t_peek(pc + 1)
            not_translated[k] += 1
            lib.t_step()
            continue
        blocks += 1
        if not diffs:
            tot_x86 += xc; tot_z80 += steps
        else:
            key = "%04X" % pc
            if key not in fails:
                fails[key] = diffs
                if VERBOSE and len(fails) <= 3:
                    from capstone import Cs, CS_ARCH_X86, CS_MODE_16
                    print("FAIL at %04X:" % pc, diffs)
                    print("  z80:", ' '.join('%02X' % lib.t_peek(pc + i) for i in range(24)))
                    for ins in Cs(CS_ARCH_X86, CS_MODE_16).disasm(code, CODE_OFF):
                        print("   %04X %-22s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
    print("%s: %d samples, %d blocks compared, %d failing block addresses"
          % (ROMPATH.split('/')[-1], N, blocks, len(fails)))
    if tot_z80:
        print("x86 instructions per Z80 instruction: %.2f (%d / %d)" % (tot_x86 / tot_z80, tot_x86, tot_z80))
    print("blocks not started (first instruction left to the interpreter):", dict(not_translated.most_common(12)))
    print("helper calls by kind (1 write, 2 out, 3 in, 4 step):", dict(helper_ops))
    print("interpreted inside blocks:", dict(step_ops.most_common(12)))
    for k, d in list(fails.items())[:15]:
        print("  %s: %s" % (k, '; '.join(d)))

main()
