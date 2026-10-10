/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Graphics Kit's primitives - fill, copy, blend, stretch, rounded
 * shapes and a triangle - as C functions on surfaces, and nothing of Lua
 * (Astra's D3b-1, `docs/astra-display.md`). They lived inside `gfx.c`'s Lua
 * methods; the methods read their arguments and call these now, and so do
 * the scene and the window server. Moved, not rewritten: each is the code
 * it was, and `test_draw` and the gate hold the pixels to what they were.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "surface.h"
#include "draw_primitives.h"
#include "rows.h"
#include "shadow.h"

/*
 * One row of source-over. `memcpy` into and out of the vectors rather than a
 * cast, because a surface's rows are only guaranteed 4-byte aligned - the
 * pitch is deliberately not width * 4 - and a misaligned vector load is a
 * fault on some machines and merely slow on others.
 */
void blend_row(uint32_t *dp, const uint32_t *sp, long w,
                      uint32_t global)
{
    u32x4 gv = { global, global, global, global };
    long i = 0;

    for (; i + 4 <= w; i += 4) {
        u32x4 s, d;

        __builtin_memcpy(&s, sp + i, sizeof s);

        /* The two cases `over` short-circuits, asked of four pixels. Only
         * when the global alpha is opaque, because otherwise the source's
         * own alpha is not the whole answer. */
        if (global == 255u) {
            u32x4 a = s >> 24;

            if (a[0] == 255u && a[1] == 255u && a[2] == 255u && a[3] == 255u) {
                u32x4 opaque = s | 0xff000000u;

                __builtin_memcpy(dp + i, &opaque, sizeof opaque);
                continue;
            }

            if (a[0] == 0 && a[1] == 0 && a[2] == 0 && a[3] == 0) {
                continue;               /* nothing of the source shows */
            }
        }

        __builtin_memcpy(&d, dp + i, sizeof d);

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
                __builtin_memcpy(dp + i, &d, sizeof d);
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

/*
 * **Each primitive is a C function on surfaces**, and the Lua method only
 * reads its arguments and calls it - so the scene (`scene.c`, Astra's D3a)
 * draws with the same code a window manager in Lua did, not a second copy.
 */
void fill_on(struct surface *s, long x, long y, long w, long h,
                    uint32_t colour)
{
    long row;

    if (!clip(s, &x, &y, &w, &h, NULL, NULL)) {
        return;
    }

    for (row = 0; row < h; row++) {
        gfx_fill_row(row_of(s, (unsigned)(y + row)) + x, w, colour, true);
    }
}

/*
 * A wash: `colour` laid over the rectangle by its own alpha - a window
 * dimmed behind a sheet (`ui.sheet`). The row is `rows.c`'s, in the lanes.
 */
void wash_on(struct surface *s, long x, long y, long w, long h,
             uint32_t colour)
{
    long row;

    if (!clip(s, &x, &y, &w, &h, NULL, NULL)) {
        return;
    }

    for (row = 0; row < h; row++) {
        gfx_wash_row(row_of(s, (unsigned)(y + row)) + x, w, colour, true);
    }
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

void triangle_on(struct surface *s, double vx[3], double vy[3],
                        uint32_t colour)
{
    unsigned a = 0, b = 1, c = 2, t;
    long y, y_first, y_last;

    /* Sorted by y, as indices, so the coordinates are not copied around. */
    if (vy[a] > vy[b]) { t = a; a = b; b = t; }
    if (vy[b] > vy[c]) { t = b; b = c; c = t; }
    if (vy[a] > vy[b]) { t = a; a = b; b = t; }

    if (vy[c] - vy[a] <= 0.0) {
        return;                 /* zero height: nothing to fill */
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
}

/*
 * dst:blit(src, sx, sy, w, h, dx, dy)
 *
 * The argument order is `gfx.md` §19.2's, and it reads as "take this
 * rectangle of src and put it there": the destination is the receiver, so
 * clipping is against the receiver, which is the surface that can actually
 * be overrun.
 */
void blit_on(struct surface *dst, struct surface *src,
                    long sx, long sy, long w, long h, long dx, long dy)
{
    long row;

    /* Clipped against the source first, so a rectangle that runs off the
     * edge of what is being copied from cannot read past it. The destination
     * origin moves with it. */
    if (!clip(src, &sx, &sy, &w, &h, &dx, &dy)) {
        return;
    }

    if (!clip(dst, &dx, &dy, &w, &h, &sx, &sy)) {
        return;
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
}

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
void blit_round_on(struct surface *dst, struct surface *src,
                          long sx, long sy, long w, long h, long dx, long dy,
                          long rx, long ry, long rw, long rh, long r, int outside)
{
    long row;

    if (!clip(src, &sx, &sy, &w, &h, &dx, &dy)) {
        return;
    }

    if (!clip(dst, &dx, &dy, &w, &h, &sx, &sy)) {
        return;
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

}

void fill_round_on(struct surface *dst, long x, long y, long w, long h,
                          uint32_t colour, long r)
{
    long row;

    if (w <= 0 || h <= 0) {
        return;
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
void frame_round_on(struct surface *dst, long x, long y, long w, long h,
                           uint32_t colour, long r)
{
    long row;

    if (w <= 2 || h <= 2) {
        return;
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
}

void blend_on(struct surface *dst, struct surface *src, long sx, long sy,
                     long w, long h, long dx, long dy, uint32_t global)
{
    long row;

    if (!clip(src, &sx, &sy, &w, &h, &dx, &dy)) {
        return;
    }

    if (!clip(dst, &dx, &dy, &w, &h, &sx, &sy)) {
        return;
    }

    for (row = 0; row < h; row++) {
        const uint32_t *sp = row_of(src, (unsigned)(sy + row)) + sx;
        uint32_t *dp = row_of(dst, (unsigned)(dy + row)) + dx;
        blend_row(dp, sp, w, global);
    }
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

/*
 * The scaler behind `stretch`, apart from its Lua arguments, so that another
 * kit can scale a picture into a surface of its own without a crossing -
 * NetSurf's layout drawing a page's pictures at their boxes' sizes
 * (`gfx_draw_stretch`, `roadmap.md` 6zz j4). `global` below 0 copies, 0 to
 * 255 blends at that alpha; the clip is [cx0, cx1) by [cy0, cy1).
 */
void stretch_into(struct surface *d, const struct surface *s,
                         long sx, long sy, long sw, long sh,
                         long dx, long dy, long dw, long dh,
                         long global, bool smooth,
                         long cx0, long cy0, long cx1, long cy1)
{
    long x0, y0, x1, y1, y;
    uint32_t xstep, ystep;

    /* Nothing to draw rather than something to complain about, which is the
     * answer `clip()` gives for a rectangle that is entirely off-screen. */
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        return;
    }

    x0 = dx < 0 ? 0 : dx;
    y0 = dy < 0 ? 0 : dy;
    x1 = dx + dw > (long)d->width ? (long)d->width : dx + dw;
    y1 = dy + dh > (long)d->height ? (long)d->height : dy + dh;

    if (cx0 > x0) x0 = cx0;
    if (cy0 > y0) y0 = cy0;
    if (cx1 < x1) x1 = cx1;
    if (cy1 < y1) y1 = cy1;

    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    /*
     * **At its own size**, which is how a page's pictures nearly always come
     * - a thumbnail is fetched at the size it is shown, an SVG drawn at its
     * box's (`roadmap.md` 6zz h): a row of source-over, four pixels at a
     * time (`blend_row`, which is `over` to the bit), or a row copied. Only
     * where nothing would be clamped, so the answer is the loop's below.
     */
    if (!smooth && sw == dw && sh == dh && sx >= 0 && sy >= 0
        && sx + sw <= (long)s->width && sy + sh <= (long)s->height) {
        for (y = y0; y < y1; y++) {
            const uint32_t *sp = row_of(s, (unsigned)(sy + (y - dy)))
                                 + sx + (x0 - dx);
            uint32_t *dp = row_of(d, (unsigned)y) + x0;

            if (global < 0) {
                memcpy(dp, sp, (size_t)(x1 - x0) * 4);
            } else {
                blend_row(dp, sp, x1 - x0, (uint32_t)global);
            }
        }

        return;
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

        return;
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

    return;
}
