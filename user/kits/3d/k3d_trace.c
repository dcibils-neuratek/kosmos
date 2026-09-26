/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The 3D Kit's ray tracer: Cafesa3D's Rendered view and its F12.
 *
 * **Two ways to trace, one tracer** (`docs/cafesa3d.html`). *Final* is path
 * tracing, as Blender's Cycles is: every bounce followed at random, light
 * sampled directly from each lamp's sphere, the sky seen past everything,
 * so it starts noisy and settles into soft shadows, colour carried from
 * surface to surface and glass that bends what is behind it. *Preview* is
 * Whitted's ray tracing: exact mirrors and glass, hard shadows from each
 * lamp's middle, nothing bounced - no noise, fast enough to turn the view
 * in. They share everything but what happens at a surface.
 *
 * **Shapes as they are.** A box and a plane are traced as the shapes
 * themselves, and so are a smooth sphere and a smooth cylinder - one
 * equation, a perfect outline - in the object's own space, so a sphere
 * stretched by its scale is an exact ellipsoid. A flat-shaded sphere is its
 * facets, because that is what flat shading asks to see, and so is a
 * cylinder of three sides, which is a prism and not a rounder one.
 * Everything else is its triangles.
 *
 * **Four at a time.** Nearly all of a ray's time goes on asking boxes and
 * triangles whether it meets them, so that is what is vectorised, four
 * lanes wide - NEON on ARM, SSE on a PC, written in the compiler's vector
 * types as `gfx.c`'s blitter is:
 *
 *   - Each hierarchy is four-wide. A node holds four children's boxes side
 *     by side, one lane each, and a ray meets all four in one pass of
 *     twelve subtractions and twelve multiplies. It is built binary, by the
 *     surface area heuristic, and then collapsed: each node takes its
 *     largest children's children until it has four.
 *   - A mesh's triangles are packed four to a packet - corner, edge, edge,
 *     each axis a vector - and a ray meets a packet in one Moller-Trumbore.
 *   - A shadow ray stops at the first thing in its way, which is most of
 *     them, rather than looking for the nearest.
 *
 * Four lanes because both machines have them without asking: AVX's eight
 * need the kernel to save 256-bit registers on a switch, and today it saves
 * what FXSAVE does (`arch/x86_64/fp.c`), so AVX is a kernel step first.
 *
 * `k3d.h` says what a render is and how threads share one.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "k3d.h"

#define PI      3.14159265358979323846f
#define EPS     1e-4f
#define TILE    16
#define LEAF    4
#define BINS    12
#define STACK   128
#define GLOSSY  0.02f       /* a lobe narrower than this is a mirror's */
#define CLAMP   4.0f        /* the most light a bounced path may bring back */

/*--------------------------------------------------------------------------
 * Four lanes.
 *------------------------------------------------------------------------*/

typedef float   f4 __attribute__((vector_size(16)));
typedef int32_t i4 __attribute__((vector_size(16)));

#if defined(__aarch64__)
#include <arm_neon.h>
static inline f4 min4(f4 a, f4 b) { return (f4)vminq_f32((float32x4_t)a, (float32x4_t)b); }
static inline f4 max4(f4 a, f4 b) { return (f4)vmaxq_f32((float32x4_t)a, (float32x4_t)b); }
#elif defined(__x86_64__)
static inline f4 min4(f4 a, f4 b) { return __builtin_ia32_minps(a, b); }
static inline f4 max4(f4 a, f4 b) { return __builtin_ia32_maxps(a, b); }
#else
#error "k3d_trace.c: say how this architecture takes the least of four floats"
#endif

static inline f4 all4(float x) { f4 r = { x, x, x, x }; return r; }
static inline bool any4(i4 m) { return (m[0] | m[1] | m[2] | m[3]) != 0; }

/*--------------------------------------------------------------------------
 * Vectors: a point or a direction is four lanes too, the fourth nought, so
 * adding, subtracting and scaling one is one instruction.
 *------------------------------------------------------------------------*/

typedef f4 v3;

static inline v3 V(float x, float y, float z) { v3 r = { x, y, z, 0 }; return r; }
static inline v3 mul(v3 a, float k) { return a * all4(k); }
static inline float dot(v3 a, v3 b) { v3 p = a * b; return p[0] + p[1] + p[2]; }
static inline v3 cross(v3 a, v3 b)
{
    return V(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]);
}
static inline v3 norm(v3 a)
{
    float l = sqrtf(dot(a, a));

    return l > 0 ? mul(a, 1.0f / l) : a;
}
static inline v3 from(const float *p) { return V(p[0], p[1], p[2]); }
static inline float lo2(float a, float b) { return a < b ? a : b; }
static inline float hi2(float a, float b) { return a > b ? a : b; }

/* The libc has these for doubles only. */
static inline float pw(float a, float b) { return (float)pow((double)a, (double)b); }
static inline float cs(float a) { return (float)cos((double)a); }
static inline float sn(float a) { return (float)sin((double)a); }

/*--------------------------------------------------------------------------
 * Random numbers: PCG, seeded from where a sample is and never from who
 * draws it.
 *------------------------------------------------------------------------*/

struct rng { uint64_t state; };

static uint32_t next32(struct rng *r)
{
    uint64_t old = r->state;
    uint32_t x = (uint32_t)(((old >> 18) ^ old) >> 27);
    uint32_t rot = (uint32_t)(old >> 59);

    r->state = old * 6364136223846793005ull + 1442695040888963407ull;
    return (x >> rot) | (x << ((32 - rot) & 31));
}

static float rnd(struct rng *r) { return (float)(next32(r) >> 8) * (1.0f / 16777216.0f); }

static void seed(struct rng *r, uint32_t tile, uint32_t pass, uint32_t pixel)
{
    r->state = ((uint64_t)tile * 0x9e3779b97f4a7c15ull) ^ ((uint64_t)pass << 32)
               ^ ((uint64_t)pixel * 0xbf58476d1ce4e5b9ull) ^ 0x853c49e6748fea9bull;
    next32(r);
    next32(r);
}

/*--------------------------------------------------------------------------
 * The binary hierarchy, built by the surface area heuristic and then
 * collapsed into the four-wide one the rays use.
 *------------------------------------------------------------------------*/

struct bnode {
    float lo[3], hi[3];
    uint32_t first;         /* an inner node's left child; a leaf's first item */
    uint32_t count;         /* items in a leaf; nought for an inner node */
};

struct build {
    const float *lo, *hi;   /* three a thing */
    uint32_t *item;
    struct bnode *node;
    uint32_t used;
};

static float area(const float lo[3], const float hi[3])
{
    float dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];

    return dx < 0 ? 0 : 2 * (dx * dy + dy * dz + dz * dx);
}

static void grow(float lo[3], float hi[3], const float *l, const float *h)
{
    int k;

    for (k = 0; k < 3; k++) {
        lo[k] = lo2(lo[k], l[k]);
        hi[k] = hi2(hi[k], h[k]);
    }
}

static int bin_of(float c, float lo, float extent)
{
    int bin = (int)((c - lo) / extent * BINS);

    return bin < 0 ? 0 : bin >= BINS ? BINS - 1 : bin;
}

/* Split `n` things from `first` by twelve bins of their middles along the
 * axis the surface area heuristic likes best; a leaf when splitting costs
 * more than it saves. */
static void subdivide(struct build *b, uint32_t at, uint32_t first, uint32_t n, int depth)
{
    struct bnode *nd = &b->node[at];
    float clo[3] = { 1e30f, 1e30f, 1e30f }, chi[3] = { -1e30f, -1e30f, -1e30f };
    float best_cost = 1e30f;
    int best_axis = -1, best_split = 0, axis;
    uint32_t i;

    nd->lo[0] = nd->lo[1] = nd->lo[2] = 1e30f;
    nd->hi[0] = nd->hi[1] = nd->hi[2] = -1e30f;

    for (i = first; i < first + n; i++) {
        const float *l = &b->lo[b->item[i] * 3], *h = &b->hi[b->item[i] * 3];
        float c[3] = { (l[0] + h[0]) / 2, (l[1] + h[1]) / 2, (l[2] + h[2]) / 2 };

        grow(nd->lo, nd->hi, l, h);
        grow(clo, chi, c, c);
    }

    nd->first = first;
    nd->count = n;

    if (n <= LEAF || depth > 48) {
        return;
    }

    for (axis = 0; axis < 3; axis++) {
        float blo[BINS][3], bhi[BINS][3], left_area[BINS], right_area[BINS];
        float llo[3], lhi[3], rlo[3], rhi[3];
        uint32_t bn[BINS] = { 0 }, left_n[BINS], right_n[BINS], ln = 0, rn = 0;
        float extent = chi[axis] - clo[axis];
        int k;

        if (extent <= 0) {
            continue;
        }

        for (k = 0; k < BINS; k++) {
            blo[k][0] = blo[k][1] = blo[k][2] = 1e30f;
            bhi[k][0] = bhi[k][1] = bhi[k][2] = -1e30f;
        }

        for (i = first; i < first + n; i++) {
            const float *l = &b->lo[b->item[i] * 3], *h = &b->hi[b->item[i] * 3];
            int bin = bin_of((l[axis] + h[axis]) / 2, clo[axis], extent);

            bn[bin]++;
            grow(blo[bin], bhi[bin], l, h);
        }

        llo[0] = llo[1] = llo[2] = rlo[0] = rlo[1] = rlo[2] = 1e30f;
        lhi[0] = lhi[1] = lhi[2] = rhi[0] = rhi[1] = rhi[2] = -1e30f;

        for (k = 0; k < BINS - 1; k++) {
            ln += bn[k];
            if (bn[k]) { grow(llo, lhi, blo[k], bhi[k]); }
            left_n[k] = ln;
            left_area[k] = area(llo, lhi);

            rn += bn[BINS - 1 - k];
            if (bn[BINS - 1 - k]) { grow(rlo, rhi, blo[BINS - 1 - k], bhi[BINS - 1 - k]); }
            right_n[BINS - 2 - k] = rn;
            right_area[BINS - 2 - k] = area(rlo, rhi);
        }

        for (k = 0; k < BINS - 1; k++) {
            float cost = (float)left_n[k] * left_area[k] + (float)right_n[k] * right_area[k];

            if (left_n[k] && right_n[k] && cost < best_cost) {
                best_cost = cost;
                best_axis = axis;
                best_split = k;
            }
        }
    }

    if (best_axis < 0 || best_cost >= (float)n * area(nd->lo, nd->hi)) {
        return;                         /* no split pays for itself */
    }

    /* The items on the low side of the chosen plane first. */
    {
        float extent = chi[best_axis] - clo[best_axis];
        uint32_t left = 0, l_at;

        for (i = first; i < first + n; i++) {
            const float *l = &b->lo[b->item[i] * 3], *h = &b->hi[b->item[i] * 3];

            if (bin_of((l[best_axis] + h[best_axis]) / 2, clo[best_axis], extent) <= best_split) {
                uint32_t swap = b->item[first + left];

                b->item[first + left] = b->item[i];
                b->item[i] = swap;
                left++;
            }
        }

        if (left == 0 || left == n) {
            return;
        }

        l_at = b->used;
        b->used += 2;
        nd->first = l_at;
        nd->count = 0;
        subdivide(b, l_at, first, left, depth + 1);
        subdivide(b, l_at + 1, first + left, n - left, depth + 1);
    }
}

/*--------------------------------------------------------------------------
 * The four-wide hierarchy.
 *------------------------------------------------------------------------*/

struct wnode {
    f4 box[2][3];           /* the four children's lows, then highs; x, y, z */
    int32_t child[4];       /* an inner child's node, or -1 - a leaf's first */
    uint32_t count[4];      /* a leaf's packets or objects; nought when empty */
};

struct tpack {              /* four triangles, one a lane */
    f4 v0[3], e1[3], e2[3];
    uint32_t tri[4];
};

struct wide {
    struct wnode *node;
    uint32_t nodes;
    uint32_t *item;         /* the objects, over which the top one is built */
    struct tpack *pack;     /* the triangles, for a mesh's */
    uint32_t packs;
};

struct collapse {
    const struct bnode *bn;
    const uint32_t *item;
    struct wide *w;
    const float *pos;       /* a mesh's, or NULL over objects */
    const uint32_t *tri;
};

static void empty_lane(struct wnode *wn, int k)
{
    int ax;

    /* Low above high: no ray meets it, whichever way it goes. */
    for (ax = 0; ax < 3; ax++) {
        wn->box[0][ax][k] = 1e30f;
        wn->box[1][ax][k] = -1e30f;
    }

    wn->child[k] = -1;
    wn->count[k] = 0;
}

static int32_t collapse(struct collapse *c, uint32_t at);

static void fill_lane(struct collapse *c, struct wnode *wn, int k, uint32_t at)
{
    const struct bnode *b = &c->bn[at];
    uint32_t i, first;
    int ax, l;

    for (ax = 0; ax < 3; ax++) {
        wn->box[0][ax][k] = b->lo[ax];
        wn->box[1][ax][k] = b->hi[ax];
    }

    if (b->count == 0) {
        wn->count[k] = 0;
        wn->child[k] = collapse(c, at);
        return;
    }

    if (c->pos == NULL) {
        wn->child[k] = -1 - (int32_t)b->first;
        wn->count[k] = b->count;
        return;
    }

    first = c->w->packs;

    for (i = 0; i < b->count; i += 4) {
        struct tpack *p = &c->w->pack[c->w->packs++];

        memset(p, 0, sizeof(*p));

        for (l = 0; l < 4; l++) {
            uint32_t t;
            const float *p0, *p1, *p2;

            p->tri[l] = UINT32_MAX;

            if (i + (uint32_t)l >= b->count) {
                continue;               /* all nought: meets nothing */
            }

            t = c->item[b->first + i + (uint32_t)l];
            p0 = &c->pos[c->tri[t * 3] * 3];
            p1 = &c->pos[c->tri[t * 3 + 1] * 3];
            p2 = &c->pos[c->tri[t * 3 + 2] * 3];

            for (ax = 0; ax < 3; ax++) {
                p->v0[ax][l] = p0[ax];
                p->e1[ax][l] = p1[ax] - p0[ax];
                p->e2[ax][l] = p2[ax] - p0[ax];
            }

            p->tri[l] = t;
        }
    }

    wn->child[k] = -1 - (int32_t)first;
    wn->count[k] = c->w->packs - first;
}

/* A binary inner node and as many of its descendants as make four. */
static int32_t collapse(struct collapse *c, uint32_t at)
{
    const struct bnode *bn = c->bn;
    uint32_t kid[4], n = 2, me = c->w->nodes++;
    struct wnode *wn = &c->w->node[me];
    int k;

    kid[0] = bn[at].first;
    kid[1] = bn[at].first + 1;

    while (n < 4) {
        int open = -1;
        float most = -1;

        for (k = 0; k < (int)n; k++) {
            float a = area(bn[kid[k]].lo, bn[kid[k]].hi);

            if (bn[kid[k]].count == 0 && a > most) {
                most = a;
                open = k;
            }
        }

        if (open < 0) {
            break;
        }

        kid[n++] = bn[kid[open]].first + 1;
        kid[open] = bn[kid[open]].first;
    }

    for (k = 0; k < 4; k++) {
        if (k < (int)n) {
            fill_lane(c, wn, k, kid[k]);
        } else {
            empty_lane(wn, k);
        }
    }

    return (int32_t)me;
}

/* The hierarchy over `n` boxes; over a mesh's triangles when `pos` is
 * given, packing them, and over objects when it is not. */
static bool wide_build(struct wide *w, const float *lo, const float *hi, uint32_t n,
                       const float *pos, const uint32_t *tri)
{
    struct build b;
    struct collapse c;
    uint32_t i, packs = 0;

    memset(w, 0, sizeof(*w));

    if (n == 0) {
        return true;
    }

    b.lo = lo;
    b.hi = hi;
    b.item = malloc(n * sizeof(uint32_t));
    b.node = malloc((size_t)(2 * n) * sizeof(struct bnode));
    b.used = 1;

    if (b.item == NULL || b.node == NULL) {
        free(b.item);
        free(b.node);
        return false;
    }

    for (i = 0; i < n; i++) {
        b.item[i] = i;
    }

    subdivide(&b, 0, 0, n, 0);

    for (i = 0; i < b.used; i++) {
        packs += (b.node[i].count + 3) / 4;
    }

    w->node = malloc(((b.used + 1) / 2 + 1) * sizeof(struct wnode));
    w->pack = pos ? malloc((packs ? packs : 1) * sizeof(struct tpack)) : NULL;

    if (w->node == NULL || (pos && w->pack == NULL)) {
        free(b.item);
        free(b.node);
        free(w->node);
        free(w->pack);
        memset(w, 0, sizeof(*w));
        return false;
    }

    c.bn = b.node;
    c.item = b.item;
    c.w = w;
    c.pos = pos;
    c.tri = tri;

    if (b.node[0].count > 0) {
        struct wnode *wn = &w->node[w->nodes++];
        int k;

        fill_lane(&c, wn, 0, 0);            /* few enough for one leaf */
        for (k = 1; k < 4; k++) {
            empty_lane(wn, k);
        }
    } else {
        collapse(&c, 0);
    }

    free(b.node);

    if (pos) {
        free(b.item);
    } else {
        w->item = b.item;
    }

    return true;
}

static void wide_free(struct wide *w)
{
    free(w->node);
    free(w->item);
    free(w->pack);
}

/* A ray, as the four-wide tests want it: each axis in every lane, and which
 * of a box's two sides it meets first. */
struct ray4 {
    f4 o[3], d[3], inv[3];
    int near[3];
};

static void ray4(struct ray4 *q, v3 o, v3 d)
{
    int ax;

    for (ax = 0; ax < 3; ax++) {
        float c = d[ax];

        /* Never quite nought, so a box's side is never nought times
         * infinity: a ray along an axis is one that barely leans. */
        if (fabsf(c) < 1e-20f) {
            c = c < 0 ? -1e-20f : 1e-20f;
        }

        q->o[ax] = all4(o[ax]);
        q->d[ax] = all4(d[ax]);
        q->inv[ax] = all4(1.0f / c);
        q->near[ax] = c < 0;
    }
}

/* Which of a node's four boxes the ray meets before `best`, and where. */
static inline i4 boxes(const struct wnode *nd, const struct ray4 *q, float best, f4 *tnear)
{
    f4 x0 = (nd->box[q->near[0]][0] - q->o[0]) * q->inv[0];
    f4 x1 = (nd->box[1 - q->near[0]][0] - q->o[0]) * q->inv[0];
    f4 y0 = (nd->box[q->near[1]][1] - q->o[1]) * q->inv[1];
    f4 y1 = (nd->box[1 - q->near[1]][1] - q->o[1]) * q->inv[1];
    f4 z0 = (nd->box[q->near[2]][2] - q->o[2]) * q->inv[2];
    f4 z1 = (nd->box[1 - q->near[2]][2] - q->o[2]) * q->inv[2];
    f4 tn = max4(max4(x0, y0), max4(z0, all4(0)));
    f4 tf = min4(min4(x1, y1), min4(z1, all4(best)));

    *tnear = tn;
    return tn <= tf;
}

struct entry {
    int32_t child;
    uint32_t count;
    float t;
};

/* The boxes a ray met, pushed nearest last, so it is taken first. */
static void push(struct entry *stack, uint32_t *sp, const struct wnode *nd, i4 hit, f4 tn)
{
    struct entry e[4];
    int n = 0, k, j;

    for (k = 0; k < 4; k++) {
        if (hit[k]) {
            struct entry x = { nd->child[k], nd->count[k], tn[k] };

            for (j = n; j > 0 && e[j - 1].t < x.t; j--) {
                e[j] = e[j - 1];
            }

            e[j] = x;
            n++;
        }
    }

    for (k = 0; k < n && *sp < STACK; k++) {
        stack[(*sp)++] = e[k];
    }
}

/* Four triangles against one ray: Moller and Trumbore, a lane each. A lane
 * whose triangle is edge-on divides by nought, and infinity or NaN fails
 * every comparison that follows, so no test for it is needed. */
static inline bool packet(const struct tpack *p, const struct ray4 *q, float *best,
                          uint32_t *tri, float *bu, float *bv)
{
    const f4 *d = q->d, *o = q->o;
    f4 pv0 = d[1] * p->e2[2] - d[2] * p->e2[1];
    f4 pv1 = d[2] * p->e2[0] - d[0] * p->e2[2];
    f4 pv2 = d[0] * p->e2[1] - d[1] * p->e2[0];
    f4 idet = all4(1) / (p->e1[0] * pv0 + p->e1[1] * pv1 + p->e1[2] * pv2);
    f4 t0 = o[0] - p->v0[0], t1 = o[1] - p->v0[1], t2 = o[2] - p->v0[2];
    f4 u = (t0 * pv0 + t1 * pv1 + t2 * pv2) * idet;
    f4 q0 = t1 * p->e1[2] - t2 * p->e1[1];
    f4 q1 = t2 * p->e1[0] - t0 * p->e1[2];
    f4 q2 = t0 * p->e1[1] - t1 * p->e1[0];
    f4 v = (d[0] * q0 + d[1] * q1 + d[2] * q2) * idet;
    f4 t = (p->e2[0] * q0 + p->e2[1] * q1 + p->e2[2] * q2) * idet;
    i4 ok = (u >= all4(0)) & (v >= all4(0)) & (u + v <= all4(1)) & (t > all4(EPS))
            & (t < all4(*best));
    bool got = false;
    int l;

    if (!any4(ok)) {
        return false;
    }

    for (l = 0; l < 4; l++) {
        if (ok[l] && t[l] < *best) {
            *best = t[l];
            *tri = p->tri[l];
            *bu = u[l];
            *bv = v[l];
            got = true;
        }
    }

    return got;
}

/*--------------------------------------------------------------------------
 * The snapshot.
 *------------------------------------------------------------------------*/

enum shape { MESH, SPHERE, BOX, CYLINDER, PLANE };

struct tobj {
    enum shape shape;
    bool smooth;
    uint32_t id;
    float M[12], I[12], N[9];   /* to the world, from it, and normals out */
    float a, b, c;              /* a sphere's radius; a box's halves; ... */
    float sc[3];                /* its scale, so a texture is in its metres */
    struct k3d_material mat;
    float lo[3], hi[3];         /* in the world */

    float *pos, *nrm;           /* a mesh, in the object's own space */
    uint32_t *tri, ntri;
    struct wide bvh;
};

struct k3d_render {
    struct k3d_render_setup how;
    struct k3d_light *lights;
    struct tobj *obj;
    uint32_t nobj;
    struct wide top;

    v3 eye, r, u, f;            /* the camera */
    float F;

    uint32_t tiles_x, tiles_y, tiles;
    float *acc;                 /* light so far, three a pixel */
    uint32_t *seq;              /* twice a tile's passes; odd while adding one */
    uint32_t *painted;          /* what `seq` was when each tile was painted */
    uint32_t next;              /* the next job */
    uint32_t stopped;
    uint64_t rays;
};

/* The inverse of an affine 3x4. */
static bool invert(const float M[12], float I[12])
{
    float a = M[0], b = M[1], c = M[2], d = M[4], e = M[5], f = M[6];
    float g = M[8], h = M[9], i = M[10];
    float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    float det = a * A + b * B + c * C, k;

    if (fabsf(det) < 1e-12f) {
        return false;
    }

    k = 1.0f / det;
    I[0] = A * k;  I[1] = -(b * i - c * h) * k;  I[2] = (b * f - c * e) * k;
    I[4] = B * k;  I[5] = (a * i - c * g) * k;   I[6] = -(a * f - c * d) * k;
    I[8] = C * k;  I[9] = -(a * h - b * g) * k;  I[10] = (a * e - b * d) * k;
    I[3] = -(I[0] * M[3] + I[1] * M[7] + I[2] * M[11]);
    I[7] = -(I[4] * M[3] + I[5] * M[7] + I[6] * M[11]);
    I[11] = -(I[8] * M[3] + I[9] * M[7] + I[10] * M[11]);
    return true;
}

static v3 xpoint(const float m[12], v3 p)
{
    return V(m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3],
             m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
             m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]);
}

static v3 xdir(const float m[12], v3 d)
{
    return V(m[0] * d[0] + m[1] * d[1] + m[2] * d[2],
             m[4] * d[0] + m[5] * d[1] + m[6] * d[2],
             m[8] * d[0] + m[9] * d[1] + m[10] * d[2]);
}

/* The world's bounds of an object's own box, from its eight corners. */
static void world_bounds(struct tobj *o, v3 lo, v3 hi)
{
    int k;

    o->lo[0] = o->lo[1] = o->lo[2] = 1e30f;
    o->hi[0] = o->hi[1] = o->hi[2] = -1e30f;

    for (k = 0; k < 8; k++) {
        v3 w = xpoint(o->M, V(k & 1 ? hi[0] : lo[0], k & 2 ? hi[1] : lo[1], k & 4 ? hi[2] : lo[2]));
        float p[3] = { w[0], w[1], w[2] };

        grow(o->lo, o->hi, p, p);
    }
}

/* Whether an object is traced as its true shape: a box and a plane always,
 * since their triangles are that shape; a sphere and a cylinder when they
 * are smooth and round enough to be meant as round. */
static enum shape true_shape(const struct k3d_object *o)
{
    if (o->faceted) {
        return MESH;
    }

    switch (o->kind) {
    case K3D_BOX:      return BOX;
    case K3D_PLANE:    return PLANE;
    case K3D_SPHERE:   return o->smooth && o->segments >= 8 && o->rings >= 4 ? SPHERE : MESH;
    case K3D_CYLINDER: return o->smooth && o->segments >= 8 ? CYLINDER : MESH;
    default:           return MESH;
    }
}

static bool snapshot_object(struct tobj *t, struct k3d_object *o)
{
    const struct k3d_mesh *m;
    float *lo, *hi;
    uint32_t i;
    v3 blo, bhi;
    bool ok;

    memset(t, 0, sizeof(*t));
    k3d_object_matrix(o, t->M);

    if (!invert(t->M, t->I)) {
        return false;                   /* scaled to nothing: not there */
    }

    /* Normals go through the inverse's transpose. */
    t->N[0] = t->I[0]; t->N[1] = t->I[4]; t->N[2] = t->I[8];
    t->N[3] = t->I[1]; t->N[4] = t->I[5]; t->N[5] = t->I[9];
    t->N[6] = t->I[2]; t->N[7] = t->I[6]; t->N[8] = t->I[10];
    t->id = o->id;
    t->mat = o->mat;
    t->smooth = o->smooth;
    t->sc[0] = o->scale[0];
    t->sc[1] = o->scale[1];
    t->sc[2] = o->scale[2];
    t->shape = true_shape(o);

    switch (t->shape) {
    case SPHERE:
        t->a = o->radius;
        world_bounds(t, V(-t->a, -t->a, -t->a), V(t->a, t->a, t->a));
        return true;
    case BOX:
        t->a = o->size[0] / 2; t->b = o->size[1] / 2; t->c = o->size[2] / 2;
        world_bounds(t, V(-t->a, -t->b, -t->c), V(t->a, t->b, t->c));
        return true;
    case CYLINDER:
        t->a = o->radius; t->b = o->depth / 2;
        world_bounds(t, V(-t->a, -t->a, -t->b), V(t->a, t->a, t->b));
        return true;
    case PLANE:
        t->a = o->size[0] / 2;
        world_bounds(t, V(-t->a, -t->a, -1e-3f), V(t->a, t->a, 1e-3f));
        return true;
    case MESH:
        break;
    }

    /* Its triangles, copied, with a hierarchy of their own. */
    if (o->stale && !k3d_mesh_build(o)) {
        return false;
    }

    m = &o->mesh;

    if (m->ntris == 0) {
        return false;
    }

    t->ntri = m->ntris;
    t->pos = malloc((size_t)m->nverts * 3 * sizeof(float));
    t->nrm = malloc((size_t)m->nverts * 3 * sizeof(float));
    t->tri = malloc((size_t)m->ntris * 3 * sizeof(uint32_t));
    lo = malloc((size_t)m->ntris * 3 * sizeof(float));
    hi = malloc((size_t)m->ntris * 3 * sizeof(float));

    if (!t->pos || !t->nrm || !t->tri || !lo || !hi) {
        free(lo);
        free(hi);
        return false;
    }

    memcpy(t->pos, m->pos, (size_t)m->nverts * 3 * sizeof(float));
    memcpy(t->nrm, m->nrm, (size_t)m->nverts * 3 * sizeof(float));
    memcpy(t->tri, m->tri, (size_t)m->ntris * 3 * sizeof(uint32_t));

    blo = V(1e30f, 1e30f, 1e30f);
    bhi = V(-1e30f, -1e30f, -1e30f);

    for (i = 0; i < m->ntris; i++) {
        int j;

        lo[i * 3] = lo[i * 3 + 1] = lo[i * 3 + 2] = 1e30f;
        hi[i * 3] = hi[i * 3 + 1] = hi[i * 3 + 2] = -1e30f;

        for (j = 0; j < 3; j++) {
            const float *p = &m->pos[m->tri[i * 3 + j] * 3];

            grow(&lo[i * 3], &hi[i * 3], p, p);
        }

        blo = min4(blo, V(lo[i * 3], lo[i * 3 + 1], lo[i * 3 + 2]));
        bhi = max4(bhi, V(hi[i * 3], hi[i * 3 + 1], hi[i * 3 + 2]));
    }

    ok = wide_build(&t->bvh, lo, hi, m->ntris, t->pos, t->tri);
    free(lo);
    free(hi);

    if (!ok) {
        return false;
    }

    world_bounds(t, blo, bhi);
    return true;
}

static void free_object(struct tobj *t)
{
    free(t->pos);
    free(t->nrm);
    free(t->tri);
    wide_free(&t->bvh);
}

void k3d_render_free(struct k3d_render *r)
{
    uint32_t i;

    if (r == NULL) {
        return;
    }

    for (i = 0; i < r->nobj; i++) {
        free_object(&r->obj[i]);
    }

    free(r->obj);
    free(r->lights);
    wide_free(&r->top);
    free(r->acc);
    free(r->seq);
    free(r->painted);
    free(r);
}

struct k3d_render *k3d_render_new(struct k3d_scene *s, const struct k3d_render_setup *how)
{
    struct k3d_render *r;
    float *lo, *hi;
    uint32_t i;
    bool ok;

    if (how->w <= 0 || how->h <= 0 || how->w > 8192 || how->h > 8192 || how->nlights < 0) {
        return NULL;
    }

    r = calloc(1, sizeof(*r));

    if (r == NULL) {
        return NULL;
    }

    r->how = *how;
    r->how.lights = NULL;
    r->obj = calloc(s->count ? s->count : 1, sizeof(struct tobj));
    r->lights = calloc(how->nlights > 0 ? (size_t)how->nlights : 1, sizeof(struct k3d_light));
    r->tiles_x = (uint32_t)(how->w + TILE - 1) / TILE;
    r->tiles_y = (uint32_t)(how->h + TILE - 1) / TILE;
    r->tiles = r->tiles_x * r->tiles_y;
    r->acc = calloc((size_t)how->w * (size_t)how->h * 3, sizeof(float));
    r->seq = calloc(r->tiles, sizeof(uint32_t));
    r->painted = calloc(r->tiles, sizeof(uint32_t));

    if (!r->obj || !r->lights || !r->acc || !r->seq || !r->painted) {
        k3d_render_free(r);
        return NULL;
    }

    if (how->nlights > 0) {
        memcpy(r->lights, how->lights, (size_t)how->nlights * sizeof(struct k3d_light));
    }

    for (i = 0; i < s->count; i++) {
        struct k3d_object *o = &s->obj[i];

        if (o->hidden || o->alpha <= 0) {
            continue;
        }

        if (snapshot_object(&r->obj[r->nobj], o)) {
            r->nobj++;
        } else {
            free_object(&r->obj[r->nobj]);
        }
    }

    lo = malloc((r->nobj ? r->nobj : 1) * 3 * sizeof(float));
    hi = malloc((r->nobj ? r->nobj : 1) * 3 * sizeof(float));

    if (!lo || !hi) {
        free(lo);
        free(hi);
        k3d_render_free(r);
        return NULL;
    }

    for (i = 0; i < r->nobj; i++) {
        memcpy(&lo[i * 3], r->obj[i].lo, sizeof(r->obj[i].lo));
        memcpy(&hi[i * 3], r->obj[i].hi, sizeof(r->obj[i].hi));
    }

    ok = wide_build(&r->top, lo, hi, r->nobj, NULL, NULL);
    free(lo);
    free(hi);

    if (!ok) {
        k3d_render_free(r);
        return NULL;
    }

    r->eye = from(how->eye);
    r->f = norm(from(how->target) - r->eye);
    r->r = norm(cross(r->f, V(0, 0, 1)));

    if (dot(r->r, r->r) == 0) {
        r->r = V(1, 0, 0);              /* straight down: any right will do */
    }

    r->u = cross(r->r, r->f);
    r->F = ((float)how->w / 2) / (float)tan((double)how->fov / 2);
    return r;
}

/*--------------------------------------------------------------------------
 * Hitting things.
 *------------------------------------------------------------------------*/

struct hit {
    float t;
    const struct tobj *o;
    int light;              /* a lamp's index, or -1 */
    v3 n;                   /* in the object's own space until `world_normal` */
    uint32_t tri;
    float bu, bv;           /* where in the triangle, for a smooth mesh */
};

/* A ray against a mesh's triangles, in its own space: the nearest, or with
 * `any` the first found. */
static bool mesh_hit(const struct tobj *o, v3 ro, v3 rd, bool any, struct hit *h)
{
    struct entry stack[STACK];
    uint32_t sp = 0;
    struct ray4 q;
    bool got = false;

    if (o->bvh.nodes == 0) {
        return false;
    }

    ray4(&q, ro, rd);
    stack[sp].child = 0;
    stack[sp].count = 0;
    stack[sp].t = 0;
    sp++;

    while (sp > 0) {
        struct entry e = stack[--sp];

        if (e.t >= h->t) {
            continue;                   /* something nearer has been found */
        }

        if (e.count == 0) {
            const struct wnode *nd = &o->bvh.node[e.child];
            f4 tn;
            i4 hit = boxes(nd, &q, h->t, &tn);

            if (any4(hit)) {
                push(stack, &sp, nd, hit, tn);
            }
        } else {
            const struct tpack *p = &o->bvh.pack[-1 - e.child];
            uint32_t k;

            for (k = 0; k < e.count; k++) {
                if (packet(&p[k], &q, &h->t, &h->tri, &h->bu, &h->bv)) {
                    got = true;

                    if (any) {
                        return true;
                    }
                }
            }
        }
    }

    if (got) {
        const uint32_t *tr = &o->tri[h->tri * 3];
        v3 p0 = from(&o->pos[tr[0] * 3]);

        h->n = cross(from(&o->pos[tr[1] * 3]) - p0, from(&o->pos[tr[2] * 3]) - p0);
    }

    return got;
}

/* A ray against one shape in its own space; true and `h` filled if nearer. */
static bool shape_hit(const struct tobj *o, v3 ro, v3 rd, bool any, struct hit *h)
{
    switch (o->shape) {
    case SPHERE: {
        float a = dot(rd, rd), b = dot(ro, rd), c = dot(ro, ro) - o->a * o->a;
        float disc = b * b - a * c, s, t;

        if (disc < 0) { return false; }

        s = sqrtf(disc);
        t = (-b - s) / a;
        if (t < EPS) { t = (-b + s) / a; }
        if (t < EPS || t >= h->t) { return false; }

        h->t = t;
        h->n = ro + mul(rd, t);
        return true;
    }
    case BOX: {
        float half[3] = { o->a, o->b, o->c };
        float t0 = -1e30f, t1 = 1e30f, t, sgn;
        int a0 = 0, a1 = 0, k;
        bool in;

        for (k = 0; k < 3; k++) {
            float ta, tb;

            if (fabsf(rd[k]) < 1e-12f) {
                if (fabsf(ro[k]) > half[k]) { return false; }
                continue;
            }

            ta = (-half[k] - ro[k]) / rd[k];
            tb = (half[k] - ro[k]) / rd[k];
            if (ta > tb) { float sw = ta; ta = tb; tb = sw; }
            if (ta > t0) { t0 = ta; a0 = k; }
            if (tb < t1) { t1 = tb; a1 = k; }
        }

        if (t0 > t1 || t1 < EPS) { return false; }

        in = t0 < EPS;
        t = in ? t1 : t0;
        if (t >= h->t) { return false; }

        /* Entering, the face that turns the ray back; leaving, the one ahead. */
        k = in ? a1 : a0;
        sgn = (rd[k] > 0) == in ? 1.0f : -1.0f;
        h->t = t;
        h->n = V(k == 0 ? sgn : 0, k == 1 ? sgn : 0, k == 2 ? sgn : 0);
        return true;
    }
    case CYLINDER: {
        float A = rd[0] * rd[0] + rd[1] * rd[1], B = ro[0] * rd[0] + ro[1] * rd[1];
        float C = ro[0] * ro[0] + ro[1] * ro[1] - o->a * o->a;
        bool got = false;
        int k;

        if (A > 1e-12f) {
            float D = B * B - A * C;

            if (D >= 0) {
                float s = sqrtf(D), ts[2] = { (-B - s) / A, (-B + s) / A };

                for (k = 0; k < 2; k++) {
                    float t = ts[k], z = ro[2] + rd[2] * t;

                    if (t > EPS && t < h->t && z >= -o->b && z <= o->b) {
                        h->t = t;
                        h->n = V(ro[0] + rd[0] * t, ro[1] + rd[1] * t, 0);
                        got = true;
                        break;
                    }
                }
            }
        }

        if (fabsf(rd[2]) > 1e-12f) {
            for (k = 0; k < 2; k++) {
                float zc = k ? o->b : -o->b, t = (zc - ro[2]) / rd[2];
                float x = ro[0] + rd[0] * t, y = ro[1] + rd[1] * t;

                if (t > EPS && t < h->t && x * x + y * y <= o->a * o->a) {
                    h->t = t;
                    h->n = V(0, 0, k ? 1.0f : -1.0f);
                    got = true;
                }
            }
        }

        return got;
    }
    case PLANE: {
        float t, x, y;

        if (fabsf(rd[2]) < 1e-12f) { return false; }

        t = -ro[2] / rd[2];
        if (t < EPS || t >= h->t) { return false; }

        x = ro[0] + rd[0] * t;
        y = ro[1] + rd[1] * t;
        if (fabsf(x) > o->a || fabsf(y) > o->a) { return false; }

        h->t = t;
        h->n = V(0, 0, 1);
        return true;
    }
    case MESH:
        return mesh_hit(o, ro, rd, any, h);
    }

    return false;
}

/* The nearest thing along the ray before `tmax`: an object, or a lamp when
 * `lamps`. With `any` it is a shadow ray, which wants only to know whether
 * something is in the way - and glass is not, so a window lets the sun in. */
static bool trace(const struct k3d_render *r, v3 o, v3 d, float tmax, bool lamps, bool any,
                  struct hit *h)
{
    struct entry stack[STACK];
    uint32_t sp = 0;
    struct ray4 q;
    int i;

    h->t = tmax;
    h->o = NULL;
    h->light = -1;

    if (r->top.nodes > 0) {
        ray4(&q, o, d);
        stack[sp].child = 0;
        stack[sp].count = 0;
        stack[sp].t = 0;
        sp++;
    }

    while (sp > 0) {
        struct entry e = stack[--sp];

        if (e.t >= h->t) {
            continue;
        }

        if (e.count == 0) {
            const struct wnode *nd = &r->top.node[e.child];
            f4 tn;
            i4 hit = boxes(nd, &q, h->t, &tn);

            if (any4(hit)) {
                push(stack, &sp, nd, hit, tn);
            }
        } else {
            const uint32_t *item = &r->top.item[-1 - e.child];
            uint32_t k;

            for (k = 0; k < e.count; k++) {
                const struct tobj *ob = &r->obj[item[k]];

                if (any && ob->mat.trans > 0.5f) {
                    continue;
                }

                if (shape_hit(ob, xpoint(ob->I, o), xdir(ob->I, d), any, h)) {
                    h->o = ob;

                    if (any) {
                        return true;
                    }
                }
            }
        }
    }

    if (lamps) {
        for (i = 0; i < r->how.nlights; i++) {
            const struct k3d_light *L = &r->lights[i];
            v3 oc = o - from(L->pos);
            float a = dot(d, d), b = dot(oc, d), c = dot(oc, oc) - L->radius * L->radius;
            float disc = b * b - a * c, t;

            if (disc < 0) { continue; }

            t = (-b - sqrtf(disc)) / a;
            if (t > EPS && t < h->t) {
                h->t = t;
                h->o = NULL;
                h->light = i;
            }
        }
    }

    return h->o != NULL || h->light >= 0;
}

/* The hit's normal in the world, unit length: smooth or flat. */
static v3 world_normal(const struct hit *h)
{
    const struct tobj *o = h->o;
    v3 n = h->n;

    if (o->shape == MESH && o->smooth) {
        const uint32_t *tr = &o->tri[h->tri * 3];
        v3 s = mul(from(&o->nrm[tr[0] * 3]), 1 - h->bu - h->bv)
               + mul(from(&o->nrm[tr[1] * 3]), h->bu) + mul(from(&o->nrm[tr[2] * 3]), h->bv);

        /* The smooth normal when it is on the face's side, else the flat. */
        if (dot(s, n) > 0) { n = s; }
    }

    return norm(V(o->N[0] * n[0] + o->N[1] * n[1] + o->N[2] * n[2],
                  o->N[3] * n[0] + o->N[4] * n[1] + o->N[5] * n[2],
                  o->N[6] * n[0] + o->N[7] * n[1] + o->N[8] * n[2]));
}

bool k3d_render_first_hit(const struct k3d_render *r, const float o[3], const float d[3],
                          float *t, uint32_t *id)
{
    struct hit h;

    if (!trace(r, from(o), from(d), 1e30f, false, false, &h)) {
        return false;
    }

    *t = h.t;
    *id = h.o->id;
    return true;
}

/*--------------------------------------------------------------------------
 * Light.
 *------------------------------------------------------------------------*/

static v3 sky(const struct k3d_render *r, v3 d)
{
    const struct k3d_world *w = &r->how.world;
    v3 zen = from(w->zenith), hor = from(w->horizon);
    float z = d[2] / sqrtf(dot(d, d));

    if (z < 0) {
        return mul(hor, w->strength * 0.6f);
    }

    return mul(hor + mul(zen - hor, pw(z, 0.55f)), w->strength);
}

/* A lamp's radiance: its power spread over a sphere of its radius. */
static v3 lamp_radiance(const struct k3d_light *L)
{
    float rr = L->radius > 1e-3f ? L->radius : 1e-3f;

    return mul(from(L->colour), L->power / (4 * PI * PI * rr * rr));
}

static void basis(v3 n, v3 *a, v3 *b)
{
    float s = n[2] >= 0 ? 1.0f : -1.0f, k = -1.0f / (s + n[2]), m = n[0] * n[1] * k;

    *a = V(1 + s * n[0] * n[0] * k, s * m, -s * n[0]);
    *b = V(m, s + n[1] * n[1] * k, -n[1]);
}

static v3 reflect(v3 d, v3 n) { return d - mul(n, 2 * dot(d, n)); }

/* Where `p` is on its object, in the object's own metres: its own space,
 * times its scale, which is where a texture is worked out
 * (`k3d_texture.c`). */
static void metric(const struct tobj *o, v3 p, float q[3])
{
    v3 l = xpoint(o->I, p);

    q[0] = l[0] * o->sc[0];
    q[1] = l[1] * o->sc[1];
    q[2] = l[2] * o->sc[2];
}

/*
 * **A textured surface's colour at `p`, and its normal bent by the
 * pattern's height.** The slope of the height across the surface, from four
 * more looks two millimetres either side, tilts the normal against it - so
 * mortar is a groove the light rakes across and a roof tile casts its edge,
 * with no more triangles than a box has.
 */
static v3 textured(const struct tobj *o, v3 p, v3 *n)
{
    const struct k3d_texture *t = &o->mat.tex;
    v3 base = from(o->mat.base);
    const float *M = o->M;
    float q[3], fac, h;

    /* Which way the surface faces in the object's own space - the world's
     * normal through the transpose of the object's turn - so a brick knows
     * a wall from a floor. */
    float nl[3] = { M[0] * (*n)[0] + M[4] * (*n)[1] + M[8] * (*n)[2],
                    M[1] * (*n)[0] + M[5] * (*n)[1] + M[9] * (*n)[2],
                    M[2] * (*n)[0] + M[6] * (*n)[1] + M[10] * (*n)[2] };
    float len = sqrtf(nl[0] * nl[0] + nl[1] * nl[1] + nl[2] * nl[2]);

    if (len > 0) {
        nl[0] /= len;
        nl[1] /= len;
        nl[2] /= len;
    }

    metric(o, p, q);
    k3d_pattern(t, q, nl, &fac, &h);

    if (t->bump > 0) {
        const float e = 0.002f;
        float h1, h2, h3, h4, f;
        v3 a, b, g;

        basis(*n, &a, &b);
        metric(o, p + mul(a, e), q);
        k3d_pattern(t, q, nl, &f, &h1);
        metric(o, p - mul(a, e), q);
        k3d_pattern(t, q, nl, &f, &h2);
        metric(o, p + mul(b, e), q);
        k3d_pattern(t, q, nl, &f, &h3);
        metric(o, p - mul(b, e), q);
        k3d_pattern(t, q, nl, &f, &h4);
        g = mul(a, (h1 - h2) / (2 * e)) + mul(b, (h3 - h4) / (2 * e));
        *n = norm(*n - mul(g, t->bump));
    }

    return max4(base + mul(from(t->colour2) - base, fac), all4(0));
}

/* A direction near `dir`, spread by `amount`: a glossy reflection's. */
static v3 fuzz(struct rng *g, v3 dir, float amount)
{
    v3 p;

    if (amount <= 0) { return dir; }

    do {
        p = V(rnd(g) * 2 - 1, rnd(g) * 2 - 1, rnd(g) * 2 - 1);
    } while (dot(p, p) > 1);

    return norm(dir + mul(p, amount));
}

/* Light reaching `p` facing `n` from every lamp: a point on the sphere each
 * fills, chosen at random within its cone - or its middle, for Whitted. */
static v3 direct(const struct k3d_render *r, struct rng *g, v3 p, v3 n, bool preview,
                 uint64_t *rays)
{
    v3 sum = V(0, 0, 0), start = p + mul(n, 1e-3f);
    int i;

    for (i = 0; i < r->how.nlights; i++) {
        const struct k3d_light *L = &r->lights[i];
        v3 to = from(L->pos) - p, w, dir, Le = lamp_radiance(L);
        float d2 = dot(to, to), dist = sqrtf(d2), rr = L->radius, c;
        struct hit h;

        w = mul(to, 1.0f / dist);

        if (preview || rr <= 0 || rr >= dist) {
            c = dot(w, n);

            if (c <= 0) { continue; }

            (*rays)++;
            if (trace(r, start, w, dist - rr, false, true, &h)) { continue; }

            /* The sphere as a point: its solid angle times its radiance. */
            sum += mul(Le, c * PI * rr * rr / d2);
            continue;
        }

        {
            float cosmax = sqrtf(hi2(0, 1 - rr * rr / d2));
            float ct = 1 - rnd(g) * (1 - cosmax), st = sqrtf(hi2(0, 1 - ct * ct));
            float ph = 2 * PI * rnd(g);
            v3 a, b;

            basis(w, &a, &b);
            dir = mul(a, cs(ph) * st) + mul(b, sn(ph) * st) + mul(w, ct);
            c = dot(dir, n);

            if (c <= 0) { continue; }

            (*rays)++;
            if (trace(r, start, dir, dist - rr, false, true, &h)) { continue; }

            sum += mul(Le, c * 2 * PI * (1 - cosmax));
        }
    }

    return mul(sum, 1.0f / PI);     /* a diffuse surface's 1/pi */
}

/* What a glossy lobe `amount` wide about `refl` sees of the lamps: the
 * share of each lamp's disc inside the cone `fuzz` scatters into, as though
 * the cone were even. It is what waiting for `fuzz` to hit a lamp would find
 * on average, found every time - a lamp is small and bright, and a path
 * that waits for it makes a picture of sparks. */
static v3 glossy(const struct k3d_render *r, v3 p, v3 n, v3 refl, float amount, uint64_t *rays)
{
    float alpha = (float)atan((double)amount), lobe = 2 * PI * (1 - cs(alpha));
    v3 sum = V(0, 0, 0), start = p + mul(n, 1e-3f);
    int i;

    for (i = 0; i < r->how.nlights; i++) {
        const struct k3d_light *L = &r->lights[i];
        v3 to = from(L->pos) - p, w;
        float dist = sqrtf(dot(to, to)), rr = L->radius, beta, disc, phi, share;
        struct hit h;

        w = mul(to, 1.0f / dist);

        if (dot(w, n) <= 0 || rr >= dist) {
            continue;
        }

        beta = (float)asin((double)(rr / dist));
        disc = 2 * PI * (1 - cs(beta));
        phi = (float)acos((double)hi2(-1, lo2(1, dot(w, refl))));

        if (phi >= alpha + beta) {
            continue;
        }

        share = lo2(disc, lobe) / lobe;

        if (phi > fabsf(alpha - beta)) {
            share *= (alpha + beta - phi) / (alpha + beta - fabsf(alpha - beta));
        }

        (*rays)++;
        if (trace(r, start, w, dist - rr, false, true, &h)) { continue; }

        sum += mul(lamp_radiance(L), share);
    }

    return sum;
}

/* Light brought back by a path that has already bounced off something
 * matte or rough, held to CLAMP: Cycles' indirect clamp. What it removes is
 * the one path in thousands that found a lamp, which is noise, not light. */
static v3 held(v3 c, bool bounced)
{
    float most = hi2(c[0], hi2(c[1], c[2]));

    return bounced && most > CLAMP ? mul(c, CLAMP / most) : c;
}

static v3 radiance(const struct k3d_render *r, struct rng *g, v3 o, v3 d, uint64_t *rays)
{
    v3 col = V(0, 0, 0), thr = V(1, 1, 1);
    bool preview = r->how.preview, specular = true, bounced = false;
    int bounce, most = r->how.bounces > 0 ? r->how.bounces : 6;

    for (bounce = 0; bounce <= most; bounce++) {
        struct hit h;
        const struct k3d_material *m;
        v3 p, n, base;

        (*rays)++;

        if (!trace(r, o, d, 1e30f, bounce > 0, false, &h)) {
            col += held(thr * sky(r, d), bounced);
            break;
        }

        if (h.light >= 0) {
            /* Seen in a mirror or through glass; after anything rougher it
             * was counted already, by `direct` or `glossy`. */
            if (specular) { col += held(thr * lamp_radiance(&r->lights[h.light]), bounced); }
            break;
        }

        m = &h.o->mat;
        p = o + mul(d, h.t);
        n = world_normal(&h);
        base = m->tex.pattern != K3D_PLAIN ? textured(h.o, p, &n) : from(m->base);

        if (m->emit > 0) {
            col += held(thr * mul(base, m->emit), bounced);
            break;
        }

        if (m->trans > 0.5f) {
            bool into = dot(d, n) < 0;
            v3 nn = into ? n : mul(n, -1);
            float eta = into ? 1.0f / m->ior : m->ior;
            float cosi = -dot(d, nn), k = 1 - eta * eta * (1 - cosi * cosi);
            float r0 = (1 - m->ior) / (1 + m->ior), R;

            r0 *= r0;
            R = k < 0 ? 1.0f : r0 + (1 - r0) * pw(1 - cosi, 5);

            if (preview ? R >= 0.5f : rnd(g) < R) {
                d = reflect(d, nn);
                o = p + mul(nn, 1e-3f);
            } else {
                d = norm(mul(d, eta) + mul(nn, eta * cosi - sqrtf(k)));
                o = p - mul(nn, 1e-3f);
                thr *= base;
            }

            d = fuzz(g, d, preview ? 0 : m->rough * m->rough * 2);
            specular = true;
            continue;
        }

        if (dot(n, d) > 0) { n = mul(n, -1); }

        if (m->metallic > 0.5f) {
            float amount = preview ? 0 : m->rough * m->rough * 2;
            v3 refl = reflect(d, n);

            if (amount >= GLOSSY) {
                col += held(thr * base * glossy(r, p, n, refl, amount, rays), bounced);
            }

            d = fuzz(g, refl, amount);

            if (dot(d, n) <= 0) { break; }

            thr *= base;
            o = p + mul(n, 1e-3f);
            specular = amount < GLOSSY;
            bounced = bounced || !specular;
            continue;
        }

        /* A clear coat over a coloured body: its reflection F of the light
         * by Fresnel, and the body the rest. Both lobes see the lamps every
         * time; the path goes on through one of them, chosen by F. */
        {
            float amount = m->rough * m->rough * 2;
            float F = preview || m->rough >= 0.9f ? 0 : 0.04f + 0.96f * pw(1 + dot(d, n), 5);
            v3 refl = reflect(d, n), light = mul(base, 1 - F) * direct(r, g, p, n, preview, rays);

            if (F > 0 && amount >= GLOSSY) {
                light += mul(glossy(r, p, n, refl, amount, rays), F);
            }

            col += held(thr * light, bounced);

            if (preview) {
                /* Whitted bounced nothing: a little of the sky stands in for it. */
                col += thr * base * mul(sky(r, V(0, 0, 1)), 0.35f);
                break;
            }

            if (rnd(g) < F) {
                d = fuzz(g, refl, amount);

                if (dot(d, n) <= 0) { break; }

                o = p + mul(n, 1e-3f);
                specular = amount < GLOSSY;
                bounced = bounced || !specular;
                continue;
            }
        }

        {
            float ph = 2 * PI * rnd(g), r2 = rnd(g), sr = sqrtf(r2);
            v3 a, b;

            basis(n, &a, &b);
            d = mul(a, cs(ph) * sr) + mul(b, sn(ph) * sr) + mul(n, sqrtf(1 - r2));
        }

        o = p + mul(n, 1e-3f);
        thr *= base;
        specular = false;
        bounced = true;

        if (bounce >= 2) {
            float q = hi2(0.08f, lo2(0.95f, hi2(thr[0], hi2(thr[1], thr[2]))));

            if (rnd(g) > q) { break; }

            thr = mul(thr, 1.0f / q);
        }
    }

    return col;
}

/*--------------------------------------------------------------------------
 * Tiles, passes and jobs.
 *
 * A tile's sequence number is twice the passes it has had, and odd while a
 * pass is being added in - so painting, which reads while the workers
 * write, can tell a tile it caught halfway and paint it again.
 *------------------------------------------------------------------------*/

void k3d_render_tile(struct k3d_render *r, uint32_t tile, uint32_t pass)
{
    uint32_t tx = tile % r->tiles_x, ty = tile / r->tiles_x;
    int x0 = (int)(tx * TILE), y0 = (int)(ty * TILE), x, y;
    float mine[TILE * TILE][3];
    uint64_t rays = 0;

    for (y = y0; y < y0 + TILE && y < r->how.h; y++) {
        for (x = x0; x < x0 + TILE && x < r->how.w; x++) {
            uint32_t at = (uint32_t)((y - y0) * TILE + (x - x0));
            struct rng g;
            float sx, sy;
            v3 d, c;

            seed(&g, tile, pass, at);

            /* The pixel's middle on the first pass, anywhere in it after,
             * which is what smooths the edges. */
            sx = (float)x + (pass == 0 ? 0.5f : rnd(&g)) - (float)r->how.w / 2;
            sy = (float)y + (pass == 0 ? 0.5f : rnd(&g)) - (float)r->how.h / 2;
            d = norm(mul(r->f, r->F) + mul(r->r, sx) - mul(r->u, sy));
            c = radiance(r, &g, r->eye, d, &rays);

            /* A firefly clamped, so one lucky path does not whiten a pixel. */
            mine[at][0] = lo2(c[0], 40);
            mine[at][1] = lo2(c[1], 40);
            mine[at][2] = lo2(c[2], 40);
        }
    }

    __atomic_store_n(&r->seq[tile], 2 * pass + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);

    for (y = y0; y < y0 + TILE && y < r->how.h; y++) {
        for (x = x0; x < x0 + TILE && x < r->how.w; x++) {
            float *a = &r->acc[((size_t)y * (size_t)r->how.w + (size_t)x) * 3];
            const float *m = mine[(y - y0) * TILE + (x - x0)];

            a[0] += m[0];
            a[1] += m[1];
            a[2] += m[2];
        }
    }

    __atomic_store_n(&r->seq[tile], 2 * pass + 2, __ATOMIC_RELEASE);
    __atomic_fetch_add(&r->rays, rays, __ATOMIC_RELAXED);
}

bool k3d_render_job(struct k3d_render *r, uint32_t *tile, uint32_t *pass,
                    void (*yield)(void))
{
    uint32_t job;

    if (__atomic_load_n(&r->stopped, __ATOMIC_ACQUIRE) || r->tiles == 0) {
        return false;
    }

    job = __atomic_fetch_add(&r->next, 1, __ATOMIC_RELAXED);
    *pass = job / r->tiles;
    *tile = job % r->tiles;

    if (*pass >= r->how.passes) {
        return false;
    }

    /* This tile's last pass, still being drawn by someone: wait for it. */
    while (__atomic_load_n(&r->seq[*tile], __ATOMIC_ACQUIRE) < 2 * *pass) {
        if (__atomic_load_n(&r->stopped, __ATOMIC_ACQUIRE)) {
            return false;
        }

        if (yield) { yield(); }
    }

    return true;
}

void k3d_render_stop(struct k3d_render *r)
{
    __atomic_store_n(&r->stopped, 1, __ATOMIC_RELEASE);
}

uint32_t k3d_render_passes(const struct k3d_render *r)
{
    uint32_t least = 0xffffffffu, i;

    for (i = 0; i < r->tiles; i++) {
        uint32_t done = __atomic_load_n(&r->seq[i], __ATOMIC_ACQUIRE) / 2;

        if (done < least) { least = done; }
    }

    return r->tiles ? least : 0;
}

uint64_t k3d_render_rays(const struct k3d_render *r)
{
    return __atomic_load_n(&r->rays, __ATOMIC_RELAXED);
}

/*
 * Filmic: ACES as Narkowicz fitted it, then the sRGB curve.
 *
 * The three channels are one vector through the fit, and the curve is a
 * table - painting is every pixel of the view, sixty times a second while a
 * render settles, and three `pow`s a pixel were most of what it cost.
 */
#define CURVE 8192

static uint8_t srgb[CURVE];

static void curve(void)
{
    int i;

    if (srgb[CURVE - 1] != 0) {
        return;
    }

    for (i = 0; i < CURVE; i++) {
        float x = (float)i / (CURVE - 1);
        float c = x <= 0.0031308f ? x * 12.92f : 1.055f * pw(x, 1 / 2.4f) - 0.055f;

        srgb[i] = (uint8_t)(c * 255 + 0.5f);
    }
}

static inline uint32_t tone(v3 c)
{
    v3 v = c * all4(0.9f);
    v3 x = (v * (all4(2.51f) * v + all4(0.03f))) / (v * (all4(2.43f) * v + all4(0.59f)) + all4(0.14f));

    x = min4(max4(x, all4(0)), all4(1)) * all4(CURVE - 1) + all4(0.5f);
    return 0xff000000u | ((uint32_t)srgb[(int)x[0]] << 16) | ((uint32_t)srgb[(int)x[1]] << 8)
           | srgb[(int)x[2]];
}

static void paint_tile(const struct k3d_render *r, uint32_t tile, uint32_t n, uint32_t *px,
                       size_t pitch)
{
    int x0 = (int)(tile % r->tiles_x * TILE), y0 = (int)(tile / r->tiles_x * TILE), x, y;
    float k = 1.0f / (float)n;

    for (y = y0; y < y0 + TILE && y < r->how.h; y++) {
        for (x = x0; x < x0 + TILE && x < r->how.w; x++) {
            px[(size_t)y * pitch + (size_t)x] =
                tone(mul(from(&r->acc[((size_t)y * (size_t)r->how.w + (size_t)x) * 3]), k));
        }
    }
}

uint32_t k3d_render_paint(struct k3d_render *r, uint32_t *px, size_t pitch, bool all)
{
    uint32_t tile, painted = 0;

    curve();

    for (tile = 0; tile < r->tiles; tile++) {
        int tries;

        /* A tile caught while a pass was added in is painted again; one
         * that never settles keeps what it has until the next paint. */
        for (tries = 0; tries < 3; tries++) {
            uint32_t s1 = __atomic_load_n(&r->seq[tile], __ATOMIC_ACQUIRE), s2;

            if (s1 == 0 || (s1 & 1) || (!all && s1 == r->painted[tile])) {
                break;
            }

            paint_tile(r, tile, s1 / 2, px, pitch);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            s2 = __atomic_load_n(&r->seq[tile], __ATOMIC_RELAXED);

            if (s1 == s2) {
                r->painted[tile] = s1;
                painted++;
                break;
            }
        }
    }

    return painted;
}
