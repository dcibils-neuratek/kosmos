#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""W5e's Skia on Kosmos (`docs/w5.md`).

`skiatest.elf` - Skia as WebKitGTK 2.54.2 carries it, built by WebKit's own
CMake beside WTF, with `user/kits/skiatest/skiatest.cpp` - draws eight
scenes: fills, strokes and dashes, every kind of gradient, text through
FreeType from a folder of fonts, pictures from libpng's and libjpeg-turbo's
own test images decoded by Skia's codecs and drawn with each sampling,
layers, blend modes, clips, blurs and a colour matrix, a recorded picture
played back, and a destination in linear sRGB. What it says - a line a
scene with a fingerprint of its pixels, a line a picture decoded, a font
family and a measured string - is held line for line to the same file on
this Mac, built against Skia, FreeType and the picture libraries compiled
for x86-64 here (`tools/webkit_build.py host-x86_64`, `tools/w5_build.py
host-x86_64`) and run under Rosetta, so that both sides take the same
vector paths: one pixel drawn differently is a line that disagrees.

And `skia_defines.h`, what `skiatest.cpp` is compiled with so that Skia's
headers mean what the library's did, is held to what Skia's CMake declares
PUBLIC, read from the target's property by the reference build.

Usage: run_skia.py IMAGE [APPS]

The controls: `KOSMOS_SKIA_BREAK=picture` makes the reference from a JPEG
with one byte of its picture turned over, and the guest must disagree;
`=defines` leaves a define out of what the header is held to, and that
check must fail.
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
WORK = scratch.directory("skia")
FOLDER = os.path.join(WORK, "skia")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")
TEST = os.path.join(ROOT, "user", "kits", "skiatest")
SKIA = os.path.join(ROOT, "build", "webkit", "src", "Source", "ThirdParty", "skia")

# The reference is built for the processor the guest is: x86-64 under
# Rosetta for the x86 suite. AArch64 natively would be the ARM suite's.
REF = "host-x86_64" if X86 else "host"
W5 = os.path.join(ROOT, "build", "w5", REF, "install")
REF_SKIA = os.path.join(ROOT, "build", "webkit", REF)

FONTS = ["IBMPlexSans-Regular.ttf", "IBMPlexSans-Bold.ttf", "IBMPlexSans-Italic.ttf",
         "IBMPlexMono-Regular.ttf", "IBMPlexSerif-Regular.ttf"]
PICTURES = [("libpng", "pngtest.png"),
            ("libpng", "contrib/testpngs/rgb-alpha-8-sRGB.png"),
            ("libpng", "contrib/testpngs/gray-16-tRNS.png"),
            ("libjpeg-turbo", "testimages/testorig.jpg"),
            ("libjpeg-turbo", "testimages/testimgint.jpg")]
SCENES = ["fills", "strokes", "gradients", "text", "pictures", "effects", "recorded",
          "colourspace"]
SAID = ("versions", "family", "picture", "scene", "measure", "recorded", "skiatest")

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- W5e's Skia test (`tools/run_skia.py`).
-- kosmos: image skiatest.elf
local kit = use("skiatest.elf")
for _, line in ipairs(kit.run("/Home/apps/skia/skia")) do
  print(line)
end
"""


def roots():
    """Where each release's files are: libpng is checked out at its
    commit, libjpeg-turbo unpacked from its release - as
    `run_pictures.py` finds them."""
    png = upstream.checkout("libpng", upstream.GIT["libpng"]["url"],
                            upstream.GIT["libpng"]["commit"])
    upstream.unpack("libjpeg-turbo")
    return {"libpng": png, "libjpeg-turbo": upstream.where("libjpeg-turbo")}


def gather(broken):
    """The fonts and the pictures, under the folder."""
    shutil.rmtree(FOLDER, ignore_errors=True)
    os.makedirs(os.path.join(FOLDER, "fonts"))
    os.makedirs(os.path.join(FOLDER, "pictures"))
    for f in FONTS:
        shutil.copyfile(os.path.join(ROOT, "assets", "fonts", f),
                        os.path.join(FOLDER, "fonts", f))
    where = roots()
    for name, path in PICTURES:
        into = os.path.join(FOLDER, "pictures", os.path.basename(path))
        shutil.copyfile(os.path.join(where[name], path), into)
        if broken == "picture" and path.endswith("testorig.jpg"):
            # The control: a byte well inside the entropy-coded data,
            # past every header, turned over - it still decodes, wrongly.
            b = bytearray(open(into, "rb").read())
            b[len(b) * 3 // 4] ^= 0x5a
            open(into, "wb").write(bytes(b))


def files_in_folder():
    return ["fonts/" + f for f in FONTS] + \
           ["pictures/" + os.path.basename(p) for _, p in PICTURES]


def reference():
    """What this Mac says: the same file, built for the guest's processor
    against the same releases."""
    os.makedirs(os.path.join(ROOT, "build", "w5"), exist_ok=True)
    with open(os.path.join(ROOT, "build", "w5", ".host-lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        # Only what Skia stands on: the rest of W5 is the other suites'.
        for script, args in (("w5_build.py", ["zlib", "libpng", "libjpeg-turbo", "freetype"]),
                             ("webkit_build.py", [])):
            r = subprocess.run([sys.executable, os.path.join(HERE, script), REF] + args,
                               capture_output=True, text=True)
            if r.returncode:
                raise SystemExit("FAIL: %s %s:\n%s" % (script, REF, (r.stdout + r.stderr)[-3000:]))
    binary = os.path.join(WORK, "skiatest-mac")
    lib = os.path.join(W5, "lib")
    arch = ["-arch", "x86_64", "-march=x86-64"] if X86 else ["-arch", "arm64"]
    subprocess.run(["/usr/bin/clang++"] + arch + [
                    "-mmacosx-version-min=15.0", "-std=c++20", "-O2", "-Wall", "-Wextra",
                    "-Werror", "-DSKIATEST_HOST", "-isystem", SKIA,
                    "-isystem", os.path.join(W5, "include", "freetype2"),
                    os.path.join(TEST, "skiatest.cpp"),
                    os.path.join(REF_SKIA, "lib", "libSkia.a"),
                    os.path.join(lib, "libfreetype.a"), os.path.join(lib, "libpng.a"),
                    os.path.join(lib, "libjpeg.a"), os.path.join(lib, "libz.a"),
                    "-Wl,-w", "-o", binary], check=True)
    return subprocess.run([binary, FOLDER], capture_output=True, text=True,
                          check=True).stdout.splitlines()


def defines_disagree(broken):
    """What `skia_defines.h` defines against what Skia declares PUBLIC on
    this Mac - the same list but the platform, which Kosmos names and this
    Mac is taken to be."""
    public = os.path.join(REF_SKIA, "skia-public-defines.txt")
    # `X` and `X=1` are one definition to the preprocessor, and both are
    # written `X` here.
    want = {d.strip()[:-2] if d.strip().endswith("=1") else d.strip()
            for d in open(public) if d.strip()}
    want.add("SK_BUILD_FOR_UNIX")       # Kosmos's, on both of its processors
    if broken == "defines":
        want.add("SK_CONTROL_DEFINE_THE_HEADER_LACKS")

    have = set()
    for line in open(os.path.join(TEST, "skia_defines.h")):
        parts = line.split(None, 2)
        if len(parts) >= 2 and parts[0] == "#define" and parts[1] != "SKIA_DEFINES_H":
            value = parts[2].strip() if len(parts) == 3 else ""
            have.add(parts[1] if value in ("", "1") else "%s=%s" % (parts[1], value))
    return sorted(want - have), sorted(have - want)


def main():
    built = os.path.join(APPS, "skiatest.elf")

    if not os.path.exists(built) and not os.environ.get("KOSMOS_SKIA_MAC_ONLY"):
        print("FAIL: no %s - `make apps` (and ARCH=x86_64) builds it" % built)
        return 1

    broken = os.environ.get("KOSMOS_SKIA_BREAK")
    if broken not in (None, "picture", "defines"):
        print("FAIL: KOSMOS_SKIA_BREAK is picture or defines, not %s" % broken)
        return 1
    gather(broken)
    want = reference()
    if broken == "picture":
        gather(None)                    # the guest gets the true picture

    missing, extra = defines_disagree(broken)
    if missing or extra:
        print("FAIL: skia_defines.h is not what Skia declares PUBLIC: "
              "missing %s, not declared %s" % (missing, extra))
        return 1

    if os.environ.get("KOSMOS_SKIA_MAC_ONLY"):
        print("\n".join(want))
        return 0

    # The reference must have drawn what it was asked to before it can
    # be anything's reference.
    scenes = [ln for ln in want if ln.startswith("scene: ")]
    pictures = [ln for ln in want if ln.startswith("picture: ")]
    if [ln.split()[1] for ln in scenes] != SCENES \
            or any(" painted 0" in ln or "painted" not in ln for ln in scenes) \
            or len(pictures) != len(PICTURES) \
            or any(" success " not in ln for ln in pictures) \
            or want[-1] != "skiatest: %d scenes" % len(SCENES):
        print("FAIL: Skia on this Mac did not draw every scene:\n  " + "\n  ".join(want))
        return 1

    image = os.path.join(WORK, "skiatest.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)
    with open(os.path.join(WORK, "skiatest.lua"), "w") as f:
        f.write(PROGRAM)

    files = [(os.path.join(WORK, n), "/Home/apps/skia/" + n)
             for n in ("skiatest.lua", "skiatest.elf")]
    files += [(os.path.join(FOLDER, n), "/Home/apps/skia/skia/" + n)
              for n in files_in_folder()]
    megabytes = sum(os.path.getsize(src) for src, _ in files) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)] + ["%s:%s" % f for f in files],
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
        guest.type("run /Home/apps/skia/skiatest.lua")

        while time.monotonic() < started + 240 and not done:
            time.sleep(0.2)
            guest._read_available()
            lines = guest.seen[mark:].replace("\r", "").split("\n")[1:]
            said = [ln for ln in lines if ln.split(":")[0] in SAID]
            done = any(ln.startswith("skiatest: ") for ln in said)
        took = time.monotonic() - started
    finally:
        guest.close()

    if os.environ.get("KOSMOS_SKIA_SERIAL"):
        with open(os.environ["KOSMOS_SKIA_SERIAL"], "w") as f:
            f.write(guest.seen)

    differ = [(w, s) for w, s in zip(want, said) if w != s]
    if len(want) != len(said) or differ:
        print("FAIL: Skia on Kosmos does not draw what it draws on this Mac: "
              "%d lines here, %d there, %d differ" % (len(want), len(said), len(differ)))
        for w, s in differ[:12]:
            print("  Mac:    %s\n  Kosmos: %s" % (w, s))
        if not done:
            print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        return 1

    print("PASS: Skia drew %d scenes - paths, gradients, text through FreeType, %d pictures "
          "through its codecs, effects, a recording, another colour space - every pixel as "
          "on this Mac, in %.1f s" % (len(scenes), len(pictures), took))
    return 0


if __name__ == "__main__":
    sys.exit(main())
