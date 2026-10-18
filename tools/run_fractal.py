#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""GPU Fractal, the Compute Kit's example (`docs/compute.md` C4).

Opened on the desktop, under QEMU, which has no Intel GPU:

- **the GPU half says so** in the driver's words, and the processor draws;
- **the processor's frame** is said with its time, its view and its
  iterations, and **drawn**: inside the window's picture, the main
  cardioid's centre is black and a point left of the set is not, and the
  picture holds many colours, not a fill;
- **GPU**, pressed, says there is no GPU to switch to; **Kernel**, pressed,
  shows the kernel;
- **a drag** across the picture moves the view, and the processor draws it
  again at the new centre.

The GPU's picture, and the two held to each other, are the M700's to show
(`testing.md`).

Usage: run_fractal.py IMAGE
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                   # noqa: E402

FRAME = re.compile(r"fractal: processor ([\d.]+) ms at (-?[\d.]+),(-?[\d.]+) 1 : (\d+), (\d+) iterations, 800x600")
PLACE = r" at (\d+),(\d+) (\d+)x(\d+)"


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

    def where(name, since):
        m = said(re.compile(re.escape("fractal: " + name) + PLACE), since, 15)
        return tuple(int(v) for v in m.groups()) if m else None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")

        mark = len(guest.seen)
        guest.type("wm fractal")
        R.started(guest)

        none = said(re.compile(r"fractal: gpu - (.*); the processor draws"), mark, 60)
        check(none is not None and none.group(1) == "no Intel GPU this driver knows",
              "GPU Fractal did not say there is no Intel GPU here: %r" % (none and none.group(0)))

        first = said(FRAME, mark, 90)
        check(first is not None, "GPU Fractal never said the processor's frame:\n    "
              + guest.seen[mark:][-1200:])

        if first is None:
            return report(failed, checks)

        ms, cx, cy, scale, n = first.groups()
        check(float(ms) > 0 and cx == "-0.600000" and cy == "0.000000" and scale == "1" and n == "128",
              "the first frame was not the whole set at 128 iterations: %r" % first.group(0))

        pic = where("the picture", mark)
        seg = where("GPU | Processor", mark)
        kern = where("Kernel", mark)
        check(pic is not None and seg is not None and kern is not None,
              "GPU Fractal did not say where its controls are: %r" % ((pic, seg, kern),))

        if pic is None or seg is None or kern is None:
            return report(failed, checks)

        # The picture: the cardioid's centre, c = -0.2, is black; c = -2.1,
        # left of the set on the real axis, is not; and many colours.
        px_, py_, pw, ph = pic
        deadline = time.monotonic() + 15

        while True:
            width, height, px = R.parse_ppm(guest.screendump())

            def at(x, y):
                o = ((py_ + y) * width + px_ + x) * 3
                return tuple(px[o:o + 3])

            inside = at(400 + 100, 300)          # (-0.2 + 0.6) / 0.004 = 100 right of the centre
            outside = at(400 - 375, 300)         # (-2.1 + 0.6) / 0.004 = -375
            colours = {at(x, y) for x in range(0, pw, 7) for y in range(0, ph, 7)}

            if (inside == (0, 0, 0) and outside != (0, 0, 0) and len(colours) > 12) \
               or time.monotonic() > deadline:
                break

            time.sleep(1)

        check(inside == (0, 0, 0), "the set's inside was not black: %r" % (inside,))
        check(outside != (0, 0, 0), "a point outside the set was black")
        check(len(colours) > 12, "the picture holds %d colours, not a Mandelbrot's" % len(colours))

        def click(x, y):
            guest.mouse_to(*R._to_tablet(x, y, width, height))
            time.sleep(0.3)
            guest.mouse_button(True)
            time.sleep(0.15)
            guest.mouse_button(False)

        # GPU, pressed: nothing to switch to.
        at_ = len(guest.seen)
        x, y, w, h = seg
        click(x + w // 4, y + h // 2)
        no = said(re.compile(r"fractal: no GPU to switch to - (.*)"), at_, 15)
        check(no is not None, "GPU, pressed, did not say there is no GPU to switch to")

        # Kernel, pressed: the kernel shown.
        at_ = len(guest.seen)
        x, y, w, h = kern
        click(x + w // 2, y + h // 2)
        check(said(re.compile(r"fractal: the kernel shown"), at_, 15) is not None,
              "Kernel, pressed, did not show the kernel")

        # A drag a hundred pixels right: the view a hundred pixels left,
        # -0.6 - 100 * 0.004 = -1.0, and the processor draws it again.
        at_ = len(guest.seen)
        sx, sy = px_ + 300, py_ + 300
        guest.mouse_to(*R._to_tablet(sx, sy, width, height))
        time.sleep(0.3)
        guest.mouse_button(True)

        for step in range(1, 11):
            guest.mouse_to(*R._to_tablet(sx + step * 10, sy, width, height))
            time.sleep(0.05)

        time.sleep(0.2)
        guest.mouse_button(False)

        moved = said(FRAME, at_, 30)
        check(moved is not None and abs(float(moved.group(2)) + 1.0) < 0.02 and moved.group(3) == "0.000000",
              "a drag of a hundred pixels did not move the view to -1.0: %r" % (moved and moved.group(0)))

        check("Traceback" not in guest.seen[mark:] and "attempt to" not in guest.seen[mark:],
              "GPU Fractal raised an error:\n    " + guest.seen[mark:][-1500:])
    except R.Failure as e:
        failed.append("the steps stopped: %s" % e)
    finally:
        guest.close()

    return report(failed, checks)


def report(failed, checks):
    if failed:
        print("FAIL: %d of %d checks on GPU Fractal:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on GPU Fractal without a GPU (the GPU half says so in the driver's "
          "words; the processor's frame said and drawn - black inside, colours outside; GPU and "
          "Kernel pressed; a drag moves the view)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
