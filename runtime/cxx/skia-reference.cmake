#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
# **The reference Kosmos's Skia is held to** (`docs/w5.md` W5e): WebKit's
# own CMake on this Mac, the JSC-only port as `jsc.cmake` configures it for
# Kosmos, with Skia on and the Kosmos choices taken (`KOSMOS_SKIA_REFERENCE`
# in `runtime/patches/webkit/0012-skia-on-kosmos.patch`) - fonts from a
# folder over FreeType, no native GL, FreeType's version its own. Only
# `Skia` is built from it; `tools/webkit_build.py host|host-x86_64`.

set(PORT "JSCOnly" CACHE STRING "")
set(CMAKE_BUILD_TYPE "Release" CACHE STRING "")
set(ENABLE_STATIC_JSC ON CACHE BOOL "")
set(DEVELOPER_MODE OFF CACHE BOOL "")
set(ENABLE_API_TESTS OFF CACHE BOOL "")
set(ENABLE_TOOLS OFF CACHE BOOL "")
set(USE_HEADER_MAPS OFF CACHE BOOL "")
set(USE_SYSTEM_MALLOC ON CACHE BOOL "")

set(USE_SKIA ON CACHE BOOL "")
set(USE_SKIA_ENCODERS OFF CACHE BOOL "")
set(KOSMOS_SKIA_REFERENCE ON CACHE BOOL "")
