/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A web or network process, arrived** (`docs/w7.md` W7a): what
 * `Shared/unix/AuxiliaryProcessMain.cpp` is for a process started with a
 * command line. Here the process was started by `SYS_SPAWN` with its role,
 * and `kosmos_webkit_role` - which the image's `kosmos_project_role` calls
 * - reads its launch page (`KosmosLaunch.h`), hands its mounts and the
 * network to the kits that reach them, and enters WebKit's main for the
 * role with the identifier as its one argument. The connection is the
 * three capabilities the launcher put first.
 */

#include "config.h"
#include "AuxiliaryProcessMain.h"

#include "AuxiliaryProcess.h"
#include "KosmosLaunch.h"
#include "NetworkProcessMain.h"
#include "WebProcessMain.h"
#include <kosmos.h>
#include <stdio.h>
#include "netclient.h"
#include "audioclient.h"
#include <posix_program.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace WebKit {

AuxiliaryProcessMainCommon::AuxiliaryProcessMainCommon()
{
}

bool AuxiliaryProcessMainCommon::parseCommandLine(int argc, char** argv)
{
    if (argc < 2)
        return false;
    auto processIdentifier = parseInteger<uint64_t>(StringView::fromLatin1(unsafeMakeSpan(argv, argc)[1]));
    if (!processIdentifier || !ObjectIdentifier<WebCore::ProcessIdentifierType>::isValidIdentifier(*processIdentifier))
        return false;
    m_parameters.processIdentifier = ObjectIdentifier<WebCore::ProcessIdentifierType>(*processIdentifier);
    m_parameters.connectionIdentifier = IPC::Connection::Identifier { IPC::ConnectionHandle {
        KosmosCapability::adopt(kosmosLaunchRegion), KosmosCapability::adopt(kosmosLaunchInbox),
        KosmosCapability::adopt(kosmosLaunchOutbox), false } };
    return true;
}

void AuxiliaryProcess::platformInitialize(const AuxiliaryProcessInitializationParameters&)
{
}

} // namespace WebKit

extern "C" int kosmos_webkit_role(unsigned long role)
{
    using namespace WebKit;

    long pages = kosmos_mem_size(kosmosLaunchPage);
    long address = pages > 0 ? kosmos_mem_map(kosmosLaunchPage) : -1;
    if (address < 0 || static_cast<size_t>(pages) * KOSMOS_PAGE_SIZE < sizeof(KosmosLaunch))
        return 1;
    KosmosLaunch launch;
    memcpy(&launch, reinterpret_cast<const void*>(address), sizeof launch);
    kosmos_share_unmap(static_cast<unsigned long>(address), static_cast<unsigned long>(pages));
    kosmos_cap_drop(kosmosLaunchPage);
    if (launch.magic != kosmosLaunchMagic || launch.mounts > STORAGE_MOUNTS_MAX)
        return 1;

    if (launch.console >= 0)
        posix_console(launch.console);
    launch.cwd[sizeof launch.cwd - 1] = '\0';
    storage_start(launch.mount, launch.mounts, launch.cwd);
    if (launch.network >= 0)
        net_client_start(launch.network);
    if (launch.audio >= 0)
        audio_client_start(launch.audio);

    char identifier[24];
    snprintf(identifier, sizeof identifier, "%llu", static_cast<unsigned long long>(launch.processIdentifier));
    char webProcess[] = "WebProcess";
    char networkProcess[] = "NetworkProcess";
    char* argv[] = { role == kosmosNetworkProcessRole ? networkProcess : webProcess, identifier, nullptr };

    if (role == kosmosWebProcessRole) {
        kosmos_setname("web", 3);
        return WebProcessMain(2, argv);
    }
    if (role == kosmosNetworkProcessRole) {
        kosmos_setname("network", 7);
        return NetworkProcessMain(2, argv);
    }
    return 1;
}
