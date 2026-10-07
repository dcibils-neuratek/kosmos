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
            if ch == " ":
                self.key("spc")
            else:
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

    def at(self, where):
        return (int(n) for n in re.match(r"at (-?\d+),(-?\d+)", where or "at 180,140").groups())

    def lesson2(self):
        """The converter run in the IDE, beside its code."""
        g = self.guest
        mark = self.mark()
        g.type("wm ide:/Kosmos/Tutorial/02-Converter/converter.lua")
        self.expect("ide: opened /Kosmos/Tutorial/02-Converter/converter.lua", mark, 120,
                    "the IDE did not open lesson 2's project")
        time.sleep(3)
        mark = self.mark()
        self.key("f5")
        self.expect("wm: window Converter", mark, 60, "F5 did not open the converter")
        time.sleep(3)
        self.screen()
        self.save("converter-ide.png", (IDE_X - 4, IDE_Y - 26, IDE_X + IDE_W + 4, IDE_Y + IDE_H + 4),
                  halve=True)

    def lines(self, words):
        """Words typed into whatever has the focus, one a line."""
        for i, word in enumerate(words):
            if i:
                self.key("ret")
            self.typed(word)

    def lesson3(self):
        """Notes run in the IDE, a note being written in it."""
        g = self.guest
        mark = self.mark()
        g.type("wm ide:/Kosmos/Tutorial/03-Notes/notes.lua")
        self.expect("ide: opened /Kosmos/Tutorial/03-Notes/notes.lua", mark, 120,
                    "the IDE did not open lesson 3's project")
        time.sleep(3)
        mark = self.mark()
        self.key("f5")
        where = self.expect("wm: window Notes", mark, 60, "F5 did not open Notes")
        time.sleep(3)
        self.screen()
        nx, ny = self.at(where)
        self.click(nx + 120, ny + 120)
        self.lines(["Ideas", "a clock that rings"])
        time.sleep(1)
        self.screen()
        # The whole screen: Notes opens where the desktop finds room, which
        # is beside the IDE rather than over it.
        self.save("notes-ide.png", (0, 0, self.W, self.H), halve=True)

    def lesson4(self):
        """What's Running run in the IDE, and the finished one on its own."""
        g = self.guest
        g.wait_for("kosmos> ", "a prompt")
        mark = self.mark()
        g.type("wm ide:/Kosmos/Tutorial/04-Running/running.lua")
        self.expect("ide: opened /Kosmos/Tutorial/04-Running/running.lua", mark, 120,
                    "the IDE did not open lesson 4's project")
        time.sleep(3)
        mark = self.mark()
        self.key("f5")
        self.expect("wm: window What's Running", mark, 60, "F5 did not open What's Running")
        time.sleep(4)
        self.screen()
        self.save("whats-running-ide.png", (0, 0, self.W, self.H), halve=True)

    def running(self):
        g = self.guest
        g.wait_for("kosmos> ", "a prompt")
        mark = self.mark()
        g.type("wm /Kosmos/Tutorial/04-Running/running.lua")
        where = self.expect("wm: window What's Running", mark, 120,
                            "the finished What's Running did not open")
        self.expect("running: looked 3 times", mark, 60,
                    "What's Running did not look again on its clock")
        self.screen()
        x, y = self.at(where)
        self.save("whats-running.png", (x - 12, y - 34, x + 420 + 12, y + 360 + 12))

    def notes(self):
        """The finished Notes: a list typed, saved through the Save window,
        and the window pictured holding it."""
        self.guest.wait_for("kosmos> ", "a prompt")
        mark = self.mark()
        self.guest.type("wm /Kosmos/Tutorial/03-Notes/notes.lua")
        where = self.expect("wm: window Notes", mark, 120, "the finished Notes did not open")
        time.sleep(3)
        self.screen()
        nx, ny = self.at(where)
        self.click(nx + 120, ny + 120)
        self.lines(["Shopping", "bread", "lemons", "olives", "coffee"])
        time.sleep(1)
        self.click(nx + 176 + 30, ny + 12 + 12)
        self.expect("wm: window Save", mark, 30, "Save did not open the Save window")
        time.sleep(2)
        self.key("ret")
        self.expect("notes: saved 5 lines to /Home/Notes/Untitled.note", mark, 30,
                    "Return in the Save window did not save the note")
        time.sleep(2)
        self.point(nx + 600, ny + 300)
        self.screen()
        self.save("notes.png", (nx - 12, ny - 34, nx + 560 + 12, ny + 420 + 2))

    def counter(self):
        g = self.guest
        mark = self.mark()
        g.type("wm /Kosmos/Tutorial/01-Counter/counter.lua,/Kosmos/Tutorial/02-Converter/converter.lua")
        where = self.expect("wm: window Counter", mark, 120, "the finished Counter did not open")
        there = self.expect("wm: window Converter", mark, 120, "the finished Converter did not open")
        self.expect("converter: 20 is 68.00 F", mark, 60, "the converter did not turn 20 C into 68 F")
        time.sleep(3)
        self.screen()
        cx, cy = self.at(there)
        self.save("converter.png", (cx - 12, cy - 34, cx + 440 + 12, cy + 230 + 12))
        wx, wy = self.at(where)

        for _ in range(3):
            self.click(wx + 24 + 40, wy + 96 + 14)

        self.expect("counter: pressed 3", mark, 20, "Counter did not count to three")
        time.sleep(1)
        self.screen()
        self.save("counter.png", (wx - 12, wy - 34, wx + 360 + 12, wy + 200 + 12))

    def run(self):
        self.guest.wait_for("kosmos> ", "a prompt")
        self.lesson1()

    def run2(self):
        self.guest.wait_for("kosmos> ", "a prompt")
        self.lesson2()

    def run3(self):
        self.guest.wait_for("kosmos> ", "a prompt")
        self.lesson3()


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.strip().splitlines()[-1])

    image, out = sys.argv[1], sys.argv[2]
    problems = []

    for step in ("notes", "lesson4", "running", "run", "run2", "run3", "counter"):
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

    print("PASS: lessons 1 to 4 followed as their pages give them; the pictures are in " + out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
