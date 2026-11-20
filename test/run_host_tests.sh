#!/bin/sh
# Host-side decoder regression: builds fixtures with ffmpeg, decodes them with
# the host harness and compares every pixel against ffmpeg's own conversion.
#
#   ./test/run_host_tests.sh [workdir]
#
# Requires gcc, python3 and ffmpeg.
set -e

top=$(cd "$(dirname "$0")/.." && pwd)
work=${1:-$(mktemp -d)}
mk=$work/img
mkdir -p "$mk"

echo "workdir: $work"
test -x "$top/build/hosttest" || make -C "$top" host

ff() { ffmpeg -v error -y "$@"; }

# --- fixtures -------------------------------------------------------------
ff -f lavfi -i "testsrc2=size=640x480:duration=1" -frames:v 1 "$work/src1.ppm"
ff -f lavfi -i "smptebars=size=640x480:duration=1" -frames:v 1 "$work/src2.ppm"

for s in 444 422 420 gray; do
    case $s in
        444) opt="-pix_fmt yuv444p" ;;
        422) opt="-pix_fmt yuv422p" ;;
        420) opt="-pix_fmt yuv420p" ;;
        gray) opt="-pix_fmt gray" ;;
    esac
    ff -i "$work/src1.ppm" $opt "$work/j1_$s.jpg"
    ff -i "$work/src2.ppm" $opt "$work/j2_$s.jpg"
done

ff -i "$work/src1.ppm" -pix_fmt rgb24 "$work/p24.png"
ff -i "$work/src1.ppm" -pix_fmt pal8 "$work/pal.png"
ff -i "$work/src1.ppm" -pix_fmt monob "$work/p1.png"
ff -i "$work/src1.ppm" -pix_fmt gray16be "$work/p16.png"
ff -i "$work/src1.ppm" -pix_fmt rgba "$work/prgba.png"

ff -i "$work/src1.ppm" -pix_fmt bgr24 "$work/b24.bmp"
ff -i "$work/src1.ppm" -pix_fmt bgra "$work/b32.bmp"
ff -i "$work/src1.ppm" -pix_fmt pal8 "$work/b8.bmp"
python3 "$top/test/gen_extra.py" "$work/src1.ppm" "$work/bx"

# small odd sizes
ff -f lavfi -i "testsrc2=size=64x48:duration=1" -frames:v 1 "$work/tiny.ppm"
ff -i "$work/tiny.ppm" -pix_fmt rgb24 "$work/tiny.png"
ff -i "$work/tiny.ppm" "$work/tiny.jpg"
ff -i "$work/tiny.ppm" -pix_fmt bgr24 "$work/tiny.bmp"

# a photo-sized PNG: 1600 px wide is well past the old 1024 limit, and the
# scanline is longer than the old 4K static row buffer
ff -f lavfi -i "testsrc2=size=1600x400:duration=1" -frames:v 1 \
    -pix_fmt rgb24 "$work/wide.png"

# native image format (dphoto's own .img)
python3 "$top/test/gen_img.py" "$work/native.img" "$work/native-ref.ppm"
python3 "$top/test/gen_img.py" "$work/nat6.img" "$work/nat6-ref.ppm" 6bit

# --- run ------------------------------------------------------------------
pass=0
fail=0
for f in "$work"/*.png "$work"/*.jpg "$work"/*.bmp; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    if "$top/build/hosttest" dec "$f" "$work/out.ppm" >/dev/null 2>&1; then
        res=$(python3 "$top/test/compare.py" "$work/out.ppm" "$f" 4.0 2>&1 | tail -1)
    else
        res="DECODE FAILED"
    fi
    printf '%-16s %s\n' "$base" "$(echo "$res" | tr '\n' ' ')"
    case $res in
        PASS) pass=$((pass + 1)) ;;
        *) fail=$((fail + 1)) ;;
    esac
done

for img in native nat6; do
    if "$top/build/hosttest" dec "$work/$img.img" "$work/out.ppm" >/dev/null 2>&1; then
        res=$(python3 "$top/test/compare.py" "$work/out.ppm" \
              "$work/$img-ref.ppm" 0.0 2>&1 | tail -1)
    else
        res="DECODE FAILED"
    fi
    printf '%-16s %s\n' "$img.img" "$res"
    case $res in PASS) pass=$((pass + 1)) ;; *) fail=$((fail + 1)) ;; esac
done

echo "--------------------------------------"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
