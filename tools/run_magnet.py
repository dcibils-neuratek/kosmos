#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Magnet, by dragging (`docs/magnet.html`).

Diego, 10 October 2026: "dragging a window to the right of the screen will
show that it can be docked and occupy that half of the screen", "dragging a
window to the top of the monitor will allow you to maximise it". Tracker,
dragged by its header:

  - to the right edge: the outline shown there, and on letting go the right
    half of the room, a gap of 6 round it;
  - away again: its own size back;
  - to the top: the whole room;
  - away, and to the bottom left corner: a quarter.

Usage: run_magnet.py IMAGE
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

CHECKS = 8
GAP = 6

OPEN = ('local r = fs.send("/Running/wm", { type = "launch", program = "tracker", args = "/Home" })\n'
        'print("OPENED " .. tostring(r and r.ok))\n')
# What the Magnet page sends when "Snap when dragged" is turned off, and the
# file it is kept in, read back.
OFF = ('local r = fs.send("/Running/wm", { type = "magnet", set = { drag = false } })\n'
       'local kept = use("/Kosmos/Libraries/prefs.lua").read("magnet") or {}\n'
       'print("OFF " .. tostring(r and r.ok) .. " kept " .. tostring(kept.drag))\n')
PAGE = ('local r = fs.send("/Running/wm", { type = "launch", program = "preferences", args = "magnet" })\n'
        'print("PAGE " .. tostring(r and r.ok))\n')


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web)
    fails = []

    def check(ok, complaint):
        if not ok:
            fails.append(complaint)

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
        session.put(OPEN.encode(), "/Temporary/open.lua")
        session.put(OFF.encode(), "/Temporary/off.lua")
        session.put(PAGE.encode(), "/Temporary/page.lua")
        time.sleep(1)
        mark = len(guest.seen)
        session.run("/Temporary/open.lua")

        win = line(r"wm: window Tracker at (\d+),(\d+) (\d+)x(\d+)", mark, 30)
        band = line(r"tracker: band at (\d+),(\d+)", mark, 30)

        if not win or not band:
            raise RuntimeError("Tracker never said where it is")

        x, y, w0, h0 = (int(v) for v in win.groups())
        bx, by = int(band.group(1)), int(band.group(2))
        time.sleep(2)
        sw, sh, _ = R.pixel_reader(guest.screendump())

        def drag(fx, fy, tx, ty, hold=0.6):
            guest.mouse_to(*R._to_tablet(fx, fy, sw, sh))
            time.sleep(0.2)
            guest.mouse_button(True)
            time.sleep(0.3)
            for i in range(1, 13):
                guest.mouse_to(*R._to_tablet(fx + (tx - fx) * i // 12, fy + (ty - fy) * i // 12, sw, sh))
                time.sleep(0.06)
            time.sleep(hold)
            guest.mouse_button(False)
            time.sleep(1.0)

        # To the right edge.
        mark = len(guest.seen)
        drag(x + bx, y + by, sw - 1, y + by)
        shown = line(r"wm: magnet shows right\b", mark, 5)
        put = line(r"wm: magnet put Tracker at the right, (\d+),(\d+) (\d+)x(\d+)", mark, 10)
        check(shown is not None, "the right half's outline was not shown at the right edge")
        lw = (sw - 3 * GAP) // 2

        if put:
            px, py, pw, ph = (int(v) for v in put.groups())
            check(px == 2 * GAP + lw and pw == sw - 3 * GAP - lw and px + pw == sw - GAP,
                  "the right half is not the screen's right half less the gaps: %d,%d %dx%d"
                  % (px, py, pw, ph))
        else:
            check(False, "let go at the right edge, Tracker was not put in the right half")
            px, py, pw = x, y, w0

        # Away again, by its header where it is now: its size back.
        mark = len(guest.seen)
        drag(px + bx, py + by, sw // 2, sh // 2)
        back = line(r"wm: magnet gave Tracker back its size, (\d+)x(\d+)", mark, 10)
        check(back is not None and int(back.group(1)) == w0 and int(back.group(2)) == h0,
              "dragged away, Tracker did not have its size back: %r" % (back and back.group(0)))
        moved = line(r"wm: moved Tracker by its header to (\d+),(\d+)", mark, 5)
        mx, my = (int(v) for v in moved.groups()) if moved else (sw // 2 - bx, sh // 2 - by)

        # To the top: the whole room.
        mark = len(guest.seen)
        drag(mx + bx, my + by, sw // 2, 0)
        check(line(r"wm: magnet put Tracker in the whole room", mark, 10) is not None,
              "let go at the top edge, Tracker was not given the whole room")
        at = line(r"wm: moved Tracker by its header to (\d+),(\d+)", mark, 5)
        wx, wy = (int(v) for v in at.groups()) if at else (0, 32)

        # Away, and to the bottom left corner: a quarter.
        time.sleep(0.5)
        mark = len(guest.seen)
        drag(wx + sw // 2, wy + by, sw // 2 + 40, sh // 2)
        moved = line(r"wm: moved Tracker by its header to (\d+),(\d+)", mark, 5)
        mx, my = (int(v) for v in moved.groups()) if moved else (sw // 2, sh // 2)
        mark = len(guest.seen)
        drag(mx + bx, my + by, 0, sh - 1)
        quarter = line(r"wm: magnet put Tracker at the bottom left, (\d+),(\d+) (\d+)x(\d+)", mark, 10)
        check(quarter is not None and int(quarter.group(1)) == GAP and int(quarter.group(3)) == lw,
              "let go in the bottom left corner, Tracker was not given that quarter: %r"
              % (quarter and quarter.group(0)))

        # "Snap when dragged" off, as the Magnet page turns it off: kept, and
        # a window dropped at the right edge stays where it was dropped.
        off = session.run("/Temporary/off.lua").decode(errors="replace")
        check("OFF true kept false" in off, "turning dragging off was not done and kept: %r"
              % off.strip()[-80:])
        moved = line(r"wm: moved Tracker by its header to (\d+),(\d+)", 0, 1)
        mark = len(guest.seen)
        drag(6 + bx, int(quarter.group(2)) + by if quarter else sh // 2, sw - 1, sh // 2)
        time.sleep(1)
        guest._read_available()
        snapped = re.findall(r"wm: magnet (?:shows|put)[^\r\n]*", guest.seen[mark:])
        check(not snapped, "with dragging off, a drop at the edge still snapped: %r" % snapped)

        # The Magnet page opens, its rows built.
        mark = len(guest.seen)
        session.run("/Temporary/page.lua")
        opened = line(r"wm: window Preferences at", mark, 30)
        time.sleep(2)
        guest._read_available()
        check(opened is not None and "traceback" not in guest.seen[mark:]
              and "attempt to" not in guest.seen[mark:],
              "the Magnet page did not open cleanly: %r" % guest.seen[mark:][-300:])
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        try:
            with open(os.path.join("build", "gate", "x86-magnet.ppm"), "wb") as f:
                f.write(guest.screendump())
        except Exception:                   # noqa: BLE001 - a picture is extra
            pass
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on Magnet:" % (len(fails), CHECKS))
        for f in fails:
            print("  " + f)
        for l in seen.splitlines():
            if "magnet" in l or "Tracker" in l or "error" in l.lower():
                print("  | " + l.strip()[:200])
        return 1

    print("PASS: %d checks on Magnet (the right edge's outline and the right half, the gaps to "
          "the pixel; dragged away, its size back; the top edge, the whole room; a corner, a "
          "quarter; dragging turned off and kept, and nothing snapping; the Magnet page)" % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
