/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The 3D Kit's footprints made solid (`k3d_extrude.c`, `docs/flight.md`
 * F6, buildings B2), on the host.
 *
 * Held to what a person can check on paper: a roof's triangles add up to
 * the footprint's area, holes taken out; every roof triangle faces up and
 * every wall faces out of the solid, its stored normal agreeing with its
 * winding; each roof triangle lies inside the footprint and outside its
 * holes; a ring given either way round, with its first point repeated or
 * not, comes out the same; and what is not a footprint is refused with
 * nothing appended.
 *
 *   test_extrude          the checks
 *   test_extrude --bad    the control: a roof triangle wound down, which
 *                         the checks must catch
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "k3d_extrude.h"

static int checks, failures;
static int bad;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static void sub(const float *a, const float *b, double *o)
{
    o[0] = (double)a[0] - b[0], o[1] = (double)a[1] - b[1], o[2] = (double)a[2] - b[2];
}

static void crossv(const double *a, const double *b, double *o)
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

/* Whether the point is inside the ring (x, z pairs), by crossings. */
static int in_ring(const double *xz, unsigned n, double x, double z)
{
    int in = 0;

    for (unsigned i = 0, j = n - 1; i < n; j = i++) {
        double xi = xz[2 * i], zi = xz[2 * i + 1], xj = xz[2 * j], zj = xz[2 * j + 1];

        if (((zi > z) != (zj > z)) && x < (xj - xi) * (z - zi) / (zj - zi) + xi) in = !in;
    }
    return in;
}

/* The roof's area, and how many of its triangles face down or disagree
 * with their stored normal; and for walls the same, and whether each
 * faces away from `inside_x`, `inside_z` - a point inside the solid. */
struct sums {
    double roof_area;
    unsigned roof_down, roof_tris, wall_tris, wall_wrong, wall_in;
    unsigned outside, in_hole;
};

static struct sums measure(const struct k3d_extruded *e, size_t roof_from, size_t wall_from,
                           const double *outer, unsigned nouter,
                           const double *hole, unsigned nhole)
{
    struct sums s = { 0 };

    for (size_t i = roof_from; i + 2 < e->nroof_index; i += 3) {
        const struct k3d_extrude_vertex *a = &e->roof[e->roof_index[i]];
        const struct k3d_extrude_vertex *b = &e->roof[e->roof_index[i + 1]];
        const struct k3d_extrude_vertex *c = &e->roof[e->roof_index[i + 2]];
        double u[3], v[3], n[3], cx, cz;

        sub(b->position, a->position, u);
        sub(c->position, a->position, v);
        crossv(u, v, n);
        s.roof_tris++;
        s.roof_area += fabs(n[1]) / 2;
        if (n[1] <= 0 || a->normal[1] < 0.99f) s.roof_down++;
        cx = (a->position[0] + b->position[0] + c->position[0]) / 3.0;
        cz = (a->position[2] + b->position[2] + c->position[2]) / 3.0;
        if (outer && !in_ring(outer, nouter, cx, cz)) s.outside++;
        if (hole && in_ring(hole, nhole, cx, cz)) s.in_hole++;
    }
    for (size_t i = wall_from; i < e->nwall_index; i += 3) {
        const struct k3d_extrude_vertex *a = &e->wall[e->wall_index[i]];
        const struct k3d_extrude_vertex *b = &e->wall[e->wall_index[i + 1]];
        const struct k3d_extrude_vertex *c = &e->wall[e->wall_index[i + 2]];
        double u[3], v[3], n[3], len, dot, cx, cz, px, pz;

        sub(b->position, a->position, u);
        sub(c->position, a->position, v);
        crossv(u, v, n);
        len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        s.wall_tris++;
        dot = (n[0] * a->normal[0] + n[1] * a->normal[1] + n[2] * a->normal[2]) / len;
        if (dot < 0.99) s.wall_wrong++;
        /* A step from the wall's middle along its normal must leave the
         * solid: outside the outer ring, or into a hole. */
        cx = (a->position[0] + b->position[0] + c->position[0]) / 3.0;
        cz = (a->position[2] + b->position[2] + c->position[2]) / 3.0;
        px = cx + 0.01 * a->normal[0];
        pz = cz + 0.01 * a->normal[2];
        if (outer && in_ring(outer, nouter, px, pz) && !(hole && in_ring(hole, nhole, px, pz))) {
            s.wall_in++;
        }
    }
    return s;
}

static void spoil(struct k3d_extruded *e)
{
    uint32_t t;

    if (!bad || e->nroof_index < 3) return;
    t = e->roof_index[1], e->roof_index[1] = e->roof_index[2], e->roof_index[2] = t;
}

int main(int argc, char **argv)
{
    struct k3d_extruded e;
    char what[160];

    bad = argc > 1 && strcmp(argv[1], "--bad") == 0;
    memset(&e, 0, sizeof(e));

    /* A square of 10 with a hole of 4: a courtyard. Area 100 - 16. */
    {
        static const double outer[] = { 0, 0, 10, 0, 10, 10, 0, 10 };
        static const double hole[] = { 3, 3, 3, 7, 7, 7, 7, 3 };
        struct k3d_ring rings[] = { { outer, 4 }, { hole, 4 } };
        struct sums s;

        check(k3d_extrude(&e, rings, 2, 0, 0, 0, 12), "a courtyard is a footprint");
        spoil(&e);
        s = measure(&e, 0, 0, outer, 4, hole, 4);
        snprintf(what, sizeof(what), "the courtyard's roof is 84 m2: %.3f", s.roof_area);
        check(fabs(s.roof_area - 84) < 1e-6, what);
        snprintf(what, sizeof(what), "every roof triangle faces up: %u of %u down", s.roof_down,
                 s.roof_tris);
        check(s.roof_down == 0, what);
        snprintf(what, sizeof(what), "eight walls, sixteen triangles: %u", s.wall_tris);
        check(s.wall_tris == 16, what);
        snprintf(what, sizeof(what), "every wall's winding agrees with its normal: %u wrong",
                 s.wall_wrong);
        check(s.wall_wrong == 0, what);
        snprintf(what, sizeof(what), "every wall faces out of the solid: %u face in", s.wall_in);
        check(s.wall_in == 0, what);
        snprintf(what, sizeof(what), "no roof over the courtyard: %u triangles in the hole",
                 s.in_hole);
        check(s.in_hole == 0 && s.outside == 0, what);
        snprintf(what, sizeof(what), "the courtyard's roof is eight triangles: %zu",
                 e.nroof_index / 3);
        check(e.nroof_index / 3 == 8, what);
    }

    /* An L, given clockwise with its first point repeated: area 3 * 1 + 1 * 2. */
    {
        static const double l[] = { 0, 0, 0, 3, 1, 3, 1, 1, 3, 1, 3, 0, 0, 0 };
        static const double ccw[] = { 0, 0, 3, 0, 3, 1, 1, 1, 1, 3, 0, 3 };
        struct k3d_ring ring = { l, 7 };
        size_t r0 = e.nroof_index, w0 = e.nwall_index;
        struct sums s;

        check(k3d_extrude(&e, &ring, 1, 100, 200, 2, 5), "an L, wound the other way, is one");
        s = measure(&e, r0, w0, NULL, 0, NULL, 0);
        snprintf(what, sizeof(what), "the L's roof is 5 m2: %.3f", s.roof_area);
        check(fabs(s.roof_area - 5) < 1e-6, what);
        check(s.roof_down == 0 && s.wall_wrong == 0, "the L's roof faces up and its walls agree");
        snprintf(what, sizeof(what), "the L has six walls: %u triangles", s.wall_tris);
        check(s.wall_tris == 12, what);
        snprintf(what, sizeof(what), "the L's roof is four triangles: %zu",
                 (e.nroof_index - r0) / 3);
        check((e.nroof_index - r0) / 3 == 4, what);
        {
            /* Its triangles inside the L, which is offset by (100, 200). */
            double moved[12];
            unsigned outside = 0, wall_in;

            for (unsigned i = 0; i < 6; i++) {
                moved[2 * i] = ccw[2 * i] + 100, moved[2 * i + 1] = ccw[2 * i + 1] + 200;
            }
            s = measure(&e, r0, w0, moved, 6, NULL, 0);
            outside = s.outside, wall_in = s.wall_in;
            snprintf(what, sizeof(what), "the L's roof is inside the L: %u outside", outside);
            check(outside == 0, what);
            snprintf(what, sizeof(what), "the L's walls face out: %u in", wall_in);
            check(wall_in == 0, what);
        }
        check(e.roof[e.roof_index[r0]].position[1] == 5.0f &&
              e.wall[e.wall_index[w0]].position[1] >= 2.0f, "the L stands from 2 m to 5 m");
        check(e.roof[e.roof_index[r0]].uv[0] == e.roof[e.roof_index[r0]].position[0] &&
              e.roof[e.roof_index[r0]].uv[1] == e.roof[e.roof_index[r0]].position[2],
              "a roof's (u, v) is its point's own x and z");
    }

    /* A building with two holes and a point on a straight edge. */
    {
        static const double outer[] = { 0, 0, 5, 0, 20, 0, 20, 10, 0, 10 };
        static const double h1[] = { 2, 2, 6, 2, 6, 8, 2, 8 };
        static const double h2[] = { 12, 3, 16, 3, 16, 6, 12, 6 };
        struct k3d_ring rings[] = { { outer, 5 }, { h1, 4 }, { h2, 4 } };
        size_t r0 = e.nroof_index;
        struct sums s;

        check(k3d_extrude(&e, rings, 3, 0, 0, 0, 30), "two courtyards are a footprint");
        s = measure(&e, r0, e.nwall_index, NULL, 0, NULL, 0);
        snprintf(what, sizeof(what), "its roof is 200 - 24 - 12 = 164 m2: %.3f", s.roof_area);
        check(fabs(s.roof_area - 164) < 1e-6, what);
        check(s.roof_down == 0, "its roof faces up");
    }

    /* Refusals: nothing appended. */
    {
        static const double line[] = { 0, 0, 5, 0, 10, 0 };
        static const double two[] = { 0, 0, 1, 1 };
        static const double sq[] = { 0, 0, 1, 0, 1, 1, 0, 1 };
        struct k3d_ring flat = { line, 3 }, few = { two, 2 }, ok = { sq, 4 };
        size_t nr = e.nroof, nri = e.nroof_index, nw = e.nwall, nwi = e.nwall_index;
        double *many = malloc(sizeof(double) * 2 * (K3D_EXTRUDE_MOST + 1));
        struct k3d_ring huge = { many, K3D_EXTRUDE_MOST + 1 };

        for (unsigned i = 0; i <= K3D_EXTRUDE_MOST; i++) {
            double a = 2 * M_PI * i / (K3D_EXTRUDE_MOST + 1);

            many[2 * i] = cos(a) * 50, many[2 * i + 1] = sin(a) * 50;
        }
        check(!k3d_extrude(&e, &flat, 1, 0, 0, 0, 3), "points on a line are refused");
        check(!k3d_extrude(&e, &few, 1, 0, 0, 0, 3), "two points are refused");
        check(!k3d_extrude(&e, &ok, 1, 0, 0, 3, 3), "no height is refused");
        check(!k3d_extrude(&e, &huge, 1, 0, 0, 0, 3), "too many points are refused");
        check(e.nroof == nr && e.nroof_index == nri && e.nwall == nw && e.nwall_index == nwi,
              "a refusal appends nothing");
        free(many);
    }

    /* A thousand-point circle: area near pi r^2, every triangle up. */
    {
        static double circle[2 * 1000];
        struct k3d_ring ring = { circle, 1000 };
        size_t r0 = e.nroof_index;
        double polygon = 0.5 * 1000 * 30 * 30 * sin(2 * M_PI / 1000);
        struct sums s;

        for (unsigned i = 0; i < 1000; i++) {
            circle[2 * i] = 500 + 30 * cos(2 * M_PI * i / 1000);
            circle[2 * i + 1] = 500 + 30 * sin(2 * M_PI * i / 1000);
        }
        check(k3d_extrude(&e, &ring, 1, 0, 0, 0, 40), "a round tower is a footprint");
        s = measure(&e, r0, e.nwall_index, NULL, 0, NULL, 0);
        snprintf(what, sizeof(what), "the tower's roof is its polygon's area %.3f: %.3f", polygon,
                 s.roof_area);
        check(fabs(s.roof_area - polygon) < 1e-3, what);
        check(s.roof_down == 0 && s.roof_tris == 998, "the tower's roof is 998 triangles, up");
    }

    k3d_extruded_free(&e);
    printf("%s: %d of %d checks\n", failures ? "FAIL" : "PASS", checks - failures, checks);
    return failures ? 1 : 0;
}
