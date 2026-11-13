# Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
# **WTF's sources on Kosmos** (`docs/w6.md` W6a): what `PlatformJSCOnly.cmake`
# chose for a Unix that is not Linux, FreeBSD or Darwin, written out for the
# one system this port is, plus Kosmos's own two files. The POSIX layer
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
