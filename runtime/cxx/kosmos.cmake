#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
# **A Kosmos process, as CMake sees one** (`docs/webkit.md` W1c): the
# toolchain file the C++ runtimes - libc++, libc++abi, libunwind - are
# configured with, and WebKit after them.
#
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=runtime/cxx/kosmos.cmake \
#         -DKOSMOS_ARCH=x86_64 -DKOSMOS_ROOT=$PWD ...
#
# What it says, and why each:
#
# - **No operating system CMake knows** (`Generic`), so it assumes nothing:
#   no shared libraries, no `-lpthread`, no sysroot.
# - **clang**, the one on this Mac (Homebrew's, LLVM 23.1.2 as the runtimes
#   are), with the target's triple. The rest of the userland is GCC's; the
#   two meet at the ELF ABI, which both keep.
# - **Kosmos's headers and nothing else**: `-nostdinc`, then the compiler's
#   own (`stddef.h`, `stdarg.h`, the intrinsics), then `runtime/include` and
#   `user/include` - the same libc every C file in the image compiles
#   against, so C++ cannot reach a function the image does not have.
# - **`KOSMOS_USER_BASE`**, where an image starts, which `kosmos.h` checks.
# - **Static libraries for every trial**: CMake's checks link a program,
#   and there is nothing here to link one against - it is the same reason
#   FFmpeg's `configure` was corrected by `ffmpeg_vendor.py` until M6d.
# - `_LIBCPP_HAS_CLOCK_GETTIME`: libc++ looks for `clock_gettime` only
#   where `<unistd.h>` says `_POSIX_TIMERS`, and Kosmos's says nothing; the
#   function is there (`time.h`).

if(NOT KOSMOS_ARCH)
  message(FATAL_ERROR "kosmos.cmake: KOSMOS_ARCH is x86_64 or aarch64")
endif()
if(NOT KOSMOS_ROOT)
  message(FATAL_ERROR "kosmos.cmake: KOSMOS_ROOT is the Kosmos tree")
endif()

# Carried into the trial compiles CMake runs in directories of their own.
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES KOSMOS_ARCH KOSMOS_ROOT KOSMOS_LLVM
    KOSMOS_LIBCXX_INCLUDE KOSMOS_CXX_FLAGS)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR ${KOSMOS_ARCH})
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT KOSMOS_LLVM)
  set(KOSMOS_LLVM /opt/homebrew/opt/llvm)
endif()

set(CMAKE_C_COMPILER   ${KOSMOS_LLVM}/bin/clang)
set(CMAKE_CXX_COMPILER ${KOSMOS_LLVM}/bin/clang++)
set(CMAKE_ASM_COMPILER ${KOSMOS_LLVM}/bin/clang)
set(CMAKE_AR           ${KOSMOS_LLVM}/bin/llvm-ar)
set(CMAKE_RANLIB       ${KOSMOS_LLVM}/bin/llvm-ranlib)

if(KOSMOS_ARCH STREQUAL "x86_64")
  set(triple x86_64-unknown-none-elf)
  set(base 0x40000000)
elseif(KOSMOS_ARCH STREQUAL "aarch64")
  set(triple aarch64-unknown-none-elf)
  set(base 0x4000000000)
else()
  message(FATAL_ERROR "kosmos.cmake: no Kosmos on ${KOSMOS_ARCH}")
endif()

set(CMAKE_C_COMPILER_TARGET   ${triple})
set(CMAKE_CXX_COMPILER_TARGET ${triple})
set(CMAKE_ASM_COMPILER_TARGET ${triple})

execute_process(COMMAND ${CMAKE_C_COMPILER} -print-resource-dir
                OUTPUT_VARIABLE resource OUTPUT_STRIP_TRAILING_WHITESPACE)

# The POSIX layer's headers first of all, as ported code compiles
# (`docs/w4.md` W4a): each of them reaches the compiler's or the runtime's
# own with `include_next`, which finds only what comes after it.
#
# `__kosmos__` says what the code is compiled for, as `__linux__` does on
# Linux - the target's triple names no operating system, so clang defines
# nothing of the kind, and WebKit's `OS(KOSMOS)` is made from this
# (`runtime/patches/webkit/`).
set(flags "-nostdinc -isystem ${KOSMOS_ROOT}/runtime/posix/include -D__kosmos__")
string(APPEND flags " -isystem ${resource}/include")
string(APPEND flags " -isystem ${KOSMOS_ROOT}/runtime/include")
string(APPEND flags " -isystem ${KOSMOS_ROOT}/user/include")
# The names Kosmos refuses, as headers that refuse them
# (`runtime/posix/refused/`): an empty `signal.h` and `sys/ioctl.h`, so a
# port that includes them compiles and one that calls `signal` or `ioctl`
# does not. curl includes `signal.h` whatever it was told it has.
string(APPEND flags " -isystem ${KOSMOS_ROOT}/runtime/posix/refused")
string(APPEND flags " -DKOSMOS_USER -DKOSMOS_USER_BASE=${base}")
string(APPEND flags " -D_LIBCPP_HAS_CLOCK_GETTIME=")
string(APPEND flags " -ffunction-sections -fdata-sections -fno-stack-protector")

# libc++'s build hands `-nostdinc++` to libunwind's C and assembly too, and clang
# says so once a file; it is upstream's flag, and harmless.
set(CMAKE_C_FLAGS_INIT   "${flags} -Wno-unused-command-line-argument")
# A port compiled against the C++ runtimes once they are built names
# their headers, which come before everything: libc++'s `<cstdlib>`
# includes the C library's `<stdlib.h>` after itself (`docs/w4.md`). The
# runtimes' own build leaves it unset - it supplies those headers itself.
# And a port's own flags after Kosmos's, in `KOSMOS_CXX_FLAGS` - setting
# `CMAKE_CXX_FLAGS` itself would replace these rather than add to them.
if(KOSMOS_LIBCXX_INCLUDE)
  set(CMAKE_CXX_FLAGS_INIT "-nostdinc++ -isystem ${KOSMOS_LIBCXX_INCLUDE} ${flags} ${KOSMOS_CXX_FLAGS}")
else()
  set(CMAKE_CXX_FLAGS_INIT "${flags} ${KOSMOS_CXX_FLAGS}")
endif()
# And the assembly says its stack is not executable, which a target with no
# operating system does not say by default and GNU ld warns about.
set(CMAKE_ASM_FLAGS_INIT "${flags} -Wno-unused-command-line-argument -Wa,--noexecstack")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
