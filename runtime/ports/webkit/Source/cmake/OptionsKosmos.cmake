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
# the network, the generic run loop - and WPE's for Skia. It builds what
# JSCOnly built - WTF, bmalloc, JavaScriptCore and Skia, held at W6a to
# `x86-wtf`, `x86-jsc` and `x86-skia` answering as they did - and since W6b
# PAL and WebCore with the platform below. The choices Kosmos makes as a
# cache rather than as a port - no JIT, the system's malloc, Skia without
# encoders - stay in `runtime/cxx/jsc.cmake`, read before this.

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

# **What a headless page on Kosmos has not got, yet** (`docs/w6.md` W6b).
# Each is a piece of the platform a later step brings, not a refusal:
# Web Audio waits for the Playback Kit (`docs/playback.md`), WebGL for a
# GPU, XSLT for libxslt, JPEG XL and colour management for their
# libraries. WOFF2 and AVIF are on since W5f (`docs/w5.md`): woff2 on
# Brotli, and libavif on dav1d. Video is on since M2 (`docs/media.md`):
# `<video>` plays through the port's engine on the Media Kit, and since M3
# a page's MediaSource does - which is how YouTube plays.
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_VIDEO PRIVATE ON)
# **MHTML**, for JetSki's Save page as (`docs/webkit.html`): a page, its
# styles and its pictures in one file, as WebKit writes it from what it
# holds (`WKPageGetContentsAsMHTMLData`), and opened again without the
# network. WebKitGTK's saving works the same way; off by WebKit's default.
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_MHTML PRIVATE ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_MEDIA_SOURCE PRIVATE ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_VIDEO_USES_ELEMENT_FULLSCREEN PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEB_AUDIO PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEBGL PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_XSLT PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_WOFF2 PRIVATE ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_AVIF PRIVATE ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_JPEGXL PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_LCMS PRIVATE OFF)
# **No GPU process** (`docs/w7.md` W7a): a page is painted on the
# processor in its web process, as PlayStation's port does without one,
# until there is a GPU to give a process of its own.
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_GPU_PROCESS PRIVATE OFF)
# And what a window brings, at W7: the system's notifications, a
# position, context menus, full screen, and messages to the application.
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_CONTEXT_MENUS PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_FULLSCREEN_API PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_GEOLOCATION PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_NOTIFICATIONS PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_USER_MESSAGE_HANDLERS PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_SMOOTH_SCROLLING PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_IMAGE_DIFF PRIVATE OFF)
# **Privacy and Ad Blocking** (`docs/adblock.md`, B1): WebKit's content
# extensions, Safari's content blocker - lists compiled into a DFA's
# bytecode, which each web process matches a request against before it is
# made. Mail on WebKit's W4 holds a message's network pictures with it too.
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_CONTENT_EXTENSIONS PRIVATE ON)
WEBKIT_OPTION_END()

# WebKit's generic run loop, as PlayStation's: no GLib anywhere in the process.
set(EVENT_LOOP_TYPE "Generic")
set(LOWERCASE_EVENT_LOOP_TYPE "generic")
SET_AND_EXPOSE_TO_BUILD(USE_GENERIC_EVENT_LOOP 1)
SET_AND_EXPOSE_TO_BUILD(WTF_DEFAULT_EVENT_LOOP 0)

# **WebCore, since W6b**, headless: a page laid out and painted with no
# window and no WebKit2, whose three processes are W7's. Legacy WebKit is
# the Mac's own and is never built here.
set(ENABLE_WEBCORE ON)
set(ENABLE_WEBKIT_LEGACY OFF)
set(ENABLE_WEBKIT ON)
set(ENABLE_WEBINSPECTORUI OFF)
set(ENABLE_WEBGPU OFF)

# **No pkg-config.** WebKit's Find modules ask it first, and on this Mac it
# answers with Homebrew's libraries, built for macOS. Every library WebCore
# links is W5's (`tools/w5_build.py`), found only where `tools/webkit_build.py`
# points CMake - as PlayStation's port, which has the same problem, does.
set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig ON)
macro(pkg_check_modules _prefix _module0)
endmacro()

find_package(ICU 70.1 REQUIRED COMPONENTS data i18n uc)

if (ENABLE_WEBCORE)
    find_package(CURL 7.85.0 REQUIRED)
    find_package(Freetype REQUIRED)
    find_package(HarfBuzz REQUIRED COMPONENTS ICU)
    find_package(JPEG REQUIRED)
    find_package(LibPSL 0.20.2 REQUIRED)
    find_package(LibXml2 2.9.7 REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(PNG REQUIRED)
    find_package(SQLite3 REQUIRED)
    find_package(WebP REQUIRED COMPONENTS demux)
    find_package(ZLIB REQUIRED)
    # No versions asked: with no pkg-config, FindWOFF2 cannot read one,
    # and the releases are `tools/upstream.py`'s.
    if (USE_WOFF2)
        find_package(WOFF2 REQUIRED COMPONENTS dec)
    endif ()
    if (USE_AVIF)
        find_package(AVIF REQUIRED)
    endif ()

    # **GL ES from an EGL with no display** (Diego, 13 November, `docs/w6.md`
    # W6b): WebCore links GL ES on every port, and with no display WebKit's
    # Skia paints on the processor. The headers are the Khronos ones WebKit
    # carries (`Source/ThirdParty/ANGLE/include`), in their platform-neutral
    # form, since Kosmos is no platform they list; the library is Kosmos's
    # (`user/kits/egl/`), linked by the Makefile.
    find_package(EGL REQUIRED)
    find_package(OpenGLES2 REQUIRED)
    add_definitions(-DEGL_NO_PLATFORM_SPECIFIC_TYPES=1)
endif ()

if (NOT ENABLE_STATIC_JSC)
    message(FATAL_ERROR "Kosmos links nothing dynamically: ENABLE_STATIC_JSC must be ON")
endif ()

# **WebCore's platform**, as SET_AND_EXPOSE_TO_BUILD makes each a `USE()`:
# curl for the network and OpenSSL under it (W5d), HarfBuzz for shaping and
# Skia for every pixel (W5b, W5e). And TextureMapper, WebKit's compositor
# on GL, which every Skia port builds and whose texture pool WebCore's GL
# display reaches for: built, as PlayStation's port builds it, and never
# run while there is no display - the page is painted on the processor
# and nothing composites. Coordinated graphics, the compositor's thread,
# is a window's, at W7. And Adwaita's theme, which `PlatformKosmos.cmake`
# builds: its files answer for RenderTheme, Theme and the scroll bars only
# under `USE(THEME_ADWAITA)`, as GTK and WPE set it.
if (ENABLE_WEBCORE)
    SET_AND_EXPOSE_TO_BUILD(USE_CURL ON)
    SET_AND_EXPOSE_TO_BUILD(USE_HARFBUZZ ON)
    SET_AND_EXPOSE_TO_BUILD(USE_OPENSSL ON)
    SET_AND_EXPOSE_TO_BUILD(USE_TEXTURE_MAPPER ON)
    SET_AND_EXPOSE_TO_BUILD(USE_THEME_ADWAITA ON)
endif ()

set(PAL_LIBRARY_TYPE STATIC)
set(WebCore_LIBRARY_TYPE STATIC)
