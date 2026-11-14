#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
**WebKit for Kosmos** (`docs/w4.md` W4c onwards): WebKitGTK's release, its
JSC-only port, configured with the Kosmos toolchain and built for one
architecture into `build/webkit/<arch>/`.

    python3 tools/webkit_build.py x86_64|aarch64 [target ...]

Targets are ninja's - `WTF`, `bmalloc`, `JavaScriptCore` and `Skia` by default.

**The source** is fetched and held to its sum (`tools/upstream.py`), never
committed and never edited. What is built is a copy of it under
`build/webkit/src/` - cloned file by file, which on this Mac's file system
costs no space until a file changes - with `runtime/patches/webkit/`
applied, in order, and Kosmos's own files for it (`runtime/ports/webkit/`,
laid out as WebKit's tree is) copied in. The copy is made again when the release or a patch
or a port file changes; CMake configures again when the copy, the options
(`runtime/cxx/jsc.cmake`) or the toolchain (`runtime/cxx/kosmos.cmake`)
changed.

**What the toolchain is given beyond what the runtimes' build gives it**:
libc++'s headers, as the C++ runtimes installed them, and ICU, as
`tools/icu_build.py` laid it out.
"""

import hashlib
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import upstream                                              # noqa: E402

LLVM = os.environ.get("KOSMOS_LLVM", "/opt/homebrew/opt/llvm")
PATCHES = os.path.join(ROOT, "runtime", "patches", "webkit")
PORT = os.path.join(ROOT, "runtime", "ports", "webkit")
SRC = os.path.join(ROOT, "build", "webkit", "src")
KHRONOS = os.path.join(SRC, "Source", "ThirdParty", "ANGLE", "include")


def fingerprint():
    """What the copy is made from: the release and every patch, in order."""
    h = hashlib.sha256(upstream.RELEASES["webkit"]["sha256"].encode())
    o = upstream.WEBKIT_OVERLAY
    h.update(o["commit"].encode())
    for pattern in o["paths"]:
        h.update(pattern.encode())
    for name in sorted(os.listdir(PATCHES)):
        if name.endswith(".patch"):
            h.update(name.encode())
            with open(os.path.join(PATCHES, name), "rb") as f:
                h.update(f.read())
    for path in port_files():
        h.update(path.encode())
        with open(os.path.join(PORT, path), "rb") as f:
            h.update(f.read())
    return h.hexdigest()


def port_files():
    """Kosmos's own files for WebKit, by their place in its tree."""
    found = []
    for folder, _, names in os.walk(PORT):
        for name in names:
            found.append(os.path.relpath(os.path.join(folder, name), PORT))
    return sorted(found)


def patched_source():
    stamp = os.path.join(SRC, ".patched")
    want = fingerprint()

    if os.path.exists(stamp) and open(stamp).read() == want:
        return SRC

    release = upstream.where("webkit") if os.path.exists(
        os.path.join(upstream.UNPACKED, "webkit", ".unpacked")) else None
    if release is None:
        upstream.unpack("webkit")
        release = upstream.where("webkit")

    shutil.rmtree(SRC, ignore_errors=True)
    os.makedirs(os.path.dirname(SRC), exist_ok=True)
    subprocess.run(["cp", "-Rc", release, SRC], check=True)

    # What the release does not carry, from WebKit's git at its tag's
    # commit (`docs/w6.md`): laid over the copy before the patches, so a
    # patch may change those too. A file the release already has is the
    # release's - the overlay only adds - and one that differed would mean
    # the two were not cut from the same source, which is refused.
    overlay = upstream.webkit_overlay()
    added = 0
    for folder, dirs, names in os.walk(overlay):
        dirs[:] = [d for d in dirs if d != ".git"]
        for name in names:
            path = os.path.relpath(os.path.join(folder, name), overlay)
            into = os.path.join(SRC, path)
            if os.path.exists(into):
                with open(into, "rb") as a, open(os.path.join(folder, name), "rb") as b:
                    if a.read() != b.read():
                        sys.exit("webkit_build: %s differs between the release and its tag" % path)
                continue
            os.makedirs(os.path.dirname(into), exist_ok=True)
            shutil.copy2(os.path.join(folder, name), into)
            added += 1
    print("webkit_build: %d files from WebKit's git at %s"
          % (added, upstream.WEBKIT_OVERLAY["commit"][:12]))

    for name in sorted(os.listdir(PATCHES)):
        if name.endswith(".patch"):
            with open(os.path.join(PATCHES, name), "rb") as f:
                r = subprocess.run(["patch", "-s", "-p1", "-d", SRC], stdin=f)
            if r.returncode != 0:
                sys.exit("webkit_build: %s does not apply" % name)

    # Kosmos's own files - a platform's sources WebKit has none of - in the
    # places the patches' CMake lists name them.
    for path in port_files():
        os.makedirs(os.path.dirname(os.path.join(SRC, path)), exist_ok=True)
        shutil.copy2(os.path.join(PORT, path), os.path.join(SRC, path))

    with open(stamp, "w") as f:
        f.write(want)
    return SRC


def configure(arch, out):
    cxxrt = os.path.join(ROOT, "build", "cxx", arch)
    icu = os.path.join(ROOT, "build", "icu", arch)
    w5 = os.path.join(ROOT, "build", "w5", arch, "install")
    options = os.path.join(ROOT, "runtime", "cxx", "jsc.cmake")
    toolchain = os.path.join(ROOT, "runtime", "cxx", "kosmos.cmake")
    stamp = os.path.join(out, ".configured")
    # CMake's checks - which functions exist, which headers - are answered
    # by the runtimes and the libc's and POSIX layer's headers, so a change
    # to any of them is a reason to ask again (`HAVE_LOCALTIME_R` was
    # remembered as absent after `localtime_r` arrived).
    headers = [os.path.join(folder, name)
               for top in (os.path.join(ROOT, "runtime", "include"),
                           os.path.join(ROOT, "runtime", "posix", "include"))
               for folder, _, names in os.walk(top) for name in names]
    newest = max(os.path.getmtime(p) for p in
                 [options, toolchain, os.path.join(SRC, ".patched"), __file__,
                  os.path.join(cxxrt, ".built")] + headers)

    if os.path.exists(stamp) and os.path.getmtime(stamp) >= newest \
            and os.path.exists(os.path.join(out, "build.ninja")):
        return

    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)

    command = [
        "cmake", "-G", "Ninja", "-S", SRC, "-B", out, "-Wno-dev",
        "-C", options,
        "-DCMAKE_TOOLCHAIN_FILE=" + toolchain,
        "-DKOSMOS_ARCH=" + arch, "-DKOSMOS_ROOT=" + ROOT,
        "-DKOSMOS_LIBCXX_INCLUDE=" + os.path.join(cxxrt, "include", "c++", "v1"),
        "-DICU_ROOT=" + icu,
        "-DICU_INCLUDE_DIR=" + os.path.join(icu, "include"),
        "-DICU_UC_LIBRARY_RELEASE=" + os.path.join(icu, "libicuuc.a"),
        "-DICU_I18N_LIBRARY_RELEASE=" + os.path.join(icu, "libicui18n.a"),
        "-DICU_DATA_LIBRARY_RELEASE=" + os.path.join(icu, "libicudata.a"),
        # Skia's (W5e): FreeType, libpng over zlib and libjpeg-turbo, as
        # `tools/w5_build.py` installed them.
        "-DFREETYPE_INCLUDE_DIR_ft2build=" + os.path.join(w5, "include", "freetype2"),
        "-DFREETYPE_INCLUDE_DIR_freetype2=" + os.path.join(w5, "include", "freetype2"),
        "-DFREETYPE_LIBRARY=" + os.path.join(w5, "lib", "libfreetype.a"),
        "-DPNG_PNG_INCLUDE_DIR=" + os.path.join(w5, "include"),
        "-DPNG_LIBRARY=" + os.path.join(w5, "lib", "libpng.a"),
        "-DZLIB_INCLUDE_DIR=" + os.path.join(w5, "include"),
        "-DZLIB_LIBRARY=" + os.path.join(w5, "lib", "libz.a"),
        "-DJPEG_INCLUDE_DIR=" + os.path.join(w5, "include"),
        "-DJPEG_LIBRARY=" + os.path.join(w5, "lib", "libjpeg.a"),
    ] + webcore_libraries(arch, w5)
    log = os.path.join(out, "cmake.log")

    with open(log, "w") as f:
        r = subprocess.run(command, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        print(open(log).read()[-6000:])
        sys.exit("webkit_build: CMake did not configure (%s)" % log)

    with open(stamp, "w") as f:
        f.write("configured\n")


def webcore_libraries(arch, w5):
    """**What WebCore links, as W5 built it** (`docs/w6.md` W6b): each named
    to CMake by its file, as Skia's are, so nothing on this Mac can answer
    instead. SQLite and libpsl have no install - the Makefile compiles them,
    one file and two, into what links them - so CMake is shown their
    headers and an empty archive to name: WebKit's CMake links nothing that
    runs on Kosmos, and the image that does is the Makefile's."""
    lib = lambda name: os.path.join(w5, "lib", name)
    upstream.unpack("sqlite")
    upstream.unpack("libpsl")
    empty = os.path.join(ROOT, "build", "webkit", arch + "-empty.a")
    if not os.path.exists(empty):
        subprocess.run([os.path.join(LLVM, "bin", "llvm-ar"), "rcs", empty], check=True)
    return [
        "-DCURL_INCLUDE_DIR=" + os.path.join(w5, "include"),
        "-DCURL_LIBRARY_RELEASE=" + lib("libcurl.a"),
        "-DCURL_NO_CURL_CMAKE=ON",
        "-DOPENSSL_ROOT_DIR=" + w5, "-DOPENSSL_USE_STATIC_LIBS=ON",
        "-DOPENSSL_INCLUDE_DIR=" + os.path.join(w5, "include"),
        "-DOPENSSL_CRYPTO_LIBRARY=" + lib("libcrypto.a"),
        "-DOPENSSL_SSL_LIBRARY=" + lib("libssl.a"),
        "-DHarfBuzz_INCLUDE_DIR=" + os.path.join(w5, "include", "harfbuzz"),
        "-DHarfBuzz_LIBRARY=" + lib("libharfbuzz.a"),
        "-DHarfBuzz_ICU_INCLUDE_DIR=" + os.path.join(w5, "include", "harfbuzz"),
        "-DHarfBuzz_ICU_LIBRARY=" + lib("libharfbuzz-icu.a"),
        "-DLIBXML2_INCLUDE_DIR=" + os.path.join(w5, "include", "libxml2"),
        "-DLIBXML2_LIBRARY=" + lib("libxml2.a"),
        "-DWebP_INCLUDE_DIR=" + os.path.join(w5, "include"),
        "-DWebP_LIBRARY=" + lib("libwebp.a"),
        "-DWebP_DEMUX_LIBRARY=" + lib("libwebpdemux.a"),
        # W5f's (`docs/w5.md`): woff2, which reads `<woff2/decode.h>`, and
        # libavif, whose find module looks in `include/avif` for its version
        # and reaches `<avif/avif.h>` through `include` as the rest do.
        "-DWOFF2_INCLUDE_DIR=" + os.path.join(w5, "include"),
        "-DWOFF2_LIBRARY=" + lib("libwoff2common.a"),
        "-DWOFF2_DEC_LIBRARY=" + lib("libwoff2dec.a"),
        "-DAVIF_INCLUDE_DIR=" + os.path.join(w5, "include", "avif"),
        "-DAVIF_LIBRARY=" + lib("libavif.a"),
        "-DSQLite3_INCLUDE_DIR=" + upstream.where("sqlite"),
        "-DSQLite3_LIBRARY=" + empty,
        "-DLibPSL_INCLUDE_DIR=" + os.path.join(upstream.where("libpsl"), "include"),
        "-DLibPSL_LIBRARY=" + empty,
        # The Khronos headers WebKit carries, and the EGL Kit, which the
        # Makefile links (`user/kits/egl/`, `docs/w6.md` W6b).
        "-DEGL_INCLUDE_DIRS=" + KHRONOS,
        "-DEGL_LIBRARIES=" + empty,
        "-DOpenGLES2_INCLUDE_DIR=" + KHRONOS,
        "-DOpenGLES2_LIBRARY=" + empty,
    ]


def configure_reference(arch, out):
    """**Skia's reference, on this Mac** (`docs/w5.md` W5e): the same tree
    and patches by WebKit's own CMake natively, with the Kosmos choices
    taken (`runtime/cxx/skia-reference.cmake`). `host-x86_64` is an Intel
    Mac's, which Rosetta runs: Skia's x86 vector code is what the x86 guest
    runs, and it can round otherwise than its ARM code. Pinned to the
    baseline `x86-64`, as the guest's compiler is - Apple's clang would
    choose a newer processor, and Skia's vector level with it."""
    options = os.path.join(ROOT, "runtime", "cxx", "skia-reference.cmake")
    w5 = os.path.join(ROOT, "build", "w5", arch, "install")
    stamp = os.path.join(out, ".configured")
    newest = max(os.path.getmtime(p) for p in
                 [options, os.path.join(SRC, ".patched"), __file__,
                  os.path.join(ROOT, "runtime", "cxx", "skia-reference", "CMakeLists.txt")])

    if os.path.exists(stamp) and os.path.getmtime(stamp) >= newest \
            and os.path.exists(os.path.join(out, "build.ninja")):
        return

    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    machine = "x86_64" if arch == "host-x86_64" else "arm64"
    flags = "-march=x86-64" if arch == "host-x86_64" else ""

    command = [
        "cmake", "-G", "Ninja", "-S", os.path.join(ROOT, "runtime", "cxx", "skia-reference"),
        "-B", out, "-Wno-dev", "-C", options, "-DWEBKIT_SOURCE=" + SRC,
        "-DCMAKE_C_COMPILER=/usr/bin/clang", "-DCMAKE_CXX_COMPILER=/usr/bin/clang++",
        "-DCMAKE_OSX_ARCHITECTURES=" + machine,
        # WebKit's WTF_CPU comes from the processor CMake says it builds
        # for, which on this Mac is its own unless told: x86's vector code
        # (SK_ENABLE_AVX512_OPTS, skcms's Haswell paths) follows from it.
        "-DCMAKE_SYSTEM_NAME=Darwin", "-DCMAKE_SYSTEM_PROCESSOR=" + machine,
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0",
        "-DCMAKE_C_FLAGS=" + flags, "-DCMAKE_CXX_FLAGS=" + flags,
        "-DFREETYPE_INCLUDE_DIR_ft2build=" + os.path.join(w5, "include", "freetype2"),
        "-DFREETYPE_INCLUDE_DIR_freetype2=" + os.path.join(w5, "include", "freetype2"),
        "-DFREETYPE_LIBRARY=" + os.path.join(w5, "lib", "libfreetype.a"),
        "-DPNG_PNG_INCLUDE_DIR=" + os.path.join(w5, "include"),
        "-DPNG_LIBRARY=" + os.path.join(w5, "lib", "libpng.a"),
        "-DZLIB_INCLUDE_DIR=" + os.path.join(w5, "include"),
        "-DZLIB_LIBRARY=" + os.path.join(w5, "lib", "libz.a"),
        "-DJPEG_INCLUDE_DIR=" + os.path.join(w5, "include"),
        "-DJPEG_LIBRARY=" + os.path.join(w5, "lib", "libjpeg.a"),
    ]
    log = os.path.join(out, "cmake.log")

    with open(log, "w") as f:
        r = subprocess.run(command, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        print(open(log).read()[-6000:])
        sys.exit("webkit_build: CMake did not configure (%s)" % log)

    with open(stamp, "w") as f:
        f.write("configured\n")


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("x86_64", "aarch64", "host", "host-x86_64"):
        sys.exit("usage: webkit_build.py x86_64|aarch64|host|host-x86_64 [target ...]")

    arch = sys.argv[1]

    if arch.startswith("host"):
        patched_source()
        out = os.path.join(ROOT, "build", "webkit", arch)
        configure_reference(arch, out)
        sys.exit(subprocess.run(["ninja", "-C", out] + (sys.argv[2:] or ["Skia"])).returncode)

    targets = sys.argv[2:] or ["WTF", "bmalloc", "JavaScriptCore", "Skia", "PAL", "WebCore"]

    for needed, how in ((os.path.join(ROOT, "build", "cxx", arch, ".built"),
                         "make ARCH=%s cxx-runtimes" % arch),
                        (os.path.join(ROOT, "build", "icu", arch, "libicudata.a"),
                         "python3 tools/icu_build.py %s" % arch)):
        if not os.path.exists(needed):
            sys.exit("webkit_build: first `%s`" % how)

    patched_source()
    out = os.path.join(ROOT, "build", "webkit", arch)
    configure(arch, out)

    # JavaScriptCore's bytecode, its compiler and the interpreter's glue
    # are an object library of their own, JavaScriptCoreJIT - so named
    # even with the JIT off - which CMake hands to whatever links the
    # engine rather than putting it in libJavaScriptCore.a.
    if "JavaScriptCore" in targets and "JavaScriptCoreJIT" not in targets:
        targets = targets + ["JavaScriptCoreJIT"]
    # WebCore is the same, four times over (`docs/w6.md` W6b): its style,
    # its DOM and rendering, its JavaScript bindings and its inspector are
    # object libraries of their own, which CMake hands to WebKit2 - which
    # Kosmos does not build yet - and `WebCore` does not depend on.
    if "WebCore" in targets:
        targets = targets + [t for t in WEBCORE_PARTS if t not in targets]

    r = subprocess.run(["ninja", "-C", out] + targets)

    for part in ["JavaScriptCoreJIT"] + WEBCORE_PARTS:
        if r.returncode == 0 and part in targets:
            r = archive_objects(out, part)

    sys.exit(r.returncode)


WEBCORE_PARTS = ["WebCoreStyle", "WebCoreDOMAndRendering", "WebCoreJSBindings",
                 "WebCoreInspector"]


def archive_objects(out, part):
    """`lib/libPART.a`: an object library's objects, as one thin archive
    like CMake's own, so the Makefile links one more library rather than
    hundreds of objects it would have to be told the names of."""
    project = "JavaScriptCore" if part == "JavaScriptCoreJIT" else "WebCore"
    head = "build Source/%s/%s: phony " % (project, part)
    with open(os.path.join(out, "build.ninja"), encoding="utf-8") as f:
        for line in f:
            if line.startswith(head):
                objects = [o for o in line.split()[3:] if o.endswith(".obj")]
                break
        else:
            sys.exit("webkit_build: no %s in build.ninja" % part)

    library = os.path.join(out, "lib", "lib%s.a" % part)

    if os.path.exists(library):
        os.remove(library)

    return subprocess.run(["/opt/homebrew/opt/llvm/bin/llvm-ar", "rcsT", library]
                          + [os.path.join(out, o) for o in objects])


if __name__ == "__main__":
    main()
