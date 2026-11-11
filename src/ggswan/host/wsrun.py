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
def h_in(uc, port, size, ud):
    if port == 0x02: return (st['cyc'] // 256) % 159
    if port == 0xB5: return 0
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
def sd(name):
    ax = uc.reg_read(UC_X86_REG_AX)
    if name == 'f_open':
        uc.mem_write(ax + 0xA, len(rom).to_bytes(4, 'little')); rom_pos[0] = 0
    elif name == 'f_read':
        # (fp AX, buff DX, btr and br on the stack: the callee pops them)
        buf = uc.reg_read(UC_X86_REG_DX)
        sp = uc.reg_read(UC_X86_REG_SP)
        n = int.from_bytes(uc.mem_read(sp + 4, 2), 'little')
        brp = int.from_bytes(uc.mem_read(sp + 6, 2), 'little')
        d = rom[rom_pos[0]:rom_pos[0] + n]; rom_pos[0] += len(d)
        uc.mem_write(buf, d); uc.mem_write(brp, len(d).to_bytes(2, 'little'))
        uc.reg_write(UC_X86_REG_AX, 0); retf(4); return
    uc.reg_write(UC_X86_REG_AX, 0); retf()
SD = {syms[n]: n for n in ('f_mount', 'f_open', 'f_read', 'f_close')}
FAIL, FRAME, TRANS = syms['fail'], syms['gg_run_frame'], syms['drc_translate']
KEYSYM = syms['gg'] + 0   # (gg.keys: found below)

trail = collections.deque(maxlen=40)
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
        st['frame'] += 1
        fr = st['frame']
        if fr in KEYS: pending_keys[0] = KEYS[fr]
        uc.mem_write(syms['gg'] + 0x6C, bytes([pending_keys[0]]))   # gg.keys (read_keys: none here)
        if fr % EVERY == 0: report(fr)
        if fr >= FRAMES: uc.emu_stop()
    if DYNX:
        if 0x40000 <= addr < 0xA0000 and (addr & 0xFFFF) == 0x4B:
            site = prev_addr[0]; tgt = int.from_bytes(uc.mem_read(PCSYM, 2), 'little')
            last = dyn_last.get(site)
            dyn_c['same' if last == tgt else ('first' if last is None else 'other')] += 1
            dyn_last[site] = tgt; dyn_sites[site] += 1
        prev_addr[0] = addr
    c = cost(bytes(uc.mem_read(addr, size)))
    st['cyc'] += c
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
            if STUBX and 0x40 <= o < 0xE90: stub_n[o] += 1
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
         (0x0780, 0x0920, 'LINE'), (0x0920, 0x0AA0, 'CTL'), (0x0AA0, 0x0B80, 'NTW'), (0x0B80, 0x0BF8, 'MAP'),
         (0x0BF8, 0x0C48, 'VCNT'), (0x0C48, 0x0D20, 'PAIRS'), (0x0D20, 0x0DA0, 'NT'), (0x0DA0, 0x0E90, 'CLS')]

def report(fr):
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
    ph_c.clear(); fn_c.clear(); tr_pcs.clear()
    sys.stdout.flush()

# a HLT: an interrupt would come (vertical blank: frame credit; the end of
# the WS screen: the frame shown)
FC, FW, VB = syms['frame_credit'], syms['frame_waiting'], syms['vblanks']
DUMP = os.environ.get('WSRUN_DUMP'); dumped = set()
cs, ip = syms['_start'] >> 4, syms['_start'] & 15   # (crt0: stack 0000:4000, data)
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
        uc.mem_write(FC, (7547 * 2).to_bytes(2, 'little'))
        uc.mem_write(FW, b'\0')
        if CLIX: uc.mem_write(syms['pend_start'], b'\0\0')   # (as if the line interrupt had written the log: not in VRAM here)
        uc.mem_write(VB, bytes([(uc.mem_read(VB, 1)[0] + 1) & 255]))
        st['cyc'] += 256 * 8
        start = a
        continue
    raise SystemExit('stopped at %04X:%04X (%s)' % (cs, ip, fname(a)))
