#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""GPU Monitor, `gpustat` and the desktop's label, where there is no Intel
GPU (`docs/gpumonitor.md` G0 to G2).

QEMU has none, so this holds what every machine without one must do:

- **`gpustat`** says the GPU's driver knows no GPU here, in the driver's
  own words through `gpu.lua`, and does not fault or hang;
- **the desktop's label**: the window manager says what draws the desktop
  - `virtio - cursor` where QEMU's display has a cursor of its own, `none -
  software` where it has not - and nothing that names an Intel GPU;
- **GPU Monitor** opens, says it has no GPU to watch, and draws its window.

What it shows on a machine that has one - the clock, RC6, flips, the
buffers and their holders - is the M700's to show (`testing.md` 18.664).

Usage: run_gpumon.py IMAGE
"""

import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                   # noqa: E402


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
        guest.type("gpustat")
        got = said("gpustat: ", mark, 30)
        check(got == "no Intel GPU this driver knows",
              "gpustat did not say there is no Intel GPU here: %r" % got)

        deadline = time.monotonic() + 30
        while guest.seen.find("kosmos> ", mark + 1) < 0 and time.monotonic() < deadline:
            time.sleep(0.1)

        # The blitter's test (`m700-2d.md` step 5): with no GPU, said - and
        # nothing touched.
        mark = len(guest.seen)
        guest.type("gpu engine")
        got = said("gpu: ", mark, 30)
        check(got == "no Intel GPU this driver knows",
              "gpu engine did not say there is no Intel GPU here: %r" % got)

        deadline = time.monotonic() + 30
        while guest.seen.find("kosmos> ", mark + 1) < 0 and time.monotonic() < deadline:
            time.sleep(0.1)

        # And the first program on the execution units (7b), the same.
        mark = len(guest.seen)
        guest.type("gpu compute")
        got = said("gpu: ", mark, 30)
        check(got == "no Intel GPU this driver knows",
              "gpu compute did not say there is no Intel GPU here: %r" % got)

        deadline = time.monotonic() + 30
        while guest.seen.find("kosmos> ", mark + 1) < 0 and time.monotonic() < deadline:
            time.sleep(0.1)

        # And the render engine's (7a), the same answer.
        mark = len(guest.seen)
        guest.type("gpu engine render")
        got = said("gpu: ", mark, 30)
        check(got == "no Intel GPU this driver knows",
              "gpu engine render did not say there is no Intel GPU here: %r" % got)

        deadline = time.monotonic() + 30
        while guest.seen.find("kosmos> ", mark + 1) < 0 and time.monotonic() < deadline:
            time.sleep(0.1)

        mark = len(guest.seen)
        # GPU Monitor, and the rendering modes switched while the desktop
        # runs (`docs/rendering.md` R1): Framebuffer given as asked, then
        # Blitter copy given as the most QEMU has - Cursor plane or less.
        guest.type("wm gpumon,rendering:0 3,preferences:displays")

        label = said("wm: the desktop drawn with: ", mark, 90)
        check(label in ("virtio - cursor plane", "none - framebuffer"),
              "the desktop's label did not say what QEMU draws with: %r" % label)

        first = said("rendering: ", mark, 90)
        check(first == "0, Framebuffer", "rendering 0 did not answer Framebuffer: %r" % first)
        at = guest.seen.find("rendering: 0, Framebuffer", mark)
        second = said("rendering: ", at + 1 if at >= 0 else mark, 30)
        check(second is not None and second.startswith(("1, Cursor plane - the most", "0, Framebuffer - the most")),
              "rendering 3 did not answer the most QEMU gives: %r" % second)

        # What was asked reached the window server, each in turn - under
        # QEMU's plain display every mode is given as Framebuffer, so the
        # answers alone cannot tell a mode heard from one ignored.
        told0 = said("windowserver: rendering 0", mark, 30)
        at0 = guest.seen.find("windowserver: rendering 0", mark)
        told3 = said("windowserver: rendering 3", at0 + 1 if at0 >= 0 else mark, 30)
        check(told0 is not None and told3 is not None,
              "the window server was not told 0 and then 3: %r, %r" % (told0, told3))

        # Preferences' Displays page lists the six modes (R2).
        listed = said("preferences: the rendering list, ", mark, 90)
        check(listed is not None and listed.startswith("6 modes"),
              "Preferences' Displays page did not list the six modes: %r" % listed)

        opened = said("gpumon: ", mark, 90)
        check(opened == "no GPU: no Intel GPU this driver knows",
              "GPU Monitor did not say it has no GPU to watch: %r" % opened)

        # Drawn, and still running a few samples later: a window whose
        # first paint faulted would have ended by now.
        time.sleep(3)
        check("gpumon" in guest.seen[mark:] and "gpumon: error" not in guest.seen[mark:].lower()
              and "process died" not in guest.seen[mark:],
              "GPU Monitor's window said an error: %r" % guest.seen[mark:][-600:])
    finally:
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on GPU Monitor without a GPU:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on GPU Monitor without a GPU (gpustat and gpu engine say "
          "none, in the driver's words; the desktop's label says what QEMU draws with; the window "
          "opens and says so)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
