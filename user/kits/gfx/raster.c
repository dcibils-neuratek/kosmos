/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * gfx's rasteriser (`raster.h`): the browser's SVG's since `roadmap.md`
 * 6zz j5, and gfx's paths' since `docs/maps.md` M1.
 *
 * **It accumulates area**, the way Raph Levien's font-rs does: every edge
 * adds, to each pixel it crosses, the signed area of the pixel that lies to
 * its right, and a running sum along each row is then the coverage of each
 * pixel - exact for straight edges, anti-aliased for free, and with no
 * sorting of edges or spans. The sum is a winding number, so a pixel inside
 * twice is still inside once: what SVG calls `nonzero`, its default.
 *
 * **Only what the edges touched is visited.** Each row remembers the span
 * its edges reached, and painting runs the sum over that and nothing else:
 * past an edge's last contribution the sum is back to nothing, because a
 * shape is closed and an edge off either side of the picture is drawn at
 * its border. A logo of two hundred shapes costs the shapes' areas rather
 * than two hundred times the logo's.
 *
 * **Laying coverage over pixels is four at a time**, in GCC's own vector
 * types as `gfx.c`'s blend and `pack.c` are written - NEON on AArch64, SSE2
 * on x86-64, the compiler choosing the instructions - with the scalar
 * function kept as the tail and as the reference. Diego, 29 September 2026:
 * "Make sure we use simd vector arithmetic when possible in all you code".
 * Both do the same float operations in the same order, so they agree to
 * the bit, which `tools/test_raster.c` holds them to.
 *
 * **The running sum is four at a time as well**, and it was most of the
 * time: each pixel's sum waits for the one before it, about four cycles a
 * pixel however wide the rest is, and four lanes laying colour behind it
 * measured no faster than one. So a group of four is summed inside the
 * register - shifted by one and added, by two and added, the group before's
 * total added to all four - and only the totals wait on each other. That
 * adds the same numbers in another order, and float addition cares about
 * order, so the scalar reference adds them in that order too, group by
 * group from the span's start; what is left over at the end is summed one
 * at a time by both.
 *
 * Straight alpha needs a division for every pixel - what was there keeps
 * its share of a smaller whole - and a vector unit has a float divide where
 * it has no integer one, which is why this is float throughout. One
 * reciprocal a pixel and three multiplies, rather than three divides: a
 * divide is the slowest thing either unit does, and the lanes' is no
 * cheaper a lane than the scalar one, so three of them were the time.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "raster.h"

bool raster_open(struct raster *r, int w, int h)
{
    int y;

    memset(r, 0, sizeof(*r));

    if (w < 1 || h < 1) {
        return false;
    }

    r->w = w;
    r->h = h;
    r->acc = calloc((size_t)(w + 2) * (size_t)h, sizeof(float));
    r->from = malloc((size_t)h * sizeof(int));
    r->to = malloc((size_t)h * sizeof(int));

    if (r->acc == NULL || r->from == NULL || r->to == NULL) {
        raster_close(r);
        return false;
    }

    for (y = 0; y < h; y++) {
        r->from[y] = w + 2;
        r->to[y] = 0;
    }

    r->top = h;
    r->bottom = 0;
    r->cap = (size_t)(w + 2) * (size_t)h;
    r->cap_h = h;
    return true;
}

bool raster_size(struct raster *r, int w, int h)
{
    if (w < 1 || h < 1) {
        return false;
    }

    if (r->acc != NULL && (size_t)(w + 2) * (size_t)h <= r->cap && h <= r->cap_h) {
        /* All nought already; the rows' spans say untouched however `w`
         * changed, since a span is untouched while `from >= to`. */
        r->w = w;
        r->h = h;
        r->top = h;
        r->bottom = 0;
        return true;
    }

    raster_close(r);
    return raster_open(r, w, h);
}

void raster_close(struct raster *r)
{
    free(r->acc);
    free(r->from);
    free(r->to);
    memset(r, 0, sizeof(*r));
}

/* Neither infinite nor not a number: either would, a step or two later,
 * become an index nobody can say anything about. */
static bool is_finite(float v)
{
    return v - v == 0.0f;
}

static float within(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/*
 * One piece of an edge within a row - from x `xs` to `xe`, both inside the
 * picture, sweeping `d` of the row's height, signed - into the row's cells:
 * font-rs's `draw_line`, for a row.
 */
static void sweep(struct raster *r, float *row, int y, float xs, float xe, float d)
{
    float xa = xs < xe ? xs : xe;
    float xb = xs < xe ? xe : xs;
    float xafloor = floorf(xa);
    int xai = (int)xafloor;
    int xbi = (int)ceilf(xb);
    int last;

    if (xbi <= xai + 1) {
        float xmf = 0.5f * (xs + xe) - xafloor;

        row[xai] += d - d * xmf;
        row[xai + 1] += d * xmf;
        last = xai + 1;
    } else {
        float s = 1.0f / (xb - xa);
        float xaf = xa - xafloor;
        float a0 = 0.5f * s * (1.0f - xaf) * (1.0f - xaf);
        float xbf = xb - ceilf(xb) + 1.0f;
        float am = 0.5f * s * xbf * xbf;
        int xi;

        row[xai] += d * a0;

        if (xbi == xai + 2) {
            row[xai + 1] += d * (1.0f - a0 - am);
        } else {
            float a1 = s * (1.5f - xaf);
            float a2;

            row[xai + 1] += d * (a1 - a0);

            for (xi = xai + 2; xi < xbi - 1; xi++) {
                row[xi] += d * s;
            }

            a2 = a1 + (float)(xbi - xai - 3) * s;
            row[xbi - 1] += d * (1.0f - a2 - am);
        }

        row[xbi] += d * am;
        last = xbi;
    }

    if (xai < r->from[y]) r->from[y] = xai;
    if (last + 1 > r->to[y]) r->to[y] = last + 1;
}

/* One straight edge into the accumulator. */
void raster_edge(struct raster *r, float x0, float y0, float x1, float y1)
{
    float dir = 1.0f, dxdy, x, w = (float)r->w;
    int y, ytop, ybottom;

    if (!is_finite(x0) || !is_finite(y0) || !is_finite(x1)
        || !is_finite(y1)) {
        return;
    }

    if (y0 - y1 <= 1e-6f && y1 - y0 <= 1e-6f) {
        return;
    }

    if (y0 > y1) {
        float t;

        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
        dir = -1.0f;
    }

    if (y1 <= 0.0f || y0 >= (float)r->h) {
        return;
    }

    /*
     * **Outside to the left or right counts at the picture's edge**: the
     * area is the same to every pixel inside, which is all the sum needs.
     *
     * **Exactly so, where an edge crosses a side.** Each row's piece of the
     * edge is cut where it crosses x = 0 or x = w: what lies left of the
     * picture sweeps its part of the row's height as an upright edge at
     * 0, what lies right of it as one at w, and what lies inside as it is.
     * The ends of the whole edge used to be held to the sides first, which
     * made an edge that crossed one into another edge, drawn a little off
     * for all its length inside - unseen in an SVG, whose shapes seldom
     * cross the picture's edge, and a step in every street where two map
     * tiles met, since a tile's features cross its edges all the time
     * (`docs/maps.md` M3, `tools/test_path.c`'s two boxes).
     */
    dxdy = (x1 - x0) / (y1 - y0);
    x = x0;
    ytop = 0;

    if (y0 < 0.0f) {
        x = x - y0 * dxdy;
    } else {
        ytop = (int)y0;
    }

    ybottom = y1 >= (float)r->h ? r->h : (int)ceilf(y1);

    if (ytop < r->top) r->top = ytop;
    if (ybottom > r->bottom) r->bottom = ybottom;

    for (y = ytop; y < ybottom; y++) {
        float *row = r->acc + (size_t)y * (size_t)(r->w + 2);
        float below = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        float above = (float)y > y0 ? (float)y : y0;
        float dy = below - above;
        float xnext = x + dxdy * dy;
        float cut[4], at[4];
        int n = 0;

        /* Where along this row's piece (0 to 1) it crosses a side. */
        cut[n++] = 0.0f;

        if ((x < 0.0f) != (xnext < 0.0f)) cut[n++] = (0.0f - x) / (xnext - x);
        if ((x > w) != (xnext > w)) cut[n++] = (w - x) / (xnext - x);

        if (n == 3 && cut[2] < cut[1]) {
            float t = cut[1];

            cut[1] = cut[2], cut[2] = t;
        }

        cut[n++] = 1.0f;

        for (int i = 0; i < n; i++) at[i] = x + (xnext - x) * cut[i];

        for (int i = 0; i + 1 < n; i++) {
            float piece = (cut[i + 1] - cut[i]) * dy * dir;
            float xs = within(at[i], 0.0f, w), xe = within(at[i + 1], 0.0f, w);

            if (piece != 0.0f) sweep(r, row, y, xs, xe, piece);
        }

        x = xnext;
    }
}

/* A float's magnitude, by its sign bit - as the lanes take it. */
static float magnitude(float v)
{
    uint32_t bits;

    __builtin_memcpy(&bits, &v, sizeof(bits));
    bits &= 0x7fffffffu;
    __builtin_memcpy(&v, &bits, sizeof(v));
    return v;
}

/*
 * One pixel: `sum` the running coverage, `d` what is there, (cr, cg, cb)
 * the colour. The reference the lanes are held to, operation for operation.
 */
static uint32_t lay(float sum, uint32_t d, float cr, float cg, float cb)
{
    float c = magnitude(sum);
    float da, keep, oa, inv, r, g, b;

    c = c < 1.0f ? c : 1.0f;
    da = (float)(d >> 24) * (1.0f / 255.0f);
    keep = da * (1.0f - c);
    oa = c + keep;
    inv = 1.0f / (oa > 0.0f ? oa : 1.0f);
    r = (cr * c + (float)((d >> 16) & 0xffu) * keep) * inv;
    g = (cg * c + (float)((d >> 8) & 0xffu) * keep) * inv;
    b = (cb * c + (float)(d & 0xffu) * keep) * inv;

    return (uint32_t)(oa * 255.0f + 0.5f) << 24
         | (uint32_t)(r + 0.5f) << 16
         | (uint32_t)(g + 0.5f) << 8
         | (uint32_t)(b + 0.5f);
}

/*
 * The lanes load and store through `__builtin_memcpy` of a vector's size,
 * which GCC makes one instruction. Plain `memcpy` was a call: under
 * `-ffreestanding` GCC may not take `memcpy` to be the C library's, so on
 * ARM every sixteen bytes went through the libc's loop - which a test on
 * the Mac, whose compiler inlines it, could not show (`roadmap.md` 6zz h).
 */
typedef float    f32x4 __attribute__((vector_size(16)));
typedef uint32_t u32x4 __attribute__((vector_size(16)));
typedef int32_t  i32x4 __attribute__((vector_size(16)));

/* Where `pick` is all ones, `a`; elsewhere `b` - a select, without the
 * `?:` on vectors that C does not have. */
static f32x4 choose(i32x4 pick, f32x4 a, f32x4 b)
{
    return (f32x4)(((i32x4)a & pick) | ((i32x4)b & ~pick));
}

/* Four pixels at once: `lay`, lane by lane. */
static void lay4(uint32_t *out, f32x4 s, f32x4 cr, f32x4 cg, f32x4 cb)
{
    const f32x4 one = { 1.0f, 1.0f, 1.0f, 1.0f };
    const f32x4 zero = { 0.0f, 0.0f, 0.0f, 0.0f };
    const f32x4 to_unit = { 1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f,
                            1.0f / 255.0f };
    const f32x4 scale = { 255.0f, 255.0f, 255.0f, 255.0f };
    const f32x4 half = { 0.5f, 0.5f, 0.5f, 0.5f };
    const u32x4 byte = { 0xffu, 0xffu, 0xffu, 0xffu };
    f32x4 c, da, keep, oa, inv, r, g, b;
    u32x4 d, a, ri, gi, bi;

    __builtin_memcpy(&d, out, sizeof(d));

    c = (f32x4)((i32x4)s & 0x7fffffff);
    c = choose(c < one, c, one);
    da = __builtin_convertvector(d >> 24, f32x4) * to_unit;
    keep = da * (one - c);
    oa = c + keep;
    inv = one / choose(oa > zero, oa, one);
    r = (cr * c + __builtin_convertvector((d >> 16) & byte, f32x4) * keep)
        * inv;
    g = (cg * c + __builtin_convertvector((d >> 8) & byte, f32x4) * keep)
        * inv;
    b = (cb * c + __builtin_convertvector(d & byte, f32x4) * keep) * inv;

    a = __builtin_convertvector(oa * scale + half, u32x4);
    ri = __builtin_convertvector(r + half, u32x4);
    gi = __builtin_convertvector(g + half, u32x4);
    bi = __builtin_convertvector(b + half, u32x4);
    d = a << 24 | ri << 16 | gi << 8 | bi;
    __builtin_memcpy(out, &d, sizeof(d));
}

/*
 * Four slots' running sums, after `carry`: [a, a+b, (b+c)+a, (c+d)+(a+b)],
 * each plus `carry` - in the lanes, and below one at a time in exactly the
 * same additions.
 */
static f32x4 running4(f32x4 v, f32x4 carry)
{
    const f32x4 zero = { 0.0f, 0.0f, 0.0f, 0.0f };

    v += __builtin_shufflevector(zero, v, 0, 4, 5, 6);
    v += __builtin_shufflevector(zero, v, 0, 1, 4, 5);
    return v + carry;
}

static void running1(float *v, float carry)
{
    float p0 = v[0] + 0.0f, p1 = v[1] + v[0];
    float p2 = v[2] + v[1], p3 = v[3] + v[2];

    v[0] = (p0 + 0.0f) + carry;
    v[1] = (p1 + 0.0f) + carry;
    v[2] = (p2 + p0) + carry;
    v[3] = (p3 + p1) + carry;
}

void raster_paint(struct raster *r, uint32_t *pixels, unsigned pitch,
                  uint32_t rgb, bool vectors)
{
    float cr = (float)((rgb >> 16) & 0xffu);
    float cg = (float)((rgb >> 8) & 0xffu);
    float cb = (float)(rgb & 0xffu);
    f32x4 vr = { cr, cr, cr, cr };
    f32x4 vg = { cg, cg, cg, cg };
    f32x4 vb = { cb, cb, cb, cb };
    int y;

    for (y = r->top; y < r->bottom; y++) {
        float *row = r->acc + (size_t)y * (size_t)(r->w + 2);
        uint32_t *out = (uint32_t *)((uint8_t *)pixels + (size_t)y * pitch);
        int from = r->from[y], to = r->to[y];
        int end = to < r->w ? to : r->w;
        float sum = 0.0f;
        int x = from;

        if (from >= to) {
            continue;
        }

        if (vectors) {
            f32x4 carry = { 0.0f, 0.0f, 0.0f, 0.0f };

            for (; x + 4 <= end; x += 4) {
                f32x4 v;

                __builtin_memcpy(&v, row + x, sizeof(v));
                v = running4(v, carry);
                carry = __builtin_shufflevector(v, v, 3, 3, 3, 3);
                lay4(out + x, v, vr, vg, vb);
            }

            sum = carry[0];
        } else {
            for (; x + 4 <= end; x += 4) {
                float v[4];
                int k;

                __builtin_memcpy(v, row + x, sizeof(v));
                running1(v, sum);
                sum = v[3];

                for (k = 0; k < 4; k++) {
                    out[x + k] = lay(v[k], out[x + k], cr, cg, cb);
                }
            }
        }

        for (; x < end; x++) {
            sum += row[x];
            out[x] = lay(sum, out[x], cr, cg, cb);
        }

        memset(row + from, 0, (size_t)(to - from) * sizeof(*row));
        r->from[y] = r->w + 2;
        r->to[y] = 0;
    }

    r->top = r->h;
    r->bottom = 0;
}
