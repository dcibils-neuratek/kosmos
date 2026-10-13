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
    tab chosen, and what is typed there answered by Alpine's shell - two
    backspaces stepping the shown line back, with nothing drawn for them;
  * **Restart**: the new boot shown from its first line;
  * **its settings**: the Settings tab's form, 128 MB chosen and saved, and
    the definition on the disk saying so;
  * **how sandboxes work**: the help, from its row under the machine;
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

                # Two backspaces: the shell's `\b \b` echo steps back over
                # what was typed, and nothing is drawn for it (13 October).
                at = len(guest.seen)
                for key in ("e", "c", "h", "o", "spc", "a", "b", "c", "d"):
                    guest.sendkey(key)

                time.sleep(0.5)
                guest.sendkey("backspace")
                guest.sendkey("backspace")
                time.sleep(1)
                time.sleep(1)
                guest._read_available()
                lines = [l.rstrip("\r") for l in
                         re.findall(r"sandboxes: shown shell1: ([^\n]*)\n", guest.seen[at:])]
                check(lines != [] and lines[-1].endswith("# echo ab"),
                      "two backspaces did not step the shown line back to 'echo ab': %r" % lines[-3:])
                guest.sendkey("ret")
                check(until(r"sandboxes: console shell1: ab\n", at, 15) is not None,
                      "the shell did not run 'echo ab'")

            # **Restart, and the new boot seen from its start** (Diego, 13
            # October: "does restarting a sandbox work? i dont see linux
            # restarting at all"): the window kept reading from where the old
            # console was, so the new one showed nothing until it had said as
            # much. The first line shown after Restart is the first line the
            # first boot showed.
            first_line = re.search(r"sandboxes: console shell1: ([^\n]*)\n", guest.seen)

            if "Restart" in spots and first_line:
                at = len(guest.seen)
                click(ox + spots["Restart"][0], oy + spots["Restart"][1])
                # The old monitor's last word first ("vmm: stopped, as asked").
                again = until(r"sandboxes: console shell1: (?!vmm: )([^\n]*)\n", at, 120)
                check(again is not None and again.group(1).rstrip("\r") == first_line.group(1).rstrip("\r"),
                      "after Restart the console did not show the new boot from its start: %r, not %r"
                      % (again and again.group(1), first_line.group(1)))
                check(until(r"sandboxes: states [^\n]*shell1=running", at, 120) is not None,
                      "shell1 was not running again after Restart")

            # **Its settings, editable** (Diego, 13 October: "it should be
            # all editable"): the Settings tab's form, 128 MB chosen, Save -
            # and the definition on the disk says 128.
            if "Settings" in spots:
                at = len(guest.seen)
                click(ox + spots["Settings"][0], oy + spots["Settings"][1])
                form = until(r"sandboxes: settings of shell1, memory at (\d+),(\d+), save at (\d+),(\d+)", at, 15)
                check(form is not None, "the Settings tab did not show shell1's form")

                if form:
                    mx, my, sx, sy = (int(v) for v in form.groups())
                    click(ox + mx, oy + my)
                    time.sleep(0.5)
                    at = len(guest.seen)
                    click(ox + sx, oy + sy)
                    saved = until(r"sandboxes: shell1 settings saved, memory (\d+)", at, 15)
                    check(saved is not None and saved.group(1) == "128",
                          "Save in shell1's settings did not keep 128 MB: %r" % (saved and saved.group(0)))
                    kept = until(r"sandboxes: shell1 kept, memory (\d+)", at, 15)
                    check(kept is not None and kept.group(1) == "128",
                          "shell1's definition, read back from the disk, did not say 128 MB: %r"
                          % (kept and kept.group(0)))

            # Back to every sandbox: a group in the sidebar, as the drawing has it.
            click(ox + 60, oy + 100)
            time.sleep(1)

        # **How sandboxes work** (13 October): the row under the machine
        # opens the help in place of the cards, every card of it drawn.
        row = until(r"sandboxes: help row at (\d+),(\d+)", 0, 10)
        check(row is not None, "the sidebar did not say where its help row is")

        if row:
            at = len(guest.seen)
            click(ox + int(row.group(1)), oy + int(row.group(2)))
            shown = until(r"sandboxes: help shown, (\d+) cards, (\d+) tall", at, 10)
            check(shown is not None and shown.group(1) == "5",
                  "the help did not show its five cards: %r" % (shown and shown.group(0)))
            click(ox + 60, oy + 100)                    # All: the cards again
            time.sleep(1)

        at = len(guest.seen)
        click(ox + nx, oy + ny)
        sheet = until(r"sandboxes: new sandbox sheet, name at (\d+),(\d+), create at (\d+),(\d+), "
                      r"memory at (\d+),(\d+), ports at (\d+),(\d+), port field at (\d+),(\d+)", at, 15)
        check(sheet is not None, "+ New sandbox did not open the sheet")

        if sheet:
            fx, fy, bx, by, mx, my, wx, wy, px, py = (int(v) for v in sheet.groups())
            click(ox + mx, oy + my)                     # 128 MB, the first
            time.sleep(0.3)
            click(ox + wx, oy + wy)                     # the network: Ports
            time.sleep(0.3)
            click(ox + px, oy + py)
            time.sleep(0.3)
            guest.type("8082:80")
            time.sleep(0.5)
            click(ox + fx, oy + fy)
            time.sleep(0.3)
            guest.type("made1")
            time.sleep(0.5)
            at = len(guest.seen)
            click(ox + bx, oy + by)
            check(until(r"sandboxes: made made1", at, 15) is not None, "Create and start did not make made1")
            check(until(r"sandboxes: states [^\n]*made1=running", at, 240) is not None,
                  "made1 never arrived running")

            # What the sheet chose is what was written: its network included.
            written = until(r"sandboxes: made1 written, network (\S+), ports ([^\n]*)", 0, 15)
            check(written is not None and written.group(1) == "outgoing" and written.group(2).strip() == "8082:80",
                  "made1 was not written with its network outgoing and its port 8082:80: %r"
                  % (written.group(0) if written else guest.seen[-300:]))

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
          "click and its Console tab typed into and answered; New sandbox made and started, its network and a port chosen and written)"
          % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
