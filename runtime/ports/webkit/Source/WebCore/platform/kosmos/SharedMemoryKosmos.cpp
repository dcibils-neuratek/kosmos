/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **WebCore's shared memory on Nebula regions** (`docs/w6.md` W6b), what
 * `SharedMemoryUnix.cpp` is with `memfd_create` and `mmap`: a region made
 * with `kosmos_mem_create`, mapped with `kosmos_mem_map`, let go of with
 * `kosmos_share_unmap`, and handed on by its capability (`KosmosRegion.h`).
 *
 * **A read-only handle is a read-only capability** (`docs/w7.md` W7a):
 * `kosmos_mem_readonly` makes a second capability to the region that
 * Nebula maps without write access wherever it goes, so a process handed
 * one cannot write the pages however it maps them - which a file
 * descriptor opened read-only does not promise WebKit's Unix port
 * (webkit.org/b/131542).
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

auto SharedMemory::createHandle(Protection protection) -> std::optional<Handle>
{
    if (!m_region)
        return std::nullopt;
    if (protection == Protection::ReadWrite)
        return { Handle(KosmosRegion { m_region }, m_size) };
    auto readOnly = KosmosRegion::adopt(kosmos_mem_readonly(m_region.capability()));
    if (!readOnly)
        return std::nullopt;
    return { Handle(WTF::move(readOnly), m_size) };
}

} // namespace WebCore
