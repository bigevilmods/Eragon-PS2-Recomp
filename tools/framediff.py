"""Mean absolute difference of the upper half (background) between the first frame and each later one.

  python -I framediff.py f1.bmp f2.bmp ...
"""
import struct, sys


def read_bmp(path):
    d = open(path, 'rb').read()
    off, = struct.unpack_from('<I', d, 10)
    w, h = struct.unpack_from('<ii', d, 18)
    top_down = h < 0
    h = abs(h)
    row = (w * 3 + 3) & ~3
    rows = [d[off + y * row: off + y * row + w * 3] for y in range(h)]
    if not top_down:
        rows.reverse()
    return w, h, rows


def main():
    files = sys.argv[1:]
    w0, h0, r0 = read_bmp(files[0])
    for f in files[1:]:
        w, h, r = read_bmp(f)
        if (w, h) != (w0, h0):
            print(f'{f}: size differs'); continue
        tot = 0; n = 0
        for y in range(0, h // 2, 2):
            a, b = r0[y], r[y]
            for x in range(0, w * 3, 6):
                tot += abs(a[x] - b[x]); n += 1
        print(f'{f.split(chr(92))[-1]}: background change {tot / n:.1f}')


if __name__ == '__main__':
    main()
