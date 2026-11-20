/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **`anv`'s Kosmos backend** (`anv_kosmos.h`, `docs/gpu3d.md` G7).
 *
 * Three parts, in the order a program meets them:
 *
 * 1. **The physical device**: found by asking for `/Devices/gpu`, which a
 *    program has only when it was granted the GPU and the machine has an
 *    Intel one the driver knows; described by `dev/kosmos/` and Mesa's
 *    own table. Its memory is Xe's on a part that shares the last-level
 *    cache: write-back, coherent, one heap.
 *
 * 2. **The sync type**: a *point* - `{context, value}` once a submission
 *    will signal it, or signalled by the processor outright. A point is
 *    reached when its context's fence page says so, which the processor
 *    reads without asking anybody; a thread that has to wait creates an
 *    endpoint, asks the driver (`GPU_OP_FENCE_WAIT`) to pulse it when each
 *    point it waits for is reached, and receives with the deadline it was
 *    given. A point signalled or submitted on the processor pulses every
 *    endpoint that is waiting, so nothing waits for a clock to come round.
 *    Timelines are Mesa's emulation over the point (`vk_sync_timeline`).
 *
 * 3. **The buffer objects**: regions, created, mapped and dropped here.
 *    Binding and submission are G7b's, and refuse with a sentence until
 *    then - as does creating a logical device, which needs both.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <kosmos.h>
#include <kosmos_gpu.h>

#include "anv_private.h"
#include "anv_kmd_backend.h"
#include "kosmos/anv_kosmos.h"

#include "util/os_time.h"
#include "util/simple_mtx.h"
#include "vk_sync_timeline.h"

unsigned long posix_ticks_for_ns(long long ns);

#define PAGE 4096ull

/* --- Contexts' fence pages, by context id ------------------------------ */

/*
 * What a point needs to be read without a message: its context's fence
 * page. Registered by G7b's queue as it makes the context; grown as it
 * fills, never shrunk, as the kernel's pools are.
 */
struct kosmos_fence {
   uint32_t context;
   const void *map;
};

static simple_mtx_t lock = SIMPLE_MTX_INITIALIZER;
static struct kosmos_fence *fences;
static uint32_t fence_count, fence_room;

static long *waiters;                   /* endpoints of threads waiting now */
static uint32_t waiter_count, waiter_room;

static bool
grow(void **array, uint32_t *room, uint32_t need, size_t size)
{
   if (need <= *room)
      return true;
   uint32_t to = *room ? *room * 2 : 8;
   while (to < need)
      to *= 2;
   void *n = realloc(*array, (size_t)to * size);
   if (n == NULL)
      return false;
   *array = n;
   *room = to;
   return true;
}

/* The page a context's timeline is read from; NULL for one not known. */
static const void *
fence_of(uint32_t context)
{
   for (uint32_t i = 0; i < fence_count; i++) {
      if (fences[i].context == context)
         return fences[i].map;
   }
   return NULL;
}

/* Every waiting thread told that something changed on the processor's
 * side; each looks again. Called with the lock held. */
static void
wake_waiters_locked(void)
{
   for (uint32_t i = 0; i < waiter_count; i++)
      (void)kosmos_pulse(waiters[i], 2, 0);
}

/* --- The point ---------------------------------------------------------- */

struct kosmos_point {
   struct vk_sync base;
   bool cpu_signaled;
   bool submitted;          /* a submission will signal `context` at `value` */
   uint32_t context;
   uint64_t value;
};

static struct kosmos_point *
to_point(struct vk_sync *sync)
{
   return container_of(sync, struct kosmos_point, base);
}

/* Reached: signalled by the processor, or its context past its value. */
static bool
point_reached_locked(const struct kosmos_point *p)
{
   if (p->cpu_signaled)
      return true;
   if (!p->submitted)
      return false;
   const void *map = fence_of(p->context);
   return map != NULL && kg_reached(map) >= p->value;
}

static VkResult
point_init(struct vk_device *device, struct vk_sync *sync, uint64_t initial)
{
   struct kosmos_point *p = to_point(sync);
   (void)device;
   p->cpu_signaled = initial != 0;
   p->submitted = false;
   p->context = 0;
   p->value = 0;
   return VK_SUCCESS;
}

static void
point_finish(struct vk_device *device, struct vk_sync *sync)
{
   (void)device;
   (void)sync;
}

static VkResult
point_signal(struct vk_device *device, struct vk_sync *sync, uint64_t value)
{
   struct kosmos_point *p = to_point(sync);
   (void)device;
   (void)value;
   simple_mtx_lock(&lock);
   p->cpu_signaled = true;
   wake_waiters_locked();
   simple_mtx_unlock(&lock);
   return VK_SUCCESS;
}

static VkResult
point_reset(struct vk_device *device, struct vk_sync *sync)
{
   struct kosmos_point *p = to_point(sync);
   (void)device;
   simple_mtx_lock(&lock);
   p->cpu_signaled = false;
   p->submitted = false;
   simple_mtx_unlock(&lock);
   return VK_SUCCESS;
}

static VkResult
point_move(struct vk_device *device, struct vk_sync *dst, struct vk_sync *src)
{
   struct kosmos_point *d = to_point(dst), *s = to_point(src);
   (void)device;
   simple_mtx_lock(&lock);
   d->cpu_signaled = s->cpu_signaled;
   d->submitted = s->submitted;
   d->context = s->context;
   d->value = s->value;
   s->cpu_signaled = false;
   s->submitted = false;
   simple_mtx_unlock(&lock);
   return VK_SUCCESS;
}

static VkResult
point_wait_many(struct vk_device *device, uint32_t count,
                const struct vk_sync_wait *waits,
                enum vk_sync_wait_flags flags, uint64_t abs_timeout_ns)
{
   const bool any = (flags & VK_SYNC_WAIT_ANY) != 0;
   long gpu = kg_device();
   long endpoint = -1;
   bool *asked = NULL;
   VkResult result = VK_SUCCESS;
   (void)device;

   for (;;) {
      uint32_t reached = 0;

      simple_mtx_lock(&lock);
      for (uint32_t i = 0; i < count; i++) {
         if (point_reached_locked(to_point(waits[i].sync)))
            reached++;
      }
      if (any ? reached > 0 || count == 0 : reached == count) {
         simple_mtx_unlock(&lock);
         break;
      }

      const uint64_t now = os_time_get_nano();
      if (now >= abs_timeout_ns) {
         simple_mtx_unlock(&lock);
         result = VK_TIMEOUT;
         break;
      }

      /* Waiting, then: an endpoint of this call's own, registered while
       * the lock is held so a signal from the processor cannot slip by
       * between looking and listening. */
      if (endpoint < 0) {
         asked = calloc(count, sizeof(*asked));
         if (asked == NULL ||
             !grow((void **)&waiters, &waiter_room, waiter_count + 1,
                   sizeof(*waiters)) ||
             (endpoint = kosmos_endpoint()) < 0) {
            simple_mtx_unlock(&lock);
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            break;
         }
         waiters[waiter_count++] = endpoint;
      }

      /* Each point a submission will signal is asked about once: the
       * driver pulses at once if it is already reached. */
      for (uint32_t i = 0; i < count; i++) {
         const struct kosmos_point *p = to_point(waits[i].sync);
         if (!asked[i] && p->submitted && !p->cpu_signaled) {
            asked[i] = true;
            if (kg_fence_wait(gpu, p->context, p->value, endpoint, 1) != GPU_OK)
               result = VK_ERROR_DEVICE_LOST;
         }
      }
      simple_mtx_unlock(&lock);
      if (result != VK_SUCCESS)
         break;

      /* `os_time`'s infinity is a deadline that never comes: no timeout. */
      unsigned long ticks = 0;
      if (abs_timeout_ns != OS_TIMEOUT_INFINITE) {
         ticks = posix_ticks_for_ns((long long)(abs_timeout_ns - now));
         if (ticks == 0)
            ticks = 1;
      }
      struct message msg;
      uint64_t sender;
      (void)kosmos_receive(endpoint, &msg, &sender, 0, ticks);
   }

   if (endpoint >= 0) {
      simple_mtx_lock(&lock);
      for (uint32_t i = 0; i < waiter_count; i++) {
         if (waiters[i] == endpoint) {
            waiters[i] = waiters[--waiter_count];
            break;
         }
      }
      simple_mtx_unlock(&lock);
      (void)kosmos_endpoint_destroy(endpoint);
   }
   free(asked);
   return result;
}

static const struct vk_sync_type kosmos_point_type = {
   .size = sizeof(struct kosmos_point),
   .features = VK_SYNC_FEATURE_BINARY |
               VK_SYNC_FEATURE_GPU_WAIT |
               VK_SYNC_FEATURE_GPU_MULTI_WAIT |
               VK_SYNC_FEATURE_CPU_WAIT |
               VK_SYNC_FEATURE_CPU_RESET |
               VK_SYNC_FEATURE_CPU_SIGNAL |
               VK_SYNC_FEATURE_WAIT_ANY,
   .init = point_init,
   .finish = point_finish,
   .signal = point_signal,
   .reset = point_reset,
   .move = point_move,
   .wait_many = point_wait_many,
};

static struct vk_sync_timeline_type kosmos_timeline_type;

void
anv_kosmos_physical_device_init_sync_types(struct anv_physical_device *device)
{
   kosmos_timeline_type = vk_sync_timeline_get_type(&kosmos_point_type);

   device->sync_syncobj_type = kosmos_point_type;
   device->sync_types[0] = &device->sync_syncobj_type;
   device->sync_types[1] = &kosmos_timeline_type.sync;
   device->sync_types[2] = NULL;
   device->vk.supported_sync_types = device->sync_types;
}

/* --- The physical device ------------------------------------------------ */

VkResult
anv_kosmos_enumerate(struct vk_instance *instance)
{
   struct vk_physical_device *pdevice;
   VkResult result = anv_physical_device_try_create(instance, NULL, &pdevice);

   /* No GPU, or not one `anv` drives: no device, and no other way to look
    * for one - there is no DRM to fall back to. */
   if (result == VK_ERROR_INCOMPATIBLE_DRIVER)
      return VK_SUCCESS;
   if (result != VK_SUCCESS)
      return result;

   list_addtail(&pdevice->link, &instance->physical_devices.list);
   return VK_SUCCESS;
}

VkResult
anv_kosmos_physical_device_get_parameters(struct anv_physical_device *device)
{
   device->has_vm_control = false;
   device->max_context_priority = VK_QUEUE_GLOBAL_PRIORITY_MEDIUM;
   return VK_SUCCESS;
}

VkResult
anv_kosmos_physical_device_init_memory_types(struct anv_physical_device *device)
{
   /* Xe's types for a part that shares the last-level cache (the HD 530
    * does): one memory both cached and coherent, offered as two because
    * some engines cannot handle one. A part without it gets Xe's three. */
   if (device->info.has_llc) {
      device->memory.type_count = 2;
      device->memory.types[0] = (struct anv_memory_type) {
         .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
         .heapIndex = 0,
      };
      device->memory.types[1] = (struct anv_memory_type) {
         .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
         .heapIndex = 0,
      };
   } else {
      device->memory.types[device->memory.type_count++] = (struct anv_memory_type) {
         .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
         .heapIndex = 0,
      };
      device->memory.types[device->memory.type_count++] = (struct anv_memory_type) {
         .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
         .heapIndex = 0,
      };
      device->memory.types[device->memory.type_count++] = (struct anv_memory_type) {
         .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
         .heapIndex = 0,
      };
   }
   return VK_SUCCESS;
}

/* --- The logical device: G7b -------------------------------------------- */

VkResult
anv_kosmos_device_setup(struct anv_device *device)
{
   return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                    "a logical device on Kosmos's GPU is G7b's (docs/gpu3d.md)");
}

bool
anv_kosmos_device_destroy(struct anv_device *device)
{
   (void)device;
   return true;
}

VkResult
anv_kosmos_device_check_status(struct vk_device *device)
{
   (void)device;
   return VK_SUCCESS;
}

/* --- Buffer objects ----------------------------------------------------- */

static uint32_t
kosmos_gem_create(struct anv_device *device,
                  const struct intel_memory_class_instance **regions,
                  uint16_t num_regions, uint64_t size,
                  enum anv_bo_alloc_flags alloc_flags,
                  uint64_t *actual_size)
{
   (void)device;
   (void)regions;
   (void)num_regions;
   (void)alloc_flags;

   const uint64_t pages = (size + PAGE - 1) / PAGE;
   long region = kosmos_mem_create((unsigned long)pages);
   if (region < 0)
      return 0;

   *actual_size = pages * PAGE;
   return (uint32_t)region + 1;      /* a handle is never nought */
}

static uint32_t
kosmos_gem_create_userptr(struct anv_device *device, void *mem, uint64_t size)
{
   /* A program's own memory is not something the driver can be handed:
    * only regions cross to it. */
   (void)device;
   (void)mem;
   (void)size;
   return 0;
}

static void
kosmos_gem_close(struct anv_device *device, struct anv_bo *bo)
{
   (void)device;
   if (bo->gem_handle != 0)
      (void)kosmos_cap_drop((long)bo->gem_handle - 1);
}

static void *
kosmos_gem_mmap(struct anv_device *device, struct anv_bo *bo,
                uint64_t offset, uint64_t size, void *placed_addr)
{
   (void)device;
   (void)size;

   /* A region is mapped whole, where the window puts it. A piece of one,
    * or one at an address of the program's choosing, is G7b's question. */
   if (offset != 0 || placed_addr != NULL || bo->gem_handle == 0) {
      errno = EINVAL;
      return MAP_FAILED;
   }

   long at = kosmos_mem_map((long)bo->gem_handle - 1);
   if (at < 0) {
      errno = ENOMEM;
      return MAP_FAILED;
   }
   return (void *)(uintptr_t)at;
}

void
anv_kosmos_unmap(void *map, uint64_t size)
{
   (void)kosmos_share_unmap((unsigned long)(uintptr_t)map,
                            (unsigned long)((size + PAGE - 1) / PAGE));
}

static VkResult
kosmos_vm_bind(struct anv_device *device, struct anv_sparse_submission *submit,
               enum anv_vm_bind_flags flags)
{
   (void)submit;
   (void)flags;
   return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                    "sparse binding is not offered on Kosmos");
}

static VkResult
kosmos_vm_bind_bo(struct anv_device *device, struct anv_bo *bo)
{
   (void)bo;
   return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                    "binding a buffer object is G7b's (docs/gpu3d.md)");
}

static VkResult
kosmos_queue_exec_locked(struct anv_queue *queue,
                         uint32_t wait_count,
                         const struct vk_sync_wait *waits,
                         uint32_t cmd_buffer_count,
                         struct anv_cmd_buffer **cmd_buffers,
                         uint32_t signal_count,
                         const struct vk_sync_signal *signals,
                         struct anv_query_pool *perf_query_pool,
                         uint32_t perf_query_pass,
                         struct anv_utrace_submit *utrace_submit)
{
   (void)wait_count; (void)waits; (void)cmd_buffer_count; (void)cmd_buffers;
   (void)signal_count; (void)signals; (void)perf_query_pool;
   (void)perf_query_pass; (void)utrace_submit;
   return vk_errorf(queue, VK_ERROR_FEATURE_NOT_PRESENT,
                    "submission is G7b's (docs/gpu3d.md)");
}

static VkResult
kosmos_queue_exec_async(struct anv_async_submit *submit,
                        uint32_t wait_count,
                        const struct vk_sync_wait *waits,
                        uint32_t signal_count,
                        const struct vk_sync_signal *signals)
{
   (void)submit; (void)wait_count; (void)waits;
   (void)signal_count; (void)signals;
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

static uint32_t
kosmos_bo_alloc_flags_to_bo_flags(struct anv_device *device,
                                  enum anv_bo_alloc_flags alloc_flags)
{
   (void)device;
   (void)alloc_flags;
   return 0;
}

const struct anv_kmd_backend *
anv_kosmos_kmd_backend_get(void)
{
   static const struct anv_kmd_backend backend = {
      .gem_create = kosmos_gem_create,
      .gem_create_userptr = kosmos_gem_create_userptr,
      .gem_close = kosmos_gem_close,
      .gem_mmap = kosmos_gem_mmap,
      .vm_bind = kosmos_vm_bind,
      .vm_bind_bo = kosmos_vm_bind_bo,
      .vm_unbind_bo = kosmos_vm_bind_bo,
      .queue_exec_locked = kosmos_queue_exec_locked,
      .queue_exec_async = kosmos_queue_exec_async,
      .bo_alloc_flags_to_bo_flags = kosmos_bo_alloc_flags_to_bo_flags,
   };
   return &backend;
}

/* Used by G7b's queue as it makes a context; kept here beside the table
 * the points read. */
bool anv_kosmos_fence_register(uint32_t context, const void *map);
bool
anv_kosmos_fence_register(uint32_t context, const void *map)
{
   bool ok;
   simple_mtx_lock(&lock);
   ok = grow((void **)&fences, &fence_room, fence_count + 1, sizeof(*fences));
   if (ok)
      fences[fence_count++] = (struct kosmos_fence){ context, map };
   simple_mtx_unlock(&lock);
   return ok;
}
