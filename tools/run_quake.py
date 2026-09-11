#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Quake on the machine: the shareware pak read, a demo played, a command typed.

`make quake` says the engine compiles, and `make test` checks the scanner that
reads its demos and a region the size of its pak. None of that is Quake: the
pak read through the namespace into a region, the engine started on its own
stack, a map loaded and drawn, and keys arriving at the game. This is.

**The pak is not in the repository**, so this needs one:
`make quake-check PAK=/path/to/pak0.pak`, the shareware 1.06 file. It goes on
a disk image of its own under `build/quake/`, never on `build/kosmos.img`.

**Checked through what Quake says**, which reaches the machine's console: the
engine's own start-up lines, the demo it plays and the map that demo loads -
and a command typed at Quake's console, whose answer appears twice only if
the keys arrived and the command ran, because Quake echoes the line first.
The one picture check is that the window holds more than a handful of
colours: a window that opened and drew nothing is the failure every port here
has had at least once.

Only for an image built with `make QUAKE=1`, which the ordinary image is not.

Usage: run_quake.py [image] [pak]
"""

import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DISK = os.path.join(ROOT, "build", "quake", "quake.img")
LUA = os.path.join(ROOT, "build", "host", "lua")

# The shareware 1.06 pak. Another pak may well work; this is the one checked.
PAK_SIZE = 18689235

# The word typed at Quake's console. Unusual enough not to appear otherwise.
MARKER = "kosmosquake"

# QEMU's names for the keys that are not letters.
KEY = {" ": "spc", "`": "grave_accent"}


def soak(guest, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        guest._read_available()
        time.sleep(0.25)


def said_after(guest, mark, text, seconds):
    """Whether `text` appears after `mark` within `seconds`."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        guest._read_available()
        if text in guest.seen[mark:]:
            return True
        time.sleep(0.25)
    return text in guest.seen[mark:]


def keys(guest, text):
    for ch in text:
        guest.sendkey(KEY.get(ch, ch))


def colours_in(parse_ppm, data, x, y, w, h):
    """How many distinct colours a region of a screendump holds, sampled."""
    sw, sh, rgb = parse_ppm(data)
    seen = set()
    for row in range(y, min(y + h, sh), 3):
        for col in range(x, min(x + w, sw), 3):
            at = (row * sw + col) * 3
            seen.add(rgb[at:at + 3])
    return len(seen)


def disk_with(pak):
    """A 64 MB kfs image holding the pak where `quake.lua` looks for it."""
    os.makedirs(os.path.dirname(DISK), exist_ok=True)
    if os.path.exists(DISK):
        os.remove(DISK)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", DISK, "64",
                    pak + ":/home/id1/pak0.pak"], check=True)


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    pak = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("PAK", "")

    if not pak or not os.path.isfile(pak):
        print("FAIL: no pak. make quake-check PAK=/path/to/pak0.pak - the "
              "shareware 1.06 pak0.pak, which is not in the repository.")
        return 1

    # `kfs.lua create` splits `host:guest` at the first colon.
    if ":" in pak:
        print("FAIL: %s has a colon in it, which kfs.lua would read as the "
              "end of the host path - copy the pak somewhere without one" % pak)
        return 1

    with open(pak, "rb") as f:
        if f.read(4) != b"PACK":
            print("FAIL: %s does not start with PACK" % pak)
            return 1

    if os.path.getsize(pak) != PAK_SIZE:
        print("note: %s is %d bytes, not the shareware 1.06 pak's %d"
              % (pak, os.path.getsize(pak), PAK_SIZE))

    disk_with(pak)

    # Read by run_screenshot when it is imported, so it is set first.
    os.environ["KOSMOS_DISK"] = DISK
    sys.path.insert(0, HERE)
    from run_screenshot import Guest, Failure, PROMPT, parse_ppm   # noqa: E402

    guest = Guest(image, 600)
    checks = 0
    mark = 0

    try:
        guest.wait_for(PROMPT, "reached a shell")
        mark = len(guest.seen)
        guest.type("wm quake")

        if not said_after(guest, mark, "wm: window Quake at", 90):
            raise Failure("Quake opened no window:\n" + guest.seen[mark:][-1500:])
        checks += 1

        if not said_after(guest, mark, "Quake Initialized", 180):
            raise Failure("the engine did not start:\n" + guest.seen[mark:][-1500:])
        checks += 1

        if not (said_after(guest, mark, "Playing demo from demo1.dem", 120)
                and said_after(guest, mark, "maps/e1m3.bsp", 180)):
            raise Failure("no demo, or not its map:\n" + guest.seen[mark:][-1500:])
        checks += 1

        soak(guest, 20)

        where = re.search(r"wm: window Quake at (\d+),(\d+) (\d+)x(\d+)",
                          guest.seen[mark:])
        x, y, w, h = (int(v) for v in where.groups())
        seen = colours_in(parse_ppm, guest.screendump(), x, y, w, h)

        if seen <= 32:
            raise Failure("the window holds %d colours - it is not drawing "
                          "the game" % seen)
        checks += 1

        # Quake's console, a command, and the answer. The line is echoed
        # when it is entered, and `echo` prints it again when it runs.
        before = guest.seen.replace("\r", "").replace("\n", "").count(MARKER)
        keys(guest, "`")
        soak(guest, 3)
        keys(guest, "echo " + MARKER)
        guest.sendkey("ret")

        end = time.monotonic() + 60
        count = before
        while time.monotonic() < end and count < before + 2:
            soak(guest, 1)
            count = guest.seen.replace("\r", "").replace("\n", "").count(MARKER)

        if count < before + 2:
            raise Failure("a command typed at Quake's console did not run "
                          "(%d of 2 appeared)" % (count - before))
        checks += 1

        # Control-C closes the window, and the process ends without a fault.
        end_mark = len(guest.seen)
        guest.sendkey("ctrl-c")

        if not said_after(guest, end_mark, "(quake) ended", 30):
            raise Failure("Control-C did not close Quake:\n"
                          + guest.seen[end_mark:][-800:])

        if "died:" in guest.seen[mark:]:
            raise Failure("the process faulted on the way:\n"
                          + guest.seen[mark:][-1500:])
        checks += 1
    except Failure as e:
        print("FAIL: %s" % e)
        return 1
    finally:
        guest.close()

    print("PASS: %d checks on Quake (a window, the engine started, a demo and "
          "its map, the game drawn, a console command run, and closed "
          "without a fault)." % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
