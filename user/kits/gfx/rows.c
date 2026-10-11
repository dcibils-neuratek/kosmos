/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Filling, and a glyph's coverage over a row, four pixels at a time
 * (`rows.h`, `roadmap.md` 6zz h).
 *
 * In GCC's own vector types, as `pack.c` and `gfx.c`'s blend are written -
 * NEON on AArch64, SSE2 on x86-64, the compiler choosing the instructions -
 * with the scalar loop as the tail and as the reference `tools/test_rows.c`
 * holds the lanes to.
 *
 * **The rounding is the scalar one to the bit.** A glyph's mix divides by
 * 255 with rounding, `(x + 127) / 255`, and a vector unit has no divide.
 * The lanes take the identity `pack.c` does, `(t + (t >> 8)) >> 8` with
 * `t = x + 128`, which is the same answer for every `x` a mix can make: 255
 * is odd, so no `x / 255` is ever exactly a half, and rounding to nearest
 * up and down agree. The test checks that, every coverage over every pair.
 */

#include <stddef.h>
#include <string.h>

#include "rows.h"
#include "surface.h"

/*
 * The lanes load and store through `__builtin_memcpy` of a vector's size,
 * which GCC makes one instruction. Plain `memcpy` was a call: under
 * `-ffreestanding` GCC may not take `memcpy` to be the C library's, so on
 * ARM every sixteen bytes went through the libc's loop - which a test on
 * the Mac, whose compiler inlines it, could not show (`roadmap.md` 6zz h).
 */
typedef uint32_t u32x4 __attribute__((vector_size(16)));
typedef uint8_t  u8x4  __attribute__((vector_size(4)));

void gfx_fill_row(uint32_t *p, long n, uint32_t colour, bool vectors)
{
    long i = 0;

    if (vectors) {
        const u32x4 v = { colour, colour, colour, colour };

        for (; i + 8 <= n; i += 8) {
            __builtin_memcpy(p + i, &v, sizeof(v));
            __builtin_memcpy(p + i + 4, &v, sizeof(v));
        }

        for (; i + 4 <= n; i += 4) {
            __builtin_memcpy(p + i, &v, sizeof(v));
        }
    }

    for (; i < n; i++) {
        p[i] = colour;
    }
}

/* One pixel: `gfx.c`'s `mix`, with its two ends. */
static uint32_t cover1(uint32_t dst, uint32_t ink, unsigned a)
{
    unsigned inv = 255u - a, r, g, b;

    if (a == 0) {
        return dst;
    }

    if (a == 255) {
        return ink;
    }

    /*
     * **A destination that is not opaque gets the compositor's `over`**,
     * with the coverage as the ink's alpha. The mix below stamps the pixel
     * opaque after mixing the ink with whatever colour was there - which on
     * the desktop, a transparent surface laid over the wallpaper, is
     * transparent black. Every soft edge of a white name became an opaque
     * dark grey, and with the shadow under it thin text read as black with
     * a fringe (Diego's T14, 11 October 2026: "Text has some weird black
     * shadow in the desktop"). `blend_row` learnt this for Haiku's icon
     * shadows; text had not.
     */
    if ((dst >> 24) != 255u) {
        return over((ink & 0x00ffffffu) | ((uint32_t)a << 24), dst, 255u);
    }

    r = ((((ink >> 16) & 0xff) * a) + (((dst >> 16) & 0xff) * inv) + 127)
        / 255;
    g = ((((ink >> 8) & 0xff) * a) + (((dst >> 8) & 0xff) * inv) + 127)
        / 255;
    b = (((ink & 0xff) * a) + ((dst & 0xff) * inv) + 127) / 255;

    return 0xff000000u | (r << 16) | (g << 8) | b;
}

/* `x` divided by 255, rounded, in the lanes - for `x` up to 65535. */
static u32x4 div255(u32x4 x)
{
    u32x4 t = x + 128;

    return (t + (t >> 8)) >> 8;
}

void gfx_cover_row(uint32_t *dst, const uint8_t *coverage, long n,
                   uint32_t ink, bool vectors)
{
    long i = 0;

    if (vectors) {
        const u32x4 ir = { (ink >> 16) & 0xff, (ink >> 16) & 0xff,
                           (ink >> 16) & 0xff, (ink >> 16) & 0xff };
        const u32x4 ig = { (ink >> 8) & 0xff, (ink >> 8) & 0xff,
                           (ink >> 8) & 0xff, (ink >> 8) & 0xff };
        const u32x4 ib = { ink & 0xff, ink & 0xff, ink & 0xff, ink & 0xff };
        const u32x4 whole = { ink, ink, ink, ink };
        const u32x4 opaque = { 0xff000000u, 0xff000000u, 0xff000000u,
                               0xff000000u };

        for (; i + 4 <= n; i += 4) {
            u8x4 c8;
            u32x4 a, inv, d, mixed, none, all;

            __builtin_memcpy(&c8, coverage + i, sizeof(c8));

            /* A run of nothing - the space around a stem - is left as it
             * is, which is most of a glyph's box. */
            if ((c8[0] | c8[1] | c8[2] | c8[3]) == 0) {
                continue;
            }

            a = __builtin_convertvector(c8, u32x4);
            inv = 255 - a;
            __builtin_memcpy(&d, dst + i, sizeof(d));

            /* The lanes mix onto an opaque ground, which is a window's;
             * anything else - the desktop - goes a pixel at a time. */
            if (((d[0] & d[1] & d[2] & d[3]) >> 24) != 255u) {
                long k;

                for (k = 0; k < 4; k++) {
                    dst[i + k] = cover1(dst[i + k], ink, coverage[i + k]);
                }

                continue;
            }

            mixed = opaque
                    | div255(ir * a + ((d >> 16) & 0xff) * inv) << 16
                    | div255(ig * a + ((d >> 8) & 0xff) * inv) << 8
                    | div255(ib * a + (d & 0xff) * inv);

            /* 0 leaves the ground and 255 is the ink whole, alpha and all,
             * as the scalar loop has it. */
            none = (u32x4)(a == 0);
            all = (u32x4)(a == 255);
            d = (d & none) | (whole & all) | (mixed & ~(none | all));
            __builtin_memcpy(dst + i, &d, sizeof(d));
        }
    }

    for (; i < n; i++) {
        dst[i] = cover1(dst[i], ink, coverage[i]);
    }
}

/*
 * A wash: `colour` laid over a row by its own alpha, the same for every
 * pixel - a dimmed window behind a sheet, a tint over a picture. A run of
 * coverage that never changes, so the same lanes and the same rounding as
 * `gfx_cover_row`, and `tools/test_rows.c` holds it to that function given
 * a row of one byte.
 */
void gfx_wash_row(uint32_t *dst, long n, uint32_t colour, bool vectors)
{
    unsigned a = colour >> 24;
    uint32_t ink = 0xff000000u | (colour & 0x00ffffffu);
    long i = 0;

    if (a == 0) {
        return;
    }

    if (a == 255) {
        gfx_fill_row(dst, n, ink, vectors);
        return;
    }

    if (vectors) {
        const u32x4 inv = { 255 - a, 255 - a, 255 - a, 255 - a };
        const u32x4 ir = { ((ink >> 16) & 0xff) * a, ((ink >> 16) & 0xff) * a,
                           ((ink >> 16) & 0xff) * a, ((ink >> 16) & 0xff) * a };
        const u32x4 ig = { ((ink >> 8) & 0xff) * a, ((ink >> 8) & 0xff) * a,
                           ((ink >> 8) & 0xff) * a, ((ink >> 8) & 0xff) * a };
        const u32x4 ib = { (ink & 0xff) * a, (ink & 0xff) * a,
                           (ink & 0xff) * a, (ink & 0xff) * a };
        const u32x4 opaque = { 0xff000000u, 0xff000000u, 0xff000000u,
                               0xff000000u };

        for (; i + 4 <= n; i += 4) {
            u32x4 d;

            __builtin_memcpy(&d, dst + i, sizeof(d));

            d = opaque
                | div255(ir + ((d >> 16) & 0xff) * inv) << 16
                | div255(ig + ((d >> 8) & 0xff) * inv) << 8
                | div255(ib + (d & 0xff) * inv);
            __builtin_memcpy(dst + i, &d, sizeof(d));
        }
    }

    for (; i < n; i++) {
        dst[i] = cover1(dst[i], ink, a);
    }
}
