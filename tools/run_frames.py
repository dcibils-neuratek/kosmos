#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Where a window manager pass goes, under three different loads.

Kosmos is aiming at a desktop that stays responsive on a Pi 5, and the
recurring question - should the window manager be C? - has never had a
number attached to it. All five gated benchmarks measure the kernel: IPC,
context switch, page fault, allocation. None of them measures a frame.

So this measures frames. `wm` keeps per-stage counters, `/bin/frames.lua`
reads them, and this drives a desktop under three loads so the numbers can
be read against each other rather than in isolation:

  idle       a Deskbar and nothing else. What the loop costs to exist.
  animating  a plasma redrawing its whole window as fast as it can.
  dragging   a window pulled across the screen, which is the largest
             damage the compositor ever sees and the case a person would
             actually call slow.

**These are QEMU numbers**, so `CLAUDE.md` applies: they catch regressions
and they do not say whether something is fast. What survives the emulator
is the shape - which stage dominates, and whether the worst pass is a
garbage collection - and the shape is what the C question turns on.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from run_screenshot import Guest, Failure, PROMPT, GEOMETRY   # noqa: E402

# The tablet's range, which is what `mouse_to` speaks.
ABS = 32767


def wait_prompt(guest):
    guest.wait_for(PROMPT, "reached a shell")


def geometry(guest):
    line = None
    for text in guest.seen.splitlines():
        if GEOMETRY.search(text):
            line = GEOMETRY.search(text)
    if not line:
        raise Failure("the boot never announced a display geometry.")
    return int(line.group(1)), int(line.group(2))


def collect(guest, marker, seconds):
    """Wait for a report and hand back the lines of it."""
    deadline = time.monotonic() + seconds + 40
    start = len(guest.seen)

    while time.monotonic() < deadline:
        guest._read_available()
        if marker in guest.seen[start:]:
            time.sleep(0.5)
            guest._read_available()
            return guest.seen[start:]
        if guest.proc.poll() is not None:
            raise Failure("QEMU exited during the measurement.")

    raise Failure(
        "no report arrived.\n--- what it did say ---\n" + guest.seen[start:])


def show(title, text):
    print()
    print("=" * 68)
    print(f"  {title}")
    print("=" * 68)

    # Everything from the pass count to the heap line; the rest is the
    # shell echoing and the desktop narrating its own startup.
    keep = False
    for line in text.splitlines():
        if "passes," in line:
            keep = True
        if keep:
            print("   " + line.rstrip())
        if line.strip().startswith("heap ") or line.strip().startswith("->"):
            if "->" in line:
                keep = False

    print()


def scenario_idle(guest, seconds):
    guest.type(f"wm deskbar,frames:{seconds}")
    return collect(guest, "collections,", seconds)


def scenario_animating(guest, seconds):
    guest.type(f"wm deskbar,plasma,frames:{seconds}")
    return collect(guest, "collections,", seconds)


def scenario_dragging(guest, seconds, w, h):
    """A window dragged across the screen while the profile runs.

    The drag is what a person means by "the desktop feels slow": every step
    damages the rectangle the window left and the one it arrived in, so the
    compositor does its largest amount of work, repeatedly, with the Lua
    that decides *what* to redraw in front of it.
    """
    guest.type(f"wm deskbar,hello-win,frames:{seconds}")

    # Let the window exist before grabbing it.
    time.sleep(6)
    guest._read_available()

    # The title bar of a window `wm` opens at its default place. Ten pixels
    # down from its top edge is inside the tab and outside the close box.
    x, y = 150 + 120, 90 + 10

    guest.mouse_to(x * ABS // w, y * ABS // h)
    guest.mouse_button(True)

    steps = 40
    for i in range(steps):
        nx = x + int(320 * (i + 1) / steps)
        ny = y + int(180 * (i + 1) / steps)
        guest.mouse_to(nx * ABS // w, ny * ABS // h)
        time.sleep(0.06)

    guest.mouse_button(False)

    return collect(guest, "collections,", seconds)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--seconds", type=int, default=6)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--only", default=None,
                    help="idle, animating or dragging")
    args = ap.parse_args()

    guest = Guest(args.image, args.timeout)

    try:
        wait_prompt(guest)
        w, h = geometry(guest)
        print(f"display {w}x{h}")

        wanted = args.only
        runs = []

        if wanted in (None, "idle"):
            runs.append(("idle - a Deskbar and nothing else",
                         lambda: scenario_idle(guest, args.seconds)))
        if wanted in (None, "animating"):
            runs.append(("animating - a plasma redrawing continuously",
                         lambda: scenario_animating(guest, args.seconds)))
        if wanted in (None, "dragging"):
            runs.append(("dragging - a window pulled across the screen",
                         lambda: scenario_dragging(guest, args.seconds, w, h)))

        for title, run in runs:
            text = run()
            show(title, text)

            # Back to a prompt for the next one. Control-C is what `wm`
            # documents as giving the screen back.
            guest.proc.stdin.write(b"\x03")
            guest.proc.stdin.flush()
            time.sleep(2)
            guest._read_available()

    except Failure as why:
        print(f"\nFAIL: {why}", file=sys.stderr)
        return 1
    finally:
        guest.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
