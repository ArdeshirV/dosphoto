/* ui.h - video output, BIOS text and simple drawing for the DOS viewer. */
#ifndef UI_H
#define UI_H

#include <stdint.h>

/* Modes */
#define UIMODE_13H   0      /* 320x200x256 */
#define UIMODE_VESA  1      /* 640x480x256 */

int      vid_init(int want);        /* -1 auto */
void     vid_done(void);
uint16_t vid_width(void);
uint16_t vid_height(void);
int      vid_mode(void);

void vid_clear(uint8_t color);
void vid_setpal(const uint8_t *rgb /* 768 bytes */);
int  vid_getpal(uint16_t idx, uint8_t *rgb3);   /* read back one DAC entry */

/* Blit n pixels at (x,y); matches render.h blit_fn. */
void vid_blit(void *ctx, uint16_t y, uint16_t x, const uint8_t *pix, uint16_t n);

int  vid_getpix(uint16_t x, uint16_t y);
void vid_pixel(uint16_t x, uint16_t y, uint8_t c);

void txt_char(uint16_t x, uint16_t y, uint8_t ch, uint8_t color);
void txt_str(uint16_t x, uint16_t y, const char *s, uint8_t color);
void txt_strn(uint16_t x, uint16_t y, const char *s, uint16_t n, uint8_t color);
void txt_bar(uint16_t x, uint16_t y, uint16_t w, uint8_t color);

#endif
