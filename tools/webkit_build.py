#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
**WebKit for Kosmos** (`docs/w4.md` W4c onwards): WebKitGTK's release, its
JSC-only port, configured with the Kosmos toolchain and built for one
architecture into `build/webkit/<arch>/`.

    python3 tools/webkit_build.py x86_64|aarch64 [target ...]

Targets are ninja's - `WTF` and `bmalloc` by default.

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

PATCHES = os.path.join(ROOT, "runtime", "patches", "webkit")
PORT = os.path.join(ROOT, "runtime", "ports", "webkit")
SRC = os.path.join(ROOT, "build", "webkit", "src")


def fingerprint():
    """What the copy is made from: the release and every patch, in order."""
    h = hashlib.sha256(upstream.RELEASES["webkit"]["sha256"].encode())
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
    if len(sys.argv) < 2 or sys.argv[1] not in ("x86_64", "aarch64"):
        sys.exit("usage: webkit_build.py x86_64|aarch64 [target ...]")

    arch = sys.argv[1]
    targets = sys.argv[2:] or ["WTF", "bmalloc"]

    for needed, how in ((os.path.join(ROOT, "build", "cxx", arch, ".built"),
                         "make ARCH=%s cxx-runtimes" % arch),
                        (os.path.join(ROOT, "build", "icu", arch, "libicudata.a"),
                         "python3 tools/icu_build.py %s" % arch)):
        if not os.path.exists(needed):
            sys.exit("webkit_build: first `%s`" % how)

    patched_source()
    out = os.path.join(ROOT, "build", "webkit", arch)
    configure(arch, out)

    r = subprocess.run(["ninja", "-C", out] + targets)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
