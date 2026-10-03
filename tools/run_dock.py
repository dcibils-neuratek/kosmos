#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Deskbar as a dock, in the Night look (`roadmap.md`, a dock at the bottom).

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
  2. The dock floating: centred, twelve points above the edge, its strip
     across the top; the room a maximised window is given ending above the
     dock rather than under it.
  3. The Kosmos button: its menu opening upwards, from above the dock; the
     button lit while it is open, and dark again once a press elsewhere has
     dismissed it - which the window manager now tells the owner
     (`menus_gone`), where before the button stayed lit.
  4. The whole width: the dock along the foot, and the room given back to
     the gap it no longer leaves.
  5. The bar at the top again: no dock, and all the room back.

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

GAP = 12            # `DOCK_GAP` in `wm.lua`: a floating dock above the edge
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
ROOM = ('local r = fs.send("/Running/wm", { type = "workarea" })\n'
        'print(("ROOM %d %d %d %d"):format(r.x, r.y, r.w, r.h))\n')


def room(session):
    out = session.run("/Temporary/room.lua").decode(errors="replace")
    m = re.search(r"ROOM (\d+) (\d+) (\d+) (\d+)", out)

    return tuple(int(v) for v in m.groups()) if m else None


def last_dock(seen):
    """The dock's place, as the window manager last said it."""
    places = re.findall(r"wm: the dock at (\d+),(\d+) (\d+)x(\d+)", seen)

    return tuple(int(v) for v in places[-1]) if places else None


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web)
    fails = []
    said = {}

    def click(x, y, width, height):
        guest.mouse_to(*R._to_tablet(x, y, width, height))
        time.sleep(0.3)
        guest.mouse_button(True)
        time.sleep(0.2)
        guest.mouse_button(False)
        time.sleep(0.4)

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
        click(dx + PAD + 40, by, width, height)
        said["menu"] = guest.wait_for_line("wm: menu of Deskbar at ", "the Kosmos menu", mark)
        time.sleep(1)
        _, _, at = R.pixel_reader(guest.screendump())
        said["lit"] = at(bx, by)
        click(width // 2, STRIP_H // 2, width, height)    # the strip's middle: nothing there
        time.sleep(1.5)
        _, _, at = R.pixel_reader(guest.screendump())
        said["dismissed"] = at(bx, by)

        # ---- 4: the whole width ----
        mark = len(guest.seen)
        said["whole"] = session.run("setprop /Running/Deskbar/dock whole").decode(errors="replace")
        guest.wait_for("deskbar: a dock, the whole width", "the dock along the foot")
        said["along"] = guest.wait_for_line("wm: window Deskbar at ", "the dock's window", mark)
        time.sleep(1)
        said["room whole"] = room(session)

        # ---- 5: back to the top ----
        mark = len(guest.seen)
        said["top"] = session.run("setprop /Running/Deskbar/bar top").decode(errors="replace")
        said["again"] = guest.wait_for_line("wm: window Deskbar at ", "the bar again", mark)
        time.sleep(1)
        said["room again"] = room(session)
        guest._read_available()
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

    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:400])

    checks = 14

    if fails:
        print("FAIL: %d of %d checks on the dock:" % (len(fails), checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on the Deskbar as a dock, in Night (the look applied; "
          "the write moving the bar answered, and the same place asked "
          "for again starting nothing; the login items opened once; the "
          "strip across the top; the dock centred %d above the edge; a "
          "maximised window ending above it; the Kosmos menu opening upwards; "
          "its button lit while it is open and dark once it is dismissed; the "
          "whole width along the foot, giving the gap back; and the bar at the "
          "top again with all the room back)." % (checks, GAP))
    return 0


if __name__ == "__main__":
    sys.exit(main())
