#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
**LLVM for Kosmos** (`docs/gpu3d.md` G1): LLVM 23.1.2's libraries - code
generation for x86-64 and AArch64, and the ORC JIT - configured with the
Kosmos toolchain and built for one architecture into `build/llvm/<arch>/`.

    python3 tools/llvm_build.py x86_64|aarch64 [target ...]

Targets are ninja's; by default `LIBRARIES`, below, and what they need.

**The source** is the release, fetched and held to its sum
(`tools/upstream.py`, the same tarball the C++ runtimes were vendored from),
never committed and never edited. What is built is a copy of it under
`build/llvm/tree/` - cloned file by file, which on this Mac's file system
costs no space until a file changes - with `runtime/patches/llvm/` applied
in order. The copy is cut again when the release or a patch changes, and
carried over only where its bytes differ, so ninja rebuilds what a patch
touched and nothing else; CMake configures again when the copy, the
options (`runtime/cxx/llvm.cmake`), the toolchain (`runtime/cxx/kosmos.cmake`)
or a header of the libc or the POSIX layer changed.

**What the toolchain is given beyond the C++ runtimes' build**: libc++'s
headers, as `make cxx-runtimes` installed them.
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

PATCHES = os.path.join(ROOT, "runtime", "patches", "llvm")
SRC = os.path.join(ROOT, "build", "llvm", "tree")


def patches():
    return sorted(n for n in os.listdir(PATCHES) if n.endswith(".patch"))


def fingerprint():
    """What the copy is made from: the release and every patch, in order."""
    h = hashlib.sha256(upstream.unpacked_as("llvm").encode())
    for name in patches():
        h.update(name.encode())
        with open(os.path.join(PATCHES, name), "rb") as f:
            h.update(f.read())
    return h.hexdigest()


def patched_source():
    stamp = os.path.join(SRC, ".patched")
    want = fingerprint()
    if os.path.exists(stamp) and open(stamp).read() == want:
        return SRC

    upstream.unpack("llvm")
    release = upstream.where("llvm")

    cut = SRC + ".next"
    shutil.rmtree(cut, ignore_errors=True)
    os.makedirs(os.path.dirname(SRC), exist_ok=True)
    subprocess.run(["cp", "-Rc", release, cut], check=True)

    for name in patches():
        with open(os.path.join(PATCHES, name), "rb") as f:
            r = subprocess.run(["patch", "-s", "-p1", "-d", cut], stdin=f)
        if r.returncode != 0:
            sys.exit("llvm_build: %s does not apply" % name)

    # Only what differs is carried into the copy ninja built from, so a
    # patch to one file rebuilds that file and what includes it.
    if os.path.isdir(SRC):
        r = subprocess.run(["rsync", "-rl", "--checksum", "--delete",
                            "--exclude", "/.patched", cut + "/", SRC + "/"])
        if r.returncode != 0:
            sys.exit("llvm_build: the new copy did not carry into %s" % SRC)
        shutil.rmtree(cut)
    else:
        os.rename(cut, SRC)

    with open(stamp, "w") as f:
        f.write(want)
    return SRC


def configure(arch, out):
    cxxrt = os.path.join(ROOT, "build", "cxx", arch)
    options = os.path.join(ROOT, "runtime", "cxx", "llvm.cmake")
    toolchain = os.path.join(ROOT, "runtime", "cxx", "kosmos.cmake")
    stamp = os.path.join(out, ".configured")
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

    os.makedirs(out, exist_ok=True)
    # Never from the cache it left (`testing.md` 18.813, WebKit's lesson):
    # every value is read from the files again; the build tree stays.
    if os.path.exists(os.path.join(out, "CMakeCache.txt")):
        os.remove(os.path.join(out, "CMakeCache.txt"))

    triple = arch + "-unknown-none-elf"
    command = [
        "cmake", "-G", "Ninja", "-S", os.path.join(SRC, "llvm"), "-B", out, "-Wno-dev",
        "-C", options,
        "-DCMAKE_TOOLCHAIN_FILE=" + toolchain,
        "-DKOSMOS_ARCH=" + arch, "-DKOSMOS_ROOT=" + ROOT,
        "-DKOSMOS_LIBCXX_INCLUDE=" + os.path.join(cxxrt, "include", "c++", "v1"),
        # The processor a JIT writes for by default is the one it runs on.
        "-DLLVM_HOST_TRIPLE=" + triple,
        "-DLLVM_DEFAULT_TARGET_TRIPLE=" + triple,
    ]
    log = os.path.join(out, "cmake.log")
    with open(log, "w") as f:
        r = subprocess.run(command, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        print(open(log).read()[-6000:])
        sys.exit("llvm_build: CMake did not configure (%s)" % log)

    with open(stamp, "w") as f:
        f.write("configured\n")


# **What is built** (`docs/gpu3d.md` G1): what the ORC JIT and both back
# ends need, and what Mesa's lavapipe asks LLVM for - its IR's text and
# bitcode, the optimiser's passes, coroutines, a disassembler - and ninja
# builds what each of them stands on. Not every library LLVM has: the
# rest - debug information's readers, YAML objects, coverage, Mach-O's
# text stubs - is for tools Kosmos does not build, and some of it reads
# files through iostreams the C++ library here does not have.
LIBRARIES = ["LLVMOrcJIT", "LLVMJITLink", "LLVMMCJIT",
             "LLVMX86CodeGen", "LLVMX86AsmParser", "LLVMX86Disassembler",
             "LLVMAArch64CodeGen", "LLVMAArch64AsmParser", "LLVMAArch64Disassembler",
             "LLVMAsmParser", "LLVMBitWriter", "LLVMPasses", "LLVMipo",
             "LLVMCoroutines", "LLVMMCDisassembler"]


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("x86_64", "aarch64"):
        sys.exit("usage: llvm_build.py x86_64|aarch64 [target ...]")
    arch = sys.argv[1]

    if not os.path.exists(os.path.join(ROOT, "build", "cxx", arch, ".built")):
        sys.exit("llvm_build: first `make ARCH=%s cxx-runtimes`" % arch)

    patched_source()
    out = os.path.join(ROOT, "build", "llvm", arch)
    configure(arch, out)

    # `LLVM_KEEP_GOING=1` builds everything that compiles and lists every
    # file that does not, as `WEBKIT_KEEP_GOING` does for WebKit.
    keep = ["-k", "0"] if os.environ.get("LLVM_KEEP_GOING") else []
    targets = sys.argv[2:] or LIBRARIES
    sys.exit(subprocess.run(["ninja", "-C", out] + keep + targets).returncode)


if __name__ == "__main__":
    main()
