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

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
X86 = "x86_64" in IMAGE
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    ROOT, "build", "user-x86_64-test" if X86 else "user-test", "apps")
WORK = scratch.directory("flight")
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


def tile_server():
    asked = {"elev": 0, "img": 0, "search": None}
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
            if re.match(r"^/img/(\d+)/(\d+)/(\d+)$", self.path):
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
  lon = %(lon)r, lat = %(lat)r, width = %(width)d, height = %(height)d,
  report = 2, frames = 0, finder = "%(base)s/search",
}
local s = prefs.open("flight", {})
s:reset("city") s:reset("place") s:reset("recent") s:reset("start")
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


def strip_band(R, data, across, tall=(34, 46), white_least=20, inside=None):
    """The strip on the screen: a stack of 34 to 46 rows each holding a
    stretch as wide as Flight's window, `across`, that is at least half dark
    - the band laid over the ground by its alpha - starting at the same x to
    four pixels, with words in it. Every stack is found and a strip-shaped
    one kept, so a taller dark panel elsewhere - the Log's - is not taken
    for it. (left, top, rows, white pixels) of the one kept, or else of the
    tallest; None when there is no stack at all."""
    width, height, px = R.parse_ppm(data)
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
        dark = [inside(row[x * 3], row[x * 3 + 1], row[x * 3 + 2]) for x in range(width)]
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
        f.write(SETUP % dict(base=base, lon=LON, lat=LAT, width=WIDTH, height=HEIGHT))
    files = [(os.path.join(WORK, "setup.lua"), FOLDER + "/setup.lua"), (image, FOLDER + "/k3dvk.elf"),
             (os.path.join(ROOT, "user", "installed", "Flight", "flight.lua"),
              FOLDER + "/flight.lua"),
             (os.path.join(ROOT, "assets", "models", "cessna172", "cessna_172.glb"),
              FOLDER + "/cessna_172.glb")]
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
    guest = SV.boot(IMAGE, telnet, web)
    fails = []
    screen = None
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

        # Flying straight while the tiles come.
        before = wait_frames(6, "flying straight")
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

        chase = guest.screendump()                          # the strip is looked for in it
        if os.environ.get("KOSMOS_FLIGHT_SHOT"):            # the chase view, for a person
            R.write_png(os.environ["KOSMOS_FLIGHT_SHOT"].replace(".png", "-chase.png"), chase)

        # The yoke back: it climbs.
        start = level
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
        sheet_key("down", "down to the buttons")
        sheet_key("left", "left to Restart")
        sheet_key("ret", "Restart")
        guest.wait_for("flight started runway", "the restart")
        rio = wait_frames(3, "on Rio's runway")
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
    if asked["elev"] == 0 or asked["img"] == 0 or viewed["tiles"] == 0:
        fails.append("tiles were not fetched and handed: %d elevation, %d imagery asked, %d handed"
                     % (asked["elev"], asked["img"], viewed["tiles"]))
    if rolled["roll"] < 5:
        fails.append("the right arrow held did not roll it right: %d degrees" % rolled["roll"])
    turned = (level["heading"] - before["heading"] + 540) % 360 - 180
    if turned < 2:
        fails.append("rolled right, it did not turn east: %d degrees" % turned)
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
    band = strip_band(R, chase, WIDTH)
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
            fails.append("no panel on the cockpit's screen - a near-black block %dx%d with "
                         "dials in it: %r" % (pw, ph, block))
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
            fails.append("no graphics panel on the chase view's screen - a near-black box "
                         "%dx%d at %d,%d with words in it: %r" % (gw, gh, gx, gy, box))
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
