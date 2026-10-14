#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The PDF Viewer's window (`docs/pdfview.md` step 2).

A PDF of six pages made here by `tools/mkpdf.py`, a JPEG on its first -
the four solid quadrants of `assets/images/test-quads.jpg` - opened in
the viewer on a desktop:

  - it opens as the whole document at Fit width, page 1 and its
    neighbour rendered, the picture's red on the screen (none before);
  - End goes to page 6 and Home back to 1, the cache rendering what comes
    into view;
  - Control-plus zooms a step in from Fit width, Control-0 back to it;
  - the View menu, pressed by name (`menu/View/One page at a time`), lays
    out one page, the Go menu's Next page turns it, and the published
    Page field takes a number;
  - the window made smaller by the window manager lays its pages out
    again, at Fit width a smaller zoom, at the same page - and, narrower
    than the sidebar leaves room for, hides it;
  - the sidebar (step 3): thumbnails rendered for the pages in it, one
    clicked with the pointer going to its page and ringing it; Control-B
    hiding it and the pages growing into the room; the Contents tab from the
    View menu, and a chapter in it pressed by its title.

Usage: run_pdfview.py IMAGE
"""

import os
import random
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402
import run_screenshot as R                                  # noqa: E402
import run_servers as S                                     # noqa: E402

PAGES = 6
PATH = "/Temporary/doc.pdf"
NAME = "doc.pdf"

LAUNCH = ('local r = fs.send("/Running/wm", { type = "launch", program = "pdfview", args = args })\n'
          'print("LAUNCH " .. tostring(r and r.ok))\n')

# The name under /Running of every window, one a line.
NAMES = ('for _, n in ipairs(fs.list("/Running") or {}) do\n'
         '  print("NAME " .. tostring(type(n) == "table" and n.name or n))\n'
         'end\n')

# `args` is "<app>|<verb>|<address>[|<value>]".
ASK = ('local app, verb, address, value = args:match("^([^|]*)|([^|]*)|([^|]*)|?(.*)$")\n'
       'local r, why = fs.send("/Running/" .. app .. "/controls/" .. address,\n'
       '                       { type = verb, value = value ~= "" and value or nil })\n'
       'print(("ASK %s %s %s %s"):format(verb, address, r and "ok" or "refused",\n'
       '      tostring(r and r.value or why)))\n')

# The viewer's window made `args` ("W H") in size, by the window manager.
RESIZE = ('local wmproto = use("/Kosmos/Libraries/wmproto.lua")\n'
          'local w, h = args:match("^(%d+) (%d+)$")\n'
          'for _, win in ipairs((wmproto.windows() or {}).windows or {}) do\n'
          '  if tostring(win.title):find("' + NAME + '", 1, true) then\n'
          '    local r, why = fs.send("/Running/wm", { type = "resize", window = win.handle,\n'
          '                                            w = tonumber(w), h = tonumber(h) })\n'
          '    print("RESIZE " .. tostring(r and r.ok) .. " " .. tostring(why or (r and r.error)))\n'
          '  end\n'
          'end\n')

RED = (0xd0, 0x28, 0x28)

LAYOUT = r"pdfview: (\d+) of (\d+), ([^,]+), (\d+)%, ([^,]+), the view (\d+)x(\d+), the sidebar (\w+)"


def fixture(work):
    text = os.path.join(work, "doc.txt")
    out = os.path.join(work, "doc.pdf")
    with open(text, "w") as f:
        # 46 lines a page (`mkpdf.py`'s `layout`), and no line after the
        # last: a trailing newline is a line, and a seventh page.
        lines = []
        for n in range(1, PAGES + 1):
            lines += ["# Page %d" % n, "This is the page numbered %d of the document." % n]
            lines += [""] * (44 if n < PAGES else 0)
        f.write("\n".join(lines))
    subprocess.run([sys.executable, os.path.join(HERE, "mkpdf.py"), text, out,
                    "--tounicode", "--outline", "--jpeg", "assets/images/test-quads.jpg"],
                   check=True, capture_output=True, cwd=ROOT)
    with open(out, "rb") as f:
        return f.read()


def reds(guest):
    width, height, px = R.parse_ppm(guest.screendump())
    n = 0
    for o in range(0, len(px) - 2, 3 * 2):
        if (abs(px[o] - RED[0]) <= 24 and abs(px[o + 1] - RED[1]) <= 24
                and abs(px[o + 2] - RED[2]) <= 24):
            n += 1
    return n


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    pdf = fixture(scratch.directory("pdfview"))

    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web)
    checks, fails, said = 0, [], {}

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            fails.append(complaint)

    def line(pattern, since, seconds=20):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            guest._read_available()
            m = re.search(pattern, guest.seen[since:])
            if m:
                return m
            time.sleep(0.05)
        return None

    def last(pattern, since, seconds=10):
        """The last match after `since` once the guest has gone quiet on it."""
        line(pattern, since, seconds)
        time.sleep(0.6)
        guest._read_available()
        found = list(re.finditer(pattern, guest.seen[since:]))
        return found[-1] if found else None

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)

        for name, text in (("launch", LAUNCH), ("names", NAMES), ("ask", ASK), ("resize", RESIZE)):
            session.put(text.encode(), "/Temporary/%s.lua" % name)
        session.put(pdf, PATH)

        red_before = reds(guest)

        # ---- it opens: the whole document at Fit width ----
        mark = len(guest.seen)
        session.run("/Temporary/launch.lua " + PATH)
        opened = line(r"pdfview: %s, (\d+) pages, (\d+) in its contents" % re.escape(PATH), mark, 40)
        check(opened is not None and opened.group(1) == str(PAGES) and opened.group(2) == str(PAGES),
              "the document did not open with its %d pages and %d entries of contents: %r"
              % (PAGES, PAGES, opened and opened.group(0)))
        laid = line(LAYOUT, mark, 10)
        check(laid is not None and laid.group(1) == "1" and laid.group(3) == "Fit width"
              and laid.group(5) == "the whole document" and laid.group(8) == "pages",
              "it did not open on page 1, Fit width, the whole document: %r" % (laid and laid.group(0)))
        fit_pct = int(laid.group(4)) if laid else 0
        view = (int(laid.group(6)), int(laid.group(7))) if laid else (0, 0)

        first = line(r"pdfview: page 1 of %d, (\d+) glyphs, (\d+) faces missing, (\d+) pictures" % PAGES,
                     mark, 40)
        check(first is not None and int(first.group(1)) > 20 and first.group(2) == "0"
              and first.group(3) == "1",
              "page 1 was not rendered with its words and its picture: %r" % (first and first.group(0)))
        second = line(r"pdfview: page 2 of %d, " % PAGES, mark, 30)
        check(second is not None, "page 2, next to the view, was not rendered ahead")

        time.sleep(1.0)
        red_after = reds(guest)
        check(red_after > red_before + 400,
              "the picture's red is not on the screen: %d red points before, %d after"
              % (red_before, red_after))
        said["red"] = "%d -> %d" % (red_before, red_after)

        # ---- the keys: End, Home ----
        mark = len(guest.seen)
        guest.sendkey("end")
        at = line(r"pdfview: at page (\d+) of %d" % PAGES, mark, 10)
        check(at is not None and at.group(1) == str(PAGES), "End did not go to page %d: %r"
              % (PAGES, at and at.group(0)))
        rendered = line(r"pdfview: page %d of %d, " % (PAGES, PAGES), mark, 30)
        check(rendered is not None, "page %d was not rendered when it came into view" % PAGES)

        mark = len(guest.seen)
        guest.sendkey("home")
        at = line(r"pdfview: at page (\d+) of %d" % PAGES, mark, 10)
        check(at is not None and at.group(1) == "1", "Home did not go to page 1: %r"
              % (at and at.group(0)))

        # ---- zoom: a step in, and back to Fit width ----
        mark = len(guest.seen)
        guest.sendkey("ctrl-equal")
        zoomed = line(LAYOUT, mark, 10)
        check(zoomed is not None and zoomed.group(3).endswith("%")
              and int(zoomed.group(4)) > fit_pct,
              "Control-plus did not zoom a step in from %d%%: %r" % (fit_pct, zoomed and zoomed.group(0)))
        again = line(r"pdfview: page 1 of %d, .* at (\d+)%%" % PAGES, mark, 30)
        check(again is not None and zoomed is not None and again.group(1) == zoomed.group(4),
              "page 1 was not rendered again at the new zoom: %r" % (again and again.group(0)))

        mark = len(guest.seen)
        guest.sendkey("ctrl-0")
        back = line(LAYOUT, mark, 10)
        check(back is not None and back.group(3) == "Fit width" and int(back.group(4)) == fit_pct,
              "Control-0 did not go back to Fit width at %d%%: %r" % (fit_pct, back and back.group(0)))

        # ---- the menus and the page, by name ----
        names = session.run("/Temporary/names.lua").decode(errors="replace")
        app = None
        for n in re.findall(r"NAME ([^\r\n]+)", names):
            if "PDF" in n or NAME in n:
                app = n.strip()
        said["app"] = app
        check(app is not None, "the viewer is not in /Running: %r" % names)
        app = app or "PDF Viewer"

        def ask(verb, address, value=""):
            out = session.run("/Temporary/ask.lua %s|%s|%s|%s" % (app, verb, address, value))
            out = out.decode(errors="replace")
            said["%s %s" % (verb, address)] = out
            return out

        mark = len(guest.seen)
        out = ask("press", "menu/View/One page at a time")
        laid = line(LAYOUT, mark, 10)
        check("ok" in out and laid is not None and laid.group(5) == "one page at a time",
              "View, One page at a time did not lay out one page: %r %r" % (out, laid and laid.group(0)))

        mark = len(guest.seen)
        out = ask("press", "menu/Go/Next page")
        laid = line(LAYOUT, mark, 10)
        check(laid is not None and laid.group(1) == "2" and laid.group(5) == "one page at a time",
              "Go, Next page did not turn to page 2: %r %r" % (out, laid and laid.group(0)))

        mark = len(guest.seen)
        out = ask("write", "field/Page", "4")
        at = line(r"pdfview: at page (\d+) of %d" % PAGES, mark, 10)
        check(at is not None and at.group(1) == "4", "the Page field did not go to page 4: %r %r"
              % (out, at and at.group(0)))
        out = ask("read", "field/Page")
        check("ok 4" in out, "the Page field does not read 4: %r" % out)

        mark = len(guest.seen)
        ask("press", "menu/View/The whole document, scrolled")
        laid = line(LAYOUT, mark, 10)
        check(laid is not None and laid.group(5) == "the whole document" and laid.group(1) == "4",
              "back to the whole document, page 4 was not kept: %r" % (laid and laid.group(0)))

        # ---- the sidebar: thumbnails, one clicked ----
        thumbs = {}
        for m in re.finditer(r"pdfview: thumbnail (\d+) of %d at (\d+),(\d+)" % PAGES, guest.seen):
            thumbs[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))
        if 3 not in thumbs:
            line(r"pdfview: thumbnail 3 of %d at " % PAGES, 0, 20)
            for m in re.finditer(r"pdfview: thumbnail (\d+) of %d at (\d+),(\d+)" % PAGES, guest.seen):
                thumbs[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))
        check(all(k in thumbs for k in (1, 2, 3)),
              "the sidebar's thumbnails were not rendered: %r" % sorted(thumbs))
        said["thumbs"] = sorted(thumbs)

        if 3 in thumbs:
            width, height, _ = R.parse_ppm(guest.screendump())
            mark = len(guest.seen)
            guest.mouse_to(*R._to_tablet(thumbs[3][0], thumbs[3][1], width, height))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.1)
            guest.mouse_button(False)
            at = line(r"pdfview: at page (\d+) of %d" % PAGES, mark, 10)
            check(at is not None and at.group(1) == "3",
                  "a click on page 3's thumbnail did not go to page 3: %r" % (at and at.group(0)))

        # ---- Control-B: hidden, and the pages given the room ----
        mark = len(guest.seen)
        guest.sendkey("ctrl-b")
        hidden = line(LAYOUT, mark, 10)
        check(hidden is not None and hidden.group(8) == "hidden"
              and int(hidden.group(6)) > view[0] and int(hidden.group(4)) > fit_pct,
              "Control-B did not hide the sidebar and widen the pages: %r (the view was %r at %d%%)"
              % (hidden and hidden.group(0), view, fit_pct))
        mark = len(guest.seen)
        guest.sendkey("ctrl-b")
        shown = line(LAYOUT, mark, 10)
        check(shown is not None and shown.group(8) == "pages" and int(shown.group(6)) == view[0],
              "Control-B again did not bring the sidebar back: %r" % (shown and shown.group(0)))

        # ---- the contents, from the View menu, and a chapter by its title ----
        mark = len(guest.seen)
        ask("press", "menu/View/Contents")
        told = line(r"pdfview: the sidebar shows its (\w+)", mark, 10)
        check(told is not None and told.group(1) == "contents",
              "View, Contents did not show the contents: %r" % (told and told.group(0)))
        mark = len(guest.seen)
        out = ask("press", "contents/Page 5")
        at = line(r"pdfview: at page (\d+) of %d" % PAGES, mark, 10)
        check(at is not None and at.group(1) == "5",
              "the contents' Page 5 did not go to page 5: %r %r" % (out, at and at.group(0)))
        mark = len(guest.seen)
        ask("press", "menu/Go/Previous page")
        at = line(r"pdfview: at page (\d+) of %d" % PAGES, mark, 10)
        mark = len(guest.seen)
        ask("press", "contents/Page 4")
        line(r"pdfview: at page (\d+) of %d" % PAGES, mark, 10)

        # ---- resized: laid out again, smaller at Fit width, the same page ----
        mark = len(guest.seen)
        out = session.run("/Temporary/resize.lua 620 520").decode(errors="replace")
        said["resize"] = out
        laid = last(LAYOUT, mark, 10)
        check(laid is not None and (int(laid.group(6)), int(laid.group(7))) != view
              and int(laid.group(6)) < view[0] and int(laid.group(4)) < fit_pct
              and laid.group(1) == "4" and laid.group(8) == "hidden",
              "resized, the pages were not laid out again smaller at page 4, the sidebar hidden: "
              "%r (was %r at %d%%)"
              % (laid and laid.group(0), view, fit_pct))
        rerendered = line(r"pdfview: page 4 of %d, .* at (\d+)%%" % PAGES, mark, 30)
        check(rerendered is not None and laid is not None and rerendered.group(1) == laid.group(4),
              "page 4 was not rendered again at the new size: %r" % (rerendered and rerendered.group(0)))
    except Exception as e:                  # noqa: BLE001 - said below
        check(False, "the steps stopped: %s: %s" % (type(e).__name__, str(e)[:500]))
    finally:
        transcript = guest.seen
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on the PDF Viewer's window:" % (len(fails), checks))
        for f in fails:
            print("  " + f)
        for k, v in said.items():
            print("  %s: %r" % (k, str(v)[-300:]))
        print("--- the guest said ---\n" + transcript[-4000:])
        return 1

    print("PASS: %d checks on the PDF Viewer's window (six pages at Fit width with the "
          "picture on the screen - red %s; End and Home; a zoom step and back; one page at a "
          "time, Next page and the Page field by name; thumbnails, one clicked; Control-B; the "
          "contents by name; resized, laid out again, the sidebar hidden)"
          % (checks, said.get("red")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
