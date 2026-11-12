#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
# **WebKit's options for Kosmos** (`docs/w4.md`): the initial cache
# `tools/webkit_build.py` configures WebKit with, beside the toolchain file
# `kosmos.cmake` - as `runtimes.cmake` is for the C++ runtimes.
#
# The JSC-only port (`OptionsJSCOnly.cmake`), with its Generic event loop
# and no GLib, built static so it links into an image as every kit here
# does. No JIT of any tier, and no WebAssembly, whose tiers are JITs: the
# LLInt interprets, and nothing a process writes is ever run (`docs/w4.md`,
# *Not yet decided*). No remote inspector, which is a socket server, and
# no tests of WebKit's own, which are programs CMake would link for a
# system it cannot run.

set(PORT "JSCOnly" CACHE STRING "")
set(CMAKE_BUILD_TYPE "Release" CACHE STRING "")
set(ENABLE_STATIC_JSC ON CACHE BOOL "")
set(EVENT_LOOP_TYPE "Generic" CACHE STRING "")
set(DEVELOPER_MODE OFF CACHE BOOL "")
set(ENABLE_API_TESTS OFF CACHE BOOL "")
set(ENABLE_TOOLS OFF CACHE BOOL "")

set(ENABLE_JIT OFF CACHE BOOL "")
set(ENABLE_DFG_JIT OFF CACHE BOOL "")
set(ENABLE_FTL_JIT OFF CACHE BOOL "")
set(ENABLE_WEBASSEMBLY OFF CACHE BOOL "")
set(ENABLE_WEBASSEMBLY_BBQJIT OFF CACHE BOOL "")
set(ENABLE_WEBASSEMBLY_OMGJIT OFF CACHE BOOL "")
set(ENABLE_SAMPLING_PROFILER OFF CACHE BOOL "")
set(ENABLE_REMOTE_INSPECTOR OFF CACHE BOOL "")
set(ENABLE_RESOURCE_USAGE OFF CACHE BOOL "")
set(USE_LIBBACKTRACE OFF CACHE BOOL "")

# Clang's header maps collapse include directories, made by a script the
# GTK release does not ship (`Tools/Scripts/hmaptool`); a plain include
# path does the same work.
set(USE_HEADER_MAPS OFF CACHE BOOL "")

# **The system's malloc under bmalloc, for now.** bmalloc's choices are
# libpas - WebKit's own allocator, which reserves wide ranges of addresses
# and commits and gives back pages within them - mimalloc, or the system's.
# The system's first, so that `jsc` runs on the least that could be wrong;
# libpas on W2's reservations is a step of its own, taken when a
# measurement says the allocator is where the time goes (`docs/w4.md`).
set(USE_SYSTEM_MALLOC ON CACHE BOOL "")

# **`<cstdlib>` before everything.** WebKit calls `malloc`, `abort` and the
# rest in headers that include nothing for them, because on Linux libc++'s
# locale headers bring `<cstdlib>` in along the way. Kosmos's libc++ is
# built without localisation (`runtimes.cmake`), so nothing brings it, and
# it is included first rather than added to each header that leans on it.
set(KOSMOS_CXX_FLAGS "-include cstdlib" CACHE STRING "")
