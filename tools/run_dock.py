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
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# A disk for /Home, as the M700's stick is one: without it /Home is held in
# memory, which held 16 KB a file until 5 October 2026 - and a picture of
# the screen is a megabyte or two. Named before `run_screenshot` is imported, which reads
# KOSMOS_DISK once, as it loads.
import scratch                                              # noqa: E402
import subprocess                                           # noqa: E402

_DISK = os.path.join(scratch.directory("dock"), "disk.img")
subprocess.run([os.path.join(os.path.dirname(HERE), "build", "host", "lua"),
                os.path.join(HERE, "kfs.lua"), "create", _DISK, "64"],
               check=True, capture_output=True, cwd=os.path.dirname(HERE))
os.environ["KOSMOS_DISK"] = _DISK

import run_screenshot as R                                  # noqa: E402
import run_servers as S                                     # noqa: E402
import kosmos_vnc as V                                      # noqa: E402

ROOT = os.path.dirname(HERE)

GAP = 6             # `DOCK_GAP` in `wm.lua`: a floating dock above the edge
DOCK_H = 60         # `dock.H`
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

# The dock's cells and pins, as the Deskbar says them, and the pins as the
# file keeps them.
CELLS = ('print("CELLS " .. tostring(fs.read("/Running/Deskbar/cells")))\n'
         'print("PINS " .. tostring(fs.read("/Running/Deskbar/pins")))\n'
         'local f = fs.read("/Home/Preferences/dock")\n'
         'print("KEPT " .. (type(f) == "table" and type(f.pins) == "table" '
         'and table.concat(f.pins, ",") or "nothing"))\n')

# Which window has the focus, as the window manager lists them.
FOCUS = ('local r = use("/Kosmos/Libraries/wmproto.lua").windows()\n'
         'for _, w in ipairs(r and r.windows or {}) do\n'
         '  if w.focused then print("FOCUS " .. tostring(w.title)) end\n'
         'end\n')

ROOM = ('local r = fs.send("/Running/wm", { type = "workarea" })\n'
        'print(("ROOM %d %d %d %d"):format(r.x, r.y, r.w, r.h))\n')

# Preferences on Appearance, as a press on its row opens it; and a window
# closed by its title.
PREFS = ('local r = fs.send("/Running/wm", { type = "launch", '
         'program = "/Kosmos/Apps/preferences.lua", args = "appearance" })\n'
         'print("PREFS " .. tostring(r and r.ok))\n')
CLOSE = ('local r = use("/Kosmos/Libraries/wmproto.lua").windows()\n'
         'for _, w in ipairs(r and r.windows or {}) do\n'
         '  if w.title == args then fs.send("/Running/wm", { type = "close", window = w.handle }) end\n'
         'end\n')
CLEAR = ('local f = fs.read("/Home/Preferences/appearance")\n'
         'print("CLEAR " .. tostring(type(f) == "table" and f.dock_transparency))\n')


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
    # A USB keyboard, which QEMU's keys then go to: the M700's path, where
    # a key repeats, Super alone is a tap and Print Screen exists.
    guest = S.boot(image, telnet, web,
                   extra=("-fw_cfg", "name=opt/kosmos/fb,string=1720x1440",
                          "-device", "qemu-xhci,id=usbk",
                          "-device", "usb-kbd,bus=usbk.0"))
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
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)
        session.put(LOOK.encode(), "/Temporary/look.lua")
        session.put(ROOM.encode(), "/Temporary/room.lua")
        session.put(PREFS.encode(), "/Temporary/prefs.lua")
        session.put(CLOSE.encode(), "/Temporary/close.lua")
        session.put(CLEAR.encode(), "/Temporary/clear.lua")
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

        # ---- 2b: a name over the icon under the pointer ----
        session.put(FOCUS.encode(), "/Temporary/focus.lua")
        dx, dy, dw, dh = said["dock"]
        over = dx + dw - PAD - 25                 # the last cell, always an application
        mark = len(guest.seen)
        guest.mouse_to(*R._to_tablet(over, dy + dh // 2, width, height))
        said["tip name"] = guest.wait_for_line("deskbar: tip ", "a name over an icon", mark)
        said["tip"] = guest.wait_for_line("wm: window Deskbar tip at ", "the tip's window", mark)
        time.sleep(1)
        said["tip focus"] = session.run("/Temporary/focus.lua").decode(errors="replace")
        mark = len(guest.seen)
        guest.mouse_to(*R._to_tablet(width // 2, height // 3, width, height))
        time.sleep(2)
        guest._read_available()
        said["tip gone"] = "wm: closed Deskbar tip" in guest.seen[mark:]
        said["tip over"] = over

        # ---- 2c: the dock arranged by hand ----
        session.put(CELLS.encode(), "/Temporary/cells.lua")

        def cells():
            out = session.run("/Temporary/cells.lua").decode(errors="replace")
            c = re.search(r"CELLS (.*)", out)
            found = {}

            for part in (c.group(1).split("; ") if c else []):
                bits = part.split(" ")

                if len(bits) == 3:
                    found[bits[0]] = (int(bits[1]), int(bits[2]))

            pins_ = re.search(r"PINS (\S*)", out)
            kept_ = re.search(r"KEPT (\S*)", out)
            return found, pins_ and pins_.group(1), kept_ and kept_.group(1)

        def dock_now():
            """Where the dock is now: it is centred again as it grows or
            shrinks with what is pinned."""
            time.sleep(1)
            guest._read_available()
            return last_dock(guest.seen)

        def maybe(text, what, since):
            """A line, or None when it never comes - so a check that fails
            fails alone rather than stopping those after it."""
            try:
                return guest.wait_for_line(text, what, since)
            except Exception:              # noqa: BLE001 - its check says
                return None

        def drag_from_to(x0, y0, x1, y1):
            guest.mouse_to(*R._to_tablet(x0, y0, width, height))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.3)

            for k in range(1, 7):
                guest.mouse_to(*R._to_tablet(x0 + (x1 - x0) * k // 6, y0 + (y1 - y0) * k // 6,
                                             width, height))
                time.sleep(0.25)

            time.sleep(0.4)
            guest.mouse_button(False)
            time.sleep(1.5)

        found, said["pins before"], _ = cells()
        dx, dy, dw, dh = said["dock"]
        mid = dy + dh // 2

        if "terminal" in found and "tracker" in found:
            mark = len(guest.seen)
            tx, tw = found["terminal"]
            rx, _ = found["tracker"]
            drag_from_to(dx + tx + tw // 2, mid, dx + rx + 4, mid)
            said["moved"] = maybe("deskbar: moved terminal - the dock is ", "Terminal moved", mark)

        found, _, _ = cells()
        dx, dy, dw, dh = dock_now()

        if "music" in found:
            mark = len(guest.seen)
            mx, mw = found["music"]
            drag_from_to(dx + mx + mw // 2, mid, dx + mx + mw // 2, dy - 120)
            said["removed"] = maybe("deskbar: removed music - the dock is ", "Music taken out", mark)

        found, _, _ = cells()
        dx, dy, dw, dh = dock_now()
        loose = [n for n in found if n in ("logview", "sysmon", "procs")]

        if loose:
            mark = len(guest.seen)
            lx, lw = found[loose[0]]
            click(dx + lx + lw // 2, mid, width, height, "right")
            menu = maybe("wm: menu of Deskbar at ", "an icon's menu", mark)
            m = re.match(r"(\d+),(\d+) (\d+)x(\d+)", menu or "")

            if m:
                mx_, my_, mw_, mh_ = (int(v) for v in m.groups())
                click(mx_ + mw_ // 2, my_ + mh_ * 3 // 6, width, height)  # the second of three rows
                said["kept"] = maybe("deskbar: kept ", "Keep in Dock", mark)
            said["kept name"] = loose[0]

        mark = len(guest.seen)
        said["pin"] = session.run("setprop /Running/Deskbar/pin calc").decode(errors="replace")
        said["pinned"] = maybe("deskbar: kept calc - the dock is ", "calc added", mark)
        time.sleep(2)
        found, said["pins after"], said["pins kept"] = cells()
        dx, dy, dw, dh = dock_now()

        if "calc" in found:
            mark = len(guest.seen)
            cx, cw = found["calc"]
            click(dx + cx + cw // 2, mid, width, height)
            said["launched"] = maybe("deskbar: the dock launched ", "a click on a pin", mark)

            if said["launched"]:
                guest.wait_for("wm: window Calculator at ", "the Calculator from the dock")
            time.sleep(1)

        # ---- 2d: how much shows through the dock ----
        # Diego, 4 October: a slider from 100% to 0%, 25% unless said, and
        # the icons as they are. The pill sampled in its end's padding and an
        # icon at its middle, at 0, 100 and 25; then the slider in
        # Preferences pressed at its middle.
        dx, dy, dw, dh = dock_now()
        found, _, _ = cells()
        cell = found.get("terminal") or found.get("tracker")

        def dock_at(percent):
            mark_ = len(guest.seen)
            session.run("setprop /Running/Deskbar/transparency %d" % percent)
            heard = maybe("deskbar: the dock %d%% transparent" % percent, "the transparency", mark_)
            time.sleep(1)
            _, _, at_ = R.pixel_reader(guest.screendump())
            pill = at_(dx + dw - 5, dy + dh // 2)
            icon = at_(dx + cell[0] + cell[1] // 2, dy + dh // 2) if cell else None
            return heard is not None, pill, icon

        said["clear 0"] = dock_at(0)
        said["clear 100"] = dock_at(100)
        said["clear 25"] = dock_at(25)

        mark = len(guest.seen)
        session.run("/Temporary/prefs.lua")
        prefs = maybe("wm: window Preferences at ", "Preferences", mark)
        slider = maybe("preferences slider: dock_transparency at ", "the slider", mark)
        p_ = re.match(r"(\d+),(\d+)", prefs or "")
        s_ = re.match(r"(\d+),(\d+), (\d+) wide", slider or "")

        if p_ and s_:
            time.sleep(1)
            mark = len(guest.seen)
            click(int(p_.group(1)) + int(s_.group(1)) + int(s_.group(3)) // 2,
                  int(p_.group(2)) + int(s_.group(2)), width, height)
            said["slid"] = maybe("deskbar: the dock ", "the slider pressed", mark)
            said["slid kept"] = session.run("/Temporary/clear.lua").decode(errors="replace")

        session.run("/Temporary/close.lua Preferences")
        dock_at(25)
        time.sleep(1)

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

        # A second press on the button closes it, and starts nothing - after
        # its sections: the rows in Diego's order, Demos shown by the pointer
        # resting on it with no click, and Down to the next (7 October,
        # `docs/launcher.html`).
        mark = len(guest.seen)
        click(*kosmos_button(), width, height)
        at_grid = guest.wait_for_line("launchpad: the grid at ", "the launcher grid again", mark)
        said["pills"] = guest.wait_for_line("launchpad: rows ", "the sections' rows", mark)
        time.sleep(2)
        gx, gy = (int(v) for v in re.match(r"(\d+),(\d+)", at_grid).groups())
        pill = re.search(r"Demos (\d+),(\d+) (\d+)x(\d+)", said["pills"])

        if pill:
            px, py, pw, ph = (int(v) for v in pill.groups())
            # Moved onto it, the button never pressed.
            guest.mouse_to(*R._to_tablet(gx + px + 60, gy + py + ph // 2, width, height))
            said["demos"] = guest.wait_for_line("launchpad: Demos, ", "Demos shown by resting on it", mark)
            # The launcher kept as it looked, for whoever reads the run after.
            time.sleep(1.5)
            w_, h_, rgb_ = R.parse_ppm(guest.screendump())
            m_ = re.match(r"(\d+),(\d+) (\d+)x(\d+)", at_grid)

            if m_:
                lx, ly, lw, lh = (int(v) for v in m_.groups())
                rows_ = [rgb_[((y_ * w_) + lx) * 3:((y_ * w_) + lx + lw) * 3]
                         for y_ in range(ly, min(h_, ly + lh))]
                os.makedirs(os.path.join(ROOT, "build", "dock"), exist_ok=True)
                V.png(os.path.join(ROOT, "build", "dock", "launcher.png"), lw, len(rows_),
                      b"".join(rows_))
            guest.sendkey("down")
            said["tabbed"] = guest.wait_for_line("launchpad: Preferences, ", "Down to the next section", mark)

        click(*kosmos_button(), width, height)
        time.sleep(3)
        guest._read_available()
        said["second"] = guest.seen[mark:]

        # ---- 3c: Super and the key left of 1 - º on Diego's keyboard ----
        mark = len(guest.seen)
        guest.sendkey("meta_l-grave_accent")
        said["modal"] = guest.wait_for_line("wm: window Shortcuts at ", "the shortcuts, modal", mark)
        time.sleep(2)
        guest.sendkey("esc")
        time.sleep(2)
        guest._read_available()
        said["modal closed"] = "wm: closed Shortcuts" in guest.seen[mark:]

        # ---- 3d: the Windows key alone, on the USB keyboard ----
        mark = len(guest.seen)
        guest.sendkey("meta_l")
        said["super grid"] = guest.wait_for_line("launchpad: the grid at ", "the grid from Super alone", mark)
        time.sleep(2)
        guest.sendkey("meta_l")
        time.sleep(2)
        guest._read_available()
        said["super closed"] = ("deskbar: the launcher closed" in guest.seen[mark:]
                                and "wm: closed Open" in guest.seen[mark:])

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

        # ---- 6b: a picture of the screen, by Print Screen, Control Alt 1
        # and Super Shift 3 - each saved into /Home/Captures ----
        shots = []

        for keys_ in ("print", "ctrl-alt-1", "meta_l-shift-3"):
            mark = len(guest.seen)
            guest.sendkey(keys_)

            try:
                shots.append((keys_, guest.wait_for_line("screenshot: ", "a screenshot by " + keys_, mark)))
            except Exception:              # noqa: BLE001 - said below
                shots.append((keys_, None))

            time.sleep(1.2)                # another second, another name

        said["shots"] = shots
        # Each said as a notification, which opens the picture when pressed.
        guest._read_available()
        said["shot notes"] = len(re.findall(
            r'notify: \d+ "Screenshot saved" from /Kosmos/Programs/screenshot\.lua',
            guest.seen))
        said["shot opens"] = session.run("notify --list").decode(errors="replace")
        first = re.match(r"(\S+), (\d+)x(\d+), (\d+) bytes", shots[0][1] or "")

        if first:
            # Its first 24 bytes, read where it is: the signature, and the
            # width and height in its IHDR. The whole of it over Telnet is
            # longer than a session waits under emulation.
            head = ('local s = fs.read(%r) or ""\n'
                    'print("PNGHEAD " .. s:sub(1, 24):gsub(".", function(c) '
                    'return ("%%02x"):format(c:byte()) end))\n' % first.group(1))
            session.put(head.encode(), "/Temporary/pnghead.lua")
            out = session.run("/Temporary/pnghead.lua").decode(errors="replace")
            m = re.search(r"PNGHEAD ([0-9a-f]+)", out)
            said["png"] = bytes.fromhex(m.group(1)) if m else out.encode()

        # Where the window manager put the dock: a floating one is "the dock
        # at", one the whole width is the Deskbar's window - either is
        # "x,y WxH" after it.
        def dock_line(since, what):
            deadline = time.monotonic() + 60

            while time.monotonic() < deadline:
                guest._read_available()
                m = re.search(r"(?:wm: the dock at |wm: window Deskbar at )(\d+,\d+ \d+x\d+)",
                              guest.seen[since:])

                if m and not m.group(1).startswith("0,0 "):
                    return m.group(1)

                time.sleep(0.3)

            raise R.Failure("the guest never said " + what)

        # ---- 6c: the dock's size, a setting (7 October) - Large, the dock
        # started again at 72 tall, and Medium back at 60 ----
        mark = len(guest.seen)
        session.run("setprop /Running/Deskbar/bar dock")
        guest.wait_for_line("deskbar: again, the bar dock", "the bar a dock for its size", mark)
        time.sleep(2)
        mark = len(guest.seen)
        session.run("setprop /Running/Deskbar/size large")
        said["large"] = dock_line(mark, "the dock at its Large size")
        mark = len(guest.seen)
        session.run("setprop /Running/Deskbar/size medium")
        said["medium"] = dock_line(mark, "the dock at its Medium size")
        time.sleep(2)
        mark = len(guest.seen)
        session.run("setprop /Running/Deskbar/bar top")
        guest.wait_for_line("wm: window Deskbar at 0,0 ", "the bar back at the top", mark)
        time.sleep(2)

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

    if not (said.get("moved") or "").startswith("terminal,tracker,"):
        fails.append("Terminal dragged before Tracker did not go first: %r (it was %r)"
                     % (said.get("moved"), said.get("pins before")))

    if "music" in (said.get("removed") or "music"):
        fails.append("Music dragged up off the dock was not taken out: %r" % said.get("removed"))

    if not (said.get("kept") or "").startswith(str(said.get("kept name")) + " - "):
        fails.append("Keep in Dock on a running application's menu did not pin it: %r"
                     % said.get("kept"))

    if not (said.get("pinned") or "").endswith(",calc") or "is now" not in said.get("pin", ""):
        fails.append("setprop /Running/Deskbar/pin calc did not add it at the end: %r, %r"
                     % (said.get("pin"), said.get("pinned")))

    if not said.get("pins kept") or said.get("pins kept") != said.get("pins after"):
        fails.append("the dock's pins were not written to /Home/Preferences/dock: %r kept, %r shown"
                     % (said.get("pins kept"), said.get("pins after")))

    if (said.get("launched") or "") != "calc":
        fails.append("a click on a pinned icon did not open it on the release: %r"
                     % said.get("launched"))

    tip = re.match(r"(\d+),(\d+) (\d+)x(\d+)", said.get("tip", ""))

    if not tip or not dock or not re.match(r"[A-Z]", said.get("tip name", "")) \
       or abs(int(tip.group(1)) + int(tip.group(3)) // 2 - said.get("tip over", 0)) > 3 \
       or int(tip.group(2)) + int(tip.group(4)) > dock[1]:
        fails.append("no name over the icon under the pointer, centred over it "
                     "and above the dock: %r at %r, the pointer at %r, dock %r"
                     % (said.get("tip name"), said.get("tip"), said.get("tip over"), dock))

    if "FOCUS Deskbar tip" in said.get("tip focus", "") or "FOCUS" not in said.get("tip focus", ""):
        fails.append("the tip took the focus: %r" % said.get("tip focus"))

    if not said.get("tip gone"):
        fails.append("the tip stayed when the pointer left the dock")

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

    order = [p.split(" ")[0] for p in said.get("pills", "").split("; ")]

    if order != ["All", "Applications", "System", "Development", "Demos", "Preferences"]:
        fails.append("the sections' rows were not All and the five in Diego's order: %r"
                     % said.get("pills"))

    demos = re.match(r"(\d+)", said.get("demos", ""))
    every = int(grid.group(5)) if grid else 0

    if not demos or not 0 < int(demos.group(1)) < every:
        fails.append("resting on Demos, with no click, did not show Demos alone: %r of %d"
                     % (said.get("demos"), every))

    if not said.get("tabbed"):
        fails.append("Down did not step from Demos to Preferences")

    if not said.get("modal") or not said.get("modal closed"):
        fails.append("Super and the key left of 1 did not open the shortcuts "
                     "over everything, or Escape did not close them: %r, closed %s"
                     % (said.get("modal"), said.get("modal closed")))

    if "wm: closed Open" not in said.get("outside", ""):
        fails.append("a press outside the launcher did not close it")

    if not said.get("super grid") or not said.get("super closed"):
        fails.append("the Windows key alone on a USB keyboard did not open the "
                     "grid and close it again: %r, closed %s"
                     % (said.get("super grid"), said.get("super closed")))

    for keys_, line in said.get("shots", []):
        if not line or not re.match(r"/Home/Captures/screenshot-\d{4}-\d\d-\d\d-\d{6}\.png, %dx%d, \d+ bytes"
                                    % (width, height), line):
            fails.append("%s did not save a picture of the %dx%d screen into "
                         "Captures: %r" % (keys_, width, height, line))

    if said.get("shot notes") != 3:
        fails.append("three screenshots were not said as three notifications: %r"
                     % said.get("shot notes"))

    if not re.search(r"Screenshot saved\s+\S.*-> /Home/Captures/screenshot-\d{4}-\d\d-\d\d-\d{6}\.png",
                     said.get("shot opens", "")):
        fails.append("a screenshot's notification does not open its picture: %r"
                     % said.get("shot opens", "")[-300:])

    png = said.get("png") or b""

    if png[:8] != b"\x89PNG\r\n\x1a\n" or png[16:24] != struct.pack(">II", width, height):
        fails.append("the screenshot read back is not a %dx%d PNG: %r"
                     % (width, height, png[:32]))

    tall = [re.search(r"\d+,\d+ \d+x(\d+)", said.get(k) or "") for k in ("large", "medium")]

    if not all(tall) or [int(m.group(1)) for m in tall] != [72, 60]:
        fails.append("the dock's size, Large and then Medium, did not make it 72 and "
                     "then 60 tall: %r, %r" % (said.get("large"), said.get("medium")))

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

    # ---- 2d ----
    c0, c100, c25 = said.get("clear 0"), said.get("clear 100"), said.get("clear 25")

    if not (c0 and c100 and c25 and c0[0] and c100[0] and c25[0]):
        fails.append("the dock's transparency was not taken as it was set: %r"
                     % ((c0, c100, c25),))
    elif not (c0[1] != c25[1] != c100[1] and c0[1] != c100[1]):
        fails.append("the dock's pill did not change with its transparency - 0, 25, "
                     "100: %r %r %r" % (c0[1], c25[1], c100[1]))
    elif not (c0[2] is not None and c0[2] == c100[2] == c25[2]):
        fails.append("the dock's icon changed with the pill's transparency: %r %r %r"
                     % (c0[2], c25[2], c100[2]))

    # Pressed at its middle, the slider says about half - a pixel either
    # side of the middle is a percent - and the file keeps what it says.
    slid = re.match(r"(\d+)% transparent", said.get("slid") or "")
    kept = re.search(r"CLEAR (\d+)", said.get("slid kept") or "")

    if not (slid and kept and 45 <= int(slid.group(1)) <= 55
            and kept.group(1) == slid.group(1)):
        fails.append("the slider in Preferences, pressed at its middle, did not make "
                     "the dock about half transparent and keep it: %r"
                     % ((said.get("slid"), said.get("slid kept")),))

    checks = 47

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
          "maximised window ending above it; a name over the icon under the "
          "pointer, centred, not taking the focus, gone when it leaves; the Kosmos menu, the right "
          "button's, opening upwards over its button, floating and along the "
          "whole width, Restart and Shut Down in it wearing a picture; the launcher grid above the dock with every "
          "application, the button lit while it is open, a name typed and "
          "Return opening it, its sections in Diego's order, Demos shown by resting on it and Down, the "
          "Windows key alone on a USB keyboard opening and closing it, and "
          "a second press or one outside closing it; Super and º opening the "
          "shortcuts over everything and Escape closing them; "
          "its button lit while it is open and dark once it is dismissed; the "
          "whole width along the foot, giving the gap back; the bar at the "
          "top again with all the room back) - and a 1920x1080 wallpaper "
          "filling the screen, centred leaving the desktop below it, and a "
          "fit that is neither refused; Print Screen, Control Alt 1 and Super "
          "Shift 3 each saving a PNG of the screen into Captures; and one program moving the bar four "
          "times, heard each time by the Deskbar that started since." % (checks, GAP))
    return 0


if __name__ == "__main__":
    sys.exit(main())
