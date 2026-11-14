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

Outside the gate, `KOSMOS_JETSKI_LIVE=https://...` opens a real site
instead, and the picture is kept in `build/jetski/` to be looked at.

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
<p><a id="go" href="/other.html">Another page</a></p>
</body></html>
""" % ((TITLE.encode(), BLOCK_W, BLOCK_H) + BLOCK)

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
#field { display: block; margin: 120px 0 0 40px; width: 320px; height: 44px;
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

    def do_GET(self):
        Page.asked.append(self.path)
        body = {"/": PAGE, "/other.html": OTHER}.get(self.path)
        if BROKEN == "missing" or body is None:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
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


def main():
    built = os.path.join(APPS, "jetski.elf")
    if not os.path.exists(built):
        print("FAIL: no %s - `make ARCH=x86_64 apps` builds it" % built)
        return 1

    server = Server(("0.0.0.0", 0), Page)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    address = LIVE or "http://10.0.2.2:%d/" % server.server_address[1]

    image = os.path.join(WORK, "jetski.elf")
    subprocess.run([("x86_64-elf-" if "x86_64" in IMAGE else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)
    lua = os.path.join(ROOT, "user", "installed", "JetSki", "jetski.lua")
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

    checks = 8
    if failed:
        print("FAIL: %d of %d checks on JetSki's window (the picture is %s):\n  "
              % (len(failed), checks, picture) + "\n  ".join(failed))
        return 1
    print("PASS: JetSki's window - opened from the shell, installed in /Home/Apps/JetSki, "
          "a page fetched over HTTP by WebKit's network process and painted below the "
          "address field, %dx%d at %d,%d in the window, and the title the page's; "
          "then its link clicked to a second page, the wheel turned and its script told "
          "the page scrolled, and its field clicked and \"%s\" typed into it"
          % (box[2], box[3], box[0] - x, box[1] - y, TYPED))
    return 0


if __name__ == "__main__":
    sys.exit(main())
