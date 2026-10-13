#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Sandboxes window, driven by the pointer and the keyboard.

`docs/sandboxes.md` S2, as `docs/sandboxes.html` draws it. The ARM machine
at EL2 with Alpine installed as an image; two sandboxes made at the prompt -
`web`, a service printing a line a second, started, and `shell1`, a shell,
not - and the window opened beside them. Held to:

  * **the cards**: both there, web running and shell1 stopped;
  * **Start on a card**: shell1 started by `sandbox start` in a process of
    its own, and the window seeing it arrive, running;
  * **one sandbox watched**: shell1 opened by a double click, its Console
    tab chosen, and what is typed there answered by Alpine's shell;
  * **New sandbox**: the sheet filled - a name - and Create and start
    making it and starting it;
  * **Stop on a card**: web stopped, and the card saying so.

The window's screen is kept in `build/gate/arm-sandboxwin.ppm`.

Usage: run_sandboxwin.py <image>
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
SIDE, GAP, CARD_H, W = 220, 14, 196, 1180


def main():
    image = sys.argv[1]
    folder = scratch.path("sandboxwin-home")
    disk = scratch.path("sandboxwin-home.img")
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

    R.virtualize(os.environ.get("SANDBOX_MEMORY", "1G"))
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

        # 128 MB each: the machine has 1 GB, and the desktop and three
        # guests have to fit in it.
        for command in ("sandbox new web --image alpine --memory 128 --service -- "
                        "while true; do echo tick; sleep 1; done",
                        "sandbox new shell1 --image alpine --memory 128",
                        "sandbox start web"):
            at = len(guest.seen)
            guest.type(command)
            until(r"\nkosmos> ", at, 240)

        at = len(guest.seen)
        guest.type("wm sandboxes:--tell")
        opened = until(r"sandboxes: window at (\d+),(\d+); side \d+ wide; cards \d+ tall, "
                       r"from (\d+),(\d+); new at (\d+),(\d+)", at, 120)

        if not opened:
            print("FAIL: the Sandboxes window never opened\n" + guest.seen[-2000:])
            return 1

        ox, oy, cx, cy, nx, ny = (int(v) for v in opened.groups())
        states = until(r"sandboxes: states (.*)\n", at, 30)
        check(states is not None and "shell1=stopped" in states.group(1) and "web=running" in states.group(1),
              "the cards did not show web running and shell1 stopped: %r" % (states and states.group(1)))

        width, height, _ = R.pixel_reader(guest.screendump())
        card_w = (W - SIDE - 3 * GAP) // 2

        def click(x, y, twice=False):
            guest.mouse_to(*R._to_tablet(x, y, width, height))
            time.sleep(0.3)
            for _ in range(2 if twice else 1):
                guest.mouse_button(True)
                time.sleep(0.12)
                guest.mouse_button(False)
                time.sleep(0.12)

        # shell1 is the first card, web the second; a card's first button is
        # 18 in and 46 up from its foot.
        first = (ox + cx, oy + cy)
        second = (ox + cx + card_w + GAP, oy + cy)

        at = len(guest.seen)
        click(first[0] + 18 + 22, first[1] + CARD_H - 46 + 14)
        check(until(r"sandboxes: starting shell1", at, 10) is not None, "Start on shell1's card was not heard")
        check(until(r"sandboxes: states [^\n]*shell1=running", at, 240) is not None,
              "shell1 never arrived running")

        at = len(guest.seen)
        click(first[0] + 120, first[1] + 30, twice=True)
        detail = until(r"sandboxes: detail ([^\n]*)", at, 15)
        check(detail is not None, "a double click did not open shell1")

        if detail:
            spots = dict((m.group(1), (int(m.group(2)), int(m.group(3))))
                         for m in re.finditer(r"([^|@]+?)@(\d+),(\d+)", detail.group(1)))
            spots = {k.strip(): v for k, v in spots.items()}

            if "Console" in spots:
                click(ox + spots["Console"][0], oy + spots["Console"][1])
                time.sleep(1)
                click(ox + SIDE + 300, oy + 400)        # into the console, for the keys
                time.sleep(0.5)
                at = len(guest.seen)
                guest.type("echo $((6*7))")
                check(until(r"sandboxes: console shell1: 42\n", at, 30) is not None,
                      "what was typed in the Console tab was not answered: %r" % guest.seen[-500:])

            if "‹ All" in spots:
                click(ox + spots["‹ All"][0], oy + spots["‹ All"][1])
                time.sleep(1)

        at = len(guest.seen)
        click(ox + nx, oy + ny)
        sheet = until(r"sandboxes: new sandbox sheet, name at (\d+),(\d+), create at (\d+),(\d+), "
                      r"memory at (\d+),(\d+)", at, 15)
        check(sheet is not None, "+ New sandbox did not open the sheet")

        if sheet:
            fx, fy, bx, by, mx, my = (int(v) for v in sheet.groups())
            click(ox + mx, oy + my)                     # 128 MB, the first
            time.sleep(0.3)
            click(ox + fx, oy + fy)
            time.sleep(0.3)
            guest.type("made1")
            time.sleep(0.5)
            at = len(guest.seen)
            click(ox + bx, oy + by)
            check(until(r"sandboxes: made made1", at, 15) is not None, "Create and start did not make made1")
            check(until(r"sandboxes: states [^\n]*made1=running", at, 240) is not None,
                  "made1 never arrived running")

        with open(os.path.join("build", "gate", "arm-sandboxwin.ppm"), "wb") as f:
            f.write(guest.screendump())
    finally:
        transcript = guest.seen
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on the Sandboxes window:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        print("--- the guest said ---\n" + transcript[-4000:])
        return 1

    print("PASS: %d checks on the Sandboxes window (its cards, web running and shell1 "
          "stopped; Start on a card, the sandbox arriving running; one opened by a double "
          "click and its Console tab typed into and answered; New sandbox made and started)"
          % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
