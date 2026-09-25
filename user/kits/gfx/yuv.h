/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_GFX_YUV_H
#define KOSMOS_GFX_YUV_H

/*
 * **A camera's YUY2 into the screen's pixels** (`roadmap.md` 6d).
 *
 * YUY2 is 4:2:2: two pixels in four bytes, Y0 U Y1 V - each its own
 * brightness, sharing one colour. A webcam's is BT.601 in studio range
 * (brightness 16 to 235, colour 16 to 240 about 128), which is what a C920
 * sends and what UVC's uncompressed payload means unless a colour-matching
 * descriptor says otherwise.
 *
 * Its own file, with no Lua and nothing of the kit's, for the reason
 * `shadow.c` is: a loop over every pixel of every frame, thirty times a
 * second, held on the Mac to a reference that does it the slow, obvious way
 * (`tools/test_yuv.c`).
 */

#include <stdbool.h>
#include <stdint.h>

/*
 * `width` by `height` pixels of YUY2 at `src`, `width * 2` bytes a row, into
 * `dst`, `pitch` bytes a row, as 0xffRRGGBB. `width` is even; an odd one
 * converts one fewer column. `mirror` flips it left for right, which is how
 * a person expects to see themselves.
 */
void gfx_yuy2(uint32_t *dst, unsigned long pitch, const uint8_t *src,
              unsigned width, unsigned height, bool mirror);

/*
 * The same, one pair at a time: the specification the vector paths are
 * held to (`tools/test_yuv.c`), and what `gfx_yuy2` does on a machine that
 * has neither NEON nor SSE2.
 */
void gfx_yuy2_scalar(uint32_t *dst, unsigned long pitch, const uint8_t *src,
                     unsigned width, unsigned height, bool mirror);

/* One pixel of it, for the reference and anything that wants one. */
uint32_t gfx_yuv_pixel(int y, int u, int v);

/*
 * The same YUY2 into three planes, 4:2:0, as an H.264 encoder takes it: Y
 * `y_stride` bytes a row, U and V `uv_stride` bytes a row and half as many
 * rows, each the rounded average of two rows' (the Record Kit, `roadmap.md`
 * 6d 8f). `width` is even. NEON or SSE2 where there is one, and the scalar
 * path - the specification - for the ends of rows and everywhere else.
 */
void gfx_yuy2_i420(uint8_t *y, uint8_t *u, uint8_t *v, unsigned y_stride,
                   unsigned uv_stride, const uint8_t *src, unsigned width,
                   unsigned height);

void gfx_yuy2_i420_scalar(uint8_t *y, uint8_t *u, uint8_t *v,
                          unsigned y_stride, unsigned uv_stride,
                          const uint8_t *src, unsigned width, unsigned height);

/*
 * **A film's picture: three planes, 4:2:0, into the screen's pixels** - what
 * an H.264 decoder hands back (`roadmap.md` 4e). Y is a byte a pixel; U and
 * V one for each two by two, so row `r` of the picture takes chroma row
 * `r / 2`. Each plane has its own stride, since a decoder pads them.
 *
 * Unlike a webcam's, a film says which matrix it was made with and whether
 * it is studio or full range, so the matrix is an argument: the four below
 * are the ones that exist in practice. Each is the conversion scaled by 256,
 * as `gfx_yuv_pixel`'s is:
 *
 *   C = y (Y - y_offset)     D = U - 128     E = V - 128
 *   R = (C + rv E + 128) >> 8
 *   G = (C - gu D - gv E + 128) >> 8
 *   B = (C + bu D + 128) >> 8
 */
struct gfx_yuv_matrix {
    int16_t y, rv, gu, gv, bu;
    int16_t y_offset;
};

extern const struct gfx_yuv_matrix gfx_yuv_bt601;       /* studio range */
extern const struct gfx_yuv_matrix gfx_yuv_bt709;
extern const struct gfx_yuv_matrix gfx_yuv_bt601_full;  /* 0-255, JPEG's */
extern const struct gfx_yuv_matrix gfx_yuv_bt709_full;

/* `width` by `height` pixels into `dst`, `pitch` bytes a row. NEON or SSE2
 * where there is one, and the scalar path for the ends of rows. */
void gfx_i420(uint32_t *dst, unsigned long pitch, const uint8_t *y,
              const uint8_t *u, const uint8_t *v, long y_stride,
              long u_stride, long v_stride, unsigned width, unsigned height,
              const struct gfx_yuv_matrix *m);

/* The specification the vector paths are held to (`tools/test_yuv.c`). */
void gfx_i420_scalar(uint32_t *dst, unsigned long pitch, const uint8_t *y,
                     const uint8_t *u, const uint8_t *v, long y_stride,
                     long u_stride, long v_stride, unsigned width,
                     unsigned height, const struct gfx_yuv_matrix *m);

#endif /* KOSMOS_GFX_YUV_H */
