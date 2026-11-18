#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Liquid Glass's wallpaper glass (`docs/astra.md`, Liquid Glass LG2), on
one machine.

The desktop started in Liquid Glass with a carried wallpaper, and a program
at login choosing Endeavour and then Liquid Glass Night, as Appearance does:

  - the desktop wears Liquid Glass, its wallpaper placed;
  - the wallpaper's glass is made once, the size of the picture as shown -
    the screen's when it fills it, its own when it is larger - in three passes
    whose reach is the look's `glass_blur` - 24 points, so 8 a pass at
    100% - and in less than a few seconds even under TCG;
  - Endeavour, which is not glass, gives the glass back;
  - Liquid Glass Night, which is, makes it again;
  - nothing died, and no program stopped on a Lua error.

The control is Endeavour: a window manager that made the glass for every
look, or never gave it back, fails the third; one that never made it
fails the second.

The pixels of the glass are LG4's to check, when something shows them;
the blur itself is held to arithmetic on the Mac (`tools/test_blur.c`).

Usage: run_glass.py IMAGE
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                  # noqa: E402

CHECKS = 6

SWAP = r'''
local theme = use("/Kosmos/Libraries/theme.lua")
local themes = use("/Kosmos/Libraries/themes.lua")

local function send(name)
  local look = theme.read(themes[name], "dark")
  local colours = {}

  for _, k in ipairs(theme.tokens) do colours[k] = look[k] end

  local r = fs.send("/Running/wm", { type = "theme", palette = colours })

  print("GLASSSWAP " .. name .. " " .. tostring(r and r.ok))
end

send("endeavour")
send("liquidglassnight")
'''


def line(guest, pattern, mark, seconds=60):
    end = time.time() + seconds
    while time.time() < end:
        guest._read_available()
        m = re.search(pattern, guest.seen[mark:])
        if m:
            return m
        time.sleep(0.2)
    return None


def lua_string(text):
    return "[==[" + text + "]==]"


def run(fails):
    image = "build/x86_64/kosmos.elf" if len(sys.argv) < 2 else sys.argv[1]
    guest = R.Guest(image, 150)
    try:
        guest.wait_for_line("kosmos> ", "the prompt")
        mark = len(guest.seen)
        guest.type('print("PREFS", fs.write("/Temporary/glassswap.lua", %s), '
                   'fs.write("/Home/Preferences/appearance", { palette = "liquidglass", '
                   'wallpaper = "wallpaper/glass-wave.jpg", wallpaper_fit = "fill" }), '
                   'fs.write("/Home/Preferences/startup", { items = { "/Temporary/glassswap.lua" } }))'
                   % lua_string(SWAP.replace("\n", " ")))
        guest.wait_for_line("PREFS", "the appearance and startup settings", mark)
        mark = len(guest.seen)
        guest.type("wm")

        screen = line(guest, r"wm: the window server answers, the screen (\d+)x(\d+)", mark, 120)
        made = line(guest, r"wm: the wallpaper's glass, (\d+)x(\d+), three passes at (\d+), "
                           r"in (\d+) ms", mark, 60)
        swapped = line(guest, r"GLASSSWAP liquidglassnight (\S+)", mark, 60)
        time.sleep(2)
        guest._read_available()
        seen = guest.seen[mark:]
        print("glass: " + "; ".join(re.findall(r"wm: (the wallpaper[^\r\n]*)", seen)))

        if not re.search(r"wm: theme liquidglass\b", seen):
            fails.append("the desktop did not start in Liquid Glass")
        placed = re.search(r"wm: the wallpaper, (\d+)x(\d+), (fills the screen|at its own size)", seen)

        if not placed:
            fails.append("the carried wallpaper was not placed")

        if not (screen and made):
            fails.append("the wallpaper's glass was not made: %r" % (made and made.group(0)))
            return seen

        sw, sh = int(screen.group(1)), int(screen.group(2))
        w, h, r, ms = (int(made.group(i)) for i in range(1, 5))

        # The picture as shown: the screen's size when it fills the screen,
        # its own when it is larger and only its middle shows - the glass
        # lies where the picture lies, so a surface finds its part the same way.
        want = (sw, sh) if placed and placed.group(3) == "fills the screen" else \
            (placed and (int(placed.group(1)), int(placed.group(2))))

        if (w, h) != want or r != 8 or ms > 8000:
            fails.append("the glass was %dx%d at %d a pass in %d ms; the picture as shown is "
                         "%r and 24 points is 8 a pass" % (w, h, r, ms, want))

        after = seen[made.end():]
        back = re.search(r"wm: the wallpaper's glass given back", after)

        if not (swapped and re.search(r"GLASSSWAP endeavour true", seen) and back):
            fails.append("Endeavour, which is not glass, did not give the glass back: %r, %r"
                         % (swapped and swapped.group(0), back and back.group(0)))
        elif not re.search(r"wm: the wallpaper's glass, \d+x\d+, three passes", after[back.end():]):
            fails.append("Liquid Glass Night did not make the glass again")

        broke = re.search(r" died: |\.lua:\d+: [^\n]*(attempt to|expected)|stack traceback", seen)

        if broke:
            fails.append("something stopped: " + seen[max(0, broke.start() - 80):][:300])

        return seen
    finally:
        guest.close()


def main():
    fails = []
    seen = ""

    try:
        seen = run(fails) or ""
    except Exception as e:                                  # noqa: BLE001
        fails.append("the run itself failed: %s" % e)

    if fails:
        print("FAIL: %d of %d checks on the wallpaper's glass:" % (len(fails), CHECKS))
        for f in fails:
            print("  " + f)
        for l in seen.splitlines()[-25:]:
            print("  | " + l)
        return 1

    print("PASS: %d checks on the wallpaper's glass (the desktop in Liquid Glass, its "
          "wallpaper placed; the glass made the picture's size, three passes "
          "at the look's reach; given back for Endeavour and made again for Liquid Glass "
          "Night; nothing stopped)." % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
