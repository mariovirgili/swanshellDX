#!/usr/bin/env python3
"""A hand-assembled SG-1000 test ROM for sgtest (and, later, the plugin):
one screen in the mode asked for, every character of it different, and
sprites placed to try the corners of the TMS9918A:
  0     moves one pixel to the right a frame (the interrupt writes its X)
  1     in its way: the overlap flag comes and goes
  2-4   with 0 on the same lines: where 1 is there too, 4 is the fifth
  5     half above the top, 6 half out on the left (early clock), 7 half
        out on the right, 8 half below the bottom
  9, 10 (g2 only) a sprite of color 0 over another: nothing to see, but
        the overlap flag is always set
  then the end of the table (D0h) and one more that must not be seen
The interrupt stores the status it reads at C000h and counts the frames at
C001h; the Pause button (the non-maskable interrupt) counts at C002h. The
work RAM is tried at its mirror: C004h-C006h must read 5A A5 3C.
usage: mksgtest.py out.sg [g1|g2|mc|text]
  g1: Graphics I, 16x16 sprites      g2: Graphics II, 16x16 sprites
  mc: multicolor, magnified 8x8 sprites      text: text (no sprites)"""
import sys

OUT = sys.argv[1]
MODE = sys.argv[2] if len(sys.argv) > 2 else 'g1'
NT, CT, PG, SAT, SPG = 0x1800, 0x2000, 0x0000, 0x1B00, 0x3800
REGS = {
    'g1':   [0x00, 0xE2, 0x06, 0x80, 0x00, 0x36, 0x07, 0x04],
    'g2':   [0x02, 0xE2, 0x06, 0xFF, 0x03, 0x36, 0x07, 0x01],
    'mc':   [0x00, 0xE9, 0x06, 0x80, 0x00, 0x36, 0x07, 0x04],
    'text': [0x00, 0xF0, 0x06, 0x80, 0x00, 0x36, 0x07, 0xF4],
}[MODE]

def pattern(n, t=0):
    rows = []
    for r in range(8):
        v = 0x80 >> ((n + r + t) & 7)
        if r in (3, 4):
            v |= n
        if r == 7:
            v |= 0xFF if not t else 0xAA >> (t - 1)
        rows.append(v & 0xFF)
    return rows

blocks = []   # (VRAM address, bytes)
if MODE == 'g2':
    pg, ct = [], []
    for t in range(3):
        for n in range(256):
            pg += pattern(n, t)
            ct += [((((n + r) % 15) + 1) << 4) | ((n * 3 + t + r // 4) % 16) for r in range(8)]
    blocks += [(PG, pg), (CT, ct)]
    blocks.append((NT, [(i & 0xFF) for i in range(768)]))
elif MODE == 'mc':
    pg = []
    for n in range(256):
        pg += [(((n + r) % 16) << 4) | ((n * 5 + r * 3) % 16) for r in range(8)]
    blocks += [(PG, pg), (NT, [(i * 3) & 0xFF for i in range(768)])]
elif MODE == 'text':
    pg = []
    for n in range(256):
        pg += pattern(n)
    blocks += [(PG, pg), (NT, [(i & 0xFF) for i in range(960)])]
else:
    pg = []
    for n in range(256):
        pg += pattern(n)
    ct = [((i % 15 + 1) << 4) | ((i * 7) % 16) for i in range(32)]
    blocks += [(PG, pg), (CT, ct), (NT, [(i & 0xFF) for i in range(768)])]

if MODE != 'text':
    box = [0xFF, 0xFF, 0xC0, 0xC0, 0xC3, 0xC3, 0xC3, 0xC3] + [0xC3, 0xC3, 0xC3, 0xC3, 0xC0, 0xC0, 0xFF, 0xFF] \
        + [0xFF, 0xFF, 0x03, 0x03, 0xC3, 0xC3, 0xC3, 0xC3] + [0xC3, 0xC3, 0xC3, 0xC3, 0x03, 0x03, 0xFF, 0xFF]
    diag = [0x80 >> r for r in range(8)] + [0] * 8 + [0] * 8 + [0x80 >> r for r in range(8)]
    for k in range(8):
        diag[k] |= 0x01; diag[24 + k] |= 0x80
    blocks.append((SPG, box + diag))
    sat = [
        (40, 0, 0, 15), (44, 100, 4, 8), (40, 140, 0, 2), (40, 170, 0, 5), (40, 200, 0, 9),
        (0xF8, 20, 4, 11), (100, 10, 0, 6 | 0x80), (100, 250, 0, 7), (185, 60, 4, 3),
    ]
    if MODE == 'g2':
        sat += [(120, 120, 0, 0), (124, 124, 4, 13)]
    sat += [(0xD0, 0, 0, 0), (50, 50, 0, 15)]
    blocks.append((SAT, [b for s in sat for b in s]))

code = bytearray()
def b(*x): code.extend(x)
def w(v): return (v & 0xFF, (v >> 8) & 0xFF)
def org(a):
    assert len(code) <= a, hex(len(code))
    code.extend(b'\x00' * (a - len(code)))
def vdp_reg(r, v): b(0x3E, v, 0xD3, 0xBF, 0x3E, 0x80 | r, 0xD3, 0xBF)
def vdp_addr(a): b(0x3E, a & 0xFF, 0xD3, 0xBF, 0x3E, ((a >> 8) & 0x3F) | 0x40, 0xD3, 0xBF)

b(0xF3, 0xED, 0x56, 0x31, *w(0xC400), 0xC3, *w(0x0100))        # DI ; IM 1 ; LD SP,C400 ; JP 0100
org(0x38)
b(0xF5, 0xDB, 0xBF, 0x32, *w(0xC000))                          # PUSH AF ; IN A,(BF) ; LD (C000),A
b(0x3A, *w(0xC001), 0x3C, 0x32, *w(0xC001))                    # LD A,(C001) ; INC A ; LD (C001),A
if MODE != 'text':
    vdp_addr(SAT + 1)
    b(0x3A, *w(0xC001), 0xD3, 0xBE)                            # LD A,(C001) ; OUT (BE),A
b(0xF1, 0xFB, 0xED, 0x4D)                                      # POP AF ; EI ; RETI
org(0x66)
b(0xF5, 0x3A, *w(0xC002), 0x3C, 0x32, *w(0xC002), 0xF1, 0xED, 0x45)   # the Pause button ; RETN
org(0x100)
for r, v in enumerate(REGS):
    vdp_reg(r, v & ~0x60 if r == 1 else v)                     # (display and interrupt off for now)
# the whole VRAM to 0
vdp_addr(0)
b(0x01, *w(0x4000))                                            # LD BC,4000
b(0xAF, 0xD3, 0xBE, 0x0B, 0x78, 0xB1, 0x20, 0xF8)              # XOR A ; OUT (BE),A ; DEC BC ; LD A,B ; OR C ; JR NZ
fix = []
for addr, data in blocks:
    vdp_addr(addr)
    b(0x21); fix.append((len(code), data)); b(0, 0)            # LD HL,data
    b(0x01, *w(len(data)))                                     # LD BC,length
    b(0x7E, 0xD3, 0xBE, 0x23, 0x0B, 0x78, 0xB1, 0x20, 0xF7)    # LD A,(HL) ; OUT (BE),A ; INC HL ; DEC BC ; LD A,B ; OR C ; JR NZ
# the work RAM: 8 KiB, there again at E000h (the Mark III's: see SG1000.md)
b(0x3E, 0x5A, 0x32, *w(0xDFFF))                                # LD A,5A ; LD (DFFF),A
b(0x3A, *w(0xFFFF), 0x32, *w(0xC004))                          # LD A,(FFFF) ; LD (C004),A
b(0x3E, 0xA5, 0x32, *w(0xE010))                                # LD A,A5 ; LD (E010),A
b(0x3A, *w(0xC010), 0x32, *w(0xC005))                          # LD A,(C010) ; LD (C005),A
b(0x3E, 0x3C, 0x32, *w(0xFFFC))                                # LD A,3C ; LD (FFFC),A (no mapper there)
b(0x3A, *w(0xDFFC), 0x32, *w(0xC006))                          # LD A,(DFFC) ; LD (C006),A
vdp_reg(1, REGS[1])
b(0xDB, 0xBF, 0xFB)                                            # IN A,(BF) ; EI
loop = len(code)
b(0x76, 0x18, (loop - (len(code) + 3)) & 0xFF)                 # HALT ; JR loop
for at, data in fix:
    code[at:at + 2] = bytes(w(len(code)))
    code.extend(bytes(data))
assert len(code) <= 0xC000
size = 0x4000 if len(code) <= 0x4000 else 0x8000 if len(code) <= 0x8000 else 0xC000
open(OUT, 'wb').write(bytes(code) + b'\xFF' * (size - len(code)))
print('%s: %s, %d bytes of code and data in %d' % (OUT, MODE, len(code), size))
