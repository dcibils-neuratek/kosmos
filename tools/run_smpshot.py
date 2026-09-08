#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""One picture of four processors with work on all of them.

`run_gallery.py` takes the desktop's portrait; this takes the one picture
that could not be taken before `docs/smp.md` step five, and it is a
different picture for one reason: **it has to be doing something.** A
desktop at rest on four cores looks exactly like a desktop at rest on one,
so a screenshot of an idle machine proves nothing at all about SMP no
matter how many bars are drawn on it.

So the scene is chosen to be load rather than to be pretty. Three
software-rendered 3D demos and an animating cube, a browser laying out a
page, and a PDF being scanned and painted - six processes that all want a
processor at the same instant, with the Monitor's four bars and the
Processes list's `processor` column beside them saying where each one
actually ran.

**Nothing here is a spinner.** `spin` would light every bar in one line and
would be a picture of a benchmark rather than of a system; every process in
this shot is doing work somebody would plausibly ask it to do. The bars are
what they are, and if they read low that is a result rather than something
to arrange around.

The boot option is the whole point:

    KOSMOS_SMPWORK=4    the kernel places new threads on all four cores
    (unset)             every thread is homed on core zero, the default

Run it both ways and the difference between the two pictures is the
feature. `Makefile`'s `smpshot` target does the first.

Usage: run_smpshot.py <image> --out <file.png>
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

#
# Both before the import, because `run_screenshot` builds its QEMU argument
# list at module scope: setting them afterwards would set them for nobody.
#
os.environ.setdefault("KOSMOS_SMPWORK", "4")
os.environ.setdefault("KOSMOS_DISK", "build/smpshot.img")

from run_screenshot import Guest, Failure, PROMPT, parse_ppm   # noqa: E402
from run_gallery import png, ABS                               # noqa: E402

#
# What to open, and why each one is here.
#
# `sysmon` is the evidence and goes first so it is never the window that
# failed to start. `procs` is the second half of the same evidence: the
# Monitor says how busy each processor is and the process list says which
# processor each program is on, and neither of those alone is the claim.
#
# Then the load. The three GL demos and the cube are software renderers -
# `gfx.md` is clear that no pixel loop is in Lua, so what they burn is C in
# a process, which is exactly the shape of work SMP is supposed to help.
# The browser lays out and paints a real page; the PDF viewer runs the
# scanner that `CLAUDE.md` measures at 4.7 ms a page in C.
#
# `tile` last, because it arranges whatever it finds and finding all of
# them means being started after all of them.
#
# `procs` is late on purpose. A window that opens later is stacked on top,
# and its rightmost column - `processor` - is the one thing here that says
# which core each program actually ran on. Opened early it went under the
# gears, and the column that carried the evidence was the part covered up.
OPEN = [
    "sysmon",
    "cube3d",
    "glgears",
    "glteapot",
    "glmorph3d",
    "browser:/home/welcome.html",
    "pdfview:/home/odyssey.pdf",
    "procs",
    "tile",
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--out", required=True)
    ap.add_argument("--timeout", type=int, default=420)
    ap.add_argument("--settle", type=int, default=75,
                    help="seconds to let every application draw and start working")
    ap.add_argument("--open", default=",".join(OPEN),
                    help="comma-separated app list handed to wm")
    ap.add_argument("--samples", type=int, default=1,
                    help="how many pictures to take, a few seconds apart")
    ap.add_argument("--gap", type=float, default=8.0,
                    help="seconds between samples")
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

        #
        # Said out loud in the log, because a picture of four bars taken on a
        # machine that was placing on one would be the most convincing wrong
        # screenshot this project could produce.
        #
        placing = os.environ.get("KOSMOS_SMPWORK") or "1 (default)"
        print("placing across: %s" % placing)

        guest.type("wm deskbar," + args.open)

        #
        # One long wait, for `run_gallery`'s reason and one more of its own:
        # these applications are not finished when they have drawn. The
        # browser has to fetch, parse and lay out; the PDF viewer has to scan
        # a page before it has anything to paint; and the demos have to get
        # far enough into their animation that the load meters have two
        # readings to subtract. A poll would have to know what six different
        # programs look like when they are busy.
        #
        time.sleep(args.settle)
        guest._read_available()

        # Out of the way, so the arrow is not sitting on a window in the
        # picture that goes into the repository.
        guest.mouse_to((width - 30) * ABS // width, (height - 30) * ABS // height)
        time.sleep(2.0)

        os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)

        #
        # More than one picture, seconds apart, and it is not cherry-picking.
        #
        # A load meter is a *sample*: it subtracts two readings a second
        # apart and says what happened between them. A processor whose
        # threads all happened to be waiting on the window manager at that
        # instant reads near zero while being perfectly busy either side of
        # it - so a single frame of four bars says less about the machine
        # than about the moment the button was pressed.
        #
        # Several frames of the same running desktop is what a person
        # watching the meter for ten seconds would see, and picking the
        # representative one from those is what they would report. What
        # would be dishonest is changing the *scene* until the bars look
        # right; the scene here is fixed before the first shot.
        #
        for n in range(args.samples):
            if n:
                time.sleep(args.gap)
                guest._read_available()

            w_, h_, rgb = parse_ppm(guest.screendump())

            out = args.out

            if args.samples > 1:
                stem, dot, ext = args.out.rpartition(".")
                out = "%s-%d.%s" % (stem or args.out, n + 1, ext or "png")

            with open(out, "wb") as f:
                f.write(png(w_, h_, rgb))

            print("wrote %s (%dx%d)" % (out, w_, h_))

    except Failure as why:
        print("\nFAIL: %s" % why, file=sys.stderr)
        return 1
    finally:
        guest.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
