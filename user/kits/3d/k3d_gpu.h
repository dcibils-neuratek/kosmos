/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A render's scene, laid out for the GPU's kernel** (`docs/cafesa3d-gpu.md`
 * R4): what `k3d_render_new` snapshots - objects, meshes, the hierarchies,
 * the lamps and the camera - as flat arrays of 32-bit words, each one a
 * Compute Kit buffer the tracer's kernel reads by byte offset.
 *
 * **The hierarchies are threaded rather than stacked.** The processor walks
 * a four-wide tree with a stack of 128 entries a ray; a kernel's lanes
 * would need that stack in registers addressed through `a0`, which the
 * driver's check refuses, or in memory, which at 1280 by 720 is hundreds
 * of megabytes. So each tree is the 3D Kit's own binary one
 * (`k3d_bvh_binary`), laid out depth first, every node carrying where to go
 * when its box is missed - its *skip*, the next node outside it. A ray
 * walks it with nothing but the node it is at: met, the next; missed, the
 * skip; a leaf, its items and then the skip. The order is fixed rather
 * than nearest first, which costs boxes and changes no answer.
 *
 * Every number is in the word its kind's table below names; the tracer's
 * kernel reads them at the same places (`K3D_GPU_*`).
 */

#ifndef KOSMOS_K3D_GPU_H
#define KOSMOS_K3D_GPU_H

#include <stdbool.h>
#include <stdint.h>

#include "k3d.h"

/* A node: eight words. */
#define K3D_GPU_NODE_WORDS   8u
#define K3D_GPU_NODE_LO      0u     /* three floats */
#define K3D_GPU_NODE_HI      3u     /* three floats */
#define K3D_GPU_NODE_SKIP    6u     /* the next node outside this one, or K3D_GPU_END */
#define K3D_GPU_NODE_LEAF    7u     /* 0 for an inner node; else 1 + where in the items its count is */

#define K3D_GPU_END          0xffffffffu

/* The items: a leaf's count, then that many - objects for the top tree,
 * absolute triangles for a mesh's. */

/* An object: 64 words, 256 bytes. */
#define K3D_GPU_OBJ_WORDS    64u
#define K3D_GPU_OBJ_SHAPE    0u     /* K3D_GPU_MESH ... K3D_GPU_PLANE */
#define K3D_GPU_OBJ_SMOOTH   1u
#define K3D_GPU_OBJ_ROOT     2u     /* a mesh's tree's first node, or K3D_GPU_END */
#define K3D_GPU_OBJ_ID       3u     /* the scene's id for it */
#define K3D_GPU_OBJ_I        4u     /* twelve floats: from the world into its own space */
#define K3D_GPU_OBJ_N        16u    /* nine floats: its normals out to the world */
#define K3D_GPU_OBJ_A        25u    /* a sphere's radius; a box's halves; a cylinder's */
#define K3D_GPU_OBJ_B        26u    /*   radius and half its depth; a plane's half side */
#define K3D_GPU_OBJ_C        27u
#define K3D_GPU_OBJ_BASE     28u    /* three floats, linear */
#define K3D_GPU_OBJ_METALLIC 31u
#define K3D_GPU_OBJ_ROUGH    32u
#define K3D_GPU_OBJ_TRANS    33u
#define K3D_GPU_OBJ_IOR      34u
#define K3D_GPU_OBJ_EMIT     35u
#define K3D_GPU_OBJ_PATTERN  36u    /* enum k3d_pattern: K3D_PLAIN, nought, is none (R7b) */
#define K3D_GPU_OBJ_COLOUR2  37u    /* three floats, linear: the pattern's second colour */
#define K3D_GPU_OBJ_TSCALE   40u    /* struct k3d_texture's scale, detail, distortion, */
#define K3D_GPU_OBJ_DETAIL   41u    /*   bump, mortar, ratio and offset, in its order */
#define K3D_GPU_OBJ_DISTORT  42u
#define K3D_GPU_OBJ_BUMP     43u
#define K3D_GPU_OBJ_MORTAR   44u
#define K3D_GPU_OBJ_RATIO    45u
#define K3D_GPU_OBJ_OFFSET   46u
#define K3D_GPU_OBJ_SCALE    47u    /* three floats: its scale, so a texture is in its metres */

enum { K3D_GPU_MESH = 0, K3D_GPU_SPHERE = 1, K3D_GPU_BOX = 2, K3D_GPU_CYLINDER = 3, K3D_GPU_PLANE = 4 };

/* A triangle: twelve words, in its object's own space, as the processor
 * packs it - a corner and the two edges from it - and the three corners'
 * places in the normals, for a smooth mesh. */
#define K3D_GPU_TRI_WORDS    12u
#define K3D_GPU_TRI_V0       0u
#define K3D_GPU_TRI_E1       3u
#define K3D_GPU_TRI_E2       6u
#define K3D_GPU_TRI_CORNERS  9u

/* A lamp: eight words. */
#define K3D_GPU_LAMP_WORDS   8u
#define K3D_GPU_LAMP_POS     0u
#define K3D_GPU_LAMP_RADIUS  3u
#define K3D_GPU_LAMP_COLOUR  4u
#define K3D_GPU_LAMP_POWER   7u

struct k3d_gpu_scene {
    uint32_t *nodes, nnodes;        /* in nodes */
    uint32_t *items, nitems;        /* in words */
    uint32_t *objects, nobjects;    /* in objects */
    uint32_t *tris, ntris;          /* in triangles */
    float    *normals;              /* three a vertex */
    uint32_t nnormals;              /* in vertices */
    uint32_t *lamps, nlamps;
    uint32_t root;                  /* the top tree's first node, or K3D_GPU_END */

    /* The camera, as the processor's rays start from it. */
    float eye[3], right[3], up[3], forward[3], F;
    int   w, h;
    struct k3d_world world;
    bool  preview;
    int   bounces;
};

/* The render's scene laid out; false when there was no memory. */
bool k3d_gpu_scene(const struct k3d_render *r, struct k3d_gpu_scene *out);
void k3d_gpu_scene_free(struct k3d_gpu_scene *s);

#endif
