/* jpeg.c - baseline / extended sequential Huffman JPEG decoder.
 *
 * Supports 8-bit precision, 1 or 3 components, sampling factors up to 4x4,
 * restart intervals, DQT/DHT and interleaved scans.  Progressive JPEG and
 * arithmetic coding are reported as unsupported.
 */
#include <string.h>
#include "img.h"

typedef struct {
    uint8_t  bits[17];
    uint8_t  vals[256];
    uint16_t mincode[17];
    int16_t  valptr[17];
    int32_t  maxcode[18];
    uint8_t  present;
} jhuff_t;

typedef struct {
    uint8_t  id, hs, vs, tq, td, ta;
    uint16_t bw;        /* blocks across one band row */
    uint16_t pw;        /* plane samples per row */
    int32_t  pred;
    buf_t    plane;
    uint8_t  rowloaded;
} jcomp_t;

typedef struct {
    bstream_t bs;
    uint16_t width, height;
    uint8_t  ncomp;
    jcomp_t  comp[3];
    uint16_t hmax, vmax;
    uint16_t mcus_x, mcus_y;
    uint16_t restart_interval;
    uint16_t mcu_count;
    uint16_t qt[4][64];
    uint8_t  qt_present[4];
    jhuff_t  dc[4];
    jhuff_t  ac[4];
    uint8_t  bitbuf, bitcnt;
    uint8_t  hit_marker, marker;
    uint32_t bits_read;
    int32_t  coef[64];
    int16_t  tab_r[256], tab_g1[256], tab_g2[256], tab_b[256];
    uint16_t xmap[2][IMG_MAX_WIDTH];
    uint8_t  rowloaded[3];
    uint8_t  scan_idx[3];
} jdec_t;

static jdec_t g_jd;
static uint8_t g_jrows[3][IMG_MAX_WIDTH + 64];

static const uint8_t zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* ---------------------------------------------------------------- bits */

static int jbyte(jdec_t *d)
{
    int c;

    if (d->hit_marker)
        return -1;
    c = bs_getc(&d->bs);
    if (c < 0) {
        d->hit_marker = 1;
        return -1;
    }
    if (c == 0xFF) {
        int c2 = bs_getc(&d->bs);
        if (c2 == 0)
            return 0xFF;
        if (c2 < 0) {
            d->hit_marker = 1;
            return -1;
        }
        d->marker = (uint8_t)c2;
        d->hit_marker = 1;
        bs_seek(&d->bs, bs_tell(&d->bs) - 1);
        return -1;
    }
    return c;
}

static int jbits(jdec_t *d, int n)
{
    int v = 0;

    while (n > 0) {
        int take;
        if (d->bitcnt == 0) {
            int c = jbyte(d);
            if (c < 0)
                return -1;
            d->bitbuf = (uint8_t)c;
            d->bitcnt = 8;
        }
        take = (n < (int)d->bitcnt) ? n : (int)d->bitcnt;
        v = (v << take) |
            ((d->bitbuf >> (d->bitcnt - take)) & ((1 << take) - 1));
        d->bitcnt = (uint8_t)(d->bitcnt - take);
        n -= take;
    }
    return v;
}

static int jhuff_decode(jdec_t *d, const jhuff_t *h)
{
    int len, code = 0;

    for (len = 1; len <= 16; len++) {
        int b = jbits(d, 1);
        if (b < 0)
            return -1;
        code = (code << 1) | b;
        if (code <= h->maxcode[len])
            return h->vals[h->valptr[len] + code - h->mincode[len]];
    }
    return -1;
}

static int extend(int v, int n)
{
    if (n == 0)
        return 0;
    if (v < (1 << (n - 1)))
        v -= (1 << n) - 1;
    return v;
}

/* ------------------------------------------------------------ huffman */

static void huff_build(jhuff_t *h)
{
    int i, k = 0;
    uint32_t code = 0;

    for (i = 1; i <= 16; i++) {
        h->valptr[i] = (int16_t)k;
        h->mincode[i] = (uint16_t)code;
        if (h->bits[i]) {
            code += h->bits[i];
            h->maxcode[i] = (int32_t)code - 1;
        } else {
            h->maxcode[i] = -1;
        }
        k += h->bits[i];
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFFL;
    h->present = 1;
}

/* --------------------------------------------------------------- idct */

#define FIX_0_298631336  1223
#define FIX_0_390180644   1598
#define FIX_0_541196100   2217
#define FIX_0_765366865   3135
#define FIX_0_899976223   3686
#define FIX_1_175875602   4816
#define FIX_1_501321110   6149
#define FIX_1_847759065   7568
#define FIX_1_961570560   8035
#define FIX_2_053119869   8410
#define FIX_2_562915447  10497
#define FIX_3_072711026  12586

static uint8_t clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static void idct8x8(const int16_t *in, uint8_t *out)
{
    static int32_t val[64];
    int i;
    const int16_t *d = in;
    int32_t *v = val;

    for (i = 0; i < 8; i++, d++, v++) {
        int32_t t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;

        if (d[8] == 0 && d[16] == 0 && d[24] == 0 && d[32] == 0 &&
            d[40] == 0 && d[48] == 0 && d[56] == 0) {
            int dc = d[0] * 4;
            v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dc;
            continue;
        }

        p2 = d[16];
        p3 = d[48];
        p1 = (p2 + p3) * FIX_0_541196100;
        t2 = p1 + p3 * (-FIX_1_847759065);
        t3 = p1 + p2 * FIX_0_765366865;

        p2 = d[0];
        p3 = d[32];
        t0 = (p2 + p3) << 12;
        t1 = (p2 - p3) << 12;

        x0 = t0 + t3;
        x3 = t0 - t3;
        x1 = t1 + t2;
        x2 = t1 - t2;

        t0 = d[56];
        t1 = d[40];
        t2 = d[24];
        t3 = d[8];

        p3 = t0 + t2;
        p4 = t1 + t3;
        p1 = t0 + t3;
        p2 = t1 + t2;
        p5 = (p3 + p4) * FIX_1_175875602;

        t0 = t0 * FIX_0_298631336;
        t1 = t1 * FIX_2_053119869;
        t2 = t2 * FIX_3_072711026;
        t3 = t3 * FIX_1_501321110;
        p1 = p5 + p1 * (-FIX_0_899976223);
        p2 = p5 + p2 * (-FIX_2_562915447);
        p3 = p3 * (-FIX_1_961570560);
        p4 = p4 * (-FIX_0_390180644);

        t3 += p1 + p4;
        t2 += p2 + p3;
        t1 += p2 + p4;
        t0 += p1 + p3;

        v[0]  = (x0 + t3 + 512) >> 10;
        v[56] = (x0 - t3 + 512) >> 10;
        v[8]  = (x1 + t2 + 512) >> 10;
        v[48] = (x1 - t2 + 512) >> 10;
        v[16] = (x2 + t1 + 512) >> 10;
        v[40] = (x2 - t1 + 512) >> 10;
        v[24] = (x3 + t0 + 512) >> 10;
        v[32] = (x3 - t0 + 512) >> 10;
    }

    v = val;
    for (i = 0; i < 8; i++, v += 8) {
        int32_t t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;
        uint8_t *o = out + i * 8;

        if (v[1] == 0 && v[2] == 0 && v[3] == 0 && v[4] == 0 &&
            v[5] == 0 && v[6] == 0 && v[7] == 0) {
            uint8_t dc = clamp_u8((int)(((long)v[0] * 4096L + 16842752L) >> 17));
            o[0] = o[1] = o[2] = o[3] = o[4] = o[5] = o[6] = o[7] = dc;
            continue;
        }

        p2 = v[2];
        p3 = v[6];
        p1 = (p2 + p3) * FIX_0_541196100;
        t2 = p1 + p3 * (-FIX_1_847759065);
        t3 = p1 + p2 * FIX_0_765366865;

        p2 = v[0];
        p3 = v[4];
        t0 = (p2 + p3) << 12;
        t1 = (p2 - p3) << 12;

        x0 = t0 + t3;
        x3 = t0 - t3;
        x1 = t1 + t2;
        x2 = t1 - t2;

        t0 = v[7];
        t1 = v[5];
        t2 = v[3];
        t3 = v[1];

        p3 = t0 + t2;
        p4 = t1 + t3;
        p1 = t0 + t3;
        p2 = t1 + t2;
        p5 = (p3 + p4) * FIX_1_175875602;

        t0 = t0 * FIX_0_298631336;
        t1 = t1 * FIX_2_053119869;
        t2 = t2 * FIX_3_072711026;
        t3 = t3 * FIX_1_501321110;
        p1 = p5 + p1 * (-FIX_0_899976223);
        p2 = p5 + p2 * (-FIX_2_562915447);
        p3 = p3 * (-FIX_1_961570560);
        p4 = p4 * (-FIX_0_390180644);

        t3 += p1 + p4;
        t2 += p2 + p3;
        t1 += p2 + p4;
        t0 += p1 + p3;

        x0 += 65536L + (128L << 17);
        x1 += 65536L + (128L << 17);
        x2 += 65536L + (128L << 17);
        x3 += 65536L + (128L << 17);

        o[0] = clamp_u8((x0 + t3) >> 17);
        o[7] = clamp_u8((x0 - t3) >> 17);
        o[1] = clamp_u8((x1 + t2) >> 17);
        o[6] = clamp_u8((x1 - t2) >> 17);
        o[2] = clamp_u8((x2 + t1) >> 17);
        o[5] = clamp_u8((x2 - t1) >> 17);
        o[3] = clamp_u8((x3 + t0) >> 17);
        o[4] = clamp_u8((x3 - t0) >> 17);
    }
}

/* ------------------------------------------------------------- markers */

static int rd16(jdec_t *d)
{
    int a = bs_getc(&d->bs);
    int b = bs_getc(&d->bs);
    if (a < 0 || b < 0)
        return -1;
    return (a << 8) | b;
}

static int skip_bytes(jdec_t *d, int n)
{
    while (n-- > 0)
        if (bs_getc(&d->bs) < 0)
            return -1;
    return 0;
}

static int next_marker(jdec_t *d)
{
    int c;

    for (;;) {
        c = bs_getc(&d->bs);
        if (c < 0)
            return -1;
        if (c != 0xFF)
            continue;
        do {
            c = bs_getc(&d->bs);
        } while (c == 0xFF);
        if (c < 0)
            return -1;
        if (c == 0)
            continue;
        return c;
    }
}

static int parse_dqt(jdec_t *d, int len)
{
    while (len > 0) {
        int pq, tq, i;
        int c = bs_getc(&d->bs);
        if (c < 0) return IMG_ERR_TRUNC;
        pq = c >> 4;
        tq = c & 15;
        len--;
        if (tq > 3) return IMG_ERR_CORRUPT;
        for (i = 0; i < 64; i++) {
            int v;
            if (pq) {
                v = rd16(d);
                len -= 2;
            } else {
                v = bs_getc(&d->bs);
                len--;
            }
            if (v < 0) return IMG_ERR_TRUNC;
            d->qt[tq][zigzag[i]] = (uint16_t)v;
        }
        d->qt_present[tq] = 1;
    }
    return IMG_OK;
}

static int parse_dht(jdec_t *d, int len)
{
    while (len > 2) {
        int tc, th, i, total = 0;
        jhuff_t *h;
        int c = bs_getc(&d->bs);
        if (c < 0) return IMG_ERR_TRUNC;
        len--;
        tc = c >> 4;
        th = c & 15;
        if (th > 3 || tc > 1) return IMG_ERR_CORRUPT;
        h = tc ? &d->ac[th] : &d->dc[th];
        for (i = 1; i <= 16; i++) {
            int v = bs_getc(&d->bs);
            if (v < 0) return IMG_ERR_TRUNC;
            h->bits[i] = (uint8_t)v;
            total += v;
            len--;
        }
        if (total > 256)
            return IMG_ERR_CORRUPT;
        for (i = 0; i < total; i++) {
            int v = bs_getc(&d->bs);
            if (v < 0) return IMG_ERR_TRUNC;
            h->vals[i] = (uint8_t)v;
            len--;
        }
        huff_build(h);
    }
    return IMG_OK;
}

static int parse_sof(jdec_t *d, int len)
{
    int prec, i;

    if (len < 6)
        return IMG_ERR_CORRUPT;
    prec = bs_getc(&d->bs);
    if (prec != 8)
        return IMG_ERR_UNSUP;
    d->height = (uint16_t)rd16(d);
    d->width = (uint16_t)rd16(d);
    d->ncomp = (uint8_t)bs_getc(&d->bs);
    len -= 6;
    if (d->ncomp != 1 && d->ncomp != 3)
        return IMG_ERR_UNSUP;
    if (d->width == 0 || d->height == 0)
        return IMG_ERR_CORRUPT;
    if (d->width > IMG_MAX_WIDTH)
        return IMG_ERR_TOOBIG;

    d->hmax = d->vmax = 1;
    for (i = 0; i < d->ncomp; i++) {
        int c1, c2, c3;
        if (len < 3)
            return IMG_ERR_TRUNC;
        c1 = bs_getc(&d->bs);
        c2 = bs_getc(&d->bs);
        c3 = bs_getc(&d->bs);
        len -= 3;
        if (c1 < 0 || c2 < 0 || c3 < 0)
            return IMG_ERR_TRUNC;
        d->comp[i].id = (uint8_t)c1;
        d->comp[i].hs = (uint8_t)(c2 >> 4);
        d->comp[i].vs = (uint8_t)(c2 & 15);
        d->comp[i].tq = (uint8_t)c3;
        if (d->comp[i].hs < 1 || d->comp[i].hs > 4 ||
            d->comp[i].vs < 1 || d->comp[i].vs > 4 ||
            d->comp[i].tq > 3)
            return IMG_ERR_UNSUP;
        if (d->comp[i].hs > d->hmax) d->hmax = d->comp[i].hs;
        if (d->comp[i].vs > d->vmax) d->vmax = d->comp[i].vs;
    }
    if (d->hmax > 4 || d->vmax > 4)
        return IMG_ERR_UNSUP;
    if (d->hmax * d->vmax > 10)
        return IMG_ERR_UNSUP;

    d->mcus_x = (uint16_t)(((uint32_t)d->width + 8 * d->hmax - 1) / (8 * d->hmax));
    d->mcus_y = (uint16_t)(((uint32_t)d->height + 8 * d->vmax - 1) / (8 * d->vmax));
    for (i = 0; i < d->ncomp; i++) {
        d->comp[i].bw = (uint16_t)(d->mcus_x * d->comp[i].hs);
        d->comp[i].pw = (uint16_t)(d->comp[i].bw * 8);
    }
    return IMG_OK;
}

static int parse_sos(jdec_t *d, int len)
{
    int ns, i;

    ns = bs_getc(&d->bs);
    len--;
    if (ns < 1 || ns > d->ncomp)
        return IMG_ERR_CORRUPT;
    for (i = 0; i < ns; i++) {
        int cs = bs_getc(&d->bs);
        int tbl = bs_getc(&d->bs);
        int j;
        if (cs < 0 || tbl < 0)
            return IMG_ERR_TRUNC;
        for (j = 0; j < d->ncomp; j++) {
            if (d->comp[j].id == (uint8_t)cs) {
                d->comp[j].td = (uint8_t)(tbl >> 4);
                d->comp[j].ta = (uint8_t)(tbl & 15);
                d->scan_idx[i] = (uint8_t)j;
                break;
            }
        }
        if (j == d->ncomp)
            return IMG_ERR_CORRUPT;
        len -= 2;
    }
    i = bs_getc(&d->bs);   /* Ss */
    if (i != 0)
        return IMG_ERR_UNSUP;
    i = bs_getc(&d->bs);   /* Se */
    if (i != 63)
        return IMG_ERR_UNSUP;
    i = bs_getc(&d->bs);   /* Ah/Al */
    if (i != 0)
        return IMG_ERR_UNSUP;
    return IMG_OK;
}

/* Parse headers up to (and including) the first SOS. */
static int jpg_parse(jdec_t *d, const buf_t *f, img_info_t *info)
{
    int m, got_sof = 0;

    memset(d, 0, sizeof(*d));
    bs_init(&d->bs, (buf_t *)f, 0, f->len);

    m = next_marker(d);
    if (m != 0xD8)
        return IMG_ERR_FORMAT;

    for (;;) {
        int len;

        m = next_marker(d);
        if (m < 0)
            return IMG_ERR_TRUNC;
        if (m == 0xD9)
            return IMG_ERR_CORRUPT;      /* EOI before SOS */
        if (m >= 0xD0 && m <= 0xD7)
            continue;
        if (m == 0x01)
            continue;
        len = rd16(d);
        if (len < 2)
            return IMG_ERR_CORRUPT;
        len -= 2;

        switch (m) {
        case 0xDB: { int r = parse_dqt(d, len); if (r) return r; break; }
        case 0xC4: { int r = parse_dht(d, len); if (r) return r; break; }
        case 0xC0:
        case 0xC1: { int r = parse_sof(d, len); if (r) return r; got_sof = 1; break; }
        case 0xC2:
        case 0xC3:
        case 0xC5:
        case 0xC6:
        case 0xC7:
        case 0xC9:
        case 0xCA:
        case 0xCB:
        case 0xCD:
        case 0xCE:
        case 0xCF:
            return IMG_ERR_UNSUP;
        case 0xDD:
            d->restart_interval = (uint16_t)rd16(d);
            break;
        case 0xDA: {
            int r;
            if (!got_sof)
                return IMG_ERR_CORRUPT;
            r = parse_sos(d, len);
            if (r)
                return r;
            info->width = d->width;
            info->height = d->height;
            info->format = IMG_FMT_JPEG;
            return IMG_OK;
        }
        default:
            if (skip_bytes(d, len) < 0)
                return IMG_ERR_TRUNC;
            break;
        }
    }
}

/* -------------------------------------------------------------- decode */

static int restart_sync(jdec_t *d)
{
    int i;

    d->bitcnt = 0;
    for (;;) {
        int c = bs_getc(&d->bs);
        if (c < 0)
            return -1;
        if (c == 0xFF) {
            int c2 = bs_getc(&d->bs);
            if (c2 == 0xFF)
                continue;
            if (c2 >= 0xD0 && c2 <= 0xD7)
                break;
            return -1;
        }
    }
    for (i = 0; i < d->ncomp; i++)
        d->comp[i].pred = 0;
    d->hit_marker = 0;
    d->marker = 0;
    return 0;
}

static int decode_block(jdec_t *d, const jhuff_t *dc, const jhuff_t *ac,
                        const uint16_t *qt, int32_t *pred)
{
    int t, k, s;

    memset(d->coef, 0, sizeof(d->coef));

    t = jhuff_decode(d, dc);
    if (t < 0)
        return -1;
    s = t;
    if (s > 16)
        return -1;
    t = jbits(d, s);
    if (t < 0)
        return -1;
    *pred += extend(t, s);
    d->coef[0] = *pred * (int32_t)qt[0];

    k = 1;
    while (k < 64) {
        int rs = jhuff_decode(d, ac);
        int r, sz;

        if (rs < 0)
            return -1;
        r = rs >> 4;
        sz = rs & 15;
        if (sz == 0) {
            if (r == 15) {
                k += 16;
                continue;
            }
            break;
        }
        k += r;
        if (k > 63)
            break;
        t = jbits(d, sz);
        if (t < 0)
            return -1;
        d->coef[zigzag[k]] = (int32_t)extend(t, sz) * (int32_t)qt[zigzag[k]];
        k++;
    }
    return 0;
}

static void store_block(jdec_t *d, jcomp_t *c, int bx, int by)
{
    int16_t tmp[64];
    uint8_t blk[64];
    int i;

    for (i = 0; i < 64; i++) {
        int32_t v = d->coef[i];
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        tmp[i] = (int16_t)v;
    }
    idct8x8(tmp, blk);
    for (i = 0; i < 8; i++)
        fput_n(&c->plane, (uint32_t)(by + i) * c->pw + (uint32_t)bx,
               blk + i * 8, 8);

}

static void build_tabs(jdec_t *d)
{
    int i;

    for (i = 0; i < 256; i++) {
        int x = i - 128;
        d->tab_r[i]  = (int16_t)((91881L * x) >> 16);
        d->tab_g1[i] = (int16_t)((22554L * x) >> 16);
        d->tab_g2[i] = (int16_t)((46802L * x) >> 16);
        d->tab_b[i]  = (int16_t)((116130L * x) >> 16);
    }
}

static void emit_row(jdec_t *d)
{
    uint8_t *out = g_rgbrow;
    uint16_t w = d->width, x;

    if (d->ncomp == 1) {
        const uint8_t *yp = g_jrows[0];
        for (x = 0; x < w; x++) {
            uint8_t g = yp[x];
            out[3 * x] = g; out[3 * x + 1] = g; out[3 * x + 2] = g;
        }
        return;
    }
    {
        const uint8_t *yp = g_jrows[0];
        const uint8_t *cb = g_jrows[1];
        const uint8_t *cr = g_jrows[2];
        const uint16_t *mx1 = d->xmap[0];
        const uint16_t *mx2 = d->xmap[1];

        for (x = 0; x < w; x++) {
            int y = yp[x];
            int u = cb[mx1[x]];
            int v = cr[mx2[x]];
            int r = y + d->tab_r[v];
            int g = y - d->tab_g1[u] - d->tab_g2[v];
            int b = y + d->tab_b[u];
            out[3 * x] = clamp_u8(r);
            out[3 * x + 1] = clamp_u8(g);
            out[3 * x + 2] = clamp_u8(b);
        }
    }
}

int jpg_info(const buf_t *f, img_info_t *info)
{
    return jpg_parse(&g_jd, f, info);
}

int jpg_decode(const buf_t *f, img_row_fn cb, void *ctx)
{
    jdec_t *d = &g_jd;
    img_info_t info;
    int err, i, ci;
    int bandrows, my, rows, stop;
    uint32_t mx;

    err = jpg_parse(d, f, &info);
    if (err != IMG_OK)
        return err;

    build_tabs(d);

    for (i = 0; i < d->ncomp; i++) {
        uint32_t bytes = (uint32_t)d->comp[i].pw * d->comp[i].vs * 8;
        d->comp[i].pred = 0;
        d->comp[i].plane.lin = 0;
        d->comp[i].plane.len = 0;
        if (mem_alloc(bytes, &d->comp[i].plane) != 0) {
            int j;
            for (j = 0; j < i; j++)
                mem_free(&d->comp[j].plane);
            return IMG_ERR_NOMEM;
        }
        fzero(&d->comp[i].plane, 0, d->comp[i].plane.len);
        if (d->comp[i].tq && !d->qt_present[d->comp[i].tq]) {
            /* keep going: quant table may still be valid */
        }
        if (!d->qt_present[d->comp[i].tq]) {
            int j;
            for (j = 0; j <= i; j++)
                mem_free(&d->comp[j].plane);
            return IMG_ERR_CORRUPT;
        }
        if (!d->dc[d->comp[i].td].present || !d->ac[d->comp[i].ta].present) {
            int j;
            for (j = 0; j <= i; j++)
                mem_free(&d->comp[j].plane);
            return IMG_ERR_CORRUPT;
        }
    }

    for (ci = 1; ci < d->ncomp; ci++) {
        uint16_t x;
        for (x = 0; x < d->width; x++)
            d->xmap[ci - 1][x] =
                (uint16_t)((uint32_t)x * d->comp[ci].hs / d->hmax);
    }

    bandrows = d->vmax * 8;
    d->mcu_count = 0;
    d->hit_marker = 0;
    d->marker = 0;
    d->bitcnt = 0;
    d->bitbuf = 0;

    stop = 0;
    rows = 0;
    for (my = 0; my < d->mcus_y && !stop; my++) {
        int ry;

        for (ci = 0; ci < d->ncomp; ci++)
            d->rowloaded[ci] = 0xFF;

        for (mx = 0; mx < d->mcus_x; mx++) {
            if (d->restart_interval && d->mcu_count &&
                (d->mcu_count % d->restart_interval) == 0) {
                if (restart_sync(d) < 0) {
                    stop = 1;
                    break;
                }
            }
            for (ci = 0; ci < d->ncomp; ci++) {
                jcomp_t *c = &d->comp[d->scan_idx[ci]];
                int hx, vy;
                for (vy = 0; vy < c->vs && !stop; vy++) {
                    for (hx = 0; hx < c->hs; hx++) {
                        int bx = (int)((mx * c->hs + hx) * 8);
                        int by = vy * 8;
                        if (decode_block(d, &d->dc[c->td], &d->ac[c->ta],
                                         d->qt[c->tq], &c->pred) < 0) {
                            stop = 1;
                            break;
                        }
                        store_block(d, c, bx, by);
                    }
                }
                if (stop)
                    break;
            }
            if (stop)
                break;
            d->mcu_count++;
        }

        for (ry = 0; ry < bandrows; ry++) {
            int ypix = my * bandrows + ry;
            if (ypix >= d->height)
                break;
            for (ci = 0; ci < d->ncomp; ci++) {
                uint8_t crow = (uint8_t)((ry * d->comp[ci].vs) / d->vmax);
                if (d->rowloaded[ci] != crow) {
                    fget_n(&d->comp[ci].plane,
                           (uint32_t)crow * d->comp[ci].pw,
                           g_jrows[ci], d->comp[ci].pw);
                    d->rowloaded[ci] = crow;
                }
            }
            emit_row(d);
            cb(ctx, g_rgbrow, (uint16_t)ypix);
            rows++;
        }
    }

    for (i = 0; i < d->ncomp; i++)
        mem_free(&d->comp[i].plane);
    if (rows < (int)d->height)
        return IMG_ERR_TRUNC;
    return IMG_OK;
}
