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
 *     a Kosmos window as a surface and its swapchain (`VK_KOSMOS_surface`);
 *     a compute pipeline, and its dispatches recorded with the buffers
 *     they read and write (`docs/xavier.md` X12: the LLM Kit's tensors).
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
    X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)                         \
    X(vkCreateComputePipelines) X(vkCmdDispatch) X(vkCmdFillBuffer)         \
    X(vkFlushMappedMemoryRanges) X(vkInvalidateMappedMemoryRanges)

struct kvk {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceMemoryProperties memory;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    char device_name[64];       /* "llvmpipe/1.4": to its first space, and the API */
    int depth_filters;          /* a sampled D32_SFLOAT filters linearly: a shadow
                                   map's compare then blends four texels */
    /* The device's clock, for a recording's time (`kvk_time_*`): two
     * timestamps, nanoseconds a tick, and the bits a stamp keeps - no pool
     * where the queue keeps no time. */
    VkQueryPool stamps;
    double stamp_ns;
    uint32_t stamp_bits;
    uint32_t marks;             /* the marks in this recording (`kvk_mark`) */
    char why[96];               /* the step that failed and its VkResult */
    /* What compute on tensors may use, asked for with KVK_COMPUTE: the
     * KVK_HAS_ bits the device has, and the one call of
     * VK_KHR_push_descriptor's. */
    unsigned tensors;
    uint32_t subgroup_size;     /* lanes a subgroup, as the device reports */
    uint32_t max_groups[3];     /* workgroups a dispatch may ask for, each way */
    uint32_t storage_align;     /* where a span of a storage buffer may begin */
    PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR;
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
 * `why`. KVK_WINDOWED, and the instance has surfaces and the device
 * swapchains; KVK_COMPUTE, and the device has what tensor shaders use
 * that it can give (`tensors`) and push descriptors, which it must.
 * `device_name` is set as soon as the device is found, so a failure after
 * it can still say which device it was. */
#define KVK_WINDOWED 1
#define KVK_COMPUTE  2

#define KVK_HAS_STORAGE16     0x01u   /* 16-bit values in storage buffers */
#define KVK_HAS_STORAGE8      0x02u   /* 8-bit values in storage buffers */
#define KVK_HAS_FLOAT16       0x04u   /* float16_t arithmetic */
#define KVK_HAS_INT8          0x08u   /* int8_t arithmetic */
#define KVK_HAS_INT16         0x10u   /* int16_t arithmetic */
#define KVK_HAS_SUBGROUP_SIZE 0x20u   /* a pipeline's subgroup size chosen */

int  kvk_open(struct kvk *v, const char *program, int flags);

/* The device's own time for what a recording does between `kvk_time_start`
 * (first in it, outside any rendering) and `kvk_time_end`, read after its
 * submission has finished: milliseconds, or -1 where the device keeps no
 * time or the stamps are not there. Start always answers 1, so it can sit
 * in a chain of steps. */
int    kvk_time_start(struct kvk *v);
void   kvk_time_end(struct kvk *v);
double kvk_time_ms(struct kvk *v);

/* **Marks** (Xavier on Vulkan, V6): the device's time at up to
 * `KVK_MARKS` points of one recording, for where its time goes - a mark
 * after each barrier says how long the work before it took. Started first
 * in a recording, outside any rendering; read after the submit, as the
 * milliseconds between each mark and the one before, `room` at most.
 * Marks past `KVK_MARKS` are not made, and none where the queue keeps no
 * time. */
#define KVK_MARKS 512
void     kvk_marks_start(struct kvk *v);
void     kvk_mark(struct kvk *v);
uint32_t kvk_marks_read(struct kvk *v, double *ms, uint32_t room);
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
    float bias_constant, bias_slope;/* depth pushed away as it is drawn - the
                                     * 3D Kit's shadow map, so a surface does
                                     * not shadow itself; 0, none. A `format`
                                     * of VK_FORMAT_UNDEFINED is depth alone */
};

int kvk_pipeline(struct kvk *v, VkPipeline *out, VkPipelineLayout layout,
                 const struct kvk_pipeline_shape *shape);

/* **A compute pipeline** (`docs/xavier.md` X12): a shader of SPIR-V whose
 * storage buffers are bindings 0 to `buffers` - 1 of set 0, given as each
 * dispatch is recorded (push descriptors), with `push_bytes` of push
 * constants and `nspec` specialisation constants, ids 0 up, four bytes
 * each - which is how llama.cpp's shaders take their block sizes. */
#define KVK_COMPUTE_BUFFERS 8

struct kvk_compute {
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    uint32_t buffers;
    uint32_t push_bytes;
};

int  kvk_compute_make(struct kvk *v, struct kvk_compute *c, const uint32_t *spirv,
                      size_t spirv_bytes, uint32_t buffers, uint32_t push_bytes,
                      const uint32_t *spec, uint32_t nspec, const char *what);
void kvk_compute_free(struct kvk *v, struct kvk_compute *c);

/* A run of a buffer a shader is given: `range` 0 is to its end. */
struct kvk_span {
    VkBuffer buffer;
    VkDeviceSize offset, range;
};

/* Recorded: `c` bound, its buffers and push constants given, and `x` by
 * `y` by `z` workgroups dispatched. */
void kvk_compute_dispatch(struct kvk *v, const struct kvk_compute *c,
                          const struct kvk_span *spans, const void *push,
                          uint32_t x, uint32_t y, uint32_t z);

/* Recorded: what the dispatches before it wrote is what the ones after
 * it read - and, at the end of a recording, what the processor reads. */
void kvk_compute_barrier(struct kvk *v);

/* A buffer as `kvk_buffer_make` makes it, for compute: storage and
 * transfers, in memory the processor maps and reads from its caches where
 * the device has such memory - the GPU's results and the weights it reads
 * both read back at the processor's speed (`docs/xavier.md` X12). */
int kvk_buffer_make_storage(struct kvk *v, struct kvk_buffer *b, VkDeviceSize size,
                            const char *what);

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
