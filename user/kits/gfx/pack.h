/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_GFX_PACK_H
#define KOSMOS_GFX_PACK_H

/*
 * A surface's pixels in somebody else's pixel format.
 *
 * `surface:pack` (`gfx.c`) is the Lua door; this is the loop, in a file of
 * its own so the Mac can hold it to its scalar self (`tools/test_pack.c`),
 * as `yuv.c` is held. The first caller is `vncd`, which sends a VNC viewer
 * the screen in whatever true-colour format it asked for (`roadmap.md`,
 * remote 7a).
 *
 * The format is true colour as RFB's SetPixelFormat describes it: 8, 16 or
 * 32 bits a pixel, either byte order, and for each channel its largest
 * value and where it sits. A channel of 0-255 becomes `round(c * max /
 * 255)`, placed at its shift.
 */

#include <stdbool.h>
#include <stdint.h>

struct gfx_pack_format {
    unsigned bpp;                   /* 8, 16 or 32 */
    bool     big;                   /* most significant byte first */
    uint32_t rmax, gmax, bmax;      /* 1 to 65535 */
    unsigned rshift, gshift, bshift;    /* 0 to 31 */
};

/* One pixel, `0xAARRGGBB`, as the value the format stores - the reference
 * the vector loop is held to, and its tail. */
uint32_t gfx_pack_pixel(uint32_t pixel, const struct gfx_pack_format *f);

/* `width` pixels from `src` into `out`, `width * bpp / 8` bytes of it. */
void gfx_pack_row(const uint32_t *src, uint8_t *out, unsigned width,
                  const struct gfx_pack_format *f);

/* The same, a pixel at a time, for the test to time against. */
void gfx_pack_row_scalar(const uint32_t *src, uint8_t *out, unsigned width,
                         const struct gfx_pack_format *f);

#endif /* KOSMOS_GFX_PACK_H */
