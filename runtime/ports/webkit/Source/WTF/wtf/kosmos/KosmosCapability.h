/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A Nebula capability, as WebKit holds one** (`docs/w7.md` W7a): an index
 * into this process's table naming a region or an endpoint, which is what
 * a file descriptor is to WebKit's Unix ports - a shared memory's handle,
 * an IPC attachment, a connection's end, a semaphore.
 *
 * WebKit copies these: a shared memory handle's copy constructor, an
 * attachment kept by one message and sent in another. A file descriptor is
 * duplicated for that. Nebula has no duplicate and needs none - a
 * capability sent with a message is *copied* to the receiver, the sender
 * keeping its own (`kernel/ipc.h`) - so one index in this process serves
 * every copy, and KosmosCapability is a counted reference to one index,
 * dropped with the last reference.
 *
 * It began in WebCore as `KosmosRegion` (W6b), for shared memory alone;
 * W7's connection holds endpoints in the same way, so it is WTF's, and
 * `KosmosRegion` is this under its first name.
 */

#pragma once

#include <atomic>
#include <wtf/RefPtr.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace WTF {

class KosmosCapability {
public:
    KosmosCapability() = default;
    // Adopts the index: it is dropped with the last copy. A negative index
    // is none.
    WTF_EXPORT_PRIVATE static KosmosCapability adopt(long index);

    explicit operator bool() const { return !!m_capability; }
    // The index, or negative when there is none.
    long capability() const { return m_capability ? m_capability->index.load() : -1; }

    // Ends the endpoint this names, for every holder: whatever waits on it
    // wakes with an error. Nebula empties this process's slot as it does,
    // so every copy here forgets the index rather than dropping it later,
    // when the number may name something else.
    WTF_EXPORT_PRIVATE void destroyEndpoint() const;

private:
    struct Capability : ThreadSafeRefCounted<Capability> {
        explicit Capability(long i) : index(i) { }
        WTF_EXPORT_PRIVATE ~Capability();
        std::atomic<long> index;
    };
    RefPtr<Capability> m_capability;
};

} // namespace WTF

using WTF::KosmosCapability;
