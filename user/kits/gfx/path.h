/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Polygons and wide lines, anti-aliased (`docs/maps.md` step M1), on gfx's
 * rasteriser (`raster.h`). `path.c` is the argument; this is the shape
 * `gfx.c`, the Map Kit and the host test (`tools/test_path.c`) compile
 * against.
 *
 * A path is the rasteriser's buffer over a box of pixels. Rings and lines
 * are added to it, then it is painted in one colour: everything a style's
 * rule draws is one pass over the pixels, however many shapes made it.
 *
 * Pure: no surface, no Lua, no kernel - it compiles on the Mac as here.
 */

#ifndef KOSMOS_GFX_PATH_H
#define KOSMOS_GFX_PATH_H

#include <stddef.h>
#include <stdint.h>

#include "raster.h"

struct gfx_path {
    struct raster r;        /* kept between paths, grown when it must be */
    long x0, y0;            /* the box's top left, in the caller's pixels */
    long w, h;              /* its size; nothing outside it is touched    */
};

/* Begun over the box `x0, y0, w, h`, empty. 0, or -1 when there was no
 * memory for it, after which nothing is drawn. */
int  gfx_path_begin(struct gfx_path *p, long x0, long y0, long w, long h);

/* Gives the buffer back. The path may be begun again after. */
void gfx_path_free(struct gfx_path *p);

/* One edge, in the caller's pixels. */
void gfx_path_edge(struct gfx_path *p, float ax, float ay, float bx, float by);

/* A closed ring of `n` points, `xy` as x0, y0, x1, y1 ...: the last point
 * joined back to the first. A hole is a ring wound the other way, as a
 * vector tile's are; two rings wound alike are their union. */
void gfx_path_ring(struct gfx_path *p, const float *xy, size_t n);

/* A line through `n` points, `width` wide, round at its joins and ends. */
void gfx_path_stroke(struct gfx_path *p, const float *xy, size_t n,
                     float width);

/* Painted in `rgb` (0xRRGGBB) over `pixels`, which is the box's top left -
 * `pitch` bytes a row - and the path left empty for the next one. */
void gfx_path_paint(struct gfx_path *p, uint32_t *pixels, unsigned pitch,
                    uint32_t rgb);

#endif
