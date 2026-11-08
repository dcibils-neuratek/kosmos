#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Menus at the top of the screen (`docs/pewter.md` P3), in two machines
side by side.

Pewter, with the PDF viewer - a window that draws its own pixels, its menu
bar File, Edit, View, Go - and the Text Editor, a kit window whose menu is
the dots in its header:

  - the desktop says its menus go on the screen, and the menu bar is up;
  - both windows handed their titles to the window manager - the Text
    Editor's dots by the application's name - and the PDF viewer drew none
    in its own header;
  - the bar follows the front: the PDF viewer clicked, its four titles; the
    Text Editor clicked, its one;
  - View pressed in the bar: the PDF viewer opened its own View menu under
    it, at the top of the screen, and kept the front - its close box still
    drawn, which Platinum draws only on the window in front;
  - the Text Editor's title pressed and New window chosen from its menu:
    a second Text Editor;
  - the Kosmos menu, opened from the bar's left end.

Astra, with the PDF viewer alone:

  - the desktop says its menus stay in the window: nothing handed over, and
    no menu bar;
  - the PDF viewer's titles in its own header, and File pressed there opens
    its menu inside the window.

And the busiest path, measured on both: from the press on a title to the
menu drawn, as the kit says it (`window:open_menu`) - under Pewter from the
bar's press, so the two posts between them are in it. QEMU's numbers are
not performance numbers; both are printed, and each must be said.

The control is the window manager's post: with `menu_open` answering and
posting nothing, the bar's titles open nothing, which fails it.

Usage: run_menus.py IMAGE
"""

import os
import re
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                  # noqa: E402
from run_platinum import startup, line, pixels              # noqa: E402

CHECKS = 11

# Platinum's close box ring on a light look (`OUT.platinum_colours`), drawn
# only on the window in front.
RING = (0x7a, 0x7a, 0x80)

WINDOW = r"wm: window %s at (\d+),(\d+) (\d+)x(\d+)"
MENU = r"wm: menu of %s at (\d+),(\d+) (\d+)x(\d+)"
DRAWN = r"ui: %s's menu drawn ([\d.]+) ms after the press"

PDF = "PDF Viewer"
TE = "Untitled - Text Editor"


def boot(desktop, items, timeout=150):
    image = "build/x86_64/kosmos.elf" if len(sys.argv) < 2 else sys.argv[1]
    guest = R.Guest(image, timeout)
    guest.wait_for_line("kosmos> ", "the prompt")
    mark = len(guest.seen)
    guest.type(startup(items))
    guest.wait_for_line("STARTUP", "the Startup setting", mark)
    guest.type("wm %s.desktop" % desktop)
    return guest


def latest(guest, pattern, want, seconds=15):
    """The bar's last line of a kind, once it is the one wanted: what it
    shows now, whether or not the click that made it so was needed."""
    end = time.time() + seconds
    while time.time() < end:
        guest._read_available()
        found = list(re.finditer(pattern, guest.seen))
        if found and re.search(want, found[-1].group(0)):
            return re.search(want, found[-1].group(0))
        time.sleep(0.2)
    return None


FRONT = r"menubar: [^\r\n]* in front, its menus [^\r\n]*"
AT = r"menubar: at [^\r\n]*"


def rect(m):
    return tuple(int(m.group(k)) for k in range(1, 5))


def inside(x, y, r):
    return r[0] <= x < r[0] + r[2] and r[1] <= y < r[1] + r[3]


def clicker(guest, sw, sh):
    def click(x, y):
        guest.mouse_to(*R._to_tablet(x, y, sw, sh))
        time.sleep(0.4)
        guest.mouse_button(True)
        time.sleep(0.08)
        guest.mouse_button(False)
        time.sleep(0.1)
    return click


def a_point_of(own, over):
    """A point in `own`, below its header, that `over` does not cover."""
    for y in (own[1] + own[3] // 2, own[1] + own[3] - 30, own[1] + 80):
        for x in range(own[0] + 20, own[0] + own[2] - 20, 20):
            if not inside(x, y, over):
                return x, y
    return None


def pewter(fails, ms):
    guest = boot("Pewter", ["pdfview", "texteditor"])
    try:
        loaded = line(guest, r"wm: desktop Pewter loaded, (\d+) started, (\d+) failed", 0, 150)
        screen = line(guest, r"wm: the window server answers, the screen (\d+)x(\d+)", 0, 5)
        pdf = line(guest, WINDOW % re.escape(PDF), 0, 30)
        te = line(guest, WINDOW % re.escape(TE), 0, 30)
        handed = (line(guest, r"wm: PDF Viewer's menus: File, Edit, View, Go", 0, 30),
                  line(guest, r"wm: Untitled - Text Editor's menus: Text Editor\r?\n", 0, 30))
        time.sleep(3)
        guest._read_available()
        seen = guest.seen

        if not (loaded and int(loaded.group(2)) == 0
                and "wm: menus in the screen" in seen and "menubar: up" in seen):
            fails.append("Pewter did not come up with its menus on the screen and the bar: %r"
                         % (loaded and loaded.group(0)))
        if not all(handed):
            fails.append("the windows did not hand their titles over: PDF %s, Text Editor %s"
                         % (bool(handed[0]), bool(handed[1])))
        if "ui: PDF Viewer's menus in its header" in seen:
            fails.append("the PDF viewer drew its titles in its own header under Pewter")
        if not (screen and pdf and te):
            fails.append("the windows were never placed: PDF %s, Text Editor %s"
                         % (bool(pdf), bool(te)))
            return guest.seen

        sw, sh = int(screen.group(1)), int(screen.group(2))
        click = clicker(guest, sw, sh)
        pdf_r, te_r = rect(pdf), rect(te)
        pdf_at, te_at = a_point_of(pdf_r, te_r), a_point_of(te_r, pdf_r)

        # ---- the PDF viewer in front, and View from the bar ----
        mark = len(guest.seen)
        click(*pdf_at)
        front = latest(guest, FRONT, r"PDF Viewer in front, its menus File, Edit, View, Go$")
        where = latest(guest, AT, r"at File \d+,\d+, Edit \d+,\d+, View (\d+),(\d+)")

        if where:
            time.sleep(1)
            mark = len(guest.seen)
            click(int(where.group(1)), int(where.group(2)))
            opened = line(guest, r"ui: PDF Viewer's menu View opened from the menu bar", mark, 15)
            menu = line(guest, MENU % re.escape(PDF), mark, 15)
            drawn = line(guest, DRAWN % re.escape(PDF), mark, 15)

            if drawn:
                ms["pewter"] = float(drawn.group(1))
            if not (opened and menu and rect(menu)[1] <= 40):
                fails.append("View in the bar did not open the PDF viewer's menu under it: "
                             "opened %s, menu %r" % (bool(opened), menu and menu.group(0)))

            # Still in front, its boxes with it: the bar took no focus.
            time.sleep(1)
            _, _, at = pixels(guest.screendump())
            ring = any(at(x, y) == RING for x in range(pdf_r[0], pdf_r[0] + 48)
                       for y in range(pdf_r[1], pdf_r[1] + 40))

            if not ring:
                fails.append("the PDF viewer lost the front to the bar: no close box drawn "
                             "on it while its menu was open")

            click(*pdf_at)
            time.sleep(1)

        # ---- the Text Editor in front, and New window from its menu ----
        mark = len(guest.seen)
        click(*te_at)
        te_front = latest(guest, FRONT, r"Untitled - Text Editor in front, its menus Text Editor$")
        te_where = latest(guest, AT, r"at Text Editor (\d+),(\d+)")

        if not (front and where and te_front and te_where):
            fails.append("the bar did not follow the front: PDF %s at %s, Text Editor %s at %s"
                         % (bool(front), bool(where), bool(te_front), bool(te_where)))

        if te_where:
            time.sleep(1)
            mark = len(guest.seen)
            click(int(te_where.group(1)), int(te_where.group(2)))
            menu = line(guest, MENU % re.escape(TE), mark, 15)

            if menu:
                mx, my, mw, _ = rect(menu)
                time.sleep(1)
                mark2 = len(guest.seen)
                click(mx + mw // 2, my + 14)
                again = line(guest, WINDOW % re.escape(TE), mark2, 30)
            else:
                again = None

            if not (menu and again):
                fails.append("the Text Editor's menu from the bar did not open a new window: "
                             "menu %r, new window %s" % (menu and menu.group(0), bool(again)))

        # ---- the Kosmos menu ----
        time.sleep(1)
        kosmos = line(guest, r"menubar: at .*the Kosmos menu (\d+),(\d+)", 0, 5)
        mark = len(guest.seen)

        if kosmos:
            click(int(kosmos.group(1)), int(kosmos.group(2)))

        if not (kosmos and line(guest, MENU % "Menu bar", mark, 15)):
            fails.append("the Kosmos menu did not open from the bar")

        guest._read_available()
        return guest.seen
    finally:
        guest.close()


def astra(fails, ms):
    guest = boot("Astra", ["pdfview"])
    try:
        line(guest, r"wm: desktop Astra loaded", 0, 150)
        screen = line(guest, r"wm: the window server answers, the screen (\d+)x(\d+)", 0, 5)
        pdf = line(guest, WINDOW % re.escape(PDF), 0, 30)
        header = line(guest, r"ui: PDF Viewer's menus in its header: File (\d+),(\d+)", 0, 30)
        time.sleep(3)
        guest._read_available()
        seen = guest.seen

        if not ("wm: menus in the window" in seen and "menubar:" not in seen
                and not re.search(r"wm: .*'s menus: ", seen)):
            fails.append("Astra handed menus over or started a menu bar")

        if not (screen and pdf and header):
            fails.append("the PDF viewer's titles were not in its header under Astra")
            return guest.seen

        click = clicker(guest, int(screen.group(1)), int(screen.group(2)))
        r = rect(pdf)
        mark = len(guest.seen)
        click(r[0] + int(header.group(1)), r[1] + int(header.group(2)))
        menu = line(guest, MENU % re.escape(PDF), mark, 15)
        drawn = line(guest, DRAWN % re.escape(PDF), mark, 15)

        if drawn:
            ms["astra"] = float(drawn.group(1))
        if not (menu and rect(menu)[1] > r[1] + 10):
            fails.append("File in the PDF viewer's header did not open its menu in the window: %r"
                         % (menu and menu.group(0)))

        guest._read_available()
        return guest.seen
    finally:
        guest.close()


def main():
    fails, seen, ms = [], {}, {}

    def run(name, f):
        try:
            seen[name] = f(fails, ms)
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

    if not ("pewter" in ms and "astra" in ms):
        fails.append("the press to the menu drawn was not measured on both: %r" % ms)

    if fails:
        print("FAIL: %d of %d checks on menus at the top:" % (len(fails), CHECKS))
        for f in fails:
            print("  " + f)
        for name, text in seen.items():
            for l in (text or "").splitlines():
                if re.search(r"menubar:|wm: (menus|menu )|'s menus|menu drawn|opened from|died",
                             l):
                    print("  %s | %s" % (name, l.strip()[:200]))
        return 1

    print("PASS: %d checks on menus at the top (Pewter: the PDF viewer's and the Text "
          "Editor's titles handed over and none in the window, the bar following the front, "
          "View opened from the bar under it with the front kept, New window chosen from the Text Editor's menu "
          "there, the Kosmos menu; Astra: nothing handed over, the titles in the window; "
          "press to menu drawn %.1f ms under Pewter, %.1f ms under Astra, QEMU's)"
          % (CHECKS, ms["pewter"], ms["astra"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
