#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Flight, flown (`docs/flight.md` F5).

The application as a person runs it: `flight.lua` on `k3dvk.elf`,
installed in `/Home`, opened by the window manager, its tiles fetched by
the `tiles` server from a server on this Mac - reached by the guest as
10.0.2.2, as `x86-tiles` serves them - and the keys held down through
QEMU's own keyboard, so what reaches the aircraft is the window's
`rawkey` path rather than anything a test could say to it directly.

The Mac serves elevation as terrarium PNGs of rolling hills whose height
is known everywhere, and imagery as a JPEG. Flight starts 600 m over
Montevideo, heading north at 55 m/s, and its console lines are read:

- it opens a Vulkan window and draws, frame after frame;
- **tiles arrive and are handed to the terrain**: both kinds asked of the
  server, and the count of tiles handed grows;
- **the keys fly it**: holding the right arrow rolls it right and turns it
  east; let go, it rolls back towards level; holding the down arrow (the
  yoke back) climbs it; `V` changes the view - each read from the state it
  reports, against the state before the key;
- the ground it reports under it is the served hills' height there, so the
  aircraft stands on the ground it is drawn over;
- the Cessna (`assets/models/cessna172`) is read by the 3D Kit's GLB
  reader and put on the device whole: its 77,317 triangles, the 10 of its
  28 textures its parts are drawn with, 8.15 m long and 10.92 m across.

Usage: run_flight.py IMAGE [APPS]
"""

import http.server
import math
import os
import re
import struct
import subprocess
import sys
import threading
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402

# `--island` (F5j): the sources at a port nobody serves, so nothing comes
# and Flight flies over its invented island instead.
ISLAND = "--island" in sys.argv
# `--land` (F8): on a short final to Rio's 02R, hands off, until the wheels
# touch - a landing under 600 feet a minute, on the runway.
LAND = "--land" in sys.argv
# `--wrong-layer` (F6, B3) is the buildings' control: the served vector
# tiles name their layer `buildings`, which OpenMapTiles' is not, so none
# is stood up and the suite must fail.
WRONG_LAYER = "--wrong-layer" in sys.argv
# `--no-roofs` (B4) is the roofs' control: the imagery at zoom 16 - the
# roofs' pieces - is refused, so the roofs are the walls' concrete, no
# tile's wears a photograph, and the suite must fail.
NO_ROOFS = "--no-roofs" in sys.argv
# `--no-shadows` (S2) is the shadows' control: Flight's `shadows` setting
# off, so nothing is drawn into the sun's map and the suite must fail.
NO_SHADOWS = "--no-shadows" in sys.argv
# `--clear` (W1) is the weather's control: the landing flown in a clear sky
# rather than under the low cloud it asks for, so no deck is made and the
# landing suite must fail.
CLEAR = "--clear" in sys.argv
# `--calm` (W2) is the wind's control: the landing flown in still air rather
# than into 25 kt down the runway, so the wheels meet the ground at the
# air's speed and the landing suite must fail.
CALM = "--calm" in sys.argv
# `--no-pad` (F7) is the game controller's control: `padtest` writes the
# pad with its stick in the middle rather than full left, so nothing rolls
# it and the suite must fail.
NO_PAD = "--no-pad" in sys.argv
# `--b747` (F12b): the 747 on Rio's runway, taken off at 4x by the
# autopilot on a route (F10d) - its jets spooling up over
# seconds, and off the ground before the runway ends. `--instant-spool` is
# its control: the same 747 with jets that follow the throttle in a
# twentieth of a second, which the spool's check must refuse.
B747 = "--b747" in sys.argv or "--instant-spool" in sys.argv
INSTANT = "--instant-spool" in sys.argv
sys.argv = [a for a in sys.argv if a not in ("--island", "--land", "--wrong-layer", "--no-roofs",
                                             "--no-shadows", "--clear", "--calm", "--no-pad",
                                             "--b747", "--instant-spool")]
IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
X86 = "x86_64" in IMAGE
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    ROOT, "build", "user-x86_64-test" if X86 else "user-test", "apps")
# Where a failing check's screen is kept: under build/, since WORK goes
# when the suite ends.
KEPT = os.path.join(ROOT, "build", "flight-failed")
os.makedirs(KEPT, exist_ok=True)
WORK = scratch.directory("flight-island" if ISLAND else "flight-land" if LAND
                         else "flight-747" if B747 else "flight")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")
FOLDER = "/Home/Apps/Flight"

WIDTH, HEIGHT = 160, 100
if os.environ.get("KOSMOS_FLIGHT_SIZE"):                     # a picture for a person
    WIDTH, HEIGHT = map(int, os.environ["KOSMOS_FLIGHT_SIZE"].split("x"))
LON, LAT = -56.1650, -34.9060
EQUATOR = 40075016.68557849

with open(os.path.join(ROOT, "assets", "images", "test-quads.jpg"), "rb") as f:
    JPEG = f.read()


def hills(mx, my):
    """The served ground's metres at a place across the world, 0 to 1:
    hills a few kilometres apart, from 20 to 220 m."""
    metres = EQUATOR * math.cos(math.radians(LAT))
    x, y = mx * metres, my * metres
    return 120 + 100 * math.sin(x / 1500.0) * math.cos(y / 2100.0)


def terrarium(z, x, y):
    side = 64
    n = 2 ** z
    rows = []
    for py in range(side):
        row = bytearray([0])
        for px in range(side):
            h = hills((x + (px + 0.5) / side) / n, (y + (py + 0.5) / side) / n)
            v = int(round((h + 32768) * 256))
            row += bytes([(v >> 16) & 255, (v >> 8) & 255, v & 255])
        rows.append(bytes(row))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", side, side, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 6))
            + chunk(b"IEND", b""))


# **The buildings' tile** (F6, B3): every zoom-14 vector tile the same - a
# block of 6 by 6 boxes in its middle, 20 by 15 m and from 10 to 30 m
# tall, and one with a courtyard - so how many are stood up and of how many
# triangles is known: a box is a roof of 2 and 4 walls of 2, 10; the
# courtyard a roof of 8 and 8 walls, 24.
BUILDINGS_A_TILE = 37
TRIANGLES_A_TILE = 36 * 10 + 24


def vector_tile():
    import mapcity                                           # noqa: E402
    feats = []
    for i in range(6):
        for j in range(6):
            x0, y0 = 1800 + 100 * i, 1800 + 100 * j
            ring = [(x0, y0), (x0 + 40, y0), (x0 + 40, y0 + 30), (x0, y0 + 30)]
            if mapcity.signed_area(ring) < 0:
                ring = ring[::-1]
            feats.append(("polygon", mapcity.geometry("polygon", [ring]),
                          {"render_height": 10 + (i + j) % 5 * 5}))
    outer = [(2600, 2600), (2720, 2600), (2720, 2720), (2600, 2720)]
    hole = [(2640, 2640), (2680, 2640), (2680, 2680), (2640, 2680)]
    if mapcity.signed_area(outer) < 0:
        outer = outer[::-1]
    if mapcity.signed_area(hole) > 0:
        hole = hole[::-1]
    feats.append(("polygon", mapcity.geometry("polygon", [outer, hole]),
                  {"render_height": 12.5, "render_min_height": 0}))
    name = "buildings" if WRONG_LAYER else "building"
    return mapcity.gz(mapcity.lendelim(3, mapcity.layer_bytes(name, feats)))


def tile_server():
    asked = {"elev": 0, "img": 0, "vec": 0, "roof": 0, "search": None}
    vector = vector_tile()
    made = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):
            pass

        def send(self, code, body, kind):
            self.send_response(code)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            m = re.match(r"^/elev/(\d+)/(\d+)/(\d+)\.png$", self.path)
            if m:
                asked["elev"] += 1
                key = tuple(map(int, m.groups()))
                if key not in made:
                    made[key] = terrarium(*key)
                self.send(200, made[key], "image/png")
                return
            if self.path.startswith("/search?q="):
                # Nominatim's shape, as the tiles server asks it (F5i).
                asked["search"] = self.path
                body = (b'[{"lat":"48.8566","lon":"2.3522","name":"Paris","type":"city",'
                        b'"place_rank":16,"display_name":"Paris, Ile-de-France, France"}]')
                self.send(200, body, "application/json")
                return
            if re.match(r"^/vec/14/(\d+)/(\d+)\.pbf$", self.path):
                asked["vec"] += 1
                self.send(200, vector, "application/x-protobuf")
                return
            m = re.match(r"^/img/(\d+)/(\d+)/(\d+)$", self.path)
            if m and m.group(1) == "16":
                asked["roof"] += 1
                if NO_ROOFS:
                    self.send(404, b"", "text/plain")
                    return
            if m:
                asked["img"] += 1
                self.send(200, JPEG, "image/jpeg")
                return
            self.send(404, b"", "text/plain")

    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, asked


SETUP = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Flight's settings for its suite (`tools/run_flight.py`).
local prefs = use("/Kosmos/Libraries/prefs.lua")
prefs.open("flight", {}):set{
  imagery = "%(base)s/img/{z}/{y}/{x}", imagery_format = "jpg",
  elevation = "%(base)s/elev/{z}/{x}/{y}.png",
  vector = "%(base)s/vec/{z}/{x}/{y}.pbf", buildings = true, shadows = %(shadows)s,
  lon = %(lon)r, lat = %(lat)r, width = %(width)d, height = %(height)d,
  report = 2, frames = 0, finder = "%(base)s/search",
  fly_to = "EZE",                 -- Q engages on the route to Ezeiza (F10c)
}
local s = prefs.open("flight", {})
s:reset("city") s:reset("place") s:reset("recent") s:reset("start") s:reset("time") s:reset("weather") s:reset("wind") s:reset("wind_from")
print("flight setup done")
"""

LINE = re.compile(r"flight frame=(\d+) view=(\d) x=(-?\d+) y=(-?\d+) z=(-?\d+) v=([\d.]+) "
                  r"heading=(\d+) roll=(-?\d+) ground=(\w+) throttle=[\d.]+ tiles=(\d+) "
                  r"mean_ms=([\d.]+) slowest_ms=([\d.]+) (.*)$")


def states(text):
    out = []
    for line in text.splitlines():
        m = LINE.search(line)
        if m:
            g = m.groups()
            out.append(dict(frame=int(g[0]), view=int(g[1]), x=int(g[2]), y=int(g[3]),
                            z=int(g[4]), v=float(g[5]), heading=int(g[6]), roll=int(g[7]),
                            ground=g[8],
                            tiles=int(g[9]), mean=float(g[10]), drawn=g[12]))
    return out


def strip_band(R, data, across, tall=(34, 46), white_least=20, inside=None, left_near=None,
               box=None):
    """The strip on the screen: a stack of 34 to 46 rows each holding a
    stretch as wide as Flight's window, `across`, that is at least half dark
    - the band laid over the ground by its alpha - starting at the same x to
    four pixels, with words in it. Every stack is found and a strip-shaped
    one kept, so a taller dark panel elsewhere - the Log's - is not taken
    for it. (left, top, rows, white pixels) of the one kept, or else of the
    tallest; None when there is no stack at all.

    `left_near`, where wm said Flight's window is: a strip-shaped stack
    starting there is the one kept. Without it the stack with the most
    words was, and with shadows Flight loads the processor enough that the
    Monitor's two graphs and their labels made a stack of that shape with
    more white in it than the strip's (21 November).

    `box`, the window's rectangle as wm said it, (x, y, w, h): only its
    pixels are looked at, and every one outside it counts as light. A
    window beside Flight's is not Flight's, and on 23 November System
    Monitor's Overview put a black graph at the strip's height ending at
    Flight's left edge - so the strip had no light pixel beyond its left
    end and was not found at all."""
    width, height, px = R.parse_ppm(data)
    x0, y0, x1, y1 = (0, 0, width, height) if box is None else \
        (box[0], box[1], box[0] + box[2], box[1] + box[3])
    if inside is None:
        inside = lambda r, g, b: max(r, g, b) < 90

    def light(r, g, b):
        return min(r, g, b) > 200

    def stretches(y):
        """Where a stretch `across` wide is at least half dark, with lighter
        pixels just outside both ends - a window's width of band and no more
        - counted with a prefix sum so a 1080-row screen stays quick. Both
        ends: allowing one let a piece of the Log's panel pass for the strip
        (its control, 20 November)."""
        row = px[y * width * 3:(y + 1) * width * 3]
        dark = [x0 <= x < x1 and y0 <= y < y1 and inside(row[x * 3], row[x * 3 + 1], row[x * 3 + 2])
                for x in range(width)]
        sums = [0]
        for d in dark:
            sums.append(sums[-1] + d)
        out = []
        for left in range(2, width - across - 2):
            if sums[left + across] - sums[left] >= 0.5 * across \
                    and not dark[left - 2] and not dark[left + across + 1]:
                if not out or left - out[-1] > 8:
                    out.append(left)
        return out

    def words(left, top, rows):
        white = 0
        for y in range(top, top + rows):
            row = px[y * width * 3:(y + 1) * width * 3]
            white += sum(1 for x in range(left, min(width, left + across))
                         if light(row[x * 3], row[x * 3 + 1], row[x * 3 + 2]))
        return white

    stacks, runs = [], {}
    for y in range(height + 1):
        now = {}
        for left in (stretches(y) if y < height else []):
            key = next((k for k in runs if abs(k - left) <= 4), left)
            top, rows = runs.get(key, (y, 0))
            now[key] = (top, rows + 1)
        stacks += [(k, top, rows) for k, (top, rows) in runs.items() if k not in now]
        runs = now
    if not stacks:
        return None
    shaped = [(k, t, r, words(k, t, r)) for k, t, r in stacks if tall[0] <= r <= tall[1]]
    shaped = [b for b in shaped if b[3] >= white_least]
    if left_near is not None and any(abs(b[0] - left_near) <= 6 for b in shaped):
        shaped = [b for b in shaped if abs(b[0] - left_near) <= 6]
    if shaped:
        return max(shaped, key=lambda b: b[3])
    k, t, r = max(stacks, key=lambda b: b[2])
    return k, t, r, words(k, t, r)

def panel_seen(R, data, band, pw, ph, at=None):
    """The panel's rectangle on the screen, from the strip's band - centred
    on the window's foot, or at `at`, (x, y) in the window: (the
    fraction of it that is the panel's near-black - its ground and the
    dials' faces, which neither the sky nor the gate's ground has - its
    light pixels, the fraction of the row just above it that is the sky)."""
    width, height, px = R.parse_ppm(data)
    if at is None:
        left = band[0] + (WIDTH - pw) // 2
        top = band[1] + 40 - ph                     # the strip's foot, less the panel
    else:
        left = band[0] + at[0]
        top = band[1] + 40 - HEIGHT + at[1]         # the window's top, and down
    def pixel(x, y):
        i = (y * width + x) * 3
        return px[i], px[i + 1], px[i + 2]
    def sky(r, g, b):
        return max(r, g, b) >= 90 and b - r >= 30
    black = light = 0
    for y in range(top, top + ph):
        for x in range(left, left + pw):
            r, g, b = pixel(x, y)
            black += max(r, g, b) < 70 and max(r, g, b) - min(r, g, b) < 24
            light += min(r, g, b) > 200
    above = sum(sky(*pixel(x, top - 2)) for x in range(left, left + pw))
    return round(black / (pw * ph), 2), light, round(above / pw, 2)

def strip_words_at(R, data, band):
    """The strip's light pixels - its words - as a set of places in the
    band: what changes when its numbers do, and what the ground behind its
    dark band, which shows through by the band's alpha, never lights."""
    width, height, px = R.parse_ppm(data)
    left, top, rows = band[0], band[1], band[2]
    out = set()
    for y in range(top, top + rows):
        for x in range(left, min(width, left + WIDTH)):
            i = (y * width + x) * 3
            if min(px[i], px[i + 1], px[i + 2]) > 200:
                out.add((x - left, y - top))
    return out

def orange(ppm, box):
    """How many of the window's pixels are a dusk's warm colour: red over
    blue, and bright. Only the window's - `box` is where the window manager
    put it, x, y, width, height - since the desktop's wallpaper has warm
    colours of its own."""
    import run_screenshot
    w, h, rgb = run_screenshot.parse_ppm(ppm)
    x0, y0, bw, bh = box
    n = 0
    for y in range(y0, min(h, y0 + bh)):
        for x in range(x0, min(w, x0 + bw)):
            i = (y * w + x) * 3
            r, g, b = rgb[i], rgb[i + 1], rgb[i + 2]
            if r > 150 and r - b > 40 and g < r - 20:
                n += 1
    return n


def island_checks(seen, island, asked, guest):
    """The invented island (F5j): Flight said it went offline and why, the
    place box says so, the credit is gone, and the terrain was handed the
    island's tiles - drawn, with nothing asked of the Mac."""
    fails = []
    off = re.search(r"flight offline: ([^\r\n]*)", seen)
    pl = re.findall(r"flight place (.*?) - (.*?) (?:laid at|not laid)", seen)
    print("flight: %s" % (off.group(0) if off else "no offline line"))
    print("flight: places %r" % (pl,))
    print("flight: over the island %r" % ({k: island[k] for k in ("frame", "x", "y", "z",
                                                                  "tiles")},))
    print("flight: %s" % island["drawn"][:80])
    if not off or "failed" not in off.group(1):
        fails.append("Flight did not go offline because its fetches failed: %r"
                     % (off.group(0) if off else None))
    after = seen[off.end():] if off else ""
    again = re.search(r"flight started (\w+) at x=(-?\d+) y=(-?\d+) z=(-?\d+)", after)
    print("flight: then %s" % (again.group(0) if again else "no start"))
    if not re.search(r"flight started runway at", seen[:off.start()] if off else seen):
        fails.append("the flight did not begin on Rio's runway, so the move is not tested")
    if not again or again.groups() != ("city", "0", again.group(3), "0") \
            or int(again.group(3)) < 457 or island["y"] < 300:
        fails.append("going offline did not start the flight again over the island's middle: "
                     "%r, then y=%r" % (again.group(0) if again else None, island["y"]))
    if not pl or pl[-1] != ("Offline: an invented island",
                            "the imagery and terrain could not be fetched"):
        fails.append("the place box did not say the island: %r" % (pl,))
    drawn = re.match(r"drawn=(\d+) of (\d+)", island["drawn"])
    if island["tiles"] < 20 or not drawn or int(drawn.group(1)) == 0:
        fails.append("the island's tiles were not handed and drawn: %d handed, %r"
                     % (island["tiles"], island["drawn"][:60]))
    if asked["elev"] or asked["img"]:
        fails.append("the Mac was asked for tiles: %r" % (asked,))
    if fails:
        for f in fails:
            print("FAIL: " + f)
        return 1
    print("PASS: Flight flew over its invented island - %d tiles made, %s of the chunks drawn"
          % (island["tiles"], drawn.group(0)))
    return 0


def b747_checks(seen):
    """The 747 (F12b): flown from its own file on a jet engine, its spool
    following the throttle over seconds, off Rio's 1,323 m runway."""
    fails = []
    flown = re.search(r"flight aircraft (Boeing 747) from Aircraft/(\w+)\.lua, (\w+) engine", seen)
    model = re.search(r"flight aircraft parts=(\d+) triangles=(\d+)", seen)
    print("flight: %s; %s" % (flown.group(0) if flown else "no 747 flown",
                              model.group(0) if model else "no model line"))
    if not flown or flown.group(3) != "jet":
        fails.append("the 747 was not flown on a jet: %r" % re.findall(r"flight aircraft[^\r\n]*", seen))
    # Taken off by the autopilot on a route (F10d): from the runway it is
    # on, SDU 02R, towards Galeao, the take-off said and then the departure.
    route = re.search(r"flight route SDU to GIG (\w+), (\d+) nm", seen)
    ways = re.findall(r"flight route (takeoff|departure),", seen)
    print("flight: the route %r, then %r" % (route.groups() if route else None, ways))
    if not route or ways[:2] != ["takeoff", "departure"]:
        fails.append("Q with fly_to = GIG did not take the 747 off on its route: %r %r"
                     % (route.groups() if route else None, ways))
    if not model or model.group(2) != "1904":
        fails.append("the 747's model, 1,904 triangles, was not loaded: %r"
                     % (model.group(0) if model else None))
    # The spool: from a quarter to three quarters of the way, as a jet's
    # six-second spool towards full power takes - 6.6 s, the autopilot's
    # take-off opening the throttle (F10d) - and no time at all for one
    # that follows the throttle at once (the control).
    quarters = {float(q): float(t) for q, t, kind in re.findall(
        r"flight spool ([\d.]+) at ([\d.]+) s, throttle 1\.00, (\w+)", seen) if kind == "jet"}
    q1 = next((t for q, t in sorted(quarters.items()) if 0.25 <= q < 0.5), None)
    q3 = next((t for q, t in sorted(quarters.items()) if 0.75 <= q < 1.0), None)
    print("flight: the jets' spool %r" % (sorted(quarters.items()),))
    if q1 is None or q3 is None or not 6 <= q3 - q1 <= 12:
        fails.append("the jets did not spool from a quarter to three quarters in 6 to 12 s: %r"
                     % (sorted(quarters.items()),))
    off = re.search(r"flight lifted off at (\d+) kt after (\d+) m on the runway in ([\d.]+) s", seen)
    crashed = re.search(r"flight crashed: ([^\r\n]*)", seen)
    print("flight: %s" % (off.group(0) if off else crashed.group(0) if crashed else "never left the ground"))
    if crashed or not off:
        fails.append("the 747 did not take off: %r" % (crashed.group(0) if crashed else None))
    elif not 120 <= int(off.group(1)) <= 175 or int(off.group(2)) > 1323:
        fails.append("the 747 did not lift off at 120-175 kt within SDU's 1,323 m: %r" % off.group(0))
    # The terrain moved under it (F10a), every 400 m here: on the runway the
    # same runway is under the wheels after as before, found among the new
    # centre's - and the roll measured across the moves is the 1,002 m it
    # is unmoved (the autopilot's take-off, F10d; 1,080 by the keys before),
    # since where the roll began moved with the frame.
    moves = re.findall(r"flight anchored again at (-?[\d.]+) (-?[\d.]+), (\d+) m from the last; "
                       r"on ([^\r\n]*?) before, ([^\r\n]*?) after", seen)
    print("flight: the terrain moved %d times: %r" % (len(moves), moves[:4]))
    on_runway = [m for m in moves if m[3] != "no runway"]
    if len(on_runway) < 2 or any(m[3] != "SDU 02R/20L" or m[4] != m[3] for m in on_runway):
        fails.append("the terrain did not move twice in the roll, the runway the same under it: %r"
                     % (moves,))
    if off and not 930 <= int(off.group(2)) <= 1080:
        fails.append("the roll across the moves was not the 1,002 m it is unmoved: %r" % off.group(0))
    if fails:
        for f in fails:
            print("FAIL: " + f)
        return 1
    print("PASS: Flight's 747 took off on its jets - %s" % off.group(0))
    return 0


def land_checks(seen, roll=None):
    """A landing (F8): begun on final to SDU's 02R, the flight touched down
    under 600 feet a minute, on SDU 02R/20L, and did not crash."""
    fails = []
    st = re.search(r"flight started final at x=(-?\d+) y=(-?\d+) z=(-?\d+) heading=(\d+) "
                   r"in ([^\r\n]*)", seen)
    print("flight: %s" % (st.group(0) if st else "no start line"))
    if not st or st.group(5).strip() != "Rio de Janeiro" or st.group(4) != "357":
        fails.append("the flight did not begin on final to Rio's 02R: %r"
                     % re.findall(r"flight (?:started|restarted).*", seen))
    landed = re.search(r"flight landed at (\d+) fpm ([^\r\n]*)", seen)
    crashed = re.search(r"flight crashed: ([^\r\n]*)", seen)
    print("flight: %s" % (landed.group(0) if landed else crashed.group(0) if crashed
                          else "neither landed nor crashed"))
    # The runways drawn (F8b): Rio's fields within 30 km, each a strip of
    # asphalt with its paint - SDU's two and Galeao's two at least.
    drawn = re.search(r"flight runways drawn (\d+) strips (\d+) marks", seen)
    print("flight: %s" % (drawn.group(0) if drawn else "no runways drawn: %r"
                          % re.findall(r"flight runways not drawn[^\r\n]*", seen)))
    if not drawn or int(drawn.group(1)) < 4 or int(drawn.group(2)) < 40 * int(drawn.group(1)):
        fails.append("the runways near Rio were not drawn, strips and paint")
    # The weather (W1): flown under an overcast deck 900 m over the field -
    # made, placed, and the sky the deck's grey with the sun's share a
    # quarter.
    deck = re.search(r"flight cloud deck made (\d+) triangles at (-?\d+) m", seen)
    under = re.search(r"flight weather cloud under haze=(\d+)-(\d+) sun=([\d.]+)", seen)
    print("flight: %s; %s" % (deck.group(0) if deck else "no cloud deck made",
                              under.group(0) if under else "never under a deck"))
    if not deck or int(deck.group(1)) < 10000 or not under or float(under.group(3)) > 0.3:
        fails.append("the landing was not flown under a cloud deck: %r"
                     % re.findall(r"flight (?:weather|cloud)[^\r\n]*", seen))
    # The wind (W2): 25 kt from the north, down SDU's 02R - the wheels meet
    # the runway at the air's speed less the headwind.
    wind = re.search(r"flight wind from (\d+) at (\d+) kt", seen)
    touch = re.search(r"flight touched down at (\d+) kt through the air, (\d+) kt over the ground",
                      seen)
    print("flight: %s; %s" % (wind.group(0) if wind else "no wind line",
                              touch.group(0) if touch else "no touchdown speeds"))
    if not wind or not touch or int(touch.group(1)) - int(touch.group(2)) < 15:
        fails.append("the landing was not into the wind: %r"
                     % (touch.group(0) if touch else wind and wind.group(0)))
    # Under the cloud deck the runway's lights are on (F8c).
    if not re.search(r"flight runway lights on", seen):
        fails.append("the runway lights were not on under the cloud: %r"
                     % re.findall(r"flight runway lights[^\r\n]*", seen))
    # The tyres (Diego: "the landing sound like wheels squeaking"): struck
    # at touchdown, as hard as the sink rate - when the sound is on.
    squeak = re.search(r"flight squeak at ([\d.]+)", seen)
    sound = "flight sound on" in seen
    print("flight: %s (sound %s)" % (squeak.group(0) if squeak else "no squeak", sound))
    if landed and sound and not squeak:
        fails.append("the tyres were not heard at touchdown")
    # Stopping on the runway at idle: each half's slowing, from the frames'
    # speeds, and the distance it would roll from the speed it had. A
    # C172's braked roll is about 180 m and its free one some 400: braked
    # under 250 m, and slowing at least 0.5 m/s2 more than without.
    def slowing(pair):
        a, b = pair
        return (a["v"] - b["v"]) / ((b["frame"] - a["frame"]) * 0.05), a["v"]
    if landed and roll:
        free, v0 = slowing(roll["free"])
        braked, v1 = slowing(roll["braked"])
        print("flight: rolling at idle %.2f m/s2 (would roll %.0f m from %.1f m/s), "
              "braked %.2f m/s2 (%.0f m from %.1f)"
              % (free, v0 * v0 / (2 * max(free, 0.01)), v0,
                 braked, v1 * v1 / (2 * max(braked, 0.01)), v1))
        # The control is the free roll: into this landing's 25 kt the
        # wings still carry some of the weight, so the brakes add less
        # than on a calm day - but they must add.
        if braked < free + 0.5 or v1 * v1 / (2 * max(braked, 0.01)) > 250:
            fails.append("the brakes did not stop it within 250 m: %.2f m/s2 braked, %.2f free"
                         % (braked, free))
    elif landed:
        fails.append("the roll was not measured")
    if crashed or not landed:
        fails.append("it did not land: %r" % (crashed.group(0) if crashed else "no touchdown"))
    elif int(landed.group(1)) >= 600 or landed.group(2).strip() != "on SDU 02R/20L":
        fails.append("the landing was not under 600 fpm on SDU 02R/20L: %r" % landed.group(0))
    if fails:
        for f in fails:
            print("FAIL: " + f)
        return 1
    print("PASS: Flight landed on SDU's 02R from a short final, hands off - %s fpm, %s"
          % (landed.group(1), landed.group(2).strip()))
    return 0


def main():
    built = os.path.join(APPS, "k3dvk.elf")
    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` builds it" % built)
        return 1
    image = os.path.join(WORK, "k3dvk.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)

    httpd, asked = tile_server()
    base = "http://10.0.2.2:%d" % httpd.server_address[1]
    with open(os.path.join(WORK, "setup.lua"), "w") as f:
        f.write(SETUP % dict(base="http://10.0.2.2:9" if ISLAND else base, lon=LON, lat=LAT,
                             width=WIDTH, height=HEIGHT,
                             shadows="false" if NO_SHADOWS else "true"))
        if ISLAND:
            # Begun on Rio's runway, as the M700 was: going offline has to
            # take the flight off a field that is not there.
            f.write('prefs.open("flight", {}):set{ city = "rio", start = "runway" }\n')
        if (B747 or LAND) and not os.environ.get("KOSMOS_FLIGHT_SHOT"):
            # Neither the takeoff nor the landing is held to a shadow or a
            # building - `x86-flight` is - and under TCG they are most of a
            # frame: the sun's map draws the scene a second time. A frame
            # here is a twentieth of a second of flight however long it
            # takes, so what they cost is wall clock and nothing else. A
            # picture for a person keeps them.
            f.write('prefs.open("flight", {}):set{ shadows = false, buildings = false }\n')
        if B747:
            # The terrain moved every 400 m (F10a) - twice in the roll -
            # where a flight moves it every 40 km.
            f.write('prefs.open("flight", {}):set{ city = "rio", start = "runway", aircraft = "%s",'
                    ' weather = "clear", wind = 0, anchor_after = 400, fastest = 16, lon = -43.205,'
                    ' lat = -22.955, fly_to = "GIG" }\n'
                    % ("b747instant" if INSTANT else "b747"))
        if LAND:
            # 150 m out on a three-degree path: eight metres up, four seconds
            # of flying, eighty frames at the gate's pace.
            f.write('prefs.open("flight", {}):set{ city = "rio", start = "final", final = 150,'
                    ' lon = -43.205, lat = -22.955, weather = "%s", wind = %d,'
                    ' wind_from = 0 }\n' % ("clear" if CLEAR else "cloud", 0 if CALM else 25))
            if os.environ.get("KOSMOS_FLIGHT_TIME"):            # a picture's hour, for a person
                f.write('prefs.open("flight", {}):set{ time = "%s" }\n'
                        % os.environ["KOSMOS_FLIGHT_TIME"])
    files = [(os.path.join(WORK, "setup.lua"), FOLDER + "/setup.lua"), (image, FOLDER + "/k3dvk.elf")]
    # Its program and its model (F10b), its aircraft's models, its airports
    # (F8) and its aircraft files (F12), as `installed.py` puts them beside it.
    import installed                                         # noqa: E402
    files += [(here, FOLDER + "/" + inside) for here, inside in installed.programs("Flight")]
    files += [(os.path.join(ROOT, there), FOLDER + "/" + here)
              for there, here in installed.ASSETS["Flight"]]
    files += [(here, FOLDER + "/" + inside) for here, inside in installed.flight_made()]
    # Two aircraft files more (F12): the Cessna under another name, to be
    # chosen in the sheet and flown, and the Cessna with its mass left out -
    # the control, which must be refused by that number's name.
    cessna = open(os.path.join(ROOT, "user", "installed", "Flight", "Aircraft",
                               "cessna172.lua")).read()
    jumbo = open(os.path.join(ROOT, "user", "installed", "Flight", "Aircraft", "b747.lua")).read()
    made_files = [("testplane", cessna.replace('name = "Cessna 172"', 'name = "Test Cessna"')),
                  ("nomass", cessna.replace("mass = 1043, ", ""))]
    if INSTANT:
        made_files.append(("b747instant", jumbo.replace("spool_s = 6", "spool_s = 0.05")))
    for name, text in made_files:
        if text == cessna:
            raise SystemExit("run_flight: the Cessna's file no longer has what %s changes" % name)
        made = os.path.join(WORK, name + ".lua")
        with open(made, "w") as f:
            f.write(text)
        files.append((made, FOLDER + "/Aircraft/" + name + ".lua"))
    megabytes = os.path.getsize(image) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK, str(megabytes + 80)]
                   + ["%s:%s" % f for f in files], check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import random
    import run_screenshot as R                               # noqa: E402
    import run_servers as SV                                 # noqa: E402

    # The desktop as the machine starts it, so the window manager holds
    # `/Tiles` and hands it to a program that says it needs it - which a
    # window manager started at the prompt does not; Flight opened over
    # Telnet, as a person opens it from the Deskbar.
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    # A machine's memory, not a suite's 512 MB: Flight holds chunks for the
    # memory it finds, and every machine it is for has gigabytes.
    i = R.X86_ARGS.index("-m")
    R.X86_ARGS = R.X86_ARGS[:i + 1] + ["1536M"] + R.X86_ARGS[i + 2:]
    # A sound card writing what it is given to a WAV here (F6c), as Doom's
    # suite hears its demo: the engine is checked by what reached it.
    heard = os.path.join(WORK, "heard.wav")
    if os.path.exists(heard):
        os.remove(heard)
    # And two processors, as every machine Flight is for has more: the sound
    # is a thread in the audio band and the audio server's mixing, a
    # quarter of one TCG processor, which on one left Flight 0.8 frames a
    # second and the overlays' pictures taken before they were drawn.
    if "-smp" not in R.X86_ARGS:
        R.X86_ARGS = R.X86_ARGS + ["-smp", "2"]
    R.X86_ARGS = R.X86_ARGS + ["-audiodev", "wav,id=snd0,path=%s" % heard,
                               "-device", "ich9-intel-hda", "-device", "hda-output,audiodev=snd0"]
    # The pads' page writable by `padtest` (F7): a pad QEMU cannot plug in.
    guest = SV.boot(IMAGE, telnet, web, extra=["-fw_cfg", "name=opt/kosmos/pad,string=test"])
    fails = []
    screen = None
    noon_screen = dusk_screen = None
    flight_left = None
    flight_box = None
    seen = ""

    def latest(after=0):
        s = states(guest.seen[mark:])
        return [t for t in s if t["frame"] > after]

    def wait_frames(count, what, limit=180):
        """Until `count` more reports than now have come; the newest."""
        have = len(latest())
        until = time.monotonic() + limit
        while time.monotonic() < until:
            guest._read_available()
            s = latest()
            if len(s) >= have + count:
                return s[-1]
            if "flight failed=" in guest.seen[mark:]:
                raise R.Failure(guest.seen[mark:].split("flight failed=")[1][:300])
            time.sleep(0.2)
        raise R.Failure("no frames came while " + what)

    def keys(down, name):
        guest._qmp("input-send-event", {"events": [
            {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": name}}}]})

    mark = 0
    try:
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = SV.connect(telnet)
        mark = len(guest.seen)
        said = session.run(FOLDER + "/setup.lua").decode(errors="replace")
        if "flight setup done" not in said:
            raise R.Failure("the settings were not written: %r" % said[-300:])
        session.run("open " + FOLDER + "/flight.lua")
        guest.wait_for("flight device=", "the window opened")

        if B747:
            # The time 16x, then Q with a route set (F10d): the autopilot
            # takes off on its own - full power, the approach's flaps, the
            # yoke back at the 747's rotate speed - from the runway it is on.
            # At 16x and not 4x: each of the model's steps is the same
            # twentieth of a second, sixteen a frame, so the roll is the
            # same roll in forty frames rather than a hundred and sixty -
            # under TCG a frame is a second, and the roll was most of this.
            guest.wait_for("flight started runway", "on Rio's runway")
            wait_frames(2, "on the runway")
            for rate in (2, 3, 4, 8, 16):
                have = guest.seen.count("flight time %dx" % rate)
                keys(True, "x")
                time.sleep(0.1)
                keys(False, "x")
                until = time.monotonic() + 60
                while guest.seen.count("flight time %dx" % rate) == have and time.monotonic() < until:
                    guest._read_available()
                    time.sleep(0.1)
            keys(True, "q")
            time.sleep(0.1)
            keys(False, "q")
            wait_frames(1, "pressed Q")
            # Waited for by frames, not seconds: the roll is about 40
            # frames at 16x (170 at 4x), and a frame under TCG is one
            # second alone and three beside the gate's other suites. 260
            # frames, and a wall that only a stuck machine meets.
            rolling = latest()
            first = rolling[-1]["frame"] if rolling else 0
            until = time.monotonic() + 1500
            while time.monotonic() < until and not re.search(
                    r"flight (lifted off at|crashed:)", guest.seen[mark:]):
                guest._read_available()
                now_at = latest()
                if now_at and now_at[-1]["frame"] - first > 260:
                    break
                time.sleep(0.2)
            wait_frames(2, "climbing away")
            if os.environ.get("KOSMOS_FLIGHT_SHOT"):        # the 747 climbing, for a person
                time.sleep(3)
                R.write_png(os.environ["KOSMOS_FLIGHT_SHOT"], guest.screendump())
            return b747_checks(guest.seen[mark:])

        if LAND:
            # At 4x, a flare Flight steps four times a frame (F11) with the
            # ground asked for under each step: the same landing in a
            # quarter of the frames, and the roll's slowing measured over
            # four times the flight.
            guest.wait_for("flight started final", "on final")
            for rate in (2, 3, 4):
                have = guest.seen.count("flight time %dx" % rate)
                keys(True, "x")
                time.sleep(0.1)
                keys(False, "x")
                until = time.monotonic() + 60
                while guest.seen.count("flight time %dx" % rate) == have and time.monotonic() < until:
                    guest._read_available()
                    time.sleep(0.1)
            if os.environ.get("KOSMOS_FLIGHT_PUSH"):        # the control: the yoke forward
                keys(True, "up")
            until = time.monotonic() + 300
            while time.monotonic() < until and not re.search(
                    r"flight (landed at|crashed:)", guest.seen[mark:]):
                guest._read_available()
                time.sleep(0.2)
            # Down: the throttle closed (0), and the slowing measured
            # first without the brakes - the control - then with Space
            # held (Diego, 21 November: "a way to stop the plane in the
            # runway"). Under TCG a frame is 0.05 s of flight, the cap on
            # its step, so the stop itself is minutes away; its distance
            # is reckoned from the slowing.
            roll = {}
            if re.search(r"flight landed at", guest.seen[mark:]):
                # Back to 1x for the roll: its slowing is reckoned a frame
                # at a twentieth of a second.
                have = guest.seen.count("flight time 1x")
                keys(True, "x")
                time.sleep(0.1)
                keys(False, "x")
                until = time.monotonic() + 60
                while guest.seen.count("flight time 1x") == have and time.monotonic() < until:
                    guest._read_available()
                    time.sleep(0.1)
                keys(True, "0")
                time.sleep(0.2)
                keys(False, "0")
                wait_frames(2, "the throttle closed")
                roll["free"] = (wait_frames(1, "rolling"), wait_frames(6, "rolling on"))
                keys(True, "spc")
                wait_frames(2, "the brakes on")
                roll["braked"] = (wait_frames(1, "braking"), wait_frames(6, "braking on"))
                keys(False, "spc")
            if os.environ.get("KOSMOS_FLIGHT_SHOT"):        # on the runway, for a person
                time.sleep(3)
                R.write_png(os.environ["KOSMOS_FLIGHT_SHOT"], guest.screendump())
            return land_checks(guest.seen[mark:], roll)

        if ISLAND:
            guest.wait_for("flight offline:", "went offline")
            island = wait_frames(8, "over the island")
            if os.environ.get("KOSMOS_FLIGHT_SHOT"):        # the island built, for a person
                until = time.monotonic() + 240
                while time.monotonic() < until:
                    m = re.match(r"drawn=(\d+) of (\d+) ready=\d+ building=(\d+)", island["drawn"])
                    if m and m.group(1) == m.group(2) and m.group(3) == "0":
                        break
                    island = wait_frames(1, "the island built")
                R.write_png(os.environ["KOSMOS_FLIGHT_SHOT"], guest.screendump())
            seen = guest.seen[mark:]
            return island_checks(seen, island, asked, guest)

        # Flying straight while the tiles come.
        before = wait_frames(6, "flying straight")
        # Flight in front and with the keys: the session's own windows open
        # beside it and, on a slow machine, one can open after it and over
        # it - which took the panels' pictures and the keys both. A click
        # inside its picture, clear of the view chip, raises and focuses it.
        win = re.search(r"wm: window Flight at (\d+),(\d+) (\d+)x(\d+)", guest.seen[mark:])
        flight_left = int(win.group(1)) if win else None
        flight_box = tuple(int(v) for v in win.groups()) if win else None
        if win:
            wx, wy, ww, wh = (int(v) for v in win.groups())
            guest.mouse_to((wx + ww * 0.8) * 32767 / 1919, (wy + wh * 0.7) * 32767 / 1079)
            time.sleep(0.2)
            guest.mouse_button(True)
            time.sleep(0.1)
            guest.mouse_button(False)
            # And the pointer taken off the window: left on its strip, the
            # arrow is drawn into the frame over the strip's words, which
            # the panels' checks find the window by, and they looked in the
            # wrong place (21 November, its screen kept).
            time.sleep(0.1)
            guest.mouse_to(32767, 32767)
            wait_frames(2, "Flight raised")
        if os.environ.get("KOSMOS_FLIGHT_SHOT"):
            # A picture for a person: the ground built first.
            until = time.monotonic() + 240
            while time.monotonic() < until:
                m = re.match(r"drawn=(\d+) of (\d+) ready=\d+ building=(\d+)", before["drawn"])
                if m and m.group(1) == m.group(2) and m.group(3) == "0":
                    break
                before = wait_frames(1, "the ground built")

        # The right arrow held: it rolls right and turns east.
        keys(True, "right")
        rolled = wait_frames(4, "the right arrow held")
        keys(False, "right")
        level = wait_frames(6, "let go")

        # A game controller's left stick full left (F7): it rolls left -
        # through the pads' page, the window manager and the 3D Kit, with
        # no key down. Then the pad unplugged, and the yoke let go.
        pad_said = session.run("padtest 0 %s 0 0 0 0 0" % ("0" if NO_PAD else "-1")).decode(
            errors="replace")
        padded = wait_frames(6, "the pad's stick left")
        session.run("padtest 0 off")
        unpadded = wait_frames(4, "the pad unplugged")

        chase = guest.screendump()                          # the strip is looked for in it
        if os.environ.get("KOSMOS_FLIGHT_SHOT"):            # the chase view, for a person
            R.write_png(os.environ["KOSMOS_FLIGHT_SHOT"].replace(".png", "-chase.png"), chase)

        # The time faster (F11): X once is 2x, the model stepped twice a
        # frame, so a frame carries it twice as far for its speed. The
        # control is the same measure at 1x just before; then X three more
        # times is 3x, 4x and back to 1x.
        def carried(what):
            a = wait_frames(1, what)
            b = wait_frames(3, what + " on")
            frames = max(1, b["frame"] - a["frame"])
            speed = max(1.0, (a["v"] + b["v"]) / 2)
            return math.hypot(b["x"] - a["x"], b["z"] - a["z"]) / (frames * speed), b
        def press_x(rate):
            have = guest.seen.count("flight time %dx" % rate)
            keys(True, "x")
            time.sleep(0.1)
            keys(False, "x")
            until = time.monotonic() + 60
            while guest.seen.count("flight time %dx" % rate) == have and time.monotonic() < until:
                guest._read_available()
                time.sleep(0.1)
        faster = {}
        faster["1x"], _ = carried("at 1x")
        press_x(2)
        wait_frames(1, "2x taken")
        faster["2x"], _ = carried("at 2x")
        for rate in (3, 4, 1):
            press_x(rate)
        faster["back"] = wait_frames(1, "back at 1x")

        # The autopilot (F10b): Q holds what the aircraft is doing, the right
        # arrow held turns the heading it holds rather than the aircraft,
        # and Q again lets go and says what it was holding by then. Its
        # arithmetic is held on the host (`tools/test_flightmodel.lua`); this
        # is the key, the strip and the arrows' meaning.
        def press_q(word):
            have = guest.seen.count("flight autopilot " + word)
            keys(True, "q")
            time.sleep(0.1)
            keys(False, "q")
            until = time.monotonic() + 60
            while guest.seen.count("flight autopilot " + word) == have and time.monotonic() < until:
                guest._read_available()
                time.sleep(0.1)
        press_q("on")
        wait_frames(1, "the autopilot on")
        if os.environ.get("KOSMOS_FLIGHT_SHOT"):            # the strip under the autopilot
            wait_frames(2, "the strip redrawn")
            R.write_png(os.environ["KOSMOS_FLIGHT_SHOT"].replace(".png", "-autopilot.png"),
                        guest.screendump())
        keys(True, "right")
        wait_frames(6, "the right arrow held under the autopilot")
        keys(False, "right")
        press_q("off")
        wait_frames(1, "the autopilot off")

        # The yoke back: it climbs.
        start = faster["back"]
        keys(True, "down")
        climbed = wait_frames(5, "the down arrow held")
        keys(False, "down")
        later = guest.screendump()                          # the strip again, the numbers moved

        # V: the next view.
        keys(True, "v")
        time.sleep(0.2)
        keys(False, "v")
        viewed = wait_frames(2, "V pressed")

        time.sleep(1.0)
        screen = guest.screendump()
        if os.environ.get("KOSMOS_FLIGHT_SHOT"):
            R.write_png(os.environ["KOSMOS_FLIGHT_SHOT"], screen)

        # V again: the tower, on Carrasco's field (F5h).
        keys(True, "v")
        time.sleep(0.2)
        keys(False, "v")
        towered = wait_frames(2, "V pressed for the tower")

        # The pause sheet (F5i), by the keyboard as a person would: P, a
        # search for "paris", Rio chosen from Recent, the runway, Restart.
        # Each key waits for the sheet to say it changed - two presses in
        # one frame are one.
        def sheet_key(name, what):
            have = guest.seen.count("flight sheet")
            keys(True, name)
            time.sleep(0.1)
            keys(False, name)
            until = time.monotonic() + 30
            while time.monotonic() < until:
                guest._read_available()
                if guest.seen.count("flight sheet") > have:
                    return
                time.sleep(0.1)
            raise R.Failure("the sheet did not answer " + what)

        sheet_key("p", "P")
        for c in "paris":
            sheet_key(c, "typing " + c)
        keys(True, "ret")
        time.sleep(0.1)
        keys(False, "ret")
        until = time.monotonic() + 60
        while time.monotonic() < until and "flight sheet found" not in guest.seen[mark:]:
            guest._read_available()
            time.sleep(0.2)
        time.sleep(1.0)                                     # the found row, drawn
        guest._read_available()
        sheet_key("down", "down to Found")
        sheet_key("down", "down to Recent")
        for _ in range(4):
            sheet_key("left", "left along Recent")
        sheet_key("ret", "choosing Rio")
        sheet_key("down", "down to Start")
        sheet_key("right", "right to the runway")
        sheet_key("ret", "choosing the runway")
        # The aircraft (F12): the second file, by its name after the Cessna.
        sheet_key("down", "down to Aircraft")
        sheet_key("right", "right to the Test Cessna")
        sheet_key("ret", "choosing the Test Cessna")
        # Fly to (F10d-2): None, EZE from the setup, then the airports the
        # search for "paris" found; right once from EZE, the first of them.
        sheet_key("down", "down to Fly to")
        sheet_key("right", "right to the first airport found")
        sheet_key("ret", "choosing it to fly to")
        if os.environ.get("KOSMOS_FLIGHT_SHOT"):            # the sheet with Fly to, for a person
            wait_frames(1, "the sheet redrawn")             # the choice is drawn a frame on
            R.write_png(os.environ["KOSMOS_FLIGHT_SHOT"].replace(".png", "-sheet.png"),
                        guest.screendump())
        guest.wait_for("flight aircraft Test Cessna from", "the Test Cessna flown")
        sheet_key("down", "down to Time")
        sheet_key("down", "down to Weather")
        sheet_key("down", "down to Wind")
        sheet_key("down", "down to From")
        sheet_key("down", "down to the buttons")
        sheet_key("left", "left to Restart")
        sheet_key("ret", "Restart")
        guest.wait_for("flight started runway", "the restart")
        rio = wait_frames(3, "on Rio's runway")

        # The keys (F6h): F1 lays every key where the sheet sits, and F1
        # again closes it. Its control is the count before: no page laid
        # until F1 is pressed.
        keys_before = guest.seen.count("flight keys shown")
        keys(True, "f1")
        time.sleep(0.1)
        keys(False, "f1")
        guest.wait_for("flight keys shown", "the keys page")
        keys(True, "f1")
        time.sleep(0.1)
        keys(False, "f1")
        guest.wait_for("flight keys closed", "the keys page closed")

        # The time of day (F6a): T twice from noon is dusk, and the
        # horizon turns orange. Counted against the frame at noon, since
        # the desktop around the window has colours of its own.
        noon_screen = guest.screendump()
        for step in ("afternoon", "dusk"):
            have = guest.seen.count("flight time ")
            keys(True, "t")
            time.sleep(0.1)
            keys(False, "t")
            until = time.monotonic() + 30
            while time.monotonic() < until and guest.seen.count("flight time ") == have:
                guest._read_available()
                time.sleep(0.1)
        wait_frames(3, "at dusk")
        time.sleep(0.5)
        dusk_screen = guest.screendump()
        if os.environ.get("KOSMOS_FLIGHT_SHOT"):
            time.sleep(1.0)
            R.write_png(os.environ["KOSMOS_FLIGHT_SHOT"].replace(".png", "-tower.png"),
                        guest.screendump())
    except R.Failure as e:
        fails.append("the flight stopped: %s" % e)
    finally:
        seen = guest.seen[mark:]
        guest.close()
        httpd.shutdown()

    if fails:
        print("FAIL: " + fails[0])
        # The sheet's and the start's lines, which a frame's report - two
        # thousand characters of chunks - would push out of the last words.
        said = re.findall(r"flight (?:sheet|started|restarted|place|tower)[^\r\n]*|"
                          r"[^\r\n]*(?:error|traceback)[^\r\n]*", seen)
        print("--- what the sheet said ---\n" + "\n".join(said[-30:]))
        print("--- the guest's last words ---\n" + seen[-2000:])
        return 1

    print("flight: straight %r" % before)
    print("flight: rolled   %r" % rolled)
    print("flight: padded   %r" % padded)
    print("flight: padtest  %r" % pad_said.strip()[-120:])
    print("flight: level    %r" % level)
    print("flight: climbed  %r" % climbed)
    print("flight: viewed   %r" % viewed)
    print("flight: the Mac served %d elevation and %d imagery tiles" % (asked["elev"], asked["img"]))

    a = re.search(r"flight aircraft parts=(\d+) triangles=(\d+) textures=(\d+) of (\d+) used, "
                  r"\d+ in the file size=([\d.]+)x([\d.]+)x([\d.]+)", seen)
    print("flight: %s" % (a.group(0) if a else "no aircraft line"))
    if not a or a.group(2) != "77317" or a.group(3) != a.group(4) or a.group(4) != "10":
        fails.append("the Cessna was not put on the device whole - 77317 triangles and the 10 "
                     "textures its parts are drawn with: %r" % (a.group(0) if a else re.findall(r"flight no aircraft.*", seen)))
    elif abs(float(a.group(5)) - 8.15) > 0.05 or abs(float(a.group(7)) - 10.92) > 0.05:
        fails.append("the Cessna is not 8.15 m long and 10.92 m across: %r" % a.group(0))
    # The buildings (B3): every served tile's 37 stood up, none refused, and
    # of the triangles they make - so the reader, the extruder and the
    # upload each had the whole of it.
    b = re.findall(r"flight buildings tiles=(\d+) ready=(\d+) buildings=(\d+) refused=(\d+) "
                   r"triangles=(\d+) roofed=(\d+) pieces=(\d+)", seen)
    print("flight: the Mac served %d vector tiles; buildings %r" % (asked["vec"], b[-1] if b else None))
    if not b:
        fails.append("Flight said nothing of its buildings")
    else:
        # The report with the most tiles ready: a restart at another place
        # gives the buildings back and stands them up again.
        best = max(b, key=lambda r: int(r[1]))
        tiles_, ready, made, refused, triangles, roofed, pieces = map(int, best)
        if asked["vec"] == 0 or ready == 0:
            fails.append("no tile of buildings was stood up: %d asked, %r" % (asked["vec"], b[-1]))
        elif made != ready * BUILDINGS_A_TILE or refused != 0 or \
                triangles != ready * TRIANGLES_A_TILE:
            fails.append("the buildings are not the tiles': %d tiles ready, %d buildings of %d, "
                         "%d refused, %d triangles of %d" % (ready, made, ready * BUILDINGS_A_TILE,
                                                             refused, triangles,
                                                             ready * TRIANGLES_A_TILE))
        # The roofs (B4): their photograph asked for, piece by piece at zoom
        # 16, and a tile's roofs wearing it once its sixteen had come - in
        # any report, since a restart at another place makes them again.
        roofed = max(int(r[5]) for r in b)
        pieces = max(int(r[6]) for r in b)
        print("flight: the Mac was asked for %d roof pieces; %d held, %d tiles roofed"
              % (asked["roof"], pieces, roofed))
        if asked["roof"] < 16 or pieces < 16 or roofed < 1:
            fails.append("no tile's roofs wear the photograph: %d pieces asked, %d tiles roofed"
                         % (asked["roof"], roofed))
    # The shadows (S2): the sun's map drawn - the aircraft and the
    # buildings cast into it - over a box between 400 m and 2 km.
    sh = re.search(r"flight shadows radius=(\d+) texel=([\d.]+) triangles=(\d+)", seen)
    print("flight: %s" % (sh.group(0) if sh else "no shadows line"))
    if not sh:
        fails.append("nothing was drawn into the sun's map: %r" % re.findall(r"flight shadows.*", seen))
    elif not 400 <= int(sh.group(1)) <= 2000 or int(sh.group(3)) < 1000:
        fails.append("the sun's map is not the aircraft's and the buildings': %r" % sh.group(0))
    if asked["elev"] == 0 or asked["img"] == 0 or viewed["tiles"] == 0:
        fails.append("tiles were not fetched and handed: %d elevation, %d imagery asked, %d handed"
                     % (asked["elev"], asked["img"], viewed["tiles"]))
    if rolled["roll"] < 5:
        fails.append("the right arrow held did not roll it right: %d degrees" % rolled["roll"])
    turned = (level["heading"] - before["heading"] + 540) % 360 - 180
    if turned < 2:
        fails.append("rolled right, it did not turn east: %d degrees" % turned)
    if "flight pad in front" not in seen:
        fails.append("Flight never saw the pad: %r" % pad_said.strip()[-200:])
    if padded["roll"] > level["roll"] - 8:
        fails.append("the pad's stick full left did not roll it left: %d degrees, from %d"
                     % (padded["roll"], level["roll"]))
    if abs(level["roll"]) >= abs(rolled["roll"]):
        fails.append("let go, it did not roll back towards level: %d then %d"
                     % (rolled["roll"], level["roll"]))
    if climbed["y"] <= start["y"]:
        fails.append("the yoke back, it did not climb: %d m then %d m" % (start["y"], climbed["y"]))
    # The strip (F5e): said once, in the flight model's numbers, and on the
    # screen as a band with words in it.
    s = re.search(r"flight strip SPEED (\d+) kt  ALTITUDE (-?\d+) ft  HEADING (\d+)", seen)
    print("flight: %s" % (s.group(0) if s else "no strip line"))
    if not s:
        fails.append("the strip was never drawn: %r" % re.findall(r"flight strip.*", seen))
    elif abs(int(s.group(1)) - before["v"] * 1.94384) > 15:
        fails.append("the strip's speed, %s kt, is not the flight's %.0f" % (s.group(1),
                                                                             before["v"] * 1.94384))
    band = strip_band(R, chase, WIDTH, left_near=flight_left, box=flight_box)
    print("flight: the strip on the screen %r (left, top row, rows, white pixels)" % (band,))
    # At the gate's size the window stands clear of the desktop's dark
    # panels; a larger one for a picture (KOSMOS_FLIGHT_SIZE) may touch the
    # Log's, which no band test can tell from the strip - said, not held.
    if os.environ.get("KOSMOS_FLIGHT_SIZE"):
        print("flight: the strip's place on the screen is not held at %dx%d" % (WIDTH, HEIGHT))
    elif band is None or not 34 <= band[2] <= 46 or band[3] < 20:
        fails.append("no strip on the chase view's screen - a dark band 30 to 48 rows tall with "
                     "words in it: %r" % (band,))
    else:
        # And it follows the flight: after the roll and the climb its
        # numbers are others, with the graphics panel in its own place over
        # the same frame. On the M700 it kept its first numbers for as long
        # as the graphics panel was shown (20 November).
        first, again = strip_words_at(R, chase, band), strip_words_at(R, later, band)
        print("flight: the strip's words %d then %d light pixels, %d of them moved"
              % (len(first), len(again), len(first ^ again)))
    # And it follows the flight: laid again as its numbers change, and the
    # last it was laid saying the speed the flight model has at that report.
    # On the M700 it kept 105 kt while the flight went on at 140 (20 November).
    # Each "strip redrawn" line follows its frame's report, so it is
    # paired with the report just before it; in the cockpit there is no
    # strip, and it says nothing.
    pairs, last = [], None
    for line in seen.splitlines():
        st = states(line)
        if st:
            last = st[0]
        m = re.search(r"flight strip redrawn=(\d+) shows SPEED (\d+) kt", line)
        if m and last:
            pairs.append((last["frame"], int(m.group(1)), int(m.group(2)),
                          round(last["v"] * 1.94384)))
    print("flight: the strip (frame, times laid, its kt, the flight's kt) %r" % (pairs,))
    if len(pairs) < 2 or pairs[-1][1] <= pairs[0][1]:
        fails.append("the strip was not laid again as the flight went on: %r" % (pairs,))
    elif any(abs(p[2] - p[3]) > 15 for p in pairs):
        fails.append("the strip showed another speed than the flight's: %r" % (pairs,))
    # The panel (F5f): the cockpit's six dials, said once with what they
    # read, and on the screen as a dark block the panel's size with the
    # dials' ticks and needles light in it.
    p = re.search(r"flight panel (\d+)x(\d+) AIRSPEED (\d+) kt", seen)
    whole = re.search(r"flight panel [^\r\n]*", seen)
    print("flight: %s" % (whole.group(0) if whole else "no panel line"))
    if not p:
        fails.append("the cockpit's panel was never drawn: %r" % re.findall(r"flight panel.*", seen))
    else:
        pw, ph = int(p.group(1)), int(p.group(2))
        if abs(int(p.group(3)) - viewed["v"] * 1.94384) > 15:
            fails.append("the panel's airspeed, %s kt, is not the flight's %.0f"
                         % (p.group(3), viewed["v"] * 1.94384))
        # Where it must be: the strip found the window's picture - its left
        # edge, and its foot STRIP_H rows below the band's top - and the
        # panel is centred on that foot. Looked at there: a good part of it
        # the panel's near-black - at the gate's size a dial is fourteen
        # pixels across and half of it ticks and needle - with the dials'
        # light in it, and the row above it the sky's. "Not the sky" was
        # tried first, and the ground under the panel passed for it (its
        # control, 20 November).
        block = panel_seen(R, screen, band, pw, ph) if band else None
        print("flight: the panel on the screen %r (near-black fraction, light pixels, "
              "sky above)" % (block,))
        if os.environ.get("KOSMOS_FLIGHT_SIZE"):
            pass
        elif block is None or block[0] < 0.3 or block[1] < 12 or block[2] < 0.5:
            kept = os.path.join(KEPT, "panel-failed.png")
            R.write_png(kept, screen)
            fails.append("no panel on the cockpit's screen - a near-black block %dx%d with "
                         "dials in it: %r (the screen: %s)" % (pw, ph, block, kept))
    # The graphics panel (F5g): said once with the 3D Kit's figures for a
    # frame - its draw calls, at least the sky's and the ground's, the
    # triangles, and the device's own time for it - and on the chase view's
    # screen where it says, near-black with its words light in it. At the
    # gate's size it is the frames a second alone, in a small box.
    gr = re.search(r"flight graphics (\d+)x(\d+) at (\d+),(\d+) device=(\S+) draws=(\d+) "
                   r"triangles=(\d+) gpu_ms=(\S+) frame_ms=([\d.]+) processor_ms=([\d.]+) "
                   r"presented=([^\r\n]*)", seen)
    print("flight: %s" % (gr.group(0) if gr else "no graphics line"))
    if not gr:
        fails.append("the graphics panel never said what it showed: %r"
                     % re.findall(r"flight graphics.*", seen))
    else:
        if int(gr.group(6)) < 2 or int(gr.group(7)) < 100:
            fails.append("the graphics panel's figures are not a frame's: %d draws, %d triangles"
                         % (int(gr.group(6)), int(gr.group(7))))
        # The frame waits for the device, so the device's time for it is
        # inside the frame's.
        if gr.group(8) == "none" or not 0 < float(gr.group(8)) <= float(gr.group(9)):
            fails.append("the device's time for a frame, %s ms, is not inside the frame's %s ms"
                         % (gr.group(8), gr.group(9)))
        # And the processor's part is what is left of the frames, so more
        # than nothing: Lua flew and the 3D Kit recorded.
        if not 0 < float(gr.group(10)) < float(gr.group(9)) * 10:
            fails.append("the processor's time a frame, %s ms, is not a part of the frames'"
                         % gr.group(10))
        gw, gh, gx, gy = (int(gr.group(i)) for i in range(1, 5))
        box = panel_seen(R, chase, band, gw, gh, (gx, gy)) if band else None
        print("flight: the graphics panel on the screen %r (near-black fraction, light pixels, "
              "sky above)" % (box,))
        if os.environ.get("KOSMOS_FLIGHT_SIZE"):
            pass
        elif box is None or box[0] < 0.3 or box[1] < 8:
            kept = os.path.join(KEPT, "graphics-failed.png")
            R.write_png(kept, chase)
            fails.append("no graphics panel on the chase view's screen - a near-black box "
                         "%dx%d at %d,%d with words in it: %r (the screen: %s)"
                         % (gw, gh, gx, gy, box, kept))
    # The tower (F5h): on the city's airfield - Carrasco, MVD, about 14 km
    # east-north-east of the bay - in the terrain's own projection, and its
    # lens a sixtieth of the distance to the aircraft, between 1.4 and 400.
    # It stood where the flight began until F5h, and that is its control.
    def mercator(lon, lat):
        r = math.radians(lat)
        return (lon + 180) / 360, (1 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2
    ox, oy = mercator(LON, LAT)
    fx, fy = mercator(-56.0308, -34.8384)
    scale = EQUATOR * math.cos(math.radians(LAT))
    want_x, want_z = (fx - ox) * scale, (fy - oy) * scale
    tw = re.search(r"flight tower at x=(-?\d+) z=(-?\d+) ground=(\S+) distance=(\d+) "
                   r"focal=([\d.]+)", seen)
    print("flight: %s (Carrasco at x=%.0f z=%.0f)" % (tw.group(0) if tw else "no tower line",
                                                      want_x, want_z))
    if towered["view"] != 3:
        fails.append("V twice did not reach the tower: view %d" % towered["view"])
    if not tw:
        fails.append("the tower never said where it stood: %r" % re.findall(r"flight tower.*", seen))
    else:
        tx, tz, d, f = int(tw.group(1)), int(tw.group(2)), int(tw.group(4)), float(tw.group(5))
        if abs(tx - want_x) > 5 or abs(tz - want_z) > 5:
            fails.append("the tower is not on Carrasco's field: x=%d z=%d, the field %.0f %.0f"
                         % (tx, tz, want_x, want_z))
        if abs(f - max(1.4, min(400.0, d / 60.0))) > 0.06:
            fails.append("the tower's lens, %.1f, does not follow its distance, %d m" % (f, d))
    # The place box and the credit (F5h): what they say, from the city and
    # from the sources - the gate's tiles are the Mac's, so the credit names
    # its address; at the gate's size neither fits the window, and says so.
    pl = re.search(r"flight place (.*?) - (.*?) (laid at|not laid)", seen)
    cr = re.search(r"flight credit (Imagery .*?) (laid at|not laid)", seen)
    print("flight: %s" % (pl.group(0) if pl else "no place line"))
    print("flight: %s" % (cr.group(0) if cr else "no credit line"))
    # The dot between is one character, however the console's bytes are read.
    if not pl or not re.fullmatch(r"MVD .{1,3} Carrasco", pl.group(1)):
        fails.append("the place box did not say Montevideo's airfield: %r"
                     % re.findall(r"flight place.*", seen))
    if not cr or not re.fullmatch(r"Imagery 10\.0\.2\.2 .{1,3} Elevation 10\.0\.2\.2", cr.group(1)):
        fails.append("the credit did not name the sources the tiles came from: %r"
                     % re.findall(r"flight credit.*", seen))
    # The sheet (F5i): the search asked the finder and found Paris; Rio
    # and its runway chosen and restarted on: a new terrain centred on Rio,
    # the aircraft on 02R's threshold, stopped and along its heading, the
    # place box and the tower Santos Dumont's. Each from the positions
    # worked out here, in the terrain's projection about Rio's centre.
    print("flight: the finder was asked %r" % (asked["search"],))
    found = re.search(r"flight sheet found (\d+) first ([^\r\n]*)", seen)
    print("flight: %s" % (found.group(0) if found else "no sheet found line"))
    if not asked["search"] or "q=paris" not in asked["search"]:
        fails.append("the sheet's search did not ask the finder for paris: %r" % (asked["search"],))
    if not found or found.group(1) != "1" or not found.group(2).startswith("Paris at 48.857 2.352"):
        fails.append("the sheet did not find Paris where the finder said: %r"
                     % (found.group(0) if found else re.findall(r"flight sheet.*", seen)[-5:]))
    ro, rr = mercator(-43.205, -22.955), mercator(-43.1625, -22.916389)
    rscale = EQUATOR * math.cos(math.radians(-22.955))
    rx, rz = (rr[0] - ro[0]) * rscale, (rr[1] - ro[1]) * rscale
    st = re.search(r"flight started runway at x=(-?\d+) y=(-?\d+) z=(-?\d+) heading=(\d+) "
                   r"in ([^\r\n]*)", seen)
    print("flight: %s (02R's threshold at x=%.0f z=%.0f)" % (st.group(0) if st else "no start line",
                                                           rx, rz))
    print("flight: on the runway %r" % ({k: rio[k] for k in ("x", "y", "z", "v", "heading",
                                                               "ground")},))
    if not st or st.group(5).strip() != "Rio de Janeiro":
        fails.append("Restart did not start on a runway in Rio: %r"
                     % re.findall(r"flight (?:started|restarted|sheet).*", seen)[-6:])
    else:
        if abs(int(st.group(1)) - rx) > 5 or abs(int(st.group(3)) - rz) > 5 or st.group(4) != "357":
            fails.append("the start is not 02R's threshold along 357: %r" % st.group(0))
        if rio["ground"] != "true" or rio["v"] > 5 or abs(rio["x"] - rx) > 10 \
                or abs(rio["z"] - rz) > 10:
            fails.append("after Restart it is not stopped on Rio's runway: %r" % (rio,))
    # Real airports (F8): "paris" found Paris's three large fields from the
    # airports file before the finder answered; Rio's terrain levelled the
    # five runways of its three fields within 30 km; and on 02R's
    # threshold the ground under the wheels is the runway's own 9 feet,
    # 2.74 m, whatever the elevation tiles said - on SDU's 02R/20L.
    fields = re.search(r"flight sheet airports (\d+) first (\w+) runway (\w+)", seen)
    print("flight: %s" % (fields.group(0) if fields else "no airports line"))
    if not fields or fields.group(1) != "3" or fields.group(2) != "LFPB":
        fails.append("the sheet did not find Paris's three airports, Le Bourget first: %r"
                     % re.findall(r"flight sheet airports.*", seen))
    # Fly to (F10d-2): Le Bourget, the first airport found, chosen on the
    # Fly to row - offered there because the City field found it.
    flown_to = re.findall(r"flight fly to (\w+)", seen)
    print("flight: fly to %r" % (flown_to,))
    if flown_to != ["LFPB"]:
        fails.append("the Fly to row did not choose Le Bourget, the first airport found: %r"
                     % (flown_to,))
    flats = re.findall(r"flight flats (\d+) runways of (\d+) airports within 30 km", seen)
    print("flight: flats %r" % (flats,))
    if not flats or flats[-1] != ("5", "3"):
        fails.append("Rio's terrain did not level the 5 runways of its 3 airports: %r" % (flats,))
    under = re.search(r"flight runway ground (-?[\d.]+) m under the wheels, the runway "
                      r"(-?[\d.]+) m, ([^\r\n]*)", seen)
    print("flight: %s" % (under.group(0) if under else "no runway ground line"))
    if not under or abs(float(under.group(1)) - 2.74) > 0.05 or \
            abs(float(under.group(2)) - 2.74) > 0.01 or under.group(3).strip() != "SDU 02R/20L":
        fails.append("the ground on 02R's threshold is not the runway's 2.74 m on SDU 02R/20L: %r"
                     % (under.group(0) if under else re.findall(r"flight runway.*", seen)))
    # The window here is 160 by 100, too small for the page (as for the
    # sheet), so what is held is that F1 opens it with every key and closes
    # it; the page's drawing is seen on the M700.
    # The aircraft as files (F12): the folder's three read, the one with
    # no mass refused for it, and the Test Cessna flown once chosen.
    listed = re.search(r"flight aircraft files: ([^\r\n]*)", seen)
    print("flight: aircraft files %s" % (listed.group(1) if listed else "- no line"))
    if not listed or listed.group(1).strip() != \
            "Boeing 747 (b747), Cessna 172 (cessna172), Test Cessna (testplane); nomass refused: no mass":
        fails.append("the aircraft files were not the Cessna and the test's, with nomass refused: %r"
                     % (listed.group(1) if listed else None))
    flown = re.findall(r"flight aircraft (.+?) from Aircraft/(\w+)\.lua, (\w+) engine", seen)
    print("flight: aircraft flown %r" % (flown,))
    if flown != [("Cessna 172", "cessna172", "propeller"), ("Test Cessna", "testplane", "propeller")]:
        fails.append("the Cessna, then the Test Cessna chosen in the sheet, were not flown: %r" % (flown,))
    # The time faster (F11): a frame at 2x carries it twice the 1x one.
    rates = re.findall(r"flight time (\d)x", seen)
    ratio = faster["2x"] / faster["1x"] if faster.get("1x") else 0
    print("flight: a frame carries %.4f s of speed at 1x, %.4f at 2x - %.2f times; "
          "the time stepped %r" % (faster.get("1x", 0), faster.get("2x", 0), ratio, rates))
    if not 1.6 <= ratio <= 2.4:
        fails.append("X did not make the time 2x: a frame carried it %.2f times as far" % ratio)
    if rates != ["2", "3", "4", "1"]:
        fails.append("X did not step 2x, 3x, 4x and back to 1x: %r" % (rates,))
    held = re.findall(r"flight autopilot (on|off): heading (\d+), (\d+) ft, (\d+) kt", seen)
    print("flight: the autopilot said %r" % (held,))
    # The route (F10c): with `fly_to` set, Q made a plan to Ezeiza and the
    # autopilot took the route's bearing and cruise as what it holds; the
    # right arrow then left the route.
    route = re.search(r"flight route (\w+) to EZE (\w+), (\d+) nm, cruise (\d+) ft, bearing (\d+)", seen)
    print("flight: the route %r" % (route.groups() if route else None,))
    if not route:
        fails.append("Q with fly_to = EZE made no route")
    elif held and (held[0][1] != route.group(5) or held[0][2] != route.group(4)):
        fails.append("the autopilot did not hold the route's bearing and cruise: %r %r"
                     % (route.groups(), held[0]))
    if "flight route left" not in seen:
        fails.append("the arrows under the route did not leave it")
    if [h[0] for h in held] != ["on", "off"]:
        fails.append("Q did not switch the autopilot on and off: %r" % (held,))
    else:
        turned = (int(held[1][1]) - int(held[0][1])) % 360
        if not 1 <= turned <= 60 or held[1][2:] != held[0][2:]:
            fails.append("the right arrow under the autopilot did not turn only its heading: %r"
                         % (held,))
    shown = re.findall(r"flight keys shown: (\d+) keys in (\d+) groups", seen)
    print("flight: keys shown %r, none before F1: %s" % (shown, keys_before == 0))
    if keys_before != 0 or shown != [("19", "4")]:
        fails.append("F1 did not show the 19 keys in 4 groups once, after none: %r (before %d)"
                     % (shown, keys_before))
    if "flight keys closed" not in seen:
        fails.append("F1 again did not close the keys")
    pls = re.findall(r"flight place (.*?) - ", seen)
    if len(pls) < 2 or not re.fullmatch(r"SDU .{1,3} Santos Dumont", pls[-1]):
        fails.append("the place box did not follow to Santos Dumont: %r" % (pls,))
    sx, sz = mercator(-43.1631, -22.9105)
    sx, sz = (sx - ro[0]) * rscale, (sz - ro[1]) * rscale
    tws = re.findall(r"flight tower at x=(-?\d+) z=(-?\d+)", seen)
    if len(tws) < 2 or abs(int(tws[-1][0]) - sx) > 5 or abs(int(tws[-1][1]) - sz) > 5:
        fails.append("the tower did not move to Santos Dumont's field (%.0f, %.0f): %r"
                     % (sx, sz, tws))
    if viewed["view"] == before["view"]:
        fails.append("V did not change the view: still %d" % viewed["view"])

    # The views, said (F6b): the chip named each view as V stepped them.
    chips = [c for i, c in enumerate(re.findall(r"flight view (\w+) laid at", seen))
             if i == 0 or c != re.findall(r"flight view (\w+) laid at", seen)[i - 1]]
    print("flight: the view chip said %r" % (chips,))
    if chips[:3] != ["chase", "cockpit", "tower"]:
        fails.append("the view chip did not name chase, cockpit and tower in turn: %r" % (chips,))

    # The time of day (F6a): noon first, then T to the afternoon and dusk -
    # the sun three degrees up in Rio's west, 22.9 degrees south: at the
    # equinox at 17:45 it stands cos(22.9) cos(86.25) up, so 3.4 degrees,
    # towards 272 - and the horizon orange where it was blue.
    times = re.findall(r"flight time (\w+) at [\d.]+ h: the sun (-?[\d.]+) degrees up, "
                       r"towards (\d+)", seen)
    print("flight: times %r" % (times,))
    want = math.degrees(math.asin(math.cos(math.radians(22.9)) * math.cos(math.radians(86.25))))
    if not times or times[0][0] != "noon" or [t[0] for t in times[-2:]] != ["afternoon", "dusk"]:
        fails.append("the time did not go from noon to the afternoon and dusk: %r" % (times,))
    elif abs(float(times[-1][1]) - want) > 0.3 or abs(int(times[-1][2]) - 272) > 2:
        fails.append("dusk's sun is not %.1f degrees up towards 272: %r" % (want, times[-1]))
    where = re.search(r"wm: window Flight at (\d+),(\d+) (\d+)x(\d+)", seen)
    if noon_screen and dusk_screen and where:
        box = tuple(int(v) for v in where.groups())
        before_o, after_o = orange(noon_screen, box), orange(dusk_screen, box)
        print("flight: orange pixels at noon %d, at dusk %d" % (before_o, after_o))
        if after_o < before_o + 1000 or after_o < 3 * before_o:
            fails.append("dusk did not turn the horizon orange: %d orange pixels at noon, %d at "
                         "dusk" % (before_o, after_o))
    else:
        fails.append("no pictures at noon and dusk")
    # The minimap (F5k): its tiles chosen at the zoom the height says -
    # 13 at 457 m over Montevideo, below 600 - asked in the buildings' list
    # and decoded from the cache. The window here is too small to lay it
    # ("minimap not laid"), so its drawing is seen in a picture run.
    mini = re.findall(r"flight minimap tile (\d+)/(\d+)/(\d+) decoded", seen)
    print("flight: minimap tiles decoded %d, zooms %r" % (len(mini), sorted({m[0] for m in mini})))
    if not mini or {m[0] for m in mini} - {"12", "13"} or "13" not in {m[0] for m in mini}:
        fails.append("the minimap's tiles were not decoded at zoom 13 under 600 m: %r" % (mini[:6],))
    # The runway's lights (F8c): made for the runways near, off at noon -
    # the control - and on once T has stepped to dusk.
    made = re.findall(r"flight runway lights made (\d+) lights in (\d+) colours", seen)
    switched = re.findall(r"flight runway lights (on|off)\r?\n", seen)
    print("flight: lights made %r, switched %r" % (made, switched))
    if not made or int(made[0][0]) < 200 or made[0][1] != "4":
        fails.append("the runway lights were not made, four colours: %r" % (made,))
    if not switched or switched[0] != "off" or "on" not in switched:
        fails.append("the runway lights were not off at noon and on at dusk: %r" % (switched,))
    # The propeller and the engine (F6c): the propeller turns forwards, by
    # at most a sixth of a turn a frame - a third of the way from one blade
    # to the next - and never stands still; and the engine is heard, the
    # Synth Kit rendering into Flight's own stream with something in it.
    props = [(float(a), float(b), c) for a, b, c in
             re.findall(r"flight propeller ([\d.]+) turns a frame at throttle ([\d.]+), "
                        r"engine (\w+)", seen)]
    print("flight: the propeller %r" % (props[:6],))
    if not props or any(not 0 < p[0] <= 1 / 6 + 1e-3 for p in props):
        fails.append("the propeller did not turn forwards by under a sixth of a turn a frame: "
                     "%r" % (props,))
    elif any(p[2] == "silent" for p in props):
        fails.append("the engine was silent on a propeller line: %r" % (props,))
    sound = re.search(r"flight sound engine rpm=(\d+) hz=([\d.]+) cut=(\d+) wind=([\d.]+) "
                      r"rendered=(\d+) peak=([\d.]+)", seen)
    print("flight: %s" % (sound.group(0) if sound else
                          "no sound line: %r" % re.findall(r"flight sound.*", seen)))
    if not sound or int(sound.group(5)) == 0 or float(sound.group(6)) < 0.01:
        fails.append("the engine was not heard: %r" % re.findall(r"flight sound.*", seen))
    if "--island" not in sys.argv:
        loudest = 0
        if os.path.exists(heard):
            with open(heard, "rb") as f:
                pcm = f.read()[44:]
            n = len(pcm) // 2
            loudest = max((abs(v) for v in struct.unpack("<%dh" % n, pcm[:n * 2])), default=0)
        print("flight: the sound card's loudest sample %d" % loudest)
        if loudest < 1000:
            fails.append("the sound card heard no engine: its loudest sample was %d" % loudest)
    if not re.match(r"drawn=\d+ of \d+ ready=\d+", viewed["drawn"]) or \
            int(re.match(r"drawn=(\d+)", viewed["drawn"]).group(1)) == 0:
        fails.append("no ground was drawn: %r" % viewed["drawn"])

    if fails:
        for f in fails:
            print("FAIL: " + f)
        return 1
    print("PASS: Flight flew - %d tiles handed, rolled %d degrees, turned %d, climbed %d m, "
          "%.0f ms a frame" % (viewed["tiles"], rolled["roll"], turned,
                               climbed["y"] - start["y"], viewed["mean"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
