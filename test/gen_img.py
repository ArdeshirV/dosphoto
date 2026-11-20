#!/usr/bin/env python3
"""Build a small dphoto native image (.img) plus the PPM it should decode to.

    gen_img.py <out.img> [out-ref.ppm]

The palette holds 6x6x6 cube levels, so a reference PPM can be generated
alongside the .img and compared byte for byte.
"""
import sys

W, H = 8, 6
CUBE = [0, 51, 102, 153, 204, 255]


def build():
    pal = []
    for i in range(256):
        pal.append(bytes((CUBE[(i >> 4) % 6], CUBE[(i >> 2) % 6], CUBE[i % 6])))
    idx = bytearray()
    for y in range(H):
        for x in range(W):
            idx.append((y * 36 + x * 6 + (x ^ y)) & 0xFF)
    return pal, bytes(idx)


def encode(pal, idx, six_bit):
    hdr = bytearray(b'DPH1IMG\n')
    hdr += bytes((W & 0xFF, W >> 8, H & 0xFF, H >> 8, 8, 1 if six_bit else 0, 1, 0))
    flat = bytearray()
    for p in pal:
        if six_bit:
            flat += bytes(v >> 2 for v in p)
        else:
            flat += p
    return bytes(hdr) + bytes(flat) + idx


def expand(pal, six_bit):
    if not six_bit:
        return pal
    return [bytes((v << 2) | (v >> 4) for v in p) for p in
            (bytes(c >> 2 for c in p) for p in pal)]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    ref = sys.argv[2] if len(sys.argv) > 2 else None
    six_bit = len(sys.argv) > 3 and sys.argv[3] == '6bit'
    pal, idx = build()
    open(out, 'wb').write(encode(pal, idx, six_bit))
    if ref:
        pix = bytearray()
        e = expand(pal, six_bit)
        for i in idx:
            pix += e[i]
        open(ref, 'wb').write(b'P6\n%d %d\n255\n' % (W, H) + pix)
    print('%s: %dx%d' % (out, W, H))


if __name__ == '__main__':
    main()
