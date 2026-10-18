#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""This Machine, the window (`docs/machine.md` M3).

Opened on the desktop, under QEMU:

- **every device as a card, grouped**: the window says what it shows -
  so many devices, so many driven, so many with no driver, and each group
  with its count - and the counts add up, the processor first, the
  machine's bridges in Other with no driver;
- **drawn**: the window's picture holds the green of a "Driven by" pill
  and the grey of a "Detected, no driver" one;
- **By connection**, pressed: the same devices as a tree;
- **No driver**, pressed: only those, every group's count adding up to the
  window's own "with no driver";
- **the Deskbar's USB indicator** says how many devices are on the ports -
  none here, QEMU's harness having no xHCI - and **`machine usb`**, what a
  click on it opens, shows the USB devices alone: none of the machine's
  others (`roadmap.md` 5q).

The M700's cards are its own to show (`testing.md`).

Usage: run_machine.py IMAGE
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                   # noqa: E402

LINE = re.compile(r"machine: (by kind|by connection|the report), (all|driven|no driver|usb) - "
                  r"(\d+) devices, (\d+) driven, (\d+) with no driver; (.*)")
DRIVEN = (0x4f, 0xb8, 0x78)      # machine.lua's DRIVEN_C
NONE = (0x8b, 0x93, 0xa3)        # and NONE_C


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

    def said(pattern, since, seconds=30):
        """The first match of `pattern` on a whole line said after `since`."""
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            guest._read_available()

            for line in guest.seen[since:].splitlines(True):
                if line.endswith("\n"):
                    m = pattern.search(line)
                    if m:
                        return m

            time.sleep(0.1)

        return None

    def click(x, y, width, height):
        guest.mouse_to(*R._to_tablet(x, y, width, height))
        time.sleep(0.3)
        guest.mouse_button(True)
        time.sleep(0.15)
        guest.mouse_button(False)

    try:
        guest.wait_for("kosmos> ", "reached a prompt")

        mark = len(guest.seen)
        guest.type("wm deskbar,machine")
        R.started(guest)

        first = said(LINE, mark, 60)
        check(first is not None, "This Machine never said what it shows:\n    "
              + guest.seen[mark:][-1200:])

        if first is None:
            return report(failed, checks)

        view, filt, total, driven, none, groups = first.groups()
        total, driven, none = int(total), int(driven), int(none)
        parts = [p.rsplit(" ", 1) for p in groups.split(", ") if p]
        names = [p[0] for p in parts]

        check(view == "by kind" and filt == "all",
              "This Machine did not open by kind, all: %r" % first.group(0))
        check(total == driven + none and total >= 6,
              "This Machine's counts do not add up: %r" % first.group(0))
        check(sum(int(p[1]) for p in parts) == total,
              "This Machine's groups do not hold every device: %r" % first.group(0))
        check(names[:2] == ["Processor", "Memory"] and "Other" in names and none >= 1,
              "This Machine's groups were not the processor first and Other with "
              "the bridges: %r" % names)

        # The window's picture: green pills and grey ones.
        placed = said(re.compile(r"wm: window This Machine at (\d+),(\d+) (\d+)x(\d+)"), mark, 10)
        width, height, px = R.parse_ppm(guest.screendump())
        wx, wy, ww, wh = (int(v) for v in placed.groups()) if placed else (0, 0, width, height)

        def count(colour):
            want, n = bytes(colour), 0
            for yy in range(wy, min(height, wy + wh)):
                row = px[(yy * width + wx) * 3:(yy * width + min(width, wx + ww)) * 3]
                for o in range(0, len(row), 3):
                    if row[o:o + 3] == want:
                        n += 1
            return n

        # Looked for until they are there: the line above is said as the
        # window draws, a moment before its picture reaches the screen.
        deadline = time.monotonic() + 15
        green = grey = 0

        while True:
            green, grey = count(DRIVEN), count(NONE)

            if (green > 40 and grey > 40) or time.monotonic() > deadline:
                break

            time.sleep(1)
            width, height, px = R.parse_ppm(guest.screendump())

        check(green > 40, "This Machine drew no green \"Driven by\" pill")
        check(grey > 40, "This Machine drew no grey \"no driver\" pill")

        # The two choices, where the window said they are.
        def where(items):
            m = said(re.compile(re.escape("machine: " + items) + r" at (\d+),(\d+) (\d+)x(\d+)"),
                     mark, 15)
            return tuple(int(v) for v in m.groups()) if m else None

        kind_seg = where("By kind | By connection")
        filter_seg = where("All | Driven | No driver")
        segs = (kind_seg, filter_seg)
        check(kind_seg is not None and filter_seg is not None,
              "This Machine did not say where its choices are: %r" % (segs,))

        if kind_seg and filter_seg:
            at = len(guest.seen)
            x, y, w, h = kind_seg
            click(x + w * 3 // 4, y + h // 2, width, height)

            tree = said(LINE, at, 15)
            check(tree is not None and tree.group(1) == "by connection"
                  and int(tree.group(3)) == total,
                  "By connection, pressed, did not show the same devices as a tree: %r"
                  % (tree and tree.group(0)))

            at = len(guest.seen)
            x, y, w, h = filter_seg
            click(x + w * 5 // 6, y + h // 2, width, height)

            only = said(LINE, at, 15)
            shown = [p.rsplit(" ", 1) for p in (only.group(6) if only else "").split(", ") if p]
            check(only is not None and only.group(2) == "no driver"
                  and sum(int(p[1]) for p in shown) == none,
                  "No driver, pressed, did not show only the %d with no driver: %r"
                  % (none, only and only.group(0)))

        bar = said(re.compile(r"deskbar: (\d+) on the USB ports, (\d+) driven"), 0, 20)
        check(bar is not None and bar.group(1) == "0",
              "the Deskbar did not say the USB ports hold nothing: %r" % (bar and bar.group(0)))

        time.sleep(1)
        check("machine.lua:" not in guest.seen[mark:] and "About This Machine stopped" not in guest.seen[mark:],
              "This Machine said an error: %r" % guest.seen[mark:][-800:])
    finally:
        guest.close()

    # What the indicator opens: the USB devices alone.
    guest = R.Guest(image, 120)

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        guest.type("wm machine:usb")
        R.started(guest, seconds=60)

        usb = said(LINE, mark, 60)
        check(usb is not None and usb.group(2) == "usb" and usb.group(6).strip() == ""
              and int(usb.group(3)) >= 6,
              "machine usb did not show the USB devices alone - none, here: %r"
              % (usb and usb.group(0)))
    finally:
        guest.close()

    return report(failed, checks)


def report(failed, checks):
    if failed:
        print("FAIL: %d of %d checks on This Machine:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on This Machine (every device a card in its group, the "
          "counts adding up; green and grey pills drawn; By connection and No driver "
          "pressed and shown)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
