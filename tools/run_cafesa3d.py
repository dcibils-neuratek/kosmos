#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Cafesa3D, used as a person uses it (`roadmap.md` 4l, step one).

Booted, `wm cafesa3d`, and then driven with QEMU's tablet and keyboard the
way the drawing says it works: the still life opens with the Cube selected
and outlined in orange; a click on the gold ball in the 3D view selects it
and the outline moves; a click on a row of the Outliner selects that; a
click on a row's eye hides the object and its colour leaves the picture; a
drag turns the view, the wheel brings it closer, Z switches to Wireframe
and 7 looks from the top; a click on a tab of Properties opens it.

What the window says in the log and what is on the screen are both
checked, because they fail differently: the log can say "selected Gold"
while the outline stays on the cube, and a picture can change for a reason
that is not the click.

The positions come from the application, which prints where its window,
its objects, its rows and its tabs are - so this follows the window when
the layout changes, rather than holding pixel numbers of its own.

Usage: run_cafesa3d.py IMAGE
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                   # noqa: E402

ORANGE = 0xffa53d


def pixels(data):
    """The screen's size, and its pixels as 0xRRGGBB, out of bounds black."""
    width, height, rgb = R.pixel_reader(data)

    def at(x, y):
        if x < 0 or y < 0 or x >= width or y >= height:
            return 0
        r, g, b = rgb(x, y)
        return (r << 16) | (g << 8) | b

    return width, height, at


def count(at, box, test):
    """Pixels in `box` (x0, y0, x1, y1), every other one, passing `test`."""
    x0, y0, x1, y1 = box
    n = 0

    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            if test(at(x, y)):
                n += 1

    return n


def around(p, r):
    return (p[0] - r, p[1] - r, p[0] + r, p[1] + r)


def within(at, p, r, test):
    """Pixels within `r` of `p`, every other one, passing `test`."""
    n = 0

    for y in range(p[1] - r, p[1] + r, 2):
        for x in range(p[0] - r, p[0] + r, 2):
            if (x - p[0]) ** 2 + (y - p[1]) ** 2 <= r * r and test(at(x, y)):
                n += 1

    return n


def orange(c):
    return c == ORANGE


def goldish(c):
    r, g, b = (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff
    return r > 150 and g > 100 and b < 90 and r > g


def reddish(c):
    r, g, b = (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff
    return r > 100 and g < 70 and b < 70


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

    def said(text, since, seconds=20):
        """The rest of the line `text` begins, said after `since`."""
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            at = guest.seen.find(text, since)

            if at >= 0 and "\n" in guest.seen[at + len(text):]:
                return guest.seen[at + len(text):].split("\n", 1)[0].strip()

            time.sleep(0.1)

        return None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        guest.type("wm cafesa3d")

        opened = said("cafesa3d: window at ", mark, 60)

        if opened is None:
            print("FAIL: Cafesa3D never opened its window.\n--- the guest said ---\n"
                  + guest.seen[mark:][-1500:])
            return 1

        ox, oy = (int(v) for v in opened.split(","))
        rows = dict((m.group(1), (int(m.group(2)), int(m.group(3)), int(m.group(4))))
                    for m in re.finditer(r"([\w.]+) (\d+),(\d+) eye (\d+)",
                                         said("cafesa3d: rows ", mark)))
        tabs = dict((m.group(1), (int(m.group(2)), int(m.group(3))))
                    for m in re.finditer(r"(\w+) (\d+),(\d+)",
                                         said("cafesa3d: tabs ", mark)))
        header = dict((m.group(1), (int(m.group(2)), int(m.group(3))))
                      for m in re.finditer(r"(\w+) (\d+),(\d+)",
                                           said("cafesa3d: controls ", mark)))

        def where(since):
            line = said("cafesa3d: at ", since)
            return dict((m.group(1), (ox + int(m.group(2)), oy + int(m.group(3))))
                        for m in re.finditer(r"([\w.]+) (-?\d+),(-?\d+)", line or ""))

        at_start = where(mark)
        summary = said("cafesa3d: 7 objects, ", mark) or ""

        check(summary.startswith("2062 triangles"),
              "it opened on something other than the still life: 7 objects, "
              + summary)
        check(said("cafesa3d: selected ", mark) == "Cube",
              "it did not open with the Cube selected")

        width, height, at = pixels(guest.screendump())

        def screen():
            nonlocal width, height, at
            time.sleep(1.2)
            width, height, at = pixels(guest.screendump())
            return at

        def click(x, y):
            guest.mouse_to(*R._to_tablet(x, y, width, height))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.3)
            guest.mouse_button(False)
            time.sleep(0.8)

        cube, gold = at_start.get("Cube"), at_start.get("Gold")

        check(cube and count(at, around(cube, 140), orange) > 40,
              "the selected Cube has no orange outline round it")
        check(gold and count(at, around(gold, 70), orange) == 0,
              "there is orange round the Gold ball before it is selected")
        gold_before = count(at, around(gold, 70), goldish)
        check(gold_before > 100, "the gold ball is not gold in the Solid view "
              "(%d gold pixels)" % gold_before)

        # A click on the gold ball, in the view.
        mark = len(guest.seen)
        click(*gold)
        check(said("cafesa3d: selected ", mark) == "Gold",
              "a click on the gold ball did not select it")
        at = screen()
        check(count(at, around(gold, 70), orange) > 20,
              "the outline did not come to the gold ball")
        check(count(at, around(cube, 140), orange) == 0,
              "the outline stayed round the Cube")

        # A row of the Outliner.
        mark = len(guest.seen)
        cyl = rows.get("Cylinder")
        click(ox + cyl[0], oy + cyl[1])
        check(said("cafesa3d: selected ", mark) == "Cylinder",
              "a click on the Cylinder's row did not select it")

        # An eye: the gold ball hidden, and gone from the picture.
        mark = len(guest.seen)
        g = rows.get("Gold")
        click(ox + g[2], oy + g[1])
        check(said("cafesa3d: ", mark) == "hid Gold", "the Gold row's eye did not hide it")
        at = screen()
        gone = count(at, around(gold, 70), goldish)
        check(gone < gold_before // 10,
              "the hidden gold ball is still in the picture (%d gold pixels of %d)"
              % (gone, gold_before))
        mark = len(guest.seen)
        click(ox + g[2], oy + g[1])
        check(said("cafesa3d: ", mark) == "showed Gold", "the eye did not show it again")

        # A tab of Properties.
        mark = len(guest.seen)
        click(ox + tabs["material"][0], oy + tabs["material"][1])
        check(said("cafesa3d: tab ", mark) == "material", "the Material tab did not open")

        # A drag turns the view: the objects move on the screen.
        before = screen()
        mark = len(guest.seen)
        sx, sy = ox + 200, oy + 600
        guest.mouse_to(*R._to_tablet(sx, sy, width, height))
        time.sleep(0.4)
        guest.mouse_button(True)
        time.sleep(0.3)

        for step in range(1, 9):
            guest.mouse_to(*R._to_tablet(sx + step * 25, sy, width, height))
            time.sleep(0.15)

        guest.mouse_button(False)
        turned = said("cafesa3d: view turned to ", mark)
        check(turned is not None, "a drag in the view did not turn it")
        moved = where(mark)
        check(moved.get("Gold") and moved.get("Gold") != gold,
              "after the drag the gold ball is where it was")

        # The wheel, over the view: closer.
        mark = len(guest.seen)
        guest.mouse_to(*R._to_tablet(ox + 500, oy + 400, width, height))
        time.sleep(0.3)
        guest.mouse_button(True, "wheel-up")
        time.sleep(0.1)
        guest.mouse_button(False, "wheel-up")
        check((said("cafesa3d: closer, at ", mark) or said("cafesa3d: further, at ", mark))
              is not None, "the wheel over the view did not move it")

        # Z: Wireframe, and the faces' colour is gone.
        red_solid = count(screen(), (ox + 46, oy + 46, ox + 1060, oy + 790), reddish)
        mark = len(guest.seen)
        guest.sendkey("z")
        check(said("cafesa3d: shading ", mark) == "wire", "Z did not switch to Wireframe")
        red_wire = count(screen(), (ox + 46, oy + 46, ox + 1060, oy + 790), reddish)
        check(red_solid > 200 and red_wire < red_solid // 10,
              "Wireframe still shows the red cube's faces (%d red pixels, %d in Solid)"
              % (red_wire, red_solid))

        # 7: from the top.
        mark = len(guest.seen)
        guest.sendkey("7")
        check(said("cafesa3d: view ", mark) == "top", "7 did not look from the top")

        # **Adding, through the menus as a person does**: Add, then Mesh,
        # then Cube in the submenu that opens beside Mesh - where
        # `ui.push_menu` puts it, two pixels in from the menu's right edge
        # and level with the row.
        # In Solid again, where the selection is an outline and not every
        # edge in orange - so undoing the cube can be seen to take it away.
        mark = len(guest.seen)
        guest.sendkey("z")
        check(said("cafesa3d: shading ", mark) == "solid", "Z did not come back to Solid")

        mark = len(guest.seen)
        click(ox + header["add"][0], oy + header["add"][1])
        opened = said("cafesa3d: add menu at ", mark)
        m = re.match(r"(\d+),(\d+), (\d+) wide, rows of (\d+)", opened or "")
        check(m is not None, "Add did not open its menu")

        if m:
            mx, my, mw, row = (int(v) for v in m.groups())

            # Mesh opens its submenu on the way past; a press does it too.
            click(mx + 24, my + 2 + row // 2)
            sub_x, sub_y = mx + mw - 2, my + 2
            click(sub_x + 30, sub_y + 2 + row + row // 2)      # the second: Cube
            added = said("cafesa3d: added ", mark)
            check(added is not None and added.startswith("Cube.001, a box, at 0.00 0.00 0.00"),
                  "Add, Mesh, Cube did not add Cube.001 at the 3D cursor: %r" % added)

            # And it is in the picture, outlined, at the cursor - wherever
            # the view has taken the cursor by now, which the app says.
            at = screen()
            origin = where(max(mark, guest.seen.find("cafesa3d: added ", mark))).get("Cube.001")
            check(origin and count(at, around(origin, 130), orange) > 40,
                  "the new cube at the 3D cursor has no outline round it")

            # Undo and redo it.
            mark = len(guest.seen)
            guest.sendkey("ctrl-z")
            check(said("cafesa3d: undid ", mark) == "added Cube.001",
                  "Ctrl Z did not undo adding Cube.001")
            at = screen()
            # A circle inside where the cube was: the Cylinder, selected
            # again by the undo, has its own outline a little further out.
            check(origin and within(at, origin, 90, orange) == 0,
                  "after Ctrl Z the new cube's outline is still there")
            mark = len(guest.seen)
            guest.sendkey("ctrl-shift-z")
            check(said("cafesa3d: redid ", mark) == "added Cube.001",
                  "Ctrl Shift Z did not redo it")

            # Shift D, then Delete; then X, which asks.
            mark = len(guest.seen)
            guest.sendkey("shift-d")
            check(said("cafesa3d: duplicated ", mark) == "Cube.001 as Cube.002",
                  "Shift D did not duplicate Cube.001 as Cube.002")
            mark = len(guest.seen)
            guest.sendkey("delete")
            check(said("cafesa3d: deleted ", mark) == "Cube.002", "Delete did not delete it")

            mark = len(guest.seen)

            if origin:
                click(*origin)

            check(said("cafesa3d: selected ", mark) == "Cube.001",
                  "a click at the 3D cursor did not select the new cube")
            mark = len(guest.seen)
            guest.sendkey("x")
            asked = said("cafesa3d: delete menu at ", mark)
            d = re.match(r"(\d+),(\d+), rows of (\d+)", asked or "")
            check(d is not None, "X did not ask before deleting")

            if d:
                dx, dy, drow = (int(v) for v in d.groups())
                click(dx + 30, dy + 2 + drow // 2)
                check(said("cafesa3d: deleted ", mark) == "Cube.001",
                      "choosing Delete in X's menu did not delete Cube.001")

        # And it is still running: nothing above raised.
        check("stack traceback" not in guest.seen and "cafesa3d.lua:" not in guest.seen,
              "Cafesa3D raised an error:\n" + guest.seen[-1200:])
    finally:
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on Cafesa3D:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on Cafesa3D (the still life, the Cube outlined; the gold ball "
          "selected by a click and the outline moved to it; a row of the Outliner; an "
          "eye hiding and showing; the Material tab; a drag turning the view; the wheel; "
          "Z to Wireframe with the faces gone; 7 from the top; Add, Mesh, Cube at the "
          "3D cursor, undone and redone; Shift D, Delete, and X asking first)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
