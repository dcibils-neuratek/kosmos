#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""A fixed path over a city, through the 3D Kit's Lua door (`docs/flight.md` F4c).

`k3dvk.elf` - the image Flight will name - opens a window drawn by Vulkan
and a terrain over Montevideo, and a program in Lua flies FRAMES frames
north towards the scene's eye, the way Flight's loop will: each frame it
asks the terrain which tiles its chunks still want (`g:wanted`), makes
each as a gfx surface - an elevation tile in terrarium's encoding, an
imagery tile of a colour - hands them over (`g:elevation`, `g:imagery`),
and draws (`w:draw`). Its frame times are reported.

What is held: the last frame, found on the screen QEMU scans out, pixel by
pixel against a ray cast through each pixel's centre - where it meets the
ground the elevation tiles describe, which of the chunks the guest said
were drawn holds that place, that chunk's colour, the sun and the haze.
The elevation is a tilted plane, written into each tile at its samples'
centres, so a chunk whose heights came from the wrong tile, or none, is
misplaced; the imagery is a colour by zoom with a check by tile, so a
chunk given its neighbour's picture, or a picture read in the wrong byte
order, shows. `run_k3dground.py`'s reference is used, with this terrain.

The tiles are made in the guest rather than fetched: the tiles server and
its cache are F5's, when Flight is an application.

Usage: run_flightpath.py IMAGE [APPS]
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
import run_k3dground as G                                    # noqa: E402
from run_k3dvk import lua_table, corner                      # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
X86 = "x86_64" in IMAGE
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    ROOT, "build", "user-x86_64-test" if X86 else "user-test", "apps")
WORK = scratch.directory("flightpath")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")

WINDOW = 96
FRAMES = 16
# The path: from START metres south of the scene's eye to it, at its height.
START = 6000.0
# The terrain, as the door takes it, and the plane its elevation tiles hold.
TERRAIN = dict(lon=-56.1990, lat=-34.9064, root=11, deepest=15, radius=2, split=2.0,
               most=400, elevation=13, keep=300, uploads=16)
# Frames at the last eye, at most, for every chosen chunk to be built.
SETTLE = 400
PLANE = dict(h0=300.0, gx=0.01, gz=-0.005)
ELEVATION_SIDE = 16
SCENE = dict(
    eye=(0.0, 2500.0, 0.0), target=(0.0, 0.0, -25000.0), focal=1.0, near=1.0,
    far=100000.0, sun=(0.3, 1.0, 0.2), ambient=0.35,
    zenith=(0.2, 0.4, 0.8), horizon=(0.8, 0.85, 0.9),
    haze=(0.7, 0.75, 0.8), haze_start=3000.0, haze_end=30000.0,
)
TOLERANCE = G.TOLERANCE


def colour(z, x, y):
    """An imagery tile's one colour: by zoom, with a check between
    neighbouring tiles. Red and blue differ at every zoom, so a picture
    read in the other byte order cannot pass."""
    k = z - TERRAIN["root"]
    return (60 + 35 * k, 170 - 20 * k + 30 * ((x + y) & 1), 30 + 40 * k)


PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A fixed path over a city, through the 3D Kit's Lua door
-- (`tools/run_flightpath.py`, `docs/flight.md` F4c).
-- kosmos: application
-- kosmos: image k3dvk.elf
local k = use("k3dvk.elf")
local function tell(name, s)
  local n = (#s + 99) // 100
  for i = 1, n do
    print(("vulkantest %%s %%d/%%d: %%s"):format(name, i, n, s:sub(100 * i - 99, 100 * i)))
  end
end

local T = %(terrain)s
local P = %(plane)s
local V = %(scene)s
local E, FRAMES, START, ROOT, SETTLE = %(side)d, %(frames)d, %(start)r, %(root)d, %(settle)d

-- Web Mercator and the metres the terrain counts, as `k3d_terrain.c` has them.
local EQUATOR = 40075016.68557849
local function project(lon, lat)
  local r = math.rad(lat)
  return (lon + 180) / 360, (1 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2
end
local OX, OY = project(T.lon, T.lat)
local METRES = EQUATOR * math.cos(math.rad(T.lat))

-- An elevation tile: the plane's height at each sample's centre, in
-- terrarium's encoding (R * 256 + G + B / 256 - 32768 metres).
local function elevation(z, x, y)
  local s, n = gfx.surface{ w = E, h = E }, 2 ^ z
  for j = 0, E - 1 do
    for i = 0, E - 1 do
      local mx, my = (x + (i + 0.5) / E) / n, (y + (j + 0.5) / E) / n
      local h = P.h0 + P.gx * (mx - OX) * METRES + P.gz * (my - OY) * METRES
      local q = math.floor((h + 32768) * 256 + 0.5)
      s:set(i, j, 0xFF000000 | (q >> 16) << 16 | ((q >> 8) & 255) << 8 | (q & 255))
    end
  end
  return s
end

local function imagery(z, x, y)
  local d = z - ROOT
  local r, g, b = 60 + 35 * d, 170 - 20 * d + 30 * ((x + y) & 1), 30 + 40 * d
  local s = gfx.surface{ w = 2, h = 2 }
  s:fill(0, 0, 2, 2, 0xFF000000 | r << 16 | g << 8 | b)
  return s
end

local w, why = k.window("Flight path", %(window)d, %(window)d)
if not w then
  tell("path", "failed=" .. tostring(why))
  return
end
local g = k.terrain(T)
local times, given, last, stood = {}, { e = 0, i = 0 }, nil, 0
local function frame(ahead)
  local view = {}
  for key, v in pairs(V) do view[key] = v end
  view.eye = { V.eye[1], V.eye[2], V.eye[3] + ahead }
  view.target = { V.target[1], V.target[2], V.target[3] + ahead }
  for _, t in ipairs(g:wanted(view.eye)) do
    if t[1] == "e" then
      g:elevation(t[2], t[3], t[4], elevation(t[2], t[3], t[4]))
    else
      g:imagery(t[2], t[3], t[4], imagery(t[2], t[3], t[4]))
    end
    given[t[1]] = given[t[1]] + 1
  end
  local said, failed = w:draw(g, view)
  if not said then
    tell("path", "failed=" .. tostring(failed))
    error("drawn nothing")
  end
  last = said
  return said
end
-- The path: each frame drawn with what is built, ancestors standing in.
for f = 0, FRAMES - 1 do
  local said = frame(START * (FRAMES - 1 - f) / (FRAMES - 1))
  times[#times + 1] = said:match("ms=([%%d.]+)")
  if tonumber(said:match("fallback=(%%d+)")) > 0 then stood = stood + 1 end
end
-- Then at the last eye until every chosen chunk is built and drawn itself.
local settled = 0
while settled < SETTLE do
  local d, n, r, f = last:match("drawn=(%%d+) of (%%d+) ready=(%%d+) building=%%d+ fallback=(%%d+)")
  if d == n and r == n and f == "0" then break end
  frame(0)
  settled = settled + 1
end
tell("path", ("device=%%s frames=%%d stood=%%d settled=%%d elevation=%%d imagery=%%d ms=%%s")
  :format(w:device(), FRAMES, stood, settled, given.e, given.i, table.concat(times, ",")))
tell("last", last)
w:closed(90)
""" % dict(terrain=lua_table(TERRAIN), plane=lua_table(PLANE), scene=lua_table(SCENE),
           side=ELEVATION_SIDE, frames=FRAMES, start=START, root=TERRAIN["root"], window=WINDOW,
           settle=SETTLE)


def reference(chunks):
    """`run_k3dground.py`'s reference, over this terrain: its plane, and
    each chunk's one colour."""
    G.TERRAIN = dict(TERRAIN, **PLANE)
    G.SCENE = SCENE
    G.SIZE = WINDOW
    G.OX, G.OY = G.project(TERRAIN["lon"], TERRAIN["lat"])
    G.METRES = G.EQUATOR * math.cos(TERRAIN["lat"] * math.pi / 180.0)
    G.sample = lambda z, tx, ty, u, v: [c / 255.0 for c in colour(z, tx, ty)]
    return G.reference(chunks)


def find_window(width, height, at, want):
    """Where on the screen the frame is, as (x, y) of its top left, or None,
    and how many of its judged pixels the best place had right."""
    judged = [(x, y, want[y][x][0]) for y in range(WINDOW) for x in range(WINDOW)
              if want[y][x] is not None and not corner(x, y, WINDOW)]

    def close(a, b):
        return all(abs(a[i] - b[i]) <= TOLERANCE for i in range(3))

    # The anchor: the first judged pixel of the ground, nearest the bottom.
    fx, fy, fc = next((x, y, c) for x, y, c in reversed(judged)
                      if want[y][x][1] is not None)
    best = 0
    for sy in range(fy, height - WINDOW + fy + 1):
        for sx in range(fx, width - WINDOW + fx + 1):
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


def main():
    built = os.path.join(APPS, "k3dvk.elf")

    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` builds it" % built)
        return 1

    image = os.path.join(WORK, "k3dvk.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)
    with open(os.path.join(WORK, "flightpath.lua"), "w") as f:
        f.write(PROGRAM)

    megabytes = os.path.getsize(image) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)]
                   + ["%s:/Home/apps/flight/%s" % (os.path.join(WORK, n), n)
                      for n in ("flightpath.lua", "k3dvk.elf")],
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
        guest.type("wm /Home/apps/flight/flightpath.lua")
        while time.monotonic() < started + 300 and len(said) < 2 and \
                "failed=" not in said.get("path", ""):
            said.update(G.gather(guest.seen[mark:], ("path", "last")))
            if len(said) < 2:
                time.sleep(0.2)
                guest._read_available()
        took = time.monotonic() - started
        if len(said) == 2:
            time.sleep(1.0)                     # the compositor's next pass
            screen = guest.screendump()
            if os.environ.get("KOSMOS_FLIGHT_SHOT"):
                R.write_png(os.environ["KOSMOS_FLIGHT_SHOT"], screen)
    finally:
        guest.close()

    p = re.match(r"device=(llvmpipe/1\.\d+) (\d+)x(\d+) frames=(\d+) stood=(\d+) "
                 r"settled=(\d+) elevation=(\d+) imagery=(\d+) ms=([\d.,]+)$",
                 said.get("path", ""))
    m = re.match(r"drawn=(\d+) of (\d+) ready=(\d+) building=(\d+) fallback=(\d+) "
                 r"held=(\d+) freed=(\d+) uploads=\d+ ms=[\d.]+ chunks=([\d/,]+)$",
                 said.get("last", ""))
    if not p or not m or screen is None:
        print("FAIL: the path over the city was not flown: %r, %r"
              % (said.get("path"), said.get("last")))
        print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        return 1
    if (int(p.group(2)), int(p.group(3))) != (WINDOW, WINDOW):
        print("FAIL: the window's picture is %sx%s, not %dx%d"
              % (p.group(2), p.group(3), WINDOW, WINDOW))
        return 1
    if not (m.group(1) == m.group(2) == m.group(3) and m.group(5) == "0"):
        print("FAIL: after %s frames at the last eye, it drew %s of its %s chunks, %s ready, "
              "%s standing in for others" % (p.group(6), m.group(1), m.group(2), m.group(3),
                                            m.group(5)))
        return 1
    held, freed = int(m.group(6)), int(m.group(7))
    if held > TERRAIN["keep"] or freed == 0:
        print("FAIL: chunks are not given back: %d held, keep %d, %d freed"
              % (held, TERRAIN["keep"], freed))
        return 1
    if int(p.group(5)) == 0:
        print("FAIL: no frame of the path drew an ancestor for a chunk still being built - "
              "the path proves nothing about the builder")
        return 1
    chunks = set(tuple(int(n) for n in s.split("/")) for s in m.group(8).split(","))
    if len(chunks) != int(m.group(2)):
        print("FAIL: %s chunks said, %d listed" % (m.group(2), len(chunks)))
        return 1

    want = reference(chunks)
    width, height, at = R.pixel_reader(screen)
    place, right, count = find_window(width, height, at, want)
    zooms, used, sky = {}, set(), 0
    for row in want:
        for w in row:
            if w is None:
                continue
            if w[1] is None:
                sky += 1
            else:
                zooms[w[1][0]] = zooms.get(w[1][0], 0) + 1
                used.add(w[1])
    if place is None:
        print("FAIL: no %d by %d square on the screen is the path's last frame: the nearest "
              "had %d of its %d judged pixels right" % (WINDOW, WINDOW, right, count))
        return 1
    thin = []
    if sky < 20:
        thin.append("the sky (%d)" % sky)
    if len([z for z, n in zooms.items() if n >= 20]) < 3:
        thin.append("three zooms of 20 pixels (%s)" % zooms)
    if len(used) < 10:
        thin.append("ten chunks (%d)" % len(used))
    if thin:
        print("FAIL: the last frame holds too little to prove much: " + ", ".join(thin))
        return 1

    ms = [float(t) for t in p.group(9).split(",")]
    later = ms[1:] or ms
    print("PASS: a path over the city through `k3dvk.elf`, in %.1f s: %s frames on %s, "
          "%s elevation and %s imagery tiles made and given as they were wanted, chunks "
          "built on the builder's thread - %s frames drew an ancestor for one still being "
          "built, %d chunks given back, %d held of %d; at the last eye, %s frames until all "
          "%s were drawn themselves, found on the screen at %s, all %d judged pixels within "
          "%d of a ray through the plane (sky %d; ground by zoom %s from %d chunks). Path "
          "frames, under TCG: the first %.0f ms, then %.0f ms on average and %.0f at most"
          % (took, p.group(4), p.group(1), p.group(7), p.group(8), p.group(5), freed, held,
             TERRAIN["keep"], p.group(6), m.group(2), place, count, TOLERANCE, sky,
             ", ".join("%d: %d" % (z, zooms[z]) for z in sorted(zooms)), len(used), ms[0],
             sum(later) / len(later), max(later)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
