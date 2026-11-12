#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""ICU on Kosmos (`docs/w4.md` W4b).

`icutest.elf` - ICU 79.1 and its data in one image with
`user/kits/icutest/icutest.cpp` - run from a /Home of its own, and what it
says held to what the same file says on this Mac, compiled against the same
ICU built natively (`tools/icu_build.py host`). The sentence normalises,
collates in German and Swedish, breaks English and Thai into words and
formats a number in three locales; every piece of it comes out of ICU's
data, so a guest that did not find the data says an error's name instead.

Usage: run_icu.py IMAGE [APPS]

`APPS` is where `make apps` linked `icutest.elf` - the gate's test
userland, or the lean one's by default.
"""

import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
X86 = "x86_64" in IMAGE
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    ROOT, "build", "user-x86_64" if X86 else "user", "apps")
WORK = scratch.directory("icu")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")
ICU_SRC = os.path.join(ROOT, "build", "upstream", "icu", "icu", "source")

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- ICU's test program (`tools/run_icu.py`).
-- kosmos: image icutest.elf
local kit = use("icutest.elf")
print("icutest: " .. kit.run())
"""


def reference():
    """The sentence on this Mac: the same file, the same ICU, natively."""
    host = os.path.join(ROOT, "build", "icu", "host")
    subprocess.run([sys.executable, os.path.join(HERE, "icu_build.py"), "host"],
                   check=True, capture_output=True)
    binary = os.path.join(host, "test", "icutest")
    os.makedirs(os.path.dirname(binary), exist_ok=True)
    subprocess.run(["/usr/bin/clang++", "-std=c++20", "-O2",
                    "-DU_STATIC_IMPLEMENTATION", "-DU_DISABLE_RENAMING=1",
                    "-DU_USING_ICU_NAMESPACE=0",
                    "-I" + os.path.join(ICU_SRC, "common"), "-I" + os.path.join(ICU_SRC, "i18n"),
                    os.path.join(ROOT, "user", "kits", "icutest", "icutest.cpp"),
                    os.path.join(host, "libicui18n.a"), os.path.join(host, "libicuuc.a"),
                    os.path.join(host, "icudata.o"), "-o", binary], check=True)
    return subprocess.run([binary], capture_output=True, text=True,
                          check=True).stdout.strip()


def main():
    built = os.path.join(APPS, "icutest.elf")

    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` (and ARCH=x86_64) builds it" % built)
        return 1

    want = reference()
    image = os.path.join(WORK, "icutest.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)

    with open(os.path.join(WORK, "icutest.lua"), "w") as f:
        f.write(PROGRAM)

    megabytes = os.path.getsize(image) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)]
                   + ["%s:/Home/apps/icu/%s" % (os.path.join(WORK, n), n)
                      for n in ("icutest.lua", "icutest.elf")],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    guest = R.Guest(IMAGE, 180)
    said = None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/icu/icutest.lua")

        while time.monotonic() < started + 240 and said is None:
            for line in guest.seen[mark:].split("\n")[1:-1]:
                if line.strip().startswith("icutest: "):
                    said = line.strip()
                    break
            if said is None:
                time.sleep(0.2)
                guest._read_available()
        took = time.monotonic() - started
    finally:
        guest.close()

    if os.environ.get("KOSMOS_ICU_SERIAL"):
        with open(os.environ["KOSMOS_ICU_SERIAL"], "w") as f:
            f.write(guest.seen)

    if said != "icutest: " + want:
        print("FAIL: ICU on Kosmos did not answer as it does on this Mac")
        print("  Mac:    %s" % want)
        print("  Kosmos: %s" % said)
        if said is None:
            print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        return 1

    print("PASS: ICU on Kosmos answers as on this Mac, from a %.1f MB image, in %.1f s: %s"
          % (os.path.getsize(image) / 1e6, took, want))
    return 0


if __name__ == "__main__":
    sys.exit(main())
