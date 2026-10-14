#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Scheduler window, driven by the pointer.

`docs/scheduler.md` G1 and G2, as `docs/scheduler.html` draws it. The
window opened on a machine of four processors, and held to:

  * **its lanes**: a lane for each processor, read from what each core
    records (`sys.sched_lane`), and the switches a second counted;
  * **Fair chosen** on its card: round-robin in force on every core, as the
    kernel then says - refused on any machine of two cores until 0.11.214;
  * **a turn of 20 ms** chosen;
  * **game mode**: Super+G over the Scheduler gives it the game band while
    it is in front, its card's menu offers the applications, and Super+G
    again ends it;
  * **Balanced chosen again**, and the kernel back on priority.

Usage: run_scheduler.py <image>
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_screenshot as R                                   # noqa: E402


def main():
    image = sys.argv[1]
    # Four processors: the x86 harness boots one unless asked, and a policy
    # changed on one core is the case that always worked.
    R.extra_args(image, ["-smp", "4"])
    guest = R.Guest(image, 120)
    failed, checks, cores = [], 0, 0

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
            time.sleep(0.1)
            guest._read_available()
        return None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")

        at = len(guest.seen)
        guest.type("wm scheduler:--tell")
        opened = until(r"scheduler: window at (\d+),(\d+), (\d+) by (\d+), (\d+) cores, policy (\S+)", at, 120)

        if not opened:
            print("FAIL: the Scheduler window never opened\n" + guest.seen[-2000:])
            return 1

        ox, oy, _, _, cores = (int(v) for v in opened.groups()[:5])
        check(opened.group(6) == "priority", "the machine did not start on priority: %r" % opened.group(6))
        check(cores >= 2, "the machine has %d processor: a policy changed on one core proves nothing" % cores)

        spots_line = until(r"scheduler: spots ([^\n]*)\n", at, 30)
        spots = {}

        if spots_line:
            for m in re.finditer(r"([a-z]+ [\w-]+)@(\d+),(\d+)", spots_line.group(1)):
                spots[m.group(1)] = (int(m.group(2)), int(m.group(3)))

        check(all(k in spots for k in ("mode priority", "mode round-robin", "mode game", "turn 20")),
              "the window did not say where its cards and turns are: %r" % sorted(spots))

        lanes = until(r"scheduler: lanes (\d+), (\d+) switches a second", at, 30)
        check(lanes is not None and int(lanes.group(1)) == cores and int(lanes.group(2)) > 0,
              "the lanes were not read, or no switches were counted: %r" % (lanes and lanes.group(0)))

        width, height, _ = R.pixel_reader(guest.screendump())

        def click(key):
            x, y = spots[key]
            guest.mouse_to(*R._to_tablet(ox + x, oy + y, width, height))
            time.sleep(0.3)
            guest.mouse_button(True)
            time.sleep(0.12)
            guest.mouse_button(False)
            time.sleep(0.12)

        if "mode round-robin" in spots:
            at = len(guest.seen)
            click("mode round-robin")
            fair = until(r"scheduler: chose round-robin - ([^,\n]*), the kernel says (\S+)", at, 15)
            check(fair is not None and fair.group(1) == "in force" and fair.group(2) == "round-robin",
                  "Fair was not put in force on a machine of %d processors: %r" % (cores, fair and fair.group(0)))

        if "turn 20" in spots:
            at = len(guest.seen)
            click("turn 20")
            check(until(r"scheduler: a turn of 20 ms - in force", at, 15) is not None,
                  "a turn of 20 ms was not put in force")


        if "mode priority" in spots:
            at = len(guest.seen)
            click("mode priority")
            back = until(r"scheduler: chose priority - ([^,\n]*), the kernel says (\S+)", at, 15)
            check(back is not None and back.group(2) == "priority",
                  "Balanced did not put priority back: %r" % (back and back.group(0)))

        # **Game mode** (G3): Super+G over the window in front - the
        # Scheduler itself, since any application can have it - gives it,
        # and the band with it while it is in front; the card says who has
        # it; its menu offers the applications; Super+G again ends it.
        if "mode game" in spots:
            at = len(guest.seen)
            guest.sendkey("meta_l-g")
            on = until(r"wm: game mode on for ([^\n(]*) \(", at, 15)
            check(on is not None and on.group(1).strip() == "Scheduler",
                  "Super+G did not give the Scheduler game mode: %r" % (on and on.group(0)))
            check(until(r"wm: game band given for Scheduler\n", at, 15) is not None,
                  "the game band was not given to the window in front")

            at = len(guest.seen)
            click("mode game")
            menu = until(r"scheduler: game mode menu, (\d+) application", at, 15)
            check(menu is not None and int(menu.group(1)) >= 1,
                  "the Game mode card did not offer the applications: %r" % (menu and menu.group(0)))
            guest.sendkey("esc")
            time.sleep(0.3)

            at = len(guest.seen)
            guest.sendkey("meta_l-g")
            check(until(r"wm: game mode off for Scheduler", at, 15) is not None,
                  "Super+G again did not end game mode")

        with open(os.path.join("build", "gate", "x86-scheduler.ppm"), "wb") as f:
            f.write(guest.screendump())
    finally:
        transcript = guest.seen
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on the Scheduler window:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        print("--- the guest said ---\n" + transcript[-3000:])
        return 1

    print("PASS: %d checks on the Scheduler window, %d processors (its lanes read and switches "
          "counted; Fair put in force on every core and Balanced back; a turn of 20 ms; game mode "
          "given by Super+G with its band, offered by its card, and ended)" % (checks, cores))
    return 0


if __name__ == "__main__":
    sys.exit(main())
