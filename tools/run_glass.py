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

CHECKS = 19

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

# Ten changes of focus between About and Passwords under `profile`, and
# what was composed for them, once the start is over - the banner up and
# the Desktops chooser gone, since the chooser closing whole inside the
# measure read 13.6 Mpx on 18 November, when the network made the start
# slower. It is the last of the startup items, so that it is
# over before anything else is looked at: Passwords, its sidebar looked at
# in the five seconds it is held in front after, closed and About in front
# again. Typed as BANNER is, with no comments. (Typed after the
# desktop is up, a line goes to the window in front as keys.)
FOCUS = r'''
local S = function(m) return fs.send("/Running/wm", m) end
local P = use("/Kosmos/Libraries/wmproto.lua")
S{ type = "launch", program = "/Kosmos/Apps/passwords.lua" }
local a, c, n, d
for _ = 1, 200 do
  d = nil
  for _, w in ipairs((P.windows() or {}).windows or {}) do
    if w.title == "About Kosmos" then a = w.handle end
    if w.title == "Passwords" then c = w.handle end
    if w.title == "Notifications" then n = 1 end
    if w.title == "Desktops" then d = 1 end
  end
  if a and c and n and not d then break end
  sys.sleep(5)
end
S{ type = "move", window = c, x = 978, y = 200 }
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
print("GLASSFOCUS", p.px, p.frames)
sys.sleep(500)
S{ type = "close", window = c }
S{ type = "raise", window = a }
print("GLASSDONE")
'''


def line(guest, pattern, mark, seconds=60):
    end = time.time() + seconds
    while time.time() < end:
        guest._read_available()
        # Only whole lines: one still arriving can match short
        # (testing.md 18.939).
        text = guest.seen[mark:]
        m = re.search(pattern, text[:text.rfind("\n") + 1])
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


def rims_said(guest, mark, dockat, fails):
    """LG4's rims on the floating surfaces: the dock and a banner each told
    the window manager where their glass is, so the scene bends and lights
    a rim there - the dock's the whole pill, the dock's own size, and the
    banner's its card, inside its margin (`tools/test_draw.c` holds what
    the scene draws from it)."""
    seen = guest.seen[mark:]
    pill = re.findall(r"wm: Deskbar's glass at (\d+),(\d+) (\d+)x(\d+), round (\d+)", seen)
    card = re.findall(r"wm: Notifications's glass at (\d+),(\d+) (\d+)x(\d+), round (\d+)", seen)
    print("rims: the dock's %r, a banner's %r" % (pill[-1:], card[-1:]))
    w, h = (int(dockat.group(3)), int(dockat.group(4))) if dockat else (None, None)
    if not (pill and int(pill[-1][0]) == 0 and int(pill[-1][1]) == 0
            and (int(pill[-1][2]), int(pill[-1][3])) == (w, h) and int(pill[-1][4]) > 0):
        fails.append("the dock did not say its glass is its whole pill, %r by %r: %r"
                     % (w, h, pill[-1:]))
    if not (card and int(card[-1][0]) > 0 and int(card[-1][1]) > 0 and int(card[-1][4]) > 0):
        fails.append("a banner did not say its glass is its card, inside its margin: %r" % card[-1:])


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
    """No plate on About: the plate is a desktop's style, and Astra's says
    none, whatever look it wears (Diego, 18 November 2026: "the theme is
    colors and transparency"). Until 0.11.496 Liquid Glass brought one, and
    this checked it was amber glass; that plate is Starlight's desktop's
    alone now, worn under a glass look, and its check there is owed
    (`roadmap.md`, Liquid Glass)."""
    plate = re.search(r"wm: About Kosmos's plate at (-?\d+),(-?\d+) (\d+)x(\d+)", guest.seen)
    if plate:
        fails.append("About wears a plate on Astra under Liquid Glass - Starlight's style "
                     "from a look: %s" % plate.group(0))


def sidebar_frosted(guest, fails):
    """A sidebar's ground (LG4's test): the kit's `panel`, the look's own
    tint under glass - Passwords', held in front at 978,200 by the focus
    script for five seconds. A row half way down it, below its two rows,
    against the wallpaper just above the window."""
    at = re.findall(r"wm: window Passwords at -?\d+,-?\d+ (\d+)x(\d+)", guest.seen)
    if not at:
        fails.append("Passwords never opened for its sidebar")
        return
    w, h = (int(v) for v in at[-1])
    x, y = 978, 200
    frosted(guest, "sidebar", "of Passwords at %d,%d %dx%d" % (x, y, w, h),
            x + 12, x + 188, y + h // 2, y - 8, 0.4, fails)


def focus_damage(guest, fails):
    """LG5's test: a change of focus composes the window coming forward
    whole and, of the one going back, only what says it is behind - its
    shadow, its lights, its plate and its rim. Ten raises, About and
    Passwords in turn, Passwords moved clear of About - where they overlap,
    damage merged covers both whichever is damaged whole, and the control
    passed: what was composed, against the two windows' areas.
    Damaged whole as before, each raise would be both windows and their
    shadows; the bound is the one coming forward with its shadow and half
    the other's bare area, and the dock's once for its glints. Measured
    3.48 Mpx for ten raises against a bound of 3.97; the control, the window
    going back damaged whole, 4.93."""
    said = line(guest, r"GLASSFOCUS\s+(\d+)\s+(\d+)", 0, 60)
    seen = guest.seen
    about = re.findall(r"wm: window About Kosmos at -?\d+,-?\d+ (\d+)x(\d+)", seen)
    calc = re.findall(r"wm: window Passwords at -?\d+,-?\d+ (\d+)x(\d+)", seen)
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


# **Live glass** (LG6): About moved so that its lower part is under the
# dock, over Telnet once every other check has looked at it.
LIVE_MOVE = r'''
local S = function(m) return fs.send("/Running/wm", m) end
local P = use("/Kosmos/Libraries/wmproto.lua")
local a
for _, w in ipairs((P.windows() or {}).windows or {}) do
  if w.title == "About Kosmos" then a = w.handle end
end
local d = S{ type = "dock" } or {}
S{ type = "raise", window = a }
local m = S{ type = "move", window = a, x = (d.x or 900) - 100, y = (d.y or 900) - 160 } or {}
print("LIVEAT", m.x, m.y)
'''

LIVE_ON = r'''
print("LIVEON", fs.send("/Running/wm", { type = "theme", live_glass = true }).ok)
'''

# Ten small moves of About under the dock with live glass on, under
# `profile`: what was composed, and that it was composed at all.
LIVE_MOVES = r'''
local S = function(m) return fs.send("/Running/wm", m) end
local P = use("/Kosmos/Libraries/wmproto.lua")
local a
for _, w in ipairs((P.windows() or {}).windows or {}) do
  if w.title == "About Kosmos" then a = w.handle end
end
local at = S{ type = "move", window = a } or {}
S{ type = "profile", on = true }
for i = 1, 10 do
  S{ type = "move", window = a, x = at.x + i * 4, y = at.y }
  sys.sleep(3)
end
local p = S{ type = "profile" } or {}
S{ type = "profile", on = false }
S{ type = "theme", live_glass = false }
print("LIVEMOVES", p.px, p.frames)
'''


def screen_at(guest, what):
    shot = guest.screendump()
    os.makedirs("build/glass", exist_ok=True)
    open("build/glass/%s.ppm" % what, "wb").write(shot)
    head = shot.split(b"\n", 3)
    sw, _ = (int(v) for v in head[1].split())
    data = head[3]

    def at(px, py):
        o = (py * sw + px) * 3
        return tuple(data[o:o + 3])

    return at


def differs(a, b, x0, x1, y):
    """How far two pictures are apart along a row: the mean, over its
    pixels, of the largest of the three channels' differences."""
    total = 0
    for x in range(x0, x1):
        total += max(abs(p - q) for p, q in zip(a(x, y), b(x, y)))
    return total / max(1, x1 - x0)


def live_glass(guest, session, dockat, fails):
    """LG6's test, on the screen: the dock's glass over About, whose ground
    is darker than the wallpaper under it - wallpaper glass shows the
    wallpaper's blur there whatever is under the dock, live glass the
    window. So the same row of the dock, the two ways, is far apart over
    About and close over the wallpaper beside it, where both are the
    wallpaper blurred at the same reach. Then About moved under the dock
    ten times with live glass on: composed, and nothing stopped."""
    if not dockat:
        fails.append("live glass: the dock never said where it is")
        return
    dx, dy, dw, dh = (int(dockat.group(i)) for i in range(1, 5))
    said = run_script(guest, session, "livemove", LIVE_MOVE)
    placed = re.search(r"LIVEAT\s+(-?\d+)\s+(-?\d+)", said)
    about = re.findall(r"wm: window About Kosmos at -?\d+,-?\d+ (\d+)x(\d+)", guest.seen)
    if not (placed and about):
        fails.append("live glass: About was not moved under the dock: %r" % said[-200:])
        return
    ax, ay = int(placed.group(1)), int(placed.group(2))
    aw, ah = (int(v) for v in about[-1])
    row = dy + 4
    x0, x1 = max(ax + 16, dx + dw // 8), min(ax + aw - 16, dx + 7 * dw // 8)
    # The wallpaper beside it: the dock between the Kosmos button, which is
    # opaque - on 18 November the first try read it and passed on 0.0 - and
    # About, kept further than the blur's reach from both.
    w0, w1 = dx + 160, ax - 40
    if not (ay + ah > row + 2 and x1 - x0 >= 40 and w1 - w0 >= 30):
        fails.append("live glass: About at %d,%d %dx%d does not lie under the dock at %d,%d %dx%d "
                     "with wallpaper beside it" % (ax, ay, aw, ah, dx, dy, dw, dh))
        return
    # The pointer off the dock and the menu closed by a press on the
    # desktop: while either holds it the Deskbar keeps its lens over the
    # dock, and the lens covers the dock, not what is under it.
    guest.mouse_to(16383, 300 * 32767 // 1079)
    time.sleep(0.5)
    guest.mouse_button(True)
    time.sleep(0.1)
    guest.mouse_button(False)
    time.sleep(4)
    before = screen_at(guest, "live-off")
    mark = len(guest.seen)
    run_script(guest, session, "liveon", LIVE_ON)
    if not line(guest, r"wm: live glass on", mark, 30):
        fails.append("live glass: the window manager never said it was on")
        return
    time.sleep(3)
    after = screen_at(guest, "live-on")
    over = differs(before, after, x0, x1, row)
    beside = differs(before, after, w0, w1, row)
    print("live: the dock's row %d over About (x %d-%d) moved %.1f with live glass; over the "
          "wallpaper (x %d-%d) %.1f" % (row, x0, x1, over, w0, w1, beside))
    if not (over > 12 and beside < 6):
        fails.append("live glass: the dock over About changed %.1f and over the wallpaper %.1f; "
                     "live, the first is the window under it and the second the same blur"
                     % (over, beside))
    said = run_script(guest, session, "livemoves", LIVE_MOVES)
    moved = re.search(r"LIVEMOVES\s+(\d+)\s+(\d+)", said)
    print("live: ten moves under the dock: %r" % (moved and moved.group(0)))
    if not (moved and int(moved.group(2)) > 0):
        fails.append("live glass: About moved under the dock was not composed: %r" % said[-200:])


def run_script(guest, session, name, text):
    """A script put on the machine and run there; what it printed, on the
    session or on the serial line, whichever it reached."""
    path = "/Temporary/%s.lua" % name
    session.put(text.encode(), path)
    mark = len(guest.seen)
    said = session.run(path).decode(errors="replace")
    time.sleep(0.5)
    guest._read_available()
    return said + guest.seen[mark:]


def one_line(script):
    """A script as one typed line: its lines' indentation dropped, since
    the console takes 1024 characters and the focus script came near."""
    return " ".join(l.strip() for l in script.splitlines() if l.strip())


def lua_string(text):
    return "[==[" + text + "]==]"


def run(fails):
    image = "build/x86_64/kosmos.elf" if len(sys.argv) < 2 else sys.argv[1]
    import run_gallery
    telnet = run_gallery.with_telnet()
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
                   % lua_string(one_line(SWAP)))
        guest.wait_for_line("PREFS", "the appearance and startup settings", mark)
        mark = len(guest.seen)
        guest.type('print("BANNERFILE", fs.write("/Temporary/glassbanner.lua", %s))'
                   % lua_string(one_line(BANNER)))
        guest.wait_for_line("BANNERFILE", "the banner's script", mark)
        mark = len(guest.seen)
        guest.type('print("FOCUSFILE", fs.write("/Temporary/glassfocus.lua", %s))'
                   % lua_string(one_line(FOCUS)))
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
        # And the changes of focus over, the last startup item: Passwords
        # opened, given a cell in the dock and closed again. Under a loaded
        # gate the dock was looked at while it held the cell, on 18
        # November, so it is read where it is now, not where it first was.
        line(guest, r"GLASSFOCUS\s", mark, 90)
        sidebar_frosted(guest, fails)
        line(guest, r"GLASSDONE", mark, 30)
        time.sleep(3)
        guest._read_available()
        last = None
        for last in re.finditer(r"wm: (?:the dock|window Deskbar) at (-?\d+),(-?\d+) (\d+)x(\d+)",
                                guest.seen[mark:]):
            pass
        dock_frosted(guest, last or dockat, fails)
        banner_at = line(guest, r"notifications: banners at (-?\d+),(-?\d+) (\d+)x(\d+)", mark, 30)
        time.sleep(2)
        banner_frosted(guest, banner_at, fails)
        rims_said(guest, mark, last or dockat, fails)
        window_frosted(guest, mark, fails)
        rim_lit(guest, mark, fails)
        plate_glass(guest, mark, fails)
        menu_frosted(guest, screen, len(guest.seen), fails)
        time.sleep(2)
        focus_damage(guest, fails)
        import run_servers as S
        session = S.connect(telnet)
        live_glass(guest, session, last or dockat, fails)
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
          "Night; the dock, a banner, a window and a menu frosted; a window's rim lit from the upper left, warm in front; live glass over About under the dock, and the same blur over the wallpaper; nothing stopped)." % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
