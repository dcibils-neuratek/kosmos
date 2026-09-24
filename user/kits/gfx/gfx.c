/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `gfx`: surfaces, and the only place a pixel offset is ever computed.
 *
 * `gfx.md` §19.1 is the most important decision in that document and this
 * file is it, made concrete: **Lua tables carry intent, never pixels.** A
 * 1920x1080 image as a Lua table is 32 MB of TValue for 8 MB of picture, and
 * the collector walks two million slots every cycle. No optimisation saves
 * that; it is structural. So a surface is a userdata over flat bytes that
 * the GC frees but never traverses.
 *
 * The second rule, §19.3: **no line of Lua may compute a pixel offset.**
 * Anything doing `y * width + x` works under QEMU and produces a skewed
 * image on real hardware, because the pitch is almost never width * 4 - this
 * board's is 4160 for a 1024-pixel row precisely so that mistake surfaces
 * here. Every primitive below takes coordinates and does its own arithmetic
 * from `pitch`.
 *
 * The third, §19.2: **Lua decides what to draw, C draws it.** A Lua loop
 * costs 20-50ns an iteration, which is fine for the thousand pixels of a
 * line and unworkable for the two million of a full-screen filter. The
 * primitive set is therefore small and composable and is not meant to grow
 * much: fill, span, blit, blend, get, set. `map`, the escape hatch that
 * applies a Lua function per pixel, is M7 and is deliberately slow - if
 * something using it needs to be fast, that is the signal it has earned its
 * own primitive.
 *
 * Colour in Lua is always logical 0xAARRGGBB. Surfaces are always that
 * format, whatever the screen is; §19.3 says conversion to the device format
 * happens once, in the final blit, so that a new target changes one file.
 */

#include "shadow.h"
#include "yuv.h"
#include "cameraproto.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "gfx_draw.h"

#include "kosmos.h"

void kosmos_png_open(lua_State *L);
void kosmos_jpeg_open(lua_State *L);
void kosmos_docfont_open(lua_State *L);

#define SURFACE_MT  "kosmos.surface"

/*
 * Rows are aligned to a cache line.
 *
 * `gfx.md` §19.3: app surfaces are always the canonical format with the
 * pitch aligned to 64 bytes. It costs a few bytes a row and it means a row
 * never straddles a cache line it did not have to, which is the difference
 * that shows up in the blitter once there is one to measure.
 *
 * It also means the pitch is not width * 4 for most widths, which keeps the
 * discipline honest in the same way the framebuffer's padding does.
 */
#define ROW_ALIGN   64

struct surface {
    uint32_t *pixels;       /* NULL once freed; every method checks */
    unsigned  width;
    unsigned  height;
    unsigned  pitch;        /* bytes per row, never assume width * 4 */
    size_t    bytes;        /* what was mapped, for the GC accounting */
    size_t    pages;        /* what to hand back; 0 for the screen */
    bool      owned;        /* false for the screen: not ours to free */
};

static struct surface *check_surface(lua_State *L, int index)
{
    struct surface *s = luaL_checkudata(L, index, SURFACE_MT);

    /*
     * `gfx.md` §19.6: after free the handle is invalid and using it raises a
     * Lua error, never a segfault. That is the whole reason the pointer is
     * nulled rather than the userdata being left to dangle.
     */
    if (s->pixels == NULL) {
        luaL_error(L, "this surface has been freed");
    }

    return s;
}

/*
 * A surface's pixels, for C in another file.
 *
 * The one crack in "gfx.c is the only file that knows how a surface is laid
 * out", and it is deliberate rather than a leak: it hands back the *pitch*
 * along with the pointer, so the caller cannot compute a row offset without
 * being told the one number that makes it correct. A caller that ignored it
 * would be making the mistake `gfx.md` 19.3 names, with the right value in
 * its hand.
 *
 * It exists for Doom, which renders into a buffer of its own and needs the
 * result copied in. Nothing in Lua can reach it.
 */
uint32_t *kosmos_surface_pixels(lua_State *L, int index,
                                unsigned *w, unsigned *h, unsigned *pitch)
{
    struct surface *s = check_surface(L, index);

    if (w != NULL)     { *w = s->width; }
    if (h != NULL)     { *h = s->height; }
    if (pitch != NULL) { *pitch = s->pitch; }

    return s->pixels;
}

/* The start of a row. The one place row arithmetic happens. */
static uint32_t *row_of(const struct surface *s, unsigned y)
{
    return (uint32_t *)((uint8_t *)s->pixels + (size_t)y * s->pitch);
}

/*
 * Clips a rectangle to a surface, in place.
 *
 * Clipping rather than raising, because a window half off the edge of the
 * screen is the normal case and not an error. Returns false when nothing is
 * left, so callers can return without a special case for an empty rectangle.
 *
 * The coordinates arrive as signed, because a negative origin is exactly
 * what a partly-off-screen blit looks like, and the clip has to move the
 * source origin by the same amount it moves the destination.
 */
static bool clip(const struct surface *s, long *x, long *y, long *w, long *h,
                 long *sx, long *sy)
{
    if (*w <= 0 || *h <= 0) {
        return false;
    }

    if (*x < 0) {
        *w += *x;
        if (sx != NULL) {
            *sx -= *x;
        }
        *x = 0;
    }

    if (*y < 0) {
        *h += *y;
        if (sy != NULL) {
            *sy -= *y;
        }
        *y = 0;
    }

    if (*x + *w > (long)s->width) {
        *w = (long)s->width - *x;
    }

    if (*y + *h > (long)s->height) {
        *h = (long)s->height - *y;
    }

    return *w > 0 && *h > 0;
}

/*
 * (x * a) / 255, rounded, without a divide.
 *
 * The standard identity, and it is exact for every pair in 0..255 rather
 * than close: there is a test that checks all 65,536 of them against the
 * rounded quotient, because "exact" is the kind of claim that is repeated
 * from memory and occasionally wrong.
 */
static inline uint32_t mul255(uint32_t x, uint32_t a)
{
    uint32_t t = x * a + 128u;
    return (t + (t >> 8)) >> 8;
}

/* Source-over, with the source's own alpha scaled by a global one. */
static inline uint32_t over(uint32_t src, uint32_t dst, uint32_t global)
{
    uint32_t a = mul255((src >> 24) & 0xffu, global);
    uint32_t inv;

    if (a == 0) {
        return dst;
    }

    if (a == 255u) {
        return (src & 0x00ffffffu) | 0xff000000u;
    }

    inv = 255u - a;

    /*
     * **The destination's own alpha, which this used to assume was 255.**
     *
     * It ended `return 0xff000000u | ...`, and for a destination that is
     * opaque - a window's backbuffer, the screen - that is right and this
     * arithmetic is unchanged for it: `da` is 255, the weight below becomes
     * `inv`, and the result is opaque.
     *
     * The desktop is not that. It is a *transparent* surface with icons
     * drawn into it and blended over the wallpaper afterwards, so a
     * shadow pixel at 40% was composited against transparent black -
     * darkening it toward nothing - and then stamped fully opaque. Haiku's
     * soft drop shadows came out as hard black fringes, which is what a
     * person sees and calls a border round the icon.
     *
     * Source-over on straight alpha, written out: the result covers what
     * the source covers plus what the destination still shows through it,
     * and each contributes in proportion. The divide is the price of a
     * destination that is not opaque, and it is paid only on this path -
     * a fully transparent or fully opaque source has already returned.
     */
    {
        uint32_t da = (dst >> 24) & 0xffu;
        uint32_t wd = mul255(da, inv);
        uint32_t oa = a + wd;
        uint32_t r, g, b;

        if (oa == 0) {
            return 0;                   /* nothing covers anything */
        }

        r = (mul255((src >> 16) & 0xffu, a)
             + mul255((dst >> 16) & 0xffu, wd)) * 255u / oa;
        g = (mul255((src >>  8) & 0xffu, a)
             + mul255((dst >>  8) & 0xffu, wd)) * 255u / oa;
        b = (mul255((src      ) & 0xffu, a)
             + mul255((dst      ) & 0xffu, wd)) * 255u / oa;

        if (r > 255u) { r = 255u; }
        if (g > 255u) { g = 255u; }
        if (b > 255u) { b = 255u; }

        return (oa << 24) | (r << 16) | (g << 8) | b;
    }
}

/*
 * **The same thing, four pixels at a time - and the short circuit above is
 * why it is worth anything.**
 *
 * `frames` puts compositing at 84.7% of a busy pass, and compositing is
 * this loop. The kernel may not touch an FP or SIMD register -
 * `-mgeneral-regs-only`, which is what makes lazy FP save possible and is
 * worth 29.9% of a context switch - but **this is not the kernel.** `gfx.c`
 * is an EL0 process, so it may use the vector unit and pays the one fault
 * per time slice that `arch/aarch64/fp.c` already accounts for. A blitter
 * is precisely the thread that scheme was built to charge honestly.
 *
 * GCC's own vector types rather than intrinsics, so there is one
 * implementation rather than an SSE2 one and a NEON one. The compiler picks
 * the instructions.
 *
 * **Measured before writing, on real cores rather than under emulation**,
 * because QEMU translates every vector instruction one at a time and would
 * have answered the wrong question. At 1920x1080:
 *
 *     opaque       1200 -> 9865 Mpx/s
 *     transparent  2564 -> 7122
 *     mixed         525 -> 1293
 *
 * And the reading that decided the shape: **vectorising without the short
 * circuit is *slower* for transparent pixels** - 1546 against 2564 - because
 * removing a branch that almost always wins costs more than the lanes save.
 * The fast paths are kept and asked of four pixels at once instead of one,
 * which is why the opaque case is eight times quicker rather than one and a
 * bit: a window's interior stops being arithmetic and becomes a masked copy
 * at memory speed.
 *
 * The tail is the scalar loop. A row is rarely a multiple of four and a
 * branch at the end costs nothing next to what the body saves.
 */
typedef uint32_t u32x4 __attribute__((vector_size(16)));

static inline u32x4 mul255v(u32x4 x, u32x4 a)
{
    u32x4 t = x * a + 128u;
    return (t + (t >> 8)) >> 8;
}

/*
 * **Only for an opaque destination**, which is why `blend_row` checks before
 * calling it. `over` above has to compute the result's alpha because the
 * desktop is a transparent surface; here the destination covers everything
 * already, so the output is opaque and the weights collapse to `a` and
 * `255 - a` with no divide. Keeping the divide out of the vector path is
 * most of why this is eight times quicker.
 */
static inline u32x4 over4(u32x4 src, u32x4 dst, u32x4 global)
{
    u32x4 a   = mul255v((src >> 24) & 0xffu, global);
    u32x4 inv = 255u - a;

    u32x4 r = mul255v((src >> 16) & 0xffu, a) + mul255v((dst >> 16) & 0xffu, inv);
    u32x4 g = mul255v((src >>  8) & 0xffu, a) + mul255v((dst >>  8) & 0xffu, inv);
    u32x4 b = mul255v((src      ) & 0xffu, a) + mul255v((dst      ) & 0xffu, inv);

    return 0xff000000u | (r << 16) | (g << 8) | b;
}

/*
 * One row of source-over. `memcpy` into and out of the vectors rather than a
 * cast, because a surface's rows are only guaranteed 4-byte aligned - the
 * pitch is deliberately not width * 4 - and a misaligned vector load is a
 * fault on some machines and merely slow on others.
 */
static void blend_row(uint32_t *dp, const uint32_t *sp, long w,
                      uint32_t global)
{
    u32x4 gv = { global, global, global, global };
    long i = 0;

    for (; i + 4 <= w; i += 4) {
        u32x4 s, d;

        memcpy(&s, sp + i, sizeof s);

        /* The two cases `over` short-circuits, asked of four pixels. Only
         * when the global alpha is opaque, because otherwise the source's
         * own alpha is not the whole answer. */
        if (global == 255u) {
            u32x4 a = s >> 24;

            if (a[0] == 255u && a[1] == 255u && a[2] == 255u && a[3] == 255u) {
                u32x4 opaque = s | 0xff000000u;

                memcpy(dp + i, &opaque, sizeof opaque);
                continue;
            }

            if (a[0] == 0 && a[1] == 0 && a[2] == 0 && a[3] == 0) {
                continue;               /* nothing of the source shows */
            }
        }

        memcpy(&d, dp + i, sizeof d);

        /*
         * **The vector path assumes the destination is opaque**, which is
         * true of a window's backbuffer and of the screen, and false of the
         * desktop - a transparent surface with icons drawn into it. Getting
         * that wrong turned Haiku's soft drop shadows into hard black
         * fringes, so it is checked rather than assumed.
         *
         * Four comparisons against the alternative, which is a divide per
         * channel per pixel. The common case keeps its eight times.
         */
        {
            u32x4 da = d >> 24;

            if (da[0] == 255u && da[1] == 255u
                && da[2] == 255u && da[3] == 255u) {
                d = over4(s, d, gv);
                memcpy(dp + i, &d, sizeof d);
                continue;
            }
        }

        {
            long k;

            for (k = 0; k < 4; k++) {
                dp[i + k] = over(sp[i + k], dp[i + k], global);
            }
        }
    }

    for (; i < w; i++) {
        dp[i] = over(sp[i], dp[i], global);
    }
}

/* ------------------------------------------------------------------ */

static int l_new(lua_State *L)
{
    lua_Integer width;
    lua_Integer height;
    struct surface *s;
    unsigned pitch;
    size_t bytes;
    size_t pages;
    long mapped;
    void *pixels;

    /*
     * A table rather than two arguments, as `gfx.md` writes it:
     * gfx.surface{ w = 800, h = 600 }. Named because a surface will grow
     * more fields (a format, a backing choice) and positional arguments do
     * not survive that.
     */
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "w");
    width = luaL_checkinteger(L, -1);
    lua_getfield(L, 1, "h");
    height = luaL_checkinteger(L, -1);
    lua_pop(L, 2);

    if (width <= 0 || height <= 0) {
        return luaL_error(L, "a surface needs a positive width and height");
    }

    /* Bounded so the multiplication below cannot overflow before the
     * allocation gets a chance to fail. Far above anything this system will
     * ask for and far below where size_t wraps. */
    if (width > 16384 || height > 16384) {
        return luaL_error(L, "that surface is larger than this system allows");
    }

    pitch = (unsigned)(((width * 4) + (ROW_ALIGN - 1)) & ~(long)(ROW_ALIGN - 1));
    bytes = (size_t)pitch * (size_t)height;

    /*
     * Pages from the kernel, not the heap.
     *
     * The heap is 2 MB and deliberately so: `design.md` §5.2 wants
     * collections short, and the maximum GC pause is what decides whether
     * the system stutters. A full-screen surface is 3.2 MB and a
     * compositor's backbuffer is full-screen by definition, so putting
     * pixels there would mean choosing between a heap too big to collect
     * quickly and a compositor that cannot exist.
     *
     * They also do not belong there for a second reason: the collector would
     * be walking around several megabytes it can neither move nor look
     * inside, and every collection would be that much slower for nothing.
     */
    pages = (bytes + KOSMOS_PAGE_SIZE - 1) / KOSMOS_PAGE_SIZE;
    mapped = kosmos_map(pages);

    if (mapped < 0) {
        return luaL_error(L,
            "no room for a %dx%d surface (%d KB): the kernel refused %d pages",
            (int)width, (int)height, (int)(bytes / 1024), (int)pages);
    }

    pixels = (void *)(uintptr_t)mapped;      /* the kernel zeroed it */

    s = lua_newuserdatauv(L, sizeof(*s), 0);
    s->pixels = pixels;
    s->width  = (unsigned)width;
    s->height = (unsigned)height;
    s->pitch  = pitch;
    s->bytes  = bytes;
    s->pages  = pages;
    s->owned  = true;

    luaL_setmetatable(L, SURFACE_MT);

    /*
     * `gfx.md` §19.6: the collector sees a forty-byte userdata and feels no
     * pressure from the megabytes behind it, so a program can run the
     * machine out of memory while `collectgarbage("count")` reports that the
     * heap is nearly empty. Telling it the real size is what makes the
     * finalizer below something other than theoretical.
     */
    lua_gc(L, LUA_GCSTEP, (int)(bytes / 1024));

    return 1;
}

static int l_free(lua_State *L)
{
    struct surface *s = luaL_checkudata(L, 1, SURFACE_MT);

    /* Idempotent, and not an error: freeing twice is what a cleanup path
     * does when it cannot be sure. Using a freed surface is the error, and
     * check_surface raises it. */
    if (s->pixels != NULL && s->owned) {
        (void)kosmos_unmap((uintptr_t)s->pixels, s->pages);
    }

    s->pixels = NULL;
    return 0;
}

static int l_gc(lua_State *L)
{
    /*
     * The safety net, not the expected path (`gfx.md` §19.6). Normal code
     * calls free; this is what catches the surface that went out of scope
     * in an error path.
     */
    struct surface *s = luaL_checkudata(L, 1, SURFACE_MT);

    if (s->pixels != NULL && s->owned) {
        (void)kosmos_unmap((uintptr_t)s->pixels, s->pages);
        s->pixels = NULL;
    }

    return 0;
}

static int l_size(lua_State *L)
{
    struct surface *s = check_surface(L, 1);

    lua_pushinteger(L, s->width);
    lua_pushinteger(L, s->height);
    return 2;
}

/*
 * The pitch, exposed only so a test can assert it is not width * 4.
 *
 * Nothing that draws should ever want it: if a caller is reading this to do
 * its own arithmetic, that is the bug this whole file exists to prevent.
 */
static int l_pitch(lua_State *L)
{
    struct surface *s = check_surface(L, 1);

    lua_pushinteger(L, s->pitch);
    return 1;
}

static int l_fill(lua_State *L)
{
    struct surface *s = check_surface(L, 1);
    long x = (long)luaL_checkinteger(L, 2);
    long y = (long)luaL_checkinteger(L, 3);
    long w = (long)luaL_checkinteger(L, 4);
    long h = (long)luaL_checkinteger(L, 5);
    uint32_t colour = (uint32_t)luaL_checkinteger(L, 6);
    long row;

    if (!clip(s, &x, &y, &w, &h, NULL, NULL)) {
        return 0;
    }

    for (row = 0; row < h; row++) {
        uint32_t *p = row_of(s, (unsigned)(y + row)) + x;
        long i;

        for (i = 0; i < w; i++) {
            p[i] = colour;
        }
    }

    return 0;
}

/*
 * A filled circle.
 *
 * Added because it was measured, which is the bar `CLAUDE.md` sets before
 * anything moves to C. Drawn from Lua as a span per row it ran at 3 Mpx/s
 * against 208 for `fill` - seventy times slower for the same kind of work,
 * and not because of the pixels. A radius-15 disc is thirty-one separate
 * calls across the Lua boundary, and each crossing costs far more than the
 * thirty pixels it writes.
 *
 * So the loop moves and the arithmetic comes with it. One call, one
 * integer square root per row, and the same spans written by the same
 * code that `span` uses.
 *
 * The circle test is `dx*dx + dy*dy <= r*r` walked outward rather than a
 * square root per row: integer only, no libm here, and exact.
 */
/*
 * Mixing two colours by coverage.
 *
 * Integer only, and the rounding is `+ 127` so that a half-covered pixel
 * lands on the midpoint rather than a step below it - which over a whole
 * edge is the difference between a smooth line and one that reads slightly
 * thin.
 */
static uint32_t mix(uint32_t dst, uint32_t src, unsigned a)
{
    unsigned inv = 255u - a;
    unsigned r = ((((src >> 16) & 0xff) * a) + (((dst >> 16) & 0xff) * inv) + 127) / 255;
    unsigned g = ((((src >>  8) & 0xff) * a) + (((dst >>  8) & 0xff) * inv) + 127) / 255;
    unsigned b = ((((src      ) & 0xff) * a) + (((dst      ) & 0xff) * inv) + 127) / 255;

    return 0xff000000u | (r << 16) | (g << 8) | b;
}

static int l_disc(lua_State *L)
{
    struct surface *s = check_surface(L, 1);
    long cx = (long)luaL_checkinteger(L, 2);
    long cy = (long)luaL_checkinteger(L, 3);
    long r  = (long)luaL_checkinteger(L, 4);
    uint32_t colour = (uint32_t)luaL_checkinteger(L, 5);
    int smooth = lua_toboolean(L, 6);
    long dy;

    if (r <= 0) {
        return 0;
    }

    /*
     * A smooth edge, when asked for.
     *
     * Coverage from the squared distance rather than a real one: with
     * everything scaled by two, a pixel is solid inside (2r-1)^2, empty
     * outside (2r+1)^2, and blended in between. That is a straight line
     * through a curve and it is close enough at these radii - what the eye
     * objects to is a *step*, not a slightly wrong ramp.
     *
     * There is no libm here and no floating point in this file. This is
     * also why it lives in C at all: `gfx.md` 19.11 measured a Lua loop
     * calling a primitive per row at 3 Mpx/s, and this would be a call per
     * *pixel*.
     */
    if (smooth) {
        long inner = (2 * r - 1) * (2 * r - 1);
        long outer = (2 * r + 1) * (2 * r + 1);
        long dx;

        for (dy = -r - 1; dy <= r + 1; dy++) {
            long y = cy + dy;
            uint32_t *p;

            if (y < 0 || y >= (long)s->height) {
                continue;
            }

            p = row_of(s, (unsigned)y);

            for (dx = -r - 1; dx <= r + 1; dx++) {
                long x = cx + dx;
                long d2 = 4 * (dx * dx + dy * dy);

                if (x < 0 || x >= (long)s->width || d2 >= outer) {
                    continue;
                }

                if (d2 <= inner) {
                    p[x] = colour;
                } else {
                    unsigned a = (unsigned)((outer - d2) * 255 / (outer - inner));

                    p[x] = mix(p[x], colour, a);
                }
            }
        }

        return 0;
    }

    for (dy = -r; dy <= r; dy++) {
        long y = cy + dy;
        long dx = 0;
        long x0, x1;
        uint32_t *p;

        if (y < 0 || y >= (long)s->height) {
            continue;
        }

        /* The widest dx with dx*dx + dy*dy <= r*r. Walked up from zero,
         * which is a handful of steps and no floating point. */
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r) {
            dx++;
        }

        x0 = cx - dx;
        x1 = cx + dx;

        if (x0 < 0) {
            x0 = 0;
        }

        if (x1 >= (long)s->width) {
            x1 = (long)s->width - 1;
        }

        if (x1 < x0) {
            continue;
        }

        p = row_of(s, (unsigned)y);

        for (; x0 <= x1; x0++) {
            p[x0] = colour;
        }
    }

    return 0;
}

/*
 * A filled triangle.
 *
 * The one primitive the 3D engine needs that the others could not fake. A
 * rasteriser written in Lua would compute a pixel offset per pixel, which is
 * the rule `CLAUDE.md` states twice and the reason `l_span` above exists.
 * Everything the engine decides - where the vertices are, which faces to
 * draw, what colour each one is - stays in Lua. This fills the pixels.
 *
 * Doubles rather than fixed point. Userland is not built
 * -mgeneral-regs-only, the vertices arrive from Lua as doubles already, and
 * converting them to fixed point to avoid a cost that has not been measured
 * is the optimisation `CLAUDE.md` asks for a profile before.
 *
 * The fill rule is pixel centres: a pixel belongs to the triangle when its
 * centre does. Two triangles sharing an edge therefore meet without a seam
 * and without drawing the shared column twice, which matters as soon as a
 * mesh has more than one face and shows up as flickering along the joins.
 */
static long ifloor(double v)
{
    long i = (long)v;

    /* Truncation goes toward zero, which is not the floor for negatives -
     * and a vertex left of the screen is the normal case, not an error. */
    return (v < (double)i) ? i - 1 : i;
}

static void fill_span(struct surface *s, long y, double xa, double xb,
                      uint32_t colour)
{
    long x0, x1;
    uint32_t *p;

    if (xa > xb) {
        double t = xa; xa = xb; xb = t;
    }

    /* Centres again: the first centre at or right of xa, the last one left
     * of xb. `ceil(v)` is `-floor(-v)`; there is no libm here. */
    x0 = -ifloor(-(xa - 0.5));
    x1 = -ifloor(-(xb - 0.5)) - 1;

    if (x0 < 0) {
        x0 = 0;
    }

    if (x1 >= (long)s->width) {
        x1 = (long)s->width - 1;
    }

    if (x1 < x0) {
        return;
    }

    p = row_of(s, (unsigned)y);

    for (; x0 <= x1; x0++) {
        p[x0] = colour;
    }
}

static int l_triangle(lua_State *L)
{
    struct surface *s = check_surface(L, 1);
    double vx[3], vy[3];
    uint32_t colour = (uint32_t)luaL_checkinteger(L, 8);
    unsigned a = 0, b = 1, c = 2, t;
    long y, y_first, y_last;
    int i;

    for (i = 0; i < 3; i++) {
        vx[i] = (double)luaL_checknumber(L, 2 + i * 2);
        vy[i] = (double)luaL_checknumber(L, 3 + i * 2);
    }

    /* Sorted by y, as indices, so the coordinates are not copied around. */
    if (vy[a] > vy[b]) { t = a; a = b; b = t; }
    if (vy[b] > vy[c]) { t = b; b = c; c = t; }
    if (vy[a] > vy[b]) { t = a; a = b; b = t; }

    if (vy[c] - vy[a] <= 0.0) {
        return 0;               /* zero height: nothing to fill */
    }

    y_first = -ifloor(-(vy[a] - 0.5));
    y_last  = -ifloor(-(vy[c] - 0.5)) - 1;

    if (y_first < 0) {
        y_first = 0;
    }

    if (y_last >= (long)s->height) {
        y_last = (long)s->height - 1;
    }

    for (y = y_first; y <= y_last; y++) {
        double cy = (double)y + 0.5;
        double xl, xs;

        /* The long edge, a to c, is present for every row. */
        xl = vx[a] + (vx[c] - vx[a]) * (cy - vy[a]) / (vy[c] - vy[a]);

        if (cy < vy[b]) {
            if (vy[b] - vy[a] <= 0.0) {
                continue;       /* flat top: this half does not exist */
            }

            xs = vx[a] + (vx[b] - vx[a]) * (cy - vy[a]) / (vy[b] - vy[a]);
        } else {
            if (vy[c] - vy[b] <= 0.0) {
                continue;       /* flat bottom, likewise */
            }

            xs = vx[b] + (vx[c] - vx[b]) * (cy - vy[b]) / (vy[c] - vy[b]);
        }

        fill_span(s, y, xl, xs, colour);
    }

    return 0;
}

static int l_span(lua_State *L)
{
    /* One row. Separate from fill because a rasteriser emits spans and
     * paying fill's outer loop for a single one is the sort of thing that
     * shows up once there is a triangle to draw. */
    struct surface *s = check_surface(L, 1);
    long x   = (long)luaL_checkinteger(L, 2);
    long y   = (long)luaL_checkinteger(L, 3);
    long len = (long)luaL_checkinteger(L, 4);
    uint32_t colour = (uint32_t)luaL_checkinteger(L, 5);
    long h = 1;
    long i;
    uint32_t *p;

    if (!clip(s, &x, &y, &len, &h, NULL, NULL)) {
        return 0;
    }

    p = row_of(s, (unsigned)y) + x;

    for (i = 0; i < len; i++) {
        p[i] = colour;
    }

    return 0;
}

/*
 * dst:blit(src, sx, sy, w, h, dx, dy)
 *
 * The argument order is `gfx.md` §19.2's, and it reads as "take this
 * rectangle of src and put it there": the destination is the receiver, so
 * clipping is against the receiver, which is the surface that can actually
 * be overrun.
 */
static int l_blit(lua_State *L)
{
    struct surface *dst = check_surface(L, 1);
    struct surface *src = check_surface(L, 2);
    long sx = (long)luaL_checkinteger(L, 3);
    long sy = (long)luaL_checkinteger(L, 4);
    long w  = (long)luaL_checkinteger(L, 5);
    long h  = (long)luaL_checkinteger(L, 6);
    long dx = (long)luaL_checkinteger(L, 7);
    long dy = (long)luaL_checkinteger(L, 8);
    long row;

    /* Clipped against the source first, so a rectangle that runs off the
     * edge of what is being copied from cannot read past it. The destination
     * origin moves with it. */
    if (!clip(src, &sx, &sy, &w, &h, &dx, &dy)) {
        return 0;
    }

    if (!clip(dst, &dx, &dy, &w, &h, &sx, &sy)) {
        return 0;
    }

    for (row = 0; row < h; row++) {
        const uint32_t *sp = row_of(src, (unsigned)(sy + row)) + sx;
        uint32_t *dp = row_of(dst, (unsigned)(dy + row)) + dx;

        /* memcpy rather than a loop: it is the same operation and the libc
         * one is allowed to be clever about alignment. Copy, not move -
         * blitting a surface onto itself overlapping is not supported and
         * would need a direction choice. */
        memcpy(dp, sp, (size_t)w * 4);
    }

    return 0;
}

/*
 * **A rounded rectangle, as the one question both new primitives ask.**
 *
 * Is the destination pixel (x, y) inside the rectangle (rx, ry, rw, rh)
 * with corners of `r`? Everywhere but the four corner squares the answer is
 * yes without arithmetic, which is what makes a rounded blit cost almost
 * nothing: only `4 * r * r` pixels are ever tested, whatever the window's
 * size.
 *
 * The corner test is the circle's, on integers: a pixel is outside when its
 * distance from the corner's centre is more than the radius. Squared on
 * both sides, so there is no square root and no float - `CLAUDE.md` puts
 * floats out of the kernel and there is no reason to want one here either.
 *
 * Half a pixel is added to the radius before squaring. Without it the
 * boundary lands between two integers and the arc comes out visibly
 * flat-sided at small radii, which is exactly the size a window corner is.
 */
/*
 * `dst:blit_round(src, sx, sy, w, h, dx, dy, rx, ry, rw, rh, radius)`
 *
 * A blit that leaves the destination alone outside a rounded rectangle.
 *
 * **Written this way round because of how the desktop composes.** The
 * window manager paints back to front into one backbuffer, a damaged
 * rectangle at a time, so when a window's pixels are written everything
 * behind it is already there. A blit that skips the corner pixels therefore
 * *shows what is behind* without knowing or caching anything about it - and
 * it works when the damaged rectangle is only part of a corner, which a
 * "round off the window afterwards" primitive could not.
 *
 * The rounded rectangle is given in destination coordinates and separately
 * from the copy, because they are not the same thing: the copy is the piece
 * being repainted and the rounding belongs to the whole window.
 *
 * **A fourteenth argument inverts it**, copying only what is *outside* -
 * which is how the desktop actually rounds a window. Everything a window
 * draws goes through half a dozen calls (a gradient for the tab, fills for
 * the border, a blit for the content), and rounding each of them would be
 * six places to keep in step. So the manager saves the four corner squares
 * before the window is painted and puts them back afterwards with this,
 * which rounds the whole thing in one place and needs no drawing call to
 * know it is happening.
 */
static int l_blit_round(lua_State *L)
{
    struct surface *dst = check_surface(L, 1);
    struct surface *src = check_surface(L, 2);
    long sx = (long)luaL_checkinteger(L, 3);
    long sy = (long)luaL_checkinteger(L, 4);
    long w  = (long)luaL_checkinteger(L, 5);
    long h  = (long)luaL_checkinteger(L, 6);
    long dx = (long)luaL_checkinteger(L, 7);
    long dy = (long)luaL_checkinteger(L, 8);
    long rx = (long)luaL_checkinteger(L, 9);
    long ry = (long)luaL_checkinteger(L, 10);
    long rw = (long)luaL_checkinteger(L, 11);
    long rh = (long)luaL_checkinteger(L, 12);
    long r  = (long)luaL_checkinteger(L, 13);
    /* `outside` inverts the test, which is what puts a corner back: the
     * same geometry, copying exactly the pixels the other call skipped. */
    int outside = lua_toboolean(L, 14);
    long row;

    if (!clip(src, &sx, &sy, &w, &h, &dx, &dy)) {
        return 0;
    }

    if (!clip(dst, &dx, &dy, &w, &h, &sx, &sy)) {
        return 0;
    }

    for (row = 0; row < h; row++) {
        const uint32_t *sp = row_of(src, (unsigned)(sy + row)) + sx;
        uint32_t *dp = row_of(dst, (unsigned)(dy + row)) + dx;
        long y = dy + row;
        long col;

        /* A row that is clear of both corner bands is a memcpy, which is
         * every row of a window but the first and last `r` of them. The
         * inverted call has nothing to do on such a row at all. */
        if (y >= ry + r && y < ry + rh - r) {
            if (!outside) {
                memcpy(dp, sp, (size_t)w * 4);
            }

            continue;
        }

        for (col = 0; col < w; col++) {
            long cover = gfx_round_cover(dx + col, y, rx, ry, rw, rh, r);

            /* Restoring wants the complement: the background comes back
             * over exactly as much of the pixel as the window does not
             * cover. */
            if (outside) {
                cover = 255 - cover;
            }

            if (cover >= 255) {
                dp[col] = sp[col];
            } else if (cover > 0) {
                dp[col] = over((sp[col] & 0x00ffffffu)
                               | ((uint32_t)cover << 24), dp[col], 255);
            }
        }
    }

    return 0;
}

/*
 * `dst:shadow(rx, ry, rw, rh, radius, spread, alpha, cx, cy, cw, ch)`
 *
 * The soft edge a window casts. `shadow.c` draws it and says how.
 */
static int l_shadow(lua_State *L)
{
    struct surface *dst = check_surface(L, 1);
    long clip[4];
    int clipped = !lua_isnoneornil(L, 9);

    if (clipped) {
        clip[0] = (long)luaL_checkinteger(L, 9);
        clip[1] = (long)luaL_checkinteger(L, 10);
        clip[2] = (long)luaL_checkinteger(L, 11);
        clip[3] = (long)luaL_checkinteger(L, 12);
    }

    gfx_shadow(dst->pixels, dst->pitch, (long)dst->width, (long)dst->height,
               (long)luaL_checkinteger(L, 2), (long)luaL_checkinteger(L, 3),
               (long)luaL_checkinteger(L, 4), (long)luaL_checkinteger(L, 5),
               (long)luaL_checkinteger(L, 6), (long)luaL_checkinteger(L, 7),
               (long)luaL_optinteger(L, 8, 90), clipped ? clip : NULL);

    return 0;
}

/*
 * `dst:fill_round(x, y, w, h, colour, radius)`
 *
 * A filled rectangle with rounded corners, blended at the edge.
 *
 * The same geometry that rounds a window (`round_cover`), one level down:
 * there it decides which of two pictures a pixel comes from, here it decides
 * how much of a colour goes onto what is already there. A button drawn this
 * way sits on whatever is behind it without knowing what that is, which is
 * what lets one live in a list, on a title bar and on a card.
 *
 * Every row outside the corner bands is a plain fill, so a control of any
 * size costs the corners and nothing else.
 */
/*
 * One pixel of a rounded shape: `cover` is how much of the pixel the shape
 * takes (0..255), and the colour's own alpha says how opaque the shape is.
 *
 * **Both, multiplied.** The round primitives used to take only the
 * coverage and write the colour's alpha byte straight through where it was
 * whole, which is right for every opaque colour and wrong for any other: a
 * shadow under a switch's knob, `0x38000000`, came out solid black. A
 * translucent round fill is a thing the kit wants - a knob's shadow, a
 * selection laid over a picture - so the primitive does it rather than each
 * caller faking it with a grey.
 */
static inline void round_put(uint32_t *p, uint32_t colour, long cover)
{
    long a = (cover * (long)(colour >> 24) + 127) / 255;

    if (a >= 255) {
        *p = colour | 0xff000000u;
    } else if (a > 0) {
        *p = over((colour & 0x00ffffffu) | ((uint32_t)a << 24), *p, 255);
    }
}

static int l_fill_round(lua_State *L)
{
    struct surface *dst = check_surface(L, 1);
    long x = (long)luaL_checkinteger(L, 2);
    long y = (long)luaL_checkinteger(L, 3);
    long w = (long)luaL_checkinteger(L, 4);
    long h = (long)luaL_checkinteger(L, 5);
    uint32_t colour = (uint32_t)luaL_checkinteger(L, 6);
    long r = (long)luaL_optinteger(L, 7, 0);
    long row;

    if (w <= 0 || h <= 0) {
        return 0;
    }

    if (r * 2 > w) { r = w / 2; }
    if (r * 2 > h) { r = h / 2; }

    for (row = 0; row < h; row++) {
        long py = y + row;
        long col;

        if (py < 0 || py >= (long)dst->height) {
            continue;
        }

        for (col = 0; col < w; col++) {
            long px = x + col;
            long cover;
            uint32_t *p;

            if (px < 0 || px >= (long)dst->width) {
                continue;
            }

            cover = gfx_round_cover(px, py, x, y, w, h, r);

            if (cover <= 0) {
                continue;
            }

            p = row_of(dst, (unsigned)py) + px;
            round_put(p, colour, cover);
        }
    }

    return 0;
}

/*
 * `dst:frame_round(x, y, w, h, colour, radius)`
 *
 * A one-pixel outline of the same shape.
 *
 * The ring is the difference between two coverages - the rectangle and the
 * same rectangle inset by one - which gives a line that is solid on the
 * straights and correctly faint where the arc passes between two pixels.
 * Drawing it as four fills and four arcs would be the same picture with a
 * seam at each corner.
 */
static int l_frame_round(lua_State *L)
{
    struct surface *dst = check_surface(L, 1);
    long x = (long)luaL_checkinteger(L, 2);
    long y = (long)luaL_checkinteger(L, 3);
    long w = (long)luaL_checkinteger(L, 4);
    long h = (long)luaL_checkinteger(L, 5);
    uint32_t colour = (uint32_t)luaL_checkinteger(L, 6);
    long r = (long)luaL_optinteger(L, 7, 0);
    long row;

    if (w <= 2 || h <= 2) {
        return 0;
    }

    if (r * 2 > w) { r = w / 2; }
    if (r * 2 > h) { r = h / 2; }

    for (row = 0; row < h; row++) {
        long py = y + row;
        long col;

        /* Inside the straight part, only the two edge columns can be on the
         * ring - which is most of a control's height. */
        int band = (row < r || row >= h - r);

        if (py < 0 || py >= (long)dst->height) {
            continue;
        }

        for (col = 0; col < w; col++) {
            long px = x + col;
            long cover, inner, ring;
            uint32_t *p;

            if (!band && col > 0 && col < w - 1) {
                continue;
            }

            if (px < 0 || px >= (long)dst->width) {
                continue;
            }

            cover = gfx_round_cover(px, py, x, y, w, h, r);
            inner = gfx_round_cover(px, py, x + 1, y + 1, w - 2, h - 2,
                                r > 0 ? r - 1 : 0);
            ring = cover - inner;

            if (ring <= 0) {
                continue;
            }

            p = row_of(dst, (unsigned)py) + px;
            round_put(p, colour, ring);
        }
    }

    return 0;
}

static int l_blend(lua_State *L)
{
    struct surface *dst = check_surface(L, 1);
    struct surface *src = check_surface(L, 2);
    long sx = (long)luaL_checkinteger(L, 3);
    long sy = (long)luaL_checkinteger(L, 4);
    long w  = (long)luaL_checkinteger(L, 5);
    long h  = (long)luaL_checkinteger(L, 6);
    long dx = (long)luaL_checkinteger(L, 7);
    long dy = (long)luaL_checkinteger(L, 8);
    lua_Integer global = luaL_optinteger(L, 9, 255);
    long row;

    if (global < 0 || global > 255) {
        return luaL_error(L, "alpha is 0 to 255, not %d", (int)global);
    }

    if (!clip(src, &sx, &sy, &w, &h, &dx, &dy)) {
        return 0;
    }

    if (!clip(dst, &dx, &dy, &w, &h, &sx, &sy)) {
        return 0;
    }

    for (row = 0; row < h; row++) {
        const uint32_t *sp = row_of(src, (unsigned)(sy + row)) + sx;
        uint32_t *dp = row_of(dst, (unsigned)(dy + row)) + dx;
        blend_row(dp, sp, w, (uint32_t)global);
    }

    return 0;
}

/*
 * `dst:tint(src, sx, sy, w, h, dx, dy, colour)`
 *
 * **A picture used as a mask**: the source's alpha says how much of each
 * pixel is covered, and the colour comes from the call. The same arithmetic
 * a glyph is drawn with, for a shape that is not a glyph.
 *
 * It exists for line icons (`roadmap.md` 5zp): the sidebar in
 * `docs/preferences.html` draws each category's icon grey and the chosen
 * one in the accent, and a look's grey and a look's accent are different
 * colours in each of five looks. Storing an icon once per colour per look
 * would be forty-five pictures that drift; storing its coverage once and
 * painting through it is one picture and a number. The icon buttons of
 * `roadmap.md` 5zg are the same thing and will use it.
 *
 * The colour's own alpha multiplies the mask's, as `round_put` does for the
 * round shapes, so a faint icon is a colour rather than a second picture.
 */
static int l_tint(lua_State *L)
{
    struct surface *dst = check_surface(L, 1);
    struct surface *src = check_surface(L, 2);
    long sx = (long)luaL_checkinteger(L, 3);
    long sy = (long)luaL_checkinteger(L, 4);
    long w  = (long)luaL_checkinteger(L, 5);
    long h  = (long)luaL_checkinteger(L, 6);
    long dx = (long)luaL_checkinteger(L, 7);
    long dy = (long)luaL_checkinteger(L, 8);
    uint32_t colour = (uint32_t)luaL_checkinteger(L, 9);
    long row;

    if (!clip(src, &sx, &sy, &w, &h, &dx, &dy)) {
        return 0;
    }

    if (!clip(dst, &dx, &dy, &w, &h, &sx, &sy)) {
        return 0;
    }

    for (row = 0; row < h; row++) {
        const uint32_t *sp = row_of(src, (unsigned)(sy + row)) + sx;
        uint32_t *dp = row_of(dst, (unsigned)(dy + row)) + dx;
        long col;

        for (col = 0; col < w; col++) {
            long cover = (long)(sp[col] >> 24);

            if (cover > 0) {
                round_put(dp + col, colour, cover);
            }
        }
    }

    return 0;
}

/*
 * Text.
 *
 * `roadmap.md` M6 wants an 8x16 bitmap font before `stb_truetype`, and this
 * is it: Spleen 8x16, vendored unmodified under assets/fonts/ and turned
 * into an array by tools/bdf2c.py at build time. BSD-2-Clause, and the
 * notice travels in the generated file.
 *
 * A glyph is sixteen bytes, one per row, MSB leftmost - the VGA ROM layout,
 * which is why drawing one is a shift and a test rather than a lookup.
 *
 * It is a primitive rather than a Lua loop for the reason everything else
 * here is (`gfx.md` §19.2): a line of forty characters is five thousand
 * pixels, and a screen of them is a hundred and thirty thousand. Lua decides
 * what the string says and where it goes; C puts it there.
 */
extern const unsigned char font_8x16[];
extern const unsigned long font_8x16_len;

/*
 * What each glyph past the unknown box is. `tools/bdf2c.py` emits printable
 * ASCII, then one hollow box, then the Block Elements - which are not
 * contiguous with ASCII and so have to be named rather than computed.
 */
extern const unsigned short font_8x16_extra[];
extern const unsigned long  font_8x16_extra_len;

#define GLYPH_W     8
#define GLYPH_H     16
#define GLYPH_FIRST 0x20
#define GLYPH_LAST  0x7e

/*
 * The rows of one character, or the box the font uses for anything it does
 * not have. That glyph sits immediately after the ASCII range, which is what
 * makes "outside the range" a bounds check and not a special case - and the
 * Block Elements sit after *it*, which is why they are a search.
 *
 * The same lookup as `kernel/console.c`, and it has to be: they read the
 * same generated array, so a font whose layout only one of them understood
 * would draw one picture at the boot console and another in a window.
 */
static const unsigned char *glyph_of(unsigned c)
{
    unsigned index = (unsigned)(GLYPH_LAST - GLYPH_FIRST + 1);
    unsigned long i;

    if (c >= GLYPH_FIRST && c <= GLYPH_LAST) {
        index = (unsigned)(c - GLYPH_FIRST);
    } else {
        for (i = 0; i < font_8x16_extra_len; i++) {
            if (font_8x16_extra[i] == c) {
                index = (unsigned)(GLYPH_LAST - GLYPH_FIRST + 2 + i);
                break;
            }
        }
    }

    return font_8x16 + (size_t)index * GLYPH_H;
}

/*--------------------------------------------------------------------------
 * Outline fonts.
 *
 * The 8x16 bitmap from `assets/fonts/` is still here and is still the
 * default: it is exact, it costs nothing, and on a 1024-wide screen it is
 * perfectly readable. What it cannot be is *smooth*, and it cannot be any
 * other size.
 *
 * So a glyph can come from a TrueType outline instead, rasterised by
 * `stb_truetype` into 8-bit coverage and blended in. Rasterising happens
 * **once per glyph per size**, into a cache - a glyph is far more expensive
 * to make than to draw, and `gfx.md` 19.11 is the reason to care: this is
 * exactly the shape that must not be paid per frame.
 *
 * **Per-glyph metrics from the start, though both fonts here are
 * monospaced.** Every glyph carries its own advance and bearings, and
 * `text` walks a pen along by them rather than by a constant. That is what
 * a proportional font needs, and doing it now costs nothing while doing it
 * later would mean revisiting this loop with a working system depending on
 * it. What is *not* ready for proportional text is the fifty-odd places in
 * the applications that compute `#text * gfx.font.w`; `gfx.measure` exists
 * for them to move to.
 *------------------------------------------------------------------------*/

#include "stb_truetype.h"

struct kosmos_font_asset {
    const char          *name;
    const unsigned char *bytes;
    size_t               length;
};

extern const struct kosmos_font_asset fonts_table[];

#define GLYPH_MIN   32
#define GLYPH_MAX   126
#define GLYPH_COUNT (GLYPH_MAX - GLYPH_MIN + 1)

struct glyph {
    unsigned char *coverage;    /* w * h bytes, or NULL for a blank */
    int w, h;
    int xoff, yoff;             /* from the pen, at the baseline */
    int advance;
};

static int font_table_ref = LUA_NOREF;

/*
 * Three fonts, not one.
 *
 * A titlebar, a paragraph and a terminal want different faces, and a
 * terminal's *must* be fixed-width whatever the other two are - so one
 * setting for all text was always going to be wrong the moment a
 * proportional face existed. Roles rather than a font per widget: three is
 * the number of decisions somebody actually has, and a fourth can be added
 * the day something needs one.
 *
 * ROLE_UI is what everything draws with unless it says otherwise, which
 * keeps every existing `text` call working.
 */
#define ROLE_UI      0
#define ROLE_TEXT    1
#define ROLE_MONO    2
#define ROLE_TITLE   3
/*
 * **A heading, since 20 September.** It arrived with the style guide as a
 * face applications were told to use, and `role_of` had no name for it - so
 * every `heading` asked for fell through to `ROLE_UI` and was drawn in the
 * widget font, which is precisely the silent wrong-font failure the comment
 * below this one describes. Diego chose to put it in the Appearance panel
 * ("3 yes"), and a role a person can set has to be a role the drawing knows.
 */
#define ROLE_HEADING 4
/*
 * **A label, since 24 September**: the name of a thing - a settings row's
 * name, the chosen place in a sidebar, a header's crumb - which the
 * application mockups draw in the reading size at medium weight, where
 * notes and running text are regular. It is a role because it is the same
 * decision in every window: `docs/preferences.html` and `docs/tracker2.html`
 * both make it, and a medium face asked for by name in each application
 * would be the choosing-a-size-each that roles exist to end.
 */
#define ROLE_LABEL   5
#define ROLE_COUNT   6

/*
 * Where a codepoint outside printable ASCII lives.
 *
 * The 95 below are rasterised when the font loads, because every one of
 * them is on the screen within a second and an array indexed by
 * `codepoint - 32` is as fast as a lookup gets. Unicode cannot be
 * pre-rasterised, so the rest arrive on demand and stay.
 *
 * A hundred and twenty-eight, open-addressed, and nothing is ever evicted:
 * a page of accented Latin, Greek or Cyrillic fits several times over, and
 * a document that overflows it draws `?` rather than growing without a
 * bound. Fixed pools are what this system does everywhere else.
 */
#define WIDE_SLOTS  128

struct outline_font {
    bool  loaded;
    char  name[24];
    int   px;
    int   ascent, descent, line_gap;
    int   widest;
    struct glyph glyphs[GLYPH_COUNT];

    /* Kept so a glyph can be rasterised long after the font was loaded.
     * `info` points into the asset, which is in the image and static. */
    stbtt_fontinfo info;
    float scale;

    unsigned     wide_cp[WIDE_SLOTS];       /* 0 means the slot is free */
    struct glyph wide[WIDE_SLOTS];
};

/*
 * One codepoint from a UTF-8 string, and how many bytes it was.
 *
 * **The text path used to cast each byte to a codepoint**, so a three-byte
 * character became three lookups, each outside the cache, each drawing a
 * box. That is what the conformance benchmark showed: not one missing
 * glyph but three failed ones per character.
 *
 * Malformed input yields U+FFFD and advances one byte, which is what keeps
 * a bad encoding from turning into an infinite loop.
 */
static unsigned utf8_next(const char *str, size_t len, size_t *at)
{
    const unsigned char *s = (const unsigned char *)str + *at;
    size_t left = len - *at;
    unsigned cp;
    unsigned need;

    if (s[0] < 0x80u) {
        *at += 1;
        return s[0];
    }

    if ((s[0] & 0xe0u) == 0xc0u)      { cp = s[0] & 0x1fu; need = 1; }
    else if ((s[0] & 0xf0u) == 0xe0u) { cp = s[0] & 0x0fu; need = 2; }
    else if ((s[0] & 0xf8u) == 0xf0u) { cp = s[0] & 0x07u; need = 3; }
    else                              { *at += 1; return 0xfffdu; }

    if (left <= need) {
        *at += 1;
        return 0xfffdu;
    }

    {
        unsigned i;

        for (i = 1; i <= need; i++) {
            if ((s[i] & 0xc0u) != 0x80u) {
                *at += 1;
                return 0xfffdu;
            }

            cp = (cp << 6) | (s[i] & 0x3fu);
        }
    }

    *at += need + 1;
    return cp;
}

/*
 * Every face this process has open.
 *
 * The first `ROLE_COUNT` are the desktop's roles and are what every widget
 * draws with. The rest are asked for by *name and size* through
 * `gfx.face`, because a page is not four faces: a heading, a paragraph and
 * a quotation differ in size on the same screen, and layout needs all of
 * them at once rather than one at a time.
 *
 * Twelve, fixed, and nothing is evicted. A face costs its 95 eager ASCII
 * glyphs plus whatever the page reaches for, so a dozen is tens of
 * kilobytes rather than hundreds; when they run out `gfx.face` says so and
 * the caller uses one it already has. Fixed pools with an honest refusal
 * are what this system does everywhere.
 *
 * **A face is addressed by an index into this array**, which is what lets
 * `measure`, `height` and drawing take either a role name or a face - they
 * were already taking an index, and a role is just one of the first four.
 */
/*
 * **The sized pool is eight, whatever the roles come to.**
 *
 * This was a flat 12 with four roles, so the eight slots a caller can ask
 * for by size were what happened to be left over - and the day a fifth role
 * arrived (`heading`, 20 September) they silently became seven. Nothing
 * said so: `gfx.face` answers "no room for another face" exactly as it does
 * when a program really has asked for nine sizes, and the display suite
 * found it as two faces that would not load.
 *
 * Written as a sum so that the next role costs a slot of its own rather
 * than one of these.
 */
#define FACES_SIZED 8
#define FACES_MAX   (ROLE_COUNT + FACES_SIZED)

static struct outline_font faces[FACES_MAX];

/*
 * Which face a call names: a role by name, or a face by the number
 * `gfx.face` handed back. Nothing means the interface font.
 */
static int role_of(lua_State *L, int index)
{
    const char *name;

    if (lua_type(L, index) == LUA_TNUMBER) {
        int at = (int)lua_tointeger(L, index);

        return (at >= 0 && at < FACES_MAX) ? at : ROLE_UI;
    }

    name = luaL_optstring(L, index, "ui");

    /*
     * Compared in full, which it was not.
     *
     * This tested the first letter alone - 't' meant text, 'm' meant mono -
     * and that was fine for exactly as long as there were three roles whose
     * names began with different letters. "title" begins with a 't', so a
     * title bar asking for its own face silently got the paragraph font,
     * and there is no error anywhere for a font that is merely the wrong
     * one.
     */
    if (strcmp(name, "text") == 0)  return ROLE_TEXT;
    if (strcmp(name, "mono") == 0)  return ROLE_MONO;
    if (strcmp(name, "title") == 0) return ROLE_TITLE;
    if (strcmp(name, "heading") == 0) return ROLE_HEADING;
    if (strcmp(name, "label") == 0) return ROLE_LABEL;

    return ROLE_UI;
}

/*
 * The glyph for a codepoint, rasterising it if this is the first time.
 *
 * ASCII takes the array and costs a subtraction, which is what it cost
 * before and is why the common path did not change. Anything else probes
 * the wide table and, on a miss, is rasterised once and kept.
 *
 * `f` is const because drawing does not change what a font *is*; the cache
 * is a memo and the cast says so rather than pushing non-const through
 * every caller of a drawing routine.
 */
static const struct glyph *glyph_for(const struct outline_font *cf, unsigned cp)
{
    struct outline_font *f = (struct outline_font *)cf;
    unsigned slot, tried;

    if (cp >= GLYPH_MIN && cp <= GLYPH_MAX) {
        return &f->glyphs[cp - GLYPH_MIN];
    }

    if (cp == 0) {
        return &f->glyphs['?' - GLYPH_MIN];
    }

    slot = cp % WIDE_SLOTS;

    for (tried = 0; tried < WIDE_SLOTS; tried++) {
        if (f->wide_cp[slot] == cp) {
            return &f->wide[slot];
        }

        if (f->wide_cp[slot] == 0) {
            struct glyph *gl = &f->wide[slot];
            int adv, lsb;

            /* A codepoint the face does not have rasterises to nothing, and
             * `?` is a better answer than an empty box the width of a space:
             * it says something is missing rather than hiding it. */
            if (stbtt_FindGlyphIndex(&f->info, (int)cp) == 0) {
                return &f->glyphs['?' - GLYPH_MIN];
            }

            stbtt_GetCodepointHMetrics(&f->info, (int)cp, &adv, &lsb);
            gl->advance = (int)(adv * f->scale + 0.5f);
            gl->coverage = stbtt_GetCodepointBitmap(&f->info, f->scale,
                                                    f->scale, (int)cp,
                                                    &gl->w, &gl->h,
                                                    &gl->xoff, &gl->yoff);
            f->wide_cp[slot] = cp;
            return gl;
        }

        slot = (slot + 1) % WIDE_SLOTS;
    }

    /* Full. Bounded pools mean this can happen, and saying so with a `?` is
     * better than evicting something the next character will want back. */
    return &f->glyphs['?' - GLYPH_MIN];
}

static void outline_release(struct outline_font *f)
{
    int i;

    for (i = 0; i < GLYPH_COUNT; i++) {
        if (f->glyphs[i].coverage != NULL) {
            free(f->glyphs[i].coverage);
            f->glyphs[i].coverage = NULL;
        }
    }

    for (i = 0; i < WIDE_SLOTS; i++) {
        if (f->wide[i].coverage != NULL) {
            free(f->wide[i].coverage);
            f->wide[i].coverage = NULL;
        }

        f->wide_cp[i] = 0;
    }

    f->loaded = false;
}

/*
 * The name somebody types, from the name of the file.
 *
 * "IBMPlexMono-Regular.ttf" is "ibmplexmono": everything before the first
 * `-` or `.`, lowercased. So a font is added by dropping it in
 * `assets/fonts/` and nothing here has to learn about it.
 *
 * The first version matched two characters - `want[0] == 'p' && n[0] ==
 * 'I'` - which worked exactly as long as there were two fonts, and was
 * wrong the moment a third arrived.
 */
static void font_short_name(const char *file, char *out, size_t max)
{
    static const char regular[] = "-regular";
    size_t i, n;

    /*
     * The whole stem, lowercased, with `-Regular` dropped.
     *
     * **It used to stop at the first `-`**, which was exactly right while
     * every file in `assets/fonts/` ended `-Regular.ttf` and became its
     * family name. The moment a second weight arrives that rule collapses
     * them: `IBMPlexSans-Bold.ttf` and `IBMPlexSans-Regular.ttf` both
     * become `ibmplexsans`, and `FONT_FILES` is sorted - so "Bold" comes
     * before "Regular" and the desktop's mono font silently becomes bold.
     *
     * Dropping only `-Regular` keeps every existing name byte for byte,
     * because every existing file has it, and gives the new weights names
     * of their own: `ibmplexsans-bold`, `ibmplexsans-italic`.
     */
    for (i = 0; i + 1 < max && file[i] != '\0' && file[i] != '.'; i++) {
        char c = file[i];

        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }

    out[i] = '\0';

    n = sizeof(regular) - 1;

    if (i > n && memcmp(out + i - n, regular, n) == 0) {
        out[i - n] = '\0';
    }
}

static const struct kosmos_font_asset *font_asset(const char *want)
{
    unsigned i;

    for (i = 0; fonts_table[i].name != NULL; i++) {
        char shortname[32];

        font_short_name(fonts_table[i].name, shortname, sizeof(shortname));

        if (strcmp(want, shortname) == 0) {
            return &fonts_table[i];
        }
    }

    /*
     * Then a prefix, and only after every exact name has been tried.
     *
     * A prefix is a convenience - "ibm" reaches `ibmplexsans` - and it
     * became a hazard when weights arrived: `ibmplexsans` is a prefix of
     * `ibmplexsans-bold`, which sorts first, so the shorthand would have
     * quietly returned the wrong weight. An exact name now always wins over
     * a longer one that merely starts with it.
     */
    for (i = 0; fonts_table[i].name != NULL; i++) {
        char shortname[32];
        size_t n;

        font_short_name(fonts_table[i].name, shortname, sizeof(shortname));

        for (n = 0; want[n] != '\0' && shortname[n] != '\0'; n++) {
            if (want[n] != shortname[n]) {
                break;
            }
        }

        if (want[n] == '\0' && n > 0) {
            return &fonts_table[i];
        }
    }

    return NULL;
}

static bool outline_load(struct outline_font *f, const char *name, int px)
{
    const struct kosmos_font_asset *asset = font_asset(name);
    stbtt_fontinfo info;
    float scale;
    int ch;

    if (asset == NULL || px < 6 || px > 64) {
        return false;
    }

    if (!stbtt_InitFont(&info, asset->bytes,
                        stbtt_GetFontOffsetForIndex(asset->bytes, 0))) {
        return false;
    }

    outline_release(f);

    scale = stbtt_ScaleForPixelHeight(&info, (float)px);

    {
        int a, d, g;

        stbtt_GetFontVMetrics(&info, &a, &d, &g);
        f->ascent   = (int)(a * scale + 0.5f);
        f->descent  = (int)(-d * scale + 0.5f);
        f->line_gap = (int)(g * scale + 0.5f);
    }

    f->widest = 0;
    f->info   = info;               /* points into the asset, which is static */
    f->scale  = scale;

    for (ch = GLYPH_MIN; ch <= GLYPH_MAX; ch++) {
        struct glyph *gl = &f->glyphs[ch - GLYPH_MIN];
        int adv, lsb;

        stbtt_GetCodepointHMetrics(&info, ch, &adv, &lsb);
        gl->advance = (int)(adv * scale + 0.5f);

        if (gl->advance > f->widest) {
            f->widest = gl->advance;
        }

        gl->coverage = stbtt_GetCodepointBitmap(&info, scale, scale, ch,
                                                &gl->w, &gl->h,
                                                &gl->xoff, &gl->yoff);
    }

    {
        size_t i;

        for (i = 0; i + 1 < sizeof(f->name) && name[i] != '\0'; i++) {
            f->name[i] = name[i];
        }

        f->name[i] = '\0';
    }

    f->px = px;
    f->loaded = true;
    return true;
}

/* How wide a string is with the font in force. */
static long text_width(const struct outline_font *f, const char *str,
                       size_t len)
{
    size_t i;
    long w = 0;

    /*
     * The built-in 8x16 is a bitmap face and `stbtt` cannot load it, so this
     * is the path every default-font measurement takes. It counted *bytes*,
     * which made one two-byte character two columns wide and every wrapped
     * line short.
     */
    if (!f->loaded) {
        size_t at = 0;
        long n = 0;

        while (at < len) {
            (void)utf8_next(str, len, &at);
            n++;
        }

        return n * GLYPH_W;
    }

    for (i = 0; i < len; ) {
        w += glyph_for(f, utf8_next(str, len, &i))->advance;
    }

    return w;
}

static void draw_outline_text(struct surface *s, const struct outline_font *f,
                              long x, long y, const char *str, size_t len,
                              uint32_t fg, const uint32_t *bg)
{
    long pen = x;
    long baseline = y + f->ascent;
    size_t i;

    if (bg != NULL) {
        long w = text_width(f, str, len);
        long h = f->ascent + f->descent;

        if (w > 0) {
            long bx = x, by = y, bw = w, bh = h;

            if (clip(s, &bx, &by, &bw, &bh, NULL, NULL)) {
                long row;

                for (row = 0; row < bh; row++) {
                    uint32_t *p = row_of(s, (unsigned)(by + row)) + bx;
                    long n;

                    for (n = 0; n < bw; n++) {
                        p[n] = *bg;
                    }
                }
            }
        }
    }

    for (i = 0; i < len; ) {
        const struct glyph *gl = glyph_for(f, utf8_next(str, len, &i));
        int gy;

        for (gy = 0; gy < gl->h && gl->coverage != NULL; gy++) {
            long py = baseline + gl->yoff + gy;
            uint32_t *row;
            int gx;

            if (py < 0 || py >= (long)s->height) {
                continue;
            }

            row = row_of(s, (unsigned)py);

            for (gx = 0; gx < gl->w; gx++) {
                long px_ = pen + gl->xoff + gx;
                unsigned a = gl->coverage[gy * gl->w + gx];

                if (px_ < 0 || px_ >= (long)s->width || a == 0) {
                    continue;
                }

                /* Blended, which is the whole point: coverage is what an
                 * outline produces and a threshold would throw it away. */
                row[px_] = (a == 255) ? fg : mix(row[px_], fg, a);
            }
        }

        pen += gl->advance;
    }
}

/*
 * `gfx.use_font(name, px)` - the font this process draws with.
 *
 * "spleen" is the bitmap and needs no size. Anything else is an outline at
 * `px` pixels. Per process, because a font is a drawing state like a colour
 * and not a property of the machine - though in practice the window manager
 * draws almost all the text, so setting it there sets it for what you see.
 */
/* Publishes the metrics of whatever is in force into the `gfx.font` table
 * every caller already holds. */
static void publish_font(lua_State *L, const char *name)
{
    if (font_table_ref == LUA_NOREF) {
        return;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, font_table_ref);

    /* `w` is the widest advance rather than a promise every glyph is that
     * wide - true for these two monospaced faces, and still *honest* for a
     * proportional one, where it becomes an upper bound and `gfx.measure`
     * becomes the thing to ask. */
    lua_pushinteger(L, faces[ROLE_UI].loaded
                       ? faces[ROLE_UI].widest : GLYPH_W);
    lua_setfield(L, -2, "w");

    lua_pushinteger(L, faces[ROLE_UI].loaded
                       ? (faces[ROLE_UI].ascent + faces[ROLE_UI].descent)
                       : GLYPH_H);
    lua_setfield(L, -2, "h");

    lua_pushstring(L, name);
    lua_setfield(L, -2, "name");

    lua_pop(L, 1);
}

/*
 * face(name, px) -> a number usable anywhere a role name is, or nil.
 *
 * What layout asks for. A role is a *decision the desktop made* - this is
 * the interface font, that is the terminal's - and a page has no such
 * decisions to make: it has a heading at one size and a paragraph at
 * another, both on screen at once, both chosen by the document.
 *
 * Faces are shared rather than reloaded. Opening one rasterises 95 glyphs,
 * and a page asks for the same paragraph face on every block it lays out,
 * so the same name and size hands back the same face every time.
 */
static int l_face(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int px = (int)luaL_optinteger(L, 2, 16);
    int i;

    for (i = ROLE_COUNT; i < FACES_MAX; i++) {
        if (faces[i].loaded && faces[i].px == px
            && strcmp(faces[i].name, name) == 0) {
            lua_pushinteger(L, i);
            return 1;
        }
    }

    for (i = ROLE_COUNT; i < FACES_MAX; i++) {
        if (faces[i].loaded) {
            continue;
        }

        if (!outline_load(&faces[i], name, px)) {
            lua_pushnil(L);
            lua_pushliteral(L, "no such font");
            return 2;
        }

        lua_pushinteger(L, i);
        return 1;
    }

    /* Said rather than evicted: throwing out a face the next block wants
     * back would rasterise it again, and a caller that hears "no" can draw
     * with one it already holds. */
    lua_pushnil(L);
    lua_pushliteral(L, "no room for another face");
    return 2;
}

/*
 * `gfx.release_faces()` - every face asked for by size, given back.
 *
 * **The pool was fixed and nothing ever left it**, which was right while
 * the sizes a process asked for were the sizes its pages had. The window
 * manager asks on behalf of every window, at every size an application
 * names - and since 22 September at that size times a scale (`roadmap.md`
 * 5z), so each change of scale asks for a new set, and eight slots were
 * gone after a few. It keeps what it asked for in a cache it clears when
 * the faces change; this is the other half of clearing it, so the slots
 * are free again when the cache says they are.
 *
 * The roles are not touched: those are replaced in place by `use_font`.
 * An index into the sized pool handed out before this means nothing after
 * it, which is why only the caller that holds all of them may call it.
 */
static int l_release_faces(lua_State *L)
{
    int i;

    (void)L;

    for (i = ROLE_COUNT; i < FACES_MAX; i++) {
        if (faces[i].loaded) {
            outline_release(&faces[i]);
        }
    }

    return 0;
}

static int l_use_font(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int px = (int)luaL_optinteger(L, 2, 16);
    int role = role_of(L, 3);

    if (name[0] == 's') {                       /* spleen: the bitmap */
        outline_release(&faces[role]);

        if (role == ROLE_UI) {
            publish_font(L, "spleen");
        }

        lua_pushboolean(L, 1);
        return 1;
    }

    if (!outline_load(&faces[role], name, px)) {
        lua_pushnil(L);
        lua_pushstring(L, "no such font");
        return 2;
    }

    if (role == ROLE_UI) {
        publish_font(L, name);
    }

    lua_pushboolean(L, 1);
    return 1;
}

/*
 * How tall a line of a role's font is.
 *
 * `gfx.font.h` answers this for the interface font and only for that one,
 * which was enough while the title bar and the widgets shared a face. They
 * do not any more, and centring a title vertically with the *widget* font's
 * height puts it off centre by however far the two differ - which for a
 * 22-pixel display face against 16-pixel widgets is three pixels, and looks
 * exactly like a mistake because it is one.
 *
 * The companion to `gfx.measure`: that one is the width nobody may compute
 * themselves, this is the height.
 */
static int l_height(lua_State *L)
{
    const struct outline_font *f = &faces[role_of(L, 1)];

    lua_pushinteger(L, f->loaded ? (f->ascent + f->descent) : GLYPH_H);

    return 1;
}

/* How wide a string would be. What the fifty places computing
 * `#text * gfx.font.w` should ask instead, and what a proportional font
 * makes compulsory. */
static int l_measure(lua_State *L)
{
    size_t len;
    const char *str = luaL_checklstring(L, 1, &len);

    lua_pushinteger(L,
        (lua_Integer)text_width(&faces[role_of(L, 2)], str, len));
    return 1;
}

/* What is available to choose from. */
static int l_font_names(lua_State *L)
{
    unsigned i;

    lua_newtable(L);
    lua_pushstring(L, "spleen");
    lua_rawseti(L, -2, 1);

    for (i = 0; fonts_table[i].name != NULL; i++) {
        char shortname[32];

        font_short_name(fonts_table[i].name, shortname, sizeof(shortname));
        lua_pushstring(L, shortname);
        lua_rawseti(L, -2, (lua_Integer)(i + 2));
    }

    return 1;
}

/*
 * s:text(x, y, string, colour [, background])
 *
 * Returns the x the next character would start at, so a caller can lay out a
 * line without knowing the cell width - which is the only number about the
 * font that Lua should ever need, and it is better returned than published.
 *
 * Without a background only the set pixels are written, which is what
 * drawing over a picture wants. With one, the whole cell is written, which is
 * both what a terminal wants and considerably faster than filling first.
 */
static int l_text(lua_State *L)
{
    struct surface *s = check_surface(L, 1);
    long x = (long)luaL_checkinteger(L, 2);
    long y = (long)luaL_checkinteger(L, 3);
    size_t len;
    const char *text = luaL_checklstring(L, 4, &len);
    uint32_t fg = (uint32_t)luaL_checkinteger(L, 5);
    bool opaque = !lua_isnoneornil(L, 6);
    uint32_t bg = opaque ? (uint32_t)luaL_checkinteger(L, 6) : 0;
    size_t i;
    long col;

    /* An outline font, when one is in force. Same call, same arguments;
     * what changes is where the glyphs come from. */
    {
        const struct outline_font *f = &faces[role_of(L, 7)];

        if (f->loaded) {
            draw_outline_text(s, f, x, y, text, len, fg,
                              opaque ? &bg : NULL);
            lua_pushinteger(L, x + text_width(f, text, len));
            return 1;
        }
    }

    /*
     * `col` rather than `i`, because they stopped being the same number the
     * moment this decoded UTF-8: a character is one column wide and one to
     * four bytes long.
     */
    for (i = 0, col = 0; i < len; col++) {
        const unsigned char *rows = glyph_of(utf8_next(text, len, &i));
        long cx = x + col * GLYPH_W;
        long row;

        /* Whole glyphs off either edge are skipped rather than clipped, which
         * saves the inner loop on a long string that starts off-screen. The
         * partly-visible ones still go through the per-pixel bounds check
         * below. */
        if (cx + GLYPH_W <= 0 || cx >= (long)s->width) {
            continue;
        }

        for (row = 0; row < GLYPH_H; row++) {
            long py = y + row;
            unsigned bits = rows[row];
            uint32_t *p;
            long bit;

            if (py < 0 || py >= (long)s->height) {
                continue;
            }

            p = row_of(s, (unsigned)py);

            for (bit = 0; bit < GLYPH_W; bit++) {
                long px = cx + bit;

                if (px < 0 || px >= (long)s->width) {
                    continue;
                }

                if (bits & (0x80u >> bit)) {
                    p[px] = fg;
                } else if (opaque) {
                    p[px] = bg;
                }
            }
        }
    }

    lua_pushinteger(L, x + (lua_Integer)col * GLYPH_W);
    return 1;
}

static int l_get(lua_State *L)
{
    struct surface *s = check_surface(L, 1);
    lua_Integer x = luaL_checkinteger(L, 2);
    lua_Integer y = luaL_checkinteger(L, 3);

    /* Out of bounds is nil rather than an error or a clamped read. A single
     * pixel is allowed to cross into Lua (`gfx.md` §19.1) and a caller
     * sampling near an edge should not have to bounds-check first. */
    if (x < 0 || y < 0 || x >= (lua_Integer)s->width || y >= (lua_Integer)s->height) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushinteger(L, row_of(s, (unsigned)y)[x]);
    return 1;
}

static int l_set(lua_State *L)
{
    struct surface *s = check_surface(L, 1);
    lua_Integer x = luaL_checkinteger(L, 2);
    lua_Integer y = luaL_checkinteger(L, 3);
    uint32_t colour = (uint32_t)luaL_checkinteger(L, 4);

    if (x < 0 || y < 0 || x >= (lua_Integer)s->width || y >= (lua_Integer)s->height) {
        return 0;               /* silently clipped, like every other write */
    }

    row_of(s, (unsigned)y)[x] = colour;
    return 0;
}

/*
 * `dst:stretch(src, sx, sy, sw, sh, dx, dy, dw, dh [, alpha [, smooth
 * [, cx, cy, cw, ch]]])` - a rectangle of `src` drawn into a rectangle of
 * `dst` of another size, and only where it falls inside `cx, cy, cw, ch`
 * when that is given.
 *
 * **Nearest neighbour, for the reason `snes_blit.c` gives**: what this is for
 * is a cover or an icon, drawn when something changes rather than every
 * frame, and a smooth resampler is a different primitive with a different
 * cost. What is not acceptable is this loop written in Lua, which is what
 * `photo.lua` and the image widget each say they are waiting for - the
 * per-pixel loop `gfx.md` 19.2 forbids.
 *
 * **The step is fixed point, not a division per pixel**: 16.16 over the
 * source, so a 500-pixel cover into 78 costs an add per pixel.
 *
 * **Only the destination is clipped**, and that is the difference from
 * `blit`. `clip()` moves a paired source origin one for one, which is
 * exactly wrong here: a destination edge cut by a window's border maps back
 * to a fraction of a source pixel rather than to the same number of them. So
 * the destination is clipped against `dst` and each source position is
 * computed from where the pixel actually landed; a source position outside
 * `src` is clamped to its edge rather than refused, because a caller should
 * not have to measure before it can draw.
 *
 * `alpha` is `blend`'s: absent, the pixels are copied as `blit` copies them;
 * given, they are composited over what is there. One primitive rather than
 * two, because the difference is a branch and the pair would be two entries
 * in every list that names the primitives.
 *
 * **`smooth`, since 22 September: each pixel the average of what it
 * covers.** Nearest neighbour takes one source pixel and skips the rest, so
 * an icon drawn at three quarters of its size loses every fourth row of its
 * outline. With `smooth` each destination pixel is the area-weighted mean
 * of the source rectangle it covers, partial pixels at its edges counted by
 * how much of them it covers (`area_sample`). What asked for it is a
 * 32-pixel Deskbar holding 24-pixel icons drawn from 64-pixel ones
 * (`roadmap.md` 5v); what will ask next is everything, at a scale
 * (`roadmap.md` 5z).
 *
 * **Weighted by alpha when composited**: a transparent pixel beside an
 * opaque one says nothing about colour, so it adds to the coverage and not
 * to the colour - or the edge of every icon would come out darkened
 * towards the transparent black around it. Copied without `alpha`, the
 * source is taken as opaque, as `blit` takes it.
 *
 * Opt-in rather than the rule, because it reads every source pixel under a
 * destination pixel where nearest reads one: a 4000-pixel photograph fitted
 * into 800 is twenty-five times the reads, which is a cost for whoever
 * wants the quality to choose.
 *
 * **The clip rectangle, since 22 September** (`roadmap.md` 5z): a window
 * that draws its own pixels, at a scale, is composed stretched - and one
 * damaged piece of it at a time, since the compositor redraws what changed
 * and not whole windows. Each destination pixel's source is worked out from
 * where the *whole* rectangle lands, so the pieces meet exactly; the clip
 * only says which of those pixels to write. Without it the only clip was
 * `dst`'s own edges.
 */
/*
 * `dst:pixels(t, w, h [, scale])` - a Lua array of pixels onto a surface.
 *
 * **One native call for a whole frame**, which is the only shape that makes
 * sense for a renderer written in Lua: the alternative is a call per pixel,
 * and half a million of those is not a frame rate, it is an apology.
 *
 * `t` is `t[1 .. w*h]`, row-major from the top-left, each entry a number
 * `0xRRGGBB`. That is the shape the portable solar-system renderer
 * produces, and it is the shape any Lua rasterizer naturally produces, so
 * this is a general primitive rather than one program's favour.
 *
 * **Why a table at all**, when `gfx.md` 19.1 says pixels never go inside
 * one: because for a rasterizer written in portable Lua there is no
 * alternative that is not worse. A userdata with `__index`/`__newindex`
 * turns every pixel the *renderer* writes into a metamethod call, which is
 * several times slower on PUC Lua - it only pays with a JIT and FFI. So the
 * table is accepted at the boundary, deliberately, and the crossing happens
 * exactly once a frame, here, in C. Diego agreed to the exception on 21
 * September for the solar system port.
 *
 * `scale` is a whole-number nearest-neighbour enlargement, so a small
 * render can fill a larger window without a second pass over the pixels.
 *
 * **Byte order.** The word is written whole, `0xFF000000 | rgb`, which is
 * what every other primitive in this file does and what XRGB8888 wants on
 * the two architectures Kosmos runs on. A big-endian target would have to
 * change this function *and* every other one here; writing channels one at
 * a time in this one place would cost speed now and still not make the
 * file portable, so it is one convention rather than one exception.
 */
static int l_pixels(lua_State *L)
{
    struct surface *s = check_surface(L, 1);
    long w = (long)luaL_checkinteger(L, 3);
    long h = (long)luaL_checkinteger(L, 4);
    long scale = (long)luaL_optinteger(L, 5, 1);
    long y;

    luaL_checktype(L, 2, LUA_TTABLE);

    if (w <= 0 || h <= 0) {
        return 0;
    }

    if (scale < 1) {
        scale = 1;
    }

    /*
     * Clipped against the surface rather than trusted, because the caller
     * is Lua and the cost of being wrong here is somebody else's memory.
     * A render larger than the window draws the part that fits.
     */
    for (y = 0; y < h; y++) {
        long copies;

        for (copies = 0; copies < scale; copies++) {
            long dy = y * scale + copies;
            uint32_t *row;
            long x;

            if (dy >= (long)s->height) {
                return 0;
            }

            row = row_of(s, (unsigned)dy);

            for (x = 0; x < w; x++) {
                long dx = x * scale;
                uint32_t rgb;
                long lane;

                if (dx >= (long)s->width) {
                    break;
                }

                /*
                 * `lua_rawgeti` rather than `lua_geti`: the table is an
                 * array of numbers and a metamethod on it would mean the
                 * caller had handed us something other than what this
                 * function documents.
                 */
                lua_rawgeti(L, 2, (lua_Integer)(y * w + x + 1));
                rgb = (uint32_t)lua_tointeger(L, -1);
                lua_pop(L, 1);

                rgb |= 0xff000000u;

                for (lane = 0; lane < scale; lane++) {
                    if (dx + lane >= (long)s->width) {
                        break;
                    }

                    row[dx + lane] = rgb;
                }
            }
        }
    }

    return 0;
}

/*
 * The mean of the source under one destination pixel: the rectangle from
 * `fx0` to `fx1` and `fy0` to `fy1`, in 16.16 source pixels from `(sx,
 * sy)`, each source pixel weighted by how much of it the rectangle covers.
 * Positions past the source's edges are clamped to it, as the nearest path
 * clamps them. With `weigh_alpha`, colour is weighted by alpha as well and
 * the result's alpha is the mean alpha; without it, alpha is ignored and
 * the result is opaque.
 */
static uint32_t area_sample(const struct surface *s, long sx, long sy,
                            uint64_t fx0, uint64_t fx1,
                            uint64_t fy0, uint64_t fy1, bool weigh_alpha)
{
    uint64_t total = 0, a = 0, r = 0, g = 0, b = 0;
    uint64_t y;

    for (y = fy0 >> 16; (y << 16) < fy1; y++) {
        uint64_t top = y << 16, bottom = top + 65536u;
        uint64_t hy = (bottom < fy1 ? bottom : fy1) - (top > fy0 ? top : fy0);
        long row = sy + (long)y;
        const uint32_t *sp;
        uint64_t x;

        if (row > (long)s->height - 1) row = (long)s->height - 1;
        if (row < 0) row = 0;

        sp = row_of(s, (unsigned)row);

        for (x = fx0 >> 16; (x << 16) < fx1; x++) {
            uint64_t left = x << 16, right = left + 65536u;
            uint64_t wx = (right < fx1 ? right : fx1) - (left > fx0 ? left : fx0);
            uint64_t w = (wx * hy) >> 16;
            long col = sx + (long)x;
            uint32_t p;
            uint64_t k;

            if (col > (long)s->width - 1) col = (long)s->width - 1;
            if (col < 0) col = 0;

            p = sp[col];
            k = weigh_alpha ? w * (p >> 24) : w * 255u;

            total += w;
            a += k;
            r += k * ((p >> 16) & 0xffu);
            g += k * ((p >> 8) & 0xffu);
            b += k * (p & 0xffu);
        }
    }

    if (total == 0 || a == 0) {
        return 0;
    }

    return (uint32_t)((a + total / 2) / total) << 24
         | (uint32_t)((r + a / 2) / a) << 16
         | (uint32_t)((g + a / 2) / a) << 8
         | (uint32_t)((b + a / 2) / a);
}

static int l_stretch(lua_State *L)
{
    struct surface *d = check_surface(L, 1);
    struct surface *s = check_surface(L, 2);
    long sx = (long)luaL_checkinteger(L, 3);
    long sy = (long)luaL_checkinteger(L, 4);
    long sw = (long)luaL_checkinteger(L, 5);
    long sh = (long)luaL_checkinteger(L, 6);
    long dx = (long)luaL_checkinteger(L, 7);
    long dy = (long)luaL_checkinteger(L, 8);
    long dw = (long)luaL_checkinteger(L, 9);
    long dh = (long)luaL_checkinteger(L, 10);
    long global = (long)luaL_optinteger(L, 11, -1);
    bool smooth = lua_toboolean(L, 12);
    bool clipped = !lua_isnoneornil(L, 13);
    long x0, y0, x1, y1, y;
    uint32_t xstep, ystep;

    if (global > 255 || (global < 0 && !lua_isnoneornil(L, 11))) {
        return luaL_error(L, "alpha is 0 to 255, not %d", (int)global);
    }

    /* Nothing to draw rather than something to complain about, which is the
     * answer `clip()` gives for a rectangle that is entirely off-screen. */
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        return 0;
    }

    x0 = dx < 0 ? 0 : dx;
    y0 = dy < 0 ? 0 : dy;
    x1 = dx + dw > (long)d->width ? (long)d->width : dx + dw;
    y1 = dy + dh > (long)d->height ? (long)d->height : dy + dh;

    if (clipped) {
        long cx = (long)luaL_checkinteger(L, 13);
        long cy = (long)luaL_checkinteger(L, 14);
        long cw = (long)luaL_checkinteger(L, 15);
        long ch = (long)luaL_checkinteger(L, 16);

        if (cx > x0) x0 = cx;
        if (cy > y0) y0 = cy;
        if (cx + cw < x1) x1 = cx + cw;
        if (cy + ch < y1) y1 = cy + ch;
    }

    if (x0 >= x1 || y0 >= y1) {
        return 0;
    }

    xstep = (uint32_t)((sw << 16) / dw);
    ystep = (uint32_t)((sh << 16) / dh);

    if (smooth) {
        for (y = y0; y < y1; y++) {
            uint32_t *dp = row_of(d, (unsigned)y) + x0;
            uint64_t fy0 = (uint64_t)(y - dy) * ystep;
            long x;

            for (x = x0; x < x1; x++) {
                uint64_t fx0 = (uint64_t)(x - dx) * xstep;
                uint32_t p = area_sample(s, sx, sy, fx0, fx0 + xstep,
                                         fy0, fy0 + ystep, global >= 0);

                *dp = global < 0 ? p : over(p, *dp, (uint32_t)global);
                dp++;
            }
        }

        return 0;
    }

    for (y = y0; y < y1; y++) {
        uint32_t *dp = row_of(d, (unsigned)y) + x0;
        long from = sy + (long)(((uint32_t)(y - dy) * ystep) >> 16);
        const uint32_t *sp;
        uint32_t at = (uint32_t)(x0 - dx) * xstep;
        long x;

        if (from < 0) {
            from = 0;
        }

        if (from > (long)s->height - 1) {
            from = (long)s->height - 1;
        }

        sp = row_of(s, (unsigned)from);

        for (x = x0; x < x1; x++, at += xstep) {
            long take = sx + (long)(at >> 16);

            if (take < 0) {
                take = 0;
            }

            if (take > (long)s->width - 1) {
                take = (long)s->width - 1;
            }

            if (global < 0) {
                *dp = sp[take];
            } else {
                *dp = over(sp[take], *dp, (uint32_t)global);
            }

            dp++;
        }
    }

    return 0;
}

/*
 * `surface:camera(at, mirror, last)` - the newest whole frame from a camera's
 * region into this surface (`cameraproto.h`, `roadmap.md` 6d).
 *
 * `at` is where the program mapped the region it handed the driver;
 * `mirror` flips the picture left for right; `last` is the sequence the
 * program last drew, so an unchanged picture costs nothing. The answer is
 * the frame's sequence when one was drawn, or false and why not: "same",
 * "waiting" before the first frame, "stopped" once the driver has closed the
 * stream, or "mjpeg" for a format this does not convert yet.
 *
 * **The handshake is here, in C, and not in Lua**, because it is three
 * stores and a fence in an order that matters (`cameraproto.h`): say which
 * slot is being read, make sure it is still the newest, convert it, let it
 * go. A Lua program cannot fence, and should not have to know it needs to.
 * `taken` moves forward with every frame, which is what keeps the stream
 * open: a program that stops calling this loses the camera three seconds
 * later.
 */
static int l_camera(lua_State *L)
{
    struct surface *dst = check_surface(L, 1);
    lua_Integer at = luaL_checkinteger(L, 2);
    int mirror = lua_toboolean(L, 3);
    uint32_t last = (uint32_t)luaL_optinteger(L, 4, 0);
    volatile struct camera_ring *r = (volatile struct camera_ring *)(uintptr_t)at;
    uint32_t sequence, slot = CAMERA_NOT_READING, width, height, length;
    unsigned tries;

    if (at == 0 || r->magic != CAMERA_RING_MAGIC) {
        return luaL_error(L, "that is not a camera's region");
    }

    r->looked = r->looked + 1u;         /* the lease: still here */

    if (r->stopped) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "stopped");
        return 2;
    }

    sequence = r->sequence;
    CAMERA_FENCE();

    if (sequence == 0 || sequence == last) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, sequence == 0 ? "waiting" : "same");
        return 2;
    }

    /* Mark a slot, then make sure it is still the newest: the driver
     * never writes into the one marked, but it may have moved on first. */
    for (tries = 0; tries < 8; tries++) {
        slot = r->latest;
        r->reading = slot;
        CAMERA_FENCE();

        if (r->latest == slot) {
            break;
        }
    }

    sequence = r->sequence;
    width = r->width;
    height = r->height;

    if (slot >= CAMERA_SLOTS || slot >= r->slots) {
        r->reading = CAMERA_NOT_READING;
        lua_pushboolean(L, 0);
        lua_pushstring(L, "waiting");
        return 2;
    }

    length = r->length[slot];

    if (r->pixels != CAMERA_PIXELS_YUY2) {
        CAMERA_FENCE();
        r->reading = CAMERA_NOT_READING;
        r->taken = sequence;
        lua_pushboolean(L, 0);
        lua_pushstring(L, "mjpeg");
        return 2;
    }

    if (length == width * height * 2u && dst->width >= width
        && dst->height >= height && r->slot_bytes >= length) {
        const uint8_t *src = (const uint8_t *)(uintptr_t)at + CAMERA_RING_DATA
                             + (uintptr_t)slot * r->slot_bytes;

        gfx_yuy2(dst->pixels, dst->pitch, src, width, height, mirror != 0);
    }

    /* Done with it: let the driver have the slot back, and renew the lease. */
    CAMERA_FENCE();
    r->reading = CAMERA_NOT_READING;
    r->taken = sequence;

    lua_pushinteger(L, (lua_Integer)sequence);
    return 1;
}

static const luaL_Reg surface_methods[] = {
    { "size",   l_size },
    { "pitch",  l_pitch },
    { "fill",   l_fill },
    { "span",   l_span },
    { "triangle", l_triangle },
    { "disc",   l_disc },
    { "blit",   l_blit },
    { "blit_round", l_blit_round },
    { "shadow", l_shadow },
    { "camera", l_camera },
    { "fill_round",  l_fill_round },
    { "frame_round", l_frame_round },
    { "blend",  l_blend },
    { "tint",   l_tint },
    { "stretch", l_stretch },
    { "pixels",  l_pixels },
    { "text",   l_text },
    { "get",    l_get },
    { "set",    l_set },
    { "free",   l_free },
    { NULL, NULL }
};

/*
 * The screen, as a surface.
 *
 * Not owned and never freed: these are the board's pages, mapped into this
 * process because it was handed the device. `free` is a no-op on it and the
 * finalizer leaves it alone, because releasing them would mean unmapping the
 * display out from under whatever draws next.
 *
 * It is the one surface that does not have the canonical pitch. The board
 * chose 4160 bytes for a 1024-pixel row, and that number arrives here and is
 * used; nothing recomputes it. `gfx.md` §19.3's rule that only the app
 * server's backbuffer knows the device's real format is exactly this - and
 * the conversion it talks about is the identity today, because XRGB8888 and
 * the canonical 0xAARRGGBB have the same bytes and the display ignores the
 * top one. When a board arrives whose format differs, this is the function
 * that grows a conversion and nothing above it changes.
 *
 * nil rather than an error when this process does not hold the screen: a
 * program asking whether it has one is asking a reasonable question, and
 * every process but one gets no.
 */
/*
 * `gfx.wrap{ at = address, w = , h = }` - a surface over memory somebody
 * else also has.
 *
 * The other half of `sys.memory`. The kernel hands back an address; this
 * turns it into a surface, and from then on it behaves exactly like any
 * other - the same `fill`, `blit`, `text` and the same pitch arithmetic, all
 * of it in C, with no line of Lua computing a pixel offset.
 *
 * `owned` is false: these pages belong to the region, not to this surface,
 * and the region belongs to whoever holds capabilities to it. Freeing the
 * surface must not free them, or one process could pull the memory out from
 * under another - which is the whole hazard of sharing and the reason this
 * flag already existed for the screen.
 *
 * The address is not checked, and cannot usefully be: it is a number from
 * the kernel, and a program that passes a different one has written past its
 * own mapping, which is a fault it takes itself. That is the same bargain
 * `gfx.screen` makes.
 */
static int l_wrap(lua_State *L)
{
    lua_Integer at, width, height;
    struct surface *s;
    unsigned pitch;

    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "at");
    at = luaL_checkinteger(L, -1);
    lua_getfield(L, 1, "w");
    width = luaL_checkinteger(L, -1);
    lua_getfield(L, 1, "h");
    height = luaL_checkinteger(L, -1);
    lua_pop(L, 3);

    if (at == 0 || width <= 0 || height <= 0) {
        return luaL_error(L, "gfx.wrap needs an address and a size");
    }

    if (width > 16384 || height > 16384) {
        return luaL_error(L, "that surface is larger than this system allows");
    }

    /*
     * The same padded pitch a created surface gets, so that a wrapped one
     * and an allocated one are the same shape and a caller cannot tell them
     * apart - which is what lets the compositor treat both alike.
     */
    pitch = (unsigned)(((width * 4) + (ROW_ALIGN - 1)) & ~(long)(ROW_ALIGN - 1));

    s = lua_newuserdatauv(L, sizeof(*s), 0);
    s->pixels = (uint32_t *)(uintptr_t)at;
    s->width  = (unsigned)width;
    s->height = (unsigned)height;
    s->pitch  = pitch;
    s->bytes  = (size_t)pitch * (size_t)height;
    s->pages  = 0;
    s->owned  = false;          /* the region's, not ours */

    luaL_setmetatable(L, SURFACE_MT);
    return 1;
}

/*
 * How many bytes a surface of this size needs, so a caller can ask the
 * kernel for the right number of pages.
 *
 * Here rather than in Lua because it is pitch arithmetic, and `gfx.md` 19.3
 * is that no line of Lua computes a pixel offset - including the one that
 * works out how many there are.
 */
static int l_surface_bytes(lua_State *L)
{
    lua_Integer width = luaL_checkinteger(L, 1);
    lua_Integer height = luaL_checkinteger(L, 2);
    unsigned pitch;

    if (width <= 0 || height <= 0) {
        return luaL_error(L, "a surface needs a positive width and height");
    }

    pitch = (unsigned)(((width * 4) + (ROW_ALIGN - 1)) & ~(long)(ROW_ALIGN - 1));

    lua_pushinteger(L, (lua_Integer)((size_t)pitch * (size_t)height));
    return 1;
}

static int l_screen(lua_State *L)
{
    struct screen_info info;
    struct surface *s;

    if (kosmos_screen(&info) < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "this process was not given the screen");
        return 2;
    }

    s = lua_newuserdatauv(L, sizeof(*s), 0);
    s->pixels = (uint32_t *)(uintptr_t)info.address;
    s->width  = info.width;
    s->height = info.height;
    s->pitch  = info.pitch;
    s->bytes  = 0;
    s->pages  = 0;
    s->owned  = false;

    luaL_setmetatable(L, SURFACE_MT);
    return 1;
}

/*
 * ---------------------------------------------------------------------
 * What another kit borrows to draw with. See `gfx_draw.h`.
 * ---------------------------------------------------------------------
 *
 * A page is thousands of boxes and glyphs, and the window manager's draw
 * ops carry one operation each. So the web kit paints its own surface in C
 * and calls these, exactly as `docfont.c` paints a page of a PDF - the
 * difference being that these reuse the face pool and the glyph cache above
 * rather than rasterising a second time.
 */

void gfx_draw_fill(struct surface *s, long x, long y, long w, long h,
                   uint32_t colour)
{
    long row;

    if (s == NULL || s->pixels == NULL) {
        return;
    }

    if (!clip(s, &x, &y, &w, &h, NULL, NULL)) {
        return;
    }

    for (row = 0; row < h; row++) {
        uint32_t *p = row_of(s, (unsigned)(y + row)) + x;
        long n;

        for (n = 0; n < w; n++) {
            p[n] = colour;
        }
    }
}

/* The face a number names, or the interface font when it names nothing
 * this process has open. */
static const struct outline_font *face_at(int face)
{
    if (face < 0 || face >= FACES_MAX) {
        face = ROLE_UI;
    }

    return &faces[face];
}

/*
 * **An outline face only.**
 *
 * The built-in 8x16 is a bitmap and its drawing lives inside `l_text`,
 * where it is entangled with the Lua stack. A caller here has asked for a
 * face by name and size and can be told when there was not one; drawing
 * nothing is the honest answer rather than silently substituting a face of
 * a different size, which would make every line of a laid-out page the
 * wrong height.
 */
void gfx_draw_text(struct surface *s, int face, long x, long y,
                   const char *str, size_t len,
                   uint32_t fg, const uint32_t *bg)
{
    const struct outline_font *f = face_at(face);

    if (s == NULL || s->pixels == NULL || str == NULL || !f->loaded) {
        return;
    }

    draw_outline_text(s, f, x, y, str, len, fg, bg);
}

long gfx_draw_measure(int face, const char *str, size_t len)
{
    return text_width(face_at(face), str, len);
}

int gfx_draw_height(int face)
{
    const struct outline_font *f = face_at(face);

    return f->loaded ? (f->ascent + f->descent) : GLYPH_H;
}

/*
 * Where the baseline sits below the top of a line.
 *
 * Drawing does not need this - `draw_outline_text` adds the ascent itself,
 * because it is the only place that knows it. Laying *two faces on one
 * line* does: they share a baseline and not a top edge, so the caller has
 * to work out each one's top from the tallest ascent on the line.
 */
int gfx_draw_ascent(int face)
{
    const struct outline_font *f = face_at(face);

    return f->loaded ? f->ascent : GLYPH_H;
}

static const luaL_Reg gfx_functions[] = {
    { "use_font", l_use_font },
    { "face",     l_face },
    { "release_faces", l_release_faces },
    { "measure",  l_measure },
    { "height",   l_height },
    { "fonts",    l_font_names },
    { "surface", l_new },
    { "wrap",    l_wrap },
    { "bytes",   l_surface_bytes },
    { "screen",  l_screen },
    { NULL, NULL }
};

int luaopen_gfx(lua_State *L)
{
    /* The font is checked once, here, rather than trusted. It is generated,
     * so a mismatch means the converter and this file disagree about the
     * layout - and that produces a plausible wrong picture rather than a
     * failure. */
    if (font_8x16_len
        != (GLYPH_LAST - GLYPH_FIRST + 2 + font_8x16_extra_len) * GLYPH_H) {
        return luaL_error(L, "the font is %d bytes and this expects %d",
                          (int)font_8x16_len,
                          (int)((GLYPH_LAST - GLYPH_FIRST + 2
                                 + font_8x16_extra_len) * GLYPH_H));
    }

    luaL_newmetatable(L, SURFACE_MT);

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");     /* methods are found on the metatable */

    lua_pushcfunction(L, l_gc);
    lua_setfield(L, -2, "__gc");

    luaL_setfuncs(L, surface_methods, 0);
    lua_pop(L, 1);

    luaL_newlib(L, gfx_functions);

    /*
     * The cell size, because layout needs it and computing it from a string
     * width would be Lua doing arithmetic about pixels. A table rather than
     * two functions: it is a property of the font, not a question to ask.
     */
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, GLYPH_W);
    lua_setfield(L, -2, "w");
    lua_pushinteger(L, GLYPH_H);
    lua_setfield(L, -2, "h");
    lua_pushstring(L, "spleen");
    lua_setfield(L, -2, "name");

    /*
     * Kept in the registry so that `use_font` can refresh it *in place*.
     *
     * The same reasoning as the palette in `theme.lua`: every caller reads
     * `gfx.font.w` when it draws, so changing the fields of the one table
     * changes what they all see. Replacing the table would leave every
     * existing reference pointing at the old one.
     */
    lua_pushvalue(L, -1);
    font_table_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_setfield(L, -2, "font");

    /* `gfx.png`, which lives in its own file because a decoder and a
     * blitter have nothing to say to each other. */
    kosmos_png_open(L);
    kosmos_jpeg_open(L);

    /* Doom, Quake and the Super Nintendo were opened here too, as globals,
     * and hid their own programs from the prompt. They are kits now, in
     * `sys_user.c`'s list: `use("/kits/doom")`. */
    kosmos_docfont_open(L);

    return 1;
}
