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

        mark = len(guest.seen)
        guest.type("wm gpumon")

        label = said("wm: the desktop drawn with: ", mark, 90)
        check(label in ("virtio - cursor", "none - software"),
              "the desktop's label did not say what QEMU draws with: %r" % label)

        opened = said("gpumon: ", mark, 90)
        check(opened == "no GPU: no Intel GPU this driver knows",
              "GPU Monitor did not say it has no GPU to watch: %r" % opened)

        # Drawn, and still running a few samples later: a window whose
        # first paint faulted would have ended by now.
        time.sleep(3)
        check("gpumon" in guest.seen[mark:] and "error" not in guest.seen[mark:].lower(),
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
