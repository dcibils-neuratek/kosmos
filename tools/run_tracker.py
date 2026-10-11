#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Tracker's rows opened by a double click, and only by one.

Diego, on the M700, 11 October 2026: "if i double click to open a folder,
and in the next instant, i single click one of its inner folders or files it
counts it as a double click opening the folder or file". A row opened on any
click on the row already chosen, however long after, and a folder just
opened has one chosen. A desktop, `/Home/Outer` holding `Inner` holding
`Deep`, Tracker opened on Outer, and the pointer:

  - a double click on Inner opens it;
  - one click on Deep straight after only chooses it;
  - a second click more than the double-click span later does not open it;
  - a double click on Deep opens it.

Usage: run_tracker.py IMAGE
"""

import os
import random
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                  # noqa: E402
import run_servers as S                                     # noqa: E402

CHECKS = 4

MAKE = ('local files = use("/Kosmos/Libraries/files.lua")\n'
        'print("MADE", files.make_folder("/Home/Outer"), files.make_folder("/Home/Outer/Inner"),\n'
        '      files.make_folder("/Home/Outer/Inner/Deep"))\n'
        'local r = fs.send("/Running/wm", { type = "launch", program = "tracker", args = "/Home/Outer" })\n'
        'print("LAUNCH " .. tostring(r and r.ok))\n')


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web)
    fails = []

    def line(pattern, mark, seconds=15):
        end = time.time() + seconds
        while time.time() < end:
            guest._read_available()
            m = re.search(pattern, guest.seen[mark:])
            if m:
                return m
            time.sleep(0.1)
        return None

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)
        session.put(MAKE.encode(), "/Temporary/make.lua")
        time.sleep(2)
        mark = len(guest.seen)
        session.run("/Temporary/make.lua")
        outer = line(r"tracker: rows of /Home/Outer at (\d+),(\d+), (\d+) apart", mark, 30)
        if not outer:
            raise RuntimeError("Tracker never showed /Home/Outer")
        time.sleep(1.5)
        width, height, _ = R.parse_ppm(guest.screendump())

        def press(x, y):
            guest.mouse_to(*R._to_tablet(x, y, width, height))
            time.sleep(0.05)
            guest.mouse_button(True)
            time.sleep(0.05)
            guest.mouse_button(False)

        def double(x, y):
            press(x, y)
            time.sleep(0.12)
            press(x, y)

        x, y = int(outer.group(1)), int(outer.group(2))

        # A double click on Inner: it opens.
        mark = len(guest.seen)
        double(x, y)
        inner = line(r"tracker: rows of /Home/Outer/Inner at (\d+),(\d+)", mark)
        if not inner:
            fails.append("a double click on Inner did not open it")
            raise RuntimeError("nothing to go on with")
        x, y = int(inner.group(1)), int(inner.group(2))

        # One click on Deep at once: chosen, not opened.
        mark = len(guest.seen)
        press(x, y)
        said = line(r"tracker: (click|double click) on row 1", mark)
        time.sleep(2)
        guest._read_available()
        if not said or said.group(1) != "click" or "/Home/Outer/Inner/Deep" in guest.seen[mark:]:
            fails.append("one click in a folder just opened opened its row: %r"
                         % (said and said.group(0)))

        # Another, well past the span: still not a double click.
        mark = len(guest.seen)
        press(x, y)
        time.sleep(2)
        guest._read_available()
        if "/Home/Outer/Inner/Deep" in guest.seen[mark:]:
            fails.append("two clicks two seconds apart opened the row")

        # And a double click on Deep: it opens.
        time.sleep(1.2)
        mark = len(guest.seen)
        double(x, y)
        if not line(r"tracker: rows of /Home/Outer/Inner/Deep at", mark):
            fails.append("a double click on Deep did not open it")
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on Tracker's double click:" % (len(fails), CHECKS))
        for f in fails:
            print("  " + f)
        for l in seen.splitlines():
            if l.startswith("tracker: rows") or "click on row" in l:
                print("  | " + l.strip()[:160])
        return 1

    print("PASS: %d checks on Tracker's double click (Inner opened by one; one click in it "
          "straight after only choosing Deep; two clicks two seconds apart not opening it; "
          "a double click on Deep opening it)" % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
