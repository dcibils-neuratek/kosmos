#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
# **What the C++ runtimes are built with** (`docs/webkit.md` W1c), as an
# initial cache: `cmake -C runtime/cxx/runtimes.cmake`.
#
# Everything static, as every Kosmos image is. libc++abi under libc++, and
# LLVM's own unwinder under libc++abi, in its bare-metal form, which finds
# the image's unwinding tables by the names `user/user.ld` gives them
# (`__eh_frame_hdr_start`) rather than by asking a dynamic loader.
#
# **Off, for now, and each for a reason:**
# - threads: Kosmos has them, but a C++ `std::mutex` stands on a futex,
#   which is W2; the runtimes are built again with them when it lands;
# - localization and wide characters: text is UTF-8 bytes here, and
#   WebKit keeps its own UTF-16;
# - the filesystem, the random device and the time zone database: each is
#   a thing a process asks the namespace for, not a path or a device file,
#   and arrives with the POSIX layer (W3) if WebKit wants it;
# - the monotonic clock as libc++ means it: `steady_clock` wants threads'
#   headers in this release. `clock_gettime(CLOCK_MONOTONIC)` is there for
#   whatever reads it directly.

set(LLVM_ENABLE_RUNTIMES "libcxx;libcxxabi;libunwind" CACHE STRING "")
set(CMAKE_BUILD_TYPE Release CACHE STRING "")

set(LIBCXX_ENABLE_SHARED OFF CACHE BOOL "")
set(LIBCXX_ENABLE_STATIC ON CACHE BOOL "")
set(LIBCXX_CXX_ABI libcxxabi CACHE STRING "")
set(LIBCXX_ENABLE_STATIC_ABI_LIBRARY ON CACHE BOOL "")
set(LIBCXX_ENABLE_NEW_DELETE_DEFINITIONS OFF CACHE BOOL "")
set(LIBCXX_ENABLE_THREADS OFF CACHE BOOL "")
set(LIBCXX_ENABLE_MONOTONIC_CLOCK OFF CACHE BOOL "")
set(LIBCXX_ENABLE_LOCALIZATION OFF CACHE BOOL "")
set(LIBCXX_ENABLE_WIDE_CHARACTERS OFF CACHE BOOL "")
set(LIBCXX_ENABLE_FILESYSTEM OFF CACHE BOOL "")
set(LIBCXX_ENABLE_RANDOM_DEVICE OFF CACHE BOOL "")
set(LIBCXX_ENABLE_TIME_ZONE_DATABASE OFF CACHE BOOL "")
set(LIBCXX_ENABLE_VENDOR_AVAILABILITY_ANNOTATIONS OFF CACHE BOOL "")
set(LIBCXX_INCLUDE_TESTS OFF CACHE BOOL "")
set(LIBCXX_INCLUDE_BENCHMARKS OFF CACHE BOOL "")
set(LIBCXX_INCLUDE_DOCS OFF CACHE BOOL "")
set(LIBCXX_USE_COMPILER_RT OFF CACHE BOOL "")

set(LIBCXXABI_ENABLE_SHARED OFF CACHE BOOL "")
set(LIBCXXABI_ENABLE_STATIC ON CACHE BOOL "")
set(LIBCXXABI_ENABLE_THREADS OFF CACHE BOOL "")
set(LIBCXXABI_BAREMETAL ON CACHE BOOL "")
set(LIBCXXABI_USE_LLVM_UNWINDER ON CACHE BOOL "")
set(LIBCXXABI_ENABLE_STATIC_UNWINDER ON CACHE BOOL "")
set(LIBCXXABI_USE_COMPILER_RT OFF CACHE BOOL "")
set(LIBCXXABI_INCLUDE_TESTS OFF CACHE BOOL "")

set(LIBUNWIND_ENABLE_SHARED OFF CACHE BOOL "")
set(LIBUNWIND_ENABLE_STATIC ON CACHE BOOL "")
set(LIBUNWIND_ENABLE_THREADS OFF CACHE BOOL "")
set(LIBUNWIND_IS_BAREMETAL ON CACHE BOOL "")
set(LIBUNWIND_USE_COMPILER_RT OFF CACHE BOOL "")
set(LIBUNWIND_INCLUDE_TESTS OFF CACHE BOOL "")
