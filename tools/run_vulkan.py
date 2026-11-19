#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Vulkan on Kosmos (`docs/gpu3d.md` G2).

`vulkantest.elf` - `lavapipe`, Mesa's Vulkan driver, on llvmpipe and LLVM's
ORC JIT, with no loader, in one image with `user/kits/vulkantest/` - run
from a /Home of its own and held to the picture it must draw: a triangle
whose corners and colour this script chose, drawn by Vulkan into an image
in memory and read back as a mask; then a cube, turned and placed and seen
through a lens this script chose, each face an 8 by 8 checkerboard read
from a texture, with a depth buffer. Which pixels had to be which is
worked out here - the triangle's from its edges, the cube's by casting a
ray through each pixel at the same cube - and not by anything in the
guest.

Usage: run_vulkan.py IMAGE [APPS]
"""

import math
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

# The cube - turned by PITCH about x and then YAW about y, DISTANCE in
# front of the eye, through a lens of FOCAL - and its checks' two colours.
CUBE = 64
YAW, PITCH, DISTANCE, FOCAL = 0.6, 0.45, 4.0, 1.8
CHECKS = 8
COLOURS = ((240, 200, 40), (30, 90, 200))


def arguments(values):
    return ", ".join(repr(v) for v in values)


PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Vulkan's test program (`tools/run_vulkan.py`).
-- kosmos: image vulkantest.elf
local kit = use("vulkantest.elf")
print("vulkantest triangle: " .. kit.triangle(%s))
print("vulkantest cube: " .. kit.cube(%s))
""" % (arguments([c for p in CORNERS for c in p] + list(COLOUR)),
       arguments([YAW, PITCH, DISTANCE, FOCAL] + list(COLOURS[0]) + list(COLOURS[1])))

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


def compare(got, want, size):
    """None if every pixel of `got` is one `want` allows, or what was
    wrong. `want` holds, a pixel at a time, the letters it may be."""
    if len(got) != size or any(len(r) != size for r in got):
        return "a picture of %d rows, not %d by %d" % (len(got), size, size)
    wrong = []
    for y in range(size):
        for x in range(size):
            g, w = got[y][x], want[y][x]
            if g not in w:
                wrong.append("(%d,%d) %s for %s" % (x, y, g, "/".join(sorted(w))))
    if wrong:
        return "%d pixels wrong: %s" % (len(wrong), ", ".join(wrong[:8]))
    return None


def picture(said):
    m = re.match(r"vulkan device=llvmpipe/1\.(\d+) picture=([#ab.?/]+)$", said)
    return m.group(2).split("/") if m else None


def judge(said):
    """None if the triangle is right, or what was wrong."""
    got = picture(said)
    if got is None:
        return "not a picture from llvmpipe"
    allowed = {"#": {"#"}, ".": {"."}, "*": {"#", "."}}
    return compare(got, [[allowed[c] for c in row] for row in expected()], SIZE)


# ------------------------------------------------------------------ the cube

def rotation():
    """Ry(YAW) * Rx(PITCH), by rows - what `cube_transform` turns by."""
    cy, sy = math.cos(YAW), math.sin(YAW)
    cp, sp = math.cos(PITCH), math.sin(PITCH)
    return [[cy, sy * sp, sy * cp], [0.0, cp, -sp], [-sy, cy * sp, cy * cp]]


def cube_at(px, py, r):
    """The letter a ray through the point (px, py) of the picture meets: the
    checkerboard of the face it enters the cube by, or "." if it misses."""
    d_eye = [((px / CUBE) * 2 - 1) / FOCAL, ((py / CUBE) * 2 - 1) / FOCAL, 1.0]
    # Into the cube's own space: the eye is at R^T (0, 0, -DISTANCE).
    o = [-DISTANCE * r[2][i] for i in range(3)]
    d = [sum(r[k][i] * d_eye[k] for k in range(3)) for i in range(3)]
    near, far, entry = -math.inf, math.inf, None
    for axis in range(3):
        if abs(d[axis]) < 1e-12:
            if abs(o[axis]) > 1:
                return "."
            continue
        t1, t2 = (-1 - o[axis]) / d[axis], (1 - o[axis]) / d[axis]
        if min(t1, t2) > near:
            near, entry = min(t1, t2), axis
        far = min(far, max(t1, t2))
    if entry is None or near > far or near <= 0:
        return "."
    hit = [o[i] + near * d[i] for i in range(3)]
    # The face's texture runs along the next axis round (u) and the one
    # after (v), -1 to 1 as 0 to 1: `cube_mesh`'s rule.
    u = (hit[(entry + 1) % 3] + 1) / 2
    v = (hit[(entry + 2) % 3] + 1) / 2
    i = min(CHECKS - 1, max(0, int(math.floor(u * CHECKS))))
    j = min(CHECKS - 1, max(0, int(math.floor(v * CHECKS))))
    return "a" if (i + j) % 2 == 0 else "b"


# A pixel is held to what its centre sees when the centre and four points
# 0.3 of a pixel from it all see the same; otherwise to any of what they
# see - an edge, of the cube or of a check, is the rasterizer's and the
# sampler's to round. Never `?`, which is neither colour nor the clear one.
SPREAD = 0.3


def cube_expected():
    r = rotation()
    rows = []
    for y in range(CUBE):
        row = []
        for x in range(CUBE):
            cx, cy = x + 0.5, y + 0.5
            seen = {cube_at(cx + dx, cy + dy, r)
                    for dx, dy in ((0, 0), (SPREAD, 0), (-SPREAD, 0), (0, SPREAD), (0, -SPREAD))}
            row.append(seen)
        rows.append(row)
    return rows


def cube_judge(said):
    got = picture(said)
    if got is None:
        return "not a picture from llvmpipe"
    return compare(got, cube_expected(), CUBE)


def cube_drawn():
    return ["".join(next(iter(w)) if len(w) == 1 else "*" for w in row)
            for row in cube_expected()]


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
    said = {}

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/vulkan/vulkantest.lua")

        while time.monotonic() < started + 240 and len(said) < 2:
            for line in guest.seen[mark:].split("\n")[1:-1]:
                for name in ("triangle", "cube"):
                    prefix = "vulkantest %s: " % name
                    if line.strip().startswith(prefix):
                        said[name] = line.strip()[len(prefix):]
            if len(said) < 2:
                time.sleep(0.2)
                guest._read_available()
        took = time.monotonic() - started
    finally:
        guest.close()

    if os.environ.get("KOSMOS_VULKAN_SERIAL"):
        with open(os.environ["KOSMOS_VULKAN_SERIAL"], "w") as f:
            f.write(guest.seen)

    failed = False
    for name, judged, want in (("triangle", judge, expected), ("cube", cube_judge, cube_drawn)):
        wrong = judged(said[name]) if name in said else "nothing said"
        if wrong:
            failed = True
            print("FAIL: Vulkan on Kosmos did not draw the %s: %s" % (name, wrong))
            print("  wanted:\n    " + "\n    ".join(want()))
            print("  said:   %s" % said.get(name))
    if failed:
        if len(said) < 2:
            print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        return 1

    def count(name, letters):
        rows = picture(said[name])
        return sum(row.count(c) for row in rows for c in letters)

    print("PASS: Vulkan on Kosmos, from a %.1f MB image, in %.1f s: llvmpipe drew"
          " the triangle %s in %s, %d pixels, and the cube turned %.2f by %.2f at %.1f"
          " through %.1f, %d pixels of its checks (%d and %d), every one where it must be"
          % (os.path.getsize(image) / 1e6, took, CORNERS, COLOUR, count("triangle", "#"),
             YAW, PITCH, DISTANCE, FOCAL, count("cube", "ab"), count("cube", "a"),
             count("cube", "b")))
    return 0

if __name__ == "__main__":
    sys.exit(main())
