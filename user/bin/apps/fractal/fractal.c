/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **GPU Fractal's processor half** (`docs/compute.md` C4): the same
 * Mandelbrot picture the kernel computes, on one of the processor's cores,
 * for the times side by side - and the arithmetic the kernel's, so the two
 * pictures can be held to each other.
 *
 *   local fractal = use("/Kosmos/Kits/fractal")
 *   fractal.draw(surface, x0, dx, y0, dy, most)   -- every pixel of it
 *   fractal.differ(a, b)       -- how many pixels two surfaces disagree on
 *
 * **As the kernel does it, step for step** (`eukernels.lua`, `mandelbrot`):
 * in single precision; a pixel's point the corner plus its place times a
 * pixel's size; z = z^2 + c with the count n taken while |z|^2 < 4, |z|^2
 * from the squares before the step; z's imaginary part (zx zy + zx zy) + cy
 * and its real part (zx^2 - zy^2) + cx; and at most `most` steps. The
 * kernel's lanes go on stepping once they have left, which changes nothing:
 * a point that has left the circle of radius 2 never comes back.
 *
 * The colour the same too: red the low byte of 9n, green of 4n, blue of
 * 15n, black inside. The one place the two may part is the kernel's `mad`
 * for the point, which rounds once where this rounds twice - a pixel on an
 * edge now and then, which `differ` counts.
 */

#include <stdint.h>
#include <stddef.h>

#include "lua.h"
#include "lauxlib.h"

#include "kits/gfx/gfx_draw.h"

void kosmos_fractal_kit(lua_State *L);

static uint32_t colour(int32_t n, int32_t most)
{
    if (n >= most) {
        return 0xff000000u;
    }

    return 0xff000000u | (((uint32_t)(n * 9) & 0xffu) << 16) | (((uint32_t)(n * 4) & 0xffu) << 8)
         | ((uint32_t)(n * 15) & 0xffu);
}

static int32_t escape(float cx, float cy, int32_t most)
{
    float zx = 0.0f, zy = 0.0f;
    int32_t n = 0, k;

    for (k = 0; k < most; k++) {
        float xx = zx * zx, yy = zy * zy, t;

        if (!(xx + yy < 4.0f)) {
            break;
        }

        n++;
        t = zx * zy;
        zy = (t + t) + cy;
        zx = (xx - yy) + cx;
    }

    return n;
}

/* fractal.draw(surface, x0, dx, y0, dy, most) */
static int l_draw(lua_State *L)
{
    unsigned w, h, pitch, x, y;
    uint32_t *px = kosmos_surface_pixels(L, 1, &w, &h, &pitch);
    float x0 = (float)luaL_checknumber(L, 2), dx = (float)luaL_checknumber(L, 3);
    float y0 = (float)luaL_checknumber(L, 4), dy = (float)luaL_checknumber(L, 5);
    lua_Integer most = luaL_checkinteger(L, 6);

    luaL_argcheck(L, most >= 1 && most <= 1000000, 6, "1 to a million iterations");

    for (y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)((char *)px + (size_t)y * pitch);
        float cy = y0 + (float)y * dy;

        for (x = 0; x < w; x++) {
            row[x] = colour(escape(x0 + (float)x * dx, cy, (int32_t)most), (int32_t)most);
        }
    }

    return 0;
}

/* fractal.differ(a, b): the pixels of the smaller of the two that differ. */
static int l_differ(lua_State *L)
{
    unsigned aw, ah, ap, bw, bh, bp, x, y, w, h;
    const uint32_t *a = kosmos_surface_pixels(L, 1, &aw, &ah, &ap);
    const uint32_t *b = kosmos_surface_pixels(L, 2, &bw, &bh, &bp);
    lua_Integer n = 0;

    w = aw < bw ? aw : bw;
    h = ah < bh ? ah : bh;

    for (y = 0; y < h; y++) {
        const uint32_t *ra = (const uint32_t *)((const char *)a + (size_t)y * ap);
        const uint32_t *rb = (const uint32_t *)((const char *)b + (size_t)y * bp);

        for (x = 0; x < w; x++) {
            n += ra[x] != rb[x];
        }
    }

    lua_pushinteger(L, n);
    return 1;
}

void kosmos_fractal_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_draw);
    lua_setfield(L, -2, "draw");
    lua_pushcfunction(L, l_differ);
    lua_setfield(L, -2, "differ");
}
