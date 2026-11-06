#!/usr/bin/env python3
"""Differential test of the Z80 -> x86 recompiler.

Each test sets up a random machine state, translates the block at PC,
runs the generated x86 code in Unicorn (16-bit real mode, the same memory
layout as on the WonderSwan) and compares the result with the interpreter.

usage: drctest.py libdrctest.so [tests] [seed] [--rom file.gg --trace N]
"""
import ctypes, random, struct, sys
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE, UcError
from unicorn.x86_const import *

lib = ctypes.CDLL(sys.argv[1])
N = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 1
VERBOSE = '-v' in sys.argv
ONLY = int(sys.argv[sys.argv.index('--only') + 1]) if '--only' in sys.argv else None

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
                ("link", ctypes.c_uint16)]
class Res(ctypes.Structure):
    _fields_ = [("length", ctypes.c_uint16), ("z80_instr", ctypes.c_uint16), ("z80_end", ctypes.c_uint16)]

lib.t_z80.restype = ctypes.POINTER(Z80)
lib.t_st.restype = ctypes.POINTER(St)
z = lib.t_z80().contents
st = lib.t_st().contents
ROM = (ctypes.c_uint8 * (512 * 1024)).in_dll(lib, "rom")
RAM = (ctypes.c_uint8 * 0x2000).in_dll(lib, "ram")
CRAM = (ctypes.c_uint8 * 0x8000).in_dll(lib, "cart_ram")
MAPPER = (ctypes.c_uint8 * 4).in_dll(lib, "mapper")
lib.t_init()

Z80_ADDR, ST_ADDR = 0xC000, 0xC100
RAM_IRAM = 0x8000
RAM_DELTA = RAM_IRAM - 0xC000
CODE_SEG, EXIT_OFF, HELPER_OFF, CODE_OFF = 0x8000, 0x0000, 0x0010, 0x0100
WIN = {0: 0x2000, 1: 0x3000, 2: 0x1000}   # slot -> window segment

iram = ctypes.create_string_buffer(0x10000)
uc = Uc(UC_ARCH_X86, UC_MODE_16)
iram_addr = ctypes.addressof(iram)
uc.mem_map_ptr(0x00000, 0x10000, 7, iram_addr)
uc.mem_map_ptr(0x100000, 0x10000, 7, iram_addr)   # 1 MiB wrap-around
uc.mem_map(0x10000, 0x30000)                         # RAM, ROM0, ROM1 windows
uc.mem_map(CODE_SEG << 4, 0x10000)

def z80_off(name):
    return getattr(Z80, name).offset

def map_windows():
    segtab = []
    for slot in range(3):
        if slot == 2 and MAPPER[0] & 8:
            page = (MAPPER[0] >> 2) & 1
            uc.mem_write(0x10000, bytes(CRAM) + bytes(0x8000))
            seg = 0x1000 + page * 0x400 - 0x800
        else:
            bank = MAPPER[1 + slot] % 32
            base = (bank >> 2) * 0x10000
            uc.mem_write(WIN[slot] << 4, bytes(ROM[base:base + 0x10000]))
            seg = WIN[slot] + (bank & 3) * 0x400 - slot * 0x400
        segtab += [seg & 0xFFFF] * 2
    segtab.append((RAM_IRAM - 0xC000) // 16 & 0xFFFF)
    segtab.append((RAM_IRAM - 0xE000) // 16 & 0xFFFF)
    return segtab

def load_uc_from_lib():
    """Copy registers, RAM and the z80/state blocks into the x86 machine."""
    uc.reg_write(UC_X86_REG_AX, z.a | z.f << 8)
    uc.reg_write(UC_X86_REG_CX, z.c | z.b << 8)
    uc.reg_write(UC_X86_REG_DX, z.e | z.d << 8)
    uc.reg_write(UC_X86_REG_BX, z.l | z.h << 8)
    uc.reg_write(UC_X86_REG_BP, z.sp)
    ctypes.memmove(iram_addr + RAM_IRAM, RAM, 0x2000)
    ctypes.memmove(iram_addr + Z80_ADDR, ctypes.addressof(z), ctypes.sizeof(Z80))
    segtab = map_windows()
    for i, s in enumerate(segtab):
        st.segtab[i] = s
    ctypes.memmove(iram_addr + ST_ADDR, ctypes.addressof(st), ctypes.sizeof(St))

def store_lib_from_uc():
    ctypes.memmove(ctypes.addressof(z), iram_addr + Z80_ADDR, ctypes.sizeof(Z80))
    ctypes.memmove(ctypes.addressof(st), iram_addr + ST_ADDR, ctypes.sizeof(St))
    ax = uc.reg_read(UC_X86_REG_AX); cx = uc.reg_read(UC_X86_REG_CX)
    dx = uc.reg_read(UC_X86_REG_DX); bx = uc.reg_read(UC_X86_REG_BX)
    z.a, z.f = ax & 255, ax >> 8
    z.c, z.b = cx & 255, cx >> 8
    z.e, z.d = dx & 255, dx >> 8
    z.l, z.h = bx & 255, bx >> 8
    z.sp = uc.reg_read(UC_X86_REG_BP)
    ctypes.memmove(RAM, iram_addr + RAM_IRAM, 0x2000)

helper_calls = [0]
def hook(uc_, address, size, user):
    if address == (CODE_SEG << 4) + HELPER_OFF:
        helper_calls[0] += 1
        store_lib_from_uc()
        lib.t_helper()
        load_uc_from_lib()   # (the x86 stack is untouched: ret follows)

uc.hook_add(UC_HOOK_CODE, hook, begin=(CODE_SEG << 4) + HELPER_OFF, end=(CODE_SEG << 4) + HELPER_OFF)
uc.mem_write(CODE_SEG << 4, b'\xF4' + bytes(15) + b'\xC3')

class Snapshot:
    def __init__(self):
        self.z = bytes(z) if False else ctypes.string_at(ctypes.addressof(z), ctypes.sizeof(Z80))
        self.ram = bytes(RAM); self.cram = bytes(CRAM); self.mapper = bytes(MAPPER)
    def restore(self):
        ctypes.memmove(ctypes.addressof(z), self.z, len(self.z))
        ctypes.memmove(RAM, self.ram, 0x2000); ctypes.memmove(CRAM, self.cram, 0x8000)
        ctypes.memmove(MAPPER, self.mapper, 4)

def state():
    return dict(a=z.a, f=z.f & 0xD7, bc=z.b << 8 | z.c, de=z.d << 8 | z.e, hl=z.h << 8 | z.l,
                sp=z.sp, ix=z.ixh << 8 | z.ixl, iy=z.iyh << 8 | z.iyl, pc=z.pc,
                af_=z.a_ << 8 | (z.f_ & 0xD7), bc_=z.b_ << 8 | z.c_, de_=z.d_ << 8 | z.e_,
                hl_=z.h_ << 8 | z.l_, iff=z.iff1, ram=bytes(RAM), mapper=bytes(MAPPER),
                cram=bytes(CRAM))

out = (ctypes.c_uint8 * 0x4000)()
res = Res()
BUDGET = 400

def run_case(pc):
    """Returns None if the block was not translated, else a list of diffs."""
    snap = Snapshot()
    ok = lib.t_translate(Z80_ADDR, ST_ADDR, EXIT_OFF, HELPER_OFF, ctypes.c_int16(RAM_DELTA),
                         pc, out, CODE_OFF, 0x3000, ctypes.byref(res))
    if not ok:
        return None, None
    code = bytes(out[:res.length])
    uc.mem_write((CODE_SEG << 4) + CODE_OFF, code)
    uc.ctl_flush_tb()   # Unicorn caches translated x86 code
    st.cycles = BUDGET
    z.pc = pc
    load_uc_from_lib()
    uc.reg_write(UC_X86_REG_DS, 0); uc.reg_write(UC_X86_REG_SS, 0)
    uc.reg_write(UC_X86_REG_ES, 0x5555)
    uc.reg_write(UC_X86_REG_SP, 0x7F00)
    uc.reg_write(UC_X86_REG_CS, CODE_SEG)
    try:
        uc.emu_start((CODE_SEG << 4) + CODE_OFF, (CODE_SEG << 4) + EXIT_OFF, count=200000)
    except UcError as e:
        return ["x86 crash: %s at %05X" % (e, uc.reg_read(UC_X86_REG_EIP))], code
    store_lib_from_uc()
    used = BUDGET - st.cycles
    drc = state()
    # reference: the interpreter from the same state
    snap.restore()
    z.pc = pc
    c = 0
    steps = 0
    while True:
        f0, op0 = z.f, lib.t_peek(z.pc)
        c += lib.t_step()
        # the recompiler leaves the undocumented bits 3 and 5 of F at 0
        if z.f != f0 and op0 not in (0xF1, 0x08):
            z.f &= 0xD7
        steps += 1
        if not (0xC002 <= z.sp <= 0xE000):
            return ["skip"], code   # the stack left work RAM: not supported
        if z.pc == drc['pc'] and c >= used:
            break
        if c > used + 100 or steps > 500000:
            break
    ref = state()
    diffs = []
    for k in drc:
        if drc[k] != ref[k]:
            if k in ('ram', 'cram'):
                # ignore the undocumented flag bits 3 and 5 (PUSH AF)
                d = [i for i in range(len(drc[k])) if (drc[k][i] ^ ref[k][i]) & 0xD7]
                if d:
                    diffs.append("%s[%d bytes, first %04X: %02X/%02X]" % (k, len(d), d[0], drc[k][d[0]], ref[k][d[0]]))
            else:
                diffs.append("%s drc=%s ref=%s" % (k, hex(drc[k]) if isinstance(drc[k], int) else drc[k],
                                                   hex(ref[k]) if isinstance(ref[k], int) else ref[k]))
    if c != used:
        diffs.append("cycles drc=%d ref=%d" % (used, c))
    return diffs, code

# --- random code -----------------------------------------------------------

SP_OPS = ([0xF9], [0x31], [0xED, 0x7B], [0xDD, 0xF9], [0xFD, 0xF9])
def rnd_instr():
    # (the recompiler assumes the stack is in work RAM: no LD SP,x here)
    while True:
        i = rnd_instr1()
        if not any(i[:len(p)] == p for p in SP_OPS):
            return i

def rnd_instr1():
    r = random.random()
    b = lambda: random.randrange(256)
    if r < 0.35: return [random.randrange(0x40, 0xC0)] if True else []
    if r < 0.55:
        op = random.choice([0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
                            0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,
                            0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,
                            0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F])
        n = {0x01:2,0x11:2,0x21:2,0x31:2,0x22:2,0x2A:2,0x32:2,0x3A:2,0x06:1,0x0E:1,0x16:1,0x1E:1,
             0x26:1,0x2E:1,0x36:1,0x3E:1}.get(op, 0)
        if op in (0x22, 0x2A, 0x32, 0x3A):   # addresses: mostly RAM
            a = random.choice([random.randrange(0xC000, 0xE000), random.randrange(0, 0x10000)])
            return [op, a & 255, a >> 8]
        return [op] + [b() for _ in range(n)]
    if r < 0.65:
        op = random.choice([0xC6,0xCE,0xD6,0xDE,0xE6,0xEE,0xF6,0xFE,0xC5,0xD5,0xE5,0xF5,0xC1,0xD1,0xE1,0xF1,
                            0xD9,0xEB,0xE3,0xF9,0xD3,0xDB,0xF3])
        n = 1 if op in (0xC6,0xCE,0xD6,0xDE,0xE6,0xEE,0xF6,0xFE,0xD3,0xDB) else 0
        return [op] + [b() for _ in range(n)]
    if r < 0.78: return [0xCB, b()]
    if r < 0.85:
        return [0xED, random.choice([0x44, 0x43, 0x4B, 0x53, 0x5B, 0x73, 0x7B, 0x42, 0x52, 0x4A, 0x5A,
                                     0xA0, 0xA3, 0xA8, 0xAB, 0xB0, 0xA1, 0x47, 0x57, 0x41, 0x49, 0x51, 0x59, 0x61, 0x69, 0x79, 0x78, 0x56]), 0xC0 + random.randrange(32), b()]
    pre = random.choice([0xDD, 0xFD])
    r2 = random.random()
    if r2 < 0.25: return [pre, 0xCB, b(), random.choice([0x46, 0x4E, 0x7E, 0xC6, 0x86, 0xFE, 0xBE, 0x06, 0x16])]
    op = random.choice([0x21, 0x2A, 0x22, 0x23, 0x2B, 0x09, 0x19, 0x29, 0x39, 0xE5, 0xE1, 0xE3, 0xF9,
                        0x36, 0x46, 0x4E, 0x56, 0x5E, 0x66, 0x6E, 0x7E, 0x70, 0x71, 0x72, 0x73, 0x74,
                        0x75, 0x77, 0x86, 0x8E, 0x96, 0x9E, 0xA6, 0xAE, 0xB6, 0xBE, 0x34, 0x35])
    if op in (0x21,): return [pre, op, b(), b()]
    if op in (0x2A, 0x22):
        a = random.choice([random.randrange(0xC000, 0xE000), random.randrange(0, 0x10000)])
        return [pre, op, a & 255, a >> 8]
    if op == 0x36: return [pre, op, b(), b()]
    if op in (0x09, 0x19, 0x29, 0x39, 0x23, 0x2B, 0xE5, 0xE1, 0xE3, 0xF9): return [pre, op]
    return [pre, op, b()]

def rnd_end():
    b = lambda: random.randrange(256)
    r = random.random()
    if r < 0.15: return [random.choice([0x20, 0x28, 0x30, 0x38, 0x18, 0x10]), random.choice([0xFE, 0xFC, b()])]
    if r < 0.30: return [random.choice([0xC2, 0xCA, 0xD2, 0xDA, 0xE2, 0xEA, 0xF2, 0xFA, 0xC3, 0xC4, 0xCC, 0xCD, 0xD4, 0xDC, 0xF4, 0xFC]), b(), b()]
    if r < 0.40: return [random.choice([0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8, 0xC9, 0xE9, 0xC7, 0xFF])]
    if r < 0.45: return [0xDD, 0xE9]
    return []

def setup_random():
    random_bytes = random.getrandbits(8 * 0x2000).to_bytes(0x2000, 'little')
    ctypes.memmove(RAM, random_bytes, 0x2000)
    MAPPER[0] = random.choice([0, 0, 0, 8, 12]); MAPPER[1] = 0
    MAPPER[2] = random.randrange(32); MAPPER[3] = random.randrange(32)
    RAM[0x1FFC:0x2000] = list(MAPPER)
    for k in "a f c b e d l h a_ f_ c_ b_ e_ d_ l_ h_ ixl ixh iyl iyh".split():
        setattr(z, k, random.randrange(256))
    # pointers mostly into RAM, so that memory operations do something
    for hi in ("h", "ixh", "iyh", "b", "d"):
        if random.random() < 0.7: setattr(z, hi, random.randrange(0xC0, 0xE0))
    z.sp = random.randrange(0xC100, 0xDF00)
    z.iff1 = z.iff2 = 0; z.halted = 0; z.ei_pending = 0; z.irq = 0; z.im = 1
    st.cycles = BUDGET

def dump(pc, code, x86, diffs):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_16
    print("pc %04X  code %s" % (pc, ' '.join('%02X' % v for v in code)))
    print("   z80 bytes:", " ".join("%02X" % lib.t_peek(pc + i) for i in range(24)))
    if x86:
        for ins in Cs(CS_ARCH_X86, CS_MODE_16).disasm(x86, CODE_OFF):
            print("   %04X  %-24s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
    print("diffs:", diffs)

def main():
    random.seed(SEED)
    rom = bytearray(random.getrandbits(8 * len(ROM)).to_bytes(len(ROM), 'little'))
    ctypes.memmove(ROM, bytes(rom), len(rom))
    fails = {}
    untranslated = 0
    skipped = 0
    for t in range(N):
        lib.t_init()
        setup_random()
        slot = random.randrange(3)
        pc = slot * 0x4000 + random.randrange(0x400, 0x3F00)
        code = []
        for _ in range(random.randrange(1, 10)):
            code += rnd_instr()
        code += rnd_end()
        # write the code into the ROM bank / cart RAM that is mapped there
        if slot == 2 and MAPPER[0] & 8:
            base = ((MAPPER[0] >> 2) & 1) * 0x4000 + (pc & 0x3FFF)
            for i, v in enumerate(code): CRAM[(base + i) & 0x7FFF] = v
        else:
            bank = MAPPER[1 + slot] if slot else 0
            base = bank * 0x4000 + (pc & 0x3FFF)
            for i, v in enumerate(code): ROM[base + i] = v
        if ONLY is not None and t != ONLY:
            continue
        diffs, x86 = run_case(pc)
        if ONLY is not None:
            dump(pc, code, x86, diffs)
        if diffs is None:
            untranslated += 1
            continue
        if diffs == ["skip"]:
            skipped += 1
            continue
        if diffs:
            key = ' '.join('%02X' % v for v in code[:12])
            fails[key] = (diffs, pc, t)
            if VERBOSE and len(fails) <= 5:
                print("FAIL", key, diffs, "pc %04X" % pc)
                print("   x86:", x86.hex())
    print("tests %d, not translated %d, skipped (stack outside RAM) %d, failing %d" % (N, untranslated, skipped, len(fails)))
    for k, (d, pc, t) in list(fails.items())[:25]:
        print("  #%-5d %-40s %s" % (t, k, '; '.join(d)))

main()
