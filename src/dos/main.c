/* main.c - dphoto: DOS image viewer (PNG / JPEG / BMP -> VGA 256 colors).
 *
 *   DPHOTO [/320|/640] [file]     browse and view
 *   DPHOTO /dump out.ppm file     decode a file to a PPM
 *   DPHOTO /selftest              run built-in checks
 */
#include <dos.h>
#include <i86.h>
#include <string.h>
#include <stdio.h>
#include <libi86/string.h>

#include "ui.h"
#include "../img.h"
#include "../render.h"

#define MAXFILES 96

typedef struct {
    char     name[13];
    uint32_t size;
} entry_t;

static entry_t g_files[MAXFILES];
static int     g_nfiles;
static int     g_cur;
static int     g_wantmode = -1;

static uint8_t g_pal[768];
static uint8_t g_io[4096];
static uint16_t g_srcw;

/* ---- pointers and formatting ---- */

/* Far pointer to near (DGROUP) static data, for use with the DOS calls. */
static void __far *near_fp(const void *p)
{
    struct SREGS sr;

    segread(&sr);
    return (void __far *)(((unsigned long)sr.ds << 16) |
                          (unsigned long)(uint16_t)(uintptr_t)p);
}

static int u16str(uint16_t v, char *out)
{
    char tmp[6];
    int i = 0, j = 0;

    do {
        tmp[i++] = (char)('0' + v % 10);
        v = (uint16_t)(v / 10);
    } while (v);
    while (i)
        out[j++] = tmp[--i];
    out[j] = 0;
    return j;
}

static int u32str(uint32_t v, char *out)
{
    char tmp[11];
    int i = 0, j = 0;

    do {
        tmp[i++] = (char)('0' + (char)(v % 10));
        v /= 10;
    } while (v);
    while (i)
        out[j++] = tmp[--i];
    out[j] = 0;
    return j;
}

static void title_bar(const char *name, const char *sub)
{
    txt_bar(0, 0, vid_width(), 0);
    txt_str(4, 0, name, 15);
    if (sub)
        txt_str((uint16_t)(vid_width() / 2), 0, sub, 7);
    txt_bar(0, (uint16_t)(vid_height() - 8), vid_width(), 0);
}

static void footer(const char *s, uint8_t color)
{
    txt_bar(0, (uint16_t)(vid_height() - 8), vid_width(), 0);
    txt_str(4, (uint16_t)(vid_height() - 8), s, color);
}

/* ---- keyboard ---- */

static unsigned key_get(void)
{
    union REGS r;

    r.h.ah = 0x00;
    int86(0x16, &r, &r);
    if (r.h.al == 0)
        return 0x100u | r.h.ah;
    return r.h.al;
}

/* ---- files ---- */

static uint32_t g_fcap;                 /* allocated size of the file buffer */

/* Grow the file buffer so it can hold at least `need` bytes. */
static int ensure_cap(buf_t *b, uint32_t need)
{
    buf_t nb;
    uint32_t i, ncap = g_fcap ? g_fcap : 65536UL;

    if (need <= g_fcap)
        return IMG_OK;
    while (ncap < need) {
        ncap *= 2;
        if (ncap > 0x70000UL)
            ncap = 0x70000UL;
        if (ncap == 0x70000UL && need > ncap)
            return IMG_ERR_NOMEM;
    }
    if (mem_alloc(ncap, &nb) != IMG_OK)
        return IMG_ERR_NOMEM;
    for (i = 0; i < b->len; i += 2048) {
        uint32_t k = b->len - i;

        if (k > 2048)
            k = 2048;
        fget_n(b, i, g_io, k);
        fput_n(&nb, i, g_io, k);
    }
    nb.len = b->len;
    mem_free(b);
    *b = nb;
    g_fcap = ncap;
    return IMG_OK;
}

static int load_file(const char *name, buf_t *b)
{
    int h;
    unsigned got;
    uint32_t pos = 0;

    b->lin = 0;
    b->len = 0;
    g_fcap = 0;
    if (_dos_open(name, 0, &h) != 0)
        return IMG_ERR_TRUNC;
    for (;;) {
        if (ensure_cap(b, pos + (uint32_t)sizeof g_io) != IMG_OK) {
            mem_free(b);
            _dos_close(h);
            return IMG_ERR_NOMEM;
        }
        if (_dos_read(h, near_fp(g_io), (unsigned)sizeof g_io, &got) != 0)
            break;
        if (got == 0)
            break;
        fput_n(b, pos, g_io, got);
        pos += got;
        b->len = pos;
    }
    _dos_close(h);
    b->len = pos;
    return pos ? IMG_OK : IMG_ERR_TRUNC;
}

static void build_list(void)
{
    struct find_t f;
    static const char *const exts[3] = {"*.BMP", "*.PNG", "*.JPG"};
    int e;

    g_nfiles = 0;
    for (e = 0; e < 3; e++) {
        unsigned rc = _dos_findfirst(exts[e], _A_ARCH | _A_RDONLY, &f);

        while (rc == 0 && g_nfiles < MAXFILES) {
            int j = 0;

            while (f.name[j] && j < 12) {
                g_files[g_nfiles].name[j] = f.name[j];
                j++;
            }
            g_files[g_nfiles].name[j] = 0;
            g_files[g_nfiles].size = f.size;
            g_nfiles++;
            rc = _dos_findnext(&f);
        }
    }
}

static int list_index(const char *name)
{
    int i;

    for (i = 0; i < g_nfiles; i++) {
        const char *a = g_files[i].name;
        const char *b = name;
        int k = 0;

        while (a[k] && b[k] && a[k] == b[k])
            k++;
        if (!a[k] && !b[k])
            return i;
    }
    return -1;
}

/* ---- viewer ---- */

static void vblit(void *ctx, uint16_t y, uint16_t x, const uint8_t *p, uint16_t n)
{
    (void)ctx;
    vid_blit(0, (uint16_t)(y + 8), x, p, n);
}

static void row_cb(void *ctx, const uint8_t *rgb, uint16_t y)
{
    (void)y;
    render_row(ctx, rgb, y);
}

static int draw_current(void)
{
    buf_t f;
    img_info_t info;
    render_t r;
    char line[64];
    int rc, n = 0;

    title_bar(g_files[g_cur].name, 0);
    if (load_file(g_files[g_cur].name, &f) != IMG_OK) {
        footer("cannot load file", 12);
        return IMG_ERR_NOMEM;
    }
    rc = img_read_info(&f, &info);
    if (rc != IMG_OK) {
        footer(img_errstr(rc), 12);
        mem_free(&f);
        return rc;
    }

    n = u16str(info.width, line);
    line[n++] = 'x';
    n += u16str(info.height, line + n);
    line[n++] = ' ';
    line[n++] = ' ';
    {
        const char *fn = img_fmtname(info.format);
        int k = 0;

        while (fn[k] && n < 40)
            line[n++] = fn[k++];
    }
    line[n++] = ' ';
    line[n++] = ' ';
    n += u32str(g_files[g_cur].size / 1024, line + n);
    line[n++] = 'K';
    line[n] = 0;
    title_bar(g_files[g_cur].name, line);

    vid_clear(1);
    txt_bar(0, 0, vid_width(), 0);
    txt_str(4, 0, g_files[g_cur].name, 15);
    txt_str((uint16_t)(vid_width() / 2), 0, line, 7);
    render_begin(&r, info.width, info.height, vid_width(),
                 (uint16_t)(vid_height() - 8), vblit, 0);
    rc = img_decode(&f, &info, row_cb, &r);
    mem_free(&f);

    if (rc != IMG_OK)
        footer(img_errstr(rc), 12);
    else
        footer("n/p next/prev  m mode  r redraw  ESC quit", 8);
    return rc;
}

static void view_loop(void)
{
    unsigned k;

    for (;;) {
        draw_current();
        k = key_get();
        if (k == 27 || k == 'q' || k == 'Q')
            return;
        if (k == 'n' || k == ' ' || k == 0x14D || k == 'p' || k == 0x4B) {
            int dir = (k == 'p' || k == 0x4B) ? -1 : 1;

            if (g_nfiles > 1) {
                g_cur = (g_cur + dir + g_nfiles) % g_nfiles;
            }
        } else if (k == 'm' || k == 'M') {
            g_wantmode = (vid_mode() == UIMODE_13H) ? UIMODE_VESA : UIMODE_13H;
            vid_done();
            vid_init(g_wantmode);
            vid_setpal(g_pal);
        } else if (k == 'r' || k == 'R') {
            ;
        }
    }
}

/* ---- dump ---- */

static int fwrite_near(int h, const void *p, unsigned n)
{
    unsigned got;

    if (_dos_write(h, near_fp(p), n, &got) != 0)
        return -1;
    return got == n ? 0 : -1;
}

static void dump_ctx(void *ctx, const uint8_t *rgb, uint16_t y);

typedef struct {
    int h;
    int err;
} dump_t;

static dump_t g_dump;

static void dump_ctx(void *ctx, const uint8_t *rgb, uint16_t y)
{
    dump_t *d = (dump_t *)ctx;

    (void)y;
    if (!d->err && fwrite_near(d->h, rgb, (unsigned)(g_srcw * 3)) != 0)
        d->err = 1;
}

static int do_dump(const char *out, const char *in)
{
    buf_t f;
    img_info_t info;
    char hdr[40];
    int n = 0, h, rc;

    if (load_file(in, &f) != IMG_OK) {
        printf("dphoto: cannot read %s\n", in);
        return 1;
    }
    rc = img_read_info(&f, &info);
    if (rc != IMG_OK) {
        printf("dphoto: %s: %s\n", in, img_errstr(rc));
        mem_free(&f);
        return 1;
    }
    hdr[n++] = 'P';
    hdr[n++] = '6';
    hdr[n++] = '\n';
    n += u16str(info.width, hdr + n);
    hdr[n++] = ' ';
    n += u16str(info.height, hdr + n);
    hdr[n++] = '\n';
    hdr[n++] = '2';
    hdr[n++] = '5';
    hdr[n++] = '5';
    hdr[n++] = '\n';

    if (_dos_creat(out, _A_NORMAL, &h) != 0) {
        printf("dphoto: cannot create %s\n", out);
        mem_free(&f);
        return 1;
    }
    g_dump.h = h;
    g_dump.err = 0;
    if (fwrite_near(h, hdr, (unsigned)n) != 0)
        g_dump.err = 1;
    g_srcw = info.width;
    rc = img_decode(&f, &info, dump_ctx, &g_dump);
    _dos_close(h);
    mem_free(&f);
    if (rc != IMG_OK) {
        printf("dphoto: decode failed: %s\n", img_errstr(rc));
        return 1;
    }
    if (g_dump.err) {
        printf("dphoto: write failed\n");
        return 1;
    }
    printf("dphoto: wrote %s (%ux%u)\n", out, info.width, info.height);
    return 0;
}

/* ---- render capture (screenshot of the viewer pipeline to .dph) ---- */

static int do_render_dph(const char *out, const char *in)
{
    buf_t f;
    img_info_t info;
    render_t r;
    int h, rc, y, x;
    uint8_t hdr[8];
    uint8_t line[640];
    int bad = 0;

    if (load_file(in, &f) != IMG_OK) {
        printf("dphoto: cannot read %s\n", in);
        return 1;
    }
    rc = img_read_info(&f, &info);
    if (rc != IMG_OK) {
        printf("dphoto: %s: %s\n", in, img_errstr(rc));
        mem_free(&f);
        return 1;
    }
    vid_init(g_wantmode);
    render_init();
    render_palette(g_pal);
    vid_setpal(g_pal);
    vid_clear(0);
    render_begin(&r, info.width, info.height, vid_width(),
                 (uint16_t)(vid_height() - 8), vblit, 0);
    rc = img_decode(&f, &info, row_cb, &r);
    if (rc == IMG_OK)
        rc = img_decode(&f, &info, row_cb, &r); /* render twice: determinism */
    mem_free(&f);
    if (rc == IMG_OK) {
        /* same chrome as the interactive viewer, so the capture covers the
         * BIOS font path as well */
        title_bar(in, img_fmtname(info.format));
        footer("n/p next  m mode  Esc quit", 8);
    }
    if (rc != IMG_OK) {
        printf("dphoto: decode: %s\n", img_errstr(rc));
        vid_done();
        return 1;
    }
    if (_dos_creat(out, _A_NORMAL, &h) != 0) {
        printf("dphoto: cannot create %s\n", out);
        vid_done();
        return 1;
    }
    hdr[0] = 'D'; hdr[1] = 'P'; hdr[2] = 'H'; hdr[3] = '1';
    hdr[4] = (uint8_t)(vid_width() & 0xFF);
    hdr[5] = (uint8_t)(vid_width() >> 8);
    hdr[6] = (uint8_t)(vid_height() & 0xFF);
    hdr[7] = (uint8_t)(vid_height() >> 8);
    if (fwrite_near(h, hdr, 8) != 0)
        bad = 1;
    if (!bad && fwrite_near(h, g_pal, 768) != 0)
        bad = 1;
    for (y = 0; y < (int)vid_height() && !bad; y++) {
        for (x = 0; x < (int)vid_width(); x++) {
            int c = vid_getpix((uint16_t)x, (uint16_t)y);

            line[x] = (uint8_t)(c < 0 ? 0 : c);
        }
        if (fwrite_near(h, line, (unsigned)vid_width()) != 0)
            bad = 1;
    }
    _dos_close(h);
    vid_done();
    printf("dphoto: %s %ux%u -> %s (%ux%u)\n", in, info.width,
           info.height, out, vid_width(), vid_height());
    return bad;
}

/* ---- selftest ---- */

static uint32_t crc32_of(const uint8_t *p, unsigned n, uint32_t crc)
{
    unsigned i;
    int k;

    crc = ~crc;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1u)));
    }
    return ~crc;
}

static uint32_t adler32_of(const uint8_t *p, unsigned n)
{
    uint32_t a = 1, b = 0;
    unsigned i;

    for (i = 0; i < n; i++) {
        a = (a + p[i]) % 65521UL;
        b = (b + a) % 65521UL;
    }
    return (b << 16) | a;
}

static unsigned png_chunk(uint8_t *o, const char *type, const uint8_t *data,
                          uint32_t n)
{
    unsigned k = 0;
    uint32_t crc;
    uint8_t t[4];
    int i;

    o[k++] = (uint8_t)(n >> 24);
    o[k++] = (uint8_t)(n >> 16);
    o[k++] = (uint8_t)(n >> 8);
    o[k++] = (uint8_t)n;
    for (i = 0; i < 4; i++)
        t[i] = (uint8_t)type[i];
    for (i = 0; i < 4; i++)
        o[k++] = t[i];
    for (i = 0; i < (int)n; i++)
        o[k++] = data[i];
    crc = crc32_of(t, 4, 0);
    crc = crc32_of(data, n, crc);
    o[k++] = (uint8_t)(crc >> 24);
    o[k++] = (uint8_t)(crc >> 16);
    o[k++] = (uint8_t)(crc >> 8);
    o[k++] = (uint8_t)crc;
    return k;
}

/* Build a 4x2 RGB PNG with one stored deflate block. */
static unsigned build_png(uint8_t *o, const uint8_t *raw, uint32_t rawlen)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t ihdr[13];
    uint8_t z[64];
    unsigned n = 0, k = 0, i;
    uint32_t ad;

    for (i = 0; i < 8; i++)
        o[n++] = sig[i];
    ihdr[0] = 0; ihdr[1] = 0; ihdr[2] = 0; ihdr[3] = 4;   /* width 4 */
    ihdr[4] = 0; ihdr[5] = 0; ihdr[6] = 0; ihdr[7] = 2;   /* height 2 */
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    n += png_chunk(o + n, "IHDR", ihdr, 13);

    z[k++] = 0x78;              /* zlib: deflate, 32K window */
    z[k++] = 0x01;
    z[k++] = 0x01;              /* stored, final */
    z[k++] = (uint8_t)rawlen;
    z[k++] = 0;
    z[k++] = (uint8_t)~rawlen;
    z[k++] = (uint8_t)(~rawlen >> 8);
    for (i = 0; i < rawlen; i++)
        z[k++] = raw[i];
    ad = adler32_of(raw, rawlen);
    z[k++] = (uint8_t)(ad >> 24);
    z[k++] = (uint8_t)(ad >> 16);
    z[k++] = (uint8_t)(ad >> 8);
    z[k++] = (uint8_t)ad;
    n += png_chunk(o + n, "IDAT", z, k);
    n += png_chunk(o + n, "IEND", z, 0);
    return n;
}

typedef struct {
    const uint8_t *want;
    uint16_t stride;            /* bytes per expected row */
    int bad;
    int rows;
} vfy_t;

static void vfy_row(void *ctx, const uint8_t *rgb, uint16_t y)
{
    vfy_t *v = (vfy_t *)ctx;
    uint16_t i;

    for (i = 0; i < v->stride; i++)
        if (rgb[i] != v->want[(uint32_t)y * v->stride + i])
            v->bad++;
    v->rows++;
}

static void st_line(int row, const char *what, int ok)
{
    txt_str(4, (uint16_t)(row * 8), what, 15);
    txt_str(200, (uint16_t)(row * 8), ok ? "ok" : "FAIL", ok ? 10 : 12);
    printf("selftest %-22s %s\n", what, ok ? "ok" : "FAIL");
    fflush(stdout);
}

static int do_selftest(void)
{
    buf_t b;
    int fails = 0, ok, k;
    uint32_t i;
    static const uint8_t pngpx[24] = {
        255, 0, 0,   0, 255, 0,   0, 0, 255,   10, 20, 30,
        200, 100, 50, 1, 2, 3,    250, 250, 0,  0, 0, 0
    };
    static const uint8_t bmppx[45] = {
        255, 0, 0,  0, 255, 0,  0, 0, 255,  10, 20, 30,  9, 8, 7,
        200, 100, 50, 1, 2, 3, 250, 250, 0, 0, 0, 0, 6, 5, 4,
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    };
    uint8_t raw[32];
    uint8_t pngb[256];
    unsigned pngn;
    vfy_t v;
    img_info_t info;
    uint8_t pal[768];
    render_t r;

    vid_init(g_wantmode);
    render_init();
    render_palette(g_pal);
    vid_setpal(g_pal);
    vid_clear(0);
    txt_str(4, 8, "dphoto selftest", 15);

    /* 1. far memory across 64K boundaries */
    ok = mem_alloc(150000UL, &b) == IMG_OK;
    if (ok) {
        for (i = 0; i < 64; i++)
            fput(&b, 65500 + i, (uint8_t)(0xA0 + i));
        for (i = 0; i < 64; i++)
            if (fget(&b, 65500 + i) != (uint8_t)(0xA0 + i))
                ok = 0;
        fget_n(&b, 65530, g_io, 12);
        for (i = 0; i < 12; i++)
            if (g_io[i] != (uint8_t)(0xA0 + 30 + i))
                ok = 0;
        fzero(&b, 65500, 100);
        for (i = 0; i < 100; i++)
            if (fget(&b, 65500 + i) != 0)
                ok = 0;
        mem_free(&b);
    }
    st_line(3, "memory across 64K", ok);
    fails += !ok;

    /* 2. BMP decode: 5x3, 24 bpp, bottom-up */
    ok = mem_alloc(200, &b) == IMG_OK;
    if (ok) {
        uint8_t rows[3][16];
        int y, x;

        for (y = 0; y < 3; y++) {
            for (x = 0; x < 16; x++)
                rows[y][x] = 0;
            for (x = 0; x < 5; x++) {
                const uint8_t *px = &bmppx[(2 - y) * 15 + x * 3];

                rows[y][x * 3 + 0] = px[2];
                rows[y][x * 3 + 1] = px[1];
                rows[y][x * 3 + 2] = px[0];
            }
        }
        fput_n(&b, 0, "BM", 2);
        fput_n(&b, 10, "\x36\0\0\0", 4);
        fput_n(&b, 14, "\x28\0\0\0", 4);
        fput_n(&b, 18, "\x05\0\0\0", 4);
        fput_n(&b, 22, "\x03\0\0\0", 4);
        fput_n(&b, 26, "\x01\0", 2);
        fput_n(&b, 28, "\x18\0", 2);
        for (y = 0; y < 3; y++)
            fput_n(&b, (uint32_t)(54 + y * 16), rows[y], 16);
        v.want = bmppx;
        v.stride = 15;
        v.bad = 0;
        v.rows = 0;
        ok = bmp_info(&b, &info) == IMG_OK && info.width == 5 &&
             info.height == 3 && bmp_decode(&b, vfy_row, &v) == IMG_OK &&
             v.bad == 0 && v.rows == 3;
        mem_free(&b);
    }
    st_line(4, "bmp decode", ok);
    fails += !ok;

    /* 3. PNG decode: 4x2, 24 bpp, stored deflate block */
    ok = 0;
    {
        uint32_t n = 0;
        int y;

        for (y = 0; y < 2; y++) {
            raw[n++] = 0;                       /* filter: none */
            for (i = 0; i < 12; i++)
                raw[n++] = pngpx[y * 12 + i];
        }
        pngn = build_png(pngb, raw, n);
        ok = mem_alloc(pngn + 8, &b) == IMG_OK;
        if (ok) {
            fput_n(&b, 0, pngb, pngn);
            v.want = pngpx;
            v.stride = 12;
            v.bad = 0;
            v.rows = 0;
            ok = png_info(&b, &info) == IMG_OK && info.width == 4 &&
                 info.height == 2 && png_decode(&b, vfy_row, &v) == IMG_OK &&
                 v.bad == 0 && v.rows == 2;
            win_free();
            mem_free(&b);
        }
    }
    st_line(5, "png decode", ok);
    fails += !ok;

    /* 4. renderer: a solid colour maps to exactly one palette entry */
    ok = 0;
    {
        uint8_t solid[24];
        int x, y, idx = -1, good = 1;

        for (i = 0; i < 24; i += 3) {
            solid[i + 0] = 255;
            solid[i + 1] = 0;
            solid[i + 2] = 0;
        }
        render_begin(&r, 8, 8, 8, 8, vid_blit, 0);
        for (k = 0; k < 8; k++)
            render_row(&r, solid, (uint16_t)k);
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++) {
                int c = vid_getpix((uint16_t)x, (uint16_t)y);

                if (idx < 0)
                    idx = c;
                else if (c != idx)
                    good = 0;
            }
        ok = good && idx == 180;
        printf("render idx=%d good=%d\n", idx, good);
    }
    st_line(6, "render/palette", ok);
    fails += !ok;

    render_palette(pal);
    ok = 1;
    for (k = 0; k < 768; k++)
        if (pal[k] != g_pal[k])
            ok = 0;
    st_line(7, "palette stable", ok);
    fails += !ok;

    /* 5. the DAC really received the palette (6-bit values) */
    ok = 0;
    {
        uint8_t dac[3], want[3];
        int e;

        vid_getpal(180, dac);
        for (e = 0; e < 3; e++)
            want[e] = (uint8_t)(g_pal[180 * 3 + e] >> 2);
        ok = dac[0] == want[0] && dac[1] == want[1] && dac[2] == want[2];
        printf("dac %d,%d,%d want %d,%d,%d\n", dac[0], dac[1], dac[2],
               want[0], want[1], want[2]);
    }
    st_line(8, "dac programmed", ok);
    fails += !ok;

    txt_str(4, 10 * 8, fails ? "FAILED" : "ALL OK", fails ? 12 : 10);
    printf("selftest %s\n", fails ? "FAILED" : "ALL OK");
    fflush(stdout);
    vid_done();
    return fails ? 1 : 0;
}

/* ---- main ---- */

static void usage(void)
{
    printf("dphoto - DOS image viewer (BMP/PNG/JPEG)\n"
           "  DPHOTO [/320|/640] [file]   view images\n"
           "  DPHOTO /dump out.ppm file   decode to PPM\n"
           "  DPHOTO /selftest            run self test\n");
}

int main(int argc, char *argv[])
{
    int i, selftest = 0, dump = 0;
    const char *outp = 0, *inp = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (a[0] == '/' || a[0] == '-') {
            const char *o = a + 1;

            if (!strcmp(o, "selftest") || !strcmp(o, "SELFTEST") ||
                !strcmp(o, "st"))
                selftest = 1;
            else if (!strcmp(o, "render")) {
                if (i + 2 >= argc) {
                    usage();
                    return 2;
                }
                return do_render_dph(argv[i + 1], argv[i + 2]);
            } else if (!strcmp(o, "320")) {
                g_wantmode = UIMODE_13H;
            } else if (!strcmp(o, "640")) {
                g_wantmode = UIMODE_VESA;
            } else {
                usage();
                return 2;
            }
        } else {
            inp = a;
        }
    }

    if (selftest)
        return do_selftest();
    if (dump) {
        if (!outp || !inp) {
            usage();
            return 2;
        }
        return do_dump(outp, inp);
    }

    vid_init(g_wantmode);
    render_init();
    render_palette(g_pal);
    vid_setpal(g_pal);
    build_list();
    if (inp) {
        int k = list_index(inp);

        if (k >= 0)
            g_cur = k;
    }
    view_loop();
    vid_done();
    return 0;
}
