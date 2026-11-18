# dphoto - a DOS image viewer

A small real-mode DOS image viewer written in C for 16-bit x86 (8086 and up).
It decodes BMP, PNG and baseline JPEG images, scales them to the screen and
renders them with a fixed 216-colour palette and ordered dithering.

Everything is written from scratch for the 16-bit target: no image library is
used, and the decoders also compile unmodified on a PC host so they can be
compared pixel by pixel against `ffmpeg`.

## Features

* BMP: 1/4/8/16 (RGB555 and bitfields/565)/24/32 bpp, top-down and bottom-up.
* PNG: non-interlaced, bit depths 1/2/4/8/16, colour types 0/2/3/4/6, all five
  filter types, zlib streams via a built-in DEFLATE decoder (stored, fixed and
  dynamic Huffman blocks).
* JPEG: baseline and extended sequential Huffman, 1 or 3 components, any
  sampling factors, restart markers.
* Video: VGA mode 13h (320x200) by default; VESA mode 101h (640x480x8) when the
  BIOS offers it (`/640` or the `m` key).
* Rendering: aspect-preserving fit, 6x6x6 colour cube with a 4x4 Bayer
  dither, drawn progressively as rows are decoded.
* 16-bit constraints are respected throughout: a 64K data segment, far buffers
  addressed as `segment:offset` pairs, a 32K DEFLATE window inside one segment,
  and no use of `long` where a 32-bit type is not required.

## Building

DOS executable (needs `ia16-elf-gcc` plus `libi86`):

    make dos            # -> build/DPHOTO.EXE

Host harness used for testing (any C99 compiler):

    make host           # -> build/hosttest

## Usage

    DPHOTO [/320|/640] [file]    browse images in the current directory
    DPHOTO /dump out.ppm file    decode a file to a PPM (P6) image
    DPHOTO /selftest             run the built-in self test
    DPHOTO /render out.dph file  render to a .dph capture (test aid)

Without a file name the viewer starts at the first BMP/PNG/JPG found in the
current directory. Keys:

    n, space, right    next image
    p, backspace, left previous image
    m                  toggle 320x200 / 640x480 (VESA)
    r                  redraw
    Esc, q             quit

`/render` writes the palette (768 bytes) and the screen contents (8-bit
indices) with a `DPH1` header; the same format is produced by
`build/hosttest render`, which makes it possible to compare the DOS rendering
pipeline against the host one bit for bit.

## Limits

* Source width up to 1024 pixels; one decoded/source scanline up to 4096 bytes
  (e.g. 1024 wide RGBA, or 512 wide 16-bit RGBA).
* Interlaced PNG and progressive/arithmetic JPEG are reported as unsupported
  instead of being decoded.
* Images are only scaled to fit the screen; there is no zooming or scrolling.
  Mode 13h pixels are not square, so 320x200 output looks slightly stretched.
* Files are read into DOS conventional memory, so very large JPEGs (above
  about 448 KB) are rejected.

## Testing

Host comparison against ffmpeg (any number of formats, pixel exact for BMP and
PNG, within IDCT rounding for JPEG):

    make test

The same comparison can be run inside an emulator against the real DOS binary.
The script takes the emulator command from the environment, for example with
DOSBox-X:

    DOSBOX="env HOME=$PWD/dbx/home SDL_VIDEODRIVER=dummy \
            LD_LIBRARY_PATH=$PWD/dbx/root/usr/lib/x86_64-linux-gnu \
            ./dbx/root/usr/bin/dosbox-x" \
    DOSBOX_CONF_EXTRA="[dosbox]\nmemsize=16" \
        ./test/run_dos_tests.sh build/DPHOTO.EXE dbx/dosrun

The built-in `/selftest` checks far memory across 64K boundaries, an in-memory
BMP and PNG decode, the renderer/palette and the palette generator, and can be
used on real hardware.
