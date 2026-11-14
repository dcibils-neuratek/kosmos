#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""W5f's formats on Kosmos (`docs/w5.md`): Brotli 1.2.1, woff2 1.0.2,
libavif 1.4.2 and dav1d 1.5.4.

`formattest.elf` - the four libraries with
`user/kits/formattest/formattest.cpp` - given test files from a /Home of
its own: Brotli's own `tests/testdata`, libavif's own `tests/data`, and the
repository's TrueType fonts compressed to WOFF2 on this Mac by woff2's own
compressor (its release ships no fonts). What it says, a line a file, is
held to what the same file says on this Mac, compiled natively against the
same releases (`tools/w5_build.py host`).

Usage: run_formats.py IMAGE [APPS]

`KOSMOS_FORMATS_BREAK=<brotli|woff2|avif>` is the control: the reference is
made from that library's files with one byte changed in each, and the
guest must disagree.
"""

import fcntl
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402
import upstream                                              # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
X86 = "x86_64" in IMAGE
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    ROOT, "build", "user-x86_64" if X86 else "user", "apps")
WORK = scratch.directory("formats")
FOLDER = os.path.join(WORK, "formats")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")
# The reference is built for the guest's processor: libavif turns a deep
# picture's YUV into RGB in floating point, and ARM's compiler fuses a
# multiply and an add that x86-64 rounds twice - so an x86-64 guest is held
# to this Mac as an Intel one, under Rosetta, as `x86-skia` is (`docs/w5.md`
# W5e, W5f). Brotli, woff2 and dav1d answer the same either way.
REF = "host-x86_64" if X86 else "host"
ARCH_FLAGS = ["-arch", "x86_64", "-mmacosx-version-min=15.0"] if X86 else []
HOST_TOP = os.path.join(ROOT, "build", "w5", REF)
HOST = os.path.join(HOST_TOP, "install")
TEST = os.path.join(ROOT, "user", "kits", "formattest")
FONTS = os.path.join(ROOT, "assets", "fonts")

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- W5f's format test (`tools/run_formats.py`).
-- kosmos: image formattest.elf
local kit = use("formattest.elf")
for _, line in ipairs(kit.run("/Home/apps/formats/formats")) do
  print(line)
end
"""


def source(name):
    g = upstream.GIT[name]
    return upstream.checkout(name, g["url"], g["commit"])


def mac_libraries():
    """This Mac's builds of the four, and woff2's compressor linked here:
    its CMake leaves out `libbrotlicommon`, which a static brotli needs."""
    os.makedirs(os.path.join(ROOT, "build", "w5"), exist_ok=True)
    with open(os.path.join(ROOT, "build", "w5", ".host-lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        subprocess.run([sys.executable, os.path.join(HERE, "w5_build.py"), "host",
                        "brotli", "woff2"], check=True, capture_output=True)
        subprocess.run([sys.executable, os.path.join(HERE, "w5_build.py"), REF,
                        "brotli", "woff2", "dav1d", "libavif"], check=True, capture_output=True)
        subprocess.run(["ninja", "-C", os.path.join(ROOT, "build", "w5", "host", "woff2"),
                        "woff2enc"], check=True, capture_output=True)
    # The compressor only makes the test's fonts, so it is this Mac's own.
    native = os.path.join(ROOT, "build", "w5", "host")
    lib = os.path.join(native, "install", "lib")
    compressor = os.path.join(WORK, "woff2_compress")
    subprocess.run(["/usr/bin/clang++", "-std=c++17", "-O2",
                    "-I" + os.path.join(source("woff2"), "include"),
                    os.path.join(source("woff2"), "src", "woff2_compress.cc"),
                    os.path.join(native, "woff2", "libwoff2enc.a"),
                    os.path.join(lib, "libwoff2common.a"), os.path.join(lib, "libbrotlienc.a"),
                    os.path.join(lib, "libbrotlicommon.a"), "-o", compressor], check=True)
    return compressor


def files(compressor):
    """(name, path) for every file the guest is given."""
    found = []
    data = os.path.join(source("brotli"), "tests", "testdata")
    for n in sorted(os.listdir(data)):
        if n.endswith(".compressed") and os.path.exists(os.path.join(data, n[:-11])):
            found += [("brotli/" + n[:-11], os.path.join(data, n[:-11])),
                      ("brotli/" + n, os.path.join(data, n))]
    fonts = os.path.join(WORK, "woff2")
    shutil.rmtree(fonts, ignore_errors=True)
    os.makedirs(fonts)
    for n in sorted(os.listdir(FONTS)):
        if n.endswith(".ttf"):
            shutil.copyfile(os.path.join(FONTS, n), os.path.join(fonts, n))
            subprocess.run([compressor, os.path.join(fonts, n)], check=True, capture_output=True)
            found.append(("woff2/" + n[:-4] + ".woff2", os.path.join(fonts, n[:-4] + ".woff2")))
    data = os.path.join(source("libavif"), "tests", "data")
    for n in sorted(os.listdir(data)):
        if n.endswith(".avif"):
            found.append(("avif/" + n, os.path.join(data, n)))
    return found


def gather(found, broken):
    shutil.rmtree(FOLDER, ignore_errors=True)
    for name, path in found:
        into = os.path.join(FOLDER, name)
        os.makedirs(os.path.dirname(into), exist_ok=True)
        shutil.copyfile(path, into)
        # The control: one byte in the middle of each of that library's
        # files turned over, for the reference only (brotli's compressed
        # files, not the originals they are held to).
        if broken and name.startswith(broken + "/") and not (
                broken == "brotli" and not name.endswith(".compressed")):
            b = bytearray(open(into, "rb").read())
            if b:
                b[len(b) // 2] ^= 0xff
                open(into, "wb").write(bytes(b))
    with open(os.path.join(FOLDER, "manifest.txt"), "w") as f:
        f.write("".join(n + "\n" for n, _ in found))


def reference():
    binary = os.path.join(WORK, "formattest-mac")
    lib = os.path.join(HOST, "lib")
    # nasm's objects carry no macOS version, which the linker remarks on
    # once an object; said only if the link fails.
    subprocess.run(["/usr/bin/clang++"] + ARCH_FLAGS + [
                    "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-DFORMATTEST_HOST", "-I" + os.path.join(HOST, "include"),
                    os.path.join(TEST, "formattest.cpp")]
                   + [os.path.join(lib, n) for n in (
                       "libavif.a", "libdav1d.a", "libwoff2dec.a", "libwoff2common.a",
                       "libbrotlidec.a", "libbrotlicommon.a")] + ["-o", binary],
                   check=True, capture_output=True)
    return subprocess.run([binary, FOLDER], capture_output=True, text=True,
                          check=True).stdout.splitlines()


def main():
    built = os.path.join(APPS, "formattest.elf")
    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` (and ARCH=x86_64) builds it" % built)
        return 1

    found = files(mac_libraries())
    broken = os.environ.get("KOSMOS_FORMATS_BREAK")
    gather(found, broken)
    want = reference()
    if broken:
        gather(found, None)             # the guest gets the true files

    image = os.path.join(WORK, "formattest.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)
    with open(os.path.join(WORK, "formattest.lua"), "w") as f:
        f.write(PROGRAM)

    disk = [(os.path.join(WORK, n), "/Home/apps/formats/" + n)
            for n in ("formattest.lua", "formattest.elf")]
    disk += [(os.path.join(FOLDER, n), "/Home/apps/formats/formats/" + n)
             for n in [n for n, _ in found] + ["manifest.txt"]]
    megabytes = sum(os.path.getsize(src) for src, _ in disk) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)] + ["%s:%s" % f for f in disk],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    guest = R.Guest(IMAGE, 300)
    said = []
    done = False
    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/formats/formattest.lua")
        while time.monotonic() < started + 300 and not done:
            time.sleep(0.2)
            guest._read_available()
            lines = guest.seen[mark:].replace("\r", "").split("\n")[1:]
            said = [ln for ln in lines if ln.split(" ")[0].rstrip(":")
                    in ("versions", "brotli", "woff2", "avif", "formattest")]
            done = any(ln.startswith("formattest: ") for ln in said) or "ended, code" in guest.seen[mark:]
        took = time.monotonic() - started
    finally:
        guest.close()

    if os.environ.get("KOSMOS_FORMATS_SERIAL"):
        with open(os.environ["KOSMOS_FORMATS_SERIAL"], "w") as f:
            f.write(guest.seen)

    differ = [(w, s) for w, s in zip(want, said) if w != s]
    if len(want) != len(said) or differ:
        print("FAIL: the formats on Kosmos are not what they are on this Mac: "
              "%d lines here, %d there, %d differ" % (len(want), len(said), len(differ)))
        for w, s in differ[:12]:
            print("  Mac:    %s\n  Kosmos: %s" % (w, s))
        if not any(ln.startswith("formattest: ") for ln in said):
            print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        return 1

    kinds = {}
    for ln in want:
        k = ln.split(" ")[0].rstrip(":")
        kinds[k] = kinds.get(k, 0) + 1
    refused = sum(1 for ln in want if ln.startswith("avif ") and " frame" not in ln)
    print("PASS: %d files on Kosmos, every line as on this Mac, in %.1f s (%s; %d AVIF "
          "files refused, as libavif's tests mean them to be)"
          % (len(want) - 2, took, ", ".join("%s %d" % kv for kv in sorted(kinds.items())
                                            if kv[0] not in ("versions", "formattest")),
             refused))
    return 0


if __name__ == "__main__":
    sys.exit(main())
