/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A capability in a WebKit message** (`docs/w7.md` W7a), as
 * `ArgumentCodersUnix.h` codes a file descriptor: an attachment, which the
 * connection sends by a call ahead of the message (`ConnectionKosmos.cpp`).
 */

#pragma once

#include <wtf/ArgumentCoder.h>

namespace WTF {
class KosmosCapability;
}

namespace IPC {

template<> struct ArgumentCoder<WTF::KosmosCapability> {
    static void encode(Encoder&, const WTF::KosmosCapability&);
    static void encode(Encoder&, WTF::KosmosCapability&&);
    static std::optional<WTF::KosmosCapability> decode(Decoder&);
};

}
