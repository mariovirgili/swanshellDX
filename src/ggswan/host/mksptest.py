# A tiny test ROM for the stack outside the work RAM (drc.c sp_loaded_exit,
# drc_rt.c SP_OK): every frame it reads a table in ROM with POPs (LD SP,nn),
# pushes with the stack in the RAM's mirror (LD SP,HL), then with LD SP,IX
# inside the work RAM, each time back through LD SP,(nn), and stores what
# it got at C000h on. Run it with drcsys.py (recompiler against the
# interpreter): the RAM must be the same.
#   python3 mksptest.py sp.gg && python3 drcsys.py ./libdrcsys.so sp.gg 60 /tmp/sp "" 0
import sys
code = bytearray()
def b(*x): code.extend(x)
def w(v): return (v & 0xFF, v >> 8)
b(0xF3, 0xED, 0x56, 0x31, 0xF0, 0xDF, 0xC3, 0x00, 0x01)      # DI ; IM 1 ; LD SP,DFF0 ; JP 0100
code.extend(b'\x00' * (0x38 - len(code)))
b(0xF5, 0xDB, 0xBF, 0xF1, 0xFB, 0xED, 0x4D)                  # PUSH AF ; IN A,(BF) ; POP AF ; EI ; RETI
code.extend(b'\x00' * (0x80 - len(code)))
TABLE = len(code)
b(0x11, 0x11, 0x22, 0x22, 0x33, 0x33)                        # the table: 1111h, 2222h, 3333h
code.extend(b'\x00' * (0x100 - len(code)))
for r, v in ((0, 0x06), (1, 0xE0), (2, 0xFF), (5, 0xFF), (6, 0xFB), (10, 0xFF)):
    b(0x3E, v, 0xD3, 0xBF, 0x3E, 0x80 | r, 0xD3, 0xBF)       # the VDP registers: display and frame interrupt on
loop = len(code)
b(0xF3)                                                      # DI
b(0xED, 0x73, *w(0xC100))                                    # LD (C100),SP
b(0x31, *w(TABLE))                                           # LD SP,table (ROM)
b(0xE1, 0xD1, 0xC1)                                          # POP HL ; POP DE ; POP BC
b(0xED, 0x7B, *w(0xC100))                                    # LD SP,(C100)
b(0x22, *w(0xC000), 0xED, 0x53, *w(0xC002), 0xED, 0x43, *w(0xC004))
b(0x21, *w(0xF000), 0xF9)                                    # LD HL,F000 ; LD SP,HL (the mirror)
b(0x01, *w(0x1234), 0xC5, 0xD1)                              # LD BC,1234 ; PUSH BC ; POP DE
b(0xED, 0x7B, *w(0xC100))                                    # LD SP,(C100)
b(0xED, 0x53, *w(0xC006))                                    # LD (C006),DE
b(0x2A, *w(0xCFFE), 0x22, *w(0xC008))                        # LD HL,(CFFE) ; LD (C008),HL (what the push wrote)
b(0xDD, 0x21, *w(0xC800), 0xDD, 0xF9)                        # LD IX,C800 ; LD SP,IX (in the work RAM)
b(0x01, *w(0x5678), 0xC5)                                    # LD BC,5678 ; PUSH BC
b(0xED, 0x7B, *w(0xC100))                                    # LD SP,(C100)
b(0x2A, *w(0xC7FE), 0x22, *w(0xC00A))                        # LD HL,(C7FE) ; LD (C00A),HL
b(0x2A, *w(0xC00C), 0x23, 0x22, *w(0xC00C))                  # a counter of the turns
b(0xFB, 0x76)                                                # EI ; HALT
b(0x18, (loop - (len(code) + 2)) & 0xFF)                     # JR loop
rom = bytes(code) + b'\x00' * (0x8000 - len(code))
open(sys.argv[1], 'wb').write(rom)
print(len(code), "bytes of code")
