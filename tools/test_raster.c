/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The SVG rasteriser against what a shape covers, and its four lanes
 * against its one (`user/bin/apps/browser/web_raster.c`, `roadmap.md`
 * 6zz j5).
 *
 *   - a rectangle with half-pixel edges: whole pixels inside, half ones on
 *     its sides, nothing outside, and the same wound either way;
 *   - two rectangles over each other covering once, as `nonzero` does;
 *   - half a pixel of red over opaque blue and over nothing, which is
 *     where straight alpha's division shows;
 *   - edges that are not finite numbers doing nothing, and edges a long
 *     way off the picture drawn at its border;
 *   - the accumulator empty after every paint, so the next shape starts
 *     from nothing;
 *   - random polygons on every width from 1 to 40 - each tail length - over
 *     random pixels, four at a time and one at a time, to the bit;
 *   - and a disc filling a 1024 by 1024 picture timed both ways, printed,
 *     because the lanes exist to be faster and a test is where that is seen.
 *
 * Built twice, as `tools/test_pack.c` is: natively, which is NEON on this
 * Mac, and `-arch x86_64`, which Rosetta runs as SSE2.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "web_raster.h"

static int failures, checks;

static void check(bool ok, const char *what)
{
    checks++;

    if (!ok) {
        failures++;
        printf("not ok %d - %s\n", checks, what);
    }
}

static uint32_t seed = 0x6A2C9E11u;

static uint32_t next(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

/* A float in [lo, hi), from the seed. */
static float between(float lo, float hi)
{
    return lo + (hi - lo) * (float)(next() & 0xffffffu) / 16777216.0f;
}

static double now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

static void rect(struct raster *r, float x0, float y0, float x1, float y1,
                 bool clockwise)
{
    if (clockwise) {
        raster_edge(r, x0, y0, x1, y0);
        raster_edge(r, x1, y0, x1, y1);
        raster_edge(r, x1, y1, x0, y1);
        raster_edge(r, x0, y1, x0, y0);
    } else {
        raster_edge(r, x0, y0, x0, y1);
        raster_edge(r, x0, y1, x1, y1);
        raster_edge(r, x1, y1, x1, y0);
        raster_edge(r, x1, y0, x0, y0);
    }
}

/* Is every slot of the accumulator nothing, and every span reset? */
static bool empty(const struct raster *r)
{
    size_t i, n = (size_t)(r->w + 2) * (size_t)r->h;
    int y;

    for (i = 0; i < n; i++) {
        if (r->acc[i] != 0.0f) {
            return false;
        }
    }

    for (y = 0; y < r->h; y++) {
        if (r->from[y] < r->to[y]) {
            return false;
        }
    }

    return r->top >= r->bottom;
}

int main(void)
{
    /* A rectangle from (2.5, 3) to (7.5, 9), both ways round. */
    {
        uint32_t a[12][10], b[12][10];
        struct raster r;
        bool inside = true, sides = true, outside = true;
        int x, y;

        memset(a, 0, sizeof(a));
        memset(b, 0, sizeof(b));
        check(raster_open(&r, 10, 12), "room for a 10 by 12 picture");

        rect(&r, 2.5f, 3.0f, 7.5f, 9.0f, true);
        raster_paint(&r, &a[0][0], 40, 0x112233, true);
        check(empty(&r), "the accumulator is empty after a paint");

        rect(&r, 2.5f, 3.0f, 7.5f, 9.0f, false);
        raster_paint(&r, &b[0][0], 40, 0x112233, true);

        for (y = 0; y < 12; y++) {
            for (x = 0; x < 10; x++) {
                bool rows = y >= 3 && y < 9;

                if (rows && x >= 3 && x <= 6) {
                    inside = inside && a[y][x] == 0xff112233u;
                } else if (rows && (x == 2 || x == 7)) {
                    sides = sides && a[y][x] == 0x80112233u;
                } else {
                    outside = outside && a[y][x] == 0;
                }
            }
        }

        check(inside, "a rectangle's whole pixels are its colour, opaque");
        check(sides, "the pixels its sides halve are half covered");
        check(outside, "nothing outside it is touched");
        check(memcmp(a, b, sizeof(a)) == 0,
              "a rectangle wound the other way is the same rectangle");
        raster_close(&r);
    }

    /* Two rectangles over each other, painted as one shape. */
    {
        uint32_t px[8][8];
        struct raster r;

        memset(px, 0, sizeof(px));
        raster_open(&r, 8, 8);
        rect(&r, 1.0f, 1.0f, 6.0f, 6.0f, true);
        rect(&r, 2.0f, 2.0f, 7.0f, 7.0f, true);
        raster_paint(&r, &px[0][0], 32, 0x00ff00, true);
        check(px[3][3] == 0xff00ff00u && px[1][1] == 0xff00ff00u
              && px[6][6] == 0xff00ff00u && px[0][0] == 0,
              "overlapping parts of one shape are covered once (nonzero)");
        raster_close(&r);
    }

    /* Half a pixel of red: over opaque blue, and over nothing. */
    {
        uint32_t px[1][4] = { { 0xff0000ffu, 0, 0, 0 } };
        struct raster r;

        raster_open(&r, 4, 1);
        rect(&r, 0.5f, 0.0f, 1.5f, 1.0f, true);
        raster_paint(&r, &px[0][0], 16, 0xff0000, true);
        check(px[0][0] == 0xff800080u,
              "half red over opaque blue is opaque purple");
        check(px[0][1] == 0x80ff0000u,
              "half red over nothing is red, half there (straight alpha)");
        raster_close(&r);
    }

    /* Edges that are not finite, and edges far away. */
    {
        uint32_t px[6][6];
        struct raster r;
        bool whole = true;
        int x, y;

        memset(px, 0, sizeof(px));
        raster_open(&r, 6, 6);
        raster_edge(&r, NAN, 0.0f, 3.0f, 5.0f);
        raster_edge(&r, 0.0f, -INFINITY, 3.0f, 5.0f);
        raster_edge(&r, 1.0f, 1.0f, INFINITY, 4.0f);
        raster_edge(&r, 1.0f, 1.0f, 2.0f, NAN);
        check(r.top >= r.bottom && empty(&r),
              "an edge that is not a finite number is no edge");

        rect(&r, -1e30f, -1e30f, 1e30f, 1e30f, true);
        raster_paint(&r, &px[0][0], 24, 0x445566, true);

        for (y = 0; y < 6; y++) {
            for (x = 0; x < 6; x++) {
                whole = whole && px[y][x] == 0xff445566u;
            }
        }

        check(whole, "a rectangle far larger than the picture covers it");
        check(empty(&r), "and leaves the accumulator empty");
        raster_close(&r);
    }

    /* Four lanes against one, every width to 40, over random pixels. */
    {
        int w, h;
        bool same = true, cleared = true;

        for (w = 1; w <= 40 && same; w++) {
            for (h = 1; h <= 9 && same; h += 4) {
                static uint32_t vec[9][40], one[9][40];
                struct raster rv, rs;
                int shape;

                raster_open(&rv, w, h);
                raster_open(&rs, w, h);

                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        vec[y][x] = one[y][x] = next();
                    }
                }

                for (shape = 0; shape < 6; shape++) {
                    int n = 3 + (int)(next() % 9), k;
                    float fx = between(-5.0f, (float)w + 5.0f);
                    float fy = between(-5.0f, (float)h + 5.0f);
                    float px = fx, py = fy;
                    uint32_t rgb = next() & 0xffffffu;

                    for (k = 1; k <= n; k++) {
                        float qx = k == n ? fx : between(-5.0f, (float)w + 5.0f);
                        float qy = k == n ? fy : between(-5.0f, (float)h + 5.0f);

                        raster_edge(&rv, px, py, qx, qy);
                        raster_edge(&rs, px, py, qx, qy);
                        px = qx;
                        py = qy;
                    }

                    raster_paint(&rv, &vec[0][0], sizeof(vec[0]), rgb, true);
                    raster_paint(&rs, &one[0][0], sizeof(one[0]), rgb, false);
                    cleared = cleared && empty(&rv) && empty(&rs);
                }

                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        if (vec[y][x] != one[y][x] && same) {
                            printf("  %dx%d at (%d, %d): four at a time "
                                   "%08x, one at a time %08x\n", w, h, x, y,
                                   vec[y][x], one[y][x]);
                            same = false;
                        }
                    }
                }

                raster_close(&rv);
                raster_close(&rs);
            }
        }

        check(same, "four pixels at a time and one at a time agree to the "
                    "bit, every width from 1 to 40");
        check(cleared, "every paint leaves the accumulator empty");
    }

    /* A disc filling a 1024 by 1024 picture, each way. */
    {
        enum { SIDE = 1024, ROUNDS = 8 };
        uint32_t *px = calloc((size_t)SIDE * SIDE, 4);
        double took[2] = { 0, 0 };
        struct raster r;
        int way, round, k;

        raster_open(&r, SIDE, SIDE);

        for (way = 0; way < 2; way++) {
            for (round = 0; round < ROUNDS; round++) {
                double t0;

                for (k = 0; k < 256; k++) {
                    float a0 = (float)k * 6.2831853f / 256.0f;
                    float a1 = (float)(k + 1) * 6.2831853f / 256.0f;

                    raster_edge(&r, 512.0f + 500.0f * cosf(a0),
                                512.0f + 500.0f * sinf(a0),
                                512.0f + 500.0f * cosf(a1),
                                512.0f + 500.0f * sinf(a1));
                }

                t0 = now_ms();
                raster_paint(&r, px, SIDE * 4, 0xe8761e, way == 0);
                took[way] += now_ms() - t0;
            }
        }

        check(px[512 * SIDE + 512] == 0xffe8761eu && px[0] == 0,
              "the disc is drawn, and not its corners");
        printf("  a disc of radius 500 laid over 1024x1024: %.2f ms four "
               "pixels at a time, %.2f ms one at a time (%.1fx)\n",
               took[0] / ROUNDS, took[1] / ROUNDS,
               took[0] > 0 ? took[1] / took[0] : 0.0);
        raster_close(&r);
        free(px);
    }

    if (failures == 0) {
        printf("PASS: %d checks on the SVG rasteriser, on this machine.\n",
               checks);
        return 0;
    }

    printf("FAIL: %d of %d checks on the SVG rasteriser.\n", failures,
           checks);
    return 1;
}
