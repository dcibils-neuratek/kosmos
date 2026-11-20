/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **VK_KOSMOS_surface: a swapchain into a Kosmos window** (`docs/gpu3d.md`
 * G3). The window is the Window Kit's (`user/kits/window/kosmos_window.h`):
 * two buffers in the region the window manager composes from, and
 * `kw_commit` making the one just drawn the one shown.
 *
 * **The images are the device's, and presenting copies.** A swapchain
 * image is a CPU image of Mesa's (`WSI_IMAGE_TYPE_CPU`, linear, in memory
 * the device maps), as Win32's software path has; `queue_present` copies
 * its rows into the window's buffer to draw into and commits it. On a GPU
 * (G7c, `anv`) the image is the GPU's own, tiled, and Mesa's buffer blit
 * copies each frame into a linear buffer the processor maps; that buffer
 * is what is copied, once its fence has signalled. The
 * design was the image *as* the window's buffer, with no copy
 * (`gpu3d.md` 5.5), and that waits on one thing: a window's buffer starts
 * under the header's band, on no page's boundary, so lavapipe cannot take
 * it as an image's memory - the window manager placing it on one is the
 * follow-up, measured first. A row is `width * 4` bytes of memcpy, and at
 * 1920 by 1080 a frame's copy is a few milliseconds beside lavapipe's
 * rendering of it.
 *
 * **What is presented is what the window says.** The current extent is
 * the window's buffer's size; a swapchain of another size presents the
 * part both have and answers VK_SUBOPTIMAL_KHR, so the program makes one
 * of the new size - which is what it does after a KW_RESIZE.
 *
 * **Pixels are 0xAARRGGBB in a word**, bytes B, G, R, A: the formats are
 * B8G8R8A8, UNORM and SRGB, and nothing is converted on the way.
 *
 * The program polls its own window, and only one thread draws into it:
 * the Window Kit runs on the process's Lua state, as every call to it does.
 */

#include <string.h>

#include "util/macros.h"
#include "vk_util.h"
#include "vk_instance.h"
#include "vk_physical_device.h"
#include "wsi_common_entrypoints.h"
#include "wsi_common_private.h"

#include "kosmos_window.h"

struct wsi_kosmos {
   struct wsi_interface base;
};

static VkResult
wsi_kosmos_surface_get_support(VkIcdSurfaceBase *surface,
                               struct wsi_device *wsi_device,
                               uint32_t queueFamilyIndex,
                               VkBool32 *pSupported)
{
   *pSupported = true;
   return VK_SUCCESS;
}

/* FIFO, which every implementation has; and immediate, which is what
 * presenting here is in fact - the copy and the commit, at once. */
static const VkPresentModeKHR present_modes[] = {
   VK_PRESENT_MODE_FIFO_KHR,
   VK_PRESENT_MODE_IMMEDIATE_KHR,
};

static const VkFormat formats[] = {
   VK_FORMAT_B8G8R8A8_UNORM,
   VK_FORMAT_B8G8R8A8_SRGB,
};

static VkResult
wsi_kosmos_surface_get_capabilities(VkIcdSurfaceBase *icd_surface,
                                    struct wsi_device *wsi_device,
                                    VkSurfaceCapabilities2KHR *caps)
{
   VkIcdSurfaceKosmos *surface = (VkIcdSurfaceKosmos *)icd_surface;
   struct kw_surface s = kw_surface(surface->window);

   if (s.pixels == NULL)
      return VK_ERROR_SURFACE_LOST_KHR;

   caps->surfaceCapabilities.minImageCount = 2;
   caps->surfaceCapabilities.maxImageCount = 0;
   caps->surfaceCapabilities.currentExtent = (VkExtent2D) { s.width, s.height };
   caps->surfaceCapabilities.minImageExtent = (VkExtent2D) { 1, 1 };
   caps->surfaceCapabilities.maxImageExtent = (VkExtent2D) {
      wsi_device->maxImageDimension2D,
      wsi_device->maxImageDimension2D,
   };
   caps->surfaceCapabilities.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->surfaceCapabilities.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->surfaceCapabilities.maxImageArrayLayers = 1;
   caps->surfaceCapabilities.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
   caps->surfaceCapabilities.supportedUsageFlags = wsi_caps_get_image_usage();

   vk_foreach_struct(ext, caps->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR: {
         VkSurfaceProtectedCapabilitiesKHR *protected = (void *)ext;
         protected->supportsProtected = VK_FALSE;
         break;
      }
      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_KHR: {
         VkSurfacePresentScalingCapabilitiesKHR *scaling = (void *)ext;
         scaling->supportedPresentScaling = 0;
         scaling->supportedPresentGravityX = 0;
         scaling->supportedPresentGravityY = 0;
         scaling->minScaledImageExtent = caps->surfaceCapabilities.minImageExtent;
         scaling->maxScaledImageExtent = caps->surfaceCapabilities.maxImageExtent;
         break;
      }
      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR: {
         VkSurfacePresentModeCompatibilityKHR *compat = (void *)ext;
         if (compat->pPresentModes == NULL) {
            compat->presentModeCount = ARRAY_SIZE(present_modes);
         } else {
            compat->presentModeCount = MIN2(compat->presentModeCount,
                                            ARRAY_SIZE(present_modes));
            typed_memcpy(compat->pPresentModes, present_modes,
                         compat->presentModeCount);
         }
         break;
      }
      default:
         break;
      }
   }

   return VK_SUCCESS;
}

static VkResult
wsi_kosmos_surface_get_capabilities2(VkIcdSurfaceBase *surface,
                                     struct wsi_device *wsi_device,
                                     const void *info_next,
                                     VkSurfaceCapabilities2KHR *caps)
{
   assert(caps->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);
   return wsi_kosmos_surface_get_capabilities(surface, wsi_device, caps);
}

static VkResult
wsi_kosmos_surface_get_formats(VkIcdSurfaceBase *icd_surface,
                               struct wsi_device *wsi_device,
                               uint32_t *pSurfaceFormatCount,
                               VkSurfaceFormatKHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats, pSurfaceFormatCount);

   for (unsigned i = 0; i < ARRAY_SIZE(formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, f) {
         f->format = formats[i];
         f->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static VkResult
wsi_kosmos_surface_get_formats2(VkIcdSurfaceBase *icd_surface,
                                struct wsi_device *wsi_device,
                                const void *info_next,
                                uint32_t *pSurfaceFormatCount,
                                VkSurfaceFormat2KHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats, pSurfaceFormatCount);

   for (unsigned i = 0; i < ARRAY_SIZE(formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, f) {
         f->surfaceFormat.format = formats[i];
         f->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static VkResult
wsi_kosmos_surface_get_present_modes(VkIcdSurfaceBase *surface,
                                     struct wsi_device *wsi_device,
                                     uint32_t *pPresentModeCount,
                                     VkPresentModeKHR *pPresentModes)
{
   if (pPresentModes == NULL) {
      *pPresentModeCount = ARRAY_SIZE(present_modes);
      return VK_SUCCESS;
   }

   *pPresentModeCount = MIN2(*pPresentModeCount, ARRAY_SIZE(present_modes));
   typed_memcpy(pPresentModes, present_modes, *pPresentModeCount);
   return *pPresentModeCount < ARRAY_SIZE(present_modes) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult
wsi_kosmos_surface_get_present_rectangles(VkIcdSurfaceBase *icd_surface,
                                          struct wsi_device *wsi_device,
                                          uint32_t *pRectCount,
                                          VkRect2D *pRects)
{
   VkIcdSurfaceKosmos *surface = (VkIcdSurfaceKosmos *)icd_surface;
   struct kw_surface s = kw_surface(surface->window);
   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);

   vk_outarray_append_typed(VkRect2D, &out, rect) {
      *rect = (VkRect2D) { .offset = { 0, 0 }, .extent = { s.width, s.height } };
   }

   return vk_outarray_status(&out);
}

struct wsi_kosmos_image {
   struct wsi_image base;
   bool acquired;              /* the program's, between acquire and present */
};

struct wsi_kosmos_swapchain {
   struct wsi_swapchain base;
   struct kw_window *window;
   VkExtent2D extent;
   uint32_t next;              /* where acquiring looks first: round the ring */
   struct wsi_kosmos_image images[0];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(wsi_kosmos_swapchain, base.base, VkSwapchainKHR,
                               VK_OBJECT_TYPE_SWAPCHAIN_KHR)

static struct wsi_image *
wsi_kosmos_swapchain_get_wsi_image(struct wsi_swapchain *wsi_chain, uint32_t index)
{
   struct wsi_kosmos_swapchain *chain = (struct wsi_kosmos_swapchain *)wsi_chain;
   return &chain->images[index].base;
}

static VkResult
wsi_kosmos_swapchain_release_images(struct wsi_swapchain *wsi_chain,
                                    uint32_t count, const uint32_t *indices)
{
   struct wsi_kosmos_swapchain *chain = (struct wsi_kosmos_swapchain *)wsi_chain;

   for (uint32_t i = 0; i < count; i++) {
      assert(indices[i] < chain->base.image_count);
      chain->images[indices[i]].acquired = false;
   }

   return VK_SUCCESS;
}

/*
 * The next image the program does not hold, round the ring. Presenting
 * copies at once, so an image is free again the moment it is presented:
 * there is nothing to wait for but the program's own, and a program that
 * holds every image and asks for another is told VK_NOT_READY - or, with
 * no timeout, the same, since nothing will ever give one back.
 */
static VkResult
wsi_kosmos_swapchain_acquire_next_image(struct wsi_swapchain *wsi_chain,
                                        const VkAcquireNextImageInfoKHR *info,
                                        uint32_t *image_index)
{
   struct wsi_kosmos_swapchain *chain = (struct wsi_kosmos_swapchain *)wsi_chain;

   for (uint32_t n = 0; n < chain->base.image_count; n++) {
      uint32_t i = (chain->next + n) % chain->base.image_count;

      if (!chain->images[i].acquired) {
         chain->images[i].acquired = true;
         chain->next = (i + 1) % chain->base.image_count;
         *image_index = i;
         return VK_SUCCESS;
      }
   }

   return info->timeout == 0 ? VK_NOT_READY : VK_TIMEOUT;
}

static VkResult
wsi_kosmos_swapchain_queue_present(struct wsi_swapchain *wsi_chain,
                                   uint32_t image_index,
                                   uint64_t present_id,
                                   const VkPresentRegionKHR *damage)
{
   struct wsi_kosmos_swapchain *chain = (struct wsi_kosmos_swapchain *)wsi_chain;
   struct wsi_kosmos_image *image = &chain->images[image_index];
   struct kw_surface s = kw_surface(chain->window);
   const char *from = image->base.cpu_map;
   char *to = (char *)s.pixels;

   assert(image_index < chain->base.image_count);
   image->acquired = false;

   /* A GPU's frame is drawn into its own image and copied by the GPU into
    * the buffer the processor reads (the swapchain's buffer blit), and
    * that copy has finished only when the image's fence says so. Mesa
    * waits for a software device's fence itself, and for nobody else's. */
   if (!chain->base.wsi->sw) {
      VkResult r = chain->base.wsi->WaitForFences(chain->base.device, 1,
                                                  &chain->base.fences[image_index],
                                                  true, UINT64_MAX);
      if (r != VK_SUCCESS)
         return r;
   }

   if (s.pixels == NULL)
      return VK_ERROR_SURFACE_LOST_KHR;

   unsigned width = MIN2(s.width, chain->extent.width);
   unsigned height = MIN2(s.height, chain->extent.height);

   for (unsigned y = 0; y < height; y++) {
      memcpy(to, from, (size_t)width * 4);
      to += s.pitch;
      from += image->base.row_pitches[0];
   }

   if (!kw_commit(chain->window, 0, 0, width, height))
      return VK_ERROR_SURFACE_LOST_KHR;

   if (s.width != chain->extent.width || s.height != chain->extent.height)
      return VK_SUBOPTIMAL_KHR;

   return VK_SUCCESS;
}

static VkResult
wsi_kosmos_swapchain_destroy(struct wsi_swapchain *wsi_chain,
                             const VkAllocationCallbacks *pAllocator)
{
   struct wsi_kosmos_swapchain *chain = (struct wsi_kosmos_swapchain *)wsi_chain;

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      if (chain->images[i].base.image != VK_NULL_HANDLE)
         wsi_destroy_image(&chain->base, &chain->images[i].base);
   }

   wsi_swapchain_finish(&chain->base);
   vk_free(pAllocator, chain);
   return VK_SUCCESS;
}

static VkResult
wsi_kosmos_surface_create_swapchain(VkIcdSurfaceBase *icd_surface,
                                    VkDevice device,
                                    struct wsi_device *wsi_device,
                                    const VkSwapchainCreateInfoKHR *pCreateInfo,
                                    const VkAllocationCallbacks *pAllocator,
                                    struct wsi_swapchain **swapchain_out)
{
   VkIcdSurfaceKosmos *surface = (VkIcdSurfaceKosmos *)icd_surface;
   struct wsi_kosmos_swapchain *chain;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);

   uint32_t num_images = MAX2(pCreateInfo->minImageCount, 2);
   size_t size = sizeof(*chain) + num_images * sizeof(chain->images[0]);

   chain = vk_zalloc(pAllocator, size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (chain == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   struct wsi_cpu_image_params cpu_params = {
      .base.image_type = WSI_IMAGE_TYPE_CPU,
   };

   result = wsi_swapchain_init(wsi_device, &chain->base, device,
                               pCreateInfo, &cpu_params.base, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, chain);
      return result;
   }

   chain->base.destroy = wsi_kosmos_swapchain_destroy;
   chain->base.get_wsi_image = wsi_kosmos_swapchain_get_wsi_image;
   chain->base.acquire_next_image = wsi_kosmos_swapchain_acquire_next_image;
   chain->base.release_images = wsi_kosmos_swapchain_release_images;
   chain->base.queue_present = wsi_kosmos_swapchain_queue_present;
   chain->base.present_mode = wsi_swapchain_get_present_mode(wsi_device, pCreateInfo);
   chain->base.image_count = num_images;
   chain->window = surface->window;
   chain->extent = pCreateInfo->imageExtent;

   uint32_t image = 0;
   for (; image < num_images; image++) {
      result = wsi_create_image(&chain->base, &chain->base.image_info,
                                &chain->images[image].base);
      if (result != VK_SUCCESS)
         break;
   }

   if (result != VK_SUCCESS) {
      for (uint32_t i = 0; i < image; i++)
         wsi_destroy_image(&chain->base, &chain->images[i].base);
      wsi_swapchain_finish(&chain->base);
      vk_free(pAllocator, chain);
      return result;
   }

   *swapchain_out = &chain->base;
   return VK_SUCCESS;
}

VkResult
wsi_kosmos_init_wsi(struct wsi_device *wsi_device,
                    const VkAllocationCallbacks *alloc,
                    VkPhysicalDevice physical_device)
{
   struct wsi_kosmos *wsi = vk_alloc(alloc, sizeof(*wsi), 8,
                                     VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);

   if (wsi == NULL) {
      wsi_device->wsi[VK_ICD_WSI_PLATFORM_KOSMOS] = NULL;
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   wsi->base.get_support = wsi_kosmos_surface_get_support;
   wsi->base.get_capabilities2 = wsi_kosmos_surface_get_capabilities2;
   wsi->base.get_formats = wsi_kosmos_surface_get_formats;
   wsi->base.get_formats2 = wsi_kosmos_surface_get_formats2;
   wsi->base.get_present_modes = wsi_kosmos_surface_get_present_modes;
   wsi->base.get_present_rectangles = wsi_kosmos_surface_get_present_rectangles;
   wsi->base.create_swapchain = wsi_kosmos_surface_create_swapchain;

   wsi_device->wsi[VK_ICD_WSI_PLATFORM_KOSMOS] = &wsi->base;
   return VK_SUCCESS;
}

void
wsi_kosmos_finish_wsi(struct wsi_device *wsi_device,
                      const VkAllocationCallbacks *alloc)
{
   struct wsi_kosmos *wsi =
      (struct wsi_kosmos *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_KOSMOS];

   if (wsi != NULL)
      vk_free(alloc, wsi);
}

VKAPI_ATTR VkResult VKAPI_CALL
wsi_CreateKosmosSurfaceKOSMOS(VkInstance _instance,
                              const VkKosmosSurfaceCreateInfoKOSMOS *pCreateInfo,
                              const VkAllocationCallbacks *pAllocator,
                              VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   VkIcdSurfaceKosmos *surface;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_KOSMOS_SURFACE_CREATE_INFO_KOSMOS);

   if (pCreateInfo->window == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   surface = vk_alloc2(&instance->alloc, pAllocator, sizeof *surface, 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->base.platform = VK_ICD_WSI_PLATFORM_KOSMOS;
   surface->window = pCreateInfo->window;

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->base);
   return VK_SUCCESS;
}
