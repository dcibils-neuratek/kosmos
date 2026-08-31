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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kosmos.h"

void kosmos_png_open(lua_State *L);

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

    return 0xff000000u
        | (mul255((src >> 16) & 0xffu, a) + mul255((dst >> 16) & 0xffu, inv)) << 16
        | (mul255((src >>  8) & 0xffu, a) + mul255((dst >>  8) & 0xffu, inv)) <<  8
        | (mul255((src      ) & 0xffu, a) + mul255((dst      ) & 0xffu, inv));
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
        long i;

        for (i = 0; i < w; i++) {
            dp[i] = over(sp[i], dp[i], (uint32_t)global);
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

#define GLYPH_W     8
#define GLYPH_H     16
#define GLYPH_FIRST 0x20
#define GLYPH_LAST  0x7e

/* The rows of one character, or the box the font uses for anything it does
 * not have. That glyph sits immediately after the range, which is what makes
 * "outside the range" a bounds check and not a special case. */
static const unsigned char *glyph_of(unsigned char c)
{
    unsigned index = (c >= GLYPH_FIRST && c <= GLYPH_LAST)
                   ? (unsigned)(c - GLYPH_FIRST)
                   : (unsigned)(GLYPH_LAST - GLYPH_FIRST + 1);

    return font_8x16 + (size_t)index * GLYPH_H;
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

    for (i = 0; i < len; i++) {
        const unsigned char *rows = glyph_of((unsigned char)text[i]);
        long cx = x + (long)i * GLYPH_W;
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

    lua_pushinteger(L, x + (lua_Integer)len * GLYPH_W);
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

static const luaL_Reg surface_methods[] = {
    { "size",   l_size },
    { "pitch",  l_pitch },
    { "fill",   l_fill },
    { "span",   l_span },
    { "triangle", l_triangle },
    { "blit",   l_blit },
    { "blend",  l_blend },
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

static const luaL_Reg gfx_functions[] = {
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
    if (font_8x16_len != (GLYPH_LAST - GLYPH_FIRST + 2) * GLYPH_H) {
        return luaL_error(L, "the font is %d bytes and this expects %d",
                          (int)font_8x16_len,
                          (int)((GLYPH_LAST - GLYPH_FIRST + 2) * GLYPH_H));
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
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, GLYPH_W);
    lua_setfield(L, -2, "w");
    lua_pushinteger(L, GLYPH_H);
    lua_setfield(L, -2, "h");
    lua_setfield(L, -2, "font");

    /* `gfx.png`, which lives in its own file because a decoder and a
     * blitter have nothing to say to each other. */
    kosmos_png_open(L);

    return 1;
}
