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

void kosmos_anvtest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_devices);
    lua_setfield(L, -2, "devices");
    lua_pushcfunction(L, l_compile);
    lua_setfield(L, -2, "compile");
}
