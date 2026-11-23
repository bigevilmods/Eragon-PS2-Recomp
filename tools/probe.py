"""Ad-hoc probes: disassemble around `jr reg` sites and look for the code/data boundary."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2recomp import Image
from r5900 import decode

img = Image(sys.argv[1])
mode = sys.argv[2]

def dis(a):
    w = img.word(a); i = decode(w)
    ops = f'0x{i.ops[0]:08x}' if i.fmt == 'jt' else i.ops   # jump targets in hex
    return f'{a:08x}: {w:08x} {i.mnem} {ops}'

if mode == 'jr':
    n = 0
    for a, w in img.words():
        if a >= 0x392400: break
        i = decode(w)
        if i.mnem == 'jr' and i.ops[0] != 31:
            print('----')
            for k in range(-10, 2): print(dis(a + 4 * k))
            n += 1
            if n >= 4: break
elif mode == 'range':
    lo, hi = int(sys.argv[3], 16), int(sys.argv[4], 16)
    for a in range(lo, hi, 4): print(dis(a))
elif mode == 'density':
    # fraction of invalid decodes per 64KB block
    for blk in range(0x100000, 0x399500, 0x10000):
        bad = sum(1 for a in range(blk, min(blk + 0x10000, 0x399500), 4) if decode(img.word(a)).mnem == 'invalid')
        print(f'{blk:08x} invalid={bad}')
