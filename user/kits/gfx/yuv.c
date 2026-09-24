/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * YUY2 to 0xffRRGGBB. `yuv.h` says what and why.
 *
 * The arithmetic is BT.601's studio-range matrix scaled by 256, the form
 * every integer implementation uses - brightness stretched from 16..235 to
 * 0..255 by 298/256, and the colour differences weighted:
 *
 *   R = (298 C + 409 E + 128) >> 8
 *   G = (298 C - 100 D - 208 E + 128) >> 8
 *   B = (298 C + 516 D + 128) >> 8
 *
 * with C = Y - 16, D = U - 128, E = V - 128, and each clipped to 0..255.
 */

#include "yuv.h"

static inline uint32_t clip(int x)
{
    return x < 0 ? 0u : (x > 255 ? 255u : (uint32_t)x);
}

uint32_t gfx_yuv_pixel(int y, int u, int v)
{
    int c = 298 * (y - 16) + 128;
    int d = u - 128;
    int e = v - 128;

    return 0xff000000u
         | clip((c + 409 * e) >> 8) << 16
         | clip((c - 100 * d - 208 * e) >> 8) << 8
         | clip((c + 516 * d) >> 8);
}

/*
 * Two pixels at a time, because two share their colour: the three colour
 * terms are worked out once for the pair and added to each brightness.
 * Mirrored, a row is written from its right end leftwards - the pair's
 * second pixel first - so the loop is the same loop with a step of -1.
 */
void gfx_yuy2(uint32_t *dst, unsigned long pitch, const uint8_t *src,
              unsigned width, unsigned height, bool mirror)
{
    unsigned x, y, pairs = width / 2u;

    for (y = 0; y < height; y++) {
        const uint8_t *s = src + (unsigned long)y * width * 2u;
        uint32_t *row = (uint32_t *)((uint8_t *)dst + (unsigned long)y * pitch);

        for (x = 0; x < pairs; x++, s += 4) {
            int d = s[1] - 128;
            int e = s[3] - 128;
            int r = 409 * e + 128;
            int g = -100 * d - 208 * e + 128;
            int b = 516 * d + 128;
            int c0 = 298 * (s[0] - 16);
            int c1 = 298 * (s[2] - 16);
            uint32_t p0 = 0xff000000u | clip((c0 + r) >> 8) << 16
                        | clip((c0 + g) >> 8) << 8 | clip((c0 + b) >> 8);
            uint32_t p1 = 0xff000000u | clip((c1 + r) >> 8) << 16
                        | clip((c1 + g) >> 8) << 8 | clip((c1 + b) >> 8);

            if (mirror) {
                row[width - 1u - 2u * x] = p0;
                row[width - 2u - 2u * x] = p1;
            } else {
                row[2u * x] = p0;
                row[2u * x + 1u] = p1;
            }
        }
    }
}
