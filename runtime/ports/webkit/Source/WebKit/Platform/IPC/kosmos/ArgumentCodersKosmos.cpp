/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A capability in a WebKit message (`ArgumentCodersKosmos.h`). A copy is
 * the same index - Nebula copies a capability to the receiver and leaves
 * the sender its own - so encoding a copy needs nothing duplicated.
 */

#include "config.h"
#include "ArgumentCodersKosmos.h"

#include "Decoder.h"
#include "Encoder.h"
#include <wtf/kosmos/KosmosCapability.h>

namespace IPC {

void ArgumentCoder<KosmosCapability>::encode(Encoder& encoder, const KosmosCapability& capability)
{
    encoder.addAttachment(KosmosCapability { capability });
}

void ArgumentCoder<KosmosCapability>::encode(Encoder& encoder, KosmosCapability&& capability)
{
    encoder.addAttachment(WTF::move(capability));
}

std::optional<KosmosCapability> ArgumentCoder<KosmosCapability>::decode(Decoder& decoder)
{
    return decoder.takeLastAttachment();
}

}
