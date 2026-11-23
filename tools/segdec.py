"""Decode Eragon .SEG audio segments (PS2 SPU ADPCM, interleaved channels) to WAV, and find the interleave.

  python -I segdec.py FILE.SEG [out.wav] [interleave]
Without an interleave it tries common sizes and reports how smooth each decode is (lower = right).
"""
import struct, sys, wave

COEF = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


def decode_blocks(data):
    """PS2 ADPCM: 16-byte blocks of 28 samples. Returns int16 samples."""
    out = []; s1 = s2 = 0
    for o in range(0, len(data) - 15, 16):
        hdr = data[o]; shift = hdr & 15; filt = min(hdr >> 4, 4)
        c1, c2 = COEF[filt]
        for i in range(28):
            b = data[o + 2 + i // 2]
            n = (b >> 4) if i & 1 else (b & 15)
            if n >= 8: n -= 16
            v = (n << 12) >> shift
            v += (s1 * c1 + s2 * c2 + 32) >> 6
            v = max(-32768, min(32767, v))
            out.append(v); s2 = s1; s1 = v
    return out


def split(data, inter, nch):
    chans = [bytearray() for _ in range(nch)]
    o = 0; c = 0
    while o < len(data):
        chans[c] += data[o:o + inter]; o += inter; c = (c + 1) % nch
    return chans


def roughness(x):
    return sum(abs(x[i] - 2 * x[i - 1] + x[i - 2]) for i in range(2, len(x))) / max(1, len(x))


def main():
    fn = sys.argv[1]
    d = open(fn, 'rb').read()
    rate, nsamp, nch = struct.unpack_from('<I', d, 24)[0], struct.unpack_from('<I', d, 28)[0], struct.unpack_from('<I', d, 36)[0]
    hdr = 0x4000
    body = d[hdr:]
    if len(sys.argv) < 3:
        test = body[:0x40000]
        for inter in (0x10, 0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x8000):
            ch = split(test, inter, nch)
            r = sum(roughness(decode_blocks(bytes(c))[:20000]) for c in ch) / nch
            print(f'interleave {inter:#6x}: roughness {r:9.1f}')
        print(f'rate {rate} samples {nsamp} channels {nch}')
        return
    inter = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x800
    ch = [decode_blocks(bytes(c)) for c in split(body, inter, nch)]
    n = min(min(len(c) for c in ch), nsamp)
    w = wave.open(sys.argv[2], 'wb'); w.setnchannels(nch); w.setsampwidth(2); w.setframerate(rate)
    frames = bytearray()
    for i in range(n):
        for c in ch: frames += struct.pack('<h', c[i])
    w.writeframes(bytes(frames)); w.close()
    print(f'wrote {sys.argv[2]}: {n} frames @ {rate} Hz x{nch}')


if __name__ == '__main__':
    main()
