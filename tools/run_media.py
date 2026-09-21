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
import scratch
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


#
# **A film, built here, because none goes in the repository.**
#
# `test_mp4.lua` says it about the container and it is just as true of the
# pictures: the clip Diego tests with lives in `~/Kosmos/home`, and a suite
# that needed it would pass on this Mac and fail everywhere else. So the
# film is made - a few frames of flat grey, each a different grey, in an
# MP4 the video kit can really open and really decode.
#
# The JPEG is written by hand and is the smallest baseline one that means
# anything: a single 8x8 block whose only coefficient is DC, which *is* a
# flat grey square. No transform, no quantisation worth the name, and the
# standard Huffman tables - so what is exercised is the decoder in the
# image, not an encoder written here to be exercised by it.
#
_DC_BITS = bytes([0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0])
_DC_VALS = bytes(range(12))
_AC_BITS = bytes([0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d])
_AC_VALS = bytes([
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa])


def _huffman(bits, vals):
    """The canonical code for each value, as (code, length)."""
    codes, code, k = {}, 0, 0

    for length in range(1, 17):
        for _ in range(bits[length - 1]):
            codes[vals[k]] = (code, length)
            code += 1
            k += 1

        code <<= 1

    return codes


def jpeg_grey(level, width=8, height=8):
    """A baseline JPEG of one flat grey, `level` from 0 to 255."""
    dc = _huffman(_DC_BITS, _DC_VALS)
    ac = _huffman(_AC_BITS, _AC_VALS)

    # Quantisation of 16 on the DC, 16 everywhere else: a pixel comes back
    # as 128 + coefficient * 16 / 8, so the coefficient is what to write.
    quant = bytes([16] * 64)
    coefficient = max(-127, min(127, round((level - 128) / 2)))

    out, bit_buffer, bit_count = bytearray(), 0, 0

    def put(code, length):
        nonlocal bit_buffer, bit_count

        for i in range(length - 1, -1, -1):
            bit_buffer = (bit_buffer << 1) | ((code >> i) & 1)
            bit_count += 1

            if bit_count == 8:
                out.append(bit_buffer & 0xff)

                # A 0xFF byte in the entropy data is stuffed, or a decoder
                # reads it as the start of a marker.
                if bit_buffer & 0xff == 0xff:
                    out.append(0)

                bit_buffer, bit_count = 0, 0

    blocks = ((width + 7) // 8) * ((height + 7) // 8)

    for i in range(blocks):
        value = coefficient if i == 0 else 0     # a difference, so once
        size = value.bit_length() if value >= 0 else abs(value).bit_length()
        code, length = dc[size]

        put(code, length)

        if size:
            bits = value if value > 0 else ((1 << size) - 1 + value)
            put(bits, size)

        put(*ac[0x00])                           # end of block: no AC at all

    if bit_count:                                # pad with ones, as JPEG says
        put(0xff, 8 - bit_count)

    def marker(kind, body):
        return bytes([0xff, kind]) + struct.pack(">H", len(body) + 2) + body

    return (b"\xff\xd8"
            + marker(0xdb, b"\x00" + quant)
            + marker(0xc0, struct.pack(">BHHB", 8, height, width, 1)
                     + bytes([1, 0x11, 0]))
            + marker(0xc4, b"\x00" + _DC_BITS + _DC_VALS)
            + marker(0xc4, b"\x10" + _AC_BITS + _AC_VALS)
            + marker(0xda, bytes([1, 1, 0x00, 0, 63, 0]))
            + bytes(out)
            + b"\xff\xd9")


def mjpeg_mp4(path, levels, width=8, height=8, fps=10):
    """An MP4 whose video track is those greys, one to a frame."""
    frames = [jpeg_grey(level, width, height) for level in levels]

    def box(kind, body):
        return struct.pack(">I", len(body) + 8) + kind + body

    def full(kind, version, body):
        return box(kind, bytes([version, 0, 0, 0]) + body)

    def words(*n):
        return b"".join(struct.pack(">I", v) for v in n)

    payload = b"".join(frames)

    def moov(first):
        visual = (b"\0" * 6 + struct.pack(">H", 1) + b"\0" * 16
                  + struct.pack(">HH", width, height) + b"\0" * (78 - 28))
        #
        # The `esds` that says these are JPEGs: an ES_Descriptor (tag 3)
        # holding a DecoderConfigDescriptor (tag 4) whose object type is
        # 0x6c. Without it `mp4v` means only "MPEG-4 systems describes
        # this", and a reader is entitled to guess MPEG-4 Visual - which is
        # what `ffmpeg` does with a file that leaves it out.
        #
        es = (b"\x03\x19" + b"\x00\x01" + b"\x00"
              + b"\x04\x11" + b"\x6c\x11" + b"\0\0\0" + words(0, 0)
              + b"\x06\x01\x02")
        stsd = full(b"stsd", 0,
                    words(1) + box(b"mp4v", visual + full(b"esds", 0, es)))
        stsz = full(b"stsz", 0, words(0, len(frames), *[len(f) for f in frames]))
        stsc = full(b"stsc", 0, words(1, 1, 1, 1))

        at, offsets = first, []

        for f in frames:
            offsets.append(at)
            at += len(f)

        stco = full(b"stco", 0, words(len(offsets), *offsets))
        stts = full(b"stts", 0, words(1, len(frames), 1000 // fps))
        stbl = box(b"stbl", stsd + stts + stsc + stsz + stco)
        mdhd = full(b"mdhd", 0, words(0, 0, 1000, len(frames) * (1000 // fps))
                    + b"\0\0\0\0")
        hdlr = full(b"hdlr", 0, words(0) + b"vide" + b"\0" * 12 + b"V\0")

        return box(b"moov", box(b"mvhd", b"\0" * 100)
                   + box(b"trak", box(b"mdia", mdhd + hdlr
                                      + box(b"minf", stbl))))

    ftyp = box(b"ftyp", b"isom" + words(0x200) + b"isomavc1")
    length = len(moov(0))
    head = len(ftyp) + length + 8

    with open(path, "wb") as f:
        f.write(ftyp + moov(head) + box(b"mdat", payload))


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


SAVED_BYTE = 0x4B                       # what the test cartridge saves


def snes_rom(path):
    """A Super Nintendo cartridge of our own: 32 KB that does almost nothing.

    **Not game data**, which never goes in the repository or near it: seven
    instructions and a header, made here. `SEI`, `CLC`, `XCE` into native
    mode; `SEP #$20` for an 8-bit accumulator; `LDA #$4B` and `STA
    $700000`, one byte into the cartridge's own RAM, which a LoROM maps at
    bank 70h - so its save has something in it to find; and `BRA` to itself
    for ever. The screen stays black because nothing turns it on. LakeSnes
    takes 32 KB as its smallest cartridge and finds the header at 7FC0h, a
    LoROM's place (`snes_other.c`), where the title, the map mode, the
    chips - ROM, RAM and a battery - the size, 2^5 KB, the RAM's, 2^1 KB,
    and the checksum pair go, and the reset vector at 7FFCh points at the
    code, which a LoROM maps at 8000h. A cartridge the core refuses would
    fail this phase at its first line, which is what it is for.
    """
    rom = bytearray(0x8000)
    rom[0:13] = bytes([0x78, 0x18, 0xFB,             # sei; clc; xce
                       0xE2, 0x20,                   # sep #$20
                       0xA9, SAVED_BYTE,             # lda #$4b
                       0x8F, 0x00, 0x00, 0x70,       # sta $700000
                       0x80, 0xFE])                  # bra self
    rom[0x7FC0:0x7FD5] = b"KOSMOS TEST CARTRIDGE"[:21].ljust(21, b" ")
    rom[0x7FD5] = 0x20                  # LoROM
    rom[0x7FD6] = 0x02                  # ROM, RAM and a battery
    rom[0x7FD7] = 0x05                  # 2^5 KB
    rom[0x7FD8] = 0x01                  # 2^1 KB of RAM
    rom[0x7FD9] = 0x01                  # North America
    rom[0x7FDC:0x7FE0] = bytes([0xFF, 0xFF, 0x00, 0x00])
    rom[0x7FFC:0x7FFE] = bytes([0x00, 0x80])
    total = sum(rom) & 0xFFFF
    rom[0x7FDC:0x7FDE] = struct.pack("<H", total ^ 0xFFFF)
    rom[0x7FDE:0x7FE0] = struct.pack("<H", total)

    with open(path, "wb") as out:
        out.write(rom)


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


def whole_line(guest, mark, pattern, seconds=60):
    """The first line after `mark` matching `pattern`, once all of it has
    arrived - the pattern ends in the newline for that reason. None if it
    never does, which the check it feeds then says.

    Waiting for a phrase and then parsing the line it starts is a race: the
    master-mute check read 'mute: muted true level 256 2', the rest of the
    number still on its way from QEMU, and failed a machine that was right
    (`testing.md` 18.94).
    """
    deadline = time.monotonic() + seconds

    while time.monotonic() < deadline:
        guest._read_available()
        found = re.search(pattern, guest.seen[mark:])

        if found:
            return found

        time.sleep(0.2)

    return None


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    work = scratch.directory("media")
    wav_in = os.path.join(work, "tone.wav")
    disk = os.path.join(work, "disk.img")
    wav_out = os.path.join(work, "heard.wav")
    vbr_in = os.path.join(work, "vbr.mp3")
    cover_in = os.path.join(work, "cover.mp3")
    rom_in = os.path.join(work, "kosmos-test.sfc")
    film_in = os.path.join(work, "tiny.mp4")
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

    #
    # Four frames of flat grey, each a different one, at ten a second: the
    # smallest film that can be *watched* going by. The greys are what the
    # guest checks it decoded, so they are chosen far apart.
    #
    FILM_GREYS = [40, 120, 200, 96]
    mjpeg_mp4(film_in, FILM_GREYS, width=16, height=16, fps=10)
    mp3_with_cover(cover_in, COVER)
    snes_rom(rom_in)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", disk, "64",
                    wav_in + ":/home/tone.wav", vbr_in + ":/home/vbr.mp3",
                    cover_in + ":/home/cover.mp3",
                    rom_in + ":/home/kosmos-test.sfc",
                    film_in + ":/home/tiny.mp4"], check=True,
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

        #
        # **A film, decoded and looked at.**
        #
        # The kit is asked for the frame at four moments and gives back the
        # grey each one was made from - so this is the whole path: the
        # index read, a sample found by its offset, a JPEG decoded, and the
        # pixels landing where they were asked to land. Four flat greys,
        # far apart, so a frame off by one is not a near miss.
        #
        # `get` rather than a screendump, because what is in question here
        # is the kit rather than the compositor: the display harness has
        # its own phase for what reaches the screen.
        #
        film_program = (
            'local media = use("/lib/media.lua") '
            'local f, why = media.open("/home/tiny.mp4") '
            'if not f then print("film" .. ": no " .. tostring(why)) return end '
            'print("film" .. ": " .. f.codec .. " " .. f.width .. "x" .. f.height '
            '.. " " .. f.frames .. " frames " .. string.format("%.2f", f.duration) '
            '.. " s") '
            'local s = gfx.surface{ w = 32, h = 32 } '
            'local said = {} '
            'for i = 0, 3 do '
            '  f:draw(s, i * 0.1 + 0.02) '
            '  said[#said + 1] = (s:get(4, 4) & 0xff) '
            'end '
            'print("film" .. ": greys " .. table.concat(said, " ")) '
            'print("film" .. ": dropped " .. f.dropped) '
            'f:close() '
            'local bad, badwhy = media.open("/home/vbr.mp3") '
            'print("film" .. ": an mp3 opens as " .. tostring(bad ~= nil)) '
            'print("film" .. ": done")')

        guest.type('fs.write("/ramfs/film.lua", [[' + film_program + ']])')
        time.sleep(1.0)
        mark = len(guest.seen)
        guest.type("/ramfs/film.lua")
        guest.wait_for("film: done", "the video kit opened and decoded the film")

        heard = guest.seen[mark:]
        #
        # `Motion JPEG` is two words, so the codec is matched up to the
        # size rather than as one - and every line here ends `\r\n`, which
        # is why this file's other patterns say `\r?\n` rather than `$`.
        #
        opened = re.search(r"^film: (.+?) (\d+)x(\d+) (\d+) frames ([\d.]+) s",
                           heard, re.M)

        check(opened is not None and opened.group(1) == "Motion JPEG"
              and opened.group(2) == "16" and opened.group(3) == "16"
              and opened.group(4) == "4",
              "the kit should have opened a 16x16 Motion JPEG film of four "
              "frames, and said: %r"
              % (opened.group(0) if opened else heard[-200:]))

        greys = re.search(r"^film: greys ([\d ]+)", heard, re.M)
        got = [int(n) for n in greys.group(1).split()] if greys else []

        #
        # **Within a few levels, not exactly.** A JPEG is quantised - these
        # are DC-only blocks at a quantisation of 16 - so 40 comes back as
        # 40-ish, and a decoder that is right is not a decoder that is
        # lossless. Far enough apart that the wrong *frame* is never within
        # the tolerance.
        #
        close = (len(got) == len(FILM_GREYS)
                 and all(abs(a - b) <= 8 for a, b in zip(got, FILM_GREYS)))

        check(close, "the four frames should decode to about %s and came back "
                     "as %s" % (FILM_GREYS, got))

        check(re.search(r"^film: dropped 0\r?\n", heard, re.M) is not None,
              "the kit dropped a frame on a film of four, which it cannot "
              "have needed to: %r" % heard[-200:])

        #
        # The other half of one door: `media.open` on a song still answers
        # a player rather than being sent to the picture half.
        #
        check(re.search(r"^film: an mp3 opens as true\r?\n", heard, re.M)
              is not None,
              "`media.open` stopped opening an MP3 when it learned about films")

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
        a = whole_line(guest, mark, r"mute: muted (\w+) level (\d+) (\d+)\r?\n")
        during_mute, _ = settled(wav_out)

        guest.type('fs.write("/ramfs/muteb.lua", [[' + play_one(False, "unmuted") + ']])')
        time.sleep(1.0)
        mark = len(guest.seen)
        guest.type("/ramfs/muteb.lua")
        b = whole_line(guest, mark, r"mute: unmuted (\w+) level (\d+) (\d+)\r?\n")
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
    # **The Super Nintendo's View and Game menus**, Diego's on 19 September:
    # "Do the 2x option in snes emulator app menu and it will restart the
    # app", and "the emulator needs a pause / play mode". On the cartridge
    # above, which is ours, with the desktop's default face - so the menu
    # bar's titles are where 8-pixel glyphs put them: File at 4, View at 52,
    # Game at 100, each a glyph's width times its letters and sixteen
    # (`strips.spans`). The window manager says which title a press opened,
    # so a title missed by a face that changed is named rather than guessed.
    #
    # Game, then Pause: no frames while paused - the frame it paused at and
    # the frame it resumed at are the same after two seconds - and "Paused"
    # drawn, a box of 303030h over a picture that is otherwise black. Then P,
    # twice, the same from the keyboard, and frames having run in between.
    # View, then Double Size: a fresh Super Nintendo, 1024 by 960, and this
    # one gone; and from there Normal Size, back to 512 by 480.
    #
    guest = Guest(image, 600)

    try:
        guest.wait_for(PROMPT, "reached a shell")
        mark = len(guest.seen)
        guest.type("wm snes:/home/kosmos-test.sfc")

        def window_of(since, size, seconds=60):
            deadline = time.monotonic() + seconds
            pattern = (r"wm: window kosmos-test at (\d+),(\d+) %dx(\d+)"
                       % size)

            while time.monotonic() < deadline:
                guest._read_available()
                found = re.search(pattern, guest.seen[since:])

                if found:
                    return tuple(int(v) for v in found.groups())

                time.sleep(0.3)

            return None

        def line_after(since, pattern, seconds=20):
            deadline = time.monotonic() + seconds

            while time.monotonic() < deadline:
                guest._read_available()
                found = re.search(pattern, guest.seen[since:])

                if found:
                    return found

                time.sleep(0.3)

            return None

        placed = window_of(mark, 512)

        if placed is None:
            raise Failure("the Super Nintendo never opened a 512-wide window "
                          "on the test cartridge:\n" + guest.seen[mark:][-1200:])

        wx, wy, wh = placed
        strip = wh - 480

        check(re.search(r"snes: starting kosmos-test fresh\r?\n",
                        guest.seen[mark:]) is not None,
              "the Super Nintendo's first start on a disk with no saves did "
              "not say it was starting fresh:\n" + guest.seen[mark:][-600:])
        time.sleep(3.0)
        width, height, _ = parse_ppm(guest.screendump())

        def click(cx, cy):
            guest.mouse_to(*_to_tablet(cx, cy, width, height))
            time.sleep(0.3)
            guest.mouse_button(True)
            time.sleep(0.2)
            guest.mouse_button(False)
            time.sleep(0.6)

        # A menu's rows are the face's height and six (`menu_metrics` in
        # `ui.lua`): 22 with the default face, the first from 2.
        def choose(title, offset, marker, item=1):
            before = len(guest.seen)
            click(wx + offset + 10, wy + strip // 2)
            opened = line_after(before, r"wm: menu bar (\w+) of kosmos-test "
                                        r"at (\d+),(\d+)")

            if opened is None or opened.group(1) != title:
                raise Failure("a press where %s should be in the Super "
                              "Nintendo's menu bar opened %s:\n%s"
                              % (title, opened.group(1) if opened else
                                 "nothing", guest.seen[before:][-600:]))

            time.sleep(1.0)
            click(int(opened.group(2)) + 20,
                  int(opened.group(3)) + 2 + (item - 1) * 22 + 11)
            said = line_after(before, marker)

            if said is None:
                raise Failure("%s, then its item, did not reach the Super "
                              "Nintendo:\n%s" % (title, guest.seen[before:][-600:]))

            return said

        def boxed():
            _, _, px = parse_ppm(guest.screendump())
            count = 0

            for y in range(wy + strip, min(wy + wh, height)):
                row = y * width * 3

                for x in range(wx, min(wx + 512, width)):
                    o = row + x * 3

                    if px[o] == 0x30 and px[o + 1] == 0x30 and px[o + 2] == 0x30:
                        count += 1

            return count

        paused = choose("Game", 100, r"snes: paused at frame (\d+)")
        time.sleep(1.0)
        box = boxed()
        time.sleep(2.0)
        resumed = choose("Game", 100, r"snes: resumed at frame (\d+)")

        check(int(paused.group(1)) > 0,
              "the Super Nintendo paused at frame 0 - it had not run the "
              "cartridge at all before the pause")
        check(resumed.group(1) == paused.group(1),
              "paused at frame %s and resumed at frame %s, two seconds later: "
              "a paused console went on running"
              % (paused.group(1), resumed.group(1)))
        check(box >= 1000,
              "%d pixels of the Paused box on the screen while paused, over "
              "a black picture - the pause was not drawn" % box)

        time.sleep(2.0)
        check(boxed() == 0,
              "the Paused box was still on the screen two seconds after "
              "Resume - the frames did not start again over it")

        before = len(guest.seen)
        guest.sendkey("p")
        by_key = line_after(before, r"snes: paused at frame (\d+)")
        time.sleep(1.0)
        guest.sendkey("p")
        again = line_after(before, r"snes: resumed at frame (\d+)")

        check(by_key is not None and again is not None
              and int(by_key.group(1)) > int(resumed.group(1))
              and again.group(1) == by_key.group(1),
              "P did not pause and resume the Super Nintendo with frames run "
              "in between: resumed at %s, then %r and %r"
              % (resumed.group(1), by_key and by_key.group(0),
                 again and again.group(0)))

        before = len(guest.seen)
        choose("View", 52, r"snes: restarting at 2x, 1024 by 960")
        doubled = window_of(before, 1024)

        check(doubled is not None and doubled[2] - strip == 960,
              "View, then Double Size, did not open a 1024 by 960 Super "
              "Nintendo: %r" % (doubled,))
        check(re.search(r"\(snes\) ended", guest.seen[before:]) is not None,
              "the Super Nintendo at 1x did not end when the one at 2x "
              "started - there are two consoles")

        #
        # **And the 2x one carried on from where the 1x one was** (roadmap
        # 4g): the one closing keeps the machine and the cartridge's RAM
        # beside the ROM before the other starts, and the other continues
        # from the frame it was kept at - a fresh console would say frame 0,
        # or "fresh".
        #
        kept = line_after(before, r"snes: kept kosmos-test at frame (\d+), "
                                  r"a state of (\d+) KB, and the cartridge's "
                                  r"own save of (\d+) KB")
        went_on = line_after(before, r"snes: continuing kosmos-test from "
                                     r"frame (\d+)")

        check(kept is not None and went_on is not None
              and int(kept.group(1)) > 0
              and kept.group(1) == went_on.group(1),
              "Double Size did not continue the game where it was: %r, then %r"
              % (kept and kept.group(0), went_on and went_on.group(0)))
        check(kept is not None and kept.group(3) == "2",
              "the test cartridge's 2 KB of RAM was not kept: %r"
              % (kept and kept.group(0)))

        if doubled is not None:
            wx, wy, wh = doubled
            time.sleep(3.0)

            # Game, then Reset: the console's button - frames start again.
            reset = choose("Game", 100, r"snes: reset at frame (\d+)", item=2)
            time.sleep(2.0)

            before = len(guest.seen)
            choose("View", 52, r"snes: restarting at 1x, 512 by 480")
            check(window_of(before, 512) is not None,
                  "View, then Normal Size, on the 2x Super Nintendo did not "
                  "open a 512-wide one")

            again = line_after(before, r"snes: kept kosmos-test at frame (\d+)")
            back = line_after(before, r"snes: continuing kosmos-test from "
                                      r"frame (\d+)")

            check(again is not None and back is not None
                  and again.group(1) == back.group(1),
                  "Normal Size did not continue the game where it was: %r, "
                  "then %r" % (again and again.group(0), back and back.group(0)))
            print("snes: kept at frame %s and continued at %s; reset at frame "
                  "%s and kept at %s; a state of %s KB"
                  % (kept and kept.group(1), went_on and went_on.group(1),
                     reset.group(1), again and again.group(1),
                     kept and kept.group(2)), flush=True)
            check(again is not None
                  and int(again.group(1)) < int(reset.group(1)),
                  "Reset at frame %s, and the game was kept at frame %s "
                  "seconds later - the console did not start again"
                  % (reset.group(1), again and again.group(1)))
    except Failure as e:
        fails.append(str(e))
    finally:
        guest.close()

    #
    # **The two files, as this Mac reads them off the disk**: the cartridge's
    # RAM, 2 KB with the byte its code stored first; and the state, LakeSnes's
    # own format - "LSSF", its version, then its own length, which must be the
    # file's.
    #
    def from_disk(name):
        out = scratch.path(name)
        got = subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "get", disk,
                              "/home/" + name, out], capture_output=True,
                             cwd=os.path.dirname(HERE))

        if got.returncode != 0:
            return None

        with open(out, "rb") as f:
            return f.read()

    srm = from_disk("kosmos-test.srm")
    state = from_disk("kosmos-test.state")

    check(srm is not None and len(srm) == 2048 and srm[0] == SAVED_BYTE,
          "/home/kosmos-test.srm is not the cartridge's 2 KB with %02Xh "
          "first: %r" % (SAVED_BYTE, srm[:4] if srm else srm))
    check(state is not None and len(state) > 64 * 1024
          and state[0:4] == b"LSSF"
          and struct.unpack("<I", state[8:12])[0] == len(state),
          "/home/kosmos-test.state is not a LakeSnes state of its own "
          "length: %d bytes, %r" % (len(state or b""), (state or b"")[:12]))

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

    #
    # **The level bar, on the screen, at the level the keys set.** Drawn by
    # the window manager over everything - `docs/levels.html`, as Diego
    # approved it - and measured against what the window manager says the
    # level is, not merely found to be there: the white of the track's fill,
    # along its middle row, runs from its start to the knob, and the knob
    # sits where the level puts it. Three presses of volume down from the
    # top, so the bar is not simply full. Then, three and a half seconds
    # later, the corner is as it was before the key - faded, and nothing
    # left behind.
    #
    # Its own session, with the sound device the others use, because it
    # needs one to have a level at all, and a window of its own well away
    # from the corner.
    #
    level_prog = ("local ui = use('/lib/ui.lua') "
                  "local w = ui.window{ title = 'Level', w = 240, h = 140, "
                  "x = 200, y = 300 } "
                  "if w then w:run() end")

    guest = Guest(image, 600)

    try:
        guest.wait_for(PROMPT, "reached a shell")
        guest.type('fs.write("/ramfs/level.lua", [[' + level_prog + ']])')
        time.sleep(1.0)
        guest.type("wm /ramfs/level.lua")
        guest.wait_for("wm: window Level at", "the level bar's window opened")
        time.sleep(2.5)

        # wm.lua's osd: 300 by 74, fourteen from the right and ten below the
        # bar - and there is no bar in this session.
        OSD_W, OSD_H = 300, 74
        width, height, before_px = parse_ppm(guest.screendump())
        ox, oy = width - OSD_W - 14, 10

        def corner(px):
            return [bytes(px[((oy + y) * width + ox) * 3:
                             ((oy + y) * width + ox + OSD_W) * 3])
                    for y in range(OSD_H)]

        base = corner(before_px)
        pressed = len(guest.seen)

        for _ in range(3):
            guest.sendkey("volumedown")
            time.sleep(0.3)

        time.sleep(0.3)
        _, _, shown_px = parse_ppm(guest.screendump())
        guest._read_available()
        levels = re.findall(r"wm: volume down, (\d+) of 256",
                            guest.seen[pressed:])
        shown = corner(shown_px)

        row, run = shown[50], 0
        for x in range(48, OSD_W):
            if row[x * 3:x * 3 + 3] == b"\xff\xff\xff":
                run += 1
            elif run:
                break

        time.sleep(3.5)
        _, _, gone_px = parse_ppm(guest.screendump())
        gone = corner(gone_px)

        level = int(levels[-1]) if levels else None

        check(level is not None and shown != base,
              "a volume key drew no level bar in the top right corner: "
              "the key said %r" % (levels,))

        # Every press heard. The first level bar raised an error inside the
        # key handler, and the two presses after the first were never seen -
        # the bar had taken the keys down with it.
        check(len(levels) == 3,
              "three presses of volume down, and the window manager said %d "
              "of them %r - drawing the level bar is losing keys"
              % (len(levels), levels))

        if level is not None:
            # wm.lua: the track is 202 wide from x 48, filled to the level,
            # and the knob, 26 wide, centred on the fill's end - so the white
            # runs from 48 to the knob.
            tw = OSD_W - 48 - 50
            fw = max(6, int(tw * level / 256 + 0.5))
            kx = max(48, min(48 + tw - 26, 48 + fw - 13))

            check(abs(run - (kx - 48)) <= 3,
                  "the level bar's fill is %d pixels of white, where %d of "
                  "256 puts its knob at %d - the bar is not drawing the "
                  "level the key set" % (run, level, kx - 48))

        check(gone == base,
              "the level bar was still on the screen three and a half "
              "seconds after the last key - it fades after two")
    except Failure as e:
        fails.append(str(e))
    finally:
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on media.lua, heard:" % (len(fails), len(fails) + checks))
        for complaint in fails:
            print("  " + complaint)
        return 1

    print("PASS: %d checks on media.lua, heard (the Super Nintendo paused, resumed and switched between 1x and 2x from its menus and the P key, continuing the game where it was each time and starting it again from Reset, with its cartridge save and its state on the disk, Music's window with its cover, its larger title and a drawn play arrow, a cover read out of an MP3 and drawn, a variable-bitrate MP3's length and bitrate from its Xing header, the master muted to silence and back with its level kept, the level bar drawn at the level the keys set and gone after, a tone played, sought and "
          "finished at the prompt with the position following the sound, "
          "Music's Play and bar doing the same, Music saying why it could "
          "not list a folder, and the sound keeping real time on a device "
          "that keeps its own)." % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
