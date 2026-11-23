#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
**ICU for Kosmos** (`docs/w4.md` W4b): ICU 4C's `common` and `i18n`,
compiled by clang for one architecture into `build/icu/<arch>/` -
`libicuuc.a`, `libicui18n.a` - and its data, `icudt79l.dat`, as an object
that defines the symbol ICU looks for its common data under, so nothing
opens a file for it.

    python3 tools/icu_build.py x86_64|aarch64|host

`host` is the same ICU, the same defines and the same data built for this
Mac by its own compiler, into `build/icu/host/`: the reference
`tools/run_icu.py` holds Kosmos's answers to.

**Why a script and not ICU's `configure`**: it learns about a machine by
running programs it compiles, and it cannot run a Kosmos one - as FFmpeg's
could not (`tools/ffmpeg_vendor.py`, until M6d). What it would have concluded is said
here instead, as defines, and the files are every `.cpp` in the two
directories, which is what ICU's own Makefiles build of them.

The source is fetched, not committed (`tools/upstream.py`), and compiled as
shipped: nothing under `build/upstream/icu/` is edited.
"""

import concurrent.futures
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import upstream                                              # noqa: E402

LLVM = "/opt/homebrew/opt/llvm/bin"
VERSION = "79"

#
# What `configure` would have found, said instead. Static throughout, as
# every Kosmos image is; no loading of plugins or of data by file name; and
# ICU's own names left unrenamed by version, since only one ICU is ever in
# an image (WebKit's JSCOnly port asks for that on Apple too).
#
DEFINES = [
    "-DU_STATIC_IMPLEMENTATION",
    "-DU_ENABLE_DYLOAD=0",
    "-DU_HAVE_STRTOD_L=0",
    "-DU_HAVE_XLOCALE_H=0",
    "-DUCONFIG_NO_FILE_IO=0",
    "-DU_CHARSET_IS_UTF8=1",
    "-DU_DISABLE_RENAMING=1",
    "-DU_USING_ICU_NAMESPACE=0",
    "-DU_HIDE_OBSOLETE_UTF_OLD_H=1",
    # No locale database to ask a charset of: every byte string here is
    # UTF-8, and `nl_langinfo` does not exist.
    "-DU_HAVE_NL_LANGINFO_CODESET=0",
    # A wchar_t is 32 bits on both of Kosmos's processors, holding a code
    # point, so ICU converts it as UTF-32 itself rather than through
    # `mbstowcs`, which would need a locale.
    "-DU_WCHAR_IS_UTF32=1",
    # And the C library has no wide-character functions (`wchar.h` says
    # why), so ICU uses its own `wcslen` and the rest (`cwchar.cpp`).
    "-DU_HAVE_WCSCPY=0",
    # Kosmos has no time zones: the clock is UTC and there is no `tzset`,
    # `timezone` or `tzname`. So ICU's default zone is UTC, offset 0, and a
    # caller that wants another names it.
    "-DU_HAVE_TZSET=0",
    "-DU_HAVE_TZNAME=0",
    "-DU_TIMEZONE=0",
]


def flags(arch):
    if arch == "host":
        return ["-std=c++20", "-O2", "-w"] + DEFINES

    triple = "x86_64-unknown-none-elf" if arch == "x86_64" else "aarch64-unknown-none-elf"
    base = "0x40000000" if arch == "x86_64" else "0x4000000000"
    cxxrt = os.path.join(ROOT, "build", "cxx", arch)
    resource = subprocess.run([LLVM + "/clang", "-print-resource-dir"],
                              capture_output=True, text=True, check=True).stdout.strip()

    return ["--target=" + triple, "-std=c++20", "-O2", "-g",
            "-fno-stack-protector", "-ffunction-sections", "-fdata-sections",
            "-w",                               # ICU's warnings are ICU's
            "-nostdinc", "-nostdinc++",
            "-isystem", os.path.join(cxxrt, "include", "c++", "v1"),
            "-isystem", os.path.join(ROOT, "runtime", "ports", "icu", "include"),
            "-isystem", os.path.join(ROOT, "runtime", "posix", "include"),
            "-isystem", os.path.join(resource, "include"),
            "-isystem", os.path.join(ROOT, "runtime", "include"),
            "-isystem", os.path.join(ROOT, "user", "include"),
            # What it is compiled for, as every C++ build here says
            # (`runtime/cxx/kosmos.cmake`): libc++ chooses its locale
            # support by it (`runtime/patches/cxx/`).
            "-D__kosmos__",
            "-DKOSMOS_USER", "-DKOSMOS_USER_BASE=" + base] + DEFINES


def compile_one(job):
    cxx, source, obj, extra = job
    os.makedirs(os.path.dirname(obj), exist_ok=True)

    if os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(source):
        return None

    r = subprocess.run([cxx] + extra + ["-c", source, "-o", obj],
                       capture_output=True, text=True)
    return (source, r.stderr) if r.returncode != 0 else None


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("x86_64", "aarch64", "host"):
        sys.exit("usage: icu_build.py x86_64|aarch64|host")

    arch = sys.argv[1]
    cxx = "/usr/bin/clang++" if arch == "host" else LLVM + "/clang++"

    if arch != "host" and not os.path.exists(os.path.join(ROOT, "build", "cxx", arch, ".built")):
        sys.exit("icu_build: the C++ runtimes first - `make ARCH=%s cxx-runtimes`" % arch)

    source = os.path.join(upstream.unpack("icu"), "icu", "source")
    data_dir = upstream.unpack("icu-data")
    out = os.path.join(ROOT, "build", "icu", arch)
    base = flags(arch)
    jobs = []
    libraries = {}

    for part, define in (("common", "-DU_COMMON_IMPLEMENTATION"),
                         ("i18n", "-DU_I18N_IMPLEMENTATION")):
        here = os.path.join(source, part)
        extra = base + [define, "-I" + os.path.join(source, "common"), "-I" + here]
        objs = []

        for name in sorted(os.listdir(here)):
            if name.endswith(".cpp"):
                obj = os.path.join(out, part, name[:-4] + ".o")
                jobs.append((cxx, os.path.join(here, name), obj, extra))
                objs.append(obj)

        libraries[part] = objs

    #
    # Compiled again whole when how it is compiled changed - a define here,
    # or the headers it reads - since an object's date says only that its
    # own source has not moved. The flags and the headers' dates, written
    # after a build that worked; and the C++ runtimes' build, whose headers
    # are libc++'s - an option of theirs changes every one of those headers
    # and nothing here (localization, `docs/gpu3d.md` G2).
    #
    runtimes = os.path.join(ROOT, "build", "cxx", arch, ".built")
    said = repr((base, [os.path.getmtime(os.path.join(d, n))
                        for d in (os.path.join(ROOT, "runtime", "include"),
                                  os.path.join(ROOT, "runtime", "posix", "include"),
                                  os.path.join(ROOT, "runtime", "ports", "icu", "include"))
                        for n in sorted(os.listdir(d)) if n.endswith(".h")],
                 os.path.getmtime(runtimes) if os.path.exists(runtimes) else None))
    stamp = os.path.join(out, ".flags")

    if not os.path.exists(stamp) or open(stamp).read() != said:
        for job in jobs:
            if os.path.exists(job[2]):
                os.remove(job[2])

    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        failed = [f for f in pool.map(compile_one, jobs) if f is not None]

    if failed:
        for path, said in failed:
            first = [l for l in said.split("\n") if "error:" in l][:1]
            print("icu_build: %s: %s" % (os.path.basename(path),
                                         first[0].split("error:", 1)[1].strip() if first else said[:200]))
        sys.exit("icu_build: %d of %d files did not compile" % (len(failed), len(jobs)))

    for part, name in (("common", "libicuuc.a"), ("i18n", "libicui18n.a")):
        library = os.path.join(out, name)
        if os.path.exists(library):
            os.remove(library)
        subprocess.run([LLVM + "/llvm-ar", "rcs", library] + libraries[part], check=True)

    #
    # The data, under the name ICU's common data is looked for by
    # (`U_ICUDATA_ENTRY_POINT`, `icudt79_dat`), aligned as `udata` wants:
    # the image carries it and `udata` finds it without a file. The bytes
    # are ICU's own build of them, as published.
    #
    dat = os.path.join(data_dir, "icudt%sl.dat" % VERSION)
    asm = os.path.join(out, "icudata.S")
    # A Mac's symbols carry an underscore and its sections another name.
    symbol = ("_" if arch == "host" else "") + "icudt%s_dat" % VERSION
    section = "__TEXT,__const" if arch == "host" else ".rodata.icudata, \"a\""

    with open(asm, "w") as f:
        f.write("/* ICU's data, %s, as published (tools/icu_build.py). */\n"
                "    .section %s\n"
                "    .balign 16\n"
                "    .globl %s\n"
                "%s:\n"
                "    .incbin \"%s\"\n" % (os.path.basename(dat), section, symbol, symbol, dat))
    subprocess.run(["/usr/bin/clang" if arch == "host" else LLVM + "/clang"]
                   + ([] if arch == "host" else [base[0]])
                   + ["-c", asm, "-o", os.path.join(out, "icudata.o")], check=True)

    #
    # And as ICU is when installed, which is how a port's build looks for
    # it (CMake's `FindICU`, WebKit's): one `include/unicode/` with both
    # parts' public headers, and the data as a library of its own,
    # `libicudata.a`.
    #
    unicode = os.path.join(out, "include", "unicode")
    if os.path.isdir(unicode):
        shutil.rmtree(unicode)
    os.makedirs(unicode)
    for part in ("common", "i18n"):
        here = os.path.join(source, part, "unicode")
        for name in os.listdir(here):
            shutil.copy2(os.path.join(here, name), unicode)
    # **And the defines it was built with, written at the top of
    # `uconfig.h`**, as ICU's own `configure` asks of an install whose
    # options a caller must share (its `uconfig.h.prepend`): without them a
    # port compiled against these headers calls `u_strToUpper_79` into a
    # library that has `u_strToUpper`.
    config = os.path.join(unicode, "uconfig.h")
    with open(config) as f:
        body = f.read()
    with open(config, "w") as f:
        f.write("/* ICU customizations (tools/icu_build.py): the defines this ICU was\n"
                " * built with, as ICU's install prepends them. */\n")
        for flag in DEFINES:
            name, _, value = flag[2:].partition("=")
            f.write("#ifndef %s\n#define %s %s\n#endif\n" % (name, name, value or "1"))
        f.write("\n" + body)
    data_lib = os.path.join(out, "libicudata.a")
    if os.path.exists(data_lib):
        os.remove(data_lib)
    subprocess.run([LLVM + "/llvm-ar", "rcs", data_lib, os.path.join(out, "icudata.o")],
                   check=True)

    with open(stamp, "w") as f:
        f.write(said)

    print("icu_build: %d files, %s and %s, and %.1f MB of data, in %s"
          % (len(jobs), "libicuuc.a", "libicui18n.a", os.path.getsize(dat) / 1e6,
             os.path.relpath(out, ROOT)))


if __name__ == "__main__":
    main()
