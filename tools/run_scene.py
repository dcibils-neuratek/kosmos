#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The desktop composed in C (`docs/astra-display.md` D3a, D3b-2).

The window manager describes its scene before each pass - every window's
place, stacking, picture and what it wears, the menus, the pointer - and
the window server composes it (`user/servers/windowserver.c`), from the
pictures it was handed as regions and the scene written into a region they
share. Booted as `wm compose-check`, each pass is composed there and again
by the Lua it replaced, over it, and the pixels that differ are counted.

One desktop, as much of it as can be put in front of the composer: the
Deskbar, the desktop's icons and wallpaper, Calculator and Preferences
(their shadows, corners and three lights), a cube turning (a pass every
frame), a menu, a window moved over another, and the scale changed to 150
per cent and back. Every pass composed the same both
ways: not one pixel differs, over at least two hundred passes.

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
        "-fw_cfg", "name=opt/kosmos/boot,string=wm compose-check",
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
        guest._read_available()
        said["before"] = len(guest.seen)
        session.run("/Temporary/launch.lua /Temporary/closer.lua")
        guest.wait_for("CLOSER closed", "the window that closes")
        said["closed"] = len(guest.seen)

        deadline = time.time() + 60

        while time.time() < deadline:
            guest._read_available()
            checks = re.findall(r"wm: compose check: (\d+) passes composed both ways, "
                                r"(-?\d+) pixels differ", guest.seen)

            later = re.findall(r"pictures held", guest.seen[said["closed"] + 200:])

            if checks and int(checks[-1][0]) >= 200 and len(later) >= 2:
                break

            time.sleep(1)

        said["checks"] = checks
        said["seen"] = guest.seen

        # **The screen given back** (step 3): the window manager killed, and
        # the window server gives the screen to the console, which clears
        # it and prints from the top. (Starting it again is not checked: a
        # window manager started from Telnet is not lent the door, which is
        # right, and the console's own shell does not answer after the kill,
        # which is older than this and on `roadmap.md`.)
        said["desktop"] = plain(guest)
        session.run("kill wm")
        guest.wait_for("the window manager is gone; the screen is the console's",
                       "the screen given back")
        time.sleep(2)
        said["console"] = plain(guest)
        said["menu"] = "SCENE menu true" in guest.seen
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        guest.close()

    checks = said.get("checks") or []

    if not checks:
        fails.append("the window manager never said its compose check")
    else:
        passes, differ = (int(v) for v in checks[-1])

        if passes < 200:
            fails.append("only %d passes were composed both ways" % passes)

        if differ != 0:
            first = next((c for c in checks if int(c[1]) != 0), checks[-1])
            where = re.findall(r"wm: compose check: (\d+ differ in [^\n]*)", seen)
            fails.append("the scene and the Lua differ in %d pixels over %d passes (first at "
                         "pass %s): %s" % (differ, passes, first[0], "; ".join(where[:8])))

    # The pictures held: before the window, while it was open, and after.
    seen_then = said.get("seen") or ""
    held = [(m.start(), int(m.group(1))) for m in
            re.finditer(r"pixels differ, (\d+) pictures held", seen_then)]
    if "before" in said and "closed" in said:
        before = [n for at, n in held if at < said["before"]]
        during = [n for at, n in held if said["before"] <= at < said["closed"]]
        after = [n for at, n in held if at > said["closed"] + 200]
        if not before or not during or not after:
            fails.append("the pictures held were not said around the window: %r" % held[-12:])
        elif not (max(during) > before[-1] and after[-1] == before[-1]):
            fails.append("a closed window's pictures were not let go: %d held before it, "
                         "%d while open, %d after" % (before[-1], max(during), after[-1]))
        else:
            said["held"] = (before[-1], max(during), after[-1])

    if "console" not in said:
        fails.append("the window manager was not killed, or the screen not given back")
    elif not (said["console"] > 0.9 and said["desktop"] < 0.8):
        fails.append("the screen was not given back: the commonest colour %.0f%% of the "
                     "desktop, %.0f%% after the kill" % (said["desktop"] * 100,
                                                       said["console"] * 100))

    if not said.get("menu"):
        fails.append("the menu did not open")
    # The window server (D3b-2): started by init, its door lent to the
    # window manager alone, which greets it.
    if not re.search(r"windowserver: the screen, \d+x\d+", seen) \
            or not re.search(r"wm: the window server answers, the screen \d+x\d+", seen):
        fails.append("the window manager did not greet the window server: %r"
                     % re.findall(r"[^\n]*window ?server[^\n]*", seen)[:4])
    # And composing there (step two): the back buffer and the scene's
    # region handed over, every picture the scene names in a region of its
    # own, and no pass refused - so the pixels the check compared are the
    # window server's.
    if not re.search(r"wm: the window server composes, \d+x\d+", seen):
        fails.append("the window server does not compose: %r"
                     % re.findall(r"wm: [^\n]*window server[^\n]*", seen)[:4])
    for refused in ("with no region of its own", "the window server did not compose",
                    "the window server was not given"):
        if refused in seen:
            at = seen.find(refused)
            fails.append("a picture or a pass refused: " + seen[seen.rfind("\n", 0, at) + 1:][:200])
    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:300])

    if fails:
        print("FAIL: %d of 7 checks on the desktop composed in C:" % len(fails))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: 7 checks on the desktop composed in C (the window server greeted and composing; %s passes composed by the window server and by "
          "the Lua it replaced - the Deskbar, the desktop, Calculator, Preferences, a cube "
          "turning, a menu, a window moved, 150 per cent and back - and not one "
          "pixel differs; a window's picture held while it was open and let go after it "
          "closed, %d, %d, %d; the window manager killed and the screen the console's, %.0f%% one "
          "colour)." % ((checks[-1][0],) + said["held"]
                                                             + (said["console"] * 100,)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
