# Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
# **WebKit2 on Kosmos** (`docs/w7.md` W7a): the UI, web and network
# processes, as PlayStation's port has them - curl in the network process,
# Skia painting on the processor, a page drawn into a bitmap the UI process
# is handed - with what only Kosmos can say beside them, in `kosmos/`
# folders: the connection between processes on Nebula's endpoints, pulses
# and regions, the launcher on `SYS_SPAWN`, and the entry points a role
# arrives at.
#
# The processes are not WebKit's executables: one image (`jetski.elf`, or a
# suite's) is started three ways, and each role calls its entry point, so
# only the library is built here and the Makefile links it.
#
# Where another port's file says nothing about that port, it is used as it
# is, as WebCore's `PlatformKosmos.cmake` uses PlayStation's.

include(Headers.cmake)
include(Platform/Curl.cmake)
include(Platform/Skia.cmake)

WEBKIT_ADD_TARGET_CXX_FLAGS(WebKit -Wno-unused-lambda-capture)

list(APPEND WebKit_SOURCES
    NetworkProcess/NetworkDataTaskDataURL.cpp

    # Privacy and Ad Blocking (`docs/adblock.md`, B1): a compiled rule
    # list's bytes copied into a region for the web processes, and no
    # default store.
    NetworkProcess/cache/kosmos/NetworkCacheDataKosmos.cpp
    UIProcess/API/kosmos/APIContentRuleListStoreKosmos.cpp

    NetworkProcess/Classifier/WebResourceLoadStatisticsStore.cpp

    Platform/classifier/ResourceLoadStatisticsClassifier.cpp

    Platform/IPC/kosmos/ArgumentCodersKosmos.cpp
    Platform/IPC/kosmos/ConnectionKosmos.cpp
    Platform/IPC/kosmos/IPCSemaphoreKosmos.cpp

    Platform/unix/LoggingUnix.cpp
    Platform/unix/ModuleUnix.cpp

    UIProcess/DefaultUndoController.cpp
    UIProcess/LegacySessionStateCodingNone.cpp
    UIProcess/WebGrammarDetail.cpp
    UIProcess/WebMemoryPressureHandler.cpp
    UIProcess/WebViewportAttributes.cpp

    UIProcess/API/C/WKUserScriptRef.cpp
    UIProcess/API/C/WKViewportAttributes.cpp

    UIProcess/API/C/kosmos/WKPageKosmos.cpp

    UIProcess/CoordinatedGraphics/DrawingAreaProxyCoordinatedGraphics.cpp

    UIProcess/skia/BackingStoreSkia.cpp

    UIProcess/Launcher/kosmos/ProcessLauncherKosmos.cpp

    # Web Inspector, docked beside the page (`docs/webkit.html`).
    UIProcess/Inspector/kosmos/WebInspectorUIProxyKosmos.cpp

    UIProcess/WebsiteData/kosmos/WebsiteDataStoreKosmos.cpp

    UIProcess/kosmos/WebPageProxyKosmos.cpp
    UIProcess/kosmos/WebProcessPoolKosmos.cpp

    Shared/kosmos/AuxiliaryProcessMainKosmos.cpp
    Shared/kosmos/KosmosFrameTimes.cpp
    Shared/kosmos/KosmosStore.cpp
    Shared/kosmos/KosmosWake.cpp

    WebProcess/InjectedBundle/kosmos/InjectedBundleKosmos.cpp

    # PlayStation's, with keys that edit (`WebPageKosmos.cpp`).
    WebProcess/WebPage/kosmos/WebPageKosmos.cpp

    # PlayStation's, and nothing in them is PlayStation's: no compositing,
    # no threaded scrolling, the standard user agent, a page's platform
    # hooks left as WebKit's defaults.
    UIProcess/API/C/playstation/WKView.cpp
    UIProcess/playstation/PageClientImpl.cpp
    UIProcess/playstation/PlayStationWebView.cpp
    UIProcess/playstation/WebPreferencesPlayStation.cpp
    WebProcess/playstation/WebProcessMainPlayStation.cpp
    WebProcess/playstation/WebProcessPlayStation.cpp

    WebProcess/WebPage/CoordinatedGraphics/DrawingAreaCoordinatedGraphics.cpp
)

# The public C API's headers are found as WebKit/... first. A source file's
# prefix header names `"WKPageUIClient.h"` by quotes and `WKPage.h` names
# `<WebKit/WKPageUIClient.h>`; with the source folder first those are two
# files and `#pragma once` cannot tell they are one. Apple's are symbolic
# links, and copies are not.
list(PREPEND WebKit_PRIVATE_INCLUDE_DIRECTORIES
    "${WebKit_FRAMEWORK_HEADERS_DIR}/WebKit"
)

list(APPEND WebKit_PRIVATE_INCLUDE_DIRECTORIES
    "${WEBKIT_DIR}/Platform/IPC/kosmos"
    "${WEBKIT_DIR}/Platform/classifier"
    "${WEBKIT_DIR}/Platform/generic"
    "${WEBKIT_DIR}/Shared/kosmos"
    "${WEBKIT_DIR}/UIProcess/API/C/kosmos"
    "${WEBKIT_DIR}/UIProcess/API/C/playstation"
    "${WEBKIT_DIR}/UIProcess/API/playstation"
    "${WEBKIT_DIR}/UIProcess/CoordinatedGraphics"
    "${WEBKIT_DIR}/UIProcess/kosmos"
    "${WEBKIT_DIR}/UIProcess/playstation"
    "${WEBKIT_DIR}/WebProcess/WebPage/CoordinatedGraphics"
)

# The C API a UI process is written against: WebKit's own, the view's, and
# a page's picture into a surface with a pitch.
list(APPEND WebKit_PUBLIC_FRAMEWORK_HEADERS
    Shared/API/c/kosmos/WKBaseKosmos.h

    UIProcess/API/C/kosmos/WKPageKosmos.h
    UIProcess/API/C/playstation/WKView.h
    UIProcess/API/C/playstation/WKViewClient.h
)

# Nebula's calls (`user/include/kosmos.h`), which take the kernel's
# `syscall.h` by quotes: the kernel's folder is searched for quoted names in
# these files alone, as WebCore's `PlatformKosmos.cmake` says why.
set_source_files_properties(
    Platform/IPC/kosmos/ConnectionKosmos.cpp
    Platform/IPC/kosmos/IPCSemaphoreKosmos.cpp
    PROPERTIES COMPILE_OPTIONS "-iquote;${KOSMOS_ROOT}/kernel")

# The launcher and a role's entry hand a process the network and the
# sound, and the Network Kit's and the audio client's headers are in their
# kits' own folders.
set_source_files_properties(
    Shared/kosmos/AuxiliaryProcessMainKosmos.cpp
    UIProcess/Launcher/kosmos/ProcessLauncherKosmos.cpp
    PROPERTIES COMPILE_OPTIONS "-iquote;${KOSMOS_ROOT}/kernel;-iquote;${KOSMOS_ROOT}/user/kits/network;-iquote;${KOSMOS_ROOT}/user/kits/audio")
