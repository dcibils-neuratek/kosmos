/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Game Kit's software rasterizer: a surface, and every loop that fills it.
 *
 *   local Soft = use("/kits/game").soft
 *   local g = Soft.new(960, 540, gfx.surface{ w = 960, h = 540 })
 *   g:clear(0)  g:line(...)  g:sphere{...}  g:ring{...}  g:sun{...}
 *
 * **Why this exists, and it is a measurement rather than a preference.**
 * `user/lib/solar/soft.lua` is the same rasterizer in portable Lua, and it
 * is good code - the inner loops are hoisted, the blends inlined, nothing
 * allocated per pixel. What it cannot escape is where its pixels live: a
 * Lua table, because that is the only thing every interpreter has.
 *
 * Clearing 960x540 ten times, measured on this machine (`solar --bench`):
 *
 *     Lua into a table       24.81 ms
 *     C into that table      43.81 ms   0.56x
 *     C into a surface        1.32 ms  18.72x
 *
 * A table store from C is `lua_pushinteger` and `lua_rawseti` - the whole
 * table API, its boxing and its write barrier - and it costs about 85 ns a
 * pixel however clever the loop around it is. At 960x540 that is thirteen
 * milliseconds a frame in *stores alone*. A surface is two and a half
 * nanoseconds.
 *
 * So moving a loop to C buys nothing while the pixels are a table, and
 * moving the pixels is the whole of the win. That is why this file is the
 * *entire* rasterizer rather than a handful of accelerated primitives:
 * `sphere`, `ring` and `sun` index the framebuffer directly, so the moment
 * it stops being a table they have to come too. All of it or none of it.
 *
 * **It is written to draw the same picture**, function for function and
 * rounding for rounding, as `solar/soft.lua` - which stays exactly as its
 * author wrote it and still runs under stock `lua` and LOVE. A host picks
 * one of the two. A picture that differs is a bug in this file.
 *
 * Three places knowingly differ, every one of them in the safe direction,
 * and each is marked SAFER below: a texture read is bounds-checked, a
 * truncated PPM is refused rather than returned, and a short glyph reads
 * as blank instead of raising. The Lua would fault or throw in all three,
 * so nothing that works today can tell the difference.
 *
 * `roadmap.md` 4f: this is the Game Kit's 2D layer, and the solar system
 * is what proved it. Nothing here knows what a planet is.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#include "lua.h"
#include "lauxlib.h"

/*
 * The one door into a surface from outside `gfx.c`, which exists already:
 * it was exported for Doom, which renders into a buffer of its own and
 * needs the result copied in. A second way in would be a second thing to
 * keep in step with the struct.
 */
uint32_t *kosmos_surface_pixels(lua_State *L, int index,
                                unsigned *w, unsigned *h, unsigned *pitch);

#define SOFT_MT "kosmos.game.soft"

#define PI_D    3.14159265358979323846
#define INV_TAU (1.0 / (2.0 * PI_D))
#define INV_PI  (1.0 / PI_D)

/*
 * The rasterizer's state.
 *
 * The surface is kept in the userdata's own value slot rather than as a
 * bare pointer, so it cannot be collected while this is alive, and the
 * pixels are asked for again at the start of every call - once per
 * primitive, never per pixel, and still correct if the surface moved.
 */
struct soft {
    uint32_t *pixels;
    unsigned  pitch;        /* bytes to the next row; never width * 4 */
    int       w;
    int       h;
};

static struct soft *check_soft(lua_State *L)
{
    struct soft *s = luaL_checkudata(L, 1, SOFT_MT);
    unsigned w = 0, h = 0, pitch = 0;

    lua_getiuservalue(L, 1, 1);
    s->pixels = kosmos_surface_pixels(L, -1, &w, &h, &pitch);
    lua_pop(L, 1);

    if ((int)w != s->w || (int)h != s->h) {
        luaL_error(L, "surface is %dx%d, rasterizer is %dx%d",
                   (int)w, (int)h, s->w, s->h);
    }

    s->pitch = pitch;

    return s;
}

static uint32_t *row_of(const struct soft *s, int y)
{
    return (uint32_t *)((uint8_t *)s->pixels + (size_t)y * s->pitch);
}

/*
 * Lua's `%` on floats is a floored modulo - `a - floor(a / b) * b` - and
 * is never negative for a positive divisor. C's truncates toward zero and
 * would wrap a texture the wrong way at the seam.
 */
static double mod_floor(double a, double b)
{
    return a - floor(a / b) * b;
}

/* `Noise.smoothstep`, which `soft.lua` leans on in six places. */
static double smoothstep(double a, double b, double x)
{
    double t = (x - a) / (b - a);

    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;

    return t * t * (3.0 - 2.0 * t);
}

static double fract(double x)
{
    return x - floor(x);
}

/* `Noise.hash` and `Noise.vnoise`: the ring profile's fine structure. */
static double noise_hash(double x, double y, double z)
{
    double d;

    x = fract(x * 0.1031);
    y = fract(y * 0.1030);
    z = fract(z * 0.0973);

    d = x * (y + 33.33) + y * (x + 33.33) + z * (z + 33.33);

    x += d;
    y += d;
    z += d;

    return fract((x + y) * z);
}

static double vnoise(double x, double y, double z)
{
    double ix = floor(x), iy = floor(y), iz = floor(z);
    double fx = x - ix, fy = y - iy, fz = z - iz;
    double a, b, c, d;

    fx = fx * fx * (3.0 - 2.0 * fx);
    fy = fy * fy * (3.0 - 2.0 * fy);
    fz = fz * fz * (3.0 - 2.0 * fz);

    a = noise_hash(ix, iy, iz);
    a += (noise_hash(ix + 1, iy, iz) - a) * fx;
    b = noise_hash(ix, iy + 1, iz);
    b += (noise_hash(ix + 1, iy + 1, iz) - b) * fx;
    c = noise_hash(ix, iy, iz + 1);
    c += (noise_hash(ix + 1, iy, iz + 1) - c) * fx;
    d = noise_hash(ix, iy + 1, iz + 1);
    d += (noise_hash(ix + 1, iy + 1, iz + 1) - d) * fx;

    a += (b - a) * fy;
    c += (d - c) * fy;

    return a + (c - a) * fz;
}

/* ------------------------------------------------------------- the pixel */

static uint32_t pack(double r, double g, double b)
{
    return 0xff000000u | ((uint32_t)floor(r) << 16)
           | ((uint32_t)floor(g) << 8) | (uint32_t)floor(b);
}

/*
 * One pixel, blended: `soft.lua`'s `S:blend`, with the same three `floor`s.
 *
 * The alpha byte is set on the way out because a surface is opaque and the
 * compositor reads it; the portable renderer's table has no alpha byte at
 * all, which is the only difference between the two and is invisible.
 */
static void blend_px(const struct soft *s, int x, int y,
                     double r, double g, double b, double a)
{
    uint32_t *p, d;
    double dr, dg, db;

    if (x < 0 || y < 0 || x >= s->w || y >= s->h || a <= 0.0) {
        return;
    }

    p = row_of(s, y) + x;

    if (a >= 1.0) {
        *p = pack(r, g, b);
        return;
    }

    d = *p;
    dr = (double)((d >> 16) & 0xff);
    dg = (double)((d >> 8) & 0xff);
    db = (double)(d & 0xff);

    *p = pack(dr + (r - dr) * a, dg + (g - dg) * a, db + (b - db) * a);
}

/*
 * `fillBlock` and `blendBlock`: a step x step patch, for when `sphere`,
 * `ring` and `sun` shade once per block. `step` is the graphics level's
 * big lever, and cost falls with its square.
 */
static void fill_block(const struct soft *s, long px, long py, long step,
                       uint32_t c)
{
    long x1 = px + step - 1, y1 = py + step - 1;
    long x, y;

    if (x1 > s->w - 1) x1 = s->w - 1;
    if (y1 > s->h - 1) y1 = s->h - 1;

    for (y = py; y <= y1; y++) {
        uint32_t *row = row_of(s, (int)y);

        for (x = px; x <= x1; x++) {
            row[x] = c;
        }
    }
}

static void blend_block(const struct soft *s, long px, long py, long step,
                        double r, double g, double b, double a)
{
    double k = 1.0 - a;
    double sr = r * a, sg = g * a, sb = b * a;
    long x1 = px + step - 1, y1 = py + step - 1;
    long x, y;

    if (x1 > s->w - 1) x1 = s->w - 1;
    if (y1 > s->h - 1) y1 = s->h - 1;

    for (y = py; y <= y1; y++) {
        uint32_t *row = row_of(s, (int)y);

        for (x = px; x <= x1; x++) {
            uint32_t d = row[x];

            row[x] = pack(sr + (double)((d >> 16) & 0xff) * k,
                          sg + (double)((d >> 8) & 0xff) * k,
                          sb + (double)(d & 0xff) * k);
        }
    }
}

/* ---------------------------------------------------------------- clear */

static int l_clear(lua_State *L)
{
    struct soft *s = check_soft(L);
    uint32_t c = ((uint32_t)luaL_optinteger(L, 2, 0) & 0x00ffffffu)
                 | 0xff000000u;
    int x, y;

    for (y = 0; y < s->h; y++) {
        uint32_t *row = row_of(s, y);

        for (x = 0; x < s->w; x++) {
            row[x] = c;
        }
    }

    return 0;
}

static int l_blend(lua_State *L)
{
    struct soft *s = check_soft(L);

    blend_px(s, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
             luaL_checknumber(L, 4), luaL_checknumber(L, 5),
             luaL_checknumber(L, 6), luaL_checknumber(L, 7));

    return 0;
}

/* A point 1 to 3 pixels wide: size 2 is a soft plus, size 3 a soft 3x3. */
static int l_point(lua_State *L)
{
    struct soft *s = check_soft(L);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    long size = (long)luaL_checkinteger(L, 4);
    double r = luaL_checknumber(L, 5);
    double g = luaL_checknumber(L, 6);
    double b = luaL_checknumber(L, 7);
    double a = luaL_checknumber(L, 8);

    blend_px(s, x, y, r, g, b, a);

    if (size >= 2) {
        double e = a * ((size >= 3) ? 0.6 : 0.35);

        blend_px(s, x - 1, y, r, g, b, e);
        blend_px(s, x + 1, y, r, g, b, e);
        blend_px(s, x, y - 1, r, g, b, e);
        blend_px(s, x, y + 1, r, g, b, e);

        if (size >= 3) {
            e = a * 0.25;

            blend_px(s, x - 1, y - 1, r, g, b, e);
            blend_px(s, x + 1, y - 1, r, g, b, e);
            blend_px(s, x - 1, y + 1, r, g, b, e);
            blend_px(s, x + 1, y + 1, r, g, b, e);
        }
    }

    return 0;
}

/* ----------------------------------------------------------------- rect */

static int l_rect(lua_State *L)
{
    struct soft *s = check_soft(L);
    long x = (long)floor(luaL_checknumber(L, 2));
    long y = (long)floor(luaL_checknumber(L, 3));
    long w = (long)floor(luaL_checknumber(L, 4));
    long h = (long)floor(luaL_checknumber(L, 5));
    double r = luaL_checknumber(L, 6);
    double g = luaL_checknumber(L, 7);
    double b = luaL_checknumber(L, 8);
    double a = luaL_checknumber(L, 9);

    long x0 = (x > 0) ? x : 0;
    long x1 = (x + w - 1 < s->w - 1) ? (x + w - 1) : (s->w - 1);
    long y0 = (y > 0) ? y : 0;
    long y1 = (y + h - 1 < s->h - 1) ? (y + h - 1) : (s->h - 1);
    long xx, yy;

    if (a >= 1.0) {
        uint32_t c = pack(r, g, b);

        for (yy = y0; yy <= y1; yy++) {
            uint32_t *row = row_of(s, (int)yy);

            for (xx = x0; xx <= x1; xx++) {
                row[xx] = c;
            }
        }

        return 0;
    }

    {
        /* The same hoisting the Lua does: the source terms are constant
         * across the whole rectangle, so they leave the loop. */
        double k = 1.0 - a;
        double sr = r * a, sg = g * a, sb = b * a;

        for (yy = y0; yy <= y1; yy++) {
            uint32_t *row = row_of(s, (int)yy);

            for (xx = x0; xx <= x1; xx++) {
                uint32_t d = row[xx];

                row[xx] = pack(sr + (double)((d >> 16) & 0xff) * k,
                               sg + (double)((d >> 8) & 0xff) * k,
                               sb + (double)(d & 0xff) * k);
            }
        }
    }

    return 0;
}

static int l_frame(lua_State *L)
{
    struct soft *s = check_soft(L);
    long x = (long)floor(luaL_checknumber(L, 2));
    long y = (long)floor(luaL_checknumber(L, 3));
    long w = (long)floor(luaL_checknumber(L, 4));
    long h = (long)floor(luaL_checknumber(L, 5));
    double r = luaL_checknumber(L, 6);
    double g = luaL_checknumber(L, 7);
    double b = luaL_checknumber(L, 8);
    double a = luaL_checknumber(L, 9);
    long i;

    for (i = x; i <= x + w - 1; i++) {
        blend_px(s, (int)i, (int)y, r, g, b, a);
        blend_px(s, (int)i, (int)(y + h - 1), r, g, b, a);
    }

    for (i = y + 1; i <= y + h - 2; i++) {
        blend_px(s, (int)x, (int)i, r, g, b, a);
        blend_px(s, (int)(x + w - 1), (int)i, r, g, b, a);
    }

    return 0;
}

/* ----------------------------------------------------------------- line */

static bool clip_edge(double p, double q, double *t0, double *t1)
{
    double t;

    if (p == 0.0) {
        return q >= 0.0;
    }

    t = q / p;

    if (p < 0.0) {
        if (t > *t1) return false;
        if (t > *t0) *t0 = t;
    } else {
        if (t < *t0) return false;
        if (t < *t1) *t1 = t;
    }

    return true;
}

/*
 * Liang-Barsky, then Xiaolin Wu: `S:line`. Two details are load-bearing
 * and both are the Lua's.
 *
 * Both clipped ends are computed from the *original* start, because Lua
 * evaluates a multiple assignment's right-hand side in full before
 * assigning any of it. Taking the second end from an already-moved first
 * shortens every clipped line by exactly the amount the first one moved.
 *
 * And the gradient comes from the pre-swap delta - which is the same
 * number as the post-swap one, because the swap negates both terms of the
 * ratio. It is written post-swap here, and that is why that is safe.
 */
static void draw_line(const struct soft *s, double x0, double y0,
                      double x1, double y1,
                      double r, double g, double b, double a)
{
    double w = (double)s->w - 1.0;
    double h = (double)s->h - 1.0;
    double dx = x1 - x0, dy = y1 - y0;
    double t0 = 0.0, t1 = 1.0;
    double ox = x0, oy = y0;

    if (!(clip_edge(-dx, x0, &t0, &t1)
          && clip_edge(dx, w - x0, &t0, &t1)
          && clip_edge(-dy, y0, &t0, &t1)
          && clip_edge(dy, h - y0, &t0, &t1))) {
        return;
    }

    x0 = ox + t0 * dx;
    y0 = oy + t0 * dy;
    x1 = ox + t1 * dx;
    y1 = oy + t1 * dy;

    dx = x1 - x0;
    dy = y1 - y0;

    if (fabs(dx) >= fabs(dy)) {
        double grad, yv;
        long xs, xe, x;

        if (x0 > x1) {
            double sx = x0, sy = y0;

            x0 = x1; y0 = y1; x1 = sx; y1 = sy;
        }

        grad = (dx == 0.0) ? 0.0 : (y1 - y0) / (x1 - x0);
        xs = (long)floor(x0 + 0.5);
        xe = (long)floor(x1 + 0.5);
        yv = y0 + grad * ((double)xs - x0);

        for (x = xs; x <= xe; x++) {
            double yi = floor(yv);
            double f = yv - yi;

            blend_px(s, (int)x, (int)yi, r, g, b, a * (1.0 - f));
            blend_px(s, (int)x, (int)yi + 1, r, g, b, a * f);
            yv += grad;
        }

        return;
    }

    {
        double grad, xv;
        long ys, ye, y;

        if (y0 > y1) {
            double sx = x0, sy = y0;

            x0 = x1; y0 = y1; x1 = sx; y1 = sy;
        }

        /* |dy| > |dx| >= 0 here, so the divisor cannot be zero. */
        grad = (x1 - x0) / (y1 - y0);
        ys = (long)floor(y0 + 0.5);
        ye = (long)floor(y1 + 0.5);
        xv = x0 + grad * ((double)ys - y0);

        for (y = ys; y <= ye; y++) {
            double xi = floor(xv);
            double f = xv - xi;

            blend_px(s, (int)xi, (int)y, r, g, b, a * (1.0 - f));
            blend_px(s, (int)xi + 1, (int)y, r, g, b, a * f);
            xv += grad;
        }
    }
}

static int l_line(lua_State *L)
{
    struct soft *s = check_soft(L);

    draw_line(s, luaL_checknumber(L, 2), luaL_checknumber(L, 3),
              luaL_checknumber(L, 4), luaL_checknumber(L, 5),
              luaL_checknumber(L, 6), luaL_checknumber(L, 7),
              luaL_checknumber(L, 8), luaL_optnumber(L, 9, 1.0));

    return 0;
}

/*
 * `S:lineFast` - the same rejection test, one pixel a step and no coverage
 * arithmetic. About half the cost, and what an orbit drawn at a low
 * graphics level uses.
 */
static int l_line_fast(lua_State *L)
{
    struct soft *s = check_soft(L);
    double x0 = luaL_checknumber(L, 2), y0 = luaL_checknumber(L, 3);
    double x1 = luaL_checknumber(L, 4), y1 = luaL_checknumber(L, 5);
    double r = luaL_checknumber(L, 6), g = luaL_checknumber(L, 7);
    double b = luaL_checknumber(L, 8), a = luaL_optnumber(L, 9, 1.0);
    double w = (double)s->w - 1.0, h = (double)s->h - 1.0;
    double dx, dy, sx, sy, adx, ady;
    long n, i;

    if ((x0 < 0 && x1 < 0) || (y0 < 0 && y1 < 0)
        || (x0 > w && x1 > w) || (y0 > h && y1 > h)) {
        return 0;
    }

    dx = x1 - x0;
    dy = y1 - y0;
    adx = fabs(dx);
    ady = fabs(dy);
    n = (long)floor((adx > ady) ? adx : ady) + 1;

    if (n > 4000) {                     /* huge: let Liang-Barsky clip it */
        draw_line(s, x0, y0, x1, y1, r, g, b, a);
        return 0;
    }

    sx = dx / (double)n;
    sy = dy / (double)n;

    for (i = 0; i <= n; i++) {
        blend_px(s, (int)floor(x0 + 0.5), (int)floor(y0 + 0.5), r, g, b, a);
        x0 += sx;
        y0 += sy;
    }

    return 0;
}

static int l_circle(lua_State *L)
{
    struct soft *s = check_soft(L);
    double cx = luaL_checknumber(L, 2), cy = luaL_checknumber(L, 3);
    double rad = luaL_checknumber(L, 4);
    double r = luaL_checknumber(L, 5), g = luaL_checknumber(L, 6);
    double b = luaL_checknumber(L, 7), a = luaL_checknumber(L, 8);
    long n = (long)floor(rad * 0.8);
    double px, py;
    long i;

    if (n < 16) n = 16;

    px = cx + rad;
    py = cy;

    for (i = 1; i <= n; i++) {
        double t = (double)i / (double)n * 2.0 * PI_D;
        double x = cx + rad * cos(t);
        double y = cy + rad * sin(t);

        draw_line(s, px, py, x, y, r, g, b, a);
        px = x;
        py = y;
    }

    return 0;
}

/*
 * Monospace bitmap text: `S:text`. The font stays a Lua table - a cell
 * size and a glyph per byte, each a flat array of coverage 0..1 - because
 * it is read once per character rather than once per pixel, and a whole
 * line of text is a few thousand pixels rather than a few hundred
 * thousand. Moving it would buy nothing and cost a second format.
 */
static int l_text(lua_State *L)
{
    struct soft *s = check_soft(L);
    size_t len = 0;
    const char *str;
    double r, g, b, a;
    const char *align;
    long cw, ch, x, y, i;

    luaL_checktype(L, 2, LUA_TTABLE);
    str = luaL_checklstring(L, 3, &len);
    x = (long)floor(luaL_checknumber(L, 4));
    y = (long)floor(luaL_checknumber(L, 5));
    r = luaL_checknumber(L, 6);
    g = luaL_checknumber(L, 7);
    b = luaL_checknumber(L, 8);
    a = luaL_checknumber(L, 9);
    align = luaL_optstring(L, 10, "left");

    lua_getfield(L, 2, "cw");
    cw = (long)luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 2, "ch");
    ch = (long)luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 2, "glyphs");       /* stays on the stack below */
    luaL_checktype(L, -1, LUA_TTABLE);

    if (strcmp(align, "right") == 0) {
        x -= (long)len * cw;
    }

    for (i = 0; i < (long)len; i++) {
        lua_rawgeti(L, -1, (lua_Integer)(unsigned char)str[i]);

        if (lua_type(L, -1) == LUA_TTABLE) {
            long k = 1, yy, xx;

            for (yy = 0; yy < ch; yy++) {
                for (xx = 0; xx < cw; xx++) {
                    double v;

                    lua_rawgeti(L, -1, (lua_Integer)k);
                    /* SAFER: a glyph shorter than cw * ch reads as blank
                     * here, where the Lua raises on `nil > 0`. */
                    v = lua_tonumber(L, -1);
                    lua_pop(L, 1);

                    if (v > 0.0) {
                        blend_px(s, (int)(x + xx), (int)(y + yy),
                                 r, g, b, a * v);
                    }

                    k++;
                }
            }
        }

        lua_pop(L, 1);
        x += cw;
    }

    lua_pop(L, 1);

    return 0;
}

/* -------------------------------------------------------------- textures */

/*
 * A texture's pixels stay a Lua string, exactly as the portable renderer
 * keeps them: three bytes a texel rather than a table slot each, which is
 * twelve megabytes held as three strings instead of three million slots
 * for the collector to walk. What changes here is that the string's bytes
 * are found once per *call* instead of once per pixel.
 */
struct tex {
    const unsigned char *data;
    size_t               len;
    long                 w;
    long                 h;
};

/*
 * Reads `o.<name>` as a texture, leaving its `data` string on the stack so
 * the collector cannot take the bytes out from under the loop. Returns
 * whether it left a slot behind; the caller restores the stack at the end.
 */
static bool read_tex(lua_State *L, int idx, const char *name, struct tex *t)
{
    size_t len = 0;

    t->data = NULL;
    t->len = 0;
    t->w = 0;
    t->h = 0;

    lua_getfield(L, idx, name);

    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 1);
        return false;
    }

    lua_getfield(L, -1, "w");
    t->w = (long)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "h");
    t->h = (long)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "data");

    if (lua_type(L, -1) != LUA_TSTRING || t->w <= 0 || t->h <= 0) {
        lua_pop(L, 2);
        t->w = 0;
        t->h = 0;
        return false;
    }

    t->data = (const unsigned char *)lua_tolstring(L, -1, &len);
    t->len = len;

    lua_remove(L, -2);                  /* drop the table, keep `data` */

    return true;
}

/*
 * SAFER: `string.byte` past the end of the Lua's string returns nil and
 * the arithmetic after it raises; the same read here would run off the
 * end of the bytes, so a short texture reads as black instead. A
 * well-formed one never reaches it.
 */
static void texel(const struct tex *t, long at,
                  double *r, double *g, double *b)
{
    if (at < 0 || (size_t)at + 3 > t->len) {
        *r = 0.0;
        *g = 0.0;
        *b = 0.0;
        return;
    }

    *r = (double)t->data[at];
    *g = (double)t->data[at + 1];
    *b = (double)t->data[at + 2];
}

/* The byte offset of a texel from an equirectangular lookup. */
static long equirect(double ty, double u, long tw)
{
    return (long)(ty * (double)tw
                  + mod_floor(floor(u * (double)tw), (double)tw)) * 3;
}

/*
 * `S.parsePPM` - a binary PPM (P6, maxval 255) into { w, h, data }.
 *
 * SAFER: a file whose pixel data is shorter than w * h * 3 is refused
 * here, where the Lua hands back the short string and raises on the first
 * read past its end. Refusing means the body renders flat-shaded, which is
 * exactly what a texture that failed to load already does.
 */
static int l_parse_ppm(lua_State *L)
{
    size_t len = 0;
    const char *s;
    long w = 0, h = 0, mx = 0;
    size_t at;
    int field;

    if (lua_isnoneornil(L, 1)) {
        lua_pushnil(L);
        return 1;
    }

    s = luaL_checklstring(L, 1, &len);

    if (len < 2 || s[0] != 'P' || s[1] != '6') {
        lua_pushnil(L);
        return 1;
    }

    at = 2;

    for (field = 0; field < 3; field++) {
        long value = 0;

        while (at < len && (s[at] == ' ' || s[at] == '\n' || s[at] == '\r'
                            || s[at] == '\t' || s[at] == '\f'
                            || s[at] == '\v')) {
            at++;
        }

        if (at >= len || s[at] < '0' || s[at] > '9') {
            lua_pushnil(L);
            return 1;
        }

        while (at < len && s[at] >= '0' && s[at] <= '9') {
            value = value * 10 + (s[at] - '0');
            at++;
        }

        if (field == 0)      { w = value; }
        else if (field == 1) { h = value; }
        else                 { mx = value; }
    }

    /* Exactly one whitespace byte after the maxval, which is what the
     * format says and what the Lua's `%s()` capture consumed. */
    if (at < len) {
        at++;
    }

    if (mx != 255 || w <= 0 || h <= 0
        || len - at < (size_t)w * (size_t)h * 3u) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 3);
    lua_pushinteger(L, w);
    lua_setfield(L, -2, "w");
    lua_pushinteger(L, h);
    lua_setfield(L, -2, "h");
    lua_pushlstring(L, s + at, (size_t)w * (size_t)h * 3u);
    lua_setfield(L, -2, "data");

    return 1;
}

/* ------------------------------------------------------ Saturn's rings */

/*
 * The 1-D ring profile: density and colour against rho in planet radii,
 * with the C, B and A rings, the Cassini division and the Encke gap. Built
 * once on first use, because it costs a couple of thousand noise
 * evaluations and never changes afterwards.
 */
#define RING_IN  1.22
#define RING_OUT 2.29
#define RING_N   512

#define RING_IN2  (RING_IN * RING_IN)
#define RING_OUT2 (RING_OUT * RING_OUT)
#define RING_K    ((double)RING_N / (RING_OUT - RING_IN))

static double ring_dens[RING_N];
static double ring_r[RING_N], ring_g[RING_N], ring_b[RING_N];
static bool   ring_built;

static double band(double x, double a, double b)
{
    return smoothstep(a - 0.004, a + 0.004, x)
           * (1.0 - smoothstep(b - 0.004, b + 0.004, x));
}

static void build_ring_lut(void)
{
    int i;

    for (i = 0; i < RING_N; i++) {
        double rho = RING_IN + (RING_OUT - RING_IN)
                     * ((double)i + 0.5) / (double)RING_N;
        double dens = 0.20 * band(rho, 1.24, 1.53)      /* C ring */
                    + 0.90 * band(rho, 1.53, 1.95)      /* B ring */
                    + 0.04 * band(rho, 1.95, 2.03)      /* Cassini division */
                    + 0.58 * band(rho, 2.03, 2.27);     /* A ring */
        double fine, t, k;

        dens = dens * (1.0 - 0.85 * band(rho, 2.205, 2.217));   /* Encke */

        fine = (0.62 + 0.38 * vnoise(rho * 70.0, 1.3, 0.0))
               * (0.85 + 0.15 * vnoise(rho * 260.0, 4.1, 0.0));
        t = smoothstep(1.3, 1.7, rho);
        k = 0.8 + 0.25 * fine;

        ring_dens[i] = dens * fine;
        ring_r[i] = (148.0 + (230.0 - 148.0) * t) * k;
        ring_g[i] = (133.0 + (209.0 - 133.0) * t) * k;
        ring_b[i] = (115.0 + (168.0 - 115.0) * t) * k;
    }

    ring_built = true;
}

/*
 * Every caller has already established that rho is strictly inside the
 * ring, so this cannot fall outside. It is clamped anyway, because an
 * array index computed from a square root is not a thing to leave to the
 * reader's confidence.
 */
static int ring_index(double q2)
{
    long i = (long)floor((sqrt(q2) - RING_IN) * RING_K);

    if (i < 0) return 0;
    if (i >= RING_N) return RING_N - 1;

    return (int)i;
}

/* ---------------------------------------------------- reading the options */

static double opt_num(lua_State *L, int idx, const char *name, double def)
{
    double v;

    lua_getfield(L, idx, name);
    v = lua_isnil(L, -1) ? def : lua_tonumber(L, -1);
    lua_pop(L, 1);

    return v;
}

static bool opt_bool(lua_State *L, int idx, const char *name, bool def)
{
    bool v;

    lua_getfield(L, idx, name);
    v = lua_isnil(L, -1) ? def : (lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);

    return v;
}

static bool opt_vec3(lua_State *L, int idx, const char *name, double *out)
{
    int i;

    lua_getfield(L, idx, name);

    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 1);
        return false;
    }

    for (i = 0; i < 3; i++) {
        lua_rawgeti(L, -1, i + 1);
        out[i] = lua_tonumber(L, -1);
        lua_pop(L, 1);
    }

    lua_pop(L, 1);

    return true;
}

static void need_vec3(lua_State *L, int idx, const char *name, double *out)
{
    if (!opt_vec3(L, idx, name, out)) {
        luaL_error(L, "%s: a table of three numbers is required", name);
    }
}

/* -------------------------------------------------------------- sphere */

/*
 * A lit, textured sphere drawn as an orthographic impostor, which is what
 * the GPU version this was ported from does too.
 *
 * One `atan2` and one `acos` a pixel for the equirectangular lookup, and
 * this inner loop is the single most expensive thing in the whole
 * simulation - a planet that fills the screen is half a million of them.
 */
static int l_sphere(lua_State *L)
{
    struct soft *s = check_soft(L);
    int base;
    struct tex tex, aux;
    double cx, cy, R, atm, pad, Ro;
    double ax[3], ay[3], az[3], Lv[3];
    double fr = 128.0, fg = 128.0, fb = 128.0;
    double ar = 255.0, ag = 255.0, ab = 255.0;
    double col[3], acol[3];
    double cloudU, LN, hx, hy, hz, hl, edge, half;
    bool ring_shadow;
    long step, py, py0, py1;

    luaL_checktype(L, 2, LUA_TTABLE);
    base = lua_gettop(L);

    cx = opt_num(L, 2, "sx", 0.0);
    cy = opt_num(L, 2, "sy", 0.0);
    R  = opt_num(L, 2, "sr", 0.0);
    atm = opt_num(L, 2, "atm", 0.0);
    cloudU = opt_num(L, 2, "cloudU", 0.0);
    ring_shadow = opt_bool(L, 2, "ringShadow", false);
    step = (long)opt_num(L, 2, "step", 1.0);

    if (step < 1) step = 1;
    if (!(R > 0.0)) return 0;

    pad = (atm > 0.0) ? 1.35 : 1.0;
    Ro = R * pad;

    need_vec3(L, 2, "ax", ax);
    need_vec3(L, 2, "ay", ay);
    need_vec3(L, 2, "az", az);
    need_vec3(L, 2, "L", Lv);

    if (opt_vec3(L, 2, "color", col)) {
        fr = col[0];
        fg = col[1];
        fb = col[2];
    }

    if (opt_vec3(L, 2, "atmColor", acol)) {
        ar = acol[0];
        ag = acol[1];
        ab = acol[2];
    }

    read_tex(L, 2, "tex", &tex);
    read_tex(L, 2, "aux", &aux);

    if (ring_shadow && !ring_built) {
        build_ring_lut();
    }

    LN = Lv[0] * ay[0] + Lv[1] * ay[1] + Lv[2] * ay[2];

    /* The half vector for the ocean glint: normalize(L + view), and the
     * view direction is (0, 0, -1). */
    hx = Lv[0];
    hy = Lv[1];
    hz = Lv[2] - 1.0;
    hl = sqrt(hx * hx + hy * hy + hz * hz);
    if (hl < 1e-6) hl = 1.0;
    hx /= hl;
    hy /= hl;
    hz /= hl;

    edge = 1.0 - 1.5 / R;
    edge = (edge > 0.0) ? edge * edge : 0.0;
    half = (double)step * 0.5;

    py0 = (long)floor(cy - Ro);
    py1 = (long)floor(cy + Ro) + 1;
    if (py0 < 0) py0 = 0;
    if (py1 > s->h - 1) py1 = s->h - 1;

    for (py = py0; py <= py1; py += step) {
        double dy = (double)py + half - cy;
        double span2 = Ro * Ro - dy * dy;
        double span, ny;
        uint32_t *row;
        long px, px0, px1;

        if (span2 <= 0.0) {
            continue;
        }

        span = sqrt(span2);
        ny = -dy / R;
        row = row_of(s, (int)py);

        px0 = (long)floor(cx - span);
        px1 = (long)floor(cx + span);
        if (px0 < 0) px0 = 0;
        if (px1 > s->w - 1) px1 = s->w - 1;

        for (px = px0; px <= px1; px += step) {
            double nx = ((double)px + half - cx) / R;
            double r2 = nx * nx + ny * ny;

            if (r2 < 1.0) {
                double nz = -sqrt(1.0 - r2);
                double d = nx * Lv[0] + ny * Lv[1] + nz * Lv[2];
                double cr = fr, cg = fg, cb = fb;
                double u = 0.0, ty = 0.0;
                bool have_u = false;
                double lit, er = 0.0, eg = 0.0, eb = 0.0, spec = 0.0;
                double I, r_, g_, b_;

                if (tex.data != NULL) {
                    double lx = nx * ax[0] + ny * ax[1] + nz * ax[2];
                    double ly = nx * ay[0] + ny * ay[1] + nz * ay[2];
                    double lz = nx * az[0] + ny * az[1] + nz * az[2];

                    if (ly > 1.0) ly = 1.0;
                    else if (ly < -1.0) ly = -1.0;

                    u = atan2(lz, lx) * INV_TAU + 0.5;
                    have_u = true;

                    ty = floor(acos(ly) * INV_PI * (double)tex.h);
                    if (ty >= (double)tex.h) ty = (double)tex.h - 1.0;

                    texel(&tex, equirect(ty, u, tex.w), &cr, &cg, &cb);
                }

                lit = (d > 0.0) ? d : 0.0;

                if (atm > 0.0) {
                    lit += (smoothstep(-0.15, 0.55, d) - lit) * atm * 0.6;
                }

                /* Earth only: R is cloud, G the ocean mask, B city lights. */
                if (aux.data != NULL && have_u) {
                    double ay2 = floor(ty * (double)aux.h / (double)tex.h);
                    long k1 = (long)(ay2 * (double)aux.w
                                     + mod_floor(floor(u * (double)aux.w),
                                                 (double)aux.w)) * 3;
                    long k2 = (long)(ay2 * (double)aux.w
                                     + mod_floor(floor((u + cloudU)
                                                       * (double)aux.w),
                                                 (double)aux.w)) * 3;
                    double junk, ocean, lights, cloud, cl, c9;

                    texel(&aux, k1, &junk, &ocean, &lights);
                    texel(&aux, k2, &cloud, &junk, &junk);
                    cl = cloud / 255.0;

                    if (d > 0.0 && ocean > 0.0) {
                        double sd = nx * hx + ny * hy + nz * hz;

                        if (sd > 0.9) {
                            spec = pow(sd, 50.0) * (ocean / 255.0)
                                   * (1.0 - cl) * 0.7 * 255.0;
                        }
                    }

                    if (lights > 0.0 && d < 0.1) {
                        double e = (1.0 - smoothstep(-0.15, 0.1, d))
                                   * (lights / 255.0)
                                   * (1.0 - cl * 0.7) * 0.9;

                        er = 255.0 * e;
                        eg = 191.0 * e;
                        eb = 89.0 * e;
                    }

                    c9 = cl * 0.9;
                    cr += (255.0 - cr) * c9;
                    cg += (255.0 - cg) * c9;
                    cb += (255.0 - cb) * c9;
                }

                if (ring_shadow && lit > 0.0 && fabs(LN) > 1e-4) {
                    double t = -(nx * ay[0] + ny * ay[1] + nz * ay[2]) / LN;

                    if (t > 0.0) {
                        double qx = nx + t * Lv[0];
                        double qy = ny + t * Lv[1];
                        double qz = nz + t * Lv[2];
                        double q2 = qx * qx + qy * qy + qz * qz;

                        if (q2 > RING_IN2 && q2 < RING_OUT2) {
                            lit *= 1.0 - 0.8 * ring_dens[ring_index(q2)];
                        }
                    }
                }

                I = lit * 1.15 + 0.035;
                r_ = cr * I + spec + er;
                g_ = cg * I + spec * 0.95 + eg;
                b_ = cb * I + spec * 0.85 + eb;

                if (atm > 0.0) {                    /* limb haze */
                    double t = 1.0 + nz;
                    double rim = t * t * sqrt(t) * atm * 0.7;
                    double sm = smoothstep(-0.2, 0.5, d);

                    r_ += (ar * sm - r_) * rim;
                    g_ += (ag * sm - g_) * rim;
                    b_ += (ab * sm - b_) * rim;
                }

                if (r_ > 255.0) r_ = 255.0;
                if (g_ > 255.0) g_ = 255.0;
                if (b_ > 255.0) b_ = 255.0;

                if (step > 1) {
                    fill_block(s, px, py, step, pack(r_, g_, b_));
                    continue;
                }

                if (r2 > edge) {                /* anti-aliased silhouette */
                    double cov = (1.0 - sqrt(r2)) * R;

                    if (cov < 1.0) {
                        uint32_t dd = row[px];
                        double dr = (double)((dd >> 16) & 0xff);
                        double dg = (double)((dd >> 8) & 0xff);
                        double db = (double)(dd & 0xff);

                        r_ = dr + (r_ - dr) * cov;
                        g_ = dg + (g_ - dg) * cov;
                        b_ = db + (b_ - db) * cov;
                    }
                }

                row[px] = pack(r_, g_, b_);

            } else if (atm > 0.0) {             /* halo outside the disc */
                double r = sqrt(r2);
                double hh = 1.0 - (r - 1.0) / (pad - 1.0);

                if (hh > 0.0) {
                    double lit = smoothstep(-0.5, 0.6,
                                            (nx * Lv[0] + ny * Lv[1]) / r);
                    double a = atm * hh * hh * hh * hh * lit * 0.65;

                    if (a > 0.004) {
                        if (step > 1) {
                            blend_block(s, px, py, step, ar, ag, ab, a);
                        } else {
                            uint32_t dd = row[px];
                            double dr = (double)((dd >> 16) & 0xff);
                            double dg = (double)((dd >> 8) & 0xff);
                            double db = (double)(dd & 0xff);

                            row[px] = pack(dr + (ar - dr) * a,
                                           dg + (ag - dg) * a,
                                           db + (ab - db) * a);
                        }
                    }
                }
            }
        }
    }

    lua_settop(L, base);                /* the texture strings can go now */

    return 0;
}

/* ---------------------------------------------------------------- rings */

/*
 * The rings, as a per-pixel ray/plane intersection. Drawn after the
 * planet's sphere: the globe occludes the far side analytically, so there
 * is no front/back split to do, and the planet's shadow falls across them
 * from its own shadow cylinder.
 */
static int l_ring(lua_State *L)
{
    struct soft *s = check_soft(L);
    double cx, cy, R;
    double N[3], Lv[3];
    double light, ex, ey, nx2, ny2, inv_r, inv_nz, half;
    bool cast_shadow;
    long step, py, py0, py1;

    luaL_checktype(L, 2, LUA_TTABLE);

    if (!ring_built) {
        build_ring_lut();
    }

    cx = opt_num(L, 2, "sx", 0.0);
    cy = opt_num(L, 2, "sy", 0.0);
    R  = opt_num(L, 2, "sr", 0.0);
    step = (long)opt_num(L, 2, "step", 1.0);
    cast_shadow = opt_bool(L, 2, "shadow", true);

    if (step < 1) step = 1;
    if (!(R > 0.0)) return 0;

    need_vec3(L, 2, "N", N);
    need_vec3(L, 2, "L", Lv);

    if (fabs(N[2]) < 0.015) {           /* edge-on: nothing to see */
        return 0;
    }

    light = 0.6 + 0.5 * fabs(N[0] * Lv[0] + N[1] * Lv[1] + N[2] * Lv[2]);

    /* The screen extent of the tilted disc. */
    nx2 = 1.0 - N[0] * N[0];
    ny2 = 1.0 - N[1] * N[1];
    ex = RING_OUT * R * sqrt(nx2 > 0.0 ? nx2 : 0.0);
    ey = RING_OUT * R * sqrt(ny2 > 0.0 ? ny2 : 0.0);

    inv_r = 1.0 / R;
    inv_nz = 1.0 / N[2];
    half = (double)step * 0.5;

    py0 = (long)floor(cy - ey);
    py1 = (long)floor(cy + ey) + 1;
    if (py0 < 0) py0 = 0;
    if (py1 > s->h - 1) py1 = s->h - 1;

    for (py = py0; py <= py1; py += step) {
        double qy = -((double)py + half - cy) * inv_r;
        uint32_t *row = row_of(s, (int)py);
        long px, px0, px1;

        px0 = (long)floor(cx - ex);
        px1 = (long)floor(cx + ex) + 1;
        if (px0 < 0) px0 = 0;
        if (px1 > s->w - 1) px1 = s->w - 1;

        for (px = px0; px <= px1; px += step) {
            double qx = ((double)px + half - cx) * inv_r;
            double qz = -(qx * N[0] + qy * N[1]) * inv_nz;
            double r2 = qx * qx + qy * qy;
            double q2 = r2 + qz * qz;
            int k;
            double a, sh, along, m, r_, g_, b_;

            if (q2 <= RING_IN2 || q2 >= RING_OUT2) {
                continue;
            }

            /* Behind the globe, analytically. */
            if (r2 < 1.0 && qz > -sqrt(1.0 - r2)) {
                continue;
            }

            k = ring_index(q2);
            a = ring_dens[k];

            if (a <= 0.01) {
                continue;
            }

            sh = 1.0;
            along = qx * Lv[0] + qy * Lv[1] + qz * Lv[2];

            if (cast_shadow && along < 0.0) {
                double off = q2 - along * along;

                sh = 0.06 + 0.94 * smoothstep(0.96, 1.04,
                                              sqrt(off > 0.0 ? off : 0.0));
            }

            m = light * sh;
            r_ = ring_r[k] * m;
            g_ = ring_g[k] * m;
            b_ = ring_b[k] * m;

            if (r_ > 255.0) r_ = 255.0;
            if (g_ > 255.0) g_ = 255.0;
            if (b_ > 255.0) b_ = 255.0;

            if (step > 1) {
                blend_block(s, px, py, step, r_, g_, b_, a);
            } else {
                uint32_t dd = row[px];
                double dr = (double)((dd >> 16) & 0xff);
                double dg = (double)((dd >> 8) & 0xff);
                double db = (double)(dd & 0xff);

                row[px] = pack(dr + (r_ - dr) * a,
                               dg + (g_ - dg) * a,
                               db + (b_ - db) * a);
            }
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ sun */

/*
 * The Sun: a textured emissive disc with limb darkening, and a glow whose
 * falloff is a lookup table - one per extent, built on first use, because
 * the extent is a graphics-level knob and there are only ever a few.
 */
#define SUN_PAD    5.0
#define GLOW_N     256
#define GLOW_SLOTS 4

struct glow {
    double pad;
    double a[GLOW_N];
    double g[GLOW_N];
    double b[GLOW_N];
};

static struct glow glow_cache[GLOW_SLOTS];
static int glow_count;

static const struct glow *glow_lut(double pad)
{
    struct glow *lut;
    int i;

    for (i = 0; i < glow_count; i++) {
        if (glow_cache[i].pad == pad) {
            return &glow_cache[i];
        }
    }

    /* Full: the first slot is reused. A handful of graphics levels means
     * the table settles after a few frames and nothing is rebuilt. */
    lut = (glow_count < GLOW_SLOTS) ? &glow_cache[glow_count++]
                                    : &glow_cache[0];
    lut->pad = pad;

    for (i = 0; i < GLOW_N; i++) {
        double g = ((double)i + 0.5) / (double)GLOW_N * (pad - 1.0);
        double a = (0.85 * exp(-g * 2.4) + 0.35 / (1.0 + g * g * 6.0))
                   * (1.0 - smoothstep(1.0, pad, 1.0 + g));
        double gg = 184.0 + 51.0 * exp(-g * 6.0);
        double bb = 82.0 + 102.0 * exp(-g * 6.0);

        lut->a[i] = (a > 1.0) ? 1.0 : a;
        lut->g[i] = (gg < 255.0) ? gg : 255.0;
        lut->b[i] = (bb < 255.0) ? bb : 255.0;
    }

    return lut;
}

static int l_sun(lua_State *L)
{
    struct soft *s = check_soft(L);
    int base;
    struct tex tex;
    double cx, cy, R, pad, Ro, half;
    double ax[3], ay[3], az[3];
    const struct glow *glow = NULL;
    double gk = 0.0;
    long step, py, py0, py1;

    luaL_checktype(L, 2, LUA_TTABLE);
    base = lua_gettop(L);

    cx = opt_num(L, 2, "sx", 0.0);
    cy = opt_num(L, 2, "sy", 0.0);
    R  = opt_num(L, 2, "sr", 0.0);
    pad = opt_num(L, 2, "pad", SUN_PAD);
    step = (long)opt_num(L, 2, "step", 1.0);

    if (step < 1) step = 1;
    if (pad < 1.0) pad = 1.0;
    if (!(R > 0.0)) return 0;

    Ro = R * pad;
    half = (double)step * 0.5;

    need_vec3(L, 2, "ax", ax);
    need_vec3(L, 2, "ay", ay);
    need_vec3(L, 2, "az", az);

    read_tex(L, 2, "tex", &tex);

    if (pad > 1.0) {
        glow = glow_lut(pad);
        gk = (double)GLOW_N / (pad - 1.0);
    }

    py0 = (long)floor(cy - Ro);
    py1 = (long)floor(cy + Ro) + 1;
    if (py0 < 0) py0 = 0;
    if (py1 > s->h - 1) py1 = s->h - 1;

    for (py = py0; py <= py1; py += step) {
        double dy = (double)py + half - cy;
        double span2 = Ro * Ro - dy * dy;
        double span, ny;
        uint32_t *row;
        long px, px0, px1;

        if (span2 <= 0.0) {
            continue;
        }

        span = sqrt(span2);
        ny = -dy / R;
        row = row_of(s, (int)py);

        px0 = (long)floor(cx - span);
        px1 = (long)floor(cx + span);
        if (px0 < 0) px0 = 0;
        if (px1 > s->w - 1) px1 = s->w - 1;

        for (px = px0; px <= px1; px += step) {
            double nx = ((double)px + half - cx) / R;
            double r2 = nx * nx + ny * ny;

            if (r2 < 1.0) {
                double z = sqrt(1.0 - r2);
                double cr = 255.0, cg = 200.0, cb = 90.0;
                double m, r_, g_, b_;
                uint32_t c;

                if (tex.data != NULL) {
                    double nz = -z;
                    double lx = nx * ax[0] + ny * ax[1] + nz * ax[2];
                    double ly = nx * ay[0] + ny * ay[1] + nz * ay[2];
                    double lz = nx * az[0] + ny * az[1] + nz * az[2];
                    double ty;

                    if (ly > 1.0) ly = 1.0;
                    else if (ly < -1.0) ly = -1.0;

                    ty = floor(acos(ly) * INV_PI * (double)tex.h);
                    if (ty >= (double)tex.h) ty = (double)tex.h - 1.0;

                    texel(&tex,
                          equirect(ty, atan2(lz, lx) * INV_TAU + 0.5, tex.w),
                          &cr, &cg, &cb);
                }

                m = (0.55 + 0.45 * pow(z, 0.45)) * 1.35;  /* limb darkening */
                r_ = cr * m;
                g_ = cg * m;
                b_ = cb * m;

                if (r_ > 255.0) r_ = 255.0;
                if (g_ > 255.0) g_ = 255.0;
                if (b_ > 255.0) b_ = 255.0;

                c = pack(r_, g_, b_);

                if (step > 1) {
                    fill_block(s, px, py, step, c);
                } else {
                    row[px] = c;
                }

            } else if (glow != NULL) {
                long k = (long)floor((sqrt(r2) - 1.0) * gk);

                if (k >= 0 && k < GLOW_N) {
                    double a = glow->a[k];

                    if (a > 0.004) {
                        if (step > 1) {
                            blend_block(s, px, py, step,
                                        255.0, glow->g[k], glow->b[k], a);
                        } else {
                            uint32_t dd = row[px];
                            double dr = (double)((dd >> 16) & 0xff);
                            double dg = (double)((dd >> 8) & 0xff);
                            double db = (double)(dd & 0xff);

                            row[px] = pack(dr + (255.0 - dr) * a,
                                           dg + (glow->g[k] - dg) * a,
                                           db + (glow->b[k] - db) * a);
                        }
                    }
                }
            }
        }
    }

    lua_settop(L, base);

    return 0;
}

/* ----------------------------------------------------------- the object */

static const luaL_Reg soft_methods[] = {
    { "clear",    l_clear },
    { "blend",    l_blend },
    { "point",    l_point },
    { "rect",     l_rect },
    { "frame",    l_frame },
    { "line",     l_line },
    { "lineFast", l_line_fast },
    { "circle",   l_circle },
    { "text",     l_text },
    { "sphere",   l_sphere },
    { "ring",     l_ring },
    { "sun",      l_sun },
    { NULL, NULL }
};

/*
 * `g.w`, `g.h` and `g.fb` are read by the application - the first two to
 * scale a HUD authored at 540 rows, the third by the host to put the
 * picture on screen - so the object answers for them as the Lua table did,
 * and hands every other name to the methods.
 *
 * `g.fb` is the *surface* rather than an array of numbers, which is the
 * whole point of this file: a host that wants pixels asks the surface.
 */
static int l_index(lua_State *L)
{
    struct soft *s = luaL_checkudata(L, 1, SOFT_MT);
    const char *key = (lua_type(L, 2) == LUA_TSTRING)
                      ? lua_tostring(L, 2) : NULL;

    if (key != NULL) {
        if (strcmp(key, "w") == 0) {
            lua_pushinteger(L, s->w);
            return 1;
        }

        if (strcmp(key, "h") == 0) {
            lua_pushinteger(L, s->h);
            return 1;
        }

        if (strcmp(key, "fb") == 0) {
            lua_getiuservalue(L, 1, 1);
            return 1;
        }
    }

    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1));

    return 1;
}

/*
 * `Soft.new(w, h, surface)` - the same signature as the portable
 * renderer's, whose third argument has always been "any object indexable
 * 1 .. w*h that stores numbers". Here it must be a Kosmos surface, and it
 * is required rather than optional, because this file cannot make one:
 * `gfx` owns that, and one door into a surface is enough.
 */
static int l_new(lua_State *L)
{
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    unsigned sw = 0, sh = 0, pitch = 0;
    struct soft *s;

    if (lua_isnoneornil(L, 3)) {
        return luaL_error(L, "the C rasterizer needs a surface: "
                             "Soft.new(w, h, gfx.surface{ w = w, h = h })");
    }

    kosmos_surface_pixels(L, 3, &sw, &sh, &pitch);

    if ((int)sw != w || (int)sh != h) {
        return luaL_error(L, "surface is %dx%d, asked for %dx%d",
                          (int)sw, (int)sh, w, h);
    }

    s = lua_newuserdatauv(L, sizeof *s, 1);
    s->pixels = NULL;
    s->pitch = pitch;
    s->w = w;
    s->h = h;

    lua_pushvalue(L, 3);
    lua_setiuservalue(L, -2, 1);        /* it keeps the surface alive */

    luaL_setmetatable(L, SOFT_MT);

    /* `S.new` clears, and a surface is not guaranteed to arrive blank. */
    lua_pushcfunction(L, l_clear);
    lua_pushvalue(L, -2);
    lua_pushinteger(L, 0);
    lua_call(L, 2, 0);

    return 1;
}

/*
 * The module, shaped exactly like `solar/soft.lua`'s, so that a host can
 * swap one for the other and nothing above it changes.
 */
void kosmos_gamesoft_open(lua_State *L);

void kosmos_gamesoft_open(lua_State *L)
{
    luaL_newmetatable(L, SOFT_MT);

    lua_newtable(L);
    luaL_setfuncs(L, soft_methods, 0);
    lua_pushcclosure(L, l_index, 1);    /* __index closes over the methods */
    lua_setfield(L, -2, "__index");

    lua_pop(L, 1);

    lua_newtable(L);

    lua_pushcfunction(L, l_new);
    lua_setfield(L, -2, "new");

    lua_pushcfunction(L, l_parse_ppm);
    lua_setfield(L, -2, "parsePPM");

    lua_pushnumber(L, SUN_PAD);
    lua_setfield(L, -2, "SUN_PAD");

    lua_pushnumber(L, RING_OUT);
    lua_setfield(L, -2, "RING_OUT");
}
