/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The picture into a surface. `snes_blit.h` has why it is a file of its own.
 *
 * Row by row through the surface's own pitch (`gfx.md` 19.3), because a
 * surface's rows are almost never `width * 4` bytes. Within one block of
 * `scale` rows, the first is built pixel by pixel and the rest are the same
 * bytes, so they are copied a row at a time rather than worked out again.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "snes_blit.h"

void snes_blit(uint32_t *dst, unsigned w, unsigned h, unsigned pitch,
               const uint32_t *src, unsigned sw, unsigned sh, unsigned scale)
{
    unsigned sy;

    if (scale == 0) {
        scale = 1;
    }

    if (dst == NULL || src == NULL || (size_t)pitch < (size_t)w * 4u) {
        return;
    }

    for (sy = 0; sy < sh; sy++) {
        size_t top = (size_t)sy * scale;
        const uint32_t *in = src + (size_t)sy * sw;
        uint32_t *first;
        unsigned x, rep, cols = 0;

        if (top >= h) {
            break;
        }

        first = (uint32_t *)(void *)((uint8_t *)dst + top * pitch);

        for (x = 0; x < sw && cols < w; x++) {
            uint32_t px = in[x] | 0xff000000u;

            for (rep = 0; rep < scale && cols < w; rep++) {
                first[cols++] = px;
            }
        }

        for (rep = 1; rep < scale && top + rep < h; rep++) {
            memcpy((uint8_t *)dst + (top + rep) * pitch, first,
                   (size_t)cols * 4u);
        }
    }
}
