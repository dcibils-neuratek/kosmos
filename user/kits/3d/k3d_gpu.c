/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A render's scene laid out for the GPU (`k3d_gpu.h`).
 */

#include <stdlib.h>
#include <string.h>

#include "k3d_gpu.h"
#include "k3d_trace.h"

static void put_f(uint32_t *at, float x) { memcpy(at, &x, 4); }

/* Counted before anything is laid, so every array is one allocation. */
struct sizes {
    uint32_t nodes, items, tris, normals;
};

struct lay {
    struct k3d_gpu_scene *s;
    uint32_t node, item;            /* where the next node and item go */
};

/*
 * One binary tree, depth first from `at`, its leaves' items through `map`:
 * an object's index, or a triangle's absolute place. The skip of each node
 * is the node after its subtree; the tree's last ones skip to the end.
 */
static void lay_tree(struct lay *L, const struct bnode *bn, const uint32_t *order, uint32_t at,
                     uint32_t map_base, uint32_t tree_end)
{
    uint32_t me = L->node++, *nd = &L->s->nodes[me * K3D_GPU_NODE_WORDS], k;
    const struct bnode *b = &bn[at];

    for (k = 0; k < 3; k++) {
        put_f(&nd[K3D_GPU_NODE_LO + k], b->lo[k]);
        put_f(&nd[K3D_GPU_NODE_HI + k], b->hi[k]);
    }

    if (b->count > 0) {
        nd[K3D_GPU_NODE_LEAF] = 1u + L->item;
        L->s->items[L->item++] = b->count;

        for (k = 0; k < b->count; k++) {
            L->s->items[L->item++] = map_base + order[b->first + k];
        }
    } else {
        nd[K3D_GPU_NODE_LEAF] = 0;
        lay_tree(L, bn, order, b->first, map_base, tree_end);
        lay_tree(L, bn, order, b->first + 1, map_base, tree_end);
    }

    nd[K3D_GPU_NODE_SKIP] = L->node == tree_end ? K3D_GPU_END : L->node;
}

/* A tree's nodes and items, to count them. */
static void measure(const struct bnode *bn, uint32_t at, uint32_t *nodes, uint32_t *items)
{
    (*nodes)++;

    if (bn[at].count > 0) {
        *items += 1u + bn[at].count;
    } else {
        measure(bn, bn[at].first, nodes, items);
        measure(bn, bn[at].first + 1, nodes, items);
    }
}

/* A mesh's binary tree over its triangles' boxes. */
static bool mesh_tree(const struct tobj *o, struct bnode **bn, uint32_t *used, uint32_t **order)
{
    float *lo = malloc((size_t)o->ntri * 3 * sizeof(float));
    float *hi = malloc((size_t)o->ntri * 3 * sizeof(float));
    uint32_t i, j, k;
    bool ok;

    if (lo == NULL || hi == NULL) {
        free(lo);
        free(hi);
        return false;
    }

    for (i = 0; i < o->ntri; i++) {
        for (k = 0; k < 3; k++) {
            lo[i * 3 + k] = 1e30f;
            hi[i * 3 + k] = -1e30f;
        }

        for (j = 0; j < 3; j++) {
            const float *p = &o->pos[o->tri[i * 3 + j] * 3];

            for (k = 0; k < 3; k++) {
                lo[i * 3 + k] = p[k] < lo[i * 3 + k] ? p[k] : lo[i * 3 + k];
                hi[i * 3 + k] = p[k] > hi[i * 3 + k] ? p[k] : hi[i * 3 + k];
            }
        }
    }

    ok = k3d_bvh_binary(lo, hi, o->ntri, bn, used, order);
    free(lo);
    free(hi);
    return ok;
}

void k3d_gpu_scene_free(struct k3d_gpu_scene *s)
{
    free(s->nodes);
    free(s->items);
    free(s->objects);
    free(s->tris);
    free(s->normals);
    free(s->lamps);
    memset(s, 0, sizeof(*s));
}

bool k3d_gpu_scene(const struct k3d_render *r, struct k3d_gpu_scene *out)
{
    struct bnode *top = NULL, **mesh_bn = NULL;
    uint32_t top_used = 0, *top_order = NULL, **mesh_order = NULL;
    struct sizes n = { 0, 0, 0, 0 };
    struct lay L;
    uint32_t i, k, tri_at = 0, nrm_at = 0;
    bool ok = false;

    memset(out, 0, sizeof(*out));

    mesh_bn = calloc(r->nobj ? r->nobj : 1, sizeof(*mesh_bn));
    mesh_order = calloc(r->nobj ? r->nobj : 1, sizeof(*mesh_order));

    if (mesh_bn == NULL || mesh_order == NULL) {
        goto done;
    }

    /* The trees, built and counted. */
    if (r->nobj > 0) {
        float *lo = malloc(r->nobj * 3 * sizeof(float)), *hi = malloc(r->nobj * 3 * sizeof(float));

        if (lo == NULL || hi == NULL) {
            free(lo);
            free(hi);
            goto done;
        }

        for (i = 0; i < r->nobj; i++) {
            memcpy(&lo[i * 3], r->obj[i].lo, sizeof(r->obj[i].lo));
            memcpy(&hi[i * 3], r->obj[i].hi, sizeof(r->obj[i].hi));
        }

        ok = k3d_bvh_binary(lo, hi, r->nobj, &top, &top_used, &top_order);
        free(lo);
        free(hi);

        if (!ok) {
            goto done;
        }

        ok = false;
        measure(top, 0, &n.nodes, &n.items);
    }

    for (i = 0; i < r->nobj; i++) {
        const struct tobj *o = &r->obj[i];
        uint32_t used, nverts = 0;

        if (o->shape != MESH) {
            continue;
        }

        if (!mesh_tree(o, &mesh_bn[i], &used, &mesh_order[i])) {
            goto done;
        }

        measure(mesh_bn[i], 0, &n.nodes, &n.items);
        n.tris += o->ntri;

        for (k = 0; k < o->ntri * 3; k++) {
            nverts = o->tri[k] + 1 > nverts ? o->tri[k] + 1 : nverts;
        }

        n.normals += nverts;
    }

    out->nodes = calloc(n.nodes ? n.nodes : 1, K3D_GPU_NODE_WORDS * 4);
    out->items = calloc(n.items ? n.items : 1, 4);
    out->objects = calloc(r->nobj ? r->nobj : 1, K3D_GPU_OBJ_WORDS * 4);
    out->tris = calloc(n.tris ? n.tris : 1, K3D_GPU_TRI_WORDS * 4);
    out->normals = calloc(n.normals ? n.normals : 1, 3 * sizeof(float));
    out->lamps = calloc(r->how.nlights > 0 ? (size_t)r->how.nlights : 1, K3D_GPU_LAMP_WORDS * 4);

    if (!out->nodes || !out->items || !out->objects || !out->tris || !out->normals || !out->lamps) {
        goto done;
    }

    L.s = out;
    L.node = 0;
    L.item = 0;

    /* The top tree first, its leaves' items the objects. */
    out->root = K3D_GPU_END;

    if (r->nobj > 0) {
        uint32_t nodes = 0, items = 0;

        measure(top, 0, &nodes, &items);
        out->root = 0;
        lay_tree(&L, top, top_order, 0, 0, nodes);
    }

    /* Each object, and a mesh's tree, triangles and normals after. */
    for (i = 0; i < r->nobj; i++) {
        const struct tobj *o = &r->obj[i];
        uint32_t *w = &out->objects[i * K3D_GPU_OBJ_WORDS];
        static const uint32_t shapes[] = { [MESH] = K3D_GPU_MESH, [SPHERE] = K3D_GPU_SPHERE,
                                           [BOX] = K3D_GPU_BOX, [CYLINDER] = K3D_GPU_CYLINDER,
                                           [PLANE] = K3D_GPU_PLANE };

        w[K3D_GPU_OBJ_SHAPE] = shapes[o->shape];
        w[K3D_GPU_OBJ_SMOOTH] = o->smooth ? 1u : 0u;
        w[K3D_GPU_OBJ_ROOT] = K3D_GPU_END;
        w[K3D_GPU_OBJ_ID] = o->id;

        for (k = 0; k < 12; k++) { put_f(&w[K3D_GPU_OBJ_I + k], o->I[k]); }
        for (k = 0; k < 9; k++) { put_f(&w[K3D_GPU_OBJ_N + k], o->N[k]); }

        put_f(&w[K3D_GPU_OBJ_A], o->a);
        put_f(&w[K3D_GPU_OBJ_B], o->b);
        put_f(&w[K3D_GPU_OBJ_C], o->c);

        for (k = 0; k < 3; k++) { put_f(&w[K3D_GPU_OBJ_BASE + k], o->mat.base[k]); }

        put_f(&w[K3D_GPU_OBJ_METALLIC], o->mat.metallic);
        put_f(&w[K3D_GPU_OBJ_ROUGH], o->mat.rough);
        put_f(&w[K3D_GPU_OBJ_TRANS], o->mat.trans);
        put_f(&w[K3D_GPU_OBJ_IOR], o->mat.ior);
        put_f(&w[K3D_GPU_OBJ_EMIT], o->mat.emit);
        w[K3D_GPU_OBJ_TEXTURED] = o->mat.tex.pattern != K3D_PLAIN ? 1u : 0u;

        if (o->shape == MESH) {
            uint32_t nodes = 0, items = 0, t, nverts = 0;

            measure(mesh_bn[i], 0, &nodes, &items);
            w[K3D_GPU_OBJ_ROOT] = L.node;
            lay_tree(&L, mesh_bn[i], mesh_order[i], 0, tri_at, L.node + nodes);

            /* The triangles as the processor packs them (`fill_lane`). */
            for (t = 0; t < o->ntri; t++) {
                uint32_t *tw = &out->tris[(tri_at + t) * K3D_GPU_TRI_WORDS];
                const uint32_t *c = &o->tri[t * 3];
                const float *p0 = &o->pos[c[0] * 3], *p1 = &o->pos[c[1] * 3], *p2 = &o->pos[c[2] * 3];

                for (k = 0; k < 3; k++) {
                    put_f(&tw[K3D_GPU_TRI_V0 + k], p0[k]);
                    put_f(&tw[K3D_GPU_TRI_E1 + k], p1[k] - p0[k]);
                    put_f(&tw[K3D_GPU_TRI_E2 + k], p2[k] - p0[k]);
                    tw[K3D_GPU_TRI_CORNERS + k] = nrm_at + c[k];
                    nverts = c[k] + 1 > nverts ? c[k] + 1 : nverts;
                }
            }

            memcpy(&out->normals[nrm_at * 3], o->nrm, (size_t)nverts * 3 * sizeof(float));
            tri_at += o->ntri;
            nrm_at += nverts;
        }
    }

    out->nnodes = L.node;
    out->nitems = L.item;
    out->nobjects = r->nobj;
    out->ntris = tri_at;
    out->nnormals = nrm_at;
    out->nlamps = r->how.nlights > 0 ? (uint32_t)r->how.nlights : 0;

    for (i = 0; i < out->nlamps; i++) {
        uint32_t *w = &out->lamps[i * K3D_GPU_LAMP_WORDS];
        const struct k3d_light *l = &r->lights[i];

        for (k = 0; k < 3; k++) {
            put_f(&w[K3D_GPU_LAMP_POS + k], l->pos[k]);
            put_f(&w[K3D_GPU_LAMP_COLOUR + k], l->colour[k]);
        }

        put_f(&w[K3D_GPU_LAMP_RADIUS], l->radius);
        put_f(&w[K3D_GPU_LAMP_POWER], l->power);
    }

    for (k = 0; k < 3; k++) {
        out->eye[k] = r->eye[k];
        out->right[k] = r->r[k];
        out->up[k] = r->u[k];
        out->forward[k] = r->f[k];
    }

    out->F = r->F;
    out->w = r->how.w;
    out->h = r->how.h;
    out->world = r->how.world;
    out->preview = r->how.preview;
    out->bounces = r->how.bounces > 0 ? r->how.bounces : 6;
    ok = true;

done:
    if (mesh_bn) {
        for (i = 0; i < r->nobj; i++) {
            free(mesh_bn[i]);
        }
    }

    if (mesh_order) {
        for (i = 0; i < r->nobj; i++) {
            free(mesh_order[i]);
        }
    }

    free(mesh_bn);
    free(mesh_order);
    free(top);
    free(top_order);

    if (!ok) {
        k3d_gpu_scene_free(out);
    }

    return ok;
}
