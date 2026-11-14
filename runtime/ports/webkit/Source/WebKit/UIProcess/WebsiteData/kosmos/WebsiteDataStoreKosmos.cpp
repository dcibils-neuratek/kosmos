/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Where a browser keeps what it keeps** (`docs/w7.md` W7b): cookies, the
 * cache and a site's storage go under `/Home/Settings/JetSki/`, which only
 * the network process can reach - a web process is not handed `/Home`.
 * A data store given its own folders keeps them.
 */

#include "config.h"
#include "WebsiteDataStore.h"

#include <wtf/FileSystem.h>

namespace WebKit {

static String kosmosBrowserFolder(const String& base, const String& directoryName, ASCIILiteral fallback)
{
    if (!base.isNull())
        return FileSystem::pathByAppendingComponent(base, directoryName);
    return FileSystem::pathByAppendingComponent(String(fallback), directoryName);
}

void WebsiteDataStore::platformInitialize()
{
}

void WebsiteDataStore::platformDestroy()
{
}

String WebsiteDataStore::cacheDirectoryFileSystemRepresentation(const String& directoryName, const String& baseCacheDirectory, ShouldCreateDirectory)
{
    return kosmosBrowserFolder(baseCacheDirectory, directoryName, "/Home/Settings/JetSki/Cache"_s);
}

String WebsiteDataStore::websiteDataDirectoryFileSystemRepresentation(const String& directoryName, const String& baseDataDirectory, ShouldCreateDirectory)
{
    return kosmosBrowserFolder(baseDataDirectory, directoryName, "/Home/Settings/JetSki/Data"_s);
}

UnifiedOriginStorageLevel WebsiteDataStore::defaultUnifiedOriginStorageLevel()
{
    return UnifiedOriginStorageLevel::None;
}

} // namespace WebKit
