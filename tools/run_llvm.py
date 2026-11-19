#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""LLVM on Kosmos (`docs/gpu3d.md` G1).

`llvmtest.elf` - LLVM 23.1.2's libraries, code generation for both
processors and the ORC JIT, with the C++ runtimes and the POSIX layer, in
one image with `user/kits/llvmtest/llvmtest.cpp` - run from a /Home of its
own and held to the sentence it must say: a function in LLVM's text form
compiled at run time for the processor it runs on and called with a number
the caller chose then, its answer the one this script computes; a second
function in floating point; and the same module written as an object for
the other processor.

Usage: run_llvm.py IMAGE [APPS]

`APPS` is where `make apps` linked `llvmtest.elf` - the gate's test
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
WORK = scratch.directory("llvm")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")

# The number the JIT's function is called with, chosen here and passed at
# run time, and the answer worked out here rather than by LLVM.
N = 1000
SQUARES = sum(i * i for i in range(N)) % 1000003

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- LLVM's test program (`tools/run_llvm.py`).
-- kosmos: image llvmtest.elf
local kit = use("llvmtest.elf")
print("llvmtest: " .. kit.run(%d))
""" % N

HERE_ARCH, OTHER_ARCH, OTHER_MACHINE = (("x86_64", "aarch64", 183) if X86
                                        else ("aarch64", "x86_64", 62))

WANT = " ".join([
    "llvm",
    "jit=%s/squares(%d)=%d/hypot(3,4)=5.000" % (HERE_ARCH, N, SQUARES),
    "other=%s/elf-machine-%d/object" % (OTHER_ARCH, OTHER_MACHINE),
])


def main():
    built = os.path.join(APPS, "llvmtest.elf")

    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` (and ARCH=x86_64) builds it" % built)
        return 1

    want = WANT
    image = os.path.join(WORK, "llvmtest.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)

    with open(os.path.join(WORK, "llvmtest.lua"), "w") as f:
        f.write(PROGRAM)

    megabytes = os.path.getsize(image) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)]
                   + ["%s:/Home/apps/llvm/%s" % (os.path.join(WORK, n), n)
                      for n in ("llvmtest.lua", "llvmtest.elf")],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    guest = R.Guest(IMAGE, 180)
    said = None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/llvm/llvmtest.lua")

        while time.monotonic() < started + 240 and said is None:
            for line in guest.seen[mark:].split("\n")[1:-1]:
                if line.strip().startswith("llvmtest: "):
                    said = line.strip()
                    break
            if said is None:
                time.sleep(0.2)
                guest._read_available()
        took = time.monotonic() - started
    finally:
        guest.close()

    if os.environ.get("KOSMOS_LLVM_SERIAL"):
        with open(os.environ["KOSMOS_LLVM_SERIAL"], "w") as f:
            f.write(guest.seen)

    if said != "llvmtest: " + want:
        print("FAIL: LLVM on Kosmos did not say what it must")
        print("  wanted: %s" % want)
        print("  said:   %s" % said)
        if said is None:
            print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        return 1

    print("PASS: LLVM on Kosmos, from a %.1f MB image, in %.1f s: %s"
          % (os.path.getsize(image) / 1e6, took, want))
    return 0


if __name__ == "__main__":
    sys.exit(main())
