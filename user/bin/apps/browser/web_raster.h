/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The SVG rasteriser's pixel work (`web_raster.c`, `roadmap.md` 6zz j5):
 * straight edges in, anti-aliased coverage out, laid over a row of pixels
 * in one colour. No Lua and no libsvgtiny, so `tools/test_raster.c` builds
 * it on this Mac and holds the vector loop to the scalar one.
 */

#ifndef WEB_RASTER_H
#define WEB_RASTER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The accumulator, and where in it the edges since the last paint landed:
 * a row of signed areas a pixel and one more wider than the picture, since
 * an edge's last contribution lands to the right of it, and for each row
 * the span [from, to) it touched - so a small shape on a large picture
 * costs its own size and not the picture's.
 */
struct raster {
    int    w, h;
    float *acc;             /* (w + 2) * h */
    int   *from, *to;       /* h each; from >= to is a row untouched */
    int    top, bottom;     /* the rows touched, [top, bottom) */
};

/* Room for a picture of `w` by `h`, nothing in it; false for no memory. */
bool raster_open(struct raster *r, int w, int h);
void raster_close(struct raster *r);

/* One straight edge, in pixels. Anything not a finite number is no edge. */
void raster_edge(struct raster *r, float x0, float y0, float x1, float y1);

/*
 * The edges so far as coverage, in `rgb` (0xRRGGBB), over the pixels -
 * 0xAARRGGBB, straight alpha, as `gfx` keeps them, `pitch` bytes a row - and
 * the accumulator emptied for the next shape. Four pixels at a time when
 * `vectors`, one at a time otherwise, and the two are one answer to the bit.
 */
void raster_paint(struct raster *r, uint32_t *pixels, unsigned pitch,
                  uint32_t rgb, bool vectors);

#endif /* WEB_RASTER_H */
