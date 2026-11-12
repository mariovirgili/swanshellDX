#!/usr/bin/env python3
"""Runs gbtest over the test ROMs that apply to the DMG and prints what
passed. The ROMs are not in the repository: the directory given is the
unpacked game-boy-test-roms (github.com/c-sp/game-boy-test-roms, v7.0),
which has Blargg's, Mooneye's and dmg-acid2.
usage: gbsuite.py roms-dir [filter] [-v] [--fast]
--fast: the plugin's machine (gbftest) in place of the reference.
Mooneye's tests name the machines they are for after a dash (dmgABC, G,
S, C, A, mgb, sgb...): only those with no name or with the DMG's are run.
Tests that end with a picture are compared with a reference picture
(dmg-acid2) or with the hash written here once the picture was looked at."""
import sys, os, subprocess, re

D = sys.argv[1]
FILT = [a for a in sys.argv[2:] if not a.startswith('-')]
V = '-v' in sys.argv
HERE = os.path.dirname(os.path.abspath(__file__))
GBTEST = os.path.join(HERE, 'gbftest' if '--fast' in sys.argv else 'gbtest')

def dmg(name):
    m = re.search(r'-([A-Za-z0-9]+)$', name)
    if not m: return True
    s = m.group(1)
    if s in ('GS', 'G', 'dmgABC', 'dmgABCmgb', 'dmgABCmgbS'): return True
    if re.fullmatch(r'[0-9]+', s) or s in ('ok', 'timing', 'bgp', 'obp0', 'obp1'): return True
    return re.fullmatch(r'[a-z0-9_]+', s) is not None and s not in ('dmg0', 'mgb', 'sgb', 'sgb2', 'cgb', 'cgb0', 'cgbABCDE', 'agb')

# pictures looked at and found right: name -> hash
SEEN = {
    'blargg/halt_bug.gb': '1A4E5E56',
}
groups = []
def add(title, sub, flags, frames=600, only=None):
    p = os.path.join(D, sub)
    if not os.path.isdir(p): return
    roms = []
    for root, _, files in sorted(os.walk(p)):
        for f in sorted(files):
            if not f.endswith('.gb'): continue
            rel = os.path.relpath(os.path.join(root, f), D)
            if only and not only(rel): continue
            roms.append(rel)
    groups.append((title, roms, flags, frames))
add('Blargg cpu_instrs', 'blargg/cpu_instrs', [], 4000, lambda r: 'individual' not in r)
add('Blargg instr_timing', 'blargg/instr_timing', [])
add('Blargg mem_timing', 'blargg/mem_timing', [], 600, lambda r: 'individual' not in r)
add('Blargg mem_timing-2', 'blargg/mem_timing-2', [], 600, lambda r: 'rom_singles' not in r)
add('Blargg halt_bug', 'blargg', [], 300, lambda r: r == 'blargg/halt_bug.gb')
for sub in ('bits', 'instr', 'interrupts', 'oam_dma', 'ppu', 'serial', 'timer', ''):
    def only(r, sub=sub):
        n = os.path.splitext(os.path.basename(r))[0]
        if not dmg(n): return False
        return sub != '' or os.path.dirname(r).endswith('acceptance')
    add('Mooneye acceptance' + ('/' + sub if sub else ''), 'mooneye-test-suite/acceptance' + ('/' + sub if sub else ''), ['-m'], 600, only)
add('Mooneye emulator-only/mbc1', 'mooneye-test-suite/emulator-only/mbc1', ['-m'], 600)
add('Mooneye emulator-only/mbc2', 'mooneye-test-suite/emulator-only/mbc2', ['-m'], 600)
add('Mooneye emulator-only/mbc5', 'mooneye-test-suite/emulator-only/mbc5', ['-m'], 600)
def wp(r):
    return dmg(os.path.splitext(os.path.basename(r))[0])
add('Mooneye (wilbertpol) gpu', 'mooneye-test-suite-wilbertpol/acceptance/gpu', ['-s', '-n'], 240, wp)
add('dmg-acid2', 'dmg-acid2', ['-n'], 60)

def acid2_ok(pgm):
    from PIL import Image
    a = Image.open(pgm).convert('L'); b = Image.open(os.path.join(D, 'dmg-acid2', 'dmg-acid2-dmg.png')).convert('L')
    if a.size != b.size: return False
    # (the reference's four grays, lightest to darkest: ranks compared)
    ra = sorted(set(a.getdata())); rb = sorted(set(b.getdata()))
    if len(ra) != len(rb): return False
    return [ra.index(p) for p in a.getdata()] == [rb.index(p) for p in b.getdata()]

tot = ok = 0
for title, roms, flags, frames in groups:
    roms = [r for r in roms if not FILT or any(x in r for x in FILT)]
    if not roms: continue
    res = []
    for r in roms:
        pgm = '/tmp/gbsuite.pgm'
        p = subprocess.run([GBTEST, os.path.join(D, r), '-q', '-f', str(frames), '-o', pgm] + flags, capture_output=True, text=True)
        line = p.stdout.strip().split('\n')[-1]
        v = line.split()[0] if line else 'ERR'
        h = line.split()[-1] if line else ''
        if '-s' in flags:
            # (the older Mooneye tests: the result is on the screen)
            t = p.stdout.upper()
            v = 'FAIL' if 'FAIL' in t or '!' in t else 'PASS' if 'TEST OK' in t or t.count(': OK') >= 2 else 'END'
        elif 'dmg-acid2' in r: v = 'PASS' if acid2_ok(pgm) else 'FAIL'
        elif v == 'END' and r in SEEN: v = 'PASS' if SEEN[r] == h else 'FAIL'
        res.append((r, v, h))
    n = sum(1 for x in res if x[1] == 'PASS')
    tot += len(res); ok += n
    print('%-34s %3d / %3d' % (title, n, len(res)))
    for r, v, h in res:
        if v != 'PASS' or V: print('    %-5s %s%s' % (v, r, '  ' + h if v == 'END' else ''))
print('%-34s %3d / %3d' % ('all', ok, tot))
