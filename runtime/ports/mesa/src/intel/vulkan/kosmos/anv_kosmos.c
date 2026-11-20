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
 * 3. **The buffer objects**: regions, created, mapped and dropped here,
 *    and bound in the device's context at the address `anv` chose.
 *
 * 4. **The logical device and its queue** (G7b): the device makes one
 *    context on the render engine - its page table is the device's
 *    address space, Xe's VM - and every queue submits to it, so `anv`'s
 *    buffer objects, bound before any queue exists, are where each
 *    queue's batches look. `anv` offers one queue without VM control
 *    (`has_vm_control` false), so one context is exact rather than a
 *    limit. A submission is `GPU_OP_EXEC` with the next value of the
 *    context's timeline, which every signalled point is given.
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
#include "util/stack_array.h"
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
   uint64_t given;          /* the last value a submission was given */
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

/* A context's entry; NULL for one not known. Called with the lock held. */
static struct kosmos_fence *
entry_of(uint32_t context)
{
   for (uint32_t i = 0; i < fence_count; i++) {
      if (fences[i].context == context)
         return &fences[i];
   }
   return NULL;
}

/* The page a context's timeline is read from; NULL for one not known. */
static const void *
fence_of(uint32_t context)
{
   const struct kosmos_fence *f = entry_of(context);
   return f != NULL ? f->map : NULL;
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
   /* PENDING asks only that a submission will signal it - what Mesa's
    * submit thread waits for before it hands on a wait. */
   const bool pending = (flags & VK_SYNC_WAIT_PENDING) != 0;
   long gpu = kg_device();
   long endpoint = -1;
   bool *asked = NULL;
   VkResult result = VK_SUCCESS;
   (void)device;

   for (;;) {
      uint32_t reached = 0;

      simple_mtx_lock(&lock);
      for (uint32_t i = 0; i < count; i++) {
         const struct kosmos_point *p = to_point(waits[i].sync);
         if (pending ? p->cpu_signaled || p->submitted : point_reached_locked(p))
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
         if (!pending && !asked[i] && p->submitted && !p->cpu_signaled) {
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

/* The device's context's fence entry registered; false when out of memory. */
static bool
fence_register(uint32_t context, const void *map)
{
   bool ok;
   simple_mtx_lock(&lock);
   ok = grow((void **)&fences, &fence_room, fence_count + 1, sizeof(*fences));
   if (ok)
      fences[fence_count++] = (struct kosmos_fence){ context, map, 0 };
   simple_mtx_unlock(&lock);
   return ok;
}

static void
fence_forget(uint32_t context)
{
   simple_mtx_lock(&lock);
   for (uint32_t i = 0; i < fence_count; i++) {
      if (fences[i].context == context) {
         fences[i] = fences[--fence_count];
         break;
      }
   }
   simple_mtx_unlock(&lock);
}

/* Until the context has done everything it was given, or it has hung: a
 * thread's own endpoint and the driver's pulse, never a loop on a clock. */
static VkResult
context_idle(struct anv_device *device)
{
   const uint32_t context = device->vm_id;
   const void *map;
   uint64_t given;

   simple_mtx_lock(&lock);
   struct kosmos_fence *f = entry_of(context);
   map = f != NULL ? f->map : NULL;
   given = f != NULL ? f->given : 0;
   simple_mtx_unlock(&lock);

   if (map == NULL || kg_reached(map) >= given)
      return VK_SUCCESS;

   long endpoint = kosmos_endpoint();
   if (endpoint < 0)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = VK_SUCCESS;
   if (kg_fence_wait(kg_device(), context, given, endpoint, 1) != GPU_OK) {
      result = vk_device_set_lost(&device->vk, "the GPU would not wait for its context");
   } else {
      while (kg_reached(map) < given) {
         struct message msg;
         uint64_t sender;
         (void)kosmos_receive(endpoint, &msg, &sender, 0, 0);
      }
   }
   (void)kosmos_endpoint_destroy(endpoint);
   return result;
}

VkResult
anv_kosmos_device_setup(struct anv_device *device)
{
   uint32_t context;
   long fence;
   uint32_t error = kg_context_new(kg_device(), GPU_ENGINE_RENDER, &context, &fence);

   if (error != GPU_OK) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "the GPU gave no context: %s", kg_why(error));
   }

   const void *map = kg_fence_map(fence);
   if (map == NULL || !fence_register(context, map)) {
      (void)kg_context_forget(kg_device(), context);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   device->vm_id = context;
   return VK_SUCCESS;
}

bool
anv_kosmos_device_destroy(struct anv_device *device)
{
   /* Its work finished first: forgetting a context the GPU still runs
    * would be refused, and its regions held. */
   (void)context_idle(device);
   fence_forget(device->vm_id);
   return kg_context_forget(kg_device(), device->vm_id) == GPU_OK;
}

VkResult
anv_kosmos_device_check_status(struct vk_device *vk_device)
{
   struct anv_device *device = container_of(vk_device, struct anv_device, vk);
   struct kg_state state;

   if (kg_context_state(kg_device(), device->vm_id, &state) != GPU_OK)
      return vk_device_set_lost(vk_device, "the GPU no longer knows the device's context");
   if (state.hangs != 0)
      return vk_device_set_lost(vk_device, "the GPU hung, %u times, and was reset", state.hangs);
   if (state.faults != 0) {
      return vk_device_set_lost(vk_device, "the GPU refused a command (%08x), %u times",
                                state.fault_word, state.faults);
   }
   return VK_SUCCESS;
}

VkResult
anv_kosmos_create_engine(struct anv_device *device, struct anv_queue *queue,
                         const VkDeviceQueueCreateInfo *pCreateInfo)
{
   /* **A priority above the device's is refused**, as Xe's and i915's
    * backends refuse it (dEQP-VK's `create_device_global_priority*`,
    * `testing.md` 18.934). The device's context runs at the one priority
    * there is, MEDIUM, and the spec lets a driver deny anything higher
    * with VK_ERROR_NOT_PERMITTED; LOW is taken and runs as MEDIUM. Bands
    * for a context are G9's. */
   const VkDeviceQueueGlobalPriorityCreateInfo *asked =
      vk_find_struct_const(pCreateInfo->pNext, DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO);

   if (asked != NULL &&
       asked->globalPriority > device->physical->max_context_priority)
      return vk_error(device, VK_ERROR_NOT_PERMITTED);

   /* The render engine is the one family offered (`intel_engine.c`, and
    * no VM control); its queues share the device's context. */
   if (queue->family->engine_class != INTEL_ENGINE_CLASS_RENDER) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "only the render engine takes a queue on Kosmos");
   }
   queue->exec_queue_id = device->vm_id;
   return VK_SUCCESS;
}

void
anv_kosmos_destroy_engine(struct anv_device *device, struct anv_queue *queue)
{
   (void)device;
   (void)queue;
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


/*
 * A region's one mapping, and how many maps share it. Nebula maps a region
 * whole, at an address of its choosing, and has no map of a piece of one -
 * but anv maps pieces: a small buffer sub-allocated from a slab is mapped
 * as its slab at the buffer's offset (`anv_device_map_bo`). So a region is
 * mapped once, the first time anything in it is, each map is its address
 * plus the offset, and it leaves the share window with its last map - one
 * mapping a slab rather than one a buffer, since a slab holds hundreds.
 * The table grows and is searched in order: maps are few - buffers the
 * processor reads, not every buffer - and a map is not on a frame's path.
 */
struct kosmos_mapping {
   uint32_t handle;           /* the region's gem handle; 0 is a free slot */
   uintptr_t base;
   uint64_t pages;
   uint32_t users;
};

static simple_mtx_t maps_lock = SIMPLE_MTX_INITIALIZER;
static struct kosmos_mapping *maps;
static uint32_t maps_count;

static struct kosmos_mapping *
mapping_slot(uint32_t handle)
{
   struct kosmos_mapping *free_slot = NULL;

   for (uint32_t i = 0; i < maps_count; i++) {
      if (maps[i].handle == handle)
         return &maps[i];
      if (maps[i].handle == 0 && free_slot == NULL)
         free_slot = &maps[i];
   }
   if (free_slot != NULL)
      return free_slot;

   uint32_t grown = maps_count ? maps_count * 2 : 16;
   struct kosmos_mapping *more = realloc(maps, grown * sizeof(*more));
   if (more == NULL)
      return NULL;
   memset(more + maps_count, 0, (grown - maps_count) * sizeof(*more));
   maps = more;
   free_slot = &maps[maps_count];
   maps_count = grown;
   return free_slot;
}

static void *
kosmos_gem_mmap(struct anv_device *device, struct anv_bo *bo,
                uint64_t offset, uint64_t size, void *placed_addr)
{
   (void)device;

   /* At an address of the program's choosing (VK_EXT_map_memory_placed)
    * is not offered. */
   if (placed_addr != NULL || bo->gem_handle == 0) {
      errno = EINVAL;
      return MAP_FAILED;
   }

   long cap = (long)bo->gem_handle - 1;
   void *map = MAP_FAILED;

   simple_mtx_lock(&maps_lock);
   struct kosmos_mapping *m = mapping_slot(bo->gem_handle);
   if (m == NULL) {
      errno = ENOMEM;
   } else if (m->handle == 0) {
      long pages = kosmos_mem_size(cap);
      long at = pages > 0 ? kosmos_mem_map(cap) : -1;
      if (at < 0) {
         errno = ENOMEM;
      } else {
         *m = (struct kosmos_mapping){ bo->gem_handle, (uintptr_t)at, (uint64_t)pages, 0 };
      }
   }
   if (m != NULL && m->handle != 0) {
      if (offset + size > m->pages * PAGE) {
         errno = EINVAL;
         if (m->users == 0) {
            (void)kosmos_share_unmap(m->base, m->pages);
            m->handle = 0;
         }
      } else {
         m->users++;
         map = (void *)(m->base + offset);
      }
   }
   simple_mtx_unlock(&maps_lock);
   return map;
}

void
anv_kosmos_unmap(void *map, uint64_t size)
{
   uintptr_t at = (uintptr_t)map;

   simple_mtx_lock(&maps_lock);
   for (uint32_t i = 0; i < maps_count; i++) {
      struct kosmos_mapping *m = &maps[i];
      if (m->handle != 0 && at >= m->base && at + size <= m->base + m->pages * PAGE) {
         if (--m->users == 0) {
            (void)kosmos_share_unmap(m->base, m->pages);
            m->handle = 0;
         }
         break;
      }
   }
   simple_mtx_unlock(&maps_lock);
}

/* A region closed while still mapped - which anv does not do - leaves the
 * share window with it, so the handle, a capability index the next region
 * may be given, never finds a stale mapping. */
static void
kosmos_gem_close(struct anv_device *device, struct anv_bo *bo)
{
   (void)device;
   if (bo->gem_handle == 0)
      return;

   simple_mtx_lock(&maps_lock);
   for (uint32_t i = 0; i < maps_count; i++) {
      if (maps[i].handle == bo->gem_handle) {
         (void)kosmos_share_unmap(maps[i].base, maps[i].pages);
         maps[i].handle = 0;
      }
   }
   simple_mtx_unlock(&maps_lock);
   (void)kosmos_cap_drop((long)bo->gem_handle - 1);
}

static VkResult
kosmos_vm_bind(struct anv_device *device, struct anv_sparse_submission *submit,
               enum anv_vm_bind_flags flags)
{
   (void)flags;

   /* A null binding - the device's null-initialised heap, made to read
    * zeros where the GPU prefetches past a buffer's end - is accepted and
    * does nothing, as i915's backend does: what the driver's page table
    * holds where nothing is bound is the driver's to say. Memory bound
    * piece by piece, sparse binding, is not offered. */
   for (uint32_t i = 0; i < submit->binds_len; i++) {
      if (submit->binds[i].bo != NULL) {
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "sparse binding is not offered on Kosmos");
      }
   }
   return VK_SUCCESS;
}

static VkResult
kosmos_vm_bind_bo(struct anv_device *device, struct anv_bo *bo)
{
   /* Write-back in the LLC, which the HD 530 shares with the processor:
    * what `anv`'s coherent, cached memory types promise. */
   uint32_t error = kg_bind(kg_device(), device->vm_id, intel_48b_address(bo->offset),
                            (long)bo->gem_handle - 1, 0, 0, GPU_CACHE_WB);

   if (error != GPU_OK) {
      return vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "a buffer of %"PRIu64" bytes at 0x%"PRIx64" was not bound: %s",
                       bo->size, bo->offset, kg_why(error));
   }
   return VK_SUCCESS;
}

static VkResult
kosmos_vm_unbind_bo(struct anv_device *device, struct anv_bo *bo)
{
   const uint64_t at = intel_48b_address(bo->offset);
   uint32_t error = kg_unbind(kg_device(), device->vm_id, at);

   /* Busy is the context running other work, which may still be reading
    * nothing of this buffer's - Vulkan says it is done with - but the
    * driver unbinds only an idle context. So: idle, then again. */
   if (error == GPU_ERR_BUSY) {
      VkResult result = context_idle(device);
      if (result != VK_SUCCESS)
         return result;
      error = kg_unbind(kg_device(), device->vm_id, at);
   }

   if (error != GPU_OK) {
      return vk_errorf(device, VK_ERROR_UNKNOWN, "a buffer at 0x%"PRIx64" was not unbound: %s",
                       bo->offset, kg_why(error));
   }
   return VK_SUCCESS;
}

/* --- Submission ------------------------------------------------------------- */

/*
 * One submission to the device's context: `batch` (0: none), after the
 * waits, and every signal made a point at the value it was given.
 *
 * A wait already reached, or one on this context - which runs in order -
 * needs nothing. One on another context is handed to the driver, which
 * holds the batch until it is met; past the four a request carries, the
 * rest are waited for here first. A submission with no batch and nothing
 * to wait for signals at the value last given, which is reached exactly
 * when everything before it is done.
 */
static VkResult
kosmos_submit(struct anv_device *device, struct anv_bo *batch,
              uint32_t wait_count, const struct vk_sync_wait *waits,
              uint32_t signal_count, const struct vk_sync_signal *signals)
{
   const uint32_t context = device->vm_id;
   struct kg_wait handed[GPU_EXEC_WAITS];
   uint32_t nhanded = 0;

   for (uint32_t i = 0; i < wait_count; i++) {
      const struct kosmos_point *p = to_point(waits[i].sync);
      bool here = false;

      simple_mtx_lock(&lock);
      if (point_reached_locked(p) || (p->submitted && p->context == context)) {
         here = true;
      } else if (p->submitted && nhanded < GPU_EXEC_WAITS) {
         handed[nhanded++] = (struct kg_wait){ p->context, p->value };
         here = true;
      }
      simple_mtx_unlock(&lock);

      if (!here) {
         VkResult result = vk_sync_wait(&device->vk, waits[i].sync, 0,
                                        VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
         if (result != VK_SUCCESS)
            return result;
      }
   }

   if (batch == NULL && nhanded != 0)
      batch = device->trivial_batch_bo;

   uint64_t value;
   simple_mtx_lock(&lock);
   struct kosmos_fence *f = entry_of(context);
   if (f == NULL) {
      simple_mtx_unlock(&lock);
      return vk_device_set_lost(&device->vk, "the device's context is gone");
   }

   if (batch != NULL) {
      value = f->given + 1;
      uint32_t error = kg_exec(kg_device(), context, intel_48b_address(batch->offset),
                               (uint32_t)batch->size, value, handed, nhanded);
      if (error != GPU_OK) {
         simple_mtx_unlock(&lock);
         return vk_device_set_lost(&device->vk, "the GPU refused a submission: %s",
                                   kg_why(error));
      }
      f->given = value;
   } else {
      value = f->given;
   }

   for (uint32_t i = 0; i < signal_count; i++) {
      struct kosmos_point *p = to_point(signals[i].sync);
      if (value == 0) {
         p->cpu_signaled = true;      /* nothing was ever given: done */
      } else {
         p->submitted = true;
         p->context = context;
         p->value = value;
      }
   }
   /* A thread waiting for one of these to be submitted looks again. */
   if (signal_count != 0)
      wake_waiters_locked();
   simple_mtx_unlock(&lock);
   return VK_SUCCESS;
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
   struct anv_device *device = queue->device;
   struct anv_bo *batch = NULL;
   (void)perf_query_pass;

   /* No performance counters here (the OA unit is the driver's), and no
    * trace points: both are offered by neither the device nor the build. */
   if (perf_query_pool != NULL)
      return vk_error(queue, VK_ERROR_FEATURE_NOT_PRESENT);
   (void)utrace_submit;

   if (cmd_buffer_count != 0) {
      anv_cmd_buffer_chain_command_buffers(cmd_buffers, cmd_buffer_count);
      struct anv_batch_bo *first = list_first_entry(&cmd_buffers[0]->batch_bos,
                                                    struct anv_batch_bo, link);
      batch = first->bo;
   }

   VkResult result = kosmos_submit(device, batch, wait_count, waits, signal_count, signals);
   return anv_queue_post_submit(queue, result);
}

static VkResult
kosmos_queue_exec_async(struct anv_async_submit *submit,
                        uint32_t wait_count,
                        const struct vk_sync_wait *waits,
                        uint32_t signal_count,
                        const struct vk_sync_signal *signals)
{
   struct anv_queue *queue = submit->queue;
   struct anv_device *device = queue->device;
   STACK_ARRAY(struct vk_sync_signal, all, signal_count + 1);
   uint32_t n = 0;

   for (uint32_t i = 0; i < signal_count; i++)
      all[n++] = signals[i];
   if (submit->signal.sync != NULL)
      all[n++] = submit->signal;

   struct anv_bo *batch = util_dynarray_num_elements(&submit->batch_bos, struct anv_bo *) != 0
      ? *util_dynarray_element(&submit->batch_bos, struct anv_bo *, 0) : NULL;

   VkResult result = kosmos_submit(device, batch, wait_count, waits, n, all);
   STACK_ARRAY_FINISH(all);
   return anv_queue_post_submit(queue, result);
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
      .vm_unbind_bo = kosmos_vm_unbind_bo,
      .queue_exec_locked = kosmos_queue_exec_locked,
      .queue_exec_async = kosmos_queue_exec_async,
      .bo_alloc_flags_to_bo_flags = kosmos_bo_alloc_flags_to_bo_flags,
   };
   return &backend;
}
