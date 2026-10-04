#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The desktop at the M700's size: the Deskbar as a dock, in the Night look
(`roadmap.md`, a dock at the bottom), and the wallpaper filling the screen.

Diego, 3 October 2026: "an appearance setting to place the taskbar on the
bottom center and replicate as mich as possible the design language of
googlebook", drawn in `docs/dock.html` and agreed - two settings, the dock
floating by default and the whole width as the other, a strip at the top,
the look named Night. One boot, driven over Telnet and by QEMU's pointer:

  1. Night applied, and the bar moved to the bottom the way Preferences
     moves it - a write to `/Running/Deskbar/bar` - which is answered
     rather than refused by a Deskbar on its way out, and starts a new one
     that does not open the login items a second time; the same place
     asked for again starts nothing.
  2. The dock floating: centred, six points above the edge, its strip
     across the top; the room a maximised window is given ending above the
     dock rather than under it.
  3. The Kosmos button. Its left press opens the launcher (`launchpad`, as
     a grid above the dock): centred over the dock, with every application,
     the button lit while it is open, "calc" and Return opening the
     Calculator, and a second press on the button - or one anywhere else -
     closing it. Its right press, the menu: opening upwards, from above the dock; the
     button lit while it is open, and dark again once a press elsewhere has
     dismissed it - which the window manager now tells the owner
     (`menus_gone`), where before the button stayed lit.
  4. The whole width: the dock along the foot, and the room given back to
     the gap it no longer leaves.
  5. The bar at the top again: no dock, and all the room back.
  6. **The wallpaper** (Diego, 3 October, at the M700: "the wallpaper needs
     to be either stretched or expanded to fill the screen", "either center
     or fill"): a shipped 1920x1080 picture on this 1720x1440 screen fills
     it, the bottom left corner the picture's; centred, that corner is the
     desktop's colour below it; and a fit that is neither is refused.
  7. **One program moving the bar four times**, as Preferences does - the
     dock, floating, the whole width, the top - each heard by the Deskbar
     that started since the last: the namespace forgets a name whose process
     has gone and looks it up again (Diego, 3 October: "it changes only one
     time and then does not change any more times").

All of it at 1720x1440, the M700's screen, which no shipped wallpaper is
the size of - so a fill has to resample, and a centred picture leaves bands.

Usage: run_dock.py IMAGE
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

GAP = 6             # `DOCK_GAP` in `wm.lua`: a floating dock above the edge
DOCK_H = 64         # `dock.H`
STRIP_H = 32        # `dock.STRIP_H`
PAD = 10            # `dock.PAD`: the Kosmos button's distance from the end

LOOK = ('local ui = use("/Kosmos/Libraries/ui.lua")\n'
        'local th = ui.theme\n'
        'local shipped = use("/Kosmos/Libraries/themes.lua")\n'
        'local look = th.read(shipped.night, "dark")\n'
        'local c = {}\n'
        'for _, k in ipairs(th.tokens) do c[k] = look[k] end\n'
        'local r = fs.send("/Running/wm", { type = "theme", palette = c, fonts = look.fonts })\n'
        'print("LOOK " .. tostring(r and r.ok))\n')

# What a maximised window is given: its place and size, in the screen's
# points - which at this screen's scale are its pixels.
# The wallpaper as Preferences sets it, and its fit as Appearance's
# Wallpaper size does - then a word that is neither.
WALL = ('local r, why = fs.send("/Running/wm", { type = "wallpaper", '
        'path = "wallpaper/alexander-slattery-LI748t0BK8w.jpg" })\n'
        'print("WALL " .. tostring(r and r.ok) .. " " .. tostring(why))\n')
FIT = ('local r, why = fs.send("/Running/wm", { type = "wallpaper_fit", fit = args })\n'
       'print("FIT " .. tostring(r and r.ok) .. " " .. tostring(why))\n')

# Preferences' way: **one** process writing to the Deskbar again and again
# while the Deskbar starts itself again under it - which `setprop`, new each
# time, never was. Each write's answer said, and a wait for the new Deskbar.
SWITCH = ('local hz = sys.info().tick_hz or 100\n'
          'for _, step in ipairs({ { "bar", "dock" }, { "dock", "floating" }, '
          '{ "dock", "whole" }, { "bar", "top" } }) do\n'
          '  local ok, why = fs.write("/Running/Deskbar/" .. step[1], step[2])\n'
          '  print(("SWITCH %s %s %s %s"):format(step[1], step[2], tostring(ok), tostring(why)))\n'
          '  sys.sleep(hz * 6)\n'
          'end\n')

ROOM = ('local r = fs.send("/Running/wm", { type = "workarea" })\n'
        'print(("ROOM %d %d %d %d"):format(r.x, r.y, r.w, r.h))\n')


def room(session):
    out = session.run("/Temporary/room.lua").decode(errors="replace")
    m = re.search(r"ROOM (\d+) (\d+) (\d+) (\d+)", out)

    return tuple(int(v) for v in m.groups()) if m else None


def button_left(at, dock, colour):
    """Where the Kosmos button begins, by its colour along the dock's middle."""
    dx, dy, dw, dh = dock

    for x in range(dx, dx + dw):
        if at(x, dy + dh // 2) == colour:
            return x

    return None


def last_dock(seen):
    """The dock's place, as the window manager last said it."""
    places = re.findall(r"wm: the dock at (\d+),(\d+) (\d+)x(\d+)", seen)

    return tuple(int(v) for v in places[-1]) if places else None


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web,
                   extra=("-fw_cfg", "name=opt/kosmos/fb,string=1720x1440"))
    fails = []
    said = {}

    def click(x, y, width, height, button="left"):
        guest.mouse_to(*R._to_tablet(x, y, width, height))
        time.sleep(0.3)
        guest.mouse_button(True, button)
        time.sleep(0.2)
        guest.mouse_button(False, button)
        time.sleep(0.4)

    def kosmos_button():
        """Where to press the dock's Kosmos button, now - the dock grows as
        what runs does, and stays centred."""
        guest._read_available()
        x, y, w, h = last_dock(guest.seen)
        return x + PAD + 40, y + h // 2

    try:
        guest.wait_for("net: an address from DHCP", "a lease")
        guest.wait_for("wm: window Deskbar at ", "the bar")
        session = S.connect(telnet)
        session.put(LOOK.encode(), "/Temporary/look.lua")
        session.put(ROOM.encode(), "/Temporary/room.lua")
        said["look"] = session.run("/Temporary/look.lua").decode(errors="replace")
        width, height, _ = R.parse_ppm(guest.screendump())
        said["room top"] = room(session)

        # ---- 1, 2: the dock, floating ----
        mark = len(guest.seen)
        said["bar"] = session.run("setprop /Running/Deskbar/bar dock").decode(errors="replace")
        guest.wait_for("deskbar: a dock, floating", "the dock")
        said["strip"] = guest.wait_for_line("wm: window Deskbar strip at ", "the strip", mark)
        guest.wait_for("wm: the dock at ", "the dock its own width")
        time.sleep(3)                       # the cells of what runs, settled
        guest._read_available()
        said["dock"] = last_dock(guest.seen)
        said["room dock"] = room(session)

        # The place it already has: answered, and nothing started again.
        mark = len(guest.seen)
        said["same"] = session.run("setprop /Running/Deskbar/bar dock").decode(errors="replace")
        time.sleep(2)
        guest._read_available()
        said["same again"] = "deskbar: again" in guest.seen[mark:]

        # ---- 3: the Kosmos button and its menu ----
        dx, dy, dw, dh = said["dock"]
        bx, by = dx + PAD + 8, dy + dh // 2         # inside the button, clear of its words
        _, _, at = R.pixel_reader(guest.screendump())
        said["unlit"] = at(bx, by)
        mark = len(guest.seen)
        click(dx + PAD + 40, by, width, height, "right")       # the right button: the menu
        said["menu"] = guest.wait_for_line("wm: menu of Deskbar at ", "the Kosmos menu", mark)
        time.sleep(1)
        _, _, at = R.pixel_reader(guest.screendump())
        said["lit"] = at(bx, by)
        said["button"] = button_left(at, said["dock"], said["lit"])

        # Restart and Shut Down, the menu's last two rows, wear the three
        # cubes: colour in their picture's column, where the menu is grey.
        m = re.match(r"(\d+),(\d+) (\d+)x(\d+)", said["menu"] or "")
        said["power pictures"] = 0

        if m:
            mx, my, mw, mh = (int(v) for v in m.groups())

            for y in range(my + mh - 70, my + mh - 4):
                for x in range(mx + 4, mx + 40):
                    r, g, b = at(x, y)

                    if max(r, g, b) - min(r, g, b) > 80:
                        said["power pictures"] += 1
        click(width // 2, STRIP_H // 2, width, height)    # the strip's middle: nothing there
        time.sleep(1.5)
        _, _, at = R.pixel_reader(guest.screendump())
        said["dismissed"] = at(bx, by)

        # ---- 3b: the launcher, the left button's ----
        mark = len(guest.seen)
        click(*kosmos_button(), width, height)
        said["grid"] = guest.wait_for_line("launchpad: the grid at ", "the launcher grid", mark)
        time.sleep(2)
        _, _, at = R.pixel_reader(guest.screendump())
        said["grid lit"] = at(bx, by)
        said["grid dock"] = last_dock(guest.seen)

        for key in ("c", "a", "l", "c", "ret"):
            guest.sendkey(key)
            time.sleep(0.3)

        said["opened"] = guest.wait_for_line("launchpad: opened ", "a tile opened by its name", mark)
        guest.wait_for("wm: window Calculator at ", "the Calculator")
        time.sleep(3)                                   # the dock a cell wider

        # A second press on the button closes it, and starts nothing.
        mark = len(guest.seen)
        click(*kosmos_button(), width, height)
        guest.wait_for_line("launchpad: the grid at ", "the launcher grid again", mark)
        time.sleep(2)
        click(*kosmos_button(), width, height)
        time.sleep(3)
        guest._read_available()
        said["second"] = guest.seen[mark:]

        # And a press anywhere outside it.
        mark = len(guest.seen)
        click(*kosmos_button(), width, height)
        guest.wait_for_line("launchpad: the grid at ", "the launcher grid a third time", mark)
        time.sleep(2)
        click(width // 2, STRIP_H // 2, width, height)
        time.sleep(3)
        guest._read_available()
        said["outside"] = guest.seen[mark:]

        # ---- 4: the whole width ----
        mark = len(guest.seen)
        said["whole"] = session.run("setprop /Running/Deskbar/dock whole").decode(errors="replace")
        guest.wait_for("deskbar: a dock, the whole width", "the dock along the foot")
        said["along"] = guest.wait_for_line("wm: window Deskbar at ", "the dock's window", mark)
        time.sleep(1)
        said["room whole"] = room(session)

        # Its Kosmos button is in the middle of the bar, and its menu has to
        # open there (Diego's photograph: "way off the kosmos button").
        whole_dock = (0, height - DOCK_H, width, DOCK_H)
        _, _, at = R.pixel_reader(guest.screendump())
        start = button_left(at, whole_dock, said["unlit"])
        said["whole button"] = start

        if start is not None:
            mark = len(guest.seen)
            click(start + 40, height - DOCK_H // 2, width, height, "right")
            said["whole menu"] = guest.wait_for_line("wm: menu of Deskbar at ",
                                                     "the Kosmos menu, the whole width", mark)
            click(width // 2, STRIP_H // 2, width, height)
            time.sleep(1)

        # ---- 5: back to the top ----
        mark = len(guest.seen)
        said["top"] = session.run("setprop /Running/Deskbar/bar top").decode(errors="replace")
        said["again"] = guest.wait_for_line("wm: window Deskbar at ", "the bar again", mark)
        time.sleep(1)
        said["room again"] = room(session)

        # ---- 6: the wallpaper ----
        session.put(WALL.encode(), "/Temporary/wall.lua")
        session.put(FIT.encode(), "/Temporary/fit.lua")
        corner = (6, height - 6)                  # clear of the dock, the stamp and the icons
        said["wall"] = session.run("/Temporary/wall.lua").decode(errors="replace")
        time.sleep(2)
        _, _, at = R.pixel_reader(guest.screendump())
        said["filled"] = at(*corner)
        said["centre"] = session.run("/Temporary/fit.lua centre").decode(errors="replace")
        time.sleep(2)
        _, _, at = R.pixel_reader(guest.screendump())
        said["centred"] = at(*corner)
        said["sideways"] = session.run("/Temporary/fit.lua sideways").decode(errors="replace")

        # ---- 7: one program moving the bar four times ----
        session.put(SWITCH.encode(), "/Temporary/switch.lua")
        mark = len(guest.seen)
        said["switch"] = session.run("/Temporary/switch.lua").decode(errors="replace")
        guest._read_available()
        said["switched"] = guest.seen[mark:]
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the boot stopped: %s: %s" % (type(e).__name__, e))
    finally:
        guest.close()

    seen = guest.seen

    if "LOOK true" not in said.get("look", ""):
        fails.append("Night was not applied: %r" % said.get("look"))

    if "is now dock" not in said.get("bar", ""):
        fails.append("the write that moved the bar was not answered: %r" % said.get("bar"))

    if "is now dock" not in said.get("same", "") or said.get("same again") is not False:
        fails.append("the place the bar already had started it again: %r"
                     % said.get("same"))

    if seen.count(" at login") != 1:
        fails.append("the login items were opened %d times, not once"
                     % seen.count(" at login"))

    if said.get("strip", "").split(" ")[:2] != ["0,0", "%dx%d" % (width, STRIP_H)]:
        fails.append("the strip was not across the top: %r" % said.get("strip"))

    dock = said.get("dock")

    if not dock or dock[1] != height - DOCK_H - GAP or dock[3] != DOCK_H \
       or abs(dock[0] - (width - dock[2]) // 2) > 1:
        fails.append("the floating dock was not centred %d above the edge of a "
                     "%dx%d screen: %r" % (GAP, width, height, dock))

    top, docked = said.get("room top"), said.get("room dock")

    if not top or not docked or not dock or docked[1] + docked[3] > dock[1] - GAP:
        fails.append("a maximised window would not end above the dock: room %r, "
                     "dock %r" % (docked, dock))

    menu = re.match(r"(\d+),(\d+) (\d+)x(\d+)", said.get("menu", ""))

    if not menu or not dock or int(menu.group(2)) + int(menu.group(4)) > dock[1]:
        fails.append("the Kosmos menu did not open upwards from above the dock: "
                     "%r, dock %r" % (said.get("menu"), dock))

    def menu_over(menu, button):
        m = re.match(r"(\d+),", menu or "")
        return m is not None and button is not None and abs(int(m.group(1)) - button) <= 2

    if not menu_over(said.get("menu"), said.get("button")):
        fails.append("the floating dock's Kosmos menu did not open over its "
                     "button: the menu %r, the button from %r"
                     % (said.get("menu"), said.get("button")))

    if not menu_over(said.get("whole menu"), said.get("whole button")):
        fails.append("along the whole width the Kosmos menu did not open over "
                     "its button: the menu %r, the button from %r"
                     % (said.get("whole menu"), said.get("whole button")))

    if said.get("power pictures", 0) < 50:
        fails.append("Restart and Shut Down wore no picture in the Kosmos menu: "
                     "%d coloured pixels where the three cubes go"
                     % said.get("power pictures", 0))

    if not said.get("lit") or said.get("lit") == said.get("unlit"):
        fails.append("the Kosmos button was not lit while its menu was open: "
                     "%r, then %r" % (said.get("unlit"), said.get("lit")))

    if not said.get("dismissed") or said.get("dismissed") != said.get("unlit"):
        fails.append("the Kosmos button stayed lit after its menu was dismissed: "
                     "%r, lit %r, after %r" % (said.get("unlit"), said.get("lit"),
                                               said.get("dismissed")))

    along = said.get("along", "").split(" ")[:2]

    if "is now whole" not in said.get("whole", "") \
       or along != ["0,%d" % (height - DOCK_H), "%dx%d" % (width, DOCK_H)]:
        fails.append("the whole-width dock was not along the foot: %r, %r"
                     % (said.get("whole"), said.get("along")))

    whole = said.get("room whole")

    if not whole or not docked or whole[1] + whole[3] > height - DOCK_H \
       or whole[3] <= docked[3]:
        fails.append("the whole-width dock did not give back the gap: room %r, "
                     "floating %r" % (whole, docked))

    again = said.get("room again")

    if "is now top" not in said.get("top", "") \
       or not said.get("again", "").startswith("0,0 ") \
       or not again or again != top:
        fails.append("the bar at the top again did not give all the room back: "
                     "%r, %r, room %r, at first %r"
                     % (said.get("top"), said.get("again"), again, top))

    desk = (0x0b, 0x12, 0x20)                    # Night's desktop

    if "WALL true" not in said.get("wall", "") \
       or "fills the screen from 1290x1080 of it" not in seen \
       or not said.get("filled") or said.get("filled") == desk:
        fails.append("a 1920x1080 wallpaper did not fill a 1720x1440 screen: %r, "
                     "the corner %r" % (said.get("wall"), said.get("filled")))

    if "FIT true" not in said.get("centre", "") or said.get("centred") != desk:
        fails.append("the wallpaper centred did not leave the desktop's colour "
                     "below it: %r, the corner %r" % (said.get("centre"), said.get("centred")))

    if "a wallpaper fills or is centred" not in said.get("sideways", ""):
        fails.append("a wallpaper fit that is neither was not refused: %r"
                     % said.get("sideways"))

    grid = re.match(r"(\d+),(\d+) (\d+)x(\d+), (\d+) applications", said.get("grid", ""))
    gd = said.get("grid dock")

    if not grid or not gd \
       or abs(int(grid.group(1)) + int(grid.group(3)) // 2 - (gd[0] + gd[2] // 2)) > 1 \
       or int(grid.group(2)) + int(grid.group(4)) != gd[1] - 12 \
       or int(grid.group(5)) < 40:
        fails.append("the launcher grid was not above the dock, centred on it, "
                     "with every application: %r, the dock %r" % (said.get("grid"), gd))

    if not said.get("grid lit") or said.get("grid lit") != said.get("lit"):
        fails.append("the Kosmos button was not lit while the launcher was open: %r"
                     % (said.get("grid lit"),))

    if "calc" not in said.get("opened", "").lower():
        fails.append("typing calc and Return in the launcher did not open the "
                     "Calculator: %r" % said.get("opened"))

    second = said.get("second", "")

    if "wm: closed Open" not in second or second.count("launchpad: the grid at") != 1:
        fails.append("a second press on the Kosmos button did not close the "
                     "launcher, or opened another: %d grids, closed %s"
                     % (second.count("launchpad: the grid at"), "wm: closed Open" in second))

    if "wm: closed Open" not in said.get("outside", ""):
        fails.append("a press outside the launcher did not close it")

    took = re.findall(r"SWITCH (\w+) (\w+) (true|false)", said.get("switch", ""))
    moved = re.findall(r"deskbar: again, the bar (\w+) and the dock (\w+)",
                       said.get("switched", ""))

    if [t[2] for t in took] != ["true"] * 4 \
       or moved != [("dock", "whole"), ("dock", "floating"), ("dock", "whole"),
                    ("top", "whole")]:
        fails.append("one program moving the bar four times was not heard every "
                     "time - as Preferences was not: %r, the Deskbar said %r"
                     % (said.get("switch"), moved))

    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:400])

    checks = 26

    if fails:
        print("FAIL: %d of %d checks on the dock:" % (len(fails), checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on the desktop at the M700's 1720x1440 - the Deskbar "
          "as a dock, in Night (the look applied; "
          "the write moving the bar answered, and the same place asked "
          "for again starting nothing; the login items opened once; the "
          "strip across the top; the dock centred %d above the edge; a "
          "maximised window ending above it; the Kosmos menu, the right "
          "button's, opening upwards over its button, floating and along the "
          "whole width, Restart and Shut Down in it wearing a picture; the launcher grid above the dock with every "
          "application, the button lit while it is open, a name typed and "
          "Return opening it, and a second press or one outside closing it; "
          "its button lit while it is open and dark once it is dismissed; the "
          "whole width along the foot, giving the gap back; the bar at the "
          "top again with all the room back) - and a 1920x1080 wallpaper "
          "filling the screen, centred leaving the desktop below it, and a "
          "fit that is neither refused; and one program moving the bar four "
          "times, heard each time by the Deskbar that started since." % (checks, GAP))
    return 0


if __name__ == "__main__":
    sys.exit(main())
