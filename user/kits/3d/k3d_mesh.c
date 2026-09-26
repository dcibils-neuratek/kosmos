/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The 3D Kit's scene and shapes: objects kept in a list, and each shape
 * made into triangles from its own numbers.
 *
 * **A shape keeps its numbers.** A sphere is its radius, segments and rings,
 * and its triangles are made from them whenever they change - which is what
 * lets Cafesa3D keep those numbers editable in its Data tab, where Blender
 * shows them once and forgets them. The triangles are a cache of the
 * numbers, never the other way round.
 *
 * **Every triangle is anticlockwise seen from outside**, so its normal by
 * the right-hand rule points out and the back of a shape can be skipped
 * without looking at its vertex normals. `tools/test_k3d.c` checks that for
 * every shape at several sizes, because a wound-backwards face is invisible
 * from the front and nothing else would ever notice.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "k3d.h"

#define PI 3.14159265358979323846f

/* In double and cast: Kosmos's C library has the double forms, and these
 * run when a shape is built, never once a pixel. */
static float sn(float a) { return (float)sin((double)a); }
static float cs(float a) { return (float)cos((double)a); }

void k3d_scene_init(struct k3d_scene *s)
{
    s->obj = NULL;
    s->count = s->cap = 0;
    s->next_id = 1;
}

void k3d_scene_free(struct k3d_scene *s)
{
    uint32_t i;

    for (i = 0; i < s->count; i++) {
        k3d_mesh_free(&s->obj[i].mesh);
    }

    free(s->obj);
    k3d_scene_init(s);
}

/*
 * A new object with Blender's defaults for its kind: a cube two metres a
 * side, a sphere of radius one with 32 segments and 16 rings, a cylinder of
 * radius one and depth two with 32 sides - so an object that is only added
 * is the one Blender would have added.
 */
void k3d_object_defaults(struct k3d_object *o, enum k3d_kind kind)
{
    memset(o, 0, sizeof(*o));
    o->kind = kind;
    o->size[0] = o->size[1] = o->size[2] = 2.0f;
    o->radius = 1.0f;
    o->depth = 2.0f;
    o->radius2 = kind == K3D_TORUS ? 0.25f : 0.0f;
    o->segments = kind == K3D_TORUS ? 48 : kind == K3D_GRID ? 10 : 32;
    o->rings = kind == K3D_TORUS ? 12 : kind == K3D_GRID ? 10 : 16;
    o->subdivisions = 2;
    o->scale[0] = o->scale[1] = o->scale[2] = 1.0f;
    o->colour = 0xcccccc;
    o->alpha = 1.0f;
    o->stale = true;

    /* Blender's default material: 0.8 grey, half rough, an IOR of 1.5. */
    o->mat.base[0] = o->mat.base[1] = o->mat.base[2] = 0.6038f;
    o->mat.rough = 0.5f;
    o->mat.ior = 1.5f;
}

struct k3d_object *k3d_scene_add(struct k3d_scene *s, enum k3d_kind kind)
{
    struct k3d_object *o;

    if (s->count == s->cap) {
        uint32_t cap = s->cap ? s->cap * 2 : 16;
        struct k3d_object *grown = realloc(s->obj, cap * sizeof(*grown));

        if (grown == NULL) {
            return NULL;
        }

        s->obj = grown;
        s->cap = cap;
    }

    o = &s->obj[s->count++];
    k3d_object_defaults(o, kind);
    o->id = s->next_id++;
    return o;
}

struct k3d_object *k3d_scene_find(struct k3d_scene *s, uint32_t id)
{
    uint32_t i;

    for (i = 0; i < s->count; i++) {
        if (s->obj[i].id == id) {
            return &s->obj[i];
        }
    }

    return NULL;
}

/* Removed in place, and the order of the others kept: it is drawing order. */
bool k3d_scene_remove(struct k3d_scene *s, uint32_t id)
{
    uint32_t i;

    for (i = 0; i < s->count; i++) {
        if (s->obj[i].id == id) {
            k3d_mesh_free(&s->obj[i].mesh);
            memmove(&s->obj[i], &s->obj[i + 1],
                    (s->count - i - 1) * sizeof(s->obj[0]));
            s->count--;
            return true;
        }
    }

    return false;
}

void k3d_mesh_free(struct k3d_mesh *m)
{
    free(m->pos);
    free(m->nrm);
    free(m->tri);
    free(m->edge);
    memset(m, 0, sizeof(*m));
}

/*--------------------------------------------------------------------------
 * Building a shape. `struct build` is a mesh being filled, with its counts
 * fixed before the first vertex so nothing grows while it is written.
 *------------------------------------------------------------------------*/

struct build {
    struct k3d_mesh *m;
    uint32_t v, t, e;
};

static bool reserve(struct k3d_mesh *m, uint32_t verts, uint32_t tris,
                    uint32_t edges)
{
    m->pos = malloc((size_t)verts * 3 * sizeof(float));
    m->nrm = malloc((size_t)verts * 3 * sizeof(float));
    m->tri = malloc((size_t)tris * 3 * sizeof(uint32_t));
    m->edge = malloc((size_t)edges * 2 * sizeof(uint32_t));

    if (!m->pos || !m->nrm || !m->tri || !m->edge) {
        k3d_mesh_free(m);
        return false;
    }

    m->nverts = verts;
    m->ntris = tris;
    m->nedges = edges;
    return true;
}

static uint32_t vert(struct build *b, float x, float y, float z,
                     float nx, float ny, float nz)
{
    float *p = &b->m->pos[b->v * 3], *n = &b->m->nrm[b->v * 3];

    p[0] = x; p[1] = y; p[2] = z;
    n[0] = nx; n[1] = ny; n[2] = nz;
    return b->v++;
}

static void tri(struct build *b, uint32_t a, uint32_t c, uint32_t d)
{
    uint32_t *t = &b->m->tri[b->t * 3];

    t[0] = a; t[1] = c; t[2] = d;
    b->t++;
}

/* A quad anticlockwise from outside, as two triangles. */
static void quad(struct build *b, uint32_t a, uint32_t c, uint32_t d,
                 uint32_t e)
{
    tri(b, a, c, d);
    tri(b, a, d, e);
}

static void edge(struct build *b, uint32_t a, uint32_t c)
{
    uint32_t *e = &b->m->edge[b->e * 2];

    e[0] = a; e[1] = c;
    b->e++;
}

/* A plane of `size` in X and Y at the object's origin, facing up. */
static bool build_plane(const struct k3d_object *o, struct k3d_mesh *m)
{
    struct build b = { m, 0, 0, 0 };
    float h = o->size[0] / 2;
    uint32_t a, c, d, e;

    if (!reserve(m, 4, 2, 4)) {
        return false;
    }

    a = vert(&b, -h, -h, 0, 0, 0, 1);
    c = vert(&b,  h, -h, 0, 0, 0, 1);
    d = vert(&b,  h,  h, 0, 0, 0, 1);
    e = vert(&b, -h,  h, 0, 0, 0, 1);
    quad(&b, a, c, d, e);
    edge(&b, a, c); edge(&b, c, d); edge(&b, d, e); edge(&b, e, a);
    return true;
}

/*
 * A box of `size` about its origin: four vertices a face, so each face keeps
 * its own normal and a box drawn smooth is still a box.
 *
 * Each face is listed as its corners anticlockwise when looked at from
 * outside - bottom left, bottom right, top right, top left - with the signs
 * of X, Y and Z at each; the edges are the twelve of the box, from the
 * first corner of each.
 */
static bool build_box(const struct k3d_object *o, struct k3d_mesh *m)
{
    static const signed char face[6][3 + 4 * 3] = {
        /* normal      corners */
        {  1,  0,  0,   1, -1, -1,   1,  1, -1,   1,  1,  1,   1, -1,  1 },
        { -1,  0,  0,  -1,  1, -1,  -1, -1, -1,  -1, -1,  1,  -1,  1,  1 },
        {  0,  1,  0,   1,  1, -1,  -1,  1, -1,  -1,  1,  1,   1,  1,  1 },
        {  0, -1,  0,  -1, -1, -1,   1, -1, -1,   1, -1,  1,  -1, -1,  1 },
        {  0,  0,  1,  -1, -1,  1,   1, -1,  1,   1,  1,  1,  -1,  1,  1 },
        {  0,  0, -1,   1, -1, -1,  -1, -1, -1,  -1,  1, -1,   1,  1, -1 },
    };
    /* The twelve edges, as pairs of corners of the -X, +X, -Y and +Y faces:
     * the four round +X, the four round -X, and the four joining them. */
    struct build b = { m, 0, 0, 0 };
    float hx = o->size[0] / 2, hy = o->size[1] / 2, hz = o->size[2] / 2;
    uint32_t first[6];
    int f, k;

    if (!reserve(m, 24, 12, 12)) {
        return false;
    }

    for (f = 0; f < 6; f++) {
        const signed char *d = face[f];
        uint32_t v[4];

        for (k = 0; k < 4; k++) {
            const signed char *c = d + 3 + k * 3;

            v[k] = vert(&b, c[0] * hx, c[1] * hy, c[2] * hz, d[0], d[1], d[2]);
        }

        first[f] = v[0];
        quad(&b, v[0], v[1], v[2], v[3]);
    }

    /* +X face: corners 0..3 are (+,-,-) (+,+,-) (+,+,+) (+,-,+). */
    edge(&b, first[0] + 0, first[0] + 1);
    edge(&b, first[0] + 1, first[0] + 2);
    edge(&b, first[0] + 2, first[0] + 3);
    edge(&b, first[0] + 3, first[0] + 0);
    /* -X face: (-,+,-) (-,-,-) (-,-,+) (-,+,+). */
    edge(&b, first[1] + 0, first[1] + 1);
    edge(&b, first[1] + 1, first[1] + 2);
    edge(&b, first[1] + 2, first[1] + 3);
    edge(&b, first[1] + 3, first[1] + 0);
    /* And the four running along X: from the -Y face (-,-,-) (+,-,-)
     * (+,-,+) (-,-,+) its bottom and top, and likewise from +Y. */
    edge(&b, first[3] + 0, first[3] + 1);
    edge(&b, first[3] + 2, first[3] + 3);
    edge(&b, first[2] + 0, first[2] + 1);
    edge(&b, first[2] + 2, first[2] + 3);
    return true;
}

/*
 * A UV sphere: a vertex at each pole and `rings - 1` rings of `segments`
 * between them, the smooth normal being the direction from the centre.
 */
static bool build_sphere(const struct k3d_object *o, struct k3d_mesh *m)
{
    struct build b = { m, 0, 0, 0 };
    int seg = o->segments, rings = o->rings, i, j;
    uint32_t top, bottom, first;
    float r = o->radius;

    if (seg < 3 || rings < 2) {
        return false;
    }

    if (!reserve(m, (uint32_t)(2 + (rings - 1) * seg),
                 (uint32_t)(2 * seg + (rings - 2) * seg * 2),
                 (uint32_t)(seg * (rings - 1) + seg * rings))) {
        return false;
    }

    top = vert(&b, 0, 0, r, 0, 0, 1);
    first = b.v;

    for (j = 1; j < rings; j++) {
        float th = PI * (float)j / (float)rings;

        for (i = 0; i < seg; i++) {
            float ph = 2 * PI * (float)i / (float)seg;
            float x = sn(th) * cs(ph), y = sn(th) * sn(ph), z = cs(th);

            vert(&b, r * x, r * y, r * z, x, y, z);
        }
    }

    bottom = vert(&b, 0, 0, -r, 0, 0, -1);

#define RING(j, i) (first + (uint32_t)(((j) - 1) * seg + ((i) % seg)))

    for (i = 0; i < seg; i++) {
        tri(&b, top, RING(1, i), RING(1, i + 1));
        edge(&b, top, RING(1, i));
    }

    for (j = 1; j < rings - 1; j++) {
        for (i = 0; i < seg; i++) {
            quad(&b, RING(j, i), RING(j + 1, i), RING(j + 1, i + 1),
                 RING(j, i + 1));
            edge(&b, RING(j, i), RING(j + 1, i));
        }
    }

    for (i = 0; i < seg; i++) {
        tri(&b, bottom, RING(rings - 1, i + 1), RING(rings - 1, i));
        edge(&b, RING(rings - 1, i), bottom);
    }

    for (j = 1; j < rings; j++) {
        for (i = 0; i < seg; i++) {
            edge(&b, RING(j, i), RING(j, i + 1));
        }
    }

#undef RING
    return true;
}

/*
 * A cylinder along Z about its origin: the sides and the two caps have
 * vertices of their own, so a smooth cylinder has round sides and flat ends
 * - what Blender's "shade smooth by angle" gives - and each cap is a fan
 * from a centre vertex whose spokes are not edges, as Blender's n-gon cap
 * has none.
 */
static bool build_cylinder(const struct k3d_object *o, struct k3d_mesh *m)
{
    struct build b = { m, 0, 0, 0 };
    int n = o->segments, i;
    float r = o->radius, h = o->depth / 2;
    uint32_t side, ctop, cbot, capt, capb;

    if (n < 3) {
        return false;
    }

    if (!reserve(m, (uint32_t)(4 * n + 2), (uint32_t)(4 * n),
                 (uint32_t)(3 * n))) {
        return false;
    }

    side = b.v;

    for (i = 0; i < n; i++) {
        float ph = 2 * PI * (float)i / (float)n, x = cs(ph), y = sn(ph);

        vert(&b, r * x, r * y, -h, x, y, 0);
        vert(&b, r * x, r * y,  h, x, y, 0);
    }

    capt = b.v;

    for (i = 0; i < n; i++) {
        float ph = 2 * PI * (float)i / (float)n;

        vert(&b, r * cs(ph), r * sn(ph), h, 0, 0, 1);
    }

    capb = b.v;

    for (i = 0; i < n; i++) {
        float ph = 2 * PI * (float)i / (float)n;

        vert(&b, r * cs(ph), r * sn(ph), -h, 0, 0, -1);
    }

    ctop = vert(&b, 0, 0, h, 0, 0, 1);
    cbot = vert(&b, 0, 0, -h, 0, 0, -1);

    for (i = 0; i < n; i++) {
        uint32_t lo = side + (uint32_t)(2 * i), hi = lo + 1;
        uint32_t lo2 = side + (uint32_t)(2 * ((i + 1) % n)), hi2 = lo2 + 1;

        quad(&b, lo, lo2, hi2, hi);
        tri(&b, ctop, capt + (uint32_t)i, capt + (uint32_t)((i + 1) % n));
        tri(&b, cbot, capb + (uint32_t)((i + 1) % n), capb + (uint32_t)i);
        edge(&b, lo, hi);
        edge(&b, lo, lo2);
        edge(&b, hi, hi2);
    }

    return true;
}

/*
 * An ico sphere: the icosahedron, each triangle cut into four and pushed
 * out to the radius, `subdivisions - 1` times - Blender's count, where one
 * is the icosahedron itself. The midpoint of an edge is made once and
 * shared by both its triangles, found again through a small table keyed by
 * the edge's two ends.
 */
struct midpoints {
    uint64_t *key;
    uint32_t *at;
    uint32_t  cap;
};

static uint32_t midpoint(struct build *b, struct midpoints *mp, uint32_t a,
                         uint32_t c, float r)
{
    uint64_t key = a < c ? ((uint64_t)a << 32) | c : ((uint64_t)c << 32) | a;
    uint32_t h = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 40) & (mp->cap - 1);
    const float *p, *q;
    float m[3], l;

    while (mp->key[h] != 0) {
        if (mp->key[h] == key + 1) {
            return mp->at[h];
        }

        h = (h + 1) & (mp->cap - 1);
    }

    p = &b->m->pos[a * 3];
    q = &b->m->pos[c * 3];
    m[0] = (p[0] + q[0]) / 2;
    m[1] = (p[1] + q[1]) / 2;
    m[2] = (p[2] + q[2]) / 2;
    l = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    mp->key[h] = key + 1;       /* nought is "empty" */
    mp->at[h] = vert(b, m[0] / l * r, m[1] / l * r, m[2] / l * r,
                     m[0] / l, m[1] / l, m[2] / l);
    return mp->at[h];
}

static bool build_ico(const struct k3d_object *o, struct k3d_mesh *m)
{
    static const signed char face[20][3] = {
        { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 },
        { 1, 5, 9 }, { 5, 11, 4 }, { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 },
        { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 },
        { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 },
    };
    const float t = 1.6180339887f;
    const float corner[12][3] = {
        { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 },
        { 0, -1, t }, { 0, 1, t }, { 0, -1, -t }, { 0, 1, -t },
        { t, 0, -1 }, { t, 0, 1 }, { -t, 0, -1 }, { -t, 0, 1 },
    };
    struct build b = { m, 0, 0, 0 };
    struct midpoints mp = { NULL, NULL, 0 };
    uint32_t *faces, *next, nfaces = 20, level, i, k;
    uint32_t tris, verts, edges;
    float r = o->radius;
    int s = o->subdivisions;
    bool ok = false;

    if (s < 1 || s > 7) {
        return false;
    }

    tris = 20u << (2 * (s - 1));
    edges = 30u << (2 * (s - 1));
    verts = 10u * (1u << (2 * (s - 1))) + 2;

    if (!reserve(m, verts, tris, edges)) {
        return false;
    }

    faces = malloc((size_t)tris * 3 * sizeof(uint32_t));
    next = malloc((size_t)tris * 3 * sizeof(uint32_t));
    mp.cap = 1;

    while (mp.cap < edges * 2) {
        mp.cap <<= 1;
    }

    mp.key = calloc(mp.cap, sizeof(uint64_t));
    mp.at = malloc(mp.cap * sizeof(uint32_t));

    if (faces == NULL || next == NULL || mp.key == NULL || mp.at == NULL) {
        goto out;
    }

    for (i = 0; i < 12; i++) {
        float l = sqrtf(corner[i][0] * corner[i][0] + corner[i][1] * corner[i][1]
                        + corner[i][2] * corner[i][2]);

        vert(&b, corner[i][0] / l * r, corner[i][1] / l * r, corner[i][2] / l * r,
             corner[i][0] / l, corner[i][1] / l, corner[i][2] / l);
    }

    for (i = 0; i < 20; i++) {
        faces[i * 3] = (uint32_t)face[i][0];
        faces[i * 3 + 1] = (uint32_t)face[i][1];
        faces[i * 3 + 2] = (uint32_t)face[i][2];
    }

    for (level = 1; level < (uint32_t)s; level++) {
        memset(mp.key, 0, mp.cap * sizeof(uint64_t));

        for (i = 0; i < nfaces; i++) {
            uint32_t a = faces[i * 3], c = faces[i * 3 + 1], d = faces[i * 3 + 2];
            uint32_t ac = midpoint(&b, &mp, a, c, r);
            uint32_t cd = midpoint(&b, &mp, c, d, r);
            uint32_t da = midpoint(&b, &mp, d, a, r);
            uint32_t *f = &next[i * 12];

            f[0] = a;  f[1] = ac; f[2] = da;
            f[3] = c;  f[4] = cd; f[5] = ac;
            f[6] = d;  f[7] = da; f[8] = cd;
            f[9] = ac; f[10] = cd; f[11] = da;
        }

        nfaces *= 4;
        memcpy(faces, next, (size_t)nfaces * 3 * sizeof(uint32_t));
    }

    /* The triangles, and each edge once: from the triangle on whose
     * anticlockwise walk it runs from the lower vertex to the higher. */
    for (i = 0; i < nfaces; i++) {
        tri(&b, faces[i * 3], faces[i * 3 + 1], faces[i * 3 + 2]);

        for (k = 0; k < 3; k++) {
            uint32_t a = faces[i * 3 + k], c = faces[i * 3 + (k + 1) % 3];

            if (a < c) {
                edge(&b, a, c);
            }
        }
    }

    ok = b.v == verts && b.t == tris && b.e == edges;

    if (!ok) {
        k3d_mesh_free(m);
    }

out:
    free(faces);
    free(next);
    free(mp.key);
    free(mp.at);

    if (!ok && m->pos != NULL) {
        k3d_mesh_free(m);
    }

    return ok;
}

/*
 * A cone along Z about its origin: `radius` at the bottom, `radius2` at the
 * top, and a point there when it is nought - one triangle a side then, not
 * a quad folded flat. The sides' normals lean with the slope, so a smooth
 * cone is round and not a cylinder's shading on a slanted shape.
 */
static bool build_cone(const struct k3d_object *o, struct k3d_mesh *m)
{
    struct build b = { m, 0, 0, 0 };
    int n = o->segments, i;
    float r1 = o->radius, r2 = o->radius2, h = o->depth / 2;
    bool point = r2 <= 0;
    uint32_t side, capb, capt = 0, cbot, ctop = 0;
    uint32_t verts = (uint32_t)(2 * n + n + 1 + (point ? 0 : n + 1));
    uint32_t tris = (uint32_t)((point ? n : 2 * n) + n + (point ? 0 : n));
    uint32_t edges = (uint32_t)(n + n + (point ? 0 : n));

    if (n < 3 || r1 <= 0) {
        return false;
    }

    if (!reserve(m, verts, tris, edges)) {
        return false;
    }

    side = b.v;

    for (i = 0; i < n; i++) {
        float ph = 2 * PI * (float)i / (float)n, x = cs(ph), y = sn(ph);
        float nx = x * o->depth, ny = y * o->depth, nz = r1 - r2;
        float l = sqrtf(nx * nx + ny * ny + nz * nz);

        vert(&b, r1 * x, r1 * y, -h, nx / l, ny / l, nz / l);
        vert(&b, r2 * x, r2 * y,  h, nx / l, ny / l, nz / l);
    }

    capb = b.v;

    for (i = 0; i < n; i++) {
        float ph = 2 * PI * (float)i / (float)n;

        vert(&b, r1 * cs(ph), r1 * sn(ph), -h, 0, 0, -1);
    }

    cbot = vert(&b, 0, 0, -h, 0, 0, -1);

    if (!point) {
        capt = b.v;

        for (i = 0; i < n; i++) {
            float ph = 2 * PI * (float)i / (float)n;

            vert(&b, r2 * cs(ph), r2 * sn(ph), h, 0, 0, 1);
        }

        ctop = vert(&b, 0, 0, h, 0, 0, 1);
    }

    for (i = 0; i < n; i++) {
        uint32_t lo = side + (uint32_t)(2 * i), hi = lo + 1;
        uint32_t lo2 = side + (uint32_t)(2 * ((i + 1) % n)), hi2 = lo2 + 1;

        if (point) {
            tri(&b, lo, lo2, hi);
        } else {
            quad(&b, lo, lo2, hi2, hi);
            tri(&b, ctop, capt + (uint32_t)i, capt + (uint32_t)((i + 1) % n));
            edge(&b, hi, hi2);
        }

        tri(&b, cbot, capb + (uint32_t)((i + 1) % n), capb + (uint32_t)i);
        edge(&b, lo, hi);
        edge(&b, lo, lo2);
    }

    return true;
}

/*
 * A torus in the XY plane: `segments` round the ring at `radius`, `rings`
 * round the tube at `radius2`. Each vertex's smooth normal points away from
 * the middle of the tube, which is also what "outside" means for a shape
 * with a hole in it.
 */
static bool build_torus(const struct k3d_object *o, struct k3d_mesh *m)
{
    struct build b = { m, 0, 0, 0 };
    int M = o->segments, n = o->rings, i, j;
    float R = o->radius, r = o->radius2;

    if (M < 3 || n < 3 || r <= 0 || R <= 0) {
        return false;
    }

    if (!reserve(m, (uint32_t)(M * n), (uint32_t)(2 * M * n), (uint32_t)(2 * M * n))) {
        return false;
    }

    for (i = 0; i < M; i++) {
        float u = 2 * PI * (float)i / (float)M;

        for (j = 0; j < n; j++) {
            float w = 2 * PI * (float)j / (float)n;
            float nx = cs(w) * cs(u), ny = cs(w) * sn(u), nz = sn(w);

            vert(&b, (R + r * cs(w)) * cs(u), (R + r * cs(w)) * sn(u), r * sn(w),
                 nx, ny, nz);
        }
    }

#define AT(i, j) ((uint32_t)((((i) % M) * n) + ((j) % n)))

    for (i = 0; i < M; i++) {
        for (j = 0; j < n; j++) {
            quad(&b, AT(i, j), AT(i + 1, j), AT(i + 1, j + 1), AT(i, j + 1));
            edge(&b, AT(i, j), AT(i + 1, j));
            edge(&b, AT(i, j), AT(i, j + 1));
        }
    }

#undef AT
    return true;
}

/* A grid of `segments` by `rings` squares, `size` across, facing up. */
static bool build_grid(const struct k3d_object *o, struct k3d_mesh *m)
{
    struct build b = { m, 0, 0, 0 };
    int nx = o->segments, ny = o->rings, i, j;
    float h = o->size[0] / 2;

    if (nx < 1 || ny < 1) {
        return false;
    }

    if (!reserve(m, (uint32_t)((nx + 1) * (ny + 1)), (uint32_t)(2 * nx * ny),
                 (uint32_t)(nx * (ny + 1) + ny * (nx + 1)))) {
        return false;
    }

    for (j = 0; j <= ny; j++) {
        for (i = 0; i <= nx; i++) {
            vert(&b, -h + 2 * h * (float)i / (float)nx, -h + 2 * h * (float)j / (float)ny,
                 0, 0, 0, 1);
        }
    }

#define AT(i, j) ((uint32_t)((j) * (nx + 1) + (i)))

    for (j = 0; j <= ny; j++) {
        for (i = 0; i <= nx; i++) {
            if (i < nx && j < ny) {
                quad(&b, AT(i, j), AT(i + 1, j), AT(i + 1, j + 1), AT(i, j + 1));
            }

            if (i < nx) {
                edge(&b, AT(i, j), AT(i + 1, j));
            }

            if (j < ny) {
                edge(&b, AT(i, j), AT(i, j + 1));
            }
        }
    }

#undef AT
    return true;
}

bool k3d_mesh_build(struct k3d_object *o)
{
    struct k3d_mesh fresh;
    bool ok = false;

    memset(&fresh, 0, sizeof(fresh));

    switch (o->kind) {
    case K3D_PLANE:    ok = build_plane(o, &fresh);    break;
    case K3D_BOX:      ok = build_box(o, &fresh);      break;
    case K3D_SPHERE:   ok = build_sphere(o, &fresh);   break;
    case K3D_CYLINDER: ok = build_cylinder(o, &fresh); break;
    case K3D_ICO:      ok = build_ico(o, &fresh);      break;
    case K3D_CONE:     ok = build_cone(o, &fresh);     break;
    case K3D_TORUS:    ok = build_torus(o, &fresh);    break;
    case K3D_GRID:     ok = build_grid(o, &fresh);     break;
    }

    if (!ok) {
        return false;
    }

    k3d_mesh_free(&o->mesh);
    o->mesh = fresh;
    o->stale = false;
    return true;
}

/* How many triangles a shape is, from its numbers alone. */
uint32_t k3d_triangles(const struct k3d_object *o)
{
    switch (o->kind) {
    case K3D_PLANE:    return 2;
    case K3D_BOX:      return 12;
    case K3D_SPHERE:   return (uint32_t)(2 * o->segments
                                         + (o->rings - 2) * o->segments * 2);
    case K3D_CYLINDER: return (uint32_t)(4 * o->segments);
    case K3D_ICO:      return (o->subdivisions >= 1 && o->subdivisions <= 7)
                              ? 20u << (2 * (o->subdivisions - 1)) : 0;
    case K3D_CONE:     return (uint32_t)((o->radius2 > 0 ? 4 : 2) * o->segments);
    case K3D_TORUS:    return (uint32_t)(2 * o->segments * o->rings);
    case K3D_GRID:     return (uint32_t)(2 * o->segments * o->rings);
    }

    return 0;
}

/*
 * Object to world: scale, then turn about X, Y and Z in that order, then
 * move - so `m` is T * Rz * Ry * Rx * S, as three rows of four.
 */
static void rotation(const float deg[3], float R[9])
{
    float ax = deg[0] * PI / 180, ay = deg[1] * PI / 180, az = deg[2] * PI / 180;
    float cx = cs(ax), sx = sn(ax), cy = cs(ay), sy = sn(ay);
    float cz = cs(az), sz = sn(az);

    /* Rz * Ry * Rx */
    R[0] = cz * cy;  R[1] = cz * sy * sx - sz * cx;  R[2] = cz * sy * cx + sz * sx;
    R[3] = sz * cy;  R[4] = sz * sy * sx + cz * cx;  R[5] = sz * sy * cx - cz * sx;
    R[6] = -sy;      R[7] = cy * sx;                 R[8] = cy * cx;
}

void k3d_object_matrix(const struct k3d_object *o, float m[12])
{
    float R[9];
    int i;

    rotation(o->rot, R);

    for (i = 0; i < 3; i++) {
        m[i * 4 + 0] = R[i * 3 + 0] * o->scale[0];
        m[i * 4 + 1] = R[i * 3 + 1] * o->scale[1];
        m[i * 4 + 2] = R[i * 3 + 2] * o->scale[2];
        m[i * 4 + 3] = o->loc[i];
    }
}

/* A normal goes through R * S^-1, which keeps it square to a surface that
 * has been stretched; it is made unit length where it is used. */
void k3d_normal_matrix(const struct k3d_object *o, float n[9])
{
    float R[9];
    int i;

    rotation(o->rot, R);

    for (i = 0; i < 3; i++) {
        n[i * 3 + 0] = R[i * 3 + 0] / (o->scale[0] != 0 ? o->scale[0] : 1);
        n[i * 3 + 1] = R[i * 3 + 1] / (o->scale[1] != 0 ? o->scale[1] : 1);
        n[i * 3 + 2] = R[i * 3 + 2] / (o->scale[2] != 0 ? o->scale[2] : 1);
    }
}
