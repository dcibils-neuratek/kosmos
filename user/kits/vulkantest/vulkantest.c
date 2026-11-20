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

/* VK_KOSMOS_surface: a Kosmos window as a Vulkan surface (G3). */
#define VK_USE_PLATFORM_KOSMOS
#include <vulkan/vulkan.h>

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

#define STEP(call, what)                                         \
    do {                                                         \
        VkResult r_ = (call);                                    \
        if (r_ != VK_SUCCESS) {                                  \
            say(" failed=%s/%d", (what), (int)r_);               \
            goto done;                                           \
        }                                                        \
    } while (0)

/*
 * The device, opened once a call and closed after it, and the functions
 * this kit uses, each looked up through the one door.
 */
#define FUNCTIONS(X)                                                        \
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
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)

struct vk {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceMemoryProperties memory;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cmd;
#define DECLARE(name) PFN_##name name;
    FUNCTIONS(DECLARE)
#undef DECLARE
};

static uint32_t memory_type(const struct vk *v, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < v->memory.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (v->memory.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

/* The instance, the device, its queue and a command buffer: 1, or 0 with
 * the step that failed said. `windowed`, and the instance has surfaces and
 * the device swapchains. */
static int vk_open(struct vk *v, int windowed)
{
    PFN_vkGetInstanceProcAddr gipa = vk_icdGetInstanceProcAddr;
    PFN_vkCreateInstance create_instance =
        (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    uint32_t count = 1;

    memset(v, 0, sizeof(*v));
    if (create_instance == NULL) {
        say(" failed=no-vkCreateInstance");
        return 0;
    }

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vulkantest",
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    static const char *const surfaces[] = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_KOSMOS_SURFACE_EXTENSION_NAME,
    };
    if (windowed) {
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = surfaces;
    }
    STEP(create_instance(&ici, NULL, &v->instance), "instance");

    PFN_vkEnumeratePhysicalDevices enumerate =
        (PFN_vkEnumeratePhysicalDevices)gipa(v->instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties properties =
        (PFN_vkGetPhysicalDeviceProperties)gipa(v->instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties memory =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(v->instance,
                                                      "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkCreateDevice create_device =
        (PFN_vkCreateDevice)gipa(v->instance, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr gdpa =
        (PFN_vkGetDeviceProcAddr)gipa(v->instance, "vkGetDeviceProcAddr");

    STEP(enumerate(v->instance, &count, &v->physical), "physical-device");
    if (count == 0) {
        say(" failed=no-device");
        goto done;
    }

    VkPhysicalDeviceProperties props;
    properties(v->physical, &props);
    memory(v->physical, &v->memory);
    /* "llvmpipe (LLVM 23.1.2, 256 bits)": the name to its first space. */
    {
        char name[64];
        size_t n = strcspn(props.deviceName, " ");
        if (n >= sizeof(name)) {
            n = sizeof(name) - 1;
        }
        memcpy(name, props.deviceName, n);
        name[n] = '\0';
        say(" device=%s/%u.%u", name, VK_API_VERSION_MAJOR(props.apiVersion),
            VK_API_VERSION_MINOR(props.apiVersion));
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkPhysicalDeviceVulkan13Features v13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .dynamicRendering = VK_TRUE,
        .synchronization2 = VK_TRUE,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &v13,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    static const char *const swapchain[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    if (windowed) {
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = swapchain;
    }
    STEP(create_device(v->physical, &dci, NULL, &v->device), "device");

#define LOOK_UP(name) v->name = (PFN_##name)gdpa(v->device, #name);
    FUNCTIONS(LOOK_UP)
#undef LOOK_UP
    PFN_vkGetDeviceQueue get_queue = (PFN_vkGetDeviceQueue)gdpa(v->device, "vkGetDeviceQueue");
    get_queue(v->device, 0, 0, &v->queue);

    /* Recorded again after each submit - the texture's upload, then the
     * frame, then the next frame - which Vulkan allows only of a buffer
     * whose pool says so; without it lavapipe freed a buffer twice. */
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = 0,
    };
    STEP(v->vkCreateCommandPool(v->device, &cpci, NULL, &v->pool), "command-pool");
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = v->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    STEP(v->vkAllocateCommandBuffers(v->device, &cbai, &v->cmd), "command-buffer");
    return 1;

done:
    return 0;
}

static void vk_close(struct vk *v)
{
    PFN_vkGetInstanceProcAddr gipa = vk_icdGetInstanceProcAddr;

    if (v->device != VK_NULL_HANDLE) {
        if (v->pool != VK_NULL_HANDLE) {
            v->vkDestroyCommandPool(v->device, v->pool, NULL);
        }
        v->vkDestroyDevice(v->device, NULL);
    }
    if (v->instance != VK_NULL_HANDLE) {
        ((PFN_vkDestroyInstance)gipa(v->instance, "vkDestroyInstance"))(v->instance, NULL);
    }
}

/* A buffer the processor can write and read, and its memory mapped. */
struct buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *bytes;
};

static int buffer_make(struct vk *v, struct buffer *b, VkDeviceSize size,
                       VkBufferUsageFlags usage, const char *what)
{
    VkMemoryRequirements need;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
    };

    memset(b, 0, sizeof(*b));
    STEP(v->vkCreateBuffer(v->device, &bci, NULL, &b->buffer), what);
    v->vkGetBufferMemoryRequirements(v->device, b->buffer, &need);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = need.size,
        .memoryTypeIndex = memory_type(v, need.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    STEP(v->vkAllocateMemory(v->device, &mai, NULL, &b->memory), what);
    STEP(v->vkBindBufferMemory(v->device, b->buffer, b->memory, 0), what);
    STEP(v->vkMapMemory(v->device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->bytes), what);
    return 1;

done:
    return 0;
}

static void buffer_free(struct vk *v, struct buffer *b)
{
    if (b->buffer != VK_NULL_HANDLE) {
        v->vkDestroyBuffer(v->device, b->buffer, NULL);
    }
    if (b->memory != VK_NULL_HANDLE) {
        v->vkFreeMemory(v->device, b->memory, NULL);
    }
}

/* An image in the device's own memory, and a view of all of it. */
struct image {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
};

static int image_make(struct vk *v, struct image *i, VkFormat format, uint32_t width,
                      uint32_t height, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                      const char *what)
{
    VkMemoryRequirements need;
    VkImageCreateInfo imci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    memset(i, 0, sizeof(*i));
    STEP(v->vkCreateImage(v->device, &imci, NULL, &i->image), what);
    v->vkGetImageMemoryRequirements(v->device, i->image, &need);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = need.size,
        .memoryTypeIndex = memory_type(v, need.memoryTypeBits, 0),
    };
    STEP(v->vkAllocateMemory(v->device, &mai, NULL, &i->memory), what);
    STEP(v->vkBindImageMemory(v->device, i->image, i->memory, 0), what);
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = i->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = { aspect, 0, 1, 0, 1 },
    };
    STEP(v->vkCreateImageView(v->device, &vci, NULL, &i->view), what);
    return 1;

done:
    return 0;
}

static void image_free(struct vk *v, struct image *i)
{
    if (i->view != VK_NULL_HANDLE) {
        v->vkDestroyImageView(v->device, i->view, NULL);
    }
    if (i->image != VK_NULL_HANDLE) {
        v->vkDestroyImage(v->device, i->image, NULL);
    }
    if (i->memory != VK_NULL_HANDLE) {
        v->vkFreeMemory(v->device, i->memory, NULL);
    }
}

/* An image's layout changed, with the stages and accesses either side. */
static void barrier(struct vk *v, VkImage image, VkImageAspectFlags aspect,
                    VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags after, VkAccessFlags written,
                    VkPipelineStageFlags before, VkAccessFlags read)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = written,
        .dstAccessMask = read,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { aspect, 0, 1, 0, 1 },
    };
    v->vkCmdPipelineBarrier(v->cmd, after, before, 0, 0, NULL, 0, NULL, 1, &b);
}

static int submit(struct vk *v)
{
    VkSubmitInfo s = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &v->cmd,
    };

    STEP(v->vkEndCommandBuffer(v->cmd), "end");
    STEP(v->vkQueueSubmit(v->queue, 1, &s, VK_NULL_HANDLE), "submit");
    STEP(v->vkQueueWaitIdle(v->queue), "wait");
    return 1;

done:
    return 0;
}

static int begin(struct vk *v)
{
    VkCommandBufferBeginInfo b = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    STEP(v->vkBeginCommandBuffer(v->cmd, &b), "begin");
    return 1;

done:
    return 0;
}

/* A pipeline drawing triangles into one colour image of `format`, with a
 * depth buffer when `depth` is not VK_FORMAT_UNDEFINED. */
static int pipeline_make(struct vk *v, VkPipeline *out, VkPipelineLayout layout,
                         const uint32_t *vert, size_t vert_bytes,
                         const uint32_t *frag, size_t frag_bytes,
                         const VkPipelineVertexInputStateCreateInfo *input,
                         uint32_t size, VkFormat format, VkFormat depth)
{
    VkShaderModule modules[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vert_bytes,
        .pCode = vert,
    };
    int made = 0;

    STEP(v->vkCreateShaderModule(v->device, &smci, NULL, &modules[0]), "vertex-shader");
    smci.codeSize = frag_bytes;
    smci.pCode = frag;
    STEP(v->vkCreateShaderModule(v->device, &smci, NULL, &modules[1]), "fragment-shader");

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = modules[0], .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = modules[1], .pName = "main" },
    };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport viewport = { 0, 0, (float)size, (float)size, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, { size, size } };
    VkPipelineViewportStateCreateInfo viewports = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport,
        .scissorCount = 1, .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineDepthStencilStateCreateInfo depths = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = depth != VK_FORMAT_UNDEFINED,
        .depthWriteEnable = depth != VK_FORMAT_UNDEFINED,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };
    VkPipelineColorBlendAttachmentState blend = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend,
    };
    VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &format,
        .depthAttachmentFormat = depth,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewports,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depths,
        .pColorBlendState = &blending,
        .layout = layout,
    };
    STEP(v->vkCreateGraphicsPipelines(v->device, VK_NULL_HANDLE, 1, &gpci, NULL, out),
         "pipeline");
    made = 1;

done:
    for (int i = 0; i < 2; i++) {
        if (modules[i] != VK_NULL_HANDLE) {
            v->vkDestroyShaderModule(v->device, modules[i], NULL);
        }
    }
    return made;
}

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
static void copy_out(struct vk *v, VkImage image, uint32_t size, VkBuffer out)
{
    barrier(v, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { size, size, 1 },
    };
    v->vkCmdCopyImageToBuffer(v->cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out, 1,
                              &region);
}

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
    struct vk v;
    struct image target = { 0 };
    struct buffer readback = { 0 };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    said[0] = '\0';
    say("vulkan");
    if (!vk_open(&v, 0)) {
        goto done;
    }

    if (!image_make(&v, &target, VK_FORMAT_R8G8B8A8_UNORM, TRIANGLE, TRIANGLE,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, "image") ||
        !buffer_make(&v, &readback, TRIANGLE * TRIANGLE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
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
    if (!pipeline_make(&v, &pipeline, layout, triangle_vert, sizeof(triangle_vert),
                       triangle_frag, sizeof(triangle_frag), &none, TRIANGLE,
                       VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED) ||
        !begin(&v)) {
        goto done;
    }

    barrier(&v, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
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
    copy_out(&v, target.image, TRIANGLE, readback.buffer);
    if (!submit(&v)) {
        goto done;
    }

    say_picture(readback.bytes, TRIANGLE, (const uint8_t (*)[3])colour, "#", 1);

done:
    if (v.device != VK_NULL_HANDLE) {
        if (pipeline != VK_NULL_HANDLE) {
            v.vkDestroyPipeline(v.device, pipeline, NULL);
        }
        if (layout != VK_NULL_HANDLE) {
            v.vkDestroyPipelineLayout(v.device, layout, NULL);
        }
        buffer_free(&v, &readback);
        image_free(&v, &target);
    }
    vk_close(&v);
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
    struct image depth, checks;
    struct buffer mesh, upload;
    VkSampler sampler;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool descriptors;
    VkDescriptorSet set;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    uint32_t size;
    VkDeviceSize indices_at;
};

static void scene_free(struct vk *v, struct scene *s)
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
    buffer_free(v, &s->upload);
    buffer_free(v, &s->mesh);
    image_free(v, &s->checks);
    image_free(v, &s->depth);
}

/* The scene for a `size` square target of `format`, its texture uploaded:
 * 1, or 0 with the step that failed said. */
static int scene_make(struct vk *v, struct scene *s, VkFormat format, uint32_t size,
                      const uint8_t colours[2][3])
{
    struct vertex vertices[24];
    uint16_t indices[36];

    memset(s, 0, sizeof(*s));
    s->size = size;
    s->indices_at = sizeof(vertices);
    cube_mesh(vertices, indices);

    if (!image_make(v, &s->depth, VK_FORMAT_D32_SFLOAT, size, size,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                    "depth") ||
        !image_make(v, &s->checks, VK_FORMAT_R8G8B8A8_UNORM, CHECKS, CHECKS,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, "texture") ||
        !buffer_make(v, &s->mesh, sizeof(vertices) + sizeof(indices),
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     "mesh") ||
        !buffer_make(v, &s->upload, CHECKS * CHECKS * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
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
    if (!pipeline_make(v, &s->pipeline, s->layout, cube_vert, sizeof(cube_vert), cube_frag,
                       sizeof(cube_frag), &input, size, format, VK_FORMAT_D32_SFLOAT) ||
        !begin(v)) {
        return 0;
    }

    /* The texture, from the buffer the checks were written into. */
    barrier(v, s->checks.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { CHECKS, CHECKS, 1 },
    };
    v->vkCmdCopyBufferToImage(v->cmd, s->upload.buffer, s->checks.image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(v, s->checks.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    return submit(v);

done:
    return 0;
}

/* Record the cube, turned by `transform`, into `target` through `view`:
 * from nothing to a colour attachment, cleared to opaque black, drawn. */
static void scene_draw(struct vk *v, struct scene *s, VkImage target, VkImageView view,
                       const float transform[16])
{
    VkDeviceSize at = 0;

    barrier(v, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(v, s->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
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
    struct vk v;
    struct scene s = { 0 };
    struct image target = { 0 };
    struct buffer readback = { 0 };
    float transform[16];

    said[0] = '\0';
    say("vulkan");
    if (!vk_open(&v, 0)) {
        goto done;
    }

    cube_transform(yaw, pitch, distance, focal, transform);

    if (!image_make(&v, &target, VK_FORMAT_R8G8B8A8_UNORM, CUBE, CUBE,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, "image") ||
        !buffer_make(&v, &readback, CUBE * CUBE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     "buffer") ||
        !scene_make(&v, &s, VK_FORMAT_R8G8B8A8_UNORM, CUBE, colours) ||
        !begin(&v)) {
        goto done;
    }

    scene_draw(&v, &s, target.image, target.view, transform);
    copy_out(&v, target.image, CUBE, readback.buffer);
    if (!submit(&v)) {
        goto done;
    }

    say_picture(readback.bytes, CUBE, colours, "ab", 2);

done:
    if (v.device != VK_NULL_HANDLE) {
        scene_free(&v, &s);
        buffer_free(&v, &readback);
        image_free(&v, &target);
    }
    vk_close(&v);
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

#define SWAPCHAIN_MAX 8

static struct {
    struct vk v;
    struct kw_window *window;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkImageView views[SWAPCHAIN_MAX];
    uint32_t count;
    struct scene s;
    VkFence fence;
    PFN_vkDestroySurfaceKHR destroy_surface;
} shown;

static void window_close(void)
{
    struct vk *v = &shown.v;

    if (v->device != VK_NULL_HANDLE) {
        v->vkDeviceWaitIdle(v->device);
        scene_free(v, &shown.s);
        for (uint32_t i = 0; i < shown.count; i++) {
            v->vkDestroyImageView(v->device, shown.views[i], NULL);
        }
        if (shown.fence != VK_NULL_HANDLE) {
            v->vkDestroyFence(v->device, shown.fence, NULL);
        }
        if (shown.swapchain != VK_NULL_HANDLE) {
            v->vkDestroySwapchainKHR(v->device, shown.swapchain, NULL);
        }
    }
    if (shown.surface != VK_NULL_HANDLE) {
        shown.destroy_surface(v->instance, shown.surface, NULL);
    }
    vk_close(v);
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
    struct vk *v = &shown.v;
    PFN_vkGetInstanceProcAddr gipa = vk_icdGetInstanceProcAddr;
    VkImage images[SWAPCHAIN_MAX];
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[8];
    uint32_t nformats = 8;
    VkFormat format = VK_FORMAT_UNDEFINED;
    double started;

    said[0] = '\0';
    say("vulkan");
    if (shown.window != NULL) {
        window_close();
    }
    if (!vk_open(v, 1)) {
        goto done;
    }

    shown.window = kw_open("Vulkan cube", size, size, 0);
    if (shown.window == NULL) {
        say(" failed=window/%s", kw_why());
        goto done;
    }

    PFN_vkCreateKosmosSurfaceKOSMOS create_surface =
        (PFN_vkCreateKosmosSurfaceKOSMOS)gipa(v->instance, "vkCreateKosmosSurfaceKOSMOS");
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get_caps =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gipa(
            v->instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR get_formats =
        (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)gipa(
            v->instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR get_support =
        (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)gipa(
            v->instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    shown.destroy_surface =
        (PFN_vkDestroySurfaceKHR)gipa(v->instance, "vkDestroySurfaceKHR");
    if (create_surface == NULL || get_caps == NULL || get_formats == NULL ||
        get_support == NULL || shown.destroy_surface == NULL) {
        say(" failed=no-surface-functions");
        goto done;
    }

    VkKosmosSurfaceCreateInfoKOSMOS sci = {
        .sType = VK_STRUCTURE_TYPE_KOSMOS_SURFACE_CREATE_INFO_KOSMOS,
        .window = shown.window,
    };
    STEP(create_surface(v->instance, &sci, NULL, &shown.surface), "surface");

    VkBool32 supported = VK_FALSE;
    STEP(get_support(v->physical, 0, shown.surface, &supported), "support");
    if (!supported) {
        say(" failed=queue-cannot-present");
        goto done;
    }
    STEP(get_caps(v->physical, shown.surface, &caps), "capabilities");
    STEP(get_formats(v->physical, shown.surface, &nformats, formats), "formats");
    for (uint32_t i = 0; i < nformats; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            format = formats[i].format;
        }
    }
    say(" extent=%ux%u", caps.currentExtent.width, caps.currentExtent.height);
    if (format == VK_FORMAT_UNDEFINED) {
        say(" failed=no-B8G8R8A8_UNORM");
        goto done;
    }
    if (caps.currentExtent.width != size || caps.currentExtent.height != size) {
        say(" failed=window-not-%ux%u", size, size);
        goto done;
    }

    VkSwapchainCreateInfoKHR swci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = shown.surface,
        .minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount,
        .imageFormat = format,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = caps.currentExtent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    STEP(v->vkCreateSwapchainKHR(v->device, &swci, NULL, &shown.swapchain), "swapchain");
    shown.count = SWAPCHAIN_MAX;
    STEP(v->vkGetSwapchainImagesKHR(v->device, shown.swapchain, &shown.count, images),
         "swapchain-images");
    for (uint32_t i = 0; i < shown.count; i++) {
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        STEP(v->vkCreateImageView(v->device, &vci, NULL, &shown.views[i]), "swapchain-view");
    }
    say(" images=%u", shown.count);

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    STEP(v->vkCreateFence(v->device, &fci, NULL, &shown.fence), "fence");
    if (!scene_make(v, &shown.s, format, size, colours)) {
        goto done;
    }

    started = seconds();
    for (int f = 0; f < frames; f++) {
        float transform[16];
        uint32_t index;

        cube_transform(yaw - step * (frames - 1 - f), pitch, distance, focal, transform);
        STEP(v->vkAcquireNextImageKHR(v->device, shown.swapchain, UINT64_MAX, VK_NULL_HANDLE,
                                      shown.fence, &index), "acquire");
        STEP(v->vkWaitForFences(v->device, 1, &shown.fence, VK_TRUE, UINT64_MAX), "acquired");
        STEP(v->vkResetFences(v->device, 1, &shown.fence), "fence-reset");
        if (!begin(v)) {
            goto done;
        }
        scene_draw(v, &shown.s, images[index], shown.views[index], transform);
        barrier(v, images[index], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
        if (!submit(v)) {
            goto done;
        }
        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .swapchainCount = 1,
            .pSwapchains = &shown.swapchain,
            .pImageIndices = &index,
        };
        STEP(v->vkQueuePresentKHR(v->queue, &pi), "present");
    }
    say(" frames=%d ms=%d", frames, (int)((seconds() - started) * 1000.0));
    return;

done:
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
    lua_pushcfunction(L, l_hold);
    lua_setfield(L, -2, "hold");
    lua_pushinteger(L, TRIANGLE);
    lua_setfield(L, -2, "triangle_size");
    lua_pushinteger(L, CUBE);
    lua_setfield(L, -2, "cube_size");
}
