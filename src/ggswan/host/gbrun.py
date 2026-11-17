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
GBRUN_HW=1: the cycles as the console takes them (wsrun.py, cost_hw:
measured on a SwanCrystal; about a third more than Mednafen's), and what
is to be believed for speed since patch 185; GBRUN_FAST=1 with it: the
cartridge's ports without their wait. GBRUN_KIND=1 with it: the cycles of
the translated code and of its routines by what they go on (fetching,
accesses, jumps, the cycle count, N). GBRUN_WRT=1: the addresses the
routines s_wrt and s_rd are called for. GBRUN_OAMSAME=1: how many times
the sprites are rebuilt with the OAM unchanged.
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
HW = bool(os.environ.get('GBRUN_HW'))     # GBRUN_HW=1: the cycles as the console takes them (wsrun.py's cost_hw), not Mednafen's
CYCLES = bool(os.environ.get('GBRUN_CYCLES')) or HW
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
    # read and run only: on the cartridge a write to the linear area does nothing
    # (hw_test on a WonderSwan, 2026-11-16); here it stops the run, to be found
    uc.mem_map_ptr(seg << 16, 0x10000, 7 if os.environ.get('GBRUN_LINW') else 5, ctypes.addressof(psram[(0x10 | seg) & 31]))
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
    if port == 0xE4: return 0x7E1F          # (as read on the cartridge)
    if port == 0xC0 and size == 1: return 0x3F
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
cur_file = [None, None]
def h_sd(uc, addr, size, name):
    ax = uc.reg_read(UC_X86_REG_AX)
    if name == 'f_open':
        # (the ROM, or the state's file: GBRUN_STFILE=file is the card's .st0, read if it is there and written)
        path = bytes(uc.mem_read(uc.reg_read(UC_X86_REG_DX), 260)).split(b'\0')[0]
        mode = uc.reg_read(UC_X86_REG_CX) & 0xFF
        if path.lower().endswith(b'.st0'):
            stf = os.environ.get('GBRUN_STFILE')
            if mode & 0x02:
                cur_file[0] = bytearray(); cur_file[1] = stf
            elif stf and os.path.exists(stf):
                cur_file[0] = bytearray(open(stf, 'rb').read()); cur_file[1] = None
            else:
                uc.reg_write(UC_X86_REG_AX, 4); retf(); return      # FR_NO_FILE
            print('SD: %s opened, mode %02X' % (path.decode('latin1'), mode))
        else:
            cur_file[0] = rom; cur_file[1] = None
        uc.mem_write(ax + 0xA, len(cur_file[0]).to_bytes(4, 'little')); rom_pos[0] = 0
    elif name == 'f_read':
        buf = uc.reg_read(UC_X86_REG_DX)
        sp = uc.reg_read(UC_X86_REG_SP)
        n = int.from_bytes(uc.mem_read(sp + 4, 2), 'little')
        brp = int.from_bytes(uc.mem_read(sp + 6, 2), 'little')
        d = bytes(cur_file[0][rom_pos[0]:rom_pos[0] + n]); rom_pos[0] += len(d)
        uc.mem_write(buf, d); uc.mem_write(brp, len(d).to_bytes(2, 'little'))
        uc.reg_write(UC_X86_REG_AX, 0); retf(4); return
    elif name == 'f_write':
        buf = uc.reg_read(UC_X86_REG_DX)
        sp = uc.reg_read(UC_X86_REG_SP)
        n = int.from_bytes(uc.mem_read(sp + 4, 2), 'little')
        bwp = int.from_bytes(uc.mem_read(sp + 6, 2), 'little')
        cur_file[0] += bytes(uc.mem_read(buf, n))
        uc.mem_write(bwp, n.to_bytes(2, 'little'))
        uc.reg_write(UC_X86_REG_AX, 0); retf(4); return
    elif name == 'f_close':
        if cur_file[1]:
            open(cur_file[1], 'wb').write(bytes(cur_file[0])); print('SD: %d bytes written to %s' % (len(cur_file[0]), cur_file[1]))
        cur_file[1] = None
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
                prof.clear(); hot.clear(); (CALLS.clear() if CALLS is not None else None); fhot.clear(); fcnt.clear(); prof_gb0[0] = g
                if callers is not None: callers.clear()
            if g >= FRAMES: st['stop'] = True; uc.emu_stop()
        return
    uc.reg_write(UC_X86_REG_AX, 0); retf()
for n in ('f_mount', 'f_open', 'f_read', 'f_write', 'f_close', 'fail', 'gbv_frame'):
    if n in syms: uc.hook_add(UC_HOOK_CODE, h_sd, n, syms[n], syms[n])
def h_arm(uc, addr, size, _): st['gbv_armed'] = True
if 'gbv_frame' in syms:
    _a = syms['gbv_frame'] + next(_md.disasm(bytes(uc.mem_read(syms['gbv_frame'], 8)), 0)).size
    uc.hook_add(UC_HOOK_CODE, h_arm, None, _a, _a)

# GBRUN_TRUTH=1: each picture in GBRUN_SHOTS against the same picture with
# the patterns as the Game Boy had them at line 0 of the frame shown
# (the tiles, and what gb_video.c keeps to copy to them later): T lines,
# pixels unlike with all the patterns put back and with the sprites' only.
# GBRUN_NOTQ=1: gb_video.c never keeps the patterns late
truth = {'fifo': [], 'cur': None, 'line0': None, 'shown': None}
def _b(n): return iram.raw[syms[n] & 0xFFFF] if not isinstance(iram.raw[syms[n] & 0xFFFF], bytes) else iram.raw[syms[n] & 0xFFFF][0]
def h_line0(uc, addr, size, _):
    m = bytearray(bytes(iram.raw[0x2000:0x3800]))
    if 'gbv_tq_cur' in syms:
        cur, pend, n = _b('gbv_tq_cur'), _b('tq_pend'), _b('gbv_tq_n')
        c = syms['tq_cnt'] & 0xFFFF
        cnt = bytes(iram.raw[c:c + 3])
        for g, k in [((cur + 3 - j) % 3, cnt[(cur + 3 - j) % 3]) for j in range(pend, 0, -1)] + [(cur, n)]:
            for i in range(k):
                a = int.from_bytes(bytes(iram.raw[0xCDB0 + (g * 32 + i) * 2:0xCDB2 + (g * 32 + i) * 2]), 'little') - 0x2000
                m[a:a + 16] = bytes(iram.raw[0xF800 + g * 512 + i * 16:0xF810 + g * 512 + i * 16])
    truth['line0'] = bytes(m)
def h_truth_frame(uc, addr, size, _):
    if os.environ.get('GBRUN_NOTQ'): uc.mem_write(syms['gbv_pw_last'] & 0xFFFF, b'\0\0')
    if truth.get('last') == st['gbframes']: return
    truth['last'] = st['gbframes']
    truth['fifo'].append((st['gbframes'], truth['line0']))
    if os.environ.get('GBRUN_TRUTHQ'):
        pw = int.from_bytes(bytes(iram.raw[syms['gbv_pw_last'] & 0xFFFF:(syms['gbv_pw_last'] & 0xFFFF) + 2]), 'little')
        sp = bytes(iram.raw[0x5800:0x5A00])
        tl = sorted(set((sp[i] | sp[i + 1] << 8) & 0x1FF for i in range(0, 160, 4) if sp[i + 2] < 144))
        print('Q ws %d line %d gb %d pw %04X tile %d on %d left %d spr %s' % (st['frame'], st['line'], st['gbframes'], pw, (pw - 0x8000) >> 4 if pw else -1, _b('gbv_tq_on'), _b('tq_left'), tl))
if os.environ.get('GBRUN_TRUTH') and 'gbv_line0' in syms:
    uc.hook_add(UC_HOOK_CODE, h_line0, None, syms['gbv_line0'], syms['gbv_line0'])
    uc.hook_add(UC_HOOK_CODE, h_truth_frame, None, syms['gbv_frame'], syms['gbv_frame'])

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
    hot = collections.Counter(); hcnt = collections.Counter()
    FUNC = os.environ.get('GBRUN_FUNC')     # GBRUN_FUNC=name: that function's instructions by cycles
    if FUNC in stub_names: FUNC = 'routine ' + FUNC      # (or a routine of gb_drc_asm.s, by its label)
    # GBRUN_CALLS=1: every function of the plugin by its cycles (its own, not its callees') and the times it is entered, from GBRUN_PROF_FROM
    CALLS = collections.Counter() if os.environ.get('GBRUN_CALLS') else None
    fstart = set(starts)
    fhot = collections.Counter(); fcnt = collections.Counter()
    jst = [0, 0, 0, 0]
    KIND = collections.Counter() if os.environ.get('GBRUN_KIND') and HW else None
    K_N = b'\xC6\x06' + (syms.get('drc_n', 0) & 0xFFFF).to_bytes(2, 'little'); K_CYC = b'\x83\x2E' + (syms.get('gbf_cyc', 0) & 0xFFFF).to_bytes(2, 'little'); K_CYC2 = b'\x83\x3E' + K_CYC[2:]
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
            k = cost_hw(code, uc.reg_read(UC_X86_REG_CX), jst[2] != addr) if HW else cost_m(code, uc.reg_read(UC_X86_REG_CX), jst[2] != addr) + (0,)
            if code[0] not in (0xF2, 0xF3): ccache[code] = k
        c, ex, fl = k
        if addr != jst[1]:
            if jst[0]: c += jst[0]
            if HW and (jst[0] or jst[3]) and addr & 1: c += 1
        if KIND is not None:
            # GBRUN_KIND=1 (with GBRUN_HW): the cycles of the translated code and of its routines by what they are spent on
            w = where(addr)
            if w[0] == 't' or w[:7] == 'routine':
                g = 'T ' if w[0] == 't' else 'R '
                KIND[g + 'all'] += c
                if addr != jst[1]:
                    if jst[0]: KIND[g + 'jump taken (3 each)'] += jst[0]
                    if (jst[0] or jst[3]) and addr & 1: KIND[g + 'odd target (1 each)'] += 1
                h = HWP.get(code)
                if h: KIND[g + 'longer to fetch than to run'] += h[0]; KIND[g + 'accesses to memory'] += h[1]
                o = code[0]
                if code[:4] == K_N: KIND[g + 'the N flag stored (mov byte [drc_n], n)'] += k[0]
                elif code[:4] == K_CYC or code[:4] == K_CYC2: KIND[g + 'the cycles counted (sub word [gbf_cyc], n)'] += k[0]
                elif o == 0xE9 and -128 <= int.from_bytes(code[1:3], 'little', signed=True) + 3 - 2 + 2 < 126: KIND[g + 'jmp near to a place a short jump reaches'] += k[0]
                if o in (0xE8, 0xC3, 0xC2): KIND[g + 'call, ret'] += k[0]
                elif o in (0xE9, 0xEB): KIND[g + 'jmp'] += k[0]
                elif 0xE4 <= o < 0xE8 or 0xEC <= o < 0xF0: KIND[g + 'ports'] += k[0]
                elif 0x50 <= o < 0x60 or o in (0x9C, 0x9D): KIND[g + 'push, pop'] += k[0]
                elif o in (0x26, 0x2E, 0x36, 0x3E): KIND[g + 'with a segment prefix'] += k[0]
        jst[0] = ex; jst[1] = addr + size; jst[2] = addr; jst[3] = fl
        cyc[0] += c
        w = where(addr)
        prof[w] += c
        if w == 'translated code' and HOT: hot[addr] += c; hcnt[addr] += 1
        if w == FUNC: fhot[addr] += c; fcnt[addr] += 1
        if CALLS is not None and addr in fstart: CALLS[w] += 1
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
    vtf = _c2.defaultdict(set)     # the tiles written in each Game Boy frame (144 to 144)
    def h_vw(uc, access, addr, size, value, _):
        if st['gbframes'] < int(os.environ['GBRUN_VRAMLINES']): return
        ev = int.from_bytes(uc.mem_read(syms['gbf_ev_pos'] & 0xFFFF, 2), 'little'); cy = int.from_bytes(uc.mem_read(syms['gbf_cyc'] & 0xFFFF, 2), 'little', signed=True)
        vlines[(addr - 0x2000) >> 4][((ev - cy) % 17556) // 114] += 1
        vtf[st['gbframes']].add((addr - 0x2000) >> 4)
    uc.hook_add(UC_HOOK_MEM_WRITE, h_vw, None, 0x2000, 0x37FF)
opmix = None
if os.environ.get('GBRUN_OPMIX') and 'translate_block' in syms:
    # GBRUN_OPMIX=1: the Game Boy instructions translated since GBRUN_PROF_FROM, by opcode
    import collections as _c3
    opmix = _c3.Counter()
    def h_opmix(uc, addr, size, _):
        if (st['gbframes'] if GBTIME else st['frame']) < PROF_FROM: return
        r = lambda n, k: bytes(iram.raw[(syms[n] & 0xFFFF):(syms[n] & 0xFFFF) + k])
        n = r('n_ins', 1)[0]; tb = int.from_bytes(r('tbase', 2), 'little'); tl = r('tlen', 1)[0]; buf = r('tbuf', 128)
        pcs = r('i_pc', 2 * n)
        for i in range(n):
            o = (int.from_bytes(pcs[2 * i:2 * i + 2], 'little') - tb) & 0xFFFF
            if o < tl: opmix[buf[o] if buf[o] != 0xCB or o + 1 >= tl else 0x100 | buf[o + 1]] += 1
            else: opmix[-1] += 1
    uc.hook_add(UC_HOOK_CODE, h_opmix, None, syms['translate_block'], syms['translate_block'])
if 'gbv_note' in syms:
    # a state kept or brought back (gb_main.c): the Game Boy's memory then
    def h_note(uc, addr, size, _):
        mem = bytes(iram.raw)
        print('NOTE gb %d: video %08X work %08X sprites %08X high %08X io %08X' % (st['gbframes'], zlib.crc32(mem[0x2000:0x4000]), zlib.crc32(mem[0x6000:0x8000]), zlib.crc32(mem[0x9E00:0x9EA0]), zlib.crc32(mem[0x9F80:0x9FFF]), zlib.crc32(mem[0x9F00:0x9F80])))
    uc.hook_add(UC_HOOK_CODE, h_note, None, syms['gbv_note'], syms['gbv_note'])
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
if os.environ.get('GBRUN_OAMSAME') and 'gbv_sprites_asm' in syms:
    # GBRUN_OAMSAME=1: how many times the sprites are rebuilt with the OAM as it was the time before
    oam_st = [None, 0, 0]
    def h_oam(uc, addr, size, _):
        if (st['gbframes'] if GBTIME else st['frame']) < PROF_FROM: return
        o = bytes(uc.mem_read(0x9E00, 160))
        oam_st[2] += 1
        if o == oam_st[0]: oam_st[1] += 1
        oam_st[0] = o
    uc.hook_add(UC_HOOK_CODE, h_oam, None, syms['gbv_sprites_asm'], syms['gbv_sprites_asm'])
    import atexit as _ae
    _ae.register(lambda: print('sprites rebuilt %d times, %d with the OAM unchanged' % (oam_st[2], oam_st[1])))
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
    # (and those read through s_rd and s_rdt)
    rd_hist = _c.Counter()
    def h_rdh(uc, addr, size, _):
        if (st['gbframes'] if GBTIME else st['frame']) < PROF_FROM: return
        rd_hist[uc.reg_read(UC_X86_REG_SI) & 0xF800] += 1
    for nm in ('s_rd', 's_rdt'):
        if nm in syms:
            roff = syms[nm] - syms['drc_stubs']
            for seg in range(5, 13):
                uc.hook_add(UC_HOOK_CODE, h_rdh, None, (seg << 16) + roff, (seg << 16) + roff)
if os.environ.get('GBRUN_SLOW') and 's_exit_slow' in syms:
    # GBRUN_SLOW=from-to: in those Game Boy frames, why the routine that leaves a block went to the dispatcher
    import collections as _c
    slow = _c.Counter()
    s0, s1 = [int(x) for x in os.environ['GBRUN_SLOW'].split('-')]
    soff2 = syms['s_exit_slow'] - syms['drc_stubs']
    def h_slow(uc, addr, size, _):
        if not s0 <= st['gbframes'] <= s1: return
        r = lambda n, k=2, sg=False: int.from_bytes(uc.mem_read(syms[n] & 0xFFFF, k), 'little', signed=sg)
        g = syms['gb'] & 0xFFFF; m = uc.mem_read(g, 52)
        slow['pc=%04X ei=%d interp=%d halted=%d cyc%s link=%d fdone=%d ime=%d sp=%04X' % (m[10] | m[11] << 8, r('drc_ei', 1), r('drc_interp', 1), m[20], '>0' if r('gbf_cyc', 2, True) > 0 else '<=0', r('drc_link') != 0, m[51], m[18], uc.reg_read(UC_X86_REG_BP))] += 1
    for seg in range(5, 13):
        uc.hook_add(UC_HOOK_CODE, h_slow, None, (seg << 16) + soff2, (seg << 16) + soff2)
    import atexit
    atexit.register(lambda: [print('  slow %6d  %s' % (v, k)) for k, v in slow.most_common(16)])
if os.environ.get('GBRUN_PACE'):
    # GBRUN_PACE=from-to: in those Game Boy frames, at gbv_wait's entry: the WonderSwan's time in lines, next_at, line_base, list_ready
    p0, p1 = [int(x) for x in os.environ['GBRUN_PACE'].split('-')]
    def h_pace(uc, addr, size, _):
        if not p0 <= st['gbframes'] <= p1: return
        r = lambda n, k=2: int.from_bytes(uc.mem_read(syms[n] & 0xFFFF, k), 'little')
        print('P %d ws %d line %d next_at %d line_base %d ready %d' % (st['gbframes'], st['frame'], st['line'], r('next_at'), r('line_base'), r('list_ready', 1)))
    uc.hook_add(UC_HOOK_CODE, h_pace, None, syms['gbv_wait'], syms['gbv_wait'])
e = f['e_entry'] & 0xFFFFF
uc.reg_write(UC_X86_REG_CS, 0xF000); uc.reg_write(UC_X86_REG_IP, e - 0xF0000)
uc.reg_write(UC_X86_REG_SS, 0); uc.reg_write(UC_X86_REG_SP, 0x4000)
line_ports = [None] * 144
line_pals = [None] * 144
SPRLATCH = [None, None] if os.environ.get('GBRUN_SPRLATCH') else None     # GBRUN_SPRLATCH=1: the pictures with the sprites as the WonderSwan takes them, at line 142 of the frame before
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
        print('S %d gb %d scroll %d spr0 %d,%d top %d' % (st['frame'], st['gbframes'], ports[0x12], iram.raw[t + 3][0] if isinstance(iram.raw[t + 3], bytes) else iram.raw[t + 3], iram.raw[t + 2][0] if isinstance(iram.raw[t + 2], bytes) else iram.raw[t + 2], st.get('scroll8', -1)) + ((' list %04X lines %d %d %d ready %d raw_n %d' % ((lambda lp: (lp, iram.raw[lp][0] if isinstance(iram.raw[lp], bytes) else iram.raw[lp], iram.raw[lp + 16][0] if isinstance(iram.raw[lp + 16], bytes) else iram.raw[lp + 16], iram.raw[lp + 32][0] if isinstance(iram.raw[lp + 32], bytes) else iram.raw[lp + 32]))(int.from_bytes(iram.raw[syms['list_shown'] & 0xFFFF:(syms['list_shown'] & 0xFFFF) + 2], 'little')) + (iram.raw[syms['list_ready'] & 0xFFFF][0] if isinstance(iram.raw[syms['list_ready'] & 0xFFFF], bytes) else iram.raw[syms['list_ready'] & 0xFFFF], iram.raw[syms.get('raw_n', syms.get('gbv_raw_n')) & 0xFFFF][0] if isinstance(iram.raw[syms.get('raw_n', syms.get('gbv_raw_n')) & 0xFFFF], bytes) else iram.raw[syms.get('raw_n', syms.get('gbv_raw_n')) & 0xFFFF]))) if 'list_shown' in syms else ''))
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
    if y == 0 and 'list_shown' in syms and os.environ.get('GBRUN_TRUTH'):
        ls = bytes(iram.raw[syms['list_shown'] & 0xFFFF:(syms['list_shown'] & 0xFFFF) + 2])
        if ls != truth['shown']:
            truth['shown'] = ls
            if truth['fifo']: truth['cur'] = truth['fifo'].pop(0)
    if y == 142 and SPRLATCH is not None:
        # (the WonderSwan copies the sprites' table, and takes their first and count, at line 142: the next frame's sprites)
        b = (ports[4] & 0x3F) << 9
        SPRLATCH[0] = SPRLATCH[1]; SPRLATCH[1] = (b, bytes(iram.raw[b:b + 512]), ports[5], ports[6])
    if y == 144:
        if st['frame'] in SHOTS and OUT and line_ports[0] is not None:
            mem = bytes(iram.raw)
            if SPRLATCH is not None and SPRLATCH[0]:
                b, tb, f5, f6 = SPRLATCH[0]
                mem = mem[:b] + tb + mem[b + 512:]
                for i in range(144):
                    lp = bytearray(line_ports[i]); lp[4] = b >> 9; lp[5] = f5; lp[6] = f6; line_ports[i] = bytes(lp)
            assert line_ports[0][0x60] & 0xE0 == 0x80, 'not the 2 bpp color mode'
            img = b''.join(render_line(mem, line_ports[i], i, line_pals[i]) for i in range(144))
            open('%s-%04d.ppm' % (OUT, st['frame']), 'wb').write(b'P6\n224 144\n255\n' + img)
            if truth['cur'] and truth['cur'][1]:
                m2 = mem[:0x2000] + truth['cur'][1] + mem[0x3800:]
                img2 = b''.join(render_line(m2, line_ports[i], i, line_pals[i]) for i in range(144))
                m3 = bytearray(mem); t0 = (line_ports[0][4] & 0x3F) << 9
                for i in range(128):
                    t = (mem[t0 + i * 4] | mem[t0 + i * 4 + 1] << 8) & 0x1FF
                    if t < 384: m3[0x2000 + t * 16:0x2010 + t * 16] = truth['cur'][1][t * 16:t * 16 + 16]
                img3 = b''.join(render_line(bytes(m3), line_ports[i], i, line_pals[i]) for i in range(144))
                print('T ws %d gb %d all %d spr %d on %d left %d' % (st['frame'], truth['cur'][0], sum(1 for i in range(0, len(img), 3) if img[i] != img2[i]), sum(1 for i in range(0, len(img), 3) if img[i] != img3[i]), _b('gbv_tq_on'), _b('tq_left')))
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
    # (the translator's work: the same blocks, byte for byte, from two builds?)
    if os.environ.get('GBRUN_CODEDUMP'): open(os.environ['GBRUN_CODEDUMP'], 'wb').write(b''.join(bytes(psram[i].raw) for i in range(0x14, 0x1D)))
    print('translated: table %08X code %08X' % (zlib.crc32(bytes(psram[0x14].raw)), zlib.crc32(b''.join(bytes(psram[i].raw) for i in range(0x15, 0x1D)))))
if vlines is not None:
    print('tiles written a Game Boy frame: most %d; frames by count: %s' % (max(len(v) for v in vtf.values()), sorted(_c2.Counter(len(v) for v in vtf.values()).items())))
    print('by frame:', ' '.join('%d:%d' % (f, len(v)) for f, v in sorted(vtf.items())))
    for t in sorted(vlines):
        c = vlines[t]; vis = sum(n for l, n in c.items() if l < 144)
        print('tile %3d: %5d writes, %5d in lines 0-143 (lines %s)' % (t, sum(c.values()), vis, ' '.join(str(l) for l in sorted(c)[:16])))
if st['fail']: print('fail: ' + st['fail'])
print('WS frames %d, Game Boy frames %d' % (st['frame'], st['gbframes']))
if CYCLES and FUNC and fhot:
    tot = sum(fhot.values())
    if os.environ.get('GBRUN_FUNCDUMP'):
        open(os.environ['GBRUN_FUNCDUMP'], 'w').write(''.join('%d %d %d\n' % (a - min(fhot), c, fcnt[a]) for a, c in sorted(fhot.items())))
    for a, n in sorted(fhot.most_common(40)):
        ins = next(_md.disasm(bytes(uc.mem_read(a, 8)), a))
        print('  %5.1f%%  x%-6d %05X  %s %s' % (100.0 * n / tot, fcnt[a], a, ins.mnemonic, ins.op_str))
if CYCLES and HOT and hot:
    if os.environ.get('GBRUN_JMPS'):
        # GBRUN_JMPS=1: the translated code's cycles by first opcode byte (EB: the short jumps over the slow ways)
        byop = collections.Counter(); nop = collections.Counter()
        for a_, n_ in hot.items():
            b_ = uc.mem_read(a_, 1)[0]
            byop[b_] += n_; nop[b_] += hcnt.get(a_, 0)
        t_ = sum(hot.values())
        print('translated code by opcode: ' + ' '.join('%02X:%.1f%%(x%d)' % (k, 100.0 * v / t_, nop[k]) for k, v in byop.most_common(14)))
    tab = bytes(psram[0x14].raw)
    blocks = sorted((int.from_bytes(tab[i + 6:i + 8], 'little') >> 12, int.from_bytes(tab[i + 4:i + 6], 'little'),
        int.from_bytes(tab[i:i + 2], 'little'), int.from_bytes(tab[i + 2:i + 4], 'little')) for i in range(0, 8192 * 8, 8) if tab[i + 2] | tab[i + 3])
    keys = [(b[0], b[1]) for b in blocks]
    by = collections.Counter(); size = {}
    for a, n in hot.items():
        i = bisect.bisect_right(keys, (a >> 16, a & 0xFFFF)) - 1
        if i >= 0 and blocks[i][0] == a >> 16:
            by[blocks[i][2], blocks[i][3]] += n
    if os.environ.get('GBRUN_ONCE'):
        # GBRUN_ONCE=1: how many times each block was entered at its start (since GBRUN_PROF_FROM)
        h = collections.Counter(); cy = collections.Counter()
        for i, b in enumerate(blocks):
            n = hcnt.get((b[0] << 16) + b[1], 0)
            k = '0' if n == 0 else '1' if n == 1 else '2-3' if n < 4 else '4-9' if n < 10 else '10-99' if n < 100 else '100+'
            h[k] += 1
        print('blocks by times entered at their start: ' + ' '.join('%s:%d' % (k, h[k]) for k in ('0', '1', '2-3', '4-9', '10-99', '100+')) + ' of %d' % len(blocks))
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
if wrt_hist is not None and rd_hist:
    print('s_rd: ' + ' '.join('%04X:%d' % kv for kv in sorted(rd_hist.items())))
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
if CYCLES and KIND:
    for g in 'TR':
        print('translated code' if g == 'T' else 'its routines')
        for k, v in KIND.most_common():
            if k[0] == g: print('  %5.1f%%  %s' % (100.0 * v / KIND[g + ' all'], k[2:]))
if CYCLES and st['gbframes']:
    # (not the wait for the frame's time, nor the wait for a list to be shown: HLT's cycles are not counted anyway)
    if not os.environ.get('GBRUN_KEEPWAIT'):
        for k in ('gbv_wait', 'gbv_now_lines'): prof.pop(k, None)
    tot = sum(prof.values())
    print('V30MZ cycles a Game Boy frame, from frame %d: %d (a WonderSwan frame has 40704, a Game Boy frame\'s time 51430)' % (PROF_FROM, tot // max(1, st['gbframes'] - prof_gb0[0])))
    for k, v in prof.most_common(40 if CALLS is not None else 14): print('  %5.1f%%  %s' % (100.0 * v / tot, k) + ('  %d calls, %d cycles each' % (CALLS[k], v // CALLS[k]) if CALLS is not None and CALLS[k] else ''))
if opmix:
    tot = sum(opmix.values())
    print('translated instructions: %d' % tot)
    print(' '.join('%s:%d' % ('??' if k < 0 else '%02X' % k if k < 256 else 'CB%02X' % (k & 255), v) for k, v in opmix.most_common()))
