#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
**W5's libraries, built** (`docs/w5.md`): each one from its release,
fetched and held to its sum by `tools/upstream.py`, configured with its own
CMake and built by ninja into a static library - for Kosmos with
`runtime/cxx/kosmos.cmake`, or for this Mac with its own compiler, as the
reference a guest's answers are held to.

    python3 tools/w5_build.py x86_64|aarch64|host [library ...]

What each library becomes is in `build/w5/<arch>/`: `include/` and `lib/`,
CMake's own install, so a later library finds an earlier one the way it
would anywhere (`ZLIB_ROOT`, `PNG_...`). Nothing unpacked is edited; what a
library is told is in `LIBRARIES` below, each with the reason.

A library is configured again when this script, the toolchain or a header
of Kosmos's libc or POSIX layer changed, and ninja decides the rest.
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import upstream                                              # noqa: E402

TOOLCHAIN = os.path.join(ROOT, "runtime", "cxx", "kosmos.cmake")

# In the order they stand on each other. Each library's tools, tests and
# shared builds are off: there is no shared library on Kosmos, its tools
# are programs for a shell it does not have, and its tests are held by
# `tools/run_pictures.py` instead, in the guest and on this Mac.
LIBRARIES = {
    "zlib": [
        "-DZLIB_BUILD_SHARED=OFF", "-DZLIB_BUILD_STATIC=ON",
        "-DZLIB_BUILD_TESTING=OFF",
    ],
    "libpng": [
        "-DPNG_SHARED=OFF", "-DPNG_STATIC=ON", "-DPNG_TESTS=OFF",
        "-DPNG_TOOLS=OFF", "-DPNG_EXECUTABLES=OFF", "-DPNG_FRAMEWORK=OFF",
        # A linker version script is for a shared library.
        "-Dld-version-script=OFF",
        # The vector filters for both of Kosmos's processors: NEON on ARM,
        # SSE2 on x86-64 (`CLAUDE.md`: where a loop can be vector, it is).
        "-DPNG_HARDWARE_OPTIMIZATIONS=ON",
    ],
    "libjpeg-turbo": [
        "-DENABLE_SHARED=OFF", "-DENABLE_STATIC=ON", "-DWITH_TOOLS=OFF",
        "-DWITH_TESTS=OFF",
        # TurboJPEG, its convenience API, loads and saves PNG files through
        # a libspng and a zlib of its own, compiled into it - a second PNG
        # reader and a second deflate (`docs/w5.md`; `CLAUDE.md`: a second
        # copy is a defect). WebKit decodes through libjpeg's own API,
        # `jpeglib.h`, which is what is built.
        "-DWITH_TURBOJPEG=OFF",
        # Its SIMD is the reason it is libjpeg-turbo: NEON, and on x86-64
        # SSE2 and AVX2 assembled by nasm. Required, so a build that quietly
        # fell back to C fails here rather than in a benchmark.
        "-DWITH_SIMD=ON", "-DREQUIRE_SIMD=ON",
        # Kosmos has no environment (`runtime/posix/process.c`), and the one
        # thing the library would put into one is the old API's
        # `TJFLAG_FORCESSE` and the like, asking its SIMD to pretend to be
        # less. Reading one is fine: `getenv` answers nothing.
        "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -DNO_PUTENV",
    ],
    "libwebp": [
        "-DWEBP_LINK_STATIC=ON", "-DBUILD_SHARED_LIBS=OFF",
        "-DWEBP_BUILD_ANIM_UTILS=OFF", "-DWEBP_BUILD_CWEBP=OFF",
        "-DWEBP_BUILD_DWEBP=OFF", "-DWEBP_BUILD_GIF2WEBP=OFF",
        "-DWEBP_BUILD_IMG2WEBP=OFF", "-DWEBP_BUILD_VWEBP=OFF",
        "-DWEBP_BUILD_WEBPINFO=OFF", "-DWEBP_BUILD_WEBPMUX=OFF",
        "-DWEBP_BUILD_EXTRAS=OFF", "-DWEBP_BUILD_LIBWEBPMUX=ON",
        "-DWEBP_ENABLE_SIMD=ON",
        # Threads for its encoder's passes; pthreads are the POSIX layer's.
        "-DWEBP_USE_THREAD=ON",
    ],
    # **W5b, the Text Kit's engines** (`docs/w5.md`).
    "freetype": [
        "-DBUILD_SHARED_LIBS=OFF",
        # zlib for compressed fonts and libpng for colour glyphs in PNG
        # (`sbix`, `CBDT`), both W5a's - required, so neither is quietly
        # left out because it was not found.
        "-DFT_REQUIRE_ZLIB=ON", "-DFT_REQUIRE_PNG=ON",
        # bzip2 for `.pcf.bz2` X11 fonts, which nothing here has; Brotli
        # for WOFF2, which WebKit decodes with its own copy of Google's;
        # HarfBuzz, built after FreeType and on it, so not under it; HVF,
        # a library this system does not have.
        "-DFT_DISABLE_BZIP2=ON", "-DFT_DISABLE_BROTLI=ON",
        "-DFT_DISABLE_HARFBUZZ=ON", "-DFT_DISABLE_HVF=ON",
    ],
    "harfbuzz": [
        "-DBUILD_SHARED_LIBS=OFF", "-DHB_HAVE_FREETYPE=ON",
        "-DHB_BUILD_UTILS=OFF", "-DHB_BUILD_TESTS=OFF",
        # Its subsetter, and the rasteriser, vector and GPU libraries it has
        # grown: FreeType rasterises here, and a second rasteriser is the
        # Text Kit's question to decide, not a side effect of a build.
        "-DHB_BUILD_SUBSET=OFF", "-DHB_BUILD_RASTER=OFF",
        "-DHB_BUILD_VECTOR=OFF", "-DHB_BUILD_GPU=OFF",
        # On this Mac too, so the reference shapes with HarfBuzz's own code
        # rather than asking CoreText.
        "-DHB_HAVE_CORETEXT=OFF",
    ],
}


# What each library is, as ninja names it.
TARGETS = {
    "zlib": ["zlibstatic"],
    "libpng": ["png_static"],
    "libjpeg-turbo": ["jpeg-static"],
    "libwebp": ["webp", "webpdecoder", "webpdemux", "libwebpmux", "sharpyuv"],
    "freetype": ["freetype"],
    "harfbuzz": ["harfbuzz"],
}


def source(name):
    if name in upstream.GIT:
        g = upstream.GIT[name]
        return upstream.checkout(name, g["url"], g["commit"])
    upstream.unpack(name)
    return upstream.where(name)


def configure(arch, name, src, out, prefix):
    stamp = os.path.join(out, ".configured")
    headers = [os.path.join(folder, n)
               for top in (os.path.join(ROOT, "runtime", "include"),
                           os.path.join(ROOT, "runtime", "posix", "include"))
               for folder, _, names in os.walk(top) for n in names]
    newest = max(os.path.getmtime(p) for p in [TOOLCHAIN, __file__] + headers)

    if os.path.exists(stamp) and os.path.getmtime(stamp) >= newest \
            and os.path.exists(os.path.join(out, "build.ninja")):
        return

    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)

    command = ["cmake", "-G", "Ninja", "-S", src, "-B", out, "-Wno-dev",
               "-DCMAKE_BUILD_TYPE=Release",
               "-DCMAKE_INSTALL_PREFIX=" + prefix,
               "-DCMAKE_PREFIX_PATH=" + prefix,
               "-DCMAKE_FIND_ROOT_PATH=" + prefix,
               "-DZLIB_ROOT=" + prefix,
               # Every library's position-independent code off: the image
               # is linked static, at its base.
               "-DCMAKE_POSITION_INDEPENDENT_CODE=OFF"] + LIBRARIES[name]

    if arch == "host":
        command += ["-DCMAKE_C_COMPILER=/usr/bin/clang",
                    "-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0"]
    else:
        # HarfBuzz is C++, compiled against the runtimes as WebKit is.
        command += ["-DCMAKE_TOOLCHAIN_FILE=" + TOOLCHAIN,
                    "-DKOSMOS_ARCH=" + arch, "-DKOSMOS_ROOT=" + ROOT,
                    "-DKOSMOS_LIBCXX_INCLUDE=" + os.path.join(
                        ROOT, "build", "cxx", arch, "include", "c++", "v1"),
                    "-DCMAKE_ASM_NASM_COMPILER=/opt/homebrew/bin/nasm"]

    log = os.path.join(out, "cmake.log")
    with open(log, "w") as f:
        r = subprocess.run(command, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        print(open(log).read()[-6000:])
        sys.exit("w5_build: %s did not configure for %s (%s)" % (name, arch, log))

    with open(stamp, "w") as f:
        f.write("configured\n")


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("x86_64", "aarch64", "host"):
        sys.exit("usage: w5_build.py x86_64|aarch64|host [%s]" % " ".join(LIBRARIES))

    arch = sys.argv[1]
    names = sys.argv[2:] or list(LIBRARIES)
    for name in names:
        if name not in LIBRARIES:
            sys.exit("w5_build: no library %s" % name)

    top = os.path.join(ROOT, "build", "w5", arch)
    prefix = os.path.join(top, "install")

    for name in names:
        out = os.path.join(top, name)
        configure(arch, name, source(name), out, prefix)
        # The libraries only, then CMake's install of what was built: a
        # library's `all` links programs too - libjpeg-turbo's
        # `simdcoverage` - and a program for Kosmos is linked by the
        # Makefile, against the image's own objects, never by CMake.
        r = subprocess.run(["ninja", "-C", out] + TARGETS[name],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode == 0:
            r2 = subprocess.run(["cmake", "--install", out],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            r.stdout += r2.stdout
            r.returncode = r2.returncode
        with open(os.path.join(out, "ninja.log"), "w") as f:
            f.write(r.stdout)
        if r.returncode != 0:
            print(r.stdout[-8000:])
            sys.exit("w5_build: %s did not build for %s" % (name, arch))
        print("w5_build: %s for %s, in %s" % (name, arch, prefix))


if __name__ == "__main__":
    main()
