"""Convert dumped 24-bit BMP frames to PNG; optionally tile several into a contact sheet.

  python -I frames2png.py out.png f1.bmp [f2.bmp ...]   (tiles up to 4 per row, half size)
"""
import struct, sys, zlib


def read_bmp(path):
    d = open(path, 'rb').read()
    off, = struct.unpack_from('<I', d, 10)
    w, h = struct.unpack_from('<ii', d, 18)
    top_down = h < 0
    h = abs(h)
    row = (w * 3 + 3) & ~3
    px = []
    for y in range(h):
        sy = y if top_down else h - 1 - y
        r = d[off + sy * row: off + sy * row + w * 3]
        px.append(bytes(b for i in range(0, w * 3, 3) for b in (r[i + 2], r[i + 1], r[i])))
    return w, h, px


def write_png(path, w, h, rows):
    raw = b''.join(b'\0' + r for r in rows)
    def chunk(t, data):
        c = t + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


if __name__ == '__main__':
    out, files = sys.argv[1], sys.argv[2:]
    imgs = [read_bmp(f) for f in files]
    if len(imgs) == 1:
        w, h, px = imgs[0]
        write_png(out, w, h, px)
        sys.exit()
    # half-size tiles
    tw, th = imgs[0][0] // 2, imgs[0][1] // 2
    cols = min(4, len(imgs)); rows_n = (len(imgs) + cols - 1) // cols
    W, H = tw * cols, th * rows_n
    canvas = [bytearray(W * 3) for _ in range(H)]
    for k, (w, h, px) in enumerate(imgs):
        ox, oy = (k % cols) * tw, (k // cols) * th
        for y in range(min(th, h // 2)):
            src = px[y * 2]
            for x in range(min(tw, w // 2)):
                canvas[oy + y][(ox + x) * 3:(ox + x) * 3 + 3] = src[x * 6:x * 6 + 3]
    write_png(out, W, H, [bytes(r) for r in canvas])
