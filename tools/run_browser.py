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

# Where the page is, inside the window, and the window is opened at a size
# this file and `browser.lua` both know. Content coordinates: the compositor
# adds a title bar above them, which `find_window` finds.
TOOL, STAT, SBAR = 34, 22, 16
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

        def do_GET(self):
            asked.append(self.path)

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

        guest.type(f"wm browser:10.0.2.2:{port}/{name}")

        found = settle(
            guest,
            lambda w, h, px: find_page(w, h, px),
            "the browser never showed a page: no band of white the width of "
            "its window appeared. A window that opened and rendered nothing "
            "looks exactly like this.",
            seconds=90)

        x0, y0 = found

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
        # `Reload` sits third in the row: two arrow buttons 30 wide with 4
        # between them, so it starts 74 pixels in whatever the interface
        # font is, and is at least 34 wide for any font that can spell the
        # word. The toolbar is the band above the page, and `x0, y0` is the
        # page's top-left corner, so both coordinates come from what was
        # found rather than from where the window was expected to be.
        #
        before_asked = len(asked)

        cx, cy = x0 + 108, y0 - TOOL + 16
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
        # Same row as Reload and measured the same way: the server must be
        # asked for *nothing* and a page must still be on screen. A browser
        # that could only show remote pages could not be tried without
        # starting a server first, which an operating system has no business
        # asking of the computer running it.
        #
        before_home = len(asked)
        hx, hy = x0 + 108 + 60, y0 - TOOL + 16

        guest.mouse_to(*_to_tablet(hx, hy, w4, h4))
        time.sleep(0.4)
        guest.mouse_button(True)
        time.sleep(0.2)
        guest.mouse_button(False)
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
        def typed(text):
            for ch in text:
                guest.proc.stdin.write(ch.encode())
                guest.proc.stdin.flush()
                time.sleep(0.05)

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
        guest.mouse_to(*_to_tablet(x0 + 15, y0 - TOOL + 16, w5, h5))
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
