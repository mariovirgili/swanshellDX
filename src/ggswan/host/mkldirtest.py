#!/usr/bin/env python3
"""A tiny test ROM for LDIR and LDDR to somewhere outside the work RAM (drc.c:
the LDI routine leaves them to the interpreter): to ROM (ignored), to the
cartridge RAM, and from the end of the work RAM on into its mirror (the first
bytes in place, the rest by the interpreter). After each one it stores BC, DE
and HL, and the work RAM must be as the interpreter leaves it: up to patch
143 the block went back to the instruction with BC = 0, and 64 KiB more were
copied over everything.
  python3 mkldirtest.py ldir.gg && python3 drcsys.py ./libdrcsys.so ldir.gg 30 /tmp/ldir "" 0 --check
(the same ROM for libdrcsys_sms.so)"""
import sys
code = bytearray()
def b(*x): code.extend(x)
def w(v): return (v & 0xFF, (v >> 8) & 0xFF)
b(0xF3, 0xED, 0x56, 0x31, *w(0xDFF0), 0xC3, *w(0x0100))        # DI ; IM 1 ; LD SP,DFF0 ; JP 0100
code.extend(b'\x00' * (0x38 - len(code)))
b(0xF5, 0xDB, 0xBF, 0xF1, 0xFB, 0xED, 0x4D)                    # PUSH AF ; IN A,(BF) ; POP AF ; EI ; RETI
code.extend(b'\x00' * (0x100 - len(code)))
for r, v in ((0, 0x06), (1, 0xE0), (2, 0xFF), (5, 0xFF), (6, 0xFB), (10, 0xFF)):
    b(0x3E, v, 0xD3, 0xBF, 0x3E, 0x80 | r, 0xD3, 0xBF)         # the VDP registers: display and frame interrupt on
# a pattern in the work RAM: C000h-C0FFh = 00h-FFh
b(0x21, *w(0xC000), 0xAF)                                      # LD HL,C000 ; XOR A
b(0x77, 0x23, 0x3C, 0x20, 0xFB)                                # LD (HL),A ; INC HL ; INC A ; JR NZ
out = [0xC100]
def store():
    a = out[0]
    b(0xED, 0x43, *w(a), 0xED, 0x53, *w(a + 2), 0x22, *w(a + 4))   # LD (a),BC ; LD (a+2),DE ; LD (a+4),HL
    out[0] += 6
def ldir(hl, de, bc, down=0):
    b(0x21, *w(hl), 0x11, *w(de), 0x01, *w(bc), 0xED, 0xB8 if down else 0xB0)
    store()
ldir(0xC000, 0x8000, 4)             # to ROM: nothing written
ldir(0xC010, 0x0100, 0x20)          # to ROM, more than a few
ldir(0xC03F, 0x9000, 0x10, 1)       # LDDR to ROM
b(0x3E, 0x08, 0x32, *w(0xFFFC))     # LD A,08 ; LD (FFFC),A: the cartridge RAM at 8000h
ldir(0xC000, 0x8000, 0x40)          # to the cartridge RAM
ldir(0x8000, 0xC200, 0x40)          # and back: in place (work RAM)
ldir(0xC0FF, 0x8100, 0x30, 1)       # LDDR to the cartridge RAM
ldir(0x80D1, 0xC280, 0x30)          # and back
b(0xAF, 0x32, *w(0xFFFC))           # XOR A ; LD (FFFC),A: ROM again
ldir(0xC000, 0xDFF0, 0x20)          # from the end of the work RAM into its mirror (E000h on)
ldir(0xC000, 0xC300, 1)             # one byte, in place
loop = len(code)
b(0x2A, *w(0xC1F0), 0x23, 0x22, *w(0xC1F0))                    # a counter of the turns
b(0xFB, 0x76, 0x18, (loop - (len(code) + 4)) & 0xFF)           # EI ; HALT ; JR loop
open(sys.argv[1], 'wb').write(bytes(code) + b'\x00' * (0x8000 - len(code)))
print(len(code), "bytes of code")
