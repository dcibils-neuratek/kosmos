#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The browser, with a page in it.

`make web` proves the NetSurf libraries parse. It says nothing about whether
anything is *drawn*, and drawing is the whole of the difference between a
parser and a browser - so this boots an image built with `WEB=1`, serves a
page from this Mac, points the browser at it, and looks at the screen.

**The page comes from here rather than from the internet.** slirp maps this
computer as 10.0.2.2, so a server started in this process is reachable from
the guest without a packet leaving the machine: deterministic, offline, and
about the renderer rather than about somebody else's uptime. It is the same
arrangement `run_network.py` uses for `fetch`.

What is checked is none of it subtle, which is deliberate - this is
mostly a camera, and a check that goes stale is worse than no check:

  * **The page area has ink on it.** A window that opened and rendered
    nothing is the exact failure mode of every stage of this so far, and it
    looks identical to a working browser in a thumbnail.
  * **More than one text size is present.** The reason the browser draws its
    own pixels is that the compositor would rasterise every line in one
    face. Runs of dark pixels of two clearly different heights is the
    cheapest evidence that a heading is a heading.
  * **It scrolls.** The page is laid out once and painted a band at a time
    into a surface taller than the window, and a scroll is one blit out of
    it, which is the whole reason for the mode - so the picture after six
    presses of Down has to differ from the picture before them. A browser
    that laid out correctly and would not move is a browser nobody can read
    the bottom of.
  * **Reload works when it is clicked.** A direct window has no widgets, so
    every control in the chrome is a rectangle this application knows the
    position of and a click is a comparison against it. Nothing else here
    exercises that arithmetic, and the server on this side can simply count
    how many times it was asked for the page.
  * **Home needs nothing running anywhere.** `about:start` is a page
    compiled into `browser.lua`, and clicking Home has to render it without
    the server being asked for anything. That is the property that makes a
    new build of Kosmos something you can try rather than something you have
    to set up a web server for, and it is the one most easily lost.
  * **A link is drawn as one and can be followed.** Links are painted in a
    blue nothing else on the page uses, so the harness finds one by colour -
    which also establishes that the run knew it was inside an `<a>`. Clicking
    it has to make the server serve the *other* page, which is six separate
    things at once: boxes kept, the click turned into a page coordinate, the
    run found, the relative address resolved, the fetch made, and the result
    laid out.
  * **An address can be typed** (`roadmap.md` 6zz b). Control-L selects the
    bar and what is typed replaces it, `http://` and all - which the first
    try found appending to the old address - and the second page's blue
    heading has to appear.
  * **Back leaves it**, with the server asked for nothing.
  * **NetSurf's own code runs** (`roadmap.md` 6zz j1): `web.join` resolves
    three addresses through its URL parser.
  * **NetSurf lays the test page out and draws it** (6zz j2, j3): the CSS
    box's border and ground and the table's cell borders on the surface -
    none of which the browser's own layout ever drew - and the first link
    found under its place.
  * **A linked stylesheet is fetched and applied** (6zz j4): `linked.css`
    puts a paragraph on a green ground nothing else on the page has.
  * **A class after a line break is a class**: the test page's second
    paragraph is maroon by a class that follows a newline in its attribute,
    which the cascade finds in the list libdom keeps on the element.
  * **Both pictures are drawn.** The test page's PNG carries a magenta
    square and its JPEG a cyan one, and the page is paged down until both
    have been on the screen.
  * **The end of a page longer than any band** (`roadmap.md` 6zz j): a page
    made for the run, 2,000 paragraphs and more than forty thousand pixels,
    with the PNG as the last thing on it. The picture is not fetched before
    the page is shown; `G` goes to the end and its magenta has to be on the
    screen; and `g` and `G` again show it without asking for it twice.

**The page is `assets/www/index.html`**, the browser's test page and its
page benchmark (`roadmap.md` 6zz a): every part of it says what it should
look like, and `make www` puts it in the QEMU disk's `/Home/www`.

Usage: run_browser.py <image> --out <file.png> [--page <file.html>]
"""

import argparse
import html
import http.server
import os
import re
import shutil
import ssl
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from run_screenshot import (Guest, Failure, PROMPT, _to_tablet,  # noqa: E402
                            parse_ppm, settle)
from run_gallery import png                                      # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

# What the page's own links point at, and therefore what the server must be
# asked for once one of them is clicked: the test page's first link
# (`assets/www/`, `roadmap.md` 6zz a).
LINKED = "second.html"

# How long the long page is, in paragraphs: a line and a bit each at the
# window's width, about 24 pixels, so 48,000 pixels - the Dam article is
# 52,803 - where the band the browser paints is three screens.
LONG_PARAGRAPHS = 2000

# The second page's heading, #2a55c9 - which the test page and the start
# page do not use - and the test page's pictures' marks: a square of pure
# magenta in the PNG and of pure cyan in the JPEG. Not the second page's
# pale yellow ground, which the browser does not paint yet (the box model,
# `roadmap.md`, the browser) - the page says it should, which is what a
# checklist is for.
SECOND_BLUE = (42, 85, 201)

# The test page's maroon, #b03060: the `broken` class, which follows a line
# break in its paragraph's attribute. Nothing else on the page is this colour.
CLASS_MAROON = (176, 48, 96)

# The ground `linked.css` gives its paragraph, #3fa06a.
LINKED_GREEN = (63, 160, 106)

# A page whose server says ISO-8859-1 and whose <meta> says UTF-8, with a
# title and a field's value outside ASCII - Google's Spanish page, in small
# (`roadmap.md` 6zz j7). The header is right about the bytes, and wins.
LATIN1_PAGE = (
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>B\u00fasqueda</title>"
    "<style>#ask { background: #8a5a00; color: #ffffff; }</style></head>"
    "<body><form action=\"found.html\" method=\"get\"><p>"
    "<input type=\"text\" name=\"q\" value=\"\u00f1and\u00fa\"> "
    "<input id=\"ask\" type=\"submit\" value=\"Buscar\"></p></form>"
    "</body></html>")
ASK = (138, 90, 0)

# The cache's page: asked about every time, by its ETag, with a picture on it
# that may be used for ten minutes (`roadmap.md` 6zz k).
CHECKED_PAGE = ("<!doctype html><html><head><title>Kept</title></head><body>"
                "<h1>Kept, and asked about</h1>"
                "<p><img src=\"cached.png\" width=\"240\" height=\"135\"></p>"
                "</body></html>")

# Where the page is, inside the window, and the window is opened at a size
# this file and `browser.lua` both know. Content coordinates: the compositor
# adds a title bar above them, which `find_window` finds.
# `TOOL` is the header now - the kit's, `ui.layout.head` (`roadmap.md`
# 6zz d1) - and the status line is the drawing's 26.
TOOL, STAT, SBAR, PAD = 46, 26, 16, 8
WIN_W, WIN_H = 900, 640


def reader(px, width):
    """`(x, y) -> (r, g, b)` over the pixel bytes a screendump parsed into.

    `run_screenshot.pixel_reader` takes the whole PPM; `settle` hands its
    predicate the pixels already parsed out of one, so this is the same three
    lines over the half that is left.
    """
    def at(x, y):
        o = (y * width + x) * 3
        return tuple(px[o:o + 3])

    return at


# How many requests for a picture were being answered at once, at most: the
# pictures are held a moment each, so ones fetched one after another never
# overlap and ones fetched together do (`http.get_many`, `roadmap.md` 6zz g).
PICTURES = {"now": 0, "peak": 0, "lock": threading.Lock()}


def serve(directory, asked, tls=None):
    """An HTTP server on an ephemeral port, in a thread. Returns the port.

    `asked` is a list the handler appends every path to, which is how the
    Reload check knows the button did something rather than merely looking
    pressed. `tls`, an `ssl.SSLContext`, makes it HTTPS: a handshake the
    browser refuses fails inside `accept`, which the server shrugs off.
    """
    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=directory, **kw)

        def answer(self, said):
            page = ("<!doctype html><html><head><title>Answered</title></head>"
                    "<body><h1>Answered</h1><p>%s</p></body></html>"
                    % html.escape(said)).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(page)))
            self.end_headers()
            self.wfile.write(page)

        # **Every file asked about, every time** (`roadmap.md` 6zz k): the
        # checks here count what the server is asked, and the browser's cache
        # would otherwise answer a second visit from what it kept - rightly:
        # this server sends `Last-Modified`, and a tenth of a file's age is
        # fresh. `no-cache` keeps it asking, which a 304 then answers. The
        # cache's own pages say what they mean instead (`_said_cache`).
        def end_headers(self):
            if not getattr(self, "_said_cache", False):
                self.send_header("Cache-Control", "no-cache")

            super().end_headers()

        def send_header(self, name, value):
            if name.lower() == "cache-control":
                self._said_cache = True

            super().send_header(name, value)

        def do_GET(self):
            self._said_cache = False
            asked.append(self.path)

            # The test page's first form, sent: what it asked for, said back.
            if self.path.startswith("/found.html?"):
                self.answer("asked for " + self.path)
                return

            # The cache (`roadmap.md` 6zz k): a page asked about every time
            # by its ETag, answered 304 when it has not changed - the request
            # kept with the header it came with - and a picture that may be
            # used for ten minutes without asking.
            if self.path == "/checked.html":
                inm = self.headers.get("If-None-Match")

                if inm:
                    asked[-1] = "/checked.html If-None-Match " + inm

                # And, as Wikipedia's 304 does, the encoding of what it stands
                # for - with no body to inflate.
                if inm == '"v1"':
                    self.send_response(304)
                    self.send_header("ETag", '"v1"')
                    self.send_header("Cache-Control", "no-cache")
                    self.send_header("Content-Encoding", "gzip")
                    self.end_headers()
                    return

                page = CHECKED_PAGE.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("ETag", '"v1"')
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Content-Length", str(len(page)))
                self.end_headers()
                self.wfile.write(page)
                return

            if self.path == "/cached.png":
                with open(os.path.join(directory, "kosmos.png"), "rb") as f:
                    picture = f.read()

                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Cache-Control", "max-age=600")
                self.send_header("Content-Length", str(len(picture)))
                self.end_headers()
                self.wfile.write(picture)
                return

            # A page in ISO-8859-1 by its header and UTF-8 by its <meta>, as
            # Google serves Latin America (`roadmap.md` 6zz j7).
            if self.path == "/latin1.html":
                page = LATIN1_PAGE.encode("latin-1")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=ISO-8859-1")
                self.send_header("Content-Length", str(len(page)))
                self.end_headers()
                self.wfile.write(page)
                return

            if not self.path.endswith((".png", ".jpg")):
                super().do_GET()
                return

            with PICTURES["lock"]:
                PICTURES["now"] += 1
                PICTURES["peak"] = max(PICTURES["peak"], PICTURES["now"])

            try:
                time.sleep(0.4)
                super().do_GET()
            finally:
                with PICTURES["lock"]:
                    PICTURES["now"] -= 1

        # The test page's second form: a POST, kept as "POST path body".
        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(n).decode("latin-1") if n > 0 else ""
            asked.append("POST %s %s" % (self.path, body))
            self.answer("posted " + body)

        def log_message(self, *a):
            pass

    # Threads, so pictures asked for together are answered together.
    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Handler)
    httpd.daemon_threads = True

    if tls is not None:
        httpd.socket = tls.wrap_socket(httpd.socket, server_side=True)

    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()

    return httpd, httpd.server_address[1]


def dark_rows(px, width, x0, y0, w, h):
    """Which rows in a rectangle have ink on them.

    Ink rather than "not the background", because the page is white and the
    chrome is not: a row is inked if it holds a pixel darker than a mid grey,
    which is text on paper and is not paper, a rule, or a window edge.
    """
    at = reader(px, width)
    rows = []

    for y in range(y0, y0 + h):
        n = 0

        for x in range(x0, x0 + w, 2):
            r, g, b = at(x, y)

            if r + g + b < 260:
                n += 1

        rows.append(n)

    return rows


def runs_of_ink(rows, floor=2):
    """The heights of the consecutive inked stretches, which are text lines."""
    out = []
    run = 0

    for n in rows:
        if n >= floor:
            run += 1
        elif run:
            out.append(run)
            run = 0

    if run:
        out.append(run)

    return out


def find_link(px, width, x0, y0, w, h, last=False):
    """The middle of the first stretch of link-blue text, or None - or of the
    last, from the bottom up, when `last`.

    By colour rather than by position: `web_paint.c` paints a run inside an
    `<a>` in a blue nothing else on a page uses, so finding one is also the
    check that the run knew where it came from. The test is loose because
    glyphs are antialiased against white - only the fully covered pixels of
    a stem are the ink itself.
    """
    at = reader(px, width)
    rows = range(y0 + h - 1, y0 - 1, -1) if last else range(y0, y0 + h)

    def blue_at(x, y):
        r, g, b = at(x, y)
        return b > 130 and b > r + 60 and b > g + 40

    # From the bottom the first blue is the underline, which lies below the
    # word's box and so is not the link: the middle of the word is, found by
    # climbing through its letters, across the gap above the underline.
    def middle(bottom, left, right):
        top, empty = bottom, 0

        for y in range(bottom - 1, max(y0, bottom - 40), -1):
            if any(blue_at(x, y) for x in range(left, right)):
                top, empty = y, 0
            else:
                empty += 1

                if empty > 3:
                    break

        return (top + bottom) // 2

    for y in rows:
        run = None

        for x in range(x0, x0 + w):
            r, g, b = at(x, y)
            blue = b > 130 and b > r + 60 and b > g + 40

            if blue:
                if run is None:
                    run = x
            elif run is not None:
                if x - run >= 6:
                    return (run + x) // 2, middle(y, run, x) if last else y
                run = None

    return None


def find_page(width, height, px):
    """The top-left of the browser's page area, or None if it is not there.

    Found by its *paper*: `web_paint.c` fills a page with white, and nothing
    else on this desktop is 900 pixels of white with a scrollbar beside it.
    Searching for it rather than computing it from the window position means
    the check does not depend on where the compositor decided to put the
    window.
    """
    at = reader(px, width)
    run_needed = WIN_W - SBAR - 40

    for y in range(0, height - 40, 4):
        run = 0

        for x in range(0, width):
            r, g, b = at(x, y)

            if r > 245 and g > 245 and b > 245:
                run += 1

                if run >= run_needed:
                    return x - run + 1, y
            else:
                run = 0

    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--out", required=True)
    ap.add_argument("--page", default=os.path.join(os.path.dirname(HERE), "assets",
                                                   "www", "index.html"))
    ap.add_argument("--timeout", type=int, default=240)
    args = ap.parse_args()

    directory = os.path.dirname(os.path.abspath(args.page))
    name = os.path.basename(args.page)

    asked = []
    httpd, port = serve(directory, asked)

    #
    # **And over TLS** (`roadmap.md` 6zz c): an authority made for the run,
    # a certificate for 10.0.2.2 it signed and one another authority signed,
    # the same page served under each - and the first authority in the
    # guest's `/Home/Preferences/Authorities`, on a disk made for the run,
    # which is how a person adds one of their own.
    #
    import run_tls
    import scratch

    work = scratch.directory("browser-tls")
    run_tls.pki(work, "10.0.2.2")

    def context(cert):
        c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        c.minimum_version = ssl.TLSVersion.TLSv1_2
        c.load_cert_chain(os.path.join(work, cert + ".pem"),
                          os.path.join(work, "server.key"))
        return c

    tls_asked = []
    https_good, good_port = serve(directory, tls_asked, context("good"))
    https_other, other_port = serve(directory, tls_asked, context("untrusted"))

    disk = os.path.join(work, "disk.img")
    subprocess.run([os.path.join(os.path.dirname(HERE), "build", "host", "lua"),
                    os.path.join(HERE, "kfs.lua"), "create", disk, "16",
                    os.path.join(work, "ca.der")
                    + ":/Home/Preferences/Authorities/test.der"],
                   check=True, capture_output=True)

    #
    # **A page longer than any band** (`roadmap.md` 6zz j), made here: the
    # test PNG as the very last thing on it, from a server of its own so what
    # it was asked for is its alone.
    #
    long_dir = os.path.join(work, "long")
    os.makedirs(long_dir, exist_ok=True)
    shutil.copy(os.path.join(os.path.dirname(HERE), "assets", "www", "kosmos.png"),
                long_dir)

    with open(os.path.join(long_dir, "long.html"), "w") as f:
        f.write("<!doctype html>\n<html><head><title>A long page</title></head>"
                "<body>\n<h1>A long page</h1>\n")

        for i in range(1, LONG_PARAGRAPHS + 1):
            f.write(f"<p>Paragraph {i} of {LONG_PARAGRAPHS}, on a page longer than "
                    "any band the browser paints at once: it is laid out whole and "
                    "painted where it is read.</p>\n")

        f.write('<p><img src="kosmos.png" width="240" height="135" '
                'alt="the last thing on the page"></p>\n</body></html>\n')

    long_asked = []
    long_httpd, long_port = serve(long_dir, long_asked)

    import run_screenshot
    saved = (run_screenshot.QEMU_ARGS, run_screenshot.X86_ARGS)
    run_screenshot.extra_args(args.image, [
        "-netdev", "user,id=net0",
        "-device", run_screenshot.device(args.image, "net") + ",netdev=net0",
        "-drive", f"file={disk},format=raw,if=none,id=disk",
        "-device", run_screenshot.device(args.image, "blk") + ",drive=disk",
    ])

    guest = None

    #
    # **The NetSurf libraries built as NetSurf builds them**, with `NDEBUG`:
    # without it hubbub prints its state's name for every token, and the
    # browser had been built that way from the start (`Makefile`,
    # `WEB_CFLAGS`). The name of one of those states, with the newline the
    # debugging `printf` gives it, is in the image only when it is.
    #
    with open(args.image, "rb") as f:
        if b"AFTER_AFTER_FRAMESET\n" in f.read():
            print("FAIL: the NetSurf libraries in this image were built without "
                  "NDEBUG: hubbub prints its state for every token", file=sys.stderr)
            return 1

    try:
        guest = Guest(args.image, args.timeout)
        guest.wait_for(PROMPT, "reached a shell")

        #
        # **No DOM event nobody can hear** (`runtime/patches/netsurf/`): a
        # page parsed at the prompt, and libdom's count of the mutation
        # events it made and the ones it did not. With no listener anywhere
        # it made none - they were, with the clock each one read, most of
        # what parsing Wikipedia's Dam article cost.
        #
        mark = len(guest.seen)
        guest.type('local w = sys.kit("web") local d = w.parse("<ul><li>a</li><li>b</li></ul>") '
                   'local m, k = w.events() print("DOM EV" .. "ENTS made " .. m .. ", skipped " .. k)')
        events = guest.wait_for_line("DOM EVENTS made ", "counted its DOM events", since=mark)
        made_skipped = re.match(r"(\d+), skipped (\d+)", events)

        if made_skipped is None or made_skipped.group(1) != "0" \
                or int(made_skipped.group(2)) == 0:
            raise Failure(f"parsing a list made DOM events nobody could hear, or "
                          f"skipped none: made {events!r}")

        guest.wait_for(PROMPT, "the prompt again")

        #
        # **NetSurf's own code, running on the machine** (`roadmap.md` 6zz
        # j1): its URL parser, compiled for Kosmos with the rest of its
        # layout, reached through `web.join` - a Wikipedia link climbing out
        # with `..` and keeping its fragment, a picture's scheme-relative
        # address with `.` and `..` in its path, and a host in capitals,
        # lowered as NetSurf normalises one.
        #
        mark = len(guest.seen)
        guest.type('local w = sys.kit("web") print("JOIN" .. "ED " '
                   '.. tostring(w.join("https://en.wikipedia.org/wiki/Dam", '
                   '"../w/index.php?title=Dam&action=edit#top")) .. " " '
                   '.. tostring(w.join("http://10.0.2.2:8000/a/b.html", '
                   '"//thumb.wikimedia.org/x/./y/../z.png")) .. " " '
                   '.. tostring(w.join("http://10.0.2.2:8000/", '
                   '"HTTPS://WWW.Example.COM/A")))')
        joined = guest.wait_for_line("JOINED ", "joined three addresses", since=mark)
        want_joined = ("https://en.wikipedia.org/w/index.php?title=Dam&action=edit#top "
                       "http://thumb.wikimedia.org/x/z.png https://www.example.com/A")

        if joined.strip() != want_joined:
            raise Failure(f"NetSurf's URL code joined wrongly: {joined!r}, "
                          f"where {want_joined!r}")

        guest.wait_for(PROMPT, "the prompt once more")

        #
        # **The test page laid out and drawn by NetSurf** (`roadmap.md` 6zz
        # j2, j3), on the machine, into a surface: its default stylesheets
        # set up, the page laid out at the browser's width and drawn whole,
        # and three things counted that `web_paint.c` never drew - the CSS
        # box's blue border (#2a55c9) and pale blue ground (#eef2fb), and
        # the table's grey cell borders (#999999) - and the first link found
        # looking down the page's left side.
        #
        page_url = f"http://10.0.2.2:{port}/{name}"
        probe = ('local http = use("/Kosmos/Libraries/http.lua") local w = sys.kit("web") '
                 'w.setup(sys.asset("netsurf/default.css"), sys.asset("netsurf/quirks.css")) '
                 f'local r = http.get("{page_url}") local _, _, body = http.parse(r or "") '
                 f'local doc = w.parse(body) local h, why = doc:ns_layout(868, 584, "{page_url}") '
                 'if not h then print("NS" .. "BOX none " .. tostring(why)) return end '
                 'local s = gfx.surface{w = 868, h = h} doc:ns_paint(s, 868, h, 0) '
                 'local blue, ground, grid = 0, 0, 0 for y = 0, h - 1, 2 do for x = 0, 867, 2 do '
                 'local c = s:get(x, y) & 0xffffff if c == 0x2a55c9 then blue = blue + 1 '
                 'elseif c == 0xeef2fb then ground = ground + 1 elseif c == 0x999999 then grid = grid + 1 end end end '
                 'local link for y = 60, 400, 3 do link = doc:ns_link_at(60, y) if link then break end end '
                 'print("NS" .. "BOX " .. h .. " " .. blue .. " " .. ground .. " " .. grid .. " " '
                 '.. tostring(link))')
        parts = [probe[i:i + 600] for i in range(0, len(probe), 600)]

        for i, part in enumerate(parts):
            guest.type(f'fs.write("/Temporary/nsbox{i}.lua", [==[{part}]==])')
            guest.wait_for(PROMPT, "the probe written")

        guest.type('fs.write("/Temporary/nsbox.lua", '
                   + " .. ".join(f'fs.read("/Temporary/nsbox{i}.lua")' for i in range(len(parts)))
                   + ')')
        guest.wait_for(PROMPT, "the probe put together")
        mark = len(guest.seen)
        guest.type("/Temporary/nsbox.lua")
        boxed = guest.wait_for_line("NSBOX ", "the page laid out by NetSurf", since=mark)
        drawn = re.match(r"(\d+) (\d+) (\d+) (\d+) (\S+)", boxed.strip())

        if (drawn is None or int(drawn.group(1)) < 2000 or int(drawn.group(2)) < 100
                or int(drawn.group(3)) < 1000 or int(drawn.group(4)) < 100
                or drawn.group(5) != f"http://10.0.2.2:{port}/{LINKED}"):
            raise Failure(f"the test page laid out by NetSurf was not drawn as it asks: "
                          f"height, the box's border, its ground, the table's borders "
                          f"and the first link were {boxed.strip()!r}")

        print(f"NetSurf drew the test page: {boxed.strip()} "
              "(height, border, ground and table pixels, the first link)")
        guest.wait_for(PROMPT, "the prompt after NetSurf")

        #
        # **The cache on the disk** (`httpcache.lua`, `roadmap.md` 6zz k): a
        # reply kept in `/Home/Cache/Browser`, then found by a cache that
        # starts with nothing held - so from the file and its attributes,
        # on the guest's own filesystem, which the Mac's test stands in for.
        #
        probe = ('local hc = use("/Kosmos/Libraries/httpcache.lua") '
                 'local t = fs.read("/Devices/clock").epoch '
                 'local r = "HTTP/1.1 200 OK\\r\\nCache-Control: max-age=600'
                 '\\r\\nETag: \\"p1\\"\\r\\n\\r\\n" .. ("probe"):rep(300) '
                 'local ok, why = hc.open{}:store("http://probe/x", r, '
                 '{ scheme = "http" }, t) '
                 'local h = hc.open{}:lookup("http://probe/x", t + 5) '
                 'print("CA" .. "CHE", ok, why, h and h.fresh, '
                 'h and h.reply == r, h and h.etag)')
        parts = [probe[i:i + 600] for i in range(0, len(probe), 600)]

        for i, part in enumerate(parts):
            guest.type(f'fs.write("/Temporary/cache{i}.lua", [==[{part}]==])')
            guest.wait_for(PROMPT, "the cache's probe written")

        guest.type('fs.write("/Temporary/cacheprobe.lua", '
                   + " .. ".join(f'fs.read("/Temporary/cache{i}.lua")'
                                 for i in range(len(parts)))
                   + ')')
        guest.wait_for(PROMPT, "the cache's probe put together")
        mark = len(guest.seen)
        guest.type("/Temporary/cacheprobe.lua")
        kept = guest.wait_for_line("CACHE\t", "the cache's probe", since=mark)

        if kept.split()[0:3] != ["true", "nil", "true"] or 'true\t"p1"' not in kept:
            raise Failure(
                "a reply kept in /Home/Cache/Browser was not found again from "
                f"the disk, fresh and whole: {kept.strip()!r}. Wrote {args.out}.")

        print(f"cache: kept and found again from the disk ({kept.strip()})",
              flush=True)
        guest.wait_for(PROMPT, "the prompt after the cache's probe")

        guest.type(f"wm browser:10.0.2.2:{port}/{name}")

        found = settle(
            guest,
            lambda w, h, px: find_page(w, h, px),
            "the browser never showed a page: no band of white the width of "
            "its window appeared. A window that opened and rendered nothing "
            "looks exactly like this.",
            seconds=90)

        x0, y0 = found

        #
        # **The header's controls, where the browser says they are** (`roadmap.md`
        # 6zz d1): the kit's widgets, placed by the kit's measures, so the
        # browser says where each one's centre is rather than this file
        # working it out from a font - in the window, from its top left,
        # which is the page's corner less the header.
        #
        placed = re.search(r"browser: header back (\d+),(\d+) forward (\d+),(\d+) "
                           r"reload (\d+),(\d+) field (\d+),(\d+) menu (\d+),(\d+)",
                           guest.seen)

        if not placed:
            raise Failure("the browser did not say where its header's controls are")

        def control(name):
            i = ("back", "forward", "reload", "field", "menu").index(name) * 2 + 1
            return (x0 + int(placed.group(i)), y0 - TOOL + int(placed.group(i + 1)))

        # Typed on the serial line, which reaches the focused window as a
        # keyboard's characters do.
        def typed(text):
            for ch in text:
                guest.proc.stdin.write(ch.encode())
                guest.proc.stdin.flush()
                time.sleep(0.05)

        # Out of the way, so the arrow is not sitting on the page.
        w_, h_, px = parse_ppm(guest.screendump())
        guest.mouse_to((w_ - 30) * 32767 // w_, (h_ - 30) * 32767 // h_)
        time.sleep(1.5)

        w_, h_, px = parse_ppm(guest.screendump())

        band = min(WIN_H - TOOL - STAT, h_ - y0 - 2)
        rows = dark_rows(px, w_, x0 + 4, y0, WIN_W - SBAR - 8, band)
        runs = runs_of_ink(rows)

        os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)

        with open(args.out, "wb") as f:
            f.write(png(w_, h_, px))

        if sum(rows) == 0:
            raise Failure(
                f"the page area at {x0},{y0} is blank: the window opened, the "
                "paper was filled, and not one glyph was drawn on it. "
                f"Wrote {args.out}.")

        if len(runs) < 4:
            raise Failure(
                f"only {len(runs)} line(s) of text on the page. "
                f"Wrote {args.out}.")

        # Text lines, tallest and shortest. A heading and a paragraph in the
        # same face would be within a pixel or two of each other.
        tall, short = max(runs), min(runs)

        if tall - short < 4:
            raise Failure(
                f"every line of text is {short}-{tall} pixels tall, so the "
                "page is being drawn in one face. That is what the direct "
                f"window exists to avoid. Wrote {args.out}.")

        #
        # **A class after a line break** (`web_select.c`, `roadmap.md` 6zz
        # g): the cascade answers from the class list libdom keeps on the
        # element, which split on spaces alone until it was patched to split
        # on any white space, as HTML does. The paragraph whose second class
        # follows a line break is maroon only if both are right.
        #
        # By the colour's shape rather than its value: italic text at 90% is
        # nearly all edge, blended into the white, where #b03060 keeps blue
        # above green by three-eighths of red's lead - which the headings'
        # dark red (blue equal to green) and the links' blue do not.
        def maroonish(c):
            lead = c[0] - c[1]
            return lead > 40 and abs((c[2] - c[1]) - lead * 3 / 8) <= lead / 8

        at = reader(px, w_)
        maroon = sum(1 for y in range(y0, y0 + band)
                     for x in range(x0, x0 + WIN_W - SBAR)
                     if maroonish(at(x, y)))

        if maroon < 100:
            raise Failure(
                f"the paragraph whose class follows a line break is not maroon: "
                f"{maroon} pixels of #b03060's colour on the first screen. Either the "
                "classes were not split on white space, or the cascade does not "
                f"answer from the element's list. Wrote {args.out}.")

        #
        # **A stylesheet the page links to** (`roadmap.md` 6zz j4): fetched
        # before the page is laid out and given to the cascade, where
        # `linked.css`'s one rule puts a paragraph on a green ground -
        # #3fa06a, which nothing else on the page is. Wikipedia keeps every
        # rule it has in sheets like this one.
        #
        green = sum(1 for y in range(y0, y0 + band)
                    for x in range(x0, x0 + WIN_W - SBAR)
                    if at(x, y) == LINKED_GREEN)

        if green < 1000:
            raise Failure(
                f"the paragraph styled by the linked stylesheet has no green "
                f"ground: {green} pixels of #3fa06a on the first screen - the "
                f"sheet was not fetched, or not given to the cascade. Wrote {args.out}.")

        #
        # **An SVG** (`web_svg.c`, `roadmap.md` 6zz j5): `mark.svg`, read by
        # libsvgtiny, drawn by the browser's own rasteriser at its box's
        # size - twice its own - and laid over the page. Its orange disc,
        # #e8761e, is on the screen; its purple curve, #6a2c9e, is to the
        # right of it - not blue, which `find_link` would take for a link and
        # click; and the corner of the disc's square, which the SVG leaves
        # empty, is the paragraph's pale yellow. A picture copied rather
        # than laid over the page would leave that corner black.
        #
        ORANGE, YELLOW = (232, 118, 30), (253, 241, 199)
        disc = [(x, y) for y in range(y0, y0 + band)
                for x in range(x0, x0 + WIN_W - SBAR)
                if at(x, y) == ORANGE]

        if len(disc) < 800:
            raise Failure(
                f"the test page's SVG was not drawn: {len(disc)} pixels of its "
                f"orange disc, #e8761e, on the first screen, where a disc of "
                f"radius 20 is about 1,250. Wrote {args.out}.")

        bx0 = min(x for x, _ in disc)
        by0 = min(y for _, y in disc)
        corner = at(bx0 + 2, by0 + 2)

        if corner != YELLOW:
            raise Failure(
                f"the SVG's empty corner is {corner}, not the paragraph's pale "
                f"yellow {YELLOW}: the picture was put on the page instead of "
                f"over it. Wrote {args.out}.")

        def purple(c):
            return c[2] > c[0] > c[1] + 40 and c[2] - c[1] > 80

        curve = sum(1 for y in range(by0 - 4, by0 + 44)
                    for x in range(bx0 + 46, bx0 + 90)
                    if purple(at(x, y)))

        if curve < 60:
            raise Failure(
                f"the SVG's stroked curve is missing: {curve} purple pixels to "
                f"the right of its disc - a <path> libsvgtiny could not read "
                f"(its compact start, every number's sign its separator, "
                f"is what `sscanf` once refused), or a stroke not drawn. "
                f"Wrote {args.out}.")

        print(f"SVG: {len(disc)} pixels of its disc, its corner the page's "
              f"own, {curve} of its curve", flush=True)

        #
        # **A picture scaled once** (`roadmap.md` 6zz h): the test PNG at a
        # quarter of its size beside the SVG, its magenta square on the
        # first screen - and the first paint's account of itself, which the
        # browser prints, saying it scaled nothing: the picture was scaled
        # to its box when it came, and painting draws it at its own size.
        # It was scaled again every time its band was painted.
        #
        purple_free = sum(1 for y in range(y0, y0 + band)
                          for x in range(x0, x0 + WIN_W - SBAR)
                          if at(x, y)[0] > 235 and at(x, y)[1] < 25
                          and at(x, y)[2] > 235)

        if purple_free < 15:
            raise Failure(
                f"the test picture at a quarter of its size is not drawn: "
                f"{purple_free} pixels of its magenta square on the first "
                f"screen. Wrote {args.out}.")

        painted = re.search(r"browser: painted in [^\r\n]*", guest.seen)
        scaled = painted and re.search(r"scaled [\d.]+ \((\d+)\)",
                                       painted.group(0))

        if not scaled or int(scaled.group(1)) != 0:
            raise Failure(
                "the test page's first paint scaled a picture as it painted: "
                f"{painted.group(0) if painted else 'no account of it'!r}. A "
                "picture is scaled to its box once, when it comes. "
                f"Wrote {args.out}.")

        print(f"pictures: {purple_free} pixels of the quarter-size picture's "
              f"magenta; {painted.group(0)}", flush=True)

        #
        # And that it moves.
        #
        # Six presses rather than one: a line is forty pixels and the check
        # below compares whole rows, so a single line's worth of movement on
        # a page of evenly spaced paragraphs can leave a row looking much as
        # it did. Six is a quarter of a screen and cannot.
        #
        before = rows

        for _ in range(6):
            guest.sendkey("down")

        time.sleep(1.0)
        w2, h2, px2 = parse_ppm(guest.screendump())
        after = dark_rows(px2, w2, x0 + 4, y0, WIN_W - SBAR - 8, band)

        moved = sum(1 for a, b in zip(before, after) if abs(a - b) > 2)

        if moved < band // 8:
            raise Failure(
                f"the page did not scroll: only {moved} of {band} rows "
                "changed after six presses of Down. The surface is laid out "
                "once and scrolled by blitting a band out of it, so this is "
                f"the blit or the key. Wrote {args.out}.")

        #
        # And that the chrome is wired to something.
        #
        # Reload, third in the header, where the browser said it is.
        #
        before_asked = len(asked)

        cx, cy = control("reload")
        tx, ty = _to_tablet(cx, cy, w2, h2)

        guest.mouse_to(tx, ty)
        time.sleep(0.4)
        guest.mouse_button(True)
        time.sleep(0.2)
        guest.mouse_button(False)
        time.sleep(3.0)

        if len(asked) <= before_asked:
            raise Failure(
                f"clicking Reload at {cx},{cy} asked for nothing: the server "
                f"was asked {len(asked)} time(s) in all. Every control in a "
                "direct window's chrome is a rectangle and a comparison, and "
                f"this is the only check on that arithmetic. Wrote {args.out}.")

        #
        # And that a link is a link.
        #
        # The picture is taken again first: Reload put the page back to the
        # top, so where the blue was before the scroll is not where it is
        # now.
        #
        w3, h3, px3 = parse_ppm(guest.screendump())
        spot = find_link(px3, w3, x0 + 4, y0, WIN_W - SBAR - 8, band)

        if spot is None:
            raise Failure(
                "no link-blue text on the page, so either the anchor was not "
                "recognised or its run did not carry the link. The page has "
                f"two. Wrote {args.out}.")

        before_asked = len(asked)
        tx, ty = _to_tablet(spot[0], spot[1], w3, h3)

        guest.mouse_to(tx, ty)
        time.sleep(0.4)
        guest.mouse_button(True)
        time.sleep(0.2)
        guest.mouse_button(False)
        time.sleep(4.0)

        followed = [p for p in asked[before_asked:] if LINKED in p]

        #
        # The page it arrived at, saved beside the one it came from. The
        # server being asked proves the click was routed; only a picture
        # proves what came back was laid out.
        #
        w4, h4, px4 = parse_ppm(guest.screendump())
        second = args.out.replace(".png", "-linked.png")

        with open(second, "wb") as f:
            f.write(png(w4, h4, px4))

        if not followed:
            raise Failure(
                f"clicking the link at {spot} went nowhere: the server was "
                f"asked for {asked[before_asked:]!r} after it. Wrote "
                f"{args.out}.")

        #
        # And that Home needs nothing outside the image.
        #
        # The server must be asked for *nothing* and a page must still be on
        # screen. A browser that could only show remote pages could not be
        # tried without starting a server first, which an operating system
        # has no business asking of the computer running it. Home is in the
        # menu now, as the drawing has it, and the page it opens is
        # `about:start` - typed, which is the same visit.
        #
        before_home = len(asked)
        typed("\x0c")
        time.sleep(0.4)
        typed("about:start\n")
        time.sleep(2.5)

        if len(asked) != before_home:
            raise Failure(
                "clicking Home asked the server for "
                f"{asked[before_home:]!r}. The start page is compiled into "
                f"browser.lua and must need nothing. Wrote {args.out}.")

        w5, h5, px5 = parse_ppm(guest.screendump())
        home_rows = dark_rows(px5, w5, x0 + 4, y0, WIN_W - SBAR - 8, band)

        if sum(home_rows) == 0:
            raise Failure(
                "Home rendered nothing: the page inside the image is blank, "
                f"which is the one page that cannot blame the network. "
                f"Wrote {args.out}.")

        #
        # **An address typed into the bar** (`roadmap.md` 6zz b): Control-L,
        # then the whole URL, `http://` and all - which the bar once took
        # for a machine called "http:" - and Return. The server must be
        # asked for the second page, and its pale yellow must be what the
        # page area shows. Typed on the serial line, which reaches the
        # focused window as a keyboard's characters do.
        #

        def second_page(px_, w_):
            """How many of the page's pixels are the second page's heading's blue."""
            at = reader(px_, w_)
            n = 0

            for y in range(y0, y0 + 90, 2):
                for x in range(x0, x0 + WIN_W - SBAR, 2):
                    if at(x, y) == SECOND_BLUE:
                        n += 1

            return n

        before_typed = len(asked)
        typed("\x0c")
        time.sleep(0.4)
        typed("http://10.0.2.2:%d/%s\n" % (port, LINKED))

        try:
            settle(
                guest,
                lambda w_, h_, px_: True if second_page(px_, w_) >= 10 else None,
                "an address typed into the bar did not bring the second page: its "
                "blue heading never appeared. The server was asked for "
                f"{asked[before_typed:]!r}.",
                seconds=30)
        except Failure:
            wt, ht, pxt = parse_ppm(guest.screendump())

            with open(args.out.replace(".png", "-typed.png"), "wb") as f:
                f.write(png(wt, ht, pxt))

            raise

        if not any(LINKED in p for p in asked[before_typed:]):
            raise Failure(f"the typed address asked for {asked[before_typed:]!r}, "
                          f"not {LINKED}. Wrote {args.out}.")

        #
        # **Back**, the first button in the row: to the start page Home
        # opened, which is compiled in - so the yellow goes and nothing is
        # asked of the server.
        #
        before_back = len(asked)
        guest.mouse_to(*_to_tablet(*control("back"), w5, h5))
        time.sleep(0.4)
        guest.mouse_button(True)
        time.sleep(0.2)
        guest.mouse_button(False)

        settle(
            guest,
            lambda w_, h_, px_: True if second_page(px_, w_) == 0 else None,
            "Back did not leave the second page: its blue heading stayed.",
            seconds=20)

        if len(asked) != before_back:
            raise Failure(f"Back to the start page asked the server for "
                          f"{asked[before_back:]!r}. Wrote {args.out}.")

        #
        # **The pictures drawn**: the test page again, and pages down until
        # the PNG's magenta square is on the screen, and the JPEG's cyan one.
        # Both are fetched as files of their own, so the server must be
        # asked for each as well.
        #
        typed("\x0c")
        time.sleep(0.4)
        typed("http://10.0.2.2:%d/%s\n" % (port, name))
        time.sleep(3.0)
        seen_colours = set()

        for _ in range(14):
            wi, hi, pxi = parse_ppm(guest.screendump())
            at = reader(pxi, wi)

            for y in range(y0, min(hi, y0 + WIN_H - TOOL - STAT), 3):
                for x in range(x0, min(wi, x0 + WIN_W - SBAR), 3):
                    c = at(x, y)

                    if c[0] > 235 and c[1] < 25 and c[2] > 235:
                        seen_colours.add("magenta")
                    elif c[0] < 25 and c[1] > 235 and c[2] > 235:
                        seen_colours.add("cyan")

            if seen_colours == {"magenta", "cyan"}:
                break

            guest.sendkey("spc")
            time.sleep(0.8)

        with open(args.out.replace(".png", "-images.png"), "wb") as f:
            f.write(png(wi, hi, pxi))

        pictures = [p for p in asked if p.endswith((".png", ".jpg"))]

        if PICTURES["peak"] < 2:
            raise Failure(
                "the test page's pictures were fetched one after another: at "
                f"most {PICTURES['peak']} was asked for at once, of "
                f"{pictures!r}. They are fetched together (`http.get_many`).")

        if seen_colours != {"magenta", "cyan"}:
            raise Failure(
                "the test page's pictures were not drawn: of the PNG's magenta "
                f"and the JPEG's cyan the screen showed {sorted(seen_colours) or 'neither'}; "
                f"the server was asked for {pictures!r}. Wrote "
                f"{args.out.replace('.png', '-images.png')}.")

        #
        # **Forms that work** (`roadmap.md` 6zz j6) - Diego, 1 October:
        # "google renders nicely but text entry does not work". The test
        # page's first form is a field and a button, found on the screen by
        # the field's border, #c06000, which nothing else on the page has: a
        # click in it puts a caret there, typing puts the letters in it, and
        # Return sends the form - the server asked for `found.html` with what
        # was typed in its query. The second, sent by its button, #5f6f1f, is
        # a POST, and the server is given its fields as its body.
        #
        FIELD, POST_IT = (192, 96, 0), (95, 111, 31)

        def box_of(colour):
            wb, hb, pxb = parse_ppm(guest.screendump())
            at_ = reader(pxb, wb)
            spots = [(x, y) for y in range(y0, y0 + band)
                     for x in range(x0, x0 + WIN_W - SBAR)
                     if at_(x, y) == colour]

            if len(spots) < 40:
                return None

            return (min(x for x, _ in spots), min(y for _, y in spots),
                    max(x for x, _ in spots), max(y for _, y in spots))

        def ink_in(box, colour=None):
            wb, hb, pxb = parse_ppm(guest.screendump())
            at_ = reader(pxb, wb)
            n = 0

            for y in range(box[1] + 3, box[3] - 2):
                for x in range(box[0] + 3, box[2] - 2):
                    c = at_(x, y)

                    if (c == colour) if colour else sum(c) < 300:
                        n += 1

            return n

        # And the pointer out of the way after, since it is drawn black and
        # would be counted as the caret.
        def press(x, y):
            guest.mouse_to(*_to_tablet(x, y, w4, h4))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.2)
            guest.mouse_button(False)
            time.sleep(0.4)
            guest.mouse_to(*_to_tablet(w4 - 30, h4 - 30, w4, h4))
            time.sleep(1.0)

        def asked_for(test, seconds=20):
            end = time.monotonic() + seconds

            while time.monotonic() < end:
                for p in asked:
                    if test(p):
                        return p

                time.sleep(0.3)

            return None

        guest.sendkey("g")
        time.sleep(1.5)
        field = box_of(FIELD)

        if field is None:
            raise Failure(
                "the test page's search field is not on its first screen: no "
                f"border of #c06000. Wrote {args.out}.")

        empty = ink_in(field)
        press(field[0] + 8, (field[1] + field[3]) // 2)
        caret = ink_in(field, (0, 0, 0))

        if caret < 12:
            raise Failure(
                f"a click in the search field drew no caret: {caret} black "
                f"pixels in it. Wrote {args.out}.")

        typed("kosmos rocks")
        time.sleep(1.5)
        letters = ink_in(field)
        wf, hf, pxf = parse_ppm(guest.screendump())

        with open(args.out.replace(".png", "-form.png"), "wb") as f:
            f.write(png(wf, hf, pxf))

        if letters < empty + 60:
            raise Failure(
                "typing into the search field put nothing in it: "
                f"{letters} dark pixels against {empty} before. Wrote {args.out}.")

        typed("\n")
        found = asked_for(lambda p: p.startswith("/found.html?"))

        if found != "/found.html?q=kosmos+rocks":
            raise Failure(
                "Return in the search field did not send the form as typed: "
                f"the server was asked for {found!r}, not "
                f"'/found.html?q=kosmos+rocks'. Wrote {args.out}.")

        print(f"forms: a caret of {caret} pixels, {letters - empty} pixels "
              f"of letters typed, sent as {found}", flush=True)

        typed("[")
        time.sleep(3.0)
        post_it = box_of(POST_IT)

        if post_it is None:
            raise Failure(
                "Back from the form's answer did not bring the test page back: "
                f"no Post it button, #5f6f1f. Wrote {args.out}.")

        press((post_it[0] + post_it[2]) // 2, (post_it[1] + post_it[3]) // 2)
        posted = asked_for(lambda p: p.startswith("POST "))

        if posted != "POST /posted.html note=from+Kosmos&tick=yes":
            raise Failure(
                "the Post it button did not POST its form: the server was "
                f"given {posted!r}, not 'POST /posted.html "
                f"note=from+Kosmos&tick=yes'. Wrote {args.out}.")

        print(f"forms: {posted}", flush=True)

        #
        # **A page's charset as its server says it** (`roadmap.md` 6zz j7):
        # ISO-8859-1 by the header, UTF-8 by its <meta> - the bytes are
        # the header's, so its title is "Búsqueda" and not "B?squeda", and
        # its form sends "ñandú" in ISO-8859-1, %F1and%FA, as a server
        # that serves that page expects it.
        #
        mark = len(guest.seen)
        typed("\x0c")
        time.sleep(0.4)
        typed("http://10.0.2.2:%d/latin1.html\n" % port)
        shown = guest.wait_for_line("browser: showing http://10.0.2.2:%d/latin1.html"
                                    % port, "the ISO-8859-1 page", since=mark)

        if '"B\u00fasqueda"' not in shown:
            raise Failure(
                "a page the server said is ISO-8859-1 was not read in it: "
                f"{shown!r}, where its title is \"B\u00fasqueda\". Wrote {args.out}.")

        time.sleep(1.5)
        ask = box_of(ASK)

        if ask is None:
            raise Failure(f"the ISO-8859-1 page's button, #8a5a00, is not on "
                          f"the screen. Wrote {args.out}.")

        before_ask = len(asked)
        press((ask[0] + ask[2]) // 2, (ask[1] + ask[3]) // 2)
        sent = asked_for(lambda p: p.startswith("/found.html?") and
                         p not in asked[:before_ask])

        if sent != "/found.html?q=%F1and%FA":
            raise Failure(
                "the ISO-8859-1 page's form was not sent in ISO-8859-1: the "
                f"server was asked for {sent!r}, not '/found.html?q=%F1and%FA'. "
                f"Wrote {args.out}.")

        print(f"charset: the title read as B\u00fasqueda, the form sent as {sent}",
              flush=True)

        #
        # **The cache, in the browser** (`roadmap.md` 6zz k): a page sent
        # with its ETag and `no-cache`, its picture with ten minutes. Gone
        # to, left, and gone to again: the second time the server is asked
        # about the page with its ETag and answers 304 - the page is the one
        # kept, and the status line says so - and the picture is not asked
        # for at all.
        #
        def go_to(path, what):
            mark_ = len(guest.seen)
            typed("\x0c")
            time.sleep(0.4)
            typed("http://10.0.2.2:%d/%s\n" % (port, path))
            return guest.wait_for_line("browser: showing http://10.0.2.2:%d/%s"
                                       % (port, path), what, since=mark_)

        go_to("checked.html", "the page the cache keeps")
        go_to(LINKED, "the second page, between")
        again = go_to("checked.html", "the kept page, again")
        pictures = asked.count("/cached.png")

        if '/checked.html If-None-Match "v1"' not in asked:
            raise Failure(
                "going back to a page kept with its ETag did not ask about it "
                f"with it: the server was asked {asked[-6:]!r}. Wrote {args.out}.")

        if "from the cache, checked" not in again or "Cut short" in again:
            raise Failure(
                f"the page the server said was unchanged was not shown from "
                f"the cache, whole: {again.strip()!r}. Wrote {args.out}.")

        if pictures != 1:
            raise Failure(
                f"a picture that may be used for ten minutes was fetched "
                f"{pictures} times. Wrote {args.out}.")

        print("cache: asked about the page by its ETag, answered 304 and shown "
              "from the cache; its picture fetched once", flush=True)

        #
        # **HTTPS** (`roadmap.md` 6zz c). The second page from the server
        # whose certificate the guest's authority signed: drawn, and said to
        # be Secure. Then from the one another authority signed: refused, on
        # a page saying why - and its Open anyway, the last link on it,
        # clicked: drawn, and said to be Not secure.
        #
        def showing(url, what):
            mark = len(guest.seen)
            typed("\x0c")
            time.sleep(0.4)
            typed(url + "\n")
            return guest.wait_for_line("browser: showing " + url, what, since=mark)

        good = "https://10.0.2.2:%d/%s" % (good_port, LINKED)
        line = showing(good, "the page over TLS")

        if not line.rstrip().endswith(", Secure"):
            raise Failure(f"a page over TLS, its certificate signed by an authority "
                          f"in /Home/Preferences/Authorities, was not Secure: {line!r}")

        settle(guest,
               lambda w_, h_, px_: True if second_page(px_, w_) >= 10 else None,
               "the second page over TLS was said to be shown and was not drawn.",
               seconds=20)

        #
        # **The field says how the page came, first** (`roadmap.md` 6zz d1,
        # `docs/browser.html`): Secure, with a lock, in the look's `good` -
        # green in every look this image carries - before the address.
        # Counted by the colour's shape, since the word is drawn smooth:
        # green well above red and blue, in the header, left of the host.
        #
        def badge_green():
            wb, hb, pxb = parse_ppm(guest.screendump())
            at_ = reader(pxb, wb)
            n = 0

            for y in range(y0 - TOOL + 6, y0 - 6):
                for x in range(x0 + 100, x0 + 330):
                    r, g, b = at_(x, y)

                    if g > r + 40 and g > b + 30:
                        n += 1

            return n

        green = badge_green()

        if green < 30:
            raise Failure(
                f"the field did not say the page over TLS was Secure: {green} "
                f"pixels of its green in the header. Wrote {args.out}.")

        print(f"header: Secure said in the field, {green} pixels of its green",
              flush=True)

        other = "https://10.0.2.2:%d/%s" % (other_port, LINKED)
        refused_before = len(tls_asked)
        line = showing(other, "the refusal")
        refusal = "Refused: the certificate is signed by nobody this machine trusts"

        if not line.rstrip().endswith(refusal):
            raise Failure(f"a certificate from an authority the guest does not "
                          f"trust was not refused for it: {line!r}")

        if len(tls_asked) != refused_before:
            raise Failure(f"the refused server was sent a request anyway: "
                          f"{tls_asked[refused_before:]!r}")

        #
        # **The page on the screen, not only said to be.** The log line comes
        # when the browser has drawn and committed; the window manager puts
        # it up a moment after, and a screendump taken at once can be the
        # page before - which is what happened on 30 September, the harness
        # clicking the second page's one link. So: the screen until two looks
        # agree and it shows two links, Go back above Open anyway.
        #
        def refusal_shown(w_, h_, px_):
            band = min(WIN_H - TOOL - STAT, h_ - y0 - 2)
            first = find_link(px_, w_, x0 + 4, y0, WIN_W - SBAR - 8, band)
            final = find_link(px_, w_, x0 + 4, y0, WIN_W - SBAR - 8, band, last=True)

            if first is None or final is None or final[1] - first[1] < 12:
                return None

            return w_, h_, px_

        previous = [None]

        def held(w_, h_, px_):
            same = previous[0] == px_
            previous[0] = px_
            return refusal_shown(w_, h_, px_) if same else None

        wr, hr, pxr = settle(guest, held, "the refusal page was said to be shown "
                             "and was not put up with its two links", seconds=30)

        with open(args.out.replace(".png", "-refused.png"), "wb") as f:
            f.write(png(wr, hr, pxr))

        anyway = find_link(pxr, wr, x0 + 4, y0, WIN_W - SBAR - 8,
                           min(WIN_H - TOOL - STAT, hr - y0 - 2), last=True)

        if anyway is None:
            raise Failure("the refusal page has no link to click: no Open anyway. "
                          f"Wrote {args.out.replace('.png', '-refused.png')}.")

        mark = len(guest.seen)
        guest.mouse_to(*_to_tablet(anyway[0], anyway[1], wr, hr))
        time.sleep(0.4)
        guest.mouse_button(True)
        time.sleep(0.2)
        guest.mouse_button(False)

        line = guest.wait_for_line("browser: showing " + other, "Open anyway",
                                   since=mark)

        if not line.rstrip().endswith("Not secure: the certificate is signed by "
                                      "nobody this machine trusts"):
            raise Failure(f"Open anyway did not show the page as Not secure: {line!r}")

        settle(guest,
               lambda w_, h_, px_: True if second_page(px_, w_) >= 10 else None,
               "Open anyway was said to show the page and it was not drawn.",
               seconds=20)

        #
        # **A page the size the web's are** (`roadmap.md` 6zz j): Wikipedia's
        # Dam article, 1.4 MB as it was served, from this Mac. Three things
        # it took to show it at all, all held here: the stack saying its
        # window again once the ring is emptied - it arrived at 4 KB a second
        # and cut short without it - the parser started again when the page
        # names its encoding part way, and nothing passed on shorter than the
        # server said. Shown whole, 40,000 pixels and more, in a minute.
        #
        dam = "10.0.2.2:%d/dam.html" % port
        began = time.monotonic()
        line = showing(dam, "Wikipedia's Dam article")
        took = time.monotonic() - began
        whole = re.search(r'"Dam - Wikipedia", (\d+) pixels tall', line)

        if whole is None or int(whole.group(1)) < 40000 or "Cut short" in line:
            raise Failure(f"Wikipedia's Dam article was not shown whole: {line!r}")

        if took > 60:
            raise Failure(f"Wikipedia's Dam article, 1.4 MB from this Mac, took "
                          f"{took:.0f} s to show - a window not said again is "
                          "about five minutes")

        #
        # **The end of a page longer than any band** (`roadmap.md` 6zz j).
        # The page was painted whole into one surface of at most sixteen
        # megabytes until 30 September - about eight screens - and past that
        # it was cut off: the Dam article's last forty-five thousand pixels
        # were never drawn. Now it is laid out whole and painted a band of
        # three screens at a time where it is read, and a band's pictures are
        # fetched when it is first painted and kept.
        #
        # So: the picture at the end is not fetched before the page is shown;
        # `G`, and its magenta is on the screen; `g` and `G` again, and it is
        # again - without the server being asked for it a second time.
        #
        long_url = "10.0.2.2:%d/long.html" % long_port
        line = showing(long_url, "the long page")
        long_page = re.search(r'"A long page", (\d+) pixels tall, (\d+) pictures, '
                         r'(\d+) missing', line)

        if long_page is None or int(long_page.group(1)) < 40000:
            raise Failure(f"the long page was not laid out whole, past forty "
                          f"thousand pixels: {line!r}")

        if "/kosmos.png" in long_asked or long_page.group(2, 3) != ("0", "0"):
            raise Failure(f"the picture at the end of the long page was fetched "
                          f"before the page was shown, where only the first "
                          f"band's are: {line!r}, the server asked for "
                          f"{long_asked!r}")

        def magenta(w_, h_, px_):
            at = reader(px_, w_)

            for y in range(y0, min(h_, y0 + WIN_H - TOOL - STAT), 3):
                for x in range(x0, min(w_, x0 + WIN_W - SBAR), 3):
                    c = at(x, y)

                    if c[0] > 235 and c[1] < 25 and c[2] > 235:
                        return w_, h_, px_

            return None

        def at_the_end(what):
            typed("G")

            try:
                return settle(guest, magenta, what, seconds=30)
            except Failure:
                w_, h_, px_ = parse_ppm(guest.screendump())

                with open(args.out.replace(".png", "-long.png"), "wb") as f:
                    f.write(png(w_, h_, px_))

                raise Failure(f"{what}. The server was asked for {long_asked!r}; "
                              f"wrote {args.out.replace('.png', '-long.png')}.")

        we, he, pxe = at_the_end("G on the long page did not show its last "
                                 "picture: the end of a page longer than a band "
                                 "was not drawn")

        with open(args.out.replace(".png", "-long.png"), "wb") as f:
            f.write(png(we, he, pxe))

        typed("g")
        settle(guest, lambda w_, h_, px_: None if magenta(w_, h_, px_) else True,
               "g on the long page did not go back to its top", seconds=20)
        at_the_end("G a second time on the long page did not show its last "
                   "picture again")

        if long_asked.count("/kosmos.png") != 1:
            raise Failure(f"the long page's last picture was not kept once "
                          f"decoded: the server was asked for {long_asked!r}")

        #
        # **Resized** (`roadmap.md` 6zz e) - Diego, 1 October: "make sure our
        # browser new design is resizable". The grip, the window's bottom
        # right corner, dragged 300 pixels left and 100 up: the window manager
        # resizes the frame, the kit hands over a region the new size, and
        # the browser lays the page out again - at 568 by 484, the new page
        # width and view, and taller than it was, since its lines now wrap
        # sooner. And on the screen, the page's white is the new width.
        #
        typed("g")
        time.sleep(1.5)
        mark = len(guest.seen)
        gx, gy = x0 + WIN_W - 6, y0 - TOOL + WIN_H - 6
        guest.mouse_to(*_to_tablet(gx, gy, w4, h4))
        time.sleep(0.4)
        guest.mouse_button(True)
        time.sleep(0.3)

        for step in range(1, 6):
            guest.mouse_to(*_to_tablet(gx - 60 * step, gy - 20 * step, w4, h4))
            time.sleep(0.15)

        guest.mouse_button(False)
        time.sleep(0.4)
        guest.mouse_to(*_to_tablet(w4 - 30, h4 - 30, w4, h4))
        again = guest.wait_for_line("browser: laid out again at ",
                                    "the page laid out at the new size",
                                    since=mark)
        size = re.match(r"(\d+)x(\d+), (\d+) pixels tall, drawn at (\d+)x(\d+)",
                        again)

        if not size or (int(size.group(1)), int(size.group(2))) != (568, 468):
            raise Failure(
                f"the grip dragged 300 left and 100 up did not lay the page "
                f"out at 568x468: {again.strip()!r}. Wrote {args.out}.")

        if (int(size.group(4)), int(size.group(5))) != (600, 540):
            raise Failure(
                f"the browser is not drawing into a surface the window's new "
                f"size, 600x540: {again.strip()!r} - the kit did not hand over "
                f"a new region. Wrote {args.out}.")

        if int(size.group(3)) <= int(long_page.group(1)):
            raise Failure(
                f"the page laid out narrower is not taller: {size.group(3)} "
                f"pixels against {long_page.group(1)} before - its lines did "
                f"not wrap again. Wrote {args.out}.")

        time.sleep(2.0)
        wr, hr, pxr = parse_ppm(guest.screendump())

        with open(args.out.replace(".png", "-resized.png"), "wb") as f:
            f.write(png(wr, hr, pxr))

        # The window's new right edge, row by row down the page: just inside
        # it is white - the scrollbar's track is, in this look - and just
        # past it is the frame and the desktop, as is where the window used
        # to reach.
        at_ = reader(pxr, wr)
        edge = x0 + 600
        whole = 0

        def white_at(x, y):
            r, g, b = at_(x, y)
            return r > 245 and g > 245 and b > 245

        for row in range(y0 + 20, y0 + 220, 10):
            if (white_at(edge - 4, row) and not white_at(edge + 4, row)
                    and not white_at(x0 + 750, row)):
                whole += 1

        if whole < 18:
            raise Failure(
                f"after the resize the window's right edge is not at {edge}: "
                f"{whole} of 20 rows showed it there - the window shows the old "
                f"size, or nothing. Wrote {args.out.replace('.png', '-resized.png')}.")

        # And what is in it: the page's lines end where lines wrapped at 568
        # do - within a word of it, past 516, where the old page squashed
        # into the new frame ends at about 490 (its paragraphs were one line
        # of about 730, at two thirds) - and inside 578, so it is not a wider
        # page cropped.
        reach = 0

        for row in range(y0 + 80, y0 + 400, 2):
            for x in range(x0 + 300, min(wr, x0 + 600)):
                r, g, b = at_(x, row)

                if r + g + b < 300:
                    reach = max(reach, x - x0)

        if not PAD + 508 < reach <= PAD + 568 + 2:
            raise Failure(
                f"after the resize the page's lines reach {reach} pixels in, "
                f"not between {PAD + 508} and {PAD + 570}: it is the old picture "
                f"squashed, or a page laid out wider and cropped. Wrote "
                f"{args.out.replace('.png', '-resized.png')}.")

        print(f"resized: laid out again at {size.group(1)}x{size.group(2)}, "
              f"{size.group(3)} pixels tall from {long_page.group(1)}; its right "
              f"edge where the new width puts it on {whole} of 20 rows, its "
              f"lines reaching {reach} pixels in", flush=True)

        print(f"wrote {args.out} and {second} ({w_}x{h_})")
        print(f"PASS: a page rendered - {len(runs)} lines of text, "
              f"{short} to {tall} pixels tall, it scrolled "
              f"({moved} rows changed), Reload asked again, a link led to "
              f"{followed[0]}, Home rendered with nothing served, an address "
              f"typed with http:// brought the second page, Back left it, "
              f"the PNG and the JPEG were drawn, a page over TLS was Secure, "
              f"one from an authority it does not trust was refused, "
              f"Open anyway showed it as Not secure, Wikipedia's Dam "
              f"article, 1.4 MB, was shown whole in {took:.0f} s, and the "
              f"end of a page {long_page.group(1)} pixels tall was drawn, its "
              f"picture fetched once.")

    except Failure as why:
        print("\nFAIL: %s" % why, file=sys.stderr)

        # What the screen held when the harness gave up, whichever phase that
        # was: a failure that says "never showed a page" is otherwise a
        # sentence about a picture nobody can see.
        if guest is not None:
            try:
                w_, h_, px_ = parse_ppm(guest.screendump())
                last = os.path.splitext(args.out)[0] + "-failed.png"
                os.makedirs(os.path.dirname(last) or ".", exist_ok=True)

                with open(last, "wb") as f:
                    f.write(png(w_, h_, px_))

                print("the screen then: " + last, file=sys.stderr)
                print(guest.seen[-1500:], file=sys.stderr)
            except Exception as e:              # noqa: BLE001 - a dead guest
                print("and no picture of it: %s" % e, file=sys.stderr)

        return 1
    finally:
        run_screenshot.QEMU_ARGS, run_screenshot.X86_ARGS = saved
        httpd.shutdown()
        https_good.shutdown()
        https_other.shutdown()
        long_httpd.shutdown()

        if guest is not None:
            guest.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
