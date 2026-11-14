# Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
# **WebCore's platform on Kosmos** (`docs/w6.md` W6b), from the pieces
# WebKit's ports share rather than written again: curl's network and
# OpenSSL's crypto, as PlayStation's port takes them; Skia's graphics and
# HarfBuzz's text, as WPE's; the image decoders every non-Apple port uses;
# Adwaita's theme - the form controls and scroll bars GTK and WPE draw
# for themselves, with no toolkit under them - and TextureMapper, built and
# not run (`OptionsKosmos.cmake`). What is Kosmos's own arrives
# beside them in `platform/kosmos/` as WebCore asks for it.

include(platform/Adwaita.cmake)
include(platform/Curl.cmake)
include(platform/ImageDecoders.cmake)
include(platform/OpenSSL.cmake)
include(platform/Skia.cmake)
include(platform/TextureMapper.cmake)

list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
    ${WEBCORE_DIR}/platform/generic
    ${WEBCORE_DIR}/platform/graphics/egl
    ${WEBCORE_DIR}/platform/graphics/kosmos
    ${WEBCORE_DIR}/platform/kosmos
    ${WEBCORE_DIR}/platform/video-codecs
)

# Kosmos's own, which SharedMemory.h and a program making pages include.
list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
    platform/kosmos/FontFolderKosmos.h
    platform/kosmos/KosmosRegion.h
)

list(APPEND WebCore_SOURCES
    platform/generic/KeyedDecoderGeneric.cpp
    platform/generic/KeyedEncoderGeneric.cpp
    platform/generic/ScrollbarsControllerGeneric.cpp

    platform/graphics/opentype/OpenTypeVerticalData.cpp

    # Other ports' files that say nothing about their port, used as they
    # are: PlayStation's screen of 1920 by 1080 in sRGB, its
    # system fonts by family name, its MIME types by extension and its
    # network that is always on. The screen's real size is W7's, when
    # there is a window to ask.
    platform/graphics/playstation/SystemFontDatabasePlayStation.cpp
    platform/network/playstation/NetworkStateNotifierPlayStation.cpp
    platform/playstation/MIMETypeRegistryPlayStation.cpp
    platform/playstation/PlatformScreenPlayStation.cpp

    # The display a GPU would be reached through, and its contexts and
    # fences: built as every Skia port builds them, and with the EGL Kit's
    # no display (`user/kits/egl/`) never given one - a program making
    # pages says so with ProcessCapabilities::setCanUseAcceleratedBuffers.
    platform/graphics/PlatformDisplay.cpp
    platform/graphics/egl/GLContext.cpp
    platform/graphics/egl/GLContextWrapper.cpp
    platform/graphics/egl/GLDisplay.cpp
    platform/graphics/egl/GLFence.cpp
    platform/graphics/egl/GLFenceEGL.cpp
    platform/graphics/egl/GLFenceGL.cpp

    platform/kosmos/AXObjectCacheKosmos.cpp
    platform/kosmos/CurlSSLHandleKosmos.cpp
    platform/kosmos/EditorKosmos.cpp
    platform/kosmos/FontFolderKosmos.cpp
    platform/kosmos/PasteboardKosmos.cpp
    platform/kosmos/PlatformKeyboardEventKosmos.cpp
    platform/kosmos/SharedMemoryKosmos.cpp
    platform/kosmos/UserAgentKosmos.cpp

    # The media engine on the Media Kit (`docs/media.md` M2).
    platform/graphics/kosmos/MediaPlayerPrivateKosmos.cpp

    platform/text/Hyphenation.cpp
    platform/text/LocaleICU.cpp

    platform/unix/LoggingUnix.cpp
)

# curl's and libpsl's come with `Curl.cmake`, libxml2's, SQLite's and zlib's
# with WebCore itself; OpenSSL's for TLS under curl and for Web Crypto.
list(APPEND WebCore_LIBRARIES
    OpenSSL::Crypto
    OpenSSL::SSL
)

# Nebula's calls (`user/include/kosmos.h`) for the one file that makes
# regions: `kosmos.h` takes the kernel's `syscall.h` by quotes, so the
# kernel's folder is searched for quoted names in this file alone - on
# WebCore's every file, `kernel/screen.h` would answer for `Screen.h` on a
# file system that ignores case.
set_source_files_properties(platform/kosmos/SharedMemoryKosmos.cpp
    PROPERTIES COMPILE_OPTIONS "-iquote;${KOSMOS_ROOT}/kernel")

# The media engine reaches two kits' doors, the Media Kit's `media.h` and
# the gfx kit's `yuv.h`, in this file alone and searched last, so no name
# in either folder can answer for one of WebKit's.
set_source_files_properties(platform/graphics/kosmos/MediaPlayerPrivateKosmos.cpp
    PROPERTIES COMPILE_OPTIONS "-idirafter;${KOSMOS_ROOT}/user/kits/media;-idirafter;${KOSMOS_ROOT}/user/kits/gfx")
