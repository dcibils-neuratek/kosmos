/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What Skia's CMake declares PUBLIC (`Source/ThirdParty/skia/CMakeLists.txt`,
 * WebKitGTK 2.54.2 with `0012-skia-on-kosmos.patch`): what code that uses
 * Skia must be compiled with for its headers to mean what the library's
 * did. `skiatest.cpp` includes it first, on Kosmos and on this Mac, and
 * `tools/run_skia.py` holds the list to what the Skia target declares, as
 * the reference build writes it out, so a define added there and not here
 * is a failure rather than an ABI that quietly disagrees.
 */
#ifndef SKIA_DEFINES_H
#define SKIA_DEFINES_H

#define SK_DISABLE_LEGACY_GL_MAKE_NATIVE_INTERFACE 1
#define SK_DISABLE_LEGACY_IMAGE_READBUFFER 1
#define SK_DISABLE_LEGACY_INIT_DECODERS 1
#define SK_DISABLE_LEGACY_PNG_WRITEBUFFER 1
#define SK_DISABLE_TRACING 1
#define SK_DISABLE_WEBGL_INTERFACE 1
#define SK_ENABLE_PRECOMPILE 1
#define SK_RELEASE 1
#define SK_TRIVIAL_ABI [[clang::trivial_abi]]
#define SK_ASSUME_GL 0
#define SK_ASSUME_GL_ES 1
#define SK_ASSUME_WEBGL 0
#define SK_R32_SHIFT 16

/* Kosmos only: this Mac is the platform Skia takes it to be. */
#if !defined(__APPLE__)
#define SK_BUILD_FOR_UNIX 1
#endif

#endif
