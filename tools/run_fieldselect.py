#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""A field's text selected as the editor's is (`roadmap.md`, 9 November).

A window with one `ui.field` holding twenty a's, forty b's and "cc", which
says its text and its selection after every key and every press and
release. Then, with QEMU's own pointer and keyboard:

  a double click in the b's        the b's selected, and only them
  a drag from the start past the end   all of it selected
  End, Shift-Left three times      " cc" selected
  Control-Shift-Left               the b's and " cc"
  x                                typed over the selection
  Home, Shift-End, Delete          nothing left

Before 9 November a field had select-all and nothing else, so every one
of these said no selection.

Usage: run_fieldselect.py IMAGE
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                   # noqa: E402

TEXT = "a" * 20 + " " + "b" * 40 + " cc"

PROBE = (
    "local ui = use('/Kosmos/Libraries/ui.lua') "
    "local win = ui.window{ title = 'FieldSel', w = 560, h = 80, x = 200, y = 200 } "
    "local f = ui.field{ x = 20, y = 20, w = 520, text = '%s' } "
    "local function tell(how) local a, b if f.selection then a, b = f:selection() end "
    "print(('fsel ' .. '%%s %%d %%d %%d|%%s'):format(how, a or 0, b or 0, f.caret, f.text)) end "
    "local k, m = f.key, f.mouse "
    "function f:key(c) local r = k(self, c) tell('key') return r end "
    "function f:mouse(a, x, y) local r = m(self, a, x, y) "
    "if a ~= 'move' then tell(a) end return r end "
    "win:add(f) win:focus_on(f) win:run()"
) % TEXT


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

    def last(since, seconds=5):
        """The field's last report after `since`: (a, b, caret, text)."""
        time.sleep(0.6)
        deadline = time.monotonic() + seconds
        found = None

        while time.monotonic() < deadline:
            found = re.findall(r"fsel \w+ (\d+) (\d+) (\d+)\|([^\r\n]*)", guest.seen[since:])
            if found:
                break
            time.sleep(0.1)

        if not found:
            return None
        a, b, c, t = found[-1]
        return int(a), int(b), int(c), t

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        guest.type("fs.write('/Temporary/fieldsel.lua', %r)" % PROBE)
        time.sleep(1.0)

        mark = len(guest.seen)
        guest.type("wm /Temporary/fieldsel.lua")
        window = said("wm: window FieldSel at ", mark, 90)
        check(window is not None, "the window did not open")

        if window:
            # Where the window is, and below its title bar: the size said
            # is the frame's, the 80 asked for under a header.
            wx, wy, _, wh = (int(v) for v in re.match(r"(\d+),(\d+) (\d+)x(\d+)", window).groups())
            wy += wh - 80
            sw, sh, _ = R.parse_ppm(guest.screendump())
            time.sleep(1.5)

            def at(x):
                guest.mouse_to(*R._to_tablet(wx + 20 + x, wy + 30, sw, sh))
                time.sleep(0.25)

            def click(x):
                at(x)
                guest.mouse_button(True)
                time.sleep(0.1)
                guest.mouse_button(False)
                time.sleep(0.1)

            # A double click in the b's: twenty a's and a space are
            # under 200 pixels in any face, forty b's past 250.
            mark = len(guest.seen)
            click(240)
            click(240)
            got = last(mark)
            check(got and got[3] == TEXT and TEXT[got[0] - 1:got[1] - 1] == "b" * 40,
                  "a double click in the b's did not select them: %r" % (got,))

            # A drag from the first letter to past the end.
            mark = len(guest.seen)
            at(11)
            guest.mouse_button(True)
            time.sleep(0.2)
            for x in (100, 300, 500, 540):
                at(x)
            guest.mouse_button(False)
            got = last(mark)
            check(got and (got[0], got[1]) == (1, len(TEXT) + 1),
                  "a drag from the start past the end did not select it all: %r" % (got,))

            mark = len(guest.seen)
            guest.sendkey("end")
            for _ in range(3):
                guest.sendkey("shift-left")
            got = last(mark)
            check(got and TEXT[got[0] - 1:got[1] - 1] == " cc",
                  "End and Shift-Left three times did not select \" cc\": %r" % (got,))

            mark = len(guest.seen)
            guest.sendkey("ctrl-shift-left")
            got = last(mark)
            check(got and TEXT[got[0] - 1:got[1] - 1] == "b" * 40 + " cc",
                  "Control-Shift-Left did not take the selection back a word: %r" % (got,))

            mark = len(guest.seen)
            guest.sendkey("x")
            got = last(mark)
            check(got and got[3] == "a" * 20 + " x" and (got[0], got[1]) == (0, 0),
                  "x did not replace the selection: %r" % (got,))

            mark = len(guest.seen)
            guest.sendkey("home")
            guest.sendkey("shift-end")
            guest.sendkey("delete")
            got = last(mark)
            check(got and got[3] == "",
                  "Home, Shift-End and Delete did not empty the field: %r" % (got,))
    finally:
        if failed:
            print(guest.seen[-3000:])
        guest.close()

    for complaint in failed:
        print("FAIL: " + complaint)

    if failed:
        return 1

    print("PASS: %d of %d - a field's text selected by pointer and keys" % (checks, checks))
    return 0


if __name__ == "__main__":
    sys.exit(main())
