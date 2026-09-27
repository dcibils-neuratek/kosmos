#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Cafesa3D's Script panel (`roadmap.md` 4l, 6n step 6).

Booted with a /home of its own, `wm cafesa3d`, and then with QEMU's own
keyboard and tablet:

  Shift F4        the panel opens beside the view, and the view narrows by
                  the panel's width
  Ctrl Enter      the sample, the drawing's staircase, runs: 24 steps and a
                  column and a lamp made, and what it printed shown; the
                  Outliner lists them under the script's name
  the wheel       over the Outliner, down to its last row and back
  Ctrl Enter      again: the same, replacing the 26 it made last time
  Escape, Ctrl Z  the whole run taken back, as one undo step
  a click, a line `scene.box{ sise = 1 }` typed on a new last line, run:
                  refused on its line - the box has no sise - nothing made
  a loop          the line erased and `while true do end` typed instead, run:
                  stopped by its budget of instructions, Cafesa3D still answering
  Ctrl S          the loop replaced by `print("from the file")`, and the scene
                  saved; then that line changed to `print("not saved")`
  Ctrl O          the saved scene opened again: its script back as it was
                  saved, and Run replacing the 26 it made - so which script
                  made each object came back too
  Save .lua...    the script as a file of its own
  Open .lua...    another script, `a-tower.lua`, into the panel; its Run
                  makes its spire and leaves the staircase alone
  Escape, Z       the keyboard given back: Z is the shading key again
  Shift F4        the panel closed, the view as wide as it was

and with the machine stopped, `staircase.lua` and the saved scene read off
the disk: the script's text in both, and its 26 objects marked as its own.

What Cafesa3D says in the log is what is checked; the editor in the panel
is the IDE's, drawn into Cafesa3D's own pixels (`ui.paint_view`), so every
key reaching it has crossed both.

Usage: run_script.py IMAGE
"""

import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402

# **A disk for /home**, made before the harness is imported, since that is
# when it reads `KOSMOS_DISK`: the scene and the script are saved to it and
# opened again, and it carries a second script for Open .lua... to find -
# named to come first in the Open panel, which selects the first file.
WORK = scratch.directory("script")
HOME_DISK = os.path.join(WORK, "home.img")
TOWER = os.path.join(WORK, "a-tower.lua")
LUA = os.path.join(os.path.dirname(HERE), "build", "host", "lua")

with open(TOWER, "w") as f:
    f.write('scene.cone{ name = "Spire", radius = 0.6, depth = 2, loc = { 4, 0, 1 } }\n'
            'print("tower")\n')

subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK, "64",
                TOWER + ":/home/Scenes/a-tower.lua"],
               check=True, capture_output=True, cwd=os.path.dirname(HERE))
os.environ["KOSMOS_DISK"] = HOME_DISK

import run_screenshot as R                                   # noqa: E402


def keys_for(text):
    """QEMU's names for typing `text`."""
    names = {" ": "spc", "(": "shift-9", ")": "shift-0", '"': "shift-apostrophe",
             "=": "equal", ".": "dot", "\n": "ret", "{": "shift-bracket_left",
             "}": "shift-bracket_right"}
    return [names.get(ch, ch) for ch in text]


def from_disk(path):
    """A file off the /home disk, with the machine stopped, or None."""
    got = os.path.join(WORK, os.path.basename(path))
    done = subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "get", HOME_DISK, path, got],
                          capture_output=True, cwd=os.path.dirname(HERE))

    if done.returncode != 0 or not os.path.exists(got):
        return None

    with open(got, "rb") as f:
        return f.read().decode("utf-8", "replace")


def main():
    image = sys.argv[1]
    guest = R.Guest(image, 120)
    failed = []
    checks = 0
    took = 0

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

    def last_said(text, since, seconds=10):
        """The last line beginning `text` after `since`, once things settle."""
        said(text, since, seconds)
        time.sleep(1)
        found = [line.split(text, 1)[1].strip() for line in guest.seen[since:].split("\n")
                 if text in line]
        return found[-1] if found else None

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
        ox, oy = (int(v) for v in (said("cafesa3d: window at ", mark, 5) or "0,0").split(","))
        first_row = re.search(r"[\w.]+ (\d+),(\d+) eye", said("cafesa3d: rows ", mark, 5) or "")
        width, height, _ = R.pixel_reader(guest.screendump())

        def click(x, y):
            guest.mouse_to(*R._to_tablet(x, y, width, height))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.3)
            guest.mouse_button(False)
            time.sleep(0.6)

        def panel_button(kind, since):
            """The Open or Save panel's button, at its bottom right."""
            at = said("cafesa3d: %s panel at " % kind, since, 30)
            m = re.match(r"(\d+),(\d+)", at or "")

            if not m:
                return None

            return int(m.group(1)) + 640 - 12 - 48, int(m.group(2)) + 420 - 72 + 10 + 12

        time.sleep(2)

        # Shift F4: the panel, and a narrower view.
        mark = len(guest.seen)
        press("shift-f4")
        panel = said("cafesa3d: script panel open, the view ", mark, 30)
        narrow = re.match(r"(\d+) by", panel or "")
        check(narrow is not None and int(narrow.group(1)) == view_w - 470,
              "Shift F4 did not open the panel and narrow the view by 470 from "
              "%d: %r" % (view_w, panel))
        spots = re.match(r"(\d+),(\d+) \d+x\d+; run (\d+),(\d+); open (\d+),(\d+); "
                          r"save (\d+),(\d+)", said("cafesa3d: script code ", mark, 10) or "")
        code, run, open_lua, save_lua = (
            (ox + int(spots.group(i)), oy + int(spots.group(i + 1))) if spots else None
            for i in (1, 3, 5, 7))
        time.sleep(2)

        # The sample, the staircase, run: made, and listed under its name.
        mark = len(guest.seen)
        press("ctrl-ret")
        ran = said("cafesa3d: script ran in ", mark, 60)
        check(said("cafesa3d: script 24 steps, 4.3 m up", mark, 5) is not None
              and ran is not None and ran.endswith(": 25 objects and 1 lamp"),
              "Ctrl Enter did not make the staircase: %r" % ran)
        listed = last_said("cafesa3d: outliner ", mark)
        check(listed is not None and re.match(
                  r"rows 1 to \d+ of 34: script staircase, Column, Light\.001, Step, ", listed),
              "the Outliner did not list the staircase under its script: %r" % listed)

        # The wheel over the Outliner: down to its last row, and back.
        seen_last, back_up = False, False

        if first_row:
            guest.mouse_to(*R._to_tablet(ox + int(first_row.group(1)),
                                         oy + int(first_row.group(2)), width, height))
            time.sleep(0.4)

            for button in ["wheel-down"] * 12 + ["wheel-up"] * 12:
                mark = len(guest.seen)
                guest.mouse_button(True, button)
                time.sleep(0.05)
                guest.mouse_button(False, button)
                line = said("cafesa3d: outliner ", mark, 2) or ""
                seen_last = seen_last or re.match(r"rows \d+ to 34 of 34: .*Step\.023", line)
                back_up = line.startswith("rows 1 to ") or (back_up and line == "")

        check(seen_last and back_up,
              "the wheel over the Outliner did not reach Step.023 in its last row and come "
              "back to the first")

        # Again, in its own place rather than on top of itself.
        mark = len(guest.seen)
        press("ctrl-ret")
        again = said("cafesa3d: script ran in ", mark, 60)
        check(again is not None and again.endswith(
                  "25 objects and 1 lamp, replacing the 26 it made last time"),
              "a second run did not replace what the first made: %r" % again)

        # The keyboard given back, and Ctrl Z: the whole run, one step.
        mark = len(guest.seen)
        press("esc", "ctrl-z")
        check(said("cafesa3d: undid ", mark, 20) == "ran staircase",
              "Ctrl Z did not take back the run as one step")

        # A mistake, refused on its line: a click in the code gives it the
        # keyboard again, and a new last line asks for a field there is not.
        if code:
            click(code[0] + 200, code[1] + 12)

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

        # Saved with the scene: the loop's line made a print, and Ctrl S.
        press("end", *["backspace"] * len("while true do end"))
        press(*keys_for('print("from the file")'))
        mark = len(guest.seen)
        press("esc", "ctrl-s")
        button = panel_button("save", mark)

        if button:
            click(*button)

        saved = said("cafesa3d: saved ", mark, 60) or ""
        check(re.match(r"/home/Scenes/still-life\.gltf, 33 objects, \d+ bytes$", saved),
              "Ctrl S did not save the scene with the staircase in it: %r" % saved)

        # Changed after saving, and the saved scene opened again: its script
        # as it was saved, and which script made what, so Run replaces them.
        if code:
            click(code[0] + 200, code[1] + 12)

        press("ctrl-end", "end", *["backspace"] * len('print("from the file")'))
        press(*keys_for('print("not saved")'))
        mark = len(guest.seen)
        press("esc", "ctrl-o")
        button = panel_button("open", mark)

        if button:
            click(*button)

        reopened = said("cafesa3d: opened ", mark, 60) or ""
        came = said("cafesa3d: script staircase came with the scene, ", mark, 10)
        check(reopened.startswith("Still life, 33 objects, ") and came == "25 lines",
              "the saved scene did not open with its script: %r, %r" % (reopened, came))

        mark = len(guest.seen)

        if run:
            click(*run)

        rerun = said("cafesa3d: script ran in ", mark, 60)
        check(said("cafesa3d: script from the file", mark, 5) is not None
              and "cafesa3d: script not saved" not in guest.seen[mark:]
              and rerun is not None
              and rerun.endswith("25 objects and 1 lamp, replacing the 26 it made last time"),
              "the reopened scene's script was not the saved one, or its Run did not "
              "replace what the script had made: %r" % rerun)

        # Save .lua...: the script as a file of its own.
        mark = len(guest.seen)

        if save_lua:
            click(*save_lua)

        button = panel_button("save", mark)

        if button:
            click(*button)

        lua_saved = said("cafesa3d: script saved ", mark, 30) or ""
        check(re.match(r"/home/Scenes/staircase\.lua, \d+ bytes$", lua_saved),
              "Save .lua... did not write staircase.lua: %r" % lua_saved)

        # Open .lua...: another script, run beside the staircase.
        mark = len(guest.seen)

        if open_lua:
            click(*open_lua)

        button = panel_button("open", mark)

        if button:
            click(*button)

        lua_opened = said("cafesa3d: script opened ", mark, 30)
        mark = len(guest.seen)

        if run:
            click(*run)

        tower = said("cafesa3d: script ran in ", mark, 60)
        listed = last_said("cafesa3d: outliner ", mark)
        check(lua_opened == "/home/Scenes/a-tower.lua, 2 lines"
              and said("cafesa3d: script tower", mark, 5) is not None
              and tower is not None and tower.endswith(": 1 object")
              and listed is not None
              and "script a-tower, Spire, script staircase, Column" in listed,
              "Open .lua... did not open a-tower.lua and run it beside the staircase: "
              "%r, %r, %r" % (lua_opened, tower, listed))

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
        check("stack traceback" not in guest.seen and "cafesa3d.lua:" not in guest.seen,
              "Cafesa3D raised an error:\n" + guest.seen[-1200:])
    finally:
        guest.close()

    # **Off the disk**, with the machine stopped: the script on its own, and
    # the scene with the script and its 26 objects marked as its own.
    lua = from_disk("/home/Scenes/staircase.lua") or ""
    check(lua.startswith("-- A spiral staircase") and 'print("from the file")\n' in lua,
          "staircase.lua on the disk is not the script: %r" % lua[:80])

    scene = from_disk("/home/Scenes/still-life.gltf")
    doc = json.loads(scene) if scene else {}
    own = doc.get("extras", {}).get("cafesa3d", {}).get("script", {})
    made = [n for n in doc.get("nodes", [])
            if n.get("extras", {}).get("cafesa3d", {}).get("by") == "staircase"]
    check(own.get("name") == "staircase" and 'print("from the file")' in own.get("text", "")
          and len(made) == 26,
          "the saved scene does not hold the script and its 26 objects: %r, %d made"
          % (own.get("name"), len(made)))

    if failed:
        print("FAIL: %d of %d checks on Cafesa3D's Script panel:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        said_lines = [line for line in guest.seen.splitlines() if "cafesa3d:" in line
                      and not line.lstrip().startswith(("cafesa3d: at ", "cafesa3d: fields "))]
        print("--- what Cafesa3D said last ---\n" + "\n".join(said_lines[-30:]))
        return 1

    print("PASS: %d checks on Cafesa3D's Script panel (opened with Shift F4 beside "
          "a view narrower by its width; the staircase made, listed under its script "
          "and reached with the wheel, made again in its own place, and taken back "
          "with one Ctrl Z; a field the box has not got refused on its line with nothing "
          "made; a loop without end stopped by its budget in %.0f s; the scene saved and "
          "opened again with its script and what it made; Save .lua... and Open .lua...; "
          "Escape giving the keyboard back; closed, the view as wide as it was; and both "
          "files read off the disk)" % (checks, took))
    return 0


if __name__ == "__main__":
    sys.exit(main())
