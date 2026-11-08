#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Pewter's desktop and spatial folders (`docs/pewter.md` P4), in two
machines side by side.

Pewter:

  - the desktop in Mac OS's arrangement: the Drive at the top right, the
    Trash at the bottom right;
  - the Trash opened from it, as a spatial folder: a window named for the
    folder, with no sidebar;
  - opened again while it is open: the same window brought forward, and no
    second one;
  - moved by its title bar, resized by its grip and switched to a list,
    each kept on the folder as it happens;
  - closed by its box, and opened again: in the same place, at the same
    size, in the same view.

Astra, with the same desktop folder:

  - its own arrangement, the Drive at the top left;
  - the Drive opened from it in an ordinary Tracker window, not a spatial
    one.

The controls: a folder that is never written to (`fs.setattr` in `keep`
taken out) opens the second time where a new one would, which fails the
last check; and a window manager that does not keep `doc` opens a second
window on the folder, which fails the third.

Usage: run_spatial.py IMAGE
"""

import os
import re
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                  # noqa: E402
from run_platinum import startup, line                      # noqa: E402

CHECKS = 9

TRASH = "/Home/Desktop/Trash"
DESK = r"tracker: the desktop (\d+)x(\d+), (?:.*?)Trash at (\d+),(\d+)"
DRIVE = r"tracker: the desktop (\d+)x(\d+), (?:.*?)Drive at (\d+),(\d+)"
OPENED = r"tracker: %s opened spatially at (-?\d+),(-?\d+) (\d+)x(\d+) (\w+)" % re.escape(TRASH)
KEPT = r"tracker: %s kept at (-?\d+),(-?\d+) (\d+)x(\d+) (\w+), %s"
WINDOW = r"wm: window %s at (-?\d+),(-?\d+) (\d+)x(\d+)"

CELL_W = 112          # a desktop icon's cell, `CELL_W` in tracker.lua


def boot(desktop, timeout=150):
    image = "build/x86_64/kosmos.elf" if len(sys.argv) < 2 else sys.argv[1]
    guest = R.Guest(image, timeout)
    guest.wait_for_line("kosmos> ", "the prompt")
    mark = len(guest.seen)
    guest.type(startup([]))
    guest.wait_for_line("STARTUP", "the Startup setting", mark)
    guest.type("wm %s.desktop" % desktop)
    return guest


def pointer(guest, sw, sh):
    def click(x, y, twice=False):
        guest.mouse_to(*R._to_tablet(x, y, sw, sh))
        time.sleep(0.4)
        for _ in range(2 if twice else 1):
            guest.mouse_button(True)
            time.sleep(0.05)
            guest.mouse_button(False)
            time.sleep(0.08)
        time.sleep(0.2)

    def drag(fx, fy, tx, ty):
        guest.mouse_to(*R._to_tablet(fx, fy, sw, sh))
        time.sleep(0.3)
        guest.mouse_button(True)
        time.sleep(0.3)
        for i in range(1, 13):
            guest.mouse_to(*R._to_tablet(fx + (tx - fx) * i // 12,
                                         fy + (ty - fy) * i // 12, sw, sh))
            time.sleep(0.06)
        time.sleep(0.4)
        guest.mouse_button(False)
        time.sleep(0.8)

    return click, drag


def nums(m, n=4):
    return tuple(int(m.group(k)) for k in range(1, n + 1))


def last(guest, pattern):
    """The last line of a kind: the desktop says its size again whenever a
    strip above it changes it."""
    guest._read_available()
    found = list(re.finditer(pattern, guest.seen))
    return found[-1] if found else None


def pewter(fails):
    guest = boot("Pewter")
    try:
        line(guest, r"wm: desktop Pewter loaded", 0, 150)
        screen = line(guest, r"wm: the window server answers, the screen (\d+)x(\d+)", 0, 5)
        line(guest, DESK, 0, 30)
        time.sleep(3)
        trash, drive = last(guest, DESK), last(guest, DRIVE)

        if not (screen and trash and drive):
            fails.append("Pewter's desktop never said where its Trash and Drive are")
            return guest.seen

        dw, dh, tx, ty = nums(trash)
        _, _, vx, vy = nums(drive)
        ox, oy = 0, int(screen.group(2)) - dh      # the desktop is below the bar

        if not (vx == dw - 2 - CELL_W and vy <= 4
                and tx == dw - 2 - CELL_W and ty >= dh - 200):
            fails.append("not Mac OS's arrangement on %dx%d: Drive at %d,%d, Trash at %d,%d"
                         % (dw, dh, vx, vy, tx, ty))

        click, drag = pointer(guest, *nums(screen, 2))
        at_trash = (ox + tx + CELL_W // 2, oy + ty + 30)

        # ---- the Trash, opened spatially ----
        mark = len(guest.seen)
        click(*at_trash, twice=True)
        opened = line(guest, OPENED, mark, 30)
        win = line(guest, WINDOW % "Trash", mark, 10)
        band = line(guest, r"tracker: band at (\d+),(\d+)", mark, 10)
        view = line(guest, r"tracker: content at \d+, view at (\d+),(\d+)", mark, 10)
        close = line(guest, r"wm: Trash's close at (\d+),(\d+)", mark, 10)
        time.sleep(2)
        guest._read_available()

        if not (opened and win and band and view and close):
            fails.append("the Trash did not open as a spatial folder: opened %s, window %s, "
                         "band %s, view %s, close %s" % tuple(bool(v) for v in
                                                              (opened, win, band, view, close)))
            return guest.seen

        if "tracker: sidebar" in guest.seen[mark:]:
            fails.append("the spatial window has a sidebar")

        # ---- opened again: the same window ----
        mark = len(guest.seen)
        click(*at_trash, twice=True)
        again = line(guest, r"tracker: %s is open already, brought to the front"
                     % re.escape(TRASH), mark, 20)
        time.sleep(2)
        guest._read_available()

        if not again or re.search(WINDOW % "Trash", guest.seen[mark:]):
            fails.append("opened again, the Trash was not one window: told %s, second window %s"
                         % (bool(again), bool(re.search(WINDOW % "Trash", guest.seen[mark:]))))

        x, y, w, h = nums(win)
        bx, by = nums(band, 2)

        # ---- moved, resized, a list ----
        mark = len(guest.seen)
        drag(x + bx, y + by, x + bx + 150, y + by + 90)
        moved = line(guest, KEPT % (re.escape(TRASH), "moved"), mark, 15)

        if moved:
            x, y = nums(moved, 2)

        mark = len(guest.seen)
        drag(x + w - 3, y + h - 3, x + w + 117, y + h + 77)
        resized = line(guest, KEPT % (re.escape(TRASH), "resized"), mark, 15)

        # The header's right end follows the window's right edge.
        grew = (int(resized.group(3)) - w) if resized else 0
        mark = len(guest.seen)
        click(x + int(view.group(1)) + grew, y + int(view.group(2)))
        menu = line(guest, r"wm: menu of Trash at (\d+),(\d+) (\d+)x(\d+)", mark, 15)
        rows = line(guest, r"ui: Trash's menu, rows of (\d+)", mark, 5)
        listed = None

        if menu and rows:
            mx, my, _, _ = nums(menu)
            row = int(rows.group(1))
            time.sleep(1)
            mark = len(guest.seen)
            click(mx + 30, my + 2 + row + row // 2)
            listed = line(guest, KEPT % (re.escape(TRASH), "the view"), mark, 15)

        if not (moved and resized and listed):
            fails.append("moved %s, resized %s, a list %s: not each kept on the folder"
                         % (bool(moved), bool(resized), bool(listed)))
            return guest.seen

        want = nums(listed) + (listed.group(5),)

        if not (want[0] == x and abs(want[2] - (w + 120)) <= 6
                and abs(want[3] - (h + 80)) <= 6 and want[4] == "list"):
            fails.append("what was kept is not what was done: %r, from %d,%d %dx%d"
                         % (want, x, y, w, h))

        # ---- closed, and opened again where it was left ----
        time.sleep(1)
        mark = len(guest.seen)
        cx, cy = nums(close, 2)
        click(x + cx + 10, y + cy + 10)
        closed = line(guest, r"wm: closed Trash", mark, 15)
        time.sleep(2)

        mark = len(guest.seen)
        click(*at_trash, twice=True)
        back = line(guest, OPENED, mark, 30)
        back_win = line(guest, WINDOW % "Trash", mark, 10)

        if not (closed and back and back_win):
            fails.append("closed %s and opened again %s" % (bool(closed), bool(back)))
        elif (nums(back) + (back.group(5),)) != want:
            fails.append("opened again at %s, not where it was left, %s"
                         % (back.group(0), listed.group(0)))

        guest._read_available()
        return guest.seen
    finally:
        guest.close()


def astra(fails):
    guest = boot("Astra")
    try:
        line(guest, r"wm: desktop Astra loaded", 0, 150)
        screen = line(guest, r"wm: the window server answers, the screen (\d+)x(\d+)", 0, 5)
        line(guest, DESK, 0, 30)
        time.sleep(3)
        trash, drive = last(guest, DESK), last(guest, DRIVE)

        if not (screen and trash and drive):
            fails.append("Astra's desktop never said where its Trash and Drive are")
            return guest.seen

        _, dh, tx, ty = nums(trash)
        _, _, vx, vy = nums(drive)
        ox, oy = 0, int(screen.group(2)) - dh

        if not (vx <= 4 and tx <= 4):
            fails.append("Astra's desktop is not in its own arrangement: Drive at %d,%d, "
                         "Trash at %d,%d" % (vx, vy, tx, ty))

        click, _ = pointer(guest, *nums(screen, 2))
        mark = len(guest.seen)
        # The Drive: Astra's Trash is in the corner the Deskbar is over.
        click(ox + vx + CELL_W // 2, oy + vy + 30, twice=True)
        win = line(guest, WINDOW % "Tracker", mark, 30)
        time.sleep(2)
        guest._read_available()

        if not win or "opened spatially" in guest.seen[mark:]:
            fails.append("under Astra the Drive did not open in an ordinary window: "
                         "window %s, spatial %s"
                         % (bool(win), "opened spatially" in guest.seen[mark:]))

        return guest.seen
    finally:
        guest.close()


def main():
    fails, seen = [], {}

    def run(name, f):
        try:
            seen[name] = f(fails)
        except Exception as e:                    # noqa: BLE001
            fails.append("%s: %s" % (name, e))

    jobs = [threading.Thread(target=run, args=("pewter", pewter)),
            threading.Thread(target=run, args=("astra", astra))]
    for j in jobs:
        j.start()
    for j in jobs:
        j.join()

    for name, text in seen.items():
        if text and " died: " in text:
            fails.append("%s: something died: %s"
                         % (name, text[text.find(" died: ") - 80:][:300]))

    if fails:
        print("FAIL: %d of %d checks on Pewter's desktop and spatial folders:"
              % (len(fails), CHECKS))
        for f in fails:
            print("  " + f)
        for name, text in seen.items():
            for l in (text or "").splitlines():
                if re.search(r"tracker:|wm: (window|menu of|closed|moved)|Trash|died", l):
                    print("  %s | %s" % (name, l.strip()[:200]))
        return 1

    print("PASS: %d checks on Pewter's desktop and spatial folders (Mac OS's arrangement, "
          "the Drive top right and the Trash bottom right; the Trash opened as a spatial "
          "folder with no sidebar, one window when opened twice, moved, resized and made a "
          "list with each kept on the folder, closed and opened again in the same place, "
          "size and view; Astra's own arrangement and an ordinary window)" % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
