#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Tracker's list view on the kit's columns (`docs/tracker-list.html`).

Diego, on the M700, 10 October 2026: "for big files the name overlaps the
size", "the columns are not resizable", "it needs more columns like created
at, modified at (Date time) fields". A folder holding a folder, a short
file and a film with the drawing's long name, Tracker opened on it, and:

  - the first file's dates: Modified today, and Created its Modified, since
    the disk keeps none for it yet; and its kind in Info's words, "Film";
  - the columns as they open: Name, Size, Kind, Modified, Created;
  - the long name cut at its own column's edge: the stretch of its row
    between the name's room and the size holds one colour;
  - Name's edge dragged 90 to the right, and kept;
  - a double-click on Size's edge fitting it to its widest entry;
  - a right click on the headings opening their menu;
  - a second Tracker opening with the widths the first kept;
  - a folder that keeps its own columns opened with them.

Usage: run_trackerlist.py IMAGE
"""

import os
import random
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                  # noqa: E402
import run_servers as S                                     # noqa: E402
import scratch                                              # noqa: E402

CHECKS = 9
LONG = "Rehearsal take 4, full band, room microphones and the click track muted.mp4"

MAKE = ('local files = use("/Kosmos/Libraries/files.lua")\n'
        'files.make_folder("/Home/Lists")\n'
        'files.make_folder("/Home/Lists/Taxes")\n'
        'fs.write("/Home/Lists/' + LONG + '", "x")\n'
        'fs.write("/Home/Lists/notes.txt", "hello")\n'
        'fs.setattr("/Home/Lists/Taxes", { columns = "name 200 size 70 kind 90" })\n'

        'local r = fs.send("/Running/wm", { type = "launch", program = "tracker", args = "/Home/Lists" })\n'
        'print("LAUNCH " .. tostring(r and r.ok))\n')

AGAIN = ('local r = fs.send("/Running/wm", { type = "launch", program = "tracker", args = "/Home/Lists" })\n'
         'print("AGAIN " .. tostring(r and r.ok))\n')


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)

    # `/Home` on a disk, so its files are dated - in memory they are not,
    # and Modified is a dash there, rightly.
    import subprocess

    folder, disk = scratch.path("trackerlist-home"), scratch.path("trackerlist-home.img")
    os.makedirs(folder, exist_ok=True)
    made = subprocess.run([sys.executable, os.path.join(HERE, "homeimage.py"), folder, disk, "64"],
                          capture_output=True, text=True)

    if made.returncode != 0:
        print("FAIL: homeimage.py would not make a /Home:\n" + made.stdout + made.stderr)
        return 1

    guest = S.boot(image, telnet, web, extra=["-drive", "file=%s,format=raw,if=none,id=home" % disk,
                                              "-device", "virtio-blk-pci,drive=home"])
    fails = []

    def check(ok, complaint):
        if not ok:
            fails.append(complaint)

    def line(pattern, mark, seconds=15):
        end = time.time() + seconds
        while time.time() < end:
            guest._read_available()
            m = re.search(pattern, guest.seen[mark:])
            if m:
                return m
            time.sleep(0.1)
        return None

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)
        session.put(MAKE.encode(), "/Temporary/make.lua")
        session.put(AGAIN.encode(), "/Temporary/again.lua")
        time.sleep(2)
        mark = len(guest.seen)
        session.run("/Temporary/make.lua")

        rows = line(r"tracker: rows of /Home/Lists at (\d+),(\d+), (\d+) apart", mark, 30)
        heads = line(r"tracker: headings at (\d+): ([^\n]*)", mark, 10)

        if not rows or not heads:
            raise RuntimeError("Tracker never showed /Home/Lists")

        # The dates, and the columns as they open.
        dates = line(r"tracker: dates of ([^:\n]*): modified ([^,\n]*, \d\d:\d\d), created ([^\r\n]*)", mark, 5)
        check(dates is not None and dates.group(2).startswith("Today, ")
              and dates.group(3) == dates.group(2) + " (its modified)",
              "the first file's dates were not today's, and Created its Modified: %r"
              % (dates and dates.group(0)))

        kind = line(r"tracker: kind of ([^:\n]*): ([^\r\n]*)", mark, 5)
        check(kind is not None and kind.group(1) == LONG and kind.group(2) == "Film",
              "Kind did not say what the film is, in Info's words: %r" % (kind and kind.group(0)))

        opened = line(r"tracker: columns for /Home/Lists: ([^\n]*)", mark, 5)
        check(opened is not None
              and opened.group(1).strip() == "name 330 size 80 kind 110 modified 150 created 150",
              "the columns did not open as drawn: %r" % (opened and opened.group(1)))

        time.sleep(1.5)
        shot = guest.screendump()
        width, height, at = R.pixel_reader(shot)
        x0, y0, apart = (int(v) for v in rows.groups())
        head_y = int(heads.group(1))
        edges = dict((k, int(v)) for k, v in re.findall(r"(\w+)@(\d+)", heads.group(2)))

        # The long name's row - the second, after the folder - holds one
        # colour between the end of the name's room and the size.
        long_y = y0 + apart
        colours = set()

        for y in range(long_y - 5, long_y + 6):
            for x in range(edges["name"] - 3, edges["size"] - 40):
                colours.add(at(x, y))

        check(len(colours) == 1,
              "the long name ran past its column: %d colours between Name's edge and the size"
              % len(colours))

        def press(x, y, button="left"):
            guest.mouse_to(*R._to_tablet(x, y, width, height))
            time.sleep(0.05)
            guest.mouse_button(True, button)
            time.sleep(0.05)
            guest.mouse_button(False, button)

        # Name's edge dragged 90 to the right.
        mark = len(guest.seen)
        guest.mouse_to(*R._to_tablet(edges["name"], head_y, width, height))
        time.sleep(0.1)
        guest.mouse_button(True)
        time.sleep(0.1)

        for step in range(1, 10):
            guest.mouse_to(*R._to_tablet(edges["name"] + 10 * step, head_y, width, height))
            time.sleep(0.05)

        time.sleep(0.2)
        guest.mouse_button(False)
        dragged = line(r"tracker: column name now (\d+)", mark, 5)
        kept = line(r"tracker: columns name (\d+) ", mark, 5)
        check(dragged is not None and 418 <= int(dragged.group(1)) <= 422
              and kept is not None and kept.group(1) == dragged.group(1),
              "Name dragged 90 wider was not 420 and kept: %r, %r"
              % (dragged and dragged.group(0), kept and kept.group(0)))

        # A double-click on Size's edge, which has moved with Name's.
        size_edge = edges["size"] + (int(dragged.group(1)) - 330 if dragged else 0)
        time.sleep(1.0)
        mark = len(guest.seen)
        press(size_edge, head_y)
        time.sleep(0.12)
        press(size_edge, head_y)
        fitted = line(r"tracker: column size fitted to (\d+)", mark, 5)
        check(fitted is not None and 60 <= int(fitted.group(1)) < 80,
              "a double-click on Size's edge did not fit it to its entries: %r"
              % (fitted and fitted.group(0)))

        # The headings' menu, by a right click on them.
        time.sleep(1.0)
        mark = len(guest.seen)
        press(edges["name"] - 120, head_y, "right")
        check(line(r"tracker: the headings' menu", mark, 5) is not None,
              "a right click on the headings did not open their menu")
        guest.sendkey("esc")
        time.sleep(0.5)

        # A second Tracker on the same folder: the widths the first kept.
        mark = len(guest.seen)
        session.run("/Temporary/again.lua")
        again = line(r"tracker: columns for /Home/Lists: ([^\n]*)", mark, 30)
        check(again is not None and again.group(1).startswith("name %s size %s "
                                                              % (dragged and dragged.group(1),
                                                                 fitted and fitted.group(1))),
              "a second Tracker did not open with the widths kept: %r" % (again and again.group(1)))

        # Into Taxes, which keeps its own.
        rows2 = line(r"tracker: rows of /Home/Lists at (\d+),(\d+), (\d+) apart", mark, 15)
        time.sleep(1.5)

        if rows2:
            x2, y2 = int(rows2.group(1)), int(rows2.group(2))
            press(x2, y2)                   # the new window, focused
            time.sleep(1.2)
            mark = len(guest.seen)
            press(x2, y2)
            time.sleep(0.12)
            press(x2, y2)

        own = line(r"tracker: columns for /Home/Lists/Taxes \(its own\): ([^\n]*)", mark, 15)
        check(own is not None and own.group(1).startswith("name 200 size 70 kind 90"),
              "a folder that keeps its own columns did not open with them: %r" % (own and own.group(1)))
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        try:
            with open(os.path.join("build", "gate", "x86-trackerlist.ppm"), "wb") as f:
                f.write(guest.screendump())
        except Exception:                   # noqa: BLE001 - a picture is extra
            pass
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on Tracker's list view:" % (len(fails), CHECKS))
        for f in fails:
            print("  " + f)
        for l in seen.splitlines():
            if l.startswith("tracker: ") or "error" in l.lower():
                print("  | " + l.strip()[:200])
        return 1

    print("PASS: %d checks on Tracker's list view (the dates; the kind in words; the columns as drawn; a long name "
          "cut at its own edge; Name dragged wider and kept; Size fitted by a double-click; the "
          "headings' menu; the widths read back by a second Tracker; a folder's own columns)" % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
