#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The 3D Kit on Vulkan (`docs/flight.md` F3b).

`user/kits/3d/k3d_vk.c` - the renderer Flight's terrain is drawn by - in
`vulkantest.elf` with `lavapipe`, drawing a scene this script chose: a
ground and two boxes, one near and one far, under a sun, a sky of two
colours and a haze, through a lens. The picture comes back as bytes and
every pixel is held to what a ray cast through its centre at the same
scene says it must be: which box it meets first (the depth buffer), how
that face is lit (the sun and the ambient share), how much haze its
distance gives it, and the sky's colour where it meets nothing. Nothing in
the guest knows the answer.

A pixel whose centre is within a third of a pixel of a silhouette is left
out: which side wins there is the rasterizer's tie rule, not the renderer.
Every other pixel must be within TOLERANCE of each channel.

**And in a window** (F3c): the same scene at WINDOW pixels a side, drawn
through the swapchain into a Kosmos window for FRAMES frames with the eye
going round the target, the last at the scene's own eye - found on the
screen QEMU scans out and held to the same reference, pixel by pixel. That
is the renderer's pictures arriving whole through VK_KOSMOS_surface, the
window's buffer and the compositor, in the swapchain's format rather than
the one this script asked for.

Usage: run_k3dvk.py IMAGE [APPS]
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
WORK = scratch.directory("k3dvk")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")

# The scene, chosen here. A box is (x0, y0, z0, x1, y1, z1, r, g, b).
SIZE = 64
SCENE = dict(
    eye=(0.0, 3.0, 8.0), target=(0.0, 1.0, 0.0), focal=1.2, near=0.1, far=200.0,
    sun=(0.4, 1.0, 0.3), ambient=0.3,
    zenith=(0.2, 0.4, 0.8), horizon=(0.8, 0.85, 0.9),
    haze=(0.7, 0.75, 0.8), haze_start=6.0, haze_end=40.0,
    boxes=[
        (-60.0, -1.0, -60.0, 60.0, 0.0, 60.0, 70, 140, 60),      # the ground
        (-1.0, 0.0, -1.0, 1.0, 2.0, 1.0, 220, 60, 40),           # near, red
        (3.0, 0.0, -14.0, 6.0, 4.0, -11.0, 40, 80, 220),         # far, blue
    ],
)

# The window's side, and its frames: STEP radians round the target each.
WINDOW = 96
FRAMES, STEP = 30, 0.04

# llvmpipe's float arithmetic and an 8-bit store either side of the
# shader's: two in 255 a channel is rounding, more is the renderer.
TOLERANCE = 3


def lua_value(v):
    if isinstance(v, (tuple, list)):
        return "{" + ", ".join(lua_value(x) for x in v) + "}"
    return repr(v)


def lua_table(d):
    return "{" + ", ".join("%s = %s" % (k, lua_value(v)) for k, v in d.items()) + "}"


PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The 3D Kit on Vulkan's test program (`tools/run_k3dvk.py`).
-- kosmos: image vulkantest.elf
local kit = use("vulkantest.elf")
-- Said in numbered pieces of 100 characters, each a line of its own, as
-- `run_vulkan.py`'s are, so another process's line between them is seen.
local function tell(name, s)
  local n = (#s + 99) // 100
  for k = 1, n do
    print(("vulkantest %%s %%d/%%d: %%s"):format(name, k, n, s:sub(100 * k - 99, 100 * k)))
  end
end
local said, bytes = kit.scene(%s)
tell("scene", said)
tell("pixels", (bytes:gsub(".", function(c) return ("%%02x"):format(c:byte()) end)))
""" % lua_table(dict(SCENE, size=SIZE))

WINDOW_PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The 3D Kit on Vulkan in a window (`tools/run_k3dvk.py`, F3c).
-- kosmos: application
-- kosmos: image vulkantest.elf
local kit = use("vulkantest.elf")
local function tell(name, s)
  local n = (#s + 99) // 100
  for k = 1, n do
    print(("vulkantest %%s %%d/%%d: %%s"):format(name, k, n, s:sub(100 * k - 99, 100 * k)))
  end
end
tell("window", kit.scene_window(%s, %d, %r))
kit.hold(90)
""" % (lua_table(dict(SCENE, size=WINDOW)), FRAMES, STEP)



# ------------------------------------------------------------ the reference

def sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def dot(a, b):
    return sum(a[i] * b[i] for i in range(3))


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def unit(a):
    n = math.sqrt(dot(a, a))
    return [x / n for x in a]


def frame():
    f = unit(sub(SCENE["target"], SCENE["eye"]))
    r = unit(cross(f, [0.0, 1.0, 0.0]))
    return f, r, cross(r, f)


def ray(px, py, size):
    """The direction through (px, py) of the picture, `k3d_vk.h`'s rule."""
    f, r, u = frame()
    x = (px / size) * 2.0 - 1.0
    y = (py / size) * 2.0 - 1.0
    focal = SCENE["focal"]
    return [f[i] + r[i] * x / focal - u[i] * y / focal for i in range(3)]


def hit(origin, d):
    """The first box the ray meets past the near plane: (t, box, normal)."""
    f, _, _ = frame()
    best = None
    for b, box in enumerate(SCENE["boxes"]):
        lo, hi = box[0:3], box[3:6]
        t0, t1, n0 = -math.inf, math.inf, None
        for a in range(3):
            if abs(d[a]) < 1e-12:
                if origin[a] < lo[a] or origin[a] > hi[a]:
                    t0 = math.inf
                continue
            ta, tb = (lo[a] - origin[a]) / d[a], (hi[a] - origin[a]) / d[a]
            sign = -1.0
            if ta > tb:
                ta, tb, sign = tb, ta, 1.0
            if ta > t0:
                t0, n0 = ta, [sign if k == a else 0.0 for k in range(3)]
            t1 = min(t1, tb)
        # The near plane is a distance along forward, not along the ray.
        if t0 <= t1 and n0 is not None and t0 * dot(d, f) > SCENE["near"]:
            if best is None or t0 < best[0]:
                best = (t0, b, n0)
    return best


def shade(px, py, size):
    """The colour a pixel's centre must be, 0 to 1 a channel, and what it
    met (a box's number, or -1 for the sky)."""
    eye = SCENE["eye"]
    d = ray(px, py, size)
    h = hit(eye, d)
    if h is None:
        look = unit(d)
        t = min(max(look[1], 0.0), 1.0)
        return [SCENE["horizon"][i] + (SCENE["zenith"][i] - SCENE["horizon"][i]) * t
                for i in range(3)], -1
    t, b, n = h
    box = SCENE["boxes"][b]
    sun = unit(SCENE["sun"])
    lit = SCENE["ambient"] + (1.0 - SCENE["ambient"]) * max(dot(n, sun), 0.0)
    where = [eye[i] + d[i] * t for i in range(3)]
    far = math.sqrt(dot(sub(where, eye), sub(where, eye)))
    hz = min(max((far - SCENE["haze_start"]) / (SCENE["haze_end"] - SCENE["haze_start"]), 0.0),
             1.0)
    return [(box[6 + i] / 255.0) * lit * (1 - hz) + SCENE["haze"][i] * hz for i in range(3)], b


def reference(size):
    """Per pixel: the bytes it must be, or None where a silhouette passes
    within a third of a pixel of its centre."""
    want = []
    for py in range(size):
        row = []
        for px in range(size):
            colour, what = shade(px + 0.5, py + 0.5, size)
            near_edge = any(shade(px + 0.5 + dx, py + 0.5 + dy, size)[1] != what
                            for dx, dy in ((-0.34, 0), (0.34, 0), (0, -0.34), (0, 0.34)))
            row.append(None if near_edge else
                       (tuple(round(min(max(c, 0.0), 1.0) * 255) for c in colour), what))
        want.append(row)
    return want


# --------------------------------------------------------------- the window

# **The window's rounded bottom corners**, as `run_vulkan.py` has them: wm
# cuts them on a radius of its look's and the desktop shows through, so a
# pixel outside a circle of ROUNDED in a bottom corner is not judged.
ROUNDED = 12


def corner(x, y, size):
    cy = size - ROUNDED
    if y < cy:
        return False
    cx = ROUNDED if x < ROUNDED else size - 1 - ROUNDED if x > size - 1 - ROUNDED else None
    return cx is not None and (x - cx) ** 2 + (y - cy) ** 2 > (ROUNDED - 1) ** 2


def close(a, b):
    return all(abs(a[i] - b[i]) <= TOLERANCE for i in range(3))


def find_window(width, height, at, want, size):
    """Where on the screen the scene's square is, as (x, y) of its top left,
    or None, and how many of its judged pixels the best place had right.
    The places tried put the reference's first judged pixel of the near box
    on a pixel of its colour - red, rare on a desktop."""
    judged = [(x, y, want[y][x][0]) for y in range(size) for x in range(size)
              if want[y][x] is not None and not corner(x, y, size)]
    fx, fy, fc = next((x, y, c) for x, y, c in judged if want[y][x][1] == 1)
    best = 0
    for sy in range(fy, height - size + fy + 1):
        for sx in range(fx, width - size + fx + 1):
            if not close(at(sx, sy), fc):
                continue
            ox, oy = sx - fx, sy - fy
            right = 0
            for x, y, c in judged:
                if not close(at(ox + x, oy + y), c):
                    break
                right += 1
            if right == len(judged):
                return (ox, oy), right, len(judged)
            best = max(best, right)
    return None, best, len(judged)


# ------------------------------------------------------------------ the run

def gather(seen, names):
    """What each of `names` said, put together from its numbered pieces -
    `run_vulkan.py`'s rule, once every piece has arrived whole."""
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
    with open(os.path.join(WORK, "k3dvk.lua"), "w") as f:
        f.write(PROGRAM)
    with open(os.path.join(WORK, "k3dwin.lua"), "w") as f:
        f.write(WINDOW_PROGRAM)

    megabytes = os.path.getsize(image) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)]
                   + ["%s:/Home/apps/vulkan/%s" % (os.path.join(WORK, n), n)
                      for n in ("k3dvk.lua", "k3dwin.lua", "vulkantest.elf")],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    guest = R.Guest(IMAGE, 180)
    said = {}
    mark = 0
    screen = None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/vulkan/k3dvk.lua")
        while time.monotonic() < started + 240 and len(said) < 2:
            said.update(gather(guest.seen[mark:], ("scene", "pixels")))
            if len(said) < 2:
                time.sleep(0.2)
                guest._read_available()
        took = time.monotonic() - started

        # The window, on a desktop started for it.
        if len(said) == 2:
            mark = len(guest.seen)
            guest.type("wm /Home/apps/vulkan/k3dwin.lua")
            until = time.monotonic() + 180
            while time.monotonic() < until and "window" not in said:
                said.update(gather(guest.seen[mark:], ("window",)))
                if "window" not in said:
                    time.sleep(0.2)
                    guest._read_available()
            if " frames=" in said.get("window", ""):
                time.sleep(1.0)                 # the compositor's next pass
                screen = guest.screendump()
                if os.environ.get("KOSMOS_K3D_SHOT"):
                    R.write_png(os.environ["KOSMOS_K3D_SHOT"], screen)
    finally:
        guest.close()

    m = re.match(r"vulkan device=llvmpipe/1\.\d+ scene=(\d+)x(\d+) boxes=(\d+)$",
                 said.get("scene", ""))
    pixels = said.get("pixels", "")
    if not m or len(pixels) != SIZE * SIZE * 6:
        print("FAIL: the 3D Kit on Vulkan drew no scene: %r, %d hex digits of %d"
              % (said.get("scene"), len(pixels), SIZE * SIZE * 6))
        print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        return 1

    got = bytes.fromhex(pixels)
    want = reference(SIZE)
    wrong, judged, seen = [], 0, {}
    for py in range(SIZE):
        for px in range(SIZE):
            w = want[py][px]
            if w is None:
                continue
            judged += 1
            rgb = tuple(got[(py * SIZE + px) * 3: (py * SIZE + px) * 3 + 3])
            seen[w[1]] = seen.get(w[1], 0) + 1
            if any(abs(rgb[i] - w[0][i]) > TOLERANCE for i in range(3)):
                wrong.append("(%d,%d) %s for %s" % (px, py, rgb, w[0]))

    # Each thing must be in the picture, or the scene proves nothing.
    missing = [name for k, name in ((-1, "the sky"), (0, "the ground"), (1, "the near box"),
                                    (2, "the far box")) if seen.get(k, 0) < 20]
    if wrong or missing:
        print("FAIL: the 3D Kit on Vulkan drew the scene wrong: %d of %d pixels%s: %s"
              % (len(wrong), judged, (", and too little of " + ", ".join(missing))
                 if missing else "", ", ".join(wrong[:8])))
        return 1

    print("PASS: the 3D Kit on Vulkan, in %.1f s: a %dx%d scene of %s boxes under a sun, a "
          "sky and haze, %d pixels each within %d of a ray cast through its centre (sky %d, "
          "ground %d, near box %d, far box %d; %d on a silhouette left out)"
          % (took, SIZE, SIZE, m.group(3), judged, TOLERANCE, seen.get(-1, 0), seen.get(0, 0),
             seen.get(1, 0), seen.get(2, 0), SIZE * SIZE - judged))

    w = re.match(r"vulkan device=llvmpipe/1\.\d+ extent=(\d+)x(\d+) images=(\d+)"
                 r" scene=(\d+)x(\d+) boxes=(\d+) frames=(\d+) ms=(\d+)$",
                 said.get("window", ""))
    if not w or screen is None:
        print("FAIL: the 3D Kit on Vulkan did not draw in a window: %s"
              % said.get("window", "nothing said"))
        return 1
    width, height, at = R.pixel_reader(screen)
    place, right, count = find_window(width, height, at, reference(WINDOW), WINDOW)
    if place is None:
        print("FAIL: no %d by %d square on the screen is the scene: the nearest had %d of its "
              "%d judged pixels right" % (WINDOW, WINDOW, right, count))
        return 1
    frames, ms = int(w.group(7)), int(w.group(8))
    print("PASS: and in a window, through the swapchain: %d frames round the target in %d ms "
          "(%.1f a second, under TCG), the last found on the screen at %s, all %d of its "
          "judged pixels within %d of the reference" % (frames, ms, frames * 1000.0 / max(ms, 1),
                                                        place, count, TOLERANCE))
    return 0


if __name__ == "__main__":
    sys.exit(main())
