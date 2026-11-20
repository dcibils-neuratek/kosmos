/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The 3D Kit on Vulkan (`k3d_vk.h`): a sky, then meshes lit by the sun and
 * hazed by distance, into a picture with a depth buffer.
 */

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "k3d_vk.h"

#include "k3d_mesh_vert.h"
#include "k3d_mesh_frag.h"
#include "k3d_sky_vert.h"
#include "k3d_sky_frag.h"

/* The shaders' push constants, as they declare them (std430: vec4s). */
struct mesh_push {
    float clip[16];
    float eye[4];
    float sun[4];       /* w: ambient */
    float haze[4];
    float range[4];     /* start, end */
};

struct sky_push {
    float unclip[16];
    float eye[4];
    float zenith[4];
    float horizon[4];
};

static void refuse(struct kvk *v, const char *why)
{
    strncpy(v->why, why, sizeof(v->why) - 1);
    v->why[sizeof(v->why) - 1] = '\0';
}

static int pipeline_layout(struct kvk *v, VkPipelineLayout *out, uint32_t bytes,
                           VkShaderStageFlags stages)
{
    VkPushConstantRange range = { stages, 0, bytes };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
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

    if (!kvk_image_make(v, &r->depth, VK_FORMAT_D32_SFLOAT, width, height,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                        "depth") ||
        !pipeline_layout(v, &r->mesh_layout, sizeof(struct mesh_push), both) ||
        !pipeline_layout(v, &r->sky_layout, sizeof(struct sky_push),
                         VK_SHADER_STAGE_FRAGMENT_BIT)) {
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
        width, height, format, VK_FORMAT_D32_SFLOAT, VK_CULL_MODE_NONE, 0,
    };
    /* The sky: no depth test, drawn first, under everything. */
    struct kvk_pipeline_shape sky = {
        k3d_sky_vert, sizeof(k3d_sky_vert), k3d_sky_frag, sizeof(k3d_sky_frag), &none,
        width, height, format, VK_FORMAT_D32_SFLOAT, VK_CULL_MODE_NONE, 1,
    };

    if (!kvk_pipeline(v, &r->mesh, r->mesh_layout, &mesh) ||
        !kvk_pipeline(v, &r->sky, r->sky_layout, &sky)) {
        return 0;
    }
    return 1;
}

void k3d_vk_close(struct k3d_vk *r)
{
    struct kvk *v = r->v;

    if (v == NULL || v->device == VK_NULL_HANDLE) {
        return;
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
 * straight up or down has no right, and is refused rather than guessed. */
static int frame(const struct k3d_vk_view *view, double f[3], double rt[3], double u[3])
{
    static const double world_up[3] = { 0, 1, 0 };

    for (int i = 0; i < 3; i++) {
        f[i] = (double)view->target[i] - view->eye[i];
    }
    if (!unit(f)) {
        return 0;
    }
    cross(f, world_up, rt);
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

int k3d_vk_draw(struct k3d_vk *r, VkImage target, VkImageView view_of,
                const struct k3d_vk_view *view, const struct k3d_vk_mesh *meshes,
                uint32_t count)
{
    struct kvk *v = r->v;
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
    }
    mp.sun[3] = view->ambient;
    mp.range[0] = view->haze_start;
    mp.range[1] = view->haze_end;

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

    v->vkCmdBindPipeline(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->mesh);
    v->vkCmdPushConstants(v->cmd, r->mesh_layout,
                          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                          sizeof(mp), &mp);
    for (uint32_t i = 0; i < count; i++) {
        VkDeviceSize at = 0;

        v->vkCmdBindVertexBuffers(v->cmd, 0, 1, &meshes[i].buffer.buffer, &at);
        v->vkCmdBindIndexBuffer(v->cmd, meshes[i].buffer.buffer, meshes[i].indices_at,
                                VK_INDEX_TYPE_UINT32);
        v->vkCmdDrawIndexed(v->cmd, meshes[i].count, 1, 0, 0, 0);
    }
    v->vkCmdEndRendering(v->cmd);
    return 1;
}
