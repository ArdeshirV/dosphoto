#!/usr/bin/env python3
"""Validate a dphoto native image (.img) and the PPM dumped from it.

    check_img.py picture.img dump.ppm [--dph capture.dph]

Checks that
  * the file has the "DPH1IMG" header with a consistent size,
  * every byte of the PPM matches a palette entry selected by the .img
    index data (end-to-end check of the loader),
  * every palette entry is a level of the 6x6x6 cube the renderer uses,
  * optionally, the pixels below the title bar match a /render capture
    of the same picture (the bars themselves hold text, so they are
    skipped).
"""
import sys

MAGIC = b'DPH1IMG'


def load_img(path):
    d = open(path, 'rb').read()
    if d[:8] != b'DPH1IMG\n':
        sys.exit('%s: bad magic' % path)
    w = d[8] | (d[9] << 8)
    h = d[10] | (d[11] << 8)
    bpp, flags, ver = d[12], d[13], d[14]
    if bpp != 8 or ver != 1:
        sys.exit('%s: unexpected bpp=%d version=%d' % (path, bpp, ver))
    want = 784 + w * h
    if len(d) != want:
        sys.exit('%s: size %d, expected %d for %dx%d' % (path, len(d), want, w, h))
    return w, h, d[16:784], d[784:]


def load_ppm(path):
    d = open(path, 'rb').read()
    if d[:2] != b'P6':
        sys.exit('%s: not a P6 PPM' % path)
    parts = d.split(b'\n', 3)
    w, h = (int(v) for v in parts[1].split())
    return w, h, parts[3]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    img, ppm = sys.argv[1], sys.argv[2]
    dph = None
    if len(sys.argv) > 4 and sys.argv[3] == '--dph':
        dph = sys.argv[4]

    w, h, pal, idx = load_img(img)
    pw, ph, pix = load_ppm(ppm)
    bad = 0
    if (pw, ph) != (w, h):
        sys.exit('%s: PPM is %dx%d, image is %dx%d' % (ppm, pw, ph, w, h))
    for i in range(w * h):
        o = 3 * idx[i]
        if pix[3 * i:3 * i + 3] != pal[o:o + 3]:
            bad += 1
    if bad:
        sys.exit('%s: %d of %d pixels do not match their palette entry'
                 % (ppm, bad, w * h))

    used = sorted(set(pal[0::3]) | set(pal[1::3]) | set(pal[2::3]))
    cube = set(range(0, 256, 51))
    stray = [v for v in used if v not in cube]
    if stray:
        sys.exit('%s: palette has non-cube levels %s' % (img, stray[:8]))

    if dph:
        r = open(dph, 'rb').read()
        rw = r[4] | (r[5] << 8)
        rh = r[6] | (r[7] << 8)
        if (rw, rh) != (w, h):
            sys.exit('%s: capture is %dx%d, image is %dx%d' % (dph, rw, rh, w, h))
        rpix = r[8 + 768:]
        diff = [i for i in range(w * h)
                if i // w >= 8 and i // w < h - 8 and rpix[i] != idx[i]]
        if diff:
            sys.exit('%s: %d image-area pixels differ from the capture'
                     % (dph, len(diff)))

    print('%s %dx%d: header, palette and %d pixels ok'
          % (img, w, h, w * h))


if __name__ == '__main__':
    main()
