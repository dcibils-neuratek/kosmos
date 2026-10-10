#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The app switcher, Super+Tab, and Alt+Tab (`docs/switcher.html`).

A desktop with Calculator and Preferences opened after it came up, so the
applications last used are Preferences, then Calculator, then what the
Deskbar started. Keys go in as QEMU's key events - Super held while Tab is
pressed and let go, and Super let go after - which is how a keyboard sends
them:

  - Super+Tab shows the panel, Calculator lit (the second: "back to what I
    was just using"), and it is on the screen;
  - Tab moves on, Shift+Tab back, the down arrow opens Calculator's windows
    and the up arrow closes them;
  - Super let go brings the lit application forward: Calculator has the keys;
  - Esc puts it away and nothing changes;
  - a click on an icon chooses it;
  - Q quits the lit application;
  - Alt+Tab steps the windows with nothing shown.

Usage: run_switcher.py IMAGE
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

LAUNCH = ('local r = fs.send("/Running/wm", { type = "launch", program = args })\n'
          'print("LAUNCH " .. tostring(r and r.ok))\n')
FOCUS = ('local wm = use("/Kosmos/Libraries/wmproto.lua")\n'
         'for _, w in ipairs((wm.windows() or {}).windows or {}) do\n'
         '  if w.focused then print("FOCUS " .. tostring(w.title)) end\n'
         'end\n')


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web)
    fails, said = [], {}

    def keys(down, *names):
        guest._qmp("input-send-event", {"events": [
            {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": n}}}
            for n in names]})
        time.sleep(0.25)

    def tap(*names):
        keys(True, *names)
        keys(False, *reversed(names))

    def focus(session):
        out = session.run("/Temporary/focus.lua").decode(errors="replace")
        m = re.search(r"FOCUS ([^\r\n]*)", out)
        return m.group(1) if m else None

    def wait_line(pattern, mark, seconds=10):
        end = time.time() + seconds
        while time.time() < end:
            guest._read_available()
            m = re.search(pattern, guest.seen[mark:])
            if m:
                return m
            time.sleep(0.2)
        return None

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)
        session.put(LAUNCH.encode(), "/Temporary/launch.lua")
        session.put(FOCUS.encode(), "/Temporary/focus.lua")
        time.sleep(4)

        for program in ("/Kosmos/Apps/calc.lua", "/Kosmos/Apps/preferences.lua"):
            mark = len(guest.seen)
            session.run("/Temporary/launch.lua " + program)
            guest.wait_for_line("wm: window ", "its window", mark)
            time.sleep(2)

        said["before"] = focus(session)

        # Super+Tab: the panel, Calculator lit.
        mark = len(guest.seen)
        keys(True, "meta_l")
        tap("tab")
        opened = wait_line(r"wm: switcher at (\d+),(\d+) (\d+)x(\d+), icons at ([\d/]+),(\d+)", mark)
        first = wait_line(r"wm: switcher ([^\r\n]*\[[^\r\n]*)", mark)

        if not opened or not first:
            fails.append("Super+Tab showed no switcher")
        else:
            said["first"] = first.group(1)
            if "[Calculator]" not in first.group(1) or not first.group(1).startswith("Preferences"):
                fails.append("the panel did not list Preferences first with Calculator lit: %r"
                             % first.group(1))

            # On the screen: the panel's dark in its middle.
            x, y, w, h = (int(v) for v in opened.groups()[:4])
            time.sleep(1)
            width, height, px = R.parse_ppm(guest.screendump())
            cx, cy = x + w // 2, y + 8
            at = (cy * width + cx) * 3
            r, g, b = px[at], px[at + 1], px[at + 2]
            if not (r < 0x50 and g < 0x50 and b < 0x58):
                fails.append("the panel is not on the screen at %d,%d: %02x%02x%02x" % (cx, cy, r, g, b))

        # Tab on, Shift+Tab back, the windows row and out of it.
        mark = len(guest.seen)
        tap("tab")
        on = wait_line(r"wm: switcher ([^\r\n]*)", mark)
        mark = len(guest.seen)
        tap("shift", "tab")
        back = wait_line(r"wm: switcher ([^\r\n]*)", mark)
        mark = len(guest.seen)
        tap("down")
        row = wait_line(r"wm: switcher ([^\r\n]*window 1 of[^\r\n]*)", mark)
        mark = len(guest.seen)
        tap("up")
        wait_line(r"wm: switcher ([^\r\n]*)", mark)

        if not on or "[Calculator]" in on.group(1):
            fails.append("Tab did not move on: %r" % (on and on.group(1)))
        if not back or "[Calculator]" not in back.group(1):
            fails.append("Shift+Tab did not come back to Calculator: %r" % (back and back.group(1)))
        if not row:
            fails.append("the down arrow did not open Calculator's windows")

        # Super let go: Calculator forward, with the keys.
        mark = len(guest.seen)
        keys(False, "meta_l")
        chose = wait_line(r"wm: switcher chose ([^\r\n]*)", mark)
        time.sleep(1)
        said["after"] = focus(session)

        if not chose or chose.group(1).strip() != "Calculator":
            fails.append("letting go of Super chose %r" % (chose and chose.group(1)))
        if not (said["after"] or "").startswith("Calculator"):
            fails.append("Calculator did not get the keys: %r" % said["after"])

        # Esc: put away, nothing changed.
        mark = len(guest.seen)
        keys(True, "meta_l")
        tap("tab")
        tap("esc")
        keys(False, "meta_l")
        away = wait_line(r"wm: switcher put away", mark)
        time.sleep(1)
        if not away or "chose" in guest.seen[mark:] or not (focus(session) or "").startswith("Calculator"):
            fails.append("Esc did not put it away leaving Calculator in front")

        # A click on the second icon chooses the application listed second.
        mark = len(guest.seen)
        keys(True, "meta_l")
        tap("tab")
        opened = wait_line(r"wm: switcher at \d+,\d+ \d+x\d+, icons at ([\d/]+),(\d+)", mark)
        listed = wait_line(r"wm: switcher ([^\r\n]*\[[^\r\n]*)", mark)
        second = listed and listed.group(1).split(", ")[1].strip("[]")
        if opened:
            xs = [int(v) for v in opened.group(1).split("/")]
            iy = int(opened.group(2))
            width, height, _ = R.parse_ppm(guest.screendump())
            guest.mouse_to(*R._to_tablet(xs[1], iy, width, height))
            time.sleep(0.5)
            guest.mouse_button(True)
            time.sleep(0.1)
            guest.mouse_button(False)
        clicked = wait_line(r"wm: switcher chose ([^\r\n]*)", mark)
        keys(False, "meta_l")
        if not clicked or clicked.group(1).strip() != second:
            fails.append("a click on the second icon, %r, chose %r"
                         % (second, clicked and clicked.group(1)))
        else:
            said["clicked"] = clicked.group(1).strip()

        # Q quits the lit one: Calculator is in front, so Preferences is lit.
        mark = len(guest.seen)
        keys(True, "meta_l")
        tap("tab")
        lit = wait_line(r"wm: switcher ([^\r\n]*\[[^\r\n]*)", mark)
        tap("q")
        quit_ = wait_line(r"wm: switcher quit ([^\r\n]*)", mark)
        keys(False, "meta_l")
        gone = wait_line(r"process \d+ \((preferences|calc)\) ended", mark, 15)
        if not quit_ or not gone:
            fails.append("Q did not quit the lit application: %r, %r"
                         % (lit and lit.group(1), quit_ and quit_.group(1)))
        else:
            said["quit"] = quit_.group(1).strip()

        # Alt+Tab: the windows stepped, nothing shown.
        before = focus(session)
        mark = len(guest.seen)
        keys(True, "alt")
        tap("tab")
        keys(False, "alt")
        stepped = wait_line(r"wm: Alt\+Tab", mark)
        time.sleep(1)
        after = focus(session)
        if not stepped or after == before or "wm: switcher" in guest.seen[mark:]:
            fails.append("Alt+Tab did not step the windows quietly: %r -> %r" % (before, after))
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        guest.close()

    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:300])

    if fails:
        print("FAIL: %d of 11 checks on the app switcher:" % len(fails))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: 11 checks on the app switcher (Super+Tab: %s, on the screen; Tab on, Shift+Tab "
          "back, the down arrow's windows and up again; Super let go, and Calculator has the "
          "keys; Esc changing nothing; a click choosing %s; Q quitting %s; Alt+Tab stepping "
          "the windows with nothing shown)" % (said.get("first"), said.get("clicked"),
                                              said.get("quit")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
