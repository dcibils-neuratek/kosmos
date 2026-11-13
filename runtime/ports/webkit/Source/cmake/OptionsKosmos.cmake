# Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
# **WebKit's Kosmos port** (`docs/w6.md` W6a): `PORT=Kosmos`, beside GTK,
# WPE, PlayStation and JSCOnly, and the one `tools/webkit_build.py` builds.
# W4 and W5 were built as JSCOnly, the port for JavaScriptCore alone; W6
# needs WebCore, which JSCOnly turns off, and WebCore asks a port for its
# platform - network, graphics, text, pictures - so Kosmos is a port of its
# own, whose choices are written here rather than patched into another's.
#
# It follows PlayStation's for the shape of a system without GLib - curl for
# the network, the generic run loop - and WPE's for Skia. For now it builds
# what JSCOnly built, WTF, bmalloc, JavaScriptCore and Skia, so that W6a is
# held to `x86-wtf`, `x86-jsc` and `x86-skia` answering as they did; WebCore
# is turned on by W6b. The choices Kosmos makes as a cache rather than as a
# port - no JIT, the system's malloc, Skia without encoders - stay in
# `runtime/cxx/jsc.cmake`, read before this.

set(PORT Kosmos)

find_package(Threads REQUIRED)

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# `PLATFORM(KOSMOS)` and `BPLATFORM(KOSMOS)` (`0013-a-port-named-kosmos.patch`);
# `OS(KOSMOS)` is the system's and comes from the toolchain, not the port.
add_definitions(-DBUILDING_KOSMOS__=1)

set(PROJECT_VERSION_MAJOR 1)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_MICRO 0)
set(PROJECT_VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_MICRO})

WEBKIT_OPTION_BEGIN()
# Every library WebKit builds here is static and linked into an image by
# the Makefile, as every kit is (`docs/w4.md`).
WEBKIT_OPTION_DEFINE(ENABLE_STATIC_JSC "Whether to build JavaScriptCore as a static library." PUBLIC ON)
WEBKIT_OPTION_DEFINE(USE_LIBBACKTRACE "Whether to enable usage of libbacktrace." PUBLIC OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_REMOTE_INSPECTOR PRIVATE OFF)
WEBKIT_OPTION_END()

# WebKit's generic run loop, as PlayStation's: no GLib anywhere in the process.
set(EVENT_LOOP_TYPE "Generic")
set(LOWERCASE_EVENT_LOOP_TYPE "generic")
SET_AND_EXPOSE_TO_BUILD(USE_GENERIC_EVENT_LOOP 1)
SET_AND_EXPOSE_TO_BUILD(WTF_DEFAULT_EVENT_LOOP 0)

# W6b turns WebCore on; W7 brings WebKit2's processes.
set(ENABLE_WEBCORE OFF)
set(ENABLE_WEBKIT_LEGACY OFF)
set(ENABLE_WEBKIT OFF)
set(ENABLE_WEBINSPECTORUI OFF)
set(ENABLE_WEBGL OFF)
set(ENABLE_WEBGPU OFF)

if (NOT ENABLE_STATIC_JSC)
    message(FATAL_ERROR "Kosmos links nothing dynamically: ENABLE_STATIC_JSC must be ON")
endif ()

find_package(ICU 70.1 REQUIRED COMPONENTS data i18n uc)
