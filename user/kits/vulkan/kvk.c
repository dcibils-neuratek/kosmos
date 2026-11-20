/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Vulkan Kit's base (`kvk.h`): the device, memory, recording, a
 * pipeline and a window's swapchain, once for every C program on Vulkan.
 */

#include <stdio.h>
#include <string.h>

#include "kvk.h"

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

void kvk_failed(struct kvk *v, const char *what, VkResult r)
{
    snprintf(v->why, sizeof(v->why), "%s/%d", what, (int)r);
}

static void refused(struct kvk *v, const char *what)
{
    snprintf(v->why, sizeof(v->why), "%s", what);
}

uint32_t kvk_memory_type(const struct kvk *v, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < v->memory.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (v->memory.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

int kvk_open(struct kvk *v, const char *program, int windowed)
{
    PFN_vkGetInstanceProcAddr gipa = vk_icdGetInstanceProcAddr;
    PFN_vkCreateInstance create_instance =
        (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    uint32_t count = 1;

    memset(v, 0, sizeof(*v));
    if (create_instance == NULL) {
        refused(v, "no-vkCreateInstance");
        return 0;
    }

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = program,
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
    KVK_STEP(v, create_instance(&ici, NULL, &v->instance), "instance");

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

    KVK_STEP(v, enumerate(v->instance, &count, &v->physical), "physical-device");
    if (count == 0) {
        refused(v, "no-device");
        goto done;
    }

    VkPhysicalDeviceProperties props;
    properties(v->physical, &props);
    memory(v->physical, &v->memory);
    /* "llvmpipe (LLVM 23.1.2, 256 bits)": the name to its first space. */
    {
        char name[48];
        size_t n = strcspn(props.deviceName, " ");
        if (n >= sizeof(name)) {
            n = sizeof(name) - 1;
        }
        memcpy(name, props.deviceName, n);
        name[n] = '\0';
        snprintf(v->device_name, sizeof(v->device_name), "%s/%u.%u", name,
                 VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
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
    KVK_STEP(v, create_device(v->physical, &dci, NULL, &v->device), "device");

#define KVK_LOOK_UP(name) v->name = (PFN_##name)gdpa(v->device, #name);
    KVK_FUNCTIONS(KVK_LOOK_UP)
#undef KVK_LOOK_UP
    PFN_vkGetDeviceQueue get_queue = (PFN_vkGetDeviceQueue)gdpa(v->device, "vkGetDeviceQueue");
    get_queue(v->device, 0, 0, &v->queue);

    /* Recorded again after each submit - an upload, then a frame, then the
     * next frame - which Vulkan allows only of a buffer whose pool says so;
     * without it lavapipe freed a buffer twice. */
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = 0,
    };
    KVK_STEP(v, v->vkCreateCommandPool(v->device, &cpci, NULL, &v->pool), "command-pool");
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = v->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    KVK_STEP(v, v->vkAllocateCommandBuffers(v->device, &cbai, &v->cmd), "command-buffer");
    return 1;

done:
    return 0;
}

void kvk_close(struct kvk *v)
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
    v->device = VK_NULL_HANDLE;
    v->instance = VK_NULL_HANDLE;
}

int kvk_buffer_make(struct kvk *v, struct kvk_buffer *b, VkDeviceSize size,
                    VkBufferUsageFlags usage, const char *what)
{
    VkMemoryRequirements need;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
    };

    memset(b, 0, sizeof(*b));
    KVK_STEP(v, v->vkCreateBuffer(v->device, &bci, NULL, &b->buffer), what);
    v->vkGetBufferMemoryRequirements(v->device, b->buffer, &need);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = need.size,
        .memoryTypeIndex = kvk_memory_type(v, need.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    KVK_STEP(v, v->vkAllocateMemory(v->device, &mai, NULL, &b->memory), what);
    KVK_STEP(v, v->vkBindBufferMemory(v->device, b->buffer, b->memory, 0), what);
    KVK_STEP(v, v->vkMapMemory(v->device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->bytes), what);
    return 1;

done:
    return 0;
}

void kvk_buffer_free(struct kvk *v, struct kvk_buffer *b)
{
    if (b->buffer != VK_NULL_HANDLE) {
        v->vkDestroyBuffer(v->device, b->buffer, NULL);
    }
    if (b->memory != VK_NULL_HANDLE) {
        v->vkFreeMemory(v->device, b->memory, NULL);
    }
    memset(b, 0, sizeof(*b));
}

int kvk_image_make(struct kvk *v, struct kvk_image *i, VkFormat format, uint32_t width,
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
    KVK_STEP(v, v->vkCreateImage(v->device, &imci, NULL, &i->image), what);
    v->vkGetImageMemoryRequirements(v->device, i->image, &need);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = need.size,
        .memoryTypeIndex = kvk_memory_type(v, need.memoryTypeBits, 0),
    };
    KVK_STEP(v, v->vkAllocateMemory(v->device, &mai, NULL, &i->memory), what);
    KVK_STEP(v, v->vkBindImageMemory(v->device, i->image, i->memory, 0), what);
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = i->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = { aspect, 0, 1, 0, 1 },
    };
    KVK_STEP(v, v->vkCreateImageView(v->device, &vci, NULL, &i->view), what);
    return 1;

done:
    return 0;
}

void kvk_image_free(struct kvk *v, struct kvk_image *i)
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
    memset(i, 0, sizeof(*i));
}

void kvk_barrier(struct kvk *v, VkImage image, VkImageAspectFlags aspect,
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

int kvk_submit(struct kvk *v)
{
    VkSubmitInfo s = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &v->cmd,
    };

    KVK_STEP(v, v->vkEndCommandBuffer(v->cmd), "end");
    KVK_STEP(v, v->vkQueueSubmit(v->queue, 1, &s, VK_NULL_HANDLE), "submit");
    KVK_STEP(v, v->vkQueueWaitIdle(v->queue), "wait");
    return 1;

done:
    return 0;
}

int kvk_begin(struct kvk *v)
{
    VkCommandBufferBeginInfo b = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    KVK_STEP(v, v->vkBeginCommandBuffer(v->cmd, &b), "begin");
    return 1;

done:
    return 0;
}

void kvk_copy_out(struct kvk *v, VkImage image, uint32_t width, uint32_t height, VkBuffer out)
{
    kvk_barrier(v, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { width, height, 1 },
    };
    v->vkCmdCopyImageToBuffer(v->cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out, 1,
                              &region);
}

void kvk_image_upload(struct kvk *v, VkImage image, uint32_t width, uint32_t height,
                      VkBuffer from)
{
    kvk_barrier(v, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { width, height, 1 },
    };
    v->vkCmdCopyBufferToImage(v->cmd, from, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                              &region);
    kvk_barrier(v, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT);
}

int kvk_sampled_layout(struct kvk *v, VkDescriptorSetLayout *out)
{
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

    KVK_STEP(v, v->vkCreateDescriptorSetLayout(v->device, &dslci, NULL, out), "set-layout");
    return 1;

done:
    return 0;
}

int kvk_sampled_set(struct kvk *v, VkDescriptorSetLayout layout, VkSampler sampler,
                    VkImageView view, VkDescriptorPool *pool, VkDescriptorSet *set)
{
    VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &size,
    };

    KVK_STEP(v, v->vkCreateDescriptorPool(v->device, &dpci, NULL, pool), "descriptor-pool");
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = *pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout,
    };
    KVK_STEP(v, v->vkAllocateDescriptorSets(v->device, &dsai, set), "descriptor-set");
    VkDescriptorImageInfo image = {
        .sampler = sampler,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = *set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image,
    };
    v->vkUpdateDescriptorSets(v->device, 1, &write, 0, NULL);
    return 1;

done:
    return 0;
}

int kvk_pipeline(struct kvk *v, VkPipeline *out, VkPipelineLayout layout,
                 const struct kvk_pipeline_shape *shape)
{
    VkShaderModule modules[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shape->vert_bytes,
        .pCode = shape->vert,
    };
    VkFormat format = shape->format;
    int made = 0;

    KVK_STEP(v, v->vkCreateShaderModule(v->device, &smci, NULL, &modules[0]), "vertex-shader");
    smci.codeSize = shape->frag_bytes;
    smci.pCode = shape->frag;
    KVK_STEP(v, v->vkCreateShaderModule(v->device, &smci, NULL, &modules[1]), "fragment-shader");

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
    VkViewport viewport = { 0, 0, (float)shape->width, (float)shape->height, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, { shape->width, shape->height } };
    VkPipelineViewportStateCreateInfo viewports = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport,
        .scissorCount = 1, .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = shape->cull,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineDepthStencilStateCreateInfo depths = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = shape->depth != VK_FORMAT_UNDEFINED && !shape->depth_off,
        .depthWriteEnable = shape->depth != VK_FORMAT_UNDEFINED && !shape->depth_off,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };
    /* Blended, the classic "over": the source by its alpha, what is
     * there by the rest of it. */
    VkPipelineColorBlendAttachmentState blend = {
        .blendEnable = shape->blend ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
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
        .depthAttachmentFormat = shape->depth,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = shape->input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewports,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depths,
        .pColorBlendState = &blending,
        .layout = layout,
    };
    KVK_STEP(v, v->vkCreateGraphicsPipelines(v->device, VK_NULL_HANDLE, 1, &gpci, NULL, out),
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

int kvk_swapchain_open(struct kvk *v, struct kvk_swapchain *s, struct kw_window *window)
{
    PFN_vkGetInstanceProcAddr gipa = vk_icdGetInstanceProcAddr;
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[8];
    uint32_t nformats = 8;

    memset(s, 0, sizeof(*s));
    s->format = VK_FORMAT_UNDEFINED;

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
    s->destroy_surface = (PFN_vkDestroySurfaceKHR)gipa(v->instance, "vkDestroySurfaceKHR");
    if (create_surface == NULL || get_caps == NULL || get_formats == NULL ||
        get_support == NULL || s->destroy_surface == NULL) {
        refused(v, "no-surface-functions");
        goto done;
    }

    VkKosmosSurfaceCreateInfoKOSMOS sci = {
        .sType = VK_STRUCTURE_TYPE_KOSMOS_SURFACE_CREATE_INFO_KOSMOS,
        .window = window,
    };
    KVK_STEP(v, create_surface(v->instance, &sci, NULL, &s->surface), "surface");

    VkBool32 supported = VK_FALSE;
    KVK_STEP(v, get_support(v->physical, 0, s->surface, &supported), "support");
    if (!supported) {
        refused(v, "queue-cannot-present");
        goto done;
    }
    KVK_STEP(v, get_caps(v->physical, s->surface, &caps), "capabilities");
    KVK_STEP(v, get_formats(v->physical, s->surface, &nformats, formats), "formats");
    s->extent = caps.currentExtent;
    for (uint32_t i = 0; i < nformats; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            s->format = formats[i].format;
        }
    }
    if (s->format == VK_FORMAT_UNDEFINED) {
        refused(v, "no-B8G8R8A8_UNORM");
        goto done;
    }

    VkSwapchainCreateInfoKHR swci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s->surface,
        .minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount,
        .imageFormat = s->format,
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
    KVK_STEP(v, v->vkCreateSwapchainKHR(v->device, &swci, NULL, &s->swapchain), "swapchain");
    s->count = KVK_SWAPCHAIN_MAX;
    KVK_STEP(v, v->vkGetSwapchainImagesKHR(v->device, s->swapchain, &s->count, s->images),
             "swapchain-images");
    for (uint32_t i = 0; i < s->count; i++) {
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = s->images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = s->format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        KVK_STEP(v, v->vkCreateImageView(v->device, &vci, NULL, &s->views[i]), "swapchain-view");
    }

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    KVK_STEP(v, v->vkCreateFence(v->device, &fci, NULL, &s->fence), "fence");
    return 1;

done:
    return 0;
}

void kvk_swapchain_close(struct kvk *v, struct kvk_swapchain *s)
{
    if (v->device != VK_NULL_HANDLE) {
        for (uint32_t i = 0; i < s->count; i++) {
            if (s->views[i] != VK_NULL_HANDLE) {
                v->vkDestroyImageView(v->device, s->views[i], NULL);
            }
        }
        if (s->fence != VK_NULL_HANDLE) {
            v->vkDestroyFence(v->device, s->fence, NULL);
        }
        if (s->swapchain != VK_NULL_HANDLE) {
            v->vkDestroySwapchainKHR(v->device, s->swapchain, NULL);
        }
    }
    if (s->surface != VK_NULL_HANDLE && s->destroy_surface != NULL) {
        s->destroy_surface(v->instance, s->surface, NULL);
    }
    memset(s, 0, sizeof(*s));
}

int kvk_acquire(struct kvk *v, struct kvk_swapchain *s)
{
    uint32_t index;

    KVK_STEP(v, v->vkAcquireNextImageKHR(v->device, s->swapchain, UINT64_MAX, VK_NULL_HANDLE,
                                         s->fence, &index), "acquire");
    KVK_STEP(v, v->vkWaitForFences(v->device, 1, &s->fence, VK_TRUE, UINT64_MAX), "acquired");
    KVK_STEP(v, v->vkResetFences(v->device, 1, &s->fence), "fence-reset");
    return (int)index;

done:
    return -1;
}

int kvk_present(struct kvk *v, struct kvk_swapchain *s, uint32_t index)
{
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1,
        .pSwapchains = &s->swapchain,
        .pImageIndices = &index,
    };

    KVK_STEP(v, v->vkQueuePresentKHR(v->queue, &pi), "present");
    return 1;

done:
    return 0;
}
