#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
**Mesa for Kosmos** (`docs/gpu3d.md` G1): Mesa 26.2.5's libraries -
`util`, the shader compilers' NIR and SPIR-V, and on them the Vulkan
runtime and `lavapipe` (G2) - configured with Meson for Kosmos and built
for one architecture into `build/mesa/<arch>/`.

    python3 tools/mesa_build.py x86_64|aarch64 RUNTIME [target ...]

`RUNTIME` is a userland build's directory (`build/user-x86_64-test`): its
`runtime/` objects are what Meson's checks link against. Targets are
ninja's; by default `LIBRARIES`, below.

**The source** is the release, fetched and held to its sum
(`tools/upstream.py`), never committed and never edited. What is built is
a copy of it under `build/mesa/tree/`, cut by `upstream.patched_copy` as
WebKit's and LLVM's are, with `runtime/patches/mesa/` applied in order.

**What Meson is told** (the cross file, written here because its paths
are absolute):

- **a system called `kosmos`**, which Meson does not know and Mesa's
  patches do (`0001`): every choice it would make by the system's name is
  made by the patches instead;
- **Kosmos's compiler flags**, as `runtime/cxx/kosmos.cmake` gives CMake -
  the POSIX layer's headers, the compiler's own, the libc's, `__kosmos__`
  - with one difference: Meson drops an `-isystem` that names the
  compiler's own directory, which after `-nostdinc` is wrong, so the
  compiler's headers are a clone of that directory under `build/mesa/`;
- **trial links answered by Kosmos's libc**: Meson asks whether a function
  exists by linking a program that calls it, and the answer must be
  Kosmos's. Told only to ignore what is undefined, every function existed,
  and Mesa believed in `secure_getenv` and `qsort_s`; linked for real, a
  trial stood on the Storage and Network Kits and on init's own `main`. So
  a trial is linked by `tools/mesa_trial_ld.py`, which asks whether every
  symbol the trial calls is defined by an archive of the userland's
  runtime objects, the C++ runtimes or libgcc;
- **no downloads**: Mesa's `subprojects/` would fetch zlib from Meson's
  wrap database. Kosmos has one zlib (`docs/w5.md`), and the cross file
  points at W5's.
"""

import glob
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import upstream                                              # noqa: E402

LLVM = os.environ.get("KOSMOS_LLVM", "/opt/homebrew/opt/llvm")
PATCHES = os.path.join(ROOT, "runtime", "patches", "mesa")
PORT = os.path.join(ROOT, "runtime", "ports", "mesa")
SRC = os.path.join(ROOT, "build", "mesa", "tree")
SRC_LLVM = os.path.join(ROOT, "build", "llvm", "tree")
VENV = os.path.join(ROOT, "build", "venv-mesa")
BISON = "/opt/homebrew/opt/bison/bin"

# Mesa's code generators are Python with Mako and PyYAML; they are
# installed into a venv of their own, and macOS's own bison is too old (and
# crashes), so Homebrew's is put first.
PYTHON_PACKAGES = ["mako", "pyyaml", "packaging"]

# **What is built.** G1's part: the utilities, NIR, SPIR-V's front end and
# the Vulkan runtime - which Mesa builds whole only for a driver that uses
# them, so the build is configured for `lavapipe` (G2) from the start: with
# no driver, NIR is its headers and a stub.
LIBRARIES = ["src/util/libmesa_util.a", "src/util/blake3/libblake3.a",
             "src/c11/impl/libmesa_util_c11.a",
             "src/compiler/libcompiler.a", "src/compiler/nir/libnir.a",
             "src/compiler/spirv/libvtn.a", "src/vulkan/runtime/libvulkan_runtime.a"]

# Vulkan by `lavapipe` (`swrast`) on llvmpipe's pieces, its shaders by
# LLVM's ORC JIT, which G1's `x86-llvm` showed working here (MCJIT is the
# one LLVM deprecates). No window system yet: G3 is Kosmos's own.
OPTIONS = [
    "-Dplatforms=", "-Dgallium-drivers=llvmpipe", "-Dvulkan-drivers=swrast",
    "-Dllvm=enabled", "-Dllvm-orcjit=true", "-Dshared-llvm=disabled",
    "-Dcpp_rtti=false",
    "-Dopengl=false", "-Dgles1=disabled", "-Dgles2=disabled",
    "-Degl=disabled", "-Dgbm=disabled", "-Dglx=disabled",
    "-Dzstd=disabled", "-Dexpat=disabled", "-Dxmlconfig=disabled",
    "-Dvalgrind=disabled", "-Dlibunwind=disabled", "-Dlmsensors=disabled",
    "-Dbuild-tests=false", "-Dshader-cache=disabled",
    "-Ddefault_library=static", "-Dbuildtype=release", "-Db_ndebug=true",
]


def fingerprint():
    """What the copy is made from: the release and every patch, in order."""
    h = hashlib.sha256(upstream.unpacked_as("mesa").encode())
    return upstream.patches_digest(h, PATCHES, PORT).hexdigest()


def patched_source():
    want = fingerprint()
    stamp = os.path.join(SRC, ".patched")
    if not os.path.exists(stamp) or open(stamp).read() != want:
        upstream.unpack("mesa")
    return upstream.patched_copy(upstream.where("mesa"), SRC, want, PATCHES,
                                 port=PORT, who="mesa_build")


def venv():
    python = os.path.join(VENV, "bin", "python3")
    if not os.path.exists(os.path.join(VENV, ".ready")):
        subprocess.run([sys.executable, "-m", "venv", VENV], check=True)
        subprocess.run([python, "-m", "pip", "install", "-q"] + PYTHON_PACKAGES,
                       check=True)
        open(os.path.join(VENV, ".ready"), "w").write(" ".join(PYTHON_PACKAGES))
    return python


def clang_headers(out):
    """The compiler's own headers, cloned where Meson will keep them."""
    resource = subprocess.run([LLVM + "/bin/clang", "-print-resource-dir"],
                              capture_output=True, text=True, check=True).stdout.strip()
    into = os.path.join(out, "clang-include")
    mark = os.path.join(into, ".from")
    if not (os.path.exists(mark) and open(mark).read() == resource):
        shutil.rmtree(into, ignore_errors=True)
        subprocess.run(["cp", "-Rc", os.path.join(resource, "include"), into], check=True)
        open(mark, "w").write(resource)
    return into


def trial_archive(arch, out, runtime):
    """The userland's runtime objects and the stub, for Meson's trial links."""
    objects = sorted(p for p in glob.glob(os.path.join(runtime, "runtime", "**", "*.o"),
                                          recursive=True)
                     if "/upstream/lua" not in p)
    # And the halves of the environment every process is born with that
    # libc functions are in: the clock, `errno`, the system calls.
    objects += [os.path.join(runtime, "user", "init", n) for n in
                ("clock_user.c.o", "misc_user.c.o", "panic_user.c.o", "syscalls.c.o",
                 "sys_user.c.o")]
    if not objects:
        sys.exit("mesa_build: no runtime objects under %s - build the userland first" % runtime)
    archive = os.path.join(out, "trial", "libkosmos-trial.a")
    newest = max(os.path.getmtime(p) for p in objects + [__file__])
    if os.path.exists(archive) and os.path.getmtime(archive) >= newest:
        return archive
    os.makedirs(os.path.dirname(archive), exist_ok=True)
    if os.path.exists(archive):
        os.remove(archive)
    subprocess.run([LLVM + "/bin/llvm-ar", "rcs", archive] + objects, check=True)
    return archive


def cross_file(arch, out, runtime):
    triple = arch + "-unknown-none-elf"
    base = "0x40000000" if arch == "x86_64" else "0x4000000000"
    ld = os.path.join(HERE, "mesa_trial_ld.py")
    libgcc = subprocess.run(["%s-elf-gcc" % arch, "-print-libgcc-file-name"],
                            capture_output=True, text=True, check=True).stdout.strip()
    cxx = os.path.join(ROOT, "build", "cxx", arch)
    w5 = os.path.join(ROOT, "build", "w5", arch, "install")
    archive = trial_archive(arch, out, runtime)

    def q(items):
        return "[" + ", ".join(repr(i) for i in items) + "]"

    flags = ["-isystem" + os.path.join(w5, "include"),
             "-nostdinc", "-isystem" + os.path.join(ROOT, "runtime", "posix", "include"),
             "-D__kosmos__", "-isystem" + clang_headers(out),
             "-isystem" + os.path.join(ROOT, "runtime", "include"),
             "-isystem" + os.path.join(ROOT, "user", "include"),
             "-isystem" + os.path.join(ROOT, "runtime", "posix", "refused"),
             "-DKOSMOS_USER", "-DKOSMOS_USER_BASE=" + base,
             "-ffunction-sections", "-fdata-sections", "-fno-stack-protector"]
    cxxflags = ["-nostdinc++", "-isystem" + os.path.join(cxx, "include", "c++", "v1")] + \
        flags + ["-D_LIBCPP_HAS_CLOCK_GETTIME=", "-fno-exceptions", "-fno-rtti"]
    link = ["-fuse-ld=" + ld, "-L" + os.path.join(w5, "lib"), "-nostdlib",
            "-Wl,-e,main", "-Wl,--start-group", archive]
    cxxlink = link + [os.path.join(cxx, "lib", n)
                      for n in ("libc++.a", "libc++abi.a", "libunwind.a")]

    text = "\n".join([
        "# Written by tools/mesa_build.py for %s; see its comment." % arch,
        "[binaries]",
        "c = %s" % q([LLVM + "/bin/clang", "--target=" + triple]),
        "cpp = %s" % q([LLVM + "/bin/clang++", "--target=" + triple]),
        "ar = %r" % (LLVM + "/bin/llvm-ar"),
        "strip = %r" % (LLVM + "/bin/llvm-strip"),
        "python = %r" % venv(),
        "",
        "[built-in options]",
        "c_args = %s" % q(flags),
        "cpp_args = %s" % q(cxxflags),
        "c_link_args = %s" % q(link + [libgcc, "-Wl,--end-group"]),
        "cpp_link_args = %s" % q(cxxlink + [libgcc, "-Wl,--end-group"]),
        "",
        "[properties]",
        "needs_exe_wrapper = true",
        "",
        "[host_machine]",
        "system = 'kosmos'",
        "cpu_family = %r" % arch,
        "cpu = %r" % arch,
        "endian = 'little'",
        "",
    ])
    path = os.path.join(out, "kosmos.ini")
    if not os.path.exists(path) or open(path).read() != text:
        with open(path, "w") as f:
            f.write(text)
    return path


def environment(arch):
    env = dict(os.environ)
    env["KOSMOS_TRIAL_LD"] = "/opt/homebrew/bin/%s-elf-ld" % arch
    env["PATH"] = os.path.join(VENV, "bin") + ":" + BISON + ":" + env["PATH"]
    return env


def llvm_libraries(arch, out):
    """The file naming LLVM's libraries for the subproject that hands them
    to Mesa (`runtime/ports/mesa/subprojects/llvm/`): every one built."""
    found = sorted(glob.glob(os.path.join(ROOT, "build", "llvm", arch, "lib", "libLLVM*.a")))
    if not found:
        sys.exit("mesa_build: no LLVM for %s - `python3 tools/llvm_build.py %s` first"
                 % (arch, arch))
    path = os.path.join(out, "llvm-libraries.txt")
    text = "\n".join(found) + "\n"
    if not os.path.exists(path) or open(path).read() != text:
        with open(path, "w") as f:
            f.write(text)
    return path


def configure(arch, out, runtime):
    cross = cross_file(arch, out, runtime)
    build = os.path.join(out, "build")
    stamp = os.path.join(build, ".configured")
    headers = [os.path.join(folder, name)
               for top in (os.path.join(ROOT, "runtime", "include"),
                           os.path.join(ROOT, "runtime", "posix", "include"))
               for folder, _, names in os.walk(top) for name in names]
    newest = max(os.path.getmtime(p) for p in
                 [cross, os.path.join(SRC, ".patched"), __file__,
                  os.path.join(ROOT, "build", "llvm", arch, "lib", "libLLVMCore.a")] + headers)
    if os.path.exists(stamp) and os.path.getmtime(stamp) >= newest \
            and os.path.exists(os.path.join(build, "build.ninja")):
        return build

    # Never from what it left: every value read from the files again.
    shutil.rmtree(build, ignore_errors=True)
    command = ["meson", "setup", build, SRC, "--cross-file", cross,
               "--wrap-mode=nodownload", "--force-fallback-for=llvm",
               "-Dllvm:libs=" + llvm_libraries(arch, out),
               "-Dllvm:include=" + os.path.join(SRC_LLVM, "llvm", "include") + ":" +
               os.path.join(ROOT, "build", "llvm", arch, "include")] + OPTIONS
    log = os.path.join(out, "meson.log")
    with open(log, "w") as f:
        r = subprocess.run(command, stdout=f, stderr=subprocess.STDOUT, env=environment(arch))
    if r.returncode != 0:
        print(open(log).read()[-6000:])
        sys.exit("mesa_build: Meson did not configure (%s)" % log)
    with open(stamp, "w") as f:
        f.write("configured\n")
    return build


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("x86_64", "aarch64"):
        sys.exit("usage: mesa_build.py x86_64|aarch64 RUNTIME [target ...]")
    arch, runtime = sys.argv[1], sys.argv[2]

    if not os.path.exists(os.path.join(ROOT, "build", "cxx", arch, ".built")):
        sys.exit("mesa_build: first `make ARCH=%s cxx-runtimes`" % arch)

    patched_source()
    out = os.path.join(ROOT, "build", "mesa", arch)
    os.makedirs(out, exist_ok=True)
    build = configure(arch, out, runtime)

    # `MESA_KEEP_GOING=1` builds everything that compiles and lists every
    # file that does not, as `LLVM_KEEP_GOING` does.
    keep = ["-k", "0"] if os.environ.get("MESA_KEEP_GOING") else []
    targets = sys.argv[3:] or LIBRARIES
    r = subprocess.run(["ninja", "-C", build] + keep + targets, env=environment(arch))
    if r.returncode == 0:
        defines(build, out)
    sys.exit(r.returncode)


def defines(build, out):
    """What NIR was compiled with, for code that includes its headers.

    Mesa's structures and inline functions are shaped by what Meson found -
    `UTIL_ARCH_LITTLE_ENDIAN`, `HAVE_PTHREAD`, `MESA_DEBUG` - so a file
    outside Mesa that includes them has to be told the same. Read from the
    compile of one of NIR's own files and written as a response file,
    `defines.txt`, that the compiler is handed with `@`."""
    with open(os.path.join(build, "compile_commands.json")) as f:
        commands = json.load(f)
    for entry in commands:
        if entry["file"].endswith("compiler/nir/nir_print.c"):
            args = shlex.split(entry["command"])
            break
    else:
        sys.exit("mesa_build: no compile of nir_print.c to read the defines from")
    wanted = [a for a in args if a.startswith("-D")]
    text = "".join(shlex.quote(a) + "\n" for a in wanted)
    path = os.path.join(out, "defines.txt")
    if not os.path.exists(path) or open(path).read() != text:
        with open(path, "w") as f:
            f.write(text)


if __name__ == "__main__":
    main()
