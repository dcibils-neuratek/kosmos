/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Vulkan Kit's base** (`docs/gpu3d.md` 5.1, `docs/flight.md` F3): what
 * every C program drawing with Vulkan on Kosmos starts from, once -
 *
 *     the instance and the device, through the driver's one door
 *     (`vk_icdGetInstanceProcAddr`: there is no loader, the driver is
 *     linked in), its queue and a command buffer;
 *     a buffer the processor maps, an image in the device's memory;
 *     a layout's barrier, a recording begun, submitted and waited for;
 *     a graphics pipeline of triangles, with or without depth;
 *     a Kosmos window as a surface and its swapchain (`VK_KOSMOS_surface`).
 *
 * It was `vulkantest.c`'s own until the 3D Kit needed the same (F3), and a
 * second copy is what this kit exists to prevent. Lua does not reach it:
 * Lua's doors are GL, the 3D Kit and the Game Kit, each written on this.
 *
 * A call that fails returns 0 and leaves the step and the VkResult in
 * `v->why` - "device/-3" - for the caller to say; nothing here prints.
 */

#ifndef KOSMOS_KITS_VULKAN_KVK_H
#define KOSMOS_KITS_VULKAN_KVK_H

#include <stddef.h>
#include <stdint.h>

#define VK_USE_PLATFORM_KOSMOS
#include <vulkan/vulkan.h>

struct kw_window;

/* The functions the kit's users call, each looked up through the door. */
#define KVK_FUNCTIONS(X)                                                    \
    X(vkCreateImage) X(vkDestroyImage) X(vkCreateImageView)                 \
    X(vkDestroyImageView) X(vkCreateBuffer) X(vkDestroyBuffer)              \
    X(vkGetImageMemoryRequirements) X(vkGetBufferMemoryRequirements)        \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory)                \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateShaderModule)            \
    X(vkDestroyShaderModule) X(vkCreatePipelineLayout)                      \
    X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines)                 \
    X(vkDestroyPipeline) X(vkCreateCommandPool) X(vkDestroyCommandPool)     \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)                     \
    X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdBeginRendering)    \
    X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdPushConstants)         \
    X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdBindVertexBuffers)              \
    X(vkCmdBindIndexBuffer) X(vkCmdBindDescriptorSets)                      \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkQueueSubmit)   \
    X(vkQueueWaitIdle) X(vkCreateSampler) X(vkDestroySampler)               \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout)          \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool)                    \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkDestroyDevice)  \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences)    \
    X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR)     \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)  \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool)        \
    X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)

struct kvk {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceMemoryProperties memory;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    char device_name[64];       /* "llvmpipe/1.4": to its first space, and the API */
    /* The device's clock, for a recording's time (`kvk_time_*`): two
     * timestamps, nanoseconds a tick, and the bits a stamp keeps - no pool
     * where the queue keeps no time. */
    VkQueryPool stamps;
    double stamp_ns;
    uint32_t stamp_bits;
    char why[96];               /* the step that failed and its VkResult */
#define KVK_DECLARE(name) PFN_##name name;
    KVK_FUNCTIONS(KVK_DECLARE)
#undef KVK_DECLARE
};

/* A call that did not succeed: `why` says which and how, and the caller's
 * `done:` label is gone to. */
#define KVK_STEP(v, call, what)                                          \
    do {                                                                 \
        VkResult r_ = (call);                                            \
        if (r_ != VK_SUCCESS) {                                          \
            kvk_failed((v), (what), r_);                                 \
            goto done;                                                   \
        }                                                                \
    } while (0)

void kvk_failed(struct kvk *v, const char *what, VkResult r);

/* The instance, the device, its queue and a command buffer: 1, or 0 with
 * `why`. `windowed`, and the instance has surfaces and the device
 * swapchains. `device_name` is set as soon as the device is found, so a
 * failure after it can still say which device it was. */
int  kvk_open(struct kvk *v, const char *program, int windowed);

/* The device's own time for what a recording does between `kvk_time_start`
 * (first in it, outside any rendering) and `kvk_time_end`, read after its
 * submission has finished: milliseconds, or -1 where the device keeps no
 * time or the stamps are not there. Start always answers 1, so it can sit
 * in a chain of steps. */
int    kvk_time_start(struct kvk *v);
void   kvk_time_end(struct kvk *v);
double kvk_time_ms(struct kvk *v);
void kvk_close(struct kvk *v);

uint32_t kvk_memory_type(const struct kvk *v, uint32_t bits, VkMemoryPropertyFlags want);

/* A buffer the processor can write and read, its memory mapped. */
struct kvk_buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *bytes;
};

int  kvk_buffer_make(struct kvk *v, struct kvk_buffer *b, VkDeviceSize size,
                     VkBufferUsageFlags usage, const char *what);
void kvk_buffer_free(struct kvk *v, struct kvk_buffer *b);

/* An image in the device's own memory, and a view of all of it. */
struct kvk_image {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
};

int  kvk_image_make(struct kvk *v, struct kvk_image *i, VkFormat format, uint32_t width,
                    uint32_t height, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                    const char *what);
void kvk_image_free(struct kvk *v, struct kvk_image *i);

/* An image's layout changed, with the stages and accesses either side. */
void kvk_barrier(struct kvk *v, VkImage image, VkImageAspectFlags aspect,
                 VkImageLayout from, VkImageLayout to,
                 VkPipelineStageFlags after, VkAccessFlags written,
                 VkPipelineStageFlags before, VkAccessFlags read);

/* The command buffer begun for one submission; ended, submitted and
 * waited for. */
int kvk_begin(struct kvk *v);
int kvk_submit(struct kvk *v);

/* A colour image drawn `width` by `height` copied, as it stands after
 * drawing, into a buffer the processor reads. */
void kvk_copy_out(struct kvk *v, VkImage image, uint32_t width, uint32_t height, VkBuffer out);

/* A colour image `width` by `height` filled from `from`, a buffer holding
 * its pixels packed row after row: recorded, the image going from nothing
 * to ready for fragment shaders to sample. */
void kvk_image_upload(struct kvk *v, VkImage image, uint32_t width, uint32_t height,
                      VkBuffer from);

/* **A sampled image's descriptor set**: the layout of one - a combined
 * image and sampler at binding 0, for fragment shaders - and one set of it
 * in a pool of its own, which goes when the pool does. A pool a set means
 * no count to guess; a few hundred is what a terrain holds. */
int kvk_sampled_layout(struct kvk *v, VkDescriptorSetLayout *out);
int kvk_sampled_set(struct kvk *v, VkDescriptorSetLayout layout, VkSampler sampler,
                    VkImageView view, VkDescriptorPool *pool, VkDescriptorSet *set);

/* A pipeline drawing triangles into one colour image of `format`, `width`
 * by `height`, with a depth buffer when `depth` is not
 * VK_FORMAT_UNDEFINED, and faces culled as `cull` says. */
struct kvk_pipeline_shape {
    const uint32_t *vert;
    size_t vert_bytes;
    const uint32_t *frag;
    size_t frag_bytes;
    const VkPipelineVertexInputStateCreateInfo *input;
    uint32_t width, height;
    VkFormat format, depth;
    VkCullModeFlags cull;
    int depth_off;                  /* drawn into a picture with a depth buffer
                                     * without testing it or writing it - a sky
                                     * drawn first, behind everything */
    int blend;                      /* laid over what is there by its alpha -
                                     * the 3D Kit's overlays; 0 replaces it */
};

int kvk_pipeline(struct kvk *v, VkPipeline *out, VkPipelineLayout layout,
                 const struct kvk_pipeline_shape *shape);

/* A window's swapchain: the surface, its images and their views, and a
 * fence for acquiring. Opened on a device opened `windowed`. */
#define KVK_SWAPCHAIN_MAX 8

struct kvk_swapchain {
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    uint32_t count;
    VkImage images[KVK_SWAPCHAIN_MAX];
    VkImageView views[KVK_SWAPCHAIN_MAX];
    VkFence fence;
    PFN_vkDestroySurfaceKHR destroy_surface;
};

/* 1, or 0 with `why`; the surface's extent is in `extent` either way once
 * it is known. FIFO, B8G8R8A8_UNORM, opaque. */
int  kvk_swapchain_open(struct kvk *v, struct kvk_swapchain *s, struct kw_window *window);
void kvk_swapchain_close(struct kvk *v, struct kvk_swapchain *s);

/* The next image to draw into, waited for: its index, or -1 with `why`. */
int kvk_acquire(struct kvk *v, struct kvk_swapchain *s);

/* That image, drawn and in PRESENT_SRC, handed to the window. */
int kvk_present(struct kvk *v, struct kvk_swapchain *s, uint32_t index);

#endif
