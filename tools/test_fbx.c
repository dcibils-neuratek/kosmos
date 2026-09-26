/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * FBX through ufbx, held on the host (`roadmap.md` 4l, 5c;
 * `user/kits/3d/k3d_fbx.c`).
 *
 * **Against what the program that wrote the file says it drew.** ufbx's
 * own test data (`tools/fbx_conformance.txt`) has, beside several FBX
 * files, an OBJ the same program exported of the same scene, in the file's
 * own axes and unit. Each part's points, put in the world by its matrix,
 * are held to the OBJ turned into Y up and metres by hand: every point of
 * one within a hundred-thousandth of the model's size of a point of the
 * other, and every triangle lying on one of the OBJ's polygons and facing
 * the way it faces - which is what says the turns went the right way round
 * and the triangles face outwards. Blender, Maya and 3ds Max each; FBX 6.1,
 * 7.1, 7.4, 7.5 and 7.7; binary and text; pivots moved off an object,
 * a mesh shown several times with a pivot each, a child inside a
 * stretched parent, and one turn in all six orders.
 *
 * **A mirrored object is wound the way Cafesa3D draws it**: an object
 * whose matrix turns space inside out has its triangles reversed, as
 * `scene:world_triangles` and the tracer do, and faces outwards after -
 * and inwards without, which is how the check knows it is looking.
 *
 * **Materials to their numbers**: 3ds Max files whose every value was set
 * by hand, Maya's default Lambert (a black transparency at a factor of
 * one, which is opaque), and Blender's, with its metal and see-through.
 *
 * **And what is not a model**: nothing, an OBJ, every truncation of a
 * binary file, and a thousand files with bytes changed - each a sentence or
 * a model, never a crash.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/kits/3d/k3d.h"

#define DIR "build/downloads/fbx-conformance/"

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

static unsigned char *slurp(const char *name, size_t *len)
{
    char path[256];
    FILE *f;
    long n;
    unsigned char *b;

    snprintf(path, sizeof(path), DIR "%s", name);
    f = fopen(path, "rb");

    if (f == NULL) {
        printf("test_fbx: %s is missing - `python3 tools/fetch_conformance.py fbx` fetches it\n",
               path);
        exit(1);
    }

    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);

    if (b == NULL || fread(b, 1, (size_t)n, f) != (size_t)n) {
        printf("test_fbx: could not read %s\n", path);
        exit(1);
    }

    fclose(f);
    *len = (size_t)n;
    return b;
}

/*--------------------------------------------------------------------------
 * Triangles in the world.
 *------------------------------------------------------------------------*/

struct world {
    double *pts;                /* three a point */
    size_t  npts;
    double *tris;               /* nine a triangle, the corners themselves */
    size_t  ntris;
};

static void world_free(struct world *w)
{
    free(w->pts);
    free(w->tris);
    memset(w, 0, sizeof(*w));
}

static void place(const double m[16], const float *p, double out[3])
{
    int r;

    for (r = 0; r < 3; r++) {
        out[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
    }
}

static double det3(const double m[16])
{
    return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2])
           + m[8] * (m[1] * m[6] - m[5] * m[2]);
}

/* Every part in the world. `flip` reverses a mirrored part's triangles,
 * as Cafesa3D does; without it they are as the file wound them. */
static void world_of_fbx(const struct k3d_fbx *f, int flip, struct world *w)
{
    size_t i, np = 0, nt = 0;

    for (i = 0; i < f->nparts; i++) {
        np += f->parts[i].soup.npos;
        nt += f->parts[i].soup.ntri;
    }

    w->pts = malloc((np + 1) * 3 * sizeof(double));
    w->tris = malloc((nt + 1) * 9 * sizeof(double));
    w->npts = w->ntris = 0;

    for (i = 0; i < f->nparts; i++) {
        const struct k3d_fbx_part *p = &f->parts[i];
        int mirror = flip && det3(p->matrix) < 0;
        uint32_t k;

        for (k = 0; k < p->soup.npos; k++) {
            place(p->matrix, &p->soup.pos[k * 3], &w->pts[w->npts++ * 3]);
        }

        for (k = 0; k < p->soup.ntri; k++) {
            const uint32_t *t = &p->soup.tri[k * 3];
            double *out = &w->tris[w->ntris++ * 9];

            place(p->matrix, &p->soup.pos[t[0] * 3], out);
            place(p->matrix, &p->soup.pos[t[mirror ? 2 : 1] * 3], out + 3);
            place(p->matrix, &p->soup.pos[t[mirror ? 1 : 2] * 3], out + 6);
        }
    }
}

/*
 * An OBJ's polygons, as its program wrote them, in the file's own axes and
 * unit turned into Y up and metres. Read here rather than by the 3D Kit's
 * OBJ reader, which cuts a polygon into a fan - and which diagonal a
 * four-cornered face is cut along is a choice, not a fact: Maya's parented
 * cubes are bent out of flat on purpose, and their area depends on it.
 */
struct polys {
    double  *pts;               /* three a point */
    size_t   npts;
    uint32_t *corner;           /* every polygon's corners, one after another */
    uint32_t *first, *count;    /* where each polygon's corners start, and how many */
    size_t   npolys, ncorners;
};

static void polys_free(struct polys *p)
{
    free(p->pts);
    free(p->corner);
    free(p->first);
    free(p->count);
    memset(p, 0, sizeof(*p));
}

static void read_polys(const char *text, size_t len, int z_up, double metres, struct polys *p)
{
    const char *at = text, *end = text + len;
    size_t cap = 1024;

    memset(p, 0, sizeof(*p));
    p->pts = malloc(cap * 3 * sizeof(double));
    p->corner = malloc(cap * sizeof(uint32_t));
    p->first = malloc(cap * sizeof(uint32_t));
    p->count = malloc(cap * sizeof(uint32_t));

    while (at < end) {
        const char *eol = memchr(at, '\n', (size_t)(end - at));
        char line[1024];
        size_t n;

        if (eol == NULL) eol = end;

        n = (size_t)(eol - at) < sizeof(line) - 1 ? (size_t)(eol - at) : sizeof(line) - 1;
        memcpy(line, at, n);
        line[n] = 0;
        at = eol + 1;

        if (line[0] == 'v' && line[1] == ' ') {
            double x, y, z;

            if (sscanf(line + 2, "%lf %lf %lf", &x, &y, &z) == 3 && p->npts < cap) {
                p->pts[p->npts * 3 + 0] = x * metres;
                p->pts[p->npts * 3 + 1] = (z_up ? z : y) * metres;
                p->pts[p->npts * 3 + 2] = (z_up ? -y : z) * metres;
                p->npts++;
            }
        } else if (line[0] == 'f' && line[1] == ' ' && p->npolys < cap) {
            char *word = strtok(line + 2, " \t\r");

            p->first[p->npolys] = (uint32_t)p->ncorners;
            p->count[p->npolys] = 0;

            while (word && p->ncorners < cap) {
                long i = strtol(word, NULL, 10);

                p->corner[p->ncorners++] = (uint32_t)(i < 0 ? (long)p->npts + i : i - 1);
                p->count[p->npolys]++;
                word = strtok(NULL, " \t\r");
            }

            p->npolys++;
        }
    }
}

static int same_place(const double *a, const double *b, double tol)
{
    return fabs(a[0] - b[0]) <= tol && fabs(a[1] - b[1]) <= tol && fabs(a[2] - b[2]) <= tol;
}

/* A polygon's facing: Newell's normal, which a bent one has too. */
static void facing(const struct polys *p, size_t i, double n[3])
{
    uint32_t k, c = p->count[i];

    n[0] = n[1] = n[2] = 0;

    for (k = 0; k < c; k++) {
        const double *a = &p->pts[p->corner[p->first[i] + k] * 3];
        const double *b = &p->pts[p->corner[p->first[i] + (k + 1) % c] * 3];

        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
}

/* A triangle lies on some polygon - each corner one of the polygon's - and
 * faces the way it does. */
static int on_a_polygon(const double *t, const struct polys *p, double tol)
{
    double e1[3] = { t[3] - t[0], t[4] - t[1], t[5] - t[2] };
    double e2[3] = { t[6] - t[0], t[7] - t[1], t[8] - t[2] };
    double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                    e1[0] * e2[1] - e1[1] * e2[0] };
    size_t i;

    for (i = 0; i < p->npolys; i++) {
        int c, found = 0;
        uint32_t k;
        double f[3];

        for (c = 0; c < 3; c++) {
            for (k = 0; k < p->count[i]; k++) {
                if (same_place(&t[c * 3], &p->pts[p->corner[p->first[i] + k] * 3], tol)) {
                    found++;
                    break;
                }
            }
        }

        if (found < 3) continue;

        facing(p, i, f);

        if (n[0] * f[0] + n[1] * f[1] + n[2] * f[2] > 0) return 1;
    }

    return 0;
}

static double extent(const double *pts, size_t n)
{
    double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 }, e = 0;
    size_t i;
    int k;

    for (i = 0; i < n; i++) {
        for (k = 0; k < 3; k++) {
            if (pts[i * 3 + k] < lo[k]) lo[k] = pts[i * 3 + k];
            if (pts[i * 3 + k] > hi[k]) hi[k] = pts[i * 3 + k];
        }
    }

    for (k = 0; k < 3; k++) {
        if (hi[k] - lo[k] > e) e = hi[k] - lo[k];
    }

    return e;
}

/* Every one of `n` points within `tol` of one of `m` others. */
static int covered(const double *a, size_t n, const double *b, size_t m, double tol)
{
    size_t i, j;

    for (i = 0; i < n; i++) {
        int found = 0;

        for (j = 0; j < m && !found; j++) found = same_place(&a[i * 3], &b[j * 3], tol);

        if (!found) return 0;
    }

    return 1;
}

static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }

/*--------------------------------------------------------------------------
 * Against the OBJ its program exported.
 *------------------------------------------------------------------------*/

struct against {
    const char *fbx, *obj;
    int z_up;
    double metres;              /* what one of the file's units is */
    size_t parts;
    const char *first;          /* the first part's name */
};

static const struct against AGAINST[] = {
    { "blender_279_default_7400_binary.fbx", "blender_279_default.obj", 0, 1, 1, "Cube" },
    { "max_geometry_transform_6100_binary.fbx", "max_geometry_transform.obj", 1, 0.0254, 2,
      "Box001" },
    { "max_geometry_transform_7700_ascii.fbx", "max_geometry_transform.obj", 1, 0.0254, 2,
      "Box001" },
    { "max_geometry_transform_instances_7700_ascii.fbx", "max_geometry_transform_instances.obj", 1,
      0.0254, 4, "Box001" },
    { "maya_cube_7100_ascii.fbx", "maya_cube.obj", 0, 0.01, 1, "pCube1" },
    { "maya_parented_cubes_7500_ascii.fbx", "maya_parented_cubes.obj", 0, 0.01, 2, "Parent" },
    { "maya_rotation_order_7500_ascii.fbx", "maya_rotation_order.obj", 0, 0.01, 6, "XYZ" },
};

static void test_against(const struct against *c)
{
    size_t flen, olen, i, expected = 0, on = 0;
    unsigned char *fb = slurp(c->fbx, &flen), *ob = slurp(c->obj, &olen);
    struct k3d_fbx f;
    struct polys o;
    struct world w;
    const char *why = k3d_fbx_read(fb, flen, &f);
    char what[256];
    double size, tol;

    snprintf(what, sizeof(what), "%s reads: %s", c->fbx, why ? why : "");
    check(why == NULL, what);

    if (why) {
        free(fb);
        free(ob);
        return;
    }

    read_polys((const char *)ob, olen, c->z_up, c->metres, &o);
    world_of_fbx(&f, 1, &w);
    size = extent(o.pts, o.npts);
    tol = size * 1e-5;

    snprintf(what, sizeof(what), "%s is %zu parts, the first %s: %zu, %s", c->fbx, c->parts,
             c->first, f.nparts, f.nparts ? f.parts[0].name : "none");
    check(f.nparts == c->parts && strcmp(f.parts[0].name, c->first) == 0, what);

    for (i = 0; i < o.npolys; i++) expected += o.count[i] - 2;

    snprintf(what, sizeof(what), "%s: %zu triangles, and its program's polygons make %zu",
             c->fbx, w.ntris, expected);
    check(w.ntris == expected, what);

    snprintf(what, sizeof(what), "%s: every point where its program put one, and no other "
             "(%.3f m across)", c->fbx, size);
    check(size > 0 && covered(w.pts, w.npts, o.pts, o.npts, tol)
          && covered(o.pts, o.npts, w.pts, w.npts, tol), what);

    for (i = 0; i < w.ntris; i++) on += on_a_polygon(&w.tris[i * 9], &o, tol);

    snprintf(what, sizeof(what), "%s: %zu of %zu triangles on one of its program's polygons, "
             "facing the same way", c->fbx, on, w.ntris);
    check(on == w.ntris, what);

    world_free(&w);
    polys_free(&o);
    k3d_fbx_free(&f);
    free(fb);
    free(ob);
}

/*--------------------------------------------------------------------------
 * Instances, mirrors and parts.
 *------------------------------------------------------------------------*/

static int read_file(const char *name, struct k3d_fbx *f)
{
    size_t len;
    unsigned char *b = slurp(name, &len);
    const char *why = k3d_fbx_read(b, len, f);
    char what[256];

    snprintf(what, sizeof(what), "%s reads: %s", name, why ? why : "");
    check(why == NULL, what);
    free(b);
    return why == NULL;
}

/* How far a part's triangles face away from its middle: positive when
 * they face out. */
static double outwards(const struct world *w)
{
    double mid[3] = { 0, 0, 0 }, sum = 0;
    size_t i;
    int k;

    for (i = 0; i < w->npts; i++) {
        for (k = 0; k < 3; k++) mid[k] += w->pts[i * 3 + k] / (double)w->npts;
    }

    for (i = 0; i < w->ntris; i++) {
        const double *a = &w->tris[i * 9], *b = a + 3, *c = a + 6;
        double e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        double e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                        e1[0] * e2[1] - e1[1] * e2[0] };

        for (k = 0; k < 3; k++) sum += n[k] * ((a[k] + b[k] + c[k]) / 3 - mid[k]);
    }

    return sum;
}

static double part_outwards(const struct k3d_fbx *f, size_t i, int flip)
{
    struct k3d_fbx one = *f;
    struct world w;
    double o;

    one.parts = &f->parts[i];
    one.nparts = 1;
    world_of_fbx(&one, flip, &w);
    o = outwards(&w);
    world_free(&w);
    return o;
}

static void test_instancing(void)
{
    struct k3d_fbx f;
    size_t i, j;
    int ring = 1, apart = 1, same = 1;

    if (!read_file("blender_293_instancing_7400_binary.fbx", &f)) return;

    check(f.nparts == 8, "one Suzanne shown eight times is eight parts");

    for (i = 0; i < f.nparts; i++) {
        const double *m = f.parts[i].matrix;

        ring &= near(sqrt(m[12] * m[12] + m[14] * m[14]), 3.1, 1e-3) && near(m[13], 0, 1e-6);
        same &= f.parts[i].soup.ntri == f.parts[0].soup.ntri && f.parts[i].soup.ntri == 968;

        for (j = 0; j < i; j++) {
            apart &= !near(m[12], f.parts[j].matrix[12], 1e-3)
                     || !near(m[14], f.parts[j].matrix[14], 1e-3);
        }
    }

    check(same, "each of the eight is the whole Suzanne, 968 triangles");
    check(ring, "each of the eight stands 3.1 m from the middle, on the ground");
    check(apart, "no two of the eight stand in one place");
    k3d_fbx_free(&f);
}

static void test_mirrored(void)
{
    struct k3d_fbx f;

    if (!read_file("blender_340_mirrored_normals_7400_binary.fbx", &f)) return;

    check(f.nparts == 2 && det3(f.parts[0].matrix) > 0 && det3(f.parts[1].matrix) < 0,
          "a Suzanne and her mirror image, the second's matrix turning space inside out");

    if (f.nparts == 2) {
        check(part_outwards(&f, 0, 1) > 0, "Suzanne faces outwards");
        check(part_outwards(&f, 1, 1) > 0,
              "her mirror image faces outwards, reversed as Cafesa3D reverses it");
        check(part_outwards(&f, 1, 0) < 0,
              "and inwards without the reversal - so the check sees the mirror");
    }

    k3d_fbx_free(&f);
}

static const struct k3d_fbx_material *named(const struct k3d_fbx *f, const char *name)
{
    size_t i;

    for (i = 0; i < f->nmaterials; i++) {
        if (strcmp(f->materials[i].name, name) == 0) return &f->materials[i];
    }

    return NULL;
}

static int colour(const float c[3], double r, double g, double b, double tol)
{
    return near(c[0], r, tol) && near(c[1], g, tol) && near(c[2], b, tol);
}

static void test_multimaterial(void)
{
    struct k3d_fbx f;
    const struct k3d_fbx_material *m;
    size_t i, j;
    int distinct = 1, named_by = 1;

    if (!read_file("blender_suzanne_multimaterial_7400_binary.fbx", &f)) return;

    check(f.nparts == 7 && f.nmaterials == 7, "Suzanne in seven materials is seven parts");

    for (i = 0; i < f.nparts; i++) {
        char want[64];

        if (f.parts[i].material >= f.nmaterials) {
            named_by = 0;
            continue;
        }

        snprintf(want, sizeof(want), "Suzanne %s", f.materials[f.parts[i].material].name);
        named_by &= strcmp(f.parts[i].name, want) == 0;

        for (j = 0; j < i; j++) distinct &= f.parts[i].material != f.parts[j].material;
    }

    check(named_by, "each part named for the object and its material");
    check(distinct, "each part in a material of its own");

    m = named(&f, "RightEar");
    check(m && colour(m->base, 0.0, 0.462, 0.8, 2e-3), "the right ear is blue");
    m = named(&f, "Nose");
    check(m && near(m->metallic, 0.423, 2e-3) && near(m->rough, 0.007, 2e-3),
          "the nose is part metal, and smooth");
    m = named(&f, "Monkey");
    check(m && near(m->rough, 1, 1e-3) && near(m->trans, 0, 1e-6), "the face is rough and opaque");
    k3d_fbx_free(&f);
}

/*--------------------------------------------------------------------------
 * Materials to their numbers.
 *------------------------------------------------------------------------*/

static void test_materials(void)
{
    struct k3d_fbx f;
    const struct k3d_fbx_material *m;

    if (read_file("blender_279_default_7400_binary.fbx", &f)) {
        m = named(&f, "Material");
        check(m && colour(m->base, 0.64, 0.64, 0.64, 1e-3) && near(m->trans, 0, 1e-6)
              && near(m->metallic, 0, 1e-6),
              "Blender's first material: a grey of 0.8 at 0.8, opaque, not metal");
        check(f.parts[0].material < f.nmaterials
              && strcmp(f.materials[f.parts[0].material].name, "Material") == 0,
              "the cube wears it");
        check(f.lamps == 1 && f.cameras == 1, "its lamp and camera counted, not read");
        k3d_fbx_free(&f);
    }

    if (read_file("maya_cube_7100_ascii.fbx", &f)) {
        m = named(&f, "lambert1");
        check(m && colour(m->base, 0.4, 0.4, 0.4, 1e-3),
              "Maya's Lambert: a grey of 0.5 at 0.8");
        check(m && near(m->trans, 0, 1e-6),
              "and opaque - a black transparency at a factor of one lets nothing through");
        check(m && near(m->rough, 1, 1e-6), "and without any shine, being a Lambert");
        k3d_fbx_free(&f);
    }

    if (read_file("blender_293_material_mapping_7400_binary.fbx", &f)) {
        m = named(&f, "Material.001");
        check(m && near(m->trans, 0.544, 2e-3) && near(m->rough, 0.123, 2e-3),
              "Blender's Principled at an alpha of 0.456: 0.544 of the light through");
        k3d_fbx_free(&f);
    }

    if (read_file("max_pbr_metal_rough_material_7700_ascii.fbx", &f)) {
        m = f.nmaterials == 1 ? &f.materials[0] : NULL;
        check(m && colour(m->base, 0.01, 0.02, 0.03, 1e-5) && near(m->metallic, 0.05, 1e-5)
              && near(m->rough, 0.06, 1e-5) && colour(m->emit, 0.08, 0.09, 0.10, 1e-5),
              "3ds Max's PBR material, every number as it was typed");
        k3d_fbx_free(&f);
    }

    if (read_file("max_physical_material_properties_6100_ascii.fbx", &f)) {
        m = f.nmaterials == 1 ? &f.materials[0] : NULL;
        check(m && colour(m->base, 0.0002, 0.0003, 0.0004, 1e-6)
              && near(m->metallic, 0.07, 1e-5) && near(m->rough, 0.06, 1e-5)
              && near(m->trans, 0.09 * 0.11, 1e-5) && near(m->ior, 0.8, 1e-5)
              && colour(m->emit, 0.27 * 0.28, 0.27 * 0.29, 0.27 * 0.30, 1e-5),
              "3ds Max's physical material: each colour at its weight");
        k3d_fbx_free(&f);
    }
}

/*--------------------------------------------------------------------------
 * What is not a model.
 *------------------------------------------------------------------------*/

static void test_refusals(void)
{
    struct k3d_fbx f;
    size_t len, olen, cut, i;
    unsigned char *b = slurp("blender_279_default_7400_binary.fbx", &len);
    unsigned char *obj = slurp("maya_cube.obj", &olen);
    unsigned char *copy = malloc(len);
    const char *why;
    uint32_t seed = 12345;
    int sentences = 1, cut_short = 0, damaged = 0, read_anyway = 0;

    why = k3d_fbx_read(b, 0, &f);
    check(why != NULL && f.nparts == 0 && f.parts == NULL, "nothing is refused");

    why = k3d_fbx_read(obj, olen, &f);
    check(why != NULL && strcmp(why, "not an FBX file") == 0,
          "an OBJ is not an FBX file here - it has a translator of its own");

    /* Every sixty-fourth of the way through, cut there. */
    for (i = 1; i < 64; i++) {
        cut = len * i / 64;
        why = k3d_fbx_read(b, cut, &f);

        if (why) {
            cut_short++;
            sentences &= strlen(why) > 0 && f.parts == NULL && f.nparts == 0;
        } else {
            k3d_fbx_free(&f);
        }
    }

    check(cut_short > 32, "a file cut short is refused");

    /* A thousand copies, each with a few bytes changed. */
    for (i = 0; i < 1000; i++) {
        int k;

        memcpy(copy, b, len);

        for (k = 0; k < 4; k++) {
            seed = seed * 1664525u + 1013904223u;
            copy[(seed >> 8) % len] = (unsigned char)(seed >> 24);
        }

        why = k3d_fbx_read(copy, len, &f);

        if (why) {
            damaged++;
            sentences &= strlen(why) > 0 && f.parts == NULL && f.nparts == 0;
        } else {
            read_anyway++;
            k3d_fbx_free(&f);
        }
    }

    check(sentences, "every refusal a sentence, with nothing kept");
    printf("  %d of 63 cuts refused; of 1000 damaged copies %d refused and %d read\n", cut_short,
           damaged, read_anyway);

    free(copy);
    free(b);
    free(obj);
}

int main(void)
{
    size_t i;

    for (i = 0; i < sizeof(AGAINST) / sizeof(AGAINST[0]); i++) test_against(&AGAINST[i]);

    test_instancing();
    test_mirrored();
    test_multimaterial();
    test_materials();
    test_refusals();

    if (fails) {
        printf("FAIL: %d of %d checks on FBX\n", fails, checks + fails);
        return 1;
    }

    printf("PASS: %d checks on FBX (Blender, Maya and 3ds Max files, binary and text, 6.1 to "
           "7.7, each where its program says it drew it: pivots, instances, a parent, six "
           "rotation orders; a mirror wound as Cafesa3D draws it; seven materials on one "
           "mesh; materials to their numbers; nothing, an OBJ, 63 cuts and 1000 damaged "
           "copies refused or read, never a crash)\n", checks);
    return 0;
}
