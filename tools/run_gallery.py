#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""One picture of the whole desktop, for the record.

Every other runner here checks something. This one checks nothing: it boots
the machine, opens the applications that show what the system can do, lays
them out so all of them are visible at once, and saves the screen.

The point is the *series*. A repository full of these, one per push, is the
only honest account of what a desktop looked like on a given day - a commit
message says what changed and a screenshot says what it became. Nothing else
in this project records that, and the thing being built is a thing you look
at.

They go in `docs/screenshots/`, because that is what they are. `build/` is
gitignored and `make clean` deletes it, so a history kept there would vanish
the first time anybody cleaned, and `builds/` is for things you can run.

Usage: run_gallery.py <image> --out <file.png> [--geometry 1920x1080]
"""

import argparse
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from run_screenshot import Guest, Failure, PROMPT, parse_ppm   # noqa: E402

# The tablet's own range, which is what `mouse_to` speaks.
ABS = 32767

#
# Where each window goes, and the order they are dragged.
#
# Laid out by hand rather than tiled, because a tiled screenshot looks like
# a contact sheet and a desktop does not. They overlap a little, the way a
# desktop that is being used does, and every one of them stays readable.
#
# The numbers are the *content* origin - what `move` takes - and each entry
# carries the size so the drag can aim at the middle of a title bar rather
# than at a corner that may be off screen.
#
#
# What to open, in the order they should cascade down the screen.
#
# There is no dragging here any more, and that is the whole story of this
# file. Three versions of it tried to lay the windows out by picking them up
# by their title bars, and each failed differently: applications carry a
# position in their source and several of them chose the same corner, so
# they opened exactly on top of one another. Which one a grab reached
# depended on the stacking order, and the stacking order is a *race* - the
# Deskbar's own window arrived fifth of six despite being started first.
#
# The fix was in two parts, and neither of them is here. A window manager
# should not stack windows invisibly, so `wm` cascades them; and arranging
# windows is the *system's* job, so `tile` does it from inside, over the
# protocol, by handle. A screenshot tool that had to know about z-order
# stopped needing to know anything at all.
#
OPEN = ["tracker", "gallery", "procs", "sysmon", "cube3d", "tile"]

TAB_H = 20          # has to agree with wm.lua
BORDER = 2


def png(width, height, rgb):
    """A PPM's pixels as a PNG, without a library."""
    raw = bytearray()

    for y in range(height):
        raw.append(0)                       # filter: none
        raw += rgb[y * width * 3:(y + 1) * width * 3]

    def chunk(tag, body):
        head = struct.pack(">I", len(body)) + tag
        return head + body + struct.pack(">I", zlib.crc32(tag + body))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--out", required=True)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--settle", type=int, default=40,
                    help="seconds to let every application draw its first frame")
    args = ap.parse_args()

    guest = Guest(args.image, args.timeout)

    try:
        guest.wait_for(PROMPT, "reached a shell")

        width, height = 0, 0

        for line in guest.seen.splitlines():
            if ", 32-bit" in line:
                got = line.split(",")[0].split()[-1]

                if "x" in got:
                    width, height = (int(v) for v in got.split("x"))

        if not width:
            raise Failure("the boot never announced a display geometry.")

        names = ",".join(OPEN)

        guest.type("wm deskbar," + names)

        #
        # One long wait rather than a poll. Every application here draws
        # something different on its first pass - Tracker reads a directory,
        # the cube builds a mesh - and a poll would have to know what each of
        # them looks like when it is finished, which is six checks that go
        # stale independently.
        #
        time.sleep(args.settle)
        guest._read_available()

        # Out of the way, so the arrow is not sitting on top of a window in
        # the picture that goes in the repository.
        guest.mouse_to((width - 30) * ABS // width, (height - 30) * ABS // height)
        time.sleep(2.0)

        w_, h_, rgb = parse_ppm(guest.screendump())

        os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)

        with open(args.out, "wb") as f:
            f.write(png(w_, h_, rgb))

        print("wrote %s (%dx%d)" % (args.out, w_, h_))

    except Failure as why:
        print("\nFAIL: %s" % why, file=sys.stderr)
        return 1
    finally:
        guest.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
