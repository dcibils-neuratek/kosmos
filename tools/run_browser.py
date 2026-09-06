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

Two things are checked and neither is subtle, which is deliberate - this is
mostly a camera, and a check that goes stale is worse than no check:

  * **The page area has ink on it.** A window that opened and rendered
    nothing is the exact failure mode of every stage of this so far, and it
    looks identical to a working browser in a thumbnail.
  * **More than one text size is present.** The reason the browser draws its
    own pixels is that the compositor would rasterise every line in one
    face. Runs of dark pixels of two clearly different heights is the
    cheapest evidence that a heading is a heading.
  * **It scrolls.** The page is laid out once into a surface taller than the
    window and a scroll is one blit out of it, which is the whole reason for
    the mode - so the picture after six presses of Down has to differ from
    the picture before them. A browser that laid out correctly and would not
    move is a browser nobody can read the bottom of.
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

Usage: run_browser.py <image> --out <file.png> [--page <file.html>]
"""

import argparse
import http.server
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from run_screenshot import (Guest, Failure, PROMPT, _to_tablet,  # noqa: E402
                            parse_ppm, settle)
from run_gallery import png                                      # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

# What the page's own links point at, and therefore what the server must be
# asked for once one of them is clicked.
LINKED = "test_linked.html"

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


def serve(directory, asked):
    """An HTTP server on an ephemeral port, in a thread. Returns the port.

    `asked` is a list the handler appends every path to, which is how the
    Reload check knows the button did something rather than merely looking
    pressed.
    """
    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=directory, **kw)

        def do_GET(self):
            asked.append(self.path)
            super().do_GET()

        def log_message(self, *a):
            pass

    httpd = http.server.HTTPServer(("0.0.0.0", 0), Handler)
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


def find_link(px, width, x0, y0, w, h):
    """The middle of the first stretch of link-blue text, or None.

    By colour rather than by position: `web_paint.c` paints a run inside an
    `<a>` in a blue nothing else on a page uses, so finding one is also the
    check that the run knew where it came from. The test is loose because
    glyphs are antialiased against white - only the fully covered pixels of
    a stem are the ink itself.
    """
    at = reader(px, width)

    for y in range(y0, y0 + h):
        run = None

        for x in range(x0, x0 + w):
            r, g, b = at(x, y)
            blue = b > 130 and b > r + 60 and b > g + 40

            if blue:
                if run is None:
                    run = x
            elif run is not None:
                if x - run >= 6:
                    return (run + x) // 2, y
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
    ap.add_argument("--page", default=os.path.join(HERE, "test_page.html"))
    ap.add_argument("--timeout", type=int, default=240)
    args = ap.parse_args()

    directory = os.path.dirname(os.path.abspath(args.page))
    name = os.path.basename(args.page)

    asked = []
    httpd, port = serve(directory, asked)

    import run_screenshot
    saved = (run_screenshot.QEMU_ARGS, run_screenshot.X86_ARGS)
    run_screenshot.extra_args(args.image, [
        "-netdev", "user,id=net0",
        "-device", run_screenshot.device(args.image, "net") + ",netdev=net0",
    ])

    guest = None

    try:
        guest = Guest(args.image, args.timeout)
        guest.wait_for(PROMPT, "reached a shell")

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

        print(f"wrote {args.out} and {second} ({w_}x{h_})")
        print(f"PASS: a page rendered - {len(runs)} lines of text, "
              f"{short} to {tall} pixels tall, it scrolled "
              f"({moved} rows changed), Reload asked again, a link led to "
              f"{followed[0]}, and Home rendered with nothing served.")

    except Failure as why:
        print("\nFAIL: %s" % why, file=sys.stderr)
        return 1
    finally:
        run_screenshot.QEMU_ARGS, run_screenshot.X86_ARGS = saved
        httpd.shutdown()

        if guest is not None:
            guest.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
