#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The desktop composed in C (`docs/astra-display.md` D3a).

The window manager describes its scene before each pass - every window's
place, stacking, picture and what it wears, the menus, the pointer - and
the Graphics Kit's scene composes it (`gfx.scene`, `user/kits/gfx/scene.c`).
Booted as `wm compose-check`, each pass is composed by the scene and again
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

        for name, text in (("menu", MENU), ("launch", LAUNCH), ("move", MOVE),
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

        deadline = time.time() + 60

        while time.time() < deadline:
            guest._read_available()
            checks = re.findall(r"wm: compose check: (\d+) passes composed both ways, "
                                r"(-?\d+) pixels differ", guest.seen)

            if checks and int(checks[-1][0]) >= 200:
                break

            time.sleep(1)

        said["checks"] = checks
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

    if not said.get("menu"):
        fails.append("the menu did not open")
    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:300])

    if fails:
        print("FAIL: %d of 3 checks on the desktop composed in C:" % len(fails))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: 3 checks on the desktop composed in C (%s passes composed by the scene and by "
          "the Lua it replaced - the Deskbar, the desktop, Calculator, Preferences, a cube "
          "turning, a menu, a window moved, 150 per cent and back - and not one "
          "pixel differs)." % checks[-1][0])
    return 0


if __name__ == "__main__":
    sys.exit(main())
