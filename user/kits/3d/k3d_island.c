/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * An invented island (`k3d_island.h`): value noise, a few octaves of it,
 * shaped by the distance from the frame's origin into an island.
 */

#include "k3d_island.h"

#include <math.h>

#define RADIUS   7000.0             /* metres from the centre to the coast, about */
#define PEAK      260.0             /* the shape's height at the centre */
#define RIDGES     90.0             /* the noise's, either way, inland */
#define SEA_FLOOR  -30.0

/* A lattice point's value, -1 to 1, from its integer coordinates. */
static double lattice(int64_t x, int64_t y)
{
    uint64_t h = (uint64_t)x * 0x9e3779b97f4a7c15ull ^ (uint64_t)y * 0xc2b2ae3d27d4eb4full;

    h ^= h >> 31;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 29;
    return (double)(h >> 11) / (double)(1ull << 52) - 1.0;
}

static double smooth(double t) { return t * t * (3.0 - 2.0 * t); }

/* Value noise at a place in lattice units, -1 to 1. */
static double noise(double x, double y)
{
    double fx = floor(x), fy = floor(y);
    int64_t ix = (int64_t)fx, iy = (int64_t)fy;
    double u = smooth(x - fx), v = smooth(y - fy);
    double a = lattice(ix, iy), b = lattice(ix + 1, iy);
    double c = lattice(ix, iy + 1), d = lattice(ix + 1, iy + 1);

    return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}

/* Five octaves, the first `wave` metres long: -1 to 1, about. */
static double fractal(double x, double z, double wave)
{
    double sum = 0, amplitude = 0.5, f = 1.0 / wave;

    for (int i = 0; i < 5; i++) {
        sum += amplitude * noise(x * f + 17.3 * i, z * f - 41.7 * i);
        amplitude *= 0.5;
        f *= 2.0;
    }
    return sum / 0.97;
}

float k3d_island_height(double x, double z)
{
    /* The coast wanders: the distance is stretched by noise of its own. */
    double d = sqrt(x * x + z * z) / RADIUS * (1.0 + 0.25 * fractal(x, z, 5000.0));
    double inland = d < 1.0 ? 1.0 - d : 0.0;
    double shape = PEAK * pow(inland, 1.3);
    double ridges = RIDGES * inland * fractal(x + 3100.0, z - 900.0, 2500.0);

    return (float)(shape + ridges + SEA_FLOOR * (1.0 - inland));
}

static uint32_t mix(uint32_t a, uint32_t b, double t)
{
    if (t <= 0) {
        return a;
    }
    if (t >= 1) {
        return b;
    }
    uint32_t out = 0xff000000u;

    for (int shift = 0; shift <= 16; shift += 8) {
        double ca = (a >> shift) & 255, cb = (b >> shift) & 255;

        out |= (uint32_t)(ca + (cb - ca) * t + 0.5) << shift;
    }
    return out;
}

uint32_t k3d_island_colour(double x, double z)
{
    double h = k3d_island_height(x, z);
    double step = 6.0;
    double dx = (k3d_island_height(x + step, z) - k3d_island_height(x - step, z)) / (2 * step);
    double dz = (k3d_island_height(x, z + step) - k3d_island_height(x, z - step)) / (2 * step);
    double slope = sqrt(dx * dx + dz * dz);
    /* A little grain, so a field is not one colour. */
    double grain = noise(x / 23.0, z / 23.0) * 0.5 + noise(x / 7.0, z / 7.0) * 0.5;
    uint32_t c;

    if (h < 0) {
        c = mix(0xff2f8496u, 0xff173d5eu, -h / -SEA_FLOOR);
    } else if (h < 5) {
        c = mix(0xffdcca9eu, 0xffc8b585u, h / 5);
    } else {
        uint32_t green = mix(0xff6e9440u, 0xff3f5e2au, (grain + 1) / 2 * 0.8 + h / 600);

        c = mix(green, 0xff86807au, fmax((h - 200) / 80, (slope - 0.45) / 0.3));
    }
    return mix(c, grain > 0 ? 0xffffffffu : 0xff000000u, fabs(grain) * 0.06);
}

/* A sample's place across the world: its centre, (i + 0.5) / side across
 * the tile - as an elevation tile's pixels are read. */
static void sample_at(const struct k3d_place *p, struct k3d_tile t, unsigned side, unsigned i,
                      unsigned j, double *x, double *z)
{
    double n = ldexp(1.0, t.z);

    k3d_place_world(p, (t.x + (i + 0.5) / side) / n, (t.y + (j + 0.5) / side) / n, x, z);
}

void k3d_island_heights(const struct k3d_place *p, struct k3d_tile t, unsigned side,
                        float *metres, float *lowest, float *highest)
{
    *lowest = INFINITY;
    *highest = -INFINITY;
    for (unsigned j = 0; j < side; j++) {
        for (unsigned i = 0; i < side; i++) {
            double x, z;

            sample_at(p, t, side, i, j, &x, &z);
            float h = fmaxf(0.0f, k3d_island_height(x, z));

            metres[j * side + i] = h;
            *lowest = fminf(*lowest, h);
            *highest = fmaxf(*highest, h);
        }
    }
}

void k3d_island_picture(const struct k3d_place *p, struct k3d_tile t, unsigned side,
                        uint32_t *pixels)
{
    for (unsigned j = 0; j < side; j++) {
        for (unsigned i = 0; i < side; i++) {
            double x, z;

            sample_at(p, t, side, i, j, &x, &z);
            pixels[j * side + i] = k3d_island_colour(x, z);
        }
    }
}
