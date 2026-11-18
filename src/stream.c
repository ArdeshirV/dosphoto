/* stream.c - buffered byte stream over a 32-bit spanned file image. */
#include "img.h"

void bs_init(bstream_t *s, buf_t *b, uint32_t off, uint32_t len)
{
    s->b = b;
    s->pos = off;
    s->end = off + len;
    if (s->end > b->len)
        s->end = b->len;
    s->idx = s->cnt = 0;
}

static int bs_fill(bstream_t *s)
{
    uint32_t n;

    if (s->pos >= s->end)
        return 0;
    n = s->end - s->pos;
    if (n > BS_SIZE)
        n = BS_SIZE;
    fget_n(s->b, s->pos, s->buf, n);
    s->pos += n;
    s->idx = 0;
    s->cnt = (uint16_t)n;
    return n != 0;
}

int bs_getc(bstream_t *s)
{
    if (s->idx >= s->cnt && !bs_fill(s))
        return -1;
    return s->buf[s->idx++];
}

uint32_t bs_read(bstream_t *s, void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint32_t got = 0;

    while (got < n) {
        uint32_t k;
        if (s->idx >= s->cnt && !bs_fill(s))
            break;
        k = (uint32_t)(s->cnt - s->idx);
        if (k > n - got)
            k = n - got;
        {
            uint32_t i;
            for (i = 0; i < k; i++)
                d[got + i] = s->buf[s->idx + i];
        }
        s->idx = (uint16_t)(s->idx + k);
        got += k;
    }
    return got;
}

uint32_t bs_tell(const bstream_t *s)
{
    return s->pos - (uint32_t)(s->cnt - s->idx);
}

void bs_seek(bstream_t *s, uint32_t abs)
{
    s->pos = abs;
    s->idx = s->cnt = 0;
}

uint32_t bs_left(const bstream_t *s)
{
    return s->end - bs_tell(s);
}
