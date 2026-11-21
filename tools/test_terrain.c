/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The 3D Kit's terrain, on the Mac (`user/kits/3d/k3d_terrain.c`,
 * `docs/flight.md` F4a): the frame against distances worked out by hand,
 * the chunks chosen for an eye against what a quadtree must be - the
 * roots covered once, with no gap and no overlap, deeper nearer, and
 * still covered when there is room for few - and a chunk's mesh against a
 * ground whose every height and slope is known: a tilted plane and a hill.
 */

#include "k3d_terrain.h"
#include "k3d_island.h"
#include "mercator.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  %s\n", what);
    }
}

#define DEG (3.14159265358979323846 / 180.0)

/* ------------------------------------------------------------ the frame */

static void frame(void)
{
    struct k3d_place p;
    double x, z, mx, my;
    char what[160];

    /* Montevideo's Palacio Salvo. A thousandth of a degree of longitude is
     * 111.32 cos(lat) metres there; of latitude, the Mercator scale at the
     * same latitude makes it 111.32 / cos(lat) of the world's metres... in
     * the frame, which keeps shapes, the same as the ground's 111.32. */
    double lon = -56.1990, lat = -34.9064;

    k3d_place_at(&p, lon, lat);
    map_project(lon, lat, &mx, &my);
    k3d_place_world(&p, mx, my, &x, &z);
    check(x == 0.0 && z == 0.0, "the origin is not at nought");

    map_project(lon + 0.01, lat, &mx, &my);
    k3d_place_world(&p, mx, my, &x, &z);
    snprintf(what, sizeof(what), "a hundredth of a degree east is %.3f m east, %.3f south", x, z);
    check(fabs(x - 1113.19 * cos(lat * DEG)) < 0.5 && fabs(z) < 1e-6, what);

    /* North is -z, and a hundredth of a degree of latitude is 1113 m of
     * ground, which the frame keeps to within the scale's change over it. */
    map_project(lon, lat + 0.01, &mx, &my);
    k3d_place_world(&p, mx, my, &x, &z);
    snprintf(what, sizeof(what), "a hundredth of a degree north is %.3f m south", z);
    check(z < 0 && fabs(-z - 1110.9) < 3.0 && fabs(x) < 1e-6, what);

    /* A tile's side is the Map Kit's metres a pixel times its pixels. */
    double want = map_metres_per_pixel(lat, 14, 256) * 256;
    snprintf(what, sizeof(what), "a zoom 14 tile's side is %.3f m, not %.3f",
             k3d_tile_side(&p, 14), want);
    check(fabs(k3d_tile_side(&p, 14) - want) < 1e-6 * want, what);
}

/* ----------------------------------------------------------- the chunks */

/* The chosen chunks, each marked into a grid of the deepest zoom's tiles
 * over the roots: every cell once is coverage with no gap and no overlap. */
static int covers(const struct k3d_tile *t, unsigned n, const struct k3d_choice *c,
                  uint32_t rx, uint32_t ry, unsigned roots_side, char *why, size_t why_n)
{
    unsigned scale = 1u << (c->deepest - c->root), side = roots_side * scale;
    unsigned char *seen = calloc((size_t)side * side, 1);
    int ok = 1;

    for (unsigned k = 0; k < n && ok; k++) {
        if (t[k].z < c->root || t[k].z > c->deepest) {
            snprintf(why, why_n, "a chunk at zoom %u, outside %u to %u", t[k].z, c->root,
                     c->deepest);
            ok = 0;
            break;
        }
        unsigned s = 1u << (c->deepest - t[k].z);
        int64_t x0 = (int64_t)t[k].x * s - (int64_t)rx * scale;
        int64_t y0 = (int64_t)t[k].y * s - (int64_t)ry * scale;

        for (unsigned y = 0; y < s && ok; y++) {
            for (unsigned x = 0; x < s && ok; x++) {
                int64_t gx = x0 + x, gy = y0 + y;

                if (gx < 0 || gy < 0 || gx >= side || gy >= side) {
                    snprintf(why, why_n, "chunk %u/%u/%u is outside the roots", t[k].z,
                             t[k].x, t[k].y);
                    ok = 0;
                } else if (seen[gy * side + gx]++) {
                    snprintf(why, why_n, "chunk %u/%u/%u overlaps another", t[k].z, t[k].x,
                             t[k].y);
                    ok = 0;
                }
            }
        }
    }
    for (size_t i = 0; i < (size_t)side * side && ok; i++) {
        if (!seen[i]) {
            snprintf(why, why_n, "a gap at %zu, %zu of the deepest zoom", i % side, i / side);
            ok = 0;
        }
    }
    free(seen);
    return ok;
}

/* The eye's distance to a chunk, worked out here again: to its square on
 * the ground, the eye's height counted from nought. */
static double away(const struct k3d_place *p, struct k3d_tile t, const double eye[3])
{
    double n = ldexp(1.0, t.z), x0, z0, x1, z1;

    k3d_place_world(p, t.x / n, t.y / n, &x0, &z0);
    k3d_place_world(p, (t.x + 1) / n, (t.y + 1) / n, &x1, &z1);
    double dx = fmax(fmax(x0 - eye[0], eye[0] - x1), 0.0);
    double dz = fmax(fmax(z0 - eye[2], eye[2] - z1), 0.0);
    return sqrt(dx * dx + dz * dz + eye[1] * eye[1]);
}

static void chunks(void)
{
    static struct k3d_tile t[4096];
    struct k3d_place p;
    struct k3d_choice c = { .root = 11, .deepest = 16, .radius = 2, .split = 2.0 };
    double eye[3] = { 120.0, 400.0, -300.0 };       /* 400 m up, near the origin */
    uint32_t ex, ey;
    char what[200], why[120];
    double mx, my;

    k3d_place_at(&p, -56.1990, -34.9064);
    mx = p.ox + eye[0] / p.metres;
    my = p.oy + eye[2] / p.metres;
    map_tile_of(0, 0, 0, &ex, &ey);                 /* (unused: worked out below) */
    ex = (uint32_t)floor(mx * 2048);
    ey = (uint32_t)floor(my * 2048);

    unsigned n = k3d_terrain_choose(&p, &c, eye, t, 4096);
    snprintf(what, sizeof(what), "%u chunks chosen with room for 4096", n);
    check(n > 25 && n < 4096, what);
    check(covers(t, n, &c, ex - 2, ey - 2, 5, why, sizeof(why)), why);

    /* Under the eye, the deepest; and each chunk is split exactly when the
     * eye is nearer than `split` sides - no chunk that should have been
     * split was left, and none was split that should not have been. */
    int under = 0, unsplit = 0, oversplit = 0;
    for (unsigned k = 0; k < n; k++) {
        double side = k3d_tile_side(&p, t[k].z);
        double n2 = ldexp(1.0, t[k].z), x0, z0, x1, z1;

        k3d_place_world(&p, t[k].x / n2, t[k].y / n2, &x0, &z0);
        k3d_place_world(&p, (t[k].x + 1) / n2, (t[k].y + 1) / n2, &x1, &z1);
        if (eye[0] >= x0 && eye[0] < x1 && eye[2] >= z0 && eye[2] < z1) {
            under = t[k].z;
        }
        if (t[k].z < c.deepest && away(&p, t[k], eye) < c.split * side) {
            unsplit++;
        }
        if (t[k].z > c.root) {
            struct k3d_tile parent = { t[k].x / 2, t[k].y / 2, (uint8_t)(t[k].z - 1) };

            if (away(&p, parent, eye) >= c.split * k3d_tile_side(&p, parent.z)) {
                oversplit++;
            }
        }
    }
    snprintf(what, sizeof(what), "the chunk under the eye is at zoom %d, not %u", under,
             c.deepest);
    check(under == (int)c.deepest, what);
    snprintf(what, sizeof(what), "%d chunks near enough to split were not", unsplit);
    check(unsplit == 0, what);
    snprintf(what, sizeof(what), "%d chunks were split from a parent too far to", oversplit);
    check(oversplit == 0, what);

    /* Higher, coarser: from 5 km up nothing is near enough for zoom 16. */
    double high[3] = { 120.0, 5000.0, -300.0 };
    unsigned m = k3d_terrain_choose(&p, &c, high, t, 4096);
    unsigned deepest = 0;
    for (unsigned k = 0; k < m; k++) {
        deepest = t[k].z > deepest ? t[k].z : deepest;
    }
    snprintf(what, sizeof(what), "from 5 km up, %u chunks to %u from 400 m, deepest %u", m, n,
             deepest);
    check(m < n && deepest < c.deepest, what);
    check(covers(t, m, &c, ex - 2, ey - 2, 5, why, sizeof(why)), why);

    /* Little room: still covered, never over the room. */
    unsigned rooms[] = { 25, 26, 28, 40, 100 };
    for (unsigned r = 0; r < sizeof(rooms) / sizeof(rooms[0]); r++) {
        unsigned got = k3d_terrain_choose(&p, &c, eye, t, rooms[r]);

        snprintf(what, sizeof(what), "with room for %u, %u chunks", rooms[r], got);
        check(got <= rooms[r] && got + 3 > rooms[r], what);
        snprintf(what, sizeof(what), "with room for %u: ", rooms[r]);
        check(covers(t, got, &c, ex - 2, ey - 2, 5, why, sizeof(why)),
              strncat(what, why, sizeof(what) - strlen(what) - 1));
    }
    check(k3d_terrain_choose(&p, &c, eye, t, 24) == 0, "room for fewer than the roots was not 0");

    /* At the world's edge the roots outside it are left out. */
    struct k3d_place corner;
    k3d_place_at(&corner, -179.99, 85.0);
    double at[3] = { 0, 300, 0 };
    unsigned e = k3d_terrain_choose(&corner, &c, at, t, 4096);
    int inside = e > 0;
    for (unsigned k = 0; k < e; k++) {
        uint32_t s = 1u << t[k].z;
        inside &= t[k].x < s && t[k].y < s;
    }
    snprintf(what, sizeof(what), "at the world's corner, %u chunks, all inside it", e);
    check(inside, what);
}

/* ------------------------------------------------------------ the mesh */

/* The ground, in the frame's metres: a plane rising 0.1 east and falling
 * 0.05 south, from 20 m at the origin, or a round hill. */
struct ground {
    const struct k3d_place *p;
    int hill;
};

static double ground_at(const struct ground *g, double x, double z)
{
    if (g->hill) {
        return 80.0 * exp(-(x * x + z * z) / (2.0 * 600.0 * 600.0));
    }
    return 20.0 + 0.1 * x - 0.05 * z;
}

static float height(void *context, double mx, double my)
{
    const struct ground *g = context;
    double x, z;

    k3d_place_world(g->p, mx, my, &x, &z);
    return (float)ground_at(g, x, z);
}

static struct k3d_terrain_vertex v[K3D_CHUNK_VERTICES], w[K3D_CHUNK_VERTICES];
static uint32_t idx[K3D_CHUNK_INDICES], idx2[K3D_CHUNK_INDICES];

static void cross(const float *a, const float *b, const float *c, double out[3])
{
    double u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    double s[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };

    out[0] = u[1] * s[2] - u[2] * s[1];
    out[1] = u[2] * s[0] - u[0] * s[2];
    out[2] = u[0] * s[1] - u[1] * s[0];
}

static void mesh(void)
{
    struct k3d_place p;
    struct ground plane = { &p, 0 }, hill = { &p, 1 };
    char what[200];
    float low, high;
    uint32_t tx, ty;
    const int S = K3D_CHUNK_SIDE, GRID = S * S;

    k3d_place_at(&p, -56.1990, -34.9064);
    map_tile_of(-56.1990, -34.9064, 15, &tx, &ty);
    struct k3d_tile t = { tx, ty, 15 };
    double side = k3d_tile_side(&p, 15);

    k3d_terrain_chunk(&p, t, height, &plane, 30.0f, v, idx, &low, &high);

    /* Every grid vertex where its sample is, at the plane's height, with
     * the plane's normal; the corners at the tile's corners. */
    double n = ldexp(1.0, 15), x0, z0, x1, z1;
    k3d_place_world(&p, tx / n, ty / n, &x0, &z0);
    k3d_place_world(&p, (tx + 1) / n, (ty + 1) / n, &x1, &z1);
    snprintf(what, sizeof(what), "the corners (%.3f, %.3f) and (%.3f, %.3f), not the tile's",
             v[0].position[0], v[0].position[2], v[GRID - 1].position[0],
             v[GRID - 1].position[2]);
    check(fabs(v[0].position[0] - x0) < 1e-3 && fabs(v[0].position[2] - z0) < 1e-3 &&
          fabs(v[GRID - 1].position[0] - x1) < 1e-3 && fabs(v[GRID - 1].position[2] - z1) < 1e-3,
          what);

    double nx = -0.1, ny = 1.0, nz = 0.05, nl = sqrt(nx * nx + ny * ny + nz * nz);
    int off_height = 0, off_normal = 0, off_place = 0, off_uv = 0;
    for (int j = 0; j < S; j++) {
        for (int i = 0; i < S; i++) {
            const struct k3d_terrain_vertex *a = &v[j * S + i];
            double want_x = x0 + (x1 - x0) * i / (S - 1), want_z = z0 + (z1 - z0) * j / (S - 1);

            off_place += fabs(a->position[0] - want_x) > 1e-3 || fabs(a->position[2] - want_z) > 1e-3;
            off_height += fabs(a->position[1] - ground_at(&plane, a->position[0], a->position[2])) > 1e-3;
            off_normal += fabs(a->normal[0] - nx / nl) > 1e-4 || fabs(a->normal[1] - ny / nl) > 1e-4 ||
                          fabs(a->normal[2] - nz / nl) > 1e-4;
            off_uv += a->uv[0] != (float)i / (S - 1) || a->uv[1] != (float)j / (S - 1);
        }
    }
    snprintf(what, sizeof(what), "of %d samples: %d misplaced, %d at the wrong height, %d with "
             "the wrong normal, %d with the wrong imagery place", GRID, off_place, off_height,
             off_normal, off_uv);
    check(!off_place && !off_height && !off_normal && !off_uv, what);
    snprintf(what, sizeof(what), "lowest %.3f and highest %.3f, not the corners'", low, high);
    check(fabs(low - fmin(fmin(v[0].position[1], v[S - 1].position[1]),
                          fmin(v[GRID - S].position[1], v[GRID - 1].position[1]))) < 1e-3 &&
          fabs(high - fmax(fmax(v[0].position[1], v[S - 1].position[1]),
                           fmax(v[GRID - S].position[1], v[GRID - 1].position[1]))) < 1e-3, what);

    /* Every index in range; the grid's triangles face up and together
     * cover the tile's area once; each skirt's face out and hang 30 m. */
    int range = 1, down = 0, inward = 0, skirt = 0;
    double area = 0.0;
    for (int k = 0; k < K3D_CHUNK_INDICES; k++) {
        range &= idx[k] < K3D_CHUNK_VERTICES;
    }
    check(range, "an index past the chunk's vertices");
    int grid_tris = 2 * (S - 1) * (S - 1);
    for (int k = 0; k < grid_tris; k++) {
        double c[3];

        cross(v[idx[3 * k]].position, v[idx[3 * k + 1]].position, v[idx[3 * k + 2]].position, c);
        down += c[1] <= 0;
        area += c[1] / 2.0;
    }
    snprintf(what, sizeof(what), "%d grid triangles face down; their area %.1f m2 against the "
             "tile's %.1f", down, area, (x1 - x0) * (z1 - z0));
    check(down == 0 && fabs(area - (x1 - x0) * (z1 - z0)) < 1e-3 * area, what);

    static const double out[4][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } };
    for (int k = grid_tris; k < K3D_CHUNK_INDICES / 3; k++) {
        int e = (k - grid_tris) / (2 * (S - 1));
        double c[3];

        cross(v[idx[3 * k]].position, v[idx[3 * k + 1]].position, v[idx[3 * k + 2]].position, c);
        inward += c[0] * out[e][0] + c[2] * out[e][1] <= 0;
    }
    for (int k = GRID; k < K3D_CHUNK_VERTICES; k++) {
        double y = ground_at(&plane, v[k].position[0], v[k].position[2]);

        skirt += fabs(v[k].position[1] - (y - 30.0)) > 1e-3;
    }
    snprintf(what, sizeof(what), "%d skirt triangles face in, %d skirt vertices not 30 m under "
             "the edge", inward, skirt);
    check(!inward && !skirt, what);

    /* Neighbours on a hill: the east chunk's west edge is where this one's
     * east edge is, to the bit, and faces the same way. */
    k3d_terrain_chunk(&p, t, height, &hill, 30.0f, v, idx, NULL, NULL);
    struct k3d_tile east = { tx + 1, ty, 15 };
    k3d_terrain_chunk(&p, east, height, &hill, 30.0f, w, idx2, NULL, NULL);
    int seams = 0;
    for (int j = 0; j < S; j++) {
        /* Where it is and which way it faces; its place on the imagery is
         * each tile's own - 1 across here, nought there. */
        seams += memcmp(v[j * S + (S - 1)].position, w[j * S].position, sizeof(v[0].position)) ||
                 memcmp(v[j * S + (S - 1)].normal, w[j * S].normal, sizeof(v[0].normal));
    }
    snprintf(what, sizeof(what), "%d of the %d samples on a shared edge differ", seams, S);
    check(seams == 0, what);

    /* And the hill's normals are its slope's, to the sampling's accuracy:
     * the central difference over one sample, at zoom 15 15.7 m here. */
    double worst = 0.0;
    double step = side / (S - 1);
    for (int k = 0; k < GRID; k++) {
        double x = v[k].position[0], z = v[k].position[2];
        double gx = (ground_at(&hill, x + 0.01, z) - ground_at(&hill, x - 0.01, z)) / 0.02;
        double gz = (ground_at(&hill, x, z + 0.01) - ground_at(&hill, x, z - 0.01)) / 0.02;
        double l = sqrt(gx * gx + 1 + gz * gz);
        double d = fabs(v[k].normal[0] + gx / l) + fabs(v[k].normal[1] - 1 / l) +
                   fabs(v[k].normal[2] + gz / l);

        worst = d > worst ? d : worst;
    }
    snprintf(what, sizeof(what), "a hill's normal off its slope by %.5f, with samples %.2f m apart",
             worst, step);
    check(worst < 1e-3, what);
}

/* ------------------------------------------------------- the island */

/* The invented island (F5j): land in the middle, the sea all round it,
 * nowhere higher than Flight's start over it at 1,500 feet; a tile's
 * heights and picture the island's own at each sample's centre, so two
 * tiles of different zooms agree where they meet; and its colours what
 * they say - blue sea, sand at the water's edge, green or rock inland. */
static void island(void)
{
    struct k3d_place p;
    char what[200];
    float highest = -1e9f;
    int sand = 0;

    k3d_place_at(&p, -56.1650, -34.9060);
    check(k3d_island_height(0, 0) > 100, "the island's middle is not land over 100 m");
    for (int a = 0; a < 360; a += 15) {
        double s = sin(a * DEG), c = cos(a * DEG);

        snprintf(what, sizeof what, "12 km out at %d degrees is not the sea's floor: %.1f m", a,
                 k3d_island_height(12000 * s, 12000 * c));
        check(k3d_island_height(12000 * s, 12000 * c) < -25, what);
        for (double r = 0; r < 12000; r += 25) {
            float h = k3d_island_height(r * s, r * c);
            uint32_t col = k3d_island_colour(r * s, r * c);

            highest = fmaxf(highest, h);
            if (h >= 0 && h < 5) {
                sand += ((col >> 16) & 255) > 150 && ((col >> 8) & 255) > 140;
            }
        }
    }
    snprintf(what, sizeof what, "the island rises to %.0f m, over the 457 m start", highest);
    check(highest > 150 && highest < 440, what);
    check(sand > 10, "no sand where the island meets the sea");
    uint32_t sea = k3d_island_colour(15000, 0), middle = k3d_island_colour(0, 0);
    check((sea & 255) > ((sea >> 16) & 255) + 30, "the sea is not blue");
    /* Inland is green, or rock where it is high: neither the sea's blue
     * nor the sand's yellow. */
    snprintf(what, sizeof what, "the island's middle, %.0f m, is %08x - not green or rock",
             k3d_island_height(0, 0), middle);
    check((middle & 255) <= ((middle >> 8) & 255) + 8 && ((middle >> 16) & 255) < 170, what);

    /* A tile's samples: the island's height at their centres, the sea's
     * surface at nought - a function of the place alone, so tiles of any
     * zoom agree where they meet. */
    struct k3d_tile t13 = { 2816, 4940, 13 };
    static float a[65 * 65];
    float lo, hi;
    int wrong = 0;

    k3d_island_heights(&p, t13, 65, a, &lo, &hi);
    for (unsigned j = 0; j < 65; j += 8) {
        for (unsigned i = 0; i < 65; i += 8) {
            double x, z, n = ldexp(1.0, 13);

            k3d_place_world(&p, (t13.x + (i + 0.5) / 65) / n, (t13.y + (j + 0.5) / 65) / n, &x, &z);
            wrong += fabsf(a[j * 65 + i] - fmaxf(0.0f, k3d_island_height(x, z))) > 1e-3f;
        }
    }
    check(wrong == 0, "a tile's heights are not the island's at their samples' centres");
    check(lo >= 0 && hi >= lo, "a tile's lowest and highest are not its samples'");
    static uint32_t px[64 * 64];
    k3d_island_picture(&p, t13, 64, px);
    int opaque = 1;
    for (unsigned i = 0; i < 64 * 64; i++) {
        opaque &= (px[i] >> 24) == 255;
    }
    check(opaque, "the island's picture is not opaque");
}

/* A runway's flat (F8): level inside, the tiles' beyond, a smoothstep
 * between that meets both, the heights along it the line between its
 * ends, and a hill's chunk flat where a flat lies over it. */
static float hill_under(void *context, double mx, double my)
{
    (void)context;
    return (float)(100.0 + 4e6 * (mx - 0.3) + 2e6 * (my - 0.6));
}

struct flattened {
    const struct k3d_flat *f;
    unsigned n;
};

static float hill_flattened(void *context, double mx, double my)
{
    const struct flattened *c = context;

    return k3d_flatten(c->f, c->n, mx, my, hill_under(NULL, mx, my));
}

static void flats(void)
{
    double u = 1.0 / 40e6;                          /* about a metre */
    struct k3d_flat f = { 0.3, 0.6, 0.3 + 2000 * u, 0.6, 30 * u, 150 * u, 10.0f, 20.0f };

    check(k3d_flatten(&f, 1, 0.3, 0.6, 500.0f) == 10.0f, "a flat's end is not its height");
    check(fabsf(k3d_flatten(&f, 1, 0.3 + 1000 * u, 0.6 + 29 * u, 500.0f) - 15.0f) < 1e-3f,
          "a flat's middle, inside its half-width, is not halfway between its ends");
    check(k3d_flatten(&f, 1, 0.3 + 1000 * u, 0.6 + 181 * u, 500.0f) == 500.0f,
          "beyond a flat's margin the ground is not the tiles'");
    float in = k3d_flatten(&f, 1, 0.3 + 1000 * u, 0.6 + 31 * u, 500.0f);
    float mid = k3d_flatten(&f, 1, 0.3 + 1000 * u, 0.6 + 105 * u, 500.0f);
    float out = k3d_flatten(&f, 1, 0.3 + 1000 * u, 0.6 + 179 * u, 500.0f);
    check(in > 15.0f && in < 16.0f && fabsf(mid - 257.5f) < 1.0f && out > 499.0f && out < 500.0f,
          "a flat's margin does not blend from its height to the tiles' with a smoothstep");
    check(k3d_flatten(&f, 0, 0.3, 0.6, 500.0f) == 500.0f, "no flats changed the ground");

    /* A chunk of a hill under a flat: the samples within its half-width
     * are level along it, and a chunk away from it is the hill. */
    struct k3d_place p;
    k3d_place_at(&p, -56.0, -34.8);
    double n = ldexp(1.0, 15);
    struct k3d_tile t = { (uint32_t)(p.ox * n), (uint32_t)(p.oy * n), 15 };
    double side = 1.0 / n;
    double cx = (t.x + 0.5) * side, cy = (t.y + 0.5) * side;
    struct k3d_flat g = { cx - 0.3 * side, cy, cx + 0.3 * side, cy, 0.05 * side, 0.1 * side,
                          42.0f, 42.0f };
    struct flattened c = { &g, 1 };
    static struct k3d_terrain_vertex v[K3D_CHUNK_VERTICES];
    static uint32_t idx[K3D_CHUNK_INDICES];
    k3d_terrain_chunk(&p, t, hill_flattened, &c, 20.0f, v, idx, NULL, NULL);
    int level = 0, off = 0;
    for (unsigned j = 0; j < K3D_CHUNK_SIDE; j++) {
        for (unsigned i = 0; i < K3D_CHUNK_SIDE; i++) {
            double mx = (t.x * 64.0 + i) / (64.0 * n), my = (t.y * 64.0 + j) / (64.0 * n);
            float y = v[j * K3D_CHUNK_SIDE + i].position[1];

            if (fabs(my - cy) < 0.04 * side && fabs(mx - cx) < 0.29 * side) {
                level++;
                off += fabsf(y - 42.0f) > 1e-3f;
            } else if (fabs(my - cy) > 0.16 * side) {
                off += fabsf(y - hill_under(NULL, mx, my)) > 1e-2f;
            }
        }
    }
    check(level > 20 && off == 0, "a chunk under a flat is not level along it and the hill beyond");
}

int main(void)
{
    frame();
    chunks();
    mesh();
    island();
    flats();
    if (fails) {
        printf("FAIL: %d of %d checks on the 3D Kit's terrain.\n", fails, checks + fails);
        return 1;
    }
    printf("PASS: %d checks on the 3D Kit's terrain (the frame, the chunks chosen - covered "
           "once, deeper nearer, and with little room - and a chunk's mesh on a plane and a "
           "hill, its seams the same bits, the invented island, and a runway's flat).\n", checks);
    return 0;
}
