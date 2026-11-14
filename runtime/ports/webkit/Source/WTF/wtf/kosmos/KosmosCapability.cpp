/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* A Nebula capability, counted (`KosmosCapability.h`). */

#include "config.h"
#include <wtf/kosmos/KosmosCapability.h>

#include <kosmos.h>

namespace WTF {

KosmosCapability KosmosCapability::adopt(long index)
{
    KosmosCapability capability;
    if (index >= 0)
        capability.m_capability = adoptRef(*new Capability(index));
    return capability;
}

KosmosCapability::Capability::~Capability()
{
    long i = index.load();
    if (i >= 0)
        kosmos_cap_drop(i);
}

void KosmosCapability::destroyEndpoint() const
{
    if (!m_capability)
        return;
    long i = m_capability->index.exchange(-1);
    if (i >= 0)
        kosmos_endpoint_destroy(i);
}

} // namespace WTF
