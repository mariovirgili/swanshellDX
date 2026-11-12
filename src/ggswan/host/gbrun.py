#!/usr/bin/env python3
"""Runs the WS build of gbswan (build/gbswan.elf) in Unicorn from its entry
point, with a Game Boy ROM: the SD card calls (f_mount, f_open, f_read,
f_close) answered here, the cartridge's windows on PSRAM and SRAM banks,
the WonderSwan's line counter with its vertical blank and line interrupts,
and the WonderSwan Color's picture in the 2 bits a pixel color mode drawn
line by line with the registers each line had.
usage: gbrun.py build/gbswan.elf rom.gb frames [out-prefix] [keys "frame:hex,..."]
  frames: WonderSwan frames (75.47 a second) to run
  out-prefix: PREFIX-NNNN.ppm of the frames in GBRUN_SHOTS (default: the last)
  keys: what ws_keypad_scan returns (Y1-Y4 001 002 004 008, X1-X4 010 020
        040 080, START 200, A 400, B 800), held from that frame on
Environment: GBRUN_TEXT=1 (the Game Boy's background map as text at the
end)  GBRUN_SHOTS=n,n,...  GBRUN_LINE=instructions a line (default
80: about 256 cycles; only how fast the plugin seems depends on it)
GBRUN_CYCLES=1: count V30MZ cycles (Mednafen's costs, as wsrun.py) in
place of instructions: slow, for measuring.
Prints, at the end and every 75 frames: Game Boy frames shown, and
instructions (or cycles) run a Game Boy frame; with GBRUN_CYCLES, where
the cycles went."""
import sys, os, ctypes
from elftools.elf.elffile import ELFFile
from unicorn import *
from unicorn.x86_const import *

ELF, ROM, FRAMES = sys.argv[1], sys.argv[2], int(sys.argv[3])
OUT = sys.argv[4] if len(sys.argv) > 4 else None
KEYS = dict((int(k.split(':')[0]), int(k.split(':')[1], 16)) for k in sys.argv[5].split(',')) if len(sys.argv) > 5 and sys.argv[5] else {}
SHOTS = set(int(x) for x in os.environ.get('GBRUN_SHOTS', str(FRAMES - 1)).split(',') if x)
LINE_INSNS = int(os.environ.get('GBRUN_LINE', '80'))
CYCLES = bool(os.environ.get('GBRUN_CYCLES'))
UNIT = 'cycles' if CYCLES else 'instructions'
PROF_FROM = int(os.environ.get('GBRUN_PROF_FROM', '0'))    # (the cycles counted by function from this frame on)
prof_gb0 = [0]

f = ELFFile(open(ELF, 'rb'))
uc = Uc(UC_ARCH_X86, UC_MODE_16)
psram = [ctypes.create_string_buffer(0x10000) for _ in range(32)]
sram = [ctypes.create_string_buffer(0x10000) for _ in range(8)]
iram = ctypes.create_string_buffer(0x10000)
uc.mem_map_ptr(0, 0x10000, 7, ctypes.addressof(iram))
uc.mem_map_ptr(0x100000, 0x10000, 7, ctypes.addressof(iram))
for seg in range(4, 16):
    uc.mem_map_ptr(seg << 16, 0x10000, 7, ctypes.addressof(psram[(0x10 | seg) & 31]))
for s in f.iter_sections():
    if s['sh_flags'] & 2 and s['sh_size'] and s['sh_type'] == 'SHT_PROGBITS':
        uc.mem_write(s['sh_addr'] & 0xFFFFF, s.data())
syms = {}
for y in f.get_section_by_name('.symtab').iter_symbols():
    syms.setdefault(y.name, y['st_value'] & 0xFFFFF)

rom = open(ROM, 'rb').read()
path = b'/GAME.GB\0'
for b in sram: ctypes.memmove(ctypes.addressof(b) + 0xE0, path, len(path))
ports = bytearray(256)
ports[0xA0] = 0x86
win = {}
bank = {'ram': 0, 'rom0': 0, 'rom1': 0}
def set_win(base, buf):
    if win.get(base) is buf: return
    if base in win: uc.mem_unmap(base, 0x10000)
    uc.mem_map_ptr(base, 0x10000, 7, ctypes.addressof(buf)); win[base] = buf
def remap():
    set_win(0x10000, psram[bank['ram'] & 31] if ports[0xCE] & 1 else sram[bank['ram'] & 7])
    set_win(0x20000, psram[bank['rom0'] & 31])
    set_win(0x30000, psram[bank['rom1'] & 31])
remap()

st = {'line': 0, 'frame': 0, 'keys': 0, 'gbframes': 0, 'work': 0, 'pend': 0, 'fail': None, 'halted': False}
BANKPORT = {0xC1: 'ram', 0xC2: 'rom0', 0xC3: 'rom1', 0xD0: 'ram', 0xD2: 'rom0', 0xD4: 'rom1'}
def h_in(uc, port, size, _):
    port &= 0xFF
    if port == 0x02: return st['line']
    if port == 0xB5:
        k = st['keys']; g = ports[0xB5]
        return (g & 0xF0) | (k & 15 if g & 0x10 else 0) | ((k >> 4) & 15 if g & 0x20 else 0) | ((k >> 8) & 15 if g & 0x40 else 0)
    if port == 0xB4: return st['pend']
    if port == 0xE4: return 0xFFFF
    v = ports[port]
    return v | ports[(port + 1) & 0xFF] << 8 if size == 2 else v
def h_out(uc, port, size, value, _):
    for i in range(size):
        p = (port + i) & 255; v = (value >> (8 * i)) & 255
        if p == 0xB6:
            st['pend'] &= ~v; continue
        ports[p] = v
        if p in (0xC1, 0xC2, 0xC3, 0xD0, 0xD2, 0xD4):
            bank[BANKPORT[p]] = (bank[BANKPORT[p]] & 0xFF00) | v
        elif p in (0xD1, 0xD3, 0xD5):
            k = BANKPORT[p - 1]; bank[k] = (bank[k] & 0xFF) | v << 8
    if 0xC0 <= (port & 0xFF) <= 0xD5: remap()
uc.hook_add(UC_HOOK_INSN, h_in, None, 1, 0, UC_X86_INS_IN)
uc.hook_add(UC_HOOK_INSN, h_out, None, 1, 0, UC_X86_INS_OUT)

def retf(pop=0):
    sp = uc.reg_read(UC_X86_REG_SP)
    ip, cs = int.from_bytes(uc.mem_read(sp, 2), 'little'), int.from_bytes(uc.mem_read(sp + 2, 2), 'little')
    uc.reg_write(UC_X86_REG_SP, sp + 4 + pop)
    uc.reg_write(UC_X86_REG_CS, cs); uc.reg_write(UC_X86_REG_IP, ip)
rom_pos = [0]
def h_sd(uc, addr, size, name):
    ax = uc.reg_read(UC_X86_REG_AX)
    if name == 'f_open':
        uc.mem_write(ax + 0xA, len(rom).to_bytes(4, 'little')); rom_pos[0] = 0
    elif name == 'f_read':
        buf = uc.reg_read(UC_X86_REG_DX)
        sp = uc.reg_read(UC_X86_REG_SP)
        n = int.from_bytes(uc.mem_read(sp + 4, 2), 'little')
        brp = int.from_bytes(uc.mem_read(sp + 6, 2), 'little')
        d = rom[rom_pos[0]:rom_pos[0] + n]; rom_pos[0] += len(d)
        uc.mem_write(buf, d); uc.mem_write(brp, len(d).to_bytes(2, 'little'))
        uc.reg_write(UC_X86_REG_AX, 0); retf(4); return
    elif name == 'fail':
        dx, cx = uc.reg_read(UC_X86_REG_DX), uc.reg_read(UC_X86_REG_CX)
        st['fail'] = '%s %04X' % (bytes(uc.mem_read((dx << 4) + ax, 24)).split(b'\0')[0].decode('latin1'), cx)
        uc.emu_stop(); return
    elif name == 'gbv_frame':
        st['gbframes'] += 1; return
    uc.reg_write(UC_X86_REG_AX, 0); retf()
for n in ('f_mount', 'f_open', 'f_read', 'f_close', 'fail', 'gbv_frame'):
    if n in syms: uc.hook_add(UC_HOOK_CODE, h_sd, n, syms[n], syms[n])

cyc = [0, 0]      # V30MZ cycles run; where the line being run ends
prof = {}
if CYCLES:
    # Mednafen's V30MZ costs: wsrun.py's table (taken from its source)
    import bisect, collections
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'wsrun.py')).read()
    exec(src[src.index('STR_CYC = '):src.index('def cost(code):')], globals())
    funcs = []
    for y in f.get_section_by_name('.symtab').iter_symbols():
        if y['st_info']['type'] == 'STT_FUNC' and y['st_size']:
            a0 = y['st_value'] & 0xFFFFF
            funcs.append((a0, a0 + y['st_size'], y.name))
    funcs.sort(); starts = [x[0] for x in funcs]
    stubs_n = syms['drc_stubs_end'] - syms['drc_stubs'] if 'drc_stubs' in syms else 0
    def where(addr):
        if 0x50000 <= addr < 0xD0000:
            return 'translated code' if (addr & 0xFFFF) >= stubs_n else 'the routines it calls'
        i = bisect.bisect_right(starts, addr) - 1
        return funcs[i][2] if i >= 0 and funcs[i][0] <= addr < funcs[i][1] else '?%05X' % (addr & 0xFFF00)
    prof = collections.Counter()
    jst = [0, 0, 0]
    ccache = {}
    def h_cyc(uc, addr, size, _):
        code = bytes(uc.mem_read(addr, size))
        k = ccache.get(code)
        if k is None or code[0] in (0xF2, 0xF3):
            k = cost_m(code, uc.reg_read(UC_X86_REG_CX), jst[2] != addr)
            if code[0] not in (0xF2, 0xF3): ccache[code] = k
        c, ex = k
        if jst[0] and addr != jst[1]: c += jst[0]
        jst[0] = ex; jst[1] = addr + size; jst[2] = addr
        cyc[0] += c
        prof[where(addr)] += c
        if cyc[0] >= cyc[1]: uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, h_cyc)

def irq(n):
    """the interrupt, if they are enabled; True if taken"""
    fl = uc.reg_read(UC_X86_REG_EFLAGS)
    if not fl & 0x200: return False
    vec = ((ports[0xB0] & 0xF8) + n) * 4
    ip, cs = int.from_bytes(uc.mem_read(vec, 2), 'little'), int.from_bytes(uc.mem_read(vec + 2, 2), 'little')
    sp = (uc.reg_read(UC_X86_REG_SP) - 6) & 0xFFFF
    ss = uc.reg_read(UC_X86_REG_SS) << 4
    uc.mem_write(ss + sp, uc.reg_read(UC_X86_REG_IP).to_bytes(2, 'little') + uc.reg_read(UC_X86_REG_CS).to_bytes(2, 'little') + (fl & 0xFFFF).to_bytes(2, 'little'))
    uc.reg_write(UC_X86_REG_SP, sp)
    uc.reg_write(UC_X86_REG_EFLAGS, fl & ~0x300)
    uc.reg_write(UC_X86_REG_CS, cs); uc.reg_write(UC_X86_REG_IP, ip)
    st['halted'] = False
    return True

def pal_of(mem):
    return [int.from_bytes(mem[0xFE00 + i * 2:0xFE02 + i * 2], 'little') for i in range(256)]
def render_line(mem, p, y, pal):
    """224 pixels of line y from the internal RAM and the ports: the color
    mode with 2 bits a pixel (60h = 80h)"""
    def tile_px(t, r, c):
        a = 0x2000 + t * 16 + r * 2
        b = 7 - c
        return (mem[a] >> b & 1) | (mem[a + 1] >> b & 1) << 1
    def screen(scr, x):
        base = ((p[7] >> 4) if scr else (p[7] & 15)) << 11
        sx = (x + p[0x12 if scr else 0x10]) & 255
        sy = (y + p[0x13 if scr else 0x11]) & 255
        a = base + ((sy >> 3) * 32 + (sx >> 3)) * 2
        e = mem[a] | mem[a + 1] << 8
        t = (e & 0x1FF) | ((e >> 13) & 1) << 9
        r, c = sy & 7, sx & 7
        if e & 0x8000: r = 7 - r
        if e & 0x4000: c = 7 - c
        px = tile_px(t, r, c)
        pl = (e >> 9) & 15
        # color 0: seen in palettes 0-3 and 8-11, clear in the others
        return pal[pl * 16 + px] if px or not (pl & 4) else None
    ctrl = p[0]
    tab = (p[4] & 0x3F) << 9
    spr = []
    if ctrl & 4:
        for i in range(p[5], min(p[5] + p[6], 128)):
            s = mem[tab + i * 4:tab + i * 4 + 4]
            if (y - s[2]) & 255 < 8: spr.append(s)
        spr = spr[:32]
    out = bytearray()
    for x in range(224):
        c = pal[p[1]]
        if ctrl & 1:
            v = screen(0, x)
            if v is not None: c = v
        lo = hi = None
        for s in reversed(spr):         # (lower index on top)
            dx = (x - s[3]) & 255
            if dx >= 8: continue
            if ctrl & 8:
                ins = p[0xC] <= x <= p[0xE] and p[0xD] <= y <= p[0xF]
                if ins == bool(s[1] & 0x10): continue
            dy = (y - s[2]) & 255
            if s[1] & 0x80: dy = 7 - dy
            if s[1] & 0x40: dx = 7 - dx
            px = tile_px(s[0] | (s[1] & 1) << 8, dy, dx)
            pl = 8 + ((s[1] >> 1) & 7)
            if not px and pl & 4: continue
            cc = pal[pl * 16 + px]
            if s[1] & 0x20: hi, lo = cc, None
            else: lo, hi = cc, None
        if lo is not None: c = lo
        if ctrl & 2:
            inside = p[8] <= x <= p[0xA] and p[9] <= y <= p[0xB]
            if not (ctrl & 0x20) or (inside != bool(ctrl & 0x10)):
                v = screen(1, x)
                if v is not None: c = v
        if hi is not None: c = hi
        out += bytes((((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17))
    return bytes(out)

if os.environ.get('GBRUN_WATCH'):
    # GBRUN_WATCH=addr: every write to that byte of the internal RAM, with the
    # Game Boy's PC as the plugin has it (gb.pc: the last block's exit) and A
    wa = int(os.environ['GBRUN_WATCH'], 16)
    def h_w(uc, access, addr, size, value, _):
        g = syms['gb']
        m = uc.mem_read(g, 12)
        print('W %04X=%02X pc=%04X gbframe=%d a=%02X' % (addr, value & 0xFF, m[10] | m[11] << 8, st['gbframes'], uc.reg_read(UC_X86_REG_AX) & 0xFF))
    uc.hook_add(UC_HOOK_MEM_WRITE, h_w, None, wa, wa)
callers = None
if os.environ.get('GBRUN_CALLERS') and CYCLES:
    # GBRUN_CALLERS=function: who calls it (far calls), and its first argument (AX)
    import collections as _c
    callers = _c.Counter()
    def h_call(uc, addr, size, _):
        if st['frame'] < PROF_FROM: return
        sp = uc.reg_read(UC_X86_REG_SP)
        ip, cs = int.from_bytes(uc.mem_read(sp, 2), 'little'), int.from_bytes(uc.mem_read(sp + 2, 2), 'little')
        if addr == syms.get('gbd_enter'):
            # (the dispatcher entering a block: the Game Boy's PC)
            g = syms['gb']; m = uc.mem_read(g, 12)
            callers['enter', '%04X' % (m[10] | m[11] << 8)] += 1
        else:
            callers[where((cs << 4) + ip), '%04X' % (uc.reg_read(UC_X86_REG_AX) & 0xFFF0)] += 1
    a0 = syms[os.environ['GBRUN_CALLERS']]
    uc.hook_add(UC_HOOK_CODE, h_call, None, a0, a0)
e = f['e_entry'] & 0xFFFFF
uc.reg_write(UC_X86_REG_CS, 0xF000); uc.reg_write(UC_X86_REG_IP, e - 0xF0000)
uc.reg_write(UC_X86_REG_SS, 0); uc.reg_write(UC_X86_REG_SP, 0x4000)
line_ports = [None] * 144
work_frame = 0; last_gb = 0; last_work = 0
while st['frame'] < FRAMES and not st['fail']:
    # one line
    if not st['halted']:
        cs = uc.reg_read(UC_X86_REG_CS); ip = uc.reg_read(UC_X86_REG_IP)
        try:
            if CYCLES:
                cyc[1] += 256
                if cyc[0] < cyc[1]: uc.emu_start((cs << 4) + ip, 0xFFFFFFFF)
            else:
                uc.emu_start((cs << 4) + ip, 0xFFFFFFFF, count=LINE_INSNS)
        except UcError as x:
            cs = uc.reg_read(UC_X86_REG_CS); ip = uc.reg_read(UC_X86_REG_IP)
            print('stopped: %s at %04X:%04X' % (x, cs, ip)); break
        st['work'] = cyc[0] if CYCLES else st['work'] + LINE_INSNS
        cs = uc.reg_read(UC_X86_REG_CS); ip = uc.reg_read(UC_X86_REG_IP)
        if uc.mem_read((cs << 4) + ip - 1, 1)[0] == 0xF4 and uc.mem_read((cs << 4) + ip, 1)[0] != 0xF4:
            # (Unicorn stops after a HLT: wait for an interrupt)
            st['halted'] = True
    y = st['line']
    if y < 144 and st['frame'] in SHOTS: line_ports[y] = bytes(ports)
    st['line'] = (y + 1) % 159
    y = st['line']
    if y == 144:
        if st['frame'] in SHOTS and OUT and line_ports[0] is not None:
            mem = bytes(iram.raw); pal = pal_of(mem)
            assert line_ports[0][0x60] & 0xE0 == 0x80, 'not the 2 bpp color mode'
            img = b''.join(render_line(mem, line_ports[i], i, pal) for i in range(144))
            open('%s-%04d.ppm' % (OUT, st['frame']), 'wb').write(b'P6\n224 144\n255\n' + img)
        st['pend'] |= 0x40
        st['frame'] += 1
        if st['frame'] in KEYS: st['keys'] = KEYS[st['frame']]
        if CYCLES and st['frame'] == PROF_FROM:
            prof.clear(); prof_gb0[0] = st['gbframes']
        if st['frame'] % 75 == 0:
            g = st['gbframes'] - last_gb
            print('frame %d: %d Game Boy frames (%.1f a second), %d %s each' % (st['frame'], st['gbframes'], g * 75.47 / 75, (st['work'] - last_work) // max(g, 1), UNIT))
            last_gb = st['gbframes']; last_work = st['work']
    if y == ports[3]: st['pend'] |= 0x10
    en = st['pend'] & ports[0xB2]
    for n in (7, 6, 5, 4, 3, 2, 1, 0):
        if en >> n & 1:
            irq(n); break
if os.environ.get('GBRUN_TEXT'):
    # the Game Boy's background map as text (test ROMs with a font at its ASCII places)
    mem = bytes(iram.raw)
    base = 0x3C00 if mem[0x9F40] & 8 else 0x3800
    for r in range(18):
        t = ''.join(chr(c) if 32 <= c < 127 else ' ' for c in mem[base + r * 32:base + r * 32 + 20]).rstrip()
        if t: print(t)
if os.environ.get('GBRUN_SYMS'):
    # GBRUN_SYMS=name,name,...: the plugin's variables (16 bits) at the end
    mem = bytes(iram.raw)
    print(' '.join('%s=%d' % (n, mem[syms[n] & 0xFFFF] | mem[(syms[n] & 0xFFFF) + 1] << 8) for n in os.environ['GBRUN_SYMS'].split(',')))
if os.environ.get('GBRUN_PEEK'):
    # GBRUN_PEEK=addr,addr,...: bytes of the internal RAM at the end (hex)
    mem = bytes(iram.raw)
    print(' '.join('%s=%02X' % (a, mem[int(a, 16)]) for a in os.environ['GBRUN_PEEK'].split(',')))
if st['fail']: print('fail: ' + st['fail'])
print('WS frames %d, Game Boy frames %d' % (st['frame'], st['gbframes']))
if callers:
    for k, v in callers.most_common(12): print('  %6d  %s %s' % (v, k[0], k[1]))
if CYCLES and st['gbframes']:
    tot = sum(prof.values())
    print('V30MZ cycles a Game Boy frame, from frame %d: %d (a WonderSwan frame has 40704, a Game Boy frame\'s time 51430)' % (PROF_FROM, tot // max(1, st['gbframes'] - prof_gb0[0])))
    for k, v in prof.most_common(14): print('  %5.1f%%  %s' % (100.0 * v / tot, k))

