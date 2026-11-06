# A tiny hand-assembled Game Gear test ROM: palette, two tiles, a checkered
# name table, one sprite, display on, then an IM 1 loop that scrolls.
import sys
code = bytearray()
def b(*x): code.extend(x)
def out(port, v): b(0x3E, v, 0xD3, port)            # LD A,v ; OUT (port),A
def vdp_reg(r, v): out(0xBF, v); out(0xBF, 0x80 | r)
def vdp_addr(a, code_bits): out(0xBF, a & 0xFF); out(0xBF, ((a >> 8) & 0x3F) | (code_bits << 6))
# $0000: DI ; IM 1 ; LD SP,$DFF0 ; JP $0100
b(0xF3, 0xED, 0x56, 0x31, 0xF0, 0xDF, 0xC3, 0x00, 0x01)
code.extend(b'\x00' * (0x38 - len(code)))
# $0038: interrupt: read status, scroll hscroll+1 (kept in $C000), EI, RETI
b(0xF5, 0xDB, 0xBF,                     # PUSH AF ; IN A,($BF)
  0x3A, 0x00, 0xC0, 0x3C, 0x32, 0x00, 0xC0,  # LD A,($C000) ; INC A ; LD ($C000),A
  0xD3, 0xBF, 0x3E, 0x88, 0xD3, 0xBF,   # OUT ($BF),A ; LD A,$88 ; OUT ($BF),A  (reg 8)
  0xF1, 0xFB, 0xED, 0x4D)               # POP AF ; EI ; RETI
code.extend(b'\x00' * (0x100 - len(code)))
# $0100: setup
vdp_reg(0, 0x06); vdp_reg(1, 0x80); vdp_reg(2, 0xFF); vdp_reg(5, 0xFF); vdp_reg(6, 0xFB); vdp_reg(7, 0x00); vdp_reg(10, 0xFF)
# palette: bg color 0 dark blue, 1 yellow, 2 red; sprite color 1 white
vdp_addr(0x00, 3)
for word in (0x400, 0x0FF, 0x00F) + (0,) * 13 + (0, 0xFFF) + (0,) * 14:
    out(0xBE, word & 0xFF); out(0xBE, word >> 8)
# tiles 1 (color 1 solid) and 2 (color 2 diagonal) at VRAM $0020
vdp_addr(0x20, 1)
for row in range(8): out(0xBE, 0xFF); out(0xBE, 0); out(0xBE, 0); out(0xBE, 0)
for row in range(8): out(0xBE, 0); out(0xBE, 0x80 >> row); out(0xBE, 0); out(0xBE, 0)
# name table at $3800: checkerboard of tiles 1/2 (32x28) via a loop
vdp_addr(0x3800, 1)
b(0x16, 28)                             # LD D,28
rowloop = len(code)
b(0x1E, 16)                             # LD E,16
colloop = len(code)
b(0x3E, 1, 0xD3, 0xBE, 0xAF, 0xD3, 0xBE, 0x3E, 2, 0xD3, 0xBE, 0xAF, 0xD3, 0xBE)
b(0x1D); b(0x20, (colloop - (len(code) + 2)) & 0xFF)   # DEC E ; JR NZ
b(0x15); b(0x20, (rowloop - (len(code) + 2)) & 0xFF)   # DEC D ; JR NZ
# sprite 0 at (100, 80) tile 1, then terminator
vdp_addr(0x3F00, 1); out(0xBE, 79); out(0xBE, 0xD0)
vdp_addr(0x3F80, 1); out(0xBE, 100); out(0xBE, 1)
vdp_reg(1, 0xE0)                        # display on, frame interrupt on
b(0xFB)                                 # EI
loop = len(code)
b(0x76); b(0x18, (loop - (len(code) + 2)) & 0xFF)       # HALT ; JR loop
rom = bytes(code) + b'\x00' * (0x8000 - len(code))
# TMR SEGA header at $7FF0 (not needed by our emulator)
open(sys.argv[1], 'wb').write(rom)
print(len(code), "bytes of code")
