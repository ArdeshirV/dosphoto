# dphoto — a DOS image viewer

A real-mode DOS image viewer written in C for 16-bit x86 (8086 and up). It
decodes BMP, PNG and baseline JPEG images, scales them to the screen, and shows
them with a fixed 216-colour palette and ordered dithering. It can also write
and read its own 8-bit image format, so a photo can be rendered once and then
browsed instantly.

No image library is involved: the decoders are written for the 16-bit target
and also compile unmodified on a PC, so every format can be checked pixel by
pixel against `ffmpeg` — both on the host and inside an emulator, using the
very same decoder sources.

```
+-------------------------------+   +----------------------------+
|  src/  decoders (portable)    |   |  src/dos/  DOS layer       |
|  bmp png(inflate) jpeg dimg   |   |  mem  ui  main             |
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
* **IMG**: dphoto's own native format — a magic string, the palette and one
  palette index per pixel (see below), written with `/save` and decoded like
  any other file.
* **Video**: VGA mode 13h (320x200x256) by default; VESA mode 101h
  (640x480x256) through the banked window when the BIOS provides it.
* **Rendering**: aspect-preserving fit, 6x6x6 colour cube with a 4x4 Bayer
  dither, painted row by row while the image is still decoding.
* **16-bit friendly throughout**: a single 64K data segment, far buffers held
  as `segment:offset` pairs, a 32K DEFLATE window inside one segment, 32-bit
  arithmetic only through `uint32_t`/`int32_t`, and no reliance on a 32-bit
  `int`. Scanline work buffers are allocated in far memory at the size the
  image needs instead of living in the data segment, so wide photos do not
  shrink the stack.

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
    DPHOTO /save out.img file    render a file into a native IMG
    DPHOTO /selftest             run the built-in self test
    DPHOTO /render out.dph file  render a screen capture (test aid)

* Without a file name the viewer starts at the first `*.BMP`, `*.PNG`,
  `*.JPG` or `*.IMG` in the current directory; a file given on the command
  line is selected in that list, so `n`/`p` keep browsing from there.
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

`/save` runs the same pipeline as the viewer (same scaling, dither and screen
geometry, but no text bars) and stores the result as a native image. Decoding
an `.img` is much cheaper than decoding the photo again: a 320x200 file is
64,784 bytes.

`/render` writes a `DPH1` capture: an 8-byte header (`"DPH1"`, target width,
target height as little-endian words), the 768-byte palette, then one byte of
palette index per pixel. It draws the same title and footer bars as the
interactive viewer, so the BIOS-font text path is covered as well.
`build/hosttest render file out.dph [w h]` produces the same format on the
host, which makes the DOS rendering pipeline directly comparable — palette and
pixels — against the host one.

## The native image format

`.img` files start with the ASCII magic `"DPH1IMG\n"`, so a file name
extension is not needed to recognise them, and `file(1)` can identify them via
`contrib/dphoto.magic`:

    $ file picture.img
    picture.img: DPhoto DOS 256-colour image, 320 x 200, 8-bit palette index

Layout (little endian):

| Offset | Size  | Contents                                        |
|--------|-------|-------------------------------------------------|
| 0      | 8     | `"DPH1IMG\n"`                                    |
| 8      | 2     | width in pixels                                 |
| 10     | 2     | height in pixels                                |
| 12     | 1     | bits per pixel, always 8                        |
| 13     | 1     | flags, bit 0: palette entries are 6-bit VGA DAC |
| 14     | 1     | format version, 1                               |
| 15     | 1     | reserved, 0                                     |
| 16     | 768   | palette, RGB triples, top row first after it    |
| 784    | w*h   | one palette index per pixel, top row first      |

`/save` writes the palette as 8-bit RGB (flag 0), which round-trips exactly
through the renderer. A 6-bit palette (flag 1) is expanded with
`(v << 2) | (v >> 4)` on load, which is what the VGA DAC itself does.

## Source layout

    src/img.h  img.c      format detection, info/decode dispatch, error strings
    src/stream.c          buffered byte stream over a far buffer
    src/inflate.c         raw DEFLATE (puff-style), 32K window
    src/png.c             PNG chunks, filters, colour conversion
    src/bmp.c             BMP headers, palettes, bit depths, bitfields
    src/jpeg.c            markers, Huffman, dequantisation, IDCT, upsampling
    src/dimg.c            native "DPH1IMG" palette image reader
    src/render.c  .h      row map (nearest neighbour), palette, dither
    src/dos/mem.c         far buffers (_dos_allocmem), far-pointer copies
    src/dos/ui.c  .h      mode 13h / VESA, palette, BIOS-font text, blitting
    src/dos/main.c        file list, viewer, /dump, /save, /render, /selftest
    contrib/dphoto.magic  file(1) magic for .img files

Decoders push one RGB scanline at a time through an `img_row_fn` callback, so
the renderer needs no frame buffer and large images never have to be held in
one piece.

## Limits

* Source width up to 4096 pixels and one source scanline up to 32768 bytes
  (e.g. 4096 wide 24-bit, 4096 wide 32-bit, or 2048 wide 16-bit RGBA). Larger
  widths are rejected with `IMG_ERR_TOOBIG` rather than decoded wrongly.
  Scanline buffers for those sizes come from far memory, so the data segment
  and the stack stay the same size whatever the image dimensions.
* Interlaced PNG and progressive/arithmetic JPEG are reported as unsupported
  instead of being decoded.
* Images are only scaled to fit the screen; there is no zooming or scrolling.
  Mode 13h pixels are not square, so 320x200 output looks slightly stretched.
* The whole file is read into DOS conventional memory, together with the
  scanline buffers and the 32K DEFLATE window. On a machine with 640 KB of
  conventional memory this allows input files of roughly 400-500 KB, which
  covers 640x480 and 800x600 PNG photos and 1024x768 baseline JPEGs; larger
  files fail cleanly with `IMG_ERR_NOMEM`. A native `.img` is far smaller
  (78 bytes plus one byte per pixel) and always fits.
* JPEG IDCT results can differ from libjpeg/ffmpeg by a few units of
  colour depth (integer rounding); BMP and PNG decode exactly.

Current DOS build: `DPHOTO.EXE` is 54 KB and the data segment ends at
`0xD620`, leaving about 10 KB of the data segment for the stack.

## Testing

Host comparison against `ffmpeg` (fixtures generated on the fly; BMP and PNG
must be bit exact, JPEG within IDCT rounding):

    make test

At the time of writing: 27 fixtures, 27 pass. They include a 1600x400 PNG
(longer scanline than the old 4K static row buffer) and two native `.img`
files, one with an 8-bit and one with a 6-bit palette.

The same comparison runs inside an emulator against the real DOS binary,
including `/selftest` and a `/render` capture. The emulator command comes from
the environment, so any DOSBox fork works:

    DOSBOX="env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy dosbox" \
        ./test/run_dos_tests.sh build/DPHOTO.EXE /tmp/dosrun

`dosbox-x` works the same way; add `DOSBOX_CONF_EXTRA="[dosbox]\nmemsize=16"`
when the emulator needs an explicit memory size.

`test/run_dos_tests.sh` copies the fixtures into the mounted directory as
`I1..In` (DOS 8.3 names), runs `/dump` for each of them, compares every result
against `ffmpeg`, and prints the `/selftest` output. It then renders one image
to the screen, saves it with `/save`, dumps the saved `.img` and checks the
result against the capture. At the time of writing: 16 formats, 16 pass, the
native round trip passes, and `/selftest` is clean.

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
    test/gen_img.py            small native .img plus the PPM it decodes to
    test/check_img.py          validates an .img and its PPM (and a .dph)
    test/hosttest.c            host harness: info, dec, render
    test/host_mem.c            host platform layer (24 MB arena, free list)

## Verified behaviour

| Check                                   | Result                              |
|-----------------------------------------|-------------------------------------|
| Host decoders vs ffmpeg                 | 27/27 exact (JPEG max error 4)      |
| DOS decoders vs ffmpeg (16 formats)     | 16/16 pass, BMP/PNG exact           |
| 1600x1000 PNG on DOS vs ffmpeg          | byte identical                      |
| DOS vs host decode, identical JPEG file | byte identical                      |
| DOS `/render` vs host render (320x192)  | palette equal, 0 of 61,440 differ   |
| `/save` -> `/dump` of the same picture  | palette and pixels match the capture|
| DOS `/selftest`                         | all six checks pass                 |

## 16-bit pitfalls found here

Worth knowing if you touch the decoders, because all three only show up with
real-world files, never with the small test images:

* a 16-bit `int` makes the maximum DEFLATE distance (32768) negative, and it
  makes a 15-bit canonical Huffman code wrap when the counts are accumulated —
  both use 32-bit types now;
* a `__far` pointer is a `segment:offset` pair, not a linear address: adding to
  one works, but re-deriving a segment with `ptr >> 4` does not. Far copies go
  through `fmemcpy`/`fmem_get`, which advance the pointer with far arithmetic;
* two 32K scanlines do not fit in one 64K segment, so the PNG row buffers are
  two separate allocations.

The VESA 640x480 path cannot be exercised under the dummy video driver used
for the headless runs: the video BIOS rejects the `4F00h` query there, so the
viewer takes the documented fallback to mode 13h. The code follows the VBE 2.0
mode information block (window segment, granularity, bytes per scanline) and is
kept for real hardware and emulators that do expose mode 101h.
