#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
# **LLVM's options for Kosmos** (`docs/gpu3d.md` G1): the initial cache
# `tools/llvm_build.py` configures LLVM 23.1.2 with, beside the toolchain
# file `kosmos.cmake`, as `jsc.cmake` is for WebKit.
#
# What is built is LLVM's libraries and nothing else: code generation for
# the two processors Kosmos runs on, and the ORC JIT, which is what Mesa's
# lavapipe compiles a shader with. No tools, no tests, no examples - each
# is a program CMake would link for a system it cannot run - and none of
# the libraries LLVM can use but does not need: zlib, zstd, libxml2,
# libedit, libpfm.
#
# - **`UNIX`**: LLVM chooses its `Support` library's Unix half or its
#   Windows half by CMake's `UNIX`, and stops at "Unable to determine
#   platform" without one. The POSIX layer (`runtime/posix/`, `docs/w3.md`)
#   is what the Unix half compiles against; what Kosmos does not have is
#   patched out in `runtime/patches/llvm/`.
# - **Its table generator is this Mac's**: `llvm-tblgen` is a program that
#   runs during the build, and Homebrew's is the same release, 23.1.2, so
#   it writes what LLVM's own would.
# - **No exceptions, no RTTI**: LLVM's defaults, and Mesa builds against
#   what LLVM was built with.
# - **No threads in LLVM**: lavapipe runs its shaders on threads of its
#   own; LLVM's thread pool and locks are for its tools' parallel builds.

set(UNIX 1 CACHE BOOL "")
set(CMAKE_BUILD_TYPE "Release" CACHE STRING "")

set(LLVM_TARGETS_TO_BUILD "X86;AArch64" CACHE STRING "")
set(LLVM_ENABLE_PROJECTS "" CACHE STRING "")
set(LLVM_ENABLE_RUNTIMES "" CACHE STRING "")

set(LLVM_BUILD_TOOLS OFF CACHE BOOL "")
set(LLVM_INCLUDE_TOOLS OFF CACHE BOOL "")
set(LLVM_BUILD_UTILS OFF CACHE BOOL "")
set(LLVM_INCLUDE_UTILS OFF CACHE BOOL "")
set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "")
set(LLVM_INCLUDE_EXAMPLES OFF CACHE BOOL "")
set(LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "")
set(LLVM_INCLUDE_DOCS OFF CACHE BOOL "")
set(LLVM_BUILD_LLVM_DYLIB OFF CACHE BOOL "")
set(LLVM_ENABLE_PIC OFF CACHE BOOL "")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "")

set(LLVM_ENABLE_ASSERTIONS OFF CACHE BOOL "")
set(LLVM_ABI_BREAKING_CHECKS "FORCE_OFF" CACHE STRING "")
set(LLVM_ENABLE_EH OFF CACHE BOOL "")
set(LLVM_ENABLE_RTTI OFF CACHE BOOL "")
set(LLVM_ENABLE_THREADS OFF CACHE BOOL "")
set(LLVM_ENABLE_BACKTRACES OFF CACHE BOOL "")
set(LLVM_ENABLE_CRASH_OVERRIDES OFF CACHE BOOL "")
set(LLVM_ENABLE_CRASH_DUMPS OFF CACHE BOOL "")

set(LLVM_ENABLE_ZLIB OFF CACHE STRING "")
set(LLVM_ENABLE_ZSTD OFF CACHE STRING "")
set(LLVM_ENABLE_LIBXML2 OFF CACHE STRING "")
set(LLVM_ENABLE_LIBEDIT OFF CACHE BOOL "")
set(LLVM_ENABLE_LIBPFM OFF CACHE BOOL "")
set(LLVM_ENABLE_FFI OFF CACHE BOOL "")
set(LLVM_ENABLE_HTTPLIB OFF CACHE BOOL "")
set(LLVM_ENABLE_CURL OFF CACHE BOOL "")

set(LLVM_TABLEGEN "/opt/homebrew/opt/llvm/bin/llvm-tblgen" CACHE FILEPATH "")
set(LLVM_NATIVE_TOOL_DIR "/opt/homebrew/opt/llvm/bin" CACHE PATH "")
