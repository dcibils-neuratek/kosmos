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
import run_screenshot                                         # noqa: E402

# **A gigabyte**, not the suites' 512 MB. Twenty applications at once -
# Cafesa3D, Maps, the IDE and a full-screen Groove among them - did not fit
# in half that: on 12 October Groove's window was refused and Maps ended
# while the picture was being set up. The picture is of the system on a
# machine it fits on; the suites keep the small machine, where running out
# is what they look for.
for _args in (run_screenshot.QEMU_ARGS, run_screenshot.X86_ARGS):
    if "-m" in _args:
        _args[_args.index("-m") + 1] = "1G"

# And four processors on x86, as the ARM board has: twenty applications
# starting at once on one emulated core did not all open in four minutes.
if "-smp" not in run_screenshot.X86_ARGS:
    run_screenshot.X86_ARGS += ["-smp", "4"]

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
#
# **More of the system than the same five.** Diego, 19 September: "We need
# to start showing other apps in the screenshots as well". The picture had
# Tracker, the widgets, Processes, Monitor and the cube since the start -
# and two of three of them twice, because the Deskbar also opens its login
# set, which is emptied first now. Music, Appearance, a Terminal, the
# Calculator, the gears and This Machine join them: each draws something of
# its own with nothing on a disk, which is what a diskless guest has.
#
#
# **And each new application joins it.** Diego, 25 September: "The dated
# screenshot now needs to have Cafesa3D in it", "So as we are new apps we
# improve the screenshots". Cafesa3D opens on its Rendered view - the scene
# ray traced - and `tile` gives a window that size two cells by two.
#
#
# **And Kosmos IDE, the day it opened** (`roadmap.md` 6n): on `/Kosmos/Libraries/files.lua`,
# whose code starts on its thirteenth line, so the picture shows Lua in the
# IDE's colours rather than a page of comments.
#
#
# **And Text Editor** (`roadmap.md` 6zs), on a Markdown note written into
# `/Temporary` first, since nothing Markdown ships in the image: a heading,
# bold, and a checklist with one box ticked.
#
#
# **And Groove** (`roadmap.md` 6zh), playing its techno demo: the second big
# window, so `tile` gives it two cells by two as it gives Cafesa3D, and the
# corner that shows is PulseMusic's top bar over the first tracks' clips.
#
#
# **And Servers** (`roadmap.md`, remote step 6), on its first page: the web
# server, the command line and the screen, each with its switch - all
# stopped here, since the picture's machine has no network to serve.
#
#
# **And Kosmos Write** (`docs/write.md`), on a page made in `/Temporary`
# first by a program the prompt writes and runs - `use` is a program's, not
# the prompt's: a title, a table and a chart.
#
OPEN = ["tracker", "gallery", "music", "preferences", "terminal", "calc",
        "glgears", "machine", "procs", "sysmon", "netmon", "diskmon", "cube3d",
        "cafesa3d:--rendered", "ide:/Kosmos/Libraries/files.lua",
        "texteditor:/Temporary/Grooves.md", "groove:--play", "servers",
        "writer:/Temporary/Gallery.write", "passwords", "maps", "sandboxes",
        "automations"]

# `tile` last, told how many windows to wait for: sixteen applications
# starting at once under QEMU - twenty-two now - do not all open inside the three still
# seconds it otherwise takes for "done", and on 28 September Groove,
# maximised, opened after the rest were arranged and lay over all of them.
OPEN.append("tile:%d" % len(OPEN))

# The program that makes Write's page, on one line: what is typed is
# entered at its end.
WRITE = ('local wd = use("/Kosmos/Libraries/writedoc.lua") '
         'local rt = use("/Kosmos/Libraries/richtext.lua") '
         'local d = wd.new() '
         'local function c(t) return { style = "Body", runs = { { text = t } } } end '
         'd.body = { { style = "Title", runs = { { text = "Kosmos Write" } } }, '
         'c("Pages as they print: text set by styles, tables, shapes and charts."), '
         '{ style = "Body", table = { columns = 3, header = true, rows = { '
         '{ c("Planet"), c("Moons"), c("Day") }, { c("Mars"), c("2"), c("24.6 h") }, '
         '{ c("Venus"), c("0"), c("2802 h") } } } }, rt.new_chart("column", "Body") } '
         'print("write" .. "-made", wd.save("/Temporary/Gallery.write", d))')

NOTE = ("# Grooves\\nA drum machine for Kosmos, with **swing** per track "
        "and a pattern saved as `pattern-01.grv`.\\n\\n## Next\\n"
        "- [x] Draw the grid\\n- [ ] Swing\\n- [ ] Export the loop\\n")


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

        # The Deskbar opens its login set as well, and that is what put
        # Tracker, Monitor and Processes in the picture twice: emptied here,
        # as the display harness empties it.
        guest.type('fs.write("/Home/Preferences/startup", { items = {} }) '
                   'fs.write("/Temporary/Grooves.md", "' + NOTE + '") '
                   'print("gallery" .. "-ready")')
        guest.wait_for("gallery-ready", "emptied the login set")
        guest.type('fs.write("/Temporary/mkwrite.lua", [[' + WRITE + ']]) '
                   'print("write" .. "-ready")')
        guest.wait_for("write-ready", "written Write's program")
        guest.type("run /Temporary/mkwrite.lua")
        guest.wait_for("write-made", "made Write's page")
        guest.type("wm deskbar," + names)

        #
        # **Until `tile` has arranged them**, and then a still moment for
        # every application to draw its first frame. This was forty seconds
        # alone, while `tile` waits up to two minutes for all of them to
        # open: with twenty under QEMU the picture was taken before anything
        # was arranged, Groove maximised over the rest, on 11 and 12 October.
        # What each application looks like when it is finished is still not
        # polled for - six checks that go stale independently - so the settle
        # stays, after the arrangement rather than instead of it.
        #
        deadline = time.monotonic() + 300

        while "tile: " not in guest.seen and time.monotonic() < deadline:
            time.sleep(1.0)
            guest._read_available()

        arranged = [int(l.split()[1]) for l in guest.seen.splitlines()
                    if l.startswith("tile: ") and l.split()[1].isdigit()]

        if not arranged or arranged[-1] < len(OPEN) - 1:
            said = [l for l in guest.seen.splitlines() if l.startswith("tile: ")]
            opened = [l.split(" at ")[0][len("wm: window "):] for l in guest.seen.splitlines()
                      if l.startswith("wm: window ") and " at " in l]
            trouble = [l for l in guest.seen.splitlines()
                       if "error" in l.lower() or "not enough" in l.lower() or "died" in l.lower()
                       or l.startswith("maps") or l.startswith("writer") or "(maps)" in l or "(writer)" in l]
            print("opened: %s\ntrouble: %s" % (opened, trouble[-12:]), file=sys.stderr)
            raise Failure("tile did not arrange all %d windows: %s"
                          % (len(OPEN) - 1, said or "it said nothing in five minutes"))

        time.sleep(args.settle)
        guest._read_available()

        # **And none of them ended** on the way. `tile` counts windows, and
        # on 12 October it counted twenty while Cafesa3D and Groove had
        # opened and gone: a picture without them passed for one with them.
        import re as _re
        apps = {o.split(":")[0] for o in OPEN if not o.startswith("tile")}
        gone = sorted({m for m in _re.findall(r"process \d+ \(([^)]+)\) ended", guest.seen)
                       if m in apps})

        if gone:
            said = [l for l in guest.seen.splitlines()
                    if any(l.startswith(g + ":") or "(" + g + ")" in l for g in gone)]
            print("\n".join(said[-30:]), file=sys.stderr)

            # And what came just before each ended: an error a program does
            # not prefix with its name is printed by the shell for it.
            lines = guest.seen.splitlines()

            for g in gone:
                at = max(i for i, l in enumerate(lines) if "(" + g + ") ended" in l)
                print("--- before %s ended ---\n%s" % (g, "\n".join(lines[max(0, at - 25):at + 1])),
                      file=sys.stderr)

            raise Failure("these ended before the picture: %s" % ", ".join(gone))

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
