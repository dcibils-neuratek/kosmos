#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
The C++ runtimes, vendored: libc++, libc++abi and libunwind from LLVM's
release, as shipped and less their tests (`docs/webkit.md` W1c).

    python3 tools/llvm_vendor.py

What it does, in order:

1. Checks the release tarball in `build/downloads/` against the sum below.
2. Unpacks the parts the runtimes are built from into `build/llvm/src/`.
3. Builds the three for both architectures from there, with Kosmos's
   toolchain file and options (`runtime/cxx/`), and reads from ninja's log
   which files of LLVM's own C library the build included: libc++ shares
   its number parsing with LLVM's libc, and those headers are the closure,
   chosen by the compiler rather than by reading - as
   `runtime/upstream/musl-math/` and FFmpeg's files were.
4. Copies the runtimes, the CMake they are configured with, and that
   closure into `runtime/upstream/llvm/`, byte for byte.
5. Builds the three again, from the copy, for both architectures - so a
   file the copy left out fails here, today, and not in somebody's build.

Nothing under `runtime/upstream/llvm/` is edited, as nothing vendored is;
`README.kosmos.md` there is Kosmos's and survives a rerun.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

VERSION = "23.1.2"
NAME = f"llvm-project-{VERSION}.src"
TARBALL = os.path.join(ROOT, "build", "downloads", f"llvm-{VERSION}", f"{NAME}.tar.xz")
URL = ("https://github.com/llvm/llvm-project/releases/download/"
       f"llvmorg-{VERSION}/{NAME}.tar.xz")

# The release's asset digest as GitHub states it, compared on 11 November
# 2026, and this Mac's clang is the same release.
SHA256 = "c98bbef08a2b4c2613cd50e9aa9ae7b69b1fe6c16b2c40373bc0ab6116fdf78a"

WORK = os.path.join(ROOT, "build", "llvm")
SRC = os.path.join(WORK, "src", NAME)
VENDOR = os.path.join(ROOT, "runtime", "upstream", "llvm")

ARCHES = ["x86_64", "aarch64"]

# Whole trees, less what is named in SKIP. `runtimes/` is the build that
# configures the three together; `cmake/` and `llvm/cmake/` are the
# modules it includes.
# `llvm/utils/llvm-lit` because `runtimes/` adds it as a directory even
# with every test off; the rest of `llvm/utils`, 11 MB, it never reads.
TREES = ["LICENSE.TXT", "cmake", "runtimes", "llvm/cmake", "llvm/utils/llvm-lit",
         "libcxx", "libcxxabi", "libunwind", "libc/LICENSE.TXT"]
SKIP = {"libcxx/test", "libcxxabi/test", "libunwind/test"}

# What is unpacked: the trees, and all of LLVM's libc, the closure is taken
# from.
UNPACK = TREES + ["libc"]


def say(text):
    print(f"llvm_vendor: {text}", flush=True)


def check_tarball():
    if not os.path.exists(TARBALL):
        sys.exit(f"llvm_vendor: no {TARBALL}; fetch {URL}")

    h = hashlib.sha256()
    with open(TARBALL, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)

    if h.hexdigest() != SHA256:
        sys.exit(f"llvm_vendor: {TARBALL} is {h.hexdigest()}, not {SHA256}")
    say("tarball's sum is the release's")


def wanted(member):
    rel = member.split("/", 1)[1] if "/" in member else ""
    return any(rel == t or rel.startswith(t + "/") for t in UNPACK)


def unpack():
    if os.path.isdir(SRC):
        shutil.rmtree(SRC)
    os.makedirs(os.path.dirname(SRC), exist_ok=True)

    with tarfile.open(TARBALL) as tar:
        members = [m for m in tar.getmembers() if wanted(m.name)]
        tar.extractall(os.path.dirname(SRC), members=members, filter="data")
    say(f"unpacked {len(members)} entries into {os.path.relpath(SRC, ROOT)}")


def build(src, arch, out):
    """The three runtimes for `arch`, from `src`, into `out`."""
    if os.path.isdir(out):
        shutil.rmtree(out)

    subprocess.run(["cmake", "-G", "Ninja", "-S", os.path.join(src, "runtimes"), "-B", out,
                    "-C", os.path.join(ROOT, "runtime", "cxx", "runtimes.cmake"),
                    "-DCMAKE_TOOLCHAIN_FILE=" + os.path.join(ROOT, "runtime", "cxx", "kosmos.cmake"),
                    f"-DKOSMOS_ARCH={arch}", f"-DKOSMOS_ROOT={ROOT}"],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["ninja", "-C", out], check=True, stdout=subprocess.DEVNULL)

    for lib in ("libc++.a", "libc++abi.a", "libunwind.a"):
        if not os.path.exists(os.path.join(out, "lib", lib)):
            sys.exit(f"llvm_vendor: {arch} built no {lib}")
    say(f"{arch}: libc++, libc++abi and libunwind built from {os.path.relpath(src, ROOT)}")


def libc_closure(out):
    """The files of LLVM's libc the build at `out` included."""
    text = subprocess.run(["ninja", "-C", out, "-t", "deps"], check=True,
                          capture_output=True, text=True).stdout
    prefix = SRC + "/libc/"
    found = set()

    for line in text.splitlines():
        path = line.strip()
        if path.startswith(prefix):
            found.add("libc/" + path[len(prefix):])
    return found


def copy(rel):
    src = os.path.join(SRC, rel)
    dst = os.path.join(VENDOR, rel)

    if os.path.isdir(src):
        for dirpath, dirnames, filenames in os.walk(src):
            here = os.path.relpath(dirpath, SRC)
            dirnames[:] = [d for d in dirnames if os.path.join(here, d) not in SKIP]
            os.makedirs(os.path.join(VENDOR, here), exist_ok=True)
            for name in filenames:
                shutil.copy2(os.path.join(dirpath, name), os.path.join(VENDOR, here, name))
    else:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)


def vendor(closure):
    keep = os.path.join(VENDOR, "README.kosmos.md")
    readme = open(keep).read() if os.path.exists(keep) else None

    if os.path.isdir(VENDOR):
        shutil.rmtree(VENDOR)
    os.makedirs(VENDOR)

    for rel in TREES:
        copy(rel)
    for rel in sorted(closure):
        copy(rel)

    if readme is not None:
        with open(keep, "w") as f:
            f.write(readme)

    with open(os.path.join(VENDOR, "libc-closure.txt"), "w") as f:
        f.write("# The files of LLVM's libc that libc++ includes, taken from\n")
        f.write("# ninja's log by tools/llvm_vendor.py. Generated; not edited.\n")
        for rel in sorted(closure):
            f.write(rel + "\n")
    say(f"copied the runtimes and {len(closure)} files of LLVM's libc into "
        f"{os.path.relpath(VENDOR, ROOT)}")


def main():
    check_tarball()
    unpack()

    closure = set()
    for arch in ARCHES:
        out = os.path.join(WORK, f"trial-{arch}")
        build(SRC, arch, out)
        closure |= libc_closure(out)

    vendor(closure)

    for arch in ARCHES:
        build(VENDOR, arch, os.path.join(WORK, f"vendored-{arch}"))
    say("the copy builds on its own, for both")


if __name__ == "__main__":
    main()
