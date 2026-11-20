/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The 3D Kit on Vulkan** (`docs/flight.md` F3b): meshes drawn by the GPU
 * into a picture, with a depth buffer, a sky, the sun's light and haze -
 * what Flight's terrain (F4) is drawn by, and Cafesa3D's Solid view later.
 *
 *     a renderer      for a picture of one format and size: its pipelines
 *                     and its depth buffer;
 *     a mesh          triangles in the world, each corner with its normal
 *                     and colour, uploaded once and drawn every frame;
 *     a view          where the eye is and where it looks, the lens, the
 *                     sun, the sky's two colours and the haze;
 *     a ground        a terrain chunk (`k3d_terrain.h`) with its imagery,
 *                     sampled between texels, lit and hazed as a mesh is;
 *     a texture       a picture uploaded once, sampled between texels -
 *                     a ground's imagery, or a model's skin;
 *     a model         triangles in a model's own coordinates, each corner
 *                     with its normal and its place on a texture, drawn
 *                     every frame by a matrix the caller gives it (F5d):
 *                     an aircraft where it is, as it is turned, and each
 *                     moving part - an aileron, the propeller - by its own.
 *
 * It stands on the Vulkan Kit's base (`kits/vulkan/kvk.h`) for the device
 * and everything around it, and records into the caller's command buffer:
 * the caller says what the picture is - an image read back, or a window's
 * swapchain image - and when it is submitted.
 *
 * **The view's conventions**, which a test can work out on its own: the
 * world's y is up, and so is the eye's unless the view's `up` says
 * otherwise (right is forward across that up, and up is right across
 * forward); a pixel's centre (px + 0.5, py + 0.5) of a `width` by
 * `height` picture is the direction
 *
 *     forward + right * x / focal - up * y / focal * height / width
 *
 * from the eye, where x and y are that centre from -1 to 1 across and
 * down; depth runs from `near` (0) to `far` (1). A surface is lit as
 * ambient + (1 - ambient) * max(normal . sun, 0) of its colour, then hazed
 * by its distance d from the eye - mix(lit, haze, clamp((d - start) /
 * (end - start), 0, 1)). The sky, where nothing was drawn, is the horizon
 * turning to the zenith as the direction's y rises from 0 to 1.
 */

#ifndef K3D_VK_H
#define K3D_VK_H

#include <stdint.h>

#include "kits/vulkan/kvk.h"
#include "k3d_terrain.h"

struct k3d_vk_vertex {
    float position[3];
    float normal[3];
    uint8_t colour[4];              /* RGBA */
};

struct k3d_vk_view {
    float eye[3], target[3];
    float focal;                    /* the lens: forward over the half-width */
    float near, far;
    float sun[3];                   /* towards the sun; made a unit here */
    float ambient;                  /* 0 to 1 */
    float zenith[3], horizon[3];    /* 0 to 1 */
    float haze[3];                  /* 0 to 1 */
    float haze_start, haze_end;     /* metres from the eye */
    float up[3];                    /* the eye's up, towards the top of the
                                     * picture: all zeros is the world's,
                                     * (0, 1, 0); a cockpit's rolls with the
                                     * aircraft. Last, so an initialiser that
                                     * does not name it leaves it zero. */
};

struct k3d_vk_mesh {
    struct kvk_buffer buffer;       /* vertices, then indices */
    VkDeviceSize indices_at;
    uint32_t count;                 /* indices */
};

/* A picture in the device's memory and the descriptor set that samples
 * it: a ground's imagery and a model's skin are both one. */
struct k3d_vk_texture {
    struct kvk_image image;
    VkDescriptorPool pool;
    VkDescriptorSet set;
};

struct k3d_vk_ground {
    struct kvk_buffer buffer;       /* vertices, then indices */
    VkDeviceSize indices_at;
    uint32_t count;                 /* indices */
    struct k3d_vk_texture imagery;
};

struct k3d_vk_model_vertex {
    float position[3];              /* in the model's own coordinates */
    float normal[3];
    float uv[2];                    /* on its texture, (0, 0) its first texel */
};

/* A model's geometry alone: which texture it wears is said when it is
 * drawn, because glTF's materials share textures between meshes and a
 * texture uploaded once a mesh would be the same picture twice. */
struct k3d_vk_model {
    struct kvk_buffer buffer;       /* vertices, then indices */
    VkDeviceSize indices_at;
    uint32_t count;                 /* indices */
};

/* One model drawn in one frame. */
struct k3d_vk_model_draw {
    const struct k3d_vk_model *model;
    const struct k3d_vk_texture *texture;   /* NULL: plain white, so the
                                             * tint alone is the colour */
    float tint[4];                  /* RGBA, 0 to 1, the texture times it -
                                     * glTF's base colour factor; a texel
                                     * whose alpha comes out under a half is
                                     * not drawn */
    float matrix[16];               /* the world from the model, column-major */
};

struct k3d_vk {
    struct kvk *v;
    uint32_t width, height;
    VkFormat format;
    struct kvk_image depth;
    VkPipelineLayout mesh_layout, sky_layout, ground_layout, model_layout;
    VkPipeline mesh, sky, ground, model;
    VkDescriptorSetLayout imagery_layout;
    VkSampler imagery;              /* linear, clamped to the edge */
    VkSampler skin;                 /* linear, repeating: a model's uv may
                                     * leave 0 to 1 and glTF wraps it */
    struct k3d_vk_texture white;    /* 1 by 1, what an untextured model wears */
};

/* The renderer for pictures of `format`, `width` by `height`: 1, or 0 with
 * `v->why`. It uploads a texture of its own, in a submission of its own,
 * so it is not opened inside a recording. */
int  k3d_vk_open(struct k3d_vk *r, struct kvk *v, VkFormat format,
                 uint32_t width, uint32_t height);
void k3d_vk_close(struct k3d_vk *r);

/* A mesh of `nv` corners and `ni` indices, three to a triangle. */
int  k3d_vk_mesh(struct k3d_vk *r, struct k3d_vk_mesh *m,
                 const struct k3d_vk_vertex *vertices, uint32_t nv,
                 const uint32_t *indices, uint32_t ni);
void k3d_vk_mesh_free(struct k3d_vk *r, struct k3d_vk_mesh *m);

/* A ground of `nv` vertices and `ni` indices, with its imagery `width` by
 * `height`, four bytes a texel packed a row at a time from the north, in
 * `format` - R8G8B8A8 as a test writes it, or B8G8R8A8 as gfx keeps a
 * decoded picture (0xAARRGGBB): uploaded now, in a submission of its own,
 * so not inside a recording. */
int  k3d_vk_ground(struct k3d_vk *r, struct k3d_vk_ground *g,
                   const struct k3d_terrain_vertex *vertices, uint32_t nv,
                   const uint32_t *indices, uint32_t ni, const void *texels,
                   uint32_t width, uint32_t height, VkFormat format);
void k3d_vk_ground_free(struct k3d_vk *r, struct k3d_vk_ground *g);

/* A texture `width` by `height`, four bytes a texel packed a row at a time
 * from the top, in `format` as a ground's imagery is: uploaded now, in a
 * submission of its own. */
int  k3d_vk_texture(struct k3d_vk *r, struct k3d_vk_texture *t, const void *texels,
                    uint32_t width, uint32_t height, VkFormat format);
void k3d_vk_texture_free(struct k3d_vk *r, struct k3d_vk_texture *t);

/* A model of `nv` corners and `ni` indices, three to a triangle. */
int  k3d_vk_model(struct k3d_vk *r, struct k3d_vk_model *m,
                  const struct k3d_vk_model_vertex *vertices, uint32_t nv,
                  const uint32_t *indices, uint32_t ni);
void k3d_vk_model_free(struct k3d_vk *r, struct k3d_vk_model *m);

/* The world to clip space for `view`, column-major as Vulkan reads it; 0
 * when the eye and the target are one place or the lens is not a lens. */
int k3d_vk_clip(const struct k3d_vk_view *view, uint32_t width, uint32_t height,
                float clip[16]);

/* Recorded into the command buffer: `target`, from nothing to a colour
 * attachment, filled with the sky, the grounds and the meshes. It is left in
 * COLOR_ATTACHMENT_OPTIMAL for the caller to copy out or present. 0, with
 * `why`, when the view is not one. */
int k3d_vk_draw(struct k3d_vk *r, VkImage target, VkImageView view_of,
                const struct k3d_vk_view *view, const struct k3d_vk_mesh *meshes,
                uint32_t count, const struct k3d_vk_ground *grounds, uint32_t grounds_count);

/* The same, with `models_count` models drawn after the grounds and the
 * meshes, depth-tested against them; `k3d_vk_draw` is this with none. */
int k3d_vk_draw_models(struct k3d_vk *r, VkImage target, VkImageView view_of,
                       const struct k3d_vk_view *view, const struct k3d_vk_mesh *meshes,
                       uint32_t count, const struct k3d_vk_ground *grounds,
                       uint32_t grounds_count, const struct k3d_vk_model_draw *models,
                       uint32_t models_count);

#endif
