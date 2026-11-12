/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **WTF's memory footprint on Kosmos** (`docs/w4.md` W4c): what this
 * process holds, as Nebula's process table says it - the POSIX layer's
 * `posix_footprint_np`. Linux's port reads /proc/self for the same
 * figure; the generic one asks `memoryStatus`, which only Linux, FreeBSD
 * and iOS have.
 */

#include "config.h"
#include <wtf/MemoryFootprint.h>

#include <sys/resource.h>

namespace WTF {

size_t memoryFootprint()
{
    return posix_footprint_np();
}

} // namespace WTF
