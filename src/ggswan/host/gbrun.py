#!/usr/bin/env python3
"""Runs the WS build of gbswan (build/gbswan.elf) in Unicorn from its entry
point (the C start-up included) and draws the WonderSwan Color screen in
the 2 bits a pixel color mode the plugin uses: screen 1, the sprites with
their window, screen 2 in its window. No interrupts; the line counter
(port 02h) advances with the instructions run. Checks, too, what
GB-MEMORY-MAP.md promises: the stack never above 2000h once gb_run has
moved it (its lowest point is printed: what the C data must stay under).
usage: gbrun.py build/gbswan.elf frames out.ppm [keys "frame:hex,..."]
keys: what ws_keypad_scan returns (Y1-Y4 01 02 04 08, X1-X4 10 20 40 80,
START 200, A 400, B 800), held from that frame on."""
import sys
from elftools.elf.elffile import ELFFile
from unicorn import *
from unicorn.x86_const import *

ELF, FRAMES, OUT = sys.argv[1], int(sys.argv[2]), sys.argv[3]
KEYS = dict((int(k.split(':')[0]), int(k.split(':')[1], 16)) for k in sys.argv[4].split(',')) if len(sys.argv) > 4 and sys.argv[4] else {}
f = ELFFile(open(ELF, 'rb'))
uc = Uc(UC_ARCH_X86, UC_MODE_16)
uc.mem_map(0, 0x110000)
for s in f.iter_sections():
    if s['sh_flags'] & 2 and s['sh_size'] and s['sh_type'] == 'SHT_PROGBITS':
        uc.mem_write(s['sh_addr'] & 0xFFFFF, s.data())
syms = {}
for y in f.get_section_by_name('.symtab').iter_symbols():
    syms.setdefault(y.name, y['st_value'] & 0xFFFFF)

LINE_INSNS = 60          # instructions a line (only to make the frames go by)
ports = bytearray(256)
ports[0xA0] = 0x86          # a WonderSwan Color
st = {'n': 0, 'frame': 0, 'keys': 0, 'moved': False, 'sp_max': 0, 'sp_min': 0xFFFF, 'bad': []}
def line(): return (st['n'] // LINE_INSNS) % 159
def h_in(uc, port, size, _):
    port &= 0xFF
    if port == 0x02: return line()
    if port == 0xB5:
        k = st['keys']; g = ports[0xB5]
        v = 0
        if g & 0x10: v |= k & 15
        if g & 0x20: v |= (k >> 4) & 15
        if g & 0x40: v |= (k >> 8) & 15
        return (g & 0xF0) | v
    v = ports[port]
    return v | ports[(port + 1) & 0xFF] << 8 if size == 2 else v
def h_out(uc, port, size, value, _):
    port &= 0xFF
    ports[port] = value & 0xFF
    if size == 2: ports[(port + 1) & 0xFF] = value >> 8 & 0xFF
def h_code(uc, addr, size, _):
    st['n'] += 1
    if st['n'] % (LINE_INSNS * 159) == 0:
        st['frame'] += 1
        if st['frame'] in KEYS: st['keys'] = KEYS[st['frame']]
        if st['frame'] >= FRAMES: uc.emu_stop()
    if addr == syms.get('gb_main'): st['moved'] = True
    if st['moved']:
        sp = uc.reg_read(UC_X86_REG_SP)
        if uc.reg_read(UC_X86_REG_SS) != 0: st['bad'].append('SS != 0 at %05X' % addr)
        st['sp_max'] = max(st['sp_max'], sp); st['sp_min'] = min(st['sp_min'], sp)
uc.hook_add(UC_HOOK_INSN, h_in, None, 1, 0, UC_X86_INS_IN)
uc.hook_add(UC_HOOK_INSN, h_out, None, 1, 0, UC_X86_INS_OUT)
uc.hook_add(UC_HOOK_CODE, h_code)
e = f['e_entry'] & 0xFFFFF
uc.reg_write(UC_X86_REG_CS, 0xFFFF if e >= 0xFFFF0 else (e >> 4) & 0xF000)
cs = uc.reg_read(UC_X86_REG_CS)
uc.reg_write(UC_X86_REG_IP, e - (cs << 4))
uc.reg_write(UC_X86_REG_SS, 0); uc.reg_write(UC_X86_REG_SP, 0x4000)
start = e
while st['frame'] < FRAMES:
    try:
        uc.emu_start(start, 0xFFFFFFFF)
    except UcError as x:
        cs = uc.reg_read(UC_X86_REG_CS); ip = uc.reg_read(UC_X86_REG_IP)
        print('stopped: %s at %04X:%04X' % (x, cs, ip)); break
    cs = uc.reg_read(UC_X86_REG_CS); ip = uc.reg_read(UC_X86_REG_IP)
    start = (cs << 4) + ip
    if uc.mem_read(start, 1)[0] == 0xF4:      # HLT: no interrupts here
        uc.reg_write(UC_X86_REG_IP, ip + 1); start += 1

iram = bytes(uc.mem_read(0, 0x10000))
def render(iram, ports):
    """224 x 144 of RGB from the internal RAM and the ports: the color modes
    with 2 bits a pixel (60h = 80h)"""
    assert ports[0x60] & 0xE0 == 0x80, 'not the 2 bpp color mode: %02X' % ports[0x60]
    pal = [int.from_bytes(iram[0xFE00 + i * 2:0xFE02 + i * 2], 'little') for i in range(256)]
    def tile_px(t, r, c):
        p = 0x2000 + t * 16 + r * 2
        b = 7 - c
        return (iram[p] >> b & 1) | (iram[p + 1] >> b & 1) << 1
    def screen(scr, x, y):
        base = ((ports[7] >> 4) if scr else (ports[7] & 15)) << 11
        sx = (x + ports[0x12 if scr else 0x10]) & 255
        sy = (y + ports[0x13 if scr else 0x11]) & 255
        a = base + ((sy >> 3) * 32 + (sx >> 3)) * 2
        e = iram[a] | iram[a + 1] << 8
        t = (e & 0x1FF) | ((e >> 13) & 1) << 9
        r, c = sy & 7, sx & 7
        if e & 0x8000: r = 7 - r
        if e & 0x4000: c = 7 - c
        px = tile_px(t, r, c)
        p = (e >> 9) & 15
        # color 0: seen in palettes 0-3 and 8-11, clear in the others
        return pal[p * 16 + px] if px or not (p & 4) else None
    ctrl = ports[0]
    tab = (ports[4] & 0x3F) << 9
    out = bytearray()
    for y in range(144):
        for x in range(224):
            c = pal[((ports[1] >> 4) & 15) * 16 + (ports[1] & 15)]
            if ctrl & 1:
                v = screen(0, x, y)
                if v is not None: c = v
            lo = hi = None
            if ctrl & 4:
                for i in range(ports[5] + ports[6] - 1, ports[5] - 1, -1):   # (lower index on top)
                    s = iram[tab + i * 4:tab + i * 4 + 4]
                    dy, dx = (y - s[2]) & 255, (x - s[3]) & 255
                    if dy >= 8 or dx >= 8: continue
                    if ctrl & 8:
                        ins = ports[0xC] <= x <= ports[0xE] and ports[0xD] <= y <= ports[0xF]
                        if ins == bool(s[1] & 0x10): continue
                    if s[1] & 0x80: dy = 7 - dy
                    if s[1] & 0x40: dx = 7 - dx
                    px = tile_px(s[0] | (s[1] & 1) << 8, dy, dx)
                    p = 8 + ((s[1] >> 1) & 7)
                    if not px and p & 4: continue
                    cc = pal[p * 16 + px]
                    if s[1] & 0x20: hi, lo = cc, None
                    else: lo, hi = cc, None
            if lo is not None: c = lo
            if ctrl & 2:
                inside = ports[8] <= x <= ports[0xA] and ports[9] <= y <= ports[0xB]
                if not (ctrl & 0x20) or (inside != bool(ctrl & 0x10)):
                    v = screen(1, x, y)
                    if v is not None: c = v
            if hi is not None: c = hi
            out += bytes((((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17))
    return bytes(out)
if __name__ == '__main__':
    open(OUT, 'wb').write(b'P6\n224 144\n255\n' + render(iram, ports))
    print('frames %d, stack %04X-%04X after gb_run, heap start %04X' % (st['frame'], st['sp_min'], st['sp_max'], syms.get('__wf_heap_start', 0)))
    ok = st['moved'] and st['sp_max'] <= 0x2000 and st['sp_min'] >= syms.get('__wf_heap_start', 0) and not st['bad']
    for b in st['bad'][:5]: print('  ' + b)
    print('memory map: %s' % ('ok' if ok else 'WRONG'))
    sys.exit(0 if ok else 1)
