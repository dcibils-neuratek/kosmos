/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Liquid Glass's rim table (`user/kits/gfx/lens.c`, LG3) held to rays traced
 * here another way.
 *
 *   1. Every side's entry is the ray traced with angles - the bevel's slope
 *      as an angle, Snell's law as `asin`, the deviation's tangent - within
 *      a pixel, and its light within one step of 255.
 *   2. Every corner's entry likewise, its distance and outward direction
 *      from a rounded rectangle's signed distance and that distance's
 *      gradient taken numerically, rather than from the arc's geometry.
 *   3. A corner's square, past its curve, is the side's profile exactly.
 *   4. The top-right corner is the top-left's mirror in its offsets.
 *   5. A rim with refraction 0 bends nothing: every offset nought.
 *   6. Lit from the upper left: nothing on the bottom and right sides or the
 *      bottom-right corner, something at the top's and left's very edge.
 *   7. A bevel of 0, or past the most, makes no lens.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lens.h"

static unsigned failures, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        if (failures < 30) printf("  %s\n", what);
    }
}

/* The ray, with angles: the surface's tilt, Snell's law, the deviation. */
static void trace(double d, double ox, double oy, unsigned bevel,
                  unsigned refraction, unsigned specular,
                  double *dx, double *dy, double *spec)
{
    double b = bevel, u = b - d, z = sqrt(b * b - u * u);
    double tilt = atan2(u, z);                  /* the normal from straight up */
    double bent = asin(sin(tilt) / 1.5);        /* in the glass, from the normal */
    double off = -tan(tilt - bent) * (z + b) * refraction / 100.0;
    double lit = (-ox - oy) / sqrt(2.0);

    *dx = off * ox;
    *dy = off * oy;
    *spec = lit > 0 ? pow(sin(tilt), 3) * lit * 255.0 * specular / 100.0 : 0;
}

/* A rounded rectangle's signed distance - negative inside - of a box from
 * the origin to (W, H) with corners of `r`. */
static double sdf(double x, double y, double w, double h, double r)
{
    double qx = fabs(x - w / 2) - w / 2 + r, qy = fabs(y - h / 2) - h / 2 + r;
    double ax = qx > 0 ? qx : 0, ay = qy > 0 ? qy : 0;
    double m = qx > qy ? qx : qy;

    return sqrt(ax * ax + ay * ay) + (m < 0 ? m : 0) - r;
}

static int near(const struct gfx_lens_px *p, double dx, double dy, double sp,
                double slack)
{
    return fabs(p->dx - dx) <= slack && fabs(p->dy - dy) <= slack
        && fabs(p->spec - sp) <= 1.0 + slack;
}

static void one(unsigned radius, unsigned bevel, unsigned refraction,
                unsigned specular)
{
    size_t size = gfx_lens_size(radius, bevel);
    void *mem = malloc(size);
    struct gfx_lens L;
    static const double OUT[4][2] = { { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 } };
    static const double SIGN[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    unsigned i, x, y, side, bad = 0, axis = 0;
    char what[200];
    const double W = 4000, H = 3000, e = 1e-4;

    check(size > 0 && gfx_lens_make(&L, mem, radius, bevel, refraction, specular),
          "a lens was not made");
    side = radius + bevel;

    /* 1. The sides. */
    for (i = 0; i < 4; i++) {
        for (x = 0; x < bevel; x++) {
            double dx, dy, sp;

            trace(x + 0.5, OUT[i][0], OUT[i][1], bevel, refraction, specular, &dx, &dy, &sp);
            if (!near(&L.edge[i][x], dx, dy, sp, 0.5 + 1e-9)) bad++;
        }
    }
    snprintf(what, sizeof(what), "r %u bevel %u: %u side entries not the ray traced",
             radius, bevel, bad);
    check(bad == 0, what);

    /* 2. The corners, against the signed distance of a large box, each
     * corner of it in turn: its pixel's centre in the box's coordinates. */
    bad = 0;
    for (i = 0; i < 4; i++) {
        for (y = 0; y < side; y++) {
            for (x = 0; x < side; x++) {
                double cx = x + 0.5, cy = y + 0.5;
                double px = SIGN[i][0] < 0 ? cx : W - cx;
                double py = SIGN[i][1] < 0 ? cy : H - cy;
                double s = sdf(px, py, W, H, radius), d = -s;
                double gx = (sdf(px + e, py, W, H, radius) - sdf(px - e, py, W, H, radius)) / (2 * e);
                double gy = (sdf(px, py + e, W, H, radius) - sdf(px, py - e, W, H, radius)) / (2 * e);
                double len = sqrt(gx * gx + gy * gy), dx = 0, dy = 0, sp = 0;
                const struct gfx_lens_px *got = &L.corner[i][(size_t)y * side + x];

                /* Where two edges are equally near the gradient is a
                 * crease, and either answer is the shape's: skip it. */
                if (fabs(cx - cy) < 1.0 && cx >= radius && cy >= radius) continue;

                if (d >= 0 && d < bevel && len > 0.5) {
                    trace(d, gx / len, gy / len, bevel, refraction, specular, &dx, &dy, &sp);
                }
                if (!near(got, dx, dy, sp, 1.0)) {
                    bad++;
                    if (bad < 4) {
                        printf("    corner %u (%u,%u): %d,%d,%u against %.2f,%.2f,%.1f\n",
                               i, x, y, got->dx, got->dy, got->spec, dx, dy, sp);
                    }
                }
            }
        }
    }
    snprintf(what, sizeof(what), "r %u bevel %u: %u corner entries not the ray traced",
             radius, bevel, bad);
    check(bad == 0, what);

    /* 3. A corner's square past its curve is the sides' profile. */
    for (i = 0; i < 4; i++) {
        const struct gfx_lens_px *top = L.edge[SIGN[i][1] < 0 ? GFX_LENS_TOP : GFX_LENS_BOTTOM];
        const struct gfx_lens_px *left = L.edge[SIGN[i][0] < 0 ? GFX_LENS_LEFT : GFX_LENS_RIGHT];

        for (x = 0; x < bevel; x++) {
            if (memcmp(&L.corner[i][(size_t)x * side + side - 1], &top[x], sizeof(top[x])) != 0) axis++;
            if (memcmp(&L.corner[i][(size_t)(side - 1) * side + x], &left[x], sizeof(left[x])) != 0) axis++;
        }
    }
    snprintf(what, sizeof(what), "r %u bevel %u: %u entries where a corner meets its sides "
             "differ from the sides'", radius, bevel, axis);
    check(axis == 0, what);

    /* 4. The top-right mirrors the top-left. */
    bad = 0;
    for (x = 0; x < (unsigned)(side * side); x++) {
        const struct gfx_lens_px *a = &L.corner[GFX_LENS_TL][x], *b = &L.corner[GFX_LENS_TR][x];

        if (a->dx != -b->dx || a->dy != b->dy) bad++;
    }
    snprintf(what, sizeof(what), "r %u bevel %u: %u top-right entries not the top-left's mirror",
             radius, bevel, bad);
    check(bad == 0, what);

    /* 6. The light. */
    bad = 0;
    for (x = 0; x < bevel; x++) {
        if (L.edge[GFX_LENS_BOTTOM][x].spec || L.edge[GFX_LENS_RIGHT][x].spec) bad++;
    }
    for (x = 0; x < side * side; x++) {
        if (L.corner[GFX_LENS_BR][x].spec) bad++;
    }
    snprintf(what, sizeof(what), "r %u bevel %u: %u entries lit on the side away from the light",
             radius, bevel, bad);
    check(bad == 0, what);
    /* A bevel of one pixel has its only centre halfway across, where the
     * edge has turned half up and its light is faint. */
    if (specular >= 50 && bevel >= 4) {
        check(L.edge[GFX_LENS_TOP][0].spec > 100 && L.edge[GFX_LENS_LEFT][0].spec > 100,
              "the top's and left's very edge are not lit");
    }

    free(mem);
}

int main(void)
{
    static const unsigned shapes[][2] = { { 10, 12 }, { 18, 12 }, { 0, 4 }, { 14, 1 },
                                          { 24, 3 }, { 96, 96 } };
    unsigned i;
    size_t size;
    void *mem;
    struct gfx_lens L;
    unsigned moved = 0, x;

    for (i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
        one(shapes[i][0], shapes[i][1], 60, 85);
        one(shapes[i][0], shapes[i][1], 100, 30);
    }

    /* 5. Refraction 0. */
    size = gfx_lens_size(18, 12);
    mem = malloc(size);
    gfx_lens_make(&L, mem, 18, 12, 0, 85);
    for (x = 0; x < size / sizeof(struct gfx_lens_px); x++) {
        const struct gfx_lens_px *p = (const struct gfx_lens_px *)mem + x;

        if (p->dx || p->dy) moved++;
    }
    check(moved == 0, "a rim with refraction 0 bent something");
    free(mem);

    /* 7. No lens. */
    check(gfx_lens_size(10, 0) == 0 && gfx_lens_size(97, 4) == 0
          && gfx_lens_size(10, 97) == 0, "a lens was sized past what it takes");
    check(!gfx_lens_make(&L, NULL, 10, 0, 60, 85), "a lens with no bevel was made");

    if (failures) {
        printf("FAIL: %u of %u checks on the lens.\n", failures, checks);
        return 1;
    }

    printf("PASS: %u checks on the lens (every side's and corner's entry the ray "
           "traced with angles and a signed distance, within a pixel, at 6 shapes "
           "from a bevel of 1 to 96; a corner's square past its curve the sides' "
           "profile; the top-right the top-left's mirror; refraction 0 bending "
           "nothing; lit from the upper left only; no bevel no lens). 18 and 12: "
           "%zu bytes.\n", checks, gfx_lens_size(18, 12));
    return 0;
}
