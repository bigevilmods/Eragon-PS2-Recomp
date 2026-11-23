"""Count VU0 macro-mode (COP2 special) instructions in the code ranges."""
import os, sys
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2recomp import Image

S1 = {0x1c: 'vmulq', 0x1d: 'vmaxi', 0x1e: 'vmuli', 0x1f: 'vminii', 0x20: 'vaddq', 0x21: 'vmaddq', 0x22: 'vaddi',
      0x23: 'vmaddi', 0x24: 'vsubq', 0x25: 'vmsubq', 0x26: 'vsubi', 0x27: 'vmsubi', 0x28: 'vadd', 0x29: 'vmadd',
      0x2a: 'vmul', 0x2b: 'vmax', 0x2c: 'vsub', 0x2d: 'vmsub', 0x2e: 'vopmsub', 0x2f: 'vmini', 0x30: 'viadd',
      0x31: 'visub', 0x32: 'viaddi', 0x34: 'viand', 0x35: 'vior', 0x38: 'vcallms', 0x39: 'vcallmsr'}
for k, n in enumerate(['vaddbc', 'vsubbc', 'vmaddbc', 'vmsubbc', 'vmaxbc', 'vminibc', 'vmulbc']):
    for b in range(4): S1[k * 4 + b] = n
S2 = {0x10: 'vitof0', 0x11: 'vitof4', 0x12: 'vitof12', 0x13: 'vitof15', 0x14: 'vftoi0', 0x15: 'vftoi4',
      0x16: 'vftoi12', 0x17: 'vftoi15', 0x1c: 'vmulaq', 0x1d: 'vabs', 0x1e: 'vmulai', 0x1f: 'vclipw', 0x20: 'vaddaq',
      0x21: 'vmaddaq', 0x22: 'vaddai', 0x23: 'vmaddai', 0x24: 'vsubaq', 0x25: 'vmsubaq', 0x26: 'vsubai',
      0x27: 'vmsubai', 0x28: 'vadda', 0x29: 'vmadda', 0x2a: 'vmula', 0x2c: 'vsuba', 0x2d: 'vmsuba', 0x2e: 'vopmula',
      0x2f: 'vnop', 0x30: 'vmove', 0x31: 'vmr32', 0x34: 'vlqi', 0x35: 'vsqi', 0x36: 'vlqd', 0x37: 'vsqd',
      0x38: 'vdiv', 0x39: 'vsqrt', 0x3a: 'vrsqrt', 0x3b: 'vwaitq', 0x3c: 'vmtir', 0x3d: 'vmfir', 0x3e: 'vilwr',
      0x3f: 'viswr', 0x40: 'vrnext', 0x41: 'vrget', 0x42: 'vrinit', 0x43: 'vrxor'}
for k, n in enumerate(['vaddabc', 'vsubabc', 'vmaddabc', 'vmsubabc']):
    for b in range(4): S2[k * 4 + b] = n
for b in range(4): S2[0x18 + b] = 'vmulabc'


def name(w):
    f = w & 63
    if f >= 0x3c:
        return S2.get(((w >> 4) & 0x7c) | (w & 3), f'?s2_{((w >> 4) & 0x7c) | (w & 3):02x}')
    return S1.get(f, f'?s1_{f:02x}')


if __name__ == '__main__':
    img = Image(sys.argv[1])
    cnt = Counter()
    for lo, hi in [(0x100000, 0x2e5af8), (0x38d200, 0x3923e8)]:
        for a in range(lo, hi, 4):
            w = img.word(a)
            if w >> 26 == 0x12 and (w >> 25) & 1:
                cnt[name(w)] += 1
    print(sum(cnt.values()), ' '.join(f'{k}={v}' for k, v in cnt.most_common()))
