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

int kvk_open(struct kvk *v, const char *program, int flags)
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
    if (flags & KVK_WINDOWED) {
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
    {
        PFN_vkGetPhysicalDeviceFormatProperties formats =
            (PFN_vkGetPhysicalDeviceFormatProperties)gipa(v->instance,
                                                          "vkGetPhysicalDeviceFormatProperties");
        VkFormatProperties fp = { 0 };
        if (formats) {
            formats(v->physical, VK_FORMAT_D32_SFLOAT, &fp);
        }
        v->depth_filters = (fp.optimalTilingFeatures &
                            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    }
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
    VkPhysicalDeviceVulkan12Features v12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    };
    VkPhysicalDeviceVulkan11Features v11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
    };
    VkPhysicalDeviceFeatures core = { 0 };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &v13,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    const char *extensions[2];
    uint32_t nextensions = 0;

    if (flags & KVK_WINDOWED) {
        extensions[nextensions++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    }
    /* **Compute on tensors** (`docs/xavier.md` X12): what llama.cpp's
     * shaders are written against - 8- and 16-bit values in storage
     * buffers, half floats and small integers in arithmetic, a subgroup's
     * size chosen - asked of the device and turned on where it has them,
     * and push descriptors, so a dispatch names its buffers as it is
     * recorded rather than through a set made for it. What the device
     * lacks is left off, and `v->tensors` says what it has. */
    if (flags & KVK_COMPUTE) {
        PFN_vkGetPhysicalDeviceFeatures2 features2 =
            (PFN_vkGetPhysicalDeviceFeatures2)gipa(v->instance, "vkGetPhysicalDeviceFeatures2");
        PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions =
            (PFN_vkEnumerateDeviceExtensionProperties)gipa(v->instance,
                                                           "vkEnumerateDeviceExtensionProperties");
        VkPhysicalDeviceVulkan13Features h13 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        };
        VkPhysicalDeviceVulkan12Features h12 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &h13,
        };
        VkPhysicalDeviceVulkan11Features h11 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &h12,
        };
        VkPhysicalDeviceFeatures2 has = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &h11,
        };

        if (features2 == NULL) {
            refused(v, "no-features2");
            goto done;
        }
        features2(v->physical, &has);
        v11.storageBuffer16BitAccess = h11.storageBuffer16BitAccess;
        v11.uniformAndStorageBuffer16BitAccess = h11.uniformAndStorageBuffer16BitAccess;
        v12.storageBuffer8BitAccess = h12.storageBuffer8BitAccess;
        v12.uniformAndStorageBuffer8BitAccess = h12.uniformAndStorageBuffer8BitAccess;
        v12.shaderFloat16 = h12.shaderFloat16;
        v12.shaderInt8 = h12.shaderInt8;
        v13.subgroupSizeControl = h13.subgroupSizeControl;
        v13.computeFullSubgroups = h13.computeFullSubgroups;
        core.shaderInt16 = has.features.shaderInt16;
        core.shaderInt64 = has.features.shaderInt64;
        v->tensors = (h11.storageBuffer16BitAccess ? KVK_HAS_STORAGE16 : 0) |
                     (h12.storageBuffer8BitAccess ? KVK_HAS_STORAGE8 : 0) |
                     (h12.shaderFloat16 ? KVK_HAS_FLOAT16 : 0) |
                     (h12.shaderInt8 ? KVK_HAS_INT8 : 0) |
                     (has.features.shaderInt16 ? KVK_HAS_INT16 : 0) |
                     (h13.subgroupSizeControl ? KVK_HAS_SUBGROUP_SIZE : 0);
        v13.pNext = &v12;
        v12.pNext = &v11;
        dci.pEnabledFeatures = &core;

        VkExtensionProperties all[256];
        uint32_t nall = 256;
        int push = 0;
        if (enumerate_extensions != NULL &&
            enumerate_extensions(v->physical, NULL, &nall, all) >= VK_SUCCESS) {
            for (uint32_t i = 0; i < nall; i++) {
                push |= strcmp(all[i].extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0;
            }
        }
        if (!push) {
            refused(v, "no-push-descriptor");
            goto done;
        }
        extensions[nextensions++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    }
    dci.enabledExtensionCount = nextensions;
    dci.ppEnabledExtensionNames = nextensions > 0 ? extensions : NULL;
    KVK_STEP(v, create_device(v->physical, &dci, NULL, &v->device), "device");

#define KVK_LOOK_UP(name) v->name = (PFN_##name)gdpa(v->device, #name);
    KVK_FUNCTIONS(KVK_LOOK_UP)
#undef KVK_LOOK_UP
    v->vkCmdPushDescriptorSetKHR =
        (PFN_vkCmdPushDescriptorSetKHR)gdpa(v->device, "vkCmdPushDescriptorSetKHR");
    PFN_vkGetDeviceQueue get_queue = (PFN_vkGetDeviceQueue)gdpa(v->device, "vkGetDeviceQueue");
    get_queue(v->device, 0, 0, &v->queue);

    /* The queue's clock: how many bits of a stamp it keeps (none, and it
     * keeps no time) and how long a tick is. */
    {
        PFN_vkGetPhysicalDeviceQueueFamilyProperties families =
            (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(
                v->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        VkQueueFamilyProperties family;
        uint32_t nfamilies = 1;

        if (families != NULL) {
            families(v->physical, &nfamilies, &family);
            v->stamp_bits = nfamilies > 0 ? family.timestampValidBits : 0;
        }
        v->stamp_ns = props.limits.timestampPeriod;
        if (v->stamp_bits > 0 && v->stamp_ns > 0) {
            VkQueryPoolCreateInfo qpci = {
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = 2,
            };
            if (v->vkCreateQueryPool(v->device, &qpci, NULL, &v->stamps) != VK_SUCCESS) {
                v->stamps = VK_NULL_HANDLE;
            }
        }
    }

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
        if (v->stamps != VK_NULL_HANDLE) {
            v->vkDestroyQueryPool(v->device, v->stamps, NULL);
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

int kvk_time_start(struct kvk *v)
{
    if (v->stamps != VK_NULL_HANDLE) {
        v->vkCmdResetQueryPool(v->cmd, v->stamps, 0, 2);
        v->vkCmdWriteTimestamp(v->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, v->stamps, 0);
    }
    return 1;
}

void kvk_time_end(struct kvk *v)
{
    if (v->stamps != VK_NULL_HANDLE) {
        v->vkCmdWriteTimestamp(v->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, v->stamps, 1);
    }
}

double kvk_time_ms(struct kvk *v)
{
    uint64_t at[2];
    uint64_t mask = v->stamp_bits >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << v->stamp_bits) - 1;

    if (v->stamps == VK_NULL_HANDLE ||
        v->vkGetQueryPoolResults(v->device, v->stamps, 0, 2, sizeof(at), at, sizeof(at[0]),
                                 VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return -1;
    }
    return (double)((at[1] - at[0]) & mask) * v->stamp_ns / 1e6;
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
        .depthBiasEnable = shape->bias_constant != 0.0f || shape->bias_slope != 0.0f,
        .depthBiasConstantFactor = shape->bias_constant,
        .depthBiasSlopeFactor = shape->bias_slope,
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
    /* No colour attachment at all when the picture is depth alone. */
    uint32_t colours = format != VK_FORMAT_UNDEFINED ? 1 : 0;
    VkPipelineColorBlendStateCreateInfo blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = colours, .pAttachments = &blend,
    };
    VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = colours,
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

/* --- Compute (`docs/xavier.md` X12) --------------------------------------- */

int kvk_compute_make(struct kvk *v, struct kvk_compute *c, const uint32_t *spirv,
                     size_t spirv_bytes, uint32_t buffers, uint32_t push_bytes,
                     const uint32_t *spec, uint32_t nspec, const char *what)
{
    VkDescriptorSetLayoutBinding binding[KVK_COMPUTE_BUFFERS];
    VkSpecializationMapEntry entry[16];
    VkShaderModule module = VK_NULL_HANDLE;

    memset(c, 0, sizeof(*c));
    if (buffers > KVK_COMPUTE_BUFFERS || nspec > 16 || v->vkCmdPushDescriptorSetKHR == NULL) {
        refused(v, what);
        return 0;
    }
    c->buffers = buffers;
    c->push_bytes = push_bytes;
    for (uint32_t i = 0; i < buffers; i++) {
        binding[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        };
    }
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
        .bindingCount = buffers,
        .pBindings = binding,
    };
    KVK_STEP(v, v->vkCreateDescriptorSetLayout(v->device, &dslci, NULL, &c->set_layout), what);
    VkPushConstantRange range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .size = push_bytes,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &c->set_layout,
        .pushConstantRangeCount = push_bytes > 0,
        .pPushConstantRanges = &range,
    };
    KVK_STEP(v, v->vkCreatePipelineLayout(v->device, &plci, NULL, &c->layout), what);

    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spirv_bytes,
        .pCode = spirv,
    };
    KVK_STEP(v, v->vkCreateShaderModule(v->device, &smci, NULL, &module), what);
    for (uint32_t i = 0; i < nspec; i++) {
        entry[i] = (VkSpecializationMapEntry){ .constantID = i, .offset = 4 * i, .size = 4 };
    }
    VkSpecializationInfo si = {
        .mapEntryCount = nspec,
        .pMapEntries = entry,
        .dataSize = 4 * nspec,
        .pData = spec,
    };
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main",
            .pSpecializationInfo = nspec > 0 ? &si : NULL,
        },
        .layout = c->layout,
    };
    KVK_STEP(v, v->vkCreateComputePipelines(v->device, VK_NULL_HANDLE, 1, &cpci, NULL,
                                            &c->pipeline), what);
    v->vkDestroyShaderModule(v->device, module, NULL);
    return 1;

done:
    if (module != VK_NULL_HANDLE) {
        v->vkDestroyShaderModule(v->device, module, NULL);
    }
    kvk_compute_free(v, c);
    return 0;
}

void kvk_compute_free(struct kvk *v, struct kvk_compute *c)
{
    if (c->pipeline != VK_NULL_HANDLE) {
        v->vkDestroyPipeline(v->device, c->pipeline, NULL);
    }
    if (c->layout != VK_NULL_HANDLE) {
        v->vkDestroyPipelineLayout(v->device, c->layout, NULL);
    }
    if (c->set_layout != VK_NULL_HANDLE) {
        v->vkDestroyDescriptorSetLayout(v->device, c->set_layout, NULL);
    }
    memset(c, 0, sizeof(*c));
}

void kvk_compute_dispatch(struct kvk *v, const struct kvk_compute *c,
                          const struct kvk_span *spans, const void *push,
                          uint32_t x, uint32_t y, uint32_t z)
{
    VkDescriptorBufferInfo info[KVK_COMPUTE_BUFFERS];
    VkWriteDescriptorSet write[KVK_COMPUTE_BUFFERS];

    for (uint32_t i = 0; i < c->buffers; i++) {
        info[i] = (VkDescriptorBufferInfo){
            .buffer = spans[i].buffer,
            .offset = spans[i].offset,
            .range = spans[i].range != 0 ? spans[i].range : VK_WHOLE_SIZE,
        };
        write[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &info[i],
        };
    }
    v->vkCmdBindPipeline(v->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipeline);
    v->vkCmdPushDescriptorSetKHR(v->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->layout, 0,
                                 c->buffers, write);
    if (c->push_bytes > 0) {
        v->vkCmdPushConstants(v->cmd, c->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, c->push_bytes,
                              push);
    }
    v->vkCmdDispatch(v->cmd, x, y, z);
}

/* One global barrier rather than one a buffer: llama.cpp's backend does
 * the same between the nodes of a graph, and a token's steps each read
 * what the step before wrote, so there is nothing narrower to say. */
void kvk_compute_barrier(struct kvk *v)
{
    VkMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                         VK_ACCESS_HOST_READ_BIT,
    };

    v->vkCmdPipelineBarrier(v->cmd,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
                            1, &b, 0, NULL, 0, NULL);
}

int kvk_buffer_make_storage(struct kvk *v, struct kvk_buffer *b, VkDeviceSize size,
                            const char *what)
{
    VkMemoryRequirements need;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    const VkMemoryPropertyFlags mapped =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type;

    memset(b, 0, sizeof(*b));
    KVK_STEP(v, v->vkCreateBuffer(v->device, &bci, NULL, &b->buffer), what);
    v->vkGetBufferMemoryRequirements(v->device, b->buffer, &need);
    /* Cached and the device's own first (an Intel GPU's, sharing the
     * processor's last-level cache), then cached, then any mapped. */
    type = kvk_memory_type(v, need.memoryTypeBits,
                           mapped | VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        type = kvk_memory_type(v, need.memoryTypeBits, mapped | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    }
    if (type == UINT32_MAX) {
        type = kvk_memory_type(v, need.memoryTypeBits, mapped);
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = need.size,
        .memoryTypeIndex = type,
    };
    KVK_STEP(v, v->vkAllocateMemory(v->device, &mai, NULL, &b->memory), what);
    KVK_STEP(v, v->vkBindBufferMemory(v->device, b->buffer, b->memory, 0), what);
    KVK_STEP(v, v->vkMapMemory(v->device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->bytes), what);
    return 1;

done:
    kvk_buffer_free(v, b);
    return 0;
}
