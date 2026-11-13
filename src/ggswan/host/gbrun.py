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
Environment: GBRUN_GBTIME=1 (frames, the keys' frames and GBRUN_PROF_FROM
are Game Boy frames: the same game at any speed of the plugin, for
comparing builds)  GBRUN_SOUND=file.wav (what the WonderSwan's sound registers
would play, as they are at each frame's end: squares from the wave tables,
the noise as white noise at its volume: for the notes, not the timbre)
GBRUN_TEXT=1 (the Game Boy's background map as text at the
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
import zlib
FHASH = open(os.environ['GBRUN_FHASH'], 'w') if os.environ.get('GBRUN_FHASH') else None
KEYS = dict((int(k.split(':')[0]), int(k.split(':')[1], 16)) for k in sys.argv[5].split(',')) if len(sys.argv) > 5 and sys.argv[5] else {}
SHOTS = set(int(x) for x in os.environ.get('GBRUN_SHOTS', str(FRAMES - 1)).split(',') if x)
LINE_INSNS = int(os.environ.get('GBRUN_LINE', '80'))
CYCLES = bool(os.environ.get('GBRUN_CYCLES'))
UNIT = 'cycles' if CYCLES else 'instructions'
GBTIME = bool(os.environ.get('GBRUN_GBTIME'))   # frames, keys, GBRUN_PROF_FROM count Game Boy frames
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

# where the plugin's HLT instructions are (Unicorn stops after one, and
# stops elsewhere too: a byte F4h before the stop is not enough to tell)
import capstone
_md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
HLTS = set()
for y in f.get_section_by_name('.symtab').iter_symbols():
    if y['st_info']['type'] == 'STT_FUNC' and y['st_size']:
        a0 = y['st_value'] & 0xFFFFF
        for ins in _md.disasm(bytes(uc.mem_read(a0, y['st_size'])), a0):
            if ins.mnemonic == 'hlt': HLTS.add(ins.address)
if 'gb_run' in syms:
    for ins in _md.disasm(bytes(uc.mem_read(syms['gb_run'], 16)), syms['gb_run']):
        if ins.mnemonic == 'hlt': HLTS.add(ins.address)
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
    if port == 0x02:
        # (the line counter is read to wait for a time: not counting cycles, a line ends here, or the wait would be all that is run)
        if not CYCLES: uc.emu_stop()
        return st['line']
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
        # (stopped here for an interrupt and started again, this hook is
        # called twice for the one call: count it once, until the
        # function's second instruction has been reached)
        if not st.get('gbv_armed', True): return
        st['gbv_armed'] = False
        st['gbframes'] += 1
        if os.environ.get('GBRUN_SCROLL'): print('G %d scx %d ws %d line %d' % (st['gbframes'], iram.raw[0x9F43], st['frame'], st['line']))
        if FHASH:
            # GBRUN_FHASH=file: the Game Boy's memory at each of its frames' ends (to find where two runs part)
            m = bytes(iram.raw)
            FHASH.write('%d %08X %08X %08X %08X\n' % (st['gbframes'], zlib.crc32(m[0x2000:0x4000]), zlib.crc32(m[0x6000:0x8000]), zlib.crc32(m[0x9E00:0x9EA0]), zlib.crc32(m[0x9F80:0x9FFF])))
        if os.environ.get('GBRUN_DUMPAT') and st['gbframes'] == int(os.environ['GBRUN_DUMPAT'].split(':')[0]):
            # GBRUN_DUMPAT=frame:file: the internal RAM at that Game Boy frame's end
            open(os.environ['GBRUN_DUMPAT'].split(':')[1], 'wb').write(bytes(iram.raw))
        if GBTIME:
            # (frames, keys and the profile's start by Game Boy frames: the
            # same game whatever the plugin's speed)
            g = st['gbframes']
            if g in KEYS: st['keys'] = KEYS[g]
            if CYCLES and g == PROF_FROM:
                prof.clear(); hot.clear(); fhot.clear(); fcnt.clear(); prof_gb0[0] = g
                if callers is not None: callers.clear()
            if g >= FRAMES: st['stop'] = True; uc.emu_stop()
        return
    uc.reg_write(UC_X86_REG_AX, 0); retf()
for n in ('f_mount', 'f_open', 'f_read', 'f_close', 'fail', 'gbv_frame'):
    if n in syms: uc.hook_add(UC_HOOK_CODE, h_sd, n, syms[n], syms[n])
def h_arm(uc, addr, size, _): st['gbv_armed'] = True
if 'gbv_frame' in syms:
    _a = syms['gbv_frame'] + next(_md.disasm(bytes(uc.mem_read(syms['gbv_frame'], 8)), 0)).size
    uc.hook_add(UC_HOOK_CODE, h_arm, None, _a, _a)

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
    # (the routines copied to each bank of code: by their labels in gb_drc_asm.s)
    sl = sorted((v - syms['drc_stubs'], k) for k, v in syms.items() if k.startswith('s_') and syms['drc_stubs'] <= v < syms['drc_stubs_end']) if 'drc_stubs' in syms else []
    stub_at = [0] + [x[0] for x in sl]; stub_names = ['table'] + [x[1] for x in sl]
    labs = sorted((v, k) for k, v in syms.items() if v >= 0xF0000 and not k.endswith(('!', '&')) and not k.startswith(('.', '$')))
    lab_at = [x[0] for x in labs]; lab_names = [x[1] for x in labs]
    def where(addr):
        if 0x50000 <= addr < 0xD0000:
            if (addr & 0xFFFF) >= stubs_n: return 'translated code'
            i = bisect.bisect_right(stub_at, addr & 0xFFFF) - 1
            return 'routine ' + stub_names[i]
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and funcs[i][0] <= addr < funcs[i][1]: return funcs[i][2]
        # (assembly: labels with no size; the nearest one before)
        j = bisect.bisect_right(lab_at, addr) - 1
        return lab_names[j] if j >= 0 and addr - lab_at[j] < 0x200 else '?%05X' % (addr & 0xFFF00)
    prof = collections.Counter()
    HOT = bool(os.environ.get('GBRUN_HOT'))     # GBRUN_HOT=1: the blocks most cycles are spent in
    hot = collections.Counter()
    FUNC = os.environ.get('GBRUN_FUNC')     # GBRUN_FUNC=name: that function's instructions by cycles
    fhot = collections.Counter(); fcnt = collections.Counter()
    jst = [0, 0, 0]
    ccache = {}
    def h_cyc(uc, addr, size, _):
        # (Unicorn drops what a hook raises: say it)
        try: h_cyc2(uc, addr, size)
        except Exception:
            import traceback; traceback.print_exc(); os._exit(1)
    def h_cyc2(uc, addr, size):
        code = bytes(uc.mem_read(addr, size))
        k = ccache.get(code)
        if k is None or code[0] in (0xF2, 0xF3):
            k = cost_m(code, uc.reg_read(UC_X86_REG_CX), jst[2] != addr)
            if code[0] not in (0xF2, 0xF3): ccache[code] = k
        c, ex = k
        if jst[0] and addr != jst[1]: c += jst[0]
        jst[0] = ex; jst[1] = addr + size; jst[2] = addr
        cyc[0] += c
        w = where(addr)
        prof[w] += c
        if w == 'translated code' and HOT: hot[addr] += c
        if w == FUNC: fhot[addr] += c; fcnt[addr] += 1
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
    was = [int(x, 16) for x in os.environ['GBRUN_WATCH'].split(',')]
    def h_w(uc, access, addr, size, value, _):
        g = syms['gb']
        m = uc.mem_read(g, 12)
        if st['gbframes'] < int(os.environ.get('GBRUN_WATCH_FROM', '0')): return
        ev = int.from_bytes(uc.mem_read(syms['gbf_ev_pos'] & 0xFFFF, 2), 'little'); cy = int.from_bytes(uc.mem_read(syms['gbf_cyc'] & 0xFFFF, 2), 'little', signed=True)
        print('W %04X=%02X pc=%04X gbframe=%d a=%02X pos=%d line=%d cyc=%d' % (addr, value & 0xFF, m[10] | m[11] << 8, st['gbframes'], uc.reg_read(UC_X86_REG_AX) & 0xFF, (ev - cy) % 17556, ((ev - cy) % 17556) // 114, cy))
    for wa in was: uc.hook_add(UC_HOOK_MEM_WRITE, h_w, None, wa, wa)
vlines = None
if os.environ.get('GBRUN_VRAMLINES'):
    # GBRUN_VRAMLINES=frame: from that Game Boy frame on, the writes to the
    # patterns (8000h-97FFh) by the Game Boy line they are made at: which
    # tiles a program rewrites while the picture is being drawn
    import collections as _c2
    vlines = _c2.defaultdict(_c2.Counter)
    def h_vw(uc, access, addr, size, value, _):
        if st['gbframes'] < int(os.environ['GBRUN_VRAMLINES']): return
        ev = int.from_bytes(uc.mem_read(syms['gbf_ev_pos'] & 0xFFFF, 2), 'little'); cy = int.from_bytes(uc.mem_read(syms['gbf_cyc'] & 0xFFFF, 2), 'little', signed=True)
        vlines[(addr - 0x2000) >> 4][((ev - cy) % 17556) // 114] += 1
    uc.hook_add(UC_HOOK_MEM_WRITE, h_vw, None, 0x2000, 0x37FF)
callers = None
if os.environ.get('GBRUN_CALLERS') and CYCLES:
    # GBRUN_CALLERS=function: who calls it (far calls), and its first argument (AX)
    import collections as _c
    callers = _c.Counter()
    def h_call(uc, addr, size, _):
        if (st['gbframes'] if GBTIME else st['frame']) < PROF_FROM: return
        sp = uc.reg_read(UC_X86_REG_SP)
        ip, cs = int.from_bytes(uc.mem_read(sp, 2), 'little'), int.from_bytes(uc.mem_read(sp + 2, 2), 'little')
        if addr == syms.get('gbd_enter'):
            # (the dispatcher entering a block: the Game Boy's PC)
            g = syms['gb']; m = uc.mem_read(g, 12)
            callers['enter', '%04X' % (m[10] | m[11] << 8)] += 1
        else:
            if os.environ['GBRUN_CALLERS'] == 'gb_cart_write':
                # (the register written, by 2000h, and the value)
                callers['%04X' % (uc.reg_read(UC_X86_REG_AX) & 0xE000), '%02X' % (uc.reg_read(UC_X86_REG_DX) & 0xFF)] += 1
            elif os.environ['GBRUN_CALLERS'] == 'gbf_io_write':
                # (the register and how many times)
                callers['FF%02X' % (uc.reg_read(UC_X86_REG_AX) & 0xFF), ''] += 1
            else:
                callers[where((cs << 4) + ip), '%04X' % (uc.reg_read(UC_X86_REG_AX) & 0xFFF0)] += 1
    a0 = syms[os.environ['GBRUN_CALLERS']]
    uc.hook_add(UC_HOOK_CODE, h_call, None, a0, a0)
if os.environ.get('GBRUN_ENTER'):
    # GBRUN_ENTER=from-to: in those Game Boy frames, every entry of the
    # recompiled code from the dispatcher: the address and the frame's cycle
    e0, e1 = [int(x) for x in os.environ['GBRUN_ENTER'].split('-')]
    def h_en(uc, addr, size, _):
        if not e0 <= st['gbframes'] <= e1: return
        r = lambda n, k=2, sg=False: int.from_bytes(uc.mem_read(syms[n] & 0xFFFF, k), 'little', signed=sg)
        pos = (r('gbf_ev_pos') - r('gbf_cyc', 2, True)) % 17556
        m = uc.mem_read(syms['gb'] & 0xFFFF, 12)
        print('E %d pc=%04X pos=%d line=%d cyc=%d' % (st['gbframes'], m[10] | m[11] << 8, pos, pos // 114, r('gbf_cyc', 2, True)))
    uc.hook_add(UC_HOOK_CODE, h_en, None, syms['gbd_enter'], syms['gbd_enter'])
if os.environ.get('GBRUN_TRANS'):
    # GBRUN_TRANS=frame: from that Game Boy frame on, every block translated:
    # its address, is it code in RAM, the ROM banks' keys, flushes so far
    def h_tr(uc, addr, size, _):
        if st['gbframes'] < int(os.environ['GBRUN_TRANS']): return
        r = lambda n, k=2: int.from_bytes(uc.mem_read(syms[n] & 0xFFFF, k), 'little')
        print('T %d pc=%04X ram=%d key0=%d keyx=%d flushes=%d at %04X:%04X' % (st['gbframes'], r('blk_pc'), r('in_ram', 1), r('drc_key0'), r('drc_keyx'), r('drc_flushes'), r('code_seg'), r('code_top')))
    uc.hook_add(UC_HOOK_CODE, h_tr, None, syms['translate_block'], syms['translate_block'])
stubcalls = None
if os.environ.get('GBRUN_STUB') and CYCLES:
    # GBRUN_STUB=s_stat (a label of gb_drc_asm.s): the blocks whose code is
    # running when that routine is reached (the nearest return address in
    # translated code on the stack), with the Game Boy bytes they start with
    import collections as _c
    stubcalls = _c.Counter()
    soff = syms[os.environ['GBRUN_STUB']] - syms['drc_stubs']
    def h_stub(uc, addr, size, _):
        if st['frame'] < PROF_FROM: return
        sp = uc.reg_read(UC_X86_REG_SP)
        for k in range(0, 12, 2):
            r = int.from_bytes(uc.mem_read(sp + k, 2), 'little')
            if r >= stubs_n:
                stubcalls[addr >> 16, r] += 1
                break
    for seg in range(5, 13):
        uc.hook_add(UC_HOOK_CODE, h_stub, None, (seg << 16) + soff, (seg << 16) + soff)
wrt_hist = None
if os.environ.get('GBRUN_WRT') and CYCLES:
    # GBRUN_WRT=1: the addresses written through s_wrt, by 800h
    import collections as _c
    wrt_hist = _c.Counter()
    woff = syms['s_wrt'] - syms['drc_stubs']
    def h_wrt(uc, addr, size, _):
        if (st['gbframes'] if GBTIME else st['frame']) < PROF_FROM: return
        wrt_hist[uc.reg_read(UC_X86_REG_SI) & 0xF800] += 1
    for seg in range(5, 13):
        uc.hook_add(UC_HOOK_CODE, h_wrt, None, (seg << 16) + woff, (seg << 16) + woff)
e = f['e_entry'] & 0xFFFFF
uc.reg_write(UC_X86_REG_CS, 0xF000); uc.reg_write(UC_X86_REG_IP, e - 0xF0000)
uc.reg_write(UC_X86_REG_SS, 0); uc.reg_write(UC_X86_REG_SP, 0x4000)
line_ports = [None] * 144
line_pals = [None] * 144
END_SHOT = GBTIME and OUT    # (by Game Boy frames: the picture at the end, PREFIX-end.ppm)
last_pic = [None]
snd_frames = []
work_frame = 0; last_gb = 0; last_work = 0
while (GBTIME or st['frame'] < FRAMES) and not st['fail'] and not st.get('stop'):
    # one line. It is drawn as the ports and the palettes are when it
    # starts: the line interrupt of line K is served while K is being
    # drawn and what it changes shows from K + 1 (the other plugins set
    # theirs a line early for that, and so does gb_video.c)
    y = st['line']
    if y == 8 and os.environ.get('GBRUN_SCROLL'): st['scroll8'] = ports[0x12]
    if y == 100 and os.environ.get('GBRUN_SCROLL'):
        # GBRUN_SCROLL=1: at each WonderSwan frame (its line 100) screen 2's X scroll, the first sprite's X and the Game Boy frames done: how evenly a scrolling game moves
        t = (ports[4] & 0x3F) << 9
        print('S %d gb %d scroll %d spr0 %d,%d top %d' % (st['frame'], st['gbframes'], ports[0x12], iram.raw[t + 3][0] if isinstance(iram.raw[t + 3], bytes) else iram.raw[t + 3], iram.raw[t + 2][0] if isinstance(iram.raw[t + 2], bytes) else iram.raw[t + 2], st.get('scroll8', -1)) + ((' list %04X lines %d %d %d ready %d raw_n %d' % ((lambda lp: (lp, iram.raw[lp][0] if isinstance(iram.raw[lp], bytes) else iram.raw[lp], iram.raw[lp + 16][0] if isinstance(iram.raw[lp + 16], bytes) else iram.raw[lp + 16], iram.raw[lp + 32][0] if isinstance(iram.raw[lp + 32], bytes) else iram.raw[lp + 32]))(int.from_bytes(iram.raw[syms['list_shown'] & 0xFFFF:(syms['list_shown'] & 0xFFFF) + 2], 'little')) + (iram.raw[syms['list_ready'] & 0xFFFF][0] if isinstance(iram.raw[syms['list_ready'] & 0xFFFF], bytes) else iram.raw[syms['list_ready'] & 0xFFFF], iram.raw[syms['raw_n'] & 0xFFFF][0] if isinstance(iram.raw[syms['raw_n'] & 0xFFFF], bytes) else iram.raw[syms['raw_n'] & 0xFFFF]))) if 'list_shown' in syms else ''))
    y = st['line']
    if y < 144 and (st['frame'] in SHOTS or END_SHOT):
        line_ports[y] = bytes(ports)
        line_pals[y] = [int.from_bytes(iram.raw[0xFE00 + i * 2:0xFE02 + i * 2], 'little') for i in range(256)]
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
        if ((cs << 4) + ip - 1) & 0xFFFFF in HLTS:
            # (Unicorn stops after a HLT: wait for an interrupt)
            st['halted'] = True
    y = st['line']
    st['line'] = (y + 1) % 159
    y = st['line']
    if y == 144:
        if st['frame'] in SHOTS and OUT and line_ports[0] is not None:
            mem = bytes(iram.raw)
            assert line_ports[0][0x60] & 0xE0 == 0x80, 'not the 2 bpp color mode'
            img = b''.join(render_line(mem, line_ports[i], i, line_pals[i]) for i in range(144))
            open('%s-%04d.ppm' % (OUT, st['frame']), 'wb').write(b'P6\n224 144\n255\n' + img)
        if os.environ.get('GBRUN_SOUND'):
            snd_frames.append((bytes(ports[0x80:0x92]), bytes(iram.raw[(ports[0x8F] << 6):(ports[0x8F] << 6) + 64])))
        if END_SHOT and line_ports[143] is not None:
            last_pic[0] = (bytes(iram.raw), list(line_ports), list(line_pals))
        st['pend'] |= 0x40
        st['frame'] += 1
        if not GBTIME and st['frame'] in KEYS: st['keys'] = KEYS[st['frame']]
        if not GBTIME and CYCLES and st['frame'] == PROF_FROM:
            prof.clear(); hot.clear(); prof_gb0[0] = st['gbframes']
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
if END_SHOT and last_pic[0]:
    mem, lp, lpal = last_pic[0]
    open('%s-end.ppm' % OUT, 'wb').write(b'P6\n224 144\n255\n' + b''.join(render_line(mem, lp[i], i, lpal[i]) for i in range(144)))
if os.environ.get('GBRUN_SOUND') and snd_frames:
    import numpy as np, wave as _w
    RATE = 24000; N = int(RATE / 75.47)
    out = np.zeros(len(snd_frames) * N); ph = [0.0] * 4
    rng = np.random.default_rng(1)
    for fi, (p, wt) in enumerate(snd_frames):
        mix = np.zeros(N)
        for c in range(4):
            if not (p[0x10] >> c & 1): continue
            n = (p[c * 2] | p[c * 2 + 1] << 8) & 0x7FF
            vol = ((p[8 + c] >> 4) + (p[8 + c] & 15)) / 30.0
            if c == 3 and p[0x10] & 0x80:
                mix += (rng.random(N) - 0.5) * vol
                continue
            freq = 3072000.0 / (2048 - n) / 32
            tab = np.array([(wt[c * 16 + i // 2] >> (4 * (i & 1))) & 15 for i in range(32)]) / 15.0 - 0.5
            idx = (ph[c] + np.arange(N) * freq * 32 / RATE)
            mix += tab[idx.astype(int) % 32] * vol
            ph[c] = (ph[c] + N * freq * 32 / RATE) % 32
        out[fi * N:(fi + 1) * N] = mix / 4
    w = _w.open(os.environ['GBRUN_SOUND'], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
    w.writeframes((out * 28000).astype('<i2').tobytes()); w.close()
if os.environ.get('GBRUN_SYMS'):
    # GBRUN_SYMS=name,name,...: the plugin's variables (16 bits) at the end
    mem = bytes(iram.raw)
    print(' '.join('%s=%d' % (n, mem[syms[n] & 0xFFFF] | mem[(syms[n] & 0xFFFF) + 1] << 8) for n in os.environ['GBRUN_SYMS'].split(',')))
if os.environ.get('GBRUN_PEEK'):
    # GBRUN_PEEK=addr,addr,...: bytes of the internal RAM at the end (hex)
    mem = bytes(iram.raw)
    print(' '.join('%s=%02X' % (a, mem[int(a, 16)]) for a in os.environ['GBRUN_PEEK'].split(',')))
if GBTIME:
    # (the Game Boy's memory when its last frame ended: the same for two
    # builds that ran the same game, whatever the picture's frame of delay)
    import zlib
    mem = bytes(iram.raw)
    print('Game Boy memory: video %08X work %08X sprites %08X high %08X' % (zlib.crc32(mem[0x2000:0x4000]), zlib.crc32(mem[0x6000:0x8000]), zlib.crc32(mem[0x9E00:0x9EA0]), zlib.crc32(mem[0x9F80:0x9FFF])))
if vlines is not None:
    for t in sorted(vlines):
        c = vlines[t]; vis = sum(n for l, n in c.items() if l < 144)
        print('tile %3d: %5d writes, %5d in lines 0-143 (lines %s)' % (t, sum(c.values()), vis, ' '.join(str(l) for l in sorted(c)[:16])))
if st['fail']: print('fail: ' + st['fail'])
print('WS frames %d, Game Boy frames %d' % (st['frame'], st['gbframes']))
if CYCLES and FUNC and fhot:
    tot = sum(fhot.values())
    for a, n in sorted(fhot.most_common(40)):
        ins = next(_md.disasm(bytes(uc.mem_read(a, 8)), a))
        print('  %5.1f%%  x%-6d %05X  %s %s' % (100.0 * n / tot, fcnt[a], a, ins.mnemonic, ins.op_str))
if CYCLES and HOT and hot:
    tab = bytes(psram[0x14].raw)
    blocks = sorted((int.from_bytes(tab[i + 6:i + 8], 'little') >> 12, int.from_bytes(tab[i + 4:i + 6], 'little'),
        int.from_bytes(tab[i:i + 2], 'little'), int.from_bytes(tab[i + 2:i + 4], 'little')) for i in range(0, 8192 * 8, 8) if tab[i + 2] | tab[i + 3])
    keys = [(b[0], b[1]) for b in blocks]
    by = collections.Counter(); size = {}
    for a, n in hot.items():
        i = bisect.bisect_right(keys, (a >> 16, a & 0xFFFF)) - 1
        if i >= 0 and blocks[i][0] == a >> 16:
            by[blocks[i][2], blocks[i][3]] += n
    tot = sum(hot.values())
    if os.environ.get('GBRUN_HOTDIS'):
        # GBRUN_HOTDIS=n: the n hottest blocks' V30MZ code, each instruction's cycles
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
        where_b = {(b[2], b[3]): (b[0], b[1]) for b in blocks}
        for (pc, key), n in by.most_common(int(os.environ['GBRUN_HOTDIS'])):
            seg, off = where_b[pc, key]
            i = keys.index((seg, off))
            end = blocks[i + 1][1] if i + 1 < len(blocks) and blocks[i + 1][0] == seg else off + 0x200
            print('--- block %04X key %d at %X:%04X, %d bytes' % (pc, key, seg, off, end - off))
            for ins in md.disasm(bytes(uc.mem_read((seg << 16) + off, min(end - off, 0x300))), off):
                print('  %7d  %04X  %-20s %s %s' % (hot.get((seg << 16) + ins.address, 0), ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
    for (pc, key), n in by.most_common(8):
        a = pc if pc < 0x4000 else (key - 1) * 0x4000 + (pc - 0x4000)
        print('  %5.1f%% of translated code: block %04X (bank %d): %s' % (100.0 * n / tot, pc, key - 1, rom[a:a + 20].hex() if key < 0x8000 else 'RAM'))
if wrt_hist:
    print('s_wrt: ' + ' '.join('%04X:%d' % kv for kv in sorted(wrt_hist.items())))
if stubcalls:
    tab = bytes(psram[0x14].raw)
    blocks = sorted((int.from_bytes(tab[i + 6:i + 8], 'little') >> 12, int.from_bytes(tab[i + 4:i + 6], 'little'),
        int.from_bytes(tab[i:i + 2], 'little'), int.from_bytes(tab[i + 2:i + 4], 'little')) for i in range(0, 8192 * 8, 8) if tab[i + 2] | tab[i + 3])
    by = collections.Counter()
    for (seg, r), n in stubcalls.items():
        best = None
        for b in blocks:
            if b[0] == seg and b[1] <= r: best = b
        if best: by[best[2], best[3]] += n
    for (pc, key), n in by.most_common(10):
        a = pc if pc < 0x4000 else (key - 1) * 0x4000 + (pc - 0x4000)
        print('  %6d  block %04X (bank %d): %s' % (n, pc, key - 1, rom[a:a + 14].hex() if key < 0x8000 else 'RAM'))
if callers:
    print('  calls: %d' % sum(callers.values()))
    for k, v in callers.most_common(12): print('  %6d  %s %s' % (v, k[0], k[1]))
if CYCLES and st['gbframes']:
    # (not the wait for the frame's time, nor the wait for a list to be shown: HLT's cycles are not counted anyway)
    for k in ('gbv_wait', 'gbv_now_lines'): prof.pop(k, None)
    tot = sum(prof.values())
    print('V30MZ cycles a Game Boy frame, from frame %d: %d (a WonderSwan frame has 40704, a Game Boy frame\'s time 51430)' % (PROF_FROM, tot // max(1, st['gbframes'] - prof_gb0[0])))
    for k, v in prof.most_common(14): print('  %5.1f%%  %s' % (100.0 * v / tot, k))

