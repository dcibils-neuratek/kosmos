#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Music, the window, playing an album with no gap (`docs/player.md` P5).

An album of two songs is made here by Homebrew's ffmpeg. They are one 997 Hz
tone, 44.1 kHz and 16 bits, at half of full scale, six seconds long, cut in
two at frame 132904. That number is not a multiple of any FLAC block, so the
first song ends partway through a block. Each half is a FLAC with album,
artist and track tags, and the first carries a cover of four quarters in
four colours - so a cover drawn whole tells itself from a corner of it, and
from the one-colour square the window draws for an album with none. A second
folder holds a WAV album so the library has more than one.

QEMU's HDA writes what the guest plays to a WAV on this Mac, at 44.1 kHz and
16 bits as the songs are, so nothing is resampled. The Music app opens with
the first song (`wm music:/Home/Music/Tones/01-First.flac`), plays its album
from it, and the recording is held to three things:

  - **the tone is there and unbroken**: from the first loud sample, at
    least 5.9 of the 6.0 seconds are loud;
  - **it is 997 Hz**, counted by zero crossings, within 2 Hz;
  - **nothing at the song boundary**: a sampled sine obeys
    x[n+1] = 2 cos(w) x[n] - x[n-1], and the residual of that stays within
    a few steps of quantisation over every sample of the tone. One sample
    lost, doubled or zeroed where the songs join is a residual in the
    thousands.

The window is read too, from the serial line, where it says what it shows:
the badge names "FLAC 44.1 kHz / 16-bit" and the server's verdict on it, the
second song is heard, and the queue ends. Then Control-F and "sec" search
the library, and the window says what it found.

**The window on the screen**, as `docs/player.html` draws what is playing,
in IBM Plex Sans at 14 for the small text so the title's 18 is larger: the
cover out of the file, whole, in its 68-pixel square; the title larger than
the artist and album beside it; the play arrow a triangle rather than a
rectangle; and none of the grey a new surface is filled with showing
through. Then the play arrow is pressed on the album that ended, and the
second song has to be heard again - after a press beside it, which must
sound nothing. And nothing Music says on the serial line is anything but
what it says when all is well.

**A path that is not there** is the last desktop's, on the same machine:
`wm music:/Home/nowhere/song.mp3` has to name the path it could not find,
and read the library all the same.

Those came from `run_media.py`, whose diskless image carried the VOX pilot
until the pilot went: its window pressed play, was drawn in a scalable face
over a cover, and was pointed at a folder that is not there.

**Control**: the desktop stopped, the server's master muted, and the album
played again. The same tone check is run on that part of the recording and
must fail. If it passed, the check would be passing on something other than
the sound.

Each of the window's checks has its own, in the same manner - the check run
where it must fail: the play arrow's shape read while the muted album plays,
when the button holds two bars and no triangle; the cover's four colours
read in the same square on the last desktop, where nothing plays and the
square is the empty one; the ink counter run over the ground between the
cover and the title, where there is none; the grey's scan made to count the
band's own ground too, which it must find, so it is reading the window; the
press beside the arrow, which must sound nothing; and the list of what Music
says when all is well run over the last desktop's words, where it must
catch "there is nothing at".

The disk carries the Music app in /Home/System/Apps/Music and `media.elf` in
/Home/System/Media, as the install puts them (`installed.player_pairs`),
from the test userland's APPS.

Usage: run_player.py IMAGE APPS
"""

import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402

LUA = os.path.join(ROOT, "build", "host", "lua")
FFMPEG = shutil.which("ffmpeg") or "/opt/homebrew/bin/ffmpeg"

RATE = 44100
TONE = 997
TOTAL = 6 * RATE                 # frames in the album
CUT = 132904                     # where the first song ends: inside a FLAC block
AMPLITUDE = 0.5
WAV_HEADER = 44

LOUD = 0.25 * 32767              # a sample of the tone's peak region
LOUD_RUN = 5.9                   # seconds of tone the album must give, of 6.0
RESIDUAL_MOST = 96               # steps: quantisation is a handful; a gap is thousands

FIRST = "/Home/Music/Tones/01-First.flac"
NOWHERE = "/Home/nowhere/song.mp3"

#
# **Where `music.lua` draws what is playing**, in the window's own pixels
# from the corner the window manager names. The window draws its own header
# (`header = true`), so there is no kit's header to step under, and the band
# of what is playing starts `ui.layout.head` down. Cover, title and
# transport are `nowv:draw`'s and `place_now`'s numbers; when those move,
# these do.
#
BAND = 46                        # ui.layout.head: the band's top
COVER_X, COVER_Y, COVER_SIZE = 16, BAND + 14, 68
TITLE_X, TITLE_TOP, TITLE_BOTTOM = 98, BAND + 4, BAND + 48
PLAY_X, PLAY_Y, PLAY_SIZE = 164, BAND + 52, 32
GROUND = (91, BAND + 4)          # the band's ground: right of the cover, over the title
FILL_GREY = (0x20, 0x20, 0x20)   # what a new surface is filled with; in no palette
FACE = ('fs.write("/Home/Preferences/appearance", { palette = "dark", fonts = { '
        'ui = { font = "ibmplexsans", px = 14 } } }) print("music-face" .. "-ready")')

# Everything Music says on the serial line when nothing has gone wrong. A
# line it says that is none of these is a reason, which here is a fault.
SAID_WELL = ("playing ", "heard ", "the queue ended", "library ", "badge ",
             "search ", "opened ", "sorted by ")

# The cover's four quarters: none of them a colour `music.lua` gives an
# album without a picture (its SWATCHES), so the placeholder cannot pass.
QUARTERS = ("0xe03030", "0x30c040", "0x3050e0", "0xe0c020")


def ff(*args):
    subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y"] + list(args),
                   check=True, capture_output=True)


def make_album(work):
    """The two songs, a cover, and a WAV album in another folder."""
    tone = os.path.join(work, "tone.wav")
    cover = os.path.join(work, "cover.png")
    expr = "%g*sin(2*PI*%d*t)" % (AMPLITUDE, TONE)

    ff("-f", "lavfi", "-i", "aevalsrc=%s|%s:s=%d:d=6" % (expr, expr, RATE),
       "-c:a", "pcm_s16le", tone)
    ff("-f", "lavfi", "-i", "color=c=%s:s=64x64" % QUARTERS[0],
       "-vf", ",".join("drawbox=x=%d:y=%d:w=32:h=32:color=%s:t=fill" % (x, y, c)
                       for (x, y), c in zip(((32, 0), (0, 32), (32, 32)), QUARTERS[1:])),
       "-frames:v", "1", cover)

    first = os.path.join(work, "01-First.flac")
    second = os.path.join(work, "02-Second.flac")
    tags = ["-metadata", "album=Tones", "-metadata", "artist=The Oscillators",
            "-metadata", "album_artist=The Oscillators", "-metadata", "date=2026"]

    ff("-i", tone, "-i", cover, "-map", "0:a", "-map", "1:v",
       "-af", "atrim=end_sample=%d" % CUT,
       "-c:a", "flac", "-sample_fmt", "s16", "-c:v", "png", "-disposition:v", "attached_pic",
       "-metadata", "title=First", "-metadata", "track=1/2", *tags, first)
    ff("-i", tone, "-af", "atrim=start_sample=%d,asetpts=N/SR/TB" % CUT,
       "-c:a", "flac", "-sample_fmt", "s16",
       "-metadata", "title=Second", "-metadata", "track=2/2", *tags, second)

    other = os.path.join(work, "Elsewhere.wav")
    ff("-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=1",
       "-c:a", "pcm_s16le", other)

    return [first + ":" + FIRST,
            second + ":/Home/Music/Tones/02-Second.flac",
            other + ":/Home/Music/Elsewhere/01-Elsewhere.wav"]


def make_disk(image, disk, songs):
    """The suite's /Home: the songs, the Music app and `media.elf`. False,
    said, when `make apps` has not linked the image."""
    import installed

    arch = "x86_64" if "x86_64" in image else "aarch64"
    words = [w for w in sys.argv[2:] if not w.startswith("--")]
    apps = words[0] if words else os.path.join(
        ROOT, "build", "user-x86_64-test" if arch == "x86_64" else "user-test", "apps")
    pairs = installed.player_pairs(arch, apps)

    if not any(p.endswith("/media.elf") for p in pairs):
        print("FAIL: no media.elf in %s - `make apps` builds it" % apps)
        return False

    size = sum(os.path.getsize(p.split(":", 1)[0]) for p in pairs + songs) // (1024 * 1024)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", disk, str(size + 64)]
                   + songs + pairs, check=True, capture_output=True, cwd=ROOT)
    return True


def samples(wav, start, end=None):
    """The left channel of the recording, from byte `start` to `end`."""
    with open(wav, "rb") as f:
        f.seek(start)
        data = f.read() if end is None else f.read(max(0, end - start))

    data = data[:len(data) // 4 * 4]
    return [l for l, _ in struct.iter_unpack("<hh", data)]


def tone_check(left):
    """None when `left` holds the album's tone whole, else why not.
    Returns (why, facts)."""
    loud = [i for i, x in enumerate(left) if abs(x) >= LOUD]

    if not loud:
        return "no tone at all in %d frames recorded" % len(left), {}

    start, end = loud[0], loud[-1]
    tone = left[start:end + 1]
    seconds = len(tone) / RATE
    facts = {"seconds": seconds}

    if seconds < LOUD_RUN:
        return ("%.2f s of tone, not the album's %.1f" % (seconds, TOTAL / RATE)), facts

    # The frequency, counted between the first and the last upward crossing.
    ups = [i for i in range(1, len(tone)) if tone[i - 1] < 0 <= tone[i]]

    if len(ups) < 2:
        return "the tone never crosses zero", facts

    hz = (len(ups) - 1) * RATE / (ups[-1] - ups[0])
    facts["hz"] = hz

    if abs(hz - TONE) > 2:
        return "the tone is %.2f Hz, not %d" % (hz, TONE), facts

    # The recurrence, over every sample but the ramp-in and ramp-out the
    # device may add at the very ends.
    k = 2 * math.cos(2 * math.pi * TONE / RATE)
    worst, where = 0, 0

    for n in range(64, len(tone) - 64):
        r = abs(tone[n + 1] - k * tone[n] + tone[n - 1])

        if r > worst:
            worst, where = r, n

    facts["residual"], facts["at"] = worst, where / RATE

    if worst > RESIDUAL_MOST:
        return ("the tone breaks %.4f s in (%.0f steps off a sine; the songs join at %.4f s)"
                % (where / RATE, worst, CUT / RATE)), facts

    return None, facts


def settled(wav, quiet=1.5, most=30):
    """The recording's size once it has stopped growing for `quiet` s."""
    deadline = time.monotonic() + most
    size, since = -1, time.monotonic()

    while time.monotonic() < deadline:
        now = os.path.getsize(wav) if os.path.exists(wav) else 0

        if now != size:
            size, since = now, time.monotonic()
        elif time.monotonic() - since >= quiet:
            break

        time.sleep(0.2)

    return size


def play_album(R, guest, wav, seen_name, during=None):
    """The album played in the window: the recording's span and what was said.
    `during(seen)` is run once the window has said it is playing, while the
    album still is."""
    mark = os.path.getsize(wav) if os.path.exists(wav) else WAV_HEADER
    seen = len(guest.seen)
    guest.type("wm music:" + FIRST)

    if during:
        guest.wait_for_line("music: playing 1 of 2, ", "Music to start the album", seen)
        during(seen)

    guest.wait_for_line("music: heard Second", "Music to reach the second song", seen)
    guest.wait_for_line("music: the queue ended", "Music to play the album out", seen)
    end = settled(wav)
    return mark, end, seen


def loud_seconds(left):
    """How long the tone sounds in `left`: its first loud sample to its last."""
    loud = [i for i, x in enumerate(left) if abs(x) >= LOUD]
    return (loud[-1] - loud[0]) / RATE if loud else 0.0


def window_of(guest, since, seconds=60):
    """Where the window manager says Music's window is, said after `since`:
    (x, y, w, h), or None."""
    deadline = time.monotonic() + seconds

    while time.monotonic() < deadline:
        guest._read_available()
        found = re.search(r"wm: window Music at (-?\d+),(-?\d+) (\d+)x(\d+)",
                          guest.seen[since:])

        if found:
            return tuple(int(v) for v in found.groups())

        time.sleep(0.3)

    return None


def pixel(shot, place, dx, dy):
    """The colour at (dx, dy) of the window at `place`; None off the screen."""
    width, height, px = shot
    x, y = place[0] + dx, place[1] + dy

    if not (0 <= x < width and 0 <= y < height):
        return None

    o = (y * width + x) * 3
    return (px[o], px[o + 1], px[o + 2])


def cover_colours(shot, place):
    """The colours at the middles of the cover square's four quarters: four
    for the picture drawn whole, one for a corner of it or for the square of
    an album with none."""
    q = COVER_SIZE // 4
    return {pixel(shot, place, COVER_X + dx, COVER_Y + dy)
            for dy in (q, 3 * q) for dx in (q, 3 * q)}


def arrow_drawn(shot, place):
    """Whether the play button holds a triangle pointing right, as
    `arrow(g, p.x + 12, p.y + 9, 14)` draws it: the band's ground inside the
    triangle's body, and the button's own colour at a corner of the box
    round the triangle, which a rectangle would have filled. Paused bars put
    the button's colour where the body is, so they fail it too."""
    ground = pixel(shot, place, *GROUND)
    button = pixel(shot, place, PLAY_X + PLAY_SIZE // 2, PLAY_Y + 3)
    body = pixel(shot, place, PLAY_X + 15, PLAY_Y + PLAY_SIZE // 2)
    corner = pixel(shot, place, PLAY_X + 24, PLAY_Y + 11)

    return button != ground and body == ground and corner == button


def ink_rows(shot, place, x0, x1, ground):
    """Rows of the title's band, between columns x0 and x1, holding a pixel
    that is not the ground - `run_media.py`'s count, held against the ground
    rather than a brightness, since the artist is drawn dim."""
    rows = 0

    for y in range(TITLE_TOP, TITLE_BOTTOM):
        if any(pixel(shot, place, x, y) not in (ground, None) for x in range(x0, x1)):
            rows += 1

    return rows


def title_and_meta(shot, place):
    """The columns of the title and of the artist line after it, (x0, x1)
    each: split at the first run of eight empty columns, which no letter or
    space makes and the ten pixels `nowv:draw` puts between them always do."""
    ground = pixel(shot, place, *GROUND)
    runs, start, empty = [], None, 0
    x = TITLE_X - 2

    while x < TITLE_X + 600 and len(runs) < 2:
        if ink_rows(shot, place, x, x + 1, ground):
            if start is None:
                start = x
            empty = 0
        elif start is not None:
            empty += 1

            if empty >= 8:
                runs.append((start, x - empty + 1))
                start, empty = None, 0

        x += 1

    if start is not None and len(runs) < 2:
        runs.append((start, x))

    return runs, ground


def surface_grey(shot, place):
    """How many places in the window are the grey a new surface is filled
    with - and, for the control, how many in the band are its own ground."""
    ground = pixel(shot, place, *GROUND)
    grey = own = 0

    for dy in range(8, place[3] - 8, 4):
        for dx in range(8, place[2] - 8, 4):
            c = pixel(shot, place, dx, dy)

            grey += c == FILL_GREY
            own += c == ground and BAND <= dy < BAND + 96

    return grey, own


def strange(said):
    """What Music said that it does not say when all is well."""
    out = []

    for line in said.splitlines():
        if "music: " in line:
            rest = line.split("music: ", 1)[1].strip()

            if not rest.startswith(SAID_WELL):
                out.append(rest)

    return out


def click(R, guest, shot, place, dx, dy):
    """A press and a release at (dx, dy) of the window."""
    width, height, _ = shot
    guest.mouse_to(*R._to_tablet(place[0] + dx, place[1] + dy, width, height))
    time.sleep(0.3)
    guest.mouse_button(True)
    time.sleep(0.15)
    guest.mouse_button(False)
    time.sleep(0.3)


def look(R, guest, place, ready, seconds=15):
    """A picture of the screen once `ready(shot)` holds, or the last one
    taken when it never did - so the checks say what they saw rather than
    a timeout saying nothing."""
    deadline = time.monotonic() + seconds
    shot = R.parse_ppm(guest.screendump())

    while not ready(shot) and time.monotonic() < deadline:
        time.sleep(0.4)
        shot = R.parse_ppm(guest.screendump())

    return shot


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    work = scratch.directory("player")
    wav = os.path.join(work, "heard.wav")
    disk = os.path.join(work, "disk.img")
    checks, fails = 0, []

    def check(ok, complaint):
        nonlocal checks
        if ok:
            checks += 1
        else:
            fails.append(complaint)

    if not os.path.exists(FFMPEG):
        print("FAIL: no ffmpeg on this Mac to make the album with (brew install ffmpeg)")
        return 1

    if not make_disk(image, disk, make_album(work)):
        return 1

    os.environ["KOSMOS_DISK"] = disk
    os.environ["KOSMOS_AUDIO_WAV"] = wav
    import run_screenshot as R                                  # noqa: E402

    if R.machine(image) != "x86_64":
        print("FAIL: run_player.py runs on x86-64 only for now")
        return 1

    R.extra_args(image, ["-audiodev", "wav,id=snd0,path=%s" % wav,
                         "-device", "ich9-intel-hda",
                         "-device", "hda-output,audiodev=snd0"])

    guest = R.Guest(image, 180)
    facts, control = {}, None

    try:
        guest.wait_for("kosmos>", "a shell prompt")

        # A scalable face, the small text a size below the title's, as
        # `run_media.py`'s window phase chose it: the default is the same
        # face at one size for both, and "larger" would have nothing to see.
        guest.type(FACE)
        guest.wait_for("music-face-ready", "chose a scalable face")

        mark, end, seen = play_album(R, guest, wav, "the album")
        first_seen = seen
        said = guest.seen[seen:]

        why, facts = tone_check(samples(wav, mark, end))
        check(why is None, "the album as heard: %s" % why)

        # The library the window read: the song's folder first, so it plays
        # at once, then everything - two albums, three songs.
        guest.wait_for_line("music: library 3 songs", "Music to read the whole library", seen)
        said = guest.seen[seen:]
        lines = [l.split("music: library ", 1)[1].strip()
                 for l in said.splitlines() if "music: library " in l]
        check(len(lines) >= 2 and lines[0].startswith("2 songs, 1 album,")
              and lines[-1].startswith("3 songs, 2 albums"),
              "Music read the library as %r, not the song's folder (2 songs) and then "
              "everything (3 songs in 2 albums)" % lines)

        # The badge, as the window drew it.
        badges = [l.split("music: badge ", 1)[1].strip()
                  for l in said.splitlines() if "music: badge " in l]
        check(badges, "Music never said a badge: %r" % said[-1200:])

        if badges:
            b = badges[0]
            check(b.startswith("FLAC 44.1 kHz / 16-bit · "),
                  "the badge reads %r, not FLAC 44.1 kHz / 16-bit and a verdict" % b)
            check(b.endswith("bit-perfect") or "converted (" in b,
                  "the badge %r gives no verdict from the server" % b)
            facts["badge"] = b

        check("music: heard First" in said or "music: playing 1 of 2, First" in said,
              "Music did not start the album at its first song: %r" % said[-1200:])

        # Search: Control-F to the field, then three letters.
        seen = len(guest.seen)
        guest.proc.stdin.write(b"\x06")
        guest.proc.stdin.flush()
        time.sleep(0.5)
        guest.proc.stdin.write(b"sec")
        guest.proc.stdin.flush()
        line = guest.wait_for_line('music: search "sec": ', "Music to search", seen)
        check(line.startswith("1 song"),
              "searching \"sec\" found %r, not the one song Second" % line)

        #
        # **The window on the screen**, the album played out: what a person
        # would notice of `docs/player.html`'s band. The cover out of the
        # file itself, whole - four quarters, where a crop shows one colour
        # and the square of an album with no picture one more; the title
        # larger than the artist and album beside it; the play arrow a
        # triangle and not the staircase of fills a rectangle-only kit would
        # draw; and the window's own ground everywhere, not the grey a new
        # surface is filled with (Diego dragged the VOX pilot wider on 16
        # September and saw that grey where the design stopped).
        #
        place = window_of(guest, first_seen)

        if place is None:
            check(False, "Music's window was never placed:\n"
                  + guest.seen[first_seen:][-800:])
        else:
            shot = look(R, guest, place,
                        lambda s: len(cover_colours(s, place)) == 4 and arrow_drawn(s, place))
            colours = cover_colours(shot, place)

            check(len(colours) == 4,
                  "the cover in Music's window shows %d of its four colours (%r), so "
                  "the picture in the file did not reach the square whole"
                  % (len(colours), sorted(colours, key=str)))

            runs, ground = title_and_meta(shot, place)
            title = meta = 0

            if len(runs) == 2:
                title = ink_rows(shot, place, runs[0][0], runs[0][1], ground)
                meta = ink_rows(shot, place, runs[1][0], min(runs[1][1], runs[1][0] + 60),
                                ground)

            check(len(runs) == 2 and title > meta,
                  "Music's title is %d rows of ink and the artist beside it %d (runs %r), "
                  "so the title is not larger than the text" % (title, meta, runs))

            between = ink_rows(shot, place, COVER_X + COVER_SIZE + 2, TITLE_X - 2, ground)
            check(between == 0 and title > 0,
                  "control: the ink counter found %d rows on the ground between the "
                  "cover and the title, and %d in the title, so it is not counting "
                  "letters" % (between, title))

            check(arrow_drawn(shot, place),
                  "the play button with the album ended does not hold a triangle: its "
                  "body %r, a corner of its box %r, the ground %r"
                  % (pixel(shot, place, PLAY_X + 15, PLAY_Y + PLAY_SIZE // 2),
                     pixel(shot, place, PLAY_X + 24, PLAY_Y + 11), ground))

            grey, own = surface_grey(shot, place)
            check(grey == 0,
                  "%d places in Music's window are the grey a new surface is filled "
                  "with, so part of the window is not being drawn" % grey)
            check(own > 0,
                  "control: the scan for that grey found none of the band's own ground "
                  "either, so it is not reading the window")

            #
            # **Play, pressed.** What `run_media.py`'s phase was for was
            # sound, and so is this: the album has ended, and the arrow has
            # to start it again - the queue's last song from its start, three
            # seconds of the tone. First a press beside it, on the band's
            # ground, which must sound nothing: a check that heard the tone
            # after any press would be hearing something other than the
            # arrow.
            #
            mark = os.path.getsize(wav)
            click(R, guest, shot, place, *GROUND)
            time.sleep(2.0)
            end = settled(wav, quiet=1.0, most=8)
            missed = loud_seconds(samples(wav, mark, end))
            check(missed < 0.1,
                  "control: a press beside the play arrow sounded %.2f s of the tone, "
                  "so what is heard is not the arrow's doing" % missed)

            mark = os.path.getsize(wav)
            pressed = len(guest.seen)
            click(R, guest, shot, place, PLAY_X + PLAY_SIZE // 2, PLAY_Y + PLAY_SIZE // 2)
            guest.wait_for_line("music: the queue ended",
                                "Music to play from its arrow and end again", pressed)
            end = settled(wav)
            again = loud_seconds(samples(wav, mark, end))
            facts["pressed"] = again
            check(again >= 2.5,
                  "pressing Music's play arrow on the album that ended sounded %.2f s "
                  "of the tone, where the last song is 3.0: none is a press that "
                  "missed, a fraction a song that started and stopped" % again)

        check(not strange(guest.seen[first_seen:]),
              "Music said something went wrong: %r" % strange(guest.seen[first_seen:]))

        # **Control**: the master muted, the album again, the tone check
        # on that span must fail. And while it plays, the play button is
        # read for the triangle, which must not be there: playing, it holds
        # two bars.
        R.stop_desktop(guest)
        guest.type('fs.write("/Temporary/mute.lua", [=[local audio = use("/Kosmos/Libraries/audio.lua") '
                   'audio.set{ master_muted = true } print("MUTED " .. tostring('
                   'audio.stats().master_muted))]=])')
        time.sleep(0.5)
        guest._read_available()
        seen = len(guest.seen)
        guest.type("/Temporary/mute.lua")
        line = guest.wait_for_line("MUTED ", "the master muted", seen)
        check(line == "true", "the master did not mute: %r" % line)
        playing = {}

        def while_playing(since):
            where = window_of(guest, since)

            if where:
                time.sleep(1.0)
                playing["shot"] = (R.parse_ppm(guest.screendump()), where)

        mark, end, _ = play_album(R, guest, wav, "the album muted", while_playing)
        control, _ = tone_check(samples(wav, mark, end))
        check(control is not None,
              "control: the tone check passed with the master muted, so it is "
              "not checking the sound")
        check("shot" in playing and not arrow_drawn(*playing["shot"]),
              "control: the play button read as a triangle while the album played, "
              "when it draws two bars - so the triangle check is not reading the "
              "arrow" if "shot" in playing else
              "control: Music's window was never placed while the album played")

        #
        # **A path that is not there, named.** The VOX pilot said "could not
        # list /Home/nowhere" and why, because on the ThinkPad a list that
        # failed and a folder with no music in it had looked the same. This
        # Music looks at what it was given before it opens: a path with
        # nothing at it is said, by name, and the library is read all the
        # same - the three songs of /Home/Music, not nothing.
        #
        R.stop_desktop(guest)
        seen = len(guest.seen)
        guest.type("wm music:" + NOWHERE)
        line = guest.wait_for_line("music: there is nothing at ",
                                   "Music to say there is nothing at the path", seen)
        check(line == NOWHERE,
              "Music named %r as not there, where it was given %s" % (line, NOWHERE))
        line = guest.wait_for_line("music: library ", "Music to read the library", seen)
        check(line.startswith("3 songs, 2 albums"),
              "given a path that is not there, Music read the library as %r, not the "
              "3 songs in 2 albums of /Home/Music" % line)

        # Its controls: the cover square with nothing playing is the empty
        # one, which the four-colour check must refuse; and the words of a
        # window that went wrong, which the list of what Music says when
        # all is well must catch.
        where = window_of(guest, seen)

        if where is None:
            check(False, "Music's window was never placed for the missing path:\n"
                  + guest.seen[seen:][-800:])
        else:
            time.sleep(1.5)
            empty = cover_colours(R.parse_ppm(guest.screendump()), where)
            check(len(empty) < 4,
                  "control: the cover square with nothing playing showed four colours "
                  "(%r), so the cover check is not reading the cover"
                  % sorted(empty, key=str))

        caught = strange(guest.seen[seen:])
        check(any(c.startswith("there is nothing at ") for c in caught),
              "control: the list of what Music says when all is well let %r through"
              % caught)
    except R.Failure as e:
        fails.append(str(e))
    finally:
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on Music playing an album:" % (len(fails), len(fails) + checks))
        for complaint in fails:
            print("  " + complaint)
        return 1

    print("PASS: %d checks on Music (two FLAC songs joined at frame %d: %.2f s of %d Hz "
          "heard at %.2f Hz, worst %.0f steps off a sine; badge %r; search found it; "
          "play pressed: %.2f s again; control muted: %s)"
          % (checks, CUT, facts.get("seconds", 0), TONE, facts.get("hz", 0),
             facts.get("residual", 0), facts.get("badge"), facts.get("pressed", 0), control))
    return 0


if __name__ == "__main__":
    sys.exit(main())
