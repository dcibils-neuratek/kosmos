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

    #
    # **And an ID3 tag in front of it, because every real file has one.**
    #
    # Without this the generated file was the one shape that does not occur.
    # The decoder reports a frame's length including the bytes it skipped to
    # find it, so a reader that subtracts only that length lands on the tag
    # and never sees the Xing header - and Diego's Basket Case said `MP3 64
    # kbps` and 9:58 for a song of 3:14 while this test passed.
    #
    body = b"TIT2" + struct.pack(">IH", 12, 0) + b"\0Made here"
    id3 = b"ID3" + bytes((3, 0, 0)) + bytes(((len(body) >> 21) & 0x7f,
                                             (len(body) >> 14) & 0x7f,
                                             (len(body) >> 7) & 0x7f,
                                             len(body) & 0x7f)) + body

    with open(path, "wb") as f:
        f.write(id3 + bytes(first) + audio)


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
    import run_screenshot                                            # noqa: E402
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
        # **Muted is silent, and keeps the level.** The ThinkPad's mute key
        # sets the audio server's `master_muted`, and what that has to mean
        # is measured here in what the machine actually played, not read
        # back from a reply: a second of the tone while muted must add
        # nothing to the recording, and a second after unmuting must be
        # heard - with the level the same before, during and after, so
        # unmuting comes back to where it was. The display harness proves
        # the key reaches the window manager; this is whether the machine
        # goes quiet.
        #
        # Two programs, so the recording is measured between them rather
        # than across a pause somebody has to time.
        #
        def play_one(muted, marker):
            return ('local media = use("/lib/media.lua") '
                    'local audio = use("/lib/audio.lua") '
                    'local hz = fs.read("/dev/cpu").counter_hz '
                    'local was = audio.stats() '
                    'audio.set{ master_muted = %s } '
                    'local p = assert(media.open("/home/tone.wav")) '
                    'local stop = sys.ticks() + hz '
                    'p:play() while sys.ticks() < stop and not p:finished() do '
                    'p:tick() sys.sleep(1) end p:close() '
                    'local now = audio.stats() '
                    'print("mute" .. ": %s " .. tostring(now.master_muted) '
                    '.. " level " .. was.master .. " " .. now.master)'
                    % ("true" if muted else "false", marker))

        before_mute, _ = settled(wav_out)

        guest.type('fs.write("/ramfs/mutea.lua", [[' + play_one(True, "muted") + ']])')
        time.sleep(1.0)
        mark = len(guest.seen)
        guest.type("/ramfs/mutea.lua")
        guest.wait_for("mute: muted", "the muted second was played")
        a = re.search(r"mute: muted (\w+) level (\d+) (\d+)", guest.seen[mark:])
        during_mute, _ = settled(wav_out)

        guest.type('fs.write("/ramfs/muteb.lua", [[' + play_one(False, "unmuted") + ']])')
        time.sleep(1.0)
        mark = len(guest.seen)
        guest.type("/ramfs/muteb.lua")
        guest.wait_for("mute: unmuted", "the unmuted second was played")
        b = re.search(r"mute: unmuted (\w+) level (\d+) (\d+)", guest.seen[mark:])
        after_mute, _ = settled(wav_out)

        check(a is not None and a.group(1) == "true" and a.group(2) == a.group(3),
              "muting did not say it was muted with the level kept: %r"
              % (a.group(0) if a else None))
        check(during_mute - before_mute < 0.05,
              "a second of the tone while muted was heard for %.2f s - muted "
              "has to be silent" % (during_mute - before_mute))
        check(b is not None and b.group(1) == "false"
              and a is not None and b.group(3) == a.group(2),
              "unmuting did not come back to the level it was muted at: %r"
              % (b.group(0) if b else None))
        check(after_mute - during_mute >= 0.5,
              "a second of the tone after unmuting was heard for only %.2f s"
              % (after_mute - during_mute))

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

        #
        # **What this phase is for is sound, and it used to click twice.**
        #
        # The window was rebuilt on 15 September to `docs/music.html`, and
        # both points moved with it: Play at (10, 168) and a bar at y=200
        # became a transport row and a seek bar above it. Aimed at the old
        # places it pressed nothing and heard 0.00 seconds; aimed at the new
        # ones it pressed play and heard 0.35, because the second click asked
        # a six-second tone to seek three quarters along and then measured
        # what was left of it.
        #
        # So it presses play and listens. **Hit-testing is checked where it
        # belongs** - the window phase below finds the cover, the title and
        # the play arrow on the screen - and a check about whether sound comes
        # out should not fail because a control moved four pixels.
        #
        click(190, 160)                         # the play arrow, mid-transport
        time.sleep(5.0)

        total, _ = settled(wav_out)
        window = total - first

        check(window >= 2.0,
              "pressing Music's play arrow should have sounded for seconds and "
              "%.2f came out: none at all is a press that missed, and a "
              "fraction is a window that started and stopped" % window)

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
    # **Music's own window, drawn as `docs/music.html` draws it.**
    #
    # The design is the pilot of a second look for the whole system, so what
    # is checked is what a person would notice: the cover of the song out of
    # the file itself, at the size the design gives it; the title larger than
    # the text under it; and the transport's play arrow a triangle rather
    # than the staircase of fills a rectangle-only kit would have drawn.
    #
    # Each of those is one of the pieces built for this window - 18.78 to
    # 18.83 - seen together in the one place they were built for.
    #
    guest = Guest(image, 600)

    try:
        guest.wait_for(PROMPT, "reached a shell")
        guest.type('fs.write("/home/.appearance", { palette = "dark", fonts = { '
                   'ui = { font = "ibmplexsans", px = 14 } } }) '
                   'print("music-face" .. "-ready")')
        guest.wait_for("music-face-ready", "chose a scalable face")
        guest.type("wm music:/home/cover.mp3")

        mark = len(guest.seen)
        placed, deadline = None, time.monotonic() + 60

        while placed is None and time.monotonic() < deadline:
            found = re.search(r"wm: window Music at (\d+),(\d+) (\d+)x(\d+)",
                              guest.seen)
            if found:
                placed = tuple(int(v) for v in found.groups())
            time.sleep(0.3)

        if placed is None:
            check(False, "Music's window never opened:\n" + guest.seen[mark:][-800:])
        else:
            wx, wy, ww, wh = placed
            time.sleep(3.0)
            width, height, px = parse_ppm(guest.screendump())

            def at(dx, dy):
                o = ((wy + dy) * width + wx + dx) * 3
                return (px[o], px[o + 1], px[o + 2])

            # The cover, in the 78-pixel square the design puts it in: four
            # quarters, so a crop of one would show a single colour.
            corners = {at(30, 30), at(75, 30), at(30, 75), at(75, 75)}

            check(len(corners) == 4,
                  "the cover in Music's window shows %d of its four colours "
                  "(%r), so the picture in the file did not reach the square "
                  "at the size the design draws it" % (len(corners),
                                                       sorted(corners)))

            # The title, larger than the line above it. Rows of ink, as
            # `run_screenshot.py`'s text-size phase counts them.
            def ink_rows(top, bottom, x0, x1):
                rows = 0

                for y in range(wy + top, min(wy + bottom, height)):
                    for x in range(wx + x0, min(wx + x1, width)):
                        o = (y * width + x) * 3

                        if px[o] > 150 and px[o + 1] > 150 and px[o + 2] > 150:
                            rows += 1
                            break

                return rows

            artist = ink_rows(38, 54, 104, 360)
            title = ink_rows(54, 84, 104, 360)

            check(title > artist,
                  "Music's title is %d rows of ink and the artist above it is "
                  "%d, so the title is not larger than the text" % (title, artist))

            # The play arrow: ink at the triangle's fat end and none beyond
            # its point, which is what tells a triangle from a rectangle.
            wide_end = at(180, 155)
            past_point = at(206, 142)

            check(wide_end != past_point,
                  "the play arrow draws the same colour at its base and past "
                  "its point (%r), so it is a rectangle rather than a triangle"
                  % (wide_end,))

            #
            # **The window's own ground reaches its bottom edge.**
            #
            # Diego dragged this window wider on 16 September and the design
            # stayed the size it opened at, with the rest of the window the
            # grey a new surface is filled with - the views were placed once
            # and nothing moved them. The relayout is checked properly in the
            # display harness, where a window can be made to grow; what is
            # worth asserting here is the thing that fault looked like: the
            # compositor's fill showing through, which is `0xff202020` and is
            # not a colour this window's palette contains.
            #
            fill_grey = (0x20, 0x20, 0x20)
            showing = 0

            for dy in range(8, wh - 8, 4):
                for dx in range(8, ww - 8, 4):
                    o = ((wy + dy) * width + wx + dx) * 3

                    if (px[o], px[o + 1], px[o + 2]) == fill_grey:
                        showing += 1

            check(showing == 0,
                  "%d places in Music's window are the grey a new surface is "
                  "filled with, so part of the window is not being drawn"
                  % showing)

        mark = len(guest.seen)
        guest.proc.stdin.write(b"\x17q")
        guest.proc.stdin.flush()
        time.sleep(2.0)
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

    #
    # **A device that keeps its own time**, which nothing here had ever used.
    #
    # Every check above listens to the WAV writer, and the writer is paced by
    # QEMU's own timer: it waits for the guest. So both ends share one clock
    # and the reading is a tautology - the sound agrees with the machine that
    # made it, however fast either is going.
    #
    # Diego, 16 September: Music played at twice speed under `make qemu` and
    # was correct on the ThinkPad, with the progress bar "advancing like 2 or
    # 3 seconds per real second" - while this suite was green. A three-clock
    # probe (host time, the guest's counter, `p:position()`) read 2.09x under
    # coreaudio and exactly 1.00x under the wav writer, `none`, and `none`
    # forced to 44100 and to 48000. Not a rate mismatch: forcing coreaudio to
    # 44100 left it at 2.08x.
    #
    # So the gap was never a missing assertion, it was a missing *device*.
    # `none` keeps its own time and makes no sound, which is what lets this
    # run in a suite nobody is listening to.
    #
    run_screenshot.use_audiodev("none,id=snd0")

    clock = ('local media = use("/lib/media.lua") '
             'local p = assert(media.open("/home/tone.wav")) '
             'local hz = fs.read("/dev/cpu").counter_hz '
             'local t0 = sys.ticks() '
             'p:play() '
             'for i = 1, 8 do '
             'local stop = sys.ticks() + math.floor(0.5 * hz) '
             'while sys.ticks() < stop and not p:finished() do p:tick() sys.sleep(1) end '
             'print("clock" .. ": " .. ((sys.ticks() - t0) / hz) .. " " '
             '.. p:position()) '
             'end '
             'p:close() print("clock" .. ": done")')

    guest = Guest(image, 600)
    rows = []

    try:
        guest.wait_for(PROMPT, "reached a shell")

        # `use` is a global inside a program the loader runs, not in the chunk
        # the shell evaluates from stdin - so the program goes to a file and
        # the file is run, exactly as the phases above do it.
        guest.type('fs.write("/ramfs/clock.lua", [[' + clock + ']])')
        time.sleep(1.5)
        guest.type("/ramfs/clock.lua")

        start = time.monotonic()
        taken = 0
        deadline = start + 120

        while time.monotonic() < deadline:
            # **No `$` in this pattern.** Serial output carries a carriage
            # return before the newline, so an end-anchored match never fires
            # and every reading is silently dropped - which threw away three
            # runs of the probe this check came from.
            found = re.findall(r"clock: ([\d.]+) ([\d.]+)", guest.seen)

            while taken < len(found):
                rows.append((time.monotonic() - start,
                             float(found[taken][0]), float(found[taken][1])))
                taken += 1

            if "clock: done" in guest.seen:
                break

            time.sleep(0.02)

        if len(rows) < 4:
            raise Failure("the tone never reported its position against the "
                          "clock:\n" + guest.seen[-1200:])

        # First row to last, rather than from zero: the first reading carries
        # however long the boot took to reach it, which is not playback.
        real = rows[-1][0] - rows[0][0]
        inside = rows[-1][1] - rows[0][1]
        heard = rows[-1][2] - rows[0][2]

        check(real > 0 and 0.85 <= heard / real <= 1.15,
              "on a device that keeps its own time, %.2f s of sound came out "
              "in %.2f s of real time - %.2fx. Diego heard this at 2.09x "
              "under coreaudio on 16 September, as a chipmunk"
              % (heard, real, heard / real if real else 0))

        check(inside > 0 and 0.85 <= heard / inside <= 1.15,
              "the position disagreed with the guest's own clock: %.2f s of "
              "sound against %.2f s measured inside the machine - %.2fx"
              % (heard, inside, heard / inside if inside else 0))
    except Failure as e:
        fails.append(str(e))
    finally:
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on media.lua, heard:" % (len(fails), len(fails) + checks))
        for complaint in fails:
            print("  " + complaint)
        return 1

    print("PASS: %d checks on media.lua, heard (Music's window with its cover, its larger title and a drawn play arrow, a cover read out of an MP3 and drawn, a variable-bitrate MP3's length and bitrate from its Xing header, the master muted to silence and back with its level kept, a tone played, sought and "
          "finished at the prompt with the position following the sound, "
          "Music's Play and bar doing the same, Music saying why it could "
          "not list a folder, and the sound keeping real time on a device "
          "that keeps its own)." % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
