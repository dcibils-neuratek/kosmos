#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Platinum's title bar (`docs/pewter.md` P2), in two machines side by side.

The gallery's applications, every one, opened under Pewter
(`wm Pewter.desktop`, its Startup page the gallery's list): each window
that places its boxes places them Platinum's way - close alone at the left
of its title bar and the pair, zoom and collapse, at the right - and none
of them Astra's run of three.

And Calculator alone under Pewter, pressed:

  - drawn: the close box's edge in Platinum's grey where its window said it
    is, and nothing red where Astra's close would be;
  - the collapse box, clicked: the window collapsed to its bar, and below
    the bar the desktop again;
  - a double click on the bar: opened out again;
  - one click on the bar between close and the pair: no box - neither a
    collapse nor a close;
  - the close box, clicked: the window closed.

The control is the click between the boxes. Astra's boxes were hit-tested
as one run, and a Platinum window measured that way would take its whole
title bar for boxes; the press there must be the window's drag, and acting
on it as a box is what this would catch.

Usage: run_platinum.py IMAGE
"""

import os
import re
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                  # noqa: E402
import run_gallery as G                                     # noqa: E402

CHECKS = 9

# The gallery's list, with no files behind it: what each opens on is the
# gallery's to make, and an application opened on nothing still has a bar.
APPS = [o.split(":")[0] if o.split(":")[0] in ("ide", "texteditor", "writer") else o
        for o in G.OPEN if not o.startswith("tile")]

# Platinum's close box ring on a light look (`OUT.platinum_colours`).
RING = (0x7a, 0x7a, 0x80)


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


def boot(items, timeout=150):
    guest = R.Guest("build/x86_64/kosmos.elf" if len(sys.argv) < 2 else sys.argv[1], timeout)
    guest.wait_for_line("kosmos> ", "the prompt")
    mark = len(guest.seen)
    guest.type(startup(items))
    guest.wait_for_line("STARTUP", "the Startup setting", mark)
    guest.type("wm Pewter.desktop")
    return guest


def line(guest, pattern, mark, seconds=60):
    end = time.time() + seconds
    while time.time() < end:
        guest._read_available()
        m = re.search(pattern, guest.seen[mark:])
        if m:
            return m
        time.sleep(0.2)
    return None


def gallery(fails):
    """Every application of the gallery, its boxes placed Platinum's way."""
    guest = boot(APPS)
    try:
        loaded = line(guest, r"wm: desktop Pewter loaded, (\d+) started, (\d+) failed", 0, 150)
        time.sleep(8)
        guest._read_available()
        seen = guest.seen
    finally:
        guest.close()

    told = re.findall(r"wm: (.+?)'s close at (\d+),(\d+) and its pair at (\d+) in it", seen)
    astra = re.findall(r"wm: (.+?)'s three at", seen)
    wrong = [t for t, x, y, x2 in told if not int(x) < int(x2)]

    if not (loaded and int(loaded.group(2)) == 0):
        fails.append("the gallery did not all open under Pewter: %r"
                     % (loaded and loaded.group(0)))
    if not re.search(r"wm: chrome platinum", seen):
        fails.append("Pewter's chrome was not Platinum")
    if len({t for t, *_ in told}) < len(APPS) * 2 // 3:
        fails.append("only %d of %d applications placed Platinum's boxes: %s"
                     % (len({t for t, *_ in told}), len(APPS), sorted({t for t, *_ in told})))
    if wrong or astra:
        fails.append("boxes placed as Astra's or the wrong way round: %s %s" % (wrong, astra))
    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:300])

    return seen


def calculator(fails):
    """Calculator alone: drawn, collapsed, opened out, closed."""
    guest = boot(["calc"])
    try:
        opened = line(guest, r"wm: window Calculator at (\d+),(\d+) (\d+)x(\d+)", 0, 90)
        told = line(guest, r"wm: Calculator's close at (\d+),(\d+) and its pair at (\d+) in it", 0, 60)
        screen = line(guest, r"wm: the window server answers, the screen (\d+)x(\d+)", 0, 5)
        line(guest, r"wm: desktop Pewter loaded", 0, 60)
        time.sleep(3)

        if not (opened and told and screen):
            fails.append("Calculator never said where its boxes are: window %s, boxes %s"
                         % (bool(opened), bool(told)))
            return guest.seen

        wx, wy, ww, wh = (int(opened.group(k)) for k in range(1, 5))
        cx, cy, px = (int(told.group(k)) for k in range(1, 4))
        sw, sh = int(screen.group(1)), int(screen.group(2))
        box = 20                                  # `OUT.BOX`, at one to one

        def click(x, y, twice=False):
            guest.mouse_to(*R._to_tablet(x, y, sw, sh))
            time.sleep(0.4)
            for k in range(2 if twice else 1):
                guest.mouse_button(True)
                time.sleep(0.05)
                guest.mouse_button(False)
                time.sleep(0.08)

        # ---- drawn ----
        _, _, at = pixels(guest.screendump())
        d = box - 6
        edge = at(wx + cx + (box - d) // 2, wy + cy + box // 2)
        # Astra's close: the last of a run of three, 12 in from the right.
        red = at(wx + ww - 12 - box // 2, wy + cy + box // 2)

        if edge != RING:
            fails.append("the close box's edge is %r, not Platinum's %r" % (edge, RING))
        if red[0] > 200 and red[1] < 120:
            fails.append("something red where Astra's close would be: %r" % (red,))

        # ---- collapse, by its box ----
        mark = len(guest.seen)
        click(wx + px + (box + 4) + box // 2, wy + cy + box // 2)
        collapsed = line(guest, r"wm: Calculator collapsed", mark, 15)
        time.sleep(1)
        _, _, at = pixels(guest.screendump())
        below = at(wx + ww // 2, wy + wh - 10)
        inside = at(wx + ww // 2, wy + 8)

        if not (collapsed and below != inside):
            fails.append("the collapse box did not take the window to its bar: %s, below %r"
                         % (bool(collapsed), below))

        # ---- the bar between the boxes, once: no box ----
        mark = len(guest.seen)
        click(wx + (cx + px) // 2, wy + cy + box // 2)
        time.sleep(1.5)
        guest._read_available()
        between = guest.seen[mark:]

        if re.search(r"wm: Calculator (collapsed|opened out)|wm: closed Calculator", between):
            fails.append("a press on the bar between the boxes acted as a box: %r"
                         % between[:200])

        # ---- opened out, by a double click on the bar ----
        time.sleep(1)
        mark = len(guest.seen)
        click(wx + (cx + px) // 2, wy + cy + box // 2, twice=True)

        if not line(guest, r"wm: Calculator opened out", mark, 15):
            fails.append("a double click on the bar did not open the window out")

        # ---- closed, by its box ----
        time.sleep(1)
        mark = len(guest.seen)
        click(wx + cx + box // 2, wy + cy + box // 2)

        if not line(guest, r"wm: closed Calculator", mark, 15):
            fails.append("the close box did not close the window")

        guest._read_available()
        return guest.seen
    finally:
        guest.close()


def main():
    fails = []
    seen = {}

    def run(name, f):
        try:
            seen[name] = f(fails)
        except Exception as e:                    # noqa: BLE001
            fails.append("%s: %s" % (name, e))

    jobs = [threading.Thread(target=run, args=("gallery", gallery)),
            threading.Thread(target=run, args=("calculator", calculator))]
    for j in jobs:
        j.start()
    for j in jobs:
        j.join()

    if fails:
        print("FAIL: %d of %d checks on Platinum's title bar:" % (len(fails), CHECKS))
        for f in fails:
            print("  " + f)
        for name, text in seen.items():
            for l in (text or "").splitlines():
                if re.search(r"wm: (chrome|desktop|.*close at|.*three at|.*collapsed|"
                             r".*opened out|closed)|died", l):
                    print("  %s | %s" % (name, l.strip()[:200]))
        return 1

    print("PASS: %d checks on Platinum's title bar (the gallery's %d applications opened "
          "under Pewter with close at the left and the pair at the right, none as Astra's; "
          "Calculator's close box drawn in Platinum's grey, collapsed by its box and opened "
          "out by a double click on its bar, a press between the boxes taken as no box, "
          "and closed by its close box)" % (CHECKS, len(APPS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
