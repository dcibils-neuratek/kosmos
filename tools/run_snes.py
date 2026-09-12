#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Super Nintendo on the machine: a ROM from the drive, frames, and the pad.

A ROM read from `/home/roms/snes` into a region, the core loaded, and a window
drawing the game. And the number this port was started to find out: how many
frames a second the core manages under QEMU's TCG, which `snes.lua` reports
every ten seconds. Enter is pressed to get past title screens, and whether
it reached the pad is not checked - the saved pictures show it.

**The ROM is not in the repository**, so this needs one:
`make snes-check ROM=/path/to/game.sfc`. It goes on a disk image of its own
under `build/snes/`, never on `build/kosmos.img`.

The picture is saved as `build/snes/screen.png` and the window alone as
`build/snes/window.png`, because a frame rate says nothing about whether the
frames are right.

Usage: run_snes.py [image] rom [seconds]
"""

import os
import re
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "build", "snes")
DISK = os.path.join(OUT, "snes.img")
LUA = os.path.join(ROOT, "build", "host", "lua")

REPORT = re.compile(r"snes: ([\d.]+) frames a second of ([\d.]+), "
                    r"([\d.]+) ms emulating each")


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


def png(path, w, h, rgb):
    """RGB bytes to a PNG, with nothing but zlib."""
    rows = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(rows, 6))
                + chunk(b"IEND", b""))


def crop(sw, rgb, x, y, w, h):
    return b"".join(rgb[(row * sw + x) * 3:(row * sw + x + w) * 3]
                    for row in range(y, y + h))


def colours(rgb):
    return len({rgb[i:i + 3] for i in range(0, len(rgb), 3 * 7)})


def main():
    args = sys.argv[1:]
    image = args.pop(0) if args and args[0].endswith(".elf") else "build/kosmos.elf"
    rom = args[0] if args else os.environ.get("ROM", "")
    seconds = int(args[1]) if len(args) > 1 else 45

    if not rom or not os.path.isfile(rom):
        print("FAIL: no ROM. make snes-check ROM=/path/to/game.sfc - a ROM of "
              "your own, which is not in the repository.")
        return 1

    # `kfs.lua create` splits `host:guest` at the first colon.
    if ":" in rom:
        print("FAIL: %s has a colon in it, which kfs.lua would read as the end "
              "of the host path - copy the ROM somewhere without one" % rom)
        return 1

    if os.path.getsize(rom) < 0x8000:
        print("FAIL: %s is too small to be a ROM" % rom)
        return 1

    os.makedirs(OUT, exist_ok=True)
    if os.path.exists(DISK):
        os.remove(DISK)

    name = os.path.basename(rom)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", DISK, "64",
                    rom + ":/home/roms/snes/" + name], check=True)

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
        guest.type("wm snes")

        if not said_after(guest, mark, "snes: /home/roms/snes/", 120):
            raise Failure("the ROM did not start:\n" + guest.seen[mark:][-1500:])
        checks += 1

        where = None
        end = time.monotonic() + 60
        while where is None and time.monotonic() < end:
            soak(guest, 1)
            where = re.search(r"wm: window (.+?) at (\d+),(\d+) (\d+)x(\d+)",
                              guest.seen[mark:])

        if where is None:
            raise Failure("no window:\n" + guest.seen[mark:][-1500:])
        checks += 1

        # Past the title screens: Start, a few times, with time between.
        for _ in range(4):
            soak(guest, 5)
            guest.sendkey("ret")

        soak(guest, seconds)

        reports = REPORT.findall(guest.seen[mark:])

        if not reports:
            raise Failure("the loop never reported its rate:\n"
                          + guest.seen[mark:][-1500:])
        checks += 1

        sw, sh, rgb = parse_ppm(guest.screendump())
        png(os.path.join(OUT, "screen.png"), sw, sh, rgb)

        x, y, w, h = (int(v) for v in where.groups()[1:])
        w, h = min(w, sw - x), min(h, sh - y)
        window = crop(sw, rgb, x, y, w, h)
        png(os.path.join(OUT, "window.png"), w, h, window)

        seen = colours(window)
        if seen <= 8:
            raise Failure("the window holds %d colours - it is not drawing "
                          "the game" % seen)
        checks += 1

        if "died:" in guest.seen[mark:]:
            raise Failure("the process faulted:\n" + guest.seen[mark:][-1500:])
        checks += 1
    except Failure as e:
        print("FAIL: %s" % e)
        return 1
    finally:
        guest.close()

    for fps, target, ms in reports:
        print("  %5s frames a second of %s, %s ms emulating each" % (fps, target, ms))

    print("PASS: %d checks on the Super Nintendo (the ROM started, a window, "
          "the rate reported, the game drawn in %d colours, no fault). "
          "Pictures in build/snes/." % (checks, seen))
    return 0


if __name__ == "__main__":
    sys.exit(main())
