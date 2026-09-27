#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Kosmos IDE, its window (`roadmap.md` 6n, step 2).

A small Lua file is made at the prompt, and `wm ide:` that file opens it -
its folder the project - in the code look. Then, with QEMU's own keyboard:

  down down, Tab          the third line indented: Tab kept by a code
                          editor, where every other widget passes it on
  Ctrl+/, Ctrl+Z          commented, and the comment taken back
  Ctrl+End, Enter, y = 2  a new last line
  Ctrl+S                  saved, which the IDE says

and the file read back at the prompt has to be exactly what the keys meant.
The picture has Lua's keyword colour in it, from either of the drawing's
two palettes, where the editor is.

Then `wm ide` alone opens the same project with the same file, from what it
remembered in /home/.ide; and Control-W twice - the window manager's prefix,
then itself - closes the tab, which the IDE says.

Usage: run_ide.py IMAGE
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                   # noqa: E402

FILE = "/home/development/c.lua"
BEFORE = "local ui = 1\\nlocal function f(c)\\nreturn c\\nend\\n"
WANT = "local ui = 1\nlocal function f(c)\n  return c\nend\ny = 2\n"

# Lua's keyword colour in the drawing's light palette and its dark one
# (`ui.lua`, CODE_LIGHT and CODE_DARK).
KEYWORD = ((0x7a, 0x3f, 0xb8), (0xc7, 0x92, 0xea))


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

    def stop_desktop():
        mark = len(guest.seen)
        guest.proc.stdin.write(R.STOP_DESKTOP)
        guest.proc.stdin.flush()
        deadline = time.monotonic() + 20

        while time.monotonic() < deadline and R.PROMPT not in guest.seen[mark:]:
            guest._read_available()
            time.sleep(0.2)

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        guest.type('fs.send("/home/development", { type = "mkdir" })')
        time.sleep(1)
        guest.type('fs.write("%s", "%s")' % (FILE, BEFORE))
        time.sleep(1)

        mark = len(guest.seen)
        guest.type("wm ide:" + FILE)
        opened = said("ide: project ", mark, 90)
        check(opened == "/home/development, 1 files open",
              "the IDE did not open the file it was given, with its folder as "
              "the project: %r" % opened)

        window = said("wm: window Kosmos IDE at ", mark, 5) or ""
        at = re.match(r"(\d+),(\d+) (\d+)x(\d+)", window)
        time.sleep(1.5)

        # The code look: a keyword's colour where the editor is.
        width, height, rgb = R.pixel_reader(guest.screendump())
        coloured = 0

        if at:
            x0, y0, w, h = (int(v) for v in at.groups())

            for y in range(y0 + 100, min(height, y0 + h - 220), 2):
                for x in range(x0 + 270, min(width, x0 + w), 2):
                    r, g, b = rgb(x, y)

                    if any(abs(r - k[0]) + abs(g - k[1]) + abs(b - k[2]) < 60
                           for k in KEYWORD):
                        coloured += 1

        check(coloured > 10, "no keyword coloured in the editor (%d pixels of "
              "the keyword colour) - it did not open in the code look" % coloured)

        press("down", "down", "tab")
        press("ctrl-slash", "ctrl-z")
        press("ctrl-end", "ret", "y", "spc", "equal", "spc", "2")

        mark = len(guest.seen)
        press("ctrl-s")
        saved = said("ide: saved ", mark, 20)
        check(saved == "c.lua, 5 lines", "Control-S did not save five lines: %r" % saved)

        stop_desktop()

        mark = len(guest.seen)
        guest.type('local b, t = fs.read("%s") or "", {} '
                   'for c in b:gmatch(".") do t[#t + 1] = ("%%02x"):format(c:byte()) end '
                   'print("file" .. "-hex:" .. table.concat(t, " "))' % FILE)
        hexes = said("file-hex:", mark, 20)

        if hexes is None:
            check(False, "the file could not be read back:\n" + guest.seen[mark:][-800:])
        else:
            content = bytes(int(h, 16) for h in hexes.split()).decode("utf-8", "replace")
            check(content == WANT,
                  "the keys wrote %r, where they meant %r" % (content, WANT))

        # Opened again with nothing asked: the project and its file remembered.
        mark = len(guest.seen)
        guest.type("wm ide")
        again = said("ide: project ", mark, 90)
        check(again == "/home/development, 1 files open",
              "the IDE did not come back to the project and its file: %r" % again)
        check(said("ide: opened ", mark, 5) == FILE,
              "the file it came back with was not the one open before")

        # **Run** (step 3): a program that prints and then fails on its
        # second line, run with Ctrl+Enter as it is on the screen, and the
        # IDE saying how it ended - the error's line found in what it wrote.
        stop_desktop()
        guest.type('fs.write("/home/development/r.lua", '
                   '"print(\\"ran\\")\\nerror(\\"boom\\")\\n")')
        time.sleep(1)
        mark = len(guest.seen)
        guest.type("wm ide:/home/development/r.lua")
        said("ide: project ", mark, 90)
        time.sleep(1.5)

        mark = len(guest.seen)
        press("ctrl-ret")
        ended = said("ide: r.lua ", mark, 30)
        printed = re.search(r"(\d+) lines printed$", ended or "")
        check(ended is not None and ended.startswith("ended with an error at line 2")
              and "boom" in ended and printed and int(printed.group(1)) >= 2,
              "Ctrl+Enter did not run r.lua to its error on line 2, with what "
              "it printed: %r" % ended)

        # **Stop**: a program that never ends, run with F5 and stopped with
        # Shift+F5.
        stop_desktop()
        guest.type('fs.write("/home/development/s.lua", "while true do end\\n")')
        time.sleep(1)
        mark = len(guest.seen)
        guest.type("wm ide:/home/development/s.lua")
        said("ide: project ", mark, 90)
        time.sleep(1.5)

        mark = len(guest.seen)
        press("f5")
        check(said("ide: s.lua, as process ", mark, 20) is not None,
              "F5 did not start s.lua")
        time.sleep(2)
        mark = len(guest.seen)
        press("shift-f5")
        stopped = said("ide: s.lua ", mark, 20)
        check(stopped is not None and stopped.startswith("stopped"),
              "Shift+F5 did not stop s.lua: %r" % stopped)

        # **Checking** (step 4): a file with one of each problem, checked as
        # it opens - io is not a Kosmos program's, an unused local, a name
        # never set - and then a stray `end` typed, which Lua's own parser
        # names a moment after the typing stops, and Ctrl+Z takes back.
        stop_desktop()
        guest.type('fs.write("/home/development/p.lua", '
                   '"local unused = 1\\nprint(undefined_thing)\\nio.write(1)\\n")')
        time.sleep(1)
        mark = len(guest.seen)
        guest.type("wm ide:/home/development/p.lua")
        checked = said("ide: checked p.lua: ", mark, 120)
        check(checked == "1 errors, 2 warnings",
              "p.lua was not checked as it opened to one error and two "
              "warnings: %r" % checked)
        time.sleep(1.5)

        mark = len(guest.seen)
        press("ctrl-end", "ret", "e", "n", "d")
        refused = said("ide: p.lua does not parse: ", mark, 20)
        check(refused is not None and refused.startswith("line 4:")
              and "near 'end'" in refused,
              "a stray end was not refused by Lua's parser on line 4: %r" % refused)

        mark = len(guest.seen)
        press("ctrl-z")
        check(said("ide: p.lua parses", mark, 20) is not None,
              "undoing the stray end did not make the file parse again")

        # Control-W twice: the prefix, then itself to the window - the tab closed.
        time.sleep(1.5)
        mark = len(guest.seen)
        guest.proc.stdin.write(b"\x17\x17")
        guest.proc.stdin.flush()
        # p.lua still has the new line the stray `end` was typed on, so the
        # first close is refused with a word, and the second closes it.
        refused = said("ide: p.lua is not saved", mark, 10)
        check(refused is not None,
              "closing p.lua with a change in it did not ask first")

        mark = len(guest.seen)
        guest.proc.stdin.write(b"\x17\x17")
        guest.proc.stdin.flush()
        check(said("ide: closed ", mark, 10) == "p.lua",
              "Control-W twice, again, did not close the tab in front, p.lua")

        # **Suggestions** (step 5): a file that asks ui.lua for a name it has
        # not got - checked as one error, asked of the library - and then,
        # typed on a new line, `ui.` offering ui.lua's names, `sl` and Tab
        # taking `slider`, and `win:` offering a window's methods.
        stop_desktop()
        guest.type('fs.write("/home/development/u.lua", "local ui = use(\\"/lib/ui.lua\\")\\n'
                   'local win = ui.window{}\\nlocal s = ui.slidr{}\\nprint(s, win)\\n")')
        time.sleep(1)
        mark = len(guest.seen)
        guest.type("wm ide:/home/development/u.lua")
        checked = said("ide: checked u.lua: ", mark, 120)
        check(checked == "1 errors, 0 warnings",
              "ui.slidr was not the one error, asked of ui.lua: %r" % checked)
        time.sleep(1.5)

        mark = len(guest.seen)
        press("ctrl-end", "ret", "u", "i", "dot")
        offered = said("ide: suggesting ", mark, 20)
        many = re.match(r"(\d+) names after ui\.$", offered or "")
        check(many is not None and int(many.group(1)) > 20,
              "ui. did not offer ui.lua's names: %r" % offered)

        mark = len(guest.seen)
        press("s", "l", "tab")
        check(said("ide: took ", mark, 20) == "slider",
              "sl and Tab did not take slider")

        mark = len(guest.seen)
        press("ret", "w", "i", "n", "shift-semicolon")
        offered = said("ide: suggesting ", mark, 20)
        many = re.match(r"(\d+) names after win:$", offered or "")
        check(many is not None and int(many.group(1)) > 10,
              "win: did not offer a window's methods: %r" % offered)

        press("esc", "backspace", "backspace", "backspace", "backspace", "backspace")
        mark = len(guest.seen)
        press("ctrl-s")
        check(said("ide: saved u.lua, ", mark, 20) == "5 lines",
              "the file with ui.slider taken was not saved")

        stop_desktop()
        mark = len(guest.seen)
        guest.type('print("last" .. "-line:" .. fs.read("/home/development/u.lua"):match("([^\\n]*)\\n$"))')
        check(said("last-line:", mark, 20) == "ui.slider",
              "the name taken did not reach the file")
    finally:
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on the IDE's window:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on the IDE (a file opened with its folder as the "
          "project, in the code look; Tab kept to indent, Ctrl+/ and undo, a new "
          "line, Ctrl+S, the file exactly what the keys meant; the project and "
          "its file remembered; a program run to its error with Ctrl+Enter, "
          "another started with F5 and stopped with Shift+F5; a file checked as it opened, and a stray end refused by Lua's parser and taken back; a changed tab closed only when asked twice; ui.slidr found, and ui. and win: offering ui.lua's names and a window's methods, slider taken with Tab)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
