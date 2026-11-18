/* mem.c - DOS platform layer: flat buffers in conventional/far memory.
 *
 * buf_t.lin is a real-mode linear address.  Every access converts it to a
 * segment:offset pair; a 64K block allocated with _dos_allocmem is
 * guaranteed paragraph aligned so a whole segment is available.
 */
#include <dos.h>
#include <libi86/string.h>

#include "../img.h"

void __far *dseg_fp(uint16_t seg, uint16_t off)
{
    return (void __far *)(((unsigned long)seg << 16) | (unsigned long)off);
}

uint8_t fget(const buf_t *b, uint32_t off)
{
    uint32_t a = b->lin + off;
    return *(uint8_t __far *)dseg_fp((uint16_t)(a >> 4), (uint16_t)(a & 0xF));
}

void fput(const buf_t *b, uint32_t off, uint8_t v)
{
    uint32_t a = b->lin + off;
    *(uint8_t __far *)dseg_fp((uint16_t)(a >> 4), (uint16_t)(a & 0xF)) = v;
}

/* Copy n bytes between far memory and a near buffer, splitting at segment
 * boundaries so each chunk has a 16-bit offset. */
void fget_n(const buf_t *b, uint32_t off, void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;

    while (n) {
        uint32_t a = b->lin + off;
        uint16_t o = (uint16_t)(a & 0xF);
        uint32_t k = 65536UL - o;

        if (k > n)
            k = n;
        _fmemcpy(d, dseg_fp((uint16_t)(a >> 4), o), (size_t)k);
        d += k;
        off += k;
        n -= k;
    }
}

void fput_n(const buf_t *b, uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;

    while (n) {
        uint32_t a = b->lin + off;
        uint16_t o = (uint16_t)(a & 0xF);
        uint32_t k = 65536UL - o;

        if (k > n)
            k = n;
        _fmemcpy(dseg_fp((uint16_t)(a >> 4), o), (void *)s, (size_t)k);
        s += k;
        off += k;
        n -= k;
    }
}

void fzero(const buf_t *b, uint32_t off, uint32_t n)
{
    while (n) {
        uint32_t a = b->lin + off;
        uint16_t o = (uint16_t)(a & 0xF);
        uint32_t k = 65536UL - o;

        if (k > n)
            k = n;
        _fmemset(dseg_fp((uint16_t)(a >> 4), o), 0, (size_t)k);
        off += k;
        n -= k;
    }
}

/* Allocate a paragraph-aligned block; DOS memory is precious on real
 * hardware so the request is rounded up to paragraphs. */
int mem_alloc(uint32_t n, buf_t *out)
{
    unsigned paras, seg;

    if (n == 0 || n > 0xFFFFUL * 16UL)
        return IMG_ERR_NOMEM;
    paras = (unsigned)((n + 15UL) >> 4);
    if (_dos_allocmem(paras, &seg) != 0) {
        out->lin = 0;
        out->len = 0;
        return IMG_ERR_NOMEM;
    }
    out->lin = (uint32_t)seg << 4;
    out->len = n;
    return IMG_OK;
}

void mem_free(buf_t *b)
{
    if (b->lin) {
        _dos_freemem((unsigned)(b->lin >> 4));
        b->lin = 0;
        b->len = 0;
    }
}

/* 32K deflate window that must not cross a 64K boundary: take one whole
 * segment, so offsets 0..32767 are valid. */
uint8_t IMG_FAR *g_win;
static unsigned g_winseg;

int win_alloc(void)
{
    if (g_win)
        return IMG_OK;
    if (_dos_allocmem(0x1000, &g_winseg) != 0)
        return IMG_ERR_NOMEM;
    g_win = (uint8_t IMG_FAR *)dseg_fp((uint16_t)g_winseg, 0);
    return IMG_OK;
}

void win_free(void)
{
    if (g_win) {
        _dos_freemem(g_winseg);
        g_win = 0;
    }
}
