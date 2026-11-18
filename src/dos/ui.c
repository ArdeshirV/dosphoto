/* ui.c - VGA mode 13h / VESA 640x480x8 output and BIOS-font text. */
#include <dos.h>
#include <i86.h>
#include <conio.h>
#include <string.h>
#include <libi86/string.h>

#include "ui.h"

static uint16_t g_w = 320, g_h = 200;
static uint16_t g_stride = 320;
static uint32_t g_vbase;          /* linear address of pixel (0,0) */
static uint16_t g_winseg = 0xA000;/* segment of the visible window */
static uint32_t g_gran = 65536UL; /* bytes per bank */
static uint16_t g_bank;           /* currently selected bank */
static uint8_t  g_mode;

static uint16_t g_fseg, g_foff;   /* BIOS 8x8 font */
static uint8_t  g_font;

#define FP(s, o) ((void __far *)(((unsigned long)(s) << 16) | (unsigned long)(o)))

static void set_bank(uint16_t b)
{
    union REGS r;

    if (g_mode == UIMODE_13H || b == g_bank)
        return;
    r.w.ax = 0x4F05;
    r.w.bx = 0x0000;
    r.w.dx = b;
    int86(0x10, &r, &r);
    g_bank = b;
}

/* Write pixels starting at linear address abs, splitting on bank edges. */
static void blit_abs(uint32_t abs, const uint8_t *p, uint16_t n)
{
    while (n) {
        uint32_t off, k;
        uint16_t i;
        uint8_t __far *v;

        set_bank((uint16_t)(abs / g_gran));
        off = abs % g_gran;             /* 13h: abs & 0xFFFF */
        k = g_gran - off;
        if (k > n)
            k = n;
        v = (uint8_t __far *)FP(g_winseg, (uint16_t)off);
        for (i = 0; i < (uint16_t)k; i++)
            v[i] = p[i];
        p += k;
        abs += k;
        n = (uint16_t)(n - k);
    }
}

int vid_mode(void)
{
    return g_mode;
}

uint16_t vid_width(void)
{
    return g_w;
}

uint16_t vid_height(void)
{
    return g_h;
}

int vid_init(int want)
{
    union REGS r;
    struct SREGS sr;
    static uint8_t mi[256];
    int good = 0;

    if (want == UIMODE_VESA) {
        memset(mi, 0, sizeof mi);
        segread(&sr);
        sr.es = (uint16_t)((uintptr_t)mi >> 4);
        r.w.ax = 0x4F00;
        r.x.di = (uint16_t)((uintptr_t)mi & 0xF);
        int86x(0x10, &r, &r, &sr);
        if (r.w.ax == 0x004F) {
            sr.es = (uint16_t)((uintptr_t)mi >> 4);
            r.w.ax = 0x4F01;
            r.w.cx = 0x0101;
            r.x.di = (uint16_t)((uintptr_t)mi & 0xF);
            int86x(0x10, &r, &r, &sr);
            if (r.w.ax == 0x004F) {
                uint16_t seg = (uint16_t)(mi[0x0A] | (mi[0x0B] << 8));
                uint16_t gran = (uint16_t)(mi[0x04] | (mi[0x05] << 8));
                uint16_t wsize = (uint16_t)(mi[0x06] | (mi[0x07] << 8));
                uint16_t xsiz = (uint16_t)(mi[0x12] | (mi[0x13] << 8));
                uint16_t ysiz = (uint16_t)(mi[0x14] | (mi[0x15] << 8));
                uint16_t stride = (uint16_t)(mi[0x10] | (mi[0x11] << 8));

                if (gran != wsize)
                    gran = wsize;
                if (seg && gran && xsiz == 640 && ysiz == 480 &&
                    mi[0x19] == 8 && (mi[0x19] & 0x01)) {
                    r.w.ax = 0x4F02;
                    r.w.bx = 0x0101;
                    int86(0x10, &r, &r);
                    if (r.w.ax == 0x004F) {
                        g_winseg = seg;
                        g_gran = gran ? (uint32_t)gran << 10 : 65536UL;
                        g_w = xsiz;
                        g_h = ysiz;
                        g_stride = stride >= xsiz ? stride : xsiz;
                        g_vbase = 0;
                        g_bank = 0xFFFF;
                        g_mode = UIMODE_VESA;
                        good = 1;
                    }
                }
            }
        }
    }

    if (!good) {
        r.w.ax = 0x0013;
        int86(0x10, &r, &r);
        g_w = 320;
        g_h = 200;
        g_stride = 320;
        g_vbase = 0xA0000UL;
        g_winseg = 0xA000;
        g_gran = 65536UL;
        g_bank = 0;
        g_mode = UIMODE_13H;
    }

    /* 8x8 font for all 256 characters (BH=03h); fall back to the first
     * 128 from BH=00h if the ROM only carries those. */
    segread(&sr);
    r.w.ax = 0x1130;
    r.h.bh = 0x03;
    int86x(0x10, &r, &r, &sr);
    g_fseg = sr.es;
    g_foff = r.w.bp;
    g_font = 1;
    if (g_foff == 0 && g_fseg == 0) {
        segread(&sr);
        r.w.ax = 0x1130;
        r.h.bh = 0x00;
        int86x(0x10, &r, &r, &sr);
        g_fseg = sr.es;
        g_foff = r.w.bp;
        g_font = 0;
    }

    vid_clear(0);
    return 0;
}

void vid_done(void)
{
    union REGS r;

    r.w.ax = 0x0003;
    int86(0x10, &r, &r);
}

void vid_setpal(const uint8_t *rgb)
{
    union REGS r;
    struct SREGS sr;

    segread(&sr);
    sr.es = sr.ds;                       /* the table lives in DGROUP */
    r.w.ax = 0x1012;
    r.w.bx = 0;
    r.w.cx = 256;
    r.x.dx = (uint16_t)(uintptr_t)rgb;
    int86x(0x10, &r, &r, &sr);
}

/* Read one DAC entry straight from the VGA registers (the BIOS 1015h call is
 * not implemented everywhere, DOSBox-X included). */
int vid_getpal(uint16_t idx, uint8_t *rgb3)
{
    outp(0x3C7, (uint8_t)idx);
    rgb3[0] = inp(0x3C9);
    rgb3[1] = inp(0x3C9);
    rgb3[2] = inp(0x3C9);
    return 0;
}

void vid_clear(uint8_t color)
{
    uint8_t line[640];
    uint16_t y;
    uint16_t i;

    for (i = 0; i < g_w; i++)
        line[i] = color;
    for (y = 0; y < g_h; y++)
        blit_abs(g_vbase + (uint32_t)y * g_stride, line, g_w);
}

void vid_pixel(uint16_t x, uint16_t y, uint8_t c)
{
    uint32_t abs;
    uint8_t v = c;

    if (x >= g_w || y >= g_h)
        return;
    abs = g_vbase + (uint32_t)y * g_stride + x;
    set_bank((uint16_t)(abs / g_gran));
    *(uint8_t __far *)FP(g_winseg, (uint16_t)(abs - (uint32_t)g_bank * g_gran)) = v;
}

int vid_getpix(uint16_t x, uint16_t y)
{
    uint32_t abs;

    if (x >= g_w || y >= g_h)
        return -1;
    abs = g_vbase + (uint32_t)y * g_stride + x;
    set_bank((uint16_t)(abs / g_gran));
    return *(uint8_t __far *)FP(g_winseg, (uint16_t)(abs - (uint32_t)g_bank * g_gran));
}

void vid_blit(void *ctx, uint16_t y, uint16_t x, const uint8_t *pix, uint16_t n)
{
    (void)ctx;
    if (y >= g_h)
        return;
    if (x >= g_w)
        return;
    if ((uint16_t)(x + n) > g_w)
        n = (uint16_t)(g_w - x);
    blit_abs(g_vbase + (uint32_t)y * g_stride + x, pix, n);
}

void txt_char(uint16_t x, uint16_t y, uint8_t ch, uint8_t color)
{
    uint8_t rows[8];
    uint16_t i;

    if (x + 8 > g_w || y + 8 > g_h)
        return;
    if (g_font)
        _fmemcpy(rows, FP(g_fseg, (uint16_t)(g_foff + ch * 8u)), 8);
    else {
        for (i = 0; i < 8; i++)
            rows[i] = *(uint8_t __far *)FP(g_fseg,
                       (uint16_t)(g_foff + (ch < 128 ? ch * 8u : 0) + i));
        if (ch >= 128)
            for (i = 0; i < 8; i++)
                rows[i] = 0;
    }
    for (i = 0; i < 8; i++) {
        uint8_t bits = rows[i];
        uint8_t px[8];
        uint8_t b;

        for (b = 0; b < 8; b++)
            px[b] = (bits & (0x80u >> b)) ? color : (uint8_t)0;
        vid_blit(0, (uint16_t)(y + i), x, px, 8);
    }
}

void txt_strn(uint16_t x, uint16_t y, const char *s, uint16_t n, uint8_t color)
{
    uint16_t i;

    for (i = 0; i < n && s[i]; i++) {
        txt_char((uint16_t)(x + i * 8u), y, (uint8_t)s[i], color);
        if (x + (i + 1) * 8u + 8 > g_w)
            break;
    }
}

void txt_str(uint16_t x, uint16_t y, const char *s, uint8_t color)
{
    uint16_t n = 0;

    while (s[n])
        n++;
    txt_strn(x, y, s, n, color);
}

void txt_bar(uint16_t x, uint16_t y, uint16_t w, uint8_t color)
{
    uint8_t line[640];
    uint16_t i;

    if (w > g_w)
        w = g_w;
    for (i = 0; i < w; i++)
        line[i] = color;
    vid_blit(0, y, x, line, w);
}
