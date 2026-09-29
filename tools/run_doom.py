#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Doom plays at its speed, and sleeps while it waits rather than spinning.

**Found by the profiler on the M700** (`roadmap.md`, FOUND on 29
September): `DG_SleepMs` yielded until the counter passed its deadline, and
on eight processors with room on them a yield comes straight back - 84% of
Doom's samples were on the way back from one, about 29% of a processor spent
waiting. It sleeps now, in scheduler ticks, and that could have cost the
game its speed - so both are held here: **the frames a second Doom reports**,
and **where its time went while it played**, taken by `profile` and named
by `tools/profile_report.py`.

It needs a WAD, and no WAD is in the repository - `make doom-check
WAD=/path/to/doom1.wad`, outside the gate for `quake-check`'s reason. Doom
plays its own demo with nobody at the keys.

Usage: run_doom.py IMAGE WAD
"""

import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402

SECONDS = 30            # the profile's; Doom's first report comes at ten
LEAST_FPS = 30.0        # of the thirty-five it holds itself to
MOST_SLEEPING = 0.10    # of Doom's samples, on the way back from its sleep


def main():
    if len(sys.argv) < 3 or not os.path.isfile(sys.argv[2]):
        sys.exit("FAIL: no WAD. make doom-check WAD=/path/to/doom1.wad")

    image, wad = sys.argv[1], sys.argv[2]
    arch = "x86_64" if "x86_64" in image else "aarch64"
    work = scratch.directory("doom")
    disk = os.path.join(work, "disk.img")

    built = os.path.join(ROOT, "build", "user-x86_64" if arch == "x86_64" else "user",
                         "apps", "doom.elf")
    objcopy = ("x86_64-elf-" if arch == "x86_64" else "aarch64-none-elf-") + "objcopy"
    elf = os.path.join(work, "doom.elf")
    subprocess.run([objcopy, "--strip-debug", built, elf], check=True)

    folder = "/Home/Apps/Doom"
    subprocess.run([os.path.join(ROOT, "build", "host", "lua"),
                    os.path.join(HERE, "kfs.lua"), "create", disk, "96",
                    os.path.join(ROOT, "user", "installed", "Doom", "doom.lua")
                    + ":" + folder + "/doom.lua",
                    elf + ":" + folder + "/doom.elf",
                    wad + ":" + folder + "/doom1.wad"],
                   check=True, capture_output=True, cwd=ROOT)

    os.environ["KOSMOS_DISK"] = disk
    import run_screenshot as R                                  # noqa: E402

    guest = R.Guest(image, 180)
    fails = []

    try:
        guest.wait_for("kosmos>", "a prompt")
        guest.type("profile %d doom &" % SECONDS)
        guest.wait_for("profile: %d s on" % SECONDS, "the profile starting")
        guest.type("wm %s/doom.lua" % folder)
        guest.wait_for("frames a second", "Doom's first report of its frames")
        guest.wait_for("written to /Home/profiles/doom.kprof", "the profile written")
        guest.proc.stdin.write(R.STOP_DESKTOP)
        guest.proc.stdin.flush()
        time.sleep(1)
        guest._read_available()
    finally:
        guest.close()

    said = guest.seen

    if " died: " in said:
        fails.append("something died: " + said[said.find(" died: ") - 80:][:400])

    rates = [float(x) for x in re.findall(r"doom: ([\d.]+) frames a second", said)]

    if not rates or min(rates) < LEAST_FPS:
        fails.append("Doom played at %s frames a second, not %g or more"
                     % (rates or "no", LEAST_FPS))

    got = os.path.join(work, "doom.kprof")
    subprocess.run([os.path.join(ROOT, "build", "host", "lua"),
                    os.path.join(HERE, "kfs.lua"), "get", disk,
                    "/Home/profiles/doom.kprof", got], capture_output=True, cwd=ROOT)
    summary = os.path.join(work, "summary.json")
    run = subprocess.run([sys.executable, os.path.join(HERE, "profile_report.py"), got,
                          "--out", os.path.join(work, "doom.html"), "--json", summary],
                         capture_output=True, text=True, cwd=ROOT)
    print(run.stdout)

    if run.returncode != 0 or not os.path.exists(summary):
        fails.append("the report did not run:\n" + run.stderr[-600:])
        return report(fails, rates, None)

    with open(summary) as f:
        doom = json.load(f)["processes"].get("doom")

    if not doom:
        fails.append("Doom is not in the profile")
        return report(fails, rates, None)

    sleeping = sum(f["samples"] for f in doom["functions"]
                   if f["name"].startswith("DG_SleepMs"))
    share = sleeping / max(1, doom["samples"])

    if share > MOST_SLEEPING:
        fails.append("%.0f%% of Doom's samples were in its sleep - waiting is "
                     "costing it a processor" % (100 * share))

    return report(fails, rates, (share, doom["samples"]))


def report(fails, rates, sleep):
    checks = 3

    if fails:
        print("FAIL: %d of %d checks on Doom playing (%s frames a second):"
              % (len(fails), checks, ", ".join("%.1f" % r for r in rates) or "no"))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on Doom playing (%s frames a second; %.1f%% of its "
          "%d samples in its sleep; nothing dead)."
          % (checks, ", ".join("%.1f" % r for r in rates), 100 * sleep[0], sleep[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
