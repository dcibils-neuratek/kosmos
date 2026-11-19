/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Vulkan on Kosmos** (`docs/gpu3d.md` G2): a kit in no image but
 * `vulkantest.elf`, reached as `use("vulkantest.elf")`. `triangle(...)`
 * draws one triangle with `lavapipe` - Mesa's Vulkan driver, on llvmpipe
 * and LLVM's JIT - into an image in memory and reads it back:
 *
 *     triangle(x0, y0, x1, y1, x2, y2, r, g, b)
 *
 * the corners in pixels of a `SIZE` by `SIZE` picture and the colour in
 * bytes, all the caller's, so nothing here knows the answer. What it says
 * is the device's name and the picture as a mask, a row at a time: `#` the
 * triangle's colour exactly, `.` the clear colour exactly, `?` anything
 * else - and the caller, who chose the corners, decides which pixels had
 * to be which.
 *
 * There is no Vulkan loader: nothing on Kosmos is loaded, and `lavapipe`
 * is linked in. Its `vk_icdGetInstanceProcAddr` is the one door, as a
 * loader would use it. A step that fails is named in the answer.
 */

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

#define SIZE 32

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

/* The console, so that what Mesa or LLVM says - an abort's last words
 * among it - is heard rather than spilled (`runtime/posix/fd.c`). */
void posix_console(long capability);
long posix_console_capability(void);

/* The shaders' push constants, as `triangle.vert` declares them (std430). */
struct triangle {
    float colour[4];
    float corner[3][2];
    float size[2];
};

static char said[SIZE * (SIZE + 1) + 512];

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

#define INSTANCE_FN(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
#define DEVICE_FN(name) PFN_##name name = (PFN_##name)gdpa(device, #name)

static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties *p, uint32_t bits,
                            VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < p->memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (p->memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

static void triangle(const struct triangle *t, const uint8_t colour[3])
{
    PFN_vkGetInstanceProcAddr gipa = vk_icdGetInstanceProcAddr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical;
    uint32_t count = 1;

    PFN_vkCreateInstance vkCreateInstance =
        (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");

    said[0] = '\0';
    say("vulkan");

    if (vkCreateInstance == NULL) {
        say(" failed=no-vkCreateInstance");
        return;
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
    STEP(vkCreateInstance(&ici, NULL, &instance), "instance");

    INSTANCE_FN(vkEnumeratePhysicalDevices);
    INSTANCE_FN(vkGetPhysicalDeviceProperties);
    INSTANCE_FN(vkGetPhysicalDeviceMemoryProperties);
    INSTANCE_FN(vkCreateDevice);
    INSTANCE_FN(vkGetDeviceProcAddr);
    PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;

    STEP(vkEnumeratePhysicalDevices(instance, &count, &physical), "physical-device");
    if (count == 0) {
        say(" failed=no-device");
        goto done;
    }

    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceProperties(physical, &props);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
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
    STEP(vkCreateDevice(physical, &dci, NULL, &device), "device");

    DEVICE_FN(vkGetDeviceQueue);
    DEVICE_FN(vkCreateImage);
    DEVICE_FN(vkDestroyImage);
    DEVICE_FN(vkCreateImageView);
    DEVICE_FN(vkDestroyImageView);
    DEVICE_FN(vkCreateBuffer);
    DEVICE_FN(vkDestroyBuffer);
    DEVICE_FN(vkGetImageMemoryRequirements);
    DEVICE_FN(vkGetBufferMemoryRequirements);
    DEVICE_FN(vkAllocateMemory);
    DEVICE_FN(vkFreeMemory);
    DEVICE_FN(vkBindImageMemory);
    DEVICE_FN(vkBindBufferMemory);
    DEVICE_FN(vkMapMemory);
    DEVICE_FN(vkCreateShaderModule);
    DEVICE_FN(vkDestroyShaderModule);
    DEVICE_FN(vkCreatePipelineLayout);
    DEVICE_FN(vkDestroyPipelineLayout);
    DEVICE_FN(vkCreateGraphicsPipelines);
    DEVICE_FN(vkDestroyPipeline);
    DEVICE_FN(vkCreateCommandPool);
    DEVICE_FN(vkDestroyCommandPool);
    DEVICE_FN(vkAllocateCommandBuffers);
    DEVICE_FN(vkBeginCommandBuffer);
    DEVICE_FN(vkEndCommandBuffer);
    DEVICE_FN(vkCmdPipelineBarrier);
    DEVICE_FN(vkCmdBeginRendering);
    DEVICE_FN(vkCmdEndRendering);
    DEVICE_FN(vkCmdBindPipeline);
    DEVICE_FN(vkCmdPushConstants);
    DEVICE_FN(vkCmdDraw);
    DEVICE_FN(vkCmdCopyImageToBuffer);
    DEVICE_FN(vkQueueSubmit);
    DEVICE_FN(vkQueueWaitIdle);

    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);

    /* The picture, and a buffer the processor can read it back from. */
    VkImage image = VK_NULL_HANDLE;
    VkImageCreateInfo imci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { SIZE, SIZE, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    STEP(vkCreateImage(device, &imci, NULL, &image), "image");

    VkMemoryRequirements need;
    VkDeviceMemory image_memory, buffer_memory;
    vkGetImageMemoryRequirements(device, image, &need);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = need.size,
        .memoryTypeIndex = memory_type(&memory, need.memoryTypeBits, 0),
    };
    STEP(vkAllocateMemory(device, &mai, NULL, &image_memory), "image-memory");
    STEP(vkBindImageMemory(device, image, image_memory, 0), "bind-image");

    VkImageView view;
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    STEP(vkCreateImageView(device, &vci, NULL, &view), "image-view");

    VkBuffer buffer;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = SIZE * SIZE * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    STEP(vkCreateBuffer(device, &bci, NULL, &buffer), "buffer");
    vkGetBufferMemoryRequirements(device, buffer, &need);
    mai.allocationSize = need.size;
    mai.memoryTypeIndex = memory_type(&memory, need.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    STEP(vkAllocateMemory(device, &mai, NULL, &buffer_memory), "buffer-memory");
    STEP(vkBindBufferMemory(device, buffer, buffer_memory, 0), "bind-buffer");

    /* The pipeline: two shaders, no vertex input, the colour pushed. */
    VkShaderModule vert, frag;
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(triangle_vert),
        .pCode = triangle_vert,
    };
    STEP(vkCreateShaderModule(device, &smci, NULL, &vert), "vertex-shader");
    smci.codeSize = sizeof(triangle_frag);
    smci.pCode = triangle_frag;
    STEP(vkCreateShaderModule(device, &smci, NULL, &frag), "fragment-shader");

    VkPushConstantRange range = {
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(*t)
    };
    VkPipelineLayout layout;
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    };
    STEP(vkCreatePipelineLayout(device, &plci, NULL, &layout), "pipeline-layout");

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport viewport = { 0, 0, SIZE, SIZE, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, { SIZE, SIZE } };
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
    VkPipelineColorBlendAttachmentState blend = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend,
    };
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &format,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewports,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blending,
        .layout = layout,
    };
    VkPipeline pipeline;
    STEP(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline),
         "pipeline");

    /* The commands: clear, draw, copy out. */
    VkCommandPool pool;
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0,
    };
    STEP(vkCreateCommandPool(device, &cpci, NULL, &pool), "command-pool");
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    STEP(vkAllocateCommandBuffers(device, &cbai, &cmd), "command-buffer");

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    STEP(vkBeginCommandBuffer(cmd, &begin), "begin");

    VkImageMemoryBarrier to_draw = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_draw);

    VkRenderingAttachmentInfo attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } },
    };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = scissor,
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    };
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdPushConstants(cmd, layout, range.stageFlags, 0, sizeof(*t), t);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    VkImageMemoryBarrier to_copy = to_draw;
    to_copy.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_copy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_copy.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_copy);

    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { SIZE, SIZE, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    STEP(vkEndCommandBuffer(cmd), "end");

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    STEP(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "submit");
    STEP(vkQueueWaitIdle(queue), "wait");

    /* The picture, as the caller reads it. */
    const uint8_t *pixels;
    STEP(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, (void **)&pixels), "map");
    say(" picture=");
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            const uint8_t *p = pixels + (y * SIZE + x) * 4;
            char c = '?';
            if (p[0] == colour[0] && p[1] == colour[1] && p[2] == colour[2] && p[3] == 255) {
                c = '#';
            } else if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 255) {
                c = '.';
            }
            say("%c", c);
        }
        say(y + 1 < SIZE ? "/" : "");
    }

    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyShaderModule(device, vert, NULL);
    vkDestroyShaderModule(device, frag, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, buffer_memory, NULL);
    vkDestroyImageView(device, view, NULL);
    vkDestroyImage(device, image, NULL);
    vkFreeMemory(device, image_memory, NULL);

done:
    if (device != VK_NULL_HANDLE) {
        PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)
            ((PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr"))(device, "vkDestroyDevice");
        destroy_device(device, NULL);
    }
    if (instance != VK_NULL_HANDLE) {
        ((PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance"))(instance, NULL);
    }
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
        lua_Integer c = luaL_checkinteger(L, 7 + i);
        luaL_argcheck(L, c >= 0 && c <= 255, 7 + i, "a colour is a byte");
        colour[i] = (uint8_t)c;
        t.colour[i] = (float)c / 255.0f;
    }
    t.colour[3] = 1.0f;
    t.size[0] = t.size[1] = SIZE;

    if (posix_console_capability() < 0) {
        posix_console(kit_capability(L, "/Devices/console"));
    }

    triangle(&t, colour);
    lua_pushstring(L, said);
    return 1;
}

void kosmos_vulkantest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_triangle);
    lua_setfield(L, -2, "triangle");
    lua_pushinteger(L, SIZE);
    lua_setfield(L, -2, "size");
}
