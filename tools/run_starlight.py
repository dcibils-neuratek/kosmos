#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Starlight's plate (`docs/astra.md` S1), on one machine.

Calculator and Processes opened under Starlight (`wm Starlight.desktop`),
and the one in front dragged by its header clear of the other:

  - the desktop wears its look: the window manager says the look is
    Starlight's, and its chrome is `starlight`;
  - each window's header leaves the plate's room and says so: a plate for
    both, at the header's corner, wider than its title's padding;
  - drawn: the window in front's plate is amber - the look's `tab`, as
    BeOS's tab was yellow - where it said it is, and the one behind's is
    the look's quiet `tab_idle`, never amber;
  - the one behind, clicked on its plate, comes to the front, and the two
    plates change places: amber on it, quiet on the other;
  - the Kosmos menu opened, its top-left corner cut on a slant - not the
    mirror of its round top-right, as every corner is elsewhere (S2);
  - nothing died, and no application stopped on an error - the Deskbar
    drawing its Kosmos button on a plate among them.

The control is the window behind. A plate drawn the same for every window
would pass the first pixel and fail it; a plate drawn by the window itself
could not change when another window is clicked, since nothing tells the
window - only the window manager knows which is in front.

Usage: run_starlight.py IMAGE
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                  # noqa: E402

CHECKS = 8

# `user/themes/Starlight.theme`.
TAB = (0xf2, 0xa9, 0x3b)
TAB_IDLE = (0x2a, 0x2d, 0x4c)


def startup(items):
    return ('print("STARTUP", fs.write("/Home/Preferences/startup", { items = { %s } }))'
            % ", ".join('"%s"' % i for i in items))


def pixels(ppm):
    """A PPM's width, height and a function for one pixel."""
    head = ppm.split(b"\n", 3)
    w, h = (int(v) for v in head[1].split())
    data = head[3]

    def at(x, y):
        o = (y * w + x) * 3
        return tuple(data[o:o + 3])

    return w, h, at


def near(a, b, slack=14):
    return all(abs(p - q) <= slack for p, q in zip(a, b))


def line(guest, pattern, mark, seconds=60):
    end = time.time() + seconds
    while time.time() < end:
        guest._read_available()
        m = re.search(pattern, guest.seen[mark:])
        if m:
            return m
        time.sleep(0.2)
    return None


def run(fails):
    image = "build/x86_64/kosmos.elf" if len(sys.argv) < 2 else sys.argv[1]
    guest = R.Guest(image, 150)
    try:
        guest.wait_for_line("kosmos> ", "the prompt")
        mark = len(guest.seen)
        guest.type(startup(["calc", "procs"]))
        guest.wait_for_line("STARTUP", "the Startup setting", mark)
        guest.type("wm Starlight.desktop")

        loaded = line(guest, r"wm: desktop Starlight loaded, (\d+) started, (\d+) failed", 0, 120)
        screen = line(guest, r"wm: the window server answers, the screen (\d+)x(\d+)", 0, 5)
        time.sleep(6)
        guest._read_available()
        seen = guest.seen

        if not line(guest, r"wm: look starlight, the desktop's", 0, 1):
            fails.append("the desktop did not wear Starlight's look")
        if not re.search(r"wm: chrome starlight", seen):
            fails.append("Starlight's chrome was not starlight")
        if not (loaded and int(loaded.group(2)) == 0 and screen):
            fails.append("the two did not open under Starlight: %r"
                         % (loaded and loaded.group(0)))
            return seen

        sw, sh = int(screen.group(1)), int(screen.group(2))
        where = {}

        for t, x, y, w, h in re.findall(r"wm: window (.+?) at (-?\d+),(-?\d+) (\d+)x(\d+)", seen):
            where[t] = [int(x), int(y), int(w), int(h)]

        plates = {}

        for t, x, y, w, h in re.findall(r"wm: (.+?)'s plate at (\d+),(\d+) (\d+)x(\d+)", seen):
            plates[t] = (int(x), int(y), int(w), int(h))

        named = [t for t in plates if t in where]

        if len(named) < 2 or any(plates[t][2] < 46 for t in named):
            fails.append("not both windows placed a plate: %s, windows %s"
                         % (plates, sorted(where)))
            return seen

        # The one that opened last is in front, and as large as Processes is
        # it covers the other: it is dragged by its header clear of it first,
        # which keeps it in front.
        order = [t for t in re.findall(r"wm: window (.+?) at", seen) if t in named]
        front, back = order[-1], order[-2]
        fx, fy = where[front][0], where[front][1]
        grab = (fx + plates[front][2] + 120, fy + 20)
        mark = len(guest.seen)
        guest.mouse_to(*R._to_tablet(grab[0], grab[1], sw, sh))
        time.sleep(0.4)
        guest.mouse_button(True)
        for k in range(1, 9):
            guest.mouse_to(*R._to_tablet(grab[0] + 60 * k, grab[1] + 55 * k, sw, sh))
            time.sleep(0.08)
        guest.mouse_button(False)
        moved = line(guest, r"wm: moved %s by its header to (-?\d+),(-?\d+)" % re.escape(front),
                     mark, 15)

        if not moved:
            fails.append("%s could not be dragged clear of %s" % (front, back))
            return guest.seen

        where[front][0], where[front][1] = int(moved.group(1)), int(moved.group(2))
        time.sleep(1)

        def plate_pixel(at, t):
            wx, wy = where[t][0], where[t][1]
            px, py, pw, ph = plates[t]
            # Before the title (16 in) and clear of the eased corner.
            return at(wx + px + 8, wy + py + ph // 2)

        _, _, at = pixels(guest.screendump())
        a, b = plate_pixel(at, front), plate_pixel(at, back)

        if not near(a, TAB):
            fails.append("%s, in front, has its plate %r, not amber %r" % (front, a, TAB))
        if near(b, TAB) or not near(b, TAB_IDLE):
            fails.append("%s, behind, has its plate %r, not the quiet %r" % (back, b, TAB_IDLE))

        # ---- the one behind, clicked on its plate ----
        wx, wy = where[back][0], where[back][1]
        px, py, pw, ph = plates[back]
        guest.mouse_to(*R._to_tablet(wx + px + pw // 2, wy + py + ph // 2, sw, sh))
        time.sleep(0.4)
        guest.mouse_button(True)
        time.sleep(0.05)
        guest.mouse_button(False)
        time.sleep(1.5)

        _, _, at = pixels(guest.screendump())
        a, b = plate_pixel(at, back), plate_pixel(at, front)

        if not (near(a, TAB) and near(b, TAB_IDLE)):
            fails.append("clicked, %s's plate is %r and %s's %r, not swapped"
                         % (back, a, front, b))

        # ---- the Kosmos menu, its top-left cut (S2) ----
        # A round corner is the same mirrored at the top-right; the cut is
        # not: it takes more along the diagonal and eases into the sides.
        mark = len(guest.seen)
        guest.mouse_to(*R._to_tablet(40, 16, sw, sh))
        time.sleep(0.4)
        guest.mouse_button(True)
        time.sleep(0.05)
        guest.mouse_button(False)
        menu = line(guest, r"wm: menu of Deskbar at (\d+),(\d+) (\d+)x(\d+)", mark, 15)

        if not menu:
            fails.append("the Kosmos menu did not open")
        else:
            mx, my, mw, mh = (int(menu.group(k)) for k in range(1, 5))
            time.sleep(1)
            shot = guest.screendump()
            _, _, at = pixels(shot)
            odd = 0

            if os.environ.get("KOSMOS_STARLIGHT_SHOTS"):
                os.makedirs("build/starlight", exist_ok=True)
                open("build/starlight/menu.ppm", "wb").write(shot)

            for y in range(10):
                for x in range(10):
                    a, b = at(mx + x, my + y), at(mx + mw - 1 - x, my + y)
                    # Night on night: the menu and the desktop are 14 to
                    # 18 apart in each channel.
                    if max(abs(p - q) for p, q in zip(a, b)) > 8:
                        odd += 1

            if odd < 6:
                fails.append("the menu's top-left is its top-right mirrored (%d pixels differ): "
                             "no cut" % odd)

            guest.type("\x1b")
            time.sleep(0.5)

        # An application stopped by an error is no less broken than a process
        # that died: the Deskbar calling a shape its context did not have
        # was the first thing this look broke.
        guest._read_available()
        broke = re.search(r" died: |\.lua:\d+: [^\n]*(attempt to|expected)|stack traceback",
                          guest.seen)
        if broke:
            fails.append("something stopped: "
                         + guest.seen[max(0, broke.start() - 80):][:300])
        return guest.seen
    finally:
        guest.close()


def main():
    fails = []
    seen = ""

    try:
        seen = run(fails)
    except Exception as e:                        # noqa: BLE001
        fails.append("run: %s" % e)

    if fails:
        print("FAIL: %d of %d checks on Starlight's plate:" % (len(fails), CHECKS))
        for f in fails:
            print("  " + f)
        for l in (seen or "").splitlines():
            if re.search(r"wm: (chrome|look|desktop|window|.*plate|closed)|died|ui: .*plate", l):
                print("  | " + l.strip()[:200])
        return 1

    print("PASS: %d checks on Starlight's plate (the desktop in Starlight's look and "
          "chrome; Calculator and Processes each placed a plate; the window in front's "
          "amber and the one behind's quiet; the one behind clicked, the two swapped; "
          "the Kosmos menu's top-left cut; nothing died)" % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
