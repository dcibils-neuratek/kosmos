/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_GFX_LENS_H
#define KOSMOS_GFX_LENS_H

/*
 * Liquid Glass's rim, as a table (`docs/astra.md`, Liquid Glass LG3).
 *
 * A glass surface's rim is a bevel `bevel` pixels wide whose height across
 * it is a quarter circle. A ray straight down meets it, is bent by Snell's
 * law at n = 1.5, and lands on the backdrop a glass-depth below: the pixel
 * the rim shows is the backdrop's at that offset. The lit edge comes from
 * the same surface's normal against a light at the upper left. This is the
 * method of Zaki Sheriff's Liquid-Glass (MIT), re-derived here in C; it
 * keeps a map a size, and this keeps one a radius and bevel, because a
 * rounded rectangle's rim is the same all along each straight edge and
 * differs only in its corners.
 *
 * So a lens is, for each of the four sides, a profile of `bevel` entries -
 * the first at the very edge - and for each of the four corners a square of
 * `radius + bevel` a side, measured from that corner. Made once, in doubles,
 * when a look meets that radius at that scale; never in a frame.
 *
 * An entry is where to read the backdrop from, relative to the pixel, and
 * how much light to add: `spec` from 0 to 255, already scaled by the look's
 * `specular`, and the offset by its `refraction`. A pixel past the bevel,
 * or outside the corner's curve, is `{ 0, 0, 0 }`: the backdrop, unbent.
 */

#include <stddef.h>
#include <stdint.h>

#define GFX_LENS_MOST 96u       /* the largest radius, and bevel, a lens takes */

struct gfx_lens_px {
    int8_t  dx, dy;             /* the backdrop's pixel, from this one */
    uint8_t spec;               /* light added, 0 to 255 */
};

/* Sides and corners, in this order. */
enum { GFX_LENS_TOP, GFX_LENS_RIGHT, GFX_LENS_BOTTOM, GFX_LENS_LEFT };
enum { GFX_LENS_TL, GFX_LENS_TR, GFX_LENS_BR, GFX_LENS_BL };

struct gfx_lens {
    unsigned radius, bevel, side;   /* side = radius + bevel */
    struct gfx_lens_px *edge[4];    /* `bevel` each, from the edge inward */
    struct gfx_lens_px *corner[4];  /* `side * side` each, rows from the
                                     * corner's own edge inward */
};

/* The bytes a lens of this radius and bevel needs; 0 when either is past
 * `GFX_LENS_MOST` or the bevel is 0. */
size_t gfx_lens_size(unsigned radius, unsigned bevel);

/*
 * A lens made in `mem`, `gfx_lens_size` bytes: `refraction` and `specular`
 * in percent, the look's. 0 when it could not be made, 1 when it was.
 */
int gfx_lens_make(struct gfx_lens *lens, void *mem, unsigned radius,
                  unsigned bevel, unsigned refraction, unsigned specular);

/* One rim pixel at `d` pixels in from the edge - its centre, so `d + 0.5`
 * is what is traced - whose outward direction is (`ox`, `oy`): what the
 * tables hold, for the test to hold them to. */
struct gfx_lens_px gfx_lens_trace(double d, double ox, double oy,
                                  unsigned bevel, unsigned refraction,
                                  unsigned specular);

#endif /* KOSMOS_GFX_LENS_H */
