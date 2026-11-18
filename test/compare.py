#!/usr/bin/env python3
"""Compare a decoded PPM (from hosttest dec) with an ffmpeg reference."""
import subprocess
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit("%s: not a P6 ppm" % path)
    # parse header
    idx = 2
    fields = []
    while len(fields) < 3:
        while idx < len(data) and data[idx : idx + 1].isspace():
            idx += 1
        if data[idx : idx + 1] == b"#":
            while data[idx : idx + 1] not in (b"\n", b""):
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx : idx + 1].isspace():
            idx += 1
        fields.append(int(data[start:idx]))
    idx += 1
    w, h, _ = fields
    return w, h, data[idx : idx + w * h * 3]


def ffmpeg_rgb(path, w, h):
    out = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-pix_fmt", "rgb24",
         "-f", "rawvideo", "-"],
        check=True, stdout=subprocess.PIPE).stdout
    if len(out) != w * h * 3:
        raise SystemExit("unexpected reference size %d" % len(out))
    return out


def main():
    ppm, src = sys.argv[1], sys.argv[2]
    tol = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    w, h, ours = read_ppm(ppm)
    ref = ffmpeg_rgb(src, w, h)
    if len(ours) != len(ref):
        raise SystemExit("size mismatch")
    diffs = [abs(a - b) for a, b in zip(ours, ref)]
    maxd = max(diffs)
    mean = sum(diffs) / len(diffs)
    sd = sorted(diffs)
    p99 = sd[int(len(sd) * 0.99)]
    print("size=%dx%d mean=%.3f max=%d p99=%d" % (w, h, mean, maxd, p99))
    if mean > tol:
        raise SystemExit("FAIL: mean %.3f > %.3f" % (mean, tol))
    print("PASS")


if __name__ == "__main__":
    main()
