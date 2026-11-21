/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The 3D Kit on Vulkan (`k3d_vk.h`): a sky, then grounds, meshes and
 * models lit by the sun and hazed by distance, into a picture with a depth
 * buffer.
 */

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "k3d_vk.h"

#include "k3d_mesh_vert.h"
#include "k3d_mesh_frag.h"
#include "k3d_sky_vert.h"
#include "k3d_sky_frag.h"
#include "k3d_ground_vert.h"
#include "k3d_ground_frag.h"
#include "k3d_model_vert.h"
#include "k3d_model_frag.h"
#include "k3d_overlay_vert.h"
#include "k3d_overlay_frag.h"
#include "k3d_shadow_vert.h"
#include "k3d_shadow_frag.h"

/* The shaders' push constants, as `k3d_view.glsl` declares them (std430:
 * vec4s), 192 bytes. */
struct mesh_push {
    float clip[16];
    float eye[4];       /* w: a shadow-map texel in uv; 0, no shadows */
    float sun[4];       /* w: ambient */
    float haze[4];
    float range[4];     /* the haze's start and end; the shadow's depth
                         * bias, and its offset along the normal */
    float light[4];     /* rgb: the sun's colour */
    float shadow[12];   /* the world to the sun's map: three affine rows */
};

/* A model's: the view's, then its own matrix as three affine rows and its
 * tint - 256 bytes, the most lavapipe and anv give and what Vulkan 1.4
 * promises (1.3 promises 128). The view is pushed once and only the last
 * 64 bytes for each model. Its matrix went as a whole mat4 until the
 * shadows' rows made the view 192 (`docs/flight.md`, *Shadows*). */
struct model_push {
    struct mesh_push view;
    float model[12];
    float tint[4];
};

/* A caster's, into the sun's map: the map's rows and its own, 96 bytes. */
struct shadow_push {
    float sun[12];
    float model[12];
};

/* Column-major and affine, as a model's draw gives it, into three rows. */
static void affine_rows(const float m[16], float rows[12])
{
    for (int i = 0; i < 3; i++) {
        rows[4 * i + 0] = m[i];
        rows[4 * i + 1] = m[4 + i];
        rows[4 * i + 2] = m[8 + i];
        rows[4 * i + 3] = m[12 + i];
    }
}

static const float identity_rows[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };

struct sky_push {
    float unclip[16];
    float eye[4];
    float zenith[4];
    float horizon[4];
    float sun[4];       /* xyz: towards the sun; w: the glow */
    float light[4];     /* rgb: the sun's colour */
};

static void refuse(struct kvk *v, const char *why)
{
    strncpy(v->why, why, sizeof(v->why) - 1);
    v->why[sizeof(v->why) - 1] = '\0';
}

/* A pipeline's layout: its push constants, and up to two sets of `set`'s
 * layout - a lit surface's own picture, then the sun's map. */
static int pipeline_layout(struct kvk *v, VkPipelineLayout *out, uint32_t bytes,
                           VkShaderStageFlags stages, VkDescriptorSetLayout set,
                           uint32_t sets)
{
    VkPushConstantRange range = { stages, 0, bytes };
    VkDescriptorSetLayout layouts[2] = { set, set };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = set != VK_NULL_HANDLE ? sets : 0,
        .pSetLayouts = layouts,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    };

    KVK_STEP(v, v->vkCreatePipelineLayout(v->device, &plci, NULL, out), "pipeline-layout");
    return 1;

done:
    return 0;
}

int k3d_vk_open(struct k3d_vk *r, struct kvk *v, VkFormat format,
                uint32_t width, uint32_t height)
{
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    memset(r, 0, sizeof(*r));
    r->v = v;
    r->width = width;
    r->height = height;
    r->format = format;

    /* Every lit surface's layout has two sets: its own picture - white
     * for a mesh, which has none - and the sun's map. */
    if (!kvk_image_make(v, &r->depth, VK_FORMAT_D32_SFLOAT, width, height,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                        "depth") ||
        !kvk_image_make(v, &r->sunmap, VK_FORMAT_D32_SFLOAT, K3D_VK_SHADOW, K3D_VK_SHADOW,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_DEPTH_BIT, "sun-map") ||
        !kvk_sampled_layout(v, &r->imagery_layout) ||
        !pipeline_layout(v, &r->mesh_layout, sizeof(struct mesh_push), both,
                         r->imagery_layout, 2) ||
        !pipeline_layout(v, &r->sky_layout, sizeof(struct sky_push),
                         VK_SHADER_STAGE_FRAGMENT_BIT, VK_NULL_HANDLE, 0) ||
        !pipeline_layout(v, &r->ground_layout, sizeof(struct mesh_push), both,
                         r->imagery_layout, 2) ||
        !pipeline_layout(v, &r->model_layout, sizeof(struct model_push), both,
                         r->imagery_layout, 2) ||
        !pipeline_layout(v, &r->overlay_layout, 4 * sizeof(float), VK_SHADER_STAGE_VERTEX_BIT,
                         r->imagery_layout, 1) ||
        !pipeline_layout(v, &r->shadow_layout, sizeof(struct shadow_push),
                         VK_SHADER_STAGE_VERTEX_BIT, VK_NULL_HANDLE, 0)) {
        return 0;
    }

    /* Imagery between its texels, and its edge beyond them: a chunk's
     * outermost half-texel is its own edge's colour, not its neighbour's. */
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    KVK_STEP(v, v->vkCreateSampler(v->device, &sci, NULL, &r->imagery), "sampler");
    /* A model's skin repeats: glTF's default wrap, and what a texture
     * tiled across a fuselage counts on. */
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    KVK_STEP(v, v->vkCreateSampler(v->device, &sci, NULL, &r->skin), "sampler");
    /* The sun's map, compared as it is read: a tap is 1 where the surface
     * is no further from the sun than what the map holds. Linear where the
     * device filters a depth format, so each tap blends the four texels
     * around it and a texel's step is not seen - on the M700 a texel near
     * the ground is 0.4 m, a dozen pixels of an aircraft's shadow; nearest
     * where it does not, which a depth format need not. The shader's nine
     * taps soften the edge either way. Past the map's edge, depth 1 - the
     * sun. */
    VkFilter sunmap_filter = v->depth_filters ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    VkSamplerCreateInfo cci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = sunmap_filter,
        .minFilter = sunmap_filter,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .compareEnable = VK_TRUE,
        .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
    };
    KVK_STEP(v, v->vkCreateSampler(v->device, &cci, NULL, &r->compare), "sampler");
    if (!kvk_sampled_set(v, r->imagery_layout, r->compare, r->sunmap.view, &r->sunmap_pool,
                         &r->sunmap_set)) {
        return 0;
    }

    VkVertexInputBindingDescription stream = { 0, sizeof(struct k3d_vk_vertex),
                                               VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attributes[3] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(struct k3d_vk_vertex, position) },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(struct k3d_vk_vertex, normal) },
        { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(struct k3d_vk_vertex, colour) },
    };
    VkPipelineVertexInputStateCreateInfo input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &stream,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = attributes,
    };
    VkPipelineVertexInputStateCreateInfo none = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    /* The terrain is seen from both sides as a camera flies under a
     * ridge's edge, so nothing is culled; a closed mesh's back faces are
     * behind its front ones in the depth buffer anyway. */
    struct kvk_pipeline_shape mesh = {
        k3d_mesh_vert, sizeof(k3d_mesh_vert), k3d_mesh_frag, sizeof(k3d_mesh_frag), &input,
        width, height, format, VK_FORMAT_D32_SFLOAT, VK_CULL_MODE_NONE, 0, 0, 0, 0,
    };
    /* The sky: no depth test, drawn first, under everything. */
    struct kvk_pipeline_shape sky = {
        k3d_sky_vert, sizeof(k3d_sky_vert), k3d_sky_frag, sizeof(k3d_sky_frag), &none,
        width, height, format, VK_FORMAT_D32_SFLOAT, VK_CULL_MODE_NONE, 1, 0, 0, 0,
    };

    VkVertexInputBindingDescription ground_stream = { 0, sizeof(struct k3d_terrain_vertex),
                                                      VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription ground_attributes[3] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(struct k3d_terrain_vertex, position) },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(struct k3d_terrain_vertex, normal) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(struct k3d_terrain_vertex, uv) },
    };
    VkPipelineVertexInputStateCreateInfo ground_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &ground_stream,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = ground_attributes,
    };
    struct kvk_pipeline_shape ground = {
        k3d_ground_vert, sizeof(k3d_ground_vert), k3d_ground_frag, sizeof(k3d_ground_frag),
        &ground_input, width, height, format, VK_FORMAT_D32_SFLOAT, VK_CULL_MODE_NONE, 0, 0, 0, 0,
    };

    VkVertexInputBindingDescription model_stream = { 0, sizeof(struct k3d_vk_model_vertex),
                                                     VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription model_attributes[3] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(struct k3d_vk_model_vertex, position) },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(struct k3d_vk_model_vertex, normal) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(struct k3d_vk_model_vertex, uv) },
    };
    VkPipelineVertexInputStateCreateInfo model_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &model_stream,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = model_attributes,
    };
    /* Not culled either: a glTF model's thin parts are often one sheet
     * meant to be seen from both sides, which the shader lights as the
     * side the eye is on. */
    struct kvk_pipeline_shape model = {
        k3d_model_vert, sizeof(k3d_model_vert), k3d_model_frag, sizeof(k3d_model_frag),
        &model_input, width, height, format, VK_FORMAT_D32_SFLOAT, VK_CULL_MODE_NONE, 0, 0, 0, 0,
    };

    /* An overlay: over everything, untested, blended by its alpha. */
    struct kvk_pipeline_shape overlay = {
        k3d_overlay_vert, sizeof(k3d_overlay_vert), k3d_overlay_frag, sizeof(k3d_overlay_frag),
        &none, width, height, format, VK_FORMAT_D32_SFLOAT, VK_CULL_MODE_NONE, 1, 1, 0, 0,
    };

    /* The casters into the sun's map: their corners alone, from either
     * layout, depth alone and pushed back as it is drawn, so a lit face
     * does not find itself in its own shadow. */
    VkVertexInputAttributeDescription where = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
    VkVertexInputBindingDescription mesh_corners = { 0, sizeof(struct k3d_vk_vertex),
                                                     VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputBindingDescription model_corners = { 0, sizeof(struct k3d_vk_model_vertex),
                                                      VK_VERTEX_INPUT_RATE_VERTEX };
    VkPipelineVertexInputStateCreateInfo mesh_casts = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &mesh_corners,
        .vertexAttributeDescriptionCount = 1,
        .pVertexAttributeDescriptions = &where,
    };
    VkPipelineVertexInputStateCreateInfo model_casts = mesh_casts;
    model_casts.pVertexBindingDescriptions = &model_corners;
    struct kvk_pipeline_shape caster = {
        k3d_shadow_vert, sizeof(k3d_shadow_vert), k3d_shadow_frag, sizeof(k3d_shadow_frag),
        &mesh_casts, K3D_VK_SHADOW, K3D_VK_SHADOW, VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT,
        VK_CULL_MODE_NONE, 0, 0, 1.25f, 1.75f,
    };
    struct kvk_pipeline_shape model_caster = caster;
    model_caster.input = &model_casts;

    if (!kvk_pipeline(v, &r->mesh, r->mesh_layout, &mesh) ||
        !kvk_pipeline(v, &r->sky, r->sky_layout, &sky) ||
        !kvk_pipeline(v, &r->ground, r->ground_layout, &ground) ||
        !kvk_pipeline(v, &r->model, r->model_layout, &model) ||
        !kvk_pipeline(v, &r->overlay, r->overlay_layout, &overlay) ||
        !kvk_pipeline(v, &r->shadow_mesh, r->shadow_layout, &caster) ||
        !kvk_pipeline(v, &r->shadow_model, r->shadow_layout, &model_caster)) {
        return 0;
    }

    /* The sun's map starts as a picture to read, as every frame without
     * shadows leaves it: in the sun everywhere until it is drawn. */
    if (!kvk_begin(v)) {
        return 0;
    }
    kvk_barrier(v, r->sunmap.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    if (!kvk_submit(v)) {
        return 0;
    }

    /* What an untextured model wears: white, so its tint is its colour and
     * one pipeline draws both kinds. */
    static const uint8_t white[4] = { 255, 255, 255, 255 };
    return k3d_vk_texture(r, &r->white, white, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);

done:
    return 0;
}

void k3d_vk_close(struct k3d_vk *r)
{
    struct kvk *v = r->v;

    if (v == NULL || v->device == VK_NULL_HANDLE) {
        return;
    }
    k3d_vk_texture_free(r, &r->white);
    if (r->shadow_mesh != VK_NULL_HANDLE) {
        v->vkDestroyPipeline(v->device, r->shadow_mesh, NULL);
    }
    if (r->shadow_model != VK_NULL_HANDLE) {
        v->vkDestroyPipeline(v->device, r->shadow_model, NULL);
    }
    if (r->shadow_layout != VK_NULL_HANDLE) {
        v->vkDestroyPipelineLayout(v->device, r->shadow_layout, NULL);
    }
    if (r->sunmap_pool != VK_NULL_HANDLE) {
        v->vkDestroyDescriptorPool(v->device, r->sunmap_pool, NULL);
    }
    if (r->compare != VK_NULL_HANDLE) {
        v->vkDestroySampler(v->device, r->compare, NULL);
    }
    kvk_image_free(v, &r->sunmap);
    if (r->overlay != VK_NULL_HANDLE) {
        v->vkDestroyPipeline(v->device, r->overlay, NULL);
    }
    if (r->overlay_layout != VK_NULL_HANDLE) {
        v->vkDestroyPipelineLayout(v->device, r->overlay_layout, NULL);
    }
    if (r->model != VK_NULL_HANDLE) {
        v->vkDestroyPipeline(v->device, r->model, NULL);
    }
    if (r->model_layout != VK_NULL_HANDLE) {
        v->vkDestroyPipelineLayout(v->device, r->model_layout, NULL);
    }
    if (r->skin != VK_NULL_HANDLE) {
        v->vkDestroySampler(v->device, r->skin, NULL);
    }
    if (r->ground != VK_NULL_HANDLE) {
        v->vkDestroyPipeline(v->device, r->ground, NULL);
    }
    if (r->ground_layout != VK_NULL_HANDLE) {
        v->vkDestroyPipelineLayout(v->device, r->ground_layout, NULL);
    }
    if (r->imagery_layout != VK_NULL_HANDLE) {
        v->vkDestroyDescriptorSetLayout(v->device, r->imagery_layout, NULL);
    }
    if (r->imagery != VK_NULL_HANDLE) {
        v->vkDestroySampler(v->device, r->imagery, NULL);
    }
    if (r->sky != VK_NULL_HANDLE) {
        v->vkDestroyPipeline(v->device, r->sky, NULL);
    }
    if (r->mesh != VK_NULL_HANDLE) {
        v->vkDestroyPipeline(v->device, r->mesh, NULL);
    }
    if (r->sky_layout != VK_NULL_HANDLE) {
        v->vkDestroyPipelineLayout(v->device, r->sky_layout, NULL);
    }
    if (r->mesh_layout != VK_NULL_HANDLE) {
        v->vkDestroyPipelineLayout(v->device, r->mesh_layout, NULL);
    }
    kvk_image_free(v, &r->depth);
    memset(r, 0, sizeof(*r));
}

int k3d_vk_mesh(struct k3d_vk *r, struct k3d_vk_mesh *m,
                const struct k3d_vk_vertex *vertices, uint32_t nv,
                const uint32_t *indices, uint32_t ni)
{
    VkDeviceSize vbytes = (VkDeviceSize)nv * sizeof(*vertices);
    VkDeviceSize ibytes = (VkDeviceSize)ni * sizeof(*indices);

    memset(m, 0, sizeof(*m));
    if (nv == 0 || ni == 0 || ni % 3 != 0) {
        refuse(r->v, "mesh-not-triangles");
        return 0;
    }
    for (uint32_t i = 0; i < ni; i++) {
        if (indices[i] >= nv) {
            refuse(r->v, "mesh-index-outside");
            return 0;
        }
    }
    /* Indices start on a 4-byte boundary, which a vertex of 28 bytes
     * already gives; kept explicit for when the vertex changes. */
    m->indices_at = (vbytes + 3) & ~(VkDeviceSize)3;
    if (!kvk_buffer_make(r->v, &m->buffer, m->indices_at + ibytes,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                         "mesh")) {
        return 0;
    }
    memcpy(m->buffer.bytes, vertices, (size_t)vbytes);
    memcpy((uint8_t *)m->buffer.bytes + m->indices_at, indices, (size_t)ibytes);
    m->count = ni;
    return 1;
}

void k3d_vk_mesh_free(struct k3d_vk *r, struct k3d_vk_mesh *m)
{
    kvk_buffer_free(r->v, &m->buffer);
    memset(m, 0, sizeof(*m));
}

/* A picture uploaded and its set, sampled by `sampler`: the one way a
 * texture is made here, for a ground's imagery and a model's skin alike. */
static int texture_make(struct k3d_vk *r, struct k3d_vk_texture *t, VkSampler sampler,
                        const void *texels, uint32_t width, uint32_t height, VkFormat format)
{
    struct kvk *v = r->v;
    struct kvk_buffer upload = { 0 };
    int made = 0;

    memset(t, 0, sizeof(*t));
    if (texels == NULL || width == 0 || height == 0 ||
        (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_B8G8R8A8_UNORM)) {
        refuse(v, "texture-not-a-picture");
        return 0;
    }
    if (!kvk_image_make(v, &t->image, format, width, height,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, "texture") ||
        !kvk_buffer_make(v, &upload, (VkDeviceSize)width * height * 4,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "texture-upload") ||
        !kvk_sampled_set(v, r->imagery_layout, sampler, t->image.view, &t->pool, &t->set)) {
        goto out;
    }
    memcpy(upload.bytes, texels, (size_t)width * height * 4);
    if (!kvk_begin(v)) {
        goto out;
    }
    kvk_image_upload(v, t->image.image, width, height, upload.buffer);
    if (!kvk_submit(v)) {
        goto out;
    }
    t->width = width;
    t->height = height;
    made = 1;

out:
    kvk_buffer_free(v, &upload);
    if (!made) {
        k3d_vk_texture_free(r, t);
    }
    return made;
}

int k3d_vk_texture(struct k3d_vk *r, struct k3d_vk_texture *t, const void *texels,
                   uint32_t width, uint32_t height, VkFormat format)
{
    return texture_make(r, t, r->skin, texels, width, height, format);
}

/*
 * New texels for a texture that exists: the image's contents replaced as
 * it was first filled, through a buffer for the copy. Outside a frame -
 * `kvk_submit` waits for the queue, so nothing still reads the old ones.
 */
int k3d_vk_texture_refresh(struct k3d_vk *r, struct k3d_vk_texture *t, const void *texels)
{
    struct kvk *v = r->v;
    struct kvk_buffer upload = { 0 };
    int done = 0;

    if (texels == NULL || t->image.image == VK_NULL_HANDLE || t->width == 0) {
        refuse(v, "texture-not-made");
        return 0;
    }
    if (!kvk_buffer_make(v, &upload, (VkDeviceSize)t->width * t->height * 4,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "texture-upload")) {
        return 0;
    }
    memcpy(upload.bytes, texels, (size_t)t->width * t->height * 4);
    if (kvk_begin(v)) {
        kvk_image_upload(v, t->image.image, t->width, t->height, upload.buffer);
        done = kvk_submit(v);
    }
    kvk_buffer_free(v, &upload);
    return done;
}

void k3d_vk_texture_free(struct k3d_vk *r, struct k3d_vk_texture *t)
{
    struct kvk *v = r->v;

    if (v != NULL && v->device != VK_NULL_HANDLE && t->pool != VK_NULL_HANDLE) {
        v->vkDestroyDescriptorPool(v->device, t->pool, NULL);
    }
    kvk_image_free(v, &t->image);
    memset(t, 0, sizeof(*t));
}

int k3d_vk_ground(struct k3d_vk *r, struct k3d_vk_ground *g,
                  const struct k3d_terrain_vertex *vertices, uint32_t nv,
                  const uint32_t *indices, uint32_t ni, const void *texels,
                  uint32_t width, uint32_t height, VkFormat format)
{
    struct kvk *v = r->v;
    VkDeviceSize vbytes = (VkDeviceSize)nv * sizeof(*vertices);
    VkDeviceSize ibytes = (VkDeviceSize)ni * sizeof(*indices);

    memset(g, 0, sizeof(*g));
    if (nv == 0 || ni == 0 || ni % 3 != 0 || width == 0 || height == 0 ||
        (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_B8G8R8A8_UNORM)) {
        refuse(v, "ground-not-a-ground");
        return 0;
    }
    for (uint32_t i = 0; i < ni; i++) {
        if (indices[i] >= nv) {
            refuse(v, "ground-index-outside");
            return 0;
        }
    }
    g->indices_at = (vbytes + 3) & ~(VkDeviceSize)3;
    if (!kvk_buffer_make(v, &g->buffer, g->indices_at + ibytes,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                         "ground")) {
        return 0;
    }
    memcpy(g->buffer.bytes, vertices, (size_t)vbytes);
    memcpy((uint8_t *)g->buffer.bytes + g->indices_at, indices, (size_t)ibytes);
    if (!texture_make(r, &g->imagery, r->imagery, texels, width, height, format)) {
        k3d_vk_ground_free(r, g);
        return 0;
    }
    g->count = ni;
    return 1;
}

void k3d_vk_ground_free(struct k3d_vk *r, struct k3d_vk_ground *g)
{
    k3d_vk_texture_free(r, &g->imagery);
    kvk_buffer_free(r->v, &g->buffer);
    memset(g, 0, sizeof(*g));
}

int k3d_vk_model(struct k3d_vk *r, struct k3d_vk_model *m,
                 const struct k3d_vk_model_vertex *vertices, uint32_t nv,
                 const uint32_t *indices, uint32_t ni)
{
    VkDeviceSize vbytes = (VkDeviceSize)nv * sizeof(*vertices);
    VkDeviceSize ibytes = (VkDeviceSize)ni * sizeof(*indices);

    memset(m, 0, sizeof(*m));
    if (nv == 0 || ni == 0 || ni % 3 != 0) {
        refuse(r->v, "model-not-triangles");
        return 0;
    }
    for (uint32_t i = 0; i < ni; i++) {
        if (indices[i] >= nv) {
            refuse(r->v, "model-index-outside");
            return 0;
        }
    }
    m->indices_at = (vbytes + 3) & ~(VkDeviceSize)3;
    if (!kvk_buffer_make(r->v, &m->buffer, m->indices_at + ibytes,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                         "model")) {
        return 0;
    }
    memcpy(m->buffer.bytes, vertices, (size_t)vbytes);
    memcpy((uint8_t *)m->buffer.bytes + m->indices_at, indices, (size_t)ibytes);
    m->count = ni;
    return 1;
}

void k3d_vk_model_free(struct k3d_vk *r, struct k3d_vk_model *m)
{
    kvk_buffer_free(r->v, &m->buffer);
    memset(m, 0, sizeof(*m));
}

/* ---- the view's arithmetic, in double, kept to float at the end ---- */

static void cross(const double a[3], const double b[3], double out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static int unit(double a[3])
{
    double n = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);

    if (n < 1e-12) {
        return 0;
    }
    a[0] /= n, a[1] /= n, a[2] /= n;
    return 1;
}

/* The eye's frame: forward, right, up; 0 when there is none. Looking
 * along the up the view gives - straight up or down, for the world's - has
 * no right, and is refused rather than guessed. The view's up need not be
 * square to forward: only the plane the two make is taken from it, so a
 * cockpit hands the aircraft's up as it is. */
static int frame(const struct k3d_vk_view *view, double f[3], double rt[3], double u[3])
{
    double above[3] = { view->up[0], view->up[1], view->up[2] };

    if (above[0] == 0 && above[1] == 0 && above[2] == 0) {
        above[1] = 1;               /* the world's, as before there was a choice */
    }
    for (int i = 0; i < 3; i++) {
        f[i] = (double)view->target[i] - view->eye[i];
    }
    if (!unit(f)) {
        return 0;
    }
    cross(f, above, rt);
    if (!unit(rt)) {
        return 0;
    }
    cross(rt, f, u);
    return 1;
}

/* The 4 by 4 matrices here are row-major while built, m[row][col]. */
static void eye_to_clip(const struct k3d_vk_view *view, uint32_t width, uint32_t height,
                        double m[4][4])
{
    double sx = view->focal, sy = view->focal * (double)width / (double)height;
    double n = view->near, fa = view->far;

    memset(m, 0, sizeof(double) * 16);
    /* x right, y down in Vulkan's clip space, z from near (0) to far (1),
     * w the distance along forward. */
    m[0][0] = sx;
    m[1][1] = -sy;
    m[2][2] = fa / (fa - n);
    m[2][3] = -fa * n / (fa - n);
    m[3][2] = 1.0;
}

static int world_to_clip(const struct k3d_vk_view *view, uint32_t width, uint32_t height,
                         double out[4][4])
{
    double f[3], rt[3], u[3], e[4][4], p[4][4];

    if (!(view->focal > 0) || !(view->near > 0) || !(view->far > view->near) ||
        width == 0 || height == 0 || !frame(view, f, rt, u)) {
        return 0;
    }

    /* The world to the eye: rows right, up, forward, each less the eye. */
    memset(e, 0, sizeof(e));
    for (int i = 0; i < 3; i++) {
        e[0][i] = rt[i];
        e[1][i] = u[i];
        e[2][i] = f[i];
    }
    for (int r = 0; r < 3; r++) {
        e[r][3] = -(e[r][0] * view->eye[0] + e[r][1] * view->eye[1] + e[r][2] * view->eye[2]);
    }
    e[3][3] = 1.0;

    eye_to_clip(view, width, height, p);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            double s = 0;
            for (int k = 0; k < 4; k++) {
                s += p[r][k] * e[k][c];
            }
            out[r][c] = s;
        }
    }
    return 1;
}

/* A 4 by 4's inverse by cofactors: 0 when it has none. */
static int invert(const double m[4][4], double out[4][4])
{
    double a[16], inv[16], det;

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            a[r * 4 + c] = m[r][c];
        }
    }
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] +
             a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] -
             a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] +
             a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] -
              a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] -
             a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] +
             a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] -
             a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] +
              a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] +
             a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] -
             a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] +
              a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] -
              a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] -
             a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] +
             a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] -
              a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] +
              a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];

    det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (fabs(det) < 1e-300) {
        return 0;
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out[r][c] = inv[r * 4 + c] / det;
        }
    }
    return 1;
}

/* Row-major double to the column-major float a shader's mat4 is. */
static void column_major(const double m[4][4], float out[16])
{
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            out[c * 4 + r] = (float)m[r][c];
        }
    }
}

int k3d_vk_clip(const struct k3d_vk_view *view, uint32_t width, uint32_t height,
                float clip[16])
{
    double m[4][4];

    if (!world_to_clip(view, width, height, m)) {
        return 0;
    }
    column_major(m, clip);
    return 1;
}

/*
 * **The sun's box** (`docs/flight.md`, *Shadows*): the world into the
 * sun's map, as three affine rows - x and y across the sun's light, from
 * -1 to 1 over `radius` metres either side of `centre`, and depth along it
 * from 0 nearest the sun to 1, over `radius` and a kilometre more either
 * side, so the tallest tower and the aircraft above the ground both fit.
 * The centre is held to whole texels across the light, so the map's texels
 * stay where they are on the ground as the centre moves and an edge does
 * not crawl. Out, the size of a texel in metres.
 */
static double sun_box(const double sun[3], const float centre[3], double radius,
                      float rows[12])
{
    double up[3] = { 0, 1, 0 }, across[3], above[3], c[3];
    double texel = 2 * radius / K3D_VK_SHADOW, deep = radius + 1000.0;

    if (fabs(sun[1]) > 0.99) {
        up[0] = 1, up[1] = 0;
    }
    cross(up, sun, across);
    unit(across);
    cross(sun, across, above);
    for (int i = 0; i < 3; i++) {
        c[i] = centre[i];
    }
    double ac = floor((across[0] * c[0] + across[1] * c[1] + across[2] * c[2]) / texel) * texel;
    double uc = floor((above[0] * c[0] + above[1] * c[1] + above[2] * c[2]) / texel) * texel;
    double sc = sun[0] * c[0] + sun[1] * c[1] + sun[2] * c[2];

    for (int i = 0; i < 3; i++) {
        rows[i] = (float)(across[i] / radius);
        rows[4 + i] = (float)(above[i] / radius);
        rows[8 + i] = (float)(-sun[i] / (2 * deep));
    }
    rows[3] = (float)(-ac / radius);
    rows[7] = (float)(-uc / radius);
    rows[11] = (float)((deep + sc) / (2 * deep));
    return texel;
}

/* The casters drawn into the sun's map, from `rows`: every mesh, and every
 * model by its own matrix. */
static void draw_sunmap(struct k3d_vk *r, const float rows[12], const struct k3d_vk_mesh *meshes,
                        uint32_t count, const struct k3d_vk_model_draw *models,
                        uint32_t models_count)
{
    struct kvk *v = r->v;
    struct shadow_push sp;

    kvk_barrier(v, r->sunmap.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo depth = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = r->sunmap.view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .depthStencil = { 1.0f, 0 } },
    };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { K3D_VK_SHADOW, K3D_VK_SHADOW } },
        .layerCount = 1,
        .pDepthAttachment = &depth,
    };
    v->vkCmdBeginRendering(v->cmd, &ri);
    memcpy(sp.sun, rows, sizeof(sp.sun));
    memcpy(sp.model, identity_rows, sizeof(sp.model));
    if (count > 0) {
        v->vkCmdBindPipeline(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->shadow_mesh);
        v->vkCmdPushConstants(v->cmd, r->shadow_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                              sizeof(sp), &sp);
        for (uint32_t i = 0; i < count; i++) {
            VkDeviceSize at = 0;

            v->vkCmdBindVertexBuffers(v->cmd, 0, 1, &meshes[i].buffer.buffer, &at);
            v->vkCmdBindIndexBuffer(v->cmd, meshes[i].buffer.buffer, meshes[i].indices_at,
                                    VK_INDEX_TYPE_UINT32);
            v->vkCmdDrawIndexed(v->cmd, meshes[i].count, 1, 0, 0, 0);
            r->draws++;
            r->shadow_triangles += meshes[i].count / 3;
        }
    }
    if (models_count > 0) {
        v->vkCmdBindPipeline(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->shadow_model);
        v->vkCmdPushConstants(v->cmd, r->shadow_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                              sizeof(sp.sun), sp.sun);
        for (uint32_t i = 0; i < models_count; i++) {
            const struct k3d_vk_model_draw *d = &models[i];
            float own[12];
            VkDeviceSize at = 0;

            if (d->model == NULL || d->model->count == 0) {
                continue;
            }
            affine_rows(d->matrix, own);
            v->vkCmdPushConstants(v->cmd, r->shadow_layout, VK_SHADER_STAGE_VERTEX_BIT,
                                  offsetof(struct shadow_push, model), sizeof(own), own);
            v->vkCmdBindVertexBuffers(v->cmd, 0, 1, &d->model->buffer.buffer, &at);
            v->vkCmdBindIndexBuffer(v->cmd, d->model->buffer.buffer, d->model->indices_at,
                                    VK_INDEX_TYPE_UINT32);
            v->vkCmdDrawIndexed(v->cmd, d->model->count, 1, 0, 0, 0);
            r->draws++;
            r->shadow_triangles += d->model->count / 3;
        }
    }
    v->vkCmdEndRendering(v->cmd);
    kvk_barrier(v, r->sunmap.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

int k3d_vk_draw(struct k3d_vk *r, VkImage target, VkImageView view_of,
                const struct k3d_vk_view *view, const struct k3d_vk_mesh *meshes,
                uint32_t count, const struct k3d_vk_ground *grounds, uint32_t grounds_count)
{
    return k3d_vk_draw_models(r, target, view_of, view, meshes, count, grounds, grounds_count,
                              NULL, 0);
}

int k3d_vk_draw_models(struct k3d_vk *r, VkImage target, VkImageView view_of,
                       const struct k3d_vk_view *view, const struct k3d_vk_mesh *meshes,
                       uint32_t count, const struct k3d_vk_ground *grounds,
                       uint32_t grounds_count, const struct k3d_vk_model_draw *models,
                       uint32_t models_count)
{
    return k3d_vk_draw_scene(r, target, view_of, view, meshes, count, grounds, grounds_count,
                             models, models_count, NULL, 0);
}

int k3d_vk_draw_scene(struct k3d_vk *r, VkImage target, VkImageView view_of,
                      const struct k3d_vk_view *view, const struct k3d_vk_mesh *meshes,
                      uint32_t count, const struct k3d_vk_ground *grounds,
                      uint32_t grounds_count, const struct k3d_vk_model_draw *models,
                      uint32_t models_count, const struct k3d_vk_overlay *overlays,
                      uint32_t overlays_count)
{
    struct kvk *v = r->v;
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    struct mesh_push mp;
    struct sky_push sp;
    double m[4][4], inv[4][4], sun[3] = { view->sun[0], view->sun[1], view->sun[2] };

    if (!world_to_clip(view, r->width, r->height, m) || !invert(m, inv)) {
        refuse(v, "view-not-a-view");
        return 0;
    }
    if (!unit(sun)) {
        refuse(v, "sun-has-no-direction");
        return 0;
    }
    if (!(view->haze_end > view->haze_start)) {
        refuse(v, "haze-ends-before-it-starts");
        return 0;
    }

    memset(&mp, 0, sizeof(mp));
    memset(&sp, 0, sizeof(sp));
    column_major(m, mp.clip);
    column_major(inv, sp.unclip);
    for (int i = 0; i < 3; i++) {
        mp.eye[i] = sp.eye[i] = view->eye[i];
        mp.sun[i] = (float)sun[i];
        mp.haze[i] = view->haze[i];
        sp.zenith[i] = view->zenith[i];
        sp.horizon[i] = view->horizon[i];
        sp.sun[i] = (float)sun[i];
    }
    /* The sun's colour: white when the view names none. */
    {
        int named = view->light[0] != 0.0f || view->light[1] != 0.0f || view->light[2] != 0.0f;

        for (int i = 0; i < 3; i++) {
            mp.light[i] = sp.light[i] = named ? view->light[i] : 1.0f;
        }
    }
    sp.sun[3] = view->glow;
    mp.sun[3] = view->ambient;
    mp.range[0] = view->haze_start;
    mp.range[1] = view->haze_end;

    /* The sun's map first, when the view asks for shadows, and what the
     * surfaces need to read it: its rows, a texel's size in its own uv,
     * and how far a surface is moved off itself before it looks - a texel
     * along its normal, and two texels' depth towards the sun. */
    r->draws = 0;
    r->triangles = 0;
    r->shadow_triangles = 0;
    /* A sun below the horizon lights nothing from above, so casts nothing. */
    if (view->shadow_radius > 0 && sun[1] > 0.0) {
        double texel = sun_box(sun, view->shadow_centre, view->shadow_radius, mp.shadow);
        double deep = view->shadow_radius + 1000.0;

        mp.eye[3] = 1.0f / K3D_VK_SHADOW;
        mp.range[2] = (float)(2 * texel / (2 * deep));
        mp.range[3] = (float)texel;
        draw_sunmap(r, mp.shadow, meshes, count, models, models_count);
    }

    kvk_barrier(v, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    kvk_barrier(v, r->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    /* No clear of the colour: the sky covers every pixel. */
    VkRenderingAttachmentInfo colour = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = view_of,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingAttachmentInfo depth = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = r->depth.view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue = { .depthStencil = { 1.0f, 0 } },
    };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { r->width, r->height } },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colour,
        .pDepthAttachment = &depth,
    };
    v->vkCmdBeginRendering(v->cmd, &ri);

    v->vkCmdBindPipeline(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->sky);
    v->vkCmdPushConstants(v->cmd, r->sky_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(sp),
                          &sp);
    v->vkCmdDraw(v->cmd, 3, 1, 0, 0);
    r->draws++;
    r->triangles++;

    if (grounds_count > 0) {
        v->vkCmdBindPipeline(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->ground);
        v->vkCmdPushConstants(v->cmd, r->ground_layout,
                              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                              sizeof(mp), &mp);
        v->vkCmdBindDescriptorSets(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->ground_layout,
                                   1, 1, &r->sunmap_set, 0, NULL);
        for (uint32_t i = 0; i < grounds_count; i++) {
            VkDeviceSize at = 0;

            v->vkCmdBindDescriptorSets(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       r->ground_layout, 0, 1, &grounds[i].imagery.set, 0,
                                       NULL);
            v->vkCmdBindVertexBuffers(v->cmd, 0, 1, &grounds[i].buffer.buffer, &at);
            v->vkCmdBindIndexBuffer(v->cmd, grounds[i].buffer.buffer, grounds[i].indices_at,
                                    VK_INDEX_TYPE_UINT32);
            v->vkCmdDrawIndexed(v->cmd, grounds[i].count, 1, 0, 0, 0);
            r->draws++;
            r->triangles += grounds[i].count / 3;
        }
    }

    v->vkCmdBindPipeline(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->mesh);
    v->vkCmdPushConstants(v->cmd, r->mesh_layout,
                          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                          sizeof(mp), &mp);
    {
        VkDescriptorSet sets[2] = { r->white.set, r->sunmap_set };

        v->vkCmdBindDescriptorSets(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->mesh_layout, 0,
                                   2, sets, 0, NULL);
    }
    for (uint32_t i = 0; i < count; i++) {
        VkDeviceSize at = 0;

        v->vkCmdBindVertexBuffers(v->cmd, 0, 1, &meshes[i].buffer.buffer, &at);
        v->vkCmdBindIndexBuffer(v->cmd, meshes[i].buffer.buffer, meshes[i].indices_at,
                                VK_INDEX_TYPE_UINT32);
        v->vkCmdDrawIndexed(v->cmd, meshes[i].count, 1, 0, 0, 0);
        r->draws++;
        r->triangles += meshes[i].count / 3;
    }

    /* The models last, so the ground's and the meshes' depth is there for
     * them to be hidden by. The view goes once; each model then replaces
     * only its own matrix and tint, the bytes after the view's. */
    if (models_count > 0) {
        v->vkCmdBindPipeline(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->model);
        v->vkCmdPushConstants(v->cmd, r->model_layout, both, 0, sizeof(mp), &mp);
        v->vkCmdBindDescriptorSets(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->model_layout,
                                   1, 1, &r->sunmap_set, 0, NULL);
        for (uint32_t i = 0; i < models_count; i++) {
            const struct k3d_vk_model_draw *d = &models[i];
            const struct k3d_vk_texture *skin = d->texture != NULL ? d->texture : &r->white;
            struct model_push own;
            VkDeviceSize at = 0;

            if (d->model == NULL || d->model->count == 0) {
                continue;
            }
            affine_rows(d->matrix, own.model);
            memcpy(own.tint, d->tint, sizeof(own.tint));
            v->vkCmdPushConstants(v->cmd, r->model_layout, both,
                                  offsetof(struct model_push, model),
                                  sizeof(own) - offsetof(struct model_push, model), own.model);
            v->vkCmdBindDescriptorSets(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       r->model_layout, 0, 1, &skin->set, 0, NULL);
            v->vkCmdBindVertexBuffers(v->cmd, 0, 1, &d->model->buffer.buffer, &at);
            v->vkCmdBindIndexBuffer(v->cmd, d->model->buffer.buffer, d->model->indices_at,
                                    VK_INDEX_TYPE_UINT32);
            v->vkCmdDrawIndexed(v->cmd, d->model->count, 1, 0, 0, 0);
            r->draws++;
            r->triangles += d->model->count / 3;
        }
    }

    /* The overlays over all of it, each its rectangle in clip space -
     * Vulkan's, whose y runs down as a picture's does. */
    if (overlays_count > 0) {
        v->vkCmdBindPipeline(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->overlay);
        for (uint32_t i = 0; i < overlays_count; i++) {
            const struct k3d_vk_overlay *o = &overlays[i];
            float rect[4];

            if (o->texture == NULL || o->texture->set == VK_NULL_HANDLE || o->width == 0 ||
                o->height == 0) {
                continue;
            }
            rect[0] = 2.0f * (float)o->x / (float)r->width - 1.0f;
            rect[1] = 2.0f * (float)o->y / (float)r->height - 1.0f;
            rect[2] = 2.0f * (float)(o->x + (int32_t)o->width) / (float)r->width - 1.0f;
            rect[3] = 2.0f * (float)(o->y + (int32_t)o->height) / (float)r->height - 1.0f;
            v->vkCmdPushConstants(v->cmd, r->overlay_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                  sizeof(rect), rect);
            v->vkCmdBindDescriptorSets(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       r->overlay_layout, 0, 1, &o->texture->set, 0, NULL);
            v->vkCmdDraw(v->cmd, 6, 1, 0, 0);
            r->draws++;
            r->triangles += 2;
        }
    }
    v->vkCmdEndRendering(v->cmd);
    return 1;
}
