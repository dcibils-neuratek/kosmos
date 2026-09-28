#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Two editors, typed at through a keyboard (`roadmap.md` 6n steps 0 and 1,
and 6zs).

Booted, and then every key pressed with QEMU's own keyboard - `sendkey`,
which goes through the virtio keyboard on one board and the PS/2 controller
on the other - so what is checked is the whole path a key takes: the driver,
the board's sequence with its modifiers, the window manager, the kit's
decoder, and the editor doing what the key means over
`/Kosmos/Libraries/textbuf.lua`.

**Twice, into two editors that are not the same code**: Text Editor's page
(`docview.lua` - a proportional face, rows that wrap, a caret between
characters) and the IDE's (`ui.editor`, monospace by construction). They
share the buffer and nothing on top of it, and this was the only suite that
typed at `ui.editor` - through Editor, until Editor became Text Editor.

Each file is written by keys alone and saved with Control-S, then read back
at the prompt, and it has to be exactly what the keys meant:

  hello world      typed
  shift-left x5    selects "world"; typing "there" replaces it
  ctrl-z x2        undoes the typing, then the replacement: "world" back
  end, !           the end of the line, and a mark: "hello world!"
  home, ctrl-right, delete, ctrl-z
                   Home, a word right, the space deleted and put back
  pgup, pgdn       which typed a "~" each until 26 September; nothing now
  end, ret, second, ctrl-shift-left, backspace, end
                   a new line, a word selected by words and taken back -
                   and End after it, since typing over a selection that
                   Backspace had left would hide a Backspace that did
                   nothing: until 28 September it did, and nothing noticed
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

# **And Markdown as it is written** (`roadmap.md` 6zs step 2), into a new
# `.md`: an item ticked with Control-Return, Return starting the next item
# with an open box, Return on that empty item ending the list, and Control-B
# with nothing selected putting the caret between two pairs of stars.
WANT_MD = "- [x] one\n- [ ] two\nend**b**\n"


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

    def keys_into(since, title_line, saved_line):
        """The keys, into the editor that said `title_line`; what its save said."""
        if said(title_line, since, 90) is None:
            return None

        time.sleep(1.5)

        press(*letters("hello world"))
        press(*["shift-left"] * 5)
        press(*letters("there"))
        press("ctrl-z", "ctrl-z")
        press("end", *letters("!"))
        press("home", "ctrl-right", "delete", "ctrl-z")
        press("pgup", "pgdn")
        press("end", "ret", *letters("second"))
        press("ctrl-shift-left", "backspace", "end")
        press(*letters("two"))
        press("ctrl-z", "ctrl-y")

        mark = len(guest.seen)
        press("ctrl-s")
        return said(saved_line, mark, 20)

    def to_the_prompt():
        """The desktop away, and the shell back."""
        mark = len(guest.seen)
        guest.proc.stdin.write(R.STOP_DESKTOP)
        guest.proc.stdin.flush()
        deadline = time.monotonic() + 20

        while time.monotonic() < deadline and R.PROMPT not in guest.seen[mark:]:
            guest._read_available()
            time.sleep(0.2)

    def read_back(path):
        """The file, as hex, so a stray byte cannot hide in what a terminal shows."""
        mark = len(guest.seen)
        guest.type('local b, t = fs.read("%s") or "", {} '
                   'for c in b:gmatch(".") do t[#t + 1] = ("%%02x"):format(c:byte()) end '
                   'print("file" .. "-hex:" .. table.concat(t, " "))' % path)
        hexes = said("file-hex:", mark, 20)

        if hexes is None:
            return None

        return bytes(int(h, 16) for h in hexes.split()).decode("utf-8", "replace")

    try:
        guest.wait_for("kosmos> ", "reached a prompt")

        # Text Editor's page.
        mark = len(guest.seen)
        guest.type("wm texteditor:/Temporary/keys.txt")
        saved = keys_into(mark, "wm: window keys.txt - Text Editor at ",
                          "texteditor: saved ")

        if saved is None and "Text Editor at" not in guest.seen[mark:]:
            print("FAIL: Text Editor never opened its window.\n--- the guest said ---\n"
                  + guest.seen[mark:][-1500:])
            return 1

        check(saved == "2 lines to /Temporary/keys.txt",
              "Control-S in Text Editor did not save two lines: %r" % saved)
        to_the_prompt()

        # Markdown, in Text Editor again: a new `.md`.
        mark = len(guest.seen)
        guest.type("wm texteditor:/Temporary/list.md")

        if said("wm: window list.md - Text Editor at ", mark, 90) is None:
            check(False, "Text Editor did not open list.md")
        else:
            time.sleep(1.5)
            press("minus", "spc", "bracket_left", "spc", "bracket_right", "spc",
                  *letters("one"))
            press("ctrl-ret", "end", "ret", *letters("two"), "ret", "ret")
            press(*letters("end"), "ctrl-b", "b")
            mark = len(guest.seen)
            press("ctrl-s")
            saved = said("texteditor: saved ", mark, 20)
            check(saved == "3 lines to /Temporary/list.md",
                  "Control-S did not save the Markdown's three lines: %r" % saved)

        to_the_prompt()

        # The IDE's editor, over a file in a folder of its own.
        # The file made first: the IDE takes a path that is not there for a
        # project folder to open, not a file to write.
        guest.type('fs.send("/Temporary/ide", { type = "mkdir" }) '
                   'fs.write("/Temporary/ide/keys.txt", "")')
        time.sleep(1)
        mark = len(guest.seen)
        guest.type("wm ide:/Temporary/ide/keys.txt")
        saved = keys_into(mark, "ide: editor at ", "ide: saved ")
        check(saved == "keys.txt, 2 lines",
              "Control-S in the IDE did not save two lines: %r\n%s"
              % (saved, guest.seen[mark:][-800:]))
        to_the_prompt()

        for who, path, want in (("Text Editor", "/Temporary/keys.txt", WANT),
                                ("Text Editor's Markdown", "/Temporary/list.md", WANT_MD),
                                ("the IDE", "/Temporary/ide/keys.txt", WANT)):
            content = read_back(path)

            if content is None:
                check(False, "%s's file could not be read back" % who)
            else:
                check(content == want,
                      "the keys wrote %r in %s, where they meant %r" % (content, who, want))
    finally:
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on two editors, typed at:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on two editors typed at through the keyboard - Text "
          "Editor's page and the IDE's (Shift and the arrows selecting, typing "
          "over a selection, undo and redo a step at a time, Home, End, a word "
          "right, Delete, the page keys typing nothing, Control+Shift+Left by "
          "words, Control-S; each file exactly what the keys meant) - and "
          "Markdown in Text Editor: a box ticked with Control-Return, a list "
          "going on and ending, Control-B" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
