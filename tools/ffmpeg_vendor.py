#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
FFmpeg's decoders, vendored: the files a Kosmos kit reaches, and the
configuration they are compiled under - both out of one run of this script.

    python3 tools/ffmpeg_vendor.py

**Why a script and not a copy.** FFmpeg is a million lines, and the H.264
decoder is about a hundred files of it. Which hundred is not something to
decide by reading: it is the closure of what the kit calls, taken by the
linker, which is how `runtime/upstream/musl-math/` was chosen too. And its
`config.h` is the output of a `configure` that cannot see a Kosmos process -
its tests *link* a program, and there is nothing here for it to link - so
what it concluded about this machine has to be corrected by rules, and the
rules are better kept where they can be run again than in a hand-edited
file. A new FFmpeg, or a second decoder, is this script run once more.

What it does, in order:

1. Checks the release tarball in `build/downloads/` against the sum below.
2. Unpacks it into `build/ffmpeg/src/` and runs its `configure` for a
   freestanding AArch64 target, with only the decoders named below.
3. Corrects `config.h` by the rules in `correct_config`, and writes it and
   the other generated files into `user/kits/ffmpeg/config/`.
4. Compiles every object `configure` would have built, links the kit's
   entry points against them with the linker's map switched on, and keeps
   the archive members it pulled in: that is the closure.
5. Copies those sources, and every header they include, into
   `runtime/upstream/ffmpeg/`, byte for byte, and writes the object list the
   Makefile compiles into `user/kits/ffmpeg/ffmpeg.mk`.
6. Says which C library functions the closure needs, and which of those
   `runtime/include/` does not declare - which is the list of what a new
   decoder asks of this system.

Nothing under `runtime/upstream/ffmpeg/` is edited, as nothing vendored is;
`README.kosmos.md` there is Kosmos's and survives a rerun.
"""

import concurrent.futures
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

VERSION = "9.0.2"
TARBALL = os.path.join(ROOT, "build", "downloads", f"ffmpeg-{VERSION}.tar.xz")
URL = f"https://ffmpeg.org/releases/ffmpeg-{VERSION}.tar.xz"

# Checked against FFmpeg's release signature on 24 September 2026: "Good
# signature from FFmpeg release signing key", key
# FCF986EA15E6E293A5644F10B4322F04D67658D8.
SHA256 = "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e"

WORK = os.path.join(ROOT, "build", "ffmpeg")
SRC = os.path.join(WORK, "src", f"ffmpeg-{VERSION}")
CFG = os.path.join(WORK, "cfg")
OBJS = os.path.join(WORK, "objs")

VENDOR = os.path.join(ROOT, "runtime", "upstream", "ffmpeg")
KIT = os.path.join(ROOT, "user", "kits", "ffmpeg")
CONFIG_OUT = os.path.join(KIT, "config")
MK_OUT = os.path.join(KIT, "ffmpeg.mk")

CROSS = "aarch64-none-elf-"

# The decoders, and nothing else: no demuxers (`mp4.lua` is the demuxer),
# no parsers (a packet from an MP4 is a whole access unit already), no
# encoders (the Record Kit has its own), no filters, no scaler.
DECODERS = ["h264"]

# What the kit calls. The closure is taken from these, so a function the
# kit starts calling that is not reached from here fails at the link and
# names itself, and the answer is a line here and another run.
ROOTS = [
    "avcodec_find_decoder",
    "avcodec_alloc_context3",
    "avcodec_open2",
    "avcodec_send_packet",
    "avcodec_receive_frame",
    "avcodec_flush_buffers",
    "avcodec_free_context",
    "av_frame_alloc",
    "av_frame_unref",
    "av_frame_free",
    "av_packet_alloc",
    "av_packet_free",
    "av_new_packet",
    "av_packet_unref",
    "av_get_pix_fmt_name",
    "av_malloc",
    "av_mallocz",
    "av_free",
    "av_log_set_callback",
    "av_log_set_level",
    "av_log_format_line2",
    "av_strerror",
]

CONFIGURE = [
    "--enable-cross-compile", f"--cross-prefix={CROSS}", "--arch=aarch64",
    "--target-os=none",
    "--disable-everything", "--disable-programs", "--disable-doc",
    "--disable-network", "--disable-autodetect",
    "--disable-avdevice", "--disable-avformat", "--disable-swscale",
    "--disable-swresample", "--disable-avfilter",
    "--disable-pthreads", "--disable-asm", "--disable-inline-asm",
    "--disable-debug", "--disable-runtime-cpudetect",
    # Relative, so the configuration line FFmpeg bakes into its library
    # names no directory on anybody's machine.
    "--extra-cflags=-ffreestanding -DKOSMOS_USER -I../../../runtime/include "
    "-I../../../kernel -I../../../user/include",
    "--extra-ldflags=-nostdlib -Wl,-e,main",
] + [f"--enable-decoder={d}" for d in DECODERS]

# The generated files FFmpeg's sources include, by the path they include.
GENERATED = [
    "config.h",
    "config_components.h",
    "libavutil/avconfig.h",
    "libavutil/ffversion.h",
    "libavcodec/codec_list.c",
    "libavcodec/parser_list.c",
    "libavcodec/bsf_list.c",
]

# Upstream's licence texts travel with the code whether or not a source
# includes them.
ALWAYS = ["COPYING.LGPLv2.1", "LICENSE.md", "CREDITS", "RELEASE", "VERSION"]

# And FFmpeg's own checksums for the conformance streams the Mac decodes
# (`tools/h264_conformance.txt`), which is what `tools/test_h264.c` holds
# the decoder to - FFmpeg's word on its own decoder, not a copy of ours.
CONFORMANCE = os.path.join(ROOT, "tools", "h264_conformance.txt")


def conformance_refs():
    refs = []
    for line in open(CONFORMANCE):
        if line.strip() and not line.startswith("#"):
            refs.append(f"tests/ref/fate/h264-conformance-{line.split()[0]}")
    return refs


def run(cmd, cwd=None, quiet=False):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0 and not quiet:
        sys.stdout.write(r.stdout)
        sys.stdout.write(r.stderr)
        raise SystemExit(f"failed: {' '.join(cmd)}")
    return r


def check_tarball():
    if not os.path.exists(TARBALL):
        raise SystemExit(f"{TARBALL} is missing; it is {URL}")
    h = hashlib.sha256(open(TARBALL, "rb").read()).hexdigest()
    if h != SHA256:
        raise SystemExit(f"{TARBALL}: sha256 {h}, expected {SHA256}")
    print(f"tarball   {os.path.relpath(TARBALL, ROOT)}  sha256 ok")


def unpack_and_configure():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(os.path.dirname(SRC))
    with tarfile.open(TARBALL) as t:
        t.extractall(os.path.dirname(SRC), filter="data")
    os.makedirs(CFG)
    r = run([os.path.join("..", "src", f"ffmpeg-{VERSION}", "configure")]
            + CONFIGURE, cwd=CFG)
    kept = [l for l in r.stdout.splitlines()
            if l.startswith(("Enabled decoders", "License"))
            or l.strip().startswith(tuple(DECODERS))]
    print("configure " + "; ".join(l.strip() for l in kept))


def declared(name, headers):
    pat = re.compile(r"(^|[^A-Za-z0-9_])" + re.escape(name) + r"\s*\(")
    mac = re.compile(r"#\s*define\s+" + re.escape(name) + r"\b")
    return any(pat.search(h) or mac.search(h) for h in headers)


def configure_list(name):
    text = open(os.path.join(SRC, "configure")).read()
    m = re.search(r'^' + name + r'="\n(.*?)^"', text, re.S | re.M)
    return m.group(1).split()


def correct_config():
    """
    What `configure` concluded about a machine it could not run a program
    on, and what is true of a Kosmos process instead.
    """
    inc = os.path.join(ROOT, "runtime", "include")
    headers = []
    for d, _, files in os.walk(inc):
        headers += [open(os.path.join(d, f)).read()
                    for f in files if f.endswith(".h")]
    math_h = open(os.path.join(inc, "math.h")).read()

    path = os.path.join(CFG, "config.h")
    text = open(path).read()
    changed = []

    def set_have(name, value, why):
        nonlocal text
        pat = re.compile(r"^#define HAVE_" + name + r" [01]$", re.M)
        new = f"#define HAVE_{name} {value}"
        if not pat.search(text):
            raise SystemExit(f"config.h has no HAVE_{name}")
        old = pat.search(text).group(0)
        if old != new:
            text = pat.sub(new, text)
            changed.append(f"{new:40s} {why}")

    # A maths function is there when `math.h` declares it. FFmpeg's
    # `libm.h` defines a static fallback for every one it believes absent,
    # and a static definition after this system's declaration is an error -
    # so this has to be exactly true, in both directions.
    for f in configure_list("MATH_FUNCS"):
        set_have(f.upper(), 1 if declared(f, [math_h]) else 0,
                 "math.h declares it" if declared(f, [math_h])
                 else "math.h does not")

    # A system function likewise, from the whole of `runtime/include/`.
    # Almost all of these are the personality `CLAUDE.md` forbids, and not
    # one of them is declared; `getenv` is.
    for f in configure_list("SYSTEM_FUNCS"):
        name = re.sub(r"[^A-Za-z0-9]", "_", f).upper()
        set_have(name, 1 if declared(f, headers) else 0,
                 "runtime/include declares it" if declared(f, headers)
                 else "not in runtime/include")
    if declared("getenv", headers):
        text = text.replace("#define getenv(x) NULL\n", "")

    # No system header counts. `unistd.h` is there and empty on purpose -
    # it exists to stop the toolchain's - and the rest are the toolchain's
    # own, found by `configure` because newlib ships beside the compiler.
    for h in configure_list("HEADERS_LIST"):
        set_have(h.upper(), 0, "not a Kosmos header")

    # The three things `--disable-asm` forgets along with the assembly: it
    # makes the target "c", a machine of no known shape, and so assumes the
    # worst of it. Both Kosmos targets are 64-bit, little-endian, load a
    # word from any address, and count leading zeros in one instruction -
    # which is what these say, and the bit reader under every decoder is
    # built from them.
    set_have("FAST_64BIT", 1, "LP64, both targets")
    set_have("FAST_UNALIGNED", 1, "an unaligned load is one load")
    set_have("FAST_CLZ", 1, "clz / lzcnt")

    open(path, "w").write(text)

    av = os.path.join(CFG, "libavutil", "avconfig.h")
    a = open(av).read()
    a = a.replace("#define AV_HAVE_FAST_UNALIGNED 0",
                  "#define AV_HAVE_FAST_UNALIGNED 1")
    open(av, "w").write(a)

    print(f"config.h  {len(changed)} lines corrected:")
    for c in changed:
        print("          " + c)


def write_config():
    # `ffversion.h` is made by the build rather than by `configure`.
    run(["make", "libavutil/ffversion.h"], cwd=CFG)
    shutil.rmtree(CONFIG_OUT, ignore_errors=True)
    for g in GENERATED:
        dst = os.path.join(CONFIG_OUT, g)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(CFG, g), dst)


def candidates():
    r = run(["make", "-n", "libavcodec/libavcodec.a",
             "libavutil/libavutil.a"], cwd=CFG)
    objs = re.findall(r"-o (lib\w+/[\w/]+\.o)\b", r.stdout)
    return sorted(set(o[:-2] for o in objs))


def ffmpeg_cppflags(src):
    # What `configure` put in CPPFLAGS - feature macros, and the two compat
    # directories that stand in for <stdatomic.h> and <stdbit.h> - with its
    # source directory spelled `src`.
    mak = open(os.path.join(CFG, "ffbuild", "config.mak")).read()
    cppflags = re.search(r"^CPPFLAGS=(.*)$", mak, re.M).group(1)
    return cppflags.replace("$(SRC_PATH)", src).split()


def compile_flags():
    cppflags = ffmpeg_cppflags(SRC)
    return (["-std=c17", "-U__STRICT_ANSI__", "-O2", "-ffreestanding",
             "-fno-common", "-fno-strict-aliasing", "-fno-math-errno",
             "-fno-signed-zeros", "-w", "-DKOSMOS_USER",
             # What `ffbuild/library.mak` adds to every library object, and
             # the switch that lets FFmpeg's headers include `config.h`.
             "-DHAVE_AV_CONFIG_H",
             "-I" + CONFIG_OUT, "-I" + SRC,
             "-I" + os.path.join(ROOT, "runtime", "include"),
             "-I" + os.path.join(ROOT, "kernel"),
             "-I" + os.path.join(ROOT, "user", "include")]
            + cppflags)


def obj(n):
    # Flat, because an archive member is named by its file alone and
    # `libavcodec/utils.o` and `libavutil/utils.o` are both `utils.o`.
    return os.path.join(OBJS, n.replace("/", "__") + ".o")


def compile_all(names):
    shutil.rmtree(OBJS, ignore_errors=True)
    flags = compile_flags()

    def one(n):
        out = obj(n)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        r = subprocess.run([CROSS + "gcc"] + flags
                           + ["-c", os.path.join(SRC, n + ".c"), "-o", out],
                           capture_output=True, text=True)
        return n, r.returncode, r.stdout + r.stderr

    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        results = list(pool.map(one, names))
    return {n: log for n, code, log in results if code != 0}


def closure(names, failed):
    built = [n for n in names if n not in failed]
    lib = os.path.join(OBJS, "candidates.a")
    run([CROSS + "ar", "rcs", lib]
        + [obj(n) for n in built])

    roots_c = os.path.join(OBJS, "roots.c")
    with open(roots_c, "w") as f:
        for r in ROOTS:
            f.write(f"extern char {r}[];\n")
        f.write("void *const kosmos_roots[] = {\n")
        for r in ROOTS:
            f.write(f"    {r},\n")
        f.write("};\n")
    roots_o = os.path.join(OBJS, "roots.o")
    run([CROSS + "gcc", "-ffreestanding", "-c", roots_c, "-o", roots_o])

    mapfile = os.path.join(OBJS, "closure.map")
    run([CROSS + "gcc", "-nostdlib", "-Wl,-e,0",
         "-Wl,--unresolved-symbols=ignore-all",
         "-Wl,-u,kosmos_roots", "-Wl,-Map," + mapfile,
         roots_o, lib, "-o", os.path.join(OBJS, "closure.elf")])

    text = open(mapfile).read()
    section = text.split("(symbol)\n\n", 1)[1].split("\n\n")[0]
    members = set(re.findall(r"candidates\.a\((\w+)\.o\)", section))
    picked = sorted(n for n in built if os.path.basename(obj(n))[:-2]
                    in members)

    # What the closure still wants from outside itself: the C library.
    nm = run([CROSS + "nm", "-A"] + [obj(n) for n in picked]).stdout
    defined, undefined = set(), set()
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[-2] in "TDBRCVWtdbr" and \
           parts[-2].isupper():
            defined.add(parts[-1])
        elif len(parts) >= 2 and parts[-2] == "U":
            undefined.add(parts[-1])
    wanted = sorted(undefined - defined)

    # A symbol of FFmpeg's own that nothing in the closure defines is a
    # hole - it lives in a file that did not compile - and not a function
    # the C library should have.
    holes = [w for w in wanted if w.startswith(("av_", "ff_", "avpriv_",
                                                 "avcodec_", "avutil_"))]
    if holes:
        for f, log in sorted(failed.items()):
            print(f"--- {f}.c\n{log}")
        raise SystemExit(f"the closure needs {holes}, which nothing that "
                         f"compiled defines")
    return picked, wanted


def depends(picked):
    flags = compile_flags()
    files = set()
    for n in picked:
        r = run([CROSS + "gcc"] + flags + ["-MM", os.path.join(SRC, n + ".c")])
        for tok in r.stdout.replace("\\\n", " ").split()[1:]:
            p = os.path.realpath(tok)
            if p.startswith(os.path.realpath(SRC) + os.sep):
                files.add(os.path.relpath(p, os.path.realpath(SRC)))
    return sorted(files)


def vendor(files, picked):
    keep = os.path.join(VENDOR, "README.kosmos.md")
    readme = open(keep).read() if os.path.exists(keep) else None
    shutil.rmtree(VENDOR, ignore_errors=True)
    for f in sorted(set(files) | set(ALWAYS) | set(conformance_refs())):
        dst = os.path.join(VENDOR, f)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(SRC, f), dst)
    if readme is not None:
        open(keep, "w").write(readme)

    with open(MK_OUT, "w") as mk:
        mk.write("#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see "
                 "LICENSE.\n")
        mk.write("#\n# Generated by tools/ffmpeg_vendor.py from FFmpeg "
                 f"{VERSION}; do not edit.\n")
        mk.write("# The objects the kit's entry points reach, and not one "
                 "more.\n#\n")
        mk.write("FFMPEG_NAMES := \\\n")
        for n in picked:
            mk.write(f"    {n} \\\n")
        mk.write("\n")
        mk.write("# What FFmpeg's `configure` compiles every file with.\n")
        mk.write("FFMPEG_CPPFLAGS := "
                 + " ".join(ffmpeg_cppflags(os.path.relpath(VENDOR, ROOT)))
                 + "\n")
    total = sum(os.path.getsize(os.path.join(VENDOR, f)) for f in files)
    print(f"vendored  {len(files)} files, {total // 1024} KB, "
          f"{len(picked)} objects")


def main():
    check_tarball()
    unpack_and_configure()
    correct_config()
    write_config()
    names = candidates()
    failed = compile_all(names)
    print(f"compiled  {len(names) - len(failed)} of {len(names)} "
          f"candidate objects")
    for f in sorted(failed):
        print(f"          did not compile: {f}.c")
    picked, wanted = closure(names, failed)
    print(f"closure   {len(picked)} objects: "
          + " ".join(os.path.basename(p) for p in picked))

    inc = os.path.join(ROOT, "runtime", "include")
    headers = []
    for d, _, fs in os.walk(inc):
        headers += [open(os.path.join(d, f)).read()
                    for f in fs if f.endswith(".h")]
    missing = [w for w in wanted if not declared(w, headers)]
    print(f"libc      the closure calls {len(wanted)}: " + " ".join(wanted))
    if missing:
        print("          runtime/include does not declare: "
              + " ".join(missing))

    files = depends(picked)
    vendor(files, picked)


if __name__ == "__main__":
    main()
