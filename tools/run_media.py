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
import zlib

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


def vbr_mp3(path, count=400):
    """A variable-bitrate MP3 made here, whose length only its Xing header says.

    A first frame of 64 kbps holding no music but "Xing", its flags, the count
    of frames after it and their bytes; then frames of 128 and 192 kbps
    alternating, each silent - side information all zero, so no main data.
    Read as music, the header frame makes it 64 kbps and 26 seconds, which is
    how Basket Case came to say `9:58` on the ThinkPad; counted, it is 400
    frames of 1152 samples, 10.449 s, at 160 kbps on average.
    """
    def frame(index, kbps):
        # MPEG-1 Layer III, no CRC, 44100 Hz, joint stereo.
        return bytes([0xFF, 0xFB, index << 4, 0x44]) + bytes(144000 * kbps // 44100 - 4)

    audio = b"".join(frame(9, 128) if i % 2 == 0 else frame(11, 192)
                     for i in range(count))
    first = bytearray(frame(5, 64))
    tag = b"Xing" + struct.pack(">III", 3, count, len(first) + len(audio))
    first[36:36 + len(tag)] = tag           # after 4 of header, 32 of side information

    with open(path, "wb") as f:
        f.write(bytes(first) + audio)


def png_of(width, height, rgb, quarters=None):
    """A PNG of one colour - or of four, a quarter each.

    Four colours rather than one is what tells a *scaled* picture from a
    *cropped* one: drawn at half its size, all four quarters are in the box;
    cropped, only the first is.

    Three chunks and a CRC each, which is the same construction
    `run_screenshot.py` uses to save a screenshot - and a picture whose colour
    this file chose is what lets the check below say the cover reached the
    screen rather than something else did.
    """
    raw = bytearray()

    for y in range(height):
        raw.append(0)                       # no filter on this row

        if not quarters:
            raw += bytes(rgb) * width
        else:
            top = y < height // 2
            left, right = quarters[0 if top else 2], quarters[1 if top else 3]
            raw += bytes(left) * (width // 2) + bytes(right) * (width - width // 2)

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def mp3_with_cover(path, rgb, count=40):
    """An MP3 whose ID3v2.3 tag carries a real picture of one colour.

    The tag is built the way `test_tags.lua` builds one - a syncsafe size, an
    `APIC` frame with its mime type, its kind and a description - and the
    audio after it is the silent frames `vbr_mp3` uses. The picture is a real
    PNG rather than the stand-in bytes the tag tests use, because this one has
    to decode and appear on a screen.
    """
    picture = png_of(64, 64, rgb,
                     quarters=(rgb, (0xd0, 0x30, 0x40),
                               (0xf0, 0xc0, 0x20), (0x80, 0x40, 0xc0)))
    apic = b"\0image/png\0\3front\0" + picture

    def syncsafe(n):
        return bytes(((n >> 21) & 0x7f, (n >> 14) & 0x7f,
                      (n >> 7) & 0x7f, n & 0x7f))

    def frame(tag, body):
        return tag + struct.pack(">IH", len(body), 0) + body

    body = frame(b"TIT2", b"\0One Colour") + frame(b"APIC", apic)
    tag = b"ID3" + bytes((3, 0, 0)) + syncsafe(len(body)) + body

    def audio_frame(index, kbps):
        return bytes([0xFF, 0xFB, index << 4, 0x44]) + bytes(144000 * kbps // 44100 - 4)

    with open(path, "wb") as f:
        f.write(tag + b"".join(audio_frame(9, 128) for _ in range(count)))

    return picture


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
    vbr_in = os.path.join(work, "vbr.mp3")
    cover_in = os.path.join(work, "cover.mp3")
    COVER = (0x20, 0xc0, 0x40)
    checks, fails = 0, []

    def check(ok, complaint):
        nonlocal checks
        if ok:
            checks += 1
        else:
            fails.append(complaint)

    tone(wav_in)
    vbr_mp3(vbr_in)
    mp3_with_cover(cover_in, COVER)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", disk, "64",
                    wav_in + ":/home/tone.wav", vbr_in + ":/home/vbr.mp3",
                    cover_in + ":/home/cover.mp3"], check=True,
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
               'local v = assert(media.open("/home/vbr.mp3")) '
               'print("media" .. ": vbr " .. v.info.seconds .. " s " .. v.info.bitrate '
               '.. " kbps " .. tostring(v.info.vbr)) v:close() '
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
        vbr = re.search(r"^media: vbr ([\d.]+) s (\d+) kbps (\w+)", said, re.M)

        check(vbr is not None
              and abs(float(vbr.group(1)) - 400 * 1152 / 44100.0) < 0.01
              and vbr.group(2) == "160" and vbr.group(3) == "true",
              "a variable-bitrate MP3 with a Xing header was not 10.449 s at 160 "
              "kbps on average and VBR - its header frame read as the music is 64 "
              "kbps and 26 s: %r" % (vbr.group(0) if vbr else None))
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

    #
    # **A cover inside an MP3, on the screen.**
    #
    # `tags.lua` reports where a picture is rather than reading it, so a
    # library does not decode a thousand covers to show ten; `media.cover`
    # is the other half, for the one song being played. The bytes go into a
    # region and the region to the window manager with a name, because the
    # picture is data and the message is control - and because `/ramfs` caps
    # a file at 16 KB where a cover is hundreds.
    #
    # The picture is one colour this file chose, so finding that colour on
    # screen says the cover arrived rather than that something did.
    #
    guest = Guest(image, 600)

    try:
        guest.wait_for(PROMPT, "reached a shell")

        program = (
            "local ui = use('/lib/ui.lua') "
            "local media = use('/lib/media.lua') "
            "local name, cw, ch = media.cover('/home/cover.mp3') "
            "if not name then print('cover: ' .. tostring(cw)) return end "
            "print('cover: ' .. name .. ' ' .. tostring(cw) .. 'x' .. tostring(ch)) "
            "local w = ui.window{ title = 'Cover', w = 200, h = 200, "
            "x = 700, y = 200 } "
            "if not w then return end "
            "local v = ui.view{ x = 0, y = 0, w = 200, h = 200 } "
            "v:add(ui.image{ x = 20, y = 20, w = 32, h = 32, asset = name, fit = true }) "
            "w:add(v) w:run()"
        )

        guest.type("fs.write('/ramfs/cover.lua', %r)" % program)
        guest.type("wm cover,/ramfs/cover.lua")

        mark = len(guest.seen)
        placed, deadline = None, time.monotonic() + 40

        while placed is None and time.monotonic() < deadline:
            found = re.search(r"wm: window Cover at (\d+),(\d+) (\d+)x(\d+)",
                              guest.seen)
            if found:
                placed = tuple(int(v) for v in found.groups())
            time.sleep(0.3)

        said = [l.strip() for l in guest.seen[mark:].splitlines()
                if l.strip().startswith("cover:")]

        if placed is None:
            check(False, "the window that draws a cover never opened: %r" % said)
        else:
            time.sleep(2.5)
            wx, wy, _, _ = placed
            width, height, px = parse_ppm(guest.screendump())
            found_colour = 0

            for y in range(wy + 20, min(wy + 52, height)):
                for x in range(wx + 20, min(wx + 52, width)):
                    o = (y * width + x) * 3

                    if (px[o], px[o + 1], px[o + 2]) == COVER:
                        found_colour += 1

            #
            # **A 64-pixel cover drawn into a 32-pixel box**, which is what
            # Music's window does at 78 and at 44. Before the image command
            # carried a drawn size this was a crop, so the box held the
            # picture's top-left quarter - the same colour, and no way to
            # tell. The check is that the colour fills the box it was given:
            # a crop of a one-colour picture fills it too, so the phase below
            # takes the picture apart into quarters.
            #
            #
            # A quarter of the box, because the picture is four quarters and
            # this counts the first one's colour. The threshold was 900 while
            # the picture was one colour, and stayed there when it became
            # four - so the first run of the scaled version failed at exactly
            # the 256 pixels that prove it right.
            #
            check(200 <= found_colour <= 330,
                  "the cover's first quarter covers %d of the 1024 pixels the "
                  "picture was drawn into, where a quarter is about 256: the "
                  "picture inside the MP3 did not reach the screen at the size "
                  "it was asked for. What the program said: %r"
                  % (found_colour, said))

            #
            # **And that it is the whole picture, not a corner of it.** The
            # cover is four quarters of four colours; drawn at half its size
            # all four have to be in the box, where a crop shows one.
            #
            quarters = set()

            for dy, dx in ((8, 8), (8, 24), (24, 8), (24, 24)):
                o = ((wy + 20 + dy) * width + wx + 20 + dx) * 3
                quarters.add((px[o], px[o + 1], px[o + 2]))

            check(len(quarters) == 4,
                  "the cover drawn at half its size shows %d of its four "
                  "colours (%r), so the picture was cropped rather than "
                  "scaled." % (len(quarters), sorted(quarters)))
    except Failure as e:
        fails.append(str(e))
    finally:
        guest.close()

    #
    # **And a folder Music cannot list says why.** On the ThinkPad Music said
    # "(nothing to play in /home)" beside a Tracker window listing the MP3, and
    # could not have said anything else: a list that failed and a folder with
    # no music in it looked the same. Pointed at a folder that is not there, it
    # has to name the folder and the reason, in the log `diagnose` keeps. A
    # machine of its own, because the shell is inside `wm` until it ends.
    #
    guest = Guest(image, 600)

    try:
        guest.wait_for(PROMPT, "reached a shell")
        mark = len(guest.seen)
        guest.type("wm music:/home/nowhere/song.mp3")

        said = None
        deadline = time.monotonic() + 60

        while said is None and time.monotonic() < deadline:
            said = re.search(r"music: could not list /home/nowhere: \S",
                             guest.seen[mark:])
            time.sleep(0.25)

        check(said is not None,
              "Music pointed at a folder that is not there did not say it "
              "could not list it, and why:\n" + guest.seen[mark:][-800:])
    except Failure as e:
        fails.append(str(e))
    finally:
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on media.lua, heard:" % (len(fails), len(fails) + checks))
        for complaint in fails:
            print("  " + complaint)
        return 1

    print("PASS: %d checks on media.lua, heard (a cover read out of an MP3 and drawn, a variable-bitrate MP3's length and bitrate from its Xing header, a tone played, sought and "
          "finished at the prompt with the position following the sound, "
          "Music's Play and bar doing the same, and Music saying why it could "
          "not list a folder)." % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
