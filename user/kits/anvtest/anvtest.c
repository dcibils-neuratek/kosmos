/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **`anv` on Kosmos** (`docs/gpu3d.md` G7a): a kit in no image but
 * `anvtest.elf`, reached as `use("anvtest.elf")`, with Intel's Vulkan
 * driver linked in and nothing else of Vulkan's:
 *
 *     devices()
 *         the physical devices `anv` finds - by asking for `/Devices/gpu`,
 *         as it does on Kosmos - each as its name, its vendor and device
 *         ids, its driver's name and its memory types: what the program
 *         would be offered;
 *     compile(pci_id)
 *         Intel's compiler (`brw`) for the device whose PCI id that is,
 *         taken from Mesa's own table and not from any device: the compute
 *         shader in `store.comp`, compiled on the host to SPIR-V, through
 *         Mesa's SPIR-V front end and NIR to that generation's
 *         instructions - what it says is the generation, the SIMD width it
 *         chose, and the instructions, sends, spills and bytes.
 *
 * Under QEMU there is no Intel GPU, so `devices` must find none and still
 * answer, and `compile` is the whole of the compiler on a Kosmos process.
 * On the M700, `devices` names the HD 530. What the answers must be is
 * `tools/run_anv.py`'s to say.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kits/kitenv.h"

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "spirv_info.h"
#include "dev/intel_device_info.h"
#include "compiler/brw/brw_compiler.h"
#include "compiler/brw/brw_nir.h"
#include "util/ralloc.h"
#include "compiler/glsl_types.h"

#include <vulkan/vulkan.h>

#include "store_comp.h"
#include "write_comp.h"
#include <time.h>

void posix_console(long capability);
long posix_console_capability(void);

/* The console handed to the C library, so a panic inside Mesa - which
 * ends the process before the Lua side can drain the spill ring - is
 * heard, as vulkantest's is. */
static void hand_console(lua_State *L)
{
    if (posix_console_capability() < 0) {
        posix_console(kit_capability(L, "/Devices/console"));
    }
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

static char said[2048];
static size_t said_at;

static void say(const char *f, ...)
{
    va_list ap;

    if (said_at >= sizeof said - 1) {
        return;
    }
    va_start(ap, f);
    int n = vsnprintf(said + said_at, sizeof said - said_at, f, ap);
    va_end(ap);
    if (n > 0) {
        said_at += (size_t)n;
        if (said_at > sizeof said - 1) {
            said_at = sizeof said - 1;
        }
    }
}

/* --- devices() ---------------------------------------------------------- */

static void devices(void)
{
    PFN_vkGetInstanceProcAddr gipa = vk_icdGetInstanceProcAddr;
    PFN_vkCreateInstance create_instance =
        (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkInstance instance = VK_NULL_HANDLE;
    VkResult r;

    if (create_instance == NULL) {
        say("failed=no-vkCreateInstance");
        return;
    }

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "anvtest",
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    if ((r = create_instance(&ici, NULL, &instance)) != VK_SUCCESS) {
        say("failed=instance/%d", (int)r);
        return;
    }

    PFN_vkEnumeratePhysicalDevices enumerate =
        (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties2 properties =
        (PFN_vkGetPhysicalDeviceProperties2)gipa(instance, "vkGetPhysicalDeviceProperties2");
    PFN_vkGetPhysicalDeviceMemoryProperties memory =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(instance,
                                                      "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkDestroyInstance destroy =
        (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");

    uint32_t count = 0;
    VkPhysicalDevice found[4];

    if ((r = enumerate(instance, &count, NULL)) != VK_SUCCESS) {
        say("failed=enumerate/%d", (int)r);
        goto done;
    }
    say("devices=%u", count);
    if (count > 4) {
        count = 4;
    }
    if (count > 0 && (r = enumerate(instance, &count, found)) != VK_SUCCESS &&
        r != VK_INCOMPLETE) {
        say(" failed=enumerate/%d", (int)r);
        goto done;
    }

    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceDriverProperties driver = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
        };
        VkPhysicalDeviceProperties2 p = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &driver,
        };
        VkPhysicalDeviceMemoryProperties m;

        properties(found[i], &p);
        memory(found[i], &m);
        say(" [name=\"%s\" id=%04x:%04x driver=\"%s\" api=%u.%u memory=",
            p.properties.deviceName, p.properties.vendorID, p.properties.deviceID,
            driver.driverName, VK_API_VERSION_MAJOR(p.properties.apiVersion),
            VK_API_VERSION_MINOR(p.properties.apiVersion));
        for (uint32_t t = 0; t < m.memoryTypeCount; t++) {
            VkMemoryPropertyFlags f = m.memoryTypes[t].propertyFlags;
            say("%s%s%s%s%s", t ? "," : "",
                f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "L" : "",
                f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "V" : "",
                f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? "C" : "",
                f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "H" : "");
        }
        say(" heap=%lluMB]",
            (unsigned long long)(m.memoryHeapCount ? m.memoryHeaps[0].size >> 20 : 0));
    }

done:
    if (destroy != NULL) {
        destroy(instance, NULL);
    }
}

/* --- compile(pci_id) ---------------------------------------------------- */

/* What the compiler has to say about a shader: not kept. */
static void quiet(void *data, unsigned *id, const char *f, ...)
{
    (void)data;
    (void)id;
    (void)f;
}

/* The base workgroup is always nought: nothing dispatches here with one.
 * As `anv`'s internal kernels lower it. */
static bool lower_base_workgroup_id(nir_builder *b, nir_intrinsic_instr *intrin, void *data)
{
    (void)data;
    if (intrin->intrinsic != nir_intrinsic_load_base_workgroup_id) {
        return false;
    }
    b->cursor = nir_instr_remove(&intrin->instr);
    nir_def_rewrite_uses(&intrin->def, nir_imm_zero(b, 3, 32));
    return true;
}

static void compile(int pci_id)
{
    struct intel_device_info devinfo;
    void *mem = ralloc_context(NULL);

    /* NIR's types live in one table every compiler shares, which a
       driver makes when its instance is made and nothing here would. */
    glsl_type_singleton_init_or_ref();

    if (!intel_get_device_info_from_pci_id(pci_id, &devinfo)) {
        say("failed=no-device-info/%04x", pci_id);
        goto done;
    }

    struct brw_compiler *compiler = brw_compiler_create(mem, &devinfo);
    if (compiler == NULL) {
        say("failed=compiler");
        goto done;
    }
    compiler->shader_debug_log = quiet;
    compiler->shader_perf_log = quiet;

    const struct spirv_capabilities caps = {
        .Shader = true,
        .Int64 = true,
        .PhysicalStorageBufferAddresses = true,
    };
    const struct spirv_to_nir_options spirv = {
        .environment = NIR_SPIRV_VULKAN,
        .capabilities = &caps,
        .ubo_addr_format = nir_address_format_32bit_index_offset,
        .ssbo_addr_format = nir_address_format_64bit_global_32bit_offset,
        .phys_ssbo_addr_format = nir_address_format_64bit_global,
        .push_const_addr_format = nir_address_format_logical,
        .shared_addr_format = nir_address_format_32bit_offset,
    };

    nir_shader *nir = spirv_to_nir(store_comp, sizeof(store_comp) / 4, NULL,
                                   MESA_SHADER_COMPUTE, "main", &spirv,
                                   &compiler->nir_options[MESA_SHADER_COMPUTE]);
    if (nir == NULL) {
        say("failed=spirv");
        goto done;
    }
    ralloc_steal(mem, nir);

    /* What every Vulkan driver does first: one function, its variables in
     * SSA, and memory reached by address. */
    NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
    NIR_PASS(_, nir, nir_lower_returns);
    NIR_PASS(_, nir, nir_inline_functions);
    nir_remove_non_entrypoints(nir);
    NIR_PASS(_, nir, nir_opt_copy_prop);
    NIR_PASS(_, nir, nir_opt_deref);
    NIR_PASS(_, nir, nir_lower_vars_to_ssa);
    NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_global,
             nir_address_format_64bit_global);

    struct brw_nir_compiler_opts opts;
    memset(&opts, 0, sizeof opts);
    brw_preprocess_nir(compiler, nir, &opts);

    const nir_lower_compute_system_values_options sysvals = {
        .has_base_workgroup_id = true,
        .lower_cs_local_id_to_index = true,
    };
    NIR_PASS(_, nir, nir_lower_compute_system_values, &sysvals);
    NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_base_workgroup_id,
             nir_metadata_control_flow, NULL);

    nir->info.shared_size = 0;
    nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

    /* Gen9 has no subgroup id of its own: it is pushed, the first word of
     * the push constants, as BLORP pushes it. */
    struct brw_cs_prog_data *prog = rzalloc(mem, struct brw_cs_prog_data);
    prog->base.push_sizes[0] = 4;
    brw_cs_fill_push_const_info(&devinfo, prog, 0);
    prog->base.push_sizes[0] = 32;          /* one register */

    NIR_PASS(_, nir, brw_nir_lower_cs_intrinsics, &devinfo, prog);
    NIR_PASS(_, nir, brw_nir_lower_cs_subgroup_id, &devinfo, 0);

    struct brw_cs_prog_key key;
    memset(&key, 0, sizeof key);
    struct genisa_stats stats[3];
    memset(stats, 0, sizeof stats);

    struct brw_compile_cs_params params = {
        .base = {
            .mem_ctx = mem,
            .nir = nir,
            .key = &key.base,
            .prog_data = &prog->base,
            .stats = stats,
            .debug_flag = DEBUG_CS,
        },
    };
    const unsigned *program = brw_compile(compiler, &params.base);
    if (program == NULL) {
        say("failed=brw/%s", params.base.error_str ? params.base.error_str : "?");
        goto done;
    }

    say("gen=%d simd=%u instrs=%u sends=%u spills=%u fills=%u bytes=%u",
        devinfo.ver, stats[0].dispatch_width, stats[0].instrs, stats[0].sends,
        stats[0].spills, stats[0].fills, prog->base.program_size);

done:
    ralloc_free(mem);
    glsl_type_singleton_decref();
}

/* --- run(): G7b, on the GPU ---------------------------------------------- */

/*
 * A logical device on the first physical one, and two submissions read
 * back: `vkCmdFillBuffer` over the first half of a buffer, and
 * `write.comp` dispatched over the second, each waited for by a fence.
 * The buffer is host-visible, coherent memory, so what the GPU wrote is
 * what the processor reads. Said as `fill=... dispatch=...`, with how long
 * each took from submission to its fence.
 */
#define WORDS 16384u                    /* 64 KB; each half 8192 words */
#define FILLED 0x6b6f736du              /* "msok", little-endian */

static double now_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

/* What Mesa says as it refuses something - its `vk_errorf`'s sentence -
 * heard through `VK_EXT_debug_utils` and said with the answer. */
static VKAPI_ATTR VkBool32 VKAPI_CALL heard(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT types,
                                            const VkDebugUtilsMessengerCallbackDataEXT *data,
                                            void *user)
{
    (void)types;
    (void)user;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && data->pMessage != NULL) {
        say(" mesa=\"%s\"", data->pMessage);
    }
    return VK_FALSE;
}

static void run(void)
{
    PFN_vkGetInstanceProcAddr gipa = vk_icdGetInstanceProcAddr;
    PFN_vkCreateInstance create_instance =
        (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkResult r;

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "anvtest",
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkDebugUtilsMessengerCreateInfoEXT listen = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = heard,
    };
    const char *extensions[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = &listen,
        .pApplicationInfo = &app,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = extensions,
    };
    if ((r = create_instance(&ici, NULL, &instance)) != VK_SUCCESS) {
        say("failed=instance/%d", (int)r);
        return;
    }
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkCreateDebugUtilsMessengerEXT messenger_new =
        (PFN_vkCreateDebugUtilsMessengerEXT)gipa(instance, "vkCreateDebugUtilsMessengerEXT");
    PFN_vkDestroyDebugUtilsMessengerEXT messenger_end =
        (PFN_vkDestroyDebugUtilsMessengerEXT)gipa(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (messenger_new != NULL) {
        (void)messenger_new(instance, &listen, NULL, &messenger);
    }

#define I(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
    I(vkEnumeratePhysicalDevices);
    I(vkGetPhysicalDeviceMemoryProperties);
    I(vkCreateDevice);
    I(vkGetDeviceProcAddr);
    I(vkDestroyInstance);
#undef I

    uint32_t count = 1;
    VkPhysicalDevice phys;
    r = vkEnumeratePhysicalDevices(instance, &count, &phys);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
        say("failed=no-device");
        goto out;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkPhysicalDeviceVulkan12Features v12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .bufferDeviceAddress = VK_TRUE,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &v12,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    double t0 = now_us();
    if ((r = vkCreateDevice(phys, &dci, NULL, &dev)) != VK_SUCCESS) {
        say("failed=device/%d", (int)r);
        goto out;
    }
    say("device=%.0fus", now_us() - t0);

#define D(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    D(vkDestroyDevice);
    D(vkGetDeviceQueue);
    D(vkCreateBuffer);
    D(vkDestroyBuffer);
    D(vkGetBufferMemoryRequirements);
    D(vkAllocateMemory);
    D(vkFreeMemory);
    D(vkBindBufferMemory);
    D(vkMapMemory);
    D(vkGetBufferDeviceAddress);
    D(vkCreateCommandPool);
    D(vkDestroyCommandPool);
    D(vkAllocateCommandBuffers);
    D(vkBeginCommandBuffer);
    D(vkEndCommandBuffer);
    D(vkResetCommandBuffer);
    D(vkCmdFillBuffer);
    D(vkCmdBindPipeline);
    D(vkCmdPushConstants);
    D(vkCmdDispatch);
    D(vkQueueSubmit);
    D(vkCreateFence);
    D(vkDestroyFence);
    D(vkResetFences);
    D(vkWaitForFences);
    D(vkCreateShaderModule);
    D(vkDestroyShaderModule);
    D(vkCreatePipelineLayout);
    D(vkDestroyPipelineLayout);
    D(vkCreateComputePipelines);
    D(vkDestroyPipeline);
#undef D

    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);

    /* The buffer, in memory the processor sees as the GPU does. */
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = WORDS * 4u,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    VkBuffer buf;
    if ((r = vkCreateBuffer(dev, &bci, NULL, &buf)) != VK_SUCCESS) {
        say(" failed=buffer/%d", (int)r);
        goto device;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; t++) {
        if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) {
            type = t;
        }
    }
    VkMemoryAllocateFlagsInfo flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags,
        .allocationSize = req.size,
        .memoryTypeIndex = type,
    };
    VkDeviceMemory mem;
    uint32_t *words = NULL;
    if (type == UINT32_MAX || (r = vkAllocateMemory(dev, &mai, NULL, &mem)) != VK_SUCCESS) {
        say(" failed=memory/%d", (int)r);
        goto buffer;
    }
    if ((r = vkBindBufferMemory(dev, buf, mem, 0)) != VK_SUCCESS ||
        (r = vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&words)) != VK_SUCCESS) {
        say(" failed=map/%d", (int)r);
        goto memory;
    }
    memset(words, 0, WORDS * 4u);

    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = 0,
    };
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandBufferCount = 1,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    };
    if ((r = vkCreateCommandPool(dev, &pci, NULL, &pool)) != VK_SUCCESS) {
        say(" failed=pool/%d", (int)r);
        goto memory;
    }
    cai.commandPool = pool;
    if ((r = vkAllocateCommandBuffers(dev, &cai, &cmd)) != VK_SUCCESS ||
        (r = vkCreateFence(dev, &fci, NULL, &fence)) != VK_SUCCESS) {
        say(" failed=commands/%d", (int)r);
        goto pool;
    }

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };

    /* 1. The fill. */
    vkBeginCommandBuffer(cmd, &begin);
    vkCmdFillBuffer(cmd, buf, 0, WORDS * 2u, FILLED);
    vkEndCommandBuffer(cmd);
    t0 = now_us();
    if ((r = vkQueueSubmit(queue, 1, &submit, fence)) != VK_SUCCESS ||
        (r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull)) != VK_SUCCESS) {
        say(" failed=fill/%d", (int)r);
        goto fence;
    }
    double took = now_us() - t0;
    uint32_t right = 0, i;
    for (i = 0; i < WORDS / 2u; i++) {
        right += words[i] == FILLED;
    }
    uint32_t untouched = 0;
    for (; i < WORDS; i++) {
        untouched += words[i] == 0;
    }
    say(" fill=%u/%u,%u/%u %.0fus", right, WORDS / 2u, untouched, WORDS / 2u, took);

    /* 2. The dispatch, over the second half. */
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof write_comp,
        .pCode = write_comp,
    };
    struct { VkDeviceAddress at; uint32_t count, pad; } push;
    VkPushConstantRange range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof push,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    };
    VkShaderModule module;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    if ((r = vkCreateShaderModule(dev, &smci, NULL, &module)) != VK_SUCCESS) {
        say(" failed=module/%d", (int)r);
        goto fence;
    }
    if ((r = vkCreatePipelineLayout(dev, &plci, NULL, &layout)) != VK_SUCCESS) {
        say(" failed=layout/%d", (int)r);
        goto module;
    }
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main",
        },
        .layout = layout,
    };
    t0 = now_us();
    if ((r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline)) != VK_SUCCESS) {
        say(" failed=pipeline/%d", (int)r);
        goto layout;
    }
    say(" pipeline=%.0fus", now_us() - t0);

    VkBufferDeviceAddressInfo bdai = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buf,
    };
    push.at = vkGetBufferDeviceAddress(dev, &bdai) + WORDS * 2u;
    push.count = WORDS / 2u;
    push.pad = 0;

    vkResetCommandBuffer(cmd, 0);
    vkResetFences(dev, 1, &fence);
    vkBeginCommandBuffer(cmd, &begin);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(cmd, WORDS / 2u / 64u, 1, 1);
    vkEndCommandBuffer(cmd);
    t0 = now_us();
    if ((r = vkQueueSubmit(queue, 1, &submit, fence)) != VK_SUCCESS ||
        (r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull)) != VK_SUCCESS) {
        say(" failed=dispatch/%d", (int)r);
        goto pipeline;
    }
    took = now_us() - t0;
    right = 0;
    for (i = 0; i < WORDS / 2u; i++) {
        right += words[WORDS / 2u + i] == i * 3u + 7u;
    }
    uint32_t kept = 0;
    for (i = 0; i < WORDS / 2u; i++) {
        kept += words[i] == FILLED;
    }
    say(" dispatch=%u/%u,%u/%u %.0fus", right, WORDS / 2u, kept, WORDS / 2u, took);

    /* 3. **Submit to fence** (G7d's measure): one small piece of work,
     * recorded once and submitted LATENCY times, each waited for - what a
     * frame pays to reach the GPU and hear back, with nothing in it worth
     * the GPU's time. A fill of one word, not an empty buffer: an empty
     * submission signals without the GPU (`anv_kosmos.c`). Said as the
     * least, the middle and the most. */
    enum { LATENCY = 100 };
    double waits[LATENCY];
    VkCommandBufferBeginInfo again = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &again);
    vkCmdFillBuffer(cmd, buf, 0, 4, FILLED);
    vkEndCommandBuffer(cmd);
    for (i = 0; i < LATENCY; i++) {
        vkResetFences(dev, 1, &fence);
        t0 = now_us();
        if ((r = vkQueueSubmit(queue, 1, &submit, fence)) != VK_SUCCESS ||
            (r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull)) != VK_SUCCESS) {
            say(" failed=latency/%d", (int)r);
            goto pipeline;
        }
        waits[i] = now_us() - t0;
    }
    for (i = 1; i < LATENCY; i++) {             /* a hundred: insertion */
        double w = waits[i];
        uint32_t j = i;
        for (; j > 0 && waits[j - 1] > w; j--) {
            waits[j] = waits[j - 1];
        }
        waits[j] = w;
    }
    say(" latency=%.0f/%.0f/%.0fus", waits[0], waits[LATENCY / 2], waits[LATENCY - 1]);

pipeline:
    vkDestroyPipeline(dev, pipeline, NULL);
layout:
    vkDestroyPipelineLayout(dev, layout, NULL);
module:
    vkDestroyShaderModule(dev, module, NULL);
fence:
    vkDestroyFence(dev, fence, NULL);
pool:
    vkDestroyCommandPool(dev, pool, NULL);
memory:
    vkFreeMemory(dev, mem, NULL);
buffer:
    vkDestroyBuffer(dev, buf, NULL);
device:
    vkDestroyDevice(dev, NULL);
out:
    if (messenger != VK_NULL_HANDLE && messenger_end != NULL) {
        messenger_end(instance, messenger, NULL);
    }
    vkDestroyInstance(instance, NULL);
}

/* --- the kit -------------------------------------------------------------- */

static int l_devices(lua_State *L)
{
    said_at = 0;
    said[0] = 0;
    devices();
    lua_pushstring(L, said);
    return 1;
}

static int l_compile(lua_State *L)
{
    lua_Integer id = luaL_checkinteger(L, 1);

    luaL_argcheck(L, id > 0 && id <= 0xffff, 1, "a PCI device id");
    said_at = 0;
    said[0] = 0;
    compile((int)id);
    lua_pushstring(L, said);
    return 1;
}

static int l_run(lua_State *L)
{
    hand_console(L);
    said_at = 0;
    said[0] = 0;
    run();
    lua_pushstring(L, said);
    return 1;
}

void kosmos_vulkantest_kit(lua_State *L);

/*
 * Mesa's options, which a Kosmos process has no environment to carry
 * (Mesa patch 0008): `kit.option("INTEL_DEBUG", "bat")` before a device is
 * opened, and anv decodes every batch it submits into the console - how
 * the render engine's hang on the 3D Kit's first submission was read
 * (`testing.md` 18.955). Up to eight, each name and value up to 63 bytes.
 */
static char option_name[8][64], option_value[8][64];
static unsigned options;

const char *kosmos_mesa_option(const char *name);
const char *kosmos_mesa_option(const char *name)
{
    for (unsigned i = 0; i < options; i++) {
        if (strcmp(option_name[i], name) == 0) {
            return option_value[i];
        }
    }
    return NULL;
}

static int l_option(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1), *value = luaL_checkstring(L, 2);
    unsigned i;

    luaL_argcheck(L, strlen(name) < 64, 1, "a name of up to 63 bytes");
    luaL_argcheck(L, strlen(value) < 64, 2, "a value of up to 63 bytes");
    for (i = 0; i < options && strcmp(option_name[i], name) != 0; i++) {
    }
    if (i == options) {
        luaL_argcheck(L, options < 8, 1, "eight options at most");
        options++;
    }
    strcpy(option_name[i], name);
    strcpy(option_value[i], value);
    return 0;
}

/*
 * `vulkantest`'s table - the triangle, the cube, the cube in a window and
 * `hold` (G2, G3) - on `anv` rather than `lavapipe`, since its source is
 * linked into this image too (G7c); and this kit's own three beside them.
 */
void kosmos_anvtest_kit(lua_State *L)
{
    kosmos_vulkantest_kit(L);
    lua_pushcfunction(L, l_devices);
    lua_setfield(L, -2, "devices");
    lua_pushcfunction(L, l_compile);
    lua_setfield(L, -2, "compile");
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
    lua_pushcfunction(L, l_option);
    lua_setfield(L, -2, "option");
}
