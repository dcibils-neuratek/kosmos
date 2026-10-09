#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Synth Kit and Groove, heard and seen on the machine (`roadmap.md` 6zh).

A program opens the sound, hands the kit a song - a rim shot on every beat
at 120 a minute - starts the kit's thread on the stream's ring and lets it
play for three seconds, then asks it where it is and closes it. QEMU writes
what the device played to a WAV, and the beats have to be in it, half a
second apart: which is the thread running, rendering into the ring's slots,
the sequencer keeping time in samples, and nothing of the window in the
path.

While it plays, the same bar is **exported**: rendered by an engine of the
kit's own into a region and written to the disk whole, which this Mac reads
back after - a WAV header saying what it holds, the rims on their beats,
and four seconds for the echoes after the song ends.

Then **Groove** opens on the house demo, playing, in Plex: its window has
to be PulseMusic's - the dark ground, the kick's red column, the play button
lit - with **its top bar for its title bar**, as Diego chose, the three at
the bar's right end; and the end of the WAV has to beat at 124 a minute,
which is the demo's kick heard through the window's song, the kit and the
device.

**And under heavy load**, with `--load` (`roadmap.md` 4i, step c; Diego:
"audio should be prioritized", "and not be jerky under heavy load"), a boot
of its own that plays the house demo while four `spin display` hold every
core in the band every program runs in. Groove reports what the guest
itself measured, and **each party has to have come back within what the
device holds** - four periods, 23.2 ms: the Synth Kit's thread between two
of its passes, and the audio server between two turns. A longer absence is
a gap on any device; a shorter one is covered. The kit has to be in the
audio band and to have come down from the whole ring. The kit's absence is
counted up to the report as well as between passes, so a thread held off
for good - which never makes a second pass - is seen too.

**Not the device's own count of periods that found it empty**, which is the
real thing on hardware: under QEMU its WAV writer drains the queue in
bursts, and an idle machine with nothing wrong counts a hundred of them. Not
the WAV's silences either: the server writes nothing for a lone empty
stream, and the writer waits for it, so a starved thread comes out as sound
*missing* - which the six seconds catch - and never as zeros.

**Four, one a core**, and not six: with six, Groove's own start shared the
display band with two spinners a core and took from 5 to 50 seconds to open
its window, and a suite that runs alone is paid for in full by the gate.
With four the sound thread still shares a core with one - its controls
bite as hard, 745 ms and 211 - and the suite takes 15 seconds.

**It runs on a quiet machine** (`arm-synth-load`, `alone` in `gate.py`):
inside the whole gate the Mac held the emulated machine off its processors
for 38 ms, longer than the device's whole buffer, which nothing inside the
guest can cause (18.127's lesson).

What the engine sounds like is `test_synth.lua`'s, on the Mac, and Groove's
Lua is `test_groove.lua`'s; this is the kit on the machine, with its
thread, and the application on top of it.

Usage: run_synth.py IMAGE [--load]
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

import scratch                                               # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
WORK = scratch.directory("synth")
DISK = os.path.join(WORK, "home.img")
WAV = os.path.join(WORK, "heard.wav")
LOADED = os.path.join(WORK, "loaded.wav")
SPINNERS = 4
REPORT_S = 8
LUA = os.path.join(ROOT, "build", "host", "lua")
RATE = 44100
BEAT = RATE // 2                        # 120 a minute

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Synth Kit's check on the machine (`tools/run_synth.py`).
local audio = use("/Kosmos/Libraries/audio.lua")
local synth = use("/Kosmos/Kits/synth")
local out, why = audio.open("synthtest", 8)

if not out then print("synthtest: " .. tostring(why)) return end

local steps = {}
for s = 1, 64 do steps[s] = (s % 4 == 1) and 1 or 0 end

local bar = { bpm = 120, fx = { dRet = 0, rRet = 0 }, chain = { { scene = 1, bars = 1 } },
             tracks = { { type = "drum", vol = 1, kit = 1,
                          clips = { { len = 16, steps = { [7] = steps } } } } } }

synth.song(bar)
synth.start(out.ring, out.rate)
synth.play()
sys.sleep(375)

-- The same bar exported while the thread plays: into a region, then to the
-- disk whole.
local room = 44 + 8 * 44100 * 4
local cap = sys.memory((room + 4095) // 4096)
local at = cap and sys.memory_map(cap)
local len, secs, cut = synth.export(bar, at, room)
local wrote = fs.write_from("/Home/export.wav", cap, len)

sys.release(cap)
print(("synthtest: exported %s bytes, %.2f seconds, cut %s, wrote %s")
      :format(tostring(len), secs or -1, tostring(cut), tostring(wrote)))
sys.sleep(375)

local st = synth.state()

print(("synthtest: heard step %s, playing %s"):format(tostring(st.step and math.floor(st.step)),
                                                     tostring(st.playing)))
synth.stop()
synth.close()
out:close()
print("synthtest" .. ": done")
"""


def onsets(path, level=2000, gap=BEAT // 2):
    """Frames where the left side first rises past `level` after a quiet
    `gap` - the beats."""
    with open(path, "rb") as f:
        data = f.read()[44:]

    frames = len(data) // 4
    left = struct.unpack("<%dh" % (frames * 2), data[:frames * 4])[0::2]
    found, last = [], -gap

    for i, s in enumerate(left):
        if abs(s) > level:
            if i - last >= gap:
                found.append(i)
            last = i

    return found


def envelope(left, block):
    return [max(abs(v) for v in left[i:i + block]) for i in range(0, len(left) - block, block)]


def beat_of(left, low=0.3, high=0.7, block=RATE // 200):
    """The strongest period between `low` and `high` seconds in the sound's
    envelope, by autocorrelation: the kick's, where there is one."""
    env = envelope(left, block)
    mean = sum(env) / max(1, len(env))
    env = [e - mean for e in env]
    best, lag_best = None, None

    for lag in range(int(low * RATE / block), int(high * RATE / block) + 1):
        score = sum(env[i] * env[i + lag] for i in range(len(env) - lag))

        if best is None or score > best:
            best, lag_best = score, lag

    return lag_best * block / RATE if lag_best else None


def left_of(path, start=44):
    with open(path, "rb") as f:
        data = f.read()[start:]
    frames = len(data) // 4
    return struct.unpack("<%dh" % (frames * 2), data[:frames * 4])[0::2]


def colours(ppm, box=None):
    """How many pixels of each exact colour a screendump holds, or the part
    of it in `box` - x0, y0, x1, y1."""
    import run_screenshot as R
    w, h, rgb = R.parse_ppm(ppm)
    x0, y0, x1, y1 = box or (0, 0, w, h)
    counts = {}

    for y in range(y0, y1):
        row = y * w * 3

        for i in range(row + x0 * 3, row + x1 * 3, 3):
            c = (rgb[i], rgb[i + 1], rgb[i + 2])
            counts[c] = counts.get(c, 0) + 1

    return counts, (x1 - x0) * (y1 - y0)


# Groove's palette, as `groove/ui.lua` turns PulseMusic's colours into bytes.
# Measured on its first picture at 1920 by 1080: the panels 57% of the
# screen, the ground between them 6%, the kick's column 19,560 pixels and
# the play button and meters 7,138 - the thresholds are about half of each.
PANEL = (33, 34, 40)
GROUND = (22, 23, 27)
KICK_RED = (255, 107, 107)
PLAY_GREEN = (89, 235, 140)

# Close and minimise, as the window manager draws them on the window in
# front; maximise is greyed on a window that cannot be resized.
AMBER = (254, 188, 46)
RED = (255, 95, 87)


def groove_seen(counts, total):
    """What a playing Groove on the house demo has to show: each a pass and
    what it would say if it failed."""
    panel, ground = counts.get(PANEL, 0), counts.get(GROUND, 0)
    red, green = counts.get(KICK_RED, 0), counts.get(PLAY_GREEN, 0)

    return [
        (panel >= total // 4, "Groove's panels cover %d pixels, not a quarter of the screen" % panel),
        (ground >= total // 40, "Groove's dark ground covers %d pixels, not a fortieth" % ground),
        (red >= 8000, "the kick's red column is %d pixels" % red),
        (green >= 3000, "the play button and meters are %d pixels of green" % green),
    ]


def played_seconds(path):
    """How much sound the device played, from the first sound to the last."""
    with open(path, "rb") as f:
        data = f.read()[44:]

    frames = len(data) // 4
    both = struct.unpack("<%di" % frames, data[:frames * 4])
    first = next((i for i, v in enumerate(both) if v != 0), None)

    if first is None:
        return 0.0

    last = frames - 1

    while last > first and both[last] == 0:
        last -= 1

    return (last - first + 1) / RATE


def loaded(R, check):
    """Groove's house demo with every core spun in the display band."""
    R.use_audiodev("wav,id=snd0,path=%s" % LOADED)
    guest = R.Guest(IMAGE, 120)
    said = ""

    try:
        guest.wait_for("kosmos> ", "reached a prompt, for the loaded run")
        mark = len(guest.seen)
        guest.type("wm groove:--house --play --report %d" % REPORT_S
                   + ",spin:60 display" * SPINNERS)
        deadline = time.monotonic() + 150

        while time.monotonic() < deadline and ("groove: after %d s" % REPORT_S) not in guest.seen[mark:]:
            time.sleep(0.3)
            guest._read_available()

        time.sleep(0.5)
        guest._read_available()
        said = guest.seen[mark:]
    finally:
        guest.close()

    started = said.count("started /Kosmos/Programs/spin.lua")
    check(started == SPINNERS,
          "%d spinners started, not %d:\n%s" % (started, SPINNERS, said[-900:]))
    check("groove: the sound's thread in the audio band" in said,
          "Groove's sound thread is not in the audio band:\n" + said[-900:])
    report = re.search(r"groove: after \d+ s: the device holds ([\d.]+) ms; "
                       r"the kit's worst pass ([\d.]+) ms, the audio server's worst turn ([\d.]+) ms; "
                       r"the kit (?:not )?in the audio band, (\d+) periods kept, its ring ran dry \d+ times; "
                       r"DSP \d+%; the device ran dry -?\d+ times", said)
    check(report, "Groove did not report how its sound held:\n" + said[-900:])

    if report:
        holds, kit, server = (float(report.group(i)) for i in (1, 2, 3))

        # **What the machine was held off is not the scheduler's.** The Mac
        # stopping the emulated machine stops both parties at once - 32.6 and
        # 32.7 ms once, 49.7 and 50.3 once (18.269, 18.271) - and nothing
        # inside it can cause or cure that (18.127). The machine's own ticks
        # say how long: they come late by the stall, where a thread the
        # scheduler starved leaves them on time (`roadmap.md` 4i-f). So each
        # party is held to the device's time past what the machine was held
        # off. It replaced a ceiling of twice the device's time on a shared
        # absence, which was a guess about how long the Mac can stall.
        held = re.search(r"groove: the machine was held off ([\d.]+) ms", said)
        check(held is not None,
              "Groove did not say how long the machine was held off:\n" + said[-900:])
        off = float(held.group(1)) if held else 0.0
        check(holds > 0 and kit - off < holds,
              "the kit's thread was away %.1f ms under load, and the machine held "
              "off %.1f of it - longer than the device's %.1f: the sound skipped"
              % (kit, off, holds))
        check(holds > 0 and server - off < holds,
              "the audio server was away %.1f ms under load, and the machine held "
              "off %.1f of it - longer than the device's %.1f: the sound skipped"
              % (server, off, holds))
        check(int(report.group(4)) < 8,
              "the kit kept the whole ring under load: %s" % report.group(0))

    # How much was played is QEMU's device's pace, not the guest's: the WAV
    # came out at 5.5 s of 8 in one gate with every guest-side number
    # right, and the kit's own rendered seconds say the same. Said, not held.
    played = played_seconds(LOADED) if os.path.exists(LOADED) else 0.0

    depths = re.search(r"groove: (the kit keeps [^\n]*)", said)
    held = re.search(r"groove: (the machine was held off [\d.]+ ms)", said)

    return ((report.group(0)[len("groove: "):] if report else "no report")
            + ("; " + depths.group(1).strip() if depths else "")
            + ("; " + held.group(1) if held else ""), played)


def main_loaded():
    os.environ["KOSMOS_AUDIO_WAV"] = LOADED

    import run_screenshot as R                               # noqa: E402

    failed, checks = [], 0

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    held, played = loaded(R, check)

    if failed:
        print("FAIL: %d of %d checks on Groove's sound under load:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on Groove's sound under %d display-band spinners (%.1f s played - %s)"
          % (checks, SPINNERS, played, held))
    return 0


def main():
    if "--load" in sys.argv[2:]:
        return main_loaded()

    with open(os.path.join(WORK, "synthtest.lua"), "w") as f:
        f.write(PROGRAM)

    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", DISK, "16",
                    os.path.join(WORK, "synthtest.lua") + ":/Home/synthtest.lua"],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = DISK
    os.environ["KOSMOS_AUDIO_WAV"] = WAV

    import run_screenshot as R                               # noqa: E402

    redrawn = []
    guest = R.Guest(IMAGE, 120)
    failed, checks = [], 0
    shot = None

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        guest.type("run /Home/synthtest.lua")
        deadline = time.monotonic() + 90

        while time.monotonic() < deadline and "synthtest: done" not in guest.seen[mark:]:
            time.sleep(0.2)
            guest._read_available()

        said = guest.seen[mark:]
        check("synthtest: done" in said,
              "the program did not finish:\n" + said[-1200:])
        check("playing true" in said,
              "the kit's engine was not playing when asked:\n" + said[-600:])
        check("cut false, wrote" in said and "wrote nil" not in said,
              "the export was not rendered and written whole:\n" + said[-600:])
        time.sleep(1.5)

        # Plex, whose windows wear no title bar when they have a header of
        # their own: Groove's bar is then its title bar.
        guest.type('fs.send("/Home/Preferences", { type = "mkdir" }) '
                   'fs.write("/Home/Preferences/appearance", { palette = "plex" }) '
                   'print("plex" .. "-set")')
        guest.wait_for("plex-set", "chose Plex for the window manager")

        mark = len(guest.seen)
        guest.type("wm groove:--house --play --redraw-check --report 12")
        deadline = time.monotonic() + 90

        while time.monotonic() < deadline and "groove: " not in guest.seen[mark:]:
            time.sleep(0.2)
            guest._read_available()

        # **Redrawn only where it changed, and checked** (6zh): every frame
        # drawn whole as well and the pixels compared, while the song plays
        # and while a person moves along the bar, picks a clip and opens the
        # three dots' menu and closes it by clicking elsewhere.
        time.sleep(2)
        guest._read_available()

        def at(x, y):
            guest.mouse_to(x * 32767 // 1920, y * 32767 // 1080)

        def click(x, y):
            at(x, y)
            time.sleep(0.1)
            guest.mouse_button(True)
            time.sleep(0.1)
            guest.mouse_button(False)
            time.sleep(0.6)

        for x in range(200, 1000, 80):
            at(x, 24)
            time.sleep(0.15)

        click(60, 200)                       # a clip of the kick's, which stays selected

        # **A scene launches on the press**, as Ableton's does (Diego, 9
        # October: played live, the press is the moment): said while the
        # button is still down.
        guest._read_available()
        scenes = re.findall(r"groove: scenes at (\d+),(\d+), rows of (\d+)", guest.seen[mark:])
        launched_on_press = None
        if scenes:
            sx, sy, sh = (int(v) for v in scenes[-1])
            at(sx + 10, sy + sh // 2)
            time.sleep(0.2)
            down_at = len(guest.seen)
            guest.mouse_button(True)
            time.sleep(1.0)
            guest._read_available()
            launched_on_press = "groove: scene 1 launched" in guest.seen[down_at:]
            guest.mouse_button(False)
            time.sleep(0.6)
        lights = re.search(r"Groove's three at (\d+),15 in it", guest.seen[mark:])

        if lights:
            click(int(lights.group(1)) - 25, 24)
            time.sleep(0.4)
            click(960, 640)

        while time.monotonic() < deadline and "redraw check" not in guest.seen[mark:]:
            time.sleep(0.3)
            guest._read_available()

        time.sleep(0.5)
        guest._read_available()
        said = guest.seen[mark:]
        checked = re.search(r"redraw check: (\d+) frames drawn both ways, (\d+) pixels differ", said)
        check(checked and int(checked.group(1)) >= 20 and checked.group(2) == "0",
              "Groove's frames drawn where they changed are not what a whole redraw draws: %s"
              % (checked and checked.group(0)))
        share = re.search(r"of (\d+) frames (\d+) drawn whole; ([\d.]+)% of the window", said)
        if checked and share:
            redrawn.append("%s frames checked, %s pixels differing, %s%% of the window drawn a frame"
                           % (checked.group(1), checked.group(2), share.group(3)))
        check(share and int(share.group(2)) <= 6 and float(share.group(3)) < 30,
              "Groove still draws most of its window a frame: %s" % (share and share.group(0)))
        check(launched_on_press is True,
              "a scene was not launched on the press: %r" % launched_on_press)
        check("playing into the audio stream" in said,
              "Groove did not open playing into the audio stream:\n" + said[-800:])
        check("window Groove at 0,0 1920x1080, its header the title bar" in said,
              "Groove's bar is not its title bar in Plex:\n" + said[-800:])
        three = re.search(r"Groove's three at (\d+),15 in it", said)
        check(three and 1920 - 120 < int(three.group(1)) < 1920 - 40,
              "the three are not at the right end of Groove's bar: %s" % (three and three.group(0)))
        shot = guest.screendump()

        # **And the menu's other size**, which starts Groove again with its
        # song carried: on this screen the 1920x1080 window is the whole
        # work area again, but it is a new Groove all the same. Six seconds
        # after, so the WAV's last six are the new one playing.
        if lights:
            mark = len(guest.seen)
            x = int(lights.group(1))
            click(x - 25, 24)
            time.sleep(0.4)
            click(x - 232 + 110, 91)
            carried_by = time.monotonic() + 60

            while (time.monotonic() < carried_by
                   and "groove: the song carried across" not in guest.seen[mark:]):
                time.sleep(0.3)
                guest._read_available()

            time.sleep(7)
            guest._read_available()
            again = guest.seen[mark:]
            check("groove: the song carried across, at 1920x1080" in again
                  and "playing into the audio stream" in again,
                  "the menu did not start Groove again with its song:\n" + again[-800:])
        else:
            check(False, "Groove's three were not found, so neither was its menu")
    finally:
        guest.close()

    if shot:
        counts, total = colours(shot)
        for ok, why in groove_seen(counts, total):
            check(ok, why)
        # The bar at the very top, with no tab above it, and the three in it.
        top, cells = colours(shot, (0, 0, 1800, 4))
        check(top.get(PANEL, 0) >= cells * 9 // 10,
              "a tab is above Groove's bar: its top rows are %d of %d its panel"
              % (top.get(PANEL, 0), cells))
        band, _ = colours(shot, (1920 - 80, 0, 1920, 48))
        check(band.get(AMBER, 0) >= 40 and band.get(RED, 0) >= 40,
              "the three are not in Groove's bar: %d amber, %d red"
              % (band.get(AMBER, 0), band.get(RED, 0)))
    else:
        check(False, "no picture of Groove was taken")

    # The export, off the disk: its header, its rims and its tail.
    exported = os.path.join(WORK, "export.wav")
    got = subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "get", DISK,
                          "/Home/export.wav", exported], capture_output=True, cwd=ROOT)
    check(got.returncode == 0 and os.path.exists(exported),
          "the exported WAV is not on the disk: " + got.stderr.decode(errors="replace")[-300:])

    if got.returncode == 0 and os.path.exists(exported):
        with open(exported, "rb") as f:
            head = f.read(44)
        riff, size, wave, fmt, fmt_len, pcm, channels, rate, _, align, bits, data, data_len = \
            struct.unpack("<4sI4s4sIHHIIHH4sI", head)
        length = os.path.getsize(exported)
        check(riff == b"RIFF" and wave == b"WAVE" and fmt == b"fmt " and data == b"data"
              and pcm == 1 and channels == 2 and rate == RATE and bits == 16 and align == 4
              and size == length - 8 and data_len == length - 44,
              "the exported WAV's header does not say what it holds: %r" % (head,))
        rims = onsets(exported)
        check(len(rims) == 4 and all(abs(r - i * BEAT) <= 441 for i, r in enumerate(rims)),
              "the exported bar's rims are at %r, not on its four beats" % rims)
        seconds = (length - 44) / 4 / RATE
        check(5.9 <= seconds <= 6.2,
              "the export is %.2f seconds, not the bar's two and four for the echoes" % seconds)

    beats = onsets(WAV) if os.path.exists(WAV) else []
    gaps = [b - a for a, b in zip(beats, beats[1:])]
    check(len(beats) >= 4, "fewer than four beats were heard: %r" % beats[:8])
    check(gaps and all(abs(g - BEAT) <= 441 for g in gaps[:5]),
          "the beats are not half a second apart: gaps of %r frames" % gaps[:8])

    # Groove's last six seconds: the house demo's kick, 124 a minute.
    if os.path.exists(WAV):
        tail = left_of(WAV)[-RATE * 6:]
        beat = beat_of(tail) if max((abs(v) for v in tail), default=0) > 2000 else None
        check(beat is not None and abs(beat - 60 / 124) <= 0.01,
              "Groove's house demo does not beat at 124 a minute: %s" % beat)

    if failed:
        print("FAIL: %d of %d checks on the Synth Kit and Groove:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on the Synth Kit and Groove, on the machine (its thread "
          "rendering into the stream's ring, %d beats half a second apart, "
          "the engine playing when asked, a bar exported whole, and Groove "
          "drawn as PulseMusic, its bar its title bar, and beating at 124; drawn only "
          "where it changed: %s)" % (checks, len(beats), "".join(redrawn) or "?"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
