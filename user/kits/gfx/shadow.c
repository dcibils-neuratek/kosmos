/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A window's shadow, and the corner test it shares with the rest of the
 * graphics kit - in a file of their own, with no Lua in it, so that
 * `tools/test_shadow.c` can hold them to the method they replaced on this
 * computer, the way `snes_blit.c` is held (`roadmap.md` 5zu).
 */

#include <stddef.h>
#include <stdint.h>

#include "shadow.h"

/*
 * How much of this pixel is inside: 0 outside, 255 in, and a ramp across
 * the one-pixel band the arc passes through.
 *
 * **The ramp is what makes a corner look round at eight pixels.** Without
 * it the test is a yes or a no and the result is geometrically exact and
 * visibly a chamfer - thirteen pixels removed from a 64-pixel square, which
 * is precisely a quarter disc's area and looks like a cut corner because
 * every one of them is all or nothing.
 *
 * Squared distances throughout, so there is no square root: the band is
 * between `(r - 1/2)^2` and `(r + 1/2)^2` and coverage is linear across it.
 * That is not the true area of a circle's intersection with a pixel, and at
 * this size nobody can tell - what it has to do is stop the edge stepping.
 */
long gfx_round_cover(long x, long y, long rx, long ry, long rw, long rh,
                     long r)
{
    long dx, dy;
    long d2, lo, hi;

    if (x < rx || y < ry || x >= rx + rw || y >= ry + rh) {
        return 0;
    }

    if (r <= 0) {
        return 255;
    }

    /* How far into a corner square this pixel is, or zero when it is in the
     * straight part of an edge - which is the common case and costs one
     * comparison each way. */
    if (x < rx + r) {
        dx = rx + r - x;
    } else if (x >= rx + rw - r) {
        dx = x - (rx + rw - r) + 1;
    } else {
        return 255;
    }

    if (y < ry + r) {
        dy = ry + r - y;
    } else if (y >= ry + rh - r) {
        dy = y - (ry + rh - r) + 1;
    } else {
        return 255;
    }

    /* Everything times four, so the halves disappear: `(2r - 1)^2` is
     * `4 (r - 1/2)^2` and `(2r + 1)^2` is `4 (r + 1/2)^2`. */
    d2 = 4 * (dx * dx + dy * dy);
    lo = (2 * r - 1) * (2 * r - 1);
    hi = (2 * r + 1) * (2 * r + 1);

    if (d2 <= lo) {
        return 255;
    }

    if (d2 >= hi) {
        return 0;
    }

    return 255 * (hi - d2) / (hi - lo);
}

/*
 * **Darkening a run of pixels by one amount**, which is most of a shadow:
 * above and below a window every pixel of a row is the same distance from
 * it, so the whole row keeps the same fraction of what is behind. `keep` is
 * that fraction out of 256.
 *
 * Four pixels at a time where the processor has the instructions - NEON on
 * AArch64, SSE2 on x86-64, both always present on the machines this runs on
 * - and one at a time for the ends. Every channel is multiplied, the unused
 * top byte of an XRGB pixel with the rest, which costs nothing and keeps
 * the loop one shape.
 */
#if defined(__ARM_NEON)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

static inline uint32_t darken1(uint32_t p, uint32_t keep)
{
    uint32_t rb = ((p & 0x00ff00ffu) * keep) >> 8;
    uint32_t ag = (((p >> 8) & 0x00ff00ffu) * keep) >> 8;

    return (rb & 0x00ff00ffu) | ((ag & 0x00ff00ffu) << 8);
}

void gfx_darken_span(uint32_t *p, long n, uint32_t keep)
{
    long i = 0;

    /* All of what is behind kept is nothing to do - and a byte multiply
     * cannot say 256, so 255 would darken what should be untouched. */
    if (keep >= 256) {
        return;
    }

#if defined(__ARM_NEON)
    uint8x8_t k = vdup_n_u8((uint8_t)keep);

    for (; i + 4 <= n; i += 4) {
        uint8x16_t px = vld1q_u8((const uint8_t *)(p + i));
        uint16x8_t lo = vmull_u8(vget_low_u8(px), k);
        uint16x8_t hi = vmull_u8(vget_high_u8(px), k);

        vst1q_u8((uint8_t *)(p + i),
                 vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8)));
    }
#elif defined(__SSE2__)
    __m128i k = _mm_set1_epi16((short)keep);
    __m128i zero = _mm_setzero_si128();

    for (; i + 4 <= n; i += 4) {
        __m128i px = _mm_loadu_si128((const __m128i *)(p + i));
        __m128i lo = _mm_srli_epi16(_mm_mullo_epi16(
                         _mm_unpacklo_epi8(px, zero), k), 8);
        __m128i hi = _mm_srli_epi16(_mm_mullo_epi16(
                         _mm_unpackhi_epi8(px, zero), k), 8);

        _mm_storeu_si128((__m128i *)(p + i), _mm_packus_epi16(lo, hi));
    }
#endif

    for (; i < n; i++) {
        p[i] = darken1(p[i], keep);
    }
}

/*
 * `gfx_shadow` - `dst:shadow(rx, ry, rw, rh, radius, spread, alpha, cx, cy, cw, ch)`
 *
 * The soft edge a window casts, darkening what is already there.
 *
 * **Only the band outside the window, and only inside `cx, cy, cw, ch`.**
 * It was every pixel of the window's rectangle and its band, each tested
 * against the rounded corner and each with two divisions for its falloff,
 * clipped to the screen and to nothing smaller - so every rectangle the
 * desktop repainted during a drag computed the whole shadow of every window
 * it touched. Diego, 24 September: "the drop shadow makes the entire UI
 * unsable because of the slowness when dragging windows".
 *
 * Now the falloff is a table of `spread` entries made once per call, the
 * rows above and below the window are one amount each (`darken_span`), the
 * sides are a column of amounts, and the corner test is asked only in the
 * four corner squares, where the rounding is. The picture is the same: the
 * distance is to the nearest edge, the diagonal at a corner, and the fall
 * off squared for a softer knee.
 *
 * The clip is optional, for a caller that wants the whole of it.
 *
 * The falloff is linear in the distance past the edge, which is not what a
 * Gaussian blur gives and is what this wants: a blur of a hard rectangle is
 * expensive to compute per frame and its extra realism is invisible under a
 * window at these sizes. What a shadow has to do is separate a window from
 * what is behind it, and a ramp does that.
 *
 * Offset downwards by a third of the spread, because a shadow with no
 * direction reads as a glow. Down and slightly nowhere else - a light
 * source directly above is what every desktop since 1995 has drawn and the
 * one nobody has to think about.
 */
void gfx_shadow(uint32_t *pixels, unsigned long pitch, long width,
                long height, long rx, long ry, long rw, long rh, long r,
                long spread, long alpha, const long *clip)
{
    long drop = spread / 3;
    long x0, y0, x1, y1, y, d;
    uint32_t keep[256];

    if (spread <= 0 || alpha <= 0) {
        return;
    }

    if (spread > 255) {
        spread = 255;
    }

    if (alpha > 255) {
        alpha = 255;
    }

    /* What each distance keeps of what is behind, out of 256. */
    for (d = 0; d < spread; d++) {
        long a = alpha * (spread - d) / spread;

        a = a * (spread - d) / spread;
        keep[d] = (uint32_t)(256 - (a * 256 + 127) / 255);
    }

    /* The band's box, then the screen's, then the caller's clip. */
    x0 = rx - spread;
    x1 = rx + rw + spread;
    y0 = ry - spread + drop;
    y1 = ry + rh + spread + drop;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;

    if (clip != NULL) {
        if (x0 < clip[0]) x0 = clip[0];
        if (y0 < clip[1]) y0 = clip[1];
        if (x1 > clip[0] + clip[2]) x1 = clip[0] + clip[2];
        if (y1 > clip[1] + clip[3]) y1 = clip[1] + clip[3];
    }

    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    for (y = y0; y < y1; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)pixels
                                     + (size_t)y * pitch);
        long ny = y - drop;
        long near_y = ny < ry ? ry - ny : (ny >= ry + rh ? ny - (ry + rh) + 1 : 0);
        long in_corner_rows = (y >= ry && y < ry + r) ||
                              (y >= ry + rh - r && y < ry + rh);
        long x;

        /*
         * Not skipped when `near_y` is past the spread: beside the window
         * the diagonal, three quarters of the two distances together, can
         * still be inside it.
         */
        for (x = x0; x < x1; ) {
            long near_x;

            if (x < rx) {
                near_x = rx - x;
            } else if (x >= rx + rw) {
                near_x = x - (rx + rw) + 1;
            } else {
                /*
                 * Beside or inside the window. A row clear of it above or
                 * below is one amount from here to the window's right edge:
                 * one span. A row the window covers is skipped, except in
                 * its corner squares, where the rounding leaves some of
                 * each row for the shadow.
                 */
                long end = rx + rw < x1 ? rx + rw : x1;

                /*
                 * A row above or below the window - including the rows just
                 * under it, which the downward offset puts at a distance of
                 * nothing - is one amount the whole way across.
                 */
                if (!(y >= ry && y < ry + rh)) {
                    if (near_y < spread) {
                        gfx_darken_span(row + x, end - x, keep[near_y]);
                    }

                    x = end;
                    continue;
                }

                if (in_corner_rows && r > 0 && near_y < spread) {
                    long left_end = rx + r < end ? rx + r : end;
                    long right_from = rx + rw - r > x ? rx + rw - r : x;

                    for (; x < left_end; x++) {
                        if (gfx_round_cover(x, y, rx, ry, rw, rh, r) < 255) {
                            row[x] = darken1(row[x], keep[near_y]);
                        }
                    }

                    if (x < right_from) {
                        x = right_from;
                    }

                    for (; x < end; x++) {
                        if (gfx_round_cover(x, y, rx, ry, rw, rh, r) < 255) {
                            row[x] = darken1(row[x], keep[near_y]);
                        }
                    }
                }

                x = end;
                continue;
            }

            d = near_x > near_y ? near_x : near_y;

            /* The diagonal, so a corner does not read as square. */
            if (near_x > 0 && near_y > 0) {
                d = (near_x + near_y) * 3 / 4;
            }

            if (d < spread) {
                row[x] = darken1(row[x], keep[d]);
            }

            x++;
        }
    }
}
