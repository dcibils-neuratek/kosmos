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
import gzip
import html
import http.server
import os
import re
import select
import shutil
import socket
import ssl
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from run_screenshot import (Guest, Failure, PROMPT, _to_tablet,  # noqa: E402
                            menu_row_middle, parse_ppm, settle)
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
# A page that arrives as the web's do (`roadmap.md` 6zz l1): gzipped, in
# chunks with a pause between them - and with no charset in its header and
# its <meta charset> past the first kilobyte, behind words that do not
# compress to nothing, so the parser fed as it comes has begun on a guess and
# must start again in UTF-8 from what it kept. Read any other way, its title
# is not "Señal ñandú".
def _arrives_page():
    seed, words = 7, []

    for _ in range(600):
        seed = (seed * 1103515245 + 12345) % 2147483648
        words.append("".join(chr(97 + (seed >> s) % 26) for s in (3, 8, 13, 18, 23)))

    return ("<!doctype html><html><head><!-- " + " ".join(words) + " -->"
            "<meta charset=\"utf-8\"><title>Se\u00f1al \u00f1and\u00fa</title></head>"
            "<body><h1>Se\u00f1al</h1>"
            + "".join("<p>Paragraph %d of a page that arrives in pieces.</p>" % i
                      for i in range(400))
            + "</body></html>")


ARRIVES_PAGE = _arrives_page()

# Two selects and a button (`roadmap.md` 6zz j6), each a colour of its own
# to be found by: three machines, and a hundred and twenty options - more
# than any screen's menu holds, so they come grouped into submenus.
SELECT_PAGE = ("<!doctype html><html><head><title>Choose</title><style>"
               "#machine { background: #2b6f9e; color: #ffffff; }"
               "#long { background: #9e2b6f; color: #ffffff; }"
               "#send { background: #6f9e2b; color: #ffffff; }"
               "</style></head><body><form action=\"chosen.html\" method=\"get\">"
               "<p><select id=\"machine\" name=\"machine\"><option>M700</option>"
               "<option>ThinkPad T14</option><option>QEMU</option></select></p>"
               "<p><select id=\"long\" name=\"n\">"
               + "".join("<option>Option %d</option>" % i for i in range(1, 121))
               + "</select></p><p><input id=\"send\" type=\"submit\" value=\"Send\">"
               "</p></form></body></html>")
MACHINE, LONG, SEND = (43, 111, 158), (158, 43, 111), (111, 158, 43)

# One word twice, in serif and in sans (`roadmap.md` 6zz j5), large and in
# a colour each, so how wide each is drawn can be measured off the screen.
FACES_PAGE = ("<!doctype html><html><head><title>Faces</title><style>"
              "#serif { font-family: Georgia, 'Times New Roman', serif; "
              "font-size: 40px; color: #b0201c; }"
              "#sans { font-family: Helvetica, Arial, sans-serif; font-size: 40px; "
              "color: #1c7a20; }"
              "</style></head><body><p id=\"serif\">Hamburgefonstiv</p>"
              "<p id=\"sans\">Hamburgefonstiv</p></body></html>")
SERIF_INK, SANS_INK = (176, 32, 28), (28, 122, 32)

# A page that takes thirty seconds to come (`roadmap.md` 6zz l3), and is
# never meant to be shown: Escape stops it first.
SLOW_PAGE = ("<!doctype html><html><head><title>Slow</title></head><body>"
             + "".join("<p>Line %d of a page that comes slowly.</p>" % i
                       for i in range(2000))
             + "</body></html>")

LATIN1_PAGE = (
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>B\u00fasqueda</title>"
    "<style>#ask { background: #8a5a00; color: #ffffff; }</style></head>"
    "<body><form action=\"found.html\" method=\"get\"><p>"
    "<input type=\"text\" name=\"q\" value=\"\u00f1and\u00fa\"> "
    "<input id=\"ask\" type=\"submit\" value=\"Buscar\"></p></form>"
    "</body></html>")
ASK = (138, 90, 0)

# A page that says to go elsewhere, as DuckDuckGo's front page says to a
# browser that runs no scripts: its body hidden and a refresh at once, both
# in a <noscript> - and one that waits two seconds (`roadmap.md` 6zz, meta
# refresh).
REFRESH_PAGE = ("<!doctype html><html><head><title>Elsewhere</title>"
                "<noscript><meta http-equiv=\"refresh\" "
                "content=\"0; url=&quot;second.html&quot;\"/>"
                "<style>body { display: none }</style></noscript></head>"
                "<body><h1>A page shown only to scripts</h1></body></html>")
LATER_PAGE = ("<!doctype html><html><head><title>Soon elsewhere</title>"
              "<meta http-equiv=\"refresh\" content=\"2; URL=found.html?later=1\">"
              "</head><body><h1>Going in two seconds</h1></body></html>")

# The cache's page: asked about every time, by its ETag, with a picture on it
# that may be used for ten minutes (`roadmap.md` 6zz k).
CHECKED_PAGE = ("<!doctype html><html><head><title>Kept</title></head><body>"
                "<h1>Kept, and asked about</h1>"
                "<p><img src=\"cached.png\" width=\"240\" height=\"135\"></p>"
                "</body></html>")

# Where the page is, inside the window, and the window is opened at a size
# this file and `browser.lua` both know. Content coordinates: the compositor
# adds a title bar above them, which `find_window` finds.
# `TOOL` is everything above the page: the tabs (`roadmap.md` 6zz d2) and
# under them the header - the kit's, `ui.layout.head` (6zz d1) - and the
# status line is the drawing's 26.
TABS, HEAD = 40, 46
TOOL, STAT, SBAR, PAD = TABS + HEAD, 26, 16, 8
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

# What the browser last said it is, to any of these servers (`roadmap.md`
# 6zz, the user agent in Settings).
AGENT_SEEN = {"last": None}


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
            self.answer_page(page)

        # A page of this file's own, whole.
        def answer_page(self, page):
            if isinstance(page, str):
                page = page.encode()

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
            AGENT_SEEN["last"] = self.headers.get("User-Agent")

            # The test page's first form, sent: what it asked for, said back.
            if self.path.startswith("/found.html?"):
                self.answer("asked for " + self.path)
                return

            if self.path.startswith("/chosen.html?"):
                self.answer("chose " + self.path)
                return

            if self.path == "/select.html":
                self.answer_page(SELECT_PAGE)
                return

            if self.path == "/faces.html":
                self.answer_page(FACES_PAGE)
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

            if self.path in ("/refresh.html", "/later.html"):
                self.answer_page(REFRESH_PAGE if self.path == "/refresh.html"
                                 else LATER_PAGE)
                return

            # Thirty seconds in coming, a piece every quarter of one (6zz l3),
            # so nothing the check does in between can outlast it - and
            # whether the browser let the connection go before the end.
            if self.path == "/slow.html":
                page = SLOW_PAGE.encode()
                self.close_connection = True

                try:
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html; charset=utf-8")
                    self.send_header("Content-Length", str(len(page)))
                    self.end_headers()
                    self.wfile.flush()
                    step = (len(page) + 119) // 120

                    for at in range(0, len(page), step):
                        time.sleep(0.25)

                        # The browser's end closed - its FIN, read as the
                        # end of what it sends - is it letting the page go.
                        readable, _, _ = select.select([self.connection], [], [], 0)

                        if readable and not self.connection.recv(1, socket.MSG_PEEK):
                            asked.append("/slow.html let go")
                            return

                        self.wfile.write(page[at:at + step])
                        self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    asked.append("/slow.html let go")

                return

            # Gzipped, chunked, a pause between the pieces (6zz l1): written
            # by hand, since this server speaks HTTP/1.0 and a chunk is 1.1's.
            if self.path == "/arrives.html":
                body = gzip.compress(ARRIVES_PAGE.encode("utf-8"))
                self.close_connection = True
                self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                                 b"Content-Encoding: gzip\r\n"
                                 b"Transfer-Encoding: chunked\r\n"
                                 b"Cache-Control: no-cache\r\nConnection: close\r\n\r\n")

                for at in range(0, len(body), 384):
                    piece = body[at:at + 384]
                    self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
                    self.wfile.flush()
                    time.sleep(0.15)

                self.wfile.write(b"0\r\n\r\n")
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
    #
    # **In two halves, side by side** (`tools/gate.py`): 1 is the page - drawn,
    # scrolled, followed, its forms, its charset, the cache, TLS, the Dam
    # article, the long page, the resize - and 2 the window around it - tabs,
    # favorites, history, Settings, the title bar in Plex. Each boots its own
    # machine; together, or with no `--part`, they are the whole of it. Split
    # on 1 October, when the one suite had grown to 198 seconds and the gate
    # to 9:39 of its ten minutes.
    #
    ap.add_argument("--part", type=int, choices=(0, 1, 2), default=0)
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

        if args.part != 2:
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

        #
        # **What both parts use** (`--part`): the screen's size, pressing
        # a point on it, an address typed and waited for, and the long
        # page's last picture looked for - and the pages the later half
        # reads, the Dam article and the long page.
        #
        w4, h4, _ = parse_ppm(guest.screendump())
        dam = "10.0.2.2:%d/dam.html" % port
        long_url = "10.0.2.2:%d/long.html" % long_port

        def press(x, y):
            guest.mouse_to(*_to_tablet(x, y, w4, h4))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.2)
            guest.mouse_button(False)
            time.sleep(0.4)
            guest.mouse_to(*_to_tablet(w4 - 30, h4 - 30, w4, h4))
            time.sleep(1.0)

        def go_to(path, what):
            mark_ = len(guest.seen)
            typed("\x0c")
            time.sleep(0.4)
            typed("http://10.0.2.2:%d/%s\n" % (port, path))
            return guest.wait_for_line("browser: showing http://10.0.2.2:%d/%s"
                                       % (port, path), what, since=mark_)

        def showing(url, what):
            mark = len(guest.seen)
            typed("\x0c")
            time.sleep(0.4)
            typed(url + "\n")
            return guest.wait_for_line("browser: showing " + url, what, since=mark)

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


        if args.part != 2:
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

            mark = len(guest.seen)
            typed("\n")
            found = asked_for(lambda p: p.startswith("/found.html?"))

            if found != "/found.html?q=kosmos+rocks":
                raise Failure(
                    "Return in the search field did not send the form as typed: "
                    f"the server was asked for {found!r}, not "
                    f"'/found.html?q=kosmos+rocks'. Wrote {args.out}.")

            print(f"forms: a caret of {caret} pixels, {letters - empty} pixels "
                  f"of letters typed, sent as {found}", flush=True)

            # The answer shown before Back: a load goes on while the window
            # answers (6zz l3), and Back during one is Back from the page on
            # screen, as in every browser - which here is the test page's.
            guest.wait_for_line("browser: showing 10.0.2.2:%d/found.html" % port,
                                "the form's answer", since=mark)
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
            # **A page parsed as it comes** (`roadmap.md` 6zz l1): gzipped and
            # chunked, its pieces a sixth of a second apart, so the parser is
            # handed it a piece at a time while the rest is on its way - in as
            # many pieces as it came in, inflated as a stream - and its
            # <meta charset>, past the first kilobyte, makes it start again in
            # UTF-8 from the bytes it kept. Its title is "Señal ñandú" only if
            # all of that held.
            #
            mark = len(guest.seen)
            typed("\x0c")
            time.sleep(0.4)
            typed("http://10.0.2.2:%d/arrives.html\n" % port)
            shown = guest.wait_for_line("browser: showing http://10.0.2.2:%d/arrives.html"
                                        % port, "the page that arrives in pieces",
                                        since=mark)
            fed = re.search(r"browser: parsed as it came - (\d+) pieces",
                            guest.seen[mark:])

            if '"Se\u00f1al \u00f1and\u00fa"' not in shown:
                raise Failure(
                    "a page that arrived gzipped and in pieces, its <meta charset> "
                    f"past the first kilobyte, was not read in UTF-8: {shown!r}, "
                    "where its title is \"Se\u00f1al \u00f1and\u00fa\". "
                    f"Wrote {args.out}.")

            if fed is None or int(fed.group(1)) < 4:
                raise Failure(
                    "a page that arrived in pieces a sixth of a second apart was "
                    "not parsed as it came: "
                    + (f"{fed.group(0)!r}" if fed else "no 'parsed as it came' said")
                    + f". Wrote {args.out}.")

            print(f"arrives: read in UTF-8 after its <meta>, {fed.group(0)[9:]}",
                  flush=True)

            #
            # **A load the window lives through** (`roadmap.md` 6zz l3): a page
            # thirty seconds in coming, and while it comes the page on screen
            # scrolls at a key - the window answering, not waiting for the
            # network. Escape stops it: the connection let go before the
            # server was done, the page never shown, and the address as it was
            # - so Reload loads the page that was on screen.
            #
            w_, h_, px = parse_ppm(guest.screendump())
            band = min(WIN_H - TOOL - STAT, h_ - y0 - 2)
            still = dark_rows(px, w_, x0 + 4, y0, WIN_W - SBAR - 8, band)
            mark = len(guest.seen)
            slow = "http://10.0.2.2:%d/slow.html" % port
            typed("\x0c")
            time.sleep(0.4)
            typed(slow + "\n")

            if asked_for(lambda p: p == "/slow.html") is None:
                raise Failure("the slow page was never asked for")

            time.sleep(0.5)

            for _ in range(6):
                guest.sendkey("down")

            time.sleep(1.0)
            w2, h2, px2 = parse_ppm(guest.screendump())
            moving = dark_rows(px2, w2, x0 + 4, y0, WIN_W - SBAR - 8, band)
            moved_loading = sum(1 for a, b in zip(still, moving) if abs(a - b) > 2)

            if "browser: showing " + slow in guest.seen[mark:]:
                raise Failure("the slow page came before the window could be "
                              "tried while it was loading")

            if moved_loading < band // 8:
                raise Failure(
                    f"the page on screen did not scroll while another was loading: "
                    f"{moved_loading} rows changed of {band}. Wrote {args.out}.")

            guest.sendkey("esc")
            stopped = guest.wait_for_line("browser: stopped loading ", "Escape stopping "
                                          "the slow page", since=mark)

            if not stopped.startswith(slow) or "Escape" not in stopped:
                raise Failure(f"Escape stopped something else: {stopped!r}")

            if asked_for(lambda p: p == "/slow.html let go") is None:
                raise Failure("Escape did not let the slow page's connection go: "
                              "the server went on sending it")

            if "browser: showing " + slow in guest.seen[mark:]:
                raise Failure("the slow page was shown after Escape stopped it")

            mark = len(guest.seen)
            typed("r")
            again = guest.wait_for_line("browser: showing ", "Reload after Escape",
                                        since=mark)

            if not again.startswith("http://10.0.2.2:%d/arrives.html" % port):
                raise Failure(f"Reload after Escape did not load the page on screen, "
                              f"whose address it should be again: {again!r}")

            print(f"loading: the page on screen scrolled while another came "
                  f"({moved_loading} rows), Escape stopped it and let it go, Reload "
                  f"loaded the one on screen", flush=True)

            #
            # **A select's menu** (`roadmap.md` 6zz j6): pressed, the kit's menu
            # opens under it with its options, and the third pressed is chosen.
            # The long one, a hundred and twenty: its menu is groups that fit
            # the screen, and the last option is reached by pressing the last
            # group - its submenu opens beside it - and then the option in it.
            # Send: the server is asked for both.
            # Where each menu opened is what the browser says, since the window
            # manager moves one that would leave the screen.
            #
            go_to("select.html", "the page of selects")
            time.sleep(1.0)

            def menu_at(since):
                m = re.search(r"browser: a menu at (-?\d+),(-?\d+), (\d+) by (\d+), "
                              r"rows of (\d+)", guest.seen[since:])
                return tuple(int(v) for v in m.groups()) if m else None

            def opened(since, what):
                guest.wait_for_line("browser: a menu at ", what, since=since)
                time.sleep(0.3)
                return menu_at(since)

            def chose(since, what):
                return guest.wait_for_line("browser: chose ", what, since=since)

            machine = box_of(MACHINE)

            if machine is None:
                raise Failure(f"the select of machines, #2b6f9e, is not on the "
                              f"screen. Wrote {args.out}.")

            mark = len(guest.seen)
            press((machine[0] + machine[2]) // 2, (machine[1] + machine[3]) // 2)
            said = guest.wait_for_line("browser: a select's menu, ", "the machines' menu",
                                       since=mark)
            mx, my, mw, mh, row = opened(mark, "the machines' menu opening")

            if not said.startswith("3 options in 3 rows"):
                raise Failure(f"the machines' menu was not its three options: {said!r}")

            mark = len(guest.seen)
            press(mx + 20, my + 2 + 2 * row + row // 2)
            picked = chose(mark, "QEMU chosen from the menu")

            if not picked.startswith('"QEMU", option 3 of 3'):
                raise Failure(f"the third row of the machines' menu chose {picked!r}")

            long_box = box_of(LONG)

            if long_box is None:
                raise Failure(f"the long select, #9e2b6f, is not on the screen. "
                              f"Wrote {args.out}.")

            mark = len(guest.seen)
            press((long_box[0] + long_box[2]) // 2, (long_box[1] + long_box[3]) // 2)
            said = guest.wait_for_line("browser: a select's menu, ", "the long menu",
                                       since=mark)
            grouped = re.match(r"120 options in (\d+) rows, (\d+) to a menu", said)

            if grouped is None or int(grouped.group(1)) < 2:
                raise Failure(f"a hundred and twenty options were not grouped into "
                              f"submenus that fit: {said!r}")

            groups, fit = int(grouped.group(1)), int(grouped.group(2))
            mx, my, mw, mh, row = opened(mark, "the long menu opening")
            last = 120 - (groups - 1) * fit
            mark = len(guest.seen)
            press(mx + 20, my + 2 + (groups - 1) * row + row // 2)
            sx, sy, sw, sh, srow = opened(mark, "the last group's submenu")
            press(sx + 20, sy + 2 + (last - 1) * srow + srow // 2)
            picked = chose(mark, "the hundred and twentieth chosen from its group")

            # NetSurf keeps an option's spaces as no-break spaces, as it draws
            # them; the words are what is compared.
            if not picked.replace("\xa0", " ").startswith('"Option 120", option 120 of 120'):
                raise Failure(f"the last option of the last group chose {picked!r}")

            time.sleep(1.0)
            send = box_of(SEND)

            if send is None:
                raise Failure(f"the Send button, #6f9e2b, is not on the screen. "
                              f"Wrote {args.out}.")

            press((send[0] + send[2]) // 2, (send[1] + send[3]) // 2)
            sent = asked_for(lambda p_: p_.startswith("/chosen.html?"))

            if sent != "/chosen.html?machine=QEMU&n=Option+120":
                raise Failure(f"the form did not send what was chosen: the server "
                              f"was asked for {sent!r}")

            print(f"select: QEMU from three, Option 120 from {groups} groups of up to "
                  f"{fit}, sent as {sent}", flush=True)

            #
            # **A serif face** (`roadmap.md` 6zz j5): what a page asks to be in
            # serif is drawn in IBM Plex Serif, not in the sans it fell back to -
            # one word in each, forty pixels, measured off the screen: the two
            # are not the same width.
            #
            go_to("faces.html", "the page of faces")
            time.sleep(1.0)
            serif_box, sans_box = box_of(SERIF_INK), box_of(SANS_INK)

            if serif_box is None or sans_box is None:
                raise Failure(f"the word in serif, #b0201c, or in sans, #1c7a20, is not "
                              f"on the screen. Wrote {args.out}.")

            serif_w = serif_box[2] - serif_box[0]
            sans_w = sans_box[2] - sans_box[0]

            if abs(serif_w - sans_w) < 6:
                raise Failure(f"the word in serif was drawn as wide as the one in sans, "
                              f"{serif_w} and {sans_w} pixels: the same face. "
                              f"Wrote {args.out}.")

            print(f"faces: the word {serif_w} pixels wide in serif, {sans_w} in sans",
                  flush=True)

            #
            # **The cache, in the browser** (`roadmap.md` 6zz k): a page sent
            # with its ETag and `no-cache`, its picture with ten minutes. Gone
            # to, left, and gone to again: the second time the server is asked
            # about the page with its ETag and answers 304 - the page is the one
            # kept, and the status line says so - and the picture is not asked
            # for at all.
            #

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
            # **A page that says to go elsewhere** (`roadmap.md` 6zz, meta
            # refresh) - Diego, 1 October: "duckduckgo.com is not loading at
            # all". Its front page, to a browser that runs no scripts, hides
            # its body and refreshes to its page without them, both in a
            # `<noscript>`; one shaped like it goes to the second page at
            # once, and one that waits two seconds goes when they are up.
            #
            mark_ = len(guest.seen)
            typed("\x0c")
            time.sleep(0.4)
            typed("http://10.0.2.2:%d/refresh.html\n" % port)
            went = guest.wait_for_line("browser: refreshed to ",
                                       "the page that says to go elsewhere", since=mark_)

            if went.strip() != "10.0.2.2:%d/%s" % (port, LINKED) \
                    and went.strip() != "http://10.0.2.2:%d/%s" % (port, LINKED):
                raise Failure(f"a refresh to the second page went to {went.strip()!r}")

            guest.wait_for_line("browser: showing " + went.strip(),
                                "the second page shown after the refresh", since=mark_)

            mark_ = len(guest.seen)
            began = time.monotonic()
            typed("\x0c")
            time.sleep(0.4)
            typed("http://10.0.2.2:%d/later.html\n" % port)
            guest.wait_for_line("browser: goes to ", "the page that waits two seconds",
                                since=mark_)
            guest.wait_for_line("browser: showing " , "the page that waits, shown",
                                since=mark_)
            later = guest.wait_for_line("browser: refreshed to ",
                                        "the refresh after two seconds", since=mark_)
            waited = time.monotonic() - began

            if "found.html?later=1" not in later or waited < 2:
                raise Failure(f"the page that waits two seconds went to "
                              f"{later.strip()!r} after {waited:.1f} s")

            print(f"refresh: at once to the second page, past a hidden body; and "
                  f"after two seconds ({waited:.1f})", flush=True)

            #
            # **A plain browser, as it says** - Diego, 1 October: "we have a very
            # basic browser so we need to announce that to the server". Lynx's
            # name by default, which sites were measured to send simpler pages,
            # and Kosmos in it.
            #
            said_agent = AGENT_SEEN["last"] or ""

            if not said_agent.startswith("Lynx/2.9.0 (Kosmos "):
                raise Failure(f"the browser did not say it is a plain browser, and "
                              f"Kosmos: {said_agent!r}")

            print(f"agent: {said_agent}", flush=True)

            #
            # **Searching from the address field** (`roadmap.md` 6zz d6): words
            # typed there are a search at DuckDuckGo's page without scripts,
            # the words encoded as a form sends them. Checked by the address
            # the browser says before it asks for it, so the gate does not
            # wait on a server it does not run; an address typed is gone to,
            # as every check here already types one.
            #
            mark_ = len(guest.seen)
            typed("\x0c")
            time.sleep(0.4)
            typed("kosmos & rocks\n")
            sought = guest.wait_for_line('browser: searching for "kosmos & rocks" at ',
                                         "words typed in the field, searched for",
                                         since=mark_)

            if sought.strip() != "https://html.duckduckgo.com/html/?q=kosmos+%26+rocks":
                raise Failure(f"words typed in the address field were searched for "
                              f"at {sought.strip()!r}")

            print(f"search: {sought.strip()}", flush=True)

            #
            # **An address typed while a page arrives** keeps its first
            # characters: keys come to the window in a batch once it is free,
            # and the field took the page's address at the next frame, after
            # the batch - writing over `https://1` and searching for the rest.
            # The long page takes seconds to lay out; the second page's
            # address is typed meanwhile.
            #
            mark_ = len(guest.seen)
            typed("\x0c")
            time.sleep(0.4)
            typed(long_url + "\n")
            time.sleep(0.3)
            typed("\x0c")
            typed("http://10.0.2.2:%d/%s\n" % (port, LINKED))
            guest.wait_for_line("browser: showing http://10.0.2.2:%d/%s" % (port, LINKED),
                                "an address typed while the long page arrived",
                                since=mark_)

            if "browser: searching for" in guest.seen[mark_:]:
                raise Failure("an address typed while a page arrived lost its first "
                              "characters and was searched for")

            #
            # **HTTPS** (`roadmap.md` 6zz c). The second page from the server
            # whose certificate the guest's authority signed: drawn, and said to
            # be Secure. Then from the one another authority signed: refused, on
            # a page saying why - and its Open anyway, the last link on it,
            # clicked: drawn, and said to be Not secure.
            #

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

                for y in range(y0 - HEAD + 6, y0 - 6):
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
            began = time.monotonic()
            mark = len(guest.seen)
            line = showing(dam, "Wikipedia's Dam article")
            took = time.monotonic() - began
            whole = re.search(r'"Dam - Wikipedia", (\d+) pixels tall', line)

            if whole is None or int(whole.group(1)) < 40000 or "Cut short" in line:
                raise Failure(f"Wikipedia's Dam article was not shown whole: {line!r}")

            # And parsed while it came rather than after (6zz l1): 1.4 MB
            # arrives in many reads, and each was handed to the parser.
            fed = re.search(r"browser: parsed as it came - (\d+) pieces",
                            guest.seen[mark:])

            if fed is None or int(fed.group(1)) < 2:
                raise Failure("Wikipedia's Dam article was not parsed as it came: "
                              + (f"{fed.group(0)!r}" if fed else "nothing said of it"))

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
            #
            # **And the rest of its pictures while it is read** (`roadmap.md`
            # 6zz l2): once it is shown, and without a scroll, the picture at
            # its end is fetched - once - and `G` then draws it without the
            # server being asked again: its band took the bytes that came
            # ahead, or waited for them, rather than asking twice. Counted
            # from here: the long page was shown once before, while an
            # address was typed over it, and may have begun the same then.
            #
            mark = len(guest.seen)
            asked_before = long_asked.count("/kosmos.png")
            line = showing(long_url, "the long page")
            long_page = re.search(r'"A long page", (\d+) pixels tall, (\d+) pictures, '
                             r'(\d+) missing', line)

            if long_page is None or int(long_page.group(1)) < 40000:
                raise Failure(f"the long page was not laid out whole, past forty "
                              f"thousand pixels: {line!r}")

            if long_page.group(2, 3) != ("0", "0"):
                raise Failure(f"the picture at the end of the long page was "
                              f"counted in its first band: {line!r}")

            # Read from its own showing on: the page before it - the Dam
            # article, whose pictures this server does not have - may still
            # be fetching its own ahead, and say so after the mark.
            shown_at = guest.seen.index("browser: showing " + long_url, mark)
            ahead = guest.wait_for_line("browser: fetched ", "the long page's other "
                                        "pictures fetched while it was read",
                                        since=shown_at)
            said_ahead = guest.seen[mark:]
            began_ahead = said_ahead.find("browser: fetching 1 pictures ahead")

            if began_ahead < 0 or began_ahead < said_ahead.find("browser: showing " + long_url):
                raise Failure("the long page's other pictures were not fetched after "
                              "it was shown, where only its first band's are before")

            if not ahead.startswith("1 of 1 pictures ahead") \
               or long_asked.count("/kosmos.png") != asked_before + 1:
                raise Failure(f"the picture at the end of the long page was not "
                              f"fetched while it was read: {ahead!r}, the server "
                              f"asked for {long_asked!r}")



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

            if long_asked.count("/kosmos.png") != asked_before + 1:
                raise Failure(f"the long page's last picture was asked for again - "
                              f"when its band was painted, or after it was kept "
                              f"decoded: the server was asked for {long_asked!r}")

            print(f"ahead: fetched {ahead}, its band drawn from it", flush=True)

        #
        # **The later half's own start** (`--part 2`): the Dam article shown,
        # for the history to find, and the long page at its end with its
        # last picture, fetched once - where the earlier half leaves them.
        #
        if args.part == 2:
            showing(dam, "Wikipedia's Dam article")
            line = showing(long_url, "the long page")
            long_page = re.search(r'"A long page", (\d+) pixels tall, (\d+) pictures, '
                                  r'(\d+) missing', line)
            at_the_end("G on the long page did not show its last picture")

        if args.part != 1:
            #
            # **Tabs** (`roadmap.md` 6zz d2, `docs/browser.html`): Super T opens
            # one beside the shown one, on a page offering what was open lately
            # with the address field waiting - an address typed goes to it - and
            # the long page's tab, pressed, comes back where it was read: at its
            # end, its last picture on the screen and not fetched again. Each tab
            # has its own history - Back in the second goes to its new tab's
            # page, not to the first's Dam article - and Super Shift ] and [ go
            # round them. The second closed by its cross, a third opened and
            # closed by Super W, and the long page is shown again each time.
            #
            # The keys are the board's: Super held through QMP, as a person
            # holds it, so the window manager reads them and hands on what it
            # has no binding for.
            #
            def chord(*names):
                for n in names:
                    guest._qmp("input-send-event", {"events": [
                        {"type": "key", "data": {"down": True,
                                                 "key": {"type": "qcode", "data": n}}}]})
                    time.sleep(0.08)

                for n in reversed(names):
                    guest._qmp("input-send-event", {"events": [
                        {"type": "key", "data": {"down": False,
                                                 "key": {"type": "qcode", "data": n}}}]})
                    time.sleep(0.08)

            def tab_said(prefix, what, mark_):
                return guest.wait_for_line("browser: " + prefix, what, since=mark_)

            def no_magenta(w_, h_, px_):
                return None if magenta(w_, h_, px_) else True

            mark = len(guest.seen)
            chord("meta_l", "t")
            tab_said("tab 2 of 2, new", "Super T to open a second tab", mark)
            offered = tab_said("a new tab's page, ", "the new tab's page", mark)
            newest = re.match(r"\d+ favorites, (\d+) lately, the newest (\S+)", offered)

            if not newest or newest.group(2) != long_url or int(newest.group(1)) < 3:
                raise Failure(
                    f"the new tab did not offer what was open lately, the long page "
                    f"newest: {offered.strip()!r}. Wrote {args.out}.")

            laid = re.search(r"browser: tabs 2, each (\d+) wide, the first at (\d+),(\d+)",
                             guest.seen[mark:])

            if not laid:
                raise Failure("the browser did not say where its two tabs are")

            each, fx, fy = (int(v) for v in laid.groups())
            settle(guest, no_magenta, "the new tab still shows the long page", seconds=20)

            # Its address field has the keyboard: the second page, typed.
            typed("http://10.0.2.2:%d/%s" % (port, LINKED))
            time.sleep(1.5)

            # **The caret at the end of what was typed** - Diego, 1 October:
            # "the cursor is off by some characters". The field counted 8-pixel
            # cells and drew in the look's proportional face. Along the field's
            # middle, the rightmost mark is the caret, and the words' last ink
            # is a pixel or two before it - not a character or more.
            fcx, fcy = control("field")
            wf, hf, pxf = parse_ppm(guest.screendump())
            at_ = reader(pxf, wf)
            inked = []

            for x in range(x0 + 120, fcx + (fcx - x0) - 150):
                for y in range(fcy - 7, fcy + 8):
                    r, g, b = at_(x, y)

                    if r + g + b < 620:
                        inked.append(x)
                        break

            caret = max(inked) if inked else 0
            words = max((x for x in inked if x < caret - 1), default=0)

            if not inked or caret - words > 5:
                raise Failure(f"the caret in the address field is {caret - words} "
                              f"pixels past the end of the typed address, at {caret} "
                              f"where the words end at {words}")

            typed("\n")
            tab_said("showing http://10.0.2.2:%d/%s" % (port, LINKED),
                     "an address typed into the new tab's field", mark)
            print(f"caret: {caret - words} pixels after the typed address's last ink",
                  flush=True)

            # The first tab, pressed: the long page at its end again.
            mark = len(guest.seen)
            press(x0 + fx, y0 - TOOL + fy)
            tab_said("tab 1 of 2, shown, showing " + long_url,
                     "a press on the first tab to show it", mark)
            settle(guest, magenta,
                   "the long page's tab, shown again, is not where it was read - "
                   "its last picture is not on the screen", seconds=20)

            if long_asked.count("/kosmos.png") != 1:
                raise Failure(f"showing the long page's tab again fetched its picture "
                              f"again: the server was asked for {long_asked!r}")

            # Round to the second, and Back there is its own.
            mark = len(guest.seen)
            chord("meta_l", "shift", "bracket_right")
            tab_said("tab 2 of 2, shown", "Super Shift ] to the next tab", mark)
            chord("meta_l", "bracket_left")
            tab_said("showing about:newtab",
                     "Super [ in the second tab to go back to its own first page",
                     mark)
            chord("meta_l", "shift", "bracket_left")
            tab_said("tab 1 of 2, shown, showing " + long_url,
                     "Super Shift [ to the tab before", mark)

            # The second closed by its cross, while the first is shown.
            cross_x = x0 + 8 + each + 2 + each - 16
            press(cross_x, y0 - TOOL + 22)
            tab_said("a tab closed, 1 left", "a press on the second tab's cross",
                     mark)

            # And one opened and closed by the keys: the long page again.
            mark = len(guest.seen)
            chord("meta_l", "t")
            tab_said("tab 2 of 2, new", "Super T again", mark)
            settle(guest, no_magenta, "the third tab still shows the long page",
                   seconds=20)
            chord("meta_l", "w")
            tab_said("tab 1 of 1, shown, showing " + long_url,
                     "Super W to close the shown tab and show the one left", mark)
            tab_said("a tab closed, 1 left", "Super W", mark)
            settle(guest, magenta,
                   "the long page is not back where it was read after Super W",
                   seconds=20)

            print(f"tabs: Super T opened one offering the long page lately, an "
                  f"address typed into it; the long page's tab came back at its "
                  f"end with its picture kept; Back in the second was its own; "
                  f"Super Shift ] and [ went round; closed by its cross and by "
                  f"Super W", flush=True)

            #
            # **Favorites, as files** (`roadmap.md` 6zz d3, `docs/browser.html`):
            # the star pressed on the long page keeps it as a file in
            # `/Home/Favorites`, and the star is gold; the bar appears under the
            # header, and its favorite pressed on another page shows the long
            # page; the sidebar, opened by Super Y, has it too and shows it when
            # pressed - the page laid out narrower beside it; a new tab offers
            # it; and Super D makes it not a favorite, the bar gone again.
            #
            wtop = y0 - TOOL
            heads = re.findall(r"browser: header back .*? star (\d+),(\d+) side (\d+),(\d+)",
                               guest.seen)

            if not heads:
                raise Failure("the browser did not say where its star is")

            sx, sy = (int(v) for v in heads[-1][:2])

            def gold():
                wg, hg, pxg = parse_ppm(guest.screendump())
                at_ = reader(pxg, wg)
                n = 0

                for y in range(wtop + sy - 10, wtop + sy + 10):
                    for x in range(x0 + sx - 10, x0 + sx + 10):
                        r, g, b = at_(x, y)

                        if abs(r - 0xd4) < 40 and abs(g - 0x9b) < 40 and b < 90:
                            n += 1

                return n

            unlit = gold()
            mark = len(guest.seen)
            press(x0 + sx, wtop + sy)
            made = tab_said("a favorite, ", "the star to keep the long page", mark)

            if not made.startswith("/Home/Favorites/A long page, of " + long_url):
                raise Failure(f"the star did not keep the long page as a file in "
                              f"/Home/Favorites: {made.strip()!r}")

            bar = tab_said("favorites bar 1 of 1, the first at ",
                           "the favorites bar to show the new favorite", mark)
            bx, by = (int(v) for v in re.match(r"(\d+),(\d+)", bar).groups())
            time.sleep(1.0)
            lit = gold()

            if unlit > 5 or lit < 30:
                raise Failure(f"the star is not gold on a favorite and dim on a page "
                              f"that is not one: {lit} gold pixels lit, {unlit} not")

            # Another page, then the bar's favorite.
            go_to(LINKED, "the second page, to leave the favorite")
            mark = len(guest.seen)
            press(x0 + bx, wtop + by)
            tab_said("showing " + long_url, "the bar's favorite pressed", mark)

            # The sidebar: the page beside it narrower, and its favorite shown.
            go_to(LINKED, "the second page again")
            mark = len(guest.seen)
            chord("meta_l", "y")
            opened = tab_said("the sidebar open, its first row at ",
                              "Super Y to open the sidebar", mark)
            narrower = tab_said("laid out again at ",
                                "the page laid out beside the sidebar", mark)

            if not narrower.startswith("600x"):
                raise Failure(f"the page beside the sidebar was not laid out 600 "
                              f"wide: {narrower.strip()!r}")

            rx, ry = (int(v) for v in re.match(r"(\d+),(\d+)", opened).groups())
            time.sleep(1.0)
            mark = len(guest.seen)
            press(x0 + rx, wtop + ry)
            tab_said("showing " + long_url, "the sidebar's favorite pressed", mark)
            chord("meta_l", "y")
            tab_said("the sidebar closed", "Super Y to close the sidebar", mark)

            # A new tab offers it, and is closed.
            mark = len(guest.seen)
            chord("meta_l", "t")
            offered = tab_said("a new tab's page, ", "a new tab with a favorite", mark)

            if not offered.startswith("1 favorites"):
                raise Failure(f"the new tab's page did not offer the favorite: "
                              f"{offered.strip()!r}")

            chord("meta_l", "w")
            tab_said("tab 1 of 1, shown", "Super W on the new tab", mark)

            #
            # **Dragged on the bar** (`roadmap.md` 6zz d3): on the second page,
            # the star dragged down onto the bar, left of the long page's
            # favorite - the second page kept there, first; the long page's
            # favorite dragged back to first place; then the second page let go
            # with Super D, and the long page shown again from the bar.
            #
            def drag(fx, fy, tx, ty):
                guest.mouse_to(*_to_tablet(fx, fy, w4, h4))
                time.sleep(0.4)
                guest.mouse_button(True)
                time.sleep(0.2)

                for k in range(1, 7):
                    guest.mouse_to(*_to_tablet(fx + (tx - fx) * k // 6,
                                               fy + (ty - fy) * k // 6, w4, h4))
                    time.sleep(0.15)

                time.sleep(0.3)
                guest.mouse_button(False)
                time.sleep(0.4)
                guest.mouse_to(*_to_tablet(w4 - 30, h4 - 30, w4, h4))
                time.sleep(0.6)

            def on_bar(since, what):
                line = tab_said("on the bar, ", what, since).strip()
                return [(m.group(1), int(m.group(2)), int(m.group(3)))
                        for m in re.finditer(r"(.+?) at (\d+),(\d+)(?:; |$)", line)]

            def bar_now():
                lines = re.findall(r"browser: on the bar, ([^\n]*)", guest.seen)
                return [(m.group(1), int(m.group(2)), int(m.group(3)))
                        for m in re.finditer(r"(.+?) at (\d+),(\d+)(?:; |$)",
                                             lines[-1].strip())]

            go_to(LINKED, "the second page, to drag onto the bar")
            time.sleep(1.0)
            (long_name, lx, ly), = bar_now()
            mark = len(guest.seen)
            drag(x0 + sx, wtop + sy, x0 + lx - 15, wtop + ly)
            put = tab_said("/Home/Favorites/", "the star dropped on the bar", mark)
            order = on_bar(mark, "the bar with the dropped page")

            if not put.rstrip().endswith("put at 1 on the bar") or len(order) != 2 \
               or order[1][0] != long_name or "second page" not in order[0][0]:
                raise Failure(f"the star dragged onto the bar did not keep the second "
                              f"page first: {put.strip()!r}, the bar {order!r}")

            mark = len(guest.seen)
            drag(x0 + order[1][1], wtop + order[1][2], x0 + order[0][1] - 15,
                 wtop + order[0][2])
            moved = tab_said("/Home/Favorites/", "the long page's favorite dragged", mark)
            again = on_bar(mark, "the bar after the drag")

            if not moved.rstrip().endswith("put at 1 on the bar") \
               or [n for n, _, _ in again] != [long_name, order[0][0]]:
                raise Failure(f"the long page's favorite dragged to the front did not "
                              f"go there: {moved.strip()!r}, the bar {again!r}")

            mark = len(guest.seen)
            chord("meta_l", "d")
            tab_said("http://10.0.2.2:%d/%s is not a favorite, 1 removed" % (port, LINKED),
                     "Super D on the second page", mark)
            (_, lx, ly), = on_bar(mark, "the bar with the long page alone")
            press(x0 + lx, wtop + ly)
            tab_said("showing " + long_url, "the bar's favorite pressed again", mark)
            print(f"dragged: the second page dropped first on the bar from the star, "
                  f"the long page dragged back before it", flush=True)

            # Super D: not a favorite, and the bar gone with the page back up.
            mark = len(guest.seen)
            chord("meta_l", "d")
            tab_said(long_url + " is not a favorite, 1 removed",
                     "Super D to make the long page not a favorite", mark)
            tab_said("laid out again at ", "the page laid out with no bar", mark)

            # The page's paper where it began before there was a bar - the long
            # page at its top, since the bar and the sidebar opened it afresh.
            settle(guest,
                   lambda w_, h_, px_: True if find_page(w_, h_, px_) == (x0, y0) else None,
                   "the page did not move back up to where it began when the bar "
                   "went", seconds=20)

            #
            # **History, on the disk, by day** (`roadmap.md` 6zz d4): the
            # sidebar's History half lists the pages shown today under Today;
            # "dam" typed into its field leaves the Dam article alone, and its
            # row pressed shows it. Then the long page again, for the resize.
            #
            mark = len(guest.seen)
            chord("meta_l", "y")
            opened = tab_said("the sidebar open, its first row at ",
                              "Super Y to open the sidebar again", mark)
            hx, hy = (int(v) for v in re.search(r"History at (\d+),(\d+)", opened).groups())
            time.sleep(1.0)
            press(x0 + hx, wtop + hy)
            listed = tab_said("history, ", "the sidebar's History half", mark)
            whole = re.match(r'(\d+) pages, searched for "", the first at \d+,\d+, '
                             r'the field at (\d+),(\d+)', listed)

            if not whole or int(whole.group(1)) < 4:
                raise Failure(f"the sidebar's history did not list today's pages: "
                              f"{listed.strip()!r}")

            press(x0 + int(whole.group(2)), wtop + int(whole.group(3)))
            mark = len(guest.seen)
            typed("dam")
            guest.wait_for_line('searched for "dam", ', "the history searched as typed",
                                since=mark)
            searched = re.search(r'history, (\d+) pages, searched for "dam", the first at '
                                 r'(\d+),(\d+)', guest.seen[mark:])

            if not searched or searched.group(1) != "1":
                raise Failure(f"the history searched for \"dam\" did not leave the Dam "
                              f"article alone: {searched and searched.group(0)!r}")

            mark = len(guest.seen)
            press(x0 + int(searched.group(2)), wtop + int(searched.group(3)))
            tab_said("showing " + dam, "the Dam article's row in the history pressed",
                     mark)
            chord("meta_l", "y")
            tab_said("the sidebar closed", "Super Y to close the sidebar again", mark)
            showing(long_url, "the long page again, after the history")

            print(f"history: the sidebar listed {whole.group(1)} pages today, "
                  f"\"dam\" left one, and it showed the Dam article", flush=True)

            print(f"favorites: the star kept the long page as a file, gold "
                  f"({lit} pixels, {unlit} before); the bar showed it and opened "
                  f"it; the sidebar did, the page 600 wide beside it; a new tab "
                  f"offered it; Super D let it go", flush=True)

            #
            # **Settings** (`roadmap.md` 6zz d5): the menu under its button -
            # where the button is, which it was not - and Super , opening the
            # page, its switches and choices written as they are pressed: the
            # costs off the status line, pictures not loaded, the page zoomed to 150%
            # making the second page taller, the browser to open on its tabs,
            # and the history cleared; then put back.
            #
            def menu_opened(mark_, what):
                line = guest.wait_for_line("wm: menu of Browser", what, since=mark_)
                m = re.search(r"at (\d+),(\d+) \d+x\d+", line)
                return int(m.group(1)), int(m.group(2))

            def status_ink():
                ws, hs, pxs = parse_ppm(guest.screendump())
                at_ = reader(pxs, ws)
                n = 0

                for y in range(wtop + WIN_H - STAT + 4, wtop + WIN_H - 4):
                    for x in range(x0 + WIN_W - 260, x0 + WIN_W - 10):
                        r, g, b = at_(x, y)

                        if r + g + b < 400:
                            n += 1

                return n

            ink_on = status_ink()
            menu_cx = int(placed.group(9))
            mark = len(guest.seen)
            press(*control("menu"))
            ax, ay = menu_opened(mark, "the menu to open under its button")

            if abs(ax - (x0 + menu_cx - 13)) > 3 or abs(ay - (wtop + TOOL - 2)) > 3:
                raise Failure(f"the menu opened at {ax},{ay}, not under its button at "
                              f"{x0 + menu_cx - 13},{wtop + TOOL - 2}")

            press(x0 + 20, wtop + WIN_H - 10)          # and closed, by a press elsewhere

            mark = len(guest.seen)
            chord("meta_l", "comma")
            tab_said("showing about:settings", "Super , to open Settings", mark)
            laid = tab_said("settings, ", "Settings to say where its rows are", mark)
            rows = re.match(r"(\d+) columns, (\d+) tall, costs (\d+),(\d+), zoom (\d+),(\d+), "
                            r"opens (\d+),(\d+), clear (\d+),(\d+), empty (\d+),(\d+), "
                            r"images (\d+),(\d+), agent (\d+),(\d+)", laid)

            if not rows or rows.group(1) != "2":
                raise Failure(f"Settings is not two columns in a window 900 wide: "
                              f"{laid.strip()!r}")

            def at_row(i):
                return x0 + int(rows.group(i)), wtop + int(rows.group(i + 1))

            def chosen(i, row, what):
                mark_ = len(guest.seen)
                press(*at_row(i))
                mx_, my_ = menu_opened(mark_, what)
                time.sleep(0.6)
                press(mx_ + 30, menu_row_middle(my_, row))

            time.sleep(1.0)
            mark = len(guest.seen)
            press(*at_row(3))
            tab_said("set costs false", "the costs switched off", mark)
            press(*at_row(13))
            tab_said("set images false", "Load images switched off", mark)
            chosen(5, 3, "zoom's menu")
            tab_said("set zoom 150", "the zoom at 150%", mark)
            chosen(7, 2, "When the browser opens' menu")
            tab_said("set opens tabs", "the browser to open on the tabs it had", mark)
            chosen(9, 1, "Clear history's menu")
            tab_said("history cleared, ", "the history cleared", mark)
            chosen(15, 2, "what it tells sites it is")
            tab_said("set agent netsurf", "the engine's name chosen", mark)

            # The long page at 150%, taller - all of it larger, its lines
            # wrapping sooner; no costs on the status line; and the test page
            # with no pictures asked for.
            before = int(long_page.group(1))
            bigger = showing(long_url, "the long page at 150%")
            after = int(re.search(r"(\d+) pixels tall", bigger).group(1))
            time.sleep(1.0)
            ink_off = status_ink()

            if not (AGENT_SEEN["last"] or "").startswith("NetSurf/3.11 (Kosmos "):
                raise Failure(f"the engine's name chosen in Settings was not what "
                              f"the browser said: {AGENT_SEEN['last']!r}")

            if after < before * 1.2:
                raise Failure(f"the long page at 150% is {after} pixels tall, not "
                              f"taller than {before} at 100% by a fifth")

            if ink_on < 30 or ink_off > 5:
                raise Failure(f"the costs did not leave the status line when switched "
                              f"off: {ink_off} dark pixels where they were, {ink_on} "
                              f"when on")

            plain = go_to(name, "the test page with no pictures")

            if not re.search(r", 0 pictures, 0 missing", plain):
                raise Failure(f"the test page asked for pictures with Load images off: "
                              f"{plain.strip()!r}")

            mark = len(guest.seen)
            chord("meta_l", "t")
            offered = tab_said("a new tab's page, ", "a new tab after the clear", mark)

            if not offered.startswith("0 favorites, 2 lately"):
                raise Failure(f"the history was not cleared - a new tab offers "
                              f"{offered.strip()!r}, where two pages were shown since")

            chord("meta_l", "w")
            chord("meta_l", "w")
            tab_said("tab 1 of 1, shown, showing " + long_url, "back to the long page",
                     mark)

            # Put back: the costs, the pictures and the zoom.
            mark = len(guest.seen)
            chord("meta_l", "comma")
            tab_said("settings, ", "Settings again", mark)
            time.sleep(1.0)
            press(*at_row(3))
            press(*at_row(13))
            chosen(5, 1, "zoom's menu again")
            tab_said("set zoom 100", "the zoom back at 100%", mark)
            chosen(15, 1, "what it tells sites it is, again")
            tab_said("set agent lynx", "a plain browser's name again", mark)
            tab_said("set images true", "Load images back on", mark)
            tab_said("set costs true", "the costs back on", mark)
            chord("meta_l", "w")
            tab_said("tab 1 of 1, shown, showing " + long_url, "the long page again",
                     mark)

            #
            # **Zoom by the keys** - Diego, 1 October: "zoom the page likke
            # chrome does": Super = twice, to 150%, the long page laid out
            # again where it is and not fetched - as tall as it was opened at
            # 150% in Settings, to the pixel, and its letters half again as
            # tall as they were. The first zoom laid the page out larger and
            # drew its words at 100%; the second drew them larger and placed
            # each run at the width it had at 100%, so they ran into each
            # other and the page came out shorter. Then Super 0, 100% again.
            #
            def glyphs():
                wz, hz, pxz = parse_ppm(guest.screendump())
                runs_ = sorted(runs_of_ink(dark_rows(pxz, wz, x0 + 4, y0,
                                                     WIN_W - SBAR - 8,
                                                     WIN_H - TOOL - STAT - 4)))
                return runs_[len(runs_) // 2] if runs_ else 0

            time.sleep(1.0)
            ink100 = glyphs()
            asked_before = len(long_asked)
            mark = len(guest.seen)
            chord("meta_l", "equal")
            tab_said("zoom 125", "Super = to zoom a step larger", mark)
            mark = len(guest.seen)
            chord("meta_l", "equal")
            tab_said("zoom 150", "Super = again, to 150%", mark)
            larger = tab_said("laid out again at ", "the long page laid out at 150%",
                              mark)
            tall150 = int(re.search(r"(\d+) pixels tall", larger).group(1))
            time.sleep(1.5)
            ink150 = glyphs()
            mark = len(guest.seen)
            chord("meta_l", "0")
            tab_said("zoom 100", "Super 0 back to 100%", mark)
            back100 = tab_said("laid out again at ", "the long page at 100% again",
                               mark)

            if tall150 != after or len(long_asked) != asked_before:
                raise Failure(f"the long page laid out again at 150% is {tall150} "
                              f"pixels tall where opened at 150% it is {after}, the "
                              f"server asked {len(long_asked) - asked_before} more "
                              f"times")

            if ink150 * 10 < ink100 * 13:
                raise Failure(f"at 150% the long page's letters are {ink150} pixels "
                              f"tall, against {ink100} at 100% - its words were not "
                              f"drawn larger")

            if int(re.search(r"(\d+) pixels tall", back100).group(1)) != before:
                raise Failure(f"Super 0 did not bring the long page back to its "
                              f"{before} pixels: {back100.strip()!r}")

            print(f"settings: the menu under its button; the costs off the status "
                  f"line ({ink_off} dark pixels, {ink_on} on), no pictures, the "
                  f"long page {after} pixels tall at 150% from {before}, the "
                  f"history cleared, the engine's name said when chosen, and put "
                  f"back; Super = laid it out again at 150%, {tall150} pixels "
                  f"tall as opened there, its letters {ink150} pixels from {ink100}",
                  flush=True)

        if args.part != 2:
            #
            # **Resized** (`roadmap.md` 6zz e) - Diego, 1 October: "make sure our
            # browser new design is resizable". The grip, the window's bottom
            # right corner, dragged 300 pixels left and 100 up: the window manager
            # resizes the frame, the kit hands over a region the new size, and
            # the browser lays the page out again - at 568 by 428, the new page
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

            if not size or (int(size.group(1)), int(size.group(2))) != (568, 428):
                raise Failure(
                    f"the grip dragged 300 left and 100 up did not lay the page "
                    f"out at 568x428: {again.strip()!r}. Wrote {args.out}.")

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

        if args.part != 1:
            #
            # **The tabs are the title bar** in a look with none (`roadmap.md`
            # 6zz d2, 6zj): the last tab closed by Super W, and the window with
            # it; the desktop stopped, Plex chosen, the browser opened again -
            # and the window manager says its header is its title bar, draws the
            # three at the strip's right end, and moves the window by the strip's
            # empty band.
            #
            mark = len(guest.seen)
            chord("meta_l", "d")
            tab_said("a favorite, /Home/Favorites/A long page", "Super D to keep the "
                     "long page again, for Tracker to open", mark)
            chord("meta_l", "w")
            tab_said("the last tab closed, and the window with it",
                     "Super W on the last tab", mark)
            time.sleep(1.0)

            # A prompt *after* the desktop stopped: `wait_for` finds any, and
            # one from before had the line below typed while the desktop was
            # still going, its first half lost.
            def stop_desktop(what):
                stopped = len(guest.seen)
                guest.proc.stdin.write(b"\x17q")
                guest.proc.stdin.flush()
                deadline = time.monotonic() + 30

                while PROMPT not in guest.seen[stopped:]:
                    if time.monotonic() > deadline:
                        raise Failure("the desktop did not stop " + what)

                    guest._read_available()
                    time.sleep(0.3)

                time.sleep(0.5)

            stop_desktop("for Plex to be chosen")
            guest.type('fs.send("/Home/Preferences", { type = "mkdir" }) '
                       'fs.write("/Home/Preferences/appearance", { palette = "plex" }) '
                       'print("plex" .. "-set")')
            guest.wait_for("plex-set", "Plex chosen for the window manager")

            # Tracker on /Home/Favorites, and its first row - the favorite,
            # selected when the folder is shown - clicked: the browser opens on
            # the page it keeps. Tracker passed a file's attributes to nothing
            # when it opened one, and a favorite has no extension.
            mark = len(guest.seen)
            guest.type("wm tracker:/Home/Favorites")
            tline = guest.wait_for_line("wm: window Tracker at ",
                                        "Tracker to open on the favorites", since=mark)
            content = guest.wait_for_line("tracker: content at ",
                                          "Tracker to say where its list is", since=mark)
            tx, ty = (int(v) for v in re.match(r"(\d+),(\d+)", tline).groups())
            row_y = int(re.match(r"(\d+)", content).group(1)) + 32 + 1 + 16
            time.sleep(2.0)
            wp, hp, _ = parse_ppm(guest.screendump())
            mark = len(guest.seen)
            guest.mouse_to(*_to_tablet(tx + 260, ty + row_y, wp, hp))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.1)
            guest.mouse_button(False)
            guest.wait_for_line("browser: showing " + long_url,
                                "the favorite opened from Tracker to show its page",
                                since=mark)
            opened = guest.wait_for_line("wm: window Browser at ",
                                         "the browser to open in Plex", since=mark)

            if "its header the title bar" not in opened:
                raise Failure(f"the browser opened in Plex wearing a title bar: "
                              f"{opened.strip()!r}")

            wx, wy, ww, wh = (int(v) for v in
                              re.match(r"(\d+),(\d+) (\d+)x(\d+)", opened).groups())
            three = guest.wait_for_line("wm: Browser's three at ",
                                        "the three placed in the browser's tabs",
                                        since=mark)
            lx, ly = (int(v) for v in re.match(r"(\d+),(\d+)", three).groups())

            if (lx, ly) != (ww - 12 - 62, (TABS - 18) // 2):
                raise Failure(f"the browser's three are at {lx},{ly} in a window "
                              f"{ww} wide - wanted {ww - 74},{(TABS - 18) // 2}, the "
                              f"strip's right end")

            band = re.search(r"browser: tabs 1, each \d+ wide, the first at \d+,\d+, "
                             r"new \d+,\d+, band (\d+),(\d+)", guest.seen[mark:])

            if not band:
                raise Failure("the browser in Plex did not say where its band is")

            if int(band.group(1)) >= lx:
                raise Failure(f"the browser's band, at {band.group(1)}, is not left "
                              f"of the three at {lx}")

            bx, by = wx + int(band.group(1)), wy + int(band.group(2))
            wp, hp, _ = parse_ppm(guest.screendump())
            held = len(guest.seen)
            guest.mouse_to(*_to_tablet(bx, by, wp, hp))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.3)

            for k in range(1, 7):
                guest.mouse_to(*_to_tablet(bx + 120 * k // 6, by + 80 * k // 6, wp, hp))
                time.sleep(0.15)

            time.sleep(0.4)
            guest.mouse_button(False)
            dragged = guest.wait_for_line("wm: moved Browser",
                                          "the browser to move by its tabs' band",
                                          since=held)
            to = re.search(r"by its header to (\d+),(\d+)", dragged)

            if not to or abs(int(to.group(1)) - (wx + 120)) > 3 \
                    or abs(int(to.group(2)) - (wy + 80)) > 3:
                raise Failure(f"the browser dragged by its tabs' band by 120,80 "
                              f"from {wx},{wy} moved to: {dragged.strip()!r}")

            # **And the history read back from the disk** (6zz d4): this browser
            # was started by Tracker and has shown one page, so a new tab
            # offering more is offering what the first one wrote.
            mark = len(guest.seen)
            chord("meta_l", "t")
            offered = tab_said("a new tab's page, ", "a new tab in the browser Tracker "
                               "started", mark)
            back_from_disk = re.match(r"(\d+) favorites, (\d+) lately", offered)

            if not back_from_disk or int(back_from_disk.group(2)) < 2:
                raise Failure(f"a new browser's new tab did not offer the history the "
                              f"last one wrote: {offered.strip()!r}")

            print(f"title bar: a favorite opened from Tracker showed its page; in "
                  f"Plex the tabs are the browser's title bar, the three at "
                  f"{lx},{ly}, and the strip's band moved the window to "
                  f"{to.group(1)},{to.group(2)}", flush=True)

            # **The tabs it had** (6zz d5): Settings said the browser opens on
            # them. This one closed - its new tab, then the long page, which is
            # the last and closes the window - and the browser started again
            # with no address opens on the long page.
            mark = len(guest.seen)
            chord("meta_l", "w")
            tab_said("tab 1 of 1, shown, showing " + long_url, "the new tab closed", mark)
            chord("meta_l", "w")
            tab_said("the last tab closed, and the window with it",
                     "the long page's tab closed, and the window", mark)
            time.sleep(1.0)
            stop_desktop("for the browser to be started again")
            mark = len(guest.seen)
            guest.type("wm browser")
            tab_said("opened on the tabs it had, 1",
                     "the browser to open on the tabs it had", mark)
            tab_said("showing " + long_url, "the tab it had, its page shown", mark)
            print("restored: the browser started with no address opened on the tab "
                  "it had", flush=True)

        if args.part != 2:
            print(f"wrote {args.out} and {second} ({w_}x{h_})")
            print(f"PASS: a page rendered - {len(runs)} lines of text, "
                  f"{short} to {tall} pixels tall, it scrolled "
                  f"({moved} rows changed), Reload asked again, a link led to "
                  f"{followed[0]}, Home rendered with nothing served, an address "
                  f"typed with http:// brought the second page, Back left it, "
                  f"the PNG and the JPEG were drawn, a page over TLS was Secure, "
                  f"one from an authority it does not trust was refused, "
                  f"Open anyway showed it as Not secure, Wikipedia's Dam "
                  f"article, 1.4 MB, was shown whole in {took:.0f} s, the "
                  f"end of a page {long_page.group(1)} pixels tall was drawn, its "
                  f"picture fetched once, and laid out again when resized.")

        if args.part != 1:
            print("PASS: tabs kept their pages and histories, a favorite was a "
                  "file the star, the bar and the sidebar kept and Tracker "
                  "opened, the history was kept by day on the disk and "
                  "searched, Settings was written as it changed, the caret sat "
                  "where it was, and in Plex the tabs were the title bar.")

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
