#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Desktops (`docs/pewter.md`): the desktop chosen, its loader, Preferences'
Desktop row and Switch Desktop.

A desktop is a file in `/Kosmos/Desktops`, and `wm` named nothing starts the
one the `desktop` setting names - Astra on a machine that never chose - with
the chooser's loader over it and no cards: what the machine does at every
boot since 8 November 2026 (Diego: "make sure we remove the desktop selector
upon booting, i want it to be a setting in preferences from now on"). Through
the prompt, a Startup page that ticks Tracker and a program that does not
exist; then `wm`, and:

  - Astra starts with nothing pressed: the window manager starts what its
    file names and then the startup items, and the loader is posted a row
    for each: the shell's pieces ready, Tracker ready, and the program that
    does not exist did not start, counted as failed rather than ready, with
    the desktop opened anyway;
  - the loader goes once everything has settled, and the Deskbar is up;
  - Preferences' Desktop row, pressed and Pewter chosen: every window
    closed, and Pewter started with the loader, no cards;
  - Switch Desktop in Pewter's menu bar's Kosmos menu shows the cards, with
    Pewter chosen last: the choice kept for the next boot.

The control is the failure: a row that could not start is told as one,
which a loader that took every arrival for success would not.

Usage: run_desktops.py IMAGE
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                  # noqa: E402

CHECKS = 15

# The Startup page's setting, as `startup.lua` reads it, written at the
# prompt - where there is no `use`, so straight to its file.
STARTUP = ('print("STARTUP", fs.write("/Home/Preferences/startup", '
           '{ items = { "tracker", "nosuchprogram" } }))')


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    guest = R.Guest(image, 120)
    fails = []

    def line(pattern, mark, seconds=60):
        end = time.time() + seconds
        while time.time() < end:
            guest._read_available()
            m = re.search(pattern, guest.seen[mark:])
            if m:
                return m
            time.sleep(0.2)
        return None

    def click(x, y, width, height):
        guest.mouse_to(*R._to_tablet(x, y, width, height))
        time.sleep(0.4)
        guest.mouse_button(True)
        time.sleep(0.1)
        guest.mouse_button(False)

    try:
        guest.wait_for_line("kosmos> ", "the prompt")
        mark = len(guest.seen)
        guest.type(STARTUP)
        written = line(r"STARTUP\s+(\S+)", mark, 20)

        if not (written and written.group(1) == "true"):
            fails.append("the Startup setting was not written: %r"
                         % (written and written.group(0)))

        # ---- a boot: Astra, as the setting says, and its loader ----
        mark = len(guest.seen)
        guest.type("wm")
        starting = line(r"chooser: starting (\w+), as the setting says", mark)
        drawn = line(r"wm: Desktops drawn", mark)
        screen = line(r"wm: the window server answers, the screen (\d+)x(\d+)", mark)
        width, height = (int(screen.group(1)), int(screen.group(2))) if screen else (1920, 1080)

        if not (starting and starting.group(1) == "Astra"):
            fails.append("a bare wm did not start Astra without asking: %r"
                         % (starting and starting.group(0)))
        if not drawn:
            fails.append("the loader's window was never drawn")

        chosen = line(r"wm: desktop Astra chosen", mark)
        loaded = line(r"wm: desktop Astra loaded, (\d+) started, (\d+) failed", mark, 90)
        up = line(r"chooser: Astra is up", mark, 30)
        seen = guest.seen[mark:]

        if not chosen:
            fails.append("Astra was not started")
        for piece in ("Notifications", "Deskbar", "Tracker \\(desktop\\)", "Tracker"):
            if not re.search(r"chooser: heard %s (ready|ended)" % piece, seen):
                fails.append("the loader never heard %s arrive" % piece.replace("\\", ""))
        if not re.search(r"chooser: heard nosuchprogram failed", seen):
            fails.append("the program that does not exist was not told as failed")
        if not (loaded and int(loaded.group(2)) == 1 and int(loaded.group(1)) >= 5):
            fails.append("the loading did not end with one failed and the rest started: %r"
                         % (loaded and loaded.group(0)))
        if not up:
            fails.append("the loader did not go once everything had settled")
        if not re.search(r"wm: window Deskbar at 0,0 ", seen):
            fails.append("Astra's Deskbar never came up")

        # ---- Preferences' Desktop row: Pewter, switched to at once ----
        time.sleep(2)
        # Opened as a person opens it, since the keyboard is the desktop's
        # and not the prompt's: Super+Space, its name, Return; then the
        # wheel down the Appearance page to the Desktop row, its last.
        mark = len(guest.seen)

        def keys(down, *names):
            guest._qmp("input-send-event", {"events": [
                {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": n}}}
                for n in names]})
            time.sleep(0.25)

        keys(True, "meta_l", "spc")
        keys(False, "spc", "meta_l")
        line(r"launchpad: ", mark, 20)
        time.sleep(1.0)
        for ch in "preferences":
            guest.sendkey(ch)
            time.sleep(0.05)
        time.sleep(0.5)
        guest.sendkey("ret")
        window = line(r"wm: window Preferences at (\d+),(\d+) (\d+)x(\d+)", mark, 30)
        line(r"preferences: \d+x\d+, ", mark, 30)
        time.sleep(1.5)
        places, menu = [], None

        if window:
            wx, wy = int(window.group(1)), int(window.group(2))
            ww, wh = int(window.group(3)), int(window.group(4))
            # The scrollbar's thumb dragged to the foot: the wheel stops where
            # a control under the pointer takes it for itself.
            thumb = re.findall(r"preferences scrollbar: thumb at (\d+),(\d+)", guest.seen[mark:])

            if thumb:
                tx, ty = (int(v) for v in thumb[-1])
                guest.mouse_to(*R._to_tablet(wx + tx, wy + ty, width, height))
                time.sleep(0.4)
                guest.mouse_button(True)
                for k in range(1, 9):
                    guest.mouse_to(*R._to_tablet(wx + tx, wy + ty + (wh - 20 - ty) * k // 8,
                                                 width, height))
                    time.sleep(0.15)
                guest.mouse_button(False)

            time.sleep(1.0)
            guest._read_available()
            places = re.findall(r"preferences desktop: (\w+) at (\d+),(\d+)", guest.seen[mark:])

        if window and places and 0 < int(places[-1][2]) < wh:
            _, cx, cy = places[-1]
            before = len(guest.seen)
            click(wx + int(cx), wy + int(cy), width, height)
            menu = line(r"wm: menu of Preferences at (\d+),(\d+)", before, 20)

            if menu:
                time.sleep(1.0)
                # The second choice: Astra first, then Pewter.
                click(int(menu.group(1)) + 20, R.menu_row_middle(int(menu.group(2)), 2),
                      width, height)

        switched = line(r"wm: switching desktop to Pewter", mark, 20)
        straight = line(r"chooser: starting Pewter, as the setting says", mark, 60)
        pewter = line(r"wm: desktop Pewter loaded, (\d+) started, (\d+) failed", mark, 90)
        line(r"chooser: Pewter is up", mark, 30)

        if not (places and places[-1][0] == "Astra" and menu):
            fails.append("Preferences' Desktop row was not there saying Astra, or did not "
                         "open its choices: %r, menu %s; after the command:\n%s"
                         % (places, bool(menu), guest.seen[mark:][:2500]))
        if not (switched and straight and pewter):
            fails.append("choosing Pewter in Preferences did not switch to it with no cards: "
                         "switch %s, started %s, loaded %s"
                         % (bool(switched), bool(straight), bool(pewter)))

        # Pewter's Kosmos menu is its menu bar's, at the same end of the
        # screen and the same menu (`kosmosmenu.lua`).
        mark = len(guest.seen)
        click(30, 12, width, height)
        menu = line(r"wm: menu of Menu bar at (\d+),(\d+) (\d+)x(\d+)", mark, 20)

        if menu:
            mx, my, mw, mh = (int(menu.group(k)) for k in range(1, 5))
            time.sleep(1.5)
            click(mx + mw // 2, my + mh - 92, width, height)

        empty = line(r"wm: the screen is empty, choosing again", mark, 30)
        kept = line(r"chooser: \d+ desktops?, (\w+) chosen last", mark, 60)

        if not (empty and kept and kept.group(1) == "Pewter"):
            fails.append("Switch Desktop did not show the cards with Pewter chosen last: "
                         "empty %s, %r" % (bool(empty), kept and kept.group(0)))
    finally:
        seen = guest.seen
        guest.close()

    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:300])

    if fails:
        print("FAIL: %d of %d checks on the desktops:" % (len(fails), CHECKS))
        for f in fails:
            print("  " + f)
        for l in seen.splitlines():
            if re.search(r"wm: (desktop|loading|switching|the screen is empty|window Preferences|menu of)"
                         r"|chooser:|preferences|lua|error|died", l):
                print("  | " + l.strip()[:200])
        return 1

    print("PASS: %d checks on the desktops (a bare wm starting Astra with no cards, the "
          "loader hearing each piece and Tracker arrive and a program that does not exist "
          "told as failed, the desktop opened anyway and the loader gone; Pewter chosen in "
          "Preferences and switched to at once with no cards; Switch Desktop showing the "
          "cards with Pewter kept)" % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
