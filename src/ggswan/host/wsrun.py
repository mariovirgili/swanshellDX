#!/usr/bin/env python3
"""Runs the WS build of the plugin (build/ggswan.elf) in Unicorn, from main(),
with a Game Gear ROM: the SD card calls (f_mount, f_open, f_read, f_close)
answered here, the ports and the cartridge windows emulated simply (no
interrupts: a HLT counts as a vertical blank). Prints, every N frames, the
estimated V30MZ cycles by profiler phase (prof_phase: AL CD DI PO ST IN TR
VI ID) and the Z80 addresses translated.
usage: wsrun.py build/ggswan.elf rom.gg frames [every] [keys "frame:hex,..."]
Cycles: a rough estimate, for comparing."""
import sys, os, bisect, collections
from elftools.elf.elffile import ELFFile
from unicorn import *
from unicorn.x86_const import *
import capstone

ELF, ROM, FRAMES = sys.argv[1], sys.argv[2], int(sys.argv[3])
EVERY = int(sys.argv[4]) if len(sys.argv) > 4 else 60
KEYS = dict((int(k.split(':')[0]), int(k.split(':')[1], 16)) for k in sys.argv[5].split(',')) if len(sys.argv) > 5 and sys.argv[5] else {}
f = ELFFile(open(ELF, 'rb'))
import ctypes
uc = Uc(UC_ARCH_X86, UC_MODE_16)
# memory: internal RAM, the three cartridge windows (RAM C1, ROM0 C2, ROM1
# C3: PSRAM or SRAM banks) and the linear area 40000h-FFFFFh (PSRAM banks
# (C0 << 4) | segment: C0 = 1 here, so the code cache banks are 20-25 and
# the plugin itself 30-31), all sharing the same bank buffers
psram = [ctypes.create_string_buffer(0x10000) for _ in range(32)]
sram = [ctypes.create_string_buffer(0x10000) for _ in range(8)]
iram = ctypes.create_string_buffer(0x10000)
uc.mem_map_ptr(0, 0x10000, 7, ctypes.addressof(iram))
uc.mem_map_ptr(0x100000, 0x10000, 7, ctypes.addressof(iram))   # (the 20-bit wrap: FC00:C000 is 0:8000)
LIN = 1
for seg in range(4, 16):
    uc.mem_map_ptr(seg << 16, 0x10000, 7, ctypes.addressof(psram[(LIN << 4 | seg) & 31]))
for s in f.iter_sections():
    if s['sh_flags'] & 2 and s['sh_size'] and s['sh_type'] == 'SHT_PROGBITS':
        uc.mem_write(s['sh_addr'] & 0xFFFFF, s.data())
syms, funcs = {}, []
for y in f.get_section_by_name('.symtab').iter_symbols():
    a = y['st_value'] & 0xFFFFF
    syms.setdefault(y.name, a)
    if y['st_info']['type'] == 'STT_FUNC' and y['st_size']:
        funcs.append((a, a + y['st_size'], y.name))
funcs.sort(); starts = [x[0] for x in funcs]
def fname(a):
    i = bisect.bisect_right(starts, a) - 1
    return funcs[i][2] if i >= 0 and funcs[i][0] <= a < funcs[i][1] else '?%05X' % a

rom = open(ROM, 'rb').read()
for i in range(0, len(rom), 0x10000):
    d = rom[i:i + 0x10000]; ctypes.memmove(psram[i >> 16], d, len(d))
path = b'GAME.GG\0'
for b in sram: ctypes.memmove(ctypes.addressof(b) + 0xE0, path, len(path))
ports = bytearray(256)
ports[0xC0] = LIN
win = {}
def set_win(base, buf):
    if win.get(base) is buf: return
    if base in win: uc.mem_unmap(base, 0x10000)
    uc.mem_map_ptr(base, 0x10000, 7, ctypes.addressof(buf)); win[base] = buf
bank = {'ram': 0, 'rom0': 0, 'rom1': 0}
def remap():
    set_win(0x10000, psram[bank['ram'] & 31] if ports[0xCE] & 1 else sram[bank['ram'] & 7])
    set_win(0x20000, psram[bank['rom0'] & 31])
    set_win(0x30000, psram[bank['rom1'] & 31])
    uc.ctl_flush_tb()   # (code written through the RAM window: the linear area sees it)
remap()

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
cost_cache = {}
# The cycles of an instruction as Mednafen's V30MZ core counts them (v30mz.c:
# what nileswan-medem runs; CLKM(memory, register) by the ModRM byte, a
# conditional jump 1 not taken and 3 taken, a segment prefix 1). WSRUN_ROUGH=1:
# the older rough estimate below (1 + 1 per memory operand, jumps 3), about
# a third higher on the translated code.
ROUGH = bool(os.environ.get('WSRUN_ROUGH'))
STR_CYC = {0xA4: 5, 0xA5: 5, 0xA6: 9, 0xA7: 9, 0xAA: 3, 0xAB: 3, 0xAC: 3, 0xAD: 3, 0xAE: 4, 0xAF: 4,
           0x6C: 6, 0x6D: 6, 0x6E: 7, 0x6F: 7}
def cost_m(code, cx, first):
    """(cycles, extra if the jump is taken)"""
    i = 0; c = 0; rep = False
    while i < len(code) and code[i] in (0x26, 0x2E, 0x36, 0x3E, 0xF2, 0xF3):
        if code[i] in (0xF2, 0xF3): rep = True
        else: c += 2 if rep else 1
        i += 1
    if i >= len(code): return c + 1, 0
    op = code[i]
    m = code[i + 1] if i + 1 < len(code) else 0xC0
    mem = m < 0xC0
    def clkm(a, b): return a if mem else b
    if rep and op in STR_CYC:
        # (the hook runs once for each turn and once more with CX = 0)
        return c + (5 if first else 0) + (STR_CYC[op] if cx else 0), 0
    if op in STR_CYC: return c + (6 if op in (0xA6, 0xA7) else STR_CYC[op]), 0
    if op < 0x40:
        k = op & 7
        if k < 4:
            if (op & 0x38) == 0x38: return c + clkm(2, 1), 0          # CMP
            return c + (clkm(3, 1) if k < 2 else clkm(2, 1)), 0
        if k < 6: return c + 1, 0
        if op in (0x27, 0x2F): return c + 10, 0
        if op in (0x37, 0x3F): return c + 9, 0
        return c + (2 if k == 6 else 3), 0                           # push / pop of a segment
    if op < 0x60: return c + 1, 0
    if op == 0x60: return c + 9, 0
    if op == 0x61: return c + 8, 0
    if op in (0x68, 0x6A): return c + 1, 0
    if op in (0x69, 0x6B): return c + clkm(4, 3), 0
    if 0x70 <= op < 0x80: return c + 1, 2
    if op < 0x84: return c + clkm(3, 1), 0
    if op < 0x86: return c + clkm(2, 1), 0
    if op < 0x88: return c + clkm(5, 3), 0
    if op < 0x8E: return c + 1, 0
    if op == 0x8E: return c + clkm(3, 2), 0
    if op == 0x8F: return c + clkm(3, 1), 0
    if op < 0x98: return c + 3, 0
    if op < 0x9A: return c + 1, 0
    if op == 0x9A: return c + 10, 0
    if op == 0x9C: return c + 2, 0
    if op == 0x9D: return c + 3, 0
    if op == 0x9E: return c + 4, 0
    if op == 0x9F: return c + 2, 0
    if op < 0xC0: return c + 1, 0                                    # A0-A3, A8, A9, B0-BF
    if op < 0xC2: return c + clkm(5, 3), 0
    if op < 0xC4: return c + 6, 0
    if op < 0xC6: return c + 6, 0
    if op < 0xC8: return c + 1, 0
    if op == 0xC8: return c + 19, 0
    if op == 0xC9: return c + 2, 0
    if op == 0xCA: return c + 9, 0
    if op == 0xCB: return c + 8, 0
    if op in (0xCC, 0xCD): return c + 10, 0
    if op == 0xCF: return c + 10, 0
    if op in (0xD0, 0xD1): return c + clkm(3, 1), 0
    if op in (0xD2, 0xD3): return c + clkm(5, 3), 0
    if op == 0xD4: return c + 17, 0
    if op == 0xD5: return c + 6, 0
    if op == 0xD6: return c + 3, 0
    if op == 0xD7: return c + 5, 0
    if op < 0xE0: return c + 1, 0
    if op < 0xE2: return c + 3, 3
    if op == 0xE2: return c + 2, 3
    if op == 0xE3: return c + 1, 3
    if op < 0xE8: return c + 6, 0
    if op == 0xE8: return c + 5, 0
    if op == 0xE9: return c + 4, 0
    if op == 0xEA: return c + 7, 0
    if op == 0xEB: return c + 4, 0
    if op < 0xF0: return c + 6, 0
    if op in (0xF6, 0xF7):
        r = (m >> 3) & 7; w = op & 1
        if r < 2: return c + clkm(2, 1), 0
        if r < 4: return c + clkm(3, 1), 0
        if r < 6: return c + clkm(4, 3), 0
        if r == 6: return c + (clkm(24, 23) if w else clkm(16, 15)), 0
        return c + (clkm(25, 24) if w else clkm(18, 17)), 0
    if op == 0xFE: return c + clkm(3, 1), 0
    if op == 0xFF:
        r = (m >> 3) & 7
        return c + (clkm(3, 1), clkm(3, 1), clkm(6, 5), 12, clkm(5, 4), 10, clkm(2, 1), 1)[r], 0
    if op == 0xF4: return c + 1, 0
    return c + 4, 0                                                  # F5, F8-FD

# The cycles on the console (a SwanCrystal, measured 2026-11-16 with gbswan's
# GB_HWTEST bench, the same from internal RAM and from the cartridge's): an
# instruction takes what it takes to run or to be fetched (two bytes a cycle),
# whichever is longer, and one cycle more for each access to memory (two for a
# word at an odd address); a jump taken three more than its fetch, and one
# more to an odd address; a port 8 (10 one of the cartridge's if it waits,
# port 60h bit 3, which the menu sets unless "Fast SRAM"). Measured: nop 1,
# mov al,[bx] 2, mov ax,[bx+1] 3.5, es: mov al,[bx] 2.5, push + pop 3,
# shl ax,4 3, jmp short 4/5, jmp near 5/6, call + ret 15-16, out 8/10.
# Not modelled: the cycle more of an access through the RAM window when the
# cartridge waits (port 60h bit 1), and a word at an odd address is seen only
# where the address is in the instruction.
JUMPS = (0xE8, 0xE9, 0xEA, 0xEB, 0xC2, 0xC3, 0xCA, 0xCB, 0xCF, 0x9A)
STACK = {0x06: 1, 0x0E: 1, 0x16: 1, 0x1E: 1, 0x07: 1, 0x17: 1, 0x1F: 1, 0x68: 1, 0x6A: 1, 0x9C: 1, 0x9D: 1,
         0xE8: 1, 0xC2: 1, 0xC3: 1, 0x9A: 2, 0xCA: 2, 0xCB: 2, 0xCD: 3, 0xCF: 3, 0xC9: 1, 0xD7: 1}
_mdd = []
HWP = {}      # an instruction's cycles more than it takes to run, for its length, and its accesses to memory
def accesses(code, op, m):
    """the accesses to memory of an instruction (not a string one)"""
    if not _mdd:
        import capstone as _cs
        d = _cs.Cs(_cs.CS_ARCH_X86, _cs.CS_MODE_16); d.detail = True; _mdd.append(d)
    n = STACK.get(op, 0)
    if op == 0xFF: n += (0, 0, 1, 2, 0, 0, 1, 0)[m >> 3 & 7]
    if op == 0x8D: return n
    try: ins = next(_mdd[0].disasm(code, 0))
    except StopIteration: return n
    for o in ins.operands:
        if o.type == 3:     # (memory)
            k = (1 if o.access & 1 else 0) + (1 if o.access & 2 else 0) or 1
            if o.size == 2 and o.mem.base == 0 and o.mem.index == 0 and o.mem.disp & 1: k *= 2
            n += k
    return n
def cost_hw(code, cx, first):
    """(cycles, extra if the jump is taken, 1: a jump)"""
    n = len(code); i = 0
    while i < n and code[i] in (0x26, 0x2E, 0x36, 0x3E, 0xF2, 0xF3): i += 1
    if i >= n: return n / 2, 0, 0
    op = code[i]; m = code[i + 1] if i + 1 < n else 0xC0
    if op in STR_CYC:
        c, ex = cost_m(code, cx, first)
        return c, 0, 0
    if 0x50 <= op < 0x60: return 1.5, 0, 0
    if op == 0x90: return 1, 0, 0
    if 0x70 <= op < 0x80 or 0xE0 <= op < 0xE4: return max(1, n / 2), 3, 1
    if op == 0xEB: return 4, 0, 1
    if op == 0xE9: return 5, 0, 1
    if op == 0xE8: return 8, 0, 1
    if op in (0xC2, 0xC3): return 7, 0, 1
    if 0xE4 <= op < 0xE8: return (8 + 2 * HWWAIT if m >= 0xC0 else 8), 0, 0
    if 0xEC <= op < 0xF0: return 8, 0, 0
    # (what the instruction takes between registers, and its accesses)
    r = bytearray(code[i:i + 2])
    if len(r) > 1: r[1] |= 0xC0
    c, ex = cost_m(bytes(r), cx, first)
    HWP[code] = (max(0, n / 2 - c), accesses(code, op, m))
    return max(c, n / 2) + accesses(code, op, m), 0, 1 if op in JUMPS or (op == 0xFF and (m >> 3 & 7) in (2, 3, 4, 5)) else 0
HWWAIT = 0 if os.environ.get('WSRUN_FAST') or os.environ.get('GBRUN_FAST') else 1     # WSRUN_FAST=1, GBRUN_FAST=1: the cartridge does not wait

def cost(code):
    c = cost_cache.get(code)
    if c is not None: return c
    try: i = next(md.disasm(code, 0)); m, ops = i.mnemonic, i.op_str
    except StopIteration: m, ops = '?', ''
    c = 1 + ops.count('[')
    if m == 'lcall' or (m == 'call' and ':' in ops): c = 10
    elif m == 'call': c = 5
    elif m in ('retf', 'lret', 'iret'): c = 8
    elif m == 'ret': c = 5
    elif m.startswith('j') or m.startswith('loop'): c = 3
    elif m in ('push', 'pop'): c = 2 + ops.count('[')
    elif m in ('div', 'idiv'): c = 16
    elif m in ('mul', 'imul'): c = 3
    elif m in ('in', 'out'): c = 6
    cost_cache[code] = c
    return c

st = {'cyc': 0, 'frame': 0}
CLIX = os.environ.get('WSRUN_CLI')
cli_st = [0, '']; cli_n = collections.Counter(); cli_sum = collections.Counter(); cli_max = collections.Counter()
OPSPREV = {}
PH = 'AL CD DI PO ST IN TR VI ID'.split()
ph_c = collections.Counter(); fn_c = collections.Counter(); tr_pcs = collections.Counter()
PROF = syms['prof_phase']
PCSYM = syms['z80'] + 22   # z80.pc
# WSRUN_WSKEYS="frame:hex,frame:hex": the WonderSwan's own keys from that
# frame on, as ws_keypad_scan gives them (002 START, 004 A, 008 B, 010-080
# X1-X4, 100-800 Y1-Y4): what main.c's read_keys does with them (sfswan's
# keyboard on the screen)
WS_KEYS = dict((int(x.split(':')[0]), int(x.split(':')[1], 16)) for x in os.environ.get('WSRUN_WSKEYS', '').split(',') if x)
ws_keys = [WS_KEYS.get(0, 0)]   # (frame 0: held while the plugin starts)
def h_in(uc, port, size, ud):
    if port == 0x02: return (st['cyc'] // 256) % 159
    if port == 0xB5:
        # the keypad, a group at a time (ws_keypad_scan): WSRUN_WSKEYS
        k, g = ws_keys[0], ports[0xB5]
        return (g & 0xF0) | ((k >> 8) & 15 if g & 0x10 else 0) | ((k >> 4) & 15 if g & 0x20 else 0) | (k & 15 if g & 0x40 else 0)
    if port == 0xE4: return 0xFFFF
    if port == 0xA0: return 0x86   # (Color)   # (nileswan: segment mask)
    return ports[port] if size == 1 else ports[port] | ports[port + 1] << 8
BANKPORT = {0xC1: 'ram', 0xC2: 'rom0', 0xC3: 'rom1', 0xD0: 'ram', 0xD2: 'rom0', 0xD4: 'rom1'}
def h_out(uc, port, size, value, ud):
    for i in range(size):
        p = (port + i) & 255; v = (value >> (8 * i)) & 255
        ports[p] = v
        if p in (0xC1, 0xC2, 0xC3, 0xD0, 0xD2, 0xD4):
            bank[BANKPORT[p]] = (bank[BANKPORT[p]] & 0xFF00) | v
        elif p in (0xD1, 0xD3, 0xD5):
            k = BANKPORT[p - 1]; bank[k] = (bank[k] & 0xFF) | v << 8
        if p == 0x52 and v & 0x80 and SDMA:
            src = ports[0x4A] | ports[0x4B] << 8 | (ports[0x4C] & 15) << 16
            n = ports[0x4E] | ports[0x4F] << 8
            sdma_chunks.append((st['cyc'] // 256, bytes(uc.mem_read(src, n))))
            sdma_n[0] += 1
    if 0xC0 <= port <= 0xD5: remap()
SHOT = os.environ.get('WSRUN_SHOT'); shot = set()
STUCK = int(os.environ.get('WSRUN_STUCK', '0'))
SDMA = os.environ.get('WSRUN_SDMA')
sdma_n = [0]; sdma_chunks = []
def sdma_save():
    # what the WS plays: each chunk from the line its DMA started (12 kHz:
    # a sample a line), cut by the next one; the last level held in the gaps
    if not SDMA or not sdma_chunks: return
    t0 = sdma_chunks[0][0]; out = bytearray([0x80]) * (sdma_chunks[-1][0] - t0 + len(sdma_chunks[-1][1]))
    for i, (t, d) in enumerate(sdma_chunks):
        end = sdma_chunks[i + 1][0] - t0 if i + 1 < len(sdma_chunks) else len(out)
        seg = (d + bytes([d[-1]]) * max(0, end - (t - t0) - len(d)))[:end - (t - t0)]   # (the last level held)
        out[t - t0:t - t0 + len(seg)] = seg
    import wave
    w = wave.open(SDMA, 'wb'); w.setnchannels(1); w.setsampwidth(1); w.setframerate(12000); w.writeframes(bytes(out)); w.close()
import atexit; atexit.register(sdma_save)
uc.hook_add(UC_HOOK_INSN, h_in, None, 1, 0, UC_X86_INS_IN)
uc.hook_add(UC_HOOK_INSN, h_out, None, 1, 0, UC_X86_INS_OUT)

def retf(pop=0):
    sp = uc.reg_read(UC_X86_REG_SP)
    ip, cs = int.from_bytes(uc.mem_read(sp, 2), 'little'), int.from_bytes(uc.mem_read(sp + 2, 2), 'little')
    uc.reg_write(UC_X86_REG_SP, sp + 4 + pop)
    uc.reg_write(UC_X86_REG_CS, cs); uc.reg_write(UC_X86_REG_IP, ip)
rom_pos = [0]
sys_disk = open(os.environ['WSRUN_SYS'], 'rb').read() if os.environ.get('WSRUN_SYS') else None
cv_bios = open(os.environ['WSRUN_BIOS'], 'rb').read() if os.environ.get('WSRUN_BIOS') else None
cur = [rom]
STATE_F = os.environ.get('WSRUN_STATE')
state_file = [bytearray(open(STATE_F, 'rb').read()) if STATE_F and os.path.exists(STATE_F) else None]
def sd(name):
    ax = uc.reg_read(UC_X86_REG_AX)
    if name == 'f_open':
        # (WSRUN_SYS=file: what sfswan finds at /NILESWAN/SF7000.SF, its
        # system disk; anything else opened is the ROM or disk given)
        path = bytes(uc.mem_read(uc.reg_read(UC_X86_REG_DX), 64)).split(b'\0')[0]
        if os.environ.get('WSRUN_STDBG'): print('f_open', path, uc.reg_read(UC_X86_REG_CX) & 255)
        if path.upper().endswith(b'.ST0'):
            # (a state's file: kept here, and in WSRUN_STATE=file from one run to the next)
            if uc.reg_read(UC_X86_REG_CX) & 2:
                state_file[0] = bytearray(); cur[0] = None
            elif state_file[0] is None:
                uc.reg_write(UC_X86_REG_AX, 4); retf(); return   # FR_NO_FILE
            else:
                cur[0] = bytes(state_file[0]); rom_pos[0] = 0
            uc.reg_write(UC_X86_REG_AX, 0); retf(); return
        if b'COLECO.ROM' in path.upper():
            if not cv_bios:
                uc.reg_write(UC_X86_REG_AX, 4); retf(); return   # FR_NO_FILE
            cur[0] = cv_bios
            uc.mem_write(ax + 0xA, len(cur[0]).to_bytes(4, 'little')); rom_pos[0] = 0
            uc.reg_write(UC_X86_REG_AX, 0); retf(); return
        cur[0] = sys_disk if (sys_disk and b'SF7000.SF' in path.upper()) else rom
        if b'SF7000.SF' in path.upper() and not sys_disk:
            uc.reg_write(UC_X86_REG_AX, 4); retf(); return   # FR_NO_FILE
        uc.mem_write(ax + 0xA, len(cur[0]).to_bytes(4, 'little')); rom_pos[0] = 0
    elif name == 'f_read':
        # (fp AX, buff DX, btr and br on the stack: the callee pops them)
        buf = uc.reg_read(UC_X86_REG_DX)
        sp = uc.reg_read(UC_X86_REG_SP)
        n = int.from_bytes(uc.mem_read(sp + 4, 2), 'little')
        brp = int.from_bytes(uc.mem_read(sp + 6, 2), 'little')
        d = cur[0][rom_pos[0]:rom_pos[0] + n]; rom_pos[0] += len(d)
        uc.mem_write(buf, d); uc.mem_write(brp, len(d).to_bytes(2, 'little'))
        uc.reg_write(UC_X86_REG_AX, 0); retf(4); return
    elif name == 'f_write':
        # (fp AX, buff DX with its segment in CX, btw and bw on the stack)
        buf = (uc.reg_read(UC_X86_REG_CX) << 4) + uc.reg_read(UC_X86_REG_DX)
        sp = uc.reg_read(UC_X86_REG_SP)
        n = int.from_bytes(uc.mem_read(sp + 4, 2), 'little')
        bwp = int.from_bytes(uc.mem_read(sp + 6, 2), 'little')
        state_file[0] += bytes(uc.mem_read(buf & 0xFFFFF, n))
        uc.mem_write(bwp, n.to_bytes(2, 'little'))
        uc.reg_write(UC_X86_REG_AX, 0); retf(4); return
    uc.reg_write(UC_X86_REG_AX, 0); retf()
SD = {syms[n]: n for n in ('f_mount', 'f_open', 'f_read', 'f_close', 'f_write') if n in syms}
FAIL, FRAME, TRANS = syms['fail'], syms['gg_run_frame'], syms['drc_translate']
# (a state kept and brought back: the frames; WSRUN_STCMP=1: what is not
# as it was when kept, at the first frame run after it is brought back)
st_snap = [None, 0]
def h_state(uc, a, sz, n):
    print('%s at frame %d' % (n, st['frame']))
    if n == 'state_save': st_snap[0] = (bytes(uc.mem_read(0, 0x10000)), bytes(psram[0].raw), bytes(psram[1].raw), bytes(sram[0].raw), bytes(ports))
    else: st_snap[1] = 1
def st_compare():
    st_snap[1] = 0
    now = (bytes(uc.mem_read(0, 0x10000)), bytes(psram[0].raw), bytes(psram[1].raw), bytes(sram[0].raw), bytes(ports))
    for nm, a, b in zip(('internal RAM', 'PSRAM bank 0', 'PSRAM bank 1', 'SRAM', 'ports'), st_snap[0], now):
        d = [i for i in range(len(a)) if a[i] != b[i]]
        runs = []
        for i in d:
            if runs and i - runs[-1][1] <= 8: runs[-1][1] = i
            else: runs.append([i, i])
        print('  %s: %d bytes differ: %s' % (nm, len(d), ' '.join('%04X-%04X' % (x, y) for x, y in runs[:40])))
for _n in ('state_save', 'state_load'):
    if _n in syms:
        uc.hook_add(UC_HOOK_CODE, h_state, _n, syms[_n], syms[_n])
KEYSYM = syms['gg'] + 0   # (gg.keys: found below)

trail = collections.deque(maxlen=40)
jst = [0, 0, 0, 0]   # taken-jump extra, the address after it, the last address, a jump
# WSRUN_HW=1: the cycles as the console takes them (cost_hw)
HW = bool(os.environ.get('WSRUN_HW'))
WATCH = dict((syms[n], n) for n in os.environ.get('WSRUN_WATCH', '').split(',') if n and n in syms)
long_n = collections.Counter(); long_max = collections.Counter(); rep_run = [0, 0]
LONGX = os.environ.get('WSRUN_LONGX'); long_hist = collections.Counter()   # (WSRUN_LONGX=1: those REPs by length and frame)
def blk_name(addr):
    if 0x40000 <= addr < 0xA0000:
        off = addr & 0xFFFF
        for lo, hi, nm in STUBS:
            if lo <= off < hi: return nm
        i = bisect.bisect_right(blk_starts, addr) - 1
        if i >= 0 and addr < blk_info[i][0]: return 'block %04X' % blk_info[i][1]
        return 'code %05X' % addr
    return fname(addr)
def h_code(uc, addr, size, ud):
    n = fname(addr)
    if n[0] == '?': n = '?%03X' % (addr >> 8)
    if not trail or trail[-1][0] != n: trail.append([n, addr])
    else: trail[-1][1] = addr
    if addr in SD:
        sd(SD[addr]); return
    if addr == FAIL:
        ax, dx, cx = (uc.reg_read(r) for r in (UC_X86_REG_AX, UC_X86_REG_DX, UC_X86_REG_CX))
        txt = bytes(uc.mem_read((dx << 4) + ax, 16)).split(b'\0')[0]
        raise SystemExit('fail(%r, %04X)' % (txt, cx))
    if addr in COUNT_AT: counts[COUNT_AT[addr]] += 1
    if addr in WATCH:
        # WSRUN_WATCH=function,...: every call, with the Game Gear frame and
        # line, the bytes in the log of deferred VRAM writes and the caller
        gf = int.from_bytes(uc.mem_read(syms['gg'] + 0x6C, 4), 'little')
        sp = (uc.reg_read(UC_X86_REG_SS) << 4) + uc.reg_read(UC_X86_REG_SP)
        ret = int.from_bytes(uc.mem_read(sp, 2), 'little') + (int.from_bytes(uc.mem_read(sp + 2, 2), 'little') << 4)
        print('   WATCH %s: frame %d (Game Gear %d), log %d bytes, Z80 pc %04X, called from %s' % (
            WATCH[addr], st['frame'], gf, int.from_bytes(uc.mem_read(syms['drc_st'] + 304, 2), 'little') - int.from_bytes(uc.mem_read(syms['log_start'], 2), 'little'),
            int.from_bytes(uc.mem_read(PCSYM, 2), 'little'), fname(ret & 0xFFFFF)))
    if addr == TRANS:
        tr_pcs[uc.reg_read(UC_X86_REG_DX)] += 1
        last_tr[0] = uc.reg_read(UC_X86_REG_DX)
    if addr == PSW and last_tr[0] is not None:
        # psram_write(which, off, src, len): the block just translated
        which = uc.reg_read(UC_X86_REG_AX) & 255; off = uc.reg_read(UC_X86_REG_DX)
        ln = int.from_bytes(uc.mem_read((uc.reg_read(UC_X86_REG_SS) << 4) + uc.reg_read(UC_X86_REG_SP) + 4, 2), 'little')
        if which < 6:
            base = 0x40000 + which * 0x10000 + off
            i = bisect.bisect_left(blk_starts, base)
            blk_starts.insert(i, base); blk_info.insert(i, (base + ln, last_tr[0]))
        last_tr[0] = None
    if addr == FRAME:
        st['f0'] = st['cyc']
        st['frame'] += 1
        fr = st['frame']
        if st_snap[1] and st_snap[0] and os.environ.get('WSRUN_STCMP'): st_compare()
        if fr in KEYS: pending_keys[0] = KEYS[fr]
        # WSRUN_POKE=symbol:hex[,symbol:hex]: bytes written at the start
        # of every frame (what a key of the WonderSwan would set)
        for pk in os.environ.get('WSRUN_POKE', '').split(','):
            if pk:
                uc.mem_write(syms[pk.split(':')[0]], bytes([int(pk.split(':')[1], 16)]))
        # WSRUN_POKE1=frame:symbol:hex: a byte written once, at that frame's start
        for pk in os.environ.get('WSRUN_POKE1', '').split(','):
            if pk and int(pk.split(':')[0]) == fr:
                uc.mem_write(syms[pk.split(':')[1]], bytes([int(pk.split(':')[2], 16)]))
        uc.mem_write(syms['gg'] + 0x6C, bytes([pending_keys[0]]))   # gg.keys (read_keys: none here)
        if fr in WS_KEYS: ws_keys[0] = WS_KEYS[fr]
        if os.environ.get('WSRUN_STDBG') and 'st_held' in syms and fr % 10 == 0: print('ST', fr, ws_keys[0], [uc.mem_read(syms[n], 1)[0] for n in ('st_held', 'st_req', 'st_wait', 'st_t0', 'vblanks')])
        if SF_TYPE: sf_type(fr)
        if fr % EVERY == 0: report(fr)
        if fr >= FRAMES: uc.emu_stop()
    if DYNX:
        if 0x40000 <= addr < 0xA0000 and (addr & 0xFFFF) == 0x4B:
            site = prev_addr[0]; tgt = int.from_bytes(uc.mem_read(PCSYM, 2), 'little')
            last = dyn_last.get(site)
            dyn_c['same' if last == tgt else ('first' if last is None else 'other')] += 1
            dyn_last[site] = tgt; dyn_sites[site] += 1
        prev_addr[0] = addr
    if ROUGH:
        c = cost(bytes(uc.mem_read(addr, size)))
    elif HW:
        c, ex, fl = cost_hw(bytes(uc.mem_read(addr, size)), uc.reg_read(UC_X86_REG_CX), jst[2] != addr)
        if addr != jst[1]:
            if jst[0]: c += jst[0]
            if (jst[0] or jst[3]) and addr & 1: c += 1
        jst[0] = ex; jst[1] = addr + size; jst[2] = addr; jst[3] = fl
    else:
        c, ex = cost_m(bytes(uc.mem_read(addr, size)), uc.reg_read(UC_X86_REG_CX), jst[2] != addr)
        if jst[0] and addr != jst[1]: c += jst[0]     # the jump before this one was taken
        jst[0] = ex; jst[1] = addr + size; jst[2] = addr
    st['cyc'] += c
    # WSRUN_STUCK=cycles: a frame that takes more than that many (a loop
    # that does not end): where it is, and stop
    if STUCK and st['cyc'] - st.get('f0', 0) > STUCK:
        cs = uc.reg_read(UC_X86_REG_CS); ip = uc.reg_read(UC_X86_REG_IP)
        print('STUCK in frame %d at %04X:%04X (%s); Z80 pc %04X; trail %s' % (st['frame'], cs, ip, fname(addr),
            int.from_bytes(uc.mem_read(PCSYM, 2), 'little'), ' '.join('%s@%05X' % (n, a) for n, a in trail)))
        sys.stdout.flush()
        os._exit(3)
    # an instruction no interrupt gets into (a REP: Mednafen's V30MZ runs
    # all of it in one step, so a line interrupt waits that long in
    # nileswan-medem; the real CPU takes interrupts between its turns).
    # Unicorn comes here for every turn of a REP: the turns at one address
    if addr == rep_run[0]:
        rep_run[1] += c
    else:
        if rep_run[1] > 400:
            k = blk_name(rep_run[0])
            long_n[k] += 1
            if rep_run[1] > long_max[k]: long_max[k] = rep_run[1]
            if LONGX: long_hist[k, rep_run[1] // 500 * 500, st['frame'] // 100 * 100] += 1
        rep_run[0] = addr; rep_run[1] = c
    if CLIX:
        # WSRUN_CLI=1: stretches with interrupts off (cycles), by the
        # function they start in: how late a line interrupt can be served
        if uc.reg_read(UC_X86_REG_EFLAGS) & 0x200:
            if cli_st[0]:
                f0 = cli_st[1]
                cli_n[f0] += 1; cli_sum[f0] += cli_st[0]
                if cli_st[0] > cli_max[f0]: cli_max[f0] = cli_st[0]
                cli_st[0] = 0
        else:
            if not cli_st[0]: cli_st[1] = fname(addr)
            cli_st[0] += c
    ph = uc.mem_read(PROF, 1)[0]
    ph_c[ph] += c
    if ph == 2: di_c[fname(addr)] += c
    if 0x40000 <= addr < 0xA0000:
        i = bisect.bisect_right(blk_starts, addr) - 1
        if i >= 0 and addr < blk_info[i][0]:
            blk_c[blk_info[i][1]] += c
            if DUMPB: dump_n[addr] += 1
            if LINKX:
                b = bytes(uc.mem_read(addr - 2, 7))
                if b[2] == 0xE9 and b[5:7] == b'\x90\x90':
                    d = ((addr + 3 + (b[3] | b[4] << 8)) & 0xFFFF) - (addr & 0xFFFF)
                    if d > 0x7FFF: d -= 0x10000
                    if d < -0x8000: d += 0x10000
                    kind = 'plain' if b[0:2] == b'\x7E\x05' else 'guarded'
                    link_c[kind, 'rel8' if -128 <= d <= 127 else 'near'] += 1
                    link_sites[addr] += 1
                elif b[2] == 0xEA: link_c['plain' if b[0:2] == b'\x7E\x05' else 'guarded', 'far'] += 1
                elif b[2:4] == b'\xEB\x03' and b[4:7] == b'\x90\x90\x90': link_c['unlinked', '-'] += 1
            if MNEM:
                # WSRUN_MNEM: the translated blocks' instructions by kind
                code = bytes(uc.mem_read(addr, size))
                k = mn_cache.get(code)
                if k is None:
                    ins = next(md.disasm(code, 0), None)
                    k = mn_cache[code] = (ins.mnemonic + ' ' + ins.op_str) if ins else '?'
                    k = mn_cache[code] = mn_class(k)
                mn_c[k] += c
        else:
            o = addr & 0xFFFF
            if STUBX and 0x40 <= o < 0x1110: stub_n[o] += 1
            blk_c[next((n for lo, hi, n in STUBS if lo <= o < hi), 'unmapped')] += c
    fn_c[fname(addr)] += c
    if FOCUS and fname(addr) == FOCUS: focus_c[addr] += c
uc.hook_add(UC_HOOK_CODE, h_code)
pending_keys = [0]
import os
FOCUS = os.environ.get('WSRUN_FOCUS'); focus_c = collections.Counter()
COUNT_AT = {syms[n]: n for n in ('drc_enter', 'event_step', 'drc_fast_reentry', 'drc_helper', 'gg_line_event', 'z80_step') if n in syms}
counts = collections.Counter(); di_c = collections.Counter()
blk_c = collections.Counter(); blk_starts = []; blk_info = []; last_tr = [None]
PSW = syms['psram_write']
STUBX = os.environ.get('WSRUN_STUBX'); stub_n = collections.Counter()
DYNX = os.environ.get('WSRUN_DYNX'); dyn_c = collections.Counter(); dyn_last = {}; dyn_sites = collections.Counter(); prev_addr = [0]
LINKX = os.environ.get('WSRUN_LINKX'); link_c = collections.Counter(); link_sites = collections.Counter()
DUMPB = os.environ.get('WSRUN_DUMPBLK'); dump_n = collections.Counter()
MNEM = os.environ.get('WSRUN_MNEM'); mn_c = collections.Counter(); mn_cache = {}
import re as _re
def mn_class(t):
    # the instruction with its operands generalised (numbers -> N)
    return _re.sub(r'0x[0-9a-f]+|\b\d+\b', 'N', t)
# the routines at the start of each code bank (drc_rt.c STUB_*)
STUBS = [(0x0000, 0x0020, 'EXIT'), (0x0020, 0x0040, 'HELPER'), (0x0040, 0x01E0, 'DYN'),
         (0x01E0, 0x0240, 'CRAM'), (0x0240, 0x0600, 'OUTI/OUTD'), (0x0600, 0x0730, 'LDI/LDD'), (0x0730, 0x0780, 'HCALL'),
         (0x0780, 0x0920, 'LINE'), (0x0920, 0x0AA0, 'CTL'), (0x0B80, 0x0BF8, 'MAP'),
         (0x0BF8, 0x0C48, 'VCNT'), (0x0C48, 0x0D20, 'PAIRS'), (0x0D20, 0x0DA0, 'NT'), (0x0DA0, 0x0E90, 'CLS'), (0x0E90, 0x0F70, 'TROW'), (0x0F70, 0x1010, 'OUTI/OUTD'), (0x1010, 0x1110, 'NTW')]

# sfswan: WSRUN_TYPE="frame:text|frame:text" types on the SF-7000's keyboard
# (sf_keys, the matrix: as sftest's SF_TYPE; \n is CR), WSRUN_TEXT=1 prints
# the text screen with each report (the VRAM's name table: PSRAM bank 1)
SF_TYPE = os.environ.get('WSRUN_TYPE')
SF_MATRIX = ['1QAZ\0,KI8', '2WSX .LO9', '3EDC\0/;P0', '4RFV\0\0:@-', '5TGB\0\0][^', '6YHN\0\0\r\0\\', '7UJM']
SF_SHIFTED = dict(zip('!"#$%&\'()=<>?+*`{}~|', '123456789-,./;:@[]^\\'))
sf_typing = [None, 0, 0]
def sf_type(fr):
    for item in SF_TYPE.split('|'):
        f, _, text = item.partition(':')
        if int(f) == fr: sf_typing[:] = [text.replace('\\n', '\r'), 0, fr]
    if sf_typing[0] is None: return
    t = fr - sf_typing[2]
    rows = [0] * 7
    if t % 8 < 4:
        if sf_typing[1] >= len(sf_typing[0]): sf_typing[0] = None
        else:
            c = sf_typing[0][sf_typing[1]]; shift = False
            if c.islower(): c = c.upper(); shift = True
            if c in SF_SHIFTED: c = SF_SHIFTED[c]; shift = True
            for r, row in enumerate(SF_MATRIX):
                if c in row and c != '\0': rows[r] |= 1 << row.index(c)
            if shift: rows[6] |= 1 << 11
    elif t % 8 == 7 and sf_typing[0] is not None: sf_typing[1] += 1
    uc.mem_write(syms['sf_keys'], b''.join(v.to_bytes(2, 'little') for v in rows))
def sf_text():
    reg = uc.mem_read(syms['gg'], 8)
    text = (reg[1] & 0x10) and not (reg[0] & 2)
    cols = 40 if text else 32
    nt = (reg[2] & 15) << 10
    v = bytes(psram[1])[0xC000:]
    print('   screen (%s, registers %s):' % ('text' if text else 'not text', reg.hex()))
    for r in range(24):
        line = ''.join(chr(b) if 32 <= b < 127 else ('?' if b else ' ') for b in v[(nt + r * cols) & 0x3FFF:][:cols]).rstrip()
        if line: print('   %2d|%s' % (r, line))

def report(fr):
    if os.environ.get('WSRUN_TEXT'): sf_text()
    if CLIX:
        print('   interrupts off (cycles: longest, mean, times): ' + ', '.join('%s %d/%d/%d' % (n, cli_max[n], cli_sum[n] // cli_n[n], cli_n[n]) for n, _ in cli_max.most_common(8)))
        cli_n.clear(); cli_sum.clear(); cli_max.clear()
    tot = sum(v for k, v in ph_c.items() if k != 8) or 1
    print('frames %d-%d: %s  (busy %.0f k cycles a frame)' % (fr - EVERY + 1, fr, ' '.join('%s%02d' % (PH[k], 100 * v // tot) for k, v in sorted(ph_c.items()) if k != 8), tot / EVERY / 1000))
    print('   translated %d: %s' % (sum(tr_pcs.values()), ' '.join('%04X%s' % (p, '' if n == 1 else 'x%d' % n) for p, n in tr_pcs.most_common(12))))
    print('   ' + ', '.join('%s %d%%' % (n, 100 * c // tot) for n, c in fn_c.most_common(10)))
    if FOCUS:
        g = collections.Counter()
        for a, c in focus_c.items(): g[a & ~15] += c
        print('   %s by 16 bytes (0.1%%): %s' % (FOCUS, ' '.join('%05X:%d' % (a, 1000 * c // tot) for a, c in g.most_common(20))))
        focus_c.clear()
    print('   calls a frame: ' + ', '.join('%s %.1f' % (n, c / EVERY) for n, c in counts.most_common()))
    counts.clear()
    print('   DI: ' + ', '.join('%s %.1f%%' % (n, 100 * c / tot) for n, c in di_c.most_common(10)))
    di_c.clear()
    print('   CD by block: ' + ', '.join('%s %.1f%%' % (k if isinstance(k, str) else '%04X' % k, 100 * c / tot) for k, c in blk_c.most_common(int(os.environ.get('WSRUN_TOP', '14')))))
    blk_c.clear()
    if MNEM:
        tm = sum(mn_c.values()) or 1
        print('   block code by instruction (%% of block code):')
        for k, v in mn_c.most_common(40): print('      %5.1f%%  %s' % (100 * v / tm, k))
        mn_c.clear()
    DB = os.environ.get('WSRUN_DUMPBLK')
    if DB:
        # WSRUN_DUMPBLK=pc,pc...: the x86 code of those blocks, with counts
        for want in DB.split(','):
            for j, st in enumerate(blk_starts):
                if blk_info[j][1] == int(want, 16):
                    code = bytes(uc.mem_read(st, blk_info[j][0] - st))
                    print('   block %s at %05X, %d bytes' % (want, st, len(code)))
                    for i in md.disasm(code, st):
                        print('      %05X %7d  %s %s' % (i.address, dump_n[i.address], i.mnemonic, i.op_str))
        dump_n.clear()
    if 'drc_stat_op' in syms:
        import struct
        ops = struct.unpack('<16H', bytes(uc.mem_read(syms['drc_stat_op'], 32)))
        lns = struct.unpack('<16H', bytes(uc.mem_read(syms['drc_stat_op_lines'], 32)))
        prev = OPSPREV.get('ops', (0,) * 16), OPSPREV.get('lns', (0,) * 16)
        names = ['?', 'WRITE', 'OUT', 'IN', 'STEP', 'OUTI', 'OUTD', 'MAP', 'SYNC', 'PSG']
        print('   helpers a frame: ' + ', '.join('%s %.1f (%.1f lines)' % (names[k] if k < len(names) else k,
              ((ops[k] - prev[0][k]) & 0xFFFF) / EVERY, ((lns[k] - prev[1][k]) & 0xFFFF) / EVERY)
              for k in range(16) if (ops[k] - prev[0][k]) & 0xFFFF))
        OPSPREV['ops'], OPSPREV['lns'] = ops, lns
    if DYNX:
        tot = sum(dyn_c.values()) or 1
        print('   DYN a frame: %.0f; same target as last time at that exit %d%%, other %d%%, first %d%%; exits %d, top 10 take %d%%' % (tot / EVERY,
            100 * dyn_c['same'] // tot, 100 * dyn_c['other'] // tot, 100 * dyn_c['first'] // tot, len(dyn_sites),
            100 * sum(v for _, v in dyn_sites.most_common(10)) // max(1, sum(dyn_sites.values()))))
        dyn_c.clear(); dyn_sites.clear()
    if LINKX:
        tot = sum(link_c.values()) or 1
        print('   link jumps a frame: ' + ', '.join('%s/%s %.0f (%d%%)' % (k[0], k[1], v / EVERY, 100 * v // tot) for k, v in link_c.most_common()))
        print('   link sites run: %d, top 20 take %d%%' % (len(link_sites), 100 * sum(v for _, v in link_sites.most_common(20)) // max(1, sum(link_sites.values()))))
        link_c.clear(); link_sites.clear()
    if STUBX:
        nfr = max(1, EVERY)
        for lo, hi, nm in STUBS:
            if lo < 0x40 or not any(stub_n[a] for a in range(lo, hi)): continue
            tot = sum(stub_n[a] for a in range(lo, hi))
            print('   stub %s: %.0f instr a frame' % (nm, tot / nfr))
            if STUBX == 'all' or STUBX == nm:
                code = bytes(uc.mem_read(0x40000 + lo, hi - lo))
                for i in md.disasm(code, lo):
                    if stub_n[i.address]: print('      %04X %9.1f  %s %s' % (i.address, stub_n[i.address] / nfr, i.mnemonic, i.op_str))
        stub_n.clear()
    if long_max:
        print('   instructions over 400 cycles (longest, times): ' + ', '.join('%s %d/%d' % (n, m, long_n[n]) for n, m in long_max.most_common(8)))
    if LONGX and long_hist:
        print('   long REPs (routine, cycles, frames from: times): ' + ', '.join('%s %d+ @%d: %d' % (k[0], k[1], k[2], n) for k, n in sorted(long_hist.items())))
        long_hist.clear()
    long_n.clear(); long_max.clear()
    ph_c.clear(); fn_c.clear(); tr_pcs.clear()
    sys.stdout.flush()

# a HLT: an interrupt would come (vertical blank: frame credit; the end of
# the WS screen: the frame shown)
FC, FW, VB = syms['frame_credit'], syms['frame_waiting'], syms['vblanks']
DUMP = os.environ.get('WSRUN_DUMP'); dumped = set()
cs, ip = syms['_start'] >> 4, syms['_start'] & 15   # (crt0: stack 0000:4000, data)
# WSRUN_STACK=hex address (the end of the C data, from the ELF's symbols):
# how far down the stack went, at the end (the memory between is filled
# with A5h before the start)
STACK = int(os.environ['WSRUN_STACK'], 16) if os.environ.get('WSRUN_STACK') else 0
if STACK: uc.mem_write(STACK, b'\xA5' * (0x3E00 - STACK))
uc.mem_write(0x3EFC, b'\0\0\0\0')
uc.reg_write(UC_X86_REG_SS, 0); uc.reg_write(UC_X86_REG_SP, 0x3EFC)
uc.reg_write(UC_X86_REG_DS, 0); uc.reg_write(UC_X86_REG_ES, 0)
uc.reg_write(UC_X86_REG_CS, cs)
start = (cs << 4) + ip
while st['frame'] < FRAMES:
    try:
        uc.emu_start(start, 0xFFFFFFFF)
    except UcError as e:
        cs = uc.reg_read(UC_X86_REG_CS); ip = uc.reg_read(UC_X86_REG_IP)
        raise SystemExit('%s at %04X:%04X (%s) frame %d; trail %s' % (e, cs, ip, fname((cs << 4) + ip), st['frame'], ' '.join('%s@%05X' % (n, a) for n, a in trail)))
    if st['frame'] >= FRAMES: break
    cs = uc.reg_read(UC_X86_REG_CS); ip = uc.reg_read(UC_X86_REG_IP)
    a = (cs << 4) + ip
    if uc.mem_read(a - 1, 1)[0] == 0xF4:   # HLT
        if DUMP:
            # WSRUN_DUMP=dir: the Game Gear RAM (8000h) at every 50th
            # Game Gear frame (gg.frame), to compare two builds
            gf = int.from_bytes(uc.mem_read(syms['gg'] + 0x6C, 4), 'little')
            if gf % 50 == 0 and gf not in dumped:
                dumped.add(gf)
                open('%s/ram%05d.bin' % (DUMP, gf), 'wb').write(bytes(uc.mem_read(0x8000, 0x2000)))
                # the Game Gear VRAM as the game sees it: the WS tiles with the
                # deferred writes (pending log, then the one filling) applied
                vr = bytearray(uc.mem_read(0x4000, 0x4000))
                r16 = lambda a: int.from_bytes(uc.mem_read(a, 2), 'little')
                def apply(a, e):
                    while a < e:
                        ea = r16(a); n = uc.mem_read(a + 2, 1)[0]
                        for k in range(n): vr[(ea + k) & 0x3FFF] = uc.mem_read(a + 3 + k, 1)[0]
                        a += 3 + n
                ps = r16(syms['pend_start'])
                if ps: apply(ps, r16(syms['pend_top']))
                apply(r16(syms['log_start']), r16(syms['drc_st'] + 304))
                open('%s/vram%05d.bin' % (DUMP, gf), 'wb').write(bytes(vr))
        if SHOT:
            # WSRUN_SHOT=prefix: the internal RAM and the ports at every
            # EVERY-th frame, for wsshot.py (the WS screen they make)
            gf = st['frame']
            if gf and gf % EVERY == 0 and gf not in shot:
                shot.add(gf)
                open('%s_%05d.wsd' % (SHOT, gf), 'wb').write(bytes(uc.mem_read(0, 0x10000)) + bytes(ports))
        uc.mem_write(FC, (7547 * 2).to_bytes(2, 'little'))
        uc.mem_write(FW, b'\0')
        if 'pal_wait' in syms and uc.mem_read(syms['pal_wait'], 1)[0]:
            # (as the line interrupt: the colors of the frame presented)
            pp = bytes(uc.mem_read(syms['pal_pend'], 64))
            uc.mem_write(0xFE00, pp)
            uc.mem_write(0xFF00, pp[32:])
            uc.mem_write(syms['pal_wait'], b'\0')
        if CLIX: uc.mem_write(syms['pend_start'], b'\0\0')   # (as if the line interrupt had written the log: not in VRAM here)
        uc.mem_write(VB, bytes([(uc.mem_read(VB, 1)[0] + 1) & 255]))
        st['cyc'] += 256 * 8
        start = a
        continue
    raise SystemExit('stopped at %04X:%04X (%s)' % (cs, ip, fname(a)))
import zlib as _z
print('Z80: pc %04X  work RAM %08X  video RAM %08X  bank 0 from C000h %08X  bank 0 %08X  state file %s' % (int.from_bytes(uc.mem_read(PCSYM, 2), 'little'), _z.crc32(bytes(uc.mem_read(0x8000, 0x2000))), _z.crc32(bytes(uc.mem_read(0x4000, 0x4000))), _z.crc32(bytes(psram[0].raw[0xC000:])), _z.crc32(bytes(psram[0].raw)), len(state_file[0]) if state_file[0] is not None else 'none'))
if STATE_F and state_file[0] is not None: open(STATE_F, 'wb').write(bytes(state_file[0]))
if STACK:
    m = bytes(uc.mem_read(STACK, 0x3E00 - STACK))
    low = next((i for i, v in enumerate(m) if v != 0xA5), len(m))
    print('stack: down to %04X (%d bytes used of the %d between %04X and 4000h)' % (STACK + low, 0x4000 - STACK - low, 0x4000 - STACK, STACK))
