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

**And it plays a film** (`docs/media.md` M2): the second page has a
`<video>`, fixed where the wheel cannot move it, playing six seconds of
one colour that this Mac's FFmpeg makes at the start as VP9 in WebM. The
page reports to the server once it has played a second - its size, and
whether VP9 in WebM is "probably" playable - and the colour must be in the
window, near rather than exact after YUV. `KOSMOS_JETSKI_BREAK=no-film`
serves the film as a 404.

**And a film the way YouTube plays one** (M3): beside it, a second
`<video>` whose source is a `MediaSource`, with a picture SourceBuffer
(VP9) and a sound one (Opus) as YouTube's player makes them, each fed its
file fetched whole and appended in pieces cut at arbitrary bytes - so a
WebM cluster arrives in two appends - then `endOfStream()` - all but the picture's last piece, which waits until
the page has kept its own thread busy for four and a half seconds, as a
site's script does, while the clock ran past what was buffered. It must
wait where its picture ran out (it waited where the counter had got to,
and skipped the film between), play to its end, be its size, have its
six seconds buffered, and show its own colour; `MediaSource.isTypeSupported` must say yes to VP9 and no to 10-bit
VP9, which is decoded and not yet shown. `KOSMOS_JETSKI_BREAK=no-appended`
serves its picture as a 404, which a SourceBuffer cannot parse.

Its sound must be heard (M4): the machine has a sound card whose output
QEMU writes to a WAV, and the appended film's sound is a 440 Hz tone, so
what comes out must hold seconds of it - decoded by the Media Kit, played
by the Playback Kit into a stream of the audio server's. The control,
`KOSMOS_JETSKI_BREAK=silent`, appends a sound track of silence instead,
and nothing else changes; `KOSMOS_JETSKI_BREAK=muted` mutes the film on
the page, which must silence it at the audio server's fader.

`KOSMOS_JETSKI_BREAK=foreign-cookie` has the server set its cookie for
another site and the script set none, which a browser must refuse, so the
second page arrives without them.

Outside the gate, `KOSMOS_JETSKI_LIVE=https://...` opens a real site
instead, and the picture is kept in `build/jetski/` to be looked at,
with the cookie file the site left as `live-cookies.db`;
`KOSMOS_JETSKI_LIVE=file:<path>` serves that file from this Mac as the
page, for a case to look at by eye. `KOSMOS_JETSKI_LINGER=<seconds>` waits
that long after a live page has loaded before the picture is taken - a
film starts well after its page - and takes a second, `live-later.png`,
three seconds on, so a film that plays is told from its poster.

Usage: run_jetski.py [kernel image] [apps folder]
"""

import http.server
import math
import os
import re
import socketserver
import struct
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
LINGER = float(os.environ.get("KOSMOS_JETSKI_LINGER", "0")) if LIVE else 0

# The block: a colour nothing on the desktop is drawn in, and its size.
BLOCK = (0x13, 0x7a, 0x5c)
BLOCK_W, BLOCK_H = 320, 160
TITLE = "JetSki window test"

# The cookies: one from the server's answer, with a lifetime so it is
# kept on the disk, and one from the page's script.
SERVER_COOKIE = "jetski=from-the-server"
SCRIPT_COOKIE = "script=from-the-page"
COOKIE_FILE = "/Home/Settings/JetSki/cookies.db"

# The film the second page plays: a colour of its own, made at the start
# by this Mac's FFmpeg as VP9 in WebM - YouTube's - so no site's bytes are
# in the tree. Through YUV and back a colour comes back near, not exact.
FILM_COLOUR = (0xc0, 0x30, 0x70)
FILM_SIZE = (320, 180)
FILM = None

# The film appended through a MediaSource (M3): another colour, its picture
# and its sound in files of their own, as YouTube's are.
APPENDED_COLOUR = (0x40, 0xb0, 0x50)
APPENDED = None

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
<p><a id="blank" href="/other.html" target="_blank">In a new tab</a></p>
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
BLANK = (0x6e, 0x8a, 0x2b)          # the link to a new tab, `target=_blank`
FIELD = (0x2b, 0x4f, 0x8a)
SECOND = "JetSki second page"
TYPED = "kos"

PAGE = PAGE.replace(b"#block {", b"#go { display: block; width: 240px; height: 48px; "
                    b"background: rgb(%d, %d, %d); color: #fff; }\n#block {" % LINK)
PAGE = PAGE.replace(b"#block {", b"#blank { display: block; width: 240px; height: 36px; "
                    b"background: rgb(%d, %d, %d); color: #fff; }\n#block {" % BLANK)

OTHER = b"""<!doctype html>
<html><head><meta charset="utf-8"><title>%s</title>
<style>
body { margin: 0; height: 6000px; background: #fff; }
#film { position: fixed; top: 200px; left: 40px; width: 320px; height: 180px; }
#appended { position: fixed; top: 200px; left: 400px; width: 320px; height: 180px; }
#field { position: fixed; top: 120px; left: 40px; width: 320px; height: 44px;
         border: 0; padding: 0; outline: none; color: #fff;
         background: rgb(%d, %d, %d); font-size: 20px; }
</style></head>
<body>
<input id="field">
<video id="film" src="/film.webm" autoplay muted></video>
<video id="appended" autoplay></video>
<script>
var film = document.getElementById("film"), told = false;
function tell(what) {
  if (told) return;
  told = true;
  new Image().src = "/film-report?" + new URLSearchParams(what);
}
film.addEventListener("timeupdate", function () {
  if (film.currentTime >= 1)
    tell({ time: film.currentTime, size: film.videoWidth + "x" + film.videoHeight,
           webm: document.createElement("video").canPlayType('video/webm; codecs="vp9"') });
});
film.addEventListener("error", function () { tell({ error: film.error ? film.error.code : "?" }); });

var appended = document.getElementById("appended"), source = new MediaSource(), toldAppended = false;
function tellAppended(what) {
  if (toldAppended) return;
  toldAppended = true;
  new Image().src = "/appended-report?" + new URLSearchParams(what);
}
/* Appends all but `held` of the pieces; answers with a function that
   appends the rest and answers when it is in. */
function feed(buffer, address, pieces, held) {
  return fetch(address).then(function (r) { return r.arrayBuffer(); }).then(function (bytes) {
    var size = Math.ceil(bytes.byteLength / pieces), at = 0, count = 0;
    function rest() {
      return new Promise(function (done) {
        if (at >= bytes.byteLength) { done(); return; }
        buffer.addEventListener("updateend", function () { done(); }, { once: true });
        var tail = bytes.slice(at);
        at = bytes.byteLength;
        buffer.appendBuffer(tail);
      });
    }
    return new Promise(function (done, fail) {
      function next() {
        if (count >= pieces - held || at >= bytes.byteLength) { done(rest); return; }
        buffer.appendBuffer(bytes.slice(at, at + size));
        at += size;
        count++;
      }
      buffer.addEventListener("updateend", next);
      buffer.addEventListener("error", function () { fail("an append failed"); });
      next();
    });
  });
}
/* Where the film waited when its picture ran out: the page's thread kept
   busy, as a site's script keeps it, while the clock goes on. */
var waited = null;
source.addEventListener("sourceopen", function () {
  var picture = source.addSourceBuffer('video/webm; codecs="vp9"');
  var sound = source.addSourceBuffer('audio/webm; codecs="opus"');
  Promise.all([feed(picture, "/appended-picture.webm", 3, 1), feed(sound, "/appended-sound.webm", 2, 0)])
    .then(function (rests) {
      return new Promise(function (done) {
        function started() {
          if (appended.currentTime < 0.5) return;
          appended.removeEventListener("timeupdate", started);
          var until = Date.now() + 4500;
          while (Date.now() < until) { }
          setTimeout(function () {
            waited = { at: appended.currentTime,
                       end: appended.buffered.length ? appended.buffered.end(0) : 0 };
            done(rests);
          }, 1500);
        }
        appended.addEventListener("timeupdate", started);
      });
    })
    .then(function (rests) { return rests[0](); })
    .then(function () { source.endOfStream(); })
    .catch(function (why) { tellAppended({ error: why }); });
});
/* Started once the page has been scrolled, the last thing done to it, so
   the busy thread holds up no click or key the test sends. */
window.addEventListener("scroll", function () {
  if (!appended.src) appended.src = URL.createObjectURL(source);
});
appended.addEventListener("timeupdate", function () {
  if (waited && appended.currentTime >= 5.5)
    tellAppended({ time: appended.currentTime, waitedAt: waited.at, waitedEnd: waited.end,
                   size: appended.videoWidth + "x" + appended.videoHeight,
                   buffered: appended.buffered.length ? appended.buffered.end(0) : 0,
                   vp9: MediaSource.isTypeSupported('video/webm; codecs="vp9"'),
                   deep: MediaSource.isTypeSupported('video/webm; codecs="vp09.02.10.10"') });
});
appended.addEventListener("error", function () { tellAppended({ error: appended.error ? appended.error.code : "?" }); });
var field = document.getElementById("field");
window.addEventListener("scroll", function () {
  if (window.scrollY > 0) document.title = "scrolled " + window.scrollY;
});
field.addEventListener("input", function () { document.title = "typed " + field.value; });
window.addEventListener("resize", function () {
  document.title = "resized " + window.innerWidth + "x" + window.innerHeight;
});
</script>
</body></html>
""" % ((SECOND.encode(),) + FIELD)
# The appended film is heard, and the page's mute must silence it.
if BROKEN == "muted":
    OTHER = OTHER.replace(b'<video id="appended" autoplay>', b'<video id="appended" autoplay muted>')


class Page(http.server.BaseHTTPRequestHandler):
    asked = []
    cookies = {}
    report = None
    film = None
    appended = None

    def do_GET(self):
        Page.asked.append(self.path)
        Page.cookies[self.path] = self.headers.get("Cookie", "")
        if self.path.startswith("/film-report?"):
            Page.film = urllib.parse.parse_qs(self.path.split("?", 1)[1], keep_blank_values=True)
            self.send_response(204)
            self.end_headers()
            return
        if self.path.startswith("/appended-report?"):
            Page.appended = urllib.parse.parse_qs(self.path.split("?", 1)[1], keep_blank_values=True)
            self.send_response(204)
            self.end_headers()
            return
        appended = {"/appended-picture.webm": 0, "/appended-sound.webm": 1}.get(self.path)
        if appended is not None and APPENDED and not (appended == 0 and BROKEN == "no-appended"):
            with open(APPENDED[appended], "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "video/webm")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/film.webm" and FILM and BROKEN != "no-film":
            with open(FILM, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "video/webm")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.startswith("/report?"):
            # The first page's first report: it is loaded again later, in a
            # tab of its own, after the window was made smaller.
            if Page.report is None:
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
    clicked = len(guest.seen)
    click(link)
    if not said(guest, mark, "jetski: title " + SECOND, 60):
        return ["the link was clicked and the second page did not arrive: %s"
                % (re.findall(r"jetski: [^\r\n]*", guest.seen[mark:])[-3:] or "nothing said")]
    # Said the moment it was followed, before the page began to arrive
    # (Diego: "i click some links and nothing happens").
    after = guest.seen[clicked:]
    followed = after.find("jetski: following http")
    titled = after.find("jetski: title " + SECOND)
    if followed < 0 or followed > titled:
        failed.append("the link was followed and JetSki did not show it until the page came")

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

    # The films, fixed on the page, once each has played a second.
    end = time.monotonic() + 60
    while (Page.film is None or Page.appended is None) and time.monotonic() < end:
        time.sleep(0.5)
    time.sleep(0.5)
    shot = guest.screendump()
    R.write_png(os.path.join(KEPT, "jetski-film.png"), shot)
    failed.extend(film(R, shot, window))
    failed.extend(appended(R, shot, window))
    failed.extend(resized(guest, R, window, mark, sw, sh))
    failed.extend(tabbed(guest, R, window, sw, sh))
    failed.extend(from_disk(guest, R, window, sw, sh))
    failed.extend(inspected(guest, R, window, sw, sh))
    failed.extend(stopped(guest, R, window, sw, sh))
    return failed


# **A page from the disk** (`roadmap.md`, *HTML files from the disk*): an
# HTML file in /Home, and a script beside it that the page names by a
# relative address - so what loads is the file and its folder, not only
# the one file. The script sets the title; the title is what is checked.
LOCAL_HTML = (b"<!doctype html><html><head><title>Not yet</title>"
              b"<script src=\"local.js\" onerror=\"document.title='The script beside it failed'\"></script></head>"
              b"<body><h1>From the disk</h1></body></html>\n")
LOCAL_JS = b"document.title = 'From the disk, with its script';\n"
LOCAL_TITLE = "From the disk, with its script"
CHOSEN_HTML = b"<!doctype html><html><head><title>Chosen in the Open window</title></head><body>Chosen</body></html>\n"
DROPPED_HTML = b"<!doctype html><html><head><title>Dropped on the window</title></head><body>Dropped</body></html>\n"


def keys_for(text):
    """QEMU's names for the keys that type `text`: a capital is Shift and
    its letter."""
    return [("shift-" + c.lower()) if c.isupper() else QEMU_KEYS.get(c, c) for c in text]


def from_disk(guest, R, window, sw, sh):
    """An address `file:` - an HTML file in /Home, typed into a new tab -
    loads, with the script beside it."""
    since = len(guest.seen)
    guest.sendkey("ctrl-t")
    deadline = time.time() + 20
    while not re.search(r"jetski: tab (\d+) of \1\b", guest.seen[since:]):
        if time.time() > deadline:
            return ["Control-T opened no tab for a file"]
        time.sleep(0.2)
    time.sleep(0.5)
    since = len(guest.seen)
    # A path, as a person types one: JetSki makes it `file:`.
    for key in keys_for("/Home/Pages/local.html\n"):
        guest.sendkey(key)
    if not said(guest, since, "jetski: title " + LOCAL_TITLE, 30):
        return ["a file from /Home did not load with its script: %s"
                % (re.findall(r"jetski: [^\r\n]*", guest.seen[since:])[-4:] or "nothing said")]
    failed = []
    # Control-O: the Open window, in the folder of the page shown, where
    # `a-chosen.html` is the first web page and is selected; Return opens it.
    since = len(guest.seen)
    guest.sendkey("ctrl-o")
    time.sleep(2.0)
    guest.sendkey("ret")
    if not said(guest, since, "jetski: title Chosen in the Open window", 30):
        failed.append("Control-O and Return did not open the web page the Open window chose: %s"
                      % (re.findall(r"(?:jetski|panel): [^\r\n]*", guest.seen[since:])[-4:]
                         or "nothing said"))
    # A file dropped on the window, as Tracker drops one: by the window
    # manager's own drop, which the test's key hands it (`HOOK`).
    since = len(guest.seen)
    guest.sendkey("ctrl-d")
    if not said(guest, since, "jetski: title Dropped on the window", 30):
        failed.append("a web page dropped on the window did not open: %s"
                      % (re.findall(r"jetski: [^\r\n]*", guest.seen[since:])[-4:]
                         or "nothing said"))
    return failed


def inspected(guest, R, window, sw, sh):
    """Web Inspector (`docs/webkit.html`, *Developer mode*): Control-Alt-I
    docks it against the page's right edge - WebKit's own front end, from
    `/Home/Apps/JetSki/Inspector`, in a page of its own - the page laid out
    narrower for it, and the same key puts it away."""
    x, y, w, h = window
    width = re.findall(r"jetski: resized to (\d+)x(\d+)", guest.seen)
    width = int(width[-1][0]) if width else w
    since = len(guest.seen)
    guest.sendkey("ctrl-alt-i")
    if not said(guest, since, "jetski: Web Inspector docked, the page", 60):
        return ["Control-Alt-I docked no Web Inspector: %s"
                % (re.findall(r"jetski: [^\r\n]*", guest.seen[since:])[-4:] or "nothing said")]
    failed = []
    if not re.search(r"jetski: tab \d+'s inspector docked", guest.seen[since:]):
        failed.append("WebKit made no inspector's view for the tab")
    page = int(re.findall(r"jetski: Web Inspector docked, the page (\d+) wide",
                          guest.seen[since:])[-1])
    split = min(548, width // 2)
    if page != width - split:
        failed.append("the page is %d wide beside the inspector, not %d" % (page, width - split))
    # The front end, painted: its toolbar and tabs, many colours, where the
    # page's right half was white with a line of text.
    time.sleep(INSPECTOR_WAIT)
    shot = guest.screendump()
    R.write_png(os.path.join(KEPT, "jetski-inspector.png"), shot)
    colours = inspector_colours(R, shot, x + page + 1, y + 76, split - 1, 120)
    if colours < INSPECTOR_COLOURS:
        failed.append("the inspector's half has %d colours in its top 120 rows: nothing of "
                      "Web Inspector drawn" % colours)
    since = len(guest.seen)
    guest.sendkey("ctrl-alt-i")
    if not said(guest, since, "jetski: Web Inspector away, the page %d wide" % width, 30):
        failed.append("Control-Alt-I again did not put Web Inspector away: %s"
                      % (re.findall(r"jetski: [^\r\n]*", guest.seen[since:])[-4:] or "nothing said"))
    return failed


# How long the front end has to arrive and draw - 254 files, 8.7 MB of
# script and style, under TCG - and how many colours its toolbar and tabs
# make at the least, measured where the page's own half has two or three.
INSPECTOR_WAIT = 20
INSPECTOR_COLOURS = 12


def inspector_colours(R, shot, x, y, w, h):
    sw, sh, pixels = R.parse_ppm(shot)
    seen = set()
    for row in range(max(0, y), min(sh, y + h)):
        for col in range(max(0, x), min(sw, x + w), 2):
            at = (row * sw + col) * 3
            seen.add(pixels[at:at + 3])
    return len(seen)


def stopped(guest, R, window, sw, sh):
    """A tab whose page stopped (`docs/webkit.html`): its web process ended
    - by the test's own key, which `sys.kill`s the shown tab's (`HOOK`) -
    and JetSki collects it, says so with what Nebula recorded, and draws the
    stopped page in the page's place."""
    since = len(guest.seen)
    guest.sendkey("ctrl-k")
    if not said(guest, since, "jetski: the page's process ended", 30):
        return ["a tab's web process was ended and JetSki did not say so: %s"
                % (re.findall(r"jetski: [^\r\n]*", guest.seen[since:])[-3:] or "nothing said")]
    after = guest.seen[since:]
    ended = re.search(r"jetski: test ends process (\d+)", after)
    failed = []
    if not ended or ended.group(1) == "0":
        return ["the shown tab had no web process to end: %s" % (ended and ended.group(0))]
    pid = ended.group(1)
    # The function an address is in, by the Debug Kit, from the image's
    # own symbols in /Home and demangled (`docs/debug.md`).
    if not said(guest, since, "jetski: test names %s + 0x4" % NAMED[1], 10):
        failed.append("the Debug Kit did not name %#x as %s + 0x4: %s"
                      % (NAMED[0], NAMED[1],
                         re.findall(r"jetski: (?:test names|no function)[^\r\n]*",
                                    guest.seen[since:]) or "nothing said"))
    # Collected: the zombie gone from Nebula's table, and its death known.
    if not said(guest, since, "jetski: process %s ended, code" % pid, 10):
        failed.append("JetSki never collected its ended web process %s" % pid)
    if not said(guest, since, "jetski: process   web, %s" % pid, 10) \
       or not said(guest, since, "jetski: ended     with code", 10):
        failed.append("the stopped page does not say which process ended or how: %s"
                      % re.findall(r"jetski: [^\r\n]*", guest.seen[since:])[-6:])
    time.sleep(1.5)                     # the stopped page, painted and composed
    shot = guest.screendump()
    R.write_png(os.path.join(KEPT, "jetski-stopped.png"), shot)
    x, y, w, h = window
    count, _ = block_at(R.parse_ppm, shot, x, y, w, h, FIELD)
    if count >= 200:
        failed.append("the page that stopped is still drawn: %d of its field's pixels" % count)
    return failed


# **The test's key** (`stopped`): Control-K ends the shown tab's web
# process, as a fault would have, by JetSki's own right over its children.
# Only in the suite's copy of `jetski.lua`.
HOOK = ("if c == 4 then return win:on_drop(\"files\", \"/Home/Pages/dropped.html\\n\") end "
        "if c == 11 then web.tabs(tabs) for _, t in ipairs(tabs) do if t.shown then "
        "print(\"jetski: test ends process \" .. t.pid) sys.kill(t.pid) end end "
        "print(\"jetski: test names \" .. tostring(look_up(NAMED_AT))) "
        "return true end\n")


def named_function(built):
    """A function of WebKit's in the image, its address and its name as
    binutils demangles it - one with a size, in a class, and no template,
    so libc++abi's demangler and `nm -C` spell it alike. The test's key
    asks the Debug Kit to name an address inside it (`docs/debug.md`): a
    page the test ends was killed rather than faulted, so it has no
    instruction of its own to name."""
    out = subprocess.run([("x86_64-elf-" if "x86_64" in IMAGE else "aarch64-elf-") + "nm",
                          "-S", "-C", "--defined-only", built],
                         check=True, capture_output=True, text=True).stdout
    for line in out.splitlines():
        got = re.match(r"([0-9a-f]+) ([0-9a-f]+) T (WebKit::WebPageProxy::\w+\([^<>]*\))$", line)
        if got and int(got.group(2), 16) > 8:
            return int(got.group(1), 16) + 4, got.group(3)
    raise SystemExit("run_jetski: no WebKit::WebPageProxy function in %s to name" % built)


# A character as QEMU's `sendkey` names its key.
QEMU_KEYS = {":": "shift-semicolon", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}


def tabbed(guest, R, window, sw, sh):
    """Tabs (`docs/webkit.html`): Control-T opens one and an address typed
    loads in it, Super-1 goes back to the first, its close closes it, and a
    link to a new window - `target=_blank` - opens in a tab of its own."""
    def step(keys, want, seconds=20):
        since = len(guest.seen)
        for key in keys:
            guest.sendkey(key)
        return said(guest, since, want, seconds)

    if not step(["ctrl-t"], "jetski: tab 2 of 2"):
        return ["Control-T opened no tab: %s"
                % (re.findall(r"jetski: [^\r\n]*", guest.seen)[-2:])]
    time.sleep(0.5)
    typed = [QEMU_KEYS.get(c, c) for c in Page.address + "\n"]
    if not step(typed, "jetski: title " + TITLE, 30):
        return ["an address typed into a new tab did not load there: %s"
                % (re.findall(r"jetski: [^\r\n]*", guest.seen)[-3:])]
    if not step(["meta_l-1"], "jetski: tab 1 of 2"):
        return ["Super-1 did not go back to the first tab"]
    # Its close, at the shown tab's right end: the first tab, at the
    # strip's left, as wide as a tab is (`TAB_W` in `jetski.lua`).
    x, y, w, h = window
    since = len(guest.seen)
    guest.mouse_to(*R._to_tablet(x + 8 + 210 - 17, y + 6 + 16, sw, sh))
    time.sleep(0.3)
    guest.mouse_button(True)
    time.sleep(0.15)
    guest.mouse_button(False)
    if not said(guest, since, "jetski: tab 1 of 1", 20):
        return ["the shown tab's close did not close it"]

    time.sleep(1.5)                     # the tab left, painted and composed
    shot = guest.screendump()
    count, link = block_at(R.parse_ppm, shot, x, y, w - 300, h - 100, BLANK)
    if count < 200:
        R.write_png(os.path.join(KEPT, "jetski-tabs.png"), shot)
        return ["the tab left after the close is not the first page: its link to a new tab "
                "is not on the screen (%d of its pixels)" % count]
    since = len(guest.seen)
    cx, cy = link[0] + link[2] // 2, link[1] + link[3] // 2
    guest.mouse_to(*R._to_tablet(cx, cy, sw, sh))
    time.sleep(0.3)
    guest.mouse_button(True)
    time.sleep(0.15)
    guest.mouse_button(False)
    if not said(guest, since, "jetski: a page opened a tab", 20) \
       or not said(guest, since, "jetski: tab 2 of 2", 5) \
       or not said(guest, since, "jetski: title " + SECOND, 30):
        return ["a link to a new window did not open a tab of its own: %s"
                % (re.findall(r"jetski: [^\r\n]*", guest.seen[since:])[-3:] or "nothing said")]
    time.sleep(1.5)
    R.write_png(os.path.join(KEPT, "jetski-tabs.png"), guest.screendump())
    return []


def resized(guest, R, window, mark, sw, sh):
    """The grip, the window's bottom right corner, dragged 300 left and 100
    up: the window manager resizes the frame, JetSki lays itself out again
    and tells WebKit, and the page's `resize` event says its new size."""
    x, y, w, h = window
    since = len(guest.seen)
    gx, gy = x + w - 6, y + h - 6
    guest.mouse_to(*R._to_tablet(gx, gy, sw, sh))
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)
    for step in range(1, 6):
        guest.mouse_to(*R._to_tablet(gx - 60 * step, gy - 20 * step, sw, sh))
        time.sleep(0.15)
    guest.mouse_button(False)
    time.sleep(0.4)
    guest.mouse_to(*R._to_tablet(sw - 30, sh - 30, sw, sh))
    if not said(guest, since, "jetski: resized to ", 30):
        return ["the grip was dragged and JetSki was not resized: %s"
                % (re.findall(r"(?:wm|jetski): [^\r\n]*", guest.seen[since:])[-3:] or "nothing said")]
    if not said(guest, since, "jetski: title resized ", 30):
        return ["JetSki was resized and the page was not told"]
    got = re.findall(r"jetski: resized to (\d+)x(\d+)", guest.seen[since:])[-1]
    page = re.findall(r"jetski: title resized (\d+)x(\d+)", guest.seen[since:])[-1]
    if int(got[0]) != w - 300 or int(page[0]) != int(got[0]):
        return ["the grip dragged 300 left made the window %sx%s and the page %sx%s, not %d wide"
                % (got + page + (w - 300,))]
    return []


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


def make_film():
    """Six seconds of FILM_COLOUR, VP9 in WebM, or None without FFmpeg."""
    out = os.path.join(WORK, "film.webm")
    made = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
         "color=c=0x%02x%02x%02x:size=%dx%d:rate=25:duration=6" % (FILM_COLOUR + FILM_SIZE),
         "-c:v", "libvpx-vp9", "-pix_fmt", "yuv420p", "-b:v", "200k", out],
        capture_output=True)
    return out if made.returncode == 0 and os.path.exists(out) else None


def make_appended():
    """The MediaSource's film: six seconds of APPENDED_COLOUR, its picture
    VP9 and its sound Opus, each in a WebM of its own; or None."""
    picture = os.path.join(WORK, "appended-picture.webm")
    sound = os.path.join(WORK, "appended-sound.webm")
    made = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
         "color=c=0x%02x%02x%02x:size=%dx%d:rate=25:duration=6" % (APPENDED_COLOUR + FILM_SIZE),
         "-c:v", "libvpx-vp9", "-pix_fmt", "yuv420p", "-b:v", "200k", "-g", "25", picture],
        capture_output=True)
    tone = ("anullsrc=r=48000:cl=mono:d=6" if BROKEN == "silent"
            else "sine=frequency=%d:sample_rate=48000:duration=6" % TONE)
    heard = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
         tone, "-c:a", "libopus", sound],
        capture_output=True)
    if made.returncode or heard.returncode or not os.path.exists(picture) or not os.path.exists(sound):
        return None
    return (picture, sound)


TONE = 440
HEARD = os.path.join(WORK, "heard.wav")


def sound():
    """The appended film's tone, in what the machine's sound card played."""
    if not os.path.exists(HEARD):
        return ["the machine's sound card wrote nothing"]
    with open(HEARD, "rb") as f:
        data = f.read()
    at = data.find(b"fmt ")
    if at < 0 or data.find(b"data") < 0:
        return ["the sound card's WAV has no format or no data"]
    channels, rate = struct.unpack("<HI", data[at + 10:at + 16])
    pcm = data[data.find(b"data") + 8:]
    n = len(pcm) // 2
    left = struct.unpack("<%dh" % n, pcm[:n * 2])[::max(1, channels)]
    # Loud stretches, a tenth of a second at a time, and the tone's pitch
    # from the zero crossings in them.
    step = rate // 10
    loud, crossings = 0, 0
    for start in range(0, len(left) - step, step):
        block = left[start:start + step]
        if math.sqrt(sum(v * v for v in block[::8]) / len(block[::8])) < 1000:
            continue
        loud += 1
        crossings += sum(1 for a, b in zip(block, block[1:]) if (a < 0) != (b < 0))
    seconds = loud / 10
    if seconds < 4:
        return ["the appended film's tone was heard for %.1f s of its 6" % seconds]
    pitch = crossings / 2 / seconds
    if abs(pitch - TONE) > 20:
        return ["what was heard is %.0f Hz, not the film's %d" % (pitch, TONE)]
    return []


def near(parse_ppm, data, x, y, w, h, colour, slack=12):
    """How many pixels in the window are within `slack` of `colour`."""
    sw, sh, rgb = parse_ppm(data)
    count = 0
    for row in range(max(0, y), min(y + h, sh)):
        line = rgb[row * sw * 3:(row + 1) * sw * 3]
        for col in range(max(0, x), min(x + w, sw)):
            r, g, b = line[col * 3:col * 3 + 3]
            if abs(r - colour[0]) <= slack and abs(g - colour[1]) <= slack \
                    and abs(b - colour[2]) <= slack:
                count += 1
    return count


def film(R, picture, window):
    """The second page's film: playing, its size, and its colour on the screen."""
    if FILM is None:
        return ["this Mac's FFmpeg made no film (libvpx-vp9)"]
    failed = []
    said = {k: v[0] for k, v in (Page.film or {}).items()}
    if not said:
        failed.append("the film never played a second, nor said why")
    elif "error" in said:
        failed.append("the film did not play: MediaError %s" % said["error"])
    else:
        if said.get("size") != "%dx%d" % FILM_SIZE:
            failed.append("the film is %s, not %dx%d" % ((said.get("size"),) + FILM_SIZE))
        if said.get("webm") != "probably":
            failed.append("canPlayType(VP9 in WebM) is \"%s\", not \"probably\"" % said.get("webm"))
    count = near(R.parse_ppm, picture, *window, FILM_COLOUR)
    if count < FILM_SIZE[0] * FILM_SIZE[1] * 8 // 10:
        failed.append("the film's picture is not in the window: %d of its %d pixels"
                      % (count, FILM_SIZE[0] * FILM_SIZE[1]))
    return failed


def appended(R, picture, window):
    """The MediaSource's film: playing, its size, all of it buffered, what
    isTypeSupported says, and its colour on the screen."""
    if APPENDED is None:
        return ["this Mac's FFmpeg made no film to append (libvpx-vp9, libopus)"]
    failed = []
    said = {k: v[0] for k, v in (Page.appended or {}).items()}
    if not said:
        failed.append("the appended film never played a second, nor said why")
    elif "error" in said:
        failed.append("the appended film did not play: %s" % said["error"])
    else:
        if said.get("size") != "%dx%d" % FILM_SIZE:
            failed.append("the appended film is %s, not %dx%d" % ((said.get("size"),) + FILM_SIZE))
        at, end = float(said.get("waitedAt") or 0), float(said.get("waitedEnd") or 0)
        if not end - 0.25 <= at <= end + 0.05:
            failed.append("its picture ran out at %.3f s and the film waited at %.3f s, "
                          "not there" % (end, at))
        if float(said.get("buffered") or 0) < 5.5:
            failed.append("the appended film's buffered range ends at %s s, not near 6"
                          % said.get("buffered"))
        if said.get("vp9") != "true":
            failed.append("MediaSource.isTypeSupported(VP9 in WebM) is %s" % said.get("vp9"))
        if said.get("deep") != "false":
            failed.append("MediaSource.isTypeSupported(10-bit VP9) is %s, which is not shown yet"
                          % said.get("deep"))
    count = near(R.parse_ppm, picture, *window, APPENDED_COLOUR)
    if count < FILM_SIZE[0] * FILM_SIZE[1] * 8 // 10:
        failed.append("the appended film's picture is not in the window: %d of its %d pixels"
                      % (count, FILM_SIZE[0] * FILM_SIZE[1]))
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

    global FILM, APPENDED
    FILM = None if LIVE else make_film()
    APPENDED = None if LIVE else make_appended()
    server = Server(("0.0.0.0", 0), Page)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    global PAGE
    address = LIVE or "http://10.0.2.2:%d/" % server.server_address[1]
    Page.address = address
    if LIVE and LIVE.startswith("file:"):
        with open(LIVE[5:], "rb") as f:
            PAGE = f.read()
        address = "http://10.0.2.2:%d/" % server.server_address[1]

    global NAMED
    NAMED = named_function(built)
    image = os.path.join(WORK, "jetski.elf")
    subprocess.run([("x86_64-elf-" if "x86_64" in IMAGE else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)
    lua = os.path.join(ROOT, "user", "installed", "JetSki", "jetski.lua")
    if BROKEN == "foreign-cookie":
        PAGE = PAGE.replace(SCRIPT_COOKIE.encode(), b"")
    if BROKEN == "fixed":
        # Laid out again at the new size, and WebKit never told.
        with open(lua) as f:
            text = f.read().replace("web.resize(W, VIEW_H)", "do end")
        lua = os.path.join(WORK, "jetski.lua")
        with open(lua, "w") as f:
            f.write(text)
    if BROKEN == "tabless":
        # Control-T, the window's, never answered.
        with open(lua) as f:
            text = f.read().replace("if c == 20 then new_tab() return true end", "")
        lua = os.path.join(WORK, "jetski.lua")
        with open(lua, "w") as f:
            f.write(text)
    if BROKEN == "unannounced":
        # A link followed, and nothing shown until WebKit begins the load.
        with open(lua) as f:
            text = f.read().replace("if st.asked then", "if false then")
        lua = os.path.join(WORK, "jetski.lua")
        with open(lua, "w") as f:
            f.write(text)
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
    with open(lua) as f:
        text = f.read()
    text = text.replace("if c == 12 or c == 15 or c == 20 then return true end",
                        "if c == 12 or c == 15 or c == 20 or c == 11 or c == 4 then return true end")
    text = text.replace("win.on_key = function(_, c)\n", "win.on_key = function(_, c)\n"
                        + HOOK.replace("NAMED_AT", "0x%x" % NAMED[0]))
    if BROKEN == "nameless":
        # The Debug Kit never asked.
        text = text.replace("debugkit.name(IMAGE, pc)", "nil, \"not asked\"")
    if BROKEN == "pathless":
        # A path typed is a search, not a file.
        text = text.replace('if text:match("^%s*/") then', 'if false then')
    if BROKEN == "openless":
        # Control-O, never answered.
        text = text.replace("if c == 15 then\n    local chooser", "if false then\n    local chooser")
    if BROKEN == "dropless":
        # A file dropped, never taken.
        text = text.replace('if kind ~= "files" then return false end\n  local first',
                            'do return false end\n  local first')
    if BROKEN == "uninspected":
        # Control-Alt-I and the button, never asking WebKit.
        text = text.replace("web.inspect(not inspecting)", "do end")
    if BROKEN == "unpainted":
        # Docked and never drawn.
        text = text.replace("web.paint_inspector(s,", "local _ = (function() end)(s,")
    if BROKEN == "unexplained":
        # The page that stopped, never asked why.
        text = text.replace("crash = web.crash(st.tab)", "crash = nil")
    lua = os.path.join(WORK, "jetski.lua")
    with open(lua, "w") as f:
        f.write(text)
    local_html = os.path.join(WORK, "local.html")
    local_js = os.path.join(WORK, "local.js")
    with open(local_html, "wb") as f:
        f.write(LOCAL_HTML)
    with open(local_js, "wb") as f:
        f.write(LOCAL_JS)
    chosen_html = os.path.join(WORK, "a-chosen.html")
    dropped_html = os.path.join(WORK, "dropped.html")
    with open(chosen_html, "wb") as f:
        f.write(CHOSEN_HTML)
    with open(dropped_html, "wb") as f:
        f.write(DROPPED_HTML)
    files = [(lua, "/Home/Apps/JetSki/jetski.lua"),
             (image, "/Home/Apps/JetSki/jetski.elf"),
             (local_html, "/Home/Pages/local.html"),
             (local_js, "/Home/Pages/local.js"),
             (chosen_html, "/Home/Pages/a-chosen.html"),
             (dropped_html, "/Home/Pages/dropped.html")]
    # Web Inspector's front end, as `installed.py` puts it beside JetSki.
    import inspector
    files += [(host, "/Home/Apps/JetSki/Inspector/" + inside) for host, inside in inspector.files()]
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
    #
    # And Web Inspector is a fourth web process beside three tabs', reading
    # 4 MB of script into JavaScriptCore's parser: at 512 MB the machine ran
    # out under it, and the parser met a reserved page Nebula had no page
    # to fill (`testing.md` 18.828). A gigabyte, which is still less than
    # any machine it is meant for.
    args = getattr(R, board)
    setattr(R, board, [(("2G" if LIVE else "1G") if a == "512M" else a) for a in args])
    setattr(R, board, getattr(R, board) + [
        "-netdev", "user,id=net0", "-device", R.device(IMAGE, "net") + ",netdev=net0"])
    # A sound card, whose output is kept: the film's sound must come out.
    if R.machine(IMAGE) == "x86_64":
        if os.path.exists(HEARD):
            os.remove(HEARD)
        setattr(R, board, getattr(R, board) + [
            "-audiodev", "wav,id=snd0,path=%s" % HEARD,
            "-device", "ich9-intel-hda", "-device", "hda-output,audiodev=snd0"])

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
        time.sleep(2 + LINGER)  # the last picture, painted and composed
        guest._read_available()
        seen = guest.seen[mark:]
        where = re.search(r"wm: window JetSki at (\d+),(\d+) (\d+)x(\d+)", seen)
        dump = guest.screendump()
        if LINGER:
            # A second picture, three seconds on: a film that plays has
            # moved between them, where a poster has not.
            time.sleep(3)
            R.write_png(os.path.join(KEPT, "live-later.png"), guest.screendump())
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
        if not LIVE:
            failed.extend(sound())

    checks = 37
    if failed:
        print("FAIL: %d of %d checks on JetSki's window (the picture is %s):\n  "
              % (len(failed), checks, picture) + "\n  ".join(failed))
        return 1
    print("PASS: JetSki's window - opened from the shell, installed in /Home/Apps/JetSki, "
          "a page fetched over HTTP by WebKit's network process and painted below the "
          "address field, %dx%d at %d,%d in the window, and the title the page's; "
          "then its link clicked to a second page, shown as followed the moment it was clicked, the wheel turned and its script told "
          "the page scrolled, and its field clicked and \"%s\" typed into it; and the "
          "second page asked for with the server's cookie and the script's, the lasting "
          "one kept on /Home; and the page saw \"%s\" as the platform, its window "
          "1200x800 and a canvas read back as a PNG; and a VP9 film in WebM played on "
          "the second page by WebKit's Media Kit engine, its picture in the window; and "
          "beside it a film appended in pieces to a MediaSource, VP9 and Opus in "
          "SourceBuffers of their own, played, all of it buffered, on the screen, and "
          "its tone heard from the sound card; and the window resized by its grip, the "
          "page told its new width; and a tab's web process ended, collected, and the "
          "stopped page drawn in its place, saying which process and how"
          % (box[2], box[3], box[0] - x, box[1] - y, TYPED, PLATFORM))
    return 0


if __name__ == "__main__":
    sys.exit(main())
