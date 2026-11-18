/* render.h - scanline scaler + fixed 6x6x6 palette with ordered dither. */
#ifndef RENDER_H
#define RENDER_H

#include "img.h"

typedef void (*blit_fn)(void *ctx, uint16_t y, uint16_t x,
                        const uint8_t *pix, uint16_t n);

typedef struct {
    uint16_t tw, th;        /* target surface */
    uint16_t sw, sh;        /* source image */
    uint16_t dx, dy;        /* top-left corner of the image area */
    uint16_t dw, dh;        /* scaled image size */
    uint16_t next_row;      /* next row of the image area to draw */
    blit_fn  blit;
    void    *blit_ctx;
} render_t;

void render_init(void);
void render_palette(uint8_t *pal);      /* 768 bytes, RGB triplets */
void render_begin(render_t *r, uint16_t sw, uint16_t sh,
                  uint16_t tw, uint16_t th, blit_fn blit, void *ctx);
void render_row(void *ctx, const uint8_t *rgb, uint16_t y);

#endif
