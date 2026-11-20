/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_GPU_H
#define KOSMOS_GPU_H

/*
 * **The GPU Kit** (`docs/gpu3d.md` G7a): `/Devices/gpu` for C, as
 * `user/lib/gpu.lua` is for Lua. What a program needs of the GPU's driver
 * to run work of its own on it - the device it is, contexts, regions bound
 * at addresses it chose, batches, and the timeline each context keeps -
 * spoken in `gpuproto.h`'s declared shapes.
 *
 * Its first user is Mesa's `anv`, whose Kosmos kernel-driver type is this
 * and nothing else: the `fd` Mesa carries is `kg_device()`'s capability.
 * Nothing here knows what a Vulkan object is, so anything else that wants
 * the GPU - the Compute Kit, a test - stands on the same door.
 *
 * Every verb answers a `GPU_ERR_*` (GPU_OK is nought), or KG_NO_ANSWER
 * when the driver could not be asked: no `/Devices/gpu` in this process's
 * namespace, or a reply of the wrong size.
 */

#include <stdbool.h>
#include <stdint.h>

#include <gpuproto.h>

#define KG_NO_ANSWER 0xffffffffu

/* `/Devices/gpu`, as this process holds it, or -1. Asked once. */
long kg_device(void);

/* What the GPU is and is doing: `struct gpu_stats`'s snapshot. */
uint32_t kg_stats(long gpu, struct gpu_stats *out);

/* A context on `engine` (GPU_ENGINE_*): its id, and the read-only region
 * whose first qword is its timeline's value reached. */
uint32_t kg_context_new(long gpu, uint32_t engine, uint32_t *id, long *fence);
uint32_t kg_context_forget(long gpu, uint32_t context);

/* `region`'s pages from `first_page`, `pages` of them (0: the rest),
 * entered at `at` in the context's page table, cached as `cache`
 * (GPU_CACHE_*). The program keeps its own hold on the region; the driver
 * takes one of its own while it is bound. */
uint32_t kg_bind(long gpu, uint32_t context, uint64_t at, long region,
                 uint32_t first_page, uint32_t pages, uint32_t cache);
uint32_t kg_unbind(long gpu, uint32_t context, uint64_t at);

/* A wait on another of this program's contexts reaching a value. */
struct kg_wait {
    uint32_t context;
    uint64_t value;
};

/* The batch at `batch`, `bytes` long, run once every wait is met, and the
 * timeline written as `signal` when it is done. Answered at once. */
uint32_t kg_exec(long gpu, uint32_t context, uint64_t batch, uint32_t bytes,
                 uint64_t signal, const struct kg_wait *waits, uint32_t nwaits);

/* A pulse of `code` to `endpoint` once the timeline reaches `value`. */
uint32_t kg_fence_wait(long gpu, uint32_t context, uint64_t value,
                       long endpoint, uint32_t code);

/* What a context has come to. */
struct kg_state {
    uint64_t reached, given;
    uint32_t hangs, faults, fault_word;
};
uint32_t kg_context_state(long gpu, uint32_t context, struct kg_state *out);

/* The value a context's timeline has reached, read from its fence region
 * as mapped by `kg_fence_map`. */
void *kg_fence_map(long fence);
static inline uint64_t kg_reached(const void *fence_map)
{
    return __atomic_load_n((const uint64_t *)fence_map, __ATOMIC_ACQUIRE);
}

/* A sentence for an error. */
const char *kg_why(uint32_t error);

#endif /* KOSMOS_GPU_H */
