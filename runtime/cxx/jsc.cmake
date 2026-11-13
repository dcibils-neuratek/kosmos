#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
# **WebKit's options for Kosmos** (`docs/w4.md`): the initial cache
# `tools/webkit_build.py` configures WebKit with, beside the toolchain file
# `kosmos.cmake` - as `runtimes.cmake` is for the C++ runtimes.
#
# The Kosmos port (`runtime/ports/webkit/Source/cmake/OptionsKosmos.cmake`,
# `docs/w6.md` W6a) - JSCOnly's until W6 - with the Generic event loop and
# no GLib, built static so it links into an image as every kit here does. No JIT of any tier, and no WebAssembly, whose tiers are JITs: the
# LLInt interprets, and nothing a process writes is ever run (`docs/w4.md`,
# *Not yet decided*). No remote inspector, which is a socket server, and
# no tests of WebKit's own, which are programs CMake would link for a
# system it cannot run.

set(PORT "Kosmos" CACHE STRING "")
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

# **The programs WebKit's build links, and never runs.** JavaScriptCore's
# offline assembler compiles `LLIntSettingsExtractor` and
# `LLIntOffsetsExtractor` for the target and then *reads* them, Ruby
# finding the constants the compiler laid out in their data; nothing runs
# them, here or anywhere. So they are linked by lld - clang's own linker
# for an ELF target, where it would otherwise hand them to this Mac's - as
# a static ELF with no start files and nothing left to resolve, entered at
# `main` - WebKit links with --gc-sections, which keeps only what the entry
# reaches, and without one it kept nothing and Ruby found no constants.
# What runs
# on Kosmos is linked by the Makefile against `user/user.ld`, never here.
set(CMAKE_EXE_LINKER_FLAGS
    "-fuse-ld=/opt/homebrew/opt/lld@22/bin/ld.lld -nostdlib -static -Wl,--unresolved-symbols=ignore-all -Wl,-e,main"
    CACHE STRING "")

# **Skia** (`docs/w5.md` W5e), WebKit's own copy in `Source/ThirdParty/skia`:
# on the processor, its fonts through the same FreeType as the Text Kit's,
# and pictures through W5a's libpng and libjpeg-turbo - found where
# `tools/w5_build.py` installed them, as `webkit_build.py` says. No encoders
# yet: they need libwebp's mux, and nothing asks Skia to write a picture
# before WebCore does (W6).
set(USE_SKIA ON CACHE BOOL "")
set(USE_SKIA_ENCODERS OFF CACHE BOOL "")
