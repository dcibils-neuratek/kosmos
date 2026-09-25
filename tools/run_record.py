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
size, as many samples as frames, the first a key frame. And played
(`roadmap.md` 4e): every frame decoded by FFmpeg through `/lib/video.lua`,
each coming out as the frame asked for, and the pattern's eight bars read
back off the picture in their colours.

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

# And the recording played: every frame decoded through `/lib/video.lua`, as
# the Video app decodes it - the MP4 reader, the H.264 Kit, FFmpeg and the
# conversion onto a surface - counting the frames that came out as the one
# asked for, and the eight bars' colours read off the last, a quarter of the
# way down and in the middle of each bar. A program in /ramfs rather than a
# line at the prompt, because a program has `use`; in a `[=[` string,
# because the program says `samples[order[i]]` and `]]` would end a `[[`.
DECODE = (
    'fs.write("/ramfs/decode.lua", [=[local video = use("/lib/video.lua") '
    'local f, why = video.open(args) '
    'if not f then print("DECODE-ERR " .. tostring(why)) '
    'print("DECODE" .. "-DONE") return end '
    'local samples, order = f.track.samples, f.order '
    'local exact, began = 0, sys.ticks() '
    'for i = 1, f.frames do local want = samples[order[i]].pts '
    'for _ = 1, 50 do f:frame(i) '
    'if f.shown_pts == want then exact = exact + 1 break end end end '
    'local hz = (fs.read("/dev/cpu") or {}).counter_hz or 1 '
    'local bars = {} for b = 0, 7 do bars[#bars + 1] = ("%06x"):format('
    'f.picture:get(((2 * b + 1) * f.width) // 16, f.height // 4) & 0xffffff) '
    'end print(("DECODE %s %d %d %.1f %s"):format(f.codec:gsub(" ", ""), '
    'exact, f.frames, (sys.ticks() - began) * 1000 / hz / '
    'math.max(1, f.frames), table.concat(bars, ","))) '
    'print("DECODE" .. "-DONE") f:close()]=])')

# The pattern's bars (`pattern_draw` in xhci.c) are 100% colours: white,
# yellow, cyan, green, magenta, red, blue, black. What comes back has been
# through 4:2:0 and a lossy encoder, so each channel is held within a margin
# rather than exactly. They came back within one step on both boards the
# first time; eight is room for an encoder's change of mind, and tight
# enough that the wrong matrix fails - BT.709 puts yellow's green fifteen
# steps off - and so does the wrong range, which leaves white at 235.
BARS = [0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000,
        0x0000ff, 0x000000]
MARGIN = 8


def near(got, want):
    return all(abs(((got >> s) & 0xff) - ((want >> s) & 0xff)) <= MARGIN
               for s in (0, 8, 16))


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
    decode_ms = 0.0
    bars_seen = []

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

        mark = len(guest.seen)
        guest.type(DECODE)
        guest.type("/ramfs/decode.lua " + path)
        guest.wait_for("DECODE-DONE", "the recording decoded")
        said = guest.seen[mark:]
        row = re.search(r"^DECODE (\S+) (\d+) (\d+) ([\d.]+) (\S+)", said,
                        re.M)
        check(row is not None,
              "the recording did not decode: %r" % said[-400:])

        if row:
            codec, exact, total = row.group(1), int(row.group(2)), \
                int(row.group(3))
            decode_ms = float(row.group(4))
            bars = [int(b, 16) for b in row.group(5).split(",")]
            bars_seen = bars

            check(codec == "H.264", "Video decoded it as %r" % codec)
            check(total == frames and exact == frames,
                  "%d of %d frames came out as the frame asked for"
                  % (exact, total))
            check(len(bars) == 8 and all(near(g, w)
                                         for g, w in zip(bars, BARS)),
                  "the bars came back %s, and the pattern's are %s"
                  % (",".join("%06x" % b for b in bars),
                     ",".join("%06x" % b for b in BARS)))
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
          "frames in %d bytes kept in /home/videos, read back as one H.264 "
          "track of the pattern's size and frames, and played: every frame "
          "decoded by FFmpeg through /lib/video.lua, %.1f ms each under "
          "QEMU, and the eight bars their colours: %s)"
          % (checks, frames, size, decode_ms,
             ",".join("%06x" % b for b in bars_seen)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
