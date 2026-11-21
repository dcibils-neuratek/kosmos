/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A terrain's chunks: which, and each one's mesh (`k3d_terrain.h`).
 */

#include <math.h>
#include <stddef.h>

#include "k3d_terrain.h"
#include "kits/map/mercator.h"

#define CELLS (K3D_CHUNK_SIDE - 1)          /* 64 */

void k3d_place_at(struct k3d_place *p, double lon, double lat)
{
    map_project(lon, lat, &p->ox, &p->oy);
    if (lat > MAP_LAT_MOST) {
        lat = MAP_LAT_MOST;
    } else if (lat < -MAP_LAT_MOST) {
        lat = -MAP_LAT_MOST;
    }
    p->metres = MAP_EQUATOR_METRES * cos(lat * (3.14159265358979323846 / 180.0));
}

void k3d_place_world(const struct k3d_place *p, double mx, double my, double *x, double *z)
{
    *x = (mx - p->ox) * p->metres;
    *z = (my - p->oy) * p->metres;
}

double k3d_tile_side(const struct k3d_place *p, unsigned z)
{
    return ldexp(p->metres, -(int)z);
}

/* How far the eye is from a tile: to the nearest point of its square on
 * the ground, the eye's height counted from nought. */
static double distance(const struct k3d_place *p, struct k3d_tile t, const double eye[3])
{
    double n = ldexp(1.0, t.z), x0, z0, x1, z1, dx, dz;

    k3d_place_world(p, t.x / n, t.y / n, &x0, &z0);
    k3d_place_world(p, (t.x + 1.0) / n, (t.y + 1.0) / n, &x1, &z1);
    dx = eye[0] < x0 ? x0 - eye[0] : eye[0] > x1 ? eye[0] - x1 : 0.0;
    dz = eye[2] < z0 ? z0 - eye[2] : eye[2] > z1 ? eye[2] - z1 : 0.0;
    return sqrt(dx * dx + dz * dz + eye[1] * eye[1]);
}

unsigned k3d_terrain_choose(const struct k3d_place *p, const struct k3d_choice *c,
                            const double eye[3], struct k3d_tile *out, unsigned most)
{
    double n = ldexp(1.0, (int)c->root);
    double mx = p->ox + eye[0] / p->metres, my = p->oy + eye[2] / p->metres;
    int64_t side = (int64_t)1 << c->root, ex = (int64_t)floor(mx * n), ey = (int64_t)floor(my * n);
    int64_t r = (int64_t)c->radius;
    unsigned count = 0;

    if (ex < 0) {
        ex = 0;
    } else if (ex >= side) {
        ex = side - 1;
    }
    if (ey < 0) {
        ey = 0;
    } else if (ey >= side) {
        ey = side - 1;
    }

    /* The roots: every tile within `radius` of the eye's that is in the
     * world. All of them, or none. */
    for (int64_t y = ey - r; y <= ey + r; y++) {
        for (int64_t x = ex - r; x <= ex + r; x++) {
            if (x < 0 || y < 0 || x >= side || y >= side) {
                continue;
            }
            if (count == most) {
                return 0;
            }
            out[count++] = (struct k3d_tile){ (uint32_t)x, (uint32_t)y, (uint8_t)c->root };
        }
    }

    /* A zoom at a time, nearest first: a chunk the eye is near enough to
     * becomes its four children - the first in its place, three more at
     * the end - while there is room for them. Splitting one zoom before
     * the next means a lack of room is felt in the detail far away, and
     * always as a whole chunk, so the roots stay covered exactly. */
    for (unsigned z = c->root; z < c->deepest; z++) {
        double limit = c->split * k3d_tile_side(p, z);

        for (;;) {
            unsigned best = count;
            double nearest = limit;

            for (unsigned i = 0; i < count; i++) {
                if (out[i].z == z) {
                    double d = distance(p, out[i], eye);

                    if (d < nearest) {
                        nearest = d;
                        best = i;
                    }
                }
            }
            if (best == count || count + 3 > most) {
                break;
            }

            struct k3d_tile t = out[best];
            uint32_t x = t.x * 2, y = t.y * 2;
            uint8_t deeper = (uint8_t)(z + 1);

            out[best] = (struct k3d_tile){ x, y, deeper };
            out[count++] = (struct k3d_tile){ x + 1, y, deeper };
            out[count++] = (struct k3d_tile){ x, y + 1, deeper };
            out[count++] = (struct k3d_tile){ x + 1, y + 1, deeper };
        }
    }
    return count;
}

/* Sample (i, j) of a tile, -1 to 65, across the world: the same integers
 * divided the same way, whichever chunk asks. */
static double across(uint32_t tile, int i, uint8_t z)
{
    return (double)((int64_t)tile * CELLS + i) / ldexp((double)CELLS, z);
}

void k3d_terrain_chunk(const struct k3d_place *p, struct k3d_tile t, k3d_height_fn height,
                       void *context, float skirt, struct k3d_terrain_vertex *vertices,
                       uint32_t *indices, float *lowest, float *highest)
{
    /* The heights with a ring of one sample beyond the edges, for the
     * normals there: the neighbour's own samples, so its normals agree. */
    static const int RING = K3D_CHUNK_SIDE + 2;
    float h[K3D_CHUNK_SIDE + 2][K3D_CHUNK_SIDE + 2];
    double step = k3d_tile_side(p, t.z) / CELLS;
    float low = INFINITY, high = -INFINITY;

    for (int j = 0; j < RING; j++) {
        for (int i = 0; i < RING; i++) {
            h[j][i] = height(context, across(t.x, i - 1, t.z), across(t.y, j - 1, t.z));
        }
    }

    for (int j = 0; j < K3D_CHUNK_SIDE; j++) {
        for (int i = 0; i < K3D_CHUNK_SIDE; i++) {
            struct k3d_terrain_vertex *v = &vertices[j * K3D_CHUNK_SIDE + i];
            double x, z;
            float y = h[j + 1][i + 1];
            double gx = (h[j + 1][i + 2] - h[j + 1][i]) / (2.0 * step);
            double gz = (h[j + 2][i + 1] - h[j][i + 1]) / (2.0 * step);
            double length = sqrt(gx * gx + 1.0 + gz * gz);

            k3d_place_world(p, across(t.x, i, t.z), across(t.y, j, t.z), &x, &z);
            v->position[0] = (float)x;
            v->position[1] = y;
            v->position[2] = (float)z;
            v->normal[0] = (float)(-gx / length);
            v->normal[1] = (float)(1.0 / length);
            v->normal[2] = (float)(-gz / length);
            v->uv[0] = (float)i / CELLS;
            v->uv[1] = (float)j / CELLS;
            low = y < low ? y : low;
            high = y > high ? y : high;
        }
    }

    uint32_t *at = indices;

    /* Two triangles a cell, counter-clockwise from above: with x east and
     * z south, (i, j) -> (i, j + 1) -> (i + 1, j) turns about +y. */
    for (int j = 0; j < CELLS; j++) {
        for (int i = 0; i < CELLS; i++) {
            uint32_t a = (uint32_t)(j * K3D_CHUNK_SIDE + i), b = a + 1;
            uint32_t c = a + K3D_CHUNK_SIDE, d = c + 1;

            *at++ = a, *at++ = c, *at++ = b;
            *at++ = b, *at++ = c, *at++ = d;
        }
    }

    /* The skirts: each edge's row again, `skirt` lower, facing out. Each is
     * walked west to east or north to south; `turn` is whether that walk
     * with the skirt below winds the wrong way for facing out. */
    static const struct { int i0, j0, di, dj, turn; } edges[4] = {
        { 0, 0, 1, 0, 0 },                      /* north, facing -z */
        { 0, CELLS, 1, 0, 1 },                  /* south, facing +z */
        { 0, 0, 0, 1, 1 },                      /* west, facing -x */
        { CELLS, 0, 0, 1, 0 },                  /* east, facing +x */
    };
    uint32_t base = K3D_CHUNK_SIDE * K3D_CHUNK_SIDE;

    for (int e = 0; e < 4; e++) {
        uint32_t first = base + (uint32_t)(e * K3D_CHUNK_SIDE);

        for (int k = 0; k < K3D_CHUNK_SIDE; k++) {
            int i = edges[e].i0 + edges[e].di * k, j = edges[e].j0 + edges[e].dj * k;
            struct k3d_terrain_vertex *s = &vertices[first + (uint32_t)k];

            *s = vertices[j * K3D_CHUNK_SIDE + i];
            s->position[1] -= skirt;
        }
        for (int k = 0; k < CELLS; k++) {
            int i = edges[e].i0 + edges[e].di * k, j = edges[e].j0 + edges[e].dj * k;
            uint32_t e0 = (uint32_t)(j * K3D_CHUNK_SIDE + i);
            uint32_t e1 = (uint32_t)((j + edges[e].dj) * K3D_CHUNK_SIDE + i + edges[e].di);
            uint32_t s0 = first + (uint32_t)k, s1 = s0 + 1;

            if (edges[e].turn) {
                *at++ = e0, *at++ = s0, *at++ = e1;
                *at++ = e1, *at++ = s0, *at++ = s1;
            } else {
                *at++ = e0, *at++ = e1, *at++ = s0;
                *at++ = e1, *at++ = s1, *at++ = s0;
            }
        }
    }

    if (lowest != NULL) {
        *lowest = low;
    }
    if (highest != NULL) {
        *highest = high;
    }
}

float k3d_flatten(const struct k3d_flat *flats, unsigned n, double mx, double my, float h)
{
    for (unsigned i = 0; i < n; i++) {
        const struct k3d_flat *f = &flats[i];
        double dx = f->bx - f->ax, dy = f->by - f->ay;
        double along = dx * dx + dy * dy;
        double t = along > 0 ? ((mx - f->ax) * dx + (my - f->ay) * dy) / along : 0;

        if (t < 0) {
            t = 0;
        } else if (t > 1) {
            t = 1;
        }
        double ex = mx - (f->ax + t * dx), ey = my - (f->ay + t * dy);
        double d = sqrt(ex * ex + ey * ey);

        if (d >= f->half + f->margin) {
            continue;
        }
        float level = (float)(f->ha + t * (f->hb - f->ha));

        if (d <= f->half || f->margin <= 0) {
            h = level;
        } else {
            double s = (d - f->half) / f->margin;

            s = s * s * (3 - 2 * s);
            h = (float)(level + s * (h - level));
        }
    }
    return h;
}
