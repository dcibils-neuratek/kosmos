/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A Nebula region, as WebCore's shared memory holds it** (`docs/w6.md`
 * W6b; `0014-shared-memory-on-nebula.patch`). A region is named by a
 * capability, an index into this process's table; the region lives until
 * the last capability to it is dropped.
 *
 * WebCore copies a handle - `SharedMemoryHandle`'s copy constructor - and a
 * file descriptor is duplicated for it on Unix. Nebula has no duplicate
 * and needs none: a capability sent with a message is *copied* to the
 * receiver, the sender keeping its own (`kernel/ipc.h`), so one index in
 * this process serves every copy of the handle here. KosmosRegion is
 * therefore a counted reference to one capability, and the capability is
 * dropped with the last reference.
 */

#pragma once

#include <wtf/RefPtr.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace WebCore {

class KosmosRegion {
public:
    KosmosRegion() = default;
    // Adopts the capability: it is dropped with the last copy.
    static KosmosRegion adopt(long capability);

    explicit operator bool() const { return !!m_capability; }
    // The capability's index, or negative when there is none.
    long capability() const { return m_capability ? m_capability->index : -1; }

private:
    struct Capability : ThreadSafeRefCounted<Capability> {
        explicit Capability(long i) : index(i) { }
        ~Capability();
        long index;
    };
    RefPtr<Capability> m_capability;
};

} // namespace WebCore
