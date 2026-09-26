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
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402

# **A disk for /home**, made before the harness is imported, since that is
# when it reads `KOSMOS_DISK`: a scene is saved to /home and opened again,
# and the plane is a megabyte, where the RAM filesystem a diskless guest
# has holds sixteen kilobytes a file.
#
# **And a scene that is broken in the one way the reader cannot see**: a
# triangle naming a point its mesh has not got. Its accessors fit its
# buffer, so the file reads; the 3D Kit refuses the mesh, and Cafesa3D must
# skip it and say so rather than die with the scene it had - which is what
# it did, found by a control that mangled the base64 of a saved plane.
WORK = scratch.directory("cafesa3d")
HOME_DISK = os.path.join(WORK, "home.img")
BROKEN = os.path.join(WORK, "zz-broken.gltf")


def broken_scene():
    import base64
    import json
    import struct

    data = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0) + bytes([0, 1, 5, 0])
    uri = "data:application/octet-stream;base64," + base64.b64encode(data).decode()
    own = {"loc": [0, 0, 0], "rot": [0, 0, 0], "scale": [1, 1, 1]}

    return {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"name": "Broken", "nodes": [0, 1]}],
        "nodes": [
            {"name": "Box", "extras": {"cafesa3d": dict(own, kind="box", size=[1, 1, 1])}},
            {"name": "Tri", "mesh": 0, "extras": {"cafesa3d": dict(own, kind="mesh", mesh=0)}},
        ],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5121, "count": 3, "type": "SCALAR"},
        ],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                        {"buffer": 0, "byteOffset": 36, "byteLength": 3}],
        "buffers": [{"byteLength": len(data), "uri": uri}],
    }


#
# **Other programs' files, to import** (step 5c), named so the panel's rows
# come in a known order: an STL cube as a printer's library would give it,
# an OBJ of two parts coloured by the MTL beside it, and a binary glTF with
# nodes inside nodes, a mesh of two parts and a part whose triangles are
# not listed - what Sketchfab hands out.
#
def stl_cube():
    import struct

    p = [(0, 0, 0), (10, 0, 0), (10, 10, 0), (0, 10, 0),
         (0, 0, 10), (10, 0, 10), (10, 10, 10), (0, 10, 10)]
    t = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
         (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]
    out = b"cube".ljust(80, b"\0") + struct.pack("<I", len(t))

    for a, b, c in t:
        out += struct.pack("<3f", 0, 0, 0) + struct.pack("<9f", *p[a], *p[b], *p[c]) + b"\0\0"

    return out


OBJ_TEXT = """mtllib b-parts.mtl
o Red part
usemtl Red
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
f 1 2 3 4
o Blue part
usemtl Blue
v 3 0 0
v 4 0 0
v 3 1 0
f -3 -2 -1
"""
MTL_TEXT = "newmtl Red\nKd 0.8 0.1 0.1\nNs 250\nnewmtl Blue\nKd 0.1 0.2 0.9\n"


def glb_parts():
    import json
    import struct

    tri = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0)
    data = tri + struct.pack("<9f", 2, 0, 0, 3, 0, 0, 2, 1, 0) + struct.pack("<3I", 0, 1, 2)
    doc = {
        "asset": {"version": "2.0"}, "scene": 0,
        "scenes": [{"name": "Parts", "nodes": [0]}],
        "nodes": [{"name": "Parts", "translation": [0, 1, 0], "children": [1, 2]},
                  {"name": "Body", "mesh": 0},
                  {"name": "Wheel", "scale": [2, 2, 2], "mesh": 1}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 2, "material": 0},
                                   {"attributes": {"POSITION": 1}, "indices": 2, "material": 1}]},
                   {"primitives": [{"attributes": {"POSITION": 0}}]}],
        "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [1, 0.5, 0, 1]}},
                      {"pbrMetallicRoughness": {"baseColorFactor": [0, 0.5, 1, 1]}}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
                      {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
                      {"bufferView": 2, "componentType": 5125, "count": 3, "type": "SCALAR"}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                        {"buffer": 0, "byteOffset": 36, "byteLength": 36},
                        {"buffer": 0, "byteOffset": 72, "byteLength": 12}],
        "buffers": [{"byteLength": len(data)}],
    }
    text = json.dumps(doc).encode()
    text += b" " * ((4 - len(text) % 4) % 4)
    data += b"\0" * ((4 - len(data) % 4) % 4)

    return (struct.pack("<4sII", b"glTF", 2, 12 + 8 + len(text) + 8 + len(data))
            + struct.pack("<I4s", len(text), b"JSON") + text
            + struct.pack("<I4s", len(data), b"BIN\0") + data)


with open(BROKEN, "w") as f:
    import json as _json
    _json.dump(broken_scene(), f)

# **And an FBX a program wrote**: Blender's Suzanne in seven materials, one
# of the files `tools/test_fbx.c` holds to its OBJ - fetched once, with its
# sum, into build/downloads. Named to sort last, so the Import panel's rows
# for the others are where they were.
subprocess.run([sys.executable, os.path.join(HERE, "fetch_conformance.py"), "fbx"],
               check=True, capture_output=True)

with open(os.path.join(os.path.dirname(HERE), "build", "downloads", "fbx-conformance",
                       "blender_suzanne_multimaterial_7400_binary.fbx"), "rb") as f:
    MONKEY = f.read()

IMPORTS = {"a-cube.stl": stl_cube(), "b-parts.obj": OBJ_TEXT.encode(),
           "b-parts.mtl": MTL_TEXT.encode(), "x-parts.glb": glb_parts(),
           "zz-monkey.fbx": MONKEY}

for _name, _bytes in IMPORTS.items():
    with open(os.path.join(WORK, _name), "wb") as f:
        f.write(_bytes)

subprocess.run([os.path.join(os.path.dirname(HERE), "build", "host", "lua"),
                os.path.join(HERE, "kfs.lua"), "create", HOME_DISK, "64",
                BROKEN + ":/home/Scenes/zz-broken.gltf"]
               + ["%s:/home/Scenes/%s" % (os.path.join(WORK, n), n) for n in IMPORTS],
               check=True, capture_output=True, cwd=os.path.dirname(HERE))
os.environ["KOSMOS_DISK"] = HOME_DISK

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
                      for m in re.finditer(r"([\w:]+) (\d+),(\d+)",
                                           said("cafesa3d: controls ", mark)))

        def where(since):
            line = said("cafesa3d: at ", since)
            return dict((m.group(1), (ox + int(m.group(2)), oy + int(m.group(3))))
                        for m in re.finditer(r"([\w.]+) (-?\d+),(-?\d+)", line or ""))

        at_start = where(mark)
        summary = said("cafesa3d: 7 objects, ", mark) or ""
        view0 = re.search(r"the view (\d+) by (\d+)", summary)
        view0 = (int(view0.group(1)), int(view0.group(2))) if view0 else (1014, 744)

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

        # **The Material tab, edited** (step four, which the tutorial needs):
        # a preset, a swatch, a texture whose own fields then appear, and a
        # colour typed - letters and all - each said back as what it set.
        def placed(line):
            return dict((m.group(1), (ox + int(m.group(2)), oy + int(m.group(3))))
                        for m in re.finditer(r"([\w:.]+) (\d+),(\d+)", line or ""))

        chips_at = placed(said("cafesa3d: chips ", mark))
        check("preset:Metal" in chips_at and "texture:Brick" in chips_at and "swatch:5" in chips_at,
              "the Material tab has no presets, swatches or textures to click: %r"
              % sorted(chips_at))

        if "preset:Metal" in chips_at:
            mark = len(guest.seen)
            click(*chips_at["preset:Metal"])
            got = said("cafesa3d: set preset of ", mark) or ""
            check(got.endswith(" to Metal"), "a click on Metal did not make it metal: %r" % got)

            mark = len(guest.seen)
            click(*chips_at["swatch:5"])
            got = said("cafesa3d: set Base colour of ", mark) or ""
            check(got.endswith(" to #2f6fc4"), "a click on the blue swatch did not colour it: %r"
                  % got)

            mark = len(guest.seen)
            click(*chips_at["texture:Brick"])
            got = said("cafesa3d: set texture of ", mark) or ""
            fields_now = placed(said("cafesa3d: fields ", mark))
            check(got.endswith(" to Brick") and "colour2" in fields_now and "scale" in fields_now,
                  "Brick did not give it a texture with its own fields: %r, %r"
                  % (got, sorted(fields_now)))

            if "base" in fields_now:
                mark = len(guest.seen)
                click(*fields_now["base"])
                for k in ("1", "c", "1", "c", "1", "e", "ret"):
                    guest.sendkey(k)
                    time.sleep(0.15)
                got = said("cafesa3d: set Base colour of ", mark) or ""
                check(got.endswith(" to #1c1c1e"), "a colour typed as hex was not taken: %r" % got)

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
            # Shift D, which moves the copy at once as Blender's does: Esc
            # leaves it where the original is, then Delete.
            mark = len(guest.seen)
            guest.sendkey("shift-d")
            check(said("cafesa3d: duplicated ", mark) == "Cube.001 as Cube.002",
                  "Shift D did not duplicate Cube.001 as Cube.002")
            check(said("cafesa3d: move ", mark) == "Cube.002",
                  "Shift D did not start moving the copy")
            guest.sendkey("esc")
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

        # **G, R and S**, as Blender's: no button held while it follows the
        # pointer, a click to put it down; X Y Z and a number for exact.
        def latest():
            at = guest.seen.rfind("cafesa3d: at ")
            return where(at) if at >= 0 else {}

        def keys(*names):
            for k in names:
                guest.sendkey(k)

        def location(text):
            m = re.search(r"to (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)$", text or "")
            return tuple(float(v) for v in m.groups()) if m else None

        cube = latest().get("Cube")
        mark = len(guest.seen)

        if cube:
            click(*cube)

        check(said("cafesa3d: selected ", mark) == "Cube", "a click did not select the Cube again")

        mark = len(guest.seen)
        keys("g")
        check(said("cafesa3d: move ", mark) == "Cube", "G did not start moving the Cube")

        # No button: the pointer alone, a few steps, then a click.
        for step in range(0, 8):
            guest.mouse_to(*R._to_tablet(cube[0] + 10 + step * 12, cube[1] + step * 6,
                                         width, height))
            time.sleep(0.15)

        click(cube[0] + 10 + 7 * 12, cube[1] + 7 * 6)
        moved = location(said("cafesa3d: moved Cube", mark))
        check(moved is not None and abs(moved[0] - (-1.75)) + abs(moved[1] - 0.45) > 0.1,
              "the Cube did not follow the pointer and stay where it was put: %r" % (moved,))

        if moved:
            mark = len(guest.seen)
            keys("g", "x", "2", "ret")
            exact = location(said("cafesa3d: moved Cube", mark))
            check(exact is not None and abs(exact[0] - (moved[0] + 2)) < 0.006
                  and abs(exact[1] - moved[1]) < 0.006 and abs(exact[2] - moved[2]) < 0.006,
                  "G X 2 did not move the Cube two metres along X: %r from %r" % (exact, moved))

        mark = len(guest.seen)
        keys("r", "z", "9", "0", "ret")
        turned = location(said("cafesa3d: rotated Cube", mark))
        check(turned is not None and abs(turned[0]) < 0.06 and abs(turned[1]) < 0.06
              and abs(turned[2] - 114) < 0.06,
              "R Z 90 did not turn the Cube from 24 to 114 degrees about Z: %r" % (turned,))

        mark = len(guest.seen)
        keys("s", "2", "ret")
        check(said("cafesa3d: scaled Cube to ", mark) == "2.000 2.000 2.000",
              "S 2 did not double the Cube")

        mark = len(guest.seen)
        keys("g")
        for step in range(0, 4):
            guest.mouse_to(*R._to_tablet(cube[0] + step * 20, cube[1] + 40, width, height))
            time.sleep(0.15)
        keys("esc")
        check(said("cafesa3d: cancelled ", mark) == "move", "Esc did not cancel G")

        mark = len(guest.seen)
        keys("r")
        guest.mouse_to(*R._to_tablet(cube[0] + 60, cube[1] - 30, width, height))
        time.sleep(0.2)
        guest.mouse_to(*R._to_tablet(cube[0] + 30, cube[1] - 70, width, height))
        time.sleep(0.2)
        guest.mouse_button(True, "right")
        time.sleep(0.2)
        guest.mouse_button(False, "right")
        check(said("cafesa3d: cancelled ", mark) == "rotate", "a right click did not cancel R")

        mark = len(guest.seen)
        keys("ctrl-z")
        check(said("cafesa3d: undid ", mark) == "scaled Cube", "Ctrl Z did not undo the scale")

        # **The tools' handles**, the way the KitBash guide teaches: a drag
        # on one arrow, ring or box, which changes that axis and no other.
        def drag(points):
            guest.mouse_to(*R._to_tablet(points[0][0], points[0][1], width, height))
            time.sleep(0.3)
            guest.mouse_button(True)
            time.sleep(0.2)

            for p in points[1:]:
                guest.mouse_to(*R._to_tablet(p[0], p[1], width, height))
                time.sleep(0.15)

            guest.mouse_button(False)
            time.sleep(0.6)

        def handles(tool):
            mark_ = len(guest.seen)
            click(ox + header["tool:" + tool][0], oy + header["tool:" + tool][1])
            said("cafesa3d: tool ", mark_)
            # The handles are said with the positions after the click's release.
            line = said("cafesa3d: handles " + tool + " ", mark_) or ""
            return dict((m.group(1), (ox + int(m.group(2)), oy + int(m.group(3))))
                        for m in re.finditer(r"(\w+) (-?\d+),(-?\d+)", line))

        def trio(text):
            return location(text)

        before = None
        mv = handles("move")
        check("X" in mv and "free" in mv, "the Move tool shows no handles: %r" % mv)

        if "X" in mv and "free" in mv:
            gx, gy = mv["X"]
            vx, vy = gx - mv["free"][0], gy - mv["free"][1]
            mark = len(guest.seen)
            drag([(gx, gy)] + [(gx + vx * k / 6, gy + vy * k / 6) for k in range(1, 7)])
            after = trio(said("cafesa3d: moved Cube", mark))
            before = moved and exact
            check(after is not None and exact is not None and abs(after[0] - exact[0]) > 0.1
                  and abs(after[1] - exact[1]) < 0.006 and abs(after[2] - exact[2]) < 0.006,
                  "dragging Move's X arrow did not move the Cube along X alone: %r from %r"
                  % (after, exact))

        rt = handles("rotate")
        now = latest().get("Cube")
        check("Z" in rt and now, "the Rotate tool shows no Z ring: %r" % rt)

        if "Z" in rt and now:
            gx, gy = rt["Z"]
            r0 = ((gx - now[0]) ** 2 + (gy - now[1]) ** 2) ** 0.5
            a0 = __import__("math").atan2(gy - now[1], gx - now[0])
            pts = [(now[0] + r0 * __import__("math").cos(a0 + k * 0.12),
                    now[1] + r0 * __import__("math").sin(a0 + k * 0.12)) for k in range(0, 7)]
            mark = len(guest.seen)
            drag(pts)
            turned2 = trio(said("cafesa3d: rotated Cube", mark))
            check(turned2 is not None and abs(turned2[0]) < 0.06 and abs(turned2[1]) < 0.06
                  and abs(turned2[2] - 114) > 5,
                  "dragging Rotate's Z ring did not turn the Cube about Z alone: %r" % (turned2,))

        sc = handles("scale")
        check("X" in sc and "free" in sc, "the Scale tool shows no handles: %r" % sc)

        if "X" in sc and "free" in sc:
            gx, gy = sc["X"]
            vx, vy = gx - sc["free"][0], gy - sc["free"][1]
            mark = len(guest.seen)
            drag([(gx, gy)] + [(gx + vx * k / 5, gy + vy * k / 5) for k in range(1, 6)])
            grown = trio(said("cafesa3d: scaled Cube", mark))
            check(grown is not None and grown[0] > 1.2 and abs(grown[1] - 1) < 0.002
                  and abs(grown[2] - 1) < 0.002,
                  "dragging Scale's X box did not stretch the Cube along X alone: %r" % (grown,))

        handles("select")

        # **Properties' numbers**: typed, scrubbed, and a sphere remade.
        def fields_after(since):
            line = said("cafesa3d: fields ", since) or ""
            return dict((m.group(1), (ox + int(m.group(2)), oy + int(m.group(3))))
                        for m in re.finditer(r"(\w+) (\d+),(\d+)", line))

        mark = len(guest.seen)
        click(ox + tabs["object"][0], oy + tabs["object"][1])
        fl = fields_after(mark)
        check("loc1" in fl and "rot3" in fl, "the Object tab shows no fields: %r" % fl)

        if "loc1" in fl and "rot3" in fl:
            mark = len(guest.seen)
            click(*fl["loc1"])
            check(said("cafesa3d: editing ", mark) == "Location X of Cube",
                  "a click on Location X did not start typing into it")
            keys("1", "dot", "5", "ret")
            check(said("cafesa3d: set Location X of Cube to ", mark) == "1.50 - the scene is 2062 triangles",
                  "typing 1.5 into Location X did not put the Cube there")

            rz = turned2[2] if turned2 else 114.0
            mark = len(guest.seen)
            x0, y0 = fl["rot3"]
            drag([(x0, y0)] + [(x0 + k * 8, y0) for k in range(1, 6)])
            got = said("cafesa3d: set Rotation Z of Cube to ", mark)
            m = re.match(r"(-?[\d.]+)", got or "")
            check(m is not None and abs(float(m.group(1)) - (rz + 20)) < 1.6,
                  "dragging across Rotation Z did not turn it 20 degrees from %.1f: %r" % (rz, got))

        mark = len(guest.seen)
        gr = rows.get("Gold")
        click(ox + gr[0], oy + gr[1])
        click(ox + tabs["data"][0], oy + tabs["data"][1])
        fl = fields_after(guest.seen.rfind("cafesa3d: tab data"))
        check("segments" in fl, "the Gold ball's Data tab shows no Segments: %r" % fl)

        if "segments" in fl:
            mark = len(guest.seen)
            click(*fl["segments"])
            keys("8", "ret")
            check(said("cafesa3d: set Segments of Gold to ", mark) == "8 - the scene is 1342 triangles",
                  "8 segments did not remake the gold ball as 240 triangles of 960")

            mark = len(guest.seen)
            click(*fl["radius"])
            keys("5", "esc")
            check(said("cafesa3d: left the field as it was", mark) is not None,
                  "Esc did not leave the radius as it was")

            mark = len(guest.seen)
            keys("ctrl-z")
            check(said("cafesa3d: undid ", mark) == "set Segments of Gold",
                  "Ctrl Z did not undo the segments")

        # **F frames the selection** (Diego, 26 September: "like blender
        # does"): the gold ball, chosen above, brought to the middle of the
        # view and near enough to fill it, its size read from its triangles.
        mark = len(guest.seen)
        keys("f")
        got = said("cafesa3d: framed ", mark) or ""
        # Across is its box's diagonal - a 0.6 m ball's is 2.08 m - and the
        # sphere round that box is what is fitted to the view.
        m = re.match(r"Gold, ([\d.]+) m across, from ([\d.]+) m$", got)
        check(m is not None and abs(float(m.group(1)) - 2.08) < 0.02 and float(m.group(2)) < 4,
              "F did not frame the gold ball, a 0.6 m sphere, from close by: %r" % got)
        now = where(mark).get("Gold")
        middle = (ox + 46 + view0[0] // 2, oy + 46 + view0[1] // 2)
        check(now is not None and abs(now[0] - middle[0]) <= 3 and abs(now[1] - middle[1]) <= 3,
              "after F the gold ball is at %r, not the view's middle %r" % (now, middle))

        # **The samples** - the house, the car and the plane Diego asked
        # for - opened as a person opens them: the dots, Open a sample, and
        # the scene; each with every object read and its colours on screen.
        def whiteish(c):
            r, g, b = (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff
            return r > 190 and g > 190 and b > 190

        colours = {"House": (reddish, 400), "Car": (reddish, 1500), "Plane": (whiteish, 1500)}

        # The second cut of the samples (`tools/cafesa3d_samples.py`): meshes
        # of their own and textures, a megabyte of glTF each.
        for row, (name, objects) in enumerate((("House", 202), ("Car", 76), ("Plane", 106))):
            mark = len(guest.seen)
            click(ox + header["more"][0], oy + header["more"][1])
            opened = said("cafesa3d: more menu at ", mark)
            m = re.match(r"(\d+),(\d+), (\d+) wide, rows of (\d+)", opened or "")
            check(m is not None, "the dots did not open their menu")

            if not m:
                continue

            mx, my, mw, rh = (int(v) for v in m.groups())
            click(mx + 24, my + 2 + rh // 2)                         # Open a sample
            click(mx + mw - 2 + 30, my + 2 + 2 + row * rh + rh // 2)   # the scene
            got = said("cafesa3d: opened ", mark, 120)
            check(got is not None and got.startswith("%s, %d objects" % (name, objects))
                  and got.endswith(", 0 skipped"),
                  "Open a sample, %s did not open all %d of its objects: %r" % (name, objects, got))

            test, least = colours[name]
            seen = count(screen(), (ox + 46, oy + 46, ox + 1060, oy + 790), test)
            check(seen > least, "%s opened but its colours are not on the screen (%d pixels)"
                  % (name, seen))
            open("/private/tmp/cafesa3d-%s.ppm" % name.lower(), "wb").write(guest.screendump()) \
                if os.environ.get("CAFESA3D_SHOTS") else None

        # **Saving, and opening what was saved** (step 5): the plane - a
        # megabyte, its meshes base64 in the file - saved with Ctrl S, which
        # asks where because a sample is nobody's file yet; the house opened
        # over it; the plane opened again from the Open panel with every
        # object and every triangle it had; and Ctrl S once more, which now
        # knows the file and writes the same bytes. The panels are driven at
        # their buttons, which sit where `panel.lua` puts them in a window
        # whose place Cafesa3D says.
        def panel_button(kind, since):
            at = said("cafesa3d: %s panel at " % kind, since, 30)
            m = re.match(r"(\d+),(\d+)", at or "")

            if not m:
                return None

            return int(m.group(1)) + 640 - 12 - 48, int(m.group(2)) + 420 - 72 + 10 + 12

        plane_triangles = None

        for line in guest.seen.split("\n"):
            m = re.search(r"cafesa3d: opened Plane, 106 objects, (\d+) triangles", line)

            if m:
                plane_triangles = int(m.group(1))

        # The plane as it is on the screen before it is saved: opened again,
        # it must be the same picture, pixel for pixel - which a mesh whose
        # bytes were mangled on the way, the right length and the wrong
        # points, would not be, where counting its triangles would pass it.
        m = re.search(r"the view (\d+) by (\d+)", summary)
        vw0, vh0 = (int(v) for v in m.groups()) if m else (900, 700)
        plane_box = (ox + 46, oy + 46, ox + 46 + vw0, oy + 46 + vh0)

        def parked():
            """The screen with the pointer off the window, where it cannot be
            a difference between two pictures."""
            guest.mouse_to(*R._to_tablet(width - 4, height - 4, width, height))
            return screen()

        before_save = parked()

        mark = len(guest.seen)
        keys("ctrl-s")
        button = panel_button("save", mark)
        check(button is not None, "Ctrl S on a sample did not ask where to save it")

        if button:
            click(*button)

        saved = said("cafesa3d: saved ", mark, 120) or ""
        m = re.match(r"/home/Scenes/plane\.gltf, 106 objects, (\d+) bytes$", saved)
        check(m is not None and int(m.group(1)) > 200000,
              "Save did not write the plane to /home/Scenes/plane.gltf: %r" % saved)
        size = m and m.group(1)

        mark = len(guest.seen)
        click(ox + header["more"][0], oy + header["more"][1])
        opened = said("cafesa3d: more menu at ", mark)
        m = re.match(r"(\d+),(\d+), (\d+) wide, rows of (\d+)", opened or "")

        if m:
            mx, my, mw, rh = (int(v) for v in m.groups())
            click(mx + 24, my + 2 + rh // 2)                        # Open a sample
            click(mx + mw - 2 + 30, my + 2 + 2 + rh // 2)           # the house
            check((said("cafesa3d: opened ", mark, 120) or "").startswith("House, 202 objects"),
                  "the house did not open over the saved plane")

        mark = len(guest.seen)
        keys("ctrl-o")
        button = panel_button("open", mark)
        check(button is not None, "Ctrl O did not open the Open panel")

        if button:
            click(*button)

        got = said("cafesa3d: opened ", mark, 180) or ""
        check(plane_triangles is not None and got == "Plane, 106 objects, %d triangles, "
              "0 skipped" % plane_triangles,
              "the saved plane did not open with its %s triangles: %r" % (plane_triangles, got))

        after_open = parked()
        x0, y0, x1, y1 = plane_box
        moved = sum(1 for y in range(y0, y1, 3) for x in range(x0, x1, 3)
                    if before_save(x, y) != after_open(x, y))
        check(moved == 0, "the plane opened again does not look as it did before it was "
              "saved: %d pixels differ" % moved)

        mark = len(guest.seen)
        keys("ctrl-s")
        again = said("cafesa3d: saved ", mark, 120) or ""
        check(size is not None and again == "/home/Scenes/plane.gltf, 106 objects, %s bytes"
              % size, "Ctrl S on the opened file did not write the same file: %r" % again)

        m = re.search(r"the view (\d+) by (\d+)", summary)
        vw, vh = (int(v) for v in m.groups()) if m else (900, 700)

        # **The tutorial** (`docs/cafesa3d-tutorial/`): F1 opens the browser
        # on the pages the image carries, and the first page's pictures are
        # all read and decoded; the dots' Tutorial opens it again. Between
        # the two, a click on the foot - where nothing is - brings Cafesa3D
        # back in front of the browser, and another after them, so what
        # follows has its window to itself.
        index = "asset:tutorial/cafesa3d/index.html"
        foot = (ox + vw + 300, oy + 46 + vh + 15)

        def showed(shown):
            m = re.match(re.escape(index) + r', "Cafesa3D tutorial", \d+ pixels tall, '
                         r"(\d+) pictures, (\d+) missing$", shown or "")
            return m and int(m.group(1)) > 0 and int(m.group(2)) == 0

        mark = len(guest.seen)
        keys("f1")
        got = said("cafesa3d: tutorial at ", mark, 30)
        check(got == index, "F1 did not open the tutorial: %r" % got)
        shown = said("browser: showing ", mark, 90)
        check(showed(shown), "the browser did not show the tutorial's first page with "
              "every picture on it: %r" % shown)

        click(*foot)
        mark = len(guest.seen)
        click(ox + header["more"][0], oy + header["more"][1])
        opened = said("cafesa3d: more menu at ", mark)
        m = re.match(r"(\d+),(\d+), (\d+) wide, rows of (\d+)", opened or "")
        check(m is not None, "the dots did not open their menu for the tutorial")

        if m:
            mx, my, mw, rh = (int(v) for v in m.groups())
            click(mx + 24, my + 2 + rh + rh // 2)                   # Tutorial
            got = said("cafesa3d: tutorial at ", mark, 30)
            check(got == index, "the dots' Tutorial did not open the tutorial: %r" % got)
            shown = said("browser: showing ", mark, 90)
            check(showed(shown), "the browser did not show the tutorial from the dots: %r"
                  % shown)

        click(*foot)

        # **Rendered and F12** (step 3): the plane, ray traced on every
        # processor - in the view, and through its camera in a window of its
        # own. Only each render's first pass is waited for: every pixel
        # traced once is the whole of the machinery, and 64 or 256 passes
        # under TCG would be minutes spent proving the same thing again.
        view_box = (ox + 46, oy + 46, ox + 46 + vw, oy + 46 + vh)
        foot_box = (ox + 10, oy + 46 + vh + 4, ox + 700, oy + 46 + vh + 26)

        def differing(a, b, box):
            x0, y0, x1, y1 = box
            n = total = 0

            for y in range(y0, y1, 4):
                for x in range(x0, x1, 4):
                    total += 1
                    n += a(x, y) != b(x, y)

            return n, total

        # Every processor the board was given: four on ARM, and the x86
        # machine here is one.
        cores = 4 if R.machine(image) == "aarch64" else 1
        solid = screen()
        mark = len(guest.seen)
        click(ox + header["rendered"][0], oy + header["rendered"][1])
        started = said("cafesa3d: rendering the view, ", mark, 30) or ""
        threads = re.search(r"on (\d+) threads?", started)
        check(threads is not None and int(threads.group(1)) == cores,
              "Rendered did not start a thread on each of the %d processors: %r"
              % (cores, started))
        first = said("cafesa3d: the view's first pass in ", mark, 180)
        check(first is not None, "the Rendered view's first pass never finished")
        traced = screen()
        n, total = differing(solid, traced, view_box)
        check(n > total // 2, "the Rendered view looks like the Solid one after its first "
              "pass: %d of %d pixels changed" % (n, total))

        # A second window in the same process must not take the first one's
        # faces with it: the foot bar is the same pixels before and after.
        foot_before = [traced(x, y) for y in range(foot_box[1], foot_box[3])
                       for x in range(foot_box[0], foot_box[2])]
        mark = len(guest.seen)
        keys("f12")
        opened = said("cafesa3d: render window at ", mark, 30)
        started = said("cafesa3d: rendering ", mark, 30) or ""
        check(opened is not None, "F12 did not open the Render window")
        check("through the camera" in started
              and re.search(r"on %d threads?$" % cores, started) is not None,
              "F12 did not render through the camera on %d threads: %r" % (cores, started))
        first = said("cafesa3d: the render's first pass in ", mark, 180)
        check(first is not None, "the Render window's first pass never finished")
        at = screen()

        if opened:
            rx, ry = (int(v) for v in opened.split(","))
            drawn = count(at, (rx + 20, ry + 66, rx + 620, ry + 386),
                          lambda c: c != 0x1d1f24)
            check(drawn > 20000, "the Render window's picture is still empty after its "
                  "first pass (%d pixels)" % drawn)

        foot_after = [at(x, y) for y in range(foot_box[1], foot_box[3])
                      for x in range(foot_box[0], foot_box[2])]
        check(foot_after == foot_before, "opening the Render window changed the main "
              "window's foot bar: its faces were lost (%d pixels differ)"
              % sum(a != b for a, b in zip(foot_before, foot_after)))

        # **Stop** (Diego, 26 September: "the render screen needs a stop
        # button"): the button that says Render again once a render is done
        # says Stop while it runs, in the same place. Pressed after the first
        # pass, the render stops short of its 256 samples and the picture so
        # far stays in the window.
        controls = said("cafesa3d: render controls ", mark, 10) or ""
        m = re.match(r"again (\d+),(\d+);", controls)

        if opened and m:
            mark = len(guest.seen)
            click(rx + int(m.group(1)), ry + int(m.group(2)))
            got = said("cafesa3d: stopped the render at ", mark, 30) or ""
            m = re.match(r"(\d+) of 256 samples, after [\d.]+ s$", got)
            check(m is not None and 1 <= int(m.group(1)) < 256,
                  "Stop did not stop the render short of its samples: %r" % got)
            kept = count(screen(), (rx + 20, ry + 66, rx + 620, ry + 386),
                         lambda c: c != 0x1d1f24)
            check(kept > 20000, "the stopped render's picture is gone (%d pixels)" % kept)

        # **Full screen** (F11): the same scene in a window the size of the
        # screen, at its corner, the view taking what the panels do not and
        # the Rendered view starting again at that size; a click landing in
        # the new layout; the screen's far corner Cafesa3D's rather than the
        # desktop's; and F11 again, the window it was.
        click(ox + vw + 300, oy + 46 + vh + 15)                 # the foot: in front again
        mark = len(guest.seen)
        keys("f11")
        full = said("cafesa3d: full screen, ", mark, 60) or ""
        want = "%d by %d, the view %d by %d, 106 objects" % (width, height, width - 386,
                                                              height - 76)
        check(full == want, "F11 did not lay Cafesa3D out across the screen: %r, not %r"
              % (full, want))
        check(said("cafesa3d: window at ", mark, 10) == "0,0",
              "the full-screen window is not at the screen's corner")
        check((said("cafesa3d: rendering the view, ", mark, 60) or "").startswith(
              "%d by %d" % (width - 386, height - 76)),
              "the Rendered view did not start again at the full-screen size")

        tabs_now = dict((m.group(1), (int(m.group(2)), int(m.group(3))))
                        for m in re.finditer(r"(\w+) (\d+),(\d+)",
                                             said("cafesa3d: tabs ", mark) or ""))
        top = dict((m.group(1), (int(m.group(2)), int(m.group(3))))
                   for m in re.finditer(r"([\w:]+) (\d+),(\d+)",
                                        said("cafesa3d: controls ", mark) or ""))
        mark = len(guest.seen)

        if "world" in tabs_now:
            click(*tabs_now["world"])

        check(said("cafesa3d: tab ", mark) == "world",
              "a click on the World tab where full screen put it did not open it")

        # The dots, at the screen's top right corner - which is where a
        # window's close box is, and a full-screen window has no tab: the
        # window manager took the press for one and asked Cafesa3D to close.
        mark = len(guest.seen)

        if "more" in top:
            click(*top["more"])

        check(said("cafesa3d: more menu at ", mark, 10) is not None,
              "a click on the dots in full screen did not open their menu")
        click(width // 2, height // 2)                           # and away

        at = screen()
        corner = at(width - 20, height - 12)
        check(corner not in (0x2f5bb8, 0x305cba) and (corner >> 16) > 0xc0,
              "the screen's far corner is not Cafesa3D's foot: %06x" % corner)

        mark = len(guest.seen)
        keys("f11")
        back = said("cafesa3d: a window, ", mark, 60) or ""
        check(back == "%d by %d, the view %d by %d, 106 objects" % (vw + 386, vh + 76, vw, vh),
              "F11 again did not bring the window back as it was: %r" % back)

        # **The Render tab and a saved picture**: Preview, 1280 by 720 and one
        # sample set in the tab - each said back - then F12, which makes the
        # Render window again at the new size, the render finished, and Save
        # as PNG through the panel into /home/Renders. The file itself is
        # read back off the disk once the machine has stopped, below.
        mark = len(guest.seen)
        click(ox + tabs["render"][0], oy + tabs["render"][1])
        check(said("cafesa3d: tab ", mark) == "render", "the Render tab did not open")

        # Read after the tab's own line: the World tab can draw once more
        # after the mark, and on one slow processor did - so its fields were
        # taken for these, and the Samples click was never made (x86-64,
        # 26 September; it looked like a lost click for an afternoon).
        opened_at = guest.seen.find("cafesa3d: tab render", mark)
        chips_now = placed(said("cafesa3d: chips ", opened_at))
        fields_now = placed(said("cafesa3d: fields ", opened_at))
        check("samples" in fields_now and "integrator:Preview" in chips_now,
              "the Render tab's fields and chips were not said: %r %r"
              % (sorted(fields_now), sorted(chips_now)))

        for key, sets in (("integrator:Preview", "integrator of the render to Preview"),
                          ("size:1280x720", "size of the render to 1280 by 720")):
            mark = len(guest.seen)

            if key in chips_now:
                click(*chips_now[key])

            check(said("cafesa3d: set " + sets.split(" to ")[0] + " to ", mark, 10)
                  == sets.split(" to ")[1], "the %s chip did not set %s" % (key, sets))

        mark = len(guest.seen)

        # Typed once the field says it is being typed into: a click that
        # lands while the view's render starts again can come too early.
        for _ in range(2):
            if "samples" in fields_now:
                click(*fields_now["samples"])

            if said("cafesa3d: editing Samples", mark, 5) is not None:
                keys("1", "ret")
                break

        check(said("cafesa3d: set Samples of the render to ", mark, 10) == "1",
              "typing 1 into Samples did not set it")

        click(ox + vw + 300, oy + 46 + vh + 15)                 # the foot: in front
        mark = len(guest.seen)
        keys("f12")
        rat = said("cafesa3d: render window at ", mark, 30)
        began = said("cafesa3d: rendering ", mark, 30) or ""
        buttons = said("cafesa3d: render controls ", mark, 30) or ""
        check(began.startswith("plane.gltf through the camera, 1280 by 720"),
              "F12 did not render at the size the tab says: %r" % began)
        done = said("cafesa3d: rendered 1 samples", mark, 300)
        check(done is not None, "the one-sample render never finished")

        m = re.search(r"save (\d+),(\d+)$", buttons)
        saved_png = None

        if rat and m:
            rx, ry = (int(v) for v in rat.split(","))
            mark = len(guest.seen)
            click(rx + int(m.group(1)), ry + int(m.group(2)))
            button = panel_button("save", mark)

            if button:
                click(*button)

            saved_png = said("cafesa3d: saved the render to ", mark, 60) or ""

        check(saved_png is not None and re.match(r"/home/Renders/plane\.png, 1280 by 720, "
                                                 r"\d+ bytes$", saved_png) is not None,
              "Save as PNG did not write the render to /home/Renders: %r" % saved_png)

        # Cafesa3D in front again, by its own title in the header - the
        # larger Render window now covers the foot this suite clicks for it.
        click(ox + 40, oy + 22)

        # The dots' menu, and its `i`th row clicked: a click on the dots brings
        # Cafesa3D in front, whatever else has the focus.
        def dots_row(i):
            mark_ = len(guest.seen)
            click(ox + header["more"][0], oy + header["more"][1])
            got_ = said("cafesa3d: more menu at ", mark_)
            m_ = re.match(r"(\d+),(\d+), (\d+) wide, rows of (\d+)", got_ or "")

            if not m_:
                return None

            mx_, my_, mw_, rh_ = (int(v) for v in m_.groups())
            click(mx_ + 24, my_ + 2 + i * rh_ + rh_ // 2)
            return mx_, my_, mw_, rh_

        # **The broken scene** (made above, on the disk beside the saved
        # plane): opened from the panel's second row, its box kept, its
        # triangle skipped and named, and Cafesa3D still there to say so.
        mark = len(guest.seen)
        dots_row(4)                                                # Open...
        at = said("cafesa3d: open panel at ", mark, 30)
        m = re.match(r"(\d+),(\d+)", at or "")
        check(m is not None, "the dots' Open... did not open the panel for the broken scene")

        if m:
            px, py = int(m.group(1)), int(m.group(2))
            # The third row - plane.gltf and x-parts.glb before it: the list
            # starts under its header, a line of text and four pixels below
            # the trail's 34, and a row is the look's 32 (`theme.lua`).
            click(px + 208 + 60, py + 34 + 26 + 2 * 32 + 16)
            click(px + 580, py + 370)                            # Open

        got = said("cafesa3d: opened ", mark, 60) or ""
        check(got.startswith("Broken, 1 objects, ") and got.endswith(", 1 skipped"),
              "the broken scene did not open with its box and its triangle skipped: %r" % got)
        check(said("cafesa3d:   skipped ", mark, 10)
              == "Tri: a mesh whose faces name points it does not have",
              "the skipped triangle was not named with why")

        # **Importing and exporting** (step 5c), through the dots' Import...
        # and Export, into the broken scene now open: the STL cube, the OBJ's
        # two parts with the MTL beside them found, and the .glb's three
        # parts; then the lot exported as STL - a binary STL is 84 bytes and
        # 50 a triangle - and that file imported back.
        def import_row(row, since):
            at_ = said("cafesa3d: open panel at ", since, 30)
            m_ = re.match(r"(\d+),(\d+)", at_ or "")

            if m_:
                px_, py_ = int(m_.group(1)), int(m_.group(2))
                click(px_ + 208 + 60, py_ + 34 + 26 + (row - 1) * 32 + 16)
                click(px_ + 580, py_ + 370)                          # Open

        found = said("cafesa3d: translator STL from ", mark, 1) or ""

        for row, name, want in ((1, "a-cube", "1 objects"), (2, "b-parts", "2 objects"),
                                (4, "Parts", "3 objects")):
            mark = len(guest.seen)
            dots_row(5)                                            # Import...
            import_row(row, mark)
            got = said("cafesa3d: imported ", mark, 60) or ""
            check(got.startswith("%s, %s, " % (name, want)) and got.endswith(", 0 skipped"),
                  "importing %s did not bring in %s with nothing skipped: %r" % (name, want, got))

        found = found or said("cafesa3d: translator STL from ", 0, 1) or ""
        check(found.startswith("/lib/translators/stl.lua, reads stl, writes stl"),
              "the STL translator was not found in /lib/translators: %r" % found)

        mark = len(guest.seen)
        opened = dots_row(8)                                       # Export

        if opened:
            mx, my, mw, rh = opened
            click(mx + mw - 2 + 30, my + 2 + 8 * rh + 2 + rh + rh // 2)    # STL...
            button = panel_button("save", mark)

            if button:
                click(*button)

        got = said("cafesa3d: exported ", mark, 60) or ""
        m = re.match(r"/home/Scenes/zz-broken\.stl, 7 objects, (\d+) bytes$", got)
        check(m is not None and (int(m.group(1)) - 84) % 50 == 0 and int(m.group(1)) > 84,
              "Export, STL did not write the scene's seven objects as a binary STL: %r" % got)

        mark = len(guest.seen)
        dots_row(5)
        import_row(6, mark)
        got = said("cafesa3d: imported ", mark, 60) or ""
        check(got.startswith("zz-broken, 1 objects, ") and got.endswith(", 0 skipped"),
              "the exported STL did not come back in as one object: %r" % got)

        # **FBX** (step 5c): Blender's Suzanne in seven materials, through the
        # FBX translator and ufbx - a part for each material, named for it,
        # nothing skipped.
        mark = len(guest.seen)
        dots_row(5)
        import_row(7, mark)
        got = said("cafesa3d: imported ", mark, 60) or ""
        check(got.startswith("zz-monkey, 7 objects, ") and got.endswith(", 0 skipped"),
              "the FBX did not come in as Suzanne's seven parts: %r" % got)
        check("Suzanne Nose " in (said("cafesa3d: at ",
                                       guest.seen.find("cafesa3d: imported zz-monkey", mark),
                                       10) or ""),
              "the FBX's parts are not named for the object and its material")
        found = said("cafesa3d: translator Autodesk FBX from ", 0, 1) or ""
        check(found.startswith("/lib/translators/fbx.lua, reads fbx"),
              "the FBX translator was not found in /lib/translators: %r" % found)

        # And it is still running: nothing above raised.
        check("stack traceback" not in guest.seen and "cafesa3d.lua:" not in guest.seen,
              "Cafesa3D raised an error:\n" + guest.seen[-1200:])
    finally:
        guest.close()

    # **The saved render, off the disk**, with the machine stopped: a PNG,
    # 1280 by 720, eight bits of red, green and blue, whose picture is not
    # one colour all over.
    got = os.path.join(WORK, "plane.png")
    fetched = subprocess.run([os.path.join(os.path.dirname(HERE), "build", "host", "lua"),
                              os.path.join(HERE, "kfs.lua"), "get", HOME_DISK,
                              "/home/Renders/plane.png", got],
                             capture_output=True, cwd=os.path.dirname(HERE))
    size, colours = None, 0

    if fetched.returncode == 0 and os.path.exists(got):
        import struct
        import zlib

        data = open(got, "rb").read()

        if data[:8] == b"\x89PNG\r\n\x1a\n" and data[12:16] == b"IHDR":
            w, h, depth, kind = struct.unpack(">IIBB", data[16:26])
            size = (w, h, depth, kind)
            at, idat = 8, b""

            while at < len(data):
                n, name = struct.unpack(">I4s", data[at:at + 8])

                if name == b"IDAT":
                    idat += data[at + 8:at + 8 + n]

                at += 12 + n

            raw = zlib.decompress(idat)
            stride, prev, seen = w * 3, bytearray(w * 3), set()

            # Each row unfiltered as PNG says (none, sub, up, average,
            # Paeth), and every hundred-and-first pixel's colour kept.
            for y in range(h):
                kind_, row = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:
                                                                 (y + 1) * (stride + 1)])

                for i in range(stride):
                    a = row[i - 3] if i >= 3 else 0
                    b, c = prev[i], (prev[i - 3] if i >= 3 else 0)

                    if kind_ == 1:
                        row[i] = (row[i] + a) & 255
                    elif kind_ == 2:
                        row[i] = (row[i] + b) & 255
                    elif kind_ == 3:
                        row[i] = (row[i] + (a + b) // 2) & 255
                    elif kind_ == 4:
                        pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                        row[i] = (row[i] + (a if pa <= pb and pa <= pc
                                            else b if pb <= pc else c)) & 255

                for x in range(y % 101, w, 101):
                    seen.add(bytes(row[x * 3:x * 3 + 3]))

                prev = row

            colours = len(seen)

    check(size == (1280, 720, 8, 2) and colours > 50,
          "the saved render off the disk is not a 1280 by 720 picture of something: %r, "
          "%d colours in it" % (size, colours))

    if failed:
        print("FAIL: %d of %d checks on Cafesa3D:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on Cafesa3D (the still life, the Cube outlined; the gold ball "
          "selected by a click and the outline moved to it; a row of the Outliner; an "
          "eye hiding and showing; the Material tab, a preset, a swatch, a texture and a "
          "colour typed; a drag turning the view; the wheel; "
          "Z to Wireframe with the faces gone; 7 from the top; Add, Mesh, Cube at the "
          "3D cursor, undone and redone; Shift D, Delete, and X asking first; G following "
          "the pointer with no button, G X 2, R Z 90, S 2, Esc and a right click "
          "cancelling, Ctrl Z; the Move, Rotate and Scale handles each changing its "
          "axis alone; Location X typed, Rotation Z scrubbed, a sphere's segments "
          "remaking it, Esc, Ctrl Z; the house, the car and the plane opened from the "
          "samples, every object read and their colours on the screen; the plane saved "
          "to /home, the house opened over it, the plane opened again whole and saved "
          "again the same; the tutorial "
          "opened by F1 and from the dots, its first page and every picture on it shown "
          "in the browser; the plane "
          "Rendered on every processor and F12 through its camera, each first pass drawn, "
          "and the main window's faces untouched by the second; F11 to full screen with "
          "the scene and the render carried over and a click landing, and back; the Render "
          "tab's integrator, size and samples set, F12 at that size, and the picture saved "
          "as a PNG read back off the disk; a scene "
          "with a broken mesh opened with that mesh skipped and named; an STL, an OBJ with "
          "its MTL and a .glb imported, the scene exported as STL and imported back, "
          "an FBX of Blender's in seven materials imported; F framing the gold ball; Stop "
          "in the middle of a render)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
