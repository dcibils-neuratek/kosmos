/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef VULKAN_KOSMOS_H_
#define VULKAN_KOSMOS_H_ 1

/*
 * **VK_KOSMOS_surface** (`docs/gpu3d.md` G3): a Vulkan surface over a
 * Kosmos window, the Window Kit's (`kosmos_window.h`). Written by hand in
 * the shape of Khronos's platform headers, and declared in Mesa's copy of
 * the registry (`runtime/patches/mesa/0005`), where the generators read
 * it. Not Khronos's: an extension is named `KHR` only when they ratify it.
 *
 *   VkKosmosSurfaceCreateInfoKOSMOS info = {
 *       .sType = VK_STRUCTURE_TYPE_KOSMOS_SURFACE_CREATE_INFO_KOSMOS,
 *       .window = kw_open("Cube", 640, 480, KW_RESIZABLE),
 *   };
 *   vkCreateKosmosSurfaceKOSMOS(instance, &info, NULL, &surface);
 *
 * The window stays the program's: it polls it, and closes it after
 * destroying the surface.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define VK_KOSMOS_surface 1
#define VK_KOSMOS_SURFACE_SPEC_VERSION   1
#define VK_KOSMOS_SURFACE_EXTENSION_NAME "VK_KOSMOS_surface"

struct kw_window;

typedef VkFlags VkKosmosSurfaceCreateFlagsKOSMOS;

typedef struct VkKosmosSurfaceCreateInfoKOSMOS {
    VkStructureType                  sType;
    const void*                      pNext;
    VkKosmosSurfaceCreateFlagsKOSMOS flags;
    struct kw_window*                window;
} VkKosmosSurfaceCreateInfoKOSMOS;

typedef VkResult (VKAPI_PTR *PFN_vkCreateKosmosSurfaceKOSMOS)(VkInstance instance, const VkKosmosSurfaceCreateInfoKOSMOS* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface);

#ifndef VK_NO_PROTOTYPES
VKAPI_ATTR VkResult VKAPI_CALL vkCreateKosmosSurfaceKOSMOS(
    VkInstance                              instance,
    const VkKosmosSurfaceCreateInfoKOSMOS*  pCreateInfo,
    const VkAllocationCallbacks*            pAllocator,
    VkSurfaceKHR*                           pSurface);
#endif

#ifdef __cplusplus
}
#endif

#endif
