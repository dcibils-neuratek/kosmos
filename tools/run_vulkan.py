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

**And the cube in a window** (G3): the same cube, through
VK_KOSMOS_surface and a swapchain, presented into a Kosmos window on the
desktop for `FRAMES` frames, turning to the angle the offscreen one was
drawn at - and then found in what QEMU scans out, every pixel of the
window held to the same reference. That is the swapchain, the WSI's copy
into the window's buffer, the byte order (B8G8R8A8 into a word of
0xAARRGGBB) and the compositor, with nothing in the guest saying where the
window is: the cube is looked for in the whole screen.
`KOSMOS_VULKAN_SHOT` keeps that screen as a PNG.

**And compute** (`docs/xavier.md` X12 V1): the Vulkan Kit's compute
pipelines, push descriptors, specialisation and push constants and the
barrier between two dispatches, run over words this script can make too
- twice, at two sizes and two workgroups, one of them not a whole number
of groups - and every word of the second pass held here, by its hash.
And the device must have what llama.cpp's shaders are written against.

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
-- Said in pieces of 100 characters, each a line of its own and numbered:
-- a line of 1200 goes to the console in several writes, and another
-- process's line can land between them (18.929's gate, an e1000 line in
-- the middle of the triangle's last row).
local function tell(name, s)
  local n = (#s + 99) // 100
  for k = 1, n do
    print(("vulkantest %%s %%d/%%d: %%s"):format(name, k, n, s:sub(100 * k - 99, 100 * k)))
  end
end
tell("triangle", kit.triangle(%s))
tell("cube", kit.cube(%s))
tell("compute", kit.compute(%s))
tell("compute2", kit.compute(%s))
""" % (arguments([c for p in CORNERS for c in p] + list(COLOUR)),
       arguments([YAW, PITCH, DISTANCE, FOCAL] + list(COLOURS[0]) + list(COLOURS[1])),
       "%d, %d, %d, %d" % (100003, 31, 7, 64), "%d, %d, %d, %d" % (4096, 2654435769, 0, 256))

# What the compute check runs (n, mul, add, lanes a workgroup), in the
# order the program asks.
COMPUTES = (("compute", (100003, 31, 7, 64)), ("compute2", (4096, 2654435769, 0, 256)))
# What llama.cpp's shaders cannot do without: blocks of bytes and half
# words read from storage, and small integers. Half-float arithmetic
# (`f16`) is said but not asked for - lavapipe has it only where the
# processor has F16C, and the shaders do without it (FLOAT16 unset).
TENSORS = ("s16", "s8", "i8", "i16")


def compute_words(n, mul, add):
    """`compute.comp`'s two passes, worked out here (`vulkantest.c`'s
    `word_at` and its bytes and half words)."""
    m = 0xffffffff
    word = [(i * 2654435761 + 12345) & m for i in range(max(n, 512))]
    byte = [w >> 24 for w in word[:256]]
    half = [(w >> 16) & 0xffff for w in word[256:512]]
    first = [(word[i] * mul + add + byte[i % 256] + (half[i % 256] << 8)) & m for i in range(n)]
    return [first[(i * 7 + 3) % n] ^ (first[i] >> 3) for i in range(n)]


def compute_judge(said, n, mul, add):
    """None if the device and every word are right, or what was wrong."""
    m = re.match(r"compute device=llvmpipe/1\.\d+ tensors=([a-z0-9,]*) ms=[-\d.]+"
                 r" words=(\d+) first=(\d+) last=(\d+) hash=(\d+)$", said)
    if not m:
        return "not an answer from llvmpipe: %s" % said
    missing = [t for t in TENSORS if t not in m.group(1).split(",")]
    if missing:
        return "the device lacks %s" % ", ".join(missing)
    out = compute_words(n, mul, add)
    h = 2166136261
    for w in out:
        for k in range(0, 32, 8):
            h = ((h ^ ((w >> k) & 0xff)) * 16777619) & 0xffffffff
    got = tuple(int(m.group(k)) for k in (2, 3, 4, 5))
    want = (n, out[0], out[-1], h)
    return None if got == want else "words, first, last, hash %s, wanted %s" % (got, want)

# The window: FRAMES frames, STEP further in yaw each, the last at YAW.
FRAMES, STEP = 30, 0.05

WINDOW = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Vulkan's window (`tools/run_vulkan.py`, `docs/gpu3d.md` G3).
-- kosmos: application
-- kosmos: image vulkantest.elf
local kit = use("vulkantest.elf")
-- Said in pieces of 100 characters, each a line of its own and numbered:
-- a line of 1200 goes to the console in several writes, and another
-- process's line can land between them (18.929's gate, an e1000 line in
-- the middle of the triangle's last row).
local function tell(name, s)
  local n = (#s + 99) // 100
  for k = 1, n do
    print(("vulkantest %%s %%d/%%d: %%s"):format(name, k, n, s:sub(100 * k - 99, 100 * k)))
  end
end
tell("window", kit.window(%s))
kit.hold(90)
""" % arguments([YAW, PITCH, DISTANCE, FOCAL] + list(COLOURS[0]) + list(COLOURS[1])
                + [FRAMES, STEP])

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


def gather(seen, names):
    """What each of `names` said, put together from its numbered pieces -
    only once every piece has arrived whole, each on a line of its own."""
    pieces = {}
    for line in seen.split("\n")[1:-1]:
        m = re.search(r"vulkantest (\w+) (\d+)/(\d+): (.*)$", line.rstrip("\r"))
        if m and m.group(1) in names:
            pieces.setdefault(m.group(1), {})[int(m.group(2))] = (int(m.group(3)), m.group(4))
    whole = {}
    for name, got in pieces.items():
        n = next(iter(got.values()))[0]
        if all(k in got and got[k][0] == n for k in range(1, n + 1)):
            whole[name] = "".join(got[k][1] for k in range(1, n + 1)).strip()
    return whole


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


# ---------------------------------------------------------------- the window

def letter(rgb):
    """What a pixel of the screen is, in the picture's letters."""
    if rgb == COLOURS[0]:
        return "a"
    if rgb == COLOURS[1]:
        return "b"
    if rgb == (0, 0, 0):
        return "."
    return "?"


# **The window's rounded bottom corners.** wm cuts a window's corners on a
# radius of its look's, and the desktop shows through there - where the
# reference has the cleared black. Ten pixels at the time of writing; a
# pixel outside a circle of `ROUNDED` in a bottom corner may be the
# desktop's, and nowhere else may.
ROUNDED = 12


def corner(x, y):
    """Whether (x, y) of the square lies outside its bottom corners' rounding."""
    cy = CUBE - ROUNDED
    if y < cy:
        return False
    cx = ROUNDED if x < ROUNDED else CUBE - 1 - ROUNDED if x > CUBE - 1 - ROUNDED else None
    return cx is not None and (x - cx) ** 2 + (y - cy) ** 2 > (ROUNDED - 1) ** 2


def find_cube(width, height, at):
    """Where on the screen the window's cube is, as (x, y) of its top left,
    or None: the place every pixel of the CUBE square is one the reference
    allows. The cube's own colours are rare on a desktop, so the places
    tried are those that put the reference's first certain check pixel on
    one of them."""
    want = cube_expected()
    first = next((x, y, next(iter(want[y][x]))) for y in range(CUBE) for x in range(CUBE)
                 if len(want[y][x]) == 1 and next(iter(want[y][x])) in "ab")
    fx, fy, fc = first
    for sy in range(height):
        for sx in range(width):
            if letter(at(sx, sy)) != fc:
                continue
            ox, oy = sx - fx, sy - fy
            if ox < 0 or oy < 0 or ox + CUBE > width or oy + CUBE > height:
                continue
            if all(letter(at(ox + x, oy + y)) in want[y][x]
                   or (want[y][x] == {"."} and corner(x, y)
                       and letter(at(ox + x, oy + y)) == "?")
                   for y in range(CUBE) for x in range(CUBE)):
                return ox, oy
    return None


def window_wrong(width, height, at):
    """None if the cube is on the screen whole, or the nearest it came."""
    if find_cube(width, height, at) is not None:
        return None
    seen = sum(1 for y in range(height) for x in range(width) if letter(at(x, y)) in "ab")
    return "no %d by %d square on the screen is the cube (%d pixels of its colours seen)" % (
        CUBE, CUBE, seen)


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
    with open(os.path.join(WORK, "vkwindow.lua"), "w") as f:
        f.write(WINDOW)

    megabytes = os.path.getsize(image) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)]
                   + ["%s:/Home/apps/vulkan/%s" % (os.path.join(WORK, n), n)
                      for n in ("vulkantest.lua", "vkwindow.lua", "vulkantest.elf")],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    guest = R.Guest(IMAGE, 180)
    said = {}
    screen = None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/vulkan/vulkantest.lua")

        names = ("triangle", "cube") + tuple(n for n, _ in COMPUTES)
        while time.monotonic() < started + 240 and len(said) < len(names):
            said.update(gather(guest.seen[mark:], names))
            if len(said) < len(names):
                time.sleep(0.2)
                guest._read_available()
        took = time.monotonic() - started

        # The window, on a desktop started for it.
        if len(said) == len(names):
            mark = len(guest.seen)
            guest.type("wm /Home/apps/vulkan/vkwindow.lua")
            until = time.monotonic() + 180
            while time.monotonic() < until and "window" not in said:
                said.update(gather(guest.seen[mark:], ("window",)))
                if "window" not in said:
                    time.sleep(0.2)
                    guest._read_available()
            if "window" in said and " frames=" in said["window"]:
                time.sleep(1.0)                 # the compositor's next pass
                screen = guest.screendump()
                if os.environ.get("KOSMOS_VULKAN_SHOT"):
                    R.write_png(os.environ["KOSMOS_VULKAN_SHOT"], screen)
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
    for name, (n, mul, add, _) in COMPUTES:
        wrong = compute_judge(said[name], n, mul, add) if name in said else "nothing said"
        if wrong:
            failed = True
            print("FAIL: Vulkan on Kosmos, compute (%s): %s" % (name, wrong))
    if not failed:
        m = re.match(r"vulkan device=llvmpipe/1\.\d+ extent=(\d+)x(\d+) images=(\d+)"
                     r" frames=(\d+) ms=(\d+)$", said.get("window", ""))
        wrong = None if m else "the window did not draw: %s" % said.get("window", "nothing said")
        if m and screen is not None:
            width, height, at = R.pixel_reader(screen)
            wrong = window_wrong(width, height, at)
        if wrong:
            failed = True
            print("FAIL: Vulkan on Kosmos, the cube in a window: %s" % wrong)
            print("  said: %s" % said.get("window"))
    if failed:
        if len(said) < 2 + len(COMPUTES):
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
    m = re.search(r"images=(\d+) frames=(\d+) ms=(\d+)", said["window"])
    print("PASS: and in a window, through VK_KOSMOS_surface: %s frames presented from a"
          " swapchain of %s images in %.1f s, the last one on the screen, every pixel"
          " where it must be" % (m.group(2), m.group(1), int(m.group(3)) / 1000.0))
    print("PASS: and compute, %s: two dispatches with a barrier between, over %s,"
          " every word the second pass wrote the one worked out here; the device has %s"
          % (", ".join("%d words in groups of %d" % (c[0], c[3]) for _, c in COMPUTES),
             "push descriptors and specialisation and push constants",
             said["compute"].split("tensors=")[1].split(" ")[0].rstrip(",")))
    return 0

if __name__ == "__main__":
    sys.exit(main())
