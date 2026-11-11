#!/usr/bin/env python3
"""The WonderSwan Color screen (224x144, 4 bits a pixel planar tiles) from a
snapshot of the internal RAM and the ports written by wsrun.py with
WSRUN_SHOT=prefix: screen 1, the sprites, screen 2 in its window, the
backdrop. To look at what the device build of a plugin draws (wsrun.py has
no picture of its own), next to the PC tools' pictures.
usage: wsshot.py shot.wsd out.ppm"""
import sys
d = open(sys.argv[1], 'rb').read()
iram, ports = d[:0x10000], d[0x10000:0x10100]
pal = [int.from_bytes(iram[0xFE00 + i * 2:0xFE02 + i * 2], 'little') for i in range(256)]
def tile_px(t, r, c):
    p = 0x4000 + t * 32 + r * 4
    b = 7 - c
    return (iram[p] >> b & 1) | (iram[p + 1] >> b & 1) << 1 | (iram[p + 2] >> b & 1) << 2 | (iram[p + 3] >> b & 1) << 3
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
    return pal[((e >> 9) & 15) * 16 + px] if px else None
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
                if s[1] & 0x80: dy = 7 - dy
                if s[1] & 0x40: dx = 7 - dx
                px = tile_px(s[0] | (s[1] & 1) << 8, dy, dx)
                if not px: continue
                cc = pal[(8 + ((s[1] >> 1) & 7)) * 16 + px]
                if s[1] & 0x20: hi = cc
                else: lo = cc
        if lo is not None: c = lo
        if ctrl & 2:
            inside = ports[8] <= x <= ports[0xA] and ports[9] <= y <= ports[0xB]
            if not (ctrl & 0x20) or (inside != bool(ctrl & 0x10)):
                v = screen(1, x, y)
                if v is not None: c = v
        if hi is not None: c = hi
        out += bytes((((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17))
open(sys.argv[2], 'wb').write(b'P6\n224 144\n255\n' + bytes(out))
