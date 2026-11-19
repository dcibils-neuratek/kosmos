#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
**The Vulkan conformance suite for Kosmos** (`docs/gpu3d.md` G2): Khronos's
VK-GL-CTS 1.4.7.1, configured with the Kosmos toolchain for its own target
(`runtime/ports/cts/targets/kosmos/`) and built for one architecture into
`build/cts/<arch>/`.

    python3 tools/cts_build.py x86_64|aarch64 [target ...]

Targets are ninja's; by default what `deqp-vk` links (`archives`, below).

**The source** is the release and six of its externals, each fetched by
git at its commit and refused at any other (`tools/upstream.py`, `CTS`),
never committed and never edited. What is built is a copy under
`build/cts/tree/` - cloned file by file, free on this Mac's file system
until a file changes - with the externals laid in where the release's
`external/fetch_sources.py` would put them, `runtime/patches/cts/` applied
in order, and Kosmos's own files from `runtime/ports/cts/` at their places.
CMake configures again when the copy, the toolchain, this file or a header
of the libc or the POSIX layer changed, as LLVM's build does.

**What it is configured with**: `DE_OS_UNIX` - the CTS's base libraries
written on POSIX, which inside a Kosmos process is the POSIX layer - with
C++ exceptions and RTTI, which the suite's every error and test case is
made of; and zlib and libpng from W5 (`tools/w5_build.py`), the same ones
WebKit is linked with.
"""

import hashlib
import os
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import upstream                                              # noqa: E402

PATCHES = os.path.join(ROOT, "runtime", "patches", "cts")
PORT = os.path.join(ROOT, "runtime", "ports", "cts")
SRC = os.path.join(ROOT, "build", "cts", "tree")
EXTERNALS = ("glslang", "spirv-tools", "spirv-headers", "amber", "jsoncpp", "vulkansc-pcutil")


def sources():
    return {name: upstream.checkout(name, v["url"], v["commit"])
            for name, v in upstream.CTS.items()}


def fingerprint():
    """The seven commits, and every patch and port file, in order."""
    h = hashlib.sha256(repr(sorted((n, v["commit"]) for n, v in upstream.CTS.items())).encode())
    return upstream.patches_digest(h, PATCHES, PORT).hexdigest()


def patched_source():
    where = sources()

    def externals(cut):
        for name in EXTERNALS:
            into = os.path.join(cut, "external", name, "src")
            if os.path.exists(into):
                sys.exit("cts_build: the release already has external/%s/src" % name)
            os.makedirs(os.path.dirname(into), exist_ok=True)
            subprocess.run(["cp", "-Rc", where[name], into], check=True)

    return upstream.patched_copy(where["vk-gl-cts"], SRC, fingerprint(), PATCHES,
                                 port=PORT, overlay=externals, who="cts_build")


def configure(arch, out):
    cxxrt = os.path.join(ROOT, "build", "cxx", arch)
    w5 = os.path.join(ROOT, "build", "w5", arch, "install")
    toolchain = os.path.join(ROOT, "runtime", "cxx", "kosmos.cmake")
    stamp = os.path.join(out, ".configured")
    headers = [os.path.join(folder, name)
               for top in (os.path.join(ROOT, "runtime", "include"),
                           os.path.join(ROOT, "runtime", "posix", "include"))
               for folder, _, names in os.walk(top) for name in names]
    newest = max(os.path.getmtime(p) for p in
                 [toolchain, os.path.join(SRC, ".patched"), __file__,
                  os.path.join(cxxrt, ".built")] + headers)

    if os.path.exists(stamp) and os.path.getmtime(stamp) >= newest \
            and os.path.exists(os.path.join(out, "build.ninja")):
        return

    os.makedirs(out, exist_ok=True)
    # Never from the cache it left (`testing.md` 18.813): every value is
    # read from the files again; the build tree stays.
    if os.path.exists(os.path.join(out, "CMakeCache.txt")):
        os.remove(os.path.join(out, "CMakeCache.txt"))

    command = [
        "cmake", "-G", "Ninja", "-S", SRC, "-B", out, "-Wno-dev",
        "-DCMAKE_TOOLCHAIN_FILE=" + toolchain,
        "-DKOSMOS_ARCH=" + arch, "-DKOSMOS_ROOT=" + ROOT,
        "-DKOSMOS_LIBCXX_INCLUDE=" + os.path.join(cxxrt, "include", "c++", "v1"),
        "-DCMAKE_BUILD_TYPE=Release",
        "-DDEQP_TARGET=kosmos",
        "-DDE_OS=DE_OS_UNIX",
        "-DDEQP_DISABLE_VK_VIDEO_TESTS=ON",
        "-DZLIB_INCLUDE_DIR=" + os.path.join(w5, "include"),
        "-DZLIB_LIBRARY=" + os.path.join(w5, "lib", "libz.a"),
        "-DPNG_PNG_INCLUDE_DIR=" + os.path.join(w5, "include"),
        "-DPNG_LIBRARY=" + os.path.join(w5, "lib", "libpng.a"),
    ]
    log = os.path.join(out, "cmake.log")
    with open(log, "w") as f:
        r = subprocess.run(command, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        print(open(log).read()[-6000:])
        sys.exit("cts_build: CMake did not configure (%s)" % log)

    with open(stamp, "w") as f:
        f.write("configured\n")


# **What is built**: what upstream's own `deqp-vk` links, read from that
# link's command - the package of the groups Kosmos runs
# (`runtime/patches/cts/0003`), the framework, glslang and SPIRV-Tools -
# and the package's entry, which registers it. `tcuMain.cpp` is not: the
# kit's entry (`user/kits/vkcts/`) is Kosmos's `main`. The list goes to
# `deqpvk.txt` for the Makefile's link, as lavapipe's does.
EXECUTABLE = "deqp-vk"
ENTRY = "vktTestPackageEntry.cpp.obj"


def archives(out):
    r = subprocess.run(["ninja", "-C", out, "-t", "commands", EXECUTABLE],
                       capture_output=True, text=True)
    if r.returncode != 0 or not r.stdout.strip():
        sys.exit("cts_build: no command for %s" % EXECUTABLE)
    found = []
    for word in shlex.split(r.stdout.strip().splitlines()[-1]):
        if (word.endswith(".a") or word.endswith(ENTRY)) and word not in found:
            found.append(word)
    if not [w for w in found if w.endswith(ENTRY)] or len(found) < 10:
        sys.exit("cts_build: %s's link is not the one this file reads" % EXECUTABLE)
    return found


def write_if_changed(path, text):
    if not os.path.exists(path) or open(path).read() != text:
        with open(path, "w") as f:
            f.write(text)


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("x86_64", "aarch64"):
        sys.exit("usage: cts_build.py x86_64|aarch64 [target ...]")
    arch = sys.argv[1]

    for need, what in ((os.path.join("cxx", arch, ".built"), "make ARCH=%s cxx-runtimes"),
                       (os.path.join("w5", arch, "install", "lib", "libpng.a"),
                        "python3 tools/w5_build.py %s")):
        if not os.path.exists(os.path.join(ROOT, "build", need)):
            sys.exit("cts_build: first `%s`" % (what % arch))

    os.makedirs(PATCHES, exist_ok=True)
    patched_source()
    out = os.path.join(ROOT, "build", "cts", arch)
    configure(arch, out)

    keep = ["-k", "0"] if os.environ.get("CTS_KEEP_GOING") else []
    linked = archives(out)
    # What is not this build's own - W5's zlib and libpng - is named whole.
    ours = [w for w in linked if not os.path.isabs(w)]
    r = subprocess.run(["ninja", "-C", out] + keep + (sys.argv[2:] or ours))
    if r.returncode == 0 and not sys.argv[2:]:
        write_if_changed(os.path.join(out, "deqpvk.txt"),
                         "".join((w if os.path.isabs(w) else os.path.join(out, w)) + "\n"
                                 for w in linked))
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
