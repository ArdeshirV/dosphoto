/* host_mem.c - host-side platform layer for testing the decoders.
 *
 * Big buffers live in a private arena; buf_t.lin is an offset into it so
 * the decoder code paths match the DOS build exactly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/img.h"

#define ARENA_SIZE (24u << 20)

static uint8_t *g_arena;
static uint32_t g_used;

uint8_t IMG_FAR *g_win;

static int arena_ready(void)
{
    if (!g_arena) {
        g_arena = (uint8_t *)calloc(1, ARENA_SIZE);
        if (!g_arena) {
            fprintf(stderr, "out of memory (arena)\n");
            return 0;
        }
    }
    return 1;
}

uint8_t *host_ptr(uint32_t off)
{
    return g_arena + off;
}

uint8_t fget(const buf_t *b, uint32_t off)
{
    return g_arena[b->lin + off];
}

void fput(const buf_t *b, uint32_t off, uint8_t v)
{
    g_arena[b->lin + off] = v;
}

void fget_n(const buf_t *b, uint32_t off, void *dst, uint32_t n)
{
    memcpy(dst, g_arena + b->lin + off, n);
}

void fput_n(const buf_t *b, uint32_t off, const void *src, uint32_t n)
{
    memcpy(g_arena + b->lin + off, src, n);
}

void fzero(const buf_t *b, uint32_t off, uint32_t n)
{
    memset(g_arena + b->lin + off, 0, n);
}

int mem_alloc(uint32_t n, buf_t *out)
{
    if (!arena_ready())
        return -1;
    n = (n + 15) & ~15u;
    if (g_used + n > ARENA_SIZE)
        return -1;
    out->lin = g_used;
    out->len = n;
    g_used += n;
    return 0;
}

void mem_free(buf_t *b)
{
    (void)b;
}

int win_alloc(void)
{
    buf_t b;

    if (mem_alloc(32768, &b) != 0)
        return -1;
    g_win = g_arena + b.lin;
    return 0;
}

void win_free(void)
{
    g_win = 0;
}
