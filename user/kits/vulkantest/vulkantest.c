/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Vulkan on Kosmos** (`docs/gpu3d.md` G2): a kit in no image but
 * `vulkantest.elf`, reached as `use("vulkantest.elf")`. Each call draws one
 * picture with `lavapipe` - Mesa's Vulkan driver, on llvmpipe and LLVM's
 * JIT - into an image in memory and reads it back:
 *
 *     triangle(x0, y0, x1, y1, x2, y2, r, g, b)
 *         one triangle, its corners in pixels of a `TRIANGLE` square and
 *         its colour in bytes;
 *     cube(yaw, pitch, distance, focal, r0, g0, b0, r1, g1, b1)
 *         a cube two units across, turned by `pitch` about x and then
 *         `yaw` about y, `distance` in front of the eye, seen through a
 *         lens of `focal` (the clip space's x and y over the eye's z),
 *         each face an 8 by 8 checkerboard of the two colours read from a
 *         texture, nearest texel, with a depth buffer, in a `CUBE` square;
 *     window(yaw, pitch, distance, focal, r0, g0, b0, r1, g1, b1, frames, step[, side])
 *         the same cube in a square window `side` across, `CUBE` unless said (G3), through
 *         VK_KOSMOS_surface and a swapchain: `frames` frames presented, the
 *         cube turned `step` further in yaw each, to `yaw` at the last -
 *         what it says is the surface's extent, the swapchain's images and
 *         how long the frames took, and what is on the screen at the end is
 *         the caller's to find;
 *     hold(seconds)
 *         that window kept, showing its last frame, for `seconds` or until
 *         it is closed: true if it was open to the end.
 *
 * Everything that decides the picture is the caller's, so nothing here
 * knows the answer. What it says is the device's name and the picture as a
 * mask, a row at a time: a letter for each of the caller's colours exactly
 * (`#` for the triangle's, `a` and `b` for the cube's), `.` for the clear
 * colour exactly, `?` for anything else. The caller, who chose all of it,
 * decides which pixels had to be which (`tools/run_vulkan.py`).
 *
 * There is no Vulkan loader: nothing on Kosmos is loaded, and `lavapipe`
 * is linked in. Its `vk_icdGetInstanceProcAddr` is the one door, as a
 * loader would use it. A step that fails is named in the answer.
 */

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"

#include "kits/kitenv.h"
#include "kits/window/kosmos_window.h"

/* The device, memory, recording, a pipeline and a window's swapchain:
 * the Vulkan Kit's base, shared with the 3D Kit (`docs/flight.md` F3). */
#include "kits/vulkan/kvk.h"
#include "kits/3d/k3d_vk.h"

#include "triangle_vert.h"
#include "triangle_frag.h"
#include "cube_vert.h"
#include "cube_frag.h"

#define TRIANGLE 32
#define CUBE 64
#define CHECKS 8

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

/* The console, so that what Mesa or LLVM says - an abort's last words
 * among it - is heard rather than spilled (`runtime/posix/fd.c`). */
void posix_console(long capability);
long posix_console_capability(void);

static char said[CUBE * (CUBE + 1) + 512];

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *fmt, ...)
{
    size_t at = strlen(said);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(said + at, sizeof(said) - at, fmt, ap);
    va_end(ap);
}

/* A step the Vulkan Kit refused, said as this kit's own were: once, where
 * the call gave up. */
static void said_failure(const struct kvk *v)
{
    if (v->why[0]) {
        say(" failed=%s", v->why);
    }
}

#define STEP(call, what)                                         \
    do {                                                         \
        VkResult r_ = (call);                                    \
        if (r_ != VK_SUCCESS) {                                  \
            say(" failed=%s/%d", (what), (int)r_);               \
            goto done;                                           \
        }                                                        \
    } while (0)

/* The picture, as the caller reads it: a letter for each of `n` colours
 * exactly, `.` for opaque black, `?` for anything else. */
static void say_picture(const uint8_t *pixels, uint32_t size, const uint8_t (*colours)[3],
                        const char *letters, int n)
{
    say(" picture=");
    for (uint32_t y = 0; y < size; y++) {
        for (uint32_t x = 0; x < size; x++) {
            const uint8_t *p = pixels + (y * size + x) * 4;
            char c = '?';
            if (p[3] == 255) {
                if (p[0] == 0 && p[1] == 0 && p[2] == 0) {
                    c = '.';
                }
                for (int i = 0; i < n; i++) {
                    if (p[0] == colours[i][0] && p[1] == colours[i][1] &&
                        p[2] == colours[i][2]) {
                        c = letters[i];
                    }
                }
            }
            say("%c", c);
        }
        say(y + 1 < size ? "/" : "");
    }
}

/* Record a copy of `image` into `out` for the processor to read. */
/*
 * ---------------------------------------------------------------- triangle
 */

/* The shaders' push constants, as `triangle.vert` declares them (std430). */
struct triangle {
    float colour[4];
    float corner[3][2];
    float size[2];
};

static void triangle(const struct triangle *t, const uint8_t colour[3])
{
    struct kvk v;
    struct kvk_image target = { 0 };
    struct kvk_buffer readback = { 0 };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    said[0] = '\0';
    say("vulkan");
    if (!kvk_open(&v, "vulkantest", 0)) {
        if (v.device_name[0]) {
            say(" device=%s", v.device_name);
        }
        goto done;
    }
    say(" device=%s", v.device_name);

    if (!kvk_image_make(&v, &target, VK_FORMAT_R8G8B8A8_UNORM, TRIANGLE, TRIANGLE,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, "image") ||
        !kvk_buffer_make(&v, &readback, TRIANGLE * TRIANGLE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     "buffer")) {
        goto done;
    }

    VkPushConstantRange range = {
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(*t)
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    };
    STEP(v.vkCreatePipelineLayout(v.device, &plci, NULL, &layout), "pipeline-layout");
    VkPipelineVertexInputStateCreateInfo none = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    struct kvk_pipeline_shape shape = {
        triangle_vert, sizeof(triangle_vert), triangle_frag, sizeof(triangle_frag), &none,
        TRIANGLE, TRIANGLE, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, VK_CULL_MODE_NONE, 0,
    };
    if (!kvk_pipeline(&v, &pipeline, layout, &shape) ||
        !kvk_begin(&v)) {
        goto done;
    }

    kvk_barrier(&v, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = target.view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } },
    };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { TRIANGLE, TRIANGLE } },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    };
    v.vkCmdBeginRendering(v.cmd, &ri);
    v.vkCmdBindPipeline(v.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    v.vkCmdPushConstants(v.cmd, layout, range.stageFlags, 0, sizeof(*t), t);
    v.vkCmdDraw(v.cmd, 3, 1, 0, 0);
    v.vkCmdEndRendering(v.cmd);
    kvk_copy_out(&v, target.image, TRIANGLE, TRIANGLE, readback.buffer);
    if (!kvk_submit(&v)) {
        goto done;
    }

    say_picture(readback.bytes, TRIANGLE, (const uint8_t (*)[3])colour, "#", 1);

done:
    said_failure(&v);
    if (v.device != VK_NULL_HANDLE) {
        if (pipeline != VK_NULL_HANDLE) {
            v.vkDestroyPipeline(v.device, pipeline, NULL);
        }
        if (layout != VK_NULL_HANDLE) {
            v.vkDestroyPipelineLayout(v.device, layout, NULL);
        }
        kvk_buffer_free(&v, &readback);
        kvk_image_free(&v, &target);
    }
    kvk_close(&v);
}

/*
 * -------------------------------------------------------------------- cube
 */

struct vertex {
    float position[3];
    float uv[2];
};

/*
 * The cube's 24 corners, four a face, and its 36 indices. A face is the
 * one whose `axis` coordinate is `side`; its texture runs along the next
 * axis round (u) and the one after (v), from -1 to 1 as 0 to 1 - the rule
 * the caller's reference is computed by, so the two cannot disagree about
 * which way a face's checks run.
 */
static void cube_mesh(struct vertex vertices[24], uint16_t indices[36])
{
    int n = 0, m = 0;

    for (int axis = 0; axis < 3; axis++) {
        for (int side = -1; side <= 1; side += 2) {
            int u = (axis + 1) % 3, w = (axis + 2) % 3, first = n;

            for (int corner = 0; corner < 4; corner++) {
                float cu = (corner & 1) ? 1.0f : -1.0f;
                float cw = (corner & 2) ? 1.0f : -1.0f;
                struct vertex *p = &vertices[n++];

                p->position[axis] = (float)side;
                p->position[u] = cu;
                p->position[w] = cw;
                p->uv[0] = (cu + 1.0f) / 2.0f;
                p->uv[1] = (cw + 1.0f) / 2.0f;
            }

            static const int order[6] = { 0, 1, 2, 2, 1, 3 };
            for (int i = 0; i < 6; i++) {
                indices[m++] = (uint16_t)(first + order[i]);
            }
        }
    }
}

/*
 * The transform to clip space, column by column as GLSL reads a `mat4`:
 * the cube turned by `pitch` about x and then `yaw` about y, moved
 * `distance` along the eye's z, and projected with x and y over z times
 * `focal`, depth from `NEAR` to `FAR` as 0 to 1.
 */
#define NEAR 0.5
#define FAR 20.0

static void cube_transform(double yaw, double pitch, double distance, double focal,
                           float out[16])
{
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch);
    /* Ry(yaw) * Rx(pitch), rows. */
    double r[3][3] = {
        { cy, sy * sp, sy * cp },
        { 0, cp, -sp },
        { -sy, cy * sp, cy * cp },
    };
    double a = FAR / (FAR - NEAR), b = -NEAR * FAR / (FAR - NEAR);
    double m[4][4];

    for (int col = 0; col < 3; col++) {
        m[0][col] = focal * r[0][col];
        m[1][col] = focal * r[1][col];
        m[2][col] = a * r[2][col];
        m[3][col] = r[2][col];
    }
    m[0][3] = 0;
    m[1][3] = 0;
    m[2][3] = a * distance + b;
    m[3][3] = distance;

    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            out[col * 4 + row] = (float)m[row][col];
        }
    }
}

/*
 * The scene a cube is drawn from, whatever it is drawn into: its mesh, its
 * checkerboard as a texture with a sampler and a descriptor, and a pipeline
 * drawing into `format` with a depth buffer of the target's size. The
 * offscreen picture and the window are the same cube by the same code; the
 * window only adds where the pixels go.
 */
struct scene {
    struct kvk_image depth, checks;
    struct kvk_buffer mesh, upload;
    VkSampler sampler;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool descriptors;
    VkDescriptorSet set;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    uint32_t size;
    VkDeviceSize indices_at;
};

static void scene_free(struct kvk *v, struct scene *s)
{
    if (v->device == VK_NULL_HANDLE) {
        return;
    }
    if (s->pipeline != VK_NULL_HANDLE) {
        v->vkDestroyPipeline(v->device, s->pipeline, NULL);
    }
    if (s->layout != VK_NULL_HANDLE) {
        v->vkDestroyPipelineLayout(v->device, s->layout, NULL);
    }
    if (s->descriptors != VK_NULL_HANDLE) {
        v->vkDestroyDescriptorPool(v->device, s->descriptors, NULL);
    }
    if (s->set_layout != VK_NULL_HANDLE) {
        v->vkDestroyDescriptorSetLayout(v->device, s->set_layout, NULL);
    }
    if (s->sampler != VK_NULL_HANDLE) {
        v->vkDestroySampler(v->device, s->sampler, NULL);
    }
    kvk_buffer_free(v, &s->upload);
    kvk_buffer_free(v, &s->mesh);
    kvk_image_free(v, &s->checks);
    kvk_image_free(v, &s->depth);
}

/* The scene for a `size` square target of `format`, its texture uploaded:
 * 1, or 0 with the step that failed said. */
static int scene_make(struct kvk *v, struct scene *s, VkFormat format, uint32_t size,
                      const uint8_t colours[2][3])
{
    struct vertex vertices[24];
    uint16_t indices[36];

    memset(s, 0, sizeof(*s));
    s->size = size;
    s->indices_at = sizeof(vertices);
    cube_mesh(vertices, indices);

    if (!kvk_image_make(v, &s->depth, VK_FORMAT_D32_SFLOAT, size, size,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                    "depth") ||
        !kvk_image_make(v, &s->checks, VK_FORMAT_R8G8B8A8_UNORM, CHECKS, CHECKS,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, "texture") ||
        !kvk_buffer_make(v, &s->mesh, sizeof(vertices) + sizeof(indices),
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     "mesh") ||
        !kvk_buffer_make(v, &s->upload, CHECKS * CHECKS * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     "upload")) {
        return 0;
    }

    memcpy(s->mesh.bytes, vertices, sizeof(vertices));
    memcpy((uint8_t *)s->mesh.bytes + sizeof(vertices), indices, sizeof(indices));
    for (int y = 0; y < CHECKS; y++) {
        for (int x = 0; x < CHECKS; x++) {
            uint8_t *p = (uint8_t *)s->upload.bytes + (y * CHECKS + x) * 4;
            const uint8_t *c = colours[(x + y) % 2];
            p[0] = c[0];
            p[1] = c[1];
            p[2] = c[2];
            p[3] = 255;
        }
    }

    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    STEP(v->vkCreateSampler(v->device, &sci, NULL, &s->sampler), "sampler");

    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    STEP(v->vkCreateDescriptorSetLayout(v->device, &dslci, NULL, &s->set_layout), "set-layout");
    VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    STEP(v->vkCreateDescriptorPool(v->device, &dpci, NULL, &s->descriptors), "descriptor-pool");
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = s->descriptors,
        .descriptorSetCount = 1,
        .pSetLayouts = &s->set_layout,
    };
    STEP(v->vkAllocateDescriptorSets(v->device, &dsai, &s->set), "descriptor-set");
    VkDescriptorImageInfo texture = {
        .sampler = s->sampler,
        .imageView = s->checks.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = s->set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &texture,
    };
    v->vkUpdateDescriptorSets(v->device, 1, &write, 0, NULL);

    VkPushConstantRange range = { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 * sizeof(float) };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &s->set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    };
    STEP(v->vkCreatePipelineLayout(v->device, &plci, NULL, &s->layout), "pipeline-layout");

    VkVertexInputBindingDescription stream = { 0, sizeof(struct vertex),
                                               VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attributes[2] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(struct vertex, position) },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(struct vertex, uv) },
    };
    VkPipelineVertexInputStateCreateInfo input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &stream,
        .vertexAttributeDescriptionCount = 2,
        .pVertexAttributeDescriptions = attributes,
    };
    struct kvk_pipeline_shape shape = {
        cube_vert, sizeof(cube_vert), cube_frag, sizeof(cube_frag), &input,
        size, size, format, VK_FORMAT_D32_SFLOAT, VK_CULL_MODE_NONE, 0,
    };
    if (!kvk_pipeline(v, &s->pipeline, s->layout, &shape) ||
        !kvk_begin(v)) {
        return 0;
    }

    /* The texture, from the buffer the checks were written into. */
    kvk_barrier(v, s->checks.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { CHECKS, CHECKS, 1 },
    };
    v->vkCmdCopyBufferToImage(v->cmd, s->upload.buffer, s->checks.image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    kvk_barrier(v, s->checks.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    return kvk_submit(v);

done:
    return 0;
}

/* Record the cube, turned by `transform`, into `target` through `view`:
 * from nothing to a colour attachment, cleared to opaque black, drawn. */
static void scene_draw(struct kvk *v, struct scene *s, VkImage target, VkImageView view,
                       const float transform[16])
{
    VkDeviceSize at = 0;

    kvk_barrier(v, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    kvk_barrier(v, s->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo colour_attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } },
    };
    VkRenderingAttachmentInfo depth_attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = s->depth.view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue = { .depthStencil = { 1.0f, 0 } },
    };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { s->size, s->size } },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colour_attachment,
        .pDepthAttachment = &depth_attachment,
    };
    v->vkCmdBeginRendering(v->cmd, &ri);
    v->vkCmdBindPipeline(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline);
    v->vkCmdBindDescriptorSets(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->layout, 0, 1,
                               &s->set, 0, NULL);
    v->vkCmdPushConstants(v->cmd, s->layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                          16 * sizeof(float), transform);
    v->vkCmdBindVertexBuffers(v->cmd, 0, 1, &s->mesh.buffer, &at);
    v->vkCmdBindIndexBuffer(v->cmd, s->mesh.buffer, s->indices_at, VK_INDEX_TYPE_UINT16);
    v->vkCmdDrawIndexed(v->cmd, 36, 1, 0, 0, 0);
    v->vkCmdEndRendering(v->cmd);
}

static void cube(double yaw, double pitch, double distance, double focal,
                 const uint8_t colours[2][3])
{
    struct kvk v;
    struct scene s = { 0 };
    struct kvk_image target = { 0 };
    struct kvk_buffer readback = { 0 };
    float transform[16];

    said[0] = '\0';
    say("vulkan");
    if (!kvk_open(&v, "vulkantest", 0)) {
        if (v.device_name[0]) {
            say(" device=%s", v.device_name);
        }
        goto done;
    }
    say(" device=%s", v.device_name);

    cube_transform(yaw, pitch, distance, focal, transform);

    if (!kvk_image_make(&v, &target, VK_FORMAT_R8G8B8A8_UNORM, CUBE, CUBE,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, "image") ||
        !kvk_buffer_make(&v, &readback, CUBE * CUBE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     "buffer") ||
        !scene_make(&v, &s, VK_FORMAT_R8G8B8A8_UNORM, CUBE, colours) ||
        !kvk_begin(&v)) {
        goto done;
    }

    scene_draw(&v, &s, target.image, target.view, transform);
    kvk_copy_out(&v, target.image, CUBE, CUBE, readback.buffer);
    if (!kvk_submit(&v)) {
        goto done;
    }

    say_picture(readback.bytes, CUBE, colours, "ab", 2);

done:
    said_failure(&v);
    if (v.device != VK_NULL_HANDLE) {
        scene_free(&v, &s);
        kvk_buffer_free(&v, &readback);
        kvk_image_free(&v, &target);
    }
    kvk_close(&v);
}

/*
 * ------------------------------------------------------------------ window
 *
 * **G3**: the same cube in a Kosmos window, through VK_KOSMOS_surface and
 * a swapchain. `frames` frames, the cube turning to `yaw` by the last of
 * them - so what is on the screen at the end is the picture the offscreen
 * cube at the same angle is, and the caller finds it there. The window
 * stays open, holding its last frame, until `hold` asks it to go.
 */

static struct {
    struct kvk v;
    struct kw_window *window;
    struct kvk_swapchain chain;
    struct scene s;
} shown;

static void window_close(void)
{
    struct kvk *v = &shown.v;

    if (v->device != VK_NULL_HANDLE) {
        v->vkDeviceWaitIdle(v->device);
        scene_free(v, &shown.s);
    }
    kvk_swapchain_close(v, &shown.chain);
    kvk_close(v);
    if (shown.window != NULL) {
        kw_close(shown.window);
    }
    memset(&shown, 0, sizeof(shown));
}

static double seconds(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void window(double yaw, double pitch, double distance, double focal,
                   const uint8_t colours[2][3], int frames, double step, uint32_t size)
{
    struct kvk *v = &shown.v;
    struct kvk_swapchain *c = &shown.chain;
    double started;

    said[0] = '\0';
    say("vulkan");
    if (shown.window != NULL) {
        window_close();
    }
    if (!kvk_open(v, "vulkantest", 1)) {
        if (v->device_name[0]) {
            say(" device=%s", v->device_name);
        }
        goto done;
    }
    say(" device=%s", v->device_name);

    shown.window = kw_open("Vulkan cube", size, size, 0);
    if (shown.window == NULL) {
        say(" failed=window/%s", kw_why());
        goto done;
    }

    int opened = kvk_swapchain_open(v, c, shown.window);
    if (c->extent.width != 0) {
        say(" extent=%ux%u", c->extent.width, c->extent.height);
    }
    if (!opened) {
        goto done;
    }
    if (c->extent.width != size || c->extent.height != size) {
        say(" failed=window-not-%ux%u", size, size);
        goto done;
    }
    say(" images=%u", c->count);

    if (!scene_make(v, &shown.s, c->format, size, colours)) {
        goto done;
    }

    started = seconds();
    for (int f = 0; f < frames; f++) {
        float transform[16];
        int index;

        cube_transform(yaw - step * (frames - 1 - f), pitch, distance, focal, transform);
        if ((index = kvk_acquire(v, c)) < 0 || !kvk_begin(v)) {
            goto done;
        }
        scene_draw(v, &shown.s, c->images[index], c->views[index], transform);
        kvk_barrier(v, c->images[index], VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
        if (!kvk_submit(v) || !kvk_present(v, c, (uint32_t)index)) {
            goto done;
        }
    }
    say(" frames=%d ms=%d", frames, (int)((seconds() - started) * 1000.0));
    return;

done:
    said_failure(v);
    window_close();
}

/* The window, open and holding its last frame, for `wait` seconds or until
 * it is closed: 1 if it was still open at the end. */
static int hold(double wait)
{
    double until = seconds() + wait;
    struct kw_event e;

    while (shown.window != NULL && seconds() < until) {
        if (kw_poll(shown.window, &e, 100) && e.type == KW_CLOSE) {
            window_close();
            return 0;
        }
    }
    if (shown.window == NULL) {
        return 0;
    }
    window_close();
    return 1;
}


/*
 * ---------------------------------------------------------------- the door
 */

static void hand_console(lua_State *L)
{
    if (posix_console_capability() < 0) {
        posix_console(kit_capability(L, "/Devices/console"));
    }
}

static uint8_t byte_at(lua_State *L, int arg)
{
    lua_Integer c = luaL_checkinteger(L, arg);

    luaL_argcheck(L, c >= 0 && c <= 255, arg, "a colour is a byte");
    return (uint8_t)c;
}

static int l_triangle(lua_State *L)
{
    struct triangle t;
    uint8_t colour[3];

    for (int i = 0; i < 3; i++) {
        t.corner[i][0] = (float)luaL_checknumber(L, 1 + 2 * i);
        t.corner[i][1] = (float)luaL_checknumber(L, 2 + 2 * i);
    }
    for (int i = 0; i < 3; i++) {
        colour[i] = byte_at(L, 7 + i);
        t.colour[i] = (float)colour[i] / 255.0f;
    }
    t.colour[3] = 1.0f;
    t.size[0] = t.size[1] = TRIANGLE;

    hand_console(L);
    triangle(&t, colour);
    lua_pushstring(L, said);
    return 1;
}

static int l_cube(lua_State *L)
{
    double yaw = luaL_checknumber(L, 1), pitch = luaL_checknumber(L, 2);
    double distance = luaL_checknumber(L, 3), focal = luaL_checknumber(L, 4);
    uint8_t colours[2][3];

    luaL_argcheck(L, distance > NEAR + 2.0 && distance < FAR - 2.0, 3,
                  "the cube must be between the near and far planes");
    for (int i = 0; i < 6; i++) {
        colours[i / 3][i % 3] = byte_at(L, 5 + i);
    }

    hand_console(L);
    cube(yaw, pitch, distance, focal, (const uint8_t (*)[3])colours);
    lua_pushstring(L, said);
    return 1;
}

static int l_window(lua_State *L)
{
    double yaw = luaL_checknumber(L, 1), pitch = luaL_checknumber(L, 2);
    double distance = luaL_checknumber(L, 3), focal = luaL_checknumber(L, 4);
    lua_Integer frames = luaL_checkinteger(L, 11);
    double step = luaL_checknumber(L, 12);
    /* The window's side, `CUBE` unless said: G7c's frames a second are
     * measured on a large one too (`testing.md` 18.936). */
    lua_Integer size = luaL_optinteger(L, 13, CUBE);
    uint8_t colours[2][3];

    luaL_argcheck(L, distance > NEAR + 2.0 && distance < FAR - 2.0, 3,
                  "the cube must be between the near and far planes");
    luaL_argcheck(L, frames >= 1 && frames <= 100000, 11, "frames from 1");
    luaL_argcheck(L, size >= 16 && size <= 4096, 13, "a side from 16 to 4096");
    for (int i = 0; i < 6; i++) {
        colours[i / 3][i % 3] = byte_at(L, 5 + i);
    }

    hand_console(L);
    window(yaw, pitch, distance, focal, (const uint8_t (*)[3])colours, (int)frames, step,
           (uint32_t)size);
    lua_pushstring(L, said);
    return 1;
}

/*
 * ------------------------------------------------------------------- scene
 *
 * **F3b**: the 3D Kit's renderer (`kits/3d/k3d_vk.c`) on this device, held
 * to a picture the caller works out for itself: boxes the caller places,
 * lit by a sun it chooses, hazed and under a sky of its colours, through a
 * lens it chooses - drawn into an image in memory and read back as bytes.
 */

#define SCENE_MAX 128               /* a side, in pixels */
#define BOXES_MAX 16

/* A box from (x0, y0, z0) to (x1, y1, z1), each face its own four corners
 * with that face's normal, so each is lit flat. */
static void box_mesh(const float lo[3], const float hi[3], const uint8_t rgb[3],
                     struct k3d_vk_vertex v[24], uint32_t idx[36])
{
    /* For each face: the axis it faces along, its sign, and its corners as
     * (lo or hi) on each axis, counter-clockwise from outside. */
    static const struct { int axis; float sign; uint8_t c[4][3]; } faces[6] = {
        { 0, -1, { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 1 }, { 0, 1, 0 } } },
        { 0, +1, { { 1, 0, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 1, 0, 1 } } },
        { 1, -1, { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 } } },
        { 1, +1, { { 0, 1, 0 }, { 0, 1, 1 }, { 1, 1, 1 }, { 1, 1, 0 } } },
        { 2, -1, { { 0, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 }, { 1, 0, 0 } } },
        { 2, +1, { { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } } },
    };

    for (int f = 0; f < 6; f++) {
        for (int k = 0; k < 4; k++) {
            struct k3d_vk_vertex *p = &v[f * 4 + k];

            for (int a = 0; a < 3; a++) {
                p->position[a] = faces[f].c[k][a] ? hi[a] : lo[a];
                p->normal[a] = a == faces[f].axis ? faces[f].sign : 0.0f;
            }
            p->colour[0] = rgb[0], p->colour[1] = rgb[1], p->colour[2] = rgb[2];
            p->colour[3] = 255;
        }
        static const uint32_t two[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; k++) {
            idx[f * 6 + k] = (uint32_t)(f * 4) + two[k];
        }
    }
}

/* A field of the caller's table: `n` numbers. */
static void numbers(lua_State *L, int table, const char *name, float *out, int n)
{
    lua_getfield(L, table, name);
    if (n == 1) {
        out[0] = (float)luaL_checknumber(L, -1);
    } else {
        luaL_checktype(L, -1, LUA_TTABLE);
        for (int i = 0; i < n; i++) {
            lua_rawgeti(L, -1, i + 1);
            out[i] = (float)luaL_checknumber(L, -1);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

/* kit.scene{ size, eye, target, focal, near, far, sun, ambient, zenith,
 * horizon, haze, haze_start, haze_end, boxes = { {x0, y0, z0, x1, y1, z1,
 * r, g, b}, ... } } -> what was said, and the picture's bytes, RGB, a row
 * at a time from the top. */
static int l_scene(lua_State *L)
{
    struct k3d_vk_view view;
    struct kvk v;
    struct k3d_vk r = { 0 };
    struct k3d_vk_mesh meshes[BOXES_MAX];
    struct kvk_image target = { 0 };
    struct kvk_buffer readback = { 0 };
    uint32_t nboxes = 0, size;
    float boxes[BOXES_MAX][9];
    luaL_Buffer out;

    luaL_checktype(L, 1, LUA_TTABLE);
    memset(&view, 0, sizeof(view));
    memset(meshes, 0, sizeof(meshes));
    lua_getfield(L, 1, "size");
    size = (uint32_t)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    luaL_argcheck(L, size >= 4 && size <= SCENE_MAX, 1, "a scene is 4 to 128 pixels a side");
    numbers(L, 1, "eye", view.eye, 3);
    numbers(L, 1, "target", view.target, 3);
    numbers(L, 1, "focal", &view.focal, 1);
    numbers(L, 1, "near", &view.near, 1);
    numbers(L, 1, "far", &view.far, 1);
    numbers(L, 1, "sun", view.sun, 3);
    numbers(L, 1, "ambient", &view.ambient, 1);
    numbers(L, 1, "zenith", view.zenith, 3);
    numbers(L, 1, "horizon", view.horizon, 3);
    numbers(L, 1, "haze", view.haze, 3);
    numbers(L, 1, "haze_start", &view.haze_start, 1);
    numbers(L, 1, "haze_end", &view.haze_end, 1);
    lua_getfield(L, 1, "boxes");
    luaL_checktype(L, -1, LUA_TTABLE);
    nboxes = (uint32_t)lua_rawlen(L, -1);
    luaL_argcheck(L, nboxes <= BOXES_MAX, 1, "at most 16 boxes");
    for (uint32_t b = 0; b < nboxes; b++) {
        lua_rawgeti(L, -1, (lua_Integer)b + 1);
        luaL_checktype(L, -1, LUA_TTABLE);
        for (int k = 0; k < 9; k++) {
            lua_rawgeti(L, -1, k + 1);
            boxes[b][k] = (float)luaL_checknumber(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    hand_console(L);
    said[0] = '\0';
    say("vulkan");
    luaL_buffinit(L, &out);
    if (!kvk_open(&v, "vulkantest", 0)) {
        if (v.device_name[0]) {
            say(" device=%s", v.device_name);
        }
        goto done;
    }
    say(" device=%s", v.device_name);

    if (!kvk_image_make(&v, &target, VK_FORMAT_R8G8B8A8_UNORM, size, size,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, "image") ||
        !kvk_buffer_make(&v, &readback, (VkDeviceSize)size * size * 4,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT, "buffer") ||
        !k3d_vk_open(&r, &v, VK_FORMAT_R8G8B8A8_UNORM, size, size)) {
        goto done;
    }
    for (uint32_t b = 0; b < nboxes; b++) {
        struct k3d_vk_vertex corners[24];
        uint32_t idx[36];
        uint8_t rgb[3] = { (uint8_t)boxes[b][6], (uint8_t)boxes[b][7], (uint8_t)boxes[b][8] };

        box_mesh(&boxes[b][0], &boxes[b][3], rgb, corners, idx);
        if (!k3d_vk_mesh(&r, &meshes[b], corners, 24, idx, 36)) {
            goto done;
        }
    }
    if (!kvk_begin(&v) || !k3d_vk_draw(&r, target.image, target.view, &view, meshes, nboxes)) {
        goto done;
    }
    kvk_copy_out(&v, target.image, size, size, readback.buffer);
    if (!kvk_submit(&v)) {
        goto done;
    }

    say(" scene=%ux%u boxes=%u", size, size, nboxes);
    for (uint32_t i = 0; i < size * size; i++) {
        const uint8_t *p = (const uint8_t *)readback.bytes + (size_t)i * 4;

        luaL_addlstring(&out, (const char *)p, 3);
    }

done:
    said_failure(&v);
    if (v.device != VK_NULL_HANDLE) {
        if (r.v != NULL) {
            for (uint32_t b = 0; b < nboxes; b++) {
                k3d_vk_mesh_free(&r, &meshes[b]);
            }
            k3d_vk_close(&r);
        }
        kvk_buffer_free(&v, &readback);
        kvk_image_free(&v, &target);
    }
    kvk_close(&v);
    luaL_pushresult(&out);          /* the buffer's, before anything else is pushed */
    lua_pushstring(L, said);
    lua_insert(L, -2);
    return 2;
}

static int l_hold(lua_State *L)
{
    lua_pushboolean(L, hold(luaL_checknumber(L, 1)));
    return 1;
}

void kosmos_vulkantest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_triangle);
    lua_setfield(L, -2, "triangle");
    lua_pushcfunction(L, l_cube);
    lua_setfield(L, -2, "cube");
    lua_pushcfunction(L, l_window);
    lua_setfield(L, -2, "window");
    lua_pushcfunction(L, l_scene);
    lua_setfield(L, -2, "scene");
    lua_pushcfunction(L, l_hold);
    lua_setfield(L, -2, "hold");
    lua_pushinteger(L, TRIANGLE);
    lua_setfield(L, -2, "triangle_size");
    lua_pushinteger(L, CUBE);
    lua_setfield(L, -2, "cube_size");
}
