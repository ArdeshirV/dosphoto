/* png.c - PNG decoder (non-interlaced, all standard colour types).
 *
 * Deflate output is consumed row by row through inflate_run(); each
 * filtered scanline is unfiltered in place and converted to RGB.
 */
#include <string.h>
#include "img.h"

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

typedef struct {
    buf_t   *f;
    uint32_t pos;

    uint16_t width, height;
    uint8_t  depth, ctype, interlace;
    uint8_t  nch;        /* channels per pixel in the file */
    uint8_t  bppf;       /* filter byte step */
    uint16_t rowbytes;

    uint8_t  pal[768];
    uint8_t  trns[256];
    uint16_t trns_n;
    uint8_t  has_trns;

    uint8_t IMG_FAR *prev, *cur;
    uint16_t fill;
    uint16_t y;

    img_row_fn cb;
    void *ctx;

    uint32_t chunk_left;
    uint8_t  idat_done;
    uint8_t  complete;
    uint8_t  err;
} png_t;

/* Two filtered scanlines (filter byte + data) in far memory, sized to the
 * image so wide photos fit.  They are separate blocks: a 4096-pixel RGBA16
 * row is 32K, and two of them would not fit in one 64K segment. */
static buf_t g_pngprev, g_pngcur;

static int png_channels(uint8_t ctype)
{
    switch (ctype) {
    case 0: return 1;
    case 2: return 3;
    case 3: return 1;
    case 4: return 2;
    case 6: return 4;
    }
    return 0;
}

static int png_depth_ok(uint8_t ctype, uint8_t depth)
{
    switch (ctype) {
    case 0: return depth == 1 || depth == 2 || depth == 4 ||
                   depth == 8 || depth == 16;
    case 2: return depth == 8 || depth == 16;
    case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case 4: return depth == 8 || depth == 16;
    case 6: return depth == 8 || depth == 16;
    }
    return 0;
}

/* Walk the chunk list, filling in the header fields, palette and tRNS.
 * Leaves p->pos on the first IDAT chunk header. */
static int png_parse(png_t *p, img_info_t *info)
{
    uint8_t hdr[8];
    int got_ihdr = 0;
    uint32_t pos = 8;

    for (;;) {
        uint32_t len;
        uint8_t type[4];

        if (pos + 8 > p->f->len)
            return IMG_ERR_TRUNC;
        fget_n(p->f, pos, hdr, 8);
        len = be32(hdr);
        type[0] = hdr[4]; type[1] = hdr[5]; type[2] = hdr[6]; type[3] = hdr[7];
        pos += 8;
        if (len > 0x7FFFFFFFUL || pos + len + 4 > p->f->len)
            return IMG_ERR_TRUNC;

        if (type[0] == 'I' && type[1] == 'H' && type[2] == 'D' && type[3] == 'R') {
            uint8_t b[13];
            if (len < 13)
                return IMG_ERR_CORRUPT;
            fget_n(p->f, pos, b, 13);
            if (b[0] || b[1] || b[4] || b[5])
                return IMG_ERR_TOOBIG;
            p->width  = (uint16_t)(((uint16_t)b[2] << 8) | b[3]);
            p->height = (uint16_t)(((uint16_t)b[6] << 8) | b[7]);
            p->depth = b[8];
            p->ctype = b[9];
            p->interlace = b[12];
            if (b[10] != 0 || b[11] != 0)
                return IMG_ERR_CORRUPT;
            got_ihdr = 1;
        } else if (type[0] == 'P' && type[1] == 'L' && type[2] == 'T' && type[3] == 'E') {
            uint32_t n = len / 3;
            if (n > 256) n = 256;
            fget_n(p->f, pos, p->pal, n * 3);
        } else if (type[0] == 't' && type[1] == 'R' && type[2] == 'N' && type[3] == 'S') {
            uint32_t n = len;
            if (n > 256) n = 256;
            p->trns_n = (uint16_t)n;
            p->has_trns = 1;
            fget_n(p->f, pos, p->trns, n);
        } else if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') {
            p->pos = pos - 8;
            break;
        } else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') {
            return IMG_ERR_CORRUPT;
        }
        pos += len + 4;
    }

    if (!got_ihdr)
        return IMG_ERR_CORRUPT;
    p->nch = (uint8_t)png_channels(p->ctype);
    if (!p->nch || !png_depth_ok(p->ctype, p->depth))
        return IMG_ERR_UNSUP;
    if (p->interlace != 0)
        return IMG_ERR_UNSUP;
    {
        uint32_t rb = ((uint32_t)p->nch * p->depth * p->width + 7) / 8;
        if (p->width == 0 || p->height == 0)
            return IMG_ERR_CORRUPT;
        if (rb > IMG_MAX_ROW || p->width > IMG_MAX_WIDTH)
            return IMG_ERR_TOOBIG;
        p->rowbytes = (uint16_t)rb;
        p->bppf = (uint8_t)((p->nch * p->depth + 7) / 8);
        if (p->bppf == 0)
            p->bppf = 1;
    }

    info->width = p->width;
    info->height = p->height;
    info->format = IMG_FMT_PNG;
    return IMG_OK;
}

/* Advance to the next IDAT chunk; returns 1 if data is available. */
static int png_next_idat(png_t *p)
{
    for (;;) {
        uint8_t hdr[8];
        uint32_t len;

        if (p->pos + 8 > p->f->len) {
            p->idat_done = 1;
            return 0;
        }
        fget_n(p->f, p->pos, hdr, 8);
        len = be32(hdr);
        p->pos += 8;
        if (hdr[4] == 'I' && hdr[5] == 'D' && hdr[6] == 'A' && hdr[7] == 'T') {
            if (len == 0) {
                p->pos += 4;
                continue;
            }
            if (p->pos + len > p->f->len) {
                p->idat_done = 1;
                return 0;
            }
            p->chunk_left = len;
            return 1;
        }
        if (hdr[4] == 'I' && hdr[5] == 'E' && hdr[6] == 'N' && hdr[7] == 'D') {
            p->idat_done = 1;
            return 0;
        }
        if (p->pos + len + 4 > p->f->len) {
            p->idat_done = 1;
            return 0;
        }
        p->pos += len + 4;
    }
}

static int png_rd(void *ctx, uint8_t *dst, uint32_t n)
{
    png_t *p = (png_t *)ctx;
    uint32_t got = 0;

    while (got < n && !p->idat_done && !p->complete) {
        uint32_t k;

        if (p->chunk_left == 0 && !png_next_idat(p))
            break;
        k = n - got;
        if (k > p->chunk_left)
            k = p->chunk_left;
        fget_n(p->f, p->pos, dst + got, k);
        p->pos += k;
        p->chunk_left -= k;
        got += k;
        if (p->chunk_left == 0)
            p->pos += 4;              /* CRC */
    }
    return (int)got;
}

static uint8_t paeth(int a, int b, int c)
{
    int pp = a + b - c;
    int pa = pp - a; if (pa < 0) pa = -pa;
    int pb = pp - b; if (pb < 0) pb = -pb;
    int pc = pp - c; if (pc < 0) pc = -pc;

    if (pa <= pb && pa <= pc)
        return (uint8_t)a;
    if (pb <= pc)
        return (uint8_t)b;
    return (uint8_t)c;
}

static int png_unfilter(png_t *p)
{
    uint8_t IMG_FAR *cur = p->cur;
    uint8_t IMG_FAR *prev = p->prev;
    int ft = cur[0];
    uint16_t n = p->rowbytes;
    uint16_t i;
    uint8_t bpp = p->bppf;

    if (ft > 4)
        return IMG_ERR_CORRUPT;

    for (i = 0; i < n; i++) {
        int x = cur[1 + i];
        int a = (i >= bpp) ? cur[1 + i - bpp] : 0;
        int b = prev[1 + i];
        int c = (i >= bpp) ? prev[1 + i - bpp] : 0;

        switch (ft) {
        case 1: x += a; break;
        case 2: x += b; break;
        case 3: x += (a + b) >> 1; break;
        case 4: x += paeth(a, b, c); break;
        }
        cur[1 + i] = (uint8_t)x;
    }
    return IMG_OK;
}

static int png_get_sample(const uint8_t IMG_FAR *row, uint16_t idx, uint8_t depth)
{
    switch (depth) {
    case 1: return (row[idx >> 3] >> (7 - (idx & 7))) & 1;
    case 2: return (row[idx >> 2] >> (6 - 2 * (idx & 3))) & 3;
    case 4: return (row[idx >> 1] >> (4 - 4 * (idx & 1))) & 15;
    }
    return row[idx];
}

static uint8_t s16(const uint8_t IMG_FAR *p)
{
    int v = p[0] + (p[1] >> 7);
    return v > 255 ? 255 : (uint8_t)v;
}

static uint8_t png_comp(uint8_t c, uint8_t a)
{
    if (a == 255)
        return c;
    if (a == 0)
        return 0;
    return (uint8_t)(((unsigned)c * (unsigned)a) / 255u);
}

static void png_emit(png_t *p)
{
    const uint8_t IMG_FAR *row = p->cur + 1;
    uint8_t IMG_FAR *out = g_rgbrow;
    uint16_t w = p->width, x, i;

    switch (p->ctype) {
    case 0:
        if (p->depth == 8) {
            for (x = 0; x < w; x++) {
                uint8_t g = row[x];
                out[3 * x] = g; out[3 * x + 1] = g; out[3 * x + 2] = g;
            }
        } else if (p->depth == 16) {
            for (x = 0; x < w; x++) {
                uint8_t g = s16(row + 2 * x);
                out[3 * x] = g; out[3 * x + 1] = g; out[3 * x + 2] = g;
            }
        } else {
            int scale = 255 / ((1 << p->depth) - 1);
            for (x = 0; x < w; x++) {
                uint8_t g = (uint8_t)(png_get_sample(row, x, p->depth) * scale);
                out[3 * x] = g; out[3 * x + 1] = g; out[3 * x + 2] = g;
            }
        }
        break;

    case 2:
        if (p->depth == 8) {
            for (x = 0; x < w; x++) {
                out[3 * x] = row[3 * x];
                out[3 * x + 1] = row[3 * x + 1];
                out[3 * x + 2] = row[3 * x + 2];
            }
        } else {
            for (x = 0; x < w; x++) {
                out[3 * x] = s16(row + 6 * x);
                out[3 * x + 1] = s16(row + 6 * x + 2);
                out[3 * x + 2] = s16(row + 6 * x + 4);
            }
        }
        break;

    case 3:
        for (x = 0; x < w; x++) {
            int idx = png_get_sample(row, x, p->depth);
            uint8_t a = 255;
            if (p->has_trns && idx < (int)p->trns_n)
                a = p->trns[idx];
            out[3 * x]     = png_comp(p->pal[3 * idx], a);
            out[3 * x + 1] = png_comp(p->pal[3 * idx + 1], a);
            out[3 * x + 2] = png_comp(p->pal[3 * idx + 2], a);
        }
        break;

    case 4:
        if (p->depth == 8) {
            for (x = 0; x < w; x++) {
                uint8_t g = row[2 * x], a = row[2 * x + 1];
                g = png_comp(g, a);
                out[3 * x] = g; out[3 * x + 1] = g; out[3 * x + 2] = g;
            }
        } else {
            for (x = 0; x < w; x++) {
                uint8_t g = s16(row + 4 * x), a = s16(row + 4 * x + 2);
                g = png_comp(g, a);
                out[3 * x] = g; out[3 * x + 1] = g; out[3 * x + 2] = g;
            }
        }
        break;

    case 6:
        if (p->depth == 8) {
            for (x = 0; x < w; x++) {
                uint8_t a = row[4 * x + 3];
                i = (uint16_t)(3 * x);
                out[i] = png_comp(row[4 * x], a);
                out[i + 1] = png_comp(row[4 * x + 1], a);
                out[i + 2] = png_comp(row[4 * x + 2], a);
            }
        } else {
            for (x = 0; x < w; x++) {
                uint8_t a = s16(row + 8 * x + 6);
                i = (uint16_t)(3 * x);
                out[i] = png_comp(s16(row + 8 * x), a);
                out[i + 1] = png_comp(s16(row + 8 * x + 2), a);
                out[i + 2] = png_comp(s16(row + 8 * x + 4), a);
            }
        }
        break;
    }

    p->cb(p->ctx, out, p->y);
    p->y++;
}

static void png_sink(void *ctx, const uint8_t *buf, uint16_t n)
{
    png_t *p = (png_t *)ctx;

    while (n > 0 && !p->complete) {
        uint16_t room = (uint16_t)(p->rowbytes + 1 - p->fill);
        uint16_t k = (n < room) ? n : room;

        fmemcpy(p->cur + p->fill, buf, k);
        p->fill = (uint16_t)(p->fill + k);
        buf += k;
        n = (uint16_t)(n - k);
        if (p->fill == p->rowbytes + 1) {
            uint8_t IMG_FAR *t;
            p->fill = 0;
            if (png_unfilter(p) != IMG_OK) {
                p->err = 1;
                p->complete = 1;
                return;
            }
            png_emit(p);
            t = p->prev;
            p->prev = p->cur;
            p->cur = t;
            if (p->y >= p->height)
                p->complete = 1;
        }
    }
}

static void png_reset(png_t *p, buf_t *f, img_row_fn cb, void *ctx)
{
    p->f = f;
    p->pos = 0;
    p->fill = 0;
    p->y = 0;
    p->chunk_left = 0;
    p->idat_done = 0;
    p->complete = 0;
    p->err = 0;
    p->has_trns = 0;
    p->trns_n = 0;
    p->cb = cb;
    p->ctx = ctx;
    p->prev = 0;
    p->cur = 0;
}

int png_info(const buf_t *f, img_info_t *info)
{
    static png_t p;
    png_reset(&p, (buf_t *)f, 0, 0);
    return png_parse(&p, info);
}

int png_decode(const buf_t *f, img_row_fn cb, void *ctx)
{
    static png_t p;
    img_info_t info;
    int err;

    png_reset(&p, (buf_t *)f, cb, ctx);
    err = png_parse(&p, &info);
    if (err != IMG_OK)
        return err;

    /* Two filtered scanlines: filter byte + data, in far memory. */
    {
        uint32_t need = (uint32_t)p.rowbytes + 1u;

        if (g_pngprev.len < need) {
            mem_free(&g_pngprev);
            if (mem_alloc(need, &g_pngprev) != IMG_OK)
                return IMG_ERR_NOMEM;
        }
        if (g_pngcur.len < need) {
            mem_free(&g_pngcur);
            if (mem_alloc(need, &g_pngcur) != IMG_OK)
                return IMG_ERR_NOMEM;
        }
        p.prev = buf_fp(&g_pngprev);
        p.cur = buf_fp(&g_pngcur);
    }

    /* Filter type 0 ("None") treats the previous row as zero. */
    fmemset(p.prev, 0, (uint32_t)p.rowbytes + 1);

    /* Strip the 2-byte zlib header that precedes the raw DEFLATE data. */
    {
        uint8_t zh[2];
        if (png_rd(&p, zh, 2) != 2 || (zh[0] & 0x0F) != 8)
            return IMG_ERR_CORRUPT;
    }

    if (win_alloc() != 0)
        return IMG_ERR_NOMEM;
    err = inflate_run(png_rd, &p, png_sink, &p);
    win_free();
    if (p.err)
        return IMG_ERR_CORRUPT;

    if (p.complete)
        return IMG_OK;
    if (err != IMG_OK)
        return err;
    return IMG_ERR_TRUNC;
}
