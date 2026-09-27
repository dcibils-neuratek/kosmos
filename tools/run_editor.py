#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The editor, typed at through a keyboard (`roadmap.md` 6n, steps 0 and 1).

Booted, `wm editor:/Temporary/keys.txt`, and then every key pressed with QEMU's
own keyboard - `sendkey`, which goes through the virtio keyboard on one
board and the PS/2 controller on the other - so what is checked is the
whole path a key takes: the driver, the board's sequence with its
modifiers, the window manager, the kit's decoder, and `ui.editor` doing
what the key means over `/lib/textbuf.lua`.

One file is written by keys alone and saved with Control-S, then read back
at the prompt, and it has to be exactly what the keys meant:

  hello world      typed
  shift-left x5    selects "world"; typing "there" replaces it
  ctrl-z x2        undoes the typing, then the replacement: "world" back
  end, !           the end of the line, and a mark: "hello world!"
  home, ctrl-right, delete, ctrl-z
                   Home, a word right, the space deleted and put back
  pgup, pgdn       which typed a "~" each until 26 September; nothing now
  end, ret, second, ctrl-shift-left, backspace
                   a new line, a word selected by words and taken back
  two, ctrl-z, ctrl-y
                   typed, undone, redone

  hello world!
  two

Usage: run_editor.py IMAGE
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                   # noqa: E402

WANT = "hello world!\ntwo\n"


def letters(text):
    """QEMU's names for typing `text`."""
    names = []

    for ch in text:
        if ch == " ":
            names.append("spc")
        elif ch == "!":
            names.append("shift-1")
        elif ch.isupper():
            names.append("shift-" + ch.lower())
        else:
            names.append(ch)

    return names


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
        guest.type("wm editor:/Temporary/keys.txt")

        if said("wm: window keys.txt - Editor at ", mark, 90) is None:
            print("FAIL: the Editor never opened its window.\n--- the guest said ---\n"
                  + guest.seen[mark:][-1500:])
            return 1

        time.sleep(1.5)

        press(*letters("hello world"))
        press(*["shift-left"] * 5)
        press(*letters("there"))
        press("ctrl-z", "ctrl-z")
        press("end", *letters("!"))
        press("home", "ctrl-right", "delete", "ctrl-z")
        press("pgup", "pgdn")
        press("end", "ret", *letters("second"))
        press("ctrl-shift-left", "backspace")
        press(*letters("two"))
        press("ctrl-z", "ctrl-y")

        mark = len(guest.seen)
        press("ctrl-s")
        saved = said("editor: saved ", mark, 20)
        check(saved == "2 lines to /Temporary/keys.txt",
              "Control-S did not save two lines: %r" % saved)

        # The desktop away, and the file read back at the prompt - as hex,
        # so a stray byte cannot hide in what a terminal shows.
        mark = len(guest.seen)
        guest.proc.stdin.write(R.STOP_DESKTOP)
        guest.proc.stdin.flush()
        deadline = time.monotonic() + 20

        while time.monotonic() < deadline and R.PROMPT not in guest.seen[mark:]:
            guest._read_available()
            time.sleep(0.2)

        mark = len(guest.seen)
        guest.type('local b, t = fs.read("/Temporary/keys.txt") or "", {} '
                   'for c in b:gmatch(".") do t[#t + 1] = ("%02x"):format(c:byte()) end '
                   'print("file" .. "-hex:" .. table.concat(t, " "))')
        hexes = said("file-hex:", mark, 20)

        if hexes is None:
            check(False, "the file could not be read back:\n" + guest.seen[mark:][-800:])
        else:
            content = bytes(int(h, 16) for h in hexes.split()).decode("utf-8", "replace")
            check(content == WANT,
                  "the keys wrote %r, where they meant %r" % (content, WANT))
    finally:
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on the editor, typed at:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on the editor, typed at through the keyboard (Shift "
          "and the arrows selecting, typing over a selection, undo and redo a "
          "step at a time, Home, End, a word right, Delete, the page keys "
          "typing nothing, Control+Shift+Left by words, Control-S; the file "
          "exactly what the keys meant)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
