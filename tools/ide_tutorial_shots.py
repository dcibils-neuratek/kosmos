#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The IDE tutorial's pictures, taken by following its pages in QEMU
(`docs/ide-tutorial/`, `docs/ide-tutorial.html`).

Lesson 1: New Project with Lua app and Hello Window chosen and the name
Counter typed; the project run with F5, its window pressed and the IDE's
Output beside it; and the finished counter, from `/Kosmos/Tutorial`. Each
step is checked as it is taken - a page that cannot be followed is a
failure, not a picture. Cafesa3D's `Shots` does the booting, the pointer and
the saving, at the page's width and with the licence line in each PNG.

Usage: ide_tutorial_shots.py IMAGE OUTDIR
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                   # noqa: E402
from cafesa3d_tutorial_shots import Shots                     # noqa: E402

# Where the IDE opens (`ide.lua`: 1180 by 760 at 70, 50) and its New Project
# window inside it (760 by 460, in the middle).
IDE_X, IDE_Y, IDE_W, IDE_H = 70, 50, 1180, 760
DW, DH = 760, 460


class IdeShots(Shots):
    def expect(self, text, since, seconds=60, what=None):
        got = self.said(text, since, seconds)

        if got is None:
            self.problems.append(what or ("never said %r" % text))

        return got

    def typed(self, word):
        for ch in word:
            self.key(ch.lower() if not ch.isupper() else "shift-" + ch.lower())
            time.sleep(0.08)

    def lesson1(self):
        g = self.guest
        mark = self.mark()
        g.type("wm ide:new")
        self.expect("wm: window Kosmos IDE", mark, 120, "the IDE did not open")
        time.sleep(4)
        self.screen()

        # The dialog, and in it Lua app - the first kind - and its template.
        x0, y0 = IDE_X + (IDE_W - DW) // 2, IDE_Y + (IDE_H - DH) // 2
        self.click(x0 + 24 + 30, y0 + 64 + 14)
        time.sleep(1)

        # The name: the field holds the template's; emptied and Counter typed.
        self.click(x0 + 110 + 60, y0 + 336 + 12)
        self.key("ctrl-a")                          # the name chosen whole
        self.typed("Counter")
        self.point(x0 + DW - 30, y0 + 30)          # out of the picture's way
        time.sleep(1)
        self.screen()
        self.save("new-project.png", (x0, y0, x0 + DW, y0 + DH))

        mark = self.mark()
        self.key("ret")
        self.expect("ide: opened /Home/Projects/Counter/hello.lua", mark, 30,
                    "Create did not make /Home/Projects/Counter with hello.lua open")
        time.sleep(2)

        # Run it, and press its button three times.
        mark = self.mark()
        self.key("f5")
        where = self.expect("wm: window Hello Window", mark, 60, "F5 did not open the window")
        time.sleep(3)
        self.screen()

        # Where the window manager put it - not always where it asked - and
        # its button pressed there. What it prints goes to the IDE's Output,
        # not this log: the picture shows it.
        wx, wy = (int(n) for n in re.match(r"at (-?\d+),(-?\d+)", where or "at 180,140").groups())

        for _ in range(3):
            self.click(wx + 24 + 40, wy + 92 + 14)

        time.sleep(1)
        self.screen()
        self.save("running.png", (IDE_X - 4, IDE_Y - 26, IDE_X + IDE_W + 4, IDE_Y + IDE_H + 4),
                  halve=True)

    def counter(self):
        g = self.guest
        mark = self.mark()
        g.type("wm /Kosmos/Tutorial/01-Counter/counter.lua")
        where = self.expect("wm: window Counter", mark, 120, "the finished Counter did not open")
        time.sleep(3)
        self.screen()
        wx, wy = (int(n) for n in re.match(r"at (-?\d+),(-?\d+)", where or "at 180,140").groups())

        for _ in range(3):
            self.click(wx + 24 + 40, wy + 96 + 14)

        self.expect("counter: pressed 3", mark, 20, "Counter did not count to three")
        time.sleep(1)
        self.screen()
        self.save("counter.png", (wx - 12, wy - 34, wx + 360 + 12, wy + 200 + 12))

    def run(self):
        self.guest.wait_for("kosmos> ", "a prompt")
        self.lesson1()


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.strip().splitlines()[-1])

    image, out = sys.argv[1], sys.argv[2]
    problems = []

    for step in ("run", "counter"):
        shots = IdeShots(image, out)

        try:
            if step == "counter":
                shots.guest.wait_for("kosmos> ", "a prompt")
            getattr(shots, step)()
        finally:
            shots.guest.close()

        problems += shots.problems

    if problems:
        print("FAIL: the tutorial could not be followed as written:")

        for p in problems:
            print("  " + p)

        return 1

    print("PASS: lesson 1 followed as its page gives it; the pictures are in " + out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
