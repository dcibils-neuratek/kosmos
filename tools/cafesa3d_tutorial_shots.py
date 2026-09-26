#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The pictures in Cafesa3D's tutorial, taken by doing what it says.

Boots the image, opens Cafesa3D, and takes the screen captures the pages in
`docs/cafesa3d-tutorial/` show: the window, the Add menu, G held to an axis,
the Object and Material tabs, the still life rendered - and then builds the
car of chapters 6 and 7 through the keyboard and the pointer, step by step
as the pages give them, photographing it on the way and rendering it with
F12 at the end.

So the pictures are what the application looks like *now*, and the run is
also the proof that the car chapters can be followed: a step whose field or
chip is not where the page says fails here, by name. Diego, 26 September:
"Make sure we add screen captures to the HTML tutorials to make it easy to
follow".

Not a suite - it takes twenty minutes and more under TCG, most of it the
render - and not run by `make test`. `make tutorial-shots` runs it; run it
again when Cafesa3D's look changes, and look at the pictures before
committing them.

Each picture is cropped at the size it is shown, at most 860 pixels wide,
which is the page of Kosmos's browser: shown there pixel for pixel. The
whole window is halved, and the Render window, a little wider than the
page, is scaled to fit it.

Usage: cafesa3d_tutorial_shots.py IMAGE OUTDIR
"""

import os
import re
import struct
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                   # noqa: E402
from run_gallery import png                                  # noqa: E402

PAGE = 860          # the widest a picture is, to fit the browser's page

KEYNAME = {".": "dot", "-": "minus"}
MESH = {"Cube": 1, "UV Sphere": 3, "Cylinder": 5}           # rows of Add, Mesh


def halved(rows, w):
    """Each two by two averaged into one: for the whole window, which is
    shown at half its size, and crisper for being exactly half."""
    out = []

    for y in range(0, len(rows) - 1, 2):
        a, b = rows[y], rows[y + 1]
        row = bytearray()

        for x in range(0, w - 1, 2):
            for c in range(3):
                i, j = x * 3 + c, (x + 1) * 3 + c
                row.append((a[i] + a[j] + b[i] + b[j] + 2) // 4)

        out.append(bytes(row))

    return out, w // 2


def fitted(rows, w, nw):
    """Rows `w` wide made `nw` wide, and as much shorter: bilinear, for a
    window a little wider than the page - which cropping cut a word off."""
    h = len(rows)
    nh = max(1, round(h * nw / w))
    out = []

    for y in range(nh):
        fy = (y + 0.5) * h / nh - 0.5
        y0 = min(h - 1, max(0, int(fy)))
        y1, ty = min(h - 1, y0 + 1), min(1.0, max(0.0, fy - y0))
        row = bytearray()

        for x in range(nw):
            fx = (x + 0.5) * w / nw - 0.5
            x0 = min(w - 1, max(0, int(fx)))
            x1, tx = min(w - 1, x0 + 1), min(1.0, max(0.0, fx - x0))

            for c in range(3):
                top = rows[y0][x0 * 3 + c] * (1 - tx) + rows[y0][x1 * 3 + c] * tx
                bot = rows[y1][x0 * 3 + c] * (1 - tx) + rows[y1][x1 * 3 + c] * tx
                row.append(int(top * (1 - ty) + bot * ty + 0.5))

        out.append(bytes(row))

    return out, nw


OWN = b"Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE."


def owned(data):
    """A PNG with the project's licence line in a `tEXt` chunk after its
    header - the one place a picture can carry the line every file this
    project writes opens with, and where `assets2c.py` looks for it. A
    decoder skips a chunk it does not know, `gfx.png` included."""
    body = b"Copyright\0" + OWN
    chunk = (struct.pack(">I", len(body)) + b"tEXt" + body
             + struct.pack(">I", zlib.crc32(b"tEXt" + body) & 0xffffffff))

    return data[:33] + chunk + data[33:]            # after the signature and IHDR


def write(path, rows, w, halve=False):
    """Rows of RGB as a PNG, halved if asked and never wider than the page."""
    if halve:
        rows, w = halved(rows, w)

    if w > PAGE:
        rows, w = fitted(rows, w, PAGE)

    with open(path, "wb") as f:
        f.write(owned(png(w, len(rows), b"".join(rows))))

    print("  %s, %dx%d" % (os.path.basename(path), w, len(rows)), flush=True)


class Shots:
    def __init__(self, image, out):
        self.guest = R.Guest(image, 300)
        self.out = out
        self.problems = []
        self.W = self.H = None
        self.px = None

    # --- the guest -------------------------------------------------------

    def said(self, text, since, seconds=30):
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            at = self.guest.seen.find(text, since)

            if at >= 0 and "\n" in self.guest.seen[at + len(text):]:
                return self.guest.seen[at + len(text):].split("\n", 1)[0].strip()

            time.sleep(0.1)

        return None

    def mark(self):
        return len(self.guest.seen)

    def screen(self):
        time.sleep(1.2)
        self.W, self.H, self.px = R.parse_ppm(self.guest.screendump())

    def click(self, x, y):
        self.guest.mouse_to(*R._to_tablet(x, y, self.W, self.H))
        time.sleep(0.35)
        self.guest.mouse_button(True)
        time.sleep(0.2)
        self.guest.mouse_button(False)
        time.sleep(0.5)

    def point(self, x, y):
        self.guest.mouse_to(*R._to_tablet(x, y, self.W, self.H))
        time.sleep(0.3)

    def key(self, k):
        self.guest.sendkey(k)

    # --- pictures ---------------------------------------------------------

    def save(self, name, box, halve=False):
        """The screen inside `box` (x0, y0, x1, y1), as a PNG in OUTDIR."""
        x0, y0, x1, y1 = (max(0, box[0]), max(0, box[1]),
                          min(self.W, box[2]), min(self.H, box[3]))
        rows = [self.px[((y * self.W) + x0) * 3:((y * self.W) + x1) * 3]
                for y in range(y0, y1)]

        write(os.path.join(self.out, name), rows, x1 - x0, halve)

    def centred(self, cx, cy, w, h):
        """A box `w` by `h` round a point, kept inside the 3D view."""
        vx0, vy0 = self.ox + 46, self.oy + 46
        vx1, vy1 = vx0 + self.vw, vy0 + self.vh
        x0 = min(max(vx0, cx - w // 2), vx1 - w)
        y0 = min(max(vy0, cy - h // 2), vy1 - h)

        return (x0, y0, x0 + w, y0 + h)

    def props_box(self, height):
        """Properties: the tabs down its side and what the tab shows."""
        x0 = self.ox + 46 + self.vw
        y0 = self.oy + self.tabs["render"][1] - 24

        return (x0, y0, x0 + 340, y0 + height)

    # --- the application --------------------------------------------------

    def placed(self, line):
        return dict((m.group(1), (self.ox + int(m.group(2)), self.oy + int(m.group(3))))
                    for m in re.finditer(r"([\w:.]+) (-?\d+),(-?\d+)", line or ""))

    def latest(self, kind):
        at = self.guest.seen.rfind("cafesa3d: %s " % kind)

        if at < 0:
            return {}

        return self.placed(self.guest.seen[at + len("cafesa3d: %s " % kind):]
                           .split("\n", 1)[0])

    def field(self, name, text):
        time.sleep(0.6)
        pos = self.latest("fields").get(name)

        if not pos:
            self.problems.append("no field %s where the page says (there are %s)"
                                 % (name, sorted(self.latest("fields"))))
            return

        m = self.mark()
        self.click(*pos)

        if self.said("cafesa3d: editing ", m, 10) is None:
            self.problems.append("a click on %s did not start typing into it" % name)
            return

        for ch in text:
            self.key(KEYNAME.get(ch, ch))

        self.key("ret")
        # Typing the value a field already has sets nothing, and says so.
        self.said("cafesa3d: set ", m, 5)

    def fields3(self, name, a, b, c):
        for i, v in enumerate((a, b, c)):
            self.field("%s%d" % (name, i + 1), v)

    def chip(self, k):
        time.sleep(0.6)
        pos = self.latest("chips").get(k)

        if not pos:
            self.problems.append("no %s chip where the page says" % k)
            return

        m = self.mark()
        self.click(*pos)
        self.said("cafesa3d: set ", m, 10)

    def tab(self, name):
        m = self.mark()
        self.click(self.ox + self.tabs[name][0], self.oy + self.tabs[name][1])

        if self.said("cafesa3d: tab ", m, 10) != name:
            self.problems.append("the %s tab did not open" % name)

        self.said("cafesa3d: fields ", m, 5)

    def row(self, name):
        """A row of the Outliner, which lists names in order and fits seven."""
        i = sorted(self.names).index(name)

        if i >= 7:
            self.problems.append("%s is off the end of the Outliner" % name)

        self.click(self.ox + self.first_row[0], self.oy + self.first_row[1] + i * self.ROW)

    def add(self, what):
        m = self.mark()
        self.key("shift-a")
        opened = self.said("cafesa3d: add menu at ", m, 10)
        mx, my, mw, rh = (int(v) for v in re.match(
            r"(\d+),(\d+), (\d+) wide, rows of (\d+)", opened).groups())
        self.click(mx + 24, my + 2 + rh // 2)
        self.click(mx + mw - 2 + 30, my + 2 + 2 + MESH[what] * rh + rh // 2)
        added = self.said("cafesa3d: added ", m, 10) or ""
        self.names.append(added.split(",")[0])
        self.said("cafesa3d: fields ", m, 5)

    def copy(self):
        m = self.mark()
        self.key("shift-d")
        copied = self.said("cafesa3d: duplicated ", m, 10) or ""
        self.names.append(copied.rsplit(" as ", 1)[-1])
        self.key("esc")
        time.sleep(0.8)

    def where(self, name):
        return self.latest("at").get(name)

    # --- the run ----------------------------------------------------------

    def run(self):
        g = self.guest
        g.wait_for("kosmos> ", "a prompt")
        m = self.mark()
        g.type("wm cafesa3d")
        self.ox, self.oy = (int(v) for v in
                            self.said("cafesa3d: window at ", m, 90).split(","))
        rows = dict((r.group(1), (int(r.group(2)), int(r.group(3))))
                    for r in re.finditer(r"([\w.]+) (\d+),(\d+) eye \d+",
                                         self.said("cafesa3d: rows ", m)))
        self.tabs = dict((t.group(1), (int(t.group(2)), int(t.group(3))))
                         for t in re.finditer(r"(\w+) (\d+),(\d+)",
                                              self.said("cafesa3d: tabs ", m)))
        self.header = dict((h.group(1), (int(h.group(2)), int(h.group(3))))
                           for h in re.finditer(r"([\w:]+) (\d+),(\d+)",
                                                self.said("cafesa3d: controls ", m)))
        size = re.search(r"the view (\d+) by (\d+)", self.said("cafesa3d: 7 objects, ", m))
        self.vw, self.vh = int(size.group(1)), int(size.group(2))
        self.names = list(rows)
        self.first_row = rows["Camera"]
        self.ROW = rows["Cube"][1] - rows["Camera"][1]
        width, height = 46 + self.vw + 340, 46 + self.vh + 30
        time.sleep(2)

        print("Chapter 1: the window", flush=True)
        self.screen()
        self.save("interface.png", (self.ox - 2, self.oy - 28, self.ox + width + 2,
                                    self.oy + height + 2), halve=True)

        print("Chapter 2: the Add menu", flush=True)
        cube = self.where("Cube")
        self.point(self.ox + 46 + self.vw // 3, self.oy + 46 + self.vh // 3)
        m = self.mark()
        self.key("shift-a")
        opened = self.said("cafesa3d: add menu at ", m, 10)
        mx, my, mw, rh = (int(v) for v in re.match(
            r"(\d+),(\d+), (\d+) wide, rows of (\d+)", opened).groups())
        self.point(mx + 24, my + 2 + rh // 2)                   # Mesh, on the way past
        self.click(mx + 24, my + 2 + rh // 2)
        self.point(mx + mw + 40, my + 2 + 2 + MESH["Cube"] * rh + rh // 2)
        self.screen()
        self.save("add-menu.png", (mx - 16, my - 16, mx + mw + 230, my + 12 * rh + 16))
        self.click(self.ox + width - 120, self.oy + height - 15)   # away, on the foot

        print("Chapter 3: G, X", flush=True)
        self.screen()
        self.click(*cube)
        m = self.mark()
        self.key("g")
        self.said("cafesa3d: move ", m, 10)

        for step in range(1, 7):
            self.point(cube[0] + step * 14, cube[1] + step * 3)

        self.key("x")
        self.point(cube[0] + 110, cube[1] + 20)
        self.screen()
        self.save("grab.png", self.centred(cube[0] + 60, cube[1] - 20, 760, 400))
        self.key("esc")
        time.sleep(0.8)
        self.tab("object")
        self.screen()
        self.save("object-tab.png", self.props_box(250))

        print("Chapter 4: the Material tab", flush=True)
        self.tab("material")
        self.chip("texture:Brick")
        self.screen()
        self.save("material-tab.png", self.props_box(470))

        print("Chapter 5: the still life, rendered", flush=True)
        m = self.mark()
        self.click(self.ox + self.header["rendered"][0], self.oy + self.header["rendered"][1])
        self.said("cafesa3d: rendered the view, ", m, 1500)
        self.screen()
        self.save("rendered-view.png", self.centred(self.ox + 46 + self.vw // 2,
                                                    self.oy + 46 + self.vh // 2, 860, 500))
        self.click(self.ox + self.header["solid"][0], self.oy + self.header["solid"][1])

        self.car()

    def car(self):
        print("Chapter 6: clearing the table and setting the stage", flush=True)

        for n in ("Cube", "Glass", "Gold", "Cylinder"):
            m = self.mark()
            self.row(n)
            self.said("cafesa3d: selected ", m, 10)
            self.key("delete")

            if self.said("cafesa3d: deleted ", m, 10) != n:
                self.problems.append("step 1: %s was not deleted" % n)

            self.names.remove(n)

        self.row("Ground")
        self.tab("material")
        self.chip("preset:Plastic")
        self.field("base", "5a5d62")
        self.field("rough", "0.95")
        self.chip("texture:Noise")
        self.field("colour2", "2e3034")
        self.field("scale", "28")
        self.field("bump", "0.002")

        self.row("Light")
        self.tab("object")
        self.fields3("loc", "-4", "-6", "9")
        self.tab("data")
        self.field("power", "5000")
        self.field("radius", "1")

        self.tab("world")
        self.field("zenith", "6f93c8")
        self.field("horizon", "e3e8ee")
        self.field("strength", "0.9")

        self.row("Camera")
        self.tab("object")
        self.fields3("loc", "6.8", "-6.0", "2.9")

        print("Chapter 6: the body", flush=True)
        self.add("Cube")
        self.fields3("size", "4.2", "1.8", "0.56")
        self.tab("object")
        self.fields3("loc", "0", "0", "0.62")
        self.tab("material")
        self.chip("preset:Plastic")
        self.chip("swatch:1")
        self.field("rough", "0.12")

        self.copy()                                             # the bonnet
        self.tab("data")
        self.fields3("size", "1.6", "1.74", "0.1")
        self.tab("object")
        self.fields3("loc", "1.25", "0", "0.93")
        self.fields3("rot", "0", "6", "0")

        self.copy()                                             # the roof
        self.tab("data")
        self.fields3("size", "1.7", "1.58", "0.08")
        self.tab("object")
        self.fields3("loc", "-0.4", "0", "1.49")
        self.fields3("rot", "0", "0", "0")

        self.add("Cube")                                        # the cabin
        self.fields3("size", "2.0", "1.56", "0.5")
        self.tab("object")
        self.fields3("loc", "-0.35", "0", "1.2")
        self.tab("material")
        self.chip("preset:Plastic")
        self.field("base", "0e1014")
        self.field("rough", "0.04")
        self.screen()
        body = self.where("Cube") or (self.ox + 46 + self.vw // 2, self.oy + 46 + self.vh // 2)
        self.save("car-body.png", self.centred(body[0], body[1], 760, 440))

        print("Chapter 7: the wheels", flush=True)
        wheels = (("1.35", "-0.92"), ("-1.35", "0.92"), ("-1.35", "-0.92"))
        self.add("Cylinder")
        self.field("radius", "0.38")
        self.field("depth", "0.3")
        self.chip("shade:smooth")
        self.tab("object")
        self.fields3("rot", "90", "0", "0")
        self.fields3("loc", "1.35", "0.92", "0.38")
        self.tab("material")
        self.chip("preset:Plastic")
        self.chip("swatch:9")
        self.field("rough", "0.9")
        self.chip("texture:Noise")
        self.field("scale", "40")
        self.tab("object")

        for x, y in wheels:
            self.copy()
            self.fields3("loc", x, y, "0.38")

        self.add("Cylinder")                                    # the hubs
        self.field("radius", "0.2")
        self.field("depth", "0.32")
        self.chip("shade:smooth")
        self.tab("object")
        self.fields3("rot", "90", "0", "0")
        self.fields3("loc", "1.35", "0.92", "0.38")
        self.tab("material")
        self.chip("preset:Metal")
        self.chip("swatch:7")
        self.field("rough", "0.4")
        self.tab("object")

        for x, y in wheels:
            self.copy()
            self.fields3("loc", x, y, "0.38")

        print("Chapter 7: the lamps and the bumpers", flush=True)
        self.add("UV Sphere")
        self.field("radius", "0.12")
        self.chip("shade:smooth")
        self.tab("object")
        self.fields3("scale", "0.6", "1", "1")
        self.fields3("loc", "2.08", "0.6", "0.72")
        self.tab("material")
        self.chip("preset:Light")
        self.field("base", "fff3c4")
        self.tab("object")
        self.copy()
        self.fields3("loc", "2.08", "-0.6", "0.72")

        self.add("Cube")
        self.fields3("size", "0.04", "0.36", "0.1")
        self.tab("object")
        self.fields3("loc", "-2.11", "0.6", "0.78")
        self.tab("material")
        self.chip("preset:Light")
        self.field("base", "ff2a1a")
        self.field("emit", "3")
        self.tab("object")
        self.copy()
        self.fields3("loc", "-2.11", "-0.6", "0.78")

        self.add("Cube")
        self.fields3("size", "0.14", "1.84", "0.2")
        self.tab("object")
        self.fields3("loc", "2.16", "0", "0.44")
        self.tab("material")
        self.chip("preset:Plastic")
        self.chip("swatch:9")
        self.field("rough", "0.6")
        self.tab("object")
        self.copy()
        self.fields3("loc", "-2.16", "0", "0.44")

        m = self.mark()
        self.key("home")
        self.said("cafesa3d: at ", m, 10)
        self.screen()
        body = self.where("Cube") or (self.ox + 46 + self.vw // 2, self.oy + 46 + self.vh // 2)
        self.save("car-solid.png", self.centred(body[0], body[1], 760, 440))

        print("Chapter 7: F12", flush=True)
        m = self.mark()
        self.key("f12")
        at = self.said("cafesa3d: render window at ", m, 30)
        done = self.said("cafesa3d: rendered ", m, 3600)

        if done is None:
            self.problems.append("F12's render never finished")

        print("  " + str(done), flush=True)
        self.screen()
        rx, ry = (int(v) for v in at.split(","))
        self.save("car-render.png", (rx, ry + 46, rx + 640, ry + 46 + 360))
        self.save("render-window.png", (rx - 2, ry - 26, rx + 912, ry + 408))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.strip().splitlines()[-1])

    shots = Shots(sys.argv[1], sys.argv[2])
    started = time.monotonic()

    try:
        shots.run()
    finally:
        shots.guest.close()

    print("%.0f s" % (time.monotonic() - started))

    if shots.problems:
        print("FAIL: the tutorial could not be followed as written:")

        for p in shots.problems:
            print("  " + p)

        return 1

    print("PASS: every step of the car followed as the pages give it; the pictures are in "
          + sys.argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main())
