/* inflate.c - raw DEFLATE (RFC 1951) decompressor.
 *
 * Pull model: data is read from a callback into a small near buffer and
 * decompressed into a 32K far "window" (allocated by the platform so that
 * the whole window sits inside one 64K segment).  Finished bytes are
 * handed to an output callback in chunks.
 */
#include "img.h"


#define WSIZE  32768
#define WMASK  32767
#define STAGE  1024

typedef struct {
    uint16_t count[16];
    uint16_t symbol[288];
} huff_t;

typedef struct {
    int  (*rd)(void *ctx, uint8_t *dst, uint32_t n);
    void *rctx;
    void (*out)(void *ctx, const uint8_t *p, uint16_t n);
    void *octx;

    uint8_t  inbuf[256];
    uint16_t inpos, incnt;
    uint8_t  inend;

    uint32_t bitbuf;
    uint8_t  bitcnt;

    uint16_t wpos;
    uint16_t spos;
    uint8_t  stage[STAGE];
    uint32_t total;
    uint8_t  err;
    uint8_t IMG_FAR *win;
} inf_t;

static inf_t g_inf;
static huff_t g_lencode;
static huff_t g_distcode;

static int inf_byte(inf_t *s)
{
    if (s->inpos >= s->incnt) {
        if (s->inend)
            return -1;
        s->incnt = (uint16_t)s->rd(s->rctx, s->inbuf, sizeof(s->inbuf));
        s->inpos = 0;
        if (s->incnt == 0) {
            s->inend = 1;
            return -1;
        }
    }
    return s->inbuf[s->inpos++];
}

/* Read n (0..16) bits, LSB first.  Returns -1 when the input runs dry. */
static int inf_bits(inf_t *s, int n)
{
    uint32_t v;

    while (s->bitcnt < (uint8_t)n) {
        int c = inf_byte(s);
        if (c < 0)
            return -1;
        s->bitbuf |= (uint32_t)c << s->bitcnt;
        s->bitcnt = (uint8_t)(s->bitcnt + 8);
    }
    v = s->bitbuf;
    if (n < 32)
        v &= ((uint32_t)1 << n) - 1;
    s->bitbuf >>= n;
    s->bitcnt = (uint8_t)(s->bitcnt - n);
    return (int)v;
}

static void inf_stage_flush(inf_t *s)
{
    if (s->spos) {
        s->out(s->octx, s->stage, s->spos);
        s->spos = 0;
    }
}

static void inf_put(inf_t *s, uint8_t b)
{
    s->win[s->wpos] = b;
    s->wpos = (uint16_t)((s->wpos + 1) & WMASK);
    s->stage[s->spos++] = b;
    s->total++;
    if (s->spos == STAGE)
        inf_stage_flush(s);
}

/* dist is a 32-bit value on purpose: a 16-bit int would turn the maximum
 * deflate distance (32768) into a negative number. */
static int inf_copy(inf_t *s, uint32_t dist, int len)
{
    uint32_t src;

    if (dist == 0 || dist > WSIZE || dist > s->total)
        return IMG_ERR_CORRUPT;
    src = ((uint32_t)s->wpos + WSIZE - dist) & WMASK;
    while (len-- > 0) {
        uint8_t b = s->win[src];
        src = (src + 1) & WMASK;
        inf_put(s, b);
    }
    return IMG_OK;
}

static void huff_build(huff_t *h, const uint8_t *lengths, int n)
{
    uint16_t offs[16];
    int i;

    for (i = 0; i < 16; i++)
        h->count[i] = 0;
    for (i = 0; i < n; i++)
        h->count[lengths[i] & 15]++;
    h->count[0] = 0;
    offs[0] = offs[1] = 0;
    for (i = 1; i < 15; i++)
        offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    for (i = 0; i < n; i++)
        if (lengths[i])
            h->symbol[offs[lengths[i]]++] = (uint16_t)i;
}

/* Canonical Huffman: code and first must be wider than 16 bits, because a
 * 15-bit code plus its count reaches 2^16 and would wrap in a 16-bit int. */
static int huff_decode(inf_t *s, const huff_t *h)
{
    uint32_t len, code = 0, first = 0, index = 0;

    for (len = 1; len <= 15; len++) {
        int b = inf_bits(s, 1);

        if (b < 0)
            return -1;
        code |= (uint32_t)b;
        if (code - first < h->count[len])
            return h->symbol[index + (code - first)];
        index += h->count[len];
        first = (first + h->count[len]) << 1;
        code <<= 1;
    }
    return -1;
}

static int inf_stored(inf_t *s)
{
    uint32_t len = 0, nlen = 0;
    int i, ok = 1;

    s->bitbuf >>= (s->bitcnt & 7);
    s->bitcnt = (uint8_t)(s->bitcnt & ~7);
    /* 16-bit reads go byte by byte: a 16-bit value >= 0x8000 would look
     * negative in ia16's 16-bit int if read in one piece. */
    for (i = 0; i < 2; i++) {
        int b = inf_bits(s, 8);

        if (b < 0)
            ok = 0;
        else
            len |= (uint32_t)b << (8 * i);
    }
    for (i = 0; i < 2; i++) {
        int b = inf_bits(s, 8);

        if (b < 0)
            ok = 0;
        else
            nlen |= (uint32_t)b << (8 * i);
    }
    if (!ok)
        return IMG_ERR_TRUNC;
    if ((len ^ 0xFFFFUL) != nlen)
        return IMG_ERR_CORRUPT;
    while (len-- > 0) {
        int c = inf_byte(s);
        if (c < 0)
            return IMG_ERR_TRUNC;
        inf_put(s, (uint8_t)c);
    }
    return IMG_OK;
}

static int inf_codes(inf_t *s, const huff_t *lc, const huff_t *dc)
{
    static const uint16_t lbase[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const uint8_t lext[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const uint16_t dbase[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
        6145, 8193, 12289, 16385, 24577
    };
    static const uint8_t dext[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    int sym;

    for (;;) {
        sym = huff_decode(s, lc);
        if (sym < 0)
            return IMG_ERR_TRUNC;
        if (sym < 256) {
            inf_put(s, (uint8_t)sym);
        } else if (sym == 256) {
            return IMG_OK;
        } else {
            int len, dsym, e;
            uint32_t dist;

            sym -= 257;
            if (sym >= 29)
                return IMG_ERR_CORRUPT;
            e = inf_bits(s, lext[sym]);
            if (e < 0)
                return IMG_ERR_TRUNC;
            len = lbase[sym] + e;
            dsym = huff_decode(s, dc);
            if (dsym < 0 || dsym >= 30)
                return IMG_ERR_CORRUPT;
            e = inf_bits(s, dext[dsym]);
            if (e < 0)
                return IMG_ERR_TRUNC;
            dist = (uint32_t)dbase[dsym] + (uint32_t)e;
            {
                int r = inf_copy(s, dist, len);
                if (r != IMG_OK)
                    return r;
            }
        }
    }
}

static int inf_dynamic(inf_t *s)
{
    /* Static, not stack: the DOS stack is small and every byte counts. */
    static uint8_t clen[19];
    static uint8_t lengths[286 + 30];
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    huff_t codecode;
    int hlit, hdist, hclen, i, n, v;

    v = inf_bits(s, 5);
    if (v < 0) return IMG_ERR_TRUNC;
    hlit = v + 257;
    v = inf_bits(s, 5);
    if (v < 0) return IMG_ERR_TRUNC;
    hdist = v + 1;
    v = inf_bits(s, 4);
    if (v < 0) return IMG_ERR_TRUNC;
    hclen = v + 4;
    if (hlit > 286 || hdist > 30)
        return IMG_ERR_CORRUPT;

    for (i = 0; i < 19; i++)
        clen[i] = 0;
    for (i = 0; i < hclen; i++) {
        v = inf_bits(s, 3);
        if (v < 0) return IMG_ERR_TRUNC;
        clen[order[i]] = (uint8_t)v;
    }
    huff_build(&codecode, clen, 19);

    n = 0;
    while (n < hlit + hdist) {
        int sym = huff_decode(s, &codecode);
        int rep, val;

        if (sym < 0)
            return IMG_ERR_CORRUPT;
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
            continue;
        }
        if (sym == 16) {
            if (n == 0) return IMG_ERR_CORRUPT;
            rep = inf_bits(s, 2);
            val = lengths[n - 1];
            rep += 3;
        } else if (sym == 17) {
            rep = inf_bits(s, 3);
            val = 0;
            rep += 3;
        } else {
            rep = inf_bits(s, 7);
            val = 0;
            rep += 11;
        }
        if (rep < 0)
            return IMG_ERR_TRUNC;
        while (rep-- > 0 && n < hlit + hdist)
            lengths[n++] = (uint8_t)val;
    }

    huff_build(&g_lencode, lengths, hlit);
    huff_build(&g_distcode, lengths + hlit, hdist);
    return inf_codes(s, &g_lencode, &g_distcode);
}

static void inf_fixed_tables(void)
{
    uint8_t lengths[288];
    int i;

    for (i = 0; i < 144; i++) lengths[i] = 8;
    for (; i < 256; i++) lengths[i] = 9;
    for (; i < 280; i++) lengths[i] = 7;
    for (; i < 288; i++) lengths[i] = 8;
    huff_build(&g_lencode, lengths, 288);
    for (i = 0; i < 30; i++) lengths[i] = 5;
    huff_build(&g_distcode, lengths, 30);
}

int inflate_run(int (*rd)(void *ctx, uint8_t *dst, uint32_t n), void *rctx,
                void (*out)(void *ctx, const uint8_t *p, uint16_t n), void *octx)
{
    inf_t *s = &g_inf;
    int last = 0;

    if (!g_win)
        return IMG_ERR_NOMEM;

    s->rd = rd;
    s->rctx = rctx;
    s->out = out;
    s->octx = octx;
    s->inpos = s->incnt = 0;
    s->inend = 0;
    s->bitbuf = 0;
    s->bitcnt = 0;
    s->wpos = 0;
    s->spos = 0;
    s->total = 0;
    s->win = g_win;

    inf_fixed_tables();

    while (!last) {
        int type, r;

        last = inf_bits(s, 1);
        if (last < 0)
            return IMG_ERR_TRUNC;
        type = inf_bits(s, 2);
        if (type < 0)
            return IMG_ERR_TRUNC;
        if (type == 0)
            r = inf_stored(s);
        else if (type == 1)
            r = inf_codes(s, &g_lencode, &g_distcode);
        else if (type == 2)
            r = inf_dynamic(s);
        else
            return IMG_ERR_CORRUPT;
        if (r != IMG_OK)
            return r;
    }
    inf_stage_flush(s);
    return IMG_OK;
}
