/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A Nebula region, as WebCore's shared memory holds it** (`docs/w6.md`
 * W6b; `0014-shared-memory-on-nebula.patch`): a counted capability, WTF's
 * `KosmosCapability` under the name it had first. It was this file's own
 * class until W7, when WebKit2's connection needed to hold endpoints the
 * same way, and one class for both is the one there should be
 * (`wtf/kosmos/KosmosCapability.h` says why it is counted).
 */

#pragma once

#include <wtf/kosmos/KosmosCapability.h>

namespace WebCore {

using KosmosRegion = WTF::KosmosCapability;

} // namespace WebCore
