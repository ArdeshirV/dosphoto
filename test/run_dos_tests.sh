#!/bin/sh
# DOS regression: run DPHOTO.EXE in an emulator and compare its PPM dumps
# against ffmpeg, plus the /selftest and a /render capture.
#
#   ./test/run_dos_tests.sh <path to DPHOTO.EXE> <host mounted dir> [workdir]
#
# Environment:
#   DOSBOX            emulator command line (default: dosbox-x)
#   DOSBOX_CONF_EXTRA extra conf lines (e.g. memsize), printf-style \n allowed
#
# The mounted dir must be the directory the emulator sees as C: (files are
# copied into it). Requires gcc, python3, ffmpeg, and a built host harness for
# the render comparison (skipped when build/hosttest is missing).
set -e

exe=${1:?usage: $0 DPHOTO.EXE mountdir [workdir]}
mnt=${2:?usage: $0 DPHOTO.EXE mountdir [workdir]}
top=$(cd "$(dirname "$0")/.." && pwd)
work=${3:-$(mktemp -d)}
DOSBOX=${DOSBOX:-dosbox-x}

mkdir -p "$work"
ff() { ffmpeg -v error -y "$@"; }

# --- fixtures -------------------------------------------------------------
ff -f lavfi -i "testsrc2=size=160x120:duration=1" -frames:v 1 "$work/f.ppm"
ff -i "$work/f.ppm" -pix_fmt rgb24 "$work/t24.png"
ff -i "$work/f.ppm" -pix_fmt pal8 "$work/tpal.png"
ff -i "$work/f.ppm" -pix_fmt monob "$work/t1.png"
ff -i "$work/f.ppm" -pix_fmt gray16be "$work/t16.png"
ff -i "$work/f.ppm" -pix_fmt yuv420p "$work/t420.jpg"
ff -i "$work/f.ppm" -pix_fmt yuv444p "$work/t444.jpg"
ff -i "$work/f.ppm" -pix_fmt gray "$work/tgray.jpg"
ff -i "$work/f.ppm" -pix_fmt bgr24 "$work/t24.bmp"
ff -i "$work/f.ppm" -pix_fmt bgra "$work/t32.bmp"
ff -i "$work/f.ppm" -pix_fmt pal8 "$work/t8.bmp"
python3 "$top/test/gen_extra.py" "$work/f.ppm" "$work/ge"

# --- stage into the emulated directory with 8.3 names ---------------------
list="$work/list.txt"
: > "$list"
i=0
for f in "$work"/*.png "$work"/*.jpg "$work"/*.bmp; do
    [ -f "$f" ] || continue
    i=$((i + 1))
    ext=$(echo "${f##*.}" | tr 'a-z' 'A-Z')
    cp "$f" "$mnt/I$i.$ext"
    echo "I$i $f" >> "$list"
done
cp "$exe" "$mnt/DPHOTO.EXE"
echo "copied $i images"

# --- emulator run ---------------------------------------------------------
conf="$work/run.conf"
{
    echo "[sdl]"
    echo "autolock=false"
    echo "waitonerror=false"
    [ -n "$DOSBOX_CONF_EXTRA" ] && printf "$DOSBOX_CONF_EXTRA\n"
    echo "[autoexec]"
    echo "mount c $mnt"
    echo "c:"
    echo "DPHOTO /selftest > ST.TXT"
    n=0
    while read -r key path; do
        n=$((n + 1))
        ext=$(echo "${path##*.}" | tr 'a-z' 'A-Z')
        echo "DPHOTO /dump O$n.PPM $key.$ext > L$n.TXT"
    done < "$list"
    first=$(head -1 "$list")
    echo "DPHOTO /render R.DPH ${first%% *}.$(echo "${first#* }" | sed 's/.*\.//' | tr 'a-z' 'A-Z') > R.TXT"
    echo "exit"
} > "$conf"

rm -f "$mnt"/O*.PPM "$mnt"/*.TXT "$mnt"/R.DPH
( cd "$work" && $DOSBOX -conf "$conf" -exit > emu.log 2>&1 ) || true

echo "--- selftest"
cat "$mnt/ST.TXT" 2>/dev/null || echo "no selftest output"

echo "--- dumps"
pass=0
fail=0
n=0
while read -r key path; do
    n=$((n + 1))
    if [ ! -f "$mnt/O$n.PPM" ]; then
        printf '%-3s %-24s %s\n' "$key" "$(basename "$path")" "MISSING"
        fail=$((fail + 1))
        continue
    fi
    res=$(python3 "$top/test/compare.py" "$mnt/O$n.PPM" "$path" 4.0 2>&1 | tail -1)
    printf '%-3s %-24s %s\n' "$key" "$(basename "$path")" "$res"
    case $res in PASS) pass=$((pass + 1)) ;; *) fail=$((fail + 1)) ;; esac
done < "$list"

echo "--------------------------------------"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
