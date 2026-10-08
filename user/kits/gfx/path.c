/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Polygons and wide lines, anti-aliased** - gfx's paths (`docs/maps.md`
 * step M1). Maps is their first caller, a vector tile being nothing but
 * filled rings and stroked lines; Write's shapes, Paint's vectors, the 3D
 * Kit's outlines and Cafesa3D's guides are the next, which is why they are
 * gfx's and not the Map Kit's.
 *
 * **The pixels are the rasteriser's** (`raster.c`, the browser's SVG's
 * first): signed area accumulated, exact for straight edges, its sum a
 * winding number - so a ring wound one way and a hole wound the other
 * cancel, and two rings wound alike are their union. What is here is the
 * geometry on top of it: a box placed anywhere on a surface, closed rings,
 * and a wide line as a quad for each segment and a disc at each joint, all
 * wound alike, so that where they overlap they are one shape and not a
 * darker seam.
 */

#include "path.h"

#include <stdlib.h>
#include <string.h>

/* A square root without libm: `path.c` is built where `sqrtf` may be a
 * call into a library this image does not have. Newton's, from above. */
static float sqrt_(float v)
{
    float r = v > 1.0f ? v : 1.0f;

    if (v <= 0.0f) return 0.0f;

    for (int i = 0; i < 12; i++) r = 0.5f * (r + v / r);

    return r;
}

int gfx_path_begin(struct gfx_path *p, long x0, long y0, long w, long h)
{
    p->x0 = x0, p->y0 = y0;
    p->w = w > 0 ? w : 0, p->h = h > 0 ? h : 0;

    if (p->w == 0 || p->h == 0 || !raster_size(&p->r, (int)p->w, (int)p->h)) {
        p->w = p->h = 0;
        return -1;
    }

    return 0;
}

void gfx_path_free(struct gfx_path *p)
{
    raster_close(&p->r);
    p->w = p->h = 0;
}

void gfx_path_edge(struct gfx_path *p, float ax, float ay, float bx, float by)
{
    if (p->w == 0) return;

    raster_edge(&p->r, ax - (float)p->x0, ay - (float)p->y0,
                bx - (float)p->x0, by - (float)p->y0);
}

void gfx_path_ring(struct gfx_path *p, const float *xy, size_t n)
{
    if (n < 3) return;

    for (size_t i = 0; i < n; i++) {
        size_t j = (i + 1 == n) ? 0 : i + 1;

        gfx_path_edge(p, xy[2 * i], xy[2 * i + 1], xy[2 * j], xy[2 * j + 1]);
    }
}

/*
 * A disc, as a ring of sixteen wound the way a stroke's quads are - so that
 * where they overlap they are one shape (above) - and fewer for a small one,
 * whose sides would be under a pixel anyway.
 */
static void disc(struct gfx_path *p, float cx, float cy, float r)
{
    /* cos and sin of k * 22.5 degrees, k = 0..15, clockwise on the screen. */
    static const float C[16] = { 1.0f, 0.92388f, 0.70711f, 0.38268f, 0.0f,
                                 -0.38268f, -0.70711f, -0.92388f, -1.0f,
                                 -0.92388f, -0.70711f, -0.38268f, 0.0f,
                                 0.38268f, 0.70711f, 0.92388f };
    static const float S[16] = { 0.0f, -0.38268f, -0.70711f, -0.92388f, -1.0f,
                                 -0.92388f, -0.70711f, -0.38268f, 0.0f,
                                 0.38268f, 0.70711f, 0.92388f, 1.0f,
                                 0.92388f, 0.70711f, 0.38268f };
    int step = r < 1.5f ? 4 : (r < 4.0f ? 2 : 1);
    float pts[32];
    size_t n = 0;

    for (int k = 0; k < 16; k += step) {
        pts[2 * n] = cx + r * C[k];
        pts[2 * n + 1] = cy + r * S[k];
        n++;
    }

    gfx_path_ring(p, pts, n);
}

void gfx_path_stroke(struct gfx_path *p, const float *xy, size_t n,
                     float width)
{
    float hw = 0.5f * width;

    if (n == 0 || width <= 0.0f) return;

    for (size_t i = 0; i + 1 < n; i++) {
        float ax = xy[2 * i], ay = xy[2 * i + 1];
        float bx = xy[2 * i + 2], by = xy[2 * i + 3];
        float dx = bx - ax, dy = by - ay;
        float len = sqrt_(dx * dx + dy * dy);
        float nx, ny, quad[8];

        if (len <= 0.0f) continue;

        nx = -dy / len * hw, ny = dx / len * hw;

        /* a+n, b+n, b-n, a-n: wound the same way for every direction, and
         * the same way as `disc`. */
        quad[0] = ax + nx, quad[1] = ay + ny;
        quad[2] = bx + nx, quad[3] = by + ny;
        quad[4] = bx - nx, quad[5] = by - ny;
        quad[6] = ax - nx, quad[7] = ay - ny;
        gfx_path_ring(p, quad, 4);
    }

    /* Round joins and ends; a line under two pixels wide needs none to look
     * joined, and is drawn by the thousand on a map. */
    if (width >= 2.0f) {
        for (size_t i = 0; i < n; i++) disc(p, xy[2 * i], xy[2 * i + 1], hw);
    } else if (n == 1) {
        disc(p, xy[0], xy[1], hw);
    }
}

void gfx_path_paint(struct gfx_path *p, uint32_t *pixels, unsigned pitch,
                    uint32_t rgb)
{
    if (p->w == 0) return;

    raster_paint(&p->r, pixels, pitch, rgb & 0x00ffffffu, true);
}
