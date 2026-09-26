/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The 3D Kit: a scene held in C, and the ways of drawing it.
 *
 * **One scene, drawn two ways.** Cafesa3D (`roadmap.md` 4l) shows a scene
 * rasterised while it is being arranged - the Solid and Wireframe views -
 * and ray traced when it is being judged. Both read the same objects, so
 * the objects live here, in C, and the application holds only their names
 * and its own state: what a thing is called and which tab is open is Lua's,
 * where its triangles are is this file's.
 *
 * **Why not the GL kit.** TinyGL draws the cube and the gears well, and it
 * was the first answer. Three things decided against it for this: picking
 * and Blender's outline both want to know *which object* is on each pixel,
 * which TinyGL has no buffer for; TinyGL is fed a vertex at a time, so every
 * change to a sphere would be a thousand calls through Lua; and the ray
 * tracer needs the scene in C whatever draws the Solid view. So the Solid
 * view is a small rasteriser of its own, with a depth buffer and an object
 * buffer beside the pixels.
 *
 * Nothing in this header knows Lua: `k3d_kosmos.c` is the binding, and
 * `tools/test_k3d.c` compiles the rest on the Mac and holds it there.
 */

#ifndef K3D_H
#define K3D_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The shapes a scene is made of, as Blender's Add menu names them. */
enum k3d_kind {
    K3D_PLANE,
    K3D_BOX,
    K3D_SPHERE,          /* a UV sphere */
    K3D_CYLINDER,
    K3D_ICO,             /* an ico sphere */
    K3D_CONE,
    K3D_TORUS,
    K3D_GRID,
    K3D_MESH,            /* triangles given, not made from numbers */
};

/*
 * A shape as triangles, in the object's own space.
 *
 * `edge` is the shape's own edges - a cube has twelve, not eighteen - which
 * is what Wireframe draws: the diagonal that splits a quad into two
 * triangles is the rasteriser's business and never the modeller's.
 */
struct k3d_mesh {
    float    *pos;          /* three a vertex */
    float    *nrm;          /* three a vertex: the smooth normal */
    uint32_t *tri;          /* three a triangle, anticlockwise from outside */
    uint32_t *edge;         /* two an edge */
    uint32_t  nverts, ntris, nedges;
};

/*
 * **A texture: a pattern worked out from where a point is** (`k3d_texture.c`),
 * mixing the base colour towards `colour2` and standing the surface up by
 * `bump` metres where it is high. Blender's texture nodes, in the numbers
 * they have: `scale` is how many a metre, `detail` a noise's octaves,
 * `distortion` how far noise pushes a wood's rings or a marble's veins;
 * a brick is `ratio` times as long as it is tall, with `mortar` of its
 * height between, and each row moved `offset` of a brick along.
 */
enum k3d_pattern {
    K3D_PLAIN, K3D_CHECKER, K3D_BRICK, K3D_SHINGLES, K3D_NOISE, K3D_WOOD, K3D_MARBLE
};

struct k3d_texture {
    enum k3d_pattern pattern;
    float colour2[3];       /* linear, as `base` is */
    float scale, detail, distortion, bump;
    float mortar, ratio, offset;
};

/* The pattern at `q`, in the object's own metres, on a surface facing `n` in
 * the object's own space: how far towards the second colour, and how high,
 * nought to one. */
void  k3d_pattern(const struct k3d_texture *t, const float q[3], const float n[3],
                  float *fac, float *height);

/* Perlin's gradient noise in octaves, about nought and within one. */
float k3d_noise(const float p[3], float detail);

/*
 * What a surface is made of, for the ray tracer: Blender's Principled names
 * and ranges, cut to the ones Cafesa3D's Material tab has. `base` is linear
 * light, not the sRGB a colour picker shows; the binding converts.
 */
struct k3d_material {
    float base[3];
    float metallic, rough, trans, ior, emit;
    struct k3d_texture tex;
};

/*
 * An object: where it is, what shape, what colour in the Solid view.
 *
 * `rot` is in degrees about X, then Y, then Z - Blender's default XYZ Euler
 * - and `loc`, `rot` and `scale` are the object's, never its mesh's: moving
 * an object does not touch a vertex, which is the object/data split the
 * drawing takes from Blender.
 */
struct k3d_object {
    uint32_t id;            /* stable, from 1; 0 is "nothing" */
    enum k3d_kind kind;

    /*
     * A shape's own numbers, by Blender's names where one of these fields
     * serves two shapes:
     *
     *   box       size[0..2], its sides
     *   plane     size[0]
     *   grid      size[0], and `segments` by `rings` squares
     *   sphere    radius, `segments` round the equator, `rings` pole to pole
     *   ico       radius, `subdivisions` (1 is the icosahedron's 20 faces)
     *   cylinder  radius, depth, `segments` sides
     *   cone      radius at the base, `radius2` at the top, depth, `segments`
     *   torus     `radius` the major, `radius2` the minor; `segments` round
     *             the ring, `rings` round the tube
     */
    float size[3];
    float radius, radius2;
    float depth;
    int   segments;
    int   rings;
    int   subdivisions;

    float loc[3], rot[3], scale[3];

    uint32_t colour;        /* 0xRRGGBB, as the Solid view shows it */
    float    alpha;         /* below one: drawn through, after the rest */
    bool     smooth;
    bool     hidden;
    bool     faceted;       /* traced as its triangles, not the true shape */

    struct k3d_material mat;

    struct k3d_mesh mesh;
    bool     stale;         /* the mesh wants building again */
};

struct k3d_scene {
    struct k3d_object *obj;
    uint32_t count, cap;
    uint32_t next_id;
};

/*
 * A camera: where it is, the way it looks, how wide.
 *
 * Camera space is x to the right, y up and z away from the eye, so a
 * point's depth is its z and the screen is at `F` pixels from the eye.
 */
struct k3d_camera {
    float eye[3];
    float r[3], u[3], f[3];
    float F;                /* focal length, in pixels */
    float cx, cy;           /* the centre of the picture */
    float near;
};

/*
 * A view: a camera, and the two buffers beside the pixels.
 *
 * `depth` holds 1/z, which is linear across the screen where z is not, and
 * nought for "nothing here" - so nearer is larger and a cleared buffer needs
 * no special value. `ids` holds the object on each pixel, which is picking
 * and the outline both.
 */
struct k3d_view {
    int w, h;
    float    *depth;
    uint32_t *ids;
    struct k3d_camera cam;

    /* Scratch for one object's vertices, kept between draws. */
    float    *world;        /* three a vertex */
    float    *camv;         /* three a vertex */
    uint32_t *shade;        /* a colour a vertex, for smooth shading */
    uint32_t  scratch;      /* vertices the scratch holds */
};

/* Where colour goes: a surface's pixels from the view's corner. */
struct k3d_target {
    uint32_t *px;
    size_t    pitch;        /* in pixels, never assumed to be the width */
};

enum k3d_mode {
    K3D_SOLID,
    K3D_WIRE,
};

struct k3d_draw {
    enum k3d_mode mode;
    uint32_t selected;          /* an object's id, or 0 */
    bool     grid;
    uint32_t sky_top, sky_bottom;
    uint32_t outline;           /* the selection's colour */
};

/* What a draw did, for the foot and for the tests. */
struct k3d_drawn {
    uint32_t triangles;         /* rasterised, after culling */
    uint32_t objects;
};

/* The scene. */
void               k3d_scene_init(struct k3d_scene *s);
void               k3d_scene_free(struct k3d_scene *s);
struct k3d_object *k3d_scene_add(struct k3d_scene *s, enum k3d_kind kind);
void               k3d_object_defaults(struct k3d_object *o, enum k3d_kind kind);
struct k3d_object *k3d_scene_find(struct k3d_scene *s, uint32_t id);
bool               k3d_scene_remove(struct k3d_scene *s, uint32_t id);

/* A shape's triangles, built from its numbers; false when out of memory. */
bool     k3d_mesh_build(struct k3d_object *o);
void     k3d_mesh_free(struct k3d_mesh *m);

/*
 * **A mesh's own triangles**, for `K3D_MESH`: `nverts` points and `ntris`
 * triangles of indices into them, wound anticlockwise from outside. Each
 * corner's normal is the average of the faces round its point that meet
 * this face at less than `smooth_degrees` - Blender's auto smooth - so a
 * bevel is round and the edge of a box is sharp in one mesh. False, and
 * nothing changed, for an index past the points or no memory.
 */
bool     k3d_mesh_set(struct k3d_object *o, const float *pos, uint32_t nverts,
                      const uint32_t *tri, uint32_t ntris, float smooth_degrees);
uint32_t k3d_triangles(const struct k3d_object *o);

/* Object space to world space, and a normal likewise (unit length). */
void k3d_object_matrix(const struct k3d_object *o, float m[12]);
void k3d_normal_matrix(const struct k3d_object *o, float n[9]);

/* The view. */
bool k3d_view_init(struct k3d_view *v, int w, int h);
void k3d_view_free(struct k3d_view *v);
void k3d_view_look(struct k3d_view *v, const float eye[3],
                   const float target[3], float fov);

/* A world point on the screen: false behind the eye. `iz` is its 1/z. */
bool k3d_project(const struct k3d_view *v, const float p[3],
                 float *sx, float *sy, float *iz);

/* Draw the scene; `t` is where colour goes, `v->w` by `v->h` of it. */
struct k3d_drawn k3d_draw(struct k3d_view *v, struct k3d_scene *s,
                          const struct k3d_target *t,
                          const struct k3d_draw *how);

/*
 * A segment in the world, one pixel wide, blended at `alpha` (0..255), and
 * held behind what is in front of it when `depth` is set.
 */
void k3d_line(struct k3d_view *v, const struct k3d_target *t,
              const float a[3], const float b[3], uint32_t colour,
              unsigned alpha, bool depth);

/* The object on a pixel of the last draw, or 0. */
uint32_t k3d_pick(const struct k3d_view *v, int x, int y);

/*--------------------------------------------------------------------------
 * The ray tracer (`k3d_trace.c`).
 *
 * **A render reads a snapshot**, made when it starts: every object's place,
 * shape and material copied, and a bounding volume hierarchy built over each
 * mesh's triangles and another over the objects. So the threads that trace
 * never look at anything the application is changing, and never allocate -
 * `malloc` is not under a lock yet (`threads.md` step 7).
 *
 * **The work is tiles and passes**: a pass is one sample of every pixel, a
 * tile sixteen pixels square, and a job one pass of one tile. Jobs are taken
 * in order from one counter, so any number of threads can take them; a
 * tile's next pass waits for its last, so no two ever add into the same
 * pixels. Each sample's random numbers come from its tile, pass and pixel,
 * never from which thread drew it - so a render is the same, bit for bit,
 * whether one thread made it or eight.
 *------------------------------------------------------------------------*/

struct k3d_light {
    float pos[3];
    float radius;
    float colour[3];        /* linear */
    float power;            /* watts, as Blender's point light */
};

struct k3d_world {
    float zenith[3], horizon[3];    /* linear */
    float strength;
};

struct k3d_render_setup {
    int   w, h;
    float eye[3], target[3], fov;   /* fov across the width, radians */
    bool  preview;                  /* Whitted, not paths */
    int   bounces;
    uint32_t passes;                /* stop after this many */
    struct k3d_world world;
    const struct k3d_light *lights;
    int   nlights;
};

struct k3d_render;

struct k3d_render *k3d_render_new(struct k3d_scene *s, const struct k3d_render_setup *how);
void     k3d_render_free(struct k3d_render *r);

/* The next job, taken: false when every pass is done or the render was
 * stopped. `yield` is called while a tile's last pass is still being drawn. */
bool     k3d_render_job(struct k3d_render *r, uint32_t *tile, uint32_t *pass,
                        void (*yield)(void));
void     k3d_render_tile(struct k3d_render *r, uint32_t tile, uint32_t pass);
void     k3d_render_stop(struct k3d_render *r);

/* The least number of passes every tile has had, and how many rays so far. */
uint32_t k3d_render_passes(const struct k3d_render *r);
uint64_t k3d_render_rays(const struct k3d_render *r);

/* The light so far, tone-mapped into pixels: `w` by `h` of them. Only the
 * tiles that have had a pass since the last paint, unless `all`; answers
 * how many tiles it painted. From one thread, as a window is drawn from. */
uint32_t k3d_render_paint(struct k3d_render *r, uint32_t *px, size_t pitch, bool all);

/* One ray's first hit, for tests and for picking what a render shows. */
bool     k3d_render_first_hit(const struct k3d_render *r, const float o[3],
                              const float d[3], float *t, uint32_t *id);

#endif
