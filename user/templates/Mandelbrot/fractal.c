/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * fractal.c - the heavy part of Mandelbrot, in C: every pixel of a surface,
 * computed. Lua hands it the surface; it never computes a pixel offset
 * itself, because the pitch is the surface's to say.
 */

#include "kosmos_kit.h"

/* How many steps the point takes to leave the circle of radius 2. */
static int escape(double cr, double ci, int most)
{
    double zr = 0, zi = 0;
    int n = 0;

    while (n < most && zr * zr + zi * zi <= 4.0) {
        double t = zr * zr - zi * zi + cr;

        zi = 2 * zr * zi + ci;
        zr = t;
        n++;
    }

    return n;
}

/* Inside is black; outside, a colour from how quickly it left. */
static uint32_t colour(int n, int most)
{
    if (n >= most) {
        return 0xff05060a;
    }

    unsigned t = (unsigned)(n * 255 / most);
    unsigned r = t < 128 ? t * 2 : 255;
    unsigned g = t < 128 ? t : 255 - (t - 128);
    unsigned b = t < 128 ? 128 + t : 255 - (t - 128) * 2;

    return 0xff000000u | (r << 16) | (g << 8) | b;
}

/* fractal.fill(surface, iterations) */
static int l_fill(lua_State *L)
{
    unsigned w, h, pitch;
    uint32_t *px = kosmos_surface_pixels(L, 1, &w, &h, &pitch);
    int most = (int)luaL_optinteger(L, 2, 200);

    for (unsigned y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)((char *)px + (size_t)y * pitch);

        for (unsigned x = 0; x < w; x++) {
            double cr = -2.3 + 3.2 * x / w;
            double ci = -1.25 + 2.5 * y / h;

            row[x] = colour(escape(cr, ci, most), most);
        }
    }

    return 0;
}

KOSMOS_KIT(mandelbrot)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_fill);
    lua_setfield(L, -2, "fill");
}
