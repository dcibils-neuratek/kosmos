#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Game Kit's rasterizer, held to the portable one it was ported from.

`user/kits/game/gamesoft.c` is about nine hundred lines of numeric C, and every
one of them exists to draw the picture `user/lib/solar/soft.lua` already
drew - the same `floor` in the same three places, the same coverage on an
anti-aliased line, the same texel out of the same equirectangular lookup.
It is six to eight times faster, and that is worth nothing if it is six to
eight times faster at drawing something else.

So the check is not a screenshot and not a hash. `solar --compare` runs
every primitive through both rasterizers with identical arguments and then
compares all 368,640 pixels, with no tolerance: one channel out by one is
a failure. A rounding difference that shows on a single pixel here is the
one that shows on a hundred thousand when a planet fills the screen.

**And it counts how much of the frame was drawn on**, because two
rasterizers that drew nothing agree perfectly. The guest refuses to call
that a pass, and this insists on seeing the number - which is the same
mistake `check_default_look` made by comparing a table with itself.

The negative control is on the record rather than in the suite: changing
one constant in `S:point`'s corner weight from 0.25 to 0.35 made it report
exactly five differing pixels, naming the first. Five is right - the four
corners of the size-3 point at 80,40, plus the single on-screen corner of
the one at the origin - which is how a check of this shape is shown to
bite rather than merely to pass.

It needs no disk and no textures: the flat-shaded path covers every line
of the shading except the three that index a texture string, and a wrong
texel is not a thing that hides on screen.

**And then it opens the window, because the comparison cannot see one.**
The first build of this passed the pixel comparison and showed a *black
window* at a healthy forty-three frames a second: the host had hoisted
`win:surface()` out of its loop, and the window is double-buffered, so
every frame went into the buffer that had just been shown. The rasterizer
was perfect and nothing reached the screen.

That is a whole class the comparison is blind to by construction - it
checks what is drawn, never where it lands - so the second half of this
suite boots a machine with a display, opens the app, and looks at the
window. `testing.md` 18.129.
"""

import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])

import run_disk                                        # noqa: E402
import run_screenshot                                  # noqa: E402
import scratch                                         # noqa: E402


class Failure(Exception):
    pass


# Where `solar.lua` puts its window, and the size it renders at.
WIN_X, WIN_Y, WIN_W, WIN_H = 80, 70, 960, 540


def on_screen(image):
    """The app's window, looked at. Returns the number of checks made.

    **Read from the window's own rectangle, not from the whole screen.**
    The desktop behind it is a photograph of mountains and would pass any
    "there are colours here" test on its own, which is the shape of check
    that catches nothing.
    """
    guest = run_screenshot.Guest(image, 40)

    try:
        guest.wait_for("kosmos>", "a shell prompt")
        guest.proc.stdin.write(b"wm solar\n")

        # It has to report a frame rate before there is any point looking:
        # the window exists well before the first frame reaches it.
        guest.wait_for("frames a second", "the app to draw a frame")

        width, height, at = run_screenshot.pixel_reader(guest.screendump())

        if width < WIN_X + WIN_W or height < WIN_Y + WIN_H:
            raise Failure(f"the screen is {width}x{height}, which does not "
                          "hold the window this looks inside")

        # Inside the frame and below the title bar, so nothing counted here
        # is the window manager's chrome.
        x0, y0 = WIN_X + 4, WIN_Y + 4
        x1, y1 = WIN_X + WIN_W - 4, WIN_Y + WIN_H - 4

        lit, seen = 0, set()

        for y in range(y0, y1, 3):
            for x in range(x0, x1, 3):
                p = at(x, y)
                seen.add(p)

                if max(p) > 24:
                    lit += 1

        #
        # **The black-window bug this exists for made `lit` zero**, because
        # every pixel in the rectangle was the compositor's cleared buffer.
        # Space is mostly black, so the bar is low on purpose - the HUD
        # panels and their text alone are far more than this.
        #
        if lit < 2000:
            raise Failure(f"only {lit} pixels inside the window are lit, so "
                          "it is showing the black of a buffer nothing was "
                          "drawn into")

        # And more than a handful of distinct colours, which a cleared
        # buffer with one stray panel on it would not have.
        if len(seen) < 50:
            raise Failure(f"the window holds only {len(seen)} distinct "
                          "colours, which is not a rendered frame")

        return 2
    finally:
        guest.close()


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    checks = 0

    # `boot` wants a disk and never formats this one, so nothing here can
    # reach a filesystem - which is the point: the rasterizer has no
    # business needing storage to be correct.
    disk = scratch.disk("game.img", 4 * 1024 * 1024)

    try:
        out = run_disk.boot(image, disk, ["solar --compare"], each=300)

        said = out.split("solar --compare")[-1]

        if "solar: PASS" not in said:
            raise Failure("the C rasterizer does not draw the Lua "
                          "rasterizer's picture.\n" + said[-1500:])

        checks += 1

        # The line it passed on, read rather than trusted: a pass with a
        # low `drawn` is a script that stopped drawing, and a pass with a
        # low pixel count is a comparison that shrank.
        counted = None

        for line in said.splitlines():
            if "drawn on" in line and "differ" in line:
                counted = line

        if not counted:
            raise Failure("--compare did not say what it compared.\n"
                          + said[-1500:])

        checks += 1

        # "compared 368640 pixels of 720x512, 105346 drawn on, 0 differ"
        words = counted.replace(",", " ").split()
        pixels = int(words[words.index("compared") + 1])
        drawn = int(words[words.index("drawn") - 1])

        if pixels < 300000:
            raise Failure(f"only {pixels} pixels were compared, which is "
                          "less of a frame than this has always checked")

        checks += 1

        # A fifth of the frame is what the script covers today. Held to a
        # tenth here so it is a floor rather than a number to keep in step
        # with every primitive added to the script.
        if drawn < pixels // 10:
            raise Failure(f"only {drawn} of {pixels} pixels were drawn on, "
                          "so the two rasterizers agreeing means little")

        checks += 1

        checks += on_screen(image)

        print(f"PASS: {checks} checks on the Game Kit's rasterizer "
              f"({pixels} pixels compared against the portable renderer, "
              f"{drawn} of them drawn on, none differing - clear, lines, "
              "points, rects, frames, circles, text, and spheres, rings "
              "and the sun at two block sizes - and the app's own window "
              "holding a picture rather than the black one a stale back "
              "buffer gives).")
        return 0
    except Failure as e:
        print(f"FAIL: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
