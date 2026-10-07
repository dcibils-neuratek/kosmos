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
remembered in /Home/Preferences/ide; and Control-W twice - the window manager's prefix,
then itself - closes the tab, which the IDE says.

Then Run and Stop, checking, suggestions, the text's size and the scrollbar,
each below where it is done; and last, Find a file: Ctrl P and part of a
name, in any case, listing the files the tree reaches in the order drawn.

Usage: run_ide.py IMAGE
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                   # noqa: E402

FILE = "/Home/development/c.lua"
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
        guest.type('fs.send("/Home/development", { type = "mkdir" })')
        time.sleep(1)
        guest.type('fs.write("%s", "%s")' % (FILE, BEFORE))
        time.sleep(1)

        mark = len(guest.seen)
        guest.type("wm ide:" + FILE)
        opened = said("ide: project ", mark, 90)
        check(opened == "/Home/development, 1 files open",
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
        check(again == "/Home/development, 1 files open",
              "the IDE did not come back to the project and its file: %r" % again)
        check(said("ide: opened ", mark, 5) == FILE,
              "the file it came back with was not the one open before")

        # **Run** (step 3): a program that prints and then fails on its
        # second line, run with Ctrl+Enter as it is on the screen, and the
        # IDE saying how it ended - the error's line found in what it wrote.
        # A line typed at its top first and not saved, so what runs is the
        # screen's copy, in /Home/.ide-run: the error is on line 3.
        stop_desktop()
        guest.type('fs.write("/Home/development/r.lua", '
                   '"print(\\"ran\\")\\nerror(\\"boom\\")\\n")')
        time.sleep(1)
        mark = len(guest.seen)
        guest.type("wm ide:/Home/development/r.lua")
        said("ide: project ", mark, 90)
        time.sleep(1.5)

        press("ctrl-home", "p", "r", "i", "n", "t", "shift-9", "shift-apostrophe",
              "n", "e", "w", "shift-apostrophe", "shift-0", "ret")
        mark = len(guest.seen)
        press("ctrl-ret")
        ended = said("ide: r.lua ", mark, 30)
        printed = re.search(r"(\d+) lines printed$", ended or "")
        check(ended is not None and ended.startswith("ended with an error at line 3")
              and "boom" in ended and printed and int(printed.group(1)) >= 3,
              "Ctrl+Enter did not run r.lua as changed on the screen to its error on "
              "line 3, with what it printed: %r" % ended)

        # **A library, as it is, and larger than /Temporary holds**: bench.lua is
        # 21 KB, and its run was refused - "/Temporary is full" - when every run
        # went through a copy there (Diego, 27 September). Unchanged, it runs
        # from where it is, and the IDE says first that it is a library.
        stop_desktop()
        mark = len(guest.seen)
        guest.type("wm ide:/Kosmos/Libraries/bench.lua")
        said("ide: project ", mark, 90)
        time.sleep(1.5)
        mark = len(guest.seen)
        press("ctrl-ret")
        library = said("ide: bench.lua is a library: ", mark, 20)
        bench = said("ide: bench.lua ended, ", mark, 60)
        check(library is not None and 'use("/Kosmos/Libraries/bench.lua")' in library
              and bench is not None and bench.startswith("code 0"),
              "/Kosmos/Libraries/bench.lua was not said to be a library and run to its end: "
              "%r, %r" % (library, bench))

        # **Text larger and smaller** (Diego, 27 September, "like we have in
        # the terminal app"): Ctrl = a step up, Ctrl - a step down.
        mark = len(guest.seen)
        press("ctrl-equal")
        larger = said("ide: text ", mark, 20)
        mark = len(guest.seen)
        press("ctrl-minus")
        smaller = said("ide: text ", mark, 20)
        up, down = (int(t.split()[0]) if t else 0 for t in (larger, smaller))
        check(up > down > 0, "Ctrl = and Ctrl - did not make the text larger and "
              "then smaller: %r, %r" % (larger, smaller))

        # **The scrollbar** (Diego: "the ide is missing a scrollbar to see
        # where we are on the file"): a press low in its trough, at the
        # editor's right edge, moves bench.lua's view down.
        win = re.search(r"wm: window Kosmos IDE at (\d+),(\d+)", guest.seen)
        ed = [m for m in re.finditer(r"ide: editor at (\d+),(\d+) (\d+)x(\d+)",
                                     guest.seen)]
        moved = None

        if win and ed:
            wx, wy = int(win.group(1)), int(win.group(2))
            ex, ey, ew, eh = (int(v) for v in ed[-1].groups())
            width, height, _ = R.pixel_reader(guest.screendump())
            mark = len(guest.seen)
            guest.mouse_to(*R._to_tablet(wx + ex + ew - 9, wy + ey + eh - 40,
                                         width, height))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.2)
            guest.mouse_button(False)
            moved = said("ide: bench.lua shows line ", mark, 20)

        shown = re.match(r"(\d+) of (\d+)", moved or "")
        check(shown is not None and int(shown.group(1)) > 1
              and int(shown.group(2)) > 300,
              "a press low in the editor's scrollbar did not move bench.lua's view "
              "down: %r" % moved)

        # **Stop**: a program that never ends, run with F5 and stopped with
        # Shift+F5.
        stop_desktop()
        guest.type('fs.write("/Home/development/s.lua", "while true do end\\n")')
        time.sleep(1)
        mark = len(guest.seen)
        guest.type("wm ide:/Home/development/s.lua")
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
        guest.type('fs.write("/Home/development/p.lua", '
                   '"local unused = 1\\nprint(undefined_thing)\\nio.write(1)\\n")')
        time.sleep(1)
        mark = len(guest.seen)
        guest.type("wm ide:/Home/development/p.lua")
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
        guest.type('fs.write("/Home/development/u.lua", "local ui = use(\\"/Kosmos/Libraries/ui.lua\\")\\n'
                   'local win = ui.window{}\\nlocal s = ui.slidr{}\\nprint(s, win)\\n")')
        time.sleep(1)
        mark = len(guest.seen)
        guest.type("wm ide:/Home/development/u.lua")
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

        # **Find a file** (Diego, 27 September: "an ide wide search field to
        # find files easily by name or part of name"): Ctrl P, and `Cloc` -
        # whatever its case - lists the Clock before the library of the same
        # name and the longer name after both, each with what it is; Down and
        # Enter open the library. Then `u.l`: the project's own file, which
        # begins with it, above every name that only has it inside.
        mark = len(guest.seen)
        press("ctrl-p", "shift-c", "l", "o", "c")
        listed = said("ide: find Cloc: ", mark, 30)
        check(listed == "3 files: /Kosmos/Apps/clock.lua application, /Kosmos/Libraries/clock.lua library, "
                        "/Kosmos/Libraries/clock-replicant.lua library",
              "Cloc did not find the Clock, then the library, then the longer name, "
              "whatever the case: %r" % listed)

        mark = len(guest.seen)
        press("down", "ret")
        check(said("ide: opened ", mark, 20) == "/Kosmos/Libraries/clock.lua, read only",
              "Down and Enter did not open the second file found, /Kosmos/Libraries/clock.lua")

        mark = len(guest.seen)
        press("ctrl-p", "u", "dot", "l")
        listed = said("ide: find u.l: ", mark, 30)
        check(listed is not None
              and re.match(r"\d+ files: /Home/development/u\.lua yours, ", listed),
              "u.l did not list the project's u.lua first, as yours: %r" % listed)
        press("esc")

        stop_desktop()
        mark = len(guest.seen)
        guest.type('print("last" .. "-line:" .. fs.read("/Home/development/u.lua"):match("([^\\n]*)\\n$"))')
        check(said("last-line:", mark, 20) == "ui.slider",
              "the name taken did not reach the file")

        #
        # **Text larger is the same face, larger** (`roadmap.md` 6zm). The
        # editor measured in the face `ui.sized` gave it and drew in it too,
        # so the number crossed to the compositor, where no face had it:
        # Diego, on the M700, "making the font larger in the ide changes the
        # font instead of making it larger". A window draws with such a
        # number, and what its op carries is read back: the role and the
        # size, never the number.
        #
        probe = (
            "local ui = use('/Kosmos/Libraries/ui.lua') "
            "local win = ui.window{ title = 'FaceProbe', w = 240, h = 80, "
            "x = 300, y = 300 } "
            "local v = ui.view{ x = 0, y = 0, w = 240, h = 80 } "
            "local told = false "
            "function v:draw(g) "
            "local f = ui.sized('mono', 24) "
            "g:text(4, 4, 'Plex', nil, nil, f) "
            "local op = g.ops[#g.ops] "
            "if not told then told = true "
            "print('face' .. 'probe ' .. type(f) .. ' ' .. tostring(op.role) "
            ".. ' ' .. tostring(op.px)) end end "
            "win:add(v) win:run()"
        )
        guest.type("fs.write('/Temporary/faceprobe.lua', %r)" % probe)
        time.sleep(1.0)
        mark = len(guest.seen)
        guest.type("wm /Temporary/faceprobe.lua")
        drew = said("faceprobe ", mark, 60)
        check(drew == "number mono 24",
              "a face ui.sized gave out, drawn with, crossed to the compositor "
              "as %r - it has to go as the role and its size, mono 24, or the "
              "text comes out in whatever face the compositor has at that "
              "number" % drew)
        stop_desktop()

        # **Find, and the Console** (`docs/ide-layout.html`, 7 October): a
        # file with four lamps - Ctrl F counting them and Enter choosing
        # the first, Shift F3 going back round, Ctrl R and Enter
        # replacing one, Ctrl G going to a line, Ctrl Shift F finding them
        # across the project (which the board tells from Ctrl F since this
        # day), and Escape closing the bar. Then a program that asks for a
        # line, answered in the Console.
        guest.type('fs.write("/Home/development/f.lua", "-- a lamp\\nlocal lamp = 1\\n'
                   'print(lamp + 1) -- lamp\\n")')
        guest.type('fs.write("/Home/development/asks.lua", "write(\\"How many? \\")\\n'
                   'local got = fs.read(\\"/Devices/console\\")\\n'
                   'fs.write(\\"/Home/development/answer.txt\\", \\"got \\" .. tostring(got))\\n")')
        time.sleep(1)
        mark = len(guest.seen)
        guest.type("wm ide:/Home/development/f.lua")
        said("ide: opened /Home/development/f.lua", mark, 120)
        time.sleep(2)

        mark = len(guest.seen)
        press("ctrl-f", "l", "a", "m", "p")
        check(said("ide: find lamp: ", mark, 20) == "4 matches",
              "Ctrl F and lamp did not count four in the file")
        mark = len(guest.seen)
        press("ret")
        check(said("ide: found lamp at line ", mark, 20) == "1",
              "Enter did not choose the first lamp, on line 1")
        mark = len(guest.seen)
        press("shift-f3")
        check(said("ide: found lamp at line ", mark, 20) == "3",
              "Shift F3 did not go back round to the last lamp, on line 3")

        mark = len(guest.seen)
        press("ctrl-r", "t", "o", "r", "c", "h", "ret")
        check(said("ide: replaced lamp with ", mark, 20) == "torch at line 3",
              "Ctrl R, torch and Enter did not replace the lamp chosen")

        mark = len(guest.seen)
        press("esc")
        check(said("ide: find bar ", mark, 20) == "closed", "Escape did not close the bar")

        mark = len(guest.seen)
        press("ctrl-g", "2", "ret")
        check(said("ide: went to line ", mark, 20) == "2", "Ctrl G and 2 did not go to line 2")

        mark = len(guest.seen)
        press("ctrl-shift-f")
        check(said("ide: find bar, ", mark, 20) == "project",
              "Ctrl Shift F did not open Find in the project - Shift lost on the way?")
        press("ctrl-a", "l", "a", "m", "p", "ret")
        found = said("ide: searched development for lamp: ", mark, 30)
        check(found is not None and re.match(r"\d+ in \d+ files?$", found)
              and int(found.split()[0]) >= 2,
              "Find in the project did not find the lamps: %r" % found)
        press("esc")

        # **The tutorial** (part one, 7 October): F1 opens its first page in
        # the browser, carried in the image, every picture on it shown.
        mark = len(guest.seen)
        press("f1")
        index = "asset:tutorial/ide/index.html"
        check(said("ide: tutorial at ", mark, 30) == index,
              "F1 did not open the IDE's tutorial")
        shown = said("browser: showing ", mark, 120)
        check(shown is not None
              and re.match(re.escape(index) + r', "Kosmos IDE tutorial", \d+ pixels tall, '
                           r"\d+ pictures, 0 missing", shown) is not None,
              "the browser did not show the tutorial's first page whole: %r" % shown)

        stop_desktop()

        # And lesson 1's finished project, as Help's Lesson's Project opens
        # it: it runs, a window with two buttons.
        mark = len(guest.seen)
        guest.type("wm /Kosmos/Tutorial/01-Counter/counter.lua")
        check(said("counter: a window with ", mark, 120) == "two buttons",
              "lesson 1's finished project did not run")
        stop_desktop()
        # Lesson 2's: the answer is worked out as the window opens.
        mark = len(guest.seen)
        guest.type("wm /Kosmos/Tutorial/02-Converter/converter.lua")
        check(said("converter: 20 is ", mark, 120) == "68.00 F",
              "lesson 2's finished project did not turn 20 C into 68 F")
        stop_desktop()
        # Lesson 3's: a note handed to it as its argument, as Tracker hands
        # one, is opened - the name with a space in it arriving whole.
        guest.type("mkdir /Home/Notes")
        guest.type('save "Notes/To do.note" milk')
        mark = len(guest.seen)
        guest.type('wm /Kosmos/Tutorial/03-Notes/notes.lua:"/Home/Notes/To do.note"')
        check(said("notes: opened ", mark, 120) == "/Home/Notes/To do.note, 1 lines",
              "lesson 3's finished project did not open the note it was handed")
        stop_desktop()
        mark = len(guest.seen)
        guest.type("wm ide:/Home/development/asks.lua")
        said("ide: opened /Home/development/asks.lua", mark, 120)
        time.sleep(2)
        mark = len(guest.seen)
        press("f5")
        asked = said("ide: the program asks ", mark, 60)
        check(asked == "for a line", "a program reading its console was not given the Console")
        press("6", "4", "ret")
        check(said("ide: gave the program a line, ", mark, 30) == "2 bytes",
              "64 and Enter in the Console did not reach the program")
        time.sleep(3)
        stop_desktop()
        mark = len(guest.seen)
        guest.type('print("ans" .. "wer:" .. tostring(fs.read("/Home/development/answer.txt")))')
        check(said("answer:", mark, 20) == "got 64",
              "the program did not read the line typed in the Console")
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
          "its file remembered; a program changed and not saved run to its error "
          "with Ctrl+Enter, a 21 KB library run as it is and said to be one, "
          "another started with F5 and stopped with Shift+F5; the text larger and smaller with Ctrl = and Ctrl -, in the same face, and bench.lua's view moved by its scrollbar; a file checked as it opened, and a stray end refused by Lua's parser and taken back; a changed tab closed only when asked twice; ui.slidr found, and ui. and win: offering ui.lua's names and a window's methods, slider taken with Tab; Ctrl P finding the Clock, its library and the longer name in that order whatever the case, the library opened with Down and Enter, and the project's own file first; Find counting, choosing, going back, replacing, a line by Ctrl G, the project by Ctrl Shift F; a program's line typed in the Console; F1's tutorial shown whole, and lesson 1's project run)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
