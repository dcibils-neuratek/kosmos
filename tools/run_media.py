#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""`/lib/media.lua` on the machine, heard: a tone played, sought and finished.

The engine under Music, and under a video app later (`docs/music.html`). What
it promises is about time, and time is the sound's - so this holds it to the
sound. A six-second tone made here goes on a disk; QEMU's virtio-sound writes
whatever the guest plays to a WAV on this Mac; and afterwards the WAV is
counted, which is the one witness that cannot be flattered by a number the
guest printed about itself.

**At the prompt**, a program opens the tone, plays a second, seeks to four,
and plays on to the end: the length has to be six seconds, the position has to
say about one and then about four and a half, the file has to finish, and
about three seconds of tone - one before the seek and two after - have to come
out, at the level they went in. Six would mean the seek moved the clock and not
the reading.

**In the window**, Music is opened on the same tone, its Play button clicked
and then its bar, three quarters along: about three more seconds have to come
out, not six, which is what a bar that does not seek sounds like.

Usage: run_media.py [IMAGE]
"""

import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
LUA = os.path.join(os.path.dirname(HERE), "build", "host", "lua")
RATE, SECONDS = 44100, 6
WAV_HEADER = 44


def tone(path):
    """Six seconds of 440 Hz, stereo, sixteen bits: made here, owned by nobody."""
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = bytearray()

        for i in range(RATE * SECONDS):
            v = int(12000 * math.sin(2 * math.pi * 440 * i / RATE))
            frames += struct.pack("<hh", v, v)

        w.writeframes(bytes(frames))


def heard(path):
    """(seconds not silent, loudest sample) in what QEMU has written so far."""
    if not os.path.exists(path):
        return 0.0, 0

    with open(path, "rb") as f:
        data = f.read()[WAV_HEADER:]

    frames = len(data) // 4

    if frames == 0:
        return 0.0, 0

    samples = struct.unpack("<%dh" % (frames * 2), data[:frames * 4])
    live = sum(1 for i in range(frames) if samples[2 * i] or samples[2 * i + 1])

    return live / float(RATE), max(abs(s) for s in samples)


def settled(path, seconds=1.5):
    """What was heard, once QEMU has had time to write the last of it."""
    time.sleep(seconds)
    return heard(path)


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    work = tempfile.mkdtemp(prefix="kosmos-media-")
    wav_in = os.path.join(work, "tone.wav")
    disk = os.path.join(work, "disk.img")
    wav_out = os.path.join(work, "heard.wav")
    checks, fails = 0, []

    def check(ok, complaint):
        nonlocal checks
        if ok:
            checks += 1
        else:
            fails.append(complaint)

    tone(wav_in)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", disk, "64",
                    wav_in + ":/home/tone.wav"], check=True,
                   capture_output=True, cwd=os.path.dirname(HERE))

    # Both read by run_screenshot when it is imported, so they are set first.
    os.environ["KOSMOS_DISK"] = disk
    os.environ["KOSMOS_AUDIO_WAV"] = wav_out
    sys.path.insert(0, HERE)
    from run_screenshot import Guest, Failure, PROMPT, _to_tablet, parse_ppm  # noqa: E402

    # Each line the program prints is put together from pieces, so the line
    # typed to start it - which the shell echoes - never holds the words
    # waited for.
    program = ('local media = use("/lib/media.lua") '
               'local p = assert(media.open("/home/tone.wav")) '
               'local hz = fs.read("/dev/cpu").counter_hz '
               'local function run(s) local stop = sys.ticks() + math.floor(s * hz) '
               'while sys.ticks() < stop and not p:finished() do p:tick() sys.sleep(1) end end '
               'print("media" .. ": seconds " .. p.info.seconds) '
               'p:play() run(1.0) print("media" .. ": after 1 s " .. p:position()) '
               'p:seek(4) run(0.6) print("media" .. ": after the seek to 4 " .. p:position()) '
               'run(6) print("media" .. ": finished " .. tostring(p:finished()) .. " " .. p:position()) '
               'p:close() print("media" .. ": done")')

    guest = Guest(image, 600)

    try:
        guest.wait_for(PROMPT, "reached a shell")
        guest.type('fs.write("/ramfs/media.lua", [[' + program + ']])')
        time.sleep(1.5)
        mark = len(guest.seen)
        guest.type("/ramfs/media.lua")
        guest.wait_for("media: done", "the program played the tone to its end")

        said = guest.seen[mark:]

        def number(label):
            m = re.search(r"^media: %s (-?[\d.]+)" % re.escape(label), said, re.M)
            return float(m.group(1)) if m else None

        length = number("seconds")
        after_one = number("after 1 s")
        after_seek = number("after the seek to 4")
        end = re.search(r"^media: finished (\w+) ([\d.]+)", said, re.M)

        check(length is not None and abs(length - SECONDS) < 0.01,
              "media.open did not say the tone is %d seconds: %r" % (SECONDS, length))
        check(after_one is not None and 0.7 <= after_one <= 1.3,
              "after a second of playing, the position was not about one: %r" % after_one)
        check(after_seek is not None and 4.3 <= after_seek <= 4.9,
              "after a seek to four and a little more playing, the position was "
              "not about four and a half: %r" % after_seek)
        check(end is not None and end.group(1) == "true" and float(end.group(2)) >= 5.7,
              "the tone did not finish, at about six seconds: %r"
              % (end.group(0) if end else None))

        first, loudest = settled(wav_out)

        check(2.6 <= first <= 3.5,
              "about three seconds of tone should have come out - one before the "
              "seek to four, two after - and %.2f did" % first)
        check(loudest >= 11000,
              "the loudest sample heard was %d, where the tone went in at 12000"
              % loudest)

        #
        # The window: Music on the same tone, Play, then the bar.
        #
        mark = len(guest.seen)
        guest.type("wm music:/home/tone.wav")

        where = None
        deadline = time.monotonic() + 60

        while where is None and time.monotonic() < deadline:
            where = re.search(r"wm: window Music at (\d+),(\d+) (\d+)x(\d+)",
                              guest.seen[mark:])
            time.sleep(0.25)

        if where is None:
            raise Failure("Music's window was never placed:\n"
                          + guest.seen[mark:][-1200:])

        wx, wy, ww, wh = (int(v) for v in where.groups())
        time.sleep(2.0)

        width, height, _ = parse_ppm(guest.screendump())

        # The logged position is the corner of what the window draws in, and
        # a view's coordinates start there (`run_screenshot.py`'s `covers`).
        def click(x, y):
            guest.mouse_to(*_to_tablet(wx + x, wy + y, width, height))
            time.sleep(0.3)
            guest.mouse_button(True)
            time.sleep(0.15)
            guest.mouse_button(False)
            time.sleep(0.3)

        click(10 + 20, 168 + 8)                 # Play, at (10, 168)
        time.sleep(1.5)

        bar_w = 400                             # the transport view, W - 20
        click(10 + 2 + int((bar_w - 4) * 0.75), 200 + 6)   # the bar, 3/4 along
        time.sleep(4.0)

        total, _ = settled(wav_out)
        window = total - first

        check(2.0 <= window <= 4.2,
              "Music's Play and then its bar three quarters along should have "
              "sounded about three seconds - a second and a half before the "
              "click, a second and a half after - and %.2f did (six would be a "
              "bar that does not seek, none a Play that was not pressed)"
              % window)

        errors = [l.strip() for l in guest.seen[mark:].splitlines()
                  if l.strip().startswith("music:")]

        check(not errors, "Music said something went wrong: %r" % errors)
    except Failure as e:
        fails.append(str(e))
    finally:
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on media.lua, heard:" % (len(fails), len(fails) + checks))
        for complaint in fails:
            print("  " + complaint)
        return 1

    print("PASS: %d checks on media.lua, heard (a tone played, sought and "
          "finished at the prompt with the position following the sound, and "
          "Music's Play and bar doing the same)." % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
