#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Preferences' Mouse page (`roadmap.md` 6zi, `docs/preferences.html`):
the pointer's speed and the double click's.

Diego, 27 September 2026: "preferences app need a mouse setting panel
(pointer speed, mouse click speed)". Two boots on one disk:

  1. **With the tablet**, as every suite has it: the page opens on Mouse; the
     pointer's row says a tablet has no speed rather than offering a slider;
     the folder opens on a double click at the second the kit has always
     had; the double-click slider pressed at its fast end writes 200 ms,
     the window manager says so, and every window is told - the folder,
     which counts with the kit's span as every window does, no longer opens
     for two presses that the slow span took.
  2. **With a PS/2 mouse alone**, which is relative, as the M700's is: the
     speed written at the end of the first boot and the double click from
     its slider are applied when the desktop starts - a speed outliving a
     restart, which `pointer`'s never did - the page has a speed slider,
     and a speed asked of the window manager reaches the board.

Usage: run_mouse.py IMAGE
"""

import os
import random
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# A disk for /Home, so what the first boot writes is there for the second.
# Named before `run_screenshot` is imported, which reads KOSMOS_DISK once.
import scratch                                              # noqa: E402

_DISK = os.path.join(scratch.directory("mouse"), "disk.img")
subprocess.run([os.path.join(os.path.dirname(HERE), "build", "host", "lua"),
                os.path.join(HERE, "kfs.lua"), "create", _DISK, "32"],
               check=True, capture_output=True, cwd=os.path.dirname(HERE))
os.environ["KOSMOS_DISK"] = _DISK

import run_screenshot as R                                  # noqa: E402
import run_servers as S                                     # noqa: E402

PREFS = ('local r = fs.send("/Running/wm", { type = "launch", '
         'program = "/Kosmos/Apps/preferences.lua", args = "mouse" })\n'
         'print("PREFS " .. tostring(r and r.ok))\n')
# What the file keeps, and the speed the window manager answers with.
KEPT = ('local f = fs.read("/Home/Preferences/mouse")\n'
        'print(("KEPT %s %s"):format(tostring(type(f) == "table" and f.speed), '
        'tostring(type(f) == "table" and f.double_click_ms)))\n')
# The speed, as Preferences' slider would set it on a mouse: kept for the
# next boot.
KEEP_SPEED = ('local prefs = use("/Kosmos/Libraries/prefs.lua")\n'
              'local f = prefs.read("mouse") or {}\n'
              'f.speed = 60\n'
              'print("KEEP " .. tostring(prefs.write("mouse", f)))\n')
ASK = ('local r = fs.send("/Running/wm", { type = "mouse", speed = 48 })\n'
       'print(("ASKED %s %s %s"):format(tostring(r and r.ok), tostring(r and r.speed), '
       'tostring(sys.pointer_speed())))\n')


def first(image, fails, said):
    """The tablet: the page, the folder, the double click made fast."""
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web)

    size = {}

    def click(x, y):
        guest.mouse_to(*R._to_tablet(x, y, size["w"], size["h"]))
        time.sleep(0.3)
        guest.mouse_button(True)
        time.sleep(0.1)
        guest.mouse_button(False)

    # Two presses where the pointer is, this far apart: under the slow span
    # one double click, under the fast one two clicks.
    def twice(x, y, apart=0.45):
        click(x, y)
        time.sleep(apart)
        guest.mouse_button(True)
        time.sleep(0.05)
        guest.mouse_button(False)

    def found(pattern, since, seconds=30):
        deadline = time.time() + seconds

        while time.time() < deadline:
            guest._read_available()
            m = re.findall(pattern, guest.seen[since:])

            if m:
                return m[-1]

            time.sleep(0.3)

        return None

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)
        size["w"], size["h"] = R.parse_ppm(guest.screendump())[:2]

        for name, text in (("prefs", PREFS), ("kept", KEPT), ("keep", KEEP_SPEED)):
            session.put(text.encode(), "/Temporary/%s.lua" % name)

        mark = len(guest.seen)
        session.run("/Temporary/prefs.lua")
        win = found(r"wm: window Preferences at (\d+),(\d+)", mark)
        slider = found(r"preferences slider: double_click_ms at (\d+),(\d+), (\d+) wide", mark)
        folder = found(r"preferences folder at (\d+),(\d+)", mark)
        time.sleep(1)
        guest._read_available()
        said["speed slider on a tablet"] = "preferences slider: speed at" in guest.seen[mark:]

        if not (win and slider and folder):
            fails.append("the Mouse page did not open with its slider and folder: %r %r %r"
                         % (win, slider, folder))
            return

        wx, wy = int(win[0]), int(win[1])
        fx, fy = wx + int(folder[0]), wy + int(folder[1])

        # A double click at the second the kit has always had: opens.
        mark = len(guest.seen)
        twice(fx, fy)
        said["opened"] = found(r"preferences: the folder (opened)", mark, 10)

        # The slider at its fast end: 200 ms, written, and every window told.
        mark = len(guest.seen)
        click(wx + int(slider[0]) + int(slider[2]) - 2, wy + int(slider[1]))
        said["fast"] = found(r"preferences: double_click_ms (\d+)", mark, 10)
        said["told"] = found(r"wm: mouse (double click \d+ ms)", mark, 10)
        said["kept"] = session.run("/Temporary/kept.lua").decode(errors="replace")

        # The same two presses, which the slow span took as one double click
        # and the fast one takes as two: the folder stays open.
        time.sleep(1)
        mark = len(guest.seen)
        twice(fx, fy)
        time.sleep(3)
        guest._read_available()
        said["closed at the fast span"] = "the folder closed" in guest.seen[mark:]

        said["keep"] = session.run("/Temporary/keep.lua").decode(errors="replace")
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the first boot stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        said["first seen"] = guest.seen
        guest.close()


def second(image, fails, said):
    """A PS/2 mouse alone: both settings applied at the desktop's start."""
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    saved = list(R.X86_ARGS)
    i = R.X86_ARGS.index("virtio-tablet-pci")
    del R.X86_ARGS[i - 1:i + 1]

    try:
        guest = S.boot(image, telnet, web)
    finally:
        R.X86_ARGS[:] = saved

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        guest._read_available()
        m = re.findall(r"wm: mouse ([^\n]*)", guest.seen)
        said["at the start"] = m[0].strip() if m else None
        session = S.connect(telnet)

        for name, text in (("prefs", PREFS), ("ask", ASK)):
            session.put(text.encode(), "/Temporary/%s.lua" % name)

        mark = len(guest.seen)
        session.run("/Temporary/prefs.lua")
        deadline = time.time() + 30

        while time.time() < deadline and "preferences slider: speed at" not in guest.seen[mark:]:
            time.sleep(0.3)
            guest._read_available()

        said["speed slider on a mouse"] = "preferences slider: speed at" in guest.seen[mark:]
        said["asked"] = session.run("/Temporary/ask.lua").decode(errors="replace")
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the second boot stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        said["second seen"] = guest.seen
        guest.close()


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    fails, said = [], {}

    first(image, fails, said)
    second(image, fails, said)

    if said.get("speed slider on a tablet") is not False:
        fails.append("a tablet was offered a speed slider that moves nothing")
    if said.get("opened") != "opened":
        fails.append("a double click on the folder at the slow span did not open it")
    if said.get("fast") != "200" or said.get("told") != "double click 200 ms":
        fails.append("the slider's fast end was not written and told: %r, %r"
                     % (said.get("fast"), said.get("told")))
    if "KEPT nil 200" not in said.get("kept", ""):
        fails.append("the double click was not kept in /Home/Preferences/mouse: %r"
                     % said.get("kept", "")[-200:])
    if said.get("closed at the fast span") is not False:
        fails.append("two presses the fast span takes as two still closed the folder")
    if "KEEP true" not in said.get("keep", ""):
        fails.append("the speed could not be written for the second boot: %r"
                     % said.get("keep", "")[-200:])
    if said.get("at the start") != "speed 60, double click 200 ms":
        fails.append("the desktop did not apply the mouse settings it was left with: %r"
                     % said.get("at the start"))
    if said.get("speed slider on a mouse") is not True:
        fails.append("a relative mouse was not offered a speed slider")
    if "ASKED true 48 48" not in said.get("asked", ""):
        fails.append("a speed asked of the window manager did not reach the board: %r"
                     % said.get("asked", "")[-200:])

    for which in ("first seen", "second seen"):
        seen = said.get(which, "")

        if " died: " in seen:
            fails.append("something died: " + seen[seen.find(" died: ") - 80:][:300])

    checks = 9

    if fails:
        print("FAIL: %d of %d checks on the Mouse page:" % (len(fails), checks))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: %d checks on the Mouse page (a tablet offered no speed; the folder opened by a "
          "double click at the slow span; the fast end written, told to every window, and two "
          "presses then two clicks; both settings applied at the next desktop's start on a PS/2 "
          "mouse, which has a speed slider; a speed asked of the window manager on the board)."
          % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
