#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Camera app records, on a machine with a disk (`roadmap.md` 6d 8f).

The display harness's machines have no disk - `/Home` is in memory there, and
a recording is kept whole in a region and written with one `write_from`,
which only kfs takes - so this boots its own, with a scratch disk and the
driver's test pattern for a camera.

The Camera app opens on the pattern; R starts a recording and R stops it,
four seconds later; the app says how many frames and bytes it kept and where.
Then, at the prompt, the file is asked for: that size, in `/Home/Captures`, and
read by the video player's own MP4 reader - one H.264 track, the pattern's
size, as many samples as frames, the first a key frame. And played
(`roadmap.md` 4e): every frame decoded by FFmpeg through `/Kosmos/Libraries/video.lua`,
each coming out as the frame asked for, and the pattern's eight bars read
back off the picture in their colours.

Since M6 (`docs/media.md`) a film plays in `media.elf`: the scratch disk
is made here, formatted, with `media.elf` on it as the install puts it
(`installed.film_pairs`), from the test userland's APPS.

Usage: run_record.py IMAGE APPS
"""

import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scratch                                              # noqa: E402
import installed                                            # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DISK = scratch.disk("record.img", 64 * 1024 * 1024)
os.environ["KOSMOS_DISK"] = DISK

import run_screenshot as R                                  # noqa: E402

# The file read back through the Media Kit's door (`/Kosmos/Kits/media`),
# which is how Video opens one since 0.12.96 (`docs/media.md` M6c): its
# streams, and every packet of the first counted, the first a key frame. A
# program in /Temporary naming `media.elf`, as DECODE below, since the kit
# is in no other image. It read the file with `/Kosmos/Libraries/mp4.lua`
# until M6d removed that reader: a writer and a reader that agree only
# with themselves would not be a file format, so the reader held here is
# the one that plays it.
READ_BACK = (
    'fs.write("/Temporary/readback.lua", "-- kosmos: image media\\n" .. '
    '[=[local media = use("/Kosmos/Kits/media") local path = args '
    'local a = fs.getattr(path) or {} '
    'local src, why = media.open(path) '
    'local list = src and src:streams() or {} local t = list[1] or {} '
    'local n, key = 0, nil '
    'while src do local p = src:read() if not p then break end '
    'if p.stream == 0 then n = n + 1 if key == nil then key = p.key end end '
    'p:free() end '
    'print("MP4", a.size, #list, t.kind, t.codec, t.width, t.height, n, key) '
    'print("MP4" .. "-READ")]=])')

# And the recording played: every frame decoded through `/Kosmos/Libraries/video.lua`, as
# the Video app decodes it - the Media Kit's demuxer and FFmpeg's H.264
# decoder in `media.elf` (`docs/media.md` M6c), and the picture onto a
# surface - with the film's time walked from its start to past its end a
# quarter of a frame at a time, every distinct picture shown counted, and
# the eight bars' colours read off the last, a quarter of the way down and in
# the middle of each bar. A program in /Temporary naming the image, in a
# `[=[` string, its header line put in front with a `\n` the prompt does not
# see as a line's end.
DECODE = (
    'fs.write("/Temporary/decode.lua", "-- kosmos: image media\\n" .. '
    '[=[local video = use("/Kosmos/Libraries/video.lua") '
    'local f, why = video.open(args) '
    'if not f then print("DECODE-ERR " .. tostring(why)) '
    'print("DECODE" .. "-DONE") return end '
    'local step = 1 / (4 * math.max(1, f.fps)) '
    'local t, seen, last, began = 0, 0, nil, sys.ticks() '
    'while t <= f.duration + 1 do '
    'for _ = 1, 20 do f:picture_at(t) '
    'if f.pending_pts or (f.shown_pts and f.shown_pts >= t * 1e6) then break end end '
    'if f.shown_pts and f.shown_pts ~= last then seen = seen + 1 last = f.shown_pts end '
    't = t + step end '
    'local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 1 '
    'local bars = {} for b = 0, 7 do bars[#bars + 1] = ("%06x"):format('
    'f.picture:get(((2 * b + 1) * f.width) // 16, f.height // 4) & 0xffffff) '
    'end print(("DECODE %s %d %.1f %s"):format(f.codec:gsub(" ", ""), '
    'seen, (sys.ticks() - began) * 1000 / hz / '
    'math.max(1, seen), table.concat(bars, ","))) '
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
    arch = "x86_64" if "x86_64" in image else "aarch64"
    apps = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        ROOT, "build", "user-x86_64-test" if arch == "x86_64" else "user-test", "apps")
    pairs = installed.film_pairs(arch, apps)
    checks, fails = 0, []

    if not any(p.endswith("/media.elf") for p in pairs):
        print("FAIL: no media.elf in %s - `make apps` builds it" % apps)
        return 1

    size = sum(os.path.getsize(p.split(":", 1)[0]) for p in pairs) // (1024 * 1024)
    subprocess.run([os.path.join(ROOT, "build", "host", "lua"),
                    os.path.join(HERE, "kfs.lua"), "create", DISK, str(size + 64)] + pairs,
                   check=True, capture_output=True, cwd=ROOT)

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
        check(started.startswith("/Home/Captures/")
              and started.endswith(".mp4"),
              "the recording is not going into /Home/Captures as an MP4: %r"
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
        guest.type(READ_BACK)
        guest.type("/Temporary/readback.lua " + path)
        guest.wait_for("MP4-READ", "the recording read back")
        row = re.search(r"^MP4\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+"
                        r"(\S+)\s+(\S+)\s+(\S+)", guest.seen[mark:], re.M)
        got = row.groups() if row else None

        check(got is not None and got[0] == str(size),
              "the file in /Home/Captures is not the %d bytes the app kept: %r"
              % (size, got))
        check(got is not None and got[1:6] == ("1", "video", "h264", "640",
                                               "480"),
              "the Media Kit did not find one H.264 stream of "
              "640x480: %r" % (got,))
        check(got is not None and got[6] == str(frames) and got[7] == "true",
              "it did not find %d samples with the first a key frame: %r"
              % (frames, got))

        mark = len(guest.seen)
        guest.type(DECODE)
        guest.type("/Temporary/decode.lua " + path)
        guest.wait_for("DECODE-DONE", "the recording decoded")
        said = guest.seen[mark:]
        row = re.search(r"^DECODE (\S+) (\d+) ([\d.]+) (\S+)", said, re.M)
        check(row is not None,
              "the recording did not decode: %r" % said[-400:])

        if row:
            codec, seen = row.group(1), int(row.group(2))
            decode_ms = float(row.group(3))
            bars = [int(b, 16) for b in row.group(4).split(",")]
            bars_seen = bars

            check(codec == "H.264", "Video decoded it as %r" % codec)
            check(seen == frames,
                  "%d pictures came out of the %d frames recorded"
                  % (seen, frames))
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
          "frames in %d bytes kept in /Home/Captures, read back as one H.264 "
          "track of the pattern's size and frames, and played: every frame "
          "decoded by FFmpeg through /Kosmos/Libraries/video.lua, %.1f ms each under "
          "QEMU, and the eight bars their colours: %s)"
          % (checks, frames, size, decode_ms,
             ",".join("%06x" % b for b in bars_seen)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
