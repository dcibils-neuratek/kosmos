#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""WTF on Kosmos (`docs/w4.md` W4c).

`wtftest.elf` - WTF, bmalloc, ICU and the C++ runtimes in one image with
`user/kits/wtftest/wtftest.cpp` - run from a /Home of its own, and what it
says held to the sentence it must say: its strings, a hash map, threads,
a thread suspended by Nebula and its registers read, the run loop across
threads with a timer, a thread's own rounding mode, and files through
std::filesystem on the POSIX layer. The suspension's "held" is measured
between two windows in which the counter must be seen moving, so a
suspension that did nothing cannot pass.

Usage: run_wtf.py IMAGE [APPS]

`APPS` is where `make apps` linked `wtftest.elf` - the gate's test
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
WORK = scratch.directory("wtf")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- WTF's test program (`tools/run_wtf.py`).
-- kosmos: image wtftest.elf
local kit = use("wtftest.elf")
print("wtftest: " .. kit.run())
"""

# What WTF must say, piece by piece (`user/kits/wtftest/wtftest.cpp` has
# what each piece asks).
WANT = " ".join([
    "wtf",
    "port=kosmos",                              # PLATFORM(KOSMOS), as WTF was built
    "text=25/26 KOSMOS RUNS WEBKIT 382",        # 25 UTF-16 units, 26 UTF-8 bytes
    "map=1000/1000/332833500",                  # the squares of 0..999, summed
    "threads=400000",
    "suspend=moved/held/sp-in-stack/moved",
    "runloop=abcxt/waited",
    "rounding=down/nearest-here",
    "files=made/30/copied/30/same/a,two.txt/dir/temporary/removed",
])


def main():
    built = os.path.join(APPS, "wtftest.elf")

    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` (and ARCH=x86_64) builds it" % built)
        return 1

    want = WANT
    image = os.path.join(WORK, "wtftest.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)

    with open(os.path.join(WORK, "wtftest.lua"), "w") as f:
        f.write(PROGRAM)

    megabytes = os.path.getsize(image) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)]
                   + ["%s:/Home/apps/wtf/%s" % (os.path.join(WORK, n), n)
                      for n in ("wtftest.lua", "wtftest.elf")],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    guest = R.Guest(IMAGE, 180)
    said = None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/wtf/wtftest.lua")

        while time.monotonic() < started + 240 and said is None:
            for line in guest.seen[mark:].split("\n")[1:-1]:
                if line.strip().startswith("wtftest: "):
                    said = line.strip()
                    break
            if said is None:
                time.sleep(0.2)
                guest._read_available()
        took = time.monotonic() - started
    finally:
        guest.close()

    if os.environ.get("KOSMOS_WTF_SERIAL"):
        with open(os.environ["KOSMOS_WTF_SERIAL"], "w") as f:
            f.write(guest.seen)

    if said != "wtftest: " + want:
        print("FAIL: WTF on Kosmos did not say what it must")
        print("  wanted: %s" % want)
        print("  said:   %s" % said)
        if said is None:
            print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        return 1

    print("PASS: WTF on Kosmos, from a %.1f MB image, in %.1f s: %s"
          % (os.path.getsize(image) / 1e6, took, want))
    return 0


if __name__ == "__main__":
    sys.exit(main())
