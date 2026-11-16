# Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
# **WTF's sources on Kosmos** (`docs/w6.md` W6a): what `PlatformJSCOnly.cmake`
# chose for a Unix that is not Linux, FreeBSD or Darwin, written out for the
# one system this port is, plus Kosmos's own three files. The POSIX layer
# (`runtime/posix/`, `docs/w3.md`) answers what the posix/ and unix/ files
# ask; threads are stopped by Nebula rather than a signal, so WTF's signal
# table lives in `kosmos/SignalsKosmos.cpp`, where nothing is delivered to it
# (W4c); and the footprint is Nebula's process table, read in
# `kosmos/MemoryFootprintKosmos.cpp`. The run loop is WebKit's generic one,
# on a condition variable, with no GLib.

list(APPEND WTF_SOURCES
    generic/MainThreadGeneric.cpp
    generic/MemoryPressureHandlerGeneric.cpp
    generic/RunLoopGeneric.cpp
    generic/WorkQueueGeneric.cpp

    kosmos/KosmosCapability.cpp
    kosmos/KosmosJITPool.cpp
    kosmos/MemoryFootprintKosmos.cpp
    kosmos/SignalsKosmos.cpp

    posix/CPUTimePOSIX.cpp
    posix/FileHandlePOSIX.cpp
    posix/FileSystemPOSIX.cpp
    posix/MappedFileDataPOSIX.cpp
    posix/OSAllocatorPOSIX.cpp
    posix/ThreadingPOSIX.cpp

    text/unix/TextBreakIteratorInternalICUUnix.cpp

    unix/LanguageUnix.cpp
    unix/LoggingUnix.cpp
    unix/UniStdExtrasUnix.cpp
)

list(APPEND WTF_LIBRARIES
    Threads::Threads
)

# What a Unix port's headers give the rest of WebKit: WebCore's GL fences,
# built on every Skia port, take a file descriptor by this class.
list(APPEND WTF_PUBLIC_HEADERS
    unix/UnixFileDescriptor.h
)

# A capability, counted: what WebCore's shared memory and WebKit2's
# connection hold a region or an endpoint by (`docs/w7.md` W7a).
list(APPEND WTF_PUBLIC_HEADERS
    kosmos/KosmosCapability.h
)

# The JIT's pool, one region mapped twice (`docs/jit.md`, patch 0037).
list(APPEND WTF_PUBLIC_HEADERS
    kosmos/KosmosJITPool.h
)

# Nebula's calls (`user/include/kosmos.h`), which take the kernel's
# `syscall.h` by quotes: the kernel's folder is searched for quoted names in
# this file alone, as WebCore's `PlatformKosmos.cmake` says why.
set_source_files_properties(kosmos/KosmosCapability.cpp kosmos/KosmosJITPool.cpp
    PROPERTIES COMPILE_OPTIONS "-iquote;${KOSMOS_ROOT}/kernel")
