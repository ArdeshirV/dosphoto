/* img.h - shared declarations for the DOS photo viewer image decoders.
 *
 * The decoders are written to run on 16-bit x86 (ia16-elf-gcc, real mode
 * DOS) but also compile unchanged on a host for testing.
 *
 * Large buffers are described by buf_t: a 32-bit "linear" handle plus a
 * length.  On DOS the handle is a real linear address below 1MB and is
 * converted to a segment:offset pair for every access (far pointer
 * arithmetic across 64K boundaries is not available on this toolchain).
 * On the host the handle is an offset into a private arena.
 */
#ifndef IMG_H
#define IMG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __ia16__
# define IMG16 1
# define IMG_FAR __far
#else
# define IMG16 0
# define IMG_FAR
#endif

/* Limits.  IMG_MAX_ROW caps the bytes of one source scanline (and of one
 * filtered PNG scanline); IMG_MAX_WIDTH caps the source width.  Scanline
 * work buffers are not static: decoders ask the platform for far memory
 * sized to the image, so the 64K data segment stays small whatever the
 * image size. */
#define IMG_MAX_ROW    32768
#define IMG_MAX_WIDTH  4096

typedef struct {
    uint32_t lin;
    uint32_t len;
} buf_t;

/* Error codes (all negative). */
#define IMG_OK          0
#define IMG_ERR_NOMEM   (-1)
#define IMG_ERR_FORMAT  (-2)
#define IMG_ERR_TRUNC   (-3)
#define IMG_ERR_CORRUPT (-4)
#define IMG_ERR_UNSUP   (-5)
#define IMG_ERR_TOOBIG  (-6)

#define IMG_FMT_NONE  0
#define IMG_FMT_BMP   1
#define IMG_FMT_PNG   2
#define IMG_FMT_JPEG  3
#define IMG_FMT_IMG   4     /* dphoto native 8-bit image ("DPH1IMG") */

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  format;
} img_info_t;

/* Row callback: rgb points at width*3 bytes of RGB data for source row y.
 * The pointer is only valid during the call.  On DOS the data usually lives
 * in far memory, so callbacks take a far pointer (IMG_FAR is empty on a
 * hosted build). */
typedef void (*img_row_fn)(void *ctx, const uint8_t IMG_FAR *rgb, uint16_t y);

/* Shared RGB scanline used by every decoder, sized by img_row_alloc(). */
extern uint8_t IMG_FAR *g_rgbrow;
int  img_row_alloc(uint32_t bytes);     /* enlarge g_rgbrow if needed */
void img_row_free(void);

/* ---- platform: flat memory access ---- */

uint8_t fget(const buf_t *b, uint32_t off);
void    fput(const buf_t *b, uint32_t off, uint8_t v);
void    fget_n(const buf_t *b, uint32_t off, void *dst, uint32_t n);
void    fput_n(const buf_t *b, uint32_t off, const void *src, uint32_t n);
void    fzero(const buf_t *b, uint32_t off, uint32_t n);

int  mem_alloc(uint32_t n, buf_t *out);
void mem_free(buf_t *b);

/* Far pointer to the start of an allocated buffer.  mem_alloc() returns a
 * paragraph-aligned block, so the whole buffer is addressable from offset 0
 * of one segment as long as it is smaller than 64K. */
uint8_t IMG_FAR *buf_fp(const buf_t *b);

/* memset/memcpy on far memory (the C library versions only take near
 * pointers on ia16). */
void fmemset(uint8_t IMG_FAR *dst, int c, uint32_t n);
void fmemcpy(uint8_t IMG_FAR *dst, const void *src, uint32_t n);
void fmem_get(uint8_t *dst, const uint8_t IMG_FAR *src, uint32_t n);

/* Copy between a buffer handle and far memory. */
void fget_far(const buf_t *b, uint32_t off, uint8_t IMG_FAR *dst, uint32_t n);
void fput_far(const buf_t *b, uint32_t off, const uint8_t IMG_FAR *src,
              uint32_t n);

/* Deflate needs a 32K sliding window.  The platform allocates a 64K block
 * and picks a 32K window that lies entirely inside one 64K segment, so the
 * decoder can index it with 16-bit offsets. */
extern uint8_t IMG_FAR *g_win;
int  win_alloc(void);
void win_free(void);

/* ---- platform: buffered byte stream over a buf_t ---- */

#define BS_SIZE 512
typedef struct {
    buf_t   *b;
    uint32_t pos;      /* absolute file offset of next unread byte */
    uint32_t end;
    uint16_t idx, cnt;
    uint8_t  buf[BS_SIZE];
} bstream_t;

void     bs_init(bstream_t *s, buf_t *b, uint32_t off, uint32_t len);
int      bs_getc(bstream_t *s);                 /* -1 at end */
uint32_t bs_read(bstream_t *s, void *dst, uint32_t n);
uint32_t bs_tell(const bstream_t *s);
void     bs_seek(bstream_t *s, uint32_t abs);
uint32_t bs_left(const bstream_t *s);

/* ---- dispatch ---- */

int  img_identify(const buf_t *f);
int  img_read_info(const buf_t *f, img_info_t *info);
int  img_decode(const buf_t *f, const img_info_t *info, img_row_fn cb, void *ctx);
const char *img_errstr(int err);
const char *img_fmtname(int fmt);

/* ---- per format (info + decode) ---- */

int bmp_info(const buf_t *f, img_info_t *info);
int bmp_decode(const buf_t *f, img_row_fn cb, void *ctx);

int png_info(const buf_t *f, img_info_t *info);
int png_decode(const buf_t *f, img_row_fn cb, void *ctx);

int jpg_info(const buf_t *f, img_info_t *info);
int jpg_decode(const buf_t *f, img_row_fn cb, void *ctx);

/* Native 8-bit image: a fixed header with a magic string, the VGA palette
 * and one palette index per pixel. */
int dimg_info(const buf_t *f, img_info_t *info);
int dimg_decode(const buf_t *f, img_row_fn cb, void *ctx);
uint32_t dimg_rows_off(void);          /* file offset of the pixel data */
#define DIMG_HDR_OFF   8               /* width/height in the header */
#define DIMG_PAL_OFF   16              /* palette offset */
#define DIMG_PAL_LEN   768             /* palette bytes */
#define DIMG_FLAG_6BIT 1               /* palette entries are 6-bit */

/* Raw deflate (zlib wrapper already stripped). */
int inflate_run(int (*rd)(void *ctx, uint8_t *dst, uint32_t n), void *rctx,
                void (*out)(void *ctx, const uint8_t *p, uint16_t n), void *octx);

#endif /* IMG_H */
