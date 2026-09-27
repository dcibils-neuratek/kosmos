#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Cafesa3D's Script panel (`roadmap.md` 4l, 6n step 6).

Booted, `wm cafesa3d`, and then with QEMU's own keyboard:

  Shift F4        the panel opens beside the view, and the view narrows by
                  the panel's width
  Ctrl Enter      the sample script runs: what it prints comes to the strip
                  under the code, and "ran"
  a line typed    `error("boom")` at the end, run: the error on its line,
                  and nothing made
  Ctrl Z, a loop  `while true do end` on that line instead, run: stopped by its
                  budget of instructions, and Cafesa3D still answering
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
             "=": "equal", ".": "dot", "\n": "ret"}
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

        # The sample, run.
        mark = len(guest.seen)
        press("ctrl-ret")
        ran = said("cafesa3d: script ran: ", mark, 30)
        check(said("cafesa3d: script step 1 of 3", mark, 5) is not None
              and ran == "3 lines printed",
              "Ctrl Enter did not run the sample to its three lines: %r" % ran)

        # A mistake, on the line it is on.
        press("ctrl-end", *keys_for('\nerror("boom")'))
        mark = len(guest.seen)
        press("ctrl-ret")
        failed_line = said("cafesa3d: script line ", mark, 30)
        check(failed_line is not None and failed_line.startswith("8: ")
              and "boom" in failed_line and failed_line.endswith("nothing was made"),
              "error on line 8 was not said on its line: %r" % failed_line)

        # A loop without end, stopped by its budget, and Cafesa3D answering.
        press("ctrl-z")
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
        return 1

    print("PASS: %d checks on Cafesa3D's Script panel (opened with Shift F4 beside "
          "a view narrower by its width; the sample run to what it printed; an "
          "error said on its line with nothing made; a loop without end stopped "
          "by its budget in %.0f s, Cafesa3D answering; Escape giving the keyboard "
          "back; closed, the view as wide as it was)" % (checks, took))
    return 0


if __name__ == "__main__":
    sys.exit(main())
