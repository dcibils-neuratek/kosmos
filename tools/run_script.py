#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Cafesa3D's Script panel (`roadmap.md` 4l, 6n step 6).

Booted, `wm cafesa3d`, and then with QEMU's own keyboard:

  Shift F4        the panel opens beside the view, and the view narrows by
                  the panel's width
  Ctrl Enter      the sample, the drawing's staircase, runs: 24 steps and a
                  column and a lamp made, and what it printed shown
  Ctrl Enter      again: the same, replacing the 26 it made last time
  Escape, Ctrl Z  the whole run taken back, as one undo step
  a click, a line `scene.box{ sise = 1 }` typed on a new last line, run:
                  refused on its line - the box has no sise - nothing made
  a loop          the line erased and `while true do end` typed instead, run:
                  stopped by its budget of instructions, Cafesa3D still answering
  Escape, Z       the keyboard given back: Z is the shading key again
  Shift F4        the panel closed, the view as wide as it was

What Cafesa3D says in the log is what is checked; the editor in the panel
is the IDE's, drawn into Cafesa3D's own pixels (`ui.paint_view`), so every
key reaching it has crossed both.

Usage: run_script.py IMAGE
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                   # noqa: E402


def keys_for(text):
    """QEMU's names for typing `text`."""
    names = {" ": "spc", "(": "shift-9", ")": "shift-0", '"': "shift-apostrophe",
             "=": "equal", ".": "dot", "\n": "ret", "{": "shift-bracket_left",
             "}": "shift-bracket_right"}
    return [names.get(ch, ch) for ch in text]


def main():
    image = sys.argv[1]
    guest = R.Guest(image, 120)
    failed = []
    checks = 0

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    def said(text, since, seconds=30):
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            at = guest.seen.find(text, since)

            if at >= 0 and "\n" in guest.seen[at + len(text):]:
                return guest.seen[at + len(text):].split("\n", 1)[0].strip()

            time.sleep(0.1)

        return None

    def press(*names):
        for name in names:
            guest.sendkey(name)

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        guest.type("wm cafesa3d")

        opened = said("cafesa3d: 7 objects, ", mark, 90)
        wide = re.search(r"the view (\d+) by (\d+)", opened or "")

        if not wide:
            print("FAIL: Cafesa3D never opened.\n--- the guest said ---\n"
                  + guest.seen[mark:][-1500:])
            return 1

        view_w = int(wide.group(1))
        time.sleep(2)

        # Shift F4: the panel, and a narrower view.
        mark = len(guest.seen)
        press("shift-f4")
        panel = said("cafesa3d: script panel open, the view ", mark, 30)
        narrow = re.match(r"(\d+) by", panel or "")
        check(narrow is not None and int(narrow.group(1)) == view_w - 470,
              "Shift F4 did not open the panel and narrow the view by 470 from "
              "%d: %r" % (view_w, panel))
        time.sleep(2)

        # The sample, the staircase, run: made, and then made again in its
        # own place rather than on top of itself.
        mark = len(guest.seen)
        press("ctrl-ret")
        ran = said("cafesa3d: script ran in ", mark, 60)
        check(said("cafesa3d: script 24 steps, 4.3 m up", mark, 5) is not None
              and ran is not None and ran.endswith(": 25 objects and 1 lamp"),
              "Ctrl Enter did not make the staircase: %r" % ran)

        mark = len(guest.seen)
        press("ctrl-ret")
        again = said("cafesa3d: script ran in ", mark, 60)
        check(again is not None and again.endswith(
                  "25 objects and 1 lamp, replacing the 26 it made last time"),
              "a second run did not replace what the first made: %r" % again)

        # The keyboard given back, and Ctrl Z: the whole run, one step.
        mark = len(guest.seen)
        press("esc", "ctrl-z")
        check(said("cafesa3d: undid ", mark, 20) == "ran script",
              "Ctrl Z did not take back the run as one step")

        # A mistake, refused on its line: a click in the code gives it the
        # keyboard again, and a new last line asks for a field there is not.
        window = said("cafesa3d: window at ", 0, 5) or "0,0"
        ox, oy = (int(v) for v in window.split(","))
        code = re.search(r"cafesa3d: script code (\d+),(\d+) (\d+)x(\d+)",
                         guest.seen)
        width, height, _ = R.pixel_reader(guest.screendump())

        if code:
            cx, cy = int(code.group(1)) + 200, int(code.group(2)) + 12
            guest.mouse_to(*R._to_tablet(ox + cx, oy + cy, width, height))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.3)
            guest.mouse_button(False)
            time.sleep(0.5)

        press("ctrl-end", *keys_for("\nscene.box{ sise = 1 }"))
        mark = len(guest.seen)
        press("ctrl-ret")
        failed_line = said("cafesa3d: script line ", mark, 30)
        check(failed_line is not None and failed_line.startswith("25: ")
              and "the box has no sise" in failed_line
              and failed_line.endswith("nothing was made"),
              "a field the box has not got was not refused on line 25: %r" % failed_line)

        # A loop without end, stopped by its budget, and Cafesa3D answering.
        # A refusal puts the caret at the start of the line it names, so End
        # is on line 25 only if the caret went there. The line is erased
        # rather than undone: undo takes back a word at a time, as every
        # editor does, so one Ctrl Z would leave `scene.box{ sise = 1` in
        # front of the loop.
        press("end", *["backspace"] * len("scene.box{ sise = 1 }"))
        press(*keys_for("while true do end"))
        mark = len(guest.seen)
        started = time.monotonic()
        press("ctrl-ret")
        stopped = said("cafesa3d: script it ran too long", mark, 120)
        took = time.monotonic() - started
        check(stopped is not None,
              "a loop without end was not stopped by its budget")

        # The keyboard given back: Z is the shading key again.
        mark = len(guest.seen)
        press("esc", "z")
        check(said("cafesa3d: shading ", mark, 20) is not None,
              "Escape did not give the keyboard back - Z did not change the "
              "shading")

        # Closed: the view as wide as it was.
        mark = len(guest.seen)
        press("shift-f4")
        closed = said("cafesa3d: script panel closed, the view ", mark, 30)
        again = re.match(r"(\d+) by", closed or "")
        check(again is not None and int(again.group(1)) == view_w,
              "Shift F4 did not close the panel and widen the view back to %d: %r"
              % (view_w, closed))
    finally:
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on Cafesa3D's Script panel:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        said_lines = [l for l in guest.seen.splitlines() if "cafesa3d:" in l]
        print("--- what Cafesa3D said last ---\n" + "\n".join(said_lines[-25:]))
        return 1

    print("PASS: %d checks on Cafesa3D's Script panel (opened with Shift F4 beside "
          "a view narrower by its width; the staircase made, made again in its "
          "own place, and taken back with one Ctrl Z; a field the box has not "
          "got refused on its line with nothing made; a loop without end stopped "
          "by its budget in %.0f s, Cafesa3D answering; Escape giving the keyboard "
          "back; closed, the view as wide as it was)" % (checks, took))
    return 0


if __name__ == "__main__":
    sys.exit(main())
