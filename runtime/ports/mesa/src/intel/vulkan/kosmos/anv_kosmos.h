/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ANV_KOSMOS_H
#define ANV_KOSMOS_H

/*
 * **`anv` on Kosmos's GPU driver** (`docs/gpu3d.md` G7): the third kernel
 * driver type beside i915 and Xe, `INTEL_KMD_TYPE_KOSMOS`, and the one
 * `anv` runs on here. Its `fd` is the process's capability to
 * `/Devices/gpu` (the GPU Kit, `kosmos_gpu.h`), and everything i915 and Xe
 * do by ioctl is a verb of `gpuproto.h`:
 *
 * - a buffer object is a **region**, its handle the region's capability
 *   plus one (so a valid handle is never nought);
 * - binding it is `GPU_OP_BIND` at the address `anv` chose for it, in
 *   the device's context - Xe's model, softpinned, no relocations;
 * - a queue is a **context**, and a submission `GPU_OP_EXEC`;
 * - a `vk_sync` is a **point on a context's timeline**, waited for by the
 *   fence page and `GPU_OP_FENCE_WAIT`'s pulse.
 *
 * G7a is the physical device - enumerated, described, its sync types
 * named; G7b the logical device, one context on the render engine that
 * its queue submits to, and its buffer objects bound there.
 */

#include <stdint.h>

#include "vulkan/vulkan_core.h"

struct vk_instance;
struct vk_physical_device;
struct vk_sync_type;
struct anv_physical_device;
struct anv_device;
struct anv_kmd_backend;
struct vk_device;
struct anv_queue;

/* The device, where there is one: Mesa's `enumerate` hook. */
VkResult anv_kosmos_enumerate(struct vk_instance *instance);

/* What i915's `getparam` and Xe's config query answer elsewhere. */
VkResult anv_kosmos_physical_device_get_parameters(struct anv_physical_device *device);
VkResult anv_kosmos_physical_device_init_memory_types(struct anv_physical_device *device);

/* The sync types: a binary point, and timelines emulated over it. */
void anv_kosmos_physical_device_init_sync_types(struct anv_physical_device *device);

/* The logical device's contexts (G7b). */
VkResult anv_kosmos_device_setup(struct anv_device *device);
bool anv_kosmos_device_destroy(struct anv_device *device);
VkResult anv_kosmos_device_check_status(struct vk_device *device);

/* A queue: the device's context, on the render engine. */
VkResult anv_kosmos_create_engine(struct anv_device *device, struct anv_queue *queue,
                                  const VkDeviceQueueCreateInfo *pCreateInfo);
void anv_kosmos_destroy_engine(struct anv_device *device, struct anv_queue *queue);

const struct anv_kmd_backend *anv_kosmos_kmd_backend_get(void);

/* A buffer object's mapping given back: a region leaves the share window. */
void anv_kosmos_unmap(void *map, uint64_t size);

#endif
