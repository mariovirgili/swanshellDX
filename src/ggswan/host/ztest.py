# Differential test: our Z80 core vs the "z80" PyPI package, one random
# instruction at a time.
import ctypes, random, sys, z80 as ref

lib = ctypes.CDLL(sys.argv[1])
class St(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint8) for n in
                "a f b c d e h l a_ f_ b_ c_ d_ e_ h_ l_ ixh ixl iyh iyl".split()] + \
               [("sp", ctypes.c_uint16), ("pc", ctypes.c_uint16)] + \
               [(n, ctypes.c_uint8) for n in "i r iff1 iff2 im halted ei_pending irq".split()] + \
               [("cycles", ctypes.c_int32)]
lib.t_state.restype = ctypes.POINTER(St)
lib.t_mem.restype = ctypes.POINTER(ctypes.c_uint8 * 65536)
lib.t_init()
st = lib.t_state().contents
mem = lib.t_mem().contents

random.seed(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
SKIP = {0x76, 0xDB, 0xD3, 0xF3, 0xFB}       # HALT, IN, OUT, DI, EI
SKIP_ED = set(range(0x40, 0x80, 8)) | set(range(0x41, 0x80, 8)) | {0xA2,0xAA,0xB2,0xBA,0xA3,0xAB,0xB3,0xBB,0x45,0x4D,0x55,0x5D,0x65,0x6D,0x75,0x7D,0x57,0x5F,0x4F}

def gen():
    p = random.random()
    op = random.randrange(256)
    if p < 0.55:
        if op in SKIP or op in (0xCB, 0xED, 0xDD, 0xFD): op = 0x00
        return [op] + [random.randrange(256) for _ in range(3)]
    if p < 0.70:
        return [0xCB, op, 0, 0]
    if p < 0.82:
        if op in SKIP_ED or not (0x40 <= op < 0x80 or 0xA0 <= op < 0xBC): op = 0x44
        return [0xED, op] + [random.randrange(256) for _ in range(2)]
    pre = random.choice([0xDD, 0xFD])
    if random.random() < 0.3:
        return [pre, 0xCB, random.randrange(256), op]
    if op in SKIP or op in (0xCB, 0xED, 0xDD, 0xFD): op = 0x00
    return [pre, op] + [random.randrange(256) for _ in range(2)]

fails = {}
xyfails = {}
N = int(sys.argv[3]) if len(sys.argv) > 3 else 20000
for n in range(N):
    m = ref.Z80Machine()
    data = bytes(random.randrange(256) for _ in range(65536))
    code = gen()
    pc = random.randrange(0x1000, 0xE000)
    data = bytearray(data); data[pc:pc+len(code)] = bytes(code); data = bytes(data)
    m.set_memory_block(0, data)
    ctypes.memmove(mem, data, 65536)
    regs = {k: random.randrange(256) for k in "a f b c d e h l".split()}
    ix, iy, sp = random.randrange(65536), random.randrange(65536), random.randrange(0x100, 0xFF00)
    m.a, m.f, m.b, m.c, m.d, m.e, m.h, m.l = [regs[k] for k in "a f b c d e h l".split()]
    m.ix, m.iy, m.sp, m.pc = ix, iy, sp, pc
    for k in regs: setattr(st, k, regs[k])
    alt = {k: random.randrange(256) for k in "a f b c d e h l".split()}
    m.alt_af, m.alt_bc, m.alt_de, m.alt_hl = alt['a']<<8|alt['f'], alt['b']<<8|alt['c'], alt['d']<<8|alt['e'], alt['h']<<8|alt['l']
    for k in alt: setattr(st, k + '_', alt[k])
    st.ixh, st.ixl, st.iyh, st.iyl = ix >> 8, ix & 255, iy >> 8, iy & 255
    st.sp, st.pc = sp, pc
    st.halted = 0; st.irq = 0; st.iff1 = 0; st.ei_pending = 0
    m.ticks_to_stop = 1
    m.run()
    if code[0] in (0xDD, 0xFD) and m.pc == pc + 1:
        m.ticks_to_stop = 1
        m.run()
    # (block repeats: the reference may run several iterations; compare one)
    lib.t_step()
    ours = dict(a=st.a, f=st.f, bc=st.b<<8|st.c, de=st.d<<8|st.e, hl=st.h<<8|st.l,
                ix=st.ixh<<8|st.ixl, iy=st.iyh<<8|st.iyl, sp=st.sp, pc=st.pc)
    theirs = dict(a=m.a, f=m.f, bc=m.bc, de=m.de, hl=m.hl, ix=m.ix, iy=m.iy, sp=m.sp, pc=m.pc)
    diff = [k for k in ours if ours[k] != theirs[k] and not (k == 'f' and (ours[k] ^ theirs[k]) & 0xD7 == 0)]
    mdiff = bytes(mem) != m.memory[:65536] if hasattr(m.memory, '__getitem__') else False
    xyonly = ours['f'] != theirs['f'] and (ours['f'] ^ theirs['f']) & 0xD7 == 0
    key = ' '.join('%02X' % b for b in (code[:2] if code[0] not in (0xDD,0xFD) or code[1] != 0xCB else [code[0], 0xCB, code[3]]))
    if xyonly:
        xyfails[key] = xyfails.get(key, 0) + 1
    if diff or mdiff:
        if key not in fails:
            fails[key] = (diff, mdiff, ['%s:%04X/%04X' % (k, ours[k], theirs[k]) for k in diff])
print("tests", N, "failing opcodes", len(fails))
for k in sorted(fails)[:60]:
    print(k, fails[k])

print("xy-only flag differences:", len(xyfails), sorted(xyfails)[:40])
