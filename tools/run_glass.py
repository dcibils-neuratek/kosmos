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
  - the dock, in Liquid Glass Night, frosted: a row of its ground above
    its icons far smoother than the wallpaper's row just above the dock;
  - a notification's banner frosted the same way: a row of its card above
    its words far smoother than the wallpaper's row just under the banners;
  - a window the kit draws - About, opened at login - frosted: a row of its
    ground under its header, clear of words, against the wallpaper's above
    the window;
  - the window's rim lit from the upper left: the top row of About's solid
    header far brighter than the header 12 rows down, and its right edge,
    away from the light, no brighter than 5 pixels in;
  - and warm, About being the window in front: its top row's red over its
    blue far more than the header's, as the look's `rim_front` lights it;
  - a menu - the Deskbar's, the right button on the Kosmos button - frosted too: a
    row of it above its first item's words against the wallpaper's just
    above it;
  - nothing died, and no program stopped on a Lua error.

The control is Endeavour: a window manager that made the glass for every
look, or never gave it back, fails the third; one that never made it
fails the second. And a dock the compositor does not frost - `SC_GLASS`
left unset - fails the fifth: the tint over the sharp wallpaper keeps two
thirds of its roughness. A banner left out of the glass fails the sixth,
a window the seventh, and a menu the eighth - each also held to how far its
row's colour spreads, which a solid ground does not.

The blur itself is held to arithmetic on the Mac (`tools/test_blur.c`),
and the compositor's glass to pixels there (`tools/test_draw.c`).

Usage: run_glass.py IMAGE
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                  # noqa: E402

CHECKS = 15

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

# A banner, in a script of its own: the two in one typed line are past what
# the console takes.
#
# It waits for the banners' program to be there first: a post made before it
# starts is older than what it shows. No comments in it - it is typed as one
# line, and a comment would take the rest of the line with it.
BANNER = r'''
local function there()
  for _, n in ipairs(fs.list("/Running") or {}) do
    if tostring(type(n) == "table" and n.name or n) == "Notifications" then return true end
  end
end

for _ = 1, 100 do
  if there() then break end
  sys.sleep(5)
end

local notify = use("/Kosmos/Libraries/notify.lua")

print("GLASSBANNER " .. tostring(notify.post{ title = "Glass", body = "A banner on the wallpaper's glass", alert = true }))
'''

# Ten changes of focus between About and Calculator under `profile`, and
# what was composed for them - the last of the startup items, so that it is
# over before anything else is looked at: Calculator closed and About in
# front again. Typed as BANNER is, with no comments. (Typed after the
# desktop is up, a line goes to the window in front as keys.)
FOCUS = r'''
local S = function(m) return fs.send("/Running/wm", m) end
S{ type = "launch", program = "/Kosmos/Apps/calc.lua" }
local a, c
for _ = 1, 200 do
  for _, w in ipairs((use("/Kosmos/Libraries/wmproto.lua").windows() or {}).windows or {}) do
    if w.title == "About Kosmos" then a = w.handle end
    if w.title == "Calculator" then c = w.handle end
  end
  if a and c then break end
  sys.sleep(5)
end
S{ type = "move", window = c, x = 1300, y = 300 }
sys.sleep(100)
S{ type = "profile", on = true }
for _ = 1, 5 do
  S{ type = "raise", window = a }
  sys.sleep(30)
  S{ type = "raise", window = c }
  sys.sleep(30)
end
local p = S{ type = "profile" } or {}
S{ type = "profile", on = false }
S{ type = "close", window = c }
S{ type = "raise", window = a }
print("GLASSFOCUS " .. tostring(p.px) .. " " .. tostring(p.frames))
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


def roughness(at, x0, x1, y):
    """How much a row changes pixel to pixel: the sum over its channels."""
    total = 0
    prev = at(x0, y)
    for x in range(x0 + 1, x1):
        p = at(x, y)
        total += sum(abs(a - b) for a, b in zip(p, prev))
        prev = p
    return total / max(1, x1 - x0 - 1)


def frosted(guest, what, row, x0, x1, inside_y, above_y, most, fails):
    """A surface's ground frosted: a row of it, clear of its words and
    icons, far smoother than a row of the wallpaper beside it - the blur's
    doing. Over the sharp wallpaper the tint alone would leave two thirds
    of the wallpaper's roughness."""
    shot = guest.screendump()
    os.makedirs("build/glass", exist_ok=True)
    open("build/glass/%s.ppm" % what, "wb").write(shot)
    head = shot.split(b"\n", 3)
    sw, _ = (int(v) for v in head[1].split())
    data = head[3]

    def at(px, py):
        o = (py * sw + px) * 3
        return tuple(data[o:o + 3])

    inside = roughness(at, x0, x1, inside_y)
    above = roughness(at, x0, x1, above_y)
    # And not merely smooth: a solid ground is smooth too, and is one colour
    # end to end, where glass drifts with the wallpaper under it. The menu
    # came out solid once - composed on a path that knew no glass - and read
    # 0.0, which the smoothness alone passed.
    pixels = [at(x, inside_y) for x in range(x0, x1)]
    spread = max(max(p[c] for p in pixels) - min(p[c] for p in pixels) for c in range(3))
    print("frost: the %s %s; a row inside %.1f, the wallpaper beside %.1f; the row spreads %d"
          % (what, row, inside, above, spread))
    if not (above > 3 and inside < most * above and spread >= 8):
        fails.append("the %s's ground is not frosted: a row inside it changes %.1f a pixel and "
                     "spreads %d end to end, the wallpaper beside it %.1f"
                     % (what, inside, spread, above))
    return inside


def dock_frosted(guest, dockat, fails):
    """The dock: a row inside the pill above its icons, against the
    wallpaper's just above the dock."""
    if not dockat:
        fails.append("the dock never said where it is")
        return None
    x, y, w, h = (int(dockat.group(i)) for i in range(1, 5))
    return frosted(guest, "dock", "at %d,%d %dx%d" % (x, y, w, h),
                   x + w // 4, x + 3 * w // 4, y + 4, y - 8, 0.4, fails)


def menu_frosted(guest, screen, mark, fails):
    """The Deskbar's menu, opened by the right button on the Kosmos button
    at the dock's left end, 30 in - the left one opens the launcher pad: a row of it 5 down - its first item starts 2 down, its
    words in the middle of a row - over its right half, against the
    wallpaper's row 8 above it."""
    # Where the dock is now: it is first placed 400 wide and grows to its
    # icons after (`wm: the dock at`), and a press where it first was lands
    # on an icon and launches it.
    guest._read_available()
    now = re.findall(r"wm: the dock at (-?\d+),(-?\d+) (\d+)x(\d+)", guest.seen)
    if not now:
        fails.append("no dock to open the menu from")
        return None
    x, y, w, h = (int(v) for v in now[-1])
    sw, sh = (int(screen.group(1)), int(screen.group(2))) if screen else (1920, 1080)
    guest.mouse_to((x + 30) * 32767 // (sw - 1), (y + h // 2) * 32767 // (sh - 1))
    time.sleep(0.3)
    guest.mouse_button(True, "right")
    time.sleep(0.1)
    guest.mouse_button(False, "right")
    at = line(guest, r"wm: menu of [^\r\n]* at (-?\d+),(-?\d+) (\d+)x(\d+)", mark, 30)
    if not at:
        fails.append("the Kosmos menu never opened")
        return None
    mx, my, mw, mh = (int(at.group(i)) for i in range(1, 5))
    time.sleep(2)
    return frosted(guest, "menu", "at %d,%d %dx%d" % (mx, my, mw, mh),
                   mx + mw // 2, mx + mw - 12, my + 5, my - 8, 0.4, fails)


def window_frosted(guest, mark, fails):
    """About, a window the kit draws: a row 52 under its top - below its
    header, above its first heading - over its right half, against the
    wallpaper's row 8 above the window."""
    at = line(guest, r"wm: window About Kosmos at (-?\d+),(-?\d+) (\d+)x(\d+)", mark, 30)
    if not at:
        fails.append("About never opened")
        return None
    x, y, w, h = (int(at.group(i)) for i in range(1, 5))
    return frosted(guest, "window", "at %d,%d %dx%d" % (x, y, w, h),
                   x + w // 2, x + w - 20, y + 52, y - 8, 0.4, fails)


def rim_lit(guest, mark, fails):
    """The rim's light on About (LG3): over its header, which is one solid
    colour, so the light is all that differs down the column."""
    at = re.search(r"wm: window About Kosmos at (-?\d+),(-?\d+) (\d+)x(\d+)", guest.seen[mark:])
    if not at:
        return
    x, y, w, h = (int(at.group(i)) for i in range(1, 5))
    shot = open("build/glass/window.ppm", "rb").read()
    head = shot.split(b"\n", 3)
    sw, _ = (int(v) for v in head[1].split())
    data = head[3]

    def bright(px, py):
        o = (py * sw + px) * 3
        return sum(data[o:o + 3])

    def warmth(px, py):
        o = (py * sw + px) * 3
        return data[o] - data[o + 2]

    top, under = bright(x + w // 2, y), bright(x + w // 2, y + 12)
    right, inside = bright(x + w - 1, y + h // 2), bright(x + w - 6, y + h // 2)
    print("rim: the header's top %d, 12 rows down %d; the right edge %d, 5 in %d"
          % (top, under, right, inside))
    if not (top > under + 90 and right <= inside + 6):
        fails.append("the rim is not lit from the upper left: the header's top %d against %d "
                     "12 rows down, the right edge %d against %d 5 in"
                     % (top, under, right, inside))

    warm, plain = warmth(x + w // 2, y), warmth(x + w // 2, y + 12)
    print("rim: in front, red over blue %d at the top, %d 12 rows down" % (warm, plain))
    if not warm > plain + 30:
        fails.append("the window in front's rim is not warm: red over blue %d at its top, "
                     "%d in its header" % (warm, plain))


def plate_glass(guest, mark, fails):
    """The amber plate on About (LG5), worn by a look rather than a desktop:
    amber glass, so its colour is the amber laid over the header at 0xc8 -
    warm, and not the opaque #f2a93b a plate of paint would be - over Night's
    dark header a deeper gold, its red down and its blue near paint's. Read on a
    row below its words and short of its slanted edge."""
    win = re.search(r"wm: window About Kosmos at (-?\d+),(-?\d+) (\d+)x(\d+)", guest.seen[mark:])
    plate = re.search(r"wm: About Kosmos's plate at (-?\d+),(-?\d+) (\d+)x(\d+)", guest.seen)
    if not plate:
        fails.append("About wears no plate under Liquid Glass")
        return
    if not win:
        return
    wx, wy = int(win.group(1)), int(win.group(2))
    px, py, pw, ph = (int(plate.group(i)) for i in range(1, 5))
    shot = open("build/glass/window.ppm", "rb").read()
    head = shot.split(b"\n", 3)
    sw, _ = (int(v) for v in head[1].split())
    data = head[3]
    y = wy + py + ph - 4
    xs = range(wx + px + 6, wx + px + pw - 30)
    r = sum(data[(y * sw + x) * 3] for x in xs) / len(xs)
    b = sum(data[(y * sw + x) * 3 + 2] for x in xs) / len(xs)
    print("plate: About's at %d,%d %dx%d, red %.0f and blue %.0f along its foot"
          % (px, py, pw, ph, r, b))
    if not (r > b + 60 and max(abs(r - 0xf2), abs(b - 0x3b)) > 12):
        fails.append("About's plate is not amber glass: red %.0f, blue %.0f along its foot, "
                     "where paint is 242 and 59" % (r, b))


def focus_damage(guest, fails):
    """LG5's test: a change of focus composes the window coming forward
    whole and, of the one going back, only what says it is behind - its
    shadow, its lights, its plate and its rim. Ten raises, About and
    Calculator in turn, Calculator moved clear of About - where they overlap,
    damage merged covers both whichever is damaged whole, and the control
    passed: what was composed, against the two windows' areas.
    Damaged whole as before, each raise would be both windows and their
    shadows; the bound is the one coming forward with its shadow and half
    the other's bare area, and the dock's once for its glints. Measured
    3.48 Mpx for ten raises against a bound of 3.97; the control, the window
    going back damaged whole, 4.93."""
    said = line(guest, r"GLASSFOCUS (\d+) (\d+)", 0, 60)
    seen = guest.seen
    about = re.findall(r"wm: window About Kosmos at -?\d+,-?\d+ (\d+)x(\d+)", seen)
    calc = re.findall(r"wm: window Calculator at -?\d+,-?\d+ (\d+)x(\d+)", seen)
    dock = re.findall(r"wm: the dock at -?\d+,-?\d+ (\d+)x(\d+)", seen)
    if not (said and about and calc):
        fails.append("the changes of focus were not measured: %r" % (said and said.group(0)))
        return
    px = int(said.group(1))
    aw, ah = (int(v) for v in about[-1])
    cw, ch = (int(v) for v in calc[-1])
    dw, dh = (int(v) for v in dock[-1]) if dock else (0, 0)
    sh = 24

    def shadowed(w, h):
        return (w + 2 * sh) * (h + 2 * sh)

    bound = (5 * (shadowed(aw, ah) + shadowed(cw, ch)) + int(0.5 * 5 * (aw * ah + cw * ch))
             + 10 * dw * dh)
    whole = 5 * 2 * (shadowed(aw, ah) + shadowed(cw, ch))
    print("focus: ten raises composed %d px; whole windows would be %d, the bound %d"
          % (px, whole, bound))
    if not px < bound:
        fails.append("a change of focus composed %d px for ten raises, over the %d of the "
                     "window forward and the parts of the one behind" % (px, bound))


# The card's own measures (`notifications.lua`): the banners' window has 8
# round the cards, a card's words start 12 down, and are 372 wide.
EDGE, PAD_Y, CARD_W = 8, 12, 372


def banner_frosted(guest, at, fails):
    """The banner: a row of its card above its words, against the
    wallpaper's just under the banners' window - to a tenth, not the dock's
    two fifths. A card lies on its hairline, and the two leave the sharp
    wallpaper only a fifth of its roughness without any glass: 3.0 against
    14.3 when the control was run, and 0.6 with the glass."""
    if not at:
        fails.append("the banner never said where it is")
        return None
    x, y, w, h = (int(at.group(i)) for i in range(1, 5))
    return frosted(guest, "banner", "at %d,%d %dx%d" % (x, y, w, h),
                   x + EDGE + 60, x + EDGE + CARD_W - 30, y + EDGE + PAD_Y // 2,
                   y + h + 8, 0.1, fails)


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
                   'wallpaper = "wallpaper/paint-storm.jpg", wallpaper_fit = "fill", '
                   'bar = "dock", dock = "floating" }), '
                   'fs.write("/Home/Preferences/startup", { items = { "/Temporary/glassswap.lua", '
                   '"/Temporary/glassbanner.lua", "/Kosmos/Apps/about.lua", '
                   '"/Temporary/glassfocus.lua" } }))'
                   % lua_string(SWAP.replace("\n", " ")))
        guest.wait_for_line("PREFS", "the appearance and startup settings", mark)
        mark = len(guest.seen)
        guest.type('print("BANNERFILE", fs.write("/Temporary/glassbanner.lua", %s))'
                   % lua_string(BANNER.replace("\n", " ")))
        guest.wait_for_line("BANNERFILE", "the banner's script", mark)
        mark = len(guest.seen)
        guest.type('print("FOCUSFILE", fs.write("/Temporary/glassfocus.lua", %s))'
                   % lua_string(FOCUS.replace("\n", " ")))
        guest.wait_for_line("FOCUSFILE", "the focus script", mark)
        mark = len(guest.seen)
        guest.type("wm")

        screen = line(guest, r"wm: the window server answers, the screen (\d+)x(\d+)", mark, 120)
        made = line(guest, r"wm: the wallpaper's glass, (\d+)x(\d+), three passes at (\d+), "
                           r"in (\d+) ms", mark, 60)
        dockat = line(guest, r"wm: window Deskbar at (-?\d+),(-?\d+) (\d+)x(\d+)", mark, 60)
        swapped = line(guest, r"GLASSSWAP liquidglassnight (\S+)", mark, 60)
        # The dock looked at in the look the program left - Liquid Glass
        # Night, glass again - once its glass is made and the dock redrawn.
        line(guest, r"wm: the wallpaper's glass given back[\s\S]*wm: the wallpaper's glass, ",
             mark, 30)
        time.sleep(3)
        dock_frosted(guest, dockat, fails)
        banner_at = line(guest, r"notifications: banners at (-?\d+),(-?\d+) (\d+)x(\d+)", mark, 30)
        time.sleep(2)
        banner_frosted(guest, banner_at, fails)
        window_frosted(guest, mark, fails)
        rim_lit(guest, mark, fails)
        plate_glass(guest, mark, fails)
        menu_frosted(guest, screen, len(guest.seen), fails)
        time.sleep(2)
        focus_damage(guest, fails)
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
          "Night; the dock, a banner, a window and a menu frosted; a window's rim lit from the upper left, warm in front; nothing stopped)." % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
