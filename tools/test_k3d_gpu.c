/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A render's scene laid out for the GPU (`k3d_gpu.c`, `docs/cafesa3d-gpu.md`
 * R4), held to the processor's tracer on the Mac: sixty objects of every
 * kind - exact shapes and meshes, turned and stretched - and twenty
 * thousand rays, each walked through the threaded trees exactly as the
 * GPU's kernel will walk them, by skips and with no stack, must meet the
 * same object at the same distance `k3d_render_first_hit` does.
 *
 * And then the tracer itself (R5): the kernel `k3dgpu.lua` holds, compiled
 * by `kc.lua`, run in the emulator of the execution units over a still life
 * laid out the same way, every pixel's light held to the processor's
 * Preview of it.
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
#include "../user/include/gpuproto.h"
#include "eu_emu.h"

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

/*--------------------------------------------------------------------------
 * The GPU's Preview, in the emulator, held to the processor's
 * (`cafesa3d-gpu.md` R5).
 *------------------------------------------------------------------------*/

static int nibble(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* A kernel by name from the file `tools/eu_kernels.lua` writes. */
static uint32_t kernel_named(const char *path, const char *want, uint32_t *words, size_t room)
{
    static char line[1 << 20];
    FILE *f = fopen(path, "r");
    uint32_t got = 0;

    if (f == NULL) {
        return 0;
    }

    while (fgets(line, sizeof line, f) != NULL) {
        unsigned why, at;
        char name[128];
        int skip;

        if (sscanf(line, "%u %u %127s %n", &why, &at, name, &skip) == 3 && strcmp(name, want) == 0) {
            const char *hex = line + skip;
            size_t n = strcspn(hex, "\r\n") / 2, i;

            for (i = 0; i < n && i < room; i++) {
                ((unsigned char *)words)[i] = (unsigned char)(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
            }

            got = (uint32_t)n;
            break;
        }
    }

    fclose(f);
    return got;
}

/* An array of words as a buffer of bytes the kernel reads by offset. */
static struct eu_buffer words_buffer(const void *words, size_t count)
{
    struct eu_buffer b;
    size_t bytes = count * 4, rows = (bytes + 4095) / 4096;

    b.bytes = calloc(rows ? rows : 1, 4096);
    memcpy(b.bytes, words, bytes);
    b.width = 1024;
    b.height = (uint32_t)(rows ? rows : 1);
    b.pitch = 4096;
    b.view = GPU_VIEW_BYTES;
    return b;
}

static void pack_f(uint8_t *c, size_t *at, float x) { memcpy(c + *at, &x, 4); *at += 4; }
static void pack_i(uint8_t *c, size_t *at, int32_t x) { memcpy(c + *at, &x, 4); *at += 4; }
static void pack_v(uint8_t *c, size_t *at, const float v[3])
{
    pack_f(c, at, v[0]);
    pack_f(c, at, v[1]);
    pack_f(c, at, v[2]);
}

/* The still life both tests render, Preview's as it always was and
 * Final's roughened (`testing.md` 18.706). */
static void still_life(struct k3d_scene *sc, struct k3d_light *lamp, struct k3d_render_setup *how,
                       int W, int H, bool final)
{
    struct k3d_object *o;
    uint32_t i;

    /* A still life: a floor, three spheres - matte, metal, glass - a box,
     * a smooth mesh and a lamp, under a sky. */
    k3d_scene_init(sc);
    o = k3d_scene_add(sc, K3D_PLANE);
    o->size[0] = 20;
    o->mat.base[0] = 0.8f; o->mat.base[1] = 0.78f; o->mat.base[2] = 0.72f;
    o = k3d_scene_add(sc, K3D_SPHERE);
    o->loc[0] = -1.2f; o->loc[2] = 0.7f; o->radius = 0.7f; o->smooth = true;
    o->mat.base[0] = 0.85f; o->mat.base[1] = 0.3f; o->mat.base[2] = 0.2f;
    o = k3d_scene_add(sc, K3D_SPHERE);
    o->loc[0] = 0.3f; o->loc[1] = 0.6f; o->loc[2] = 0.55f; o->radius = 0.55f; o->smooth = true;
    o->mat.base[0] = o->mat.base[1] = o->mat.base[2] = 0.9f; o->mat.metallic = 1;
    o = k3d_scene_add(sc, K3D_SPHERE);
    o->loc[0] = 1.4f; o->loc[1] = -0.4f; o->loc[2] = 0.45f; o->radius = 0.45f; o->smooth = true;
    o->mat.base[0] = o->mat.base[1] = o->mat.base[2] = 0.95f; o->mat.trans = 1; o->mat.ior = 1.5f;
    o = k3d_scene_add(sc, K3D_BOX);
    o->loc[0] = -0.2f; o->loc[1] = 1.8f; o->loc[2] = 0.4f; o->rot[2] = 30;
    o->size[0] = o->size[1] = o->size[2] = 0.8f;
    o->mat.base[0] = 0.3f; o->mat.base[1] = 0.55f; o->mat.base[2] = 0.9f;
    o = k3d_scene_add(sc, K3D_ICO);
    o->loc[0] = 2.2f; o->loc[1] = 1.5f; o->loc[2] = 0.6f; o->radius = 0.6f; o->smooth = true;
    o->mat.base[0] = 0.4f; o->mat.base[1] = 0.8f; o->mat.base[2] = 0.4f;

    /* For Final, rough enough that every lobe is taken: a glossy metal,
     * a rough coat on the red ball and a little on the box (18.706). */
    if (final) {
        sc->obj[1].mat.rough = 0.5f;
        sc->obj[2].mat.rough = 0.35f;
        sc->obj[4].mat.rough = 0.2f;
    }

    for (i = 0; i < sc->count; i++) {
        if (sc->obj[i].mat.ior == 0) { sc->obj[i].mat.ior = 1.5f; }
    }

    memset(lamp, 0, sizeof *lamp);
    lamp->pos[0] = 2; lamp->pos[1] = -2; lamp->pos[2] = 4;
    lamp->radius = 0.3f;
    lamp->colour[0] = lamp->colour[1] = lamp->colour[2] = 1;
    lamp->power = 900;

    memset(how, 0, sizeof *how);
    how->w = W;
    how->h = H;
    how->eye[0] = 0.5f; how->eye[1] = -7; how->eye[2] = 2.5f;
    how->target[0] = 0.4f; how->target[1] = 0.4f; how->target[2] = 0.5f;
    how->fov = 0.8f;
    how->preview = !final;
    how->bounces = 6;
    how->passes = 2;                 /* the pixel's middle, then a jittered pass */
    how->lights = lamp;
    how->nlights = 1;
    how->world.zenith[0] = 0.25f; how->world.zenith[1] = 0.45f; how->world.zenith[2] = 0.9f;
    how->world.horizon[0] = 0.75f; how->world.horizon[1] = 0.8f; how->world.horizon[2] = 0.9f;
    how->world.strength = 1;

}

/* Final's bounds: measured 1.1% a block and 0.07% the whole on the first
 * run, and 68.4% and 43.7% with no lamps (18.706) - so five times the noise,
 * and a twelfth of what a wrong picture is. */
#define FINAL_BLOCK 0.05
#define FINAL_WHOLE 0.02

/*
 * **Final, held to the processor's within the noise** (`cafesa3d-gpu.md`
 * R7a, `testing.md` 18.706). The two draw their random numbers differently,
 * so no pixel need agree: what must is the light of each block of 8 by 8
 * pixels and of the whole, after 32 passes each. And a control that the
 * measure can tell: the GPU's Final with no lamps is not the processor's.
 */
static void final_compare(const struct k3d_render *r, const struct eu_buffer *acc, int W, int H,
                          double *worst_block, double *whole)
{
    int bx, by, x, y;
    double all_c = 0, all_g = 0;

    *worst_block = 0;

    for (by = 0; by < H / 8; by++) {
        for (bx = 0; bx < W / 8; bx++) {
            double c = 0, g = 0;

            for (y = by * 8; y < by * 8 + 8; y++) {
                for (x = bx * 8; x < bx * 8 + 8; x++) {
                    const float *cp = &r->acc[(y * W + x) * 3];
                    float gp[3];

                    memcpy(gp, &acc->bytes[(size_t)(y * W + x) * 12], 12);
                    c += cp[0] + cp[1] + cp[2];
                    g += gp[0] + gp[1] + gp[2];
                }
            }

            all_c += c;
            all_g += g;

            if (fabs(g - c) / (c + 1e-3) > *worst_block) {
                *worst_block = fabs(g - c) / (c + 1e-3);
            }
        }
    }

    *whole = fabs(all_g - all_c) / (all_c + 1e-3);
}

static void final_tracer(const char *kernels)
{
    static uint32_t code[65536];
    struct k3d_scene sc;
    struct k3d_render *r;
    struct k3d_gpu_scene g;
    struct k3d_render_setup how;
    struct k3d_light lamp;
    struct eu_buffer b[7];
    struct eu_stats st;
    uint8_t constants[GPU_RUN_CONSTANTS];
    size_t at = 0, lamps_at;
    uint32_t n, i, tile, pass, worst_at = 0;
    const char *why;
    const int W = 48, H = 32, P = 32;
    double block = 1, whole = 1, dark_block = 0, dark_whole = 0;
    char what[256];

    n = kernel_named(kernels, "tracer-final", code, sizeof code);
    snprintf(what, sizeof what, "Final's kernel is not in %s", kernels);
    check(n > 0, what);

    if (n == 0) {
        return;
    }

    still_life(&sc, &lamp, &how, W, H, true);
    how.passes = P;
    r = k3d_render_new(&sc, &how);
    check(r != NULL && k3d_gpu_scene(r, &g), "the still life was not laid out for Final");

    if (r == NULL) {
        return;
    }

    while (k3d_render_job(r, &tile, &pass, NULL)) {
        k3d_render_tile(r, tile, pass);
    }

    memset(constants, 0, sizeof constants);
    pack_v(constants, &at, g.eye);
    pack_v(constants, &at, g.forward);
    pack_v(constants, &at, g.right);
    pack_v(constants, &at, g.up);
    pack_f(constants, &at, g.F);
    pack_i(constants, &at, g.w);
    pack_i(constants, &at, g.h);
    lamps_at = at;
    pack_i(constants, &at, (int32_t)g.nlamps);
    pack_i(constants, &at, (int32_t)g.root);
    pack_i(constants, &at, g.bounces);
    pack_i(constants, &at, 0);
    pack_v(constants, &at, g.world.zenith);
    pack_v(constants, &at, g.world.horizon);
    pack_f(constants, &at, g.world.strength);
    pack_i(constants, &at, P);                  /* runs: every pass in the one run */

    b[0] = words_buffer(g.nodes, (size_t)g.nnodes * K3D_GPU_NODE_WORDS);
    b[1] = words_buffer(g.items, g.nitems);
    b[2] = words_buffer(g.objects, (size_t)g.nobjects * K3D_GPU_OBJ_WORDS);
    b[3] = words_buffer(g.tris, (size_t)g.ntris * K3D_GPU_TRI_WORDS);
    b[4] = words_buffer(g.normals, (size_t)g.nnormals * 3);
    b[5] = words_buffer(g.lamps, (size_t)g.nlamps * K3D_GPU_LAMP_WORDS);

    for (i = 0; i < 6; i++) {
        b[i].view = GPU_VIEW_TABLE;
    }

    {
        float *zero = calloc((size_t)W * H * 3, sizeof(float));

        b[6] = words_buffer(zero, (size_t)W * H * 3);
        free(zero);
    }

    {
        struct eu_run run = { code, n, 8, (uint32_t)(W + 7) / 8, (uint32_t)H, b, 7, constants,
                              (uint32_t)((at + 31) & ~31u), 2000000000u };

        why = eu_emulate(&run, &st, &worst_at);
        snprintf(what, sizeof what, "Final did not run: %s at instruction %u", why ? why : "", worst_at);
        check(why == NULL, what);

        final_compare(r, &b[6], W, H, &block, &whole);

        /* The control: no lamps, the same 32 passes, from nought. */
        if (why == NULL) {
            int32_t none = 0;

            memset(b[6].bytes, 0, (size_t)W * H * 12);
            memcpy(constants + lamps_at, &none, 4);
            why = eu_emulate(&run, &st, &worst_at);
            final_compare(r, &b[6], W, H, &dark_block, &dark_whole);
        }
    }

    printf("  the still life, %dx%d in Final, %d passes each: blocks of 8 by 8 within %.1f%%, the whole "
           "within %.2f%% of the processor's; with no lamps %.1f%% and %.1f%% - %llu instructions\n",
           W, H, P, block * 100, whole * 100, dark_block * 100, dark_whole * 100,
           (unsigned long long)st.instructions);

    check(why == NULL && block <= FINAL_BLOCK && whole <= FINAL_WHOLE,
          "the GPU's Final is not the processor's within the noise");
    check(dark_block > FINAL_BLOCK || dark_whole > FINAL_WHOLE,
          "Final with no lamps passed as the processor's, so the measure tells nothing");

    for (i = 0; i < 7; i++) {
        free(b[i].bytes);
    }

    k3d_gpu_scene_free(&g);
    k3d_render_free(r);
    k3d_scene_free(&sc);
}

static void tracer(const char *kernels)
{
    static uint32_t code[65536];
    struct k3d_scene sc;
    struct k3d_render *r;
    struct k3d_gpu_scene g;
    struct k3d_render_setup how;
    struct k3d_light lamp;
    struct eu_buffer b[7];
    struct eu_stats st;
    uint8_t constants[GPU_RUN_CONSTANTS];
    size_t at = 0;
    uint32_t n, i, tile, pass, worst_at = 0;
    const char *why;
    int W = 48, H = 32, differ = 0, lit = 0;
    double worst = 0;
    char what[256];

    n = kernel_named(kernels, "tracer-preview", code, sizeof code);
    snprintf(what, sizeof what, "the tracer's kernel is not in %s", kernels);
    check(n > 0, what);

    if (n == 0) {
        return;
    }

    still_life(&sc, &lamp, &how, W, H, false);

    r = k3d_render_new(&sc, &how);
    check(r != NULL && k3d_gpu_scene(r, &g), "the still life was not laid out");

    if (r == NULL) {
        return;
    }

    /* The processor's two passes: the second's rays start where the
     * GPU's do, from the same hash of the pixel and the pass (18.700). */
    while (k3d_render_job(r, &tile, &pass, NULL)) {
        k3d_render_tile(r, tile, pass);
    }

    /* The GPU's, in the emulator: the constants in the kernel's order. */
    memset(constants, 0, sizeof constants);
    pack_v(constants, &at, g.eye);
    pack_v(constants, &at, g.forward);
    pack_v(constants, &at, g.right);
    pack_v(constants, &at, g.up);
    pack_f(constants, &at, g.F);
    pack_i(constants, &at, g.w);
    pack_i(constants, &at, g.h);
    pack_i(constants, &at, (int32_t)g.nlamps);
    pack_i(constants, &at, (int32_t)g.root);
    pack_i(constants, &at, g.bounces);
    pack_i(constants, &at, 0);                  /* the pass: the first, the pixel's middle */
    pack_v(constants, &at, g.world.zenith);
    pack_v(constants, &at, g.world.horizon);
    pack_f(constants, &at, g.world.strength);
    pack_i(constants, &at, 2);                  /* runs: both passes in the one run (18.705) */

    b[0] = words_buffer(g.nodes, (size_t)g.nnodes * K3D_GPU_NODE_WORDS);
    b[1] = words_buffer(g.items, g.nitems);
    b[2] = words_buffer(g.objects, (size_t)g.nobjects * K3D_GPU_OBJ_WORDS);
    b[3] = words_buffer(g.tris, (size_t)g.ntris * K3D_GPU_TRI_WORDS);
    b[4] = words_buffer(g.normals, (size_t)g.nnormals * 3);
    b[5] = words_buffer(g.lamps, (size_t)g.nlamps * K3D_GPU_LAMP_WORDS);

    /* The scene's six as tables, which the tracer reads by the sampler. */
    for (i = 0; i < 6; i++) {
        b[i].view = GPU_VIEW_TABLE;
    }
    {
        float *zero = calloc((size_t)W * H * 3, sizeof(float));

        b[6] = words_buffer(zero, (size_t)W * H * 3);
        free(zero);
    }

    /* K3D_PROFILE=FILE: each of the tracer's instructions and how many
     * times its threads ran it, over both passes - to lay beside kc's
     * assembly and see where the time goes (`testing.md` 18.701). */
    uint64_t *per_ip = getenv("K3D_PROFILE") ? calloc(n / 16u + 1u, sizeof(uint64_t)) : NULL;

    eu_profile(per_ip);

    {
        struct eu_run run = { code, n, 8, (uint32_t)(W + 7) / 8, (uint32_t)H, b, 7, constants,
                              (uint32_t)((at + 31) & ~31u), 200000000u };

        why = eu_emulate(&run, &st, &worst_at);
        snprintf(what, sizeof what, "the tracer did not run: %s at instruction %u", why ? why : "", worst_at);
        check(why == NULL, what);

    }

    eu_profile(NULL);

    if (per_ip) {
        FILE *f = fopen(getenv("K3D_PROFILE"), "w");

        for (i = 0; f && i < n / 16u; i++) {
            fprintf(f, "%u %llu\n", i, (unsigned long long)per_ip[i]);
        }

        if (f) { fclose(f); }
        free(per_ip);
    }

    /* Every pixel's light, the GPU's against the processor's. */
    for (i = 0; i < (uint32_t)(W * H) && why == NULL; i++) {
        const float *cpu = &r->acc[i * 3];
        float gpu[3];
        int k;

        memcpy(gpu, &b[6].bytes[i * 12], 12);
        lit += cpu[0] + cpu[1] + cpu[2] > 0.05f;

        for (k = 0; k < 3; k++) {
            double e = fabs((double)gpu[k] - cpu[k]) / (1e-3 + fabs((double)cpu[k]));

            if (e > worst) { worst = e; worst_at = i; }
            if (e > 2e-3) { differ++; break; }
        }
    }

    printf("  the still life, %dx%d in Preview, two passes: %d of %d pixels as the processor's within 0.2%%, "
           "the worst %.4f%% at %u,%u; %llu instructions in %llu threads\n",
           W, H, W * H - differ, W * H, worst * 100, worst_at % (uint32_t)W, worst_at / (uint32_t)W,
           (unsigned long long)st.instructions, (unsigned long long)st.threads);

    snprintf(what, sizeof what, "%d of %d pixels of the GPU's Preview are not the processor's", differ, W * H);
    check(why == NULL && differ <= W * H / 200, what);
    check(lit > W * H / 2, "the still life is mostly dark, so the comparison says little");

    /* The paint: the processor's own light through the GPU's paint
     * kernel, held to `k3d_render_paint`'s colours. */
    {
        static uint32_t paint_code[4096];
        uint32_t pn = kernel_named(kernels, "tracer-paint", paint_code, sizeof paint_code);
        uint32_t *want = calloc((size_t)W * H, 4), level[K3D_CURVE];
        const uint8_t *curve = k3d_render_curve();
        struct eu_buffer pb[3];
        uint8_t pc[32];
        size_t pat = 0;
        int off = 0, apart = 0;

        for (i = 0; i < K3D_CURVE; i++) { level[i] = curve[i]; }

        pb[0] = words_buffer(r->acc, (size_t)W * H * 3);
        pb[1] = words_buffer(level, K3D_CURVE);
        pb[1].view = GPU_VIEW_TABLE;                       /* the curve, a table */
        pb[2] = words_buffer(want, (size_t)W * H);         /* nought: the picture */
        memset(pc, 0, sizeof pc);
        pack_i(pc, &pat, W);
        pack_i(pc, &pat, H);
        pack_i(pc, &pat, W);
        pack_i(pc, &pat, 2);
        (void)k3d_render_paint(r, want, (size_t)W, true);

        {
            struct eu_run run = { paint_code, pn, 8, (uint32_t)(W + 7) / 8, (uint32_t)H, pb, 3, pc, 32, 0 };

            why = eu_emulate(&run, &st, &worst_at);
            snprintf(what, sizeof what, "the paint did not run: %s at %u", why ? why : "", worst_at);
            check(pn > 0 && why == NULL, what);
        }

        for (i = 0; i < (uint32_t)(W * H) && why == NULL; i++) {
            uint32_t got;
            int k, most = 0;

            memcpy(&got, &pb[2].bytes[i * 4], 4);

            for (k = 0; k < 24; k += 8) {
                int d = (int)((got >> k) & 0xff) - (int)((want[i] >> k) & 0xff);

                most = d < 0 ? -d : d > most ? d : most;
            }

            off += most > 0;
            apart = most > apart ? most : apart;
        }

        printf("  the paint: %d of %d pixels the processor's colour to the byte, at most %d apart\n",
               W * H - off, W * H, apart);
        snprintf(what, sizeof what, "the GPU's paint differs from the processor's by %d levels", apart);
        check(why == NULL && apart <= 1, what);

        for (i = 0; i < 3; i++) { free(pb[i].bytes); }
        free(want);
    }

    for (i = 0; i < 7; i++) {
        free(b[i].bytes);
    }

    k3d_gpu_scene_free(&g);
    k3d_render_free(r);
    k3d_scene_free(&sc);
}

int main(int argc, char **argv)
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

    if (argc > 1) {
        tracer(argv[1]);
        final_tracer(argv[1]);
    } else {
        check(0, "no kernels file for the tracer - tools/eu_kernels.lua writes it");
    }

    if (fails) {
        printf("FAIL: %d of %d checks on the GPU's scene\n", fails, checks + fails);
        return 1;
    }

    printf("PASS: %d checks on the GPU's scene (sixty objects laid out, the trees threaded, twenty thousand "
           "rays walked by skips meeting what the processor's tracer meets, and the tracer's kernel drawing "
           "the processor's Preview)\n", checks);
    return 0;
}
