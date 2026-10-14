#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Folders that tell: Tracker sees a change the moment it happens
(`docs/pulses.md` P4).

One boot with a disk for /Home. Tracker is opened on a folder of the disk,
and then - from another program, over Telnet - a file is written there,
renamed, and deleted: each time Tracker lists the folder again by itself,
told by the disk's pulse through the window manager, with nothing looking
on a clock. The same in /Temporary, whose server is the other one. A change
in another folder tells it nothing.

Usage: run_livefolders.py IMAGE
"""

import os
import random
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402

_DISK = os.path.join(scratch.directory("livefolders"), "disk.img")
subprocess.run([os.path.join(os.path.dirname(HERE), "build", "host", "lua"),
                os.path.join(HERE, "kfs.lua"), "create", _DISK, "16"],
               check=True, capture_output=True, cwd=os.path.dirname(HERE))
os.environ["KOSMOS_DISK"] = _DISK

import run_servers as S                                     # noqa: E402

# A program that is not Tracker changing a folder: `args` is the verb and
# its paths.
CHANGE = ('local verb, a, b = args:match("^(%S+)%s+(%S+)%s*(%S*)$")\n'
          'local files = use("/Kosmos/Libraries/files.lua")\n'
          'local ok, why\n'
          'if verb == "write" then ok, why = fs.write(a, "written by another program\\n")\n'
          'elseif verb == "mkdir" then ok, why = files.make_folder(a)\n'
          'elseif verb == "rename" then ok, why = files.move(a, b)\n'
          'elseif verb == "delete" then ok, why = files.remove(a) end\n'
          'print("CHANGED " .. verb .. " " .. tostring(ok and true) .. " " .. tostring(why))\n')

LAUNCH = ('local r = fs.send("/Running/wm", { type = "launch", program = "tracker", args = args })\n'
          'print("LAUNCH " .. tostring(r and r.ok))\n')


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"

    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web)
    checks, fails, said = 0, [], {}

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            fails.append(complaint)

    def line(pattern, since, seconds=10):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            guest._read_available()
            m = re.search(pattern, guest.seen[since:])
            if m:
                return m
            time.sleep(0.05)
        return None

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)

        session.put(CHANGE.encode(), "/Temporary/change.lua")
        session.put(LAUNCH.encode(), "/Temporary/launch.lua")

        def change(words):
            out = session.run("/Temporary/change.lua " + words).decode(errors="replace")
            said[words] = out
            return "true" in out

        for folder in ("/Home/Live", "/Temporary/Live"):
            check(change("mkdir " + folder), "%s was not made" % folder)
            change("write %s/first.txt" % folder)

            mark = len(guest.seen)
            session.run("/Temporary/launch.lua " + folder)
            told = line(r"tracker: %s (tells when it changes|is looked at as before[^\r\n]*)" % re.escape(folder),
                        mark, 30)
            check(told is not None and told.group(1) == "tells when it changes",
                  "%s did not say it tells: %r" % (folder, told and told.group(0)))
            shown = line(r"tracker: showing %s, (\d+) item" % re.escape(folder), mark, 5)
            check(shown is not None and shown.group(1) == "1",
                  "Tracker did not open %s with its one file: %r" % (folder, shown and shown.group(0)))

            # Written by another program: listed again by itself, at once.
            for verb, words, count in (("written", "write %s/second.txt" % folder, "2"),
                                       ("renamed", "rename %s/second.txt %s/renamed.txt" % (folder, folder), "2"),
                                       ("deleted", "delete %s/first.txt" % folder, "1")):
                mark = len(guest.seen)
                started = time.monotonic()
                ok = change(words)
                again = line(r"tracker: %s changed, listed again" % re.escape(folder), mark, 5)
                took = time.monotonic() - started
                shown = line(r"tracker: showing %s, (\d+) item" % re.escape(folder), mark, 2)
                check(ok and again is not None and shown is not None and shown.group(1) == count,
                      "a file %s in %s by another program was not shown by itself: %s, %r"
                      % (verb, folder, ok, shown and shown.group(0)))
                said["%s %s took" % (folder, verb)] = "%.2f s" % took

            # A change in another folder tells it nothing.
            mark = len(guest.seen)
            change("write /Home/elsewhere-%d.txt" % random.randint(1, 1 << 30))
            time.sleep(1.5)
            guest._read_available()
            check("tracker: %s changed" % folder not in guest.seen[mark:],
                  "a change elsewhere told %s's window" % folder)
    except Exception as e:                  # noqa: BLE001 - said below
        check(False, "the steps stopped: %s: %s" % (type(e).__name__, str(e)[:500]))
    finally:
        transcript = guest.seen
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on folders that tell:" % (len(fails), checks))
        for f in fails:
            print("  " + f)
        for k, v in said.items():
            print("  %s: %r" % (k, v[-200:]))
        print("--- the guest said ---\n" + transcript[-3000:])
        return 1

    times = ", ".join("%s %s" % (k.split(" ", 1)[1].replace(" took", ""), v)
                      for k, v in said.items() if k.endswith(" took") and k.startswith("/Home"))
    print("PASS: %d checks on folders that tell (Tracker on a folder of the disk and one in "
          "/Temporary; a file written, renamed and deleted by another program shown by itself "
          "each time - %s; a change elsewhere telling it nothing)" % (checks, times))
    return 0


if __name__ == "__main__":
    sys.exit(main())
