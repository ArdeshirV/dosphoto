/* bmp.c - BMP (Windows DIB) decoder: 1/4/8/16/24/32 bpp, BI_RGB and
 * BI_BITFIELDS, bottom-up and top-down rows. */
#include <string.h>
#include "img.h"

typedef struct {
    uint32_t data_off;
    uint32_t stride;
    int32_t  width, height;
    uint16_t bpp;
    uint32_t compression;
    uint16_t ncolors;
    uint8_t  pal[256][3];
    uint8_t  topdown;
    uint32_t mask[3];
    uint8_t  shift[3];
    uint8_t  bits[3];
} bmp_t;

static uint32_t le32(const uint8_t IMG_FAR *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t IMG_FAR *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void mask_setup(bmp_t *b, int i)
{
    uint32_t m = b->mask[i];
    uint8_t sh = 0, bits = 0;

    if (m) {
        while (!((m >> sh) & 1) && sh < 32)
            sh++;
        while (bits < 32 && ((m >> (sh + bits)) & 1))
            bits++;
    }
    b->shift[i] = sh;
    b->bits[i] = bits;
}

static uint8_t scale_bits(uint32_t v, uint8_t bits)
{
    switch (bits) {
    case 0: return 0;
    case 1: return v ? 255 : 0;
    case 2: return (uint8_t)(v * 85u);
    case 3: return (uint8_t)(v * 36u);
    case 4: return (uint8_t)(v * 17u);
    case 5: return (uint8_t)((v << 3) | (v >> 2));
    case 6: return (uint8_t)((v << 2) | (v >> 4));
    case 7: return (uint8_t)((v << 1) | (v >> 6));
    }
    return (uint8_t)v;
}

static int bmp_parse(bmp_t *b, const buf_t *f, img_info_t *info)
{
    uint8_t h[54];
    uint32_t hsize;
    int32_t h32;
    uint32_t rbs;
    uint32_t need;

    if (f->len < 54)
        return IMG_ERR_TRUNC;
    fget_n(f, 0, h, 54);
    if (h[0] != 'B' || h[1] != 'M')
        return IMG_ERR_FORMAT;

    b->data_off = le32(h + 10);
    hsize = le32(h + 14);
    if (hsize < 40)
        return IMG_ERR_UNSUP;
    b->width = (int32_t)le32(h + 18);
    h32 = (int32_t)le32(h + 22);
    b->topdown = h32 < 0;
    b->height = b->topdown ? -h32 : h32;
    b->bpp = le16(h + 28);
    b->compression = le32(h + 30);

    if (b->width <= 0 || b->height <= 0)
        return IMG_ERR_CORRUPT;
    if (b->width > IMG_MAX_WIDTH || b->height > 32767)
        return IMG_ERR_TOOBIG;
    if (b->bpp != 1 && b->bpp != 4 && b->bpp != 8 && b->bpp != 16 &&
        b->bpp != 24 && b->bpp != 32)
        return IMG_ERR_UNSUP;
    if (b->compression != 0 && b->compression != 3)
        return IMG_ERR_UNSUP;

    b->ncolors = 0;
    if (b->bpp <= 8) {
        uint32_t used = le32(h + 46);
        uint32_t n = used ? used : (1UL << b->bpp);
        uint32_t i, off = 14 + hsize;

        if (n > 256)
            return IMG_ERR_CORRUPT;
        if (off + n * 4 > f->len)
            return IMG_ERR_TRUNC;
        for (i = 0; i < n; i++) {
            uint8_t e[4];
            fget_n(f, off + i * 4, e, 4);
            b->pal[i][0] = e[2];
            b->pal[i][1] = e[1];
            b->pal[i][2] = e[0];
        }
        b->ncolors = (uint16_t)n;
    }

    if (b->bpp == 16 || b->bpp == 32) {
        if (b->compression == 3) {
            uint8_t m[12];
            uint32_t moff;
            if (hsize >= 52)
                moff = 14 + 40;
            else
                moff = 14 + 40;
            if (moff + 12 > f->len)
                return IMG_ERR_TRUNC;
            fget_n(f, moff, m, 12);
            b->mask[0] = le32(m);
            b->mask[1] = le32(m + 4);
            b->mask[2] = le32(m + 8);
        } else {
            if (b->bpp == 16) {
                b->mask[0] = 0x7C00;
                b->mask[1] = 0x03E0;
                b->mask[2] = 0x001F;
            } else {
                b->mask[0] = 0xFF0000;
                b->mask[1] = 0x00FF00;
                b->mask[2] = 0x0000FF;
            }
        }
        mask_setup(b, 0);
        mask_setup(b, 1);
        mask_setup(b, 2);
    }

    rbs = ((uint32_t)b->bpp * (uint32_t)b->width + 31) / 32 * 4;
    if (rbs > IMG_MAX_ROW)
        return IMG_ERR_TOOBIG;
    b->stride = rbs;

    need = b->data_off + (uint32_t)rbs * (uint32_t)b->height;
    if (need > f->len)
        return IMG_ERR_TRUNC;

    info->width = (uint16_t)b->width;
    info->height = (uint16_t)b->height;
    info->format = IMG_FMT_BMP;
    return IMG_OK;
}

/* Palette/index extractor for sub-byte depths. */
static int bmp_index(const uint8_t IMG_FAR *row, int x, int bpp)
{
    switch (bpp) {
    case 1: return (row[x >> 3] >> (7 - (x & 7))) & 1;
    case 4: return (row[x >> 1] >> (4 - 4 * (x & 1))) & 15;
    }
    return row[x];
}

int bmp_info(const buf_t *f, img_info_t *info)
{
    static bmp_t b;
    return bmp_parse(&b, f, info);
}

int bmp_decode(const buf_t *f, img_row_fn cb, void *ctx)
{
    static bmp_t b;
    static buf_t rowbuf;
    uint8_t IMG_FAR *row;
    img_info_t info;
    int err;
    int y;
    uint16_t x;
    uint16_t w;

    err = bmp_parse(&b, f, &info);
    if (err != IMG_OK)
        return err;

    if (b.stride > rowbuf.len) {
        mem_free(&rowbuf);
        if (mem_alloc(b.stride, &rowbuf) != IMG_OK)
            return IMG_ERR_NOMEM;
    }
    row = buf_fp(&rowbuf);

    w = info.width;
    for (y = 0; y < (int)b.height; y++) {
        uint32_t src_y = b.topdown ? (uint32_t)y : (uint32_t)(b.height - 1 - y);
        const uint8_t IMG_FAR *rp = row;
        uint8_t IMG_FAR *out = g_rgbrow;
        uint32_t roff = b.data_off + src_y * b.stride;

        if (roff + b.stride > f->len)
            return IMG_ERR_TRUNC;
        fget_far(f, roff, row, b.stride);

        switch (b.bpp) {
        case 1:
        case 4:
        case 8:
            for (x = 0; x < w; x++) {
                int idx = bmp_index(rp, x, b.bpp);
                if (idx >= (int)b.ncolors) idx = 0;
                out[3 * x]     = b.pal[idx][0];
                out[3 * x + 1] = b.pal[idx][1];
                out[3 * x + 2] = b.pal[idx][2];
            }
            break;
        case 16:
            for (x = 0; x < w; x++) {
                uint32_t v = le16(rp + 2 * x);
                out[3 * x]     = scale_bits((v & b.mask[0]) >> b.shift[0], b.bits[0]);
                out[3 * x + 1] = scale_bits((v & b.mask[1]) >> b.shift[1], b.bits[1]);
                out[3 * x + 2] = scale_bits((v & b.mask[2]) >> b.shift[2], b.bits[2]);
            }
            break;
        case 24:
            for (x = 0; x < w; x++) {
                out[3 * x]     = rp[3 * x + 2];
                out[3 * x + 1] = rp[3 * x + 1];
                out[3 * x + 2] = rp[3 * x];
            }
            break;
        case 32:
            for (x = 0; x < w; x++) {
                uint32_t v = le32(rp + 4 * x);
                out[3 * x]     = scale_bits((v & b.mask[0]) >> b.shift[0], b.bits[0]);
                out[3 * x + 1] = scale_bits((v & b.mask[1]) >> b.shift[1], b.bits[1]);
                out[3 * x + 2] = scale_bits((v & b.mask[2]) >> b.shift[2], b.bits[2]);
            }
            break;
        }
        cb(ctx, out, (uint16_t)y);
    }
    return IMG_OK;
}
