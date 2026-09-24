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

/* One pixel of it, for the reference and anything that wants one. */
uint32_t gfx_yuv_pixel(int y, int u, int v);

#endif /* KOSMOS_GFX_YUV_H */
