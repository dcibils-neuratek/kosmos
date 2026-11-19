#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Vulkan on Kosmos (`docs/gpu3d.md` G2).

`vulkantest.elf` - `lavapipe`, Mesa's Vulkan driver, on llvmpipe and LLVM's
ORC JIT, with no loader, in one image with `user/kits/vulkantest/` - run
from a /Home of its own and held to the picture it must draw: a triangle
whose corners and colour this script chose, drawn by Vulkan into an image
in memory and read back as a mask. Which pixels had to be the triangle's
and which the clear colour's is worked out here, from the corners, and not
by anything in the guest.

Usage: run_vulkan.py IMAGE [APPS]
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
WORK = scratch.directory("vulkan")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")

# The triangle - its corners in pixels of the kit's 32 by 32 picture, and
# its colour - chosen here.
SIZE = 32
CORNERS = [(4.0, 3.0), (28.5, 10.25), (9.0, 29.5)]
COLOUR = (230, 96, 17)

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Vulkan's test program (`tools/run_vulkan.py`).
-- kosmos: image vulkantest.elf
local kit = use("vulkantest.elf")
print("vulkantest: " .. kit.triangle(%s))
""" % ", ".join(repr(v) for v in [c for p in CORNERS for c in p] + list(COLOUR))

# A pixel's centre a quarter of a pixel or more inside every edge must be
# the triangle's; that far outside any edge, the clear colour's. Nearer an
# edge either is right - that is the rasterizer's tie rule, not this
# script's - but never a third colour.
MARGIN = 0.25


def edge(a, b, p):
    """How far `p` is on the inner side of the edge a->b, in pixels, for a
    triangle wound as CORNERS is."""
    (ax, ay), (bx, by), (px, py) = a, b, p
    length = ((bx - ax) ** 2 + (by - ay) ** 2) ** 0.5
    return ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) / length


def expected():
    a, b, c = CORNERS
    sign = 1 if edge(a, b, c) > 0 else -1
    rows = []
    for y in range(SIZE):
        row = ""
        for x in range(SIZE):
            p = (x + 0.5, y + 0.5)
            d = [sign * edge(a, b, p), sign * edge(b, c, p), sign * edge(c, a, p)]
            if min(d) >= MARGIN:
                row += "#"
            elif min(d) <= -MARGIN:
                row += "."
            else:
                row += "*"
        rows.append(row)
    return rows


def judge(said):
    """None if the picture is right, or what was wrong."""
    m = re.match(r"vulkan device=llvmpipe/1\.(\d+) picture=([#.?/]+)$", said)
    if not m:
        return "not a picture from llvmpipe"
    got = m.group(2).split("/")
    want = expected()
    if len(got) != SIZE or any(len(r) != SIZE for r in got):
        return "a picture of %d rows, not %d by %d" % (len(got), SIZE, SIZE)
    wrong = []
    for y in range(SIZE):
        for x in range(SIZE):
            g, w = got[y][x], want[y][x]
            if g == "?" or (w != "*" and g != w):
                wrong.append("(%d,%d) %s for %s" % (x, y, g, w))
    if wrong:
        return "%d pixels wrong: %s" % (len(wrong), ", ".join(wrong[:8]))
    return None


def main():
    built = os.path.join(APPS, "vulkantest.elf")

    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` builds it" % built)
        return 1

    image = os.path.join(WORK, "vulkantest.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)

    with open(os.path.join(WORK, "vulkantest.lua"), "w") as f:
        f.write(PROGRAM)

    megabytes = os.path.getsize(image) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)]
                   + ["%s:/Home/apps/vulkan/%s" % (os.path.join(WORK, n), n)
                      for n in ("vulkantest.lua", "vulkantest.elf")],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    guest = R.Guest(IMAGE, 180)
    said = None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/vulkan/vulkantest.lua")

        while time.monotonic() < started + 240 and said is None:
            for line in guest.seen[mark:].split("\n")[1:-1]:
                if line.strip().startswith("vulkantest: "):
                    said = line.strip()
                    break
            if said is None:
                time.sleep(0.2)
                guest._read_available()
        took = time.monotonic() - started
    finally:
        guest.close()

    if os.environ.get("KOSMOS_VULKAN_SERIAL"):
        with open(os.environ["KOSMOS_VULKAN_SERIAL"], "w") as f:
            f.write(guest.seen)

    said_text = said[len("vulkantest: "):] if said else None
    wrong = judge(said_text) if said_text is not None else "nothing said"
    if wrong:
        print("FAIL: Vulkan on Kosmos did not draw the triangle: %s" % wrong)
        print("  wanted:\n    " + "\n    ".join(expected()))
        print("  said:   %s" % said)
        if said is None:
            print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        return 1

    inside = sum(r.count("#") for r in said_text.split("=")[-1].split("/"))
    print("PASS: Vulkan on Kosmos, from a %.1f MB image, in %.1f s: llvmpipe drew"
          " the triangle %s in %s, %d pixels, every one where it must be"
          % (os.path.getsize(image) / 1e6, took, CORNERS, COLOUR, inside))
    return 0


if __name__ == "__main__":
    sys.exit(main())
