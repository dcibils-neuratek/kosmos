#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""W5a's pictures on Kosmos (`docs/w5.md`).

`picturetest.elf` - zlib 1.4, libpng 1.6.59, libjpeg-turbo 3.2.1 and
libwebp 1.6.0 with `user/kits/picturetest/picturetest.c` - given the test
pictures each library's own release ships, from a /Home of its own; what it
says, a line a picture, held to what the same file says on this Mac,
compiled natively against the same releases (`tools/w5_build.py host`).

Usage: run_pictures.py IMAGE [APPS]

`KOSMOS_PICTURES_BREAK=<library>` is the control: the reference is made
from that library's pictures with one byte changed in each, and the guest
must disagree.
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
WORK = scratch.directory("pictures")
FOLDER = os.path.join(WORK, "pictures")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")
HOST = os.path.join(ROOT, "build", "w5", "host", "install")
TEST = os.path.join(ROOT, "user", "kits", "picturetest")

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- W5a's picture test (`tools/run_pictures.py`).
-- kosmos: image picturetest.elf
local kit = use("picturetest.elf")
for _, line in ipairs(kit.run("/Home/apps/pictures/pictures")) do
  print(line)
end
"""


def pictures():
    """Each library's own test pictures, under the library's name: every
    PNG in libpng's tree - PngSuite, its broken and crashing files - every
    picture in libjpeg-turbo's `testimages`, and libwebp's WebPs with the
    PPM `test.webp` decodes to."""
    png = upstream.checkout("libpng", upstream.GIT["libpng"]["url"],
                            upstream.GIT["libpng"]["commit"])
    webp = upstream.checkout("libwebp", upstream.GIT["libwebp"]["url"],
                             upstream.GIT["libwebp"]["commit"])
    upstream.unpack("libjpeg-turbo")
    jpeg = os.path.join(upstream.where("libjpeg-turbo"), "testimages")

    found = []
    for folder, dirs, names in os.walk(png):
        dirs[:] = sorted(d for d in dirs if d != ".git")
        for n in sorted(names):
            if n.endswith(".png"):
                path = os.path.join(folder, n)
                found.append(("libpng/" + os.path.relpath(path, png), path))
    for n in sorted(os.listdir(jpeg)):
        if n.endswith((".jpg", ".png", ".ppm")):
            found.append(("libjpeg-turbo/" + n, os.path.join(jpeg, n)))
    for rel in ("examples/test.webp", "examples/test_ref.ppm",
                "webp_js/test_webp_js.webp", "webp_js/test_webp_wasm.webp"):
        found.append(("libwebp/" + rel, os.path.join(webp, rel)))
    return found


def gather(broken):
    shutil.rmtree(FOLDER, ignore_errors=True)
    names = []
    for name, path in pictures():
        into = os.path.join(FOLDER, name)
        os.makedirs(os.path.dirname(into), exist_ok=True)
        shutil.copyfile(path, into)
        names.append(name)
    with open(os.path.join(FOLDER, "manifest.txt"), "w") as f:
        f.write("".join(n + "\n" for n in names))

    if broken:
        # The control: the same names, one byte in the middle of each of
        # that library's pictures turned over, for the reference only.
        for n in names:
            if n.startswith(broken + "/"):
                p = os.path.join(FOLDER, n)
                b = bytearray(open(p, "rb").read())
                if b:
                    b[len(b) // 2] ^= 0xff
                    open(p, "wb").write(bytes(b))
    return names


def reference():
    """What this Mac says: the same file, natively, against the same
    releases."""
    # One build of the libraries for this Mac at a time: the gate runs the
    # suites for both processors side by side, and both would be in the
    # same ninja folder.
    os.makedirs(os.path.join(ROOT, "build", "w5"), exist_ok=True)
    with open(os.path.join(ROOT, "build", "w5", ".host-lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        subprocess.run([sys.executable, os.path.join(HERE, "w5_build.py"), "host"],
                       check=True, capture_output=True)
    binary = os.path.join(WORK, "picturetest-mac")
    lib = os.path.join(HOST, "lib")
    subprocess.run(["/usr/bin/clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-DPICTURETEST_HOST", "-I" + os.path.join(HOST, "include"),
                    os.path.join(TEST, "picturetest.c"),
                    os.path.join(lib, "libwebp.a"), os.path.join(lib, "libsharpyuv.a"),
                    os.path.join(lib, "libpng.a"), os.path.join(lib, "libjpeg.a"),
                    os.path.join(lib, "libz.a"), "-o", binary], check=True)
    return subprocess.run([binary, FOLDER], capture_output=True, text=True,
                          check=True).stdout.splitlines()


def main():
    built = os.path.join(APPS, "picturetest.elf")

    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` (and ARCH=x86_64) builds it" % built)
        return 1

    broken = os.environ.get("KOSMOS_PICTURES_BREAK")
    names = gather(broken)
    want = reference()
    if broken:
        gather(None)                    # the guest gets the true pictures

    image = os.path.join(WORK, "picturetest.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)
    with open(os.path.join(WORK, "picturetest.lua"), "w") as f:
        f.write(PROGRAM)

    files = [(os.path.join(WORK, n), "/Home/apps/pictures/" + n)
             for n in ("picturetest.lua", "picturetest.elf")]
    files += [(os.path.join(FOLDER, n), "/Home/apps/pictures/pictures/" + n)
              for n in names + ["manifest.txt"]]
    megabytes = (os.path.getsize(image)
                 + sum(os.path.getsize(src) for src, _ in files)) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)] + ["%s:%s" % f for f in files],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    guest = R.Guest(IMAGE, 180)
    said = []
    done = False

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/pictures/picturetest.lua")

        while time.monotonic() < started + 240 and not done:
            time.sleep(0.2)
            guest._read_available()
            lines = guest.seen[mark:].replace("\r", "").split("\n")[1:]
            said = [ln for ln in lines if ": " in ln and ln.split(":")[0].split(" ")[0]
                    in ("versions", "zlib", "png", "png-encode", "jpeg", "jpeg-encode",
                        "webp", "webp-reference", "webp-encode", "picturetest")
                    or ln.endswith(": not read")]
            done = any(ln.startswith("picturetest: ") for ln in said)
        took = time.monotonic() - started
    finally:
        guest.close()

    if os.environ.get("KOSMOS_PICTURES_SERIAL"):
        with open(os.environ["KOSMOS_PICTURES_SERIAL"], "w") as f:
            f.write(guest.seen)

    differ = [(w, s) for w, s in zip(want, said) if w != s]
    if len(want) != len(said) or differ:
        print("FAIL: the pictures on Kosmos are not what they are on this Mac: "
              "%d lines here, %d there, %d differ" % (len(want), len(said), len(differ)))
        for w, s in differ[:12]:
            print("  Mac:    %s\n  Kosmos: %s" % (w, s))
        if not done:
            print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        return 1

    kinds = {}
    for ln in want:
        k = ln.split(" ")[0].rstrip(":")
        kinds[k] = kinds.get(k, 0) + 1
    print("PASS: %d pictures on Kosmos, %d lines, every one as on this Mac, in %.1f s (%s)"
          % (len(names), len(want), took,
             ", ".join("%s %d" % kv for kv in sorted(kinds.items()))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
