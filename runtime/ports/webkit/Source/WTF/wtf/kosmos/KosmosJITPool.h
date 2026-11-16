/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The JIT's pool** (`docs/jit.md`): one region, its pages given as they
 * are first touched, mapped twice - read and execute, where compiled code
 * runs, and read and write at another address, where it is written. Nebula
 * never maps a page writable and executable at one address, so this is the
 * only way JavaScriptCore makes code here (patch 0037,
 * `ExecutableAllocator.cpp`).
 *
 * WTF's rather than JavaScriptCore's because WTF's Kosmos files are the ones
 * that speak to Nebula (`kosmos.h`), and JavaScriptCore compiles its files
 * together, where no one of them can be given the kernel's headers alone.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <wtf/ExportMacros.h>

namespace WTF {

struct KosmosJITPool {
    void* executable { nullptr };   // read and execute: where code runs
    uintptr_t writable { 0 };       // read and write: where it is written
};

// A pool of `bytes`, a whole number of pages; both addresses null when
// Nebula refused it.
WTF_EXPORT_PRIVATE KosmosJITPool kosmosMakeJITPool(size_t bytes, size_t pageSize);

// How many of the pool's pages have been written - code made - or -1 when
// there is no pool. What says the JIT ran, from Nebula's side of it.
WTF_EXPORT_PRIVATE long kosmosJITPoolPagesWritten();

// The bytes written into the pool so far, every write the JIT makes - its
// code, and every repatch - counted by the one writer
// (`kosmosWriteToJITRegion`): grows with each compilation, where pages
// grow only when a compilation reaches one not yet touched.
WTF_EXPORT_PRIVATE void kosmosJITPoolWrote(size_t bytes);
WTF_EXPORT_PRIVATE size_t kosmosJITPoolBytesWritten();

} // namespace WTF

using WTF::KosmosJITPool;
using WTF::kosmosMakeJITPool;
using WTF::kosmosJITPoolPagesWritten;
using WTF::kosmosJITPoolWrote;
using WTF::kosmosJITPoolBytesWritten;
