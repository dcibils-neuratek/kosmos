/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A footprint made solid (`k3d_extrude.h`): its rings cleaned and wound
 * one way, its walls stood up, and its roof clipped into triangles.
 *
 * **The roof** is ear clipping (Meisters' theorem: a simple polygon of
 * n > 3 points always has two ears). A hole is made part of the outer ring
 * first by a bridge - two edges, there and back, from the hole's
 * easternmost point to the nearest point of the ring it can see - the
 * holes taken east to west, so a bridge never crosses a hole still to be
 * joined. Quadratic in the points, which a building's dozen is nothing
 * to; `K3D_EXTRUDE_MOST` refuses the footprint that would not be.
 *
 * Every sum is in double: a tile's coordinates are metres from its corner,
 * up to some thousands, and an ear's area is a difference of products of
 * them.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "k3d_extrude.h"

struct pt { double x, z; };

static bool grow(void **p, size_t *cap, size_t need, size_t size)
{
    size_t c = *cap ? *cap : 256;
    void *n;

    if (need <= *cap) return true;
    while (c < need) c *= 2;
    if ((n = realloc(*p, c * size)) == NULL) return false;
    *p = n;
    *cap = c;
    return true;
}

/* Twice the signed area: positive for a ring wound counter-clockwise with
 * x to the right and z up the page. */
static double area2(const struct pt *p, unsigned n)
{
    double a = 0;

    for (unsigned i = 0; i < n; i++) {
        const struct pt *b = &p[(i + 1) % n];

        a += p[i].x * b->z - b->x * p[i].z;
    }
    return a;
}

static double cross(struct pt a, struct pt b, struct pt c)
{
    return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

/* A ring copied into `out`, offset, without repeated points or its first
 * repeated last: how many points are left. */
static unsigned cleaned(const struct k3d_ring *r, double ox, double oz, struct pt *out)
{
    unsigned n = 0;

    for (unsigned i = 0; i < r->n; i++) {
        struct pt p = { r->xz[2 * i] + ox, r->xz[2 * i + 1] + oz };

        if (!isfinite(p.x) || !isfinite(p.z)) return 0;
        if (n > 0 && fabs(p.x - out[n - 1].x) < 1e-6 && fabs(p.z - out[n - 1].z) < 1e-6) continue;
        out[n++] = p;
    }
    while (n > 1 && fabs(out[0].x - out[n - 1].x) < 1e-6 && fabs(out[0].z - out[n - 1].z) < 1e-6) {
        n--;
    }
    return n;
}

static void reverse(struct pt *p, unsigned n)
{
    for (unsigned i = 0; i < n / 2; i++) {
        struct pt t = p[i];

        p[i] = p[n - 1 - i];
        p[n - 1 - i] = t;
    }
}

/* Whether segments ab and cd cross at a point inside both - touching at an
 * end is not crossing, which a bridge to a vertex must be allowed. */
static bool crosses(struct pt a, struct pt b, struct pt c, struct pt d)
{
    double d1 = cross(a, b, c), d2 = cross(a, b, d), d3 = cross(c, d, a), d4 = cross(c, d, b);

    return ((d1 > 1e-12 && d2 < -1e-12) || (d1 < -1e-12 && d2 > 1e-12)) &&
           ((d3 > 1e-12 && d4 < -1e-12) || (d3 < -1e-12 && d4 > 1e-12));
}

static bool same(struct pt a, struct pt b)
{
    return fabs(a.x - b.x) < 1e-9 && fabs(a.z - b.z) < 1e-9;
}

/* p strictly inside the triangle abc, wound counter-clockwise. */
static bool inside(struct pt p, struct pt a, struct pt b, struct pt c)
{
    return cross(a, b, p) > 1e-12 && cross(b, c, p) > 1e-12 && cross(c, a, p) > 1e-12;
}

static bool add_vertex(struct k3d_extruded *o, bool roof, float x, float y, float z,
                       float nx, float ny, float nz, float u, float v)
{
    struct k3d_extrude_vertex **arr = roof ? &o->roof : &o->wall;
    size_t *n = roof ? &o->nroof : &o->nwall, *cap = roof ? &o->cap_roof : &o->cap_wall;
    struct k3d_extrude_vertex *e;

    if (!grow((void **)arr, cap, *n + 1, sizeof(**arr))) return false;
    e = &(*arr)[(*n)++];
    e->position[0] = x, e->position[1] = y, e->position[2] = z;
    e->normal[0] = nx, e->normal[1] = ny, e->normal[2] = nz;
    e->uv[0] = u, e->uv[1] = v;
    return true;
}

static bool add_index(struct k3d_extruded *o, bool roof, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t **arr = roof ? &o->roof_index : &o->wall_index;
    size_t *n = roof ? &o->nroof_index : &o->nwall_index;
    size_t *cap = roof ? &o->cap_roof_index : &o->cap_wall_index;

    if (!grow((void **)arr, cap, *n + 3, sizeof(**arr))) return false;
    (*arr)[(*n)++] = a, (*arr)[(*n)++] = b, (*arr)[(*n)++] = c;
    return true;
}

/* A ring's walls: a quad an edge, its normal out of the solid - to the
 * right of the way an outer ring (counter-clockwise) and a hole (clockwise)
 * each go, which for both is away from what is built. */
static bool walls(struct k3d_extruded *o, const struct pt *p, unsigned n, float base, float top)
{
    double along = 0;

    for (unsigned i = 0; i < n; i++) {
        struct pt a = p[i], b = p[(i + 1) % n];
        double dx = b.x - a.x, dz = b.z - a.z, len = sqrt(dx * dx + dz * dz);
        float nx = (float)(dz / len), nz = (float)(-dx / len);
        uint32_t k = (uint32_t)o->nwall;

        if (len < 1e-9) continue;
        if (!add_vertex(o, false, (float)a.x, base, (float)a.z, nx, 0, nz, (float)along, base) ||
            !add_vertex(o, false, (float)b.x, base, (float)b.z, nx, 0, nz, (float)(along + len), base) ||
            !add_vertex(o, false, (float)b.x, top, (float)b.z, nx, 0, nz, (float)(along + len), top) ||
            !add_vertex(o, false, (float)a.x, top, (float)a.z, nx, 0, nz, (float)along, top)) {
            return false;
        }
        /* Wound so the triangle's own normal is the wall's: (b-a)x(c-a)
         * for a, b, top b is (dz, 0, -dx) times the height, out of the
         * solid as the normal is. */
        if (!add_index(o, false, k, k + 2, k + 1) || !add_index(o, false, k, k + 3, k + 2)) {
            return false;
        }
        along += len;
    }
    return true;
}

/* The hole `h` joined to the polygon `poly` (n points, room for n + m + 2)
 * by a bridge from its easternmost point to the nearest point of the
 * polygon it can see, past every edge of the polygon and of the holes
 * still to come (`rest`, `nrest`). The new length, or 0 when nothing can
 * be seen - a hole touching its ring - and the hole is left out. */
static unsigned bridge(struct pt *poly, unsigned n, const struct pt *h, unsigned m,
                       const struct pt *const *rest, const unsigned *nrest, unsigned others)
{
    unsigned east = 0, best = n;
    double best_d = INFINITY;

    for (unsigned i = 1; i < m; i++) {
        if (h[i].x > h[east].x) east = i;
    }
    for (unsigned v = 0; v < n; v++) {
        struct pt a = h[east], b = poly[v];
        double d = (b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z);
        bool clear = true;

        if (d >= best_d) continue;
        for (unsigned e = 0; e < n && clear; e++) {
            clear = !crosses(a, b, poly[e], poly[(e + 1) % n]);
        }
        for (unsigned e = 0; e < m && clear; e++) {
            clear = !crosses(a, b, h[e], h[(e + 1) % m]);
        }
        for (unsigned o = 0; o < others && clear; o++) {
            for (unsigned e = 0; e < nrest[o] && clear; e++) {
                clear = !crosses(a, b, rest[o][e], rest[o][(e + 1) % nrest[o]]);
            }
        }
        if (clear) best = v, best_d = d;
    }
    if (best == n) return 0;
    /* poly[0..best], then the hole from `east` round to `east` again, then
     * poly[best] again and the rest of the polygon. */
    memmove(&poly[best + m + 3], &poly[best + 1], (n - best - 1) * sizeof(*poly));
    for (unsigned i = 0; i <= m; i++) {
        poly[best + 1 + i] = h[(east + i) % m];
    }
    poly[best + m + 2] = poly[best];
    return n + m + 2;
}

/* The roof: the polygon `poly` (counter-clockwise, its holes bridged in)
 * clipped into triangles at `top`, each facing up. */
static bool roof(struct k3d_extruded *o, const struct pt *poly, unsigned n, float top)
{
    unsigned *left = malloc(n * sizeof(*left));
    uint32_t first = (uint32_t)o->nroof;
    unsigned count = n, guard = 0;

    if (left == NULL) return false;
    for (unsigned i = 0; i < n; i++) {
        left[i] = i;
        if (!add_vertex(o, true, (float)poly[i].x, top, (float)poly[i].z, 0, 1, 0,
                        (float)poly[i].x, (float)poly[i].z)) {
            free(left);
            return false;
        }
    }
    while (count > 3 && guard < n * n + 16) {
        bool clipped = false;

        for (unsigned k = 0; k < count && !clipped; k++) {
            unsigned ip = left[(k + count - 1) % count], ii = left[k], in = left[(k + 1) % count];
            struct pt a = poly[ip], b = poly[ii], c = poly[in];
            bool ear = cross(a, b, c) > 1e-12;

            for (unsigned j = 0; j < count && ear; j++) {
                struct pt q = poly[left[j]];

                if (left[j] == ip || left[j] == ii || left[j] == in) continue;
                if (same(q, a) || same(q, b) || same(q, c)) continue;
                ear = !inside(q, a, b, c);
            }
            if (ear) {
                /* a, c, b: facing up (y) for a counter-clockwise ear. */
                if (!add_index(o, true, first + ip, first + in, first + ii)) {
                    free(left);
                    return false;
                }
                memmove(&left[k], &left[k + 1], (count - k - 1) * sizeof(*left));
                count--;
                clipped = true;
            }
        }
        if (!clipped) {
            /* No ear: a point on a straight line, or a polygon that touches
             * itself. The flattest corner goes, with no triangle. */
            unsigned flattest = 0;
            double least = INFINITY;

            for (unsigned k = 0; k < count; k++) {
                double c = fabs(cross(poly[left[(k + count - 1) % count]], poly[left[k]],
                                      poly[left[(k + 1) % count]]));

                if (c < least) least = c, flattest = k;
            }
            memmove(&left[flattest], &left[flattest + 1], (count - flattest - 1) * sizeof(*left));
            count--;
        }
        guard++;
    }
    if (count == 3 && cross(poly[left[0]], poly[left[1]], poly[left[2]]) > 1e-12) {
        if (!add_index(o, true, first + left[0], first + left[2], first + left[1])) {
            free(left);
            return false;
        }
    }
    free(left);
    return true;
}

bool k3d_extrude(struct k3d_extruded *out, const struct k3d_ring *rings, unsigned nrings,
                 double ox, double oz, float base, float top)
{
    unsigned total = 0, outer_n, used = 0, *sizes = NULL, joined;
    struct pt *pts = NULL, *poly = NULL, **ring = NULL;
    size_t keep_roof = out->nroof, keep_roof_index = out->nroof_index;
    size_t keep_wall = out->nwall, keep_wall_index = out->nwall_index;
    bool ok = false;

    if (nrings == 0 || rings[0].n < 3 || !(top > base)) return false;
    for (unsigned r = 0; r < nrings; r++) {
        total += rings[r].n;
        if (total > K3D_EXTRUDE_MOST) return false;
    }
    pts = malloc(total * sizeof(*pts));
    poly = malloc((total + 3 * nrings) * sizeof(*poly));
    ring = malloc(nrings * sizeof(*ring));
    sizes = malloc(nrings * sizeof(*sizes));
    if (pts == NULL || poly == NULL || ring == NULL || sizes == NULL) goto done;

    /* The rings, cleaned and wound: the outer counter-clockwise, the
     * holes the other way; a ring with no area is no ring. */
    for (unsigned r = 0, at = 0; r < nrings; r++) {
        unsigned n = cleaned(&rings[r], ox, oz, &pts[at]);
        double a = n >= 3 ? area2(&pts[at], n) : 0;

        if (fabs(a) < 1e-9) {
            if (r == 0) goto done;
            continue;
        }
        if ((r == 0) != (a > 0)) reverse(&pts[at], n);
        ring[used] = &pts[at];
        sizes[used++] = n;
        at += n;
    }

    for (unsigned r = 0; r < used; r++) {
        if (!walls(out, ring[r], sizes[r], base, top)) goto done;
    }

    /* The holes east to west, each bridged into the polygon. */
    outer_n = sizes[0];
    memcpy(poly, ring[0], outer_n * sizeof(*poly));
    joined = outer_n;
    for (unsigned r = 1; r < used; r++) {
        for (unsigned s = r + 1; s < used; s++) {
            double er = -INFINITY, es = -INFINITY;

            for (unsigned i = 0; i < sizes[r]; i++) er = fmax(er, ring[r][i].x);
            for (unsigned i = 0; i < sizes[s]; i++) es = fmax(es, ring[s][i].x);
            if (es > er) {
                struct pt *t = ring[r];
                unsigned tn = sizes[r];

                ring[r] = ring[s], sizes[r] = sizes[s];
                ring[s] = t, sizes[s] = tn;
            }
        }
    }
    for (unsigned r = 1; r < used; r++) {
        unsigned grown = bridge(poly, joined, ring[r], sizes[r],
                                (const struct pt *const *)&ring[r + 1], &sizes[r + 1],
                                used - r - 1);

        if (grown) joined = grown;
    }
    ok = roof(out, poly, joined, top);

done:
    if (!ok) {
        out->nroof = keep_roof, out->nroof_index = keep_roof_index;
        out->nwall = keep_wall, out->nwall_index = keep_wall_index;
    }
    free(pts);
    free(poly);
    free(ring);
    free(sizes);
    return ok;
}

void k3d_extruded_free(struct k3d_extruded *out)
{
    free(out->roof);
    free(out->roof_index);
    free(out->wall);
    free(out->wall_index);
    memset(out, 0, sizeof(*out));
}
