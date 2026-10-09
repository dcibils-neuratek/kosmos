#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""A window that draws itself (`docs/astra-display.md` D2).

Diego, 9 October 2026, on the Kosmos Board: "Every window draws itself" -
the UI kit draws a window's frame into the window's own surface, in the
application's process, and commits it, rather than sending the drawing
for the window manager to carry out.

One desktop, and the same window opened twice by the same program - a
heading, a button, a ticked box, a slider - once drawing itself and once
sending its drawing:

  - the window manager took the first one's region, and said so;
  - below their headers the two are the same, pixel for pixel: the commands
    are `paint.lua`'s either way, with this process's faces and pictures in
    one and the window manager's in the other;
  - each painted sixty frames, and the processor time of the program and
    of the window manager together is said for each way: what D2 is for;
  - Calculator, the first application to draw itself, opens drawing itself.

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

# The window, drawn by itself or sent, and sixty frames of it timed: the
# ticks the scheduler gave this program and the window manager while they
# were painted, and the counter's time.
PROGRAM = r'''-- kosmos: application
local ui = use("/Kosmos/Libraries/ui.lua")
local itself = tostring(args or ""):match("itself") ~= nil
local win = ui.window{ title = "D2", w = 420, h = 260, x = itself and 60 or 600, y = 160,
                       draws_itself = itself }
win:add(ui.label{ x = 16, y = 16, text = "Every window draws itself", role = "heading" })
win:add(ui.button{ x = 16, y = 64, text = "A button" })
win:add(ui.checkbox{ x = 16, y = 112, text = "A ticked box", checked = true })
win:add(ui.slider{ x = 16, y = 156, w = 240, value = 40 })
win:add(ui.label{ x = 16, y = 196, text = "The quick brown fox, 0123456789" })
print(("D2 %s: draws itself %s"):format(itself and "itself" or "sent", tostring(win.draws_itself == true)))

-- Side by side, wherever the window manager first put them: a window over
-- the other would hide what is compared.
local at = fs.send("/Running/wm", { type = "move", window = win.handle,
                                    x = itself and 80 or 620, y = 200 })
print(("D2 %s at %d,%d"):format(itself and "itself" or "sent", at and at.x or -1, at and at.y or -1))

local function ticks(name)
  for _, p in ipairs(sys.processes()) do
    if p.name == name then return p.ticks end
  end
  return 0
end

local me = nil
for _, p in ipairs(sys.processes()) do
  if tostring(p.name):match("d2") then me = p.name end
end

win:paint()
local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local app0, wm0, t0 = ticks(me), ticks("wm"), sys.ticks()
for _ = 1, FRAMES do win:paint() end
local t1 = sys.ticks()
print(("D2 cost %s: %d frames, app %d ticks, wm %d ticks, %.2f ms a frame"):format(
      itself and "itself" or "sent", FRAMES, ticks(me) - app0, ticks("wm") - wm0,
      (t1 - t0) * 1000 / hz / FRAMES))
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

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)
        session.put(PROGRAM.encode(), "/Temporary/d2.lua")
        session.put(LAUNCH.encode(), "/Temporary/launch.lua")
        session.put(CALC.encode(), "/Temporary/calc.lua")

        mark = len(guest.seen)
        session.run("/Temporary/launch.lua itself")
        said["itself"] = found(r"D2 itself: draws itself (\w+)", mark)
        said["cost itself"] = found(r"D2 cost itself: ([^\n]*)", mark)
        session.run("/Temporary/launch.lua sent")
        said["sent"] = found(r"D2 sent: draws itself (\w+)", mark)
        said["cost sent"] = found(r"D2 cost sent: ([^\n]*)", mark)
        sizes = found(r"wm: window D2 at \d+,\d+ (\d+)x(\d+)", mark)
        places = [found(r"D2 itself at (\d+),(\d+)", mark)[-1:],
                  found(r"D2 sent at (\d+),(\d+)", mark)[-1:]]
        places = [p[0] + sizes[0] for p in places if p] if sizes else []
        # The pointer away from both, since the screen's picture has it in.
        guest.mouse_to(200, 32000)
        time.sleep(2)

        shot = guest.screendump()
        w_, h_, rgb = R.parse_ppm(shot)
        os.makedirs(os.path.join(os.path.dirname(HERE), "build", "drawself"), exist_ok=True)
        with open(os.path.join(os.path.dirname(HERE), "build", "drawself", "screen.ppm"), "wb") as f:
            f.write(shot)
        said["places"] = places

        said["differ"], said["where"] = compare(rgb, w_, places, 1)

        # **At 150 per cent** (D2c): the scale changed with both open. The
        # one drawing itself is told its new size, makes a region in the
        # new pixels and draws its commands scaled, its text in faces at the
        # scale; the other is drawn by the window manager as before.
        session.put(SCALE.encode(), "/Temporary/scale.lua")
        mark = len(guest.seen)
        said["scaled"] = session.run("/Temporary/scale.lua 150").decode(errors="replace")
        found(r"wm: rescaled D2 to ", mark)
        guest.mouse_to(200, 32000)
        time.sleep(4)
        shot = guest.screendump()
        w_, h_, rgb = R.parse_ppm(shot)
        with open(os.path.join(os.path.dirname(HERE), "build", "drawself", "screen150.ppm"), "wb") as f:
            f.write(shot)
        big = [(str(int(x) * 3 // 2), str(int(y) * 3 // 2), str(int(w) * 3 // 2), str(int(h) * 3 // 2))
               for x, y, w, h in places]
        said["places150"] = big
        said["differ150"], said["where150"] = compare(rgb, w_, big, 1.5)

        # And one opened at 150 per cent, its region made at the scale.
        mark = len(guest.seen)
        session.run("/Temporary/launch.lua itself")
        said["opened150"] = found(r"D2 itself: draws itself (\w+)", mark)
        session.run("/Temporary/scale.lua 100")

        mark = len(guest.seen)
        session.run("/Temporary/calc.lua")
        said["calc"] = found(r"wm: window Calculator at ", mark, 30)
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        guest.close()

    if said.get("itself") != ["true"]:
        fails.append("the window asking to draw itself was not given its region: %r"
                     % said.get("itself"))
    if said.get("sent") != ["false"]:
        fails.append("the window sending its drawing drew itself: %r" % said.get("sent"))

    differ = said.get("differ")

    if not differ or differ[1] == 0:
        fails.append("the two windows were not found to compare: %r" % said.get("places"))
    elif differ[0] != 0:
        fails.append("drawn by itself, the window differs from the one sent in %d of %d pixels, "
                     "within %r of the window; the windows at %r" % (differ + (said.get("where"),
                                                                          said.get("places"))))

    differ = said.get("differ150")

    if not differ or differ[1] == 0:
        fails.append("at 150 per cent the two windows were not found to compare: %r"
                     % said.get("places150"))
    elif differ[0] != 0:
        fails.append("at 150 per cent, drawn by itself, the window differs from the one sent "
                     "in %d of %d pixels, within %r" % (differ + (said.get("where150"),)))

    if said.get("opened150") != ["true"]:
        fails.append("a window opened at 150 per cent did not draw itself: %r"
                     % said.get("opened150"))

    if not said.get("cost itself") or not said.get("cost sent"):
        fails.append("the frames were not timed: %r, %r"
                     % (said.get("cost itself"), said.get("cost sent")))
    if not said.get("calc"):
        fails.append("Calculator did not open drawing itself")
    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:300])

    checks = 8

    if fails:
        print("FAIL: %d of %d checks on a window that draws itself:" % (len(fails), checks))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: %d checks on a window that draws itself (its region taken; the same as the "
          "window sending its drawing in all %d pixels below the header; %d frames each - "
          "drawing itself: %s; sent: %s; at 150 per cent the same again, in all %d pixels; "
          "Calculator open, drawing itself)."
          % (checks, said["differ"][1], FRAMES, said["cost itself"][-1], said["cost sent"][-1],
             said["differ150"][1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
