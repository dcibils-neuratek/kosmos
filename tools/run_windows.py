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
- no reply is dropped on the way;
- and `/Running` lists every Calculator. It held 24 names in a fixed
  table, and the twenty-fifth application was refused (`cleanup.md`
  finding 206); thirty, a list of more than one page, is past it.

Usage: run_windows.py IMAGE
"""

import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
COUNT = 30


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
        # A program in the desktop's list that counts the Calculators in
        # /Running once tile has had them all, since keys typed after the
        # desktop starts go to its focused window rather than to the shell.
        guest.type('fs.write("/Home/Preferences/startup", { items = {} }) '
                   'fs.write("/Temporary/count.lua", "local hz = (sys.info() or {}).tick_hz or 100 '
                   'local best = 0 for _ = 1, 120 do sys.sleep(hz // 2) local n = 0 '
                   'for _, a in ipairs(fs.list(\'/Running\') or {}) do '
                   'if tostring(a.name or a):lower():find(\'calc\') then n = n + 1 end end '
                   'if n > best then best = n end if n >= %d then break end end '
                   'print(\'RUNNING-CALCS \' .. best)") '
                   'print("windows" .. "-ready")' % COUNT)
        guest.wait_for("windows-ready", "emptied the login set")

        mark = len(guest.seen)
        guest.type("wm deskbar," + ",".join(["calc"] * COUNT) + ",tile:%d" % COUNT
                   + ",/Temporary/count.lua")
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

        deadline = time.monotonic() + 70
        while time.monotonic() < deadline and "RUNNING-CALCS " not in guest.seen[mark:]:
            time.sleep(0.3)
            guest._read_available()
        time.sleep(0.5)
        guest._read_available()
        counted = [l for l in guest.seen[mark:].splitlines() if "RUNNING-CALCS " in l]
        n = int(counted[-1].split("RUNNING-CALCS ")[1].split()[0]) if counted else -1
        check(n >= COUNT,
              "/Running listed %d Calculators of %d open" % (n, COUNT))
    finally:
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on more windows than one message lists:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on more windows than one message lists (the Deskbar and "
          "%d Calculators, every one arranged by tile from the list in pages, no "
          "reply dropped, and every one listed in /Running past its old 24)" % (checks, COUNT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
