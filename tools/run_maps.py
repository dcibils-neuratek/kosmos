#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Maps, in the machine (`docs/maps.md`).

**M3 - the Map Kit inside Kosmos**: Port Alder, carried in the image as
`maps/port-alder.pmtiles`, opened by `use("/Kosmos/Kits/map")`; its header
read; the tile at zoom 16 where Lantern Street Market is decoded; drawn
into a surface in a style of two rules - buildings grey, streets white -
and both colours on it; its labels naming the market; and the projection
there and back.

Usage: run_maps.py IMAGE
"""

import os
import random
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_servers as S                                     # noqa: E402

KIT = r"""
local map = use("/Kosmos/Kits/map")
local bytes = sys.asset("maps/port-alder.pmtiles")
print("ASSET " .. tostring(bytes and #bytes))
local region, why = map.open(bytes)
if not region then print("OPEN " .. tostring(why)) return end
local i = region:info()
print(("INFO %d %d %.5f %.5f %.5f %.5f"):format(i.min_zoom, i.max_zoom, i.west, i.south, i.east, i.north))
-- Lantern Street Market, leaned as tools/mapcity.py leans the city.
local lon, lat = -0.000234, -0.002961
local x, y = map.project(lon, lat)
local n = 1 << 16
local tx, ty = math.floor(x * n), math.floor(y * n)
local tile, twhy = region:tile(16, tx, ty)
if not tile then print("TILE " .. tostring(twhy)) return end
print(("COUNT %d %d %d"):format(tile:count()))
local style = map.style{ background = 0, rules = {
  { layer = "building", fill = 0x808080, minzoom = 14 },
  { layer = "transportation", classes = "minor,service", line = 0xffffff, width = { 14, 2, 18, 18 } },
} }
local s = gfx.surface{ w = 512, h = 512 }
s:fill(0, 0, 512, 512, 0xff000000)
local t0 = sys.ticks()
local drew = tile:draw(s, 0, 0, 512, 16, style)
local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local grey, white = 0, 0
for py = 0, 511, 4 do
  for px = 0, 511, 4 do
    local c = s:get(px, py) & 0xffffff
    if c == 0x808080 then grey = grey + 1 elseif c == 0xffffff then white = white + 1 end
  end
end
print(("DRAW %d rules, %d grey, %d white, %.1f ms"):format(drew, grey, white, (sys.ticks() - t0) * 1000 / hz))
local market
for _, l in ipairs(tile:labels()) do
  if l.name == "Lantern Street Market" then market = l end
end
print("LABEL " .. (market and ("%s %s %.3f %.3f"):format(market.layer, market.class, market.x, market.y) or "none"))
local back_lon, back_lat = map.unproject(x, y)
print(("ROUND %.6f %.6f"):format(back_lon, back_lat))
print("NONE " .. tostring(select(2, region:tile(16, 0, 0))))
"""


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web)
    fails, said = [], {}

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)
        session.put(KIT.encode(), "/Temporary/kit.lua")
        said["kit"] = session.run("/Temporary/kit.lua").decode(errors="replace")
    finally:
        guest.close()

    out = said.get("kit", "")
    asset = re.search(r"ASSET (\d+)", out)
    info = re.search(r"INFO (\d+) (\d+) (\S+) (\S+) (\S+) (\S+)", out)
    count = re.search(r"COUNT (\d+) (\d+) (\d+)", out)
    draw = re.search(r"DRAW (\d+) rules, (\d+) grey, (\d+) white", out)
    label = re.search(r"LABEL poi marketplace ([\d.]+) ([\d.]+)", out)
    round_ = re.search(r"ROUND (\S+) (\S+)", out)

    if not asset or int(asset.group(1)) < 50000:
        fails.append("Port Alder is not in the image as maps/port-alder.pmtiles: %r" % out[:300])

    if not info or (info.group(1), info.group(2)) != ("11", "16") \
            or not float(info.group(3)) < 0 < float(info.group(5)):
        fails.append("the region's header is not Port Alder's, zooms 11 to 16 round Null Island: %r"
                     % out[:400])

    if not count or int(count.group(1)) < 50:
        fails.append("the market's tile at zoom 16 decoded to too little: %r" % out[:400])

    if not draw or int(draw.group(1)) != 2 or int(draw.group(2)) < 300 or int(draw.group(3)) < 100:
        fails.append("the tile was not drawn - grey buildings and white streets on the surface: %r"
                     % (draw and draw.group(0)))

    if not label:
        fails.append("the tile's labels do not name Lantern Street Market a marketplace: %r" % out[:500])

    if not round_ or abs(float(round_.group(1)) + 0.000234) > 1e-6 \
            or abs(float(round_.group(2)) + 0.002961) > 1e-6:
        fails.append("the projection there and back is not where it started: %r"
                     % (round_ and round_.group(0)))

    if "NONE no such tile" not in out:
        fails.append("a tile off the region was not refused as no such tile: %r" % out[-200:])

    checks = 7

    if fails:
        print("FAIL: %d of %d checks on Maps:" % (len(fails), checks))

        for f in fails:
            print("  " + f)

        return 1

    timing = re.search(r"DRAW .*, ([\d.]+) ms", out)
    print("PASS: %d checks on Maps (M3: Port Alder carried in the image and opened by the "
          "Map Kit, its header, the market's tile at zoom 16 decoded, drawn in a style of "
          "two rules%s, labelled, the projection there and back, a tile off the region refused)"
          % (checks, (" in " + timing.group(1) + " ms") if timing else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
