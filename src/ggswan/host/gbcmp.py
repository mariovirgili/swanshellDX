#!/usr/bin/env python3
"""The plugin's picture next to the PC's: runs the WS build in gbrun.py
(fast enough there to show every Game Boy frame) and the same machine on
the PC (gb/host/gbftest) for as many Game Boy frames, and counts the
pixels that differ in the 160 x 144. With no keys and a still screen the
two should be the same picture; a moving one is compared by eye
(out-prefix.png has the two side by side).
usage: gbcmp.py build/gbswan.elf rom.gb ws-frames out-prefix"""
import sys, os, subprocess, re
from PIL import Image
ELF, ROM, FRAMES, OUT = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
HERE = os.path.dirname(os.path.abspath(__file__))
env = dict(os.environ, GBRUN_LINE='8000')
p = subprocess.run([sys.executable, os.path.join(HERE, 'gbrun.py'), ELF, ROM, str(FRAMES), OUT], capture_output=True, text=True, env=env)
m = re.search(r'Game Boy frames (\d+)\s*$', p.stdout)
if not m: sys.exit(p.stdout[-300:] + p.stderr[-300:])
n = int(m.group(1))
ref = OUT + '-ref.pgm'
subprocess.run([os.path.join(HERE, '../gb/host/gbftest'), ROM, '-q', '-n', '-f', str(n), '-o', ref], capture_output=True)
a = Image.open('%s-%04d.ppm' % (OUT, FRAMES - 1)).convert('L'); b = Image.open(ref)
c = a.crop((32, 0, 192, 144))
d = sum(1 for x, y in zip(c.tobytes(), b.tobytes()) if x != y)
o = Image.new('L', (224 + 160 + 8, 144), 128); o.paste(a, (0, 0)); o.paste(b, (232, 0)); o.save(OUT + '.png')
print('%s: %d Game Boy frames, %d pixels differ' % (os.path.basename(ROM)[:28], n, d))
