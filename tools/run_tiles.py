#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The tiles server with several sources at once (`docs/flight.md` F1).

Flight wants two kinds of tile together - a city's satellite imagery and
its elevation - and Maps, open beside it, a third. So `/Tiles` keeps as many
sources as are asked for, each with its own connection, queue, arrivals and
cache folder, and each tile kept under the format its source was given.

A server on this Mac, reached by the guest as 10.0.2.2, serves both kinds:
elevation as terrarium PNGs by `{z}/{x}/{y}.png`, whose heights are known,
and imagery as JPEGs by `{z}/{y}/{x}` with no extension - Esri's shape - two
seconds a tile, as slow as a server across the world. The `tiles` program
(`user/bin/programs/tiles.lua`, on the Tiles library, the one door to the
server) is opened to fetch both at once - opened rather than run at the
prompt, since `/Tiles` is handed only to a program in the image that says
it needs it, by a starter that holds it: the window manager, not the
prompt's shell. Then what the cache holds is read and decoded. Checked:

- each source has its own number and its own folder, and asking for the
  same address and format again is answered with the same number;
- a format that is not letters and digits is refused, and so is asking
  after a source there is none of;
- **the elevation's tiles all come while the imagery is still coming**: one
  slow source does not hold another back, which one queue would;
- each tile is in the cache under its source's format - `.png` and `.jpg`
  - and decodes there by its name: the elevation's heights are the ones
  served, the imagery the picture served;
- a tile the imagery has none of (a 404) is kept as an empty `.jpg`.

Maps' own suite (`x86-maps`) holds that Maps, moved onto source numbers,
behaves as before.

Usage: run_tiles.py IMAGE
"""

import http.server
import importlib
import os
import random
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

import run_screenshot as R                                  # noqa: E402
import run_servers as S                                     # noqa: E402
import scratch                                              # noqa: E402

Z = 12
TILES = [(Z, 1200 + i, 1500 + j) for i in range(2) for j in range(2)]
MISSING = (Z, 1300, 1600)           # the imagery has none of this one
SLOW = 2.0                          # seconds an imagery tile takes

with open(os.path.join(ROOT, "assets", "images", "test-quads.jpg"), "rb") as f:
    JPEG = f.read()


def height(z, x, y, px, py):
    """The height served at a pixel of a tile: different in every tile and
    every pixel, so a tile kept under another's name is seen."""
    return (x - 1200) * 1000 + (y - 1500) * 300 + px * 2 + py - 500


def terrarium(z, x, y):
    """A 256 by 256 terrarium PNG: height + 32768 = R * 256 + G + B / 256."""
    rows = []

    for py in range(256):
        row = bytearray([0])        # filter: none

        for px in range(256):
            v = height(z, x, y, px, py) + 32768
            row += bytes([v >> 8, v & 255, 0])

        rows.append(bytes(row))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", 256, 256, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 6))
            + chunk(b"IEND", b""))


def tile_server():
    asked, times = [], {}

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
            asked.append(self.path)
            times[self.path] = time.time()
            m = re.match(r"^/elev/(\d+)/(\d+)/(\d+)\.png$", self.path)

            if m:
                z, x, y = map(int, m.groups())
                self.send(200, terrarium(z, x, y), "image/png")
                return

            m = re.match(r"^/img/(\d+)/(\d+)/(\d+)$", self.path)

            if m:
                z, y, x = map(int, m.groups())          # Esri's order: y before x
                time.sleep(SLOW)

                if (z, x, y) == MISSING:
                    self.send(404, b"", "text/plain")
                else:
                    self.send(200, JPEG, "image/jpeg")
                return

            self.send(404, b"", "text/plain")

    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, asked, times


# What the cache holds, read where `tiles` said each tile is kept and
# decoded by its name - a program that needs nothing, run at the prompt.
READER = r"""
for _, path in ipairs({ @PATHS@ }) do
  local bytes = fs.read(path)
  local kind = path:match("%.(%a+)$")
  local s = bytes and #bytes > 0 and (kind == "png" and gfx.png(bytes) or gfx.jpeg(bytes))
  local heights = {}

  if s and kind == "png" then
    for _, at in ipairs({ { 0, 0 }, { 10, 3 }, { 255, 255 } }) do
      local p = s:get(at[1], at[2])
      local r, g, b = (p >> 16) & 255, (p >> 8) & 255, p & 255
      heights[#heights + 1] = math.floor(r * 256 + g + b / 256 - 32768)
    end
  end

  print(("READ %s %d %s %s"):format(path, bytes and #bytes or -1,
        s and table.concat({ s:size() }, "x") or "none", table.concat(heights, ",")))
end
print("READ done")
"""


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    httpd, asked, times = tile_server()

    # The cache is the disk server's to keep, as on a machine.
    disk = os.path.join(scratch.directory("tiles"), "disk.img")
    subprocess.run([os.path.join(ROOT, "build", "host", "lua"),
                    os.path.join(HERE, "kfs.lua"), "create", disk, "32"],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = disk
    importlib.reload(R)

    base = "http://10.0.2.2:%d" % httpd.server_address[1]
    elev, img = base + "/elev/{z}/{x}/{y}.png", base + "/img/{z}/{y}/{x}"
    named = lambda ts: " ".join("%d/%d/%d" % t for t in ts)
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web)
    fails, seen, read = [], "", ""

    try:
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)

        # Opened, as Maps is: `/Tiles` is handed to a program in the image
        # that says it needs it, by a starter that holds it - the window
        # manager, not the prompt's shell. Its lines come on the console.
        def tiles(words, end):
            mark = len(guest.seen)
            session.run("open tiles " + words)
            guest.wait_for(end, "tiles " + words.split()[0])
            return guest.seen[mark:]

        seen = tiles("fetch png %s %s and jpg %s %s %s"
                     % (elev, named(TILES), img, named(TILES), named([MISSING])), "tiles: all came")
        again = tiles("fetch jpg %s %s" % (img, named(TILES[:1])), "tiles: all came")
        bad = tiles("fetch ../x %s 1/0/0" % img, " refused: ")
        none = tiles("arrived 99", "tiles: source 99 ")

        paths = re.findall(r"tiles: came \d+ \d+/\d+/\d+ (\S+)", seen)
        session.put(READER.replace("@PATHS@", ", ".join('"%s"' % p for p in paths)).encode(),
                    "/Temporary/read.lua")
        read = session.run("/Temporary/read.lua").decode(errors="replace")
    except Exception as e:                  # noqa: BLE001 - said as a failure
        fails.append("the run stopped: %s; the guest said: %s" % (
            str(e).splitlines()[0][:200],
            " | ".join(l for l in guest.seen.splitlines()
                       if "tiles" in l or "launch" in l or "error" in l.lower())[-1500:]))
        again = bad = none = ""
    finally:
        guest.close()
        httpd.shutdown()

    sources = dict((fmt, (int(n), folder)) for n, fmt, folder in
                   re.findall(r"tiles: source (\d+) is \S+ as \.(\w+), kept in (\S+)", seen))
    e, i = sources.get("png"), sources.get("jpg")

    if not e or not i or e[0] == i[0] or e[1] == i[1] or not e[1].startswith("/Home/Cache/Maps/"):
        fails.append("two sources were not given two numbers and two folders: %r" % seen[-900:])

    if not i or not re.search(r"tiles: source %d is \S+ as \.jpg" % i[0], again):
        fails.append("the same address and format asked again was not the same source: %r"
                     % again[-400:])

    if "refused: a format is" not in bad:
        fails.append("a format with a path in it was not refused: %r" % bad[-400:])

    if "tiles: source 99 refused: no source 99" not in none:
        fails.append("asking a source there is none of was not refused: %r" % none[-400:])

    came = re.findall(r"tiles: came (\d+) (\d+)/(\d+)/(\d+) (\S+)", seen)
    from_e = [c for c in came if e and int(c[0]) == e[0]]
    from_i = [c for c in came if i and int(c[0]) == i[0]]

    if len(from_e) != len(TILES) or len(from_i) != len(TILES) + 1:
        fails.append("not every tile of both sources was said to come: %d and %d of %d and %d"
                     % (len(from_e), len(from_i), len(TILES), len(TILES) + 1))

    # One queue would have fetched the five slow imagery tiles, ten seconds of
    # them, before any elevation: here the elevation is done while at most
    # one imagery tile has come.
    done_at = seen.find("tiles: source %d done" % e[0]) if e else -1
    imagery_before = len(re.findall(r"tiles: came %d " % i[0], seen[:done_at])) if i and done_at >= 0 else 99

    if done_at < 0 or imagery_before > 1:
        fails.append("the elevation waited behind the slow imagery: %d imagery tiles before it "
                     "was done" % imagery_before)

    for (z, x, y) in TILES:
        want = ",".join(str(height(z, x, y, px, py)) for px, py in ((0, 0), (10, 3), (255, 255)))

        if not re.search(r"READ \S+/%d/%d-%d\.png \d+ 256x256 %s\b" % (z, x, y, re.escape(want)), read):
            fails.append("elevation tile %d/%d/%d was not in the cache as a .png of the heights "
                         "served (%s): %r" % (z, x, y, want, read[-900:]))
            break

    for (z, x, y) in TILES:
        if not re.search(r"READ \S+/%d/%d-%d\.jpg %d 128x128" % (z, x, y, len(JPEG)), read):
            fails.append("imagery tile %d/%d/%d was not in the cache as a .jpg that decodes: %r"
                         % (z, x, y, read[-900:]))
            break

    if not re.search(r"READ \S+/%d/%d-%d\.jpg 0 none" % MISSING, read):
        fails.append("a tile the imagery has none of was not kept as an empty .jpg: %r"
                     % read[-300:])

    esri = [a for a in asked if a.startswith("/img/")]

    if "/img/%d/%d/%d" % (TILES[0][0], TILES[0][2], TILES[0][1]) not in esri:
        fails.append("the imagery was not asked for with y before x, as its template says: %r"
                     % esri[:6])

    checks = 10

    if fails:
        print("FAIL: %d of %d checks on the tiles server's sources:" % (len(fails), checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on the tiles server with several sources (two numbers and two "
          "folders, the same one asked again the same, a bad format and an unknown source "
          "refused, elevation all come while the slow imagery was at %s of %d, PNG heights "
          "and JPEG pictures decoded from the cache by their format, a 404 kept empty)"
          % (checks, imagery_before, len(TILES) + 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
