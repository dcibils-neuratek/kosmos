#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Mesa on Kosmos (`docs/gpu3d.md` G1).

`mesatest.elf` - Mesa 26.2.5's `util`, NIR and SPIR-V libraries with the
POSIX layer, in one image with `user/kits/mesatest/mesatest.c` - run from a
/Home of its own and held to the sentence it must say: a compute shader
built with NIR's builder from three numbers this script chose, folded by
NIR's passes to the one constant this script computes; the shader printed
into a stream in memory, its first line Mesa's; and the shader serialized,
read back and printed again the same.

Usage: run_mesa.py IMAGE [APPS]

`APPS` is where `make apps` linked `mesatest.elf` - the gate's test
userland, or the lean one's by default.
"""

import os
import re
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
WORK = scratch.directory("mesa")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")

# The shader's three numbers, chosen here, and what folding them must leave
# worked out here rather than by Mesa. Each is exact in a float, so the
# sum and the product are too.
A, B, C = 1.5, 2.25, 4.0
FOLDED = (A + B) * C

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Mesa's test program (`tools/run_mesa.py`).
-- kosmos: image mesatest.elf
local kit = use("mesatest.elf")
print("mesatest: " .. kit.run(%r, %r, %r))
""" % (A, B, C)

# How many rounds the passes took is Mesa's business and is shown, not held.
WANT = re.compile(r"mesa fold=%.4f/\d+-rounds "
                  r'print="shader: MESA_SHADER_COMPUTE"/long blob=same$' % FOLDED)


def main():
    built = os.path.join(APPS, "mesatest.elf")

    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` builds it" % built)
        return 1

    image = os.path.join(WORK, "mesatest.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)

    with open(os.path.join(WORK, "mesatest.lua"), "w") as f:
        f.write(PROGRAM)

    megabytes = os.path.getsize(image) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)]
                   + ["%s:/Home/apps/mesa/%s" % (os.path.join(WORK, n), n)
                      for n in ("mesatest.lua", "mesatest.elf")],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    guest = R.Guest(IMAGE, 180)
    said = None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/mesa/mesatest.lua")

        while time.monotonic() < started + 240 and said is None:
            for line in guest.seen[mark:].split("\n")[1:-1]:
                if line.strip().startswith("mesatest: "):
                    said = line.strip()
                    break
            if said is None:
                time.sleep(0.2)
                guest._read_available()
        took = time.monotonic() - started
    finally:
        guest.close()

    if os.environ.get("KOSMOS_MESA_SERIAL"):
        with open(os.environ["KOSMOS_MESA_SERIAL"], "w") as f:
            f.write(guest.seen)

    said_text = said[len("mesatest: "):] if said else None
    if said_text is None or not WANT.match(said_text):
        print("FAIL: Mesa on Kosmos did not say what it must")
        print("  wanted: %s" % WANT.pattern)
        print("  said:   %s" % said)
        if said is None:
            print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        return 1

    print("PASS: Mesa on Kosmos, from a %.1f MB image, in %.1f s: %s"
          % (os.path.getsize(image) / 1e6, took, said_text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
