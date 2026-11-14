/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **No injected bundle.** A bundle is a library a web process loads by
 * path at run time, and nothing on Kosmos is linked dynamically. A web
 * process asked for one says it has none, and WebKit goes on without it.
 */

#include "config.h"
#include "InjectedBundle.h"

namespace WebKit {

bool InjectedBundle::initialize(const WebProcessCreationParameters&, RefPtr<API::Object>&&)
{
    return false;
}

void InjectedBundle::setBundleParameter(const String&, std::span<const uint8_t>)
{
}

void InjectedBundle::setBundleParameters(std::span<const uint8_t>)
{
}

} // namespace WebKit
