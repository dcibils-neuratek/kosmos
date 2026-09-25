#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""A film's sound, heard, and the picture keeping time with it (`roadmap.md` 4e).

A film is made here with nothing but bytes that already exist: its picture
is thirty frames of Motion JPEG greys, and its sound is the first three
seconds of an AAC conformance stream (`al05_44`, 44.1 kHz stereo), copied
frame for frame out of the MP4 FFmpeg's test suite serves - no encoder
involved. FFmpeg's reference for that stream is what those frames decode
to, so it is also what the machine has to play.

QEMU writes whatever the guest plays to a WAV on this Mac (`virtio-sound`
on the ARM board, HDA on x86-64). The guest plays the film through
`/lib/video.lua` - the kit's clock, the AAC Kit, `sys.pcm`, the audio
server - and afterwards the WAV is laid against the reference: found where
it starts, and every sample of the three seconds held within two steps.
Both boards run their sound at 44.1 kHz, so nothing is resampled and the
comparison can be sample for sample. And the film took about as long as it
lasts, by the sound's clock.

Then it is paused and sought: the clock stands still while paused and at
2.0 after the seek, and about two seconds of sound come out rather than
three. Then the Video app opens it and says it can be heard.

Usage: run_film.py IMAGE
"""

import os
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402
from run_media import jpeg_grey                              # noqa: E402

LUA = os.path.join(ROOT, "build", "host", "lua")
AAC = os.path.join(ROOT, "build", "downloads", "aac-conformance")
RATE = 44100
FRAMES = 130                    # AAC frames: 130 x 1024 samples, 3.02 s
FPS, PICTURES = 10, 30          # the picture: 3.0 s
WAV_HEADER = 44

# The program the guest runs: open the film, play it by its own clock to
# its end, let the ring play out, and say what happened. Each line it
# prints is put together from pieces, so the line typed to start it - which
# the shell echoes - never holds the words the harness waits for.
PROGRAM = (
    'local video = use("/lib/video.lua") '
    'local f, why = video.open(args) '
    'if not f then print("FILM" .. "-ERR " .. tostring(why)) '
    'print("FILM" .. "-DONE") return end '
    'local heard, silent = f:audible() '
    'local hz = (fs.read("/dev/cpu") or {}).counter_hz or 1 '
    'local began = sys.ticks() '
    'f:play(0) '
    'while f:position() < f.duration '
    'and (sys.ticks() - began) / hz < f.duration + 20 do '
    'f:tick() sys.sleep(1) end '
    'local took = (sys.ticks() - began) / hz '
    'for _ = 1, 150 do f:tick() sys.sleep(1) end '
    'print(("FILM" .. " %s %s %.2f %.2f %d %s"):format('
    'f.sound and f.sound.codec or "none", tostring(heard), f.duration, '
    'took, f.voice_state and f.voice_state.frames_out or 0, '
    '(f.voice_state and f.voice_state.frames_in or 0) .. " " .. '
    'tostring(silent))) '
    'f:close() '
    'print("FILM" .. "-DONE")')


# Paused and sought: a second of the film, a second paused, then from 2.0 to
# the end. The clock must stand still while paused - bar the ring and the
# device playing out, about a fifth of a second - and stand at 2.0 after the
# seek; and about two seconds of sound come out rather than three.
SEEKS = (
    'local video = use("/lib/video.lua") '
    'local f = assert(video.open(args)) '
    'local hz = (fs.read("/dev/cpu") or {}).counter_hz or 1 '
    'local function run(seconds) local t0 = sys.ticks() '
    'while (sys.ticks() - t0) / hz < seconds do f:tick() sys.sleep(1) end end '
    'f:play(0) run(1.0) f:pause() local paused = f:position() '
    'run(1.0) local still = f:position() '
    'f:seek(2.0) local sought = f:position() f:play() '
    'local began = sys.ticks() '
    'while f:position() < f.duration '
    'and (sys.ticks() - began) / hz < 20 do f:tick() sys.sleep(1) end '
    'run(0.6) '
    'print(("SEEK" .. " %.3f %.3f %.3f"):format(paused, still, sought)) '
    'f:close() '
    'print("SEEK" .. "-DONE")')


def aac_frames():
    """The first FRAMES frames of `al05_44`, its config, and the reference."""
    subprocess.run([sys.executable, os.path.join(HERE, "fetch_conformance.py"),
                    "aac"], check=True, capture_output=True)
    mp4 = os.path.join(AAC, "al05_44.mp4")
    out = subprocess.run([LUA, os.path.join(HERE, "mp4index.lua"), mp4],
                         check=True, capture_output=True, text=True,
                         cwd=ROOT).stdout.splitlines()
    config = bytes.fromhex(out[0].split()[-1])
    data = open(mp4, "rb").read()
    frames = []

    for line in out[1:1 + FRAMES]:
        at, size = (int(w) for w in line.split())
        frames.append(data[at:at + size])

    ref = open(os.path.join(AAC, "al05_44.s16"), "rb").read()
    ref = struct.unpack("<%dh" % (FRAMES * 1024 * 2),
                        ref[:FRAMES * 1024 * 4])
    return config, frames, ref


def film(path, config, sound):
    """An MP4 of two tracks: greys at FPS, and those AAC frames."""
    pictures = [jpeg_grey(40 + (i * 37) % 180, 16, 16)
                for i in range(PICTURES)]

    def box(kind, body):
        return struct.pack(">I", len(body) + 8) + kind + body

    def full(kind, version, body):
        return box(kind, bytes([version, 0, 0, 0]) + body)

    def words(*n):
        return b"".join(struct.pack(">I", v) for v in n)

    def table(samples, first):
        at, offsets = first, []
        for s in samples:
            offsets.append(at)
            at += len(s)
        return (full(b"stsz", 0, words(0, len(samples),
                                       *[len(s) for s in samples]))
                + full(b"stsc", 0, words(1, 1, 1, 1))
                + full(b"stco", 0, words(len(offsets), *offsets))), at

    def trak(handler, scale, count, delta, entry, samples, first):
        rest, end = table(samples, first)
        stbl = box(b"stbl", full(b"stsd", 0, words(1) + entry)
                   + full(b"stts", 0, words(1, count, delta)) + rest)
        mdhd = full(b"mdhd", 0, words(0, 0, scale, count * delta)
                    + b"\0\0\0\0")
        hdlr = full(b"hdlr", 0, words(0) + handler + b"\0" * 12 + b"\0")
        return box(b"trak", box(b"mdia", mdhd + hdlr
                                + box(b"minf", stbl))), end

    visual = (b"\0" * 6 + struct.pack(">H", 1) + b"\0" * 16
              + struct.pack(">HH", 16, 16) + b"\0" * (78 - 28))
    jpeg_es = (b"\x03\x19" + b"\x00\x01" + b"\x00"
               + b"\x04\x11" + b"\x6c\x11" + b"\0\0\0" + words(0, 0)
               + b"\x06\x01\x02")
    video_entry = box(b"mp4v", visual + full(b"esds", 0, jpeg_es))

    # An AudioSampleEntry (14496-12 12.2.3): two channels, sixteen bits, the
    # rate as 16.16; then the `esds` - object type 0x40, MPEG-4 audio, a
    # stream type of audio, and the AudioSpecificConfig as its tag 5.
    audio = (b"\0" * 6 + struct.pack(">H", 1) + b"\0" * 8
             + struct.pack(">HHHH", 2, 16, 0, 0) + struct.pack(">I", RATE << 16))
    dsi = b"\x05" + bytes([len(config)]) + config
    dcd = b"\x40\x15" + b"\0\0\0" + words(0, 0) + dsi
    es = (b"\x00\x02\x00" + b"\x04" + bytes([len(dcd)]) + dcd
          + b"\x06\x01\x02")
    audio_entry = box(b"mp4a", audio + full(b"esds", 0,
                                            b"\x03" + bytes([len(es)]) + es))

    def moov(first):
        v, end = trak(b"vide", 1000, PICTURES, 1000 // FPS, video_entry,
                      pictures, first)
        a, _ = trak(b"soun", RATE, len(sound), 1024, audio_entry, sound, end)
        return box(b"moov", box(b"mvhd", b"\0" * 100) + v + a)

    ftyp = box(b"ftyp", b"isom" + words(0x200) + b"isommp41")
    head = len(ftyp) + len(moov(0)) + 8

    with open(path, "wb") as f:
        f.write(ftyp + moov(head)
                + box(b"mdat", b"".join(pictures) + b"".join(sound)))


def heard_since(path, mark):
    """The stereo samples QEMU has written since byte `mark` of the WAV."""
    with open(path, "rb") as f:
        data = f.read()[max(mark, WAV_HEADER):]

    n = len(data) // 4 * 2
    return struct.unpack("<%dh" % n, data[:n * 2])


def lay_against(heard, ref):
    """Where the reference starts in what was heard, and how well it fits.

    A half second into the reference, sixty-four frames that are not
    silence are looked for in the recording, each sample within two steps;
    from there every frame the two share is compared.
    """
    at = RATE // 2
    while at < len(ref) // 2 - 64 and not any(ref[2 * at:2 * at + 128]):
        at += 64

    window = ref[2 * at:2 * at + 128]

    for h in range(0, len(heard) // 2 - 64):
        if abs(heard[2 * h] - window[0]) > 2:
            continue
        if all(abs(heard[2 * h + k] - window[k]) <= 2 for k in range(128)):
            offset = h - at
            shared = far = worst = 0
            first = None
            for r in range(len(ref) // 2):
                x = r + offset
                if x < 0 or 2 * x + 1 >= len(heard):
                    continue
                shared += 1
                for c in (0, 1):
                    d = abs(heard[2 * x + c] - ref[2 * r + c])
                    worst = max(worst, d)
                    far += d > 2
                    if d > 2 and first is None:
                        first = (r, c, heard[2 * x + c], ref[2 * r + c],
                                 heard[2 * x - 2 + c: 2 * x + 6 + c: 2],
                                 ref[2 * r - 2 + c: 2 * r + 6 + c: 2])
            return offset, shared, far, worst, first

    return None


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    work = scratch.directory("film")
    wav = os.path.join(work, "heard.wav")
    disk = os.path.join(work, "disk.img")
    path = os.path.join(work, "sound.mp4")
    checks, fails = 0, []

    def check(ok, complaint):
        nonlocal checks
        if ok:
            checks += 1
        else:
            fails.append(complaint)

    config, sound, ref = aac_frames()
    film(path, config, sound)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", disk, "64",
                    path + ":/home/sound.mp4"], check=True,
                   capture_output=True, cwd=ROOT)

    os.environ["KOSMOS_DISK"] = disk
    os.environ["KOSMOS_AUDIO_WAV"] = wav
    import run_screenshot as R                                  # noqa: E402

    if R.machine(image) == "x86_64":
        R.extra_args(image, ["-audiodev", "wav,id=snd0,path=%s" % wav,
                             "-device", "ich9-intel-hda",
                             "-device", "hda-output,audiodev=snd0"])

    guest = R.Guest(image, 180)
    fit = None
    said = None

    try:
        guest.wait_for("kosmos>", "a shell prompt")
        guest.type('fs.write("/ramfs/film.lua", [=[' + PROGRAM + ']=])')
        time.sleep(1.0)

        mark = os.path.getsize(wav) if os.path.exists(wav) else WAV_HEADER
        seen = len(guest.seen)
        guest.type("/ramfs/film.lua /home/sound.mp4")
        guest.wait_for("FILM-DONE", "the film to play through")
        said = re.search(r"^FILM (\S+) (\S+) ([\d.]+) ([\d.]+) (\d+) "
                         r"(.*?)\r?$", guest.seen[seen:], re.M)
        check(said is not None, "the film did not play: %r"
              % guest.seen[seen:][-400:])

        if said:
            codec, heard, length, took = (said.group(1), said.group(2),
                                          float(said.group(3)),
                                          float(said.group(4)))
            handed = int(said.group(5))
            check(codec == "AAC-LC" and heard == "true",
                  "the film's sound is %s and heard is %s (%s)"
                  % (codec, heard, said.group(6)))
            # Every frame of the sound handed to the audio server - the
            # last one too, which `sys.pcm` used to keep back.
            check(handed == FRAMES * 1024,
                  "%d frames of sound were handed over, of %d (%s)"
                  % (handed, FRAMES * 1024, said.group(0).strip()))
            # By the sound's clock: the length of the film, and the device's
            # start - a ring of 186 ms and the device's own - on top.
            check(length - 0.05 <= took <= length + 1.0,
                  "a %.2f s film took %.2f s by its own clock" % (length, took))

        time.sleep(1.5)
        fit = lay_against(heard_since(wav, mark), ref)
        check(fit is not None,
              "the reference's sound was not found in what the machine played")

        if fit:
            offset, shared, far, worst, first = fit
            check(shared >= int(RATE * 2.9),
                  "only %.2f s of the film's sound came out, of %.2f"
                  % (shared / RATE, len(ref) / 2 / RATE))
            check(far == 0,
                  "%d samples of %d more than two steps from FFmpeg's "
                  "reference (the worst %d; the first at %.4f s, channel "
                  "%d: %s against %s)"
                  % (far, shared * 2, worst,
                     first[0] / RATE if first else 0,
                     first[1] if first else 0,
                     first[4] if first else "", first[5] if first else ""))

        # Paused, and sought.
        guest.type('fs.write("/ramfs/seek.lua", [=[' + SEEKS + ']=])')
        time.sleep(1.0)
        mark = os.path.getsize(wav)
        seen = len(guest.seen)
        guest.type("/ramfs/seek.lua /home/sound.mp4")
        guest.wait_for("SEEK-DONE", "the paused and sought film")
        row = re.search(r"^SEEK ([\d.]+) ([\d.]+) ([\d.]+)",
                        guest.seen[seen:], re.M)
        check(row is not None, "the paused film said nothing: %r"
              % guest.seen[seen:][-300:])

        if row:
            paused, still, sought = (float(row.group(i)) for i in (1, 2, 3))
            check(0.7 <= paused <= 1.3,
                  "a second into the film the clock said %.3f" % paused)
            check(still - paused <= 0.3,
                  "paused for a second, the clock went from %.3f to %.3f"
                  % (paused, still))
            check(1.95 <= sought <= 2.0,
                  "sought to 2.0, the clock said %.3f" % sought)

        time.sleep(1.5)
        after = heard_since(wav, mark)
        loud = sum(1 for i in range(len(after) // 2)
                   if abs(after[2 * i]) > 64 or abs(after[2 * i + 1]) > 64)
        seconds_heard = loud / RATE
        check(1.6 <= seconds_heard <= 2.5,
              "about two seconds of sound should have come out - one before "
              "the pause, one after the seek to 2.0 - and %.2f did"
              % seconds_heard)

        # And the Video app, on the same film.
        seen = len(guest.seen)
        guest.type("wm video:/home/sound.mp4")
        line = guest.wait_for_line("video: sound.mp4, ", "the Video app",
                                   seen)
        check("sound AAC-LC, heard" in line,
              "the Video app said %r, not that the sound is heard" % line)
    except R.Failure as e:
        fails.append(str(e))
    finally:
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on a film's sound:"
              % (len(fails), len(fails) + checks))
        for complaint in fails:
            print("  " + complaint)
        return 1

    offset, shared, far, worst, _ = fit
    print("PASS: %d checks on a film's sound (AAC-LC through /lib/video.lua, "
          "%.2f s of it heard within %d of FFmpeg's reference, a %.2f s film "
          "in %.2f s by its own clock, and the Video app hearing it)"
          % (checks, shared / RATE, worst, float(said.group(3)),
             float(said.group(4))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
