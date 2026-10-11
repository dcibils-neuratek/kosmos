#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Quick Look in Tracker (`docs/quicklook.html`, Q1): Space shows a file.

A desktop, `/Home/Looks` holding a red picture 64 by 48 made here and a text
file, Tracker opened on it, the picture chosen, and the keys:

  - Space: the panel, saying a picture, PNG, 64 x 48;
  - on the screen, the picture's red - about 64 by 48 of it, so a picture
    smaller than the panel is shown at its own size, never enlarged;
  - Down: the text file, shown as its facts, and Tracker's selection with it;
  - Up: the picture again;
  - Escape: closed, and the red gone from the screen;
  - Space and Return: closed, and the picture opened in Photo.

Usage: run_quicklook.py IMAGE
"""

import os
import random
import re
import struct
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                  # noqa: E402
import run_servers as S                                     # noqa: E402

CHECKS = 6
PIC_W, PIC_H = 64, 48
RED = (220, 30, 40)

MAKE = ('local files = use("/Kosmos/Libraries/files.lua")\n'
        'print("MADE", files.make_folder("/Home/Looks"))\n')

LAUNCH = ('local r = fs.send("/Running/wm", { type = "launch", program = "tracker",'
          ' args = "/Home/Looks" })\n'
          'print("LAUNCH " .. tostring(r and r.ok))\n')


def png(w, h, rgb):
    """A picture of one colour, as PNG: made here, nobody's."""
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))
    raw = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def reds(px, width, height):
    """How many pixels on the screen are the picture's red."""
    n = 0
    for i in range(0, width * height * 3, 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if abs(r - RED[0]) < 12 and abs(g - RED[1]) < 12 and abs(b - RED[2]) < 12:
            n += 1
    return n


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web)
    fails = []
    said = {}

    def line(pattern, mark, seconds=15):
        end = time.time() + seconds
        while time.time() < end:
            guest._read_available()
            m = re.search(pattern, guest.seen[mark:])
            if m:
                return m
            time.sleep(0.1)
        return None

    def check(ok, what):
        if not ok:
            fails.append(what)

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)
        session.put(MAKE.encode(), "/Temporary/make.lua")
        session.run("/Temporary/make.lua")
        time.sleep(1)
        session.put(png(PIC_W, PIC_H, RED), "/Home/Looks/a-red.png")
        session.put(b"A note, made up for the test.\n", "/Home/Looks/b-notes.txt")
        session.put(LAUNCH.encode(), "/Temporary/launch.lua")
        time.sleep(1)
        mark = len(guest.seen)
        session.run("/Temporary/launch.lua")
        rows = line(r"tracker: rows of /Home/Looks at (\d+),(\d+), (\d+) apart", mark, 30)
        if not rows:
            raise RuntimeError("Tracker never showed /Home/Looks")
        time.sleep(1.5)
        width, height, _ = R.parse_ppm(guest.screendump())

        # The picture chosen, by one click on its row.
        x, y = int(rows.group(1)), int(rows.group(2))
        guest.mouse_to(*R._to_tablet(x, y, width, height))
        time.sleep(0.1)
        guest.mouse_button(True)
        time.sleep(0.05)
        guest.mouse_button(False)
        time.sleep(1)
        # Away from the panel's middle, so the pointer is not counted.
        guest.mouse_to(*R._to_tablet(5, height - 5, width, height))

        # Space: the panel, with the picture.
        mark = len(guest.seen)
        guest.sendkey("spc")
        shown = line(r"tracker: quick look (\S+) - (\w+), ([^;]*);", mark)
        said["picture"] = shown and shown.group(0)
        check(shown and shown.group(1) == "a-red.png" and shown.group(2) == "picture"
              and "Picture" in shown.group(3) and "64 x 48" in shown.group(3),
              "Space did not show a-red.png as a picture, 64 x 48: %r" % said["picture"])
        # Waited for, not a number of seconds: the picture's red, and how long
        # it took to come after the panel said it.
        start, n = time.time(), 0
        while time.time() - start < 10:
            _, _, px = R.parse_ppm(guest.screendump())
            n = reds(px, width, height)
            if n > 0:
                break
            time.sleep(0.25)
        said["red"], said["red after"] = n, time.time() - start
        check(PIC_W * PIC_H * 0.8 <= n <= PIC_W * PIC_H * 1.2,
              "the picture was not on the screen at its own size: %d red pixels, %d expected"
              % (n, PIC_W * PIC_H))

        # Down: the text file, as its facts.
        mark = len(guest.seen)
        guest.sendkey("down")
        shown = line(r"tracker: quick look (\S+) - (\w+), ([^;]*);", mark)
        check(shown and shown.group(1) == "b-notes.txt" and shown.group(2) == "facts",
              "Down did not show b-notes.txt as its facts: %r" % (shown and shown.group(0)))

        # Up: the picture again.
        mark = len(guest.seen)
        guest.sendkey("up")
        shown = line(r"tracker: quick look (\S+) - (\w+)", mark)
        check(shown and shown.group(1) == "a-red.png" and shown.group(2) == "picture",
              "Up did not come back to the picture: %r" % (shown and shown.group(0)))

        # Escape: closed, and the red gone.
        mark = len(guest.seen)
        guest.sendkey("esc")
        closed = line(r"tracker: quick look closed", mark)
        time.sleep(1.5)
        _, _, px = R.parse_ppm(guest.screendump())
        check(closed and reds(px, width, height) < 50,
              "Escape did not close the panel and take the picture away")

        # Space, then Return: closed, and the picture opened in Photo.
        mark = len(guest.seen)
        guest.sendkey("spc")
        line(r"tracker: quick look a-red.png", mark)
        mark = len(guest.seen)
        guest.sendkey("ret")
        check(line(r"tracker: quick look closed", mark) and line(r"wm: window Photo", mark, 20),
              "Return did not close the panel and open the picture in Photo")
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on Quick Look:" % (len(fails), CHECKS))
        for f in fails:
            print("  " + f)
        for l in seen.splitlines():
            if l.startswith("tracker:") or "Photo" in l or "error" in l.lower():
                print("  | " + l.strip()[:180])
        return 1

    print("PASS: %d checks on Quick Look (Space showing a PNG 64 x 48 as a picture, %d red "
          "pixels on the screen %.1f s after - its own size, not enlarged; Down a text file as its facts; "
          "Up the picture again; Escape closing it; Return opening the picture in Photo)"
          % (CHECKS, said.get("red", 0), said.get("red after", 0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
