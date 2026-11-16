/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* The JIT's pool, one region mapped twice (`KosmosJITPool.h`). */

#include "config.h"
#include <wtf/kosmos/KosmosJITPool.h>

#include <atomic>
#include <stdio.h>
#include <kosmos.h>

namespace WTF {

static long s_region = -1;
static std::atomic<size_t> s_written { 0 };

KosmosJITPool kosmosMakeJITPool(size_t bytes, size_t pageSize)
{
    long region = kosmos_mem_create_flags(bytes / pageSize, MEM_ON_TOUCH);
    if (region < 0)
        return { };
    long executable = kosmos_mem_map_rx(region);
    long writable = kosmos_mem_map(region);
    // The capability is kept, never dropped: the pool lives as long as the
    // process, and what a drop does to a region still mapped is not this
    // file's to rely on.
    if (executable <= 0 || writable <= 0)
        return { };
    s_region = region;
    // Said once a process, so a log shows which processes compile code.
    fprintf(stderr, "jit: a pool of %zu MB, run at %#lx and written at %#lx\n",
        bytes >> 20, static_cast<unsigned long>(executable), static_cast<unsigned long>(writable));
    return { reinterpret_cast<void*>(executable), static_cast<uintptr_t>(writable) };
}

long kosmosJITPoolPagesWritten()
{
    return s_region < 0 ? -1 : kosmos_mem_touched(s_region);
}

void kosmosJITPoolWrote(size_t bytes)
{
    s_written.fetch_add(bytes, std::memory_order_relaxed);
}

size_t kosmosJITPoolBytesWritten()
{
    return s_written.load(std::memory_order_relaxed);
}

} // namespace WTF
