/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The 3D Kit's shapes and its rasteriser, held on the host (`roadmap.md`
 * 4l, Cafesa3D's first step).
 *
 * The shapes: every triangle of every shape wound anticlockwise from
 * outside, at several sizes - a face wound backwards is invisible from the
 * front and nothing else would notice; the counts of triangles and edges
 * the Data tab and Wireframe rely on; and every vertex where the shape says
 * it is.
 *
 * The rasteriser, on scenes small enough to reason about: a point projects
 * where the pinhole says; the nearer of two objects wins whichever was
 * added first; the back of a cube is never drawn; a floor through the eye
 * is cut at the near plane and still fills what is in front; the outline
 * lies outside the selection and nowhere else; a hidden object is not
 * there; glass lets what is behind it through and is still what a click
 * finds; Wireframe still picks; a line behind a cube is held behind it.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/*--------------------------------------------------------------------------
 * Shapes.
 *------------------------------------------------------------------------*/

/* Every triangle's normal, by its winding, points away from the centre -
 * or, for a plane, up. */
static int wound_outwards(const struct k3d_object *o)
{
    const struct k3d_mesh *m = &o->mesh;
    uint32_t i;

    for (i = 0; i < m->ntris; i++) {
        const float *a = &m->pos[m->tri[i * 3] * 3];
        const float *b = &m->pos[m->tri[i * 3 + 1] * 3];
        const float *c = &m->pos[m->tri[i * 3 + 2] * 3];
        float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1],
                       e1[2] * e2[0] - e1[0] * e2[2],
                       e1[0] * e2[1] - e1[1] * e2[0] };
        float mid[3] = { (a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3,
                         (a[2] + b[2] + c[2]) / 3 };
        float out = o->kind == K3D_PLANE ? n[2]
                    : n[0] * mid[0] + n[1] * mid[1] + n[2] * mid[2];

        if (out <= 0) {
            return 0;
        }
    }

    return 1;
}

static void shapes(void)
{
    struct k3d_scene s;
    struct k3d_object *o;
    char what[160];
    int seg, rings;
    uint32_t i;

    k3d_scene_init(&s);

    o = k3d_scene_add(&s, K3D_BOX);
    o->size[0] = 1.5f; o->size[1] = 0.5f; o->size[2] = 3.0f;
    check(k3d_mesh_build(o), "a box builds");
    check(wound_outwards(o), "every face of a box is wound outwards");
    check(o->mesh.ntris == 12 && k3d_triangles(o) == 12, "a box is 12 triangles");
    check(o->mesh.nedges == 12, "a box has its 12 edges and no diagonals");

    {
        int ok = 1;

        for (i = 0; i < o->mesh.nverts; i++) {
            const float *p = &o->mesh.pos[i * 3];

            ok &= fabsf(fabsf(p[0]) - 0.75f) < 1e-6f
                  && fabsf(fabsf(p[1]) - 0.25f) < 1e-6f
                  && fabsf(fabsf(p[2]) - 1.5f) < 1e-6f;
        }

        check(ok, "every corner of a box is at half its sides");
    }

    o = k3d_scene_add(&s, K3D_PLANE);
    o->size[0] = 24;
    check(k3d_mesh_build(o) && wound_outwards(o), "a plane faces up");
    check(o->mesh.ntris == 2 && o->mesh.nedges == 4, "a plane is 2 triangles, 4 edges");

    for (seg = 3; seg <= 64; seg += 29) {
        for (rings = 2; rings <= 32; rings += 15) {
            int at = 1;

            o = k3d_scene_add(&s, K3D_SPHERE);
            o->segments = seg;
            o->rings = rings;
            o->radius = 0.9f;

            snprintf(what, sizeof(what),
                     "a sphere of %d segments and %d rings builds, wound outwards",
                     seg, rings);
            check(k3d_mesh_build(o) && wound_outwards(o), what);
            snprintf(what, sizeof(what),
                     "a sphere of %d by %d is the triangles it says (%u)",
                     seg, rings, k3d_triangles(o));
            check(o->mesh.ntris == k3d_triangles(o), what);
            check(o->mesh.nedges == (uint32_t)(seg * (2 * rings - 1)),
                  "a sphere's edges are its rings and meridians");

            for (i = 0; i < o->mesh.nverts; i++) {
                const float *p = &o->mesh.pos[i * 3];

                at &= fabsf(sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - 0.9f) < 1e-5f;
            }

            check(at, "every vertex of a sphere is at its radius");
        }
    }

    o = k3d_scene_add(&s, K3D_SPHERE);
    check(o->segments == 32 && o->rings == 16 && k3d_triangles(o) == 960,
          "a new sphere is Blender's: 32 segments, 16 rings, 960 triangles");

    for (seg = 3; seg <= 64; seg += 61) {
        o = k3d_scene_add(&s, K3D_CYLINDER);
        o->segments = seg;
        o->radius = 0.55f;
        o->depth = 1.6f;

        snprintf(what, sizeof(what), "a cylinder of %d sides builds, wound outwards", seg);
        check(k3d_mesh_build(o) && wound_outwards(o), what);
        check(o->mesh.ntris == k3d_triangles(o) && o->mesh.nedges == (uint32_t)(3 * seg),
              "a cylinder is the triangles it says, and its caps have no spokes");
    }

    o = k3d_scene_add(&s, K3D_SPHERE);
    o->segments = 2;
    check(!k3d_mesh_build(o), "a sphere of two segments is refused, not built");

    /* Removing keeps the others in order. */
    {
        uint32_t second = s.obj[1].id, third = s.obj[2].id, n = s.count;

        check(k3d_scene_remove(&s, second) && s.count == n - 1
              && s.obj[1].id == third && k3d_scene_find(&s, second) == NULL,
              "an object removed is gone, and the rest keep their order");
    }

    k3d_scene_free(&s);
}

/*--------------------------------------------------------------------------
 * The rasteriser.
 *------------------------------------------------------------------------*/

#define W 320
#define H 240

static uint32_t pixels[H * (W + 13)];      /* a pitch wider than the picture */
static const struct k3d_target target = { pixels, W + 13 };

static const struct k3d_draw solid = {
    K3D_SOLID, 0, false, 0x40444b, 0x2d3035, 0xffa53d,
};

static uint32_t at(int x, int y)
{
    return pixels[(size_t)y * target.pitch + (size_t)x] & 0xffffff;
}

static struct k3d_object *cube(struct k3d_scene *s, float x, float y, float z,
                               uint32_t colour)
{
    struct k3d_object *o = k3d_scene_add(s, K3D_BOX);

    o->loc[0] = x; o->loc[1] = y; o->loc[2] = z;
    o->colour = colour;
    return o;
}

static void rasteriser(void)
{
    struct k3d_scene s;
    struct k3d_view v;
    struct k3d_object *a, *b, *glass;
    uint32_t aid, bid;
    struct k3d_draw how = solid;
    struct k3d_drawn drawn;
    float eye[3] = { 0, -10, 0 }, target_pt[3] = { 0, 0, 0 };
    float sx, sy, iz;

    check(k3d_view_init(&v, W, H), "a view of 320 by 240");
    k3d_view_look(&v, eye, target_pt, 3.14159265f / 2);

    /* The pinhole. */
    {
        float o[3] = { 0, 0, 0 }, r[3] = { 1, 0, 0 }, behind[3] = { 0, -11, 0 };

        check(k3d_project(&v, o, &sx, &sy, &iz) && fabsf(sx - 160) < 1e-3f
              && fabsf(sy - 120) < 1e-3f && fabsf(iz - 0.1f) < 1e-6f,
              "what the eye looks at is at the centre, at a tenth");
        check(k3d_project(&v, r, &sx, &sy, &iz) && fabsf(sx - (160 + 16)) < 1e-3f,
              "a metre to the right at ten metres is F/10 to the right");
        check(!k3d_project(&v, behind, &sx, &sy, &iz), "behind the eye is nowhere");
    }

    k3d_scene_init(&s);
    a = cube(&s, 0, 0, 0, 0xc33b2c);

    drawn = k3d_draw(&v, &s, &target, &how);
    check(k3d_pick(&v, 160, 120) == a->id, "the cube is what the centre shows");
    check(k3d_pick(&v, 5, 5) == 0, "the corner shows nothing");
    check(drawn.triangles == 2, "a cube face on: its front, and none of its back or sides");
    check(at(160, 120) != 0x40444b && at(5, 0) == 0x40444b,
          "the cube is lit and the top row is the sky's top");

    /* The nearer wins, whichever came first. Objects are found by id after
     * anything is removed: removing moves the ones after it, as the Lua
     * binding knows by never keeping a pointer. */
    b = cube(&s, 0, -3, 0, 0x3d6fc4);
    b->size[0] = b->size[1] = b->size[2] = 0.5f;
    bid = b->id;
    k3d_draw(&v, &s, &target, &how);
    check(k3d_pick(&v, 160, 120) == bid, "a nearer cube added second is in front");
    k3d_scene_remove(&s, a->id);
    aid = cube(&s, 0, 0, 0, 0xc33b2c)->id;
    a = k3d_scene_find(&s, aid);
    b = k3d_scene_find(&s, bid);
    k3d_draw(&v, &s, &target, &how);
    check(k3d_pick(&v, 160, 120) == bid, "and still in front when it came first");

    /* Hidden. */
    b->hidden = true;
    k3d_draw(&v, &s, &target, &how);
    check(k3d_pick(&v, 160, 120) == a->id, "a hidden cube is not there");
    b->hidden = false;

    /* The outline: round the selection, never on it, not far away. */
    b->hidden = true;
    how.selected = a->id;
    k3d_draw(&v, &s, &target, &how);
    {
        int x, edge = -1, ok = 1;

        for (x = 160; x < W - 1; x++) {
            if (k3d_pick(&v, x, 120) != a->id) {
                edge = x;
                break;
            }
        }

        check(edge > 160, "the cube ends somewhere to the right of the centre");
        check(at(edge, 120) == 0xffa53d && at(edge + 1, 120) == 0xffa53d,
              "two pixels of orange just outside it");
        check(at(edge + 3, 120) != 0xffa53d, "and not three");

        for (x = 150; x < edge; x++) {
            ok &= at(x, 120) != 0xffa53d;
        }

        check(ok, "none on the cube itself");
    }
    how.selected = 0;
    b->hidden = false;

    /* Glass: what is behind shows through, and a click finds the glass. */
    {
        uint32_t plain;

        b->hidden = true;
        k3d_draw(&v, &s, &target, &how);
        plain = at(160, 120);
        b->hidden = false;

        glass = b;
        glass->colour = 0xffffff;
        glass->alpha = 0.45f;
        k3d_draw(&v, &s, &target, &how);
        check(k3d_pick(&v, 160, 120) == glass->id, "a click on glass finds the glass");
        check(at(160, 120) != plain && at(160, 120) != 0xffffff,
              "and the cube behind it shows through, lightened");
        /* The cube's face is nine metres away and the glass's six and
         * three quarters: 1/9 and 1/6.75. */
        check(fabsf(v.depth[120 * W + 160] - 1.0f / 9) < 1e-4f,
              "the depth there is still the cube's: glass does not hide what is behind");
        glass->alpha = 1;
    }

    /* Wireframe still picks, and draws lines. */
    how.mode = K3D_WIRE;
    k3d_draw(&v, &s, &target, &how);
    check(k3d_pick(&v, 160, 120) == b->id, "Wireframe still knows what is where");
    {
        int x, lines = 0;

        /* Row 120 is one colour of the sky but where an edge crosses it:
         * the two squares of each cube's front and back, left and right. */
        for (x = 1; x < W; x++) {
            lines += at(x, 120) != at(0, 120);
        }

        check(lines >= 4, "and draws the cubes' edges across the middle");
    }
    how.mode = K3D_SOLID;

    /* A line behind the cube is held behind it; in front, it shows. */
    {
        float p[3] = { -3, 1, 0 }, q[3] = { 3, 1, 0 };
        float p2[3] = { -3, -6, 0 }, q2[3] = { 3, -6, 0 };

        b->hidden = true;
        k3d_draw(&v, &s, &target, &how);
        k3d_line(&v, &target, p, q, 0x00ff00, 255, true);
        check(at(160, 120) != 0x00ff00, "a line behind the cube is hidden by it");
        k3d_line(&v, &target, p2, q2, 0x00ff00, 255, true);
        check(at(160, 120) == 0x00ff00, "and one in front of it is drawn");
        b->hidden = false;
    }

    /* A floor that passes under the eye: cut at the near plane, and it still
     * fills the bottom of the picture. */
    {
        struct k3d_object *floor;
        float low[3] = { 0, 0, 1 }, ahead[3] = { 0, 10, 0.5f };

        k3d_view_look(&v, low, ahead, 3.14159265f / 2);
        floor = k3d_scene_add(&s, K3D_PLANE);
        floor->size[0] = 100;
        a->hidden = true;
        b->hidden = true;
        k3d_draw(&v, &s, &target, &how);
        check(k3d_pick(&v, 160, H - 1) == floor->id && k3d_pick(&v, 5, H - 1) == floor->id,
              "a floor through the eye still fills the bottom edge");
        check(k3d_pick(&v, 160, 0) == 0, "and the top is sky");
        how.grid = true;
        k3d_draw(&v, &s, &target, &how);
        {
            int x, y, greens = 0;

            /* The floor is grey, so a green pixel on it is the axis. */
            for (y = H / 2; y < H; y++) {
                for (x = 0; x < W; x++) {
                    uint32_t c = at(x, y);
                    int r = (int)(c >> 16), g = (int)((c >> 8) & 0xff), bl = (int)(c & 0xff);

                    greens += g > r + 20 && g > bl + 20;
                }
            }

            check(greens > 20, "and the grid's green Y axis runs up it from under the eye");
        }
    }

    k3d_scene_free(&s);
    k3d_view_free(&v);
}

int main(void)
{
    shapes();
    rasteriser();

    if (fails) {
        printf("FAIL: %d of %d checks on the 3D Kit\n", fails, checks + fails);
        return 1;
    }

    printf("PASS: %d checks on the 3D Kit (every shape wound outwards, its triangles and "
           "edges counted; the nearer of two in front whichever came first, the back of a "
           "cube never drawn, the outline two pixels outside the selection, glass seen "
           "through and still picked, Wireframe picking, a line held behind a cube, a floor "
           "cut at the near plane)\n", checks);
    return 0;
}
