/* dimg.c - dphoto native image format ("DPH1IMG").
 *
 * Layout (little endian):
 *      0   8  magic "DPH1IMG\n"
 *      8   2  width in pixels
 *     10   2  height in pixels
 *     12   1  bits per pixel, always 8
 *     13   1  flags, bit 0 = the palette is VGA 6-bit
 *     14   1  format version, 1
 *     15   1  reserved, 0
 *     16 768  palette, RGB triples as sent to the VGA DAC
 *    784   w*h one byte per pixel, top row first
 *
 * The file is what dphoto writes from its own renderer, so it needs no
 * quantiser or dither stage to be displayed again: the palette goes
 * straight to the DAC and the indices straight to video memory.
 */
#include "img.h"

#define DIMG_MAGIC    "DPH1IMG\n"
#define DIMG_HDR      16
#define DIMG_PAL      768
#define DIMG_ROWS     (DIMG_HDR + DIMG_PAL)      /* 784 */
#define DIMG_VERSION  1
#define DIMG_FLAG_6BIT 1

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t dimg_rows_off(void)
{
    return DIMG_ROWS;
}

int dimg_info(const buf_t *f, img_info_t *info)
{
    uint8_t h[DIMG_HDR];
    uint16_t w, hh;

    if (f->len < DIMG_ROWS)
        return IMG_ERR_TRUNC;
    fget_n(f, 0, h, DIMG_HDR);
    w = le16(h + 8);
    hh = le16(h + 10);
    if (h[12] != 8)
        return IMG_ERR_UNSUP;
    if (h[14] != DIMG_VERSION)
        return IMG_ERR_UNSUP;
    if (w == 0 || hh == 0)
        return IMG_ERR_CORRUPT;
    if (w > IMG_MAX_WIDTH)
        return IMG_ERR_TOOBIG;
    if ((uint32_t)DIMG_ROWS + (uint32_t)w * hh > f->len)
        return IMG_ERR_TRUNC;

    info->width = w;
    info->height = hh;
    info->format = IMG_FMT_IMG;
    return IMG_OK;
}

int dimg_decode(const buf_t *f, img_row_fn cb, void *ctx)
{
    static uint8_t pal[256][3];
    uint8_t h[DIMG_HDR];
    uint8_t raw[DIMG_PAL];
    uint16_t w, hh, x, y, i;
    int err;

    err = dimg_info(f, &(img_info_t){0});
    if (err != IMG_OK)
        return err;
    fget_n(f, 0, h, DIMG_HDR);
    w = le16(h + 8);
    hh = le16(h + 10);

    fget_n(f, DIMG_HDR, raw, DIMG_PAL);
    for (i = 0; i < 256; i++) {
        if (h[13] & DIMG_FLAG_6BIT) {
            pal[i][0] = (uint8_t)((raw[3 * i] << 2) | (raw[3 * i] >> 4));
            pal[i][1] = (uint8_t)((raw[3 * i + 1] << 2) | (raw[3 * i + 1] >> 4));
            pal[i][2] = (uint8_t)((raw[3 * i + 2] << 2) | (raw[3 * i + 2] >> 4));
        } else {
            pal[i][0] = raw[3 * i];
            pal[i][1] = raw[3 * i + 1];
            pal[i][2] = raw[3 * i + 2];
        }
    }

    for (y = 0; y < hh; y++) {
        uint32_t roff = DIMG_ROWS + (uint32_t)y * w;

        for (x = 0; x < w; x++) {
            uint8_t idx = fget(f, roff + x);
            g_rgbrow[3 * x]     = pal[idx][0];
            g_rgbrow[3 * x + 1] = pal[idx][1];
            g_rgbrow[3 * x + 2] = pal[idx][2];
        }
        cb(ctx, g_rgbrow, y);
    }
    return IMG_OK;
}
