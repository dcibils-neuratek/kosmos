#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Bluetooth host stack, end to end (`bluetooth.md` B2).

QEMU has no Bluetooth adapter, so every boot of the harness has `btsim`, a
controller in software, under `opt/kosmos/bluetooth=simulated`. The USB
driver puts it where a USB adapter would be, so what this holds is the real
path with only the radio pretended:

- **the ring**: the stack made it and handed it to the USB driver, which
  says nothing of it - a machine with no controller hears nothing from that
  driver, `x86-core` holds - so the stack's line is the evidence;
- **the setup**: the stack sent Reset, Read Local Version, Read BD_ADDR,
  Read Local Supported Features and Read Buffer Size through the ring, was
  pulsed for each answer, and named the adapter in one line from what came
  back - Bluetooth 5.1, the testing company, `btsim`'s address, Classic and
  Low Energy with Secure Simple Pairing, eight ACL buffers of 1021 bytes;
- **nothing refused or lost on the way**: no wait the kernel would not
  take (the driver's seventh endpoint), and no command unanswered;
- **what is nearby** (B4), through `bt` and `bluetooth.lua`: `bt scan`
  finds `btsim`'s three made-up neighbours - the K380 by Classic inquiry,
  named in its EIR; the mouse by Low Energy scan, a random address and the
  mouse appearance; the headphones by inquiry, named only once a Remote
  Name Request is answered - each with its kind, address and RSSI; `bt`
  then lists them nearest first and says the lease has run out; and a
  program that did not declare `needs bluetooth` has no door to ask.

Twice: with an xHCI controller, where the driver serves the stack from the
wait it keeps over its controllers, and without one, where it serves it from
the wait it keeps when there is none - which is what most suites boot.

Usage: run_bluetooth.py IMAGE
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"

SCANNED = [
    ("Keyboard K380", "keyboard", "classic", "02:4B:53:00:03:80", "-48 dBm"),
    ("MX Anywhere 3", "mouse", "le", "F2:4B:53:4D:58:33", "-60 dBm"),
    ("Pixel Buds", "headphones", "classic", "02:4B:53:00:D5:5B", "-71 dBm"),
]

READY = ("bluetooth: simulated, Bluetooth 5.1 from a test company, "
         "02:4B:4F:53:4D:4F: Classic and Low Energy, SSP, 8 ACL buffers of "
         "1021 bytes")

def boot(with_controller, failed):
    import run_screenshot as R

    # Added for the second boot: `extra_args` only ever appends, which is
    # why the boot without a controller is the first.
    if with_controller:
        R.extra_args(IMAGE, ["-device", "qemu-xhci,id=usb0"])
    guest = R.Guest(IMAGE, 120)
    where = "with a controller" if with_controller else "without a controller"
    checks = 0

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append("%s: %s" % (where, complaint))

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        guest.type("log bluetooth")
        deadline = time.monotonic() + 30

        while time.monotonic() < deadline and READY not in guest.seen[mark:]:
            time.sleep(0.2)
            guest._read_available()

        said = guest.seen[mark:]

        check(READY in said, "the stack did not name the simulated adapter "
              "through the ring:\n" + said[-800:])
        check("bluetooth: the adapter did not answer" not in said
              and "bluetooth: the adapter answered" not in said,
              "a command went unanswered or failed:\n" + said[-800:])
        def run(line, until, seconds=30):
            at = len(guest.seen)
            guest.type(line)
            end = time.monotonic() + seconds

            while time.monotonic() < end and until not in guest.seen[at:]:
                time.sleep(0.2)
                guest._read_available()

            return guest.seen[at:]

        said = run("bt scan 2", "bt: done")
        check("bt: simulated adapter, Bluetooth 5.1 from a test company, "
              "02:4B:4F:53:4D:4F: Classic and Low Energy, SSP; looking" in said,
              "bt scan did not say the adapter, looking:\n" + said[-800:])

        for name, kind, transport, addr, rssi in SCANNED:
            line = re.search(r"bt:   %s\s+%s\s+%s\s+%s  %s" % (
                re.escape(name), re.escape(kind), transport, addr, rssi), said)
            check(line, "bt scan did not find %s as a %s on %s:\n%s" % (
                name, kind, transport, said[-900:]))

        check("bt: done, 3 devices heard" in said,
              "bt scan did not end with three devices:\n" + said[-600:])

        # **Until the prompt, not the count.** `bt` says "3 devices heard"
        # before it lists them, so waiting for that read the listing while it
        # was still arriving: under the 0.11.334 gate's load the third row had
        # not, and the order looked wrong (`testing.md` 18.753).
        said = run("bt", "kosmos> ")
        rows = re.findall(r"bt:   (\S.*?)\s{2,}", said)
        check(rows[:3] == [n for n, *_ in SCANNED],
              "bt did not list them nearest first: %r" % rows)
        check("; not looking" in said,
              "the scan's lease did not end by itself:\n" + said[-600:])

        # No door for a program that did not ask for one.
        run('fs.write("/Temporary/nobt.lua", [[local b = use("/Kosmos/Libraries/bluetooth.lua") '
            'local s, why = b.state() print("nobt: " .. tostring(s) .. " " .. tostring(why))]]) '
            'print("no" .. "bt-written")', "nobt-written")
        said = run("run /Temporary/nobt.lua", "kosmos> ")   # its whole line, not its start
        check("nobt: nil " in said,
              "a program that did not ask for Bluetooth reached it:\n" + said[-400:])

        check("refused the wait" not in guest.seen,
              "the kernel refused the driver's wait on its endpoints")
        check("no Bluetooth stack" not in guest.seen,
              "init could not start the stack")
    finally:
        guest.close()

    return checks


def main():
    failed = []
    checks = boot(False, failed) + boot(True, failed)

    if failed:
        for f in failed:
            print("bluetooth: FAIL: " + f)
        print("FAIL: %d of %d checks on the Bluetooth host stack" % (len(failed), checks))
        return 1

    print("PASS: %d checks on the Bluetooth host stack, on x86-64 (btsim named "
          "through the ring, with an xHCI controller and without)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
