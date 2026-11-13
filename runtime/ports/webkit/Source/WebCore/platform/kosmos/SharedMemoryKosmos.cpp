/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **WebCore's shared memory on Nebula regions** (`docs/w6.md` W6b), what
 * `SharedMemoryUnix.cpp` is with `memfd_create` and `mmap`: a region made
 * with `kosmos_mem_create`, mapped with `kosmos_mem_map`, let go of with
 * `kosmos_share_unmap`, and handed on by its capability (`KosmosRegion.h`).
 *
 * **Protection is not enforced yet.** Nebula maps a region read and write
 * or read and execute, and a read-only handle is mapped read and write,
 * as WebKit's Unix port does with a read-only file descriptor
 * (webkit.org/b/131542). It matters once a handle crosses to another
 * process, at W7, and Nebula's mapping read-only is on the roadmap for it.
 */

#include "config.h"
#include "SharedMemory.h"

#include <kosmos.h>
#include <wtf/StdLibExtras.h>

namespace WebCore {

static unsigned long pagesFor(size_t bytes)
{
    return (bytes + KOSMOS_PAGE_SIZE - 1) / KOSMOS_PAGE_SIZE;
}

KosmosRegion KosmosRegion::adopt(long capability)
{
    KosmosRegion region;
    if (capability >= 0)
        region.m_capability = adoptRef(*new Capability(capability));
    return region;
}

KosmosRegion::Capability::~Capability()
{
    kosmos_cap_drop(index);
}

RefPtr<SharedMemory> SharedMemory::allocate(size_t size)
{
    if (!size)
        return nullptr;
    long capability = kosmos_mem_create(pagesFor(size));
    if (capability < 0)
        return nullptr;
    auto region = KosmosRegion::adopt(capability);
    long address = kosmos_mem_map(capability);
    if (address < 0)
        return nullptr;

    RefPtr<SharedMemory> instance = adoptRef(new SharedMemory());
    instance->m_data = reinterpret_cast<void*>(address);
    instance->m_size = size;
    instance->m_region = WTF::move(region);
    return instance;
}

RefPtr<SharedMemory> SharedMemory::map(Handle&& handle, Protection, CopyOnWrite copyOnWrite)
{
    // Copy-on-write is not supported on Unix either (webkit.org/b/305633).
    ASSERT_UNUSED(copyOnWrite, copyOnWrite == CopyOnWrite::No);

    long pages = kosmos_mem_size(handle.m_handle.capability());
    if (pages < 0 || handle.size() > static_cast<uint64_t>(pages) * KOSMOS_PAGE_SIZE)
        return nullptr;
    long address = kosmos_mem_map(handle.m_handle.capability());
    if (address < 0)
        return nullptr;

    RefPtr<SharedMemory> instance = adoptRef(new SharedMemory());
    instance->m_data = reinterpret_cast<void*>(address);
    instance->m_size = handle.size();
    instance->m_region = WTF::move(handle.m_handle);
    return instance;
}

SharedMemory::~SharedMemory()
{
    kosmos_share_unmap(reinterpret_cast<unsigned long>(m_data), pagesFor(m_size));
}

auto SharedMemory::createHandle(Protection) -> std::optional<Handle>
{
    if (!m_region)
        return std::nullopt;
    return { Handle(KosmosRegion { m_region }, m_size) };
}

} // namespace WebCore
