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
 *         texture, nearest texel, with a depth buffer, in a `CUBE` square.
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

#include "lua.h"
#include "lauxlib.h"

#include "kits/kitenv.h"

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
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkDestroyDevice)

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
 * the step that failed said. */
static int vk_open(struct vk *v)
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
    STEP(create_device(v->physical, &dci, NULL, &v->device), "device");

#define LOOK_UP(name) v->name = (PFN_##name)gdpa(v->device, #name);
    FUNCTIONS(LOOK_UP)
#undef LOOK_UP
    PFN_vkGetDeviceQueue get_queue = (PFN_vkGetDeviceQueue)gdpa(v->device, "vkGetDeviceQueue");
    get_queue(v->device, 0, 0, &v->queue);

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
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
    if (!vk_open(&v)) {
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

static void cube(double yaw, double pitch, double distance, double focal,
                 const uint8_t colours[2][3])
{
    struct vk v;
    struct image target = { 0 }, depth = { 0 }, checks = { 0 };
    struct buffer readback = { 0 }, mesh = { 0 }, upload = { 0 };
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkDescriptorSet set;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    struct vertex vertices[24];
    uint16_t indices[36];
    float transform[16];

    said[0] = '\0';
    say("vulkan");
    if (!vk_open(&v)) {
        goto done;
    }

    cube_mesh(vertices, indices);
    cube_transform(yaw, pitch, distance, focal, transform);

    if (!image_make(&v, &target, VK_FORMAT_R8G8B8A8_UNORM, CUBE, CUBE,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, "image") ||
        !image_make(&v, &depth, VK_FORMAT_D32_SFLOAT, CUBE, CUBE,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                    "depth") ||
        !image_make(&v, &checks, VK_FORMAT_R8G8B8A8_UNORM, CHECKS, CHECKS,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, "texture") ||
        !buffer_make(&v, &readback, CUBE * CUBE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     "buffer") ||
        !buffer_make(&v, &mesh, sizeof(vertices) + sizeof(indices),
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     "mesh") ||
        !buffer_make(&v, &upload, CHECKS * CHECKS * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     "upload")) {
        goto done;
    }

    memcpy(mesh.bytes, vertices, sizeof(vertices));
    memcpy((uint8_t *)mesh.bytes + sizeof(vertices), indices, sizeof(indices));
    for (int y = 0; y < CHECKS; y++) {
        for (int x = 0; x < CHECKS; x++) {
            uint8_t *p = (uint8_t *)upload.bytes + (y * CHECKS + x) * 4;
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
    STEP(v.vkCreateSampler(v.device, &sci, NULL, &sampler), "sampler");

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
    STEP(v.vkCreateDescriptorSetLayout(v.device, &dslci, NULL, &set_layout), "set-layout");
    VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    STEP(v.vkCreateDescriptorPool(v.device, &dpci, NULL, &descriptors), "descriptor-pool");
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptors,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout,
    };
    STEP(v.vkAllocateDescriptorSets(v.device, &dsai, &set), "descriptor-set");
    VkDescriptorImageInfo texture = {
        .sampler = sampler,
        .imageView = checks.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &texture,
    };
    v.vkUpdateDescriptorSets(v.device, 1, &write, 0, NULL);

    VkPushConstantRange range = { VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(transform) };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    };
    STEP(v.vkCreatePipelineLayout(v.device, &plci, NULL, &layout), "pipeline-layout");

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
    if (!pipeline_make(&v, &pipeline, layout, cube_vert, sizeof(cube_vert), cube_frag,
                       sizeof(cube_frag), &input, CUBE, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_FORMAT_D32_SFLOAT) ||
        !begin(&v)) {
        goto done;
    }

    /* The texture, from the buffer the checks were written into. */
    barrier(&v, checks.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { CHECKS, CHECKS, 1 },
    };
    v.vkCmdCopyBufferToImage(v.cmd, upload.buffer, checks.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(&v, checks.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT);

    barrier(&v, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(&v, depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo colour_attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = target.view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } },
    };
    VkRenderingAttachmentInfo depth_attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = depth.view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue = { .depthStencil = { 1.0f, 0 } },
    };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { CUBE, CUBE } },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colour_attachment,
        .pDepthAttachment = &depth_attachment,
    };
    VkDeviceSize at = 0;
    v.vkCmdBeginRendering(v.cmd, &ri);
    v.vkCmdBindPipeline(v.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    v.vkCmdBindDescriptorSets(v.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0,
                              NULL);
    v.vkCmdPushConstants(v.cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(transform),
                         transform);
    v.vkCmdBindVertexBuffers(v.cmd, 0, 1, &mesh.buffer, &at);
    v.vkCmdBindIndexBuffer(v.cmd, mesh.buffer, sizeof(vertices), VK_INDEX_TYPE_UINT16);
    v.vkCmdDrawIndexed(v.cmd, 36, 1, 0, 0, 0);
    v.vkCmdEndRendering(v.cmd);
    copy_out(&v, target.image, CUBE, readback.buffer);
    if (!submit(&v)) {
        goto done;
    }

    say_picture(readback.bytes, CUBE, colours, "ab", 2);

done:
    if (v.device != VK_NULL_HANDLE) {
        if (pipeline != VK_NULL_HANDLE) {
            v.vkDestroyPipeline(v.device, pipeline, NULL);
        }
        if (layout != VK_NULL_HANDLE) {
            v.vkDestroyPipelineLayout(v.device, layout, NULL);
        }
        if (descriptors != VK_NULL_HANDLE) {
            v.vkDestroyDescriptorPool(v.device, descriptors, NULL);
        }
        if (set_layout != VK_NULL_HANDLE) {
            v.vkDestroyDescriptorSetLayout(v.device, set_layout, NULL);
        }
        if (sampler != VK_NULL_HANDLE) {
            v.vkDestroySampler(v.device, sampler, NULL);
        }
        buffer_free(&v, &upload);
        buffer_free(&v, &mesh);
        buffer_free(&v, &readback);
        image_free(&v, &checks);
        image_free(&v, &depth);
        image_free(&v, &target);
    }
    vk_close(&v);
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

void kosmos_vulkantest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_triangle);
    lua_setfield(L, -2, "triangle");
    lua_pushcfunction(L, l_cube);
    lua_setfield(L, -2, "cube");
    lua_pushinteger(L, TRIANGLE);
    lua_setfield(L, -2, "triangle_size");
    lua_pushinteger(L, CUBE);
    lua_setfield(L, -2, "cube_size");
}
