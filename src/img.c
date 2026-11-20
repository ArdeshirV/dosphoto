/* img.c - format detection and dispatch. */
#include "img.h"

/* Shared RGB scanline; grown on demand so that wide images do not need a
 * large static buffer. */
static buf_t g_rowbuf;
uint8_t IMG_FAR *g_rgbrow;

int img_row_alloc(uint32_t bytes)
{
    if (g_rgbrow && g_rowbuf.len >= bytes)
        return IMG_OK;
    img_row_free();
    if (mem_alloc(bytes, &g_rowbuf) != IMG_OK)
        return IMG_ERR_NOMEM;
    g_rgbrow = buf_fp(&g_rowbuf);
    return IMG_OK;
}

void img_row_free(void)
{
    mem_free(&g_rowbuf);
    g_rgbrow = 0;
}

int img_identify(const buf_t *f)
{
    uint8_t sig[8];

    if (f->len < 8)
        return IMG_FMT_NONE;
    fget_n(f, 0, sig, 8);
    if (sig[0] == 'B' && sig[1] == 'M')
        return IMG_FMT_BMP;
    if (sig[0] == 0x89 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G' &&
        sig[4] == 0x0D && sig[5] == 0x0A && sig[6] == 0x1A && sig[7] == 0x0A)
        return IMG_FMT_PNG;
    if (sig[0] == 0xFF && sig[1] == 0xD8 && sig[2] == 0xFF)
        return IMG_FMT_JPEG;
    /* tolerate a stray 0x00 before SOI, some cameras do that */
    if (sig[0] == 0x00 && sig[1] == 0xFF && sig[2] == 0xD8)
        return IMG_FMT_JPEG;
    if (sig[0] == 'D' && sig[1] == 'P' && sig[2] == 'H' && sig[3] == '1' &&
        sig[4] == 'I' && sig[5] == 'M' && sig[6] == 'G' && sig[7] == '\n')
        return IMG_FMT_IMG;
    return IMG_FMT_NONE;
}

int img_read_info(const buf_t *f, img_info_t *info)
{
    int fmt = img_identify(f);
    int err;

    info->width = info->height = 0;
    info->format = (uint8_t)fmt;
    switch (fmt) {
    case IMG_FMT_BMP:  err = bmp_info(f, info); break;
    case IMG_FMT_PNG:  err = png_info(f, info); break;
    case IMG_FMT_JPEG: err = jpg_info(f, info); break;
    case IMG_FMT_IMG:  err = dimg_info(f, info); break;
    default:           return IMG_ERR_FORMAT;
    }
    return err;
}

int img_decode(const buf_t *f, const img_info_t *info, img_row_fn cb, void *ctx)
{
    /* Every decoder emits RGB rows through g_rgbrow. */
    if (img_row_alloc((uint32_t)info->width * 3u) != IMG_OK)
        return IMG_ERR_NOMEM;
    switch (info->format) {
    case IMG_FMT_BMP:  return bmp_decode(f, cb, ctx);
    case IMG_FMT_PNG:  return png_decode(f, cb, ctx);
    case IMG_FMT_JPEG: return jpg_decode(f, cb, ctx);
    case IMG_FMT_IMG:  return dimg_decode(f, cb, ctx);
    }
    return IMG_ERR_FORMAT;
}

const char *img_fmtname(int fmt)
{
    switch (fmt) {
    case IMG_FMT_BMP:  return "BMP";
    case IMG_FMT_PNG:  return "PNG";
    case IMG_FMT_JPEG: return "JPEG";
    case IMG_FMT_IMG:  return "IMG";
    }
    return "?";
}

const char *img_errstr(int err)
{
    switch (err) {
    case IMG_OK:          return "ok";
    case IMG_ERR_NOMEM:   return "out of memory";
    case IMG_ERR_FORMAT:  return "unrecognized file format";
    case IMG_ERR_TRUNC:   return "file is truncated";
    case IMG_ERR_CORRUPT: return "corrupt image data";
    case IMG_ERR_UNSUP:   return "unsupported image variant";
    case IMG_ERR_TOOBIG:  return "image too large";
    }
    return "unknown error";
}
