/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **What a web or network process is started with** (`docs/w7.md` W7a):
 * its role, and capabilities in a fixed order - its connection's three,
 * a page of these facts, then whatever it may reach, each mount's, the
 * network's and the sound's. The page says how many of each and where they are; the
 * capabilities themselves are the child's by index, as `SYS_SPAWN` lays
 * them out, from 0.
 */

#pragma once

#include <stdint.h>
#include <storage.h>

namespace WebKit {

constexpr unsigned long kosmosWebProcessRole = 500;
constexpr unsigned long kosmosNetworkProcessRole = 501;

constexpr int kosmosLaunchRegion = 0;   // the connection's rings
constexpr int kosmosLaunchInbox = 1;    // the endpoint the child receives on
constexpr int kosmosLaunchOutbox = 2;   // the one it sends to
constexpr int kosmosLaunchPage = 3;     // a KosmosLaunch
constexpr int kosmosLaunchFirstMount = 4;

constexpr uint64_t kosmosLaunchMagic = 0x484e55414c42454bull; // "KEBLAUNH"

struct KosmosLaunch {
    uint64_t magic;
    uint64_t processIdentifier;
    uint32_t mounts;    // storage_mount entries below, caps from kosmosLaunchFirstMount
    int32_t network;    // the network's capability, or -1
    int32_t console;    // the console's (`posix_console`), or -1
    int32_t audio;      // `/Devices/audio`'s (`audioclient.h`), or -1
    uint32_t framesTraced; // a frame's time said in the log (`KosmosFrameTimes.h`)
    char cwd[STORAGE_PATH_MAX];
    struct storage_mount mount[STORAGE_MOUNTS_MAX];
};

} // namespace WebKit
