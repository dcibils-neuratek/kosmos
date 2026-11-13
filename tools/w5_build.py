#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
**W5's libraries, built** (`docs/w5.md`): each one from its release,
fetched and held to its sum by `tools/upstream.py`, configured with its own
CMake and built by ninja into a static library - for Kosmos with
`runtime/cxx/kosmos.cmake`, or for this Mac with its own compiler, as the
reference a guest's answers are held to.

    python3 tools/w5_build.py x86_64|aarch64|host|host-x86_64 [library ...]

What each library becomes is in `build/w5/<arch>/`: `include/` and `lib/`,
CMake's own install, so a later library finds an earlier one the way it
would anywhere (`ZLIB_ROOT`, `PNG_...`). Nothing unpacked is edited; what a
library is told is in `LIBRARIES` below, each with the reason.

A library is configured again when this script, the toolchain or a header
of Kosmos's libc or POSIX layer changed, and ninja decides the rest.
"""

import fcntl
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import upstream                                              # noqa: E402

LLVM = os.environ.get("KOSMOS_LLVM", "/opt/homebrew/opt/llvm")

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
    # **W5c**: WebCore's XML parser, and XSLT's when that comes.
    "libxml2": [
        "-DBUILD_SHARED_LIBS=OFF",
        # Its programs are for a shell, and its tests are `runtest.c`,
        # which `tools/run_xml.py` links itself, here and in the guest.
        "-DLIBXML2_WITH_PROGRAMS=OFF", "-DLIBXML2_WITH_TESTS=OFF",
        "-DLIBXML2_WITH_PYTHON=OFF",
        # Kosmos's libc has no iconv, and WebKit hands the parser text it
        # decoded itself; ICU's converters are not linked into it either.
        # Off on this Mac too, so the reference converts with the same
        # tables: libxml2's own, UTF-8, UTF-16, Latin-1 and ISO 8859-x.
        "-DLIBXML2_WITH_ICONV=OFF", "-DLIBXML2_WITH_ICU=OFF",
        "-DLIBXML2_WITH_ISO8859X=ON",
        # Modules are `dlopen`, which a system with no shared libraries does
        # not have.
        "-DLIBXML2_WITH_MODULES=OFF",
        "-DLIBXML2_WITH_THREADS=ON",
    ],
    # **W5d's curl**, by its own CMake, on OpenSSL, zlib and libpsl.
    "curl": [
        "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_STATIC_LIBS=ON",
        "-DBUILD_STATIC_CURL=ON", "-DBUILD_CURL_EXE=ON",
        "-DBUILD_TESTING=OFF", "-DBUILD_LIBCURL_DOCS=OFF", "-DBUILD_MISC_DOCS=OFF",
        "-DENABLE_CURL_MANUAL=OFF", "-DPICKY_COMPILER=OFF",
        "-DCURL_USE_PKGCONFIG=OFF",
        # TLS is OpenSSL, the system's one; deflate is zlib, the one deflate;
        # a cookie's domain is held to libpsl's list.
        "-DCURL_USE_OPENSSL=ON", "-DCURL_ZLIB=ON", "-DCURL_USE_LIBPSL=ON",
        # What Kosmos does not have, or the browser does not ask curl for:
        # SSH, LDAP, Kerberos, IDN (WebKit hands curl hosts in ASCII),
        # brotli and zstd (not built), HTTP/2 (nghttp2, not yet - roadmap).
        "-DCURL_USE_LIBSSH2=OFF", "-DCURL_USE_LIBSSH=OFF", "-DCURL_DISABLE_LDAP=ON",
        "-DCURL_USE_GSSAPI=OFF", "-DUSE_LIBIDN2=OFF", "-DCURL_BROTLI=OFF",
        "-DCURL_ZSTD=OFF", "-DUSE_NGHTTP2=OFF", "-DUSE_NGTCP2=OFF",
        # The POSIX layer's sockets are IPv4 and TCP.
        "-DENABLE_IPV6=OFF",
        # The threaded resolver signals its answer down a socketpair or a
        # pipe, and the POSIX layer has neither: names are looked up as the
        # call is made (`getaddrinfo`, the stack's resolver). Roadmap.
        "-DENABLE_THREADED_RESOLVER=OFF", "-DCURL_DISABLE_SOCKETPAIR=ON",
        # No default bundle of roots yet: `/Kosmos/Settings` is not served
        # (`layout.md`), and the TLS Kit's step onto OpenSSL brings them.
        "-DCURL_CA_BUNDLE=none", "-DCURL_CA_PATH=none",
    ],
}


# **W5d's OpenSSL**, the system's one TLS (`docs/w5.md`), built by its own
# `Configure` rather than CMake, which it does not have - for Kosmos from
# `runtime/cxx/openssl-kosmos.conf`, the targets that file declares. Each
# option, and why:
OPENSSL = [
    # A static library, no modules loaded from files and no `dlopen`:
    # there are no shared libraries here. Providers are compiled in.
    "no-shared", "no-module", "no-dso",
    # `async` is `makecontext` and friends, which the POSIX layer does not
    # have; AF_ALG is a Linux kernel interface; the console's prompt for a
    # password is a terminal's, and memory locked from swap is `mlock`.
    "no-async", "no-ui-console", "no-secure-memory",
    # QUIC and DTLS are datagrams, and the POSIX layer's `socket()` is TCP
    # only (`runtime/posix/socket.c`). HTTP/3 comes with UDP, not before.
    "no-quic", "no-dtls",
    # Its `openssl` program is a shell's, its manuals are files nobody here
    # reads, and its tests are held by `tools/run_ssl.py` - in the guest
    # and on this Mac - which links the ones it runs itself.
    "no-apps", "no-docs", "no-tests",
    # Compressed certificates (RFC 8879) with W5a's zlib, the one deflate.
    "zlib",
]
OPENSSL_TARGET = {
    "x86_64": "kosmos-x86_64",
    "aarch64": "kosmos-aarch64",
    "host": "darwin64-arm64-cc",
    "host-x86_64": "darwin64-x86_64-cc",
}
OPENSSL_CONF = os.path.join(ROOT, "runtime", "cxx", "openssl-kosmos.conf")


def is_host(arch):
    """This Mac: `host` as it is, `host-x86_64` as an Intel Mac, which
    Rosetta runs - the reference for a library whose x86 vector code gives
    other answers than its ARM code, as Skia's does (`docs/w5.md` W5e)."""
    return arch in ("host", "host-x86_64")


def build_openssl(arch, src, out, prefix):
    """OpenSSL's `Configure`, then its libraries and headers installed."""
    stamp = os.path.join(out, ".configured")
    headers = [os.path.join(folder, n)
               for top in (os.path.join(ROOT, "runtime", "include"),
                           os.path.join(ROOT, "runtime", "posix", "include"),
                           os.path.join(ROOT, "runtime", "posix", "refused"))
               for folder, _, names in os.walk(top) for n in names]
    newest = max(os.path.getmtime(p) for p in [OPENSSL_CONF, __file__] + headers)

    if not (os.path.exists(stamp) and os.path.getmtime(stamp) >= newest
            and os.path.exists(os.path.join(out, "Makefile"))):
        shutil.rmtree(out, ignore_errors=True)
        # With `no-apps` it still writes `apps/include/configuration.h`
        # in the build folder, without making the folder first.
        os.makedirs(os.path.join(out, "apps", "include"))
        command = ["perl", os.path.join(src, "Configure"), OPENSSL_TARGET[arch],
                   "--prefix=" + prefix, "--libdir=lib",
                   # Its settings, `openssl.cnf`, in the system's place for
                   # them (`layout.md`), as libxml2's catalog is.
                   "--openssldir=" + ("/Kosmos/Settings/ssl" if not is_host(arch)
                                      else os.path.join(prefix, "ssl")),
                   "--with-zlib-include=" + os.path.join(prefix, "include"),
                   "--with-zlib-lib=" + os.path.join(prefix, "lib")] + OPENSSL
        if not is_host(arch):
            command.insert(3, "--config=" + OPENSSL_CONF)
        log = os.path.join(out, "configure.log")
        with open(log, "w") as f:
            r = subprocess.run(command, cwd=out, stdout=f, stderr=subprocess.STDOUT,
                               env=dict(os.environ, KOSMOS_ROOT=ROOT))
        if r.returncode != 0:
            print(open(log).read()[-6000:])
            sys.exit("w5_build: openssl did not configure for %s (%s)" % (arch, log))
        with open(stamp, "w") as f:
            f.write("configured\n")

    r = subprocess.run(["make", "-j%d" % (os.cpu_count() or 4), "build_libs"], cwd=out,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode == 0:
        r2 = subprocess.run(["make", "install_dev"], cwd=out,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        r.stdout += r2.stdout
        r.returncode = r2.returncode
    with open(os.path.join(out, "make.log"), "w") as f:
        f.write(r.stdout)
    if r.returncode != 0:
        print(r.stdout[-8000:])
        sys.exit("w5_build: openssl did not build for %s" % arch)


# What each library is, as ninja names it.
TARGETS = {
    "zlib": ["zlibstatic"],
    "libpng": ["png_static"],
    "libjpeg-turbo": ["jpeg-static"],
    "libwebp": ["webp", "webpdecoder", "webpdemux", "libwebpmux", "sharpyuv"],
    "freetype": ["freetype"],
    "harfbuzz": ["harfbuzz"],
    "libxml2": ["LibXml2"],
    "curl": ["libcurl_static"],
}


def source(name):
    if name in upstream.GIT:
        g = upstream.GIT[name]
        return upstream.checkout(name, g["url"], g["commit"])
    upstream.unpack(name)
    return upstream.where(name)


def configure(arch, name, src, out, prefix, extra=(), says=""):
    """CMake, once, into `out` - again when the toolchain, this file or a
    runtime header is newer, or when `says` (what the build was told it
    has) is not what it was told last time."""
    extra = list(extra)
    stamp = os.path.join(out, ".configured")
    headers = [os.path.join(folder, n)
               for top in (os.path.join(ROOT, "runtime", "include"),
                           os.path.join(ROOT, "runtime", "posix", "include"))
               for folder, _, names in os.walk(top) for n in names]
    newest = max(os.path.getmtime(p) for p in [TOOLCHAIN, __file__] + headers)

    if os.path.exists(stamp) and os.path.getmtime(stamp) >= newest \
            and os.path.exists(os.path.join(out, "build.ninja")) \
            and open(stamp).read() == "configured\n" + says:
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
               "-DCMAKE_POSITION_INDEPENDENT_CODE=OFF"] + LIBRARIES[name] + extra

    if is_host(arch):
        command += ["-DCMAKE_C_COMPILER=/usr/bin/clang",
                    "-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0"]
        if arch == "host-x86_64":
            command += ["-DCMAKE_OSX_ARCHITECTURES=x86_64"]
    else:
        # HarfBuzz is C++, compiled against the runtimes as WebKit is.
        # A library's own settings (libxml2's default catalog) in the
        # system's place for them (`layout.md`), not in the install folder
        # on this Mac, which would put a path here into the image.
        command += ["-DCMAKE_INSTALL_SYSCONFDIR=/Kosmos/Settings"]
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
        f.write("configured\n" + says)


# **What the image has, as a trial can link against it.** `kosmos.cmake`
# makes CMake's trials static libraries, since there is nothing here to
# link a program against - so a trial is compiled and never linked, and
# `check_function_exists`, which declares the function itself, says yes to
# any name at all. curl will not have that (its CMakeLists.txt turns its
# trials back into programs), and it is right: of the 29 functions it asks
# about that way, 18 - `pipe`, `eventfd`, `sendmsg` - are not here.
#
# So curl's trials are linked, by the same GNU linker as the userland,
# against a stand-in for the image: one object holding a symbol for every
# name the libc, the POSIX layer and the system's calls define, read from
# their objects (`KOSMOS_UBUILD`, the Makefile's). A trial links exactly
# when what it calls exists. A function added to the POSIX layer changes
# the list, and the library is configured again.
def defined_symbols(arch):
    ubuild = os.environ.get("KOSMOS_UBUILD")
    if not ubuild:
        sys.exit("w5_build: KOSMOS_UBUILD names the userland's build folder (the Makefile's)")
    objects = [os.path.join(folder, n)
               for top in ("runtime/libc", "runtime/posix", "user/init")
               for folder, _, names in os.walk(os.path.join(ROOT, ubuild, top))
               for n in names if n.endswith(".o")]
    if not objects:
        sys.exit("w5_build: no objects in %s - the runtime first" % ubuild)
    listed = subprocess.run([os.path.join(LLVM, "bin", "llvm-nm"), "--defined-only"] + objects,
                            capture_output=True, text=True, check=True).stdout
    return sorted({parts[2] for parts in (line.split() for line in listed.splitlines())
                   if len(parts) == 3 and parts[1] in "TWDBR"})


def trial_link(arch, top):
    """The options that make a trial a linked program: the linker, and the
    stand-in object. And the list, for `configure` to compare."""
    names = defined_symbols(arch)
    folder = os.path.join(top, "trial")
    os.makedirs(folder, exist_ok=True)
    stub = os.path.join(folder, "image.S")
    with open(stub, "w") as f:
        f.write("/* Generated by tools/w5_build.py: what the image defines. */\n"
                "\t.section .note.GNU-stack,\"\",%progbits\n\t.text\n")
        for n in names:
            # Weak, so a trial's own `main` - init has one too - wins.
            f.write("\t.weak %s\n%s:\n" % (n, n))
    triple = "%s-unknown-none-elf" % arch
    obj = os.path.join(folder, "image.o")
    subprocess.run([os.path.join(LLVM, "bin", "clang"), "--target=" + triple, "-c", stub,
                    "-o", obj], check=True)
    linker = shutil.which("%s-elf-ld" % arch)
    if linker is None:
        sys.exit("w5_build: no %s-elf-ld" % arch)
    # The object itself, in the linker's flags: CMake carries those into
    # every trial and not its standard libraries, and an object is linked
    # whole wherever it stands on the line.
    return (["-DCMAKE_EXE_LINKER_FLAGS=--ld-path=%s -nostdlib -static -Wl,-e,main %s"
             % (linker, obj)],
            "%d names\n" % len(names) + "\n".join(names))


def curl_extra(arch, src, top, prefix):
    """curl's options that depend on where things are: libpsl, which the
    Makefile compiles rather than any CMake (`LIBPSL_OBJS`), and for Kosmos
    the trials linked against the image's names (`trial_link`)."""
    upstream.unpack("libpsl")
    psl = upstream.where("libpsl")
    library = os.path.join(top, "libpsl", "libpsl.a")
    os.makedirs(os.path.dirname(library), exist_ok=True)
    if is_host(arch):
        # This Mac's curl links a real one: the same two files, as the
        # Makefile compiles them for Kosmos.
        objects = []
        for name in ("psl.c", "lookup_string_in_fixed_set.c"):
            o = os.path.join(top, "libpsl", name + ".o")
            subprocess.run(["/usr/bin/clang", "-O2", "-w", "-DENABLE_BUILTIN",
                            '-DPACKAGE_VERSION="0.23.3"', "-I" + os.path.join(psl, "include"),
                            "-I" + os.path.join(psl, "src"), "-c",
                            os.path.join(psl, "src", name), "-o", o], check=True)
            objects.append(o)
        if os.path.exists(library):
            os.remove(library)
        subprocess.run(["ar", "rcs", library] + objects, check=True)
    elif not os.path.exists(library):
        # For Kosmos the Makefile links `LIBPSL_OBJS`; CMake is told where
        # the header is and given an empty archive to name, since it links
        # nothing here (`curltest.elf` is linked by the Makefile).
        subprocess.run([os.path.join(LLVM, "bin", "llvm-ar"), "rcs", library], check=True)
    trial, says = trial_link(arch, top) if not is_host(arch) else ([], "")
    return (["-DLIBPSL_INCLUDE_DIR=" + os.path.join(psl, "include"),
             "-DLIBPSL_LIBRARY=" + library,
             "-DOPENSSL_ROOT_DIR=" + prefix, "-DOPENSSL_USE_STATIC_LIBS=ON"] + trial,
            says)


def curl_program(arch, out, prefix):
    """curl's program: on this Mac, linked whole by its CMake; for Kosmos,
    the objects CMake compiles for it - its flags, its `curl_config.h` -
    archived with its `main` renamed and `exit` brought back to the kit
    (`posix_program.h`), for the Makefile to link into `curltest.elf`."""
    if is_host(arch):
        return ["curl"]
    listed = subprocess.run(["ninja", "-C", out, "-t", "targets", "all"],
                            capture_output=True, text=True, check=True).stdout
    objects = sorted(line.split(":")[0] for line in listed.splitlines()
                     if line.startswith("src/CMakeFiles/curl.dir/") and
                     line.split(":")[0].endswith((".o", ".obj")))
    if not objects:
        sys.exit("w5_build: curl's program has no objects in %s" % out)
    return objects


def archive_curl_program(objects, out, prefix):
    tool = os.path.join(prefix, "lib", "libcurltool.a")
    if os.path.exists(tool):
        os.remove(tool)
    subprocess.run([os.path.join(LLVM, "bin", "llvm-ar"), "rcs", tool]
                   + [os.path.join(out, o) for o in objects], check=True)
    subprocess.run([os.path.join(LLVM, "bin", "llvm-objcopy"),
                    "--redefine-sym", "main=curltest_curl_main",
                    "--redefine-sym", "exit=posix_program_exit", tool], check=True)


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("x86_64", "aarch64", "host", "host-x86_64"):
        sys.exit("usage: w5_build.py x86_64|aarch64|host|host-x86_64 [%s openssl]" % " ".join(LIBRARIES))

    arch = sys.argv[1]
    names = sys.argv[2:] or [n for n in LIBRARIES if n != "curl"] + ["openssl", "curl"]
    for name in names:
        if name not in LIBRARIES and name != "openssl":
            sys.exit("w5_build: no library %s" % name)

    top = os.path.join(ROOT, "build", "w5", arch)
    prefix = os.path.join(top, "install")

    # **One build of a folder at a time.** The gate runs suites side by
    # side, and each that holds a guest to this Mac builds the Mac's
    # libraries first (`run_xml.py`, `run_ssl.py`'s parts): two configures in
    # one folder at once leave neither. The second waits for the first, and
    # then finds nothing to do.
    os.makedirs(top, exist_ok=True)
    lock = open(os.path.join(top, ".lock"), "w")
    fcntl.flock(lock, fcntl.LOCK_EX)

    for name in names:
        out = os.path.join(top, name)
        if name == "openssl":
            build_openssl(arch, source(name), out, prefix)
            print("w5_build: openssl for %s, in %s" % (arch, prefix))
            continue
        src = source(name)
        extra, says = curl_extra(arch, src, top, prefix) if name == "curl" else ([], "")
        configure(arch, name, src, out, prefix, extra, says)
        # The libraries only, then CMake's install of what was built: a
        # library's `all` links programs too - libjpeg-turbo's
        # `simdcoverage` - and a program for Kosmos is linked by the
        # Makefile, against the image's own objects, never by CMake.
        targets = TARGETS[name] + (curl_program(arch, out, prefix) if name == "curl" else [])
        r = subprocess.run(["ninja", "-C", out] + targets,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode == 0 and name == "curl" and not is_host(arch):
            archive_curl_program(targets[len(TARGETS[name]):], out, prefix)
        if r.returncode == 0 and name == "curl" and not is_host(arch):
            # CMake's install of curl wants its program, which is linked by
            # the Makefile here, not by CMake: the library and its headers.
            os.makedirs(os.path.join(prefix, "lib"), exist_ok=True)
            shutil.copy2(os.path.join(out, "lib", "libcurl.a"), os.path.join(prefix, "lib"))
            shutil.rmtree(os.path.join(prefix, "include", "curl"), ignore_errors=True)
            shutil.copytree(os.path.join(src, "include", "curl"),
                            os.path.join(prefix, "include", "curl"))
        elif r.returncode == 0:
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
