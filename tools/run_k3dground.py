#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The ground on Vulkan (`docs/flight.md` F4b).

`k3d_terrain.c`'s chunks for an eye 2500 m over Montevideo, each one's mesh
over a tilted plane and its imagery, drawn by `k3d_vk.c` in `vulkantest.elf`
with `lavapipe` - and every pixel held to what a ray through its centre
says: where it meets the plane, which of the chunks the guest reported
holds that place, what that chunk's imagery is there between its texels
(the sampler's bilinear rule, clamped at its edge), lit by the sun and
hazed by distance; or the sky's colour where the ray leaves the ground the
chunks cover. The imagery is a colour this script can work out at any
place - red east, green south, blue the chunk's zoom and a check between
neighbouring texels, which nearest sampling would show - so a chunk drawn
with another's imagery, at the wrong place on it, or a chunk missing, shows.

A pixel whose centre is within a third of a pixel of where the chunk or
the sky changes is left out: that is the rasterizer's tie rule.

Usage: run_k3dground.py IMAGE [APPS]
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
from run_k3dvk import lua_table, sub, dot, cross, unit       # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
X86 = "x86_64" in IMAGE
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    ROOT, "build", "user-x86_64-test" if X86 else "user-test", "apps")
WORK = scratch.directory("k3dground")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")

SIZE = 96
TERRAIN = dict(lon=-56.1990, lat=-34.9064, root=11, deepest=15, radius=2, split=2.0,
               most=400, h0=50.0, gx=0.01, gz=-0.005, texels=16,
               red=100.0 / 30000.0, green=100.0 / 30000.0)
SCENE = dict(
    eye=(0.0, 2500.0, 0.0), target=(0.0, 0.0, -25000.0), focal=1.0, near=1.0,
    far=100000.0, sun=(0.3, 1.0, 0.2), ambient=0.35,
    zenith=(0.2, 0.4, 0.8), horizon=(0.8, 0.85, 0.9),
    haze=(0.7, 0.75, 0.8), haze_start=3000.0, haze_end=30000.0,
    boxes=[],
)

# The sampler's weights and an 8-bit store either side of the shader.
TOLERANCE = 3

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The ground on Vulkan's test program (`tools/run_k3dground.py`).
-- kosmos: image vulkantest.elf
local kit = use("vulkantest.elf")
local function tell(name, s)
  local n = (#s + 99) // 100
  for k = 1, n do
    print(("vulkantest %%s %%d/%%d: %%s"):format(name, k, n, s:sub(100 * k - 99, 100 * k)))
  end
end
local said, bytes = kit.terrain(%s)
tell("terrain", said)
tell("pixels", (bytes:gsub(".", function(c) return ("%%02x"):format(c:byte()) end)))
""" % lua_table(dict(SCENE, size=SIZE, terrain=None)).replace(
    "terrain = None", "terrain = " + lua_table(TERRAIN))

EQUATOR = 40075016.68557849
LAT_MOST = 85.05112878


# ------------------------------------------------------------ the frame

def project(lon, lat):
    """`mercator.c`'s map_project."""
    r = max(-LAT_MOST, min(LAT_MOST, lat)) * math.pi / 180.0
    return (lon + 180.0) / 360.0, (1.0 - math.log(math.tan(r) + 1.0 / math.cos(r)) / math.pi) / 2.0


OX, OY = project(TERRAIN["lon"], TERRAIN["lat"])
METRES = EQUATOR * math.cos(max(-LAT_MOST, min(LAT_MOST, TERRAIN["lat"])) * math.pi / 180.0)


def world(mx, my):
    return (mx - OX) * METRES, (my - OY) * METRES


def channel(c):
    return max(0, min(255, math.floor(c + 0.5)))


def texel(z, tx, ty, i, j):
    """A chunk's texel, as `imagery_of` makes it."""
    t = TERRAIN["texels"]
    n = 2.0 ** z
    x, zz = world((tx + (i + 0.5) / t) / n, (ty + (j + 0.5) / t) / n)
    return (channel(128.0 + TERRAIN["red"] * x), channel(128.0 + TERRAIN["green"] * zz),
            channel(40.0 + 30.0 * (z - TERRAIN["root"]) + 20.0 * ((i + j) & 1)))


def sample(z, tx, ty, u, v):
    """The imagery at (u, v) across the chunk: linear between texel
    centres, the edge's texel beyond the outer ones - 0 to 1 a channel."""
    t = TERRAIN["texels"]
    s, r = u * t - 0.5, v * t - 0.5
    i0, j0 = math.floor(s), math.floor(r)
    fs, fr = s - i0, r - j0

    def at(i, j):
        return texel(z, tx, ty, max(0, min(t - 1, i)), max(0, min(t - 1, j)))

    a, b, c, d = at(i0, j0), at(i0 + 1, j0), at(i0, j0 + 1), at(i0 + 1, j0 + 1)
    return [((a[k] * (1 - fs) + b[k] * fs) * (1 - fr) + (c[k] * (1 - fs) + d[k] * fs) * fr)
            / 255.0 for k in range(3)]


# ------------------------------------------------------------ the reference

def frame():
    f = unit(sub(SCENE["target"], SCENE["eye"]))
    r = unit(cross(f, [0.0, 1.0, 0.0]))
    return f, r, cross(r, f)


def ray(px, py):
    f, r, u = frame()
    x = (px / SIZE) * 2.0 - 1.0
    y = (py / SIZE) * 2.0 - 1.0
    focal = SCENE["focal"]
    return [f[i] + r[i] * x / focal - u[i] * y / focal for i in range(3)]


def meets(px, py, chunks):
    """What the ray through (px, py) meets: (chunk, x, z, t) on the ground,
    or None for the sky."""
    e, d = SCENE["eye"], ray(px, py)
    f, _, _ = frame()
    h0, gx, gz = TERRAIN["h0"], TERRAIN["gx"], TERRAIN["gz"]
    below = d[1] - gx * d[0] - gz * d[2]
    if below >= 0:
        return None
    t = (h0 + gx * e[0] + gz * e[2] - e[1]) / below
    ahead = t * dot(d, f)
    if t <= 0 or ahead <= SCENE["near"] or ahead >= SCENE["far"]:
        return None
    x, z = e[0] + d[0] * t, e[2] + d[2] * t
    mx, my = OX + x / METRES, OY + z / METRES
    for zoom in range(TERRAIN["root"], TERRAIN["deepest"] + 1):
        n = 2 ** zoom
        key = (zoom, math.floor(mx * n), math.floor(my * n))
        if key in chunks:
            return key, mx, my, x, z, t
    return None


def shade(px, py, chunks):
    hit = meets(px, py, chunks)
    if hit is None:
        look = unit(ray(px, py))
        t = min(max(look[1], 0.0), 1.0)
        return [SCENE["horizon"][i] + (SCENE["zenith"][i] - SCENE["horizon"][i]) * t
                for i in range(3)], None
    (z, tx, ty), mx, my, x, zz, t = hit
    n = 2.0 ** z
    albedo = sample(z, tx, ty, mx * n - tx, my * n - ty)
    normal = unit([-TERRAIN["gx"], 1.0, -TERRAIN["gz"]])
    lit = SCENE["ambient"] + (1 - SCENE["ambient"]) * max(dot(normal, unit(SCENE["sun"])), 0.0)
    d = ray(px, py)
    far = math.sqrt(dot(d, d)) * t
    hz = min(max((far - SCENE["haze_start"]) / (SCENE["haze_end"] - SCENE["haze_start"]), 0.0),
             1.0)
    return [albedo[i] * lit * (1 - hz) + SCENE["haze"][i] * hz for i in range(3)], (z, tx, ty)


def reference(chunks):
    want = []
    for py in range(SIZE):
        row = []
        for px in range(SIZE):
            colour, what = shade(px + 0.5, py + 0.5, chunks)
            edge = any(shade(px + 0.5 + dx, py + 0.5 + dy, chunks)[1] != what
                       for dx, dy in ((-0.34, 0), (0.34, 0), (0, -0.34), (0, 0.34)))
            row.append(None if edge else
                       (tuple(round(min(max(c, 0.0), 1.0) * 255) for c in colour), what))
        want.append(row)
    return want


# ------------------------------------------------------------------ the run

def gather(seen, names):
    import run_vulkan as V                                   # noqa: E402
    return V.gather(seen, names)


def main():
    built = os.path.join(APPS, "vulkantest.elf")

    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` builds it" % built)
        return 1

    image = os.path.join(WORK, "vulkantest.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)
    with open(os.path.join(WORK, "k3dground.lua"), "w") as f:
        f.write(PROGRAM)

    megabytes = os.path.getsize(image) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)]
                   + ["%s:/Home/apps/vulkan/%s" % (os.path.join(WORK, n), n)
                      for n in ("k3dground.lua", "vulkantest.elf")],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    guest = R.Guest(IMAGE, 180)
    said = {}
    mark = 0

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/vulkan/k3dground.lua")
        while time.monotonic() < started + 300 and len(said) < 2:
            said.update(gather(guest.seen[mark:], ("terrain", "pixels")))
            if len(said) < 2:
                time.sleep(0.2)
                guest._read_available()
        took = time.monotonic() - started
    finally:
        guest.close()

    m = re.match(r"vulkan device=llvmpipe/1\.\d+ terrain=(\d+)x(\d+) chunks=(\d+) tiles=([\d/,]*)$",
                 said.get("terrain", ""))
    pixels = said.get("pixels", "")
    if not m or len(pixels) != SIZE * SIZE * 6:
        print("FAIL: the ground on Vulkan drew nothing: %r, %d hex digits of %d"
              % (said.get("terrain"), len(pixels), SIZE * SIZE * 6))
        print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        return 1
    chunks = set(tuple(int(n) for n in s.split("/")) for s in m.group(4).split(","))
    if len(chunks) != int(m.group(3)):
        print("FAIL: %s chunks said, %d listed" % (m.group(3), len(chunks)))
        return 1

    got = bytes.fromhex(pixels)
    want = reference(chunks)
    wrong, judged, sky, zooms, used = [], 0, 0, {}, set()
    for py in range(SIZE):
        for px in range(SIZE):
            w = want[py][px]
            if w is None:
                continue
            judged += 1
            rgb = tuple(got[(py * SIZE + px) * 3: (py * SIZE + px) * 3 + 3])
            if w[1] is None:
                sky += 1
            else:
                zooms[w[1][0]] = zooms.get(w[1][0], 0) + 1
                used.add(w[1])
            if any(abs(rgb[i] - w[0][i]) > TOLERANCE for i in range(3)):
                wrong.append("(%d,%d) %s for %s" % (px, py, rgb, w[0]))

    # The picture must hold the sky, the ground at three zooms at least, and
    # many chunks, or it proves little.
    thin = []
    if sky < 20:
        thin.append("the sky (%d)" % sky)
    if len([z for z, n in zooms.items() if n >= 20]) < 3:
        thin.append("three zooms of 20 pixels (%s)" % zooms)
    if len(used) < 10:
        thin.append("ten chunks (%d)" % len(used))
    if wrong or thin:
        print("FAIL: the ground on Vulkan drawn wrong: %d of %d pixels%s: %s"
              % (len(wrong), judged, (", and too little of " + ", ".join(thin)) if thin else "",
                 ", ".join(wrong[:8])))
        return 1

    print("PASS: the ground on Vulkan, in %.1f s: %s chunks from zoom %d to %d over a tilted "
          "plane, each with its imagery, %d pixels each within %d of a ray cast through its "
          "centre (sky %d; ground by zoom %s from %d chunks; %d on an edge left out)"
          % (took, m.group(3), TERRAIN["root"], TERRAIN["deepest"], judged, TOLERANCE, sky,
             ", ".join("%d: %d" % (z, zooms[z]) for z in sorted(zooms)), len(used),
             SIZE * SIZE - judged))
    return 0


if __name__ == "__main__":
    sys.exit(main())
