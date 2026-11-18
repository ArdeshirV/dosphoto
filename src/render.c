/* render.c - nearest-neighbour scaling to the screen plus ordered-dither
 * quantization to a fixed 6x6x6 colour cube (216 colours).
 *
 * Source rows arrive one at a time from the decoder, so the row map is
 * evaluated lazily: for every drawn row we know which source row it needs.
 */
#include "render.h"

#define MAX_OUT_W 640

static uint8_t  g_qt[16][256];
static uint16_t g_xmap[MAX_OUT_W];
static uint8_t  g_outrow[MAX_OUT_W];

static const uint8_t g_l36[6] = { 0, 36, 72, 108, 144, 180 };
static const uint8_t g_l6[6]  = { 0, 6, 12, 18, 24, 30 };
static const uint8_t g_bayer[16] = {
    0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5
};

void render_init(void)
{
    int d, v;

    for (d = 0; d < 16; d++) {
        int off = ((int)g_bayer[d] * 51) / 16 - 24;
        for (v = 0; v < 256; v++) {
            int t = v + off;
            int l;
            if (t < 0) t = 0;
            else if (t > 255) t = 255;
            l = (t * 6) >> 8;
            if (l > 5) l = 5;
            g_qt[d][v] = (uint8_t)l;
        }
    }
}

void render_palette(uint8_t *pal)
{
    int i = 0, r, g, b;

    for (r = 0; r < 6; r++)
        for (g = 0; g < 6; g++)
            for (b = 0; b < 6; b++) {
                pal[i * 3]     = (uint8_t)(r * 51);
                pal[i * 3 + 1] = (uint8_t)(g * 51);
                pal[i * 3 + 2] = (uint8_t)(b * 51);
                i++;
            }
    for (; i < 256; i++) {
        pal[i * 3] = pal[i * 3 + 1] = pal[i * 3 + 2] = 0;
    }
}

void render_begin(render_t *r, uint16_t sw, uint16_t sh,
                  uint16_t tw, uint16_t th, blit_fn blit, void *ctx)
{
    uint16_t x;

    r->tw = tw;
    r->th = th;
    r->sw = sw;
    r->sh = sh;
    r->blit = blit;
    r->blit_ctx = ctx;
    r->next_row = 0;

    if (sw == 0 || sh == 0) {
        r->dw = r->dh = 1;
    } else if ((uint32_t)sw * th <= (uint32_t)sh * tw) {
        r->dh = th;
        r->dw = (uint16_t)((uint32_t)sw * th / sh);
    } else {
        r->dw = tw;
        r->dh = (uint16_t)((uint32_t)sh * tw / sw);
    }
    if (r->dw < 1) r->dw = 1;
    if (r->dh < 1) r->dh = 1;
    if (r->dw > MAX_OUT_W) r->dw = MAX_OUT_W;
    r->dx = (uint16_t)((tw - r->dw) / 2);
    r->dy = (uint16_t)((th - r->dh) / 2);

    for (x = 0; x < r->dw; x++)
        g_xmap[x] = (uint16_t)((uint32_t)x * sw / r->dw);
}

static void render_compose(render_t *r, const uint8_t *rgb, uint16_t j)
{
    uint8_t phase = (uint8_t)((j & 3) << 2);
    uint16_t x, dw = r->dw;
    uint8_t *out = g_outrow;

    for (x = 0; x < dw; x++) {
        const uint8_t *qt = g_qt[phase + (x & 3)];
        const uint8_t *p = rgb + 3 * (uint16_t)g_xmap[x];
        uint8_t lr = qt[p[0]];
        uint8_t lg = qt[p[1]];
        uint8_t lb = qt[p[2]];
        out[x] = (uint8_t)(g_l36[lr] + g_l6[lg] + lb);
    }
    r->blit(r->blit_ctx, (uint16_t)(r->dy + j), r->dx, out, dw);
}

void render_row(void *ctx, const uint8_t *rgb, uint16_t y)
{
    render_t *r = (render_t *)ctx;

    while (r->next_row < r->dh) {
        uint32_t src = (uint32_t)r->next_row * r->sh / r->dh;

        if (src > y)
            break;
        if (src == y)
            render_compose(r, rgb, r->next_row);
        r->next_row++;
    }
}
