/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Quake III's window, and Vulkan's surface in it (`docs/quake3e.md` §3).
 *
 * In place of `sdl_glimp.c`: the window is the Window Kit's (`kw_open`),
 * and the renderer is handed the driver's own door - there is no loader,
 * the driver is linked into this image (`lavapipe` under QEMU, `anv` on an
 * Intel GPU) - and makes its surface with `VK_KOSMOS_surface`. The
 * renderer never names a window system: these three functions are the
 * whole of what it asks (`tr_public.h`, `VKimp_Init`,
 * `VK_GetInstanceProcAddr`, `VK_CreateSurface`).
 *
 * **The window is the size the game asks for and does not change** - no
 * grip. Quake3e learns a new size only through `vid_restart`, and a window
 * that resized under it would be drawn into at the old size until then;
 * so a size is chosen as Quake III chooses one, with `r_mode` (or
 * `r_customwidth` and `r_customheight`), and `quake3.lua` sets 1280 by 720.
 */

/* Mesa's header first, with Kosmos's surface in it; Quake3e's own copy of
 * the registry's header, which its headers include, has the same guard
 * and is then skipped. */
#define VK_USE_PLATFORM_KOSMOS
#include <vulkan/vulkan.h>

#include "client.h"

#include "kits/window/kosmos_window.h"

#include "q3_kosmos.h"

/* The driver's one door (`kvk.h`). */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

struct kw_window *q3_window;

/* The size of a screen, as `CL_GetModeInfo` asks it: Kosmos does not tell
 * a window how large the screen is, and `r_mode -2` would be meaningless
 * in a window anyway. 1920 by 1080 is the screen every machine here has. */
#define DESKTOP_W 1920
#define DESKTOP_H 1080

void q3_window_close(void)
{
    if (q3_window != NULL) {
        kw_close(q3_window);
        q3_window = NULL;
    }
}

void VKimp_Init(glconfig_t *config)
{
    Com_Printf("Initializing Vulkan display\n");

    if (!CL_GetModeInfo(&config->vidWidth, &config->vidHeight,
                        &config->windowAspect, r_mode->integer,
                        r_modeFullscreen->string, DESKTOP_W, DESKTOP_H, qfalse)) {
        Com_Printf("...r_mode %d is not a mode; 1280 by 720\n", r_mode->integer);
        config->vidWidth = 1280;
        config->vidHeight = 720;
        config->windowAspect = (float)config->vidWidth / config->vidHeight;
    }

    if (q3_window == NULL) {
        q3_window = kw_open("Quake III Arena", (unsigned)config->vidWidth,
                            (unsigned)config->vidHeight, KW_CENTRE);
    }

    if (q3_window == NULL) {
        Com_Error(ERR_FATAL, "VKimp_Init: no window: %s", kw_why());
    }

    /* The window's surface is what Vulkan presents into, so its size is
     * the size drawn - which the window manager may have made smaller, to
     * fit the screen. */
    struct kw_surface s = kw_surface(q3_window);

    if (s.width != 0 && s.height != 0) {
        config->vidWidth = (int)s.width;
        config->vidHeight = (int)s.height;
        config->windowAspect = (float)s.width / s.height;
    }

    Com_Printf("...a window of %d by %d\n", config->vidWidth, config->vidHeight);

    config->isFullscreen = qfalse;
    config->stereoEnabled = qfalse;
    config->driverType = GLDRV_ICD;
    config->hardwareType = GLHW_GENERIC;
    config->colorBits = 32;
    config->depthBits = 24;
    config->stencilBits = 8;

    /* The engine renders nothing while it believes it is minimised. */
    gw_active = qtrue;
    gw_minimized = qfalse;

    IN_Init();
    Key_ClearStates();
}

void VKimp_Shutdown(qboolean unloadDLL)
{
    (void)unloadDLL;

    IN_Shutdown();

    /* The window outlives a `vid_restart` - the renderer's surface is
     * destroyed before this and made again after - and is closed when the
     * game ends (`Sys_Quit`, `Sys_Error`, the close box). */
    gw_active = qfalse;
}

void *VK_GetInstanceProcAddr(VkInstance instance, const char *name)
{
    return (void *)vk_icdGetInstanceProcAddr(instance, name);
}

qboolean VK_CreateSurface(VkInstance instance, VkSurfaceKHR *surface)
{
    PFN_vkCreateKosmosSurfaceKOSMOS create =
        (PFN_vkCreateKosmosSurfaceKOSMOS)vk_icdGetInstanceProcAddr(
            instance, "vkCreateKosmosSurfaceKOSMOS");
    VkKosmosSurfaceCreateInfoKOSMOS info = {
        .sType = VK_STRUCTURE_TYPE_KOSMOS_SURFACE_CREATE_INFO_KOSMOS,
        .window = q3_window,
    };

    if (create == NULL || q3_window == NULL) {
        return qfalse;
    }

    return create(instance, &info, NULL, surface) == VK_SUCCESS ? qtrue : qfalse;
}

/*
 * **No OpenGL in this image**, and no gamma ramp: a window cannot set the
 * screen's, and Quake3e's Vulkan renderer applies `r_gamma` in a pass of
 * its own when the device cannot (`deviceSupportsGamma` false).
 */
void GLimp_InitGamma(glconfig_t *config)
{
    config->deviceSupportsGamma = qfalse;
}

void GLimp_SetGamma(unsigned char red[256], unsigned char green[256],
                    unsigned char blue[256])
{
    (void)red;
    (void)green;
    (void)blue;
}
