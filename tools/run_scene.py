#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The desktop composed by the window server (`docs/astra-display.md` D3b-2).

The window manager describes its scene before each pass - every window's
place, stacking, picture and what it wears, the menus, the pointer - and
the window server composes it (`user/servers/windowserver.c`), from the
pictures it was handed as regions and the scene written into a region they
share, and puts it on the screen. Since step 4 the window manager has no
composing of its own; until then `wm compose-check` held the two to the
same pixels here, over two thousand passes, and `test_draw` holds the scene
on the Mac.

One desktop, as much of it as can be put in front of the composer: the
Deskbar, the desktop's icons and wallpaper, Calculator and Preferences
(their shadows, corners and three lights), a cube turning (a pass every
frame), a menu, a window moved over another, and the scale changed to 150
per cent and back: every picture handed over and no pass refused, the
desktop on the screen, a closed window's pictures let go, and the screen
given back to the console when the window manager is killed.

Usage: run_scene.py IMAGE
"""

import os
import random
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import collections                                          # noqa: E402

import run_screenshot as R                                  # noqa: E402
import run_servers as S                                     # noqa: E402

MENU = r'''-- kosmos: application
local ui = use("/Kosmos/Libraries/ui.lua")
local win = ui.window{ title = "Scene", w = 360, h = 220, x = 500, y = 300 }
win:add(ui.label{ x = 16, y = 16, text = "A window with a menu", role = "heading" })
win:add(ui.button{ x = 16, y = 64, text = "A button" })
win:paint()
local m = win:open_menu(win.origin_x + 120, win.origin_y + 70,
                        { { text = "One" }, { text = "Two", mark = true }, { separator = true },
                          { text = "Three" } })
print("SCENE menu " .. tostring(m ~= nil))
win:run()
'''

# A window shown for three seconds and closed: the window server holds its
# picture while it is open and lets it go when it closes.
CLOSER = r'''-- kosmos: application
local ui = use("/Kosmos/Libraries/ui.lua")
local win = ui.window{ title = "Closer", w = 240, h = 120, x = 80, y = 520 }
win:add(ui.label{ x = 16, y = 16, text = "Closing soon" })
win:paint()
print("CLOSER open")
sys.sleep(750)
win:close()
print("CLOSER closed")
'''

LAUNCH = ('local r = fs.send("/Running/wm", { type = "launch", program = args })\n'
          'print("LAUNCH " .. tostring(r and r.ok))\n')
MOVE = ('local wm = use("/Kosmos/Libraries/wmproto.lua")\n'
        'local list = wm.windows() or {}\n'
        'for _, w in ipairs(list.windows or {}) do\n'
        '  if w.title == "Calculator" then\n'
        '    for step = 0, 12 do\n'
        '      fs.send("/Running/wm", { type = "move", window = w.handle, x = 300 + step * 25, y = 200 + step * 12 })\n'
        '      sys.sleep(2)\n'
        '    end\n'
        '  end\n'
        'end\n'
        'print("MOVED")\n')
# Shadows on, which the default look has off: the scene draws them too.
SHADOW = ('local r = fs.send("/Running/wm", { type = "theme", shadow = true })\n'
          'print("SHADOW " .. tostring(r and r.ok))\n')
SCALE = ('local r = fs.send("/Running/wm", { type = "scale", pct = tonumber(args) })\n'
         'print("SCALE " .. tostring(r and r.ok))\n')


def plain(guest):
    """How much of the screen is its commonest colour: nearly all of it for
    the console, cleared and printing from the top; far less for a desktop."""
    w, h, px = R.parse_ppm(guest.screendump())
    counts = collections.Counter(px[i:i + 3] for i in range(0, len(px), 3 * 7))
    return counts.most_common(1)[0][1] / max(1, sum(counts.values()))


def boot(image, telnet, web):
    """A desktop started as `wm compose-check`, Telnet on."""
    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    forward = "hostfwd=tcp::%d-:23,hostfwd=tcp::%d-:8080" % (telnet, web)
    setattr(R, board, saved + [
        "-netdev", "user,id=net0," + forward,
        "-device", R.device(image, "net") + ",netdev=net0",
        "-fw_cfg", "name=opt/kosmos/telnetd,string=23",
        "-fw_cfg", "name=opt/kosmos/boot,string=wm",
    ])

    try:
        return R.Guest(image, 120)
    finally:
        setattr(R, board, saved)


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = boot(image, telnet, web)
    fails, said = [], {}

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)

        for name, text in (("menu", MENU), ("closer", CLOSER), ("launch", LAUNCH), ("move", MOVE),
                           ("scale", SCALE), ("shadow", SHADOW)):
            session.put(text.encode(), "/Temporary/%s.lua" % name)

        # A desktop with as much as can be put in front of the window
        # server: shadows on, windows with their lights, a cube turning, a
        # menu, a window moved over another, 150 per cent and back.
        session.run("/Temporary/shadow.lua")

        for program in ("/Kosmos/Apps/calc.lua", "/Kosmos/Apps/preferences.lua",
                        "/Kosmos/Apps/cube3d.lua", "/Temporary/menu.lua"):
            session.run("/Temporary/launch.lua " + program)
            time.sleep(2)

        guest.mouse_to(16000, 12000)
        time.sleep(1)
        session.run("/Temporary/move.lua")
        time.sleep(3)
        session.run("/Temporary/scale.lua 150")
        time.sleep(5)
        session.run("/Temporary/scale.lua 100")
        time.sleep(5)
        said["desktop"] = plain(guest)

        # A window shown and closed: the pictures the window server holds
        # go up while it is open and come back down after.
        guest._read_available()
        said["before"] = len(guest.seen)
        session.run("/Temporary/launch.lua /Temporary/closer.lua")
        guest.wait_for("CLOSER closed", "the window that closes")
        # Where the line is, not how much has been read: what follows it
        # may already have arrived with it.
        #
        # From the window manager's own "closed", not the program's: the
        # manager says what it holds as it closes the window, and which of
        # the two lines comes first is the scheduler's choice - a program
        # not in the display band said its line after the count, and the
        # count fell before the point it was read from.
        said["closed"] = guest.seen.find("CLOSER closed", said["before"])
        closed_by_wm = guest.seen.find("wm: closed Closer", said["before"])

        if 0 <= closed_by_wm < said["closed"]:
            said["closed"] = closed_by_wm
        deadline = time.time() + 30

        while time.time() < deadline:
            guest._read_available()

            if "pictures" in guest.seen[said["closed"]:]:
                time.sleep(2)
                guest._read_available()
                break

            time.sleep(0.5)

        said["menu"] = "SCENE menu true" in guest.seen

        # **The screen given back** (step 3): the window manager killed, and
        # the window server gives the screen to the console, which clears
        # it and prints from the top. (Starting it again is not checked: a
        # window manager started from Telnet is not lent the door, which is
        # right, and the console's own shell does not answer after the kill,
        # which is older than this and on `roadmap.md`.)
        session.run("kill wm")
        guest.wait_for("the window manager is gone; the screen is the console's",
                       "the screen given back")
        time.sleep(2)
        said["console"] = plain(guest)
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        guest.close()

    # The window server greeted and composing, and every picture handed over.
    if not re.search(r"windowserver: the screen, \d+x\d+", seen) \
            or not re.search(r"wm: the window server answers, the screen \d+x\d+", seen):
        fails.append("the window manager did not greet the window server: %r"
                     % re.findall(r"[^\n]*window ?server[^\n]*", seen)[:4])
    if not re.search(r"wm: the window server composes, \d+x\d+", seen):
        fails.append("the window server does not compose: %r"
                     % re.findall(r"wm: [^\n]*window server[^\n]*", seen)[:4])
    for refused in ("with no region of its own", "the window server did not compose",
                    "the window server was not given", "no desktop"):
        if refused in seen:
            at = seen.find(refused)
            fails.append("a picture or a pass refused: " + seen[seen.rfind("\n", 0, at) + 1:][:200])

    # What it composed is on the screen: a desktop, not one colour.
    if "desktop" in said and said["desktop"] >= 0.8:
        fails.append("the screen is %.0f%% one colour with the desktop up" % (said["desktop"] * 100))

    # The pictures held: before the window, while it was open, and after.
    held = [(m.start(), int(m.group(1))) for m in
            re.finditer(r"wm: the window server holds (\d+) pictures", seen)]
    said["held"] = None
    if "before" in said and "closed" in said:
        before = [n for at, n in held if at < said["before"]]
        during = [n for at, n in held if said["before"] <= at < said["closed"]]
        after = [n for at, n in held if at >= said["closed"]]
        if not before or not during or not after:
            fails.append("the pictures held were not said around the window: %r" % held[-12:])
        # **The fewest held after**, not the last: another window may take
        # a picture of its own after this one has gone - Tracker's sidebar
        # gaining a row for a machine it has just seen on the network did,
        # 21 after 20, and the check read that as this window's picture
        # kept (`testing.md` 18.652).
        elif not (max(during) > before[-1] and min(after) == before[-1]):
            fails.append("a closed window's pictures were not let go: %d held before it, "
                         "%d while open, %d at fewest after" % (before[-1], max(during), min(after)))
        else:
            said["held"] = (before[-1], max(during), min(after))

    if "console" not in said:
        fails.append("the window manager was not killed, or the screen not given back")
    elif not (said["console"] > 0.9 and said.get("desktop", 1) < 0.8):
        fails.append("the screen was not given back: the commonest colour %.0f%% of the "
                     "desktop, %.0f%% after the kill" % (said.get("desktop", 0) * 100,
                                                       said["console"] * 100))

    if not said.get("menu"):
        fails.append("the menu did not open")
    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:300])

    if fails:
        print("FAIL: %d of 7 checks on the desktop composed by the window server:" % len(fails))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: 7 checks on the desktop composed by the window server (greeted and composing; "
          "every picture in a region of its own and no pass refused - the Deskbar, the desktop, "
          "Calculator, Preferences, a cube turning, a menu, a window moved, 150 per cent and "
          "back; the desktop on the screen, %.0f%% one colour; a window's pictures held while "
          "it was open and let go after it closed, %d, %d, %d; the window manager killed and "
          "the screen the console's, %.0f%% one colour)."
          % ((said["desktop"] * 100,) + said["held"] + (said["console"] * 100,)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
