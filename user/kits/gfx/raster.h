/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * gfx's rasteriser (`raster.c`): straight edges in, anti-aliased coverage
 * out, laid over a row of pixels in one colour. No Lua and no libsvgtiny,
 * so `tools/test_raster.c` builds it on this Mac and holds the vector loop
 * to the scalar one.
 *
 * **The browser's SVG's first** (`roadmap.md` 6zz j5), in the browser's
 * own directory; **gfx's since 8 October** (`docs/maps.md` M1), when Maps
 * needed the same thing - a polygon and a wide line - and a second copy
 * was about to be written beside it. `path.c` builds rings and strokes on
 * it, and the SVG and the Map Kit both draw through it.
 */

#ifndef KOSMOS_GFX_RASTER_H
#define KOSMOS_GFX_RASTER_H

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
    size_t cap;             /* floats `acc` holds, for `raster_size` */
    int    cap_h;           /* rows `from` and `to` hold */
};

/* Room for a picture of `w` by `h`, nothing in it; false for no memory. */
bool raster_open(struct raster *r, int w, int h);
void raster_close(struct raster *r);

/*
 * The same accumulator for a picture of `w` by `h` - kept when it is large
 * enough, which every one after the largest is, and opened again larger
 * when it is not. A path drawn many times a frame (a map's rules) costs no
 * allocation and no clearing: painting empties what an edge touched, so
 * between pictures the buffer is all nought whatever its shape.
 */
bool raster_size(struct raster *r, int w, int h);

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

#endif /* KOSMOS_GFX_RASTER_H */
