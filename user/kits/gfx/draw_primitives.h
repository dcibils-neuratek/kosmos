/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_GFX_DRAW_PRIMITIVES_H
#define KOSMOS_GFX_DRAW_PRIMITIVES_H

/*
 * **The Graphics Kit's primitives, as C functions on surfaces** (`draw.c`):
 * what `gfx.c`'s Lua methods call once they have read their arguments, and
 * what the scene composes with - in the kit and in the window server, which
 * has no Lua (Astra's D3b-1). One copy of every loop over pixels.
 */

#include "surface.h"

void blend_row(uint32_t *dp, const uint32_t *sp, long w,
                      uint32_t global);

void fill_on(struct surface *s, long x, long y, long w, long h,
                    uint32_t colour);

void triangle_on(struct surface *s, double vx[3], double vy[3],
                        uint32_t colour);

void blit_on(struct surface *dst, struct surface *src,
                    long sx, long sy, long w, long h, long dx, long dy);

void blit_round_on(struct surface *dst, struct surface *src,
                          long sx, long sy, long w, long h, long dx, long dy,
                          long rx, long ry, long rw, long rh, long r, int outside);

void fill_round_on(struct surface *dst, long x, long y, long w, long h,
                          uint32_t colour, long r);

void frame_round_on(struct surface *dst, long x, long y, long w, long h,
                           uint32_t colour, long r);

void blend_on(struct surface *dst, struct surface *src, long sx, long sy,
                     long w, long h, long dx, long dy, uint32_t global);

void stretch_into(struct surface *d, const struct surface *s,
                         long sx, long sy, long sw, long sh,
                         long dx, long dy, long dw, long dh,
                         long global, bool smooth,
                         long cx0, long cy0, long cx1, long cy1);

#endif
