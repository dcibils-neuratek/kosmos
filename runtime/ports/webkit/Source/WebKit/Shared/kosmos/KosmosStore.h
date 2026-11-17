/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Where the UI process's backing store lives** (`webkit.md` *Fast* 1c):
 * memory the WebKit Kit makes - a region of Nebula's, which the window
 * manager can be handed and shows the page from - so that the page is
 * never copied into JetSki's window. WebKit knows nothing of regions: it
 * asks for bytes and is given an address, and the region rides beside it
 * as a number for whoever shows the buffers.
 *
 * Nothing set (`kosmosOnStoreMemory`), nothing made: the store is one
 * buffer of its own, as it was, and painted into the window.
 */

#pragma once

#include <stddef.h>

namespace WebKit {

struct KosmosStoreMemory {
    void* at { nullptr };
    long region { -1 };
    size_t bytes { 0 };
};

void kosmosOnStoreMemory(bool (*make)(size_t bytes, void** at, long* region),
    void (*release)(void* at, size_t bytes, long region));

bool kosmosStoreMake(size_t bytes, KosmosStoreMemory*);

// **For a suite's control only** (`run_jetski.py`, `forgetting`): the
// buffer not being drawn owes nothing, so it shows what it held before -
// what the suite's pictures must catch. Never set otherwise.
extern bool kosmosStoreForgets;
void kosmosStoreRelease(const KosmosStoreMemory&);

} // namespace WebKit
