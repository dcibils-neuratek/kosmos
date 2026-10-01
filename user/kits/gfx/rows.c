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
