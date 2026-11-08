#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Desktops (`docs/pewter.md` P1): the chooser, its loader, Switch Desktop.

A desktop is a file in `/Kosmos/Desktops`, and `wm` named nothing starts
the chooser rather than a desktop - which is what the machine does at every
boot. Through the prompt, a Startup page that ticks Tracker and a program
that does not exist; then `wm`, and:

  - the chooser lists both desktops, Astra chosen last on a machine that
    never chose;
  - Return starts Astra: the window manager starts what its file names and
    then the startup items, and the chooser - kept over the screen as the
    loader - is posted a row for each: the shell's pieces ready, Tracker
    ready, and the program that does not exist did not start, counted as
    failed rather than ready, with the desktop opened anyway;
  - the loader goes once everything has settled, and the Deskbar is up;
  - Switch Desktop in the Deskbar's menu, clicked: every window closed, the
    chooser again on an empty screen;
  - Right and Return start Pewter, and the chooser after it says Pewter was
    chosen last - the choice kept.

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

CHECKS = 13

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

        # ---- the chooser, as a boot starts it ----
        mark = len(guest.seen)
        guest.type("wm")
        listed = line(r"chooser: (\d+) desktops?, (\w+) chosen last", mark)
        drawn = line(r"wm: Desktops drawn", mark)
        screen = line(r"wm: the window server answers, the screen (\d+)x(\d+)", mark)
        width, height = (int(screen.group(1)), int(screen.group(2))) if screen else (1920, 1080)

        if not (listed and int(listed.group(1)) >= 2 and listed.group(2) == "Astra"):
            fails.append("the chooser did not list the desktops with Astra last: %r"
                         % (listed and listed.group(0)))
        if not drawn:
            fails.append("the chooser's window was never drawn")

        # ---- Astra, and its loader ----
        time.sleep(1.5)
        mark = len(guest.seen)
        guest.sendkey("ret")
        chosen = line(r"wm: desktop Astra chosen", mark)
        loaded = line(r"wm: desktop Astra loaded, (\d+) started, (\d+) failed", mark, 90)
        up = line(r"chooser: Astra is up", mark, 30)
        seen = guest.seen[mark:]

        if not chosen:
            fails.append("Return on Astra did not start it")
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

        # ---- Switch Desktop, from the Deskbar's menu ----
        time.sleep(2)
        mark = len(guest.seen)
        click(50, 16, width, height)
        menu = line(r"wm: menu of Deskbar at (\d+),(\d+) (\d+)x(\d+)", mark, 20)

        if menu:
            mx, my, mw, mh = (int(menu.group(k)) for k in range(1, 5))
            time.sleep(1.5)
            # Third from the bottom: Switch Desktop..., Restart, Shut Down.
            click(mx + mw // 2, my + mh - 92, width, height)

        switched = line(r"wm: switching desktop", mark, 20)
        empty = line(r"wm: the screen is empty, choosing again", mark, 30)
        again = line(r"chooser: \d+ desktops?, (\w+) chosen last", mark, 60)

        if not (menu and switched and empty and again):
            fails.append("Switch Desktop did not empty the screen and show the chooser: "
                         "menu %s, switch %s, empty %s, chooser %s"
                         % (bool(menu), bool(switched), bool(empty), bool(again)))

        # ---- Pewter, and the choice kept ----
        time.sleep(1.5)
        mark = len(guest.seen)
        guest.sendkey("right")
        time.sleep(0.5)
        guest.sendkey("ret")
        pewter = line(r"wm: desktop Pewter loaded, (\d+) started, (\d+) failed", mark, 90)
        line(r"chooser: Pewter is up", mark, 30)

        if not pewter:
            fails.append("Right and Return did not start Pewter")

        mark = len(guest.seen)
        click(50, 16, width, height)
        menu = line(r"wm: menu of Deskbar at (\d+),(\d+) (\d+)x(\d+)", mark, 20)

        if menu:
            mx, my, mw, mh = (int(menu.group(k)) for k in range(1, 5))
            time.sleep(1.5)
            click(mx + mw // 2, my + mh - 92, width, height)

        kept = line(r"chooser: \d+ desktops?, (\w+) chosen last", mark, 60)

        if not (kept and kept.group(1) == "Pewter"):
            fails.append("the chooser after Pewter did not have it chosen last: %r"
                         % (kept and kept.group(0)))
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
            if re.search(r"wm: (desktop|loading|switching|the screen is empty)|chooser:", l):
                print("  | " + l.strip()[:200])
        return 1

    print("PASS: %d checks on the desktops (the chooser at a bare wm, both desktops listed "
          "and Astra chosen last; Astra started from Return, the loader hearing each piece "
          "and Tracker arrive and a program that does not exist told as failed, the desktop "
          "opened anyway and the loader gone; Switch Desktop emptying the screen for the "
          "chooser; Pewter started and kept as the last choice)" % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
