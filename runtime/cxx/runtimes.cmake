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
# **Threads on** (`docs/w4.md` W4a), on the POSIX layer's pthreads, which
# stand on W2's futex: `std::mutex`, `std::thread`, `std::condition_variable`,
# a function's static initialised once by many threads, and the monotonic
# clock `steady_clock` reads. So an image that links these links
# `runtime/posix/` too, as only ported code may - and C++ here is ported
# code, ICU and WebKit's.
#
# **The filesystem on** (`docs/w4.md` W4c): `std::filesystem`, on the
# POSIX layer's descriptors and the `at` calls, which reach the Storage
# Kit - WTF's FileSystem is written on it. Without iostreams it copies a
# file with `sendfile`, which the layer has for that reason.
#
# **Off, for now, and each for a reason:**
# - localization and wide characters: text is UTF-8 bytes here, and
#   WebKit keeps its own UTF-16 - so no iostreams, and no conversion of a
#   `char8_t` path, which WTF is patched not to ask for (patches 0003,
#   0004);
# - the random device and the time zone database: each is a thing a
#   process asks the namespace for, not a device file, and arrives when a
#   port wants it;

set(LLVM_ENABLE_RUNTIMES "libcxx;libcxxabi;libunwind" CACHE STRING "")
set(CMAKE_BUILD_TYPE Release CACHE STRING "")

set(LIBCXX_ENABLE_SHARED OFF CACHE BOOL "")
set(LIBCXX_ENABLE_STATIC ON CACHE BOOL "")
set(LIBCXX_CXX_ABI libcxxabi CACHE STRING "")
set(LIBCXX_ENABLE_STATIC_ABI_LIBRARY ON CACHE BOOL "")
set(LIBCXX_ENABLE_NEW_DELETE_DEFINITIONS OFF CACHE BOOL "")
set(LIBCXX_ENABLE_THREADS ON CACHE BOOL "")
set(LIBCXX_HAS_PTHREAD_API ON CACHE BOOL "")
set(LIBCXX_ENABLE_MONOTONIC_CLOCK ON CACHE BOOL "")
set(LIBCXX_ENABLE_LOCALIZATION OFF CACHE BOOL "")
set(LIBCXX_ENABLE_WIDE_CHARACTERS OFF CACHE BOOL "")
set(LIBCXX_ENABLE_FILESYSTEM ON CACHE BOOL "")
set(LIBCXX_ENABLE_RANDOM_DEVICE OFF CACHE BOOL "")
set(LIBCXX_ENABLE_TIME_ZONE_DATABASE OFF CACHE BOOL "")
set(LIBCXX_ENABLE_VENDOR_AVAILABILITY_ANNOTATIONS OFF CACHE BOOL "")
set(LIBCXX_INCLUDE_TESTS OFF CACHE BOOL "")
set(LIBCXX_INCLUDE_BENCHMARKS OFF CACHE BOOL "")
set(LIBCXX_INCLUDE_DOCS OFF CACHE BOOL "")
set(LIBCXX_USE_COMPILER_RT OFF CACHE BOOL "")

set(LIBCXXABI_ENABLE_SHARED OFF CACHE BOOL "")
set(LIBCXXABI_ENABLE_STATIC ON CACHE BOOL "")
set(LIBCXXABI_ENABLE_THREADS ON CACHE BOOL "")
set(LIBCXXABI_HAS_PTHREAD_API ON CACHE BOOL "")
set(LIBCXXABI_BAREMETAL ON CACHE BOOL "")
set(LIBCXXABI_USE_LLVM_UNWINDER ON CACHE BOOL "")
set(LIBCXXABI_ENABLE_STATIC_UNWINDER ON CACHE BOOL "")
set(LIBCXXABI_USE_COMPILER_RT OFF CACHE BOOL "")
set(LIBCXXABI_INCLUDE_TESTS OFF CACHE BOOL "")

set(LIBUNWIND_ENABLE_SHARED OFF CACHE BOOL "")
set(LIBUNWIND_ENABLE_STATIC ON CACHE BOOL "")
set(LIBUNWIND_ENABLE_THREADS ON CACHE BOOL "")
set(LIBUNWIND_IS_BAREMETAL ON CACHE BOOL "")
set(LIBUNWIND_USE_COMPILER_RT OFF CACHE BOOL "")
set(LIBUNWIND_INCLUDE_TESTS OFF CACHE BOOL "")
