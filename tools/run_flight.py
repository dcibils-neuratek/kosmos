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
    asked = {"elev": 0, "img": 0}
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
  report = 2, frames = 0,
}
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
                            tiles=int(g[9]), mean=float(g[10]), drawn=g[12]))
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

        if os.environ.get("KOSMOS_FLIGHT_SHOT"):            # the chase view, for a person
            R.write_png(os.environ["KOSMOS_FLIGHT_SHOT"].replace(".png", "-chase.png"),
                        guest.screendump())

        # The yoke back: it climbs.
        start = level
        keys(True, "down")
        climbed = wait_frames(5, "the down arrow held")
        keys(False, "down")

        # V: the next view.
        keys(True, "v")
        time.sleep(0.2)
        keys(False, "v")
        viewed = wait_frames(2, "V pressed")

        time.sleep(1.0)
        screen = guest.screendump()
        if os.environ.get("KOSMOS_FLIGHT_SHOT"):
            R.write_png(os.environ["KOSMOS_FLIGHT_SHOT"], screen)
    except R.Failure as e:
        fails.append("the flight stopped: %s" % e)
    finally:
        seen = guest.seen[mark:]
        guest.close()
        httpd.shutdown()

    if fails:
        print("FAIL: " + fails[0])
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
