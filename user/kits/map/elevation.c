/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Heights from a terrarium tile (`elevation.h`).
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "elevation.h"

float map_terrarium(uint32_t pixel)
{
    unsigned r = (pixel >> 16) & 255u, g = (pixel >> 8) & 255u, b = pixel & 255u;

    return (float)((double)(r * 256u + g) + b / 256.0 - 32768.0);
}

bool map_heights_terrarium(struct map_heights *out, const uint32_t *pixels,
                           unsigned width, unsigned height, unsigned pitch)
{
    float lo = INFINITY, hi = -INFINITY;

    memset(out, 0, sizeof *out);

    if (width < 2 || height < 2 || pitch < width * 4u) return false;

    out->metres = malloc((size_t)width * height * sizeof *out->metres);

    if (out->metres == NULL) return false;

    for (unsigned y = 0; y < height; y++) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)pixels + (size_t)y * pitch);
        float *to = out->metres + (size_t)y * width;

        for (unsigned x = 0; x < width; x++) {
            float m = map_terrarium(row[x]);

            to[x] = m;
            if (m < lo) lo = m;
            if (m > hi) hi = m;
        }
    }

    out->width = width;
    out->height = height;
    out->lowest = lo;
    out->highest = hi;
    return true;
}

float map_heights_at(const struct map_heights *h, unsigned x, unsigned y)
{
    if (x >= h->width) x = h->width - 1;
    if (y >= h->height) y = h->height - 1;

    return h->metres[(size_t)y * h->width + x];
}

float map_heights_sample(const struct map_heights *h, double u, double v)
{
    /* Sample centres sit at (i + 0.5) / width. */
    double fx = u * h->width - 0.5, fy = v * h->height - 0.5;
    double x0, y0, tx, ty;

    if (fx < 0.0) fx = 0.0;
    if (fy < 0.0) fy = 0.0;
    if (fx > h->width - 1.0) fx = h->width - 1.0;
    if (fy > h->height - 1.0) fy = h->height - 1.0;

    x0 = floor(fx), y0 = floor(fy);
    tx = fx - x0, ty = fy - y0;

    {
        unsigned ax = (unsigned)x0, ay = (unsigned)y0;
        double a = map_heights_at(h, ax, ay), b = map_heights_at(h, ax + 1, ay);
        double c = map_heights_at(h, ax, ay + 1), d = map_heights_at(h, ax + 1, ay + 1);
        double top = a + (b - a) * tx, bottom = c + (d - c) * tx;

        return (float)(top + (bottom - top) * ty);
    }
}

void map_heights_free(struct map_heights *h)
{
    free(h->metres);
    memset(h, 0, sizeof *h);
}
