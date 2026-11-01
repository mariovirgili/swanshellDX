#!/usr/bin/env python3
"""swanshell menu icons: check, assemble and rotate.

  python3 icon_set.py split  <assets/menu/icons> <out_dir>
      splits the 4 strips into single PNGs (out_dir/16color/00-folder.png ...)
  python3 icon_set.py build  <single_dir> <assets/menu/icons>
      checks the single PNGs and writes the 4 strips + the 4 *_rot strips
      (same result as "make rotate-icons", without wf-lua): this replaces
      the BUILT-IN icons, which must use the fixed palette below
  python3 icon_set.py pack   <single_dir> <NAME.ICN>
      makes an icon set file for /NILESWAN/ICONS on the memory card, chosen
      in Settings > Theme > Icons. FREE PALETTE: the color icons may use any
      12 colors (12-bit: each channel is rounded to 0,17,34..255) plus
      #FFFFFF (row background) and #000000 (ink, white on the selected row)
  python3 icon_set.py preview <NAME.ICN> <preview.png>
      draws a set file as the menu shows it (normal row, selected row, mono)
"""
import sys, os
from PIL import Image

NAMES = ["folder", "file", "rom", "image", "music", "bfb", "com", "fr-il", "zip", "fx"]
SETS = {"16color": 16, "8color": 8, "16mono": 16, "8mono": 8}
# colors accepted in the PNGs (exactly these RGB values)
COLOR_OK = {
    (255, 255, 255): "transparent (row background)",
    (0, 0, 0): "ink: black, WHITE on the selected row",
    (15, 15, 15): "ink (same as black)",
    (204, 68, 51): "C43 red", (255, 153, 17): "F91 orange", (255, 255, 51): "FF3 yellow",
    (153, 255, 68): "9F4 light green", (17, 221, 102): "1D6 green", (17, 102, 153): "169 teal",
    (34, 17, 153): "219 dark blue", (34, 34, 255): "22F blue", (34, 119, 255): "27F mid blue",
    (85, 221, 255): "5DF light blue", (170, 170, 170): "AAA light grey", (85, 85, 85): "555 dark grey",
}
MONO_OK = {(255, 255, 255): "transparent", (170, 170, 170): "light", (85, 85, 85): "dark",
           (0, 0, 0): "ink", (15, 15, 15): "ink"}


def rotate(img, tw, th):
    """Same as tools/icon_rotate.lua: each tile turned (x,y) -> (th-1-y, x)."""
    src = img.convert("RGB")
    dst = Image.new("RGB", src.size, (255, 255, 255))
    for iy in range(0, src.height, th):
        for ix in range(0, src.width, tw):
            for ty in range(th):
                for tx in range(tw):
                    if ix + tx < src.width and iy + ty < src.height:
                        dst.putpixel((ix + th - 1 - ty, iy + tx), src.getpixel((ix + tx, iy + ty)))
    return dst


def split(icons, out):
    for name, n in SETS.items():
        strip = Image.open(os.path.join(icons, name + ".png")).convert("RGB")
        os.makedirs(os.path.join(out, name), exist_ok=True)
        for i, label in enumerate(NAMES):
            strip.crop((0, i * n, n, i * n + n)).save(os.path.join(out, name, "%02d-%s.png" % (i, label)))


def check(img, n, name, file):
    errs = []
    ok = MONO_OK if "mono" in name else COLOR_OK
    if img.size != (n, n):
        errs.append("size %dx%d, must be %dx%d" % (img.size + (n, n)))
        return errs
    bad = {img.getpixel((x, y)) for y in range(n) for x in range(n)} - set(ok)
    if bad:
        errs.append("colors not allowed: " + ", ".join("#%02X%02X%02X" % c for c in sorted(bad)))
    if n == 16 and any(img.getpixel((15, y)) != (255, 255, 255) for y in range(16)):
        errs.append("last column (x=15) must be all white: it separates the icon from the text")
    return ["%s: %s" % (file, e) for e in errs]


def build(single, icons):
    errors = []
    for name, n in SETS.items():
        strip = Image.new("RGB", (n, n * len(NAMES)), (255, 255, 255))
        for i, label in enumerate(NAMES):
            f = os.path.join(single, name, "%02d-%s.png" % (i, label))
            img = Image.open(f).convert("RGB")
            errors += check(img, n, name, f)
            strip.paste(img, (0, i * n))
        if errors:
            continue
        # indexed PNG with the original palette order is not required:
        # superfamiconv matches the colors against the menu palette
        strip.save(os.path.join(icons, name + ".png"))
        rotate(strip, n, n).save(os.path.join(icons, name + "_rot.png"))
    if errors:
        print("\n".join(errors))
        sys.exit(1)
    print("ok: strips and *_rot strips written to", icons)


# ---- .ICN set files (see src/menu/ui/ui_icons.h) ----

ICN_SIZE = 4832


def c4(c):
    if c == (15, 15, 15):   # the original icons' ink
        return (0, 0, 0)
    return tuple(int(round(v / 17)) for v in c)


def mono_index(c):
    """white -> 2 (background), light -> 0, dark -> 1, black -> 3 (ink),
    as the menu's mono icon palette {2, 5, 0, 7}"""
    v = int(c[0] * 0.299 + c[1] * 0.587 + c[2] * 0.114) >> 5
    return min(((7, 2), (5, 0), (2, 1), (0, 3)), key=lambda t: abs(t[0] - v))[1]


def tile_bytes(idx, bpp):
    out = bytearray()
    for y in range(8):
        for plane in range(bpp):
            b = 0
            for x in range(8):
                b |= ((idx[y * 8 + x] >> plane) & 1) << (7 - x)
            out.append(b)
    return bytes(out)


def strip_bytes(strip, n, bpp, index_of):
    """10 icons; 16x16 icons as tiles top-left, bottom-left, top-right, bottom-right"""
    out = bytearray()
    order = [(0, 0), (0, 8), (8, 0), (8, 8)] if n == 16 else [(0, 0)]
    for i in range(len(NAMES)):
        for tx, ty in order:
            idx = [index_of(strip.getpixel((tx + x, i * n + ty + y))) for y in range(8) for x in range(8)]
            out += tile_bytes(idx, bpp)
    return bytes(out)


def load_strip(single, name, n):
    strip = Image.new("RGB", (n, n * len(NAMES)), (255, 255, 255))
    errors = []
    for i, label in enumerate(NAMES):
        f = os.path.join(single, name, "%02d-%s.png" % (i, label))
        img = Image.open(f).convert("RGB")
        if img.size != (n, n):
            errors.append("%s: size %dx%d, must be %dx%d" % ((f,) + img.size + (n, n)))
            continue
        if n == 16 and any(c4(img.getpixel((15, y))) != (15, 15, 15) for y in range(16)):
            errors.append(f + ": last column (x=15) must be all white")
        strip.paste(img, (0, i * n))
    return strip, errors


def pack(single, out):
    strips, errors = {}, []
    for name, n in SETS.items():
        strips[name], e = load_strip(single, name, n)
        errors += e
    # the free palette: every color of the color icons but white and black
    count = {}
    for name in ("16color", "8color"):
        st = strips[name]
        for y in range(st.height):
            for x in range(st.width):
                c = c4(st.getpixel((x, y)))
                if c not in ((15, 15, 15), (0, 0, 0)):
                    count[c] = count.get(c, 0) + 1
    if len(count) > 12:
        errors.append("the color icons use %d colors besides white and black (12-bit values): at most 12. Colors: %s"
                      % (len(count), " ".join("#%X%X%X" % c for c in sorted(count, key=lambda c: -count[c]))))
    if errors:
        print("\n".join(errors))
        sys.exit(1)
    pal = sorted(count, key=lambda c: -count[c])
    index = {c: 4 + i for i, c in enumerate(pal)}
    index[(15, 15, 15)] = 2
    index[(0, 0, 0)] = 3
    col = lambda c: index[c4(c)]
    data = bytearray(b"WSIC\x01\x00\x00\x00")
    for i in range(12):
        r, g, b = pal[i] if i < len(pal) else (0, 0, 0)
        data += bytes((((g << 4) | b), r))
    for name, n in (("16color", 16), ("8color", 8)):
        data += strip_bytes(strips[name], n, 4, col)
        data += strip_bytes(rotate(strips[name], n, n), n, 4, col)
    for name, n in (("16mono", 16), ("8mono", 8)):
        data += strip_bytes(strips[name], n, 2, mono_index)
        data += strip_bytes(rotate(strips[name], n, n), n, 2, mono_index)
    assert len(data) == ICN_SIZE, len(data)
    open(out, "wb").write(data)
    print("ok: %s (%d colors: %s)" % (out, len(pal), " ".join("#%X%X%X" % c for c in pal)))


def decode_tiles(data, off, n, bpp, i):
    """pixels (n x n color indexes) of icon i of a strip"""
    tsize = 8 * bpp
    order = [(0, 0), (0, 8), (8, 0), (8, 8)] if n == 16 else [(0, 0)]
    px = [[0] * n for _ in range(n)]
    base = off + i * len(order) * tsize
    for k, (tx, ty) in enumerate(order):
        t = data[base + k * tsize: base + (k + 1) * tsize]
        for y in range(8):
            for x in range(8):
                px[ty + y][tx + x] = sum(((t[y * bpp + p] >> (7 - x)) & 1) << p for p in range(bpp))
    return px


def preview(icn, out, scale=4):
    data = open(icn, "rb").read()
    if len(data) != ICN_SIZE or data[:4] != b"WSIC":
        print("not a valid .ICN file"); sys.exit(1)
    pal = [((data[8 + i * 2 + 1] & 15) * 17, (data[8 + i * 2] >> 4) * 17, (data[8 + i * 2] & 15) * 17) for i in range(12)]
    normal = {2: (255, 255, 255), 3: (0, 0, 0), 1: (0, 0, 0), 0: (255, 0, 255)}
    selected = {2: (0, 0, 0), 3: (255, 255, 255), 1: (255, 255, 255), 0: (255, 0, 255)}
    for i, c in enumerate(pal):
        normal[4 + i] = selected[4 + i] = c
    lut = [0, 3, 5, 7, 9, 11, 13, 15]
    shade = lambda slot: tuple([255 - lut[slot] * 17] * 3)
    mono = {0: shade(2), 1: shade(5), 2: shade(0), 3: shade(7)}
    rows = [("16 color", 32, 16, 4, normal), ("16 color, selected row", 32, 16, 4, selected),
            ("8 color", 32 + 2560, 8, 4, normal), ("16 mono", 32 + 2560 + 640, 16, 2, mono),
            ("8 mono", 32 + 2560 + 640 + 1280, 8, 2, mono)]
    W = 10 * 20 * scale + 10
    img = Image.new("RGB", (W, len(rows) * (20 * scale + 6) + 6), (200, 200, 200))
    for r, (label, off, n, bpp, colors) in enumerate(rows):
        y0 = 6 + r * (20 * scale + 6)
        for i in range(10):
            px = decode_tiles(data, off, n, bpp, i)
            x0 = 5 + i * 20 * scale
            for y in range(n):
                for x in range(n):
                    c = colors[px[y][x]]
                    for a in range(scale):
                        for b in range(scale):
                            img.putpixel((x0 + x * scale + a, y0 + y * scale + b), c)
    img.save(out)
    print("ok:", out)


if __name__ == "__main__":
    cmds = {"split": split, "build": build, "pack": pack, "preview": preview, "anteprima": preview}
    if len(sys.argv) != 4 or sys.argv[1] not in cmds:
        print(__doc__)
        sys.exit(1)
    cmds[sys.argv[1]](sys.argv[2], sys.argv[3])
