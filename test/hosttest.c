/* hosttest.c - host harness for the DOS photo viewer decoders.
 *
 *   hosttest info   <file>
 *   hosttest dec    <file> <out.ppm>          full resolution decode
 *   hosttest render <file> <out.dph> [w h]    viewer pipeline (default 320x200)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/img.h"
#include "../src/render.h"

extern uint8_t *host_ptr(uint32_t off);

static int load(const char *path, buf_t *b)
{
    FILE *f = fopen(path, "rb");
    long n;

    if (!f) {
        perror(path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || mem_alloc((uint32_t)n, b) != 0) {
        fclose(f);
        return -1;
    }
    if (fread(host_ptr(b->lin), 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return -1;
    }
    fclose(f);
    b->len = (uint32_t)n;
    return 0;
}

typedef struct {
    FILE *f;
    uint16_t w, h, next;
} ppm_t;

static void ppm_row(void *ctx, const uint8_t *rgb, uint16_t y)
{
    ppm_t *p = (ppm_t *)ctx;
    (void)y;
    if (p->next < p->h) {
        fwrite(rgb, 1, (size_t)p->w * 3, p->f);
        p->next++;
    }
}

static int cmd_dec(const char *in, const char *out)
{
    buf_t f;
    img_info_t info;
    ppm_t p;
    int err;

    if (load(in, &f) != 0)
        return 1;
    err = img_read_info(&f, &info);
    if (err != IMG_OK) {
        fprintf(stderr, "%s: %s\n", in, img_errstr(err));
        return 1;
    }
    printf("%s: %s %ux%u\n", in, img_fmtname(info.format),
           info.width, info.height);
    p.f = fopen(out, "wb");
    if (!p.f) { perror(out); return 1; }
    p.w = info.width;
    p.h = info.height;
    p.next = 0;
    fprintf(p.f, "P6\n%u %u\n255\n", info.width, info.height);
    err = img_decode(&f, &info, ppm_row, &p);
    fclose(p.f);
    if (err != IMG_OK) {
        fprintf(stderr, "%s: decode: %s\n", in, img_errstr(err));
        return 1;
    }
    if (p.next != info.height) {
        fprintf(stderr, "%s: only %u of %u rows decoded\n", in, p.next, info.height);
        return 1;
    }
    return 0;
}

typedef struct {
    uint8_t *px;
    uint16_t w;
} target_t;

static void tgt_blit(void *ctx, uint16_t y, uint16_t x, const uint8_t *pix, uint16_t n)
{
    target_t *t = (target_t *)ctx;

    memcpy(t->px + (uint32_t)y * t->w + x, pix, n);
}

static int cmd_render(const char *in, const char *out, uint16_t tw, uint16_t th)
{
    buf_t f;
    img_info_t info;
    render_t r;
    target_t t;
    FILE *of;
    uint8_t pal[768];
    uint32_t n;
    int err;

    if (load(in, &f) != 0)
        return 1;
    err = img_read_info(&f, &info);
    if (err != IMG_OK) {
        fprintf(stderr, "%s: %s\n", in, img_errstr(err));
        return 1;
    }
    render_init();
    n = (uint32_t)tw * th;
    t.px = (uint8_t *)calloc(1, n);
    t.w = tw;
    if (!t.px) return 1;

    render_begin(&r, info.width, info.height, tw, th, tgt_blit, &t);
    err = img_decode(&f, &info, render_row, &r);
    if (err != IMG_OK) {
        fprintf(stderr, "%s: decode: %s\n", in, img_errstr(err));
        return 1;
    }
    render_palette(pal);
    of = fopen(out, "wb");
    if (!of) { perror(out); return 1; }
    fwrite("DPH1", 1, 4, of);
    {
        uint8_t hdr[4];
        hdr[0] = (uint8_t)(tw & 0xFF); hdr[1] = (uint8_t)(tw >> 8);
        hdr[2] = (uint8_t)(th & 0xFF); hdr[3] = (uint8_t)(th >> 8);
        fwrite(hdr, 1, 4, of);
    }
    fwrite(pal, 1, 768, of);
    fwrite(t.px, 1, n, of);
    fclose(of);
    printf("%s: %s %ux%u -> %ux%u (drawn %ux%u at %u,%u)\n", in,
           img_fmtname(info.format), info.width, info.height,
           tw, th, r.dw, r.dh, r.dx, r.dy);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s info|dec|render ...\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "info")) {
        buf_t f;
        img_info_t info;
        int err;
        if (load(argv[2], &f) != 0) return 1;
        err = img_read_info(&f, &info);
        if (err != IMG_OK) {
            printf("%s: error: %s\n", argv[2], img_errstr(err));
            return 1;
        }
        printf("%s: %s %ux%u\n", argv[2], img_fmtname(info.format),
               info.width, info.height);
        return 0;
    }
    if (!strcmp(argv[1], "dec") && argc >= 4)
        return cmd_dec(argv[2], argv[3]);
    if (!strcmp(argv[1], "render") && argc >= 4) {
        uint16_t tw = 320, th = 200;
        if (argc >= 6) {
            tw = (uint16_t)atoi(argv[4]);
            th = (uint16_t)atoi(argv[5]);
        }
        return cmd_render(argv[2], argv[3], tw, th);
    }
    fprintf(stderr, "bad arguments\n");
    return 2;
}
