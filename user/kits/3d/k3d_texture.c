/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The 3D Kit's textures: patterns worked out from where a point is.
 *
 * **Procedural, and in three dimensions**, as Blender's Brick, Checker,
 * Noise and Wave nodes are (`roadmap.md` 4l). A pattern is a function of
 * the point on the surface, so nothing is unwrapped, nothing is stored and
 * a texture costs no memory - and a brick wall cut in two shows bricks on
 * the cut, which a picture wrapped round it would not.
 *
 * **In metres of the object as it stands.** The point is the object's own,
 * times its scale, so a wall stretched to four metres has four metres of
 * bricks rather than eight stretched ones, and turning the object turns the
 * pattern with it.
 *
 * Each pattern answers two numbers: how far towards the second colour the
 * point is (`fac`), and how high it stands (`height`, nought to one) - so
 * mortar sits back from its bricks and one roof tile overlaps the next when
 * the tracer bends the surface's normal by it (`k3d_trace.c`).
 */

#include <math.h>
#include <stdint.h>

#include "k3d.h"

static inline float lo2(float a, float b) { return a < b ? a : b; }
static inline float hi2(float a, float b) { return a > b ? a : b; }
static inline float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static inline float flr(float x) { return (float)floor((double)x); }
static inline float frac(float x) { return x - flr(x); }

static inline float smooth(float a, float b, float x)
{
    float t = clamp01((x - a) / (b - a));

    return t * t * (3 - 2 * t);
}

/*--------------------------------------------------------------------------
 * Noise: Perlin's improved gradient noise, in octaves.
 *------------------------------------------------------------------------*/

/* A hash of three integers, the same on every machine and every run. */
static uint32_t hash3(int x, int y, int z)
{
    uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u
                 ^ (uint32_t)z * 0xcb1ab31fu;

    h ^= h >> 13;
    h *= 0x5bd1e995u;
    return h ^ (h >> 15);
}

static float grad(uint32_t h, float x, float y, float z)
{
    /* Twelve directions, the edges of a cube: Perlin's 2002 set. */
    switch (h % 12u) {
    case 0:  return  x + y;
    case 1:  return -x + y;
    case 2:  return  x - y;
    case 3:  return -x - y;
    case 4:  return  x + z;
    case 5:  return -x + z;
    case 6:  return  x - z;
    case 7:  return -x - z;
    case 8:  return  y + z;
    case 9:  return -y + z;
    case 10: return  y - z;
    default: return -y - z;
    }
}

static inline float fade(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }
static inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

/* One octave, about nought, roughly within one either side. */
static float noise1(float x, float y, float z)
{
    int X = (int)flr(x), Y = (int)flr(y), Z = (int)flr(z);
    float fx = x - (float)X, fy = y - (float)Y, fz = z - (float)Z;
    float u = fade(fx), v = fade(fy), w = fade(fz);

    return lerp(lerp(lerp(grad(hash3(X, Y, Z), fx, fy, fz),
                          grad(hash3(X + 1, Y, Z), fx - 1, fy, fz), u),
                     lerp(grad(hash3(X, Y + 1, Z), fx, fy - 1, fz),
                          grad(hash3(X + 1, Y + 1, Z), fx - 1, fy - 1, fz), u), v),
                lerp(lerp(grad(hash3(X, Y, Z + 1), fx, fy, fz - 1),
                          grad(hash3(X + 1, Y, Z + 1), fx - 1, fy, fz - 1), u),
                     lerp(grad(hash3(X, Y + 1, Z + 1), fx, fy - 1, fz - 1),
                          grad(hash3(X + 1, Y + 1, Z + 1), fx - 1, fy - 1, fz - 1), u),
                     v), w);
}

/* Octaves, each twice as fine and half as strong: Blender's Detail. */
float k3d_noise(const float p[3], float detail)
{
    float sum = 0, amp = 1, norm = 0, f = 1;
    int o, octaves = 1 + (int)lo2(hi2(detail, 0), 8);

    for (o = 0; o < octaves; o++) {
        sum += amp * noise1(p[0] * f, p[1] * f, p[2] * f);
        norm += amp;
        amp *= 0.5f;
        f *= 2.0f;
    }

    return sum / norm;
}

/*--------------------------------------------------------------------------
 * The patterns.
 *------------------------------------------------------------------------*/

/*
 * Bricks laid along the ground: rows `1 / scale` tall, bricks `ratio` times
 * as long as they are tall, each row moved along by `offset` of a brick, and
 * `mortar` of a row's height between them. Along the ground is along x and
 * y at once, so a wall facing either way - or turning a corner - is
 * coursed, and up is z.
 *
 * **Laid by which way the surface faces**, as Blender's box mapping lays a
 * picture: a face turned up or down - a path, a step, a chimney's top - is
 * paved in x and y instead. Coursed by height alone, a path of paving was
 * one long course: planks.
 */
static void brick(const struct k3d_texture *t, const float q[3], const float n[3],
                  float *fac, float *height, bool shingles)
{
    float H = 1.0f / hi2(t->scale, 1e-3f), W = H * hi2(t->ratio, 0.1f);
    bool floor = fabsf(n[2]) > 0.7f;
    float u = floor ? q[0] : q[0] + q[1], v = floor ? q[1] : q[2];
    float row = flr(v / H), fv = frac(v / H);
    float along = u / W + row * t->offset, col = flr(along), fu = frac(along);
    float du = lo2(fu, 1 - fu) * W, dv = lo2(fv, 1 - fv) * H;
    float m = t->mortar * H / 2, edge = lo2(du, dv);
    uint32_t id = hash3((int)col, (int)row, 7);

    if (shingles) {
        /* Each row laps over the one below: the top of a tile is under the
         * next row and its bottom edge stands proud, so the height climbs
         * from the bottom of the row to the top. The gap is only between
         * tiles in a row. */
        *fac = du < m ? 1.0f : (float)(id & 255u) / 255.0f * 0.35f;
        *height = du < m ? 0.0f : 1.0f - fv;
        return;
    }

    if (edge < m) {
        *fac = 1;                           /* mortar */
        *height = 0;
        return;
    }

    /* A brick: its own shade of the colour, and its edges rounded off into
     * the mortar rather than stepped. */
    *fac = (float)(id & 255u) / 255.0f * 0.3f - 0.15f;
    *height = smooth(m, m + H * 0.12f, edge);
}

void k3d_pattern(const struct k3d_texture *t, const float q[3], const float n[3],
                 float *fac, float *height)
{
    float s = t->scale > 0 ? t->scale : 1;

    *fac = 0;
    *height = 0;

    switch (t->pattern) {
    case K3D_CHECKER: {
        int c = (int)flr(q[0] * s) + (int)flr(q[1] * s) + (int)flr(q[2] * s);

        *fac = (float)(c & 1);
        *height = *fac;
        return;
    }
    case K3D_BRICK:
        brick(t, q, n, fac, height, false);
        return;
    case K3D_SHINGLES:
        brick(t, q, n, fac, height, true);
        return;
    case K3D_NOISE: {
        float at[3] = { q[0] * s, q[1] * s, q[2] * s };
        float n = k3d_noise(at, t->detail);

        *fac = clamp01(0.5f + n * (1 + t->distortion));
        *height = *fac;
        return;
    }
    case K3D_WOOD: {
        /* Rings round the object's own upright, each pushed about by noise
         * so no two are round: the grain of a plank sawn along z. */
        float at[3] = { q[0] * 2, q[1] * 2, q[2] * 0.25f };
        float r = sqrtf(q[0] * q[0] + q[1] * q[1]) * s
                  + t->distortion * k3d_noise(at, t->detail);
        float ring = 0.5f + 0.5f * (float)sin(2 * 3.14159265 * (double)r);

        *fac = ring * ring * ring;
        *height = 1 - *fac;
        return;
    }
    case K3D_MARBLE: {
        float at[3] = { q[0] * s, q[1] * s, q[2] * s };
        float vein = 0.5f + 0.5f * (float)sin((double)(q[0] * s * 1.5f
                                        + t->distortion * 6 * k3d_noise(at, t->detail)));

        *fac = (float)pow((double)vein, 6.0);
        *height = 0;
        return;
    }
    default:
        return;
    }
}
