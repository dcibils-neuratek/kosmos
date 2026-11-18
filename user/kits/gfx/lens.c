/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Liquid Glass's rim as a table. `lens.h` says what it is.
 *
 * **The trace**, for a point `d` in from the edge of a bevel `B` wide. The
 * surface's height across the bevel is a quarter circle, `z = sqrt(B^2 -
 * (B - d)^2)`: nought at the edge, `B` where the bevel meets the flat. Its
 * normal, in the plane of the outward direction and up, is `n = ((B - d),
 * z) / B` - lying outward at the very edge, straight up at the flat. A ray
 * straight down, `I = (0, -1)`, enters glass at `eta = 1 / 1.5`:
 *
 *     cos_i = n.z,  k = 1 - eta^2 (1 - cos_i^2),
 *     T = eta I + (eta cos_i - sqrt(k)) n
 *
 * and travels down to the backdrop, `z + B` below - the surface's height
 * over a base as thick as the bevel - so it lands `T.o / -T.z * (z + B)`
 * along the outward direction from where it entered: inward, since `T.o`
 * is negative. That, times the look's `refraction`, is the offset: at the
 * very edge `1.12 B` inward, at the flat nothing. The backdrop under the
 * glass's middle is drawn in toward its rim, which is what makes an edge
 * read as thick.
 *
 * **The light** is from the upper left, `L = (-1, -1) / sqrt 2` on the
 * screen: the lit edge is `n.o^3 * max(0, outward . L)` - bright at the
 * very edge of the top and left, nothing on the bottom and right - times
 * the look's `specular`.
 *
 * Only `sqrt` from the libc, in doubles, once a lens; the test traces the
 * same rays with angles instead, independently.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "lens.h"

#define ETA (1.0 / 1.5)

static int8_t round8(double v)
{
    v = v < 0 ? v - 0.5 : v + 0.5;
    if (v > 127.0) v = 127.0;
    if (v < -127.0) v = -127.0;
    return (int8_t)(long)v;
}

struct gfx_lens_px gfx_lens_trace(double d, double ox, double oy,
                                  unsigned bevel, unsigned refraction,
                                  unsigned specular)
{
    struct gfx_lens_px px = { 0, 0, 0 };
    double b = (double)bevel, u, z, no, nz, k, f, to, tz, off, lit, s;

    if (bevel == 0 || d < 0 || d >= b) return px;

    u = b - d;                          /* from the flat, outward */
    z = sqrt(b * b - u * u);
    no = u / b;
    nz = z / b;

    k = 1.0 - ETA * ETA * (1.0 - nz * nz);
    f = ETA * nz - sqrt(k);
    to = f * no;
    tz = -ETA + f * nz;

    off = to / -tz * (z + b) * (double)refraction / 100.0;

    px.dx = round8(off * ox);
    px.dy = round8(off * oy);

    lit = (-ox - oy) / sqrt(2.0);
    s = lit > 0 ? no * no * no * lit * 255.0 * (double)specular / 100.0 : 0.0;
    px.spec = (uint8_t)(s > 255.0 ? 255 : (unsigned)(s + 0.5));
    return px;
}

size_t gfx_lens_size(unsigned radius, unsigned bevel)
{
    size_t side = (size_t)radius + bevel;

    if (bevel == 0 || radius > GFX_LENS_MOST || bevel > GFX_LENS_MOST) return 0;

    return 4u * ((size_t)bevel + side * side) * sizeof(struct gfx_lens_px);
}

/* The outward directions of the four sides, on the screen (y down). */
static const double SIDE_OUT[4][2] = { { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 } };

/* And how each corner mirrors the top-left's: x and y measured from it. */
static const double CORNER_SIGN[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };

int gfx_lens_make(struct gfx_lens *lens, void *mem, unsigned radius,
                  unsigned bevel, unsigned refraction, unsigned specular)
{
    struct gfx_lens_px *at = mem;
    unsigned side = radius + bevel, i, x, y;

    if (gfx_lens_size(radius, bevel) == 0 || mem == NULL) return 0;

    lens->radius = radius;
    lens->bevel = bevel;
    lens->side = side;

    for (i = 0; i < 4; i++) {
        lens->edge[i] = at;
        for (x = 0; x < bevel; x++) {
            at[x] = gfx_lens_trace(x + 0.5, SIDE_OUT[i][0], SIDE_OUT[i][1],
                                   bevel, refraction, specular);
        }
        at += bevel;
    }

    for (i = 0; i < 4; i++) {
        double sx = CORNER_SIGN[i][0], sy = CORNER_SIGN[i][1];

        lens->corner[i] = at;

        for (y = 0; y < side; y++) {
            for (x = 0; x < side; x++) {
                double cx = x + 0.5, cy = y + 0.5, r = (double)radius;
                struct gfx_lens_px px = { 0, 0, 0 };

                /* Measured from the corner, as the top-left's: inside the
                 * curve's square the nearest edge is the arc, past it the
                 * nearer of the two straight ones. */
                if (cx < r && cy < r) {
                    double vx = cx - r, vy = cy - r;
                    double len = sqrt(vx * vx + vy * vy);

                    if (len <= r && len > 0) {
                        /* `v` points from the curve's centre toward the
                         * corner; on the screen that is `-s` times it. */
                        px = gfx_lens_trace(r - len, -sx * vx / len,
                                            -sy * vy / len,
                                            bevel, refraction, specular);
                    }
                } else if (cx < cy) {
                    px = gfx_lens_trace(cx, sx, 0, bevel, refraction, specular);
                } else {
                    px = gfx_lens_trace(cy, 0, sy, bevel, refraction, specular);
                }

                at[(size_t)y * side + x] = px;
            }
        }
        at += (size_t)side * side;
    }

    return 1;
}
