/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A render's scene laid out for the GPU (`k3d_gpu.c`, `docs/cafesa3d-gpu.md`
 * R4), held to the processor's tracer on the Mac: sixty objects of every
 * kind - exact shapes and meshes, turned and stretched - and twenty
 * thousand rays, each walked through the threaded trees exactly as the
 * GPU's kernel will walk them, by skips and with no stack, must meet the
 * same object at the same distance `k3d_render_first_hit` does.
 *
 * The walk is here, in C, as the kernel's is in Kosmos's kernel language.
 * Where a ray meets an exact shape the tracer's own test of it is asked
 * (`k3d_trace_object_hit`); a mesh's triangles are met from the laid-out
 * arrays, so every word of them is read the way the kernel reads it.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/kits/3d/k3d.h"
#include "../user/kits/3d/k3d_gpu.h"
#include "../user/kits/3d/k3d_trace.h"

static int checks, fails;

static void check(int ok, const char *what)
{
    checks++;

    if (!ok) {
        fails++;
        printf("  not ok: %s\n", what);
    }
}

static double frand(unsigned *s)
{
    *s = *s * 1103515245u + 12345u;
    return (double)((*s >> 8) & 0xffffff) / 16777216.0;
}

static float word_f(const uint32_t *w) { float x; memcpy(&x, w, 4); return x; }

/* A box met before `best`, the slab test the kernel makes. */
static int box(const uint32_t *nd, const float o[3], const float inv[3], float best)
{
    float t0 = 0, t1 = best;
    int k;

    for (k = 0; k < 3; k++) {
        float a = (word_f(&nd[K3D_GPU_NODE_LO + k]) - o[k]) * inv[k];
        float b = (word_f(&nd[K3D_GPU_NODE_HI + k]) - o[k]) * inv[k];

        if (a > b) { float s = a; a = b; b = s; }
        t0 = a > t0 ? a : t0;
        t1 = b < t1 ? b : t1;
    }

    return t0 <= t1;
}

static void inverse(const float d[3], float inv[3])
{
    int k;

    for (k = 0; k < 3; k++) {
        float c = fabsf(d[k]) < 1e-20f ? (d[k] < 0 ? -1e-20f : 1e-20f) : d[k];

        inv[k] = 1.0f / c;
    }
}

/* Moller and Trumbore, against a laid-out triangle, as `packet` does it. */
static int triangle(const uint32_t *tw, const float o[3], const float d[3], float *best)
{
    float v0[3], e1[3], e2[3], pv[3], tv[3], qv[3], idet, u, v, t;
    int k;

    for (k = 0; k < 3; k++) {
        v0[k] = word_f(&tw[K3D_GPU_TRI_V0 + k]);
        e1[k] = word_f(&tw[K3D_GPU_TRI_E1 + k]);
        e2[k] = word_f(&tw[K3D_GPU_TRI_E2 + k]);
    }

    pv[0] = d[1] * e2[2] - d[2] * e2[1];
    pv[1] = d[2] * e2[0] - d[0] * e2[2];
    pv[2] = d[0] * e2[1] - d[1] * e2[0];
    idet = 1.0f / (e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2]);

    for (k = 0; k < 3; k++) { tv[k] = o[k] - v0[k]; }

    u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * idet;
    qv[0] = tv[1] * e1[2] - tv[2] * e1[1];
    qv[1] = tv[2] * e1[0] - tv[0] * e1[2];
    qv[2] = tv[0] * e1[1] - tv[1] * e1[0];
    v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * idet;
    t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * idet;

    if (u >= 0 && v >= 0 && u + v <= 1 && t > 1e-4f && t < *best) {
        *best = t;
        return 1;
    }

    return 0;
}

static void to_object(const uint32_t *ow, const float o[3], const float d[3], float ro[3], float rd[3])
{
    int k;

    for (k = 0; k < 3; k++) {
        const uint32_t *m = &ow[K3D_GPU_OBJ_I + k * 4];

        ro[k] = word_f(&m[0]) * o[0] + word_f(&m[1]) * o[1] + word_f(&m[2]) * o[2] + word_f(&m[3]);
        rd[k] = word_f(&m[0]) * d[0] + word_f(&m[1]) * d[1] + word_f(&m[2]) * d[2];
    }
}

/* A tree walked by skips from `root`, calling `leaf` for each item met. */
static int walk(const struct k3d_gpu_scene *s, uint32_t root, const float o[3], const float d[3],
                float *best, int (*leaf)(const struct k3d_gpu_scene *, uint32_t, const float *,
                                          const float *, float *, void *), void *ctx, uint32_t *visits)
{
    uint32_t node = root;
    float inv[3];
    int got = 0;

    inverse(d, inv);

    while (node != K3D_GPU_END) {
        const uint32_t *nd = &s->nodes[node * K3D_GPU_NODE_WORDS];

        (*visits)++;

        if (!box(nd, o, inv, *best)) {
            node = nd[K3D_GPU_NODE_SKIP];
            continue;
        }

        if (nd[K3D_GPU_NODE_LEAF] == 0) {
            node++;
            continue;
        }

        {
            uint32_t at = nd[K3D_GPU_NODE_LEAF] - 1, n = s->items[at], k;

            for (k = 0; k < n; k++) {
                got |= leaf(s, s->items[at + 1 + k], o, d, best, ctx);
            }
        }

        node = nd[K3D_GPU_NODE_SKIP];
    }

    return got;
}

static int tri_leaf(const struct k3d_gpu_scene *s, uint32_t t, const float *o, const float *d,
                    float *best, void *ctx)
{
    (void)ctx;
    return triangle(&s->tris[t * K3D_GPU_TRI_WORDS], o, d, best);
}

struct top_ctx {
    const struct k3d_render *r;
    uint32_t hit;               /* the object met nearest, by index */
    uint32_t *visits;
};

static int object_leaf(const struct k3d_gpu_scene *s, uint32_t i, const float *o, const float *d,
                       float *best, void *ctx)
{
    struct top_ctx *c = ctx;
    const uint32_t *ow = &s->objects[i * K3D_GPU_OBJ_WORDS];
    float t = *best;
    int got;

    if (ow[K3D_GPU_OBJ_SHAPE] == K3D_GPU_MESH) {
        float ro[3], rd[3];

        to_object(ow, o, d, ro, rd);
        got = walk(s, ow[K3D_GPU_OBJ_ROOT], ro, rd, &t, tri_leaf, NULL, c->visits);
    } else {
        got = k3d_trace_object_hit(c->r, i, o, d, &t);
    }

    if (got && t < *best) {
        *best = t;
        c->hit = i;
        return 1;
    }

    return 0;
}

int main(void)
{
    static const enum k3d_kind kinds[] = { K3D_PLANE, K3D_BOX, K3D_SPHERE, K3D_CYLINDER,
                                           K3D_ICO, K3D_CONE, K3D_TORUS, K3D_GRID };
    struct k3d_scene sc;
    struct k3d_render *r;
    struct k3d_gpu_scene g;
    struct k3d_render_setup how;
    unsigned seed = 7;
    uint32_t i, visits = 0;
    int rays = 20000, hits = 0, wrong = 0, near = 0, meshes = 0, shapes = 0;
    char what[200];

    k3d_scene_init(&sc);

    for (i = 0; i < 60; i++) {
        struct k3d_object *o = k3d_scene_add(&sc, kinds[i % 8]);

        o->loc[0] = (float)(frand(&seed) * 8 - 4);
        o->loc[1] = (float)(frand(&seed) * 8 - 4);
        o->loc[2] = (float)(frand(&seed) * 8 - 4);
        o->rot[0] = (float)(frand(&seed) * 360);
        o->rot[1] = (float)(frand(&seed) * 360);
        o->rot[2] = (float)(frand(&seed) * 360);
        o->scale[0] = (float)(0.3 + frand(&seed));
        o->scale[1] = (float)(0.3 + frand(&seed));
        o->scale[2] = (float)(0.3 + frand(&seed));
        o->smooth = i % 3 == 0;
        o->faceted = i % 5 == 0;                /* some exact, some their triangles */
        o->mat.base[0] = 0.5f;
        o->mat.ior = 1.5f;
    }

    memset(&how, 0, sizeof(how));
    how.w = how.h = 16;
    how.eye[1] = -20;
    how.fov = 0.7f;
    how.preview = true;
    how.passes = 1;
    r = k3d_render_new(&sc, &how);
    check(r != NULL, "the render was not made");

    if (r == NULL || !k3d_gpu_scene(r, &g)) {
        check(0, "the scene was not laid out");
        return 1;
    }

    for (i = 0; i < g.nobjects; i++) {
        if (g.objects[i * K3D_GPU_OBJ_WORDS + K3D_GPU_OBJ_SHAPE] == K3D_GPU_MESH) { meshes++; } else { shapes++; }
    }

    snprintf(what, sizeof what, "the layout holds %u objects, not 60", g.nobjects);
    check(g.nobjects == 60, what);
    check(meshes > 10 && shapes > 10, "the scene is not both meshes and exact shapes");
    check(g.root == 0 && g.nnodes > 60 && g.ntris > 1000, "the trees or the triangles are missing");

    /* Every skip goes forward, and the last of a tree to the end. */
    {
        int back = 0;

        for (i = 0; i < g.nnodes; i++) {
            uint32_t skip = g.nodes[i * K3D_GPU_NODE_WORDS + K3D_GPU_NODE_SKIP];

            back += skip != K3D_GPU_END && skip <= i;
        }

        check(back == 0, "a skip goes backwards, which would loop for ever");
    }

    for (i = 0; i < (uint32_t)rays; i++) {
        float o[3], d[3], len, t_cpu, best = 1e30f;
        uint32_t id_cpu = 0;
        struct top_ctx ctx = { r, K3D_GPU_END, &visits };
        int k, cpu, gpu;

        /* From outside the scene towards a point inside it, so most meet
         * something and many meet several. */
        for (k = 0; k < 3; k++) {
            o[k] = (float)(frand(&seed) * 24 - 12);
            d[k] = (float)(frand(&seed) * 8 - 4) - o[k];
        }

        len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (k = 0; k < 3; k++) { d[k] /= len; }

        cpu = k3d_render_first_hit(r, o, d, &t_cpu, &id_cpu);
        gpu = walk(&g, g.root, o, d, &best, object_leaf, &ctx, &visits);

        if (cpu != gpu) {
            wrong++;
            continue;
        }

        if (!cpu) {
            continue;
        }

        hits++;

        if (g.objects[ctx.hit * K3D_GPU_OBJ_WORDS + K3D_GPU_OBJ_ID] != id_cpu
            || fabsf(best - t_cpu) > 1e-4f * (1 + t_cpu)) {
            /* The same distance to the last bits, another object: an edge
             * shared by two, met first by whichever is asked first. */
            if (fabsf(best - t_cpu) <= 1e-5f * (1 + t_cpu)) { near++; } else { wrong++; }
        }
    }

    snprintf(what, sizeof what, "%d of %d rays met something else, or nothing, walked by skips", wrong, rays);
    check(wrong == 0, what);
    check(hits > rays / 3, "too few rays met anything to say much");
    printf("  %u nodes, %u triangles in %d meshes and %d exact shapes; %d of %d rays hit, %d on a shared edge; "
           "%.1f nodes a ray\n", g.nnodes, g.ntris, meshes, shapes, hits, rays, near, (double)visits / rays);

    k3d_gpu_scene_free(&g);
    k3d_render_free(r);
    k3d_scene_free(&sc);

    if (fails) {
        printf("FAIL: %d of %d checks on the GPU's scene\n", fails, checks + fails);
        return 1;
    }

    printf("PASS: %d checks on the GPU's scene (sixty objects laid out, the trees threaded, and twenty thousand "
           "rays walked by skips meeting what the processor's tracer meets)\n", checks);
    return 0;
}
