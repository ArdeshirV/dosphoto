#!/usr/bin/env python3
"""Generate extra BMP variants (16 bpp, bitfields, 4/1 bpp, top-down) from a
PPM, for exercising the BMP decoder."""
import struct
import sys


def read_ppm(path):
    d = open(path, "rb").read()
    idx = d.index(b"255\n") + 4
    hdr = d[:idx].split()
    return int(hdr[1]), int(hdr[2]), d[idx:]


def write_bmp(path, w, h, bpp, pixels, palette=None, topdown=False,
              bitfields=None):
    if bpp == 24:
        stride = (w * 3 + 3) & ~3
    elif bpp == 32:
        stride = w * 4
    elif bpp == 16:
        stride = (w * 2 + 3) & ~3
    elif bpp == 8:
        stride = (w + 3) & ~3
    elif bpp == 4:
        stride = ((w + 1) // 2 + 3) & ~3
    elif bpp == 1:
        stride = ((w + 7) // 8 + 3) & ~3
    else:
        raise ValueError(bpp)

    rows = []
    for y in range(h - 1, -1, -1) if not topdown else range(h):
        rows.append(pixels[y])
    data = b"".join(rows)

    palsize = 0
    if palette:
        palsize = len(palette) * 4
    bf_off = 14 + 40
    masks = b""
    if bitfields:
        masks = struct.pack("<III", *bitfields)
    off = bf_off + len(masks) + palsize
    size = off + len(data)

    hdr = struct.pack("<2sIHHI", b"BM", size, 0, 0, off)
    comp = 3 if bitfields else 0
    info = struct.pack("<IiiHHIIiiII", 40, w, -h if topdown else h, 1, bpp,
                       comp, len(data), 2835, 2835,
                       len(palette) if palette else 0, 0)
    pal = b""
    if palette:
        for (r, g, b) in palette:
            pal += bytes((b, g, r, 0))
    open(path, "wb").write(hdr + info + masks + pal + data)


def pack_rows(w, h, rgb, fmt):
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r, g, b = rgb[3 * (y * w + x): 3 * (y * w + x) + 3]
            if fmt == "rgb555":
                v = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
                row += struct.pack("<H", v)
            elif fmt == "rgb565":
                v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                row += struct.pack("<H", v)
            elif fmt == "rgba":
                row += bytes((b, g, r, 0xFF))
            elif fmt == "q4":
                row.append(((r >> 4) << 4) | (g >> 4) << 0)
            elif fmt == "b1":
                row.append(1 if (r + g + b) > 384 else 0)
            elif fmt == "idx4":
                row.append(((r >> 4) << 4) | (g >> 4))
        rows.append(bytes(row))
    return rows


def pack_bits(rows, w, h, bpp):
    out = []
    for y in range(h):
        src = rows[y]
        row = bytearray()
        if bpp == 4:
            for i in range(0, w, 2):
                a = src[i] & 0x0F
                b = src[i + 1] & 0x0F if i + 1 < w else 0
                row.append((a << 4) | b)
        else:
            acc = 0
            n = 0
            for x in range(w):
                acc = (acc << 1) | (src[x] & 1)
                n += 1
                if n == 8:
                    row.append(acc)
                    acc = n = 0
            if n:
                row.append(acc << (8 - n))
        while len(row) % 4:
            row.append(0)
        out.append(bytes(row))
    return out


def main():
    src, outbase = sys.argv[1], sys.argv[2]
    w, h, rgb = read_ppm(src)

    # 16 bpp default 555
    rows = pack_rows(w, h, rgb, "rgb555")
    write_bmp(outbase + "_16.bmp", w, h, 16, [r + b"\0" * (((w * 2 + 3) & ~3) - len(r)) for r in rows])

    # 16 bpp bitfields 565
    rows = pack_rows(w, h, rgb, "rgb565")
    write_bmp(outbase + "_565.bmp", w, h, 16,
              [r + b"\0" * (((w * 2 + 3) & ~3) - len(r)) for r in rows],
              bitfields=(0xF800, 0x07E0, 0x001F))

    # top-down 24 bpp
    rows = pack_rows(w, h, rgb, "rgb24") if False else None

    # 4 bpp palette (16 greys)
    pal = [(i * 17, i * 17, i * 17) for i in range(16)]
    rows = pack_rows(w, h, rgb, "q4")
    idxrows = [[(v >> 4) & 0xF if False else ((v >> 4) if (v >> 4) < 16 else 15) for v in r] for r in rows]
    # build 4-bit rows with grey palette
    idxrows = []
    for y in range(h):
        row = []
        for x in range(w):
            r, g, b = rgb[3 * (y * w + x): 3 * (y * w + x) + 3]
            row.append(((r * 30 + g * 59 + b * 11) // 100) >> 4)
        idxrows.append(bytes(row))
    bits = pack_bits(idxrows, w, h, 4)
    write_bmp(outbase + "_4.bmp", w, h, 4, bits, palette=pal)

    # 1 bpp palette
    idxrows = []
    for y in range(h):
        row = []
        for x in range(w):
            r, g, b = rgb[3 * (y * w + x): 3 * (y * w + x) + 3]
            row.append(1 if (r + g + b) > 384 else 0)
        idxrows.append(bytes(row))
    bits = pack_bits(idxrows, w, h, 1)
    write_bmp(outbase + "_1.bmp", w, h, 1, bits, palette=[(0, 0, 0), (255, 255, 255)])

    # top-down 24 bpp: rows in top-down order, negative height
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r, g, b = rgb[3 * (y * w + x): 3 * (y * w + x) + 3]
            row += bytes((b, g, r))
        while len(row) % 4:
            row.append(0)
        rows.append(bytes(row))
    write_bmp(outbase + "_td.bmp", w, h, 24, rows, topdown=True)


if __name__ == "__main__":
    main()
