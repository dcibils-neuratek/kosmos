#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The profiler says Lua where the time was Lua, and C where it was C.

**The App Inspector's first step** (`roadmap.md`; Diego, 29 September:
"lets profile in the m700 the Lua VS C"): the kernel samples every
processor at every tick (`kernel/profile.c`), `profile` writes the samples
to `/Home/profiles`, and `tools/profile_report.py` names each address from
the build's symbols. A report nobody can check is an opinion, so this runs
two programs whose answer is known while `profile` watches:

  - one that is nothing but the interpreter - arithmetic in a Lua loop -
    which has to come out Lua;
  - one that is nothing but C - four megabytes copied by `sys.region_copy`,
    again and again - which has to come out C.

And around them what the kernel promises: about one sample a processor a
tick, none lost; a program that did not declare `needs profile` refused;
and a second profile refused while one runs; and one typed in a Terminal,
as on a machine whose desktop starts by itself, written. The report has to
have found the symbols that ran - its anchor, `str_format`, agreeing - and
nothing may have died.

Usage: run_profile.py IMAGE
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

SECONDS = 4

# **Below `profile`, and until told**, both of them. Below it because
# `profile` has to wake to drain its rings, and on the one processor QEMU's
# x86-64 has, two spinners in its own band starved it until ten thousand
# samples were lost - `spin` steps down for the same reason. And until told
# rather than for a time, because under that emulator the counter ran five
# times faster than it says, and eight seconds of it ended both spinners
# before the profile had begun. A minute of it is the limit for a harness
# that never says, and whether it has said is asked four times a second.
STOPPED = r'''
pcall(sys.step_down, 2)

local hz = fs.read("/Devices/cpu").counter_hz
local limit = sys.ticks() + hz * 60
local look = 0

local function stopped()
  local now = sys.ticks()

  if now < look then return now > limit end

  look = now + hz // 4
  return fs.getattr("/Temporary/stop") ~= nil or now > limit
end
'''

PROBES = {
    # The interpreter and nothing else.
    "luaspin.lua": STOPPED + r'''
local x = 0

repeat
  for i = 1, 20000 do x = (x * 31 + i) % 1000003 end
until stopped()

print("LUASPIN " .. x)
''',
    # C and nothing else: a copy of four megabytes is thousands of times
    # longer than the Lua around it.
    "cspin.lua": STOPPED + r'''
local a, b = sys.memory(1024), sys.memory(1024)
local n = 0

repeat
  sys.region_copy(b, 0, a, 0, 4 * 1024 * 1024)
  n = n + 1
until stopped()

print("CSPIN " .. n)
''',
    # The dark palette the display harness counts windows by - its tabs
    # are yellow - since this disk has no preferences of its own.
    "look.lua": r'''
fs.send("/Home/Preferences", { type = "mkdir" })
fs.write("/Home/Preferences/appearance", { palette = "dark" })
print("LOOK SET")
''',
    # No `needs profile` in its header, so no right to it.
    "noright.lua": r'''
local ok, why = sys.profile("start")
print("NORIGHT " .. tostring(ok) .. " " .. tostring(why))
''',
}


def in_a_terminal(guest, R):
    """`profile 1 term` typed in a Terminal; whether its file was written.

    **Where it is typed on the M700**, whose desktop starts by itself: the
    right passes from the shell to the window manager to the Terminal to
    `profile`, each holding it only to hand it on (`design.md` 9.2). A link
    missing is a spawn refused, and no file.
    """
    guest.type("run /Home/look.lua")
    guest.wait_for("LOOK SET", "the palette the harness counts windows by")
    guest.type("wm terminal")
    R.started(guest)
    width, height, _ = R.parse_ppm(guest.screendump())

    guest.mouse_to(*R._to_tablet(300, 200, width, height))
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)
    guest.mouse_button(False)
    time.sleep(0.6)

    # The window as it is before, so a change is what was typed.
    def window():
        w, h, px = R.parse_ppm(guest.screendump())
        return R._strip(px, w, 100, 100, 600, 500)

    before = window()

    for ch in "profile 1 term\n":
        guest.proc.stdin.write(ch.encode())
        guest.proc.stdin.flush()
        time.sleep(0.08)

    # Drawn, and then still for long enough that the second of profiling
    # and its summary are behind it: its first line comes at once and the
    # rest a second later.
    deadline = time.monotonic() + 40
    last, still_since, changed = before, None, False

    while time.monotonic() < deadline:
        time.sleep(0.5)
        now = window()

        if now != last:
            changed, last, still_since = True, now, time.monotonic()
        elif changed and time.monotonic() - still_since > 3:
            break

    guest.proc.stdin.write(R.STOP_DESKTOP)
    guest.proc.stdin.flush()

    # At the prompt again, asked until it says - the file is written
    # before the summary is drawn, so this is a formality when it works.
    for _ in range(20):
        time.sleep(1)
        mark = len(guest.seen)
        guest.type("ls /Home/profiles")
        time.sleep(1)

        if "term.kprof" in guest.seen[mark:]:
            return True

    return False


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    work = scratch.directory("profile")
    disk = os.path.join(work, "disk.img")
    pairs = []

    for name, source in PROBES.items():
        path = os.path.join(work, name)

        with open(path, "w") as f:
            f.write(source)

        pairs.append(path + ":/Home/" + name)

    subprocess.run([os.path.join(ROOT, "build", "host", "lua"),
                    os.path.join(HERE, "kfs.lua"), "create", disk, "32", *pairs],
                   check=True, capture_output=True, cwd=ROOT)

    os.environ["KOSMOS_DISK"] = disk
    import run_screenshot as R                                  # noqa: E402

    guest = R.Guest(image, 120)
    fails = []

    try:
        guest.wait_for("kosmos>", "a prompt")
        guest.type("run /Home/noright.lua")
        guest.wait_for("NORIGHT ", "the refusal to a program without the right")
        guest.type("run /Home/luaspin.lua &")
        guest.type("run /Home/cspin.lua &")
        time.sleep(1)
        guest.type("profile %d test &" % SECONDS)

        # Started, or refused - which is said at once and waits for nothing.
        mark = len(guest.seen)
        deadline = time.monotonic() + 60

        while time.monotonic() < deadline:
            if "profile: %d s on" % SECONDS in guest.seen[mark:]:
                break

            # Its own refusal, or the shell's when the spawn itself is
            # refused - a shell without the right to hand on.
            refused = re.search(r"profile: (?!\d+ s on).*|.*could not start a "
                                r"process for it.*", guest.seen[mark:])

            if refused:
                fails.append("profile was refused: " + refused.group(0).strip())
                return report(fails)

            time.sleep(0.1)
        else:
            fails.append("profile never started")
            return report(fails)

        guest.type("profile 1 second")
        guest.wait_for("written to /Home/profiles/test.kprof", "the profile written")
        guest.type("touch /Temporary/stop")
        guest.wait_for("CSPIN ", "the C spinner finishing")
        guest.wait_for("LUASPIN ", "the Lua spinner finishing")

        if not in_a_terminal(guest, R):
            fails.append("`profile 1 term` typed in a Terminal wrote no "
                         "/Home/profiles/term.kprof - a link of the grant "
                         "from the shell to the window manager to the "
                         "Terminal is missing")

        time.sleep(0.5)
        guest._read_available()
    finally:
        guest.close()

    said = guest.seen

    if not re.search(r"NORIGHT nil this program may not profile", said):
        fails.append("a program without `needs profile` was not refused: "
                     + (re.search(r"NORIGHT .*", said) or [said[-300:]])[0])

    if "profile: another program is profiling" not in said:
        fails.append("a second profile was not refused while one ran")

    if " died: " in said:
        fails.append("something died: " + said[said.find(" died: ") - 80:][:400])

    got = os.path.join(work, "test.kprof")
    subprocess.run([os.path.join(ROOT, "build", "host", "lua"),
                    os.path.join(HERE, "kfs.lua"), "get", disk,
                    "/Home/profiles/test.kprof", got], capture_output=True, cwd=ROOT)

    if not os.path.exists(got):
        fails.append("no /Home/profiles/test.kprof on the disk")
        return report(fails)

    summary = os.path.join(work, "summary.json")
    run = subprocess.run([sys.executable, os.path.join(HERE, "profile_report.py"), got,
                          "--out", os.path.join(work, "test.html"), "--json", summary],
                         capture_output=True, text=True, cwd=ROOT)

    if run.returncode != 0 or not os.path.exists(summary):
        fails.append("the report did not run:\n" + run.stdout[-600:] + run.stderr[-600:])
        return report(fails)

    # The report itself, into the suite's log: what a failure is read with.
    print(run.stdout)

    with open(summary) as f:
        s = json.load(f)

    due = s["cores"] * s["tick_hz"] * s["seconds"]

    # Below by a quarter at most: a Mac running the whole gate side by side
    # can make QEMU late with a tick, and a late tick is a sample not taken
    # rather than a wrong one.
    if not 0.75 * due <= s["samples"] <= 1.15 * due:
        fails.append("%d samples in %.2f s on %d processors at %d Hz, where about "
                     "%d were due" % (s["samples"], s["seconds"], s["cores"],
                                      s["tick_hz"], due))

    if s["lost"] != 0:
        fails.append("%d samples lost" % s["lost"])

    if not any("agrees" in n for n in s["notes"]):
        fails.append("the report found no symbols that ran: " + "; ".join(s["notes"]))

    # A processor each where there are two, and half of one each where
    # there is one - QEMU's x86-64 - and at least half of that.
    fair = s["tick_hz"] * s["seconds"] * min(s["cores"], 2) / 2

    for name, layer, least in (("luaspin", "Lua", 0.85), ("cspin", "C", 0.85)):
        p = s["processes"].get(name)

        if not p:
            fails.append("%s is not in the profile" % name)
            continue

        share = p["layers"].get(layer, 0) / max(1, p["samples"])

        if p["samples"] < 0.5 * fair:
            fails.append("%s had %d samples, where its share of the processors "
                         "was about %d" % (name, p["samples"], fair))

        if share < least:
            fails.append("%s was %.0f%% %s, not %.0f%% or more: %s"
                         % (name, 100 * share, layer, 100 * least,
                            json.dumps(p["classes"])))

    return report(fails, s)


def report(fails, s=None):
    checks = 10

    if fails:
        print("FAIL: %d of %d checks on the profiler:" % (len(fails), checks))

        for f in fails:
            print("  " + f)

        return 1

    lua = s["processes"]["luaspin"]
    c = s["processes"]["cspin"]
    print("PASS: %d checks on the profiler (%d samples on %d processors in %.1f s, "
          "none lost; the Lua loop %.0f%% Lua and the copy %.0f%% C, named from "
          "symbols whose anchor agreed; no right refused, a second profile "
          "refused, one typed in a Terminal written, nothing dead)."
          % (checks, s["samples"], s["cores"], s["seconds"],
             100 * lua["layers"].get("Lua", 0) / lua["samples"],
             100 * c["layers"].get("C", 0) / c["samples"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
