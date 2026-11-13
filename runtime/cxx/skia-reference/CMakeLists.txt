#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
# **Skia alone, by WebKit's CMake, on this Mac** (`docs/w5.md` W5e): what
# WebKit's own top-level file does - its modules, the SDK, `WebKitCommon`
# and the port's options - and then Skia's directory only. The whole of
# WebKit does not configure here: WTF's Mac build wants files the GTK
# release does not ship. `WEBKIT_SOURCE` is the patched copy
# (`tools/webkit_build.py`).

cmake_minimum_required(VERSION 3.20)

set(CMAKE_MODULE_PATH "${WEBKIT_SOURCE}/Source/cmake")

if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    include(WebKitXcodeSDK)
endif ()

project(SkiaReference LANGUAGES C CXX)

include(WebKitCommon)

# WebKit's compiler wrapper, a pass-through for Skia, which has no unified
# sources - named from WebKit's tree rather than from this folder.
list(TRANSFORM CMAKE_CXX_COMPILER_LAUNCHER REPLACE
     "^${CMAKE_SOURCE_DIR}/Source/cmake/" "${WEBKIT_SOURCE}/Source/cmake/")

add_subdirectory("${WEBKIT_SOURCE}/Source/ThirdParty/skia" skia)

# What Skia declares PUBLIC, as code that uses it must be compiled - the
# list `user/kits/skiatest/skia_defines.h` is held to by `tools/run_skia.py`,
# from the target's own property rather than from reading its CMake.
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/skia-public-defines.txt"
     CONTENT "$<JOIN:$<TARGET_PROPERTY:Skia,INTERFACE_COMPILE_DEFINITIONS>,\n>\n")
