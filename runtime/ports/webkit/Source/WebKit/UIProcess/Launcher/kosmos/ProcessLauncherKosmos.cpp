/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A web or a network process, started** (`docs/w7.md` W7a): this image
 * again, by `SYS_SPAWN`, in a role of its own (`KosmosLaunch.h`), handed
 * its connection and only what its role may reach. A network process gets
 * every mount and the network, since cookies, the cache and certificates
 * are files and pages come over the network; a web process gets the mounts
 * but `/Home`, and no network - what it loads, it loads by asking the
 * network process.
 *
 * Nebula has no paths to images and no command lines, so there is no
 * process path to choose and nothing to parse: the identifier WebKit gave
 * the process travels in the launch page.
 */

#include "config.h"
#include "ProcessLauncher.h"

#include "IPCUtilities.h"
#include "KosmosLaunch.h"
#include <kosmos.h>
#include "netclient.h"
#include <posix_program.h>
#include <wtf/RunLoop.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/CString.h>

namespace WebKit {

static bool reachable(const struct storage_mount& mount, ProcessLauncher::ProcessType type)
{
    if (type == ProcessLauncher::ProcessType::Network)
        return true;
    return strcmp(mount.prefix, "/Home") && strncmp(mount.prefix, "/Home/", 6);
}

void ProcessLauncher::launchProcess()
{
    auto pair = IPC::Connection::createConnectionIdentifierPair();
    if (!pair)
        return;

    bool network = m_launchOptions.processType == ProcessType::Network;
    unsigned long role = network ? kosmosNetworkProcessRole : kosmosWebProcessRole;

    size_t pages = (sizeof(KosmosLaunch) + KOSMOS_PAGE_SIZE - 1) / KOSMOS_PAGE_SIZE;
    auto page = KosmosCapability::adopt(kosmos_mem_create(pages));
    long address = page ? kosmos_mem_map(page.capability()) : -1;
    if (address < 0)
        return;
    auto& launch = *reinterpret_cast<KosmosLaunch*>(address);

    Vector<int> caps {
        static_cast<int>(pair->client.region().capability()),
        static_cast<int>(pair->client.inbox().capability()),
        static_cast<int>(pair->client.outbox().capability()),
        static_cast<int>(page.capability()),
    };

    struct storage_mount mounts[STORAGE_MOUNTS_MAX];
    unsigned count = std::min<unsigned>(storage_mounts(mounts, STORAGE_MOUNTS_MAX), STORAGE_MOUNTS_MAX);
    launch.magic = kosmosLaunchMagic;
    launch.processIdentifier = m_launchOptions.processIdentifier.toUInt64();
    launch.mounts = 0;
    launch.network = -1;
    strncpy(launch.cwd, storage_cwd(), sizeof launch.cwd - 1);
    for (unsigned i = 0; i < count; ++i) {
        if (!reachable(mounts[i], m_launchOptions.processType))
            continue;
        launch.mount[launch.mounts] = mounts[i];
        launch.mount[launch.mounts].cap = static_cast<long>(caps.size());
        caps.append(static_cast<int>(mounts[i].cap));
        launch.mounts++;
    }
    launch.console = -1;
    if (long console = posix_console_capability(); console >= 0) {
        launch.console = static_cast<int32_t>(caps.size());
        caps.append(static_cast<int>(console));
    }
    long net = network ? net_client_cap() : -1;
    if (net >= 0) {
        launch.network = static_cast<int32_t>(caps.size());
        caps.append(static_cast<int>(net));
    }
    kosmos_share_unmap(static_cast<unsigned long>(address), pages);

    long child = kosmos_spawn(role, caps.span().data(), caps.size(), 0);
    if (child < 0) {
        WTFLogAlways("ProcessLauncher: the %s process could not be started (%ld)", network ? "network" : "web", child);
        return;
    }

    RunLoop::mainSingleton().dispatch([protectedThis = Ref { *this }, child, identifier = WTF::move(pair->server)] mutable {
        protectedThis->didFinishLaunchingProcess(static_cast<ProcessID>(child), WTF::move(identifier));
    });
}

void ProcessLauncher::terminateProcess()
{
    if (!m_processID)
        return;
    kosmos_kill(static_cast<unsigned long>(m_processID));
}

void ProcessLauncher::platformInvalidate()
{
    m_processID = 0;
}

} // namespace WebKit
