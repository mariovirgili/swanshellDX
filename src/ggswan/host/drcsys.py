#!/usr/bin/env python3
"""Whole-system test of the recompiler (see drcsys.c).

Runs a game for some frames with the recompiler (translated code executed
in Unicorn, 16-bit x86) and, separately, with the interpreter; writes
screenshots side by side (interpreter | recompiler) and prints statistics.

usage: drcsys.py libdrcsys.so rom.gg frames out_prefix [keyscript] [every]
keyscript: "frame:hexkeys,..." (GG_KEY_* bits, e.g. 80 = Start)
"""
import ctypes, sys, time
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UcError
from unicorn.x86_const import *

LIB, ROM, FRAMES, PREFIX = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
KEYS = sys.argv[5] if len(sys.argv) > 5 else ""
EVERY = int(sys.argv[6]) if len(sys.argv) > 6 else 60
CHECK = "--check" in sys.argv
PCS = "--pcs" in sys.argv
TRACE_AT = int(sys.argv[sys.argv.index("--trace") + 1]) if "--trace" in sys.argv else None
keys = dict((int(k.split(':')[0]), int(k.split(':')[1], 16)) for k in KEYS.split(',')) if KEYS else {}

def load(path):
    lib = ctypes.CDLL(path)   # (two different copies: see main)
    for fn in ("sys_fb", "sys_fb_end", "sys_iram", "sys_psram", "sys_sram", "sys_ports"):
        getattr(lib, fn).restype = ctypes.c_void_p
    lib.sys_init.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    lib.sys_digest.restype = ctypes.c_uint32
    lib.sys_stat.restype = ctypes.c_uint32
    lib.sys_code_version.restype = ctypes.c_uint32
    return lib

CODE_SEG, SENTINEL_SEG = 0x4000, 0xD000

class X86:
    def __init__(self, lib):
        self.lib = lib
        self.iram = lib.sys_iram(); self.psram = lib.sys_psram(); self.sram = lib.sys_sram()
        self.ports = (ctypes.c_uint8 * 256).from_address(lib.sys_ports())
        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self.uc.mem_map_ptr(0x00000, 0x10000, 7, self.iram)
        self.uc.mem_map_ptr(0x100000, 0x10000, 7, self.iram)
        for i in range(8):   # code banks, block table, self-check copy
            self.uc.mem_map_ptr((CODE_SEG + i * 0x1000) << 4, 0x10000, 7, self.psram + (20 + i) * 0x10000)
        self.uc.mem_map(SENTINEL_SEG << 4, 0x1000)
        self.uc.mem_write(SENTINEL_SEG << 4, b'\xF4')
        self.windows = {}
        self.code_version = None
        self.cb = ctypes.CFUNCTYPE(None, ctypes.c_uint16)(self.run)
        from unicorn import UC_HOOK_INTR
        self.uc.hook_add(UC_HOOK_INTR, self.helper)
        lib.sys_set_callback(self.cb)
        self.x86_instr = 0
        self.calls = 0

    def entry_seg(self):
        return (ctypes.c_uint16 * 2).in_dll(self.lib, "drc_entry")[1]

    def map_window(self, base, ptr):
        if self.windows.get(base) == ptr:
            return
        if base in self.windows:
            self.uc.mem_unmap(base, 0x10000)
        self.uc.mem_map_ptr(base, 0x10000, 7, ptr)
        self.windows[base] = ptr

    def helper(self, uc, intno, ud):
        # int 80h in the HCALL stub: the helper, then the windows again
        self.lib.sys_helper_direct()
        self.map_windows()
        v = self.lib.sys_code_version()
        if v != self.code_version:
            uc.ctl_flush_tb()
            self.code_version = v

    def map_windows(self):
        p = self.ports
        ram_bank = p[0xC1] | (p[0xC2] << 8)
        if p[0xCE] & 1:
            self.map_window(0x10000, self.psram + (ram_bank & 31) * 0x10000)
        else:
            self.map_window(0x10000, self.sram + (ram_bank & 7) * 0x10000)
        self.map_window(0x20000, self.psram + ((p[0xC2] | p[0xC3] << 8) & 31) * 0x10000)
        self.map_window(0x30000, self.psram + ((p[0xC3] | p[0xC4] << 8) & 31) * 0x10000)

    def run(self, offset):
        self.map_windows()
        v = self.lib.sys_code_version()
        if v != self.code_version:
            self.uc.ctl_flush_tb()
            self.code_version = v
        uc = self.uc
        z = ctypes.string_at(self.iram + 0xC000, 22)
        r16 = lambda o: z[o] | z[o + 1] << 8
        uc.reg_write(UC_X86_REG_AX, r16(0)); uc.reg_write(UC_X86_REG_CX, r16(2))
        uc.reg_write(UC_X86_REG_DX, r16(4)); uc.reg_write(UC_X86_REG_BX, r16(6))
        uc.reg_write(UC_X86_REG_BP, r16(20))
        for r, val in ((UC_X86_REG_DS, 0), (UC_X86_REG_SS, 0), (UC_X86_REG_ES, 0x5555)):
            uc.reg_write(r, val)
        # far return address -> sentinel (HLT)
        uc.mem_write(0xEFFC, bytes([0, 0, SENTINEL_SEG & 255, SENTINEL_SEG >> 8]))
        uc.reg_write(UC_X86_REG_SP, 0xEFFC)   # (free IRAM, like the C stack)
        seg = self.entry_seg()
        uc.reg_write(UC_X86_REG_CS, seg)
        self.calls += 1
        try:
            uc.emu_start((seg << 4) + offset, SENTINEL_SEG << 4, count=2000000)
        except UcError as e:
            print("x86 crash: %s at %04X:%04X" % (e, uc.reg_read(UC_X86_REG_CS), uc.reg_read(UC_X86_REG_IP)))
            import os
            os._exit(1)   # (an exception would be lost inside the ctypes callback)

def write_ppm(path, a, b):
    with open(path, 'wb') as f:
        f.write(b"P6\n%d %d\n255\n" % (160 * 2 + 8, 144))
        for y in range(144):
            f.write(a[y * 480:(y + 1) * 480] + bytes(24) + b[y * 480:(y + 1) * 480])

def main():
    import shutil, os, tempfile
    # two independent copies of the library: interpreter and recompiler
    tmp = tempfile.mkdtemp()
    shutil.copy(LIB, tmp + "/a.so"); shutil.copy(LIB, tmp + "/b.so")
    ref = load(tmp + "/a.so")
    drc = load(tmp + "/b.so")
    # reference: interpreter stopping at every line; tested: the recompiler
    # (or with --interp the interpreter) stopping only where timing matters
    every = 1 if "--every-line" in sys.argv else 0
    ref_drc = "--ref-drc" in sys.argv
    assert ref.sys_init(ROM.encode(), 1 if ref_drc else 0, 0 if ref_drc else 1)
    assert drc.sys_init(ROM.encode(), 0 if "--interp" in sys.argv else 1, every)
    if ref_drc:
        # reference: the recompiler too, without links (and dynamic lookups)
        ref.sys_nolinks(1)
        x86_ref = X86(ref)
    if "--selfcheck" in sys.argv:
        drc.sys_check(1)

    x86 = X86(drc)
    t0 = time.time()
    same = 0
    same_screen = 0
    first_diff = [None]
    cur_keys = [0]
    for fr in range(FRAMES):
        if fr in keys:
            cur_keys[0] = keys[fr]
        kk = cur_keys[0]
        ref.sys_frame(kk)
        drc.sys_trace(1 if TRACE_AT is not None and fr == TRACE_AT else 0)
        drc.sys_frame(kk)
        if PCS and fr % 10 == 0:
            za = ctypes.string_at(ref.sys_iram() + 0, 0)
            print(fr, "ref pc %04X" % ref.sys_pc(), "drc pc %04X" % drc.sys_pc(), "fb same" if ctypes.string_at(ref.sys_fb(), 69120) == ctypes.string_at(drc.sys_fb(), 69120) else "fb DIFF")
        if CHECK:
            ra = ctypes.string_at(ref.sys_iram() + 0x8000, 0x2000)
            rb = ctypes.string_at(drc.sys_iram() + 0x8000, 0x2000)
            if ra != rb:
                d = [i for i in range(0x2000) if ra[i] != rb[i]]
                print("frame %d: RAM differs at %d bytes, first C%03X (%02X/%02X)" % (fr + 1, len(d), d[0], ra[d[0]], rb[d[0]]))
                break
        a = ctypes.string_at(ref.sys_fb_end(), 160 * 144 * 3)
        b = ctypes.string_at(drc.sys_fb_end(), 160 * 144 * 3)
        if a == b:
            same_screen += 1
        elif "--show-diff" in sys.argv:
            print("  frame %d: %d pixels differ" % (fr + 1, sum(1 for i in range(0, len(a), 3) if a[i:i + 3] != b[i:i + 3])))
        if a == b and ref.sys_digest() == drc.sys_digest():
            same += 1
        elif first_diff[0] is None:
            first_diff[0] = fr + 1
        if EVERY and (fr + 1) % EVERY == 0:
            diff = sum(1 for i in range(0, len(a), 3) if a[i:i + 3] != b[i:i + 3])
            print("frame %5d: %5.1f%% pixels differ  (blocks %d, flushes %d, interpreted %d, block calls %d, %.1fs)"
                  % (fr + 1, diff * 100 / (160 * 144), drc.sys_stat(0), drc.sys_stat(1), drc.sys_stat(2),
                     x86.calls, time.time() - t0))
            write_ppm("%s_%05d.ppm" % (PREFIX, fr + 1), a, b)
    print("identical screens: %d of %d; identical screen and RAM/VRAM/CRAM: %d (first different: %s)"
          % (same_screen, FRAMES, same, first_diff[0]))
    sys.stdout.flush()
    drc.sys_tr_report()
    sys.stdout.flush()
    drc.sys_step_report()
    sys.stdout.flush(); drc.sys_hs_report()

main()
