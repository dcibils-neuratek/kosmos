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
 *                     sun, the sky's two colours and the haze.
 *
 * It stands on the Vulkan Kit's base (`kits/vulkan/kvk.h`) for the device
 * and everything around it, and records into the caller's command buffer:
 * the caller says what the picture is - an image read back, or a window's
 * swapchain image - and when it is submitted.
 *
 * **The view's conventions**, which a test can work out on its own: the
 * world's y is up; a pixel's centre (px + 0.5, py + 0.5) of a `width` by
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
};

struct k3d_vk_mesh {
    struct kvk_buffer buffer;       /* vertices, then indices */
    VkDeviceSize indices_at;
    uint32_t count;                 /* indices */
};

struct k3d_vk {
    struct kvk *v;
    uint32_t width, height;
    VkFormat format;
    struct kvk_image depth;
    VkPipelineLayout mesh_layout, sky_layout;
    VkPipeline mesh, sky;
};

/* The renderer for pictures of `format`, `width` by `height`: 1, or 0 with
 * `v->why`. */
int  k3d_vk_open(struct k3d_vk *r, struct kvk *v, VkFormat format,
                 uint32_t width, uint32_t height);
void k3d_vk_close(struct k3d_vk *r);

/* A mesh of `nv` corners and `ni` indices, three to a triangle. */
int  k3d_vk_mesh(struct k3d_vk *r, struct k3d_vk_mesh *m,
                 const struct k3d_vk_vertex *vertices, uint32_t nv,
                 const uint32_t *indices, uint32_t ni);
void k3d_vk_mesh_free(struct k3d_vk *r, struct k3d_vk_mesh *m);

/* The world to clip space for `view`, column-major as Vulkan reads it; 0
 * when the eye and the target are one place or the lens is not a lens. */
int k3d_vk_clip(const struct k3d_vk_view *view, uint32_t width, uint32_t height,
                float clip[16]);

/* Recorded into the command buffer: `target`, from nothing to a colour
 * attachment, filled with the sky and the meshes. It is left in
 * COLOR_ATTACHMENT_OPTIMAL for the caller to copy out or present. 0, with
 * `why`, when the view is not one. */
int k3d_vk_draw(struct k3d_vk *r, VkImage target, VkImageView view_of,
                const struct k3d_vk_view *view, const struct k3d_vk_mesh *meshes,
                uint32_t count);

#endif
