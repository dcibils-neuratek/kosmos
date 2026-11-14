/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The UI process's pool of processes, its platform hooks** (`docs/w7.md`
 * W7a): nothing yet. What a web or network process is handed beyond
 * WebKit's own parameters - its mounts, the network - is handed at
 * `SYS_SPAWN` by the launcher (`ProcessLauncherKosmos.cpp`), not here.
 */

#include "config.h"
#include "WebProcessPool.h"

#include "WebProcessCreationParameters.h"

namespace WebKit {

void WebProcessPool::platformInitialize(NeedsGlobalStaticInitialization)
{
}

void WebProcessPool::platformInitializeNetworkProcess(NetworkProcessCreationParameters&)
{
}

void WebProcessPool::platformInitializeWebProcess(const WebProcessProxy&, WebProcessCreationParameters&)
{
}

void WebProcessPool::platformInvalidateContext()
{
}

void WebProcessPool::platformResolvePathsForSandboxExtensions()
{
}

} // namespace WebKit
