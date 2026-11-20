/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A binary glTF read into parts to draw** (`docs/flight.md` F5): an
 * aircraft as triangle lists, each with its base colour, its texture and
 * the matrix that places it, and which of its moving pieces - an aileron,
 * the propeller - it belongs to, with where that piece turns.
 *
 *     a part        one triangle list: vertices as `k3d_vk_model_vertex`
 *                   lays them out (position, normal, uv), 32-bit indices,
 *                   a base colour and a texture, a column-major matrix
 *                   putting it in the model's frame, and its movable;
 *     a movable     a node the caller names - "elevator_0", "prop_5" - and
 *                   everything under it in the hierarchy, or skinned to
 *                   it: its pivot, its own axes, and the part's size along
 *                   each, so the caller turns it about the right one;
 *     the airframe  everything else, movable -1, nodes and bones whose
 *                   names the caller did not give among them;
 *     a texture     where its picture's bytes are in the buffer handed
 *                   in - PNG or JPEG, decoded elsewhere (gfx's readers).
 *
 * **The frame is glTF's**: Y up, metres, the scene's root nodes placed by
 * their matrices from the identity. Nothing is turned into this kit's own
 * Z up (`k3d.h`), because the renderer that draws these (`k3d_vk.c`) is
 * Y up too. For the Cessna 172 in `assets/models/cessna172/` that puts the
 * nose towards +X, the right wing towards +Z and the wheels on y = 0.
 *
 * **Skinning is resolved rigidly**, because the bones of an aircraft are
 * hinges and not flesh: each vertex goes to the joint that weighs most on
 * it, and is placed in the model's frame by that joint's matrix times its
 * inverse bind matrix - exactly where glTF's skinning puts it at rest. A
 * primitive whose triangles fall to more than one movable is split, one
 * part each; a triangle goes where two or three of its vertices go, or
 * where its first does when all three differ. Those parts carry the
 * identity as their matrix, since their vertices are already in the
 * model's frame. An unskinned mesh keeps its own coordinates and its
 * node's matrix, as a renderer that instances would want it.
 *
 * **Which axis a movable turns about is reported, not assumed.** A
 * bone's roll is the modeller's choice - the Cessna's elevator hinges
 * about its bone's Z and its rudder about its bone's X - so no one local
 * axis is the hinge. What is not a choice is the shape: a control surface
 * is long along its hinge. So each movable carries its node's three axes
 * in the model's frame, the size of its parts along each, and `hinge`,
 * the longest, given the sign that makes its largest component positive -
 * the elevator's is +Z, towards the right wing. A positive turn is the
 * right hand's about it. **A propeller is not a hinge, and its shape does
 * not say its axis**: two blades at rest are long along one axis and, with
 * the spinner, about as thin along the other two (the Cessna's: 0.23 m
 * across the blade, 0.35 m along the line of flight). Which of `axes` it
 * spins about is the caller's to name - the Cessna's is its Z, which is
 * -X, the line of flight.
 *
 * What is read: one scene (the file's `scene`, or the first), nodes by
 * matrix or by translation, rotation and scale, triangle primitives with
 * POSITION, NORMAL (worked out from the faces when absent), the base
 * colour's TEXCOORD_n (float, or normalised 8 and 16 bit), indices of
 * 8, 16 or 32 bits or none, JOINTS_0 and WEIGHTS_0, and materials' base
 * colour factor and texture. What is not drawn is counted in `skipped`:
 * primitives that are not triangle lists, sparse or out-of-range
 * accessors, indices past their vertices. Every offset and count in the
 * file is held to the bytes before it is followed, since a file is
 * whatever somebody put in it. JSON keys are compared as written, so an
 * escaped key ("nodes") is a key this does not know.
 *
 * Freestanding: malloc, realloc, free, memcpy, memcmp, memset, strtod and
 * sqrt, as `runtime/libc` has them. `tools/test_glb.c` holds it on the
 * Mac against the Cessna.
 */

#ifndef K3D_GLB_H
#define K3D_GLB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The most named pieces one model may have. */
#define K3D_GLB_MOVABLES 16

/* The same layout as `k3d_vk_model_vertex`, so a part's vertices go to
 * the renderer as they are. */
struct k3d_glb_vertex {
    float position[3];
    float normal[3];                /* unit length */
    float uv[2];                    /* glTF's: (0, 0) the picture's top left */
};

struct k3d_glb_part {
    struct k3d_glb_vertex *vertices;
    uint32_t *indices;              /* three a triangle */
    uint32_t nvertices, nindices;
    int texture;                    /* base colour: into `textures`, or -1 */
    float colour[4];                /* base colour factor, linear RGBA */
    float matrix[16];               /* column-major: the model's frame from
                                     * the part's own coordinates */
    int movable;                    /* -1 the airframe, else into `movables` */
    int material;                   /* glTF's, or -1 */
    int mesh;                       /* glTF's, for a message */
};

struct k3d_glb_texture {
    size_t offset, length;          /* its picture, in the bytes read; a
                                     * length of 0: not in the file */
    char mime[16];                  /* "image/png", "image/jpeg", or "" */
};

struct k3d_glb_movable {
    int node;                       /* glTF's; -1: the file has none so named */
    float pivot[3];                 /* the node's origin, in the model frame */
    float axes[3][3];               /* the node's own X, Y and Z there, unit */
    float extent[3];                /* its parts' size along each, metres */
    float hinge[3];                 /* the axis it is longest along */
    char hinge_axis;                /* which of the node's it is: 'X'.. */
    uint32_t triangles;
};

struct k3d_glb {
    struct k3d_glb_part *parts;
    uint32_t nparts;
    struct k3d_glb_texture *textures;   /* glTF's textures, in order */
    uint32_t ntextures;
    struct k3d_glb_movable movables[K3D_GLB_MOVABLES];
    unsigned nmovables;             /* as many as the caller named */
    uint32_t triangles;             /* in every part */
    uint32_t skipped;               /* primitives not drawn (above) */
    float lo[3], hi[3];             /* every part's vertices, model frame */
};

/* Read the `n` bytes at `bytes`, a binary glTF, into `out`. `names` are
 * the nodes that move, `count` of them, at most K3D_GLB_MOVABLES; a
 * part's `movable` indexes them. False with `*why` a sentence when the
 * file is not one this reads; `out` then holds nothing to free. The
 * textures point into `bytes`, which must outlive them. */
bool k3d_glb_read(const uint8_t *bytes, size_t n, const char *const *names, unsigned count,
                  struct k3d_glb *out, const char **why);

void k3d_glb_free(struct k3d_glb *g);

#endif
