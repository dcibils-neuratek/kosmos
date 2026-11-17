/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The hooks `KosmosStore.h` describes, set once by the WebKit Kit on the
 * UI process's main thread, which is the only thread that makes or drops
 * a backing store.
 */

#include "config.h"
#include "KosmosStore.h"

namespace WebKit {

bool kosmosStoreForgets;

static bool (*storeMake)(size_t, void**, long*);
static void (*storeRelease)(void*, size_t, long);

void kosmosOnStoreMemory(bool (*make)(size_t, void**, long*), void (*release)(void*, size_t, long))
{
    storeMake = make;
    storeRelease = release;
}

bool kosmosStoreMake(size_t bytes, KosmosStoreMemory* out)
{
    if (!storeMake || !storeRelease)
        return false;
    out->bytes = bytes;
    return storeMake(bytes, &out->at, &out->region) && out->at;
}

void kosmosStoreRelease(const KosmosStoreMemory& memory)
{
    if (storeRelease && memory.at)
        storeRelease(memory.at, memory.bytes, memory.region);
}

} // namespace WebKit
