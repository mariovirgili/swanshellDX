#!/usr/bin/env python3
"""
sfswan: the IPL that the plugin has built in, in place of the SF-7000's
own 8 KiB ROM (Sega's, which is not in these sources). Written from what
the IPL is documented to do for a disk (https://www.smspower.org/
Development/SF-7000IPL): none of its code. It is Z80 code that the
emulator runs, and it talks to the emulated uPD765 as any program would:

  0000h  start: the 8255s, the PSG silent, on a first start the VRAM and
         the RAM filled as the IPL's self test leaves them, the VDP as the
         IPL sets it (text mode, a clear screen), then track 0 sector 1
         to FF00h and, if it starts with "SYS:", a jump to FF20h (the
         disk's loader)
  0008h  RST 08h: the drive on, the controller's parameters, track 0
         (carry set: no disk)
  0010h  RST 10h: read the sector B (1-16) of track C (0-39) to DE
  0018h  RST 18h: write it from DE         (both: carry set on an error;
         AF only is changed)
  0020h  RST 20h (format a disk), 0028h (the utility program): not there,
         carry set
  0030h  the start again, never as a first one
  0038h, 0066h: a return

The long parts are not run as Z80 code in the plugin: the IPL is read
with the interpreter (the recompiler has the SF-7000's RAM only), which
on a WonderSwan is some 60 times slower than the Z80 was, and filling the
memory, loading the font and reading the 121 sectors of Disk BASIC would
take a minute and a half. So the built-in IPL asks the plugin, with a
byte to port ECh (a port of the SF-7000 with nothing on it; listened to
only while the IPL is at 0000h and only for this IPL): 1 fill the VRAM
and the RAM, 2 the font and the clear screen, 3 and 4 read and write a
sector (B, C, DE as RST 10h and 18h; the result read back from ECh). The
plugin does to the memory, the VDP and the controller what the Z80 code
would have done (core/sf7000.c: sf_service). The Z80 code is all still
here: build(fast=False) is the IPL without those calls, and the PC tools
check that the two leave the same memory behind (make sfcheck).

What it does not do: the self test, messages on the screen (a disk that
does not start leaves the screen empty: the plugin looks at the disk
before starting and says so itself), the disk's speed test (the emulated
disk is at speed at once), the copy and format program.

    python3 mksfipl.py ../core/sf_ipl.h      (the C array)
    python3 mksfipl.py out.bin               (the bytes)
"""
import sys


class Asm:
    def __init__(self):
        self.b = bytearray()
        self.labels = {}
        self.fix = []   # (offset, label, kind)

    def org(self, a):
        assert a >= len(self.b), 'org %04X behind %04X' % (a, len(self.b))
        self.b += b'\xFF' * (a - len(self.b))

    def label(self, n):
        assert n not in self.labels, n
        self.labels[n] = len(self.b)

    def db(self, *v):
        for x in v:
            if isinstance(x, (bytes, bytearray)):
                self.b += x
            else:
                self.b.append(x & 0xFF)

    def abs16(self, op, lab):   # JP, CALL, LD rr,label (op bytes then a word)
        self.db(*op) if isinstance(op, tuple) else self.db(op)
        self.fix.append((len(self.b), lab, 'w'))
        self.db(0, 0)

    def rel(self, op, lab):     # JR, DJNZ
        self.db(op)
        self.fix.append((len(self.b), lab, 'r'))
        self.db(0)

    def done(self):
        for off, lab, kind in self.fix:
            t = self.labels[lab]
            if kind == 'w':
                self.b[off] = t & 0xFF
                self.b[off + 1] = t >> 8
            else:
                d = t - (off + 1)
                assert -128 <= d <= 127, lab
                self.b[off] = d & 0xFF
        return bytes(self.b)


# The characters 20h-7Fh, for the screen the IPL leaves: Disk BASIC's text
# screen is drawn with them (it loads only its own graphics characters).
# Not the SC-3000's: the plain 5x7 dot matrix letters of terminals and LCD
# modules, five columns a character, bit 0 the top row. 5Ch and 5Fh are
# the yen sign and pi on an SC-3000: here too.
FONT_5X7 = bytes.fromhex(
    '0000000000' '00005F0000' '0007000700' '147F147F14' '242A7F2A12' '2313086462' '3649552250' '0005030000'
    '001C224100' '0041221C00' '14083E0814' '08083E0808' '0050300000' '0808080808' '0060600000' '2010080402'
    '3E5149453E' '00427F4000' '4261514946' '2141454B31' '1814127F10' '2745454539' '3C4A494930' '0171090503'
    '3649494936' '064949291E' '0036360000' '0056360000' '0814224100' '1414141414' '0041221408' '0201510906'
    '324979413E' '7E1111117E' '7F49494936' '3E41414122' '7F4141221C' '7F49494941' '7F09090901' '3E4149497A'
    '7F0808087F' '00417F4100' '2040413F01' '7F08142241' '7F40404040' '7F020C027F' '7F0408107F' '3E4141413E'
    '7F09090906' '3E4151215E' '7F09192946' '4649494931' '01017F0101' '3F4040403F' '1F2040201F' '3F4038403F'
    '6314081463' '0708700807' '6151494543' '007F414100' '15167C1615' '0041417F00' '0402010204' '4438043844'
    '0001020400' '2054545478' '7F48444438' '3844444420' '384444487F' '3854545418' '087E090102' '0C5252523E'
    '7F08040478' '00447D4000' '2040443D00' '7F10284400' '00417F4000' '7C04180478' '7C08040478' '3844444438'
    '7C14141408' '081414187C' '7C08040408' '4854545420' '043F444020' '3C4040207C' '1C2040201C' '3C4030403C'
    '4428102844' '0C5050503C' '4464544C44' '0008364100' '00007F0000' '0041360800' '1008081008' '0000000000')


def glyph_5x7(g):
    """The rows of character 20h + g of FONT_5X7: the leftmost pixel in
    bit 7 (text mode shows the left 6 of the 8)."""
    cols = FONT_5X7[g * 5:g * 5 + 5]
    return bytes(sum(0x80 >> c for c in range(5) if (cols[c] >> row) & 1) for row in range(8))


def cell(f):
    """A 6x8 character from a rule: f(x, y) for x 0-5, y 0-7."""
    return bytes(sum(0x80 >> x for x in range(6) if f(x, y)) for y in range(8))


def letter(ch):
    return glyph_5x7(ord(ch) - 0x20)


def columns(hex5):
    cols = bytes.fromhex(hex5)
    return bytes(sum(0x80 >> c for c in range(5) if (cols[c] >> row) & 1) for row in range(8))


def extended():
    """Characters 80h-FFh. An SC-3000 has line and block graphics, card
    suits, accented and Greek letters there (the list of what each one is:
    the comments of the IPL's published font). These are drawn here from
    that list, not copied: lines two pixels thick through the middle of
    the cell, blocks by halves and quarters, the accented letters as the
    plain letter, what has no simple shape left empty. Disk BASIC's
    start screen has two rows of 81h."""
    v = lambda x, y: x in (2, 3)
    h = lambda x, y: y in (3, 4)
    n, s_ = (lambda x, y: v(x, y) and y <= 4), (lambda x, y: v(x, y) and y >= 3)
    e, w = (lambda x, y: h(x, y) and x >= 2), (lambda x, y: h(x, y) and x <= 3)
    any_ = lambda *fs: (lambda x, y: any(f(x, y) for f in fs))
    blank = bytes(8)
    g = {}
    g[0x80] = cell(v); g[0x81] = cell(h)
    g[0x82] = cell(any_(n, e, w)); g[0x83] = cell(any_(e, s_, w))
    g[0x84] = cell(any_(n, s_, w)); g[0x85] = cell(any_(n, e, s_))
    for k, parts in enumerate(((e, s_), (n, e), (s_, w), (n, w))):
        g[0x86 + k] = cell(any_(*parts))
        # (the "curves": the same corner without its outer pixel)
        ox, oy = (2, 3) if k == 0 else (2, 4) if k == 1 else (3, 3) if k == 2 else (3, 4)
        g[0x8A + k] = cell(lambda x, y, p=parts, ox=ox, oy=oy: any(f(x, y) for f in p) and (x, y) != (ox, oy))
    g[0x8E] = columns('04027F0204')                      # up arrow
    g[0x8F] = columns('081C2A0808')                      # left arrow
    g[0x90] = cell(lambda x, y: (x + y) % 2 == 0)        # half shade
    g[0x91] = columns('6314081463')                      # an X
    g[0x92] = cell(any_(v, h))
    g[0x93] = cell(lambda x, y: x == 5 - (y * 6) // 8)   # /
    g[0x94] = cell(lambda x, y: x == (y * 6) // 8)       # \
    g[0x95] = cell(lambda x, y: x * 7 + y * 5 >= 35)     # the triangles: SE, SW, NE, NW
    g[0x96] = cell(lambda x, y: (5 - x) * 7 + y * 5 >= 35)
    g[0x97] = cell(lambda x, y: x * 7 + (7 - y) * 5 >= 35)
    g[0x98] = cell(lambda x, y: (5 - x) * 7 + (7 - y) * 5 >= 35)
    g[0x99] = cell(lambda x, y: y == 7); g[0x9A] = cell(lambda x, y: y >= 6)
    g[0x9B] = cell(lambda x, y: y >= 4); g[0x9C] = cell(lambda x, y: y < 4)
    g[0x9D] = cell(lambda x, y: y < 2); g[0x9E] = cell(lambda x, y: y == 0)
    g[0x9F] = cell(lambda x, y: x == 0)
    for code, ch in zip(range(0xA0, 0xC3), 'AAAAAAAAEEEEEENNIIIIIIOO OOOOUUUUUU'):
        g[code] = letter(ch)
    g[0xC3] = letter('a'); g[0xC4] = letter('B'); g[0xC5] = letter('y')   # alpha, beta, gamma
    g[0xC6] = columns('6058464860')                      # Delta
    g[0xC7] = letter('u')                                # mu
    g[0xC8] = columns('6355494141')                      # Sigma
    g[0xC9] = columns('1C227F221C')                      # phi
    g[0xCA] = columns('5E61016159')                      # Omega
    g[0xCB] = letter('C')
    # the inverted ? and !: the plain ones turned over
    for code, ch in ((0xCC, '?'), (0xCD, '!')):
        r = letter(ch)[:7][::-1] + b'\0'
        g[code] = bytes(sum(0x80 >> (4 - c) for c in range(5) if b & (0x80 >> c)) for b in r)
    g[0xE0] = cell(lambda x, y: x < 2); g[0xE1] = cell(lambda x, y: x < 3)
    g[0xE2] = cell(lambda x, y: x >= 3); g[0xE3] = cell(lambda x, y: x >= 4)
    g[0xE4] = cell(lambda x, y: x == 5); g[0xE5] = cell(lambda x, y: True)
    g[0xE6] = cell(lambda x, y: 1 <= x <= 4); g[0xE7] = cell(lambda x, y: 2 <= y <= 5)
    g[0xE8] = cell(lambda x, y: 1 <= x <= 4 and (x + y) % 2 == 0)
    g[0xE9] = cell(lambda x, y: 2 <= y <= 5 and (x + y) % 2 == 0)
    g[0xEA] = cell(lambda x, y: (x >= 3) != (y >= 4))
    g[0xEB] = columns('1C2222221C')                      # circle
    g[0xEC] = columns('1C3E3E3E1C')                      # filled circle
    g[0xF5] = columns('183C5F3C18')                      # spade
    g[0xF6] = columns('0C1E3C1E0C')                      # heart
    g[0xF7] = columns('081C3E1C08')                      # diamond
    g[0xF8] = columns('18585F5818')                      # club
    g[0xF9] = columns('3E55515D3E')                      # face
    g[0xFD] = columns('24167F1624')                      # stick man
    g[0xFE] = columns('08082A0808')                      # divide
    return b''.join(g.get(code, blank) for code in range(0x80, 0x100))


def font_rows():
    """Characters 20h-FFh, 8 bytes each, as the TMS9918A wants them."""
    return b''.join(glyph_5x7(g) for g in range(96)) + extended()


def build(fast=True):
    a = Asm()
    JP, CALL, JR, JRNZ, JRZ, JRNC, JRC, DJNZ = 0xC3, 0xCD, 0x18, 0x20, 0x28, 0x30, 0x38, 0x10

    def out(port, value=None):          # [LD A,value;] OUT (port),A
        if value is not None:
            a.db(0x3E, value)
        a.db(0xD3, port)

    def put(value=None, reg=None):      # a command byte to the uPD765
        if value == 0:
            a.db(0xAF)                  # XOR A
        elif value is not None:
            a.db(0x3E, value)
        elif reg == 'B':
            a.db(0x78)                  # LD A,B
        elif reg == 'C':
            a.db(0x79)                  # LD A,C
        a.abs16(CALL, 'fdc_put')

    a.org(0x0000); a.db(0xF3); a.abs16(JP, 'boot')
    a.org(0x0008); a.abs16(JP, 'disk_init')
    a.org(0x0010); a.abs16(JP, 'disk_read')
    a.org(0x0018); a.abs16(JP, 'disk_write')
    a.org(0x0020); a.db(0x37, 0xC9)                 # SCF; RET
    a.org(0x0028); a.db(0x37, 0xC9)
    a.org(0x0030); a.db(0xF3); a.abs16(JP, 'boot30')
    a.org(0x0038); a.db(0xC9)                       # RET
    a.org(0x0040); a.db(b'sfswan IPL 1 (not Sega\'s)')
    a.org(0x0066); a.db(0xED, 0x45)                 # RETN

    # The start. As the SF-7000's IPL: FC52h = 55h says "the self test was
    # passed", and with it there the memory is left as it is (Disk BASIC's
    # BOOT command comes here with it: Kamikaze's disk, started that way,
    # counts on the screen Disk BASIC left). Without it, a first start: no
    # test here, but what the IPL's leaves behind, which a program may have
    # come to count on without knowing: every byte of VRAM the sum of the
    # two bytes of (4000h - its address), every byte of RAM the sum of the
    # two bytes of its address.
    a.org(0x0070); a.label('boot')
    a.db(0xED, 0x56)                                # IM 1
    a.db(0x31, 0x00, 0xFF)                          # LD SP,FF00h
    for v in (0x9F, 0xBF, 0xDF, 0xFF):
        out(0x7F, v)    # the PSG silent
    out(0xDF, 0x92)     # the SC-3000's 8255: A and B in, C out
    out(0xDE, 0x07)     # the joypads' row
    out(0xE7, 0x90)     # the SF-7000's: A in, B and C out (C all 0: the IPL stays)
    out(0xE6, 0x0B)     # the uPD765 reset, the motor off, the drive not in use
    out(0xE6, 0x03)
    a.db(0x3A, 0x52, 0xFC, 0xFE, 0x55)              # LD A,(FC52h); CP 55h
    a.rel(JRZ, 'warm')
    if fast:
        out(0xEC, 0x01)                             # (the plugin fills them: the same bytes)
    else:
        a.db(0xDB, 0xBF)                                # IN A,(BFh)
        out(0xBF, 0x90); out(0xBF, 0x81)                # register 1: the display off
        a.db(0xAF, 0xD3, 0xBF)                          # XOR A; OUT (BFh),A
        out(0xBF, 0x40)                                 # (write from 0000h)
        a.db(0x11, 0x00, 0x40)                          # LD DE,4000h
        a.label('vf')
        a.db(0x7B, 0x82, 0xD3, 0xBE, 0x1B, 0x7B, 0xB2)  # LD A,E; ADD A,D; OUT (BEh),A; DEC DE; LD A,E; OR D
        a.rel(JRNZ, 'vf')
        a.db(0x21, 0x00, 0x00)                          # LD HL,0000h
        a.label('rf')
        a.db(0x7D, 0x84, 0x77, 0x23, 0x7C, 0xB5)        # LD A,L; ADD A,H; LD (HL),A; INC HL; LD A,H; OR L
        a.rel(JRNZ, 'rf')
    a.db(0x3E, 0x55, 0x32, 0x52, 0xFC)              # LD A,55h; LD (FC52h),A
    a.label('warm')
    # the screen as the IPL makes it for its messages: text mode, the
    # characters' patterns at 1800h (a font of this project's), spaces
    a.db(0xDB, 0xBF)                                # IN A,(BFh)
    a.abs16(0x21, 'vdp_regs')                       # LD HL,vdp_regs
    a.db(0x06, 8, 0x0E, 0x80)                       # LD B,8; LD C,80h
    a.label('vr')
    a.db(0x7E, 0xD3, 0xBF, 0x79, 0xD3, 0xBF, 0x23, 0x0C)   # LD A,(HL); OUT; LD A,C; OUT; INC HL; INC C
    a.rel(DJNZ, 'vr')
    if fast:
        out(0xEC, 0x02)                             # (the plugin writes them: the same bytes)
    else:
        a.db(0xAF, 0xD3, 0xBF)                          # XOR A; OUT (BFh),A
        out(0xBF, 0x58)                                 # (write from 1800h)
        a.db(0x06, 0x00)                                # LD B,0
        a.label('cl')
        a.db(0xAF, 0xD3, 0xBE)                          # XOR A; OUT (BEh),A (characters 00h-1Fh: nothing)
        a.rel(DJNZ, 'cl')
        a.abs16(0x21, 'font')                           # LD HL,font
        a.db(0x01, 0x00, 0x07)                          # LD BC,1792 (20h-FFh, from 1900h)
        a.label('fo')
        a.db(0x7E, 0xD3, 0xBE, 0x23, 0x0B, 0x78, 0xB1)  # LD A,(HL); OUT (BEh),A; INC HL; DEC BC; LD A,B; OR C
        a.rel(JRNZ, 'fo')
        a.db(0xAF, 0xD3, 0xBF)                          # XOR A; OUT (BFh),A
        out(0xBF, 0x48)                                 # (write from 0800h)
        a.db(0x01, 0xC0, 0x03)                          # LD BC,960
        a.label('sp')
        a.db(0x3E, 0x20, 0xD3, 0xBE, 0x0B, 0x78, 0xB1)  # LD A,20h; OUT (BEh),A; DEC BC; LD A,B; OR C
        a.rel(JRNZ, 'sp')
    a.abs16(CALL, 'disk_init')
    a.rel(JRC, 'fail')
    a.db(0x11, 0x00, 0xFF)                          # LD DE,FF00h
    a.db(0x01, 0x00, 0x01)                          # LD BC,0100h: sector 1, track 0
    a.abs16(CALL, 'disk_read')
    a.rel(JRC, 'fail')
    a.db(0x21, 0x00, 0xFF)                          # LD HL,FF00h
    for ch in b'SYS:':
        a.db(0x7E, 0xFE, ch)                        # LD A,(HL); CP ch
        a.rel(JRNZ, 'fail')
        a.db(0x23)                                  # INC HL
    a.db(0xC3, 0x20, 0xFF)                          # JP FF20h
    a.label('fail')
    out(0xE7, 0x03)                                 # the motor off
    a.label('stop')
    a.db(0x76)                                      # HALT (interrupts off: for good)
    a.rel(JR, 'stop')

    # RST 30h: the start with the self test taken as passed
    a.label('boot30')
    a.db(0x3E, 0x55, 0x32, 0x52, 0xFC)              # LD A,55h; LD (FC52h),A
    a.abs16(JP, 'boot')

    a.label('vdp_regs')     # the values the IPL leaves (smspower.org)
    a.db(0x00, 0xD0, 0x02, 0x00, 0x03, 0x00, 0x00, 0xF1)

    # a byte to the controller, when it asks for one (AF kept)
    a.label('fdc_put')
    a.db(0xF5)                                      # PUSH AF
    a.label('fp1')
    a.db(0xDB, 0xE0, 0x07)                          # IN A,(E0h); RLCA
    a.rel(JRNC, 'fp1')
    a.db(0xF1, 0xD3, 0xE1, 0xC9)                    # POP AF; OUT (E1h),A; RET
    # a byte from it
    a.label('fdc_get')
    a.db(0xDB, 0xE0, 0x07)
    a.rel(JRNC, 'fdc_get')
    a.db(0xDB, 0xE1, 0xC9)                          # IN A,(E1h); RET
    # the end of a seek: its interrupt, then what it says (carry: failed)
    a.label('seek_end')
    a.db(0xDB, 0xE4, 0x0F)                          # IN A,(E4h); RRCA
    a.rel(JRNC, 'seek_end')
    put(0x08)                                       # SENSE INTERRUPT STATUS
    a.abs16(CALL, 'fdc_get')
    a.db(0xF5)                                      # PUSH AF (ST0)
    a.abs16(CALL, 'fdc_get')                        # (the track)
    a.db(0xF1, 0xE6, 0xC0, 0xC8, 0x37, 0xC9)        # POP AF; AND C0h; RET Z; SCF; RET

    a.label('disk_init')
    a.db(0xC5)                                      # PUSH BC
    out(0xE7, 0x02)                                 # PC1 = 0: the motor on
    out(0xE7, 0x00)                                 # PC0 = 0: the drive in use
    put(0x03); put(0x60); put(0x0B)                 # SPECIFY (no DMA)
    put(0x07); put(0)                               # RECALIBRATE
    a.abs16(CALL, 'seek_end')
    a.db(0xC1, 0xC9)                                # POP BC; RET

    # the seek and the nine command bytes of a read (46h) or a write (45h)
    def command(op):
        put(0x0F); put(0); put(reg='C')             # SEEK
        a.abs16(CALL, 'seek_end')
        a.abs16(0xDA, 'io_fail')                    # JP C,io_fail
        put(op); put(0); put(reg='C'); put(0)
        put(reg='B'); put(0x01); put(0x10); put(0x0E); put(0xFF)

    a.label('disk_read')
    if fast:
        # (the plugin moves the sector: what the routine below does to
        # the memory, the controller and the carry, at once)
        out(0xEC, 0x03)
        a.db(0xDB, 0xEC, 0xC6, 0xFF, 0xC9)          # IN A,(ECh); ADD A,FFh (carry: not 0); RET
    a.label('disk_read_plain')
    a.db(0xC5, 0xD5, 0xE5)                          # PUSH BC; PUSH DE; PUSH HL
    command(0x46)
    a.db(0x62, 0x6B, 0x06, 0x00)                    # LD H,D; LD L,E; LD B,0
    a.label('rd1')
    a.db(0xDB, 0xE0, 0x07)                          # IN A,(E0h); RLCA
    a.rel(JRNC, 'rd1')
    a.db(0xE6, 0x40)                                # AND 40h (EXM: still transferring?)
    a.rel(JRZ, 'io_res')
    a.db(0xDB, 0xE1, 0x77, 0x23)                    # IN A,(E1h); LD (HL),A; INC HL
    a.rel(DJNZ, 'rd1')
    a.rel(JR, 'io_tc')

    a.label('disk_write')
    if fast:
        out(0xEC, 0x04)
        a.db(0xDB, 0xEC, 0xC6, 0xFF, 0xC9)
    a.label('disk_write_plain')
    a.db(0xC5, 0xD5, 0xE5)
    command(0x45)
    a.db(0x62, 0x6B, 0x06, 0x00)
    a.label('wr1')
    a.db(0xDB, 0xE0, 0x07)
    a.rel(JRNC, 'wr1')
    a.db(0xE6, 0x40)
    a.rel(JRZ, 'io_res')
    a.db(0x7E, 0xD3, 0xE1, 0x23)                    # LD A,(HL); OUT (E1h),A; INC HL
    a.rel(DJNZ, 'wr1')

    a.label('io_tc')
    out(0xE7, 0x05)                                 # PC2: TC, the end of the transfer
    out(0xE7, 0x04)
    a.label('io_res')                               # the seven result bytes
    a.abs16(CALL, 'fdc_get')
    a.db(0xE6, 0xC0, 0x57, 0x06, 0x06)              # AND C0h; LD D,A; LD B,6
    a.label('rs1')
    a.abs16(CALL, 'fdc_get')
    a.rel(DJNZ, 'rs1')
    a.db(0x7A, 0xB7)                                # LD A,D; OR A
    a.rel(JRZ, 'io_ok')
    a.label('io_fail')
    a.db(0x37)                                      # SCF
    a.label('io_ok')
    a.db(0xE1, 0xD1, 0xC1, 0xC9)                    # POP HL; POP DE; POP BC; RET
    a.label('font')
    a.db(font_rows())
    code = a.done()
    return code, a.labels['font']


if __name__ == '__main__':
    code, font = build(True)
    plain, _ = build(False)
    out = sys.argv[1]
    if out.endswith('.h'):
        def array(f, name, data):
            f.write('static const uint8_t GGFAR %s[%d] = {\n' % (name, len(data)))
            for i in range(0, len(data), 16):
                f.write('    ' + ', '.join('0x%02X' % b for b in data[i:i + 16]) + ',\n')
            f.write('};\n')
        with open(out, 'w') as f:
            f.write('// sfswan: the built-in IPL (host/mksfipl.py made this: change it there)\n')
            f.write('// where its font is (characters 20h-FFh, 8 bytes each)\n')
            f.write('#define SF_IPL_FONT 0x%04X\n' % font)
            array(f, 'sf_ipl_builtin', code)
            f.write('#ifdef SF_IPL_PLAIN\n')
            f.write('// the same with nothing done by the plugin: all of it Z80 code (the PC\n')
            f.write('// tools compare the two)\n')
            array(f, 'sf_ipl_plain', plain)
            f.write('#endif\n')
    else:
        open(out, 'wb').write(plain if len(sys.argv) > 2 and sys.argv[2] == 'plain' else code)
    print('%d bytes (all of it Z80 code: %d)' % (len(code), len(plain)))
