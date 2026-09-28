#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""More windows than one message can list (`roadmap.md` 6zp).

The window manager answered `windows` - what is open - in one reply, and a
reply is a message of 2048 bytes: about sixteen windows. On 28 September the
dated picture opened sixteen applications and the Deskbar; the seventeenth
did not fit, the window manager dropped the reply, and `tile` and the
Deskbar, each waiting for it in `fs.send`, waited for ever. The picture
showed Groove, maximised, over everything, and the Deskbar had stopped.

Now the list comes a page at a time and `wmproto.windows` puts it together,
and an answer that cannot be sent becomes a small one saying so. So: the
Deskbar and twenty Calculators, and `tile` told to wait for all twenty -

- `tile` arranges twenty windows, which it can only do having read every
  page, and says so;
- no reply is dropped on the way.

Usage: run_windows.py IMAGE
"""

import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
COUNT = 20


def main():
    import run_screenshot as R

    guest = R.Guest(IMAGE, 120)
    failed, checks = [], 0

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        guest.type('fs.write("/Home/Preferences/startup", { items = {} }) '
                   'print("windows" .. "-ready")')
        guest.wait_for("windows-ready", "emptied the login set")

        mark = len(guest.seen)
        guest.type("wm deskbar," + ",".join(["calc"] * COUNT) + ",tile:%d" % COUNT)
        deadline = time.monotonic() + 100

        while time.monotonic() < deadline and "tile: " not in guest.seen[mark:]:
            time.sleep(0.3)
            guest._read_available()

        time.sleep(1)
        guest._read_available()
        said = guest.seen[mark:]
        tiled = [l for l in said.splitlines() if l.startswith("tile: ")]

        check(any(l.startswith("tile: %d windows" % COUNT) for l in tiled),
              "tile did not arrange all %d windows: %r\n%s" % (COUNT, tiled, said[-1200:]))
        check("reply for windows failed" not in said,
              "the window manager dropped a reply for the list of windows:\n" + said[-800:])
    finally:
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on more windows than one message lists:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on more windows than one message lists (the Deskbar and "
          "%d Calculators, every one arranged by tile from the list in pages, and no "
          "reply dropped)" % (checks, COUNT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
