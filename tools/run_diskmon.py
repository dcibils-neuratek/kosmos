#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Disk Monitor: what each drive reads and writes, read off the window.

`docs/diskmonitor.md` step D3. The guest boots with a FAT stick
(`fatstick.py`, PHOTOS), a program named `reader` reads its 3000-byte file
again and again in the background, and Disk Monitor - open beside it - is
held to what it says:

  * **the stick in its list of drives**, chosen by a click on its row;
  * **a rate of reading** on it, and **`reader` the busiest program**, by
    its name - the drive server naming the program that asked (D2);
  * **its graph mirrored**: `io_in` blue above the centre line, none below;
  * **its menu**: "Bits a second" chosen, it says so.

Usage: run_diskmon.py <image>
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_screenshot as R                                   # noqa: E402
import fatstick                                              # noqa: E402
import scratch                                               # noqa: E402

IO_IN = ((0x4f, 0xb3, 0xff), (0x1f, 0x7f, 0xd0))


def main():
    image = sys.argv[1]

    if not fatstick.available():
        print("PASS: 0 checks on Disk Monitor - SKIP, mtools is not installed")
        return 0

    stick = scratch.path("diskmon-stick.img")
    fatstick.build(stick)
    R.extra_args(image, ["-device", "qemu-xhci,id=usb0",
                         "-drive", "file=%s,format=raw,if=none,id=stick" % stick,
                         "-device", "usb-storage,bus=usb0.0,drive=stick"])
    guest = R.Guest(image, 120)
    failed, checks = [], 0

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
            guest._read_available()
        return None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        # The stick's volume, before anything asks for it.
        deadline = time.monotonic() + 60
        while "watching for devices" not in guest.seen and time.monotonic() < deadline:
            time.sleep(0.3)
            guest._read_available()

        reader = ('sys.name("reader") for _ = 1, 4000 do '
                  'fs.read("/Drives/PHOTOS/A Long File Name.txt") sys.sleep(5) end')
        guest.type('fs.write("/Temporary/reader.lua", [==[%s]==])' % reader)
        time.sleep(1)
        guest.type("/Temporary/reader.lua &")
        time.sleep(1)

        start = len(guest.seen)
        guest.type("wm diskmon:--tell")
        opened = said("diskmon: window at ", start, 90)
        m = re.match(r"(\d+),(\d+); more at (\d+),(\d+); graph at (\d+),(\d+), (\d+)x(\d+); "
                     r"list at (\d+),(\d+), rows of (\d+)", opened or "")

        if m is None:
            print("FAIL: Disk Monitor never opened its window: %r\n%s" % (opened, guest.seen[-1500:]))
            return 1

        ox, oy, mx, my, gx, gy, gw, gh, lx, ly, row = (int(v) for v in m.groups())
        width, height, at = R.pixel_reader(guest.screendump())

        def click(x, y):
            guest.mouse_to(*R._to_tablet(x, y, width, height))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.2)
            guest.mouse_button(False)

        # The stick: the list's second row, /Home being the first.
        mark = len(guest.seen)
        click(ox + 150, oy + ly + row + row // 2)
        chose = said("diskmon: chose ", mark, 15)
        check(chose == "/Drives/PHOTOS", "a click on the second row did not choose the stick: %r" % chose)

        best = None
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            for line in re.findall(r"diskmon: /Drives/PHOTOS read (\d+) B/s, wrote (\d+) B/s; "
                                   r"busiest (\S+) (\d+) (\d+)", guest.seen[mark:]):
                # By reader's own rate: the first reading after the stick is
                # chosen has no earlier one of reader's to subtract from.
                if line[2] == "reader" and (best is None or int(line[3]) > int(best[1][3])):
                    best = (int(line[0]), line)
            if best and best[0] > 2000 and int(best[1][3]) > 2000:
                break
            time.sleep(0.5)

        check(best is not None and best[0] > 2000,
              "Disk Monitor never saw the stick read by reader: %r" % (best,))
        check(best is not None and int(best[1][3]) > 2000,
              "reader's row did not carry the rate: %r" % (best,))

        shot = guest.screendump()
        width, height, at = R.pixel_reader(shot)

        with open(os.path.join("build", "gate", "x86-diskmon.ppm"), "wb") as f:
            f.write(shot)

        x0, y0 = ox + gx, oy + gy
        centre = y0 + 2 + (gh - 4) // 2

        def blue(*box):
            return next((p for p in (R.find_colour(at, c, *box) for c in IO_IN) if p), None)

        check(blue(x0 + gw // 2, y0 + 4, x0 + gw - 4, centre - 1) is not None,
              "no io_in blue above the graph's centre line")
        check(blue(x0 + 4, centre + 2, x0 + gw - 4, y0 + gh - 4) is None,
              "io_in blue below the centre line, where writing belongs")

        mark = len(guest.seen)
        click(ox + mx, oy + my)
        menu = said("diskmon: more menu at ", mark, 10)
        mm = re.match(r"(\d+),(\d+), (\d+) wide, rows of (\d+)", menu or "")
        check(mm is not None, "the dots did not open the menu: %r" % menu)

        if mm:
            x, y, w, r = (int(v) for v in mm.groups())
            click(x + 30, y + 2 + 4 * r + r // 3 + r // 2)
            check(said("diskmon: bits", mark, 10) is not None, "choosing Bits a second was not heard")
    finally:
        transcript = guest.seen
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on Disk Monitor:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        print("--- the guest said ---\n" + "\n".join(
            l for l in transcript.splitlines() if "wm: button" not in l)[-3000:])
        return 1

    print("PASS: %d checks on Disk Monitor (the stick chosen in its list by a click, its "
          "reading on the window and reader the busiest program by its name; the "
          "mirrored graph blue above its line and not below; Bits a second from the dots)"
          % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
