#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Every window draws itself (`docs/astra-display.md` D2).

Diego, 9 October 2026, on the Kosmos Board: "Every window draws itself" -
the UI kit draws a window's frame into the window's own pictures, in the
application's process, and commits them; the window manager carries out
no drawing for anybody. Until D2e this suite held a window drawing itself
to the same window sending its drawing, pixel for pixel, at 100 and at 150
per cent (`testing.md` 18.501, 18.502); the commands are gone now, so it
holds the two ways a window comes to a scale to each other instead.

One desktop:

  - a window opens, drawing itself, and paints sixty frames, timed - the
    processor time of the program and of the window manager;
  - its menu draws itself, and shows its second row marked;
  - Calculator opens, drawing itself;
  - the desktop goes to 150 per cent with the window open, and a second
    window opens there: the one rescaled - a region made again at the new
    scale's pixels - and the one opened at it are the same, pixel for pixel,
    below their headers.

Usage: run_drawself.py IMAGE
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

FRAMES = 60

# The window, sixty frames of it timed - the ticks the scheduler gave this
# program and the window manager while they were painted, and the counter's
# time - and on the first, a menu with its second row marked.
PROGRAM = r'''-- kosmos: application
local ui = use("/Kosmos/Libraries/ui.lua")
local first = tostring(args or ""):match("first") ~= nil
local name = first and "first" or "second"
local win = ui.window{ title = "D2", w = 420, h = 260, x = first and 60 or 600, y = 160 }
win:add(ui.label{ x = 16, y = 16, text = "Every window draws itself", role = "heading" })
win:add(ui.button{ x = 16, y = 64, text = "A button" })
win:add(ui.checkbox{ x = 16, y = 112, text = "A ticked box", checked = true })
win:add(ui.slider{ x = 16, y = 156, w = 240, value = 40 })
win:add(ui.label{ x = 16, y = 196, text = "The quick brown fox, 0123456789" })
print(("D2 %s: draws itself %s"):format(name, tostring(win.draws_itself == true)))

-- Side by side, wherever the window manager first put them.
local at = fs.send("/Running/wm", { type = "move", window = win.handle,
                                    x = first and 80 or 620, y = 200 })
print(("D2 %s at %d,%d"):format(name, at and at.x or -1, at and at.y or -1))

local function ticks(of)
  for _, p in ipairs(sys.processes()) do
    if p.name == of then return p.ticks end
  end
  return 0
end

local me = nil
for _, p in ipairs(sys.processes()) do
  if tostring(p.name):match("d2") then me = p.name end
end

win:paint()

if first then
  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
  local app0, wm0, t0 = ticks(me), ticks("wm"), sys.ticks()
  for _ = 1, FRAMES do win:paint() end
  local t1 = sys.ticks()
  print(("D2 cost: %d frames, app %d ticks, wm %d ticks, %.2f ms a frame"):format(
        FRAMES, ticks(me) - app0, ticks("wm") - wm0, (t1 - t0) * 1000 / hz / FRAMES))

  local m = win:open_menu(win.origin_x + 200, win.origin_y + 80,
                          { { text = "One" }, { text = "Two", mark = true }, { text = "Three" } })
  print(("D2 menu at %d,%d %dx%d, draws itself %s"):format(m and m.x or -1, m and m.y or -1,
        m and m.w or -1, m and m.h or -1, tostring(m and m.region ~= nil)))
end

win:run()
'''.replace("FRAMES", str(FRAMES))

LAUNCH = ('local r = fs.send("/Running/wm", { type = "launch", program = "/Temporary/d2.lua", '
          'args = args })\nprint("LAUNCH " .. tostring(r and r.ok))\n')
CALC = ('local r = fs.send("/Running/wm", { type = "launch", program = "/Kosmos/Apps/calc.lua" })\n'
        'print("CALC " .. tostring(r and r.ok))\n')
SCALE = ('local r = fs.send("/Running/wm", { type = "scale", pct = tonumber(args) })\n'
         'print("SCALE " .. tostring(r and r.ok))\n')


def compare(rgb, width, places, k):
    """Below the header and inside the edge, the first window's pixels
    against the second's: how many differ of how many, and where."""
    if len(places) < 2:
        return None, None

    (ax, ay, aw, ah), (bx, by, bw, bh) = [tuple(int(v) for v in p) for p in places[:2]]
    differ, total, where = 0, 0, []
    top, edge = int(48 * k), int(4 * k)

    for y in range(top, min(ah, bh) - edge):
        ra = ((ay + y) * width + ax + edge) * 3
        rb = ((by + y) * width + bx + edge) * 3
        n = (min(aw, bw) - 2 * edge) * 3
        a, b = rgb[ra:ra + n], rgb[rb:rb + n]
        total += n // 3

        if a != b:
            for i in range(0, n, 3):
                if a[i:i + 3] != b[i:i + 3]:
                    differ += 1
                    where.append((edge + i // 3, y))

    box = (min(p[0] for p in where), min(p[1] for p in where),
           max(p[0] for p in where), max(p[1] for p in where)) if where else None
    return (differ, total), box


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web)
    fails, said = [], {}

    def found(pattern, since, seconds=60):
        deadline = time.time() + seconds

        while time.time() < deadline:
            guest._read_available()
            m = re.findall(pattern, guest.seen[since:])

            if m:
                return m

            time.sleep(0.3)

        return []

    def press_the_desk():
        guest.mouse_to(30000, 30000)
        time.sleep(0.3)
        guest.mouse_button(True)
        time.sleep(0.1)
        guest.mouse_button(False)
        time.sleep(1)

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)

        for name, text in (("d2", PROGRAM), ("launch", LAUNCH), ("calc", CALC), ("scale", SCALE)):
            session.put(text.encode(), "/Temporary/%s.lua" % name)

        mark = len(guest.seen)
        session.run("/Temporary/launch.lua first")
        said["first"] = found(r"D2 first: draws itself (\w+)", mark)
        said["cost"] = found(r"D2 cost: ([^\n]*)", mark)
        said["menu"] = found(r"D2 menu at (\d+),(\d+) (\d+)x(\d+), draws itself (\w+)", mark)
        size = found(r"wm: window D2 at \d+,\d+ (\d+)x(\d+)", mark)
        first = found(r"D2 first at (\d+),(\d+)", mark)
        time.sleep(1.5)
        guest.mouse_to(200, 32000)
        time.sleep(1)
        w0, _, px0 = R.parse_ppm(guest.screendump())

        if said["menu"]:
            # Which rows carry the mark, read as the dock's suite reads the
            # desktop's menu: ink in the mark's column against the menu's
            # ground beside the first row.
            mx, my = (int(v) for v in said["menu"][-1][:2])
            o = ((my + 2 + R.MENU_ROW // 2) * w0 + mx + 20) * 3
            ground = px0[o:o + 3]
            marked = []

            for row in range(1, 4):
                top = my + 2 + (row - 1) * R.MENU_ROW
                ink = sum(1 for yy in range(top + 6, top + 18) for xx in range(mx + 10, mx + 18)
                          if px0[(yy * w0 + xx) * 3:(yy * w0 + xx) * 3 + 3] != ground)

                if ink > 8:
                    marked.append(row)

            said["menu marked"] = marked

        # The menu away by a press on the empty desk: an Escape would leave
        # the window keyboard-driven, its focus ringed.
        press_the_desk()

        mark = len(guest.seen)
        session.run("/Temporary/calc.lua")
        said["calc"] = found(r"wm: window Calculator at ", mark, 30)

        # **At 150 per cent**: the first window told its new size makes a
        # region at the new scale's pixels; a second opens at it.
        mark = len(guest.seen)
        said["scaled"] = session.run("/Temporary/scale.lua 150").decode(errors="replace")
        found(r"wm: rescaled D2 to ", mark)
        session.run("/Temporary/launch.lua second")
        said["second"] = found(r"D2 second: draws itself (\w+)", mark)
        second = found(r"D2 second at (\d+),(\d+)", mark)
        guest.mouse_to(200, 32000)
        time.sleep(4)
        shot = guest.screendump()
        w_, _, rgb = R.parse_ppm(shot)
        os.makedirs(os.path.join(os.path.dirname(HERE), "build", "drawself"), exist_ok=True)
        with open(os.path.join(os.path.dirname(HERE), "build", "drawself", "screen150.ppm"), "wb") as f:
            f.write(shot)

        if size and first and second:
            w, h = (int(v) * 3 // 2 for v in size[0])
            places = [(int(first[-1][0]) * 3 // 2, int(first[-1][1]) * 3 // 2, w, h),
                      (int(second[-1][0]) * 3 // 2, int(second[-1][1]) * 3 // 2, w, h)]
            said["places"] = places
            said["differ"], said["where"] = compare(rgb, w_, places, 1.5)

        session.run("/Temporary/scale.lua 100")
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        guest.close()

    if said.get("first") != ["true"]:
        fails.append("the window did not draw itself: %r" % said.get("first"))

    menu = said.get("menu")

    if not menu or menu[-1][4] != "true":
        fails.append("the menu did not draw itself: %r" % menu)
    elif said.get("menu marked") != [2]:
        fails.append("the menu drawing itself did not show its second row marked: %r"
                     % said.get("menu marked"))

    if not said.get("cost"):
        fails.append("the frames were not timed")
    if not said.get("calc"):
        fails.append("Calculator did not open")
    if said.get("second") != ["true"]:
        fails.append("a window opened at 150 per cent did not draw itself: %r" % said.get("second"))

    differ = said.get("differ")

    if not differ or differ[1] == 0:
        fails.append("the two windows were not found to compare: %r" % said.get("places"))
    elif differ[0] != 0:
        fails.append("at 150 per cent the window rescaled differs from the one opened there in "
                     "%d of %d pixels, within %r" % (differ + (said.get("where"),)))

    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:300])

    checks = 7

    if fails:
        print("FAIL: %d of %d checks on windows that draw themselves:" % (len(fails), checks))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: %d checks on windows that draw themselves (a window and its menu, the second "
          "row marked; %s; Calculator; at 150 per cent the window rescaled and one opened there "
          "the same in all %d pixels)." % (checks, said["cost"][-1], differ[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
