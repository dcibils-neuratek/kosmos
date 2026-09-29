#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The desktop keeps answering while an installed application starts.

Diego, 29 September 2026: running Doom or Quake "will get the desktop stuck
for a second and then run" - since they became applications installed in
`/Home/Apps` with images of their own (`docs/elf.md`), where before they were
compiled into the system's image. The window manager starts what the Deskbar
asks for, and an installed application's image - eighteen megabytes - was
read off the disk inside that request: nothing was drawn and nobody was
answered until it was all in. Under QEMU that was 283 ms; on the M700 the
image comes off a USB stick.

Now the window manager reads it a window a pass (`IMAGES.load`'s `pace`,
`wm.lua`'s `launching`), and this holds it to that. A disk with Doom
installed, the desktop started with two programs of this harness's: one asks
the window manager for its list of windows in a loop, the other asks it to
start Doom and notes when the answer came. **The window manager has to have
answered the first at least ten times while Doom was being started** - the
old way it answered none, or the one already in flight - and the longest it
went without answering is said beside it.

Usage: run_launch.py IMAGE
"""

import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402

ANSWERS = 10            # at least this many answers during the launch

# Asks the window manager for its windows, over and over, for four seconds,
# noting the counter after each answer; then reads when the launch began
# and ended and counts the answers inside.
PINGER = (
    'local hz = fs.read("/Devices/cpu").counter_hz '
    'local times, stop, seen, second = {}, sys.ticks() + 4 * hz, false, nil '
    'while sys.ticks() < stop do local r = fs.send("/Running/wm", { type = "windows" }) '
    'times[#times + 1] = sys.ticks() '
    'for _, s in ipairs(r and r.starting or {}) do if s.program:find("Doom") and not seen then '
    'seen = true local r2 = fs.send("/Running/wm", { type = "launch", program = "/Home/Apps/Doom/doom.lua" }) '
    'second = r2 and r2.starting end end end '
    'print("STARTING" .. " " .. tostring(seen) .. " " .. tostring(second)) '
    'local w = fs.read("/Temporary/window") or "" '
    'local a, b = w:match("(%d+) (%d+)") a, b = tonumber(a), tonumber(b) '
    'local n, worst, last = 0, 0, a '
    'for _, t in ipairs(times) do if a and t > a and t <= b then n = n + 1 '
    'worst = math.max(worst, t - last) last = t end end '
    'if a then worst = math.max(worst, b - last) end '
    'print(("PING" .. " %d answers during a launch of %.1f ms, the longest without one %.1f ms")'
    ':format(n, a and (b - a) / hz * 1000 or -1, worst / hz * 1000))\n')

# Starts the pinger, lets it get going, then asks for Doom and notes when.
PROBE = (
    'fs.send("/Running/wm", { type = "launch", program = "/Home/launch/pinger.lua" }) '
    'sys.sleep(300) local t0 = sys.ticks() '
    'local r = fs.send("/Running/wm", { type = "launch", program = "/Home/Apps/Doom/doom.lua" }) '
    'local t1 = sys.ticks() fs.write("/Temporary/window", t0 .. " " .. t1) '
    'print("LAUNCH" .. " " .. tostring(r and r.ok) .. " " .. tostring(r and r.error))\n')


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    arch = "x86_64" if "x86_64" in image else "aarch64"
    disk = scratch.path("launch-disk.img")

    # Doom's image as the gate links it, against the test userland it builds,
    # or `make apps` against the lean one - as `run_media.py` takes the Super
    # Nintendo's. No WAD: what is timed is the start, not the game.
    import installed                                            # noqa: E402

    tested = os.path.join(ROOT, "build", "user-x86_64-test" if arch == "x86_64"
                          else "user-test", "apps")
    pairs = [p for p in installed.pairs(arch, None,
                                        tested if os.path.isdir(tested) else None)
             if "/Home/Apps/Doom/" in p]

    if not any(p.endswith(".elf:/Home/Apps/Doom/doom.elf") for p in pairs):
        print("FAIL: no doom.elf to install - `make apps` builds it")
        return 1

    # The two programs go on the disk as files rather than typed at the
    # prompt: a line the shell reads stops at about a kilobyte, and the
    # pinger is longer than that.
    programs = []

    for name, source in (("pinger.lua", PINGER), ("probe.lua", PROBE)):
        path = scratch.path("launch-" + name)

        with open(path, "w") as f:
            f.write(source)

        programs.append(path + ":/Home/launch/" + name)

    subprocess.run([os.path.join(ROOT, "build", "host", "lua"),
                    os.path.join(HERE, "kfs.lua"), "create", disk, "96"]
                   + pairs + programs,
                   check=True, capture_output=True, cwd=ROOT)

    os.environ["KOSMOS_DISK"] = disk
    import run_screenshot as R                                  # noqa: E402

    guest = R.Guest(image, 240)
    fails = []

    try:
        guest.wait_for("kosmos>", "a prompt")
        guest.type("wm /Home/launch/probe.lua")
        guest.wait_for("PING ", "the pinger's count")
        time.sleep(0.5)
        guest._read_available()
    finally:
        guest.close()
        os.unlink(disk)

        for p in programs:
            os.unlink(p.split(":", 1)[0])

    said = guest.seen
    launch = re.search(r"^LAUNCH (\S+) (.*)$", said, re.M)
    ping = re.search(r"^PING (\d+) answers during a launch of ([\d.]+) ms, "
                     r"the longest without one ([\d.]+) ms", said, re.M)
    seen = re.search(r"^STARTING (\S+) (\S+)", said, re.M)
    launches = len(re.findall(r"wm: launching /Home/Apps/Doom/doom.lua", said))

    if not launch or launch.group(1) != "true":
        fails.append("Doom did not start: %s" % (launch.group(0) if launch
                                                  else said[-600:]))

    if not ping:
        fails.append("the pinger did not say what it counted:\n" + said[-600:])
    elif int(ping.group(1)) < ANSWERS:
        fails.append("the window manager answered %s times in the %s ms Doom "
                     "took to start, and went %s ms without answering - it "
                     "stopped for the launch" % ping.groups())

    # **And it says so, and starts it once** (`docs/launching.html`): while
    # Doom loads, the window manager lists it as starting - which is what
    # the Deskbar draws a button from - and a second request for it, sent
    # the moment it is listed, is answered "starting" and starts nothing.
    if not seen or seen.group(1) != "true":
        fails.append("Doom was never listed as starting while it loaded: %s"
                     % (seen.group(0) if seen else "no word from the pinger"))
    elif seen.group(2) != "true" or launches != 1:
        fails.append("a second request for Doom while it started was answered "
                     "%s and Doom was launched %d times" % (seen.group(2), launches))

    if fails:
        print("FAIL: %d of 4 checks on the desktop while Doom starts:"
              % len(fails))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: 4 checks on the desktop while an installed application starts "
          "(Doom started, listed as starting and started once for two requests; "
          "the window manager answered %s times in its %s ms, the longest "
          "without an answer %s ms)." % ping.groups())
    return 0


if __name__ == "__main__":
    sys.exit(main())
