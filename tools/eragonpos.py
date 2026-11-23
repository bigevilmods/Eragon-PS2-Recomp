"""Print ClEragonObj's position (+0x850, +0x5f0) from RAM dumps.

  python -I eragonpos.py ramdump_A.bin [ramdump_B.bin ...]
Finds heap objects whose class-info pointer (+8) names ClEragonObj (class info = {name ptr, parent ptr}).
"""
import struct, sys


def cstr(ram, a):
    a &= 0x1FFFFFF
    e = ram.find(b'\0', a, a + 64)
    return ram[a:e].decode('latin1', 'replace') if e > a else ''


def find_eragon(ram):
    # class info address: a pointer to the string "ClEragonObj"
    name = ram.find(b'ClEragonObj\0')
    infos = {0x38b408}                  # ClEragonObj's class info (found by earlier analysis)
    for off in range(0, 0x400000, 4):   # class infos live in the executable's data
        p, = struct.unpack_from('<I', ram, off)
        if p == name: infos.add(off)
    objs = []
    for off in range(0x400000, len(ram) - 16, 16):
        c, = struct.unpack_from('<I', ram, off + 8)
        if c in infos: objs.append(off)
    return objs


def main():
    for fn in sys.argv[1:]:
        ram = open(fn, 'rb').read()
        for o in find_eragon(ram):
            p = struct.unpack_from('<3f', ram, o + 0x850)
            q = struct.unpack_from('<2f', ram, o + 0x5f0)
            print(f'{fn}: obj {o:#x} pos+850 {p[0]:.3f} {p[1]:.3f} {p[2]:.3f}  pos+5f0 {q[0]:.3f} {q[1]:.3f}')


if __name__ == '__main__':
    main()
