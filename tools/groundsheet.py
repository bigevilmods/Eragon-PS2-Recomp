"""Contact sheet of the lower-middle part of each frame (where Eragon stands), at full resolution.

  python -I groundsheet.py out.png f1.bmp f2.bmp ...   (crops x 160..480, y 230..446; 4 per row)
"""
import struct, sys, zlib
sys.path.insert(0, __import__('os').path.dirname(__file__))
from crop import read_bmp, write_png


def main():
    out = sys.argv[1]; files = sys.argv[2:]
    x0, x1, y0, y1 = 160, 480, 230, 446
    w, h = x1 - x0, y1 - y0
    per = 4; rows_n = (len(files) + per - 1) // per
    canvas = [bytearray(w * per * 3) for _ in range(h * rows_n)]
    for i, f in enumerate(files):
        _, _, rows = read_bmp(f)
        cx, cy = (i % per) * w, (i // per) * h
        for y in range(h):
            r = rows[y0 + y][x0 * 3:x1 * 3]
            px = b''.join(r[j:j + 3][::-1] for j in range(0, len(r), 3))
            canvas[cy + y][cx * 3:cx * 3 + w * 3] = px
    write_png(out, w * per, h * rows_n, [bytes(r) for r in canvas])


if __name__ == '__main__':
    main()
