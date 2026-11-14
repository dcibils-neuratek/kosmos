#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""JetSki's window (`docs/w7.md` W7c): WebKit's page in a Kosmos window.

`x86-sites` says WebKit loads and paints pages with no window. This says
the browser does it as an application a person opens: `wm jetski:<address>`
from the shell, JetSki installed in /Home/Apps/JetSki as Doom is, its window
opened, the page fetched from a server on this Mac and painted into the
window's surface below the address field.

**Checked on the screen**, from a screendump: the page is a block of one
colour that nothing else on the desktop is, under a heading and a
paragraph, and the block must be in the window, the size the page says, and
below the window's top edge - where the header leaves room for it. The
window's title must become the page's, which only WebKit could have told it.

**Then a person uses it** (W7c's input), through QEMU's own tablet and
keyboard: the link is clicked - a block of a colour of its own, found on
the screen - and the second page must arrive; the wheel is turned over it,
and its script must say it scrolled; its field is clicked and three keys
typed, and its script must say what they typed. Each answer is the page's
title, set by the page's own JavaScript, so only WebKit having been handed
the event could have produced it.

Controls: `KOSMOS_JETSKI_BREAK=missing` serves the page as a 404 - the
window opens and loads, and the block is nowhere, so the screen check
fails. `KOSMOS_JETSKI_BREAK=deaf` installs a `jetski.lua` whose page view
hands WebKit nothing - the pointer, the wheel and keys reach the window
and stop there - so the link is clicked and nothing comes of it; and
`deaf-wheel` and `deaf-key` keep only the wheel or only the keys, so the
link and the other still pass and that one fails.

**And it keeps cookies**: the first page's answer sets one, with a
lifetime, and its script sets another; the second page must be asked for
with both, and the one with a lifetime must be in the cookie file on
`/Home` when the machine is off - read from the disk with `kfs.lua`.
**And it says what a browser says of itself**: the first page reports
`navigator.platform`, its window's outer size and a canvas it drew on,
encoded, to the server - each of which a site's robot check reads, and each
of which was empty here until Google's search answered every query with
one.

`KOSMOS_JETSKI_BREAK=foreign-cookie` has the server set its cookie for
another site and the script set none, which a browser must refuse, so the
second page arrives without them.

Outside the gate, `KOSMOS_JETSKI_LIVE=https://...` opens a real site
instead, and the picture is kept in `build/jetski/` to be looked at,
with the cookie file the site left as `live-cookies.db`;
`KOSMOS_JETSKI_LIVE=file:<path>` serves that file from this Mac as the
page, for a case to look at by eye.

Usage: run_jetski.py [kernel image] [apps folder]
"""

import http.server
import os
import re
import socketserver
import subprocess
import sys
import threading
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "user-x86_64", "apps")
WORK = scratch.directory("jetski")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")
KEPT = os.path.join(ROOT, "build", "jetski")
BROKEN = os.environ.get("KOSMOS_JETSKI_BREAK")
LIVE = os.environ.get("KOSMOS_JETSKI_LIVE")

# The block: a colour nothing on the desktop is drawn in, and its size.
BLOCK = (0x13, 0x7a, 0x5c)
BLOCK_W, BLOCK_H = 320, 160
TITLE = "JetSki window test"

# The cookies: one from the server's answer, with a lifetime so it is
# kept on the disk, and one from the page's script.
SERVER_COOKIE = "jetski=from-the-server"
SCRIPT_COOKIE = "script=from-the-page"
COOKIE_FILE = "/Home/Settings/JetSki/cookies.db"

# What a page sees of the browser: the system it names, its window, and a
# canvas it can read back - each of which a robot check asks.
PLATFORM = "Kosmos x86_64" if "x86_64" in IMAGE else "Kosmos aarch64"
WINDOW = (1200, 800)

PAGE = b"""<!doctype html>
<html><head><meta charset="utf-8"><title>%s</title>
<style>
body { margin: 24px; font-family: sans-serif; background: #fff; color: #222; }
#block { width: %dpx; height: %dpx; background: rgb(%d, %d, %d); }
</style></head>
<body>
<h1>A page in a window</h1>
<p>WebKit laid this out and painted it, and JetSki put it on the screen.</p>
<div id="block"></div>
<script>document.cookie = "%s; path=/";
var canvas = document.createElement("canvas");
canvas.width = 40; canvas.height = 20;
var g = canvas.getContext("2d");
g.fillStyle = "rgb(200, 40, 40)"; g.fillRect(0, 0, 40, 20);
var said = { platform: navigator.platform, outer: outerWidth + "x" + outerHeight,
             canvas: canvas.toDataURL() };
new Image().src = "/report?" + new URLSearchParams(said);
</script>
<p><a id="go" href="/other.html">Another page</a></p>
</body></html>
""" % ((TITLE.encode(), BLOCK_W, BLOCK_H) + BLOCK + (SCRIPT_COOKIE.encode(),))

# The link and the second page's field, each a colour of its own, so they
# are found on the screen rather than assumed to be where a layout put them.
LINK = (0x8a, 0x2b, 0x6e)
FIELD = (0x2b, 0x4f, 0x8a)
SECOND = "JetSki second page"
TYPED = "kos"

PAGE = PAGE.replace(b"#block {", b"#go { display: block; width: 240px; height: 48px; "
                    b"background: rgb(%d, %d, %d); color: #fff; }\n#block {" % LINK)

OTHER = b"""<!doctype html>
<html><head><meta charset="utf-8"><title>%s</title>
<style>
body { margin: 0; height: 6000px; background: #fff; }
#field { position: fixed; top: 120px; left: 40px; width: 320px; height: 44px;
         border: 0; padding: 0; outline: none; color: #fff;
         background: rgb(%d, %d, %d); font-size: 20px; }
</style></head>
<body>
<input id="field">
<script>
var field = document.getElementById("field");
window.addEventListener("scroll", function () {
  if (window.scrollY > 0) document.title = "scrolled " + window.scrollY;
});
field.addEventListener("input", function () { document.title = "typed " + field.value; });
</script>
</body></html>
""" % ((SECOND.encode(),) + FIELD)


class Page(http.server.BaseHTTPRequestHandler):
    asked = []
    cookies = {}
    report = None

    def do_GET(self):
        Page.asked.append(self.path)
        Page.cookies[self.path] = self.headers.get("Cookie", "")
        if self.path.startswith("/report?"):
            Page.report = urllib.parse.parse_qs(self.path.split("?", 1)[1], keep_blank_values=True)
            self.send_response(204)
            self.end_headers()
            return
        body = {"/": PAGE, "/other.html": OTHER}.get(self.path)
        if BROKEN == "missing" or body is None:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        if self.path == "/" and not LIVE:
            self.send_header("Set-Cookie", SERVER_COOKIE + "; Path=/; Max-Age=86400"
                             + ("; Domain=example.com" if BROKEN == "foreign-cookie" else ""))
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def block_at(parse_ppm, data, x, y, w, h, colour=BLOCK):
    """The bounding box of a colour inside the window, and how many of its
    pixels there are."""
    sw, sh, rgb = parse_ppm(data)
    want = bytes(colour)
    left = top = 1 << 30
    right = bottom = -1
    count = 0
    for row in range(max(0, y), min(y + h, sh)):
        line = rgb[row * sw * 3:(row + 1) * sw * 3]
        at = line.find(want, max(0, x) * 3)
        while 0 <= at < min(x + w, sw) * 3:
            if at % 3 == 0:
                col = at // 3
                count += 1
                left, right = min(left, col), max(right, col)
                top, bottom = min(top, row), max(bottom, row)
                at = line.find(want, at + 3)
            else:
                at = line.find(want, at + 1)
    return count, (left, top, right - left + 1, bottom - top + 1)


def said(guest, mark, text, seconds):
    """Whether the guest says `text` within `seconds`."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        guest._read_available()
        if text in guest.seen[mark:]:
            return True
        time.sleep(0.25)
    return False


def use(guest, R, dump, window, mark):
    """The link clicked, the wheel turned, the field typed into: what failed."""
    failed = []
    x, y, w, h = window
    sw, sh, _ = R.parse_ppm(dump)

    def click(box):
        cx, cy = box[0] + box[2] // 2, box[1] + box[3] // 2
        guest.mouse_to(*R._to_tablet(cx, cy, sw, sh))
        time.sleep(0.3)
        guest.mouse_button(True)
        time.sleep(0.15)
        guest.mouse_button(False)

    count, link = block_at(R.parse_ppm, dump, x, y, w, h, LINK)
    if count < 200:
        return ["the link is not on the screen: %d of its pixels" % count]
    click(link)
    if not said(guest, mark, "jetski: title " + SECOND, 60):
        return ["the link was clicked and the second page did not arrive: %s"
                % (re.findall(r"jetski: [^\r\n]*", guest.seen[mark:])[-3:] or "nothing said")]

    time.sleep(1.5)                     # its picture, painted and composed
    second = guest.screendump()
    os.makedirs(KEPT, exist_ok=True)
    R.write_png(os.path.join(KEPT, "jetski-second.png"), second)
    count, field = block_at(R.parse_ppm, second, x, y, w, h, FIELD)
    if count < 2000:
        return ["the second page's field is not on the screen: %d of its pixels" % count]

    click(field)
    time.sleep(0.5)
    for key in TYPED:
        guest.sendkey(key)
    if not said(guest, mark, "jetski: title typed " + TYPED, 30):
        failed.append("\"%s\" was typed into the page's field and it did not arrive: %s"
                      % (TYPED, re.findall(r"jetski: title [^\r\n]*", guest.seen[mark:])[-2:]))

    # The wheel, over the page and below the field.
    guest.mouse_to(*R._to_tablet(field[0] + 40, field[1] + field[3] + 120, sw, sh))
    time.sleep(0.3)
    for _ in range(3):
        guest.mouse_button(True, "wheel-down")
        time.sleep(0.05)
        guest.mouse_button(False, "wheel-down")
        time.sleep(0.1)
    if not said(guest, mark, "jetski: title scrolled ", 30):
        failed.append("the wheel was turned over the page and it did not scroll")
    return failed


def itself():
    """What the page said of the browser it is in: what every browser says."""
    if Page.report is None:
        return ["the page never said what it sees of the browser"]
    said = {k: v[0] for k, v in Page.report.items()}
    failed = []
    if said.get("platform") != PLATFORM:
        failed.append("navigator.platform is \"%s\", not \"%s\"" % (said.get("platform"), PLATFORM))
    if said.get("outer") != "%dx%d" % WINDOW:
        failed.append("the window's outer size is %s, not %dx%d" % ((said.get("outer"),) + WINDOW))
    picture = said.get("canvas", "")
    if not picture.startswith("data:image/png;base64,") or len(picture) < 80:
        failed.append("a canvas drawn on encodes as \"%s\", not a PNG" % picture[:40])
    return failed


def cookies():
    """The second page asked for with both cookies, the lasting one on the disk."""
    failed = []
    sent = Page.cookies.get("/other.html", "")
    for cookie in (SERVER_COOKIE, SCRIPT_COOKIE):
        if cookie not in sent:
            failed.append("the second page was asked for without %s: Cookie: [%s]"
                          % (cookie.split("=")[0], sent))
    kept = os.path.join(WORK, "cookies.db")
    if os.path.exists(kept):
        os.remove(kept)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "get", HOME_DISK, COOKIE_FILE, kept],
                   capture_output=True, cwd=ROOT)
    if not os.path.exists(kept):
        failed.append("there is no %s on the disk" % COOKIE_FILE)
    else:
        with open(kept, "rb") as f:
            data = f.read()
        if not data.startswith(b"SQLite format 3"):
            failed.append("%s is not a SQLite database" % COOKIE_FILE)
        elif SERVER_COOKIE.split("=")[1].encode() not in data:
            failed.append("the server's cookie, with its lifetime, is not in %s" % COOKIE_FILE)
    return failed


def main():
    built = os.path.join(APPS, "jetski.elf")
    if not os.path.exists(built):
        print("FAIL: no %s - `make ARCH=x86_64 apps` builds it" % built)
        return 1

    server = Server(("0.0.0.0", 0), Page)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    global PAGE
    address = LIVE or "http://10.0.2.2:%d/" % server.server_address[1]
    if LIVE and LIVE.startswith("file:"):
        with open(LIVE[5:], "rb") as f:
            PAGE = f.read()
        address = "http://10.0.2.2:%d/" % server.server_address[1]

    image = os.path.join(WORK, "jetski.elf")
    subprocess.run([("x86_64-elf-" if "x86_64" in IMAGE else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)
    lua = os.path.join(ROOT, "user", "installed", "JetSki", "jetski.lua")
    if BROKEN == "foreign-cookie":
        PAGE = PAGE.replace(SCRIPT_COOKIE.encode(), b"")
    if BROKEN in ("deaf", "deaf-wheel", "deaf-key"):
        # The page's view keeps what it is handed: everything, or the one.
        with open(lua) as f:
            text = f.read()
        calls = {"deaf": ("web.mouse(", "web.wheel(", "web.key("),
                 "deaf-wheel": ("web.wheel(",), "deaf-key": ("web.key(",)}[BROKEN]
        for call in calls:
            text = text.replace(call, "(function() return false end)(")
        lua = os.path.join(WORK, "jetski.lua")
        with open(lua, "w") as f:
            f.write(text)
    files = [(lua, "/Home/Apps/JetSki/jetski.lua"),
             (image, "/Home/Apps/JetSki/jetski.elf")]
    megabytes = sum(os.path.getsize(src) for src, _ in files) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)] + ["%s:%s" % f for f in files],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402 - after the disk
    board = "X86_ARGS" if R.machine(IMAGE) == "x86_64" else "QEMU_ARGS"
    # A real site is a real browser's memory: Wikipedia's article in three
    # WebKit processes does not fit beside the desktop in the suites' 512 MB
    # (`testing.md` 18.810), and no machine it is meant for has so little.
    if LIVE:
        args = getattr(R, board)
        setattr(R, board, [("2G" if a == "512M" else a) for a in args])
    setattr(R, board, getattr(R, board) + [
        "-netdev", "user,id=net0", "-device", R.device(IMAGE, "net") + ",netdev=net0"])

    guest = R.Guest(IMAGE, 400)
    failed, mark, dump, where, used = [], 0, None, None, None
    wait = 300 if LIVE else 120
    try:
        guest.wait_for(R.PROMPT, "the prompt")
        guest.wait_for("net: an address from DHCP", "a lease")
        mark = len(guest.seen)
        guest.type("wm jetski:" + address)
        end = time.monotonic() + wait
        while time.monotonic() < end:
            time.sleep(0.25)
            guest._read_available()
            seen = guest.seen[mark:]
            if re.search(r"jetski: (loaded|could not load|the page's process ended)", seen):
                break
        time.sleep(2)          # the last picture, painted and composed
        guest._read_available()
        seen = guest.seen[mark:]
        where = re.search(r"wm: window JetSki at (\d+),(\d+) (\d+)x(\d+)", seen)
        dump = guest.screendump()
        if where and not LIVE and "jetski: loaded" in seen:
            used = use(guest, R, dump, [int(v) for v in where.groups()], mark)
    finally:
        guest.close()
        server.shutdown()

    os.makedirs(KEPT, exist_ok=True)
    picture = os.path.join(KEPT, "live.png" if LIVE else "jetski.png")
    # What the machine said, beside the picture: a window that went away
    # says why only here.
    with open(os.path.join(KEPT, "live.txt" if LIVE else "jetski.txt"), "w") as f:
        f.write(guest.seen)
    if dump:
        R.write_png(picture, dump)

    if not where:
        print("FAIL: JetSki opened no window\n--- the guest's last words ---\n" + seen[-3000:])
        return 1
    if "jetski: loaded" not in seen:
        failed.append("the page was not loaded: %s"
                      % (re.findall(r"jetski: [^\r\n]*", seen)[-3:] or "nothing said"))

    if LIVE:
        # What the site left in the cookie file, kept beside the picture.
        subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "get", HOME_DISK, COOKIE_FILE,
                        os.path.join(KEPT, "live-cookies.db")], capture_output=True, cwd=ROOT)
        print("%s: %s - the picture is %s" % ("FAIL" if failed else "PASS",
                                               "; ".join(failed) or "loaded", picture))
        return 1 if failed else 0

    if "/" not in Page.asked:
        failed.append("the page was never asked for")
    if "jetski: title " + TITLE not in seen:
        failed.append("the window was not given the page's title")

    x, y, w, h = (int(v) for v in where.groups())
    count, box = block_at(R.parse_ppm, dump, x, y, w, h)
    if count < BLOCK_W * BLOCK_H * 9 // 10:
        failed.append("the page's block is not in the window: %d of its %d pixels"
                      % (count, BLOCK_W * BLOCK_H))
    elif abs(box[2] - BLOCK_W) > 2 or abs(box[3] - BLOCK_H) > 2:
        failed.append("the block is %dx%d, not %dx%d" % (box[2], box[3], BLOCK_W, BLOCK_H))
    elif box[1] - y < 46 + 24:
        failed.append("the block is %d pixels below the window's top - over the header"
                      % (box[1] - y))

    if used is None:
        failed.append("nothing was clicked: the page was not there to use")
    else:
        failed.extend(used)
        failed.extend(cookies())
        failed.extend(itself())

    checks = 14
    if failed:
        print("FAIL: %d of %d checks on JetSki's window (the picture is %s):\n  "
              % (len(failed), checks, picture) + "\n  ".join(failed))
        return 1
    print("PASS: JetSki's window - opened from the shell, installed in /Home/Apps/JetSki, "
          "a page fetched over HTTP by WebKit's network process and painted below the "
          "address field, %dx%d at %d,%d in the window, and the title the page's; "
          "then its link clicked to a second page, the wheel turned and its script told "
          "the page scrolled, and its field clicked and \"%s\" typed into it; and the "
          "second page asked for with the server's cookie and the script's, the lasting "
          "one kept on /Home; and the page saw \"%s\" as the platform, its window "
          "1200x800 and a canvas read back as a PNG"
          % (box[2], box[3], box[0] - x, box[1] - y, TYPED, PLATFORM))
    return 0


if __name__ == "__main__":
    sys.exit(main())
