/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A mapped file, handed to another process** (`docs/adblock.md`, B1).
 * A compiled content rule list is a file the UI process maps, and every
 * web process matches its requests against those bytes; WebKit hands them
 * over as shared memory. On a Unix port the file's own mapping is shared;
 * on Windows, its mapping handle. A Kosmos region is memory Nebula made,
 * not a file, so the bytes are copied into one, once a list is loaded -
 * and from there every web process maps the same region, read only.
 * Curl's `NetworkCacheDataCurl.cpp` defines this for Windows alone.
 */

#include "config.h"
#include "NetworkCacheData.h"

#include <WebCore/SharedMemory.h>
#include <wtf/StdLibExtras.h>

namespace WebKit {
namespace NetworkCache {

RefPtr<WebCore::SharedMemory> Data::tryCreateSharedMemory() const
{
    if (isNull() || !size())
        return nullptr;

    auto memory = WebCore::SharedMemory::allocate(size());
    if (!memory)
        return nullptr;
    memcpySpan(memory->mutableSpan(), span());
    return memory;
}

} // namespace NetworkCache
} // namespace WebKit
