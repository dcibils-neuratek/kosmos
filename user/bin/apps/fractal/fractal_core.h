/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **GPU Fractal's arithmetic, on the processor** (`fractal.c` has why it
 * is the kernel's, step for step): a point's escape and its colour. One
 * copy, which `fractal.c` draws with and `tools/test_eu_emu.c` holds the
 * kernel to, in the emulator of the execution units.
 */

#ifndef KOSMOS_FRACTAL_CORE_H
#define KOSMOS_FRACTAL_CORE_H

#include <stdint.h>

/* Red the low byte of 9n, green of 4n, blue of 15n; black inside. */
static inline uint32_t fractal_colour(int32_t n, int32_t most)
{
    if (n >= most) {
        return 0xff000000u;
    }

    return 0xff000000u | (((uint32_t)(n * 9) & 0xffu) << 16) | (((uint32_t)(n * 4) & 0xffu) << 8)
         | ((uint32_t)(n * 15) & 0xffu);
}

/* The steps the point takes while |z|^2 < 4, at most `most`. */
static inline int32_t fractal_escape(float cx, float cy, int32_t most)
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

#endif
