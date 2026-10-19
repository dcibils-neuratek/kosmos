/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The ray tracer's own structures**, the 3D Kit's and nobody else's: the
 * snapshot `k3d_render_new` makes of a scene - its objects, their meshes and
 * the hierarchies over them - shared by `k3d_trace.c`, which traces it on
 * the processor, and `k3d_gpu.c`, which lays it out for the GPU's kernel
 * (`docs/cafesa3d-gpu.md` R4). Not a door: programs see `k3d.h`.
 */

#ifndef KOSMOS_K3D_TRACE_H
#define KOSMOS_K3D_TRACE_H

#include <stdbool.h>
#include <stdint.h>

#include "k3d.h"

/* Four lanes of the processor's vector unit; a point or a direction is
 * four, the fourth nought. */
typedef float   f4 __attribute__((vector_size(16)));
typedef int32_t i4 __attribute__((vector_size(16)));
typedef f4 v3;


/* The binary hierarchy, as the surface area heuristic builds it: a node's
 * children at `first` and `first + 1`, or a leaf's `count` items from
 * `first` in the item list. */
struct bnode {
    float lo[3], hi[3];
    uint32_t first;         /* an inner node's left child; a leaf's first item */
    uint32_t count;         /* items in a leaf; nought for an inner node */
};

/*
 * The binary hierarchy over `n` boxes - three floats a box low, three
 * high: its nodes, `*used` of them, the root at 0, and its items' order.
 * Both are the caller's to free. False when there was no memory.
 */
bool k3d_bvh_binary(const float *lo, const float *hi, uint32_t n,
                    struct bnode **node, uint32_t *used, uint32_t **item);

/*
 * A world ray against object `obj` alone, as the tracer meets it: true and
 * `*t` the nearer distance when it is met before `*t`. For the tests that
 * hold another walk of the scene to this one (`tools/test_k3d_gpu.c`).
 */
bool k3d_trace_object_hit(const struct k3d_render *r, uint32_t obj, const float o[3],
                          const float d[3], float *t);

/* The four-wide hierarchy the processor's rays walk. */
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

#endif
