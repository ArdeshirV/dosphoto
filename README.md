# dphoto — a DOS image viewer

A real-mode DOS image viewer written in C for 16-bit x86 (8086 and up). It
decodes BMP, PNG and baseline JPEG images, scales them to the screen, and shows
them with a fixed 216-colour palette and ordered dithering.

No image library is involved: the decoders are written for the 16-bit target
and also compile unmodified on a PC, so every format can be checked pixel by
pixel against `ffmpeg` — both on the host and inside an emulator, using the
very same decoder sources.

```
+-------------------------------+   +----------------------------+
|  src/  decoders (portable)    |   |  src/dos/  DOS layer       |
|  bmp png(inflate) jpeg        |   |  mem  ui  main             |
|  stream  render  img          |   |  far buffers, VGA, viewer  |
+---------------+---------------+   +-------------+--------------+
                shared img.h / render.h (row-callback API)
```

## Features

* **BMP**: 1/4/8/16/24/32 bpp, `BI_RGB` and `BI_BITFIELDS` (RGB555 and 565),
  bottom-up and top-down, `BI_RLE` reported as unsupported.
* **PNG**: non-interlaced, bit depths 1/2/4/8/16, colour types 0/2/3/4/6
  (grey, RGB, palette, grey+alpha, RGBA), palette `tRNS` transparency
  (RGB/grey `tRNS` is parsed but not applied), all five filter types, zlib
  streams through a built-in DEFLATE decoder (stored, fixed and dynamic
  Huffman blocks). Alpha channels are composited over black.
* **JPEG**: baseline and extended sequential Huffman, 1 or 3 components, any
  sampling factors (4:4:4, 4:2:2, 4:2:0, 4:1:1, …), restart markers, custom
  quantisation and Huffman tables.
* **Video**: VGA mode 13h (320x200x256) by default; VESA mode 101h
  (640x480x256) through the banked window when the BIOS provides it.
* **Rendering**: aspect-preserving fit, 6x6x6 colour cube with a 4x4 Bayer
  dither, painted row by row while the image is still decoding.
* **16-bit friendly throughout**: a single 64K data segment, far buffers held
  as `segment:offset` pairs, a 32K DEFLATE window inside one segment, 32-bit
  arithmetic only through `uint32_t`/`int32_t`, and no reliance on a 32-bit
  `int`.

## Building

DOS executable (needs `ia16-elf-gcc` with `libi86`):

    make dos            # -> build/DPHOTO.EXE

Host harness used for testing (any C99 compiler):

    make host           # -> build/hosttest

Note: the DOS build must use `-std=gnu99` (the Makefile does). Strict `-std=c99`
defines `__STRICT_ANSI__`, which makes `ia16-elf-gcc` reject the `__far`
keyword, and every far-pointer declaration in the decoders would fail to parse.

## Usage

    DPHOTO [/320|/640] [file]    browse images in the current directory
    DPHOTO /dump out.ppm file    decode a file to a PPM (P6) image
    DPHOTO /selftest             run the built-in self test
    DPHOTO /render out.dph file  render a screen capture (test aid)

* Without a file name the viewer starts at the first `*.BMP`, `*.PNG` or
  `*.JPG` in the current directory; a file given on the command line is
  selected in that list, so `n`/`p` keep browsing from there.
* `/320` forces mode 13h (the default), `/640` requests 640x480. If the VESA
  BIOS is absent or does not offer mode 101h, the viewer silently falls back to
  mode 13h, so `/640` is always safe to pass.

Keys:

    n, space, right    next image
    p, backspace, left previous image
    m                  toggle 320x200 / 640x480
    r                  redraw
    Esc, q             quit

`/dump` writes the decoded image as binary PPM (P6) — this is the format used
by the regression tests, and it is also handy for checking a file on a machine
without a display.

`/render` writes a `DPH1` capture: an 8-byte header (`"DPH1"`, target width,
target height as little-endian words), the 768-byte palette, then one byte of
palette index per pixel. It draws the same title and footer bars as the
interactive viewer, so the BIOS-font text path is covered as well.
`build/hosttest render file out.dph [w h]` produces the same format on the
host, which makes the DOS rendering pipeline directly comparable — palette and
pixels — against the host one.

## Source layout

    src/img.h  img.c      format detection, info/decode dispatch, error strings
    src/stream.c          buffered byte stream over a far buffer
    src/inflate.c         raw DEFLATE (puff-style), 32K window
    src/png.c             PNG chunks, filters, colour conversion
    src/bmp.c             BMP headers, palettes, bit depths, bitfields
    src/jpeg.c            markers, Huffman, dequantisation, IDCT, upsampling
    src/render.c  .h      row map (nearest neighbour), palette, dither
    src/dos/mem.c         far buffers (_dos_allocmem), 64K-safe copies
    src/dos/ui.c  .h      mode 13h / VESA, palette, BIOS-font text, blitting
    src/dos/main.c        file list, viewer, /dump, /render, /selftest

Decoders push one RGB scanline at a time through an `img_row_fn` callback, so
the renderer needs no frame buffer and large images never have to be held in
one piece.

## Limits

* Source width up to 1024 pixels and one source scanline up to 4096 bytes
  (e.g. 1024 wide 24-bit, 1024 wide 32-bit, or 512 wide 16-bit RGBA). Larger
  PNG widths are rejected with `IMG_ERR_TOOBIG` rather than decoded wrongly.
* Interlaced PNG and progressive/arithmetic JPEG are reported as unsupported
  instead of being decoded.
* Images are only scaled to fit the screen; there is no zooming or scrolling.
  Mode 13h pixels are not square, so 320x200 output looks slightly stretched.
* The whole file is read into DOS conventional memory, so files above about
  448 KB are rejected with `IMG_ERR_NOMEM`.
* JPEG IDCT results can differ from libjpeg/ffmpeg by a few units of
  colour depth (integer rounding); BMP and PNG decode exactly.

Current DOS build: `DPHOTO.EXE` is 50.5 KB, near code (`cs`) is 46.7 KB, and
the data segment ends at `0xD5DC`, leaving about 10 KB for the stack and the
C library heap.

## Testing

Host comparison against `ffmpeg` (fixtures generated on the fly; BMP and PNG
must be bit exact, JPEG within IDCT rounding):

    make test

At the time of writing: 24 fixtures, 24 pass.

The same comparison runs inside an emulator against the real DOS binary,
including `/selftest` and a `/render` capture. The emulator command comes from
the environment, so any DOSBox fork works:

    DOSBOX="env HOME=$PWD/dbx/home SDL_VIDEODRIVER=dummy \
            LD_LIBRARY_PATH=$PWD/dbx/root/usr/lib/x86_64-linux-gnu \
            ./dbx/root/usr/bin/dosbox-x" \
    DOSBOX_CONF_EXTRA="[dosbox]\nmemsize=16" \
        ./test/run_dos_tests.sh build/DPHOTO.EXE dbx/dosrun

`test/run_dos_tests.sh` copies the fixtures into the mounted directory as
`I1..In` (DOS 8.3 names), runs `/dump` for each of them, compares every result
against `ffmpeg`, and prints the `/selftest` output. At the time of writing:
15 formats, 15 pass, plus a clean `/selftest`.

`/selftest` covers:

1. far memory: writes, reads and `fzero` across a 64K boundary;
2. BMP decode of an image built in memory (5x3, 24 bpp, bottom-up);
3. PNG decode of an image built in memory (4x2, stored deflate block);
4. renderer + palette: a solid colour must land on palette entry 180;
5. palette generator determinism;
6. VGA DAC programming, read back through ports `3C7h`/`3C9h`.

It needs no files and no input, so it also runs on real hardware
(`DPHOTO /selftest`).

Comparison helpers:

    test/compare.py            pixel comparison of two images via ffmpeg
    test/gen_extra.py          extra BMP variants (16 bpp, 565 bitfields,
                               4/1 bpp, top-down) from a PPM
    test/hosttest.c            host harness: info, dec, render
    test/host_mem.c            host platform layer (24 MB arena)

## Verified behaviour

| Check                                   | Result                              |
|-----------------------------------------|-------------------------------------|
| Host decoders vs ffmpeg                 | 24/24 exact (JPEG max error 4)      |
| DOS decoders vs ffmpeg (15 formats)     | 15/15 pass, BMP/PNG exact           |
| DOS vs host decode, identical JPEG file | byte identical                      |
| DOS `/render` vs host render (320x192)  | palette equal, 0 of 61,440 differ   |
| DOS `/selftest`                         | all six checks pass                 |

The VESA 640x480 path cannot be exercised in the DOSBox-X build used here: its
video BIOS rejects the `4F00h` query under the dummy video driver, so the
viewer takes the documented fallback to mode 13h. The code follows the VBE 2.0
mode information block (window segment, granularity, bytes per scanline) and is
kept for real hardware and emulators that do expose mode 101h.
