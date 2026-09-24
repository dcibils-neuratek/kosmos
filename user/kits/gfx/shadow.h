/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A window's shadow and the rounded-corner test, for the graphics kit and
 * for its host test. `shadow.c` says what each does.
 */

#ifndef KOSMOS_GFX_SHADOW_H
#define KOSMOS_GFX_SHADOW_H

#include <stdint.h>

/* How much of a pixel is inside a rounded rectangle: 0 to 255. */
long gfx_round_cover(long x, long y, long rx, long ry, long rw, long rh,
                     long r);

/* A run of pixels keeping `keep` of 256 of each channel. */
void gfx_darken_span(uint32_t *p, long n, uint32_t keep);

/*
 * The shadow of `rx, ry, rw, rh` with corners of `r`, `spread` wide and at
 * most `alpha` dark, on a surface `width` by `height` whose rows are
 * `pitch` bytes apart - only inside `clip` (x, y, w, h) when it is given.
 */
void gfx_shadow(uint32_t *pixels, unsigned long pitch, long width,
                long height, long rx, long ry, long rw, long rh, long r,
                long spread, long alpha, const long *clip);

#endif
