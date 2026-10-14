#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Linux in a Kosmos window: its screen (`docs/linux.md` L2).

The ARM machine at EL2 with Alpine installed as an image, and the Linux
window opened - `wm linux:--tell`. Alpine boots with a screen of 640 by 480
through virtio-gpu, the sandbox's monitor lays what it flushes into a region
(`vmmscreen.h`), and the window draws that region. Held to:

  * **a picture**: the window saying where its picture is once the first
    frame arrived;
  * **Linux's console in it**: QEMU's own screen, read at that place - the
    console's black, with light text on it (the line the window's shell
    prints, and its prompt), rather than one flat colour;
  * **the window's own part**: its header above the picture, which is not
    Linux's black.

The screen is kept in `build/gate/arm-sandbox-linuxwin.ppm`.

Usage: run_linuxwin.py <image>
"""

import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_screenshot as R                                   # noqa: E402
import scratch                                               # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    image = sys.argv[1]
    folder = scratch.path("linuxwin-home")
    disk = scratch.path("linuxwin-home.img")
    alpine = os.path.join(folder, "Sandboxes", "Images", "alpine")

    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(alpine)

    with open(os.path.join(alpine, "package.txt"), "w") as f:
        f.write("name        alpine\ntitle       Alpine Linux\nkind        sandbox\n")

    for step in ([sys.executable, os.path.join(HERE, "alpine.py"), alpine],
                 [sys.executable, os.path.join(HERE, "homeimage.py"), folder, disk, "128"]):
        made = subprocess.run(step, capture_output=True, text=True)

        if made.returncode != 0:
            print("FAIL: %s would not run:\n%s%s" % (os.path.basename(step[1]), made.stdout, made.stderr))
            return 1

    R.virtualize("1G")
    R.extra_args(image, ["-drive", "file=%s,format=raw,if=none,id=home" % disk,
                         "-device", R.device(image, "blk") + ",drive=home"])
    guest = R.Guest(image, 120)
    failed, checks = [], 0

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    def until(pattern, since, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            found = re.search(pattern, guest.seen[since:].replace("\r", ""))
            if found:
                return found
            time.sleep(0.3)
            guest._read_available()
        return None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")

        at = len(guest.seen)
        guest.type("wm linux:--tell 640x480 --memory 256")
        shown = until(r"linux: picture at (\d+),(\d+), (\d+)x(\d+); frames (\d+)", at, 240)
        check(shown is not None, "the Linux window never said its first frame had arrived: %r"
              % guest.seen[-1500:])

        if shown:
            px, py, pw, ph = (int(v) for v in shown.groups()[:4])

            # The shell's line and prompt are drawn by Linux's console a
            # moment after the screen's first frame: looked for until seen.
            light, dark, header = 0, 0, None
            deadline = time.monotonic() + 60

            while time.monotonic() < deadline:
                width, height, pixel = R.pixel_reader(guest.screendump())
                light = dark = 0

                for y in range(py, min(py + 64, py + ph, height), 2):
                    for x in range(px, min(px + pw, width), 2):
                        r, g, b = pixel(x, y)

                        if r + g + b > 300:
                            light += 1
                        elif r + g + b < 30:
                            dark += 1

                header = pixel(px + pw // 2, py - 10) if py >= 10 else None

                if light >= 40:
                    break

                time.sleep(2)

            check(light >= 40 and dark > light * 4,
                  "the window's picture was not Linux's console - light text on black - "
                  "at %d,%d: %d light and %d dark pixels in its first 64 rows"
                  % (px, py, light, dark))
            check(header is not None and sum(header) >= 120,
                  "the window's header above the picture was not drawn: %r" % (header,))

        os.makedirs(os.path.join("build", "gate"), exist_ok=True)

        with open(os.path.join("build", "gate", "arm-sandbox-linuxwin.ppm"), "wb") as f:
            f.write(guest.screendump())
    finally:
        transcript = guest.seen
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on the Linux window:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        print("--- the guest said ---\n" + transcript[-4000:])
        return 1

    print("PASS: %d checks on the Linux window (Alpine's screen through virtio-gpu, "
          "its console drawn where the window said its picture is, the header above it)"
          % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
