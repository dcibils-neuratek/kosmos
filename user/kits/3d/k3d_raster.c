/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The 3D Kit's rasteriser: the Solid and Wireframe views.
 *
 * What Blender calls Workbench - the view a scene is arranged in, not the
 * one it is judged in. Triangles with a depth buffer, lit by a studio light
 * that sits over the viewer's left shoulder so a shape reads from any side;
 * the floor's grid behind whatever stands on it; and the selection outlined
 * in orange, which is the one thing the object buffer is for besides
 * picking.
 *
 * **Depth is 1/z.** A triangle's 1/z is linear across the screen and its z
 * is not, so 1/z can be interpolated with the same weights as the edges and
 * be exact; and it makes "nothing here" nought and "nearer" larger, so the
 * buffer is cleared with a memset.
 *
 * **Nothing here allocates while drawing** except to grow the scratch that
 * holds one object's vertices, which happens when a larger shape than any
 * before is drawn and then not again.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "k3d.h"

#define NEAR 0.05f

static float lo2(float a, float b) { return a < b ? a : b; }
static float hi2(float a, float b) { return a > b ? a : b; }

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const float a[3], const float b[3], float o[3])
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

static void norm3(float a[3])
{
    float l = sqrtf(dot3(a, a));

    if (l > 0) {
        a[0] /= l; a[1] /= l; a[2] /= l;
    }
}

bool k3d_view_init(struct k3d_view *v, int w, int h)
{
    memset(v, 0, sizeof(*v));

    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        return false;
    }

    v->w = w;
    v->h = h;
    v->depth = calloc((size_t)w * (size_t)h, sizeof(float));
    v->ids = calloc((size_t)w * (size_t)h, sizeof(uint32_t));

    if (v->depth == NULL || v->ids == NULL) {
        k3d_view_free(v);
        return false;
    }

    return true;
}

void k3d_view_free(struct k3d_view *v)
{
    free(v->depth);
    free(v->ids);
    free(v->world);
    free(v->camv);
    free(v->shade);
    memset(v, 0, sizeof(*v));
}

/*
 * The camera at `eye`, looking at `target`, `fov` radians across the width.
 * Up is the world's +Z, as Blender's is; looking straight down it would be
 * undefined, so the caller keeps the elevation short of the pole.
 */
void k3d_view_look(struct k3d_view *v, const float eye[3],
                   const float target[3], float fov)
{
    struct k3d_camera *c = &v->cam;
    static const float up[3] = { 0, 0, 1 };

    memcpy(c->eye, eye, sizeof(c->eye));
    c->f[0] = target[0] - eye[0];
    c->f[1] = target[1] - eye[1];
    c->f[2] = target[2] - eye[2];
    norm3(c->f);
    cross3(c->f, up, c->r);
    norm3(c->r);
    cross3(c->r, c->f, c->u);
    c->F = ((float)v->w / 2) / (float)tan((double)fov / 2);
    c->cx = (float)v->w / 2;
    c->cy = (float)v->h / 2;
    c->near = NEAR;
}

static void to_camera(const struct k3d_camera *c, const float p[3], float o[3])
{
    float d[3] = { p[0] - c->eye[0], p[1] - c->eye[1], p[2] - c->eye[2] };

    o[0] = dot3(d, c->r);
    o[1] = dot3(d, c->u);
    o[2] = dot3(d, c->f);
}

bool k3d_project(const struct k3d_view *v, const float p[3],
                 float *sx, float *sy, float *iz)
{
    const struct k3d_camera *c = &v->cam;
    float q[3];

    to_camera(c, p, q);

    if (q[2] < c->near) {
        return false;
    }

    *sx = c->cx + q[0] / q[2] * c->F;
    *sy = c->cy - q[1] / q[2] * c->F;
    *iz = 1.0f / q[2];
    return true;
}

uint32_t k3d_pick(const struct k3d_view *v, int x, int y)
{
    if (x < 0 || y < 0 || x >= v->w || y >= v->h) {
        return 0;
    }

    return v->ids[(size_t)y * (size_t)v->w + (size_t)x];
}

/*--------------------------------------------------------------------------
 * Colours, as 0xRRGGBB, and the arithmetic on them.
 *------------------------------------------------------------------------*/

static uint32_t scale_rgb(uint32_t c, float k)
{
    int r = (int)((float)((c >> 16) & 0xff) * k + 0.5f);
    int g = (int)((float)((c >> 8) & 0xff) * k + 0.5f);
    int b = (int)((float)(c & 0xff) * k + 0.5f);

    if (r > 255) { r = 255; }
    if (g > 255) { g = 255; }
    if (b > 255) { b = 255; }

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* `over` on `under` at `a` of 255. */
static uint32_t blend(uint32_t under, uint32_t over, unsigned a)
{
    unsigned na = 255 - a;
    uint32_t r = ((((under >> 16) & 0xff) * na + ((over >> 16) & 0xff) * a) / 255);
    uint32_t g = ((((under >> 8) & 0xff) * na + ((over >> 8) & 0xff) * a) / 255);
    uint32_t b = (((under & 0xff) * na + (over & 0xff) * a) / 255);

    return (r << 16) | (g << 8) | b;
}

/*
 * The studio light: a key over the viewer's left shoulder, a little from the
 * sky, and enough ambient that the side away from it is dim and not black.
 * In camera space, so it follows the view as Workbench's studio light does.
 */
static float studio(const struct k3d_camera *c, const float n_world[3])
{
    static const float key[3] = { -0.47f, 0.75f, -0.46f };  /* unit length */
    float n[3] = { dot3(n_world, c->r), dot3(n_world, c->u),
                   dot3(n_world, c->f) };
    float k = dot3(n, key), sky = n_world[2];

    return 0.30f + 0.62f * (k > 0 ? k : 0) + 0.10f * (sky > 0 ? sky : 0);
}

/*--------------------------------------------------------------------------
 * Triangles.
 *------------------------------------------------------------------------*/

/* A vertex on its way to the screen: camera space, then pixels. */
struct cv {
    float x, y, z;          /* camera space */
    float r, g, b;          /* its colour, 0..255, for smooth shading */
};

struct sv {
    float x, y, iz;
    float r, g, b;
};

struct tri_state {
    struct k3d_view   *v;
    const struct k3d_target *t;
    uint32_t id;
    uint32_t flat;          /* the face's colour when not smooth */
    bool     smooth;
    bool     colour;        /* write colour, or only depth and object */
    unsigned alpha;         /* 255 opaque; less is drawn through */
    uint32_t drawn;
};

static void raster(struct tri_state *st, const struct sv *a,
                   const struct sv *b, const struct sv *c)
{
    struct k3d_view *v = st->v;
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    const struct sv *p0 = a, *p1 = b, *p2 = c;
    float minx, maxx, miny, maxy, inv;
    int x0, x1, y0, y1, x, y;

    if (area == 0 || area != area) {
        return;
    }

    /* The same winding for every triangle on the screen; which side was
     * facing the eye was decided in the world, before this. */
    if (area < 0) {
        p1 = c;
        p2 = b;
        area = -area;
    }

    inv = 1.0f / area;
    minx = lo2(p0->x, lo2(p1->x, p2->x));
    maxx = hi2(p0->x, hi2(p1->x, p2->x));
    miny = lo2(p0->y, lo2(p1->y, p2->y));
    maxy = hi2(p0->y, hi2(p1->y, p2->y));

    x0 = minx < 0 ? 0 : (int)minx;
    y0 = miny < 0 ? 0 : (int)miny;
    x1 = maxx > (float)(v->w - 1) ? v->w - 1 : (int)maxx;
    y1 = maxy > (float)(v->h - 1) ? v->h - 1 : (int)maxy;

    if (x0 > x1 || y0 > y1) {
        return;
    }

    st->drawn++;

    /* Edge functions, stepped across each row: w0 is opposite p0. */
    {
        float A0 = p1->y - p2->y, B0 = p2->x - p1->x;
        float A1 = p2->y - p0->y, B1 = p0->x - p2->x;
        float A2 = p0->y - p1->y, B2 = p1->x - p0->x;

        for (y = y0; y <= y1; y++) {
            float py = (float)y + 0.5f, px = (float)x0 + 0.5f;
            float w0 = A0 * (px - p1->x) + B0 * (py - p1->y);
            float w1 = A1 * (px - p2->x) + B1 * (py - p2->y);
            float w2 = A2 * (px - p0->x) + B2 * (py - p0->y);
            size_t row = (size_t)y * (size_t)v->w;
            uint32_t *out = st->t->px + (size_t)y * st->t->pitch;

            for (x = x0; x <= x1; x++, w0 += A0, w1 += A1, w2 += A2) {
                float iz, l0, l1, l2;
                size_t k;
                uint32_t colour;

                if (w0 < 0 || w1 < 0 || w2 < 0) {
                    continue;
                }

                l0 = w0 * inv; l1 = w1 * inv; l2 = w2 * inv;
                iz = l0 * p0->iz + l1 * p1->iz + l2 * p2->iz;
                k = row + (size_t)x;

                if (iz <= v->depth[k]) {
                    continue;
                }

                if (st->alpha == 255) {
                    v->depth[k] = iz;
                }

                v->ids[k] = st->id;

                if (!st->colour) {
                    continue;
                }

                if (st->smooth) {
                    uint32_t r = (uint32_t)(l0 * p0->r + l1 * p1->r + l2 * p2->r);
                    uint32_t g = (uint32_t)(l0 * p0->g + l1 * p1->g + l2 * p2->g);
                    uint32_t b = (uint32_t)(l0 * p0->b + l1 * p1->b + l2 * p2->b);

                    colour = ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8)
                             | (b > 255 ? 255 : b);
                } else {
                    colour = st->flat;
                }

                if (st->alpha != 255) {
                    colour = blend(out[x] & 0xffffff, colour, st->alpha);
                }

                out[x] = 0xff000000u | colour;
            }
        }
    }
}

static struct sv screen(const struct k3d_camera *c, const struct cv *p)
{
    struct sv s;

    s.x = c->cx + p->x / p->z * c->F;
    s.y = c->cy - p->y / p->z * c->F;
    s.iz = 1.0f / p->z;
    s.r = p->r; s.g = p->g; s.b = p->b;
    return s;
}

static struct cv lerp_cv(const struct cv *a, const struct cv *b, float t)
{
    struct cv o;

    o.x = a->x + (b->x - a->x) * t;
    o.y = a->y + (b->y - a->y) * t;
    o.z = a->z + (b->z - a->z) * t;
    o.r = a->r + (b->r - a->r) * t;
    o.g = a->g + (b->g - a->g) * t;
    o.b = a->b + (b->b - a->b) * t;
    return o;
}

/*
 * A triangle in camera space, cut at the near plane: what is in front of it
 * is a triangle or a quad, drawn as one or two.
 */
static void clipped(struct tri_state *st, const struct cv in[3])
{
    const struct k3d_camera *c = &st->v->cam;
    struct cv poly[4];
    struct sv s[4];
    int n = 0, i;

    for (i = 0; i < 3; i++) {
        const struct cv *a = &in[i], *b = &in[(i + 1) % 3];
        bool ain = a->z >= c->near, bin = b->z >= c->near;

        if (ain) {
            poly[n++] = *a;
        }

        if (ain != bin) {
            poly[n++] = lerp_cv(a, b, (c->near - a->z) / (b->z - a->z));
        }
    }

    if (n < 3) {
        return;
    }

    for (i = 0; i < n; i++) {
        s[i] = screen(c, &poly[i]);
    }

    raster(st, &s[0], &s[1], &s[2]);

    if (n == 4) {
        raster(st, &s[0], &s[2], &s[3]);
    }
}

/* One object's triangles, facing the eye, into the buffers. */
static void draw_object(struct k3d_view *v, const struct k3d_target *t,
                        struct k3d_object *o, bool colour, unsigned alpha,
                        struct k3d_drawn *drawn)
{
    const struct k3d_camera *c = &v->cam;
    struct k3d_mesh *m = &o->mesh;
    struct tri_state st;
    float M[12], N[9];
    uint32_t i;

    k3d_object_matrix(o, M);
    k3d_normal_matrix(o, N);

    for (i = 0; i < m->nverts; i++) {
        const float *p = &m->pos[i * 3];
        float *w = &v->world[i * 3];

        w[0] = M[0] * p[0] + M[1] * p[1] + M[2] * p[2] + M[3];
        w[1] = M[4] * p[0] + M[5] * p[1] + M[6] * p[2] + M[7];
        w[2] = M[8] * p[0] + M[9] * p[1] + M[10] * p[2] + M[11];
        to_camera(c, w, &v->camv[i * 3]);

        if (o->smooth) {
            const float *q = &m->nrm[i * 3];
            float n[3] = { N[0] * q[0] + N[1] * q[1] + N[2] * q[2],
                           N[3] * q[0] + N[4] * q[1] + N[5] * q[2],
                           N[6] * q[0] + N[7] * q[1] + N[8] * q[2] };

            norm3(n);
            v->shade[i] = scale_rgb(o->colour, studio(c, n));
        }
    }

    st.v = v;
    st.t = t;
    st.id = o->id;
    st.smooth = o->smooth;
    st.colour = colour;
    st.alpha = alpha;
    st.drawn = 0;

    for (i = 0; i < m->ntris; i++) {
        const uint32_t *tr = &m->tri[i * 3];
        const float *w0 = &v->world[tr[0] * 3], *w1 = &v->world[tr[1] * 3];
        const float *w2 = &v->world[tr[2] * 3];
        float e1[3] = { w1[0] - w0[0], w1[1] - w0[1], w1[2] - w0[2] };
        float e2[3] = { w2[0] - w0[0], w2[1] - w0[1], w2[2] - w0[2] };
        float to_eye[3] = { c->eye[0] - w0[0], c->eye[1] - w0[1],
                            c->eye[2] - w0[2] };
        float n[3];
        struct cv cvs[3];
        int k;

        cross3(e1, e2, n);

        /* The back of a shape is never seen through its front. */
        if (dot3(n, to_eye) <= 0) {
            continue;
        }

        if (!o->smooth) {
            norm3(n);
            st.flat = scale_rgb(o->colour, studio(c, n));
        }

        for (k = 0; k < 3; k++) {
            const float *q = &v->camv[tr[k] * 3];
            uint32_t sc = o->smooth ? v->shade[tr[k]] : 0;

            cvs[k].x = q[0]; cvs[k].y = q[1]; cvs[k].z = q[2];
            cvs[k].r = (float)((sc >> 16) & 0xff);
            cvs[k].g = (float)((sc >> 8) & 0xff);
            cvs[k].b = (float)(sc & 0xff);
        }

        clipped(&st, cvs);
    }

    drawn->triangles += st.drawn;
    drawn->objects++;
}

/*--------------------------------------------------------------------------
 * Lines: the grid, Wireframe, and whatever the application draws in the
 * world - a lamp's post, a camera's frame.
 *------------------------------------------------------------------------*/

void k3d_line(struct k3d_view *v, const struct k3d_target *t,
              const float a[3], const float b[3], uint32_t colour,
              unsigned alpha, bool depth)
{
    const struct k3d_camera *c = &v->cam;
    float p[3], q[3], ax, ay, az, bx, by, bz, dx, dy, len;
    int steps, i;

    to_camera(c, a, p);
    to_camera(c, b, q);

    if (p[2] < c->near && q[2] < c->near) {
        return;
    }

    if (p[2] < c->near || q[2] < c->near) {
        float s = (c->near - p[2]) / (q[2] - p[2]);
        float cut[3] = { p[0] + (q[0] - p[0]) * s, p[1] + (q[1] - p[1]) * s,
                         c->near };

        if (p[2] < c->near) {
            memcpy(p, cut, sizeof(cut));
        } else {
            memcpy(q, cut, sizeof(cut));
        }
    }

    ax = c->cx + p[0] / p[2] * c->F;  ay = c->cy - p[1] / p[2] * c->F;
    bx = c->cx + q[0] / q[2] * c->F;  by = c->cy - q[1] / q[2] * c->F;
    az = 1.0f / p[2];                 bz = 1.0f / q[2];
    dx = bx - ax;
    dy = by - ay;

    /*
     * Cut to the picture (Liang and Barsky), so a floor line that passes
     * under the eye - thousands of pixels long once projected - costs its
     * visible length. 1/z is linear along the line on the screen, so it is
     * cut with the same fractions.
     */
    {
        float lo = 0, hi = 1;
        float pk[4] = { -dx, dx, -dy, dy };
        float qk[4] = { ax, (float)(v->w - 1) - ax, ay, (float)(v->h - 1) - ay };
        int k;

        for (k = 0; k < 4; k++) {
            if (pk[k] == 0) {
                if (qk[k] < 0) {
                    return;
                }
            } else {
                float r = qk[k] / pk[k];

                if (pk[k] < 0) {
                    if (r > hi) { return; }
                    if (r > lo) { lo = r; }
                } else {
                    if (r < lo) { return; }
                    if (r < hi) { hi = r; }
                }
            }
        }

        {
            float z0 = az, z1 = bz, x0 = ax, y0 = ay;

            ax = x0 + dx * lo;  ay = y0 + dy * lo;  az = z0 + (z1 - z0) * lo;
            bx = x0 + dx * hi;  by = y0 + dy * hi;  bz = z0 + (z1 - z0) * hi;
        }

        dx = bx - ax;
        dy = by - ay;
    }

    len = hi2(fabsf(dx), fabsf(dy));
    steps = (int)len + 1;

    for (i = 0; i <= steps; i++) {
        float s = (float)i / (float)steps;
        int x = (int)(ax + dx * s + 0.5f), y = (int)(ay + dy * s + 0.5f);
        float iz = az + (bz - az) * s;
        size_t k;
        uint32_t *out;

        if (x < 0 || y < 0 || x >= v->w || y >= v->h) {
            continue;
        }

        k = (size_t)y * (size_t)v->w + (size_t)x;

        /*
         * Held behind what is nearer - with slack, so a line lying on a
         * surface, as the grid lies on the ground, is not lost in it. Two
         * per cent, because the surface's depth is taken at the middle of
         * its pixel and the line's where the line crosses it, up to half a
         * pixel away: near the eye, looking along a floor, depth changes
         * nearly one per cent a pixel, and 0.2% lost the whole grid there.
         * At ten metres it lets through a line twenty centimetres behind.
         */
        if (depth && iz < v->depth[k] * 0.98f) {
            continue;
        }

        out = t->px + (size_t)y * t->pitch + (size_t)x;
        *out = 0xff000000u | blend(*out & 0xffffff, colour, alpha);
    }
}

/*
 * The floor: a line a metre, twelve each way, fading with distance from the
 * eye as Blender's does, and the X and Y axes through the origin in red and
 * green.
 */
static unsigned fade(const struct k3d_view *v, float across)
{
    float d = sqrtf(across * across + v->cam.eye[2] * v->cam.eye[2]);

    return d > 40 ? 0 : (unsigned)(34.0f * (1.0f - d / 40.0f));
}

static void grid(struct k3d_view *v, const struct k3d_target *t, bool depth)
{
    int i;
    const int N = 12;

    for (i = -N; i <= N; i++) {
        float a[3], b[3];

        /* Along X at y = i: its distance is across Y, and the other way. */
        a[0] = -N; a[1] = (float)i; a[2] = 0;
        b[0] =  N; b[1] = (float)i; b[2] = 0;
        k3d_line(v, t, a, b, i == 0 ? 0xe0524b : 0xffffff,
                 i == 0 ? 200 : fade(v, (float)i - v->cam.eye[1]), depth);
        a[0] = (float)i; a[1] = -N;
        b[0] = (float)i; b[1] =  N;
        k3d_line(v, t, a, b, i == 0 ? 0x7cbb3a : 0xffffff,
                 i == 0 ? 200 : fade(v, (float)i - v->cam.eye[0]), depth);
    }
}

/* An object's own edges, in the world. */
static void wire(struct k3d_view *v, const struct k3d_target *t,
                 struct k3d_object *o, uint32_t colour, unsigned alpha)
{
    struct k3d_mesh *m = &o->mesh;
    float M[12];
    uint32_t i;

    k3d_object_matrix(o, M);

    for (i = 0; i < m->nedges; i++) {
        const float *p = &m->pos[m->edge[i * 2] * 3];
        const float *q = &m->pos[m->edge[i * 2 + 1] * 3];
        float a[3], b[3];
        int k;

        for (k = 0; k < 3; k++) {
            a[k] = M[k * 4] * p[0] + M[k * 4 + 1] * p[1] + M[k * 4 + 2] * p[2]
                   + M[k * 4 + 3];
            b[k] = M[k * 4] * q[0] + M[k * 4 + 1] * q[1] + M[k * 4 + 2] * q[2]
                   + M[k * 4 + 3];
        }

        k3d_line(v, t, a, b, colour, alpha, false);
    }
}

/*
 * The selection's outline: every pixel that is not the selected object but
 * lies within two of one that is. Read from the object buffer, so it follows
 * what can be seen of the object - round whatever stands in front of it, as
 * Blender's does.
 */
static void outline(struct k3d_view *v, const struct k3d_target *t,
                    uint32_t id, uint32_t colour)
{
    static const signed char off[][2] = {
        { -2, 0 }, { 2, 0 }, { 0, -2 }, { 0, 2 }, { -1, 0 }, { 1, 0 },
        { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 },
    };
    int minx = v->w, miny = v->h, maxx = -1, maxy = -1, x, y;
    size_t k;

    for (y = 0; y < v->h; y++) {
        const uint32_t *row = &v->ids[(size_t)y * (size_t)v->w];

        for (x = 0; x < v->w; x++) {
            if (row[x] == id) {
                if (x < minx) { minx = x; }
                if (x > maxx) { maxx = x; }
                if (y < miny) { miny = y; }
                if (y > maxy) { maxy = y; }
            }
        }
    }

    if (maxx < 0) {
        return;
    }

    for (y = miny - 2; y <= maxy + 2; y++) {
        if (y < 0 || y >= v->h) {
            continue;
        }

        for (x = minx - 2; x <= maxx + 2; x++) {
            unsigned j;

            if (x < 0 || x >= v->w) {
                continue;
            }

            k = (size_t)y * (size_t)v->w + (size_t)x;

            if (v->ids[k] == id) {
                continue;
            }

            for (j = 0; j < sizeof(off) / sizeof(off[0]); j++) {
                int nx = x + off[j][0], ny = y + off[j][1];

                if (nx >= 0 && ny >= 0 && nx < v->w && ny < v->h
                    && v->ids[(size_t)ny * (size_t)v->w + (size_t)nx] == id) {
                    t->px[(size_t)y * t->pitch + (size_t)x] = 0xff000000u | colour;
                    break;
                }
            }
        }
    }
}

static bool ready(struct k3d_view *v, struct k3d_object *o)
{
    if (o->stale && !k3d_mesh_build(o)) {
        return false;
    }

    if (o->mesh.nverts > v->scratch) {
        uint32_t n = o->mesh.nverts;
        float *w = realloc(v->world, (size_t)n * 3 * sizeof(float));
        float *c;
        uint32_t *s;

        if (w == NULL) {
            return false;
        }

        v->world = w;
        c = realloc(v->camv, (size_t)n * 3 * sizeof(float));

        if (c == NULL) {
            return false;
        }

        v->camv = c;
        s = realloc(v->shade, (size_t)n * sizeof(uint32_t));

        if (s == NULL) {
            return false;
        }

        v->shade = s;
        v->scratch = n;
    }

    return true;
}

struct k3d_drawn k3d_draw(struct k3d_view *v, struct k3d_scene *s,
                          const struct k3d_target *t,
                          const struct k3d_draw *how)
{
    struct k3d_drawn drawn = { 0, 0 };
    bool solid = how->mode == K3D_SOLID;
    uint32_t i;
    int y;

    memset(v->depth, 0, (size_t)v->w * (size_t)v->h * sizeof(float));
    memset(v->ids, 0, (size_t)v->w * (size_t)v->h * sizeof(uint32_t));

    /* The ground behind everything: a gradient, top to bottom. */
    for (y = 0; y < v->h; y++) {
        uint32_t c = blend(how->sky_top & 0xffffff, how->sky_bottom & 0xffffff,
                           (unsigned)(255 * y / (v->h > 1 ? v->h - 1 : 1)));
        uint32_t *row = t->px + (size_t)y * t->pitch;
        int x;

        for (x = 0; x < v->w; x++) {
            row[x] = 0xff000000u | c;
        }
    }

    if (!solid && how->grid) {
        grid(v, t, false);
    }

    /* Everything opaque first; in Wireframe only into the object buffer,
     * so a click still finds what it was pointed at. */
    for (i = 0; i < s->count; i++) {
        struct k3d_object *o = &s->obj[i];

        if (o->hidden || o->alpha < 1.0f || !ready(v, o)) {
            continue;
        }

        draw_object(v, t, o, solid, 255, &drawn);
    }

    if (solid && how->grid) {
        grid(v, t, true);
    }

    /* Then what is seen through, far to near, over the rest. */
    {
        uint32_t order[64], n = 0, j, k;

        for (i = 0; i < s->count && n < 64; i++) {
            struct k3d_object *o = &s->obj[i];

            if (!o->hidden && o->alpha < 1.0f && ready(v, o)) {
                order[n++] = i;
            }
        }

        for (j = 1; j < n; j++) {
            for (k = j; k > 0; k--) {
                const float *a = s->obj[order[k - 1]].loc, *b = s->obj[order[k]].loc;
                uint32_t swap;
                float da = (a[0] - v->cam.eye[0]) * (a[0] - v->cam.eye[0])
                         + (a[1] - v->cam.eye[1]) * (a[1] - v->cam.eye[1])
                         + (a[2] - v->cam.eye[2]) * (a[2] - v->cam.eye[2]);
                float db = (b[0] - v->cam.eye[0]) * (b[0] - v->cam.eye[0])
                         + (b[1] - v->cam.eye[1]) * (b[1] - v->cam.eye[1])
                         + (b[2] - v->cam.eye[2]) * (b[2] - v->cam.eye[2]);

                if (da >= db) {
                    break;
                }

                swap = order[k - 1];
                order[k - 1] = order[k];
                order[k] = swap;
            }
        }

        for (j = 0; j < n; j++) {
            struct k3d_object *o = &s->obj[order[j]];
            unsigned a = (unsigned)(o->alpha * 255.0f + 0.5f);

            draw_object(v, t, o, solid, solid ? a : 255, &drawn);
        }
    }

    if (!solid) {
        for (i = 0; i < s->count; i++) {
            struct k3d_object *o = &s->obj[i];

            if (!o->hidden && !o->stale) {
                bool sel = o->id == how->selected;

                wire(v, t, o, sel ? how->outline : 0xd2d7e0, sel ? 255 : 140);
            }
        }
    } else if (how->selected != 0) {
        outline(v, t, how->selected, how->outline & 0xffffff);
    }

    return drawn;
}
