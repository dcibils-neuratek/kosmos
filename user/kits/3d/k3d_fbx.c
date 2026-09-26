/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * FBX, through ufbx (`roadmap.md` 4l, 5c): Autodesk's format, which game
 * asset stores and Mixamo hand out and which nobody documents. ufbx
 * (`runtime/upstream/ufbx/`) reads the file; this turns what it read into
 * the parts and materials `/lib/translators/fbx.lua` gives Cafesa3D.
 *
 * **A part for each run of a mesh's faces under one material, for each
 * node that shows the mesh** - so a mesh a file places six times is six
 * parts, each with its own place, as glTF's instancing comes in. A part's
 * points are in its own space and `matrix` puts them in the world, which is
 * how a moved or mirrored node keeps its place, turn and size in the
 * Object tab rather than being flattened into the points.
 *
 * **The file's axes and unit are ufbx's to undo.** Maya is Y up in
 * centimetres, 3ds Max Z up in inches, Blender writes Y up with a turn on
 * every object; asked for Y up in metres, ufbx turns the geometry and the
 * transforms into that (`UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY`), so no
 * object inherits a 0.0254 from the root. And it bakes 3ds Max's pivots -
 * FBX's geometric transforms - into the points.
 *
 * **Nothing here trusts the file**, and neither does ufbx, which is fuzzed:
 * a refusal is a sentence, every count is held to what memory allows, and
 * a broken index is clamped by ufbx rather than followed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "k3d.h"
#include "ufbx.h"

#define MAX_POINTS  (1u << 24)
#define MAX_TRIS    (1u << 24)

#ifdef KOSMOS_USER
#include <panic.h>

void k3d_ufbx_failed(const char *what)
{
    char line[160];

    snprintf(line, sizeof(line), "ufbx: assertion failed: %s", what);
    panic(line);
}
#else
void k3d_ufbx_failed(const char *what)
{
    fprintf(stderr, "ufbx: assertion failed: %s\n", what);
    abort();
}
#endif

void k3d_fbx_free(struct k3d_fbx *f)
{
    size_t i;

    for (i = 0; i < f->nparts; i++) k3d_soup_free(&f->parts[i].soup);

    free(f->parts);
    free(f->materials);
    f->parts = NULL;
    f->materials = NULL;
    f->nparts = f->nmaterials = 0;
}

static const char *refuse(struct k3d_fbx *out, const char *why)
{
    k3d_fbx_free(out);
    snprintf(out->why, sizeof(out->why), "%s", why);
    return out->why;
}

/* ufbx's refusal in a sentence of ours, where there is one. */
static const char *refused(struct k3d_fbx *out, const ufbx_error *e)
{
    char line[sizeof(out->why)];

    switch (e->type) {
    case UFBX_ERROR_UNRECOGNIZED_FILE_FORMAT:
    case UFBX_ERROR_FEATURE_DISABLED:
        /* The second is ufbx knowing the bytes for an OBJ or an MTL, which
         * it could read and here is built not to: nothing else it leaves
         * out (`k3d_ufbx.h`) is reached while loading. */
        return refuse(out, "not an FBX file");
    case UFBX_ERROR_TRUNCATED_FILE:
        return refuse(out, "the file ends before its end: it was cut short");
    case UFBX_ERROR_OUT_OF_MEMORY:
    case UFBX_ERROR_MEMORY_LIMIT:
    case UFBX_ERROR_ALLOCATION_LIMIT:
        return refuse(out, "not enough memory to read it");
    case UFBX_ERROR_UNSUPPORTED_VERSION:
        return refuse(out, "an FBX older than any ufbx reads");
    case UFBX_ERROR_NODE_DEPTH_LIMIT:
        return refuse(out, "its objects are nested more deeply than anything drawn by hand");
    default:
        snprintf(line, sizeof(line), "a damaged FBX: %.*s", (int)e->description.length,
                 e->description.data);
        return refuse(out, line);
    }
}

static float value(const ufbx_material_map *m, float otherwise)
{
    return m->has_value ? (float)m->value_real : otherwise;
}

/*
 * A material in the Material tab's terms, linear as the file has it. ufbx
 * has already read every shading model it knows - Lambert and Phong,
 * Blender's, 3ds Max's physical, Arnold's - into one set of PBR maps; what
 * is decided here is only what a map that is not there means.
 */
static void material_of(const ufbx_material *m, struct k3d_fbx_material *out)
{
    const ufbx_material_pbr_maps *p = &m->pbr;
    float base = value(&p->base_factor, 1), glow = value(&p->emission_factor, 1);
    float through = 0;
    int i;

    snprintf(out->name, sizeof(out->name), "%.*s", (int)m->name.length, m->name.data);

    for (i = 0; i < 3; i++) {
        out->base[i] = p->base_color.has_value ? (float)p->base_color.value_vec3.v[i] * base : 0.8f;
        out->emit[i] = p->emission_color.has_value ? (float)p->emission_color.value_vec3.v[i] * glow
                                                   : 0;
        through += p->transmission_color.has_value ? (float)p->transmission_color.value_vec3.v[i] / 3
                                                   : 1.0f / 3;
    }

    out->metallic = value(&p->metalness, 0);
    out->ior = value(&p->specular_ior, 1.5f);

    /* A Lambert has no shine at all; anything else that does not say is
     * halfway. */
    out->rough = value(&p->roughness, m->shader_type == UFBX_SHADER_FBX_LAMBERT ? 1 : 0.5f);

    /*
     * Light passing through. A physical material says so as a weight and a
     * colour; an old Lambert or Phong as a transparent colour and a factor,
     * which ufbx gives as the same two - Maya's default is a black colour
     * at a factor of one, which is opaque, so the two are multiplied. And
     * Blender's says it as an opacity below one.
     */
    out->trans = p->transmission_factor.has_value ? value(&p->transmission_factor, 0) * through
               : p->opacity.has_value ? 1 - value(&p->opacity, 1) : 0;
}

static bool grow(void **items, size_t *cap, size_t want, size_t size)
{
    size_t next = *cap ? *cap * 2 : 16;
    void *bigger;

    if (want <= *cap) return true;
    if (next < want) next = want;

    bigger = realloc(*items, next * size);

    if (bigger == NULL) return false;

    *items = bigger;
    *cap = next;
    return true;
}

/*
 * One part: the faces of `part` in `mesh`, as `node` shows them. `local`
 * and `stamp` are one slot a vertex of the mesh, reused across parts - a
 * vertex's number in this part is `local[v]` when `stamp[v]` is `serial`.
 */
static const char *part_of(const ufbx_node *node, const ufbx_mesh *mesh,
                           const ufbx_mesh_part *part, uint32_t *corners, uint32_t *local,
                           uint32_t *stamp, uint32_t serial, struct k3d_fbx_part *out)
{
    size_t f, cap_pos = 0, cap_tri = 0;
    uint32_t npos = 0, ntri = 0;

    memset(out, 0, sizeof(*out));

    for (f = 0; f < part->face_indices.count; f++) {
        ufbx_face face = mesh->faces.data[part->face_indices.data[f]];
        uint32_t n = ufbx_triangulate_face(corners, mesh->max_face_triangles * 3, mesh, face);
        uint32_t c;

        if (ntri + n > MAX_TRIS) return "a part with more triangles than one mesh may have";

        if (!grow((void **)&out->soup.tri, &cap_tri, (size_t)(ntri + n) * 3, sizeof(uint32_t))) {
            return "not enough memory to read it";
        }

        for (c = 0; c < n * 3; c++) {
            uint32_t v = mesh->vertex_indices.data[corners[c]];

            if (stamp[v] != serial) {
                ufbx_vec3 p = ufbx_transform_position(&node->geometry_to_node,
                                                      mesh->vertices.data[v]);

                if (npos == MAX_POINTS) return "a part with more points than one mesh may have";

                if (!grow((void **)&out->soup.pos, &cap_pos, (size_t)(npos + 1) * 3,
                          sizeof(float))) {
                    return "not enough memory to read it";
                }

                out->soup.pos[npos * 3 + 0] = (float)p.x;
                out->soup.pos[npos * 3 + 1] = (float)p.y;
                out->soup.pos[npos * 3 + 2] = (float)p.z;
                stamp[v] = serial;
                local[v] = npos++;
            }

            out->soup.tri[ntri * 3 + c] = local[v];
        }

        ntri += n;
    }

    out->soup.npos = npos;
    out->soup.ntri = ntri;
    return NULL;
}

const char *k3d_fbx_read(const unsigned char *bytes, size_t len, struct k3d_fbx *out)
{
    ufbx_load_opts opts;
    ufbx_error error;
    ufbx_scene *scene;
    uint32_t *corners = NULL, *local = NULL, *stamp = NULL, serial = 0;
    size_t cap_parts = 0, i, n;
    const char *why = NULL;

    memset(out, 0, sizeof(*out));
    memset(&opts, 0, sizeof(opts));

    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    opts.use_blender_pbr_material = true;
    opts.ignore_animation = true;
    opts.ignore_embedded = true;        /* its pictures, until materials wear them */
    opts.node_depth_limit = 256;

    scene = ufbx_load_memory(bytes, len, &opts, &error);

    if (scene == NULL) return refused(out, &error);

    /* Every material the file has, in its order: a part names one by
     * where it is in this list. */
    out->nmaterials = scene->materials.count;
    out->materials = calloc(out->nmaterials ? out->nmaterials : 1, sizeof(*out->materials));

    if (out->materials == NULL) {
        ufbx_free_scene(scene);
        return refuse(out, "not enough memory to read it");
    }

    for (i = 0; i < scene->materials.count; i++) {
        material_of(scene->materials.data[i], &out->materials[i]);
    }

    out->lamps = (uint32_t)scene->lights.count;
    out->cameras = (uint32_t)scene->cameras.count;

    for (n = 0; n < scene->nodes.count && why == NULL; n++) {
        const ufbx_node *node = scene->nodes.data[n];
        const ufbx_mesh *mesh = node->mesh;
        size_t k;

        if (mesh == NULL || mesh->num_triangles == 0) continue;

        /* Scratch the size of this mesh: a face's corners at most, and a
         * slot for each of its vertices. */
        free(corners);
        free(local);
        free(stamp);
        corners = malloc((mesh->max_face_triangles * 3 + 1) * sizeof(uint32_t));
        local = malloc((mesh->num_vertices + 1) * sizeof(uint32_t));
        stamp = calloc(mesh->num_vertices + 1, sizeof(uint32_t));
        serial = 0;

        if (corners == NULL || local == NULL || stamp == NULL) {
            why = "not enough memory to read it";
            break;
        }

        for (k = 0; k < mesh->material_parts.count && why == NULL; k++) {
            const ufbx_mesh_part *part = &mesh->material_parts.data[k];
            const ufbx_material *mat = NULL;
            struct k3d_fbx_part *p;
            int c;

            if (part->num_triangles == 0) continue;

            if (!grow((void **)&out->parts, &cap_parts, out->nparts + 1, sizeof(*out->parts))) {
                why = "not enough memory to read it";
                break;
            }

            p = &out->parts[out->nparts];
            why = part_of(node, mesh, part, corners, local, stamp, ++serial, p);
            out->nparts++;              /* counted either way, so it is freed */

            if (why) break;

            /* The node's own material for that slot, which is what an
             * instance may change; the mesh's when the node has none. */
            if (part->index < node->materials.count) {
                mat = node->materials.data[part->index];
            } else if (part->index < mesh->materials.count) {
                mat = mesh->materials.data[part->index];
            }

            p->material = mat ? mat->typed_id : K3D_FBX_NO_MATERIAL;
            p->hidden = !node->visible;

            /* The object's name, and the material's after it when the mesh
             * is in several. A node ufbx made to carry a pivot of 3ds Max's
             * has none, and is its parent's. */
            {
                const ufbx_node *named = node;
                ufbx_string name;

                if (named->is_geometry_transform_helper && named->parent) named = named->parent;

                name = named->name.length ? named->name : mesh->name;

                if (mesh->material_parts.count > 1 && mat) {
                    snprintf(p->name, sizeof(p->name), "%.*s %.*s", (int)name.length, name.data,
                             (int)mat->name.length, mat->name.data);
                } else {
                    snprintf(p->name, sizeof(p->name), "%.*s", (int)name.length, name.data);
                }
            }

            /* ufbx's 4x3, as glTF's column-major 4x4. */
            for (c = 0; c < 4; c++) {
                p->matrix[c * 4 + 0] = node->node_to_world.cols[c].x;
                p->matrix[c * 4 + 1] = node->node_to_world.cols[c].y;
                p->matrix[c * 4 + 2] = node->node_to_world.cols[c].z;
                p->matrix[c * 4 + 3] = c == 3 ? 1 : 0;
            }
        }
    }

    free(corners);
    free(local);
    free(stamp);
    ufbx_free_scene(scene);

    if (why) return refuse(out, why);

    if (out->nparts == 0) return refuse(out, "an FBX with no meshes in it");

    return NULL;
}
