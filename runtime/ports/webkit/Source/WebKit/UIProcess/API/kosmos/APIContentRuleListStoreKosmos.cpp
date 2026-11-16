/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **No default store for content rule lists** (`docs/adblock.md`, B1).
 * The WebKit Kit names its own, `/Home/Settings/Filters/Rules`, through
 * `WKUserContentExtensionStoreCreate`, so nothing asks for a default -
 * as GTK's and WPE's ports have it.
 */

#include "config.h"
#include "APIContentRuleListStore.h"

namespace API {

#if ENABLE(CONTENT_EXTENSIONS)
WTF::String ContentRuleListStore::defaultStorePath()
{
    ASSERT_NOT_REACHED();
    return { };
}
#endif

} // namespace API
