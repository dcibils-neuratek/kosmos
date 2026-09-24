#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Camera app records, on a machine with a disk (`roadmap.md` 6d 8f).

The display harness's machines have no disk - `/home` is in memory there, and
a recording is kept whole in a region and written with one `write_from`,
which only kfs takes - so this boots its own, with a scratch disk the machine
formats itself and the driver's test pattern for a camera.

The Camera app opens on the pattern; R starts a recording and R stops it,
four seconds later; the app says how many frames and bytes it kept and where.
Then, at the prompt, the file is asked for: that size, in `/home/videos`, and
read by the video player's own MP4 reader - one H.264 track, the pattern's
size, as many samples as frames, the first a key frame.

Usage: run_record.py IMAGE
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scratch                                              # noqa: E402

DISK = scratch.disk("record.img", 64 * 1024 * 1024)
os.environ["KOSMOS_DISK"] = DISK

import run_screenshot as R                                  # noqa: E402

# The file read at the prompt with `/lib/mp4.lua`, which is how Video will
# open one: `use` is a program's, so the library is loaded as a chunk.
READ_BACK = (
    'local mp4 = load(fs.read("/lib/mp4.lua"))() '
    'local path = "%s" local a = fs.getattr(path) or {} '
    'local d = fs.read(path) or "" '
    'local f = #d > 0 and mp4.open(function(at, n) '
    'return d:sub(at + 1, at + n) end, #d) or {} '
    'local t = (f.tracks or {})[1] or {} '
    'print("MP4", a.size, #(f.tracks or {}), t.kind, t.codec, t.width, '
    't.height, #(t.samples or {}), t.samples and t.samples[1] '
    'and t.samples[1].key) print("MP4" .. "-READ")')


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    checks, fails = 0, []

    def check(ok, complaint):
        nonlocal checks
        if ok:
            checks += 1
        else:
            fails.append(complaint)

    guest = R.Guest(image, 180)
    frames = size = 0

    try:
        guest.wait_for("kosmos>", "a shell prompt")
        mark = len(guest.seen)
        guest.type("wm camera")
        guest.wait_for_line("camera: Test pattern at ", "the Camera app",
                            mark)
        time.sleep(2)

        guest.sendkey("r")
        started = guest.wait_for_line("camera: recording to ",
                                      "a recording to start", mark)
        check(started.startswith("/home/videos/")
              and started.endswith(".mp4"),
              "the recording is not going into /home/videos as an MP4: %r"
              % started)

        time.sleep(4)
        guest.sendkey("r")
        kept = guest.wait_for_line("camera: recorded ",
                                   "the recording to be kept", mark)
        said = re.match(r"(\d+) frames, (\d+) bytes to (.+)$", kept)
        check(said is not None, "the app did not say what it kept: %r" % kept)

        frames = int(said.group(1)) if said else 0
        size = int(said.group(2)) if said else 0
        path = said.group(3) if said else ""

        # Four seconds of a pattern: more than a few frames under TCG, and a
        # file with something in it.
        check(frames >= 5 and size > 1000,
              "four seconds recorded %d frames in %d bytes" % (frames, size))
        check(path == started, "it was kept at %r, not where it began, %r"
              % (path, started))

        R.stop_desktop(guest)
        mark = len(guest.seen)
        guest.type(READ_BACK % path)
        guest.wait_for("MP4-READ", "the recording read back")
        row = re.search(r"^MP4\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+"
                        r"(\S+)\s+(\S+)\s+(\S+)", guest.seen[mark:], re.M)
        got = row.groups() if row else None

        check(got is not None and got[0] == str(size),
              "the file in /home/videos is not the %d bytes the app kept: %r"
              % (size, got))
        check(got is not None and got[1:6] == ("1", "video", "avc1", "640",
                                               "480"),
              "the video player's reader did not find one H.264 track of "
              "640x480: %r" % (got,))
        check(got is not None and got[6] == str(frames) and got[7] == "true",
              "it did not find %d samples with the first a key frame: %r"
              % (frames, got))
    except R.Failure as e:
        fails.append(str(e))
    finally:
        guest.close()
        os.unlink(DISK)

    if fails:
        print("FAIL: %d of %d checks on recording the camera:"
              % (len(fails), len(fails) + checks))
        for complaint in fails:
            print("  " + complaint)
        return 1

    print("PASS: %d checks on recording the camera (R, four seconds, R: %d "
          "frames in %d bytes kept in /home/videos, and read back as one "
          "H.264 track of the pattern's size and frames)"
          % (checks, frames, size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
