#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Maps, in the machine (`docs/maps.md`).

**M4 - the window**: `open maps` - Port Alder drawn in the light look the
suite's machine wears, its land and its main roads on the screen in their
colours; the sidebar's button hiding the sidebar and the map taking its
place; the + button zooming in a whole step; and a drag moving the map the
way the pointer went.

**M5 - places**: "market" typed into the search finds Lantern Street
Market first, Return opens its card and centres the map there with names
on it, Save keeps it, and the settings kit's `maps` holds it.

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

import run_screenshot as R                                  # noqa: E402
import run_servers as S                                     # noqa: E402
import kosmos_vnc as V                                      # noqa: E402

# The light look's colours, as `maps.lua` draws them.
LAND, MAJOR = (0xf2, 0xef, 0xe9), (0xfd, 0xe8, 0xa8)

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

        # ---- M4: the window ----
        mark = len(guest.seen)
        session.run("open maps")
        placed = guest.wait_for_line("wm: window Maps at ", "the Maps window", mark)
        controls = guest.wait_for_line("maps: controls ", "the Maps controls", mark)
        said["first"] = guest.wait_for_line("maps: at ", "the first map drawn", mark)
        time.sleep(2)
        wx, wy, ww, wh = (int(v) for v in re.match(r"(\d+),(\d+) (\d+)x(\d+)", placed).groups())
        width, height, _ = R.parse_ppm(guest.screendump())

        def at(name, text):
            m = re.search(name + r" (\d+),(\d+)", text)
            return (int(m.group(1)), int(m.group(2))) if m else None

        def click(x, y):
            guest.mouse_to(*R._to_tablet(wx + x, wy + y, width, height))
            time.sleep(0.3)
            guest.mouse_button(True)
            time.sleep(0.2)
            guest.mouse_button(False)
            time.sleep(0.5)

        def colours_in(x0, y0, x1, y1):
            _, _, pick = R.pixel_reader(guest.screendump())
            land = major = 0
            for y in range(wy + y0, wy + y1, 3):
                for x in range(wx + x0, wx + x1, 3):
                    c = pick(x, y)
                    land += c == LAND
                    major += c == MAJOR
            return land, major

        # The window kept as it looked, for whoever reads the run after.
        w_, h_, rgb_ = R.parse_ppm(guest.screendump())
        rows_ = [rgb_[((y_ * w_) + wx) * 3:((y_ * w_) + wx + ww) * 3]
                 for y_ in range(wy, min(h_, wy + wh))]
        os.makedirs(os.path.join(os.path.dirname(HERE), "build", "maps"), exist_ok=True)
        V.png(os.path.join(os.path.dirname(HERE), "build", "maps", "maps.png"), ww, len(rows_),
              b"".join(rows_))

        mapbox = re.search(r"map (\d+),(\d+) (\d+)x(\d+)", controls)
        mx, my, mw, mh = (int(v) for v in mapbox.groups())
        said["drawn"] = colours_in(mx + 10, my + 10, mx + mw - 10, my + mh - 10)

        # The sidebar's button: hidden, and the map where it was.
        side = at("sidebar", controls)
        mark = len(guest.seen)
        click(side[0] + 13, side[1] + 13)
        said["hidden"] = guest.wait_for_line("maps: sidebar ", "the sidebar's button", mark)
        said["controls2"] = guest.wait_for_line("maps: controls ", "the controls again", mark)
        time.sleep(2)
        said["under"] = colours_in(20, my + 60, mx - 20, my + 300)

        # Zoom in, by its button.
        zin = at("zoom in", said["controls2"])
        mark = len(guest.seen)
        click(zin[0] + 20, zin[1] + 20)
        said["zoomed"] = guest.wait_for_line("maps: at ", "zoomed in", mark)

        # A drag, 200 to the right: the map moves with the pointer, so its
        # centre goes west.
        m2 = re.search(r"map (\d+),(\d+) (\d+)x(\d+)", said["controls2"])
        cx0 = int(m2.group(1)) + int(m2.group(3)) // 2
        cy0 = int(m2.group(2)) + int(m2.group(4)) // 2
        mark = len(guest.seen)
        guest.mouse_to(*R._to_tablet(wx + cx0, wy + cy0, width, height))
        time.sleep(0.3)
        guest.mouse_button(True)
        for k in range(1, 9):
            guest.mouse_to(*R._to_tablet(wx + cx0 + 25 * k, wy + cy0, width, height))
            time.sleep(0.25)
        guest.mouse_button(False)
        said["dragged"] = guest.wait_for_line("maps: at ", "dragged", mark)

        # ---- M5: search, the card, Save ----
        # The sidebar back first, then the search field pressed and typed in.
        click(side[0] + 13, side[1] + 13)
        time.sleep(2)
        srch = at("search", controls)
        mark = len(guest.seen)
        click(srch[0] + 60, srch[1] + 18)
        for k in "market":
            guest.sendkey(k)
            time.sleep(0.4)
        found = guest.wait_for_line('maps: search "market"', "the search", mark)
        time.sleep(1)
        guest._read_available()
        said["found"] = re.findall(r'maps: search "market", (.*)', guest.seen[mark:])[-1:]
        mark = len(guest.seen)
        guest.sendkey("ret")
        said["card"] = guest.wait_for_line("maps: card ", "the card", mark)
        buttons = guest.wait_for_line("maps: card buttons ", "the card's buttons", mark)
        said["there"] = guest.wait_for_line("maps: at ", "the map at the place", mark)
        time.sleep(2)
        save = at("save", buttons)
        mark = len(guest.seen)
        click(save[0] + 20, save[1] + 15)
        said["saved"] = guest.wait_for_line("maps: saved ", "Save", mark)
        time.sleep(1)
        said["kept"] = session.run("cat /Home/Preferences/maps").decode(errors="replace")
        w_, h_, rgb_ = R.parse_ppm(guest.screendump())
        rows_ = [rgb_[((y_ * w_) + wx) * 3:((y_ * w_) + wx + ww) * 3]
                 for y_ in range(wy, min(h_, wy + wh))]
        V.png(os.path.join(os.path.dirname(HERE), "build", "maps", "card.png"), ww, len(rows_),
              b"".join(rows_))
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

    first = re.match(r"(\S+) (\S+), zoom ([\d.]+), (\d+) tiles", said.get("first", ""))
    land, major = said.get("drawn", (0, 0))

    if not first or int(first.group(4)) < 2 or land < 1000 or major < 50:
        fails.append("Maps did not open with Port Alder drawn - its land and main roads in "
                     "their colours: %r, %d land, %d main road" % (said.get("first"), land, major))

    under_land, _ = said.get("under", (0, 0))

    if (said.get("hidden") or "").strip() != "hidden" or under_land < 200:
        fails.append("the sidebar's button did not hide the sidebar and give its room to the "
                     "map: %r, %d of land where it was" % (said.get("hidden"), under_land))

    zoomed = re.match(r"\S+ \S+, zoom ([\d.]+)", said.get("zoomed", ""))

    if not first or not zoomed or abs(float(zoomed.group(1)) - float(first.group(3)) - 1) > 0.01:
        fails.append("the + button did not zoom in a whole step: %r then %r"
                     % (said.get("first"), said.get("zoomed")))

    dragged = re.match(r"(\S+) (\S+), zoom", said.get("dragged", ""))

    if not zoomed or not dragged or not float(dragged.group(1)) < float(said["zoomed"].split()[0]):
        fails.append("a drag to the right did not move the map west: %r then %r"
                     % (said.get("zoomed"), said.get("dragged")))

    found = (said.get("found") or [""])[0].strip()

    if not found.endswith("first Lantern Street Market") or not re.match(r"[2-9] found", found):
        fails.append('"market" typed into the search did not find Lantern Street Market first '
                     "among two or more: %r" % found)

    there = re.match(r"\S+ \S+, zoom ([\d.]+), \d+ tiles in [\d.]+ ms, (\d+) names",
                     said.get("there", ""))

    if (said.get("card") or "").strip() != "Lantern Street Market" or not there \
            or float(there.group(1)) < 16 or int(there.group(2)) < 3:
        fails.append("Return did not open the market's card with the map there at 16 and names "
                     "on it: %r, %r" % (said.get("card"), said.get("there")))

    if (said.get("saved") or "").strip() != "Lantern Street Market" \
            or "Lantern Street Market" not in said.get("kept", ""):
        fails.append("Save did not keep the market in the settings kit's maps: %r, %r"
                     % (said.get("saved"), said.get("kept", "")[:300]))

    checks = 14

    if fails:
        print("FAIL: %d of %d checks on Maps:" % (len(fails), checks))

        for f in fails:
            print("  " + f)

        return 1

    timing = re.search(r"DRAW .*, ([\d.]+) ms", out)
    print("PASS: %d checks on Maps (M3: Port Alder carried in the image and opened by the "
          "Map Kit, its header, the market's tile at zoom 16 decoded, drawn in a style of "
          "two rules%s, labelled, the projection there and back, a tile off the region refused; "
          "M4: the window opened with the city drawn, the sidebar's button hiding it, + zooming "
          "a step, a drag moving the map; M5: a search finding the market, its card opened "
          "with names on the map, Save keeping it)"
          % (checks, (" in " + timing.group(1) + " ms") if timing else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
