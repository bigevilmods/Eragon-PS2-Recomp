"""Crop a BMP frame to PNG, optionally scaled up (nearest).

  python -I crop.py in.bmp out.png x0 y0 x1 y1 [scale]
"""
import struct, sys, zlib


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


def write_png(path, w, h, rows):
    raw = b''.join(b'\0' + r for r in rows)
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    x0, y0, x1, y1 = map(int, sys.argv[3:7])
    s = int(sys.argv[7]) if len(sys.argv) > 7 else 1
    w, h, rows = read_bmp(src)
    out = []
    for y in range(y0, y1):
        r = rows[y][x0 * 3:x1 * 3]
        px = [r[i:i + 3][::-1] for i in range(0, len(r), 3)]   # BGR -> RGB
        line = b''.join(p * s for p in px)
        out.extend([line] * s)
    write_png(dst, (x1 - x0) * s, (y1 - y0) * s, out)


if __name__ == '__main__':
    main()
