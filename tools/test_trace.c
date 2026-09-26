/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The 3D Kit's ray tracer on the host (`roadmap.md` 4l, Cafesa3D's step
 * three), held to things that can be known without looking at a picture.
 *
 * The shapes: a box traced as a box meets every ray where its twelve
 * triangles do; a smooth sphere and cylinder where fine meshes of them do,
 * a hair nearer, since a mesh lies inside the shape it approximates.
 *
 * The hierarchy: sixty objects of every kind, each its triangles, and
 * twenty thousand rays through them - the four-wide hierarchy, its packets
 * of four triangles and its early outs must find exactly the nearest hit a
 * loop over every triangle in the scene finds, in double precision.
 *
 * The light, each against the arithmetic: a floor under a lamp is as bright
 * as P cos / 4 pi^2 d^2 says, right under it and two metres off; a box
 * between them puts it in shadow and a glass one does not; a mirror shows
 * the sky's zenith; a glass ball turns what is behind it the other way
 * round; and the path tracer's soft lamp, sampled over its cone, settles on
 * what Whitted's point says.
 *
 * And the threads: the same render drawn by one thread and by four is the
 * same, bit for bit, because nothing a sample does depends on who drew it.
 */

#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../user/kits/3d/k3d.h"

static int checks;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  not ok: %s\n", what);
    }
}

static double frand(unsigned *s)
{
    *s = *s * 1103515245u + 12345u;
    return (double)((*s >> 8) & 0xffffff) / 16777216.0;
}

static struct k3d_object *put(struct k3d_scene *s, enum k3d_kind kind, float x, float y, float z)
{
    struct k3d_object *o = k3d_scene_add(s, kind);

    o->loc[0] = x;
    o->loc[1] = y;
    o->loc[2] = z;
    return o;
}

static void material(struct k3d_object *o, float r, float g, float b, float metallic,
                     float rough, float trans, float emit)
{
    o->mat.base[0] = r;
    o->mat.base[1] = g;
    o->mat.base[2] = b;
    o->mat.metallic = metallic;
    o->mat.rough = rough;
    o->mat.trans = trans;
    o->mat.emit = emit;
    o->mat.ior = 1.5f;
}

static struct k3d_render_setup camera(int w, int h, float ex, float ey, float ez,
                                      float tx, float ty, float tz, float fov_degrees)
{
    struct k3d_render_setup how;

    memset(&how, 0, sizeof(how));
    how.w = w;
    how.h = h;
    how.eye[0] = ex; how.eye[1] = ey; how.eye[2] = ez;
    how.target[0] = tx; how.target[1] = ty; how.target[2] = tz;
    how.fov = fov_degrees * 3.14159265f / 180;
    how.preview = true;
    how.bounces = 6;
    how.passes = 1;
    return how;
}

/* Every job, taken by the one thread that asks. */
static void draw(struct k3d_render *r)
{
    uint32_t tile, pass;

    while (k3d_render_job(r, &tile, &pass, NULL)) {
        k3d_render_tile(r, tile, pass);
    }
}

/* The paint's own curve, so a pixel can be predicted from a radiance. */
static int tone(double v)
{
    double x, s;

    v *= 0.9;
    x = (v * (2.51 * v + 0.03)) / (v * (2.43 * v + 0.59) + 0.14);
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    s = x <= 0.0031308 ? x * 12.92 : 1.055 * pow(x, 1 / 2.4) - 0.055;
    return (int)(s * 255 + 0.5);
}

static int near_pixel(uint32_t px, double r, double g, double b, int slack)
{
    int pr = (int)((px >> 16) & 0xff), pg = (int)((px >> 8) & 0xff), pb = (int)(px & 0xff);

    return abs(pr - tone(r)) <= slack && abs(pg - tone(g)) <= slack && abs(pb - tone(b)) <= slack;
}

/*--------------------------------------------------------------------------
 * Shapes as they are, against their triangles.
 *------------------------------------------------------------------------*/

static struct k3d_render *alone(struct k3d_scene *s)
{
    struct k3d_render_setup how = camera(16, 16, 0, -8, 0, 0, 0, 0, 40);

    return k3d_render_new(s, &how);
}

static void random_ray(unsigned *seed, float o[3], float d[3])
{
    double th = frand(seed) * 6.2831853, z = frand(seed) * 2 - 1, rr = sqrt(1 - z * z);
    double tx = (frand(seed) - 0.5) * 3, ty = (frand(seed) - 0.5) * 3, tz = (frand(seed) - 0.5) * 3;
    double l;

    o[0] = (float)(7 * rr * cos(th));
    o[1] = (float)(7 * rr * sin(th));
    o[2] = (float)(7 * z);
    d[0] = (float)(tx - o[0]);
    d[1] = (float)(ty - o[1]);
    d[2] = (float)(tz - o[2]);
    l = sqrt((double)d[0] * d[0] + (double)d[1] * d[1] + (double)d[2] * d[2]);
    d[0] = (float)(d[0] / l);
    d[1] = (float)(d[1] / l);
    d[2] = (float)(d[2] / l);
}

/* One object traced as itself and as its triangles, by the same rays. */
static void against_mesh(enum k3d_kind kind, const char *name, float most_inside)
{
    struct k3d_scene a, b;
    struct k3d_object *o;
    struct k3d_render *ra, *rb;
    unsigned seed = 7;
    int i, rays = 5000, disagree = 0, outside = 0, hits = 0;
    char what[160];

    k3d_scene_init(&a);
    k3d_scene_init(&b);

    for (i = 0; i < 2; i++) {
        o = put(i ? &b : &a, kind, 0.3f, -0.2f, 0.1f);
        o->rot[0] = 20; o->rot[1] = 30; o->rot[2] = 40;
        o->scale[0] = 1; o->scale[1] = 1.6f; o->scale[2] = 0.7f;
        o->size[0] = 2; o->size[1] = 1; o->size[2] = 1.5f;
        o->smooth = true;
        o->segments = kind == K3D_BOX ? o->segments : 512;
        o->rings = 256;
        o->faceted = i == 1;
    }

    ra = alone(&a);
    rb = alone(&b);

    for (i = 0; i < rays; i++) {
        float or[3], d[3], ta = 0, tb = 0;
        uint32_t ida, idb;
        bool ha, hb;

        random_ray(&seed, or, d);
        ha = k3d_render_first_hit(ra, or, d, &ta, &ida);
        hb = k3d_render_first_hit(rb, or, d, &tb, &idb);

        if (ha != hb) {
            disagree++;
        } else if (ha) {
            hits++;

            /* A mesh is inside its shape, so a ray meets it later - by no
             * more than the gap between a facet and the curve. */
            if (tb < ta - 1e-3f || tb > ta + most_inside) {
                outside++;
            }
        }
    }

    snprintf(what, sizeof(what), "a %s traced as itself meets %d rays as its triangles do: "
             "%d disagree on hitting, %d of %d hits differ by more than %.4f", name, rays,
             disagree, outside, hits, (double)most_inside);
    check(disagree <= rays / 500 && outside == 0 && hits > rays / 4, what);

    k3d_render_free(ra);
    k3d_render_free(rb);
    k3d_scene_free(&a);
    k3d_scene_free(&b);
}

/*--------------------------------------------------------------------------
 * The four-wide hierarchy, against every triangle.
 *------------------------------------------------------------------------*/

static double tri_hit(const double o[3], const double d[3], const double a[3], const double b[3],
                      const double c[3])
{
    double e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    double e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    double p[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2],
                    d[0] * e2[1] - d[1] * e2[0] };
    double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2], u, v, t;
    double s[3] = { o[0] - a[0], o[1] - a[1], o[2] - a[2] }, q[3];

    if (fabs(det) < 1e-14) {
        return -1;
    }

    u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) / det;
    q[0] = s[1] * e1[2] - s[2] * e1[1];
    q[1] = s[2] * e1[0] - s[0] * e1[2];
    q[2] = s[0] * e1[1] - s[1] * e1[0];
    v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) / det;
    t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det;

    return (u >= 0 && v >= 0 && u + v <= 1 && t > 1e-4) ? t : -1;
}

static void hierarchy(void)
{
    static const enum k3d_kind kinds[] = { K3D_PLANE, K3D_BOX, K3D_SPHERE, K3D_CYLINDER,
                                           K3D_ICO, K3D_CONE, K3D_TORUS, K3D_GRID };
    struct k3d_scene s;
    struct k3d_render *r;
    unsigned seed = 42;
    uint32_t i, j, k, tris = 0;
    int rays = 20000, missed = 0, invented = 0, wrong = 0, hits = 0;
    double *world;
    uint32_t *owner;
    char what[200];

    k3d_scene_init(&s);

    for (i = 0; i < 60; i++) {
        struct k3d_object *o = put(&s, kinds[i % 8], (float)(frand(&seed) * 8 - 4),
                                   (float)(frand(&seed) * 8 - 4), (float)(frand(&seed) * 8 - 4));

        o->rot[0] = (float)(frand(&seed) * 360);
        o->rot[1] = (float)(frand(&seed) * 360);
        o->rot[2] = (float)(frand(&seed) * 360);
        o->scale[0] = (float)(0.3 + frand(&seed));
        o->scale[1] = (float)(0.3 + frand(&seed));
        o->scale[2] = (float)(0.3 + frand(&seed));
        o->smooth = i % 3 == 0;
        o->faceted = true;
    }

    {
        struct k3d_render_setup how = camera(16, 16, 0, -20, 0, 0, 0, 0, 40);

        r = k3d_render_new(&s, &how);       /* which also builds every mesh */
    }

    for (i = 0; i < s.count; i++) {
        tris += s.obj[i].mesh.ntris;
    }

    world = malloc((size_t)tris * 9 * sizeof(double));
    owner = malloc(tris * sizeof(uint32_t));
    tris = 0;

    for (i = 0; i < s.count; i++) {
        const struct k3d_object *o = &s.obj[i];
        float M[12];

        k3d_object_matrix(o, M);

        for (j = 0; j < o->mesh.ntris; j++) {
            for (k = 0; k < 3; k++) {
                const float *p = &o->mesh.pos[o->mesh.tri[j * 3 + k] * 3];
                double *w = &world[(size_t)tris * 9 + k * 3];

                w[0] = (double)M[0] * p[0] + (double)M[1] * p[1] + (double)M[2] * p[2] + M[3];
                w[1] = (double)M[4] * p[0] + (double)M[5] * p[1] + (double)M[6] * p[2] + M[7];
                w[2] = (double)M[8] * p[0] + (double)M[9] * p[1] + (double)M[10] * p[2] + M[11];
            }

            owner[tris++] = o->id;
        }
    }

    for (i = 0; i < (uint32_t)rays; i++) {
        float of[3], df[3], t = 0;
        double od[3], dd[3], best = 1e30, mine = 1e30;
        uint32_t id = 0;
        bool hit;

        random_ray(&seed, of, df);
        of[0] *= 1.4f; of[1] *= 1.4f; of[2] *= 1.4f;

        for (k = 0; k < 3; k++) {
            od[k] = of[k];
            dd[k] = df[k];
        }

        hit = k3d_render_first_hit(r, of, df, &t, &id);

        for (j = 0; j < tris; j++) {
            double th = tri_hit(od, dd, &world[(size_t)j * 9], &world[(size_t)j * 9 + 3],
                                &world[(size_t)j * 9 + 6]);

            if (th > 0 && th < best) {
                best = th;
            }

            if (hit && owner[j] == id && th > 0 && th < mine) {
                mine = th;
            }
        }

        if (best < 1e30 && !hit) {
            missed++;
        } else if (best >= 1e30 && hit) {
            invented++;
        } else if (hit) {
            hits++;

            /* The nearest, and on the object it names - which may be a
             * different object from the loop's when two are within a hair. */
            if (fabs(t - best) > 1e-3 * (1 + best) || fabs(t - mine) > 1e-3 * (1 + mine)) {
                wrong++;
            }
        }
    }

    snprintf(what, sizeof(what), "the four-wide hierarchy finds what every one of %u "
             "triangles does, for %d rays: %d missed, %d invented, %d of %d hits not the "
             "nearest", tris, rays, missed, invented, wrong, hits);
    check(missed + invented <= rays / 2000 && wrong == 0 && hits > rays / 4, what);
    printf("  %u triangles in 60 objects; %d of %d rays hit\n", tris, hits, rays);

    free(world);
    free(owner);
    k3d_render_free(r);
    k3d_scene_free(&s);
}

/*--------------------------------------------------------------------------
 * Light.
 *------------------------------------------------------------------------*/

#define LW 64
#define LH 64
static uint32_t picture[LH * LW];

/* A floor looked down on from ten metres, a lamp above it, no sky. */
static struct k3d_render *lit_floor(struct k3d_scene *s, const struct k3d_light *lamp,
                                    bool preview, uint32_t passes)
{
    struct k3d_render_setup how = camera(LW, LH, 0, 0, 10, 0, 0, 0, 90);
    struct k3d_object *floor = put(s, K3D_PLANE, 0, 0, 0);
    struct k3d_render *r;

    floor->size[0] = 40;
    material(floor, 0.8f, 0.8f, 0.8f, 0, 1, 0, 0);
    how.lights = lamp;
    how.nlights = 1;
    how.preview = preview;
    how.passes = passes;
    r = k3d_render_new(s, &how);
    draw(r);
    memset(picture, 0, sizeof(picture));
    k3d_render_paint(r, picture, LW, true);
    return r;
}

/* Where on the floor a pixel's middle looks, from ten metres with a right
 * angle across: the camera's right is +x and its up +y. */
static double floor_x(int px) { return (px + 0.5 - LW / 2.0) / (LW / 2.0) * 10; }
static double floor_y(int py) { return -(py + 0.5 - LH / 2.0) / (LW / 2.0) * 10; }

/* Whitted's floor under a lamp at `lx, lz`: base P cos / (4 pi^2 d^2). */
static double expected(int px, int py, double lx, double lz, double power)
{
    double dx = lx - floor_x(px), dy = -floor_y(py), d2 = dx * dx + dy * dy + lz * lz;

    return 0.8 * power * (lz / sqrt(d2)) / (4 * 3.14159265358979 * 3.14159265358979 * d2);
}

static void light(void)
{
    struct k3d_light lamp = { { 0, 0, 4 }, 0.1f, { 1, 1, 1 }, 1000 };
    struct k3d_scene s;
    struct k3d_render *r;
    struct k3d_object *o;
    char what[200];
    double e;

    k3d_scene_init(&s);
    r = lit_floor(&s, &lamp, true, 1);
    e = expected(32, 32, 0, 4, 1000);
    snprintf(what, sizeof(what), "the floor under a lamp is as bright as P cos / 4 pi^2 d^2: "
             "%06x, expected %d (%.3f)", picture[32 * LW + 32] & 0xffffff, tone(e), e);
    check(near_pixel(picture[32 * LW + 32], e, e, e, 1), what);
    e = expected(44, 30, 0, 4, 1000);
    snprintf(what, sizeof(what), "and off to one side, by the cosine and the distance: %06x, "
             "expected %d (%.3f)", picture[30 * LW + 44] & 0xffffff, tone(e), e);
    check(near_pixel(picture[30 * LW + 44], e, e, e, 1), what);
    k3d_render_free(r);
    k3d_scene_free(&s);

    /* A box between the lamp and the floor's middle: shadow, exactly. */
    lamp.pos[0] = 4;
    k3d_scene_init(&s);
    o = put(&s, K3D_BOX, 2, 0, 2);
    o->size[0] = o->size[1] = o->size[2] = 0.6f;
    r = lit_floor(&s, &lamp, true, 1);
    check((picture[32 * LW + 32] & 0xffffff) == 0, "a box between the lamp and the floor "
          "puts it in shadow");
    e = expected(32, 16, 4, 4, 1000);
    check(near_pixel(picture[16 * LW + 32], e, e, e, 1), "and the floor beside the shadow is lit");
    k3d_render_free(r);
    k3d_scene_free(&s);

    /* Made of glass, the same box lets the lamp through. */
    k3d_scene_init(&s);
    o = put(&s, K3D_BOX, 2, 0, 2);
    o->size[0] = o->size[1] = o->size[2] = 0.6f;
    material(o, 1, 1, 1, 0, 0, 1, 0);
    r = lit_floor(&s, &lamp, true, 1);
    e = expected(32, 32, 4, 4, 1000);
    snprintf(what, sizeof(what), "a glass box does not: %06x, expected %d",
             picture[32 * LW + 32] & 0xffffff, tone(e));
    check(near_pixel(picture[32 * LW + 32], e, e, e, 1), what);
    k3d_render_free(r);
    k3d_scene_free(&s);

    /* The path tracer's lamp is a sphere sampled over its cone, which is
     * exact for a sphere: with enough passes it settles on the point. */
    lamp.pos[0] = 0;
    lamp.radius = 0.5f;
    k3d_scene_init(&s);
    r = lit_floor(&s, &lamp, false, 64);
    e = expected(32, 32, 0, 4, 1000);
    snprintf(what, sizeof(what), "the path tracer's soft lamp settles where Whitted's point "
             "is: %06x after 64 passes, expected %d", picture[32 * LW + 32] & 0xffffff, tone(e));
    check(near_pixel(picture[32 * LW + 32], e, e, e, 2), what);
    k3d_render_free(r);
    k3d_scene_free(&s);
}

static void mirror_and_glass(void)
{
    struct k3d_render_setup how = camera(LW, LH, 0, 0, 10, 0, 0, 0, 90);
    struct k3d_scene s;
    struct k3d_render *r;
    struct k3d_object *o;
    uint32_t px;
    char what[200];

    how.world.zenith[0] = 0.2f; how.world.zenith[1] = 0.4f; how.world.zenith[2] = 0.8f;
    how.world.horizon[0] = 0.9f; how.world.horizon[1] = 0.9f; how.world.horizon[2] = 0.9f;
    how.world.strength = 1;

    k3d_scene_init(&s);
    o = put(&s, K3D_PLANE, 0, 0, 0);
    o->size[0] = 40;
    material(o, 1, 1, 1, 1, 0, 0, 0);
    r = k3d_render_new(&s, &how);
    draw(r);
    k3d_render_paint(r, picture, LW, true);
    px = picture[32 * LW + 32];
    snprintf(what, sizeof(what), "a mirror looked straight down on shows the zenith: %06x",
             px & 0xffffff);
    check(near_pixel(px, 0.2, 0.4, 0.8, 1), what);
    k3d_render_free(r);
    k3d_scene_free(&s);

    /* A ball of glass in front of a red wall on the left and a blue one on
     * the right: through the ball the two change places. */
    how = camera(LW, LH, 0, -6, 0, 0, 0, 0, 40);
    k3d_scene_init(&s);
    o = put(&s, K3D_SPHERE, 0, 0, 0);
    o->smooth = true;
    material(o, 1, 1, 1, 0, 0, 1, 0);
    o = put(&s, K3D_BOX, -2, 3, 0);
    o->size[0] = 4; o->size[1] = 0.2f; o->size[2] = 8;
    material(o, 1, 0, 0, 0, 1, 0, 1);
    o = put(&s, K3D_BOX, 2, 3, 0);
    o->size[0] = 4; o->size[1] = 0.2f; o->size[2] = 8;
    material(o, 0, 0, 1, 0, 1, 0, 1);
    r = k3d_render_new(&s, &how);
    draw(r);
    k3d_render_paint(r, picture, LW, true);
    check(near_pixel(picture[32 * LW + 12], 1, 0, 0, 1), "beside the ball, the red wall is on the left");
    check(near_pixel(picture[32 * LW + 52], 0, 0, 1, 1), "and the blue one on the right");
    snprintf(what, sizeof(what), "through the ball the blue is on the left: %06x",
             picture[32 * LW + 25] & 0xffffff);
    check(near_pixel(picture[32 * LW + 25], 0, 0, 1, 1), what);
    snprintf(what, sizeof(what), "and the red on the right: %06x", picture[32 * LW + 39] & 0xffffff);
    check(near_pixel(picture[32 * LW + 39], 1, 0, 0, 1), what);
    k3d_render_free(r);

    /* The control is in the scene: the same ball, not glass, hides both. */
    s.obj[0].mat.trans = 0;
    r = k3d_render_new(&s, &how);
    draw(r);
    k3d_render_paint(r, picture, LW, true);
    check(!near_pixel(picture[32 * LW + 25], 0, 0, 1, 8) && !near_pixel(picture[32 * LW + 25], 1, 0, 0, 8),
          "and a ball of plaster shows neither");
    k3d_render_free(r);
    k3d_scene_free(&s);
}

/*--------------------------------------------------------------------------
 * Threads.
 *------------------------------------------------------------------------*/

static void yield(void) { sched_yield(); }

static void *worker(void *arg)
{
    struct k3d_render *r = arg;
    uint32_t tile, pass;

    while (k3d_render_job(r, &tile, &pass, yield)) {
        k3d_render_tile(r, tile, pass);
    }

    return NULL;
}

static void a_still_life(struct k3d_scene *s)
{
    struct k3d_object *o;

    k3d_scene_init(s);
    o = put(s, K3D_PLANE, 0, 0, 0);
    o->size[0] = 30;
    material(o, 0.7f, 0.7f, 0.65f, 0, 0.6f, 0, 0);
    o = put(s, K3D_SPHERE, -1.2f, 0, 1);
    o->smooth = true;
    material(o, 0.9f, 0.9f, 0.9f, 1, 0.15f, 0, 0);
    o = put(s, K3D_TORUS, 1.3f, 0.4f, 0.5f);
    o->smooth = true;
    o->rot[0] = 60;
    material(o, 0.8f, 0.2f, 0.1f, 0, 0.3f, 0, 0);
    o = put(s, K3D_ICO, 0.2f, -1.5f, 0.6f);
    o->radius = 0.6f;
    material(o, 1, 1, 1, 0, 0, 1, 0);
    o = put(s, K3D_CONE, 0.5f, 1.8f, 1);
    material(o, 0.2f, 0.5f, 0.9f, 0, 0.8f, 0, 0);
    o = put(s, K3D_BOX, -2.5f, 2, 0.5f);
    o->rot[2] = 30;
    material(o, 0.95f, 0.8f, 0.3f, 0, 0.4f, 0, 0);
}

static double seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void threads(void)
{
    static uint32_t one[192 * 128], four[192 * 128];
    struct k3d_light lamp = { { 3, -4, 6 }, 0.5f, { 1, 0.95f, 0.9f }, 3000 };
    struct k3d_render_setup how = camera(192, 128, 6, -7, 4, 0, 0, 0.6f, 50);
    struct k3d_scene s;
    struct k3d_render *r1, *r4;
    pthread_t th[4];
    uint64_t rays1, rays4;
    double t0, t1, t4;
    int i;
    char what[200];

    a_still_life(&s);
    how.preview = false;
    how.passes = 8;
    how.lights = &lamp;
    how.nlights = 1;
    how.world.zenith[0] = 0.25f; how.world.zenith[1] = 0.4f; how.world.zenith[2] = 0.75f;
    how.world.horizon[0] = 0.8f; how.world.horizon[1] = 0.85f; how.world.horizon[2] = 0.9f;
    how.world.strength = 0.6f;

    r1 = k3d_render_new(&s, &how);
    t0 = seconds();
    draw(r1);
    t1 = seconds() - t0;
    snprintf(what, sizeof(what), "painting only what moved on paints all %u tiles the "
             "first time", 12u * 8u);
    check(k3d_render_paint(r1, one, 192, false) == 12 * 8, what);
    check(k3d_render_paint(r1, one, 192, false) == 0, "and none when nothing has");
    rays1 = k3d_render_rays(r1);

    r4 = k3d_render_new(&s, &how);
    t0 = seconds();

    for (i = 0; i < 4; i++) {
        pthread_create(&th[i], NULL, worker, r4);
    }

    for (i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
    }

    t4 = seconds() - t0;
    k3d_render_paint(r4, four, 192, true);
    rays4 = k3d_render_rays(r4);

    snprintf(what, sizeof(what), "one thread and four draw the same picture, bit for bit, "
             "with the same %llu rays", (unsigned long long)rays1);
    check(memcmp(one, four, sizeof(one)) == 0 && rays1 == rays4, what);
    check(k3d_render_passes(r4) == 8, "and every tile had its eight passes");
    printf("  %llu rays: %.2f Mrays/s on one thread, %.2f on four (%.1fx)\n",
           (unsigned long long)rays1, (double)rays1 / t1 / 1e6, (double)rays4 / t4 / 1e6, t1 / t4);

    /* Stopped, the workers take no more. */
    k3d_render_free(r4);
    r4 = k3d_render_new(&s, &how);
    k3d_render_stop(r4);
    {
        uint32_t tile, pass;

        check(!k3d_render_job(r4, &tile, &pass, NULL) && k3d_render_passes(r4) == 0,
              "a stopped render gives no more jobs");
    }

    k3d_render_free(r1);
    k3d_render_free(r4);
    k3d_scene_free(&s);
}

/*--------------------------------------------------------------------------
 * Textures, and a mesh of the kit's own.
 *------------------------------------------------------------------------*/

/* A wall facing -y, and a floor facing up: which way a surface faces is
 * what a brick is laid by. */
static const float WALL[3] = { 0, -1, 0 };
static const float FLOOR[3] = { 0, 0, 1 };

static void patterns(void)
{
    struct k3d_texture t;
    float fac, h, fac2, h2, lo = 1e9f, hi = -1e9f, sum = 0, jump = 0;
    char what[160];
    unsigned seed = 3;
    int i, same = 1;

    memset(&t, 0, sizeof(t));
    t.pattern = K3D_CHECKER;
    t.scale = 2;
    {
        float a[3] = { 0.1f, 0.1f, 0.1f }, b[3] = { 0.6f, 0.1f, 0.1f }, c[3] = { 0.6f, 0.6f, 0.1f };

        k3d_pattern(&t, a, WALL, &fac, &h);
        k3d_pattern(&t, b, WALL, &fac2, &h2);
        check(fac != fac2, "a checker changes colour half a metre along, at two a metre");
        k3d_pattern(&t, c, WALL, &fac2, &h2);
        check(fac == fac2, "and back again half a metre across");
    }

    /* Bricks 0.1 tall, 0.2 long, a tenth mortar, each row half along. */
    t.pattern = K3D_BRICK;
    t.scale = 10;
    t.ratio = 2;
    t.mortar = 0.1f;
    t.offset = 0.5f;
    {
        float joint[3] = { 0.1f, 0, 0.1f };      /* on the bed joint between rows */
        float middle[3] = { 0.1f, 0, 0.05f };    /* the middle of a brick */
        float head[3] = { 0.2f, 0, 0.05f };      /* the head joint in row 0 */
        float above[3] = { 0.2f, 0, 0.15f };     /* the same place, row 1: mid-brick */

        k3d_pattern(&t, joint, WALL, &fac, &h);
        check(fac == 1 && h == 0, "the bed joint between two rows is mortar, and low");
        k3d_pattern(&t, middle, WALL, &fac, &h);
        check(fac < 0.2f && h > 0.99f, "the middle of a brick is brick, and high");
        k3d_pattern(&t, head, WALL, &fac, &h);
        check(fac == 1, "the joint at the end of a brick is mortar");
        k3d_pattern(&t, above, WALL, &fac, &h);
        check(fac < 0.2f && h > 0.99f, "and the row above is half a brick along, so there it "
              "is the middle of one");
    }

    /* A floor is paved in x and y: the course is along y, so a point on
     * the joint between two rows of paving is mortar - where on a wall at
     * the same height it would be the middle of a brick. */
    {
        float paved[3] = { 0, 0.1f, 0.05f };

        k3d_pattern(&t, paved, FLOOR, &fac, &h);
        check(fac == 1, "a floor is paved in x and y: between two of its rows is mortar");
        k3d_pattern(&t, paved, WALL, &fac, &h);
        check(fac < 0.2f, "and on a wall the same point is the middle of a brick");
    }

    /* Noise: the same for the same point, about nought, within one, and
     * continuous - a step of a tenth of a millimetre moves it little. */
    for (i = 0; i < 20000; i++) {
        float p[3] = { (float)(frand(&seed) * 50 - 25), (float)(frand(&seed) * 50 - 25),
                       (float)(frand(&seed) * 50 - 25) };
        float q[3] = { p[0] + 1e-4f, p[1], p[2] };
        float n = k3d_noise(p, 3), m = k3d_noise(q, 3);

        same = same && n == k3d_noise(p, 3);
        lo = n < lo ? n : lo;
        hi = n > hi ? n : hi;
        sum += n;
        jump = fabsf(n - m) > jump ? fabsf(n - m) : jump;
    }

    snprintf(what, sizeof(what), "noise is the same twice, within one, about nought and "
             "continuous: %.3f to %.3f, mean %.4f, worst step %.5f", (double)lo, (double)hi,
             (double)(sum / 20000), (double)jump);
    check(same && lo > -1 && hi < 1 && hi - lo > 0.8f && fabsf(sum / 20000) < 0.05f
          && jump < 0.01f, what);

    /* Wood: a ring a metre out from the axis at one ring a metre is the
     * same as two metres out, with no noise to push it. */
    t.pattern = K3D_WOOD;
    t.scale = 1;
    t.distortion = 0;
    {
        float a[3] = { 1, 0, 0 }, b[3] = { 0, 2, 5 };

        k3d_pattern(&t, a, WALL, &fac, &h);
        k3d_pattern(&t, b, WALL, &fac2, &h2);
        check(fabsf(fac - fac2) < 1e-4f, "wood's rings repeat outwards from its upright");
    }
}

/* A floor looked down on, lit from straight above, and how many of its
 * pixels are each of two colours. */
static void textured_floor(void)
{
    struct k3d_light lamp = { { 0, 0, 30 }, 0.1f, { 1, 1, 1 }, 60000 };
    struct k3d_render_setup how = camera(LW, LH, 0, 0, 10, 0, 0, 0, 90);
    struct k3d_scene s;
    struct k3d_render *r;
    struct k3d_object *o;
    int x, y, bright = 0, dark = 0, flat, lit;
    char what[160];

    how.lights = &lamp;
    how.nlights = 1;

    k3d_scene_init(&s);
    o = put(&s, K3D_PLANE, 0, 0, 0);
    o->size[0] = 40;
    material(o, 0.8f, 0.8f, 0.8f, 0, 1, 0, 0);
    o->mat.tex.pattern = K3D_CHECKER;
    o->mat.tex.scale = 0.5f;                /* squares two metres across */
    o->mat.tex.colour2[0] = o->mat.tex.colour2[1] = o->mat.tex.colour2[2] = 0.05f;
    r = k3d_render_new(&s, &how);
    draw(r);
    k3d_render_paint(r, picture, LW, true);

    for (y = 0; y < LH; y++) {
        for (x = 0; x < LW; x++) {
            int g = (int)((picture[y * LW + x] >> 8) & 0xff);

            bright += g > 150;              /* 0.8 under the lamp */
            dark += g < 110;                /* 0.05, which tones to about 82 */
        }
    }

    snprintf(what, sizeof(what), "a checker floor looked down on is both colours, about "
             "half each: %d light, %d dark of %d", bright, dark, LW * LH);
    check(bright > LW * LH / 3 && dark > LW * LH / 3, what);
    k3d_render_free(r);

    /* A brick wall - a box standing up, as a wall in a scene is, since
     * bricks course by height - seen face on and lit from beside it: with
     * bump the mortar is a groove the light cannot reach into, and the wall
     * is no longer one colour. */
    k3d_scene_free(&s);
    k3d_scene_init(&s);
    o = put(&s, K3D_BOX, 0, 0, 0);
    o->size[0] = 16;
    o->size[1] = 0.2f;
    o->size[2] = 16;
    material(o, 0.8f, 0.8f, 0.8f, 0, 1, 0, 0);
    o->mat.tex.pattern = K3D_BRICK;
    o->mat.tex.scale = 1;                   /* rows a metre tall, to be seen */
    o->mat.tex.ratio = 2;
    o->mat.tex.mortar = 0.15f;
    o->mat.tex.offset = 0.5f;
    o->mat.tex.colour2[0] = o->mat.tex.colour2[1] = o->mat.tex.colour2[2] = 0.8f;
    lamp.pos[0] = 40;
    lamp.pos[1] = -4;
    lamp.pos[2] = 0;
    how = camera(LW, LH, 0, -10, 0, 0, 0, 0, 90);
    how.lights = &lamp;
    how.nlights = 1;

    for (lit = 0; lit < 2; lit++) {
        int n = 0, least = 255, most = 0;

        o->mat.tex.bump = lit ? 0.05f : 0;
        r = k3d_render_new(&s, &how);
        draw(r);
        k3d_render_paint(r, picture, LW, true);

        for (y = 8; y < LH - 8; y++) {
            for (x = 8; x < LW - 8; x++) {
                int g = (int)((picture[y * LW + x] >> 8) & 0xff);

                least = g < least ? g : least;
                most = g > most ? g : most;
                n++;
            }
        }

        if (!lit) {
            flat = most - least;
        } else {
            snprintf(what, sizeof(what), "bump makes mortar a groove under a raking light: "
                     "the wall spans %d levels with it, %d without", most - least, flat);
            check(most - least > flat + 40, what);
        }

        (void)n;
        k3d_render_free(r);
    }

    k3d_scene_free(&s);
}

/* The kit's own mesh of a cube, and a box: the same shape, met alike. */
static void mesh_as_box(void)
{
    static const float pos[] = {
        -1, -1, -1,   1, -1, -1,   1, 1, -1,   -1, 1, -1,
        -1, -1,  1,   1, -1,  1,   1, 1,  1,   -1, 1,  1,
    };
    static const uint32_t tri[] = {
        0, 2, 1,  0, 3, 2,  4, 5, 6,  4, 6, 7,  0, 1, 5,  0, 5, 4,
        2, 3, 7,  2, 7, 6,  1, 2, 6,  1, 6, 5,  3, 0, 4,  3, 4, 7,
    };
    struct k3d_scene a, b;
    struct k3d_render *ra, *rb;
    struct k3d_object *o;
    unsigned seed = 11;
    int i, differ = 0, hits = 0;
    char what[160];

    k3d_scene_init(&a);
    k3d_scene_init(&b);
    o = put(&a, K3D_BOX, 0.2f, 0.1f, -0.3f);
    o->rot[2] = 30;
    o->scale[0] = 1.5f;
    o = put(&b, K3D_MESH, 0.2f, 0.1f, -0.3f);
    o->rot[2] = 30;
    o->scale[0] = 1.5f;
    check(k3d_mesh_set(o, pos, 8, tri, 12, 30), "a mesh of a cube is taken");
    ra = alone(&a);
    rb = alone(&b);

    for (i = 0; i < 5000; i++) {
        float or[3], d[3], ta = 0, tb = 0;
        uint32_t ida, idb;
        bool ha, hb;

        random_ray(&seed, or, d);
        ha = k3d_render_first_hit(ra, or, d, &ta, &ida);
        hb = k3d_render_first_hit(rb, or, d, &tb, &idb);
        hits += ha;
        differ += ha != hb || (ha && fabsf(ta - tb) > 1e-3f);
    }

    snprintf(what, sizeof(what), "a mesh of a cube meets rays where a box does: %d of 5000 "
             "differ, %d hit", differ, hits);
    check(differ <= 5 && hits > 1000, what);
    k3d_render_free(ra);
    k3d_render_free(rb);
    k3d_scene_free(&a);
    k3d_scene_free(&b);
}

int main(void)
{
    against_mesh(K3D_BOX, "box", 1e-3f);
    against_mesh(K3D_PLANE, "plane", 1e-3f);
    against_mesh(K3D_SPHERE, "sphere", 0.02f);
    against_mesh(K3D_CYLINDER, "cylinder", 0.02f);
    hierarchy();
    light();
    mirror_and_glass();
    patterns();
    textured_floor();
    mesh_as_box();
    threads();

    if (fails) {
        printf("FAIL: %d of %d checks on the ray tracer\n", fails, checks + fails);
        return 1;
    }

    printf("PASS: %d checks on the ray tracer (shapes traced as themselves against their "
           "triangles; the four-wide hierarchy against every triangle; light, shadow, glass, "
           "a mirror and a lens against the arithmetic; checker, brick, noise and wood "
           "worked out, a checker floor in both colours and mortar a groove under raking "
           "light; a mesh of a cube met where a box is; one thread and four alike)\n", checks);
    return 0;
}
