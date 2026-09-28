#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Synth Kit, heard on the machine (`roadmap.md` 6zh, Groove).

A program opens the sound, hands the kit a song - a rim shot on every beat
at 120 a minute - starts the kit's thread on the stream's ring and lets it
play for three seconds, then asks it where it is and closes it. QEMU writes
what the device played to a WAV, and the beats have to be in it, half a
second apart: which is the thread running, rendering into the ring's slots,
the sequencer keeping time in samples, and nothing of the window in the
path.

What the engine sounds like is `test_synth.lua`'s, on the Mac; this is the
kit on the machine, with its thread.

Usage: run_synth.py IMAGE
"""

import os
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

synth.song{ bpm = 120, fx = { dRet = 0, rRet = 0 },
            tracks = { { type = "drum", vol = 1, kit = 1,
                         clips = { { len = 16, steps = { [7] = steps } } } } } }
synth.start(out.ring, out.rate)
synth.play()
sys.sleep(750)

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


def main():
    with open(os.path.join(WORK, "synthtest.lua"), "w") as f:
        f.write(PROGRAM)

    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", DISK, "16",
                    os.path.join(WORK, "synthtest.lua") + ":/Home/synthtest.lua"],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = DISK
    os.environ["KOSMOS_AUDIO_WAV"] = WAV

    import run_screenshot as R                               # noqa: E402

    guest = R.Guest(IMAGE, 120)
    failed, checks = [], 0

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
        time.sleep(1.5)
    finally:
        guest.close()

    beats = onsets(WAV) if os.path.exists(WAV) else []
    gaps = [b - a for a, b in zip(beats, beats[1:])]
    check(len(beats) >= 4, "fewer than four beats were heard: %r" % beats[:8])
    check(gaps and all(abs(g - BEAT) <= 441 for g in gaps[:5]),
          "the beats are not half a second apart: gaps of %r frames" % gaps[:8])

    if failed:
        print("FAIL: %d of %d checks on the Synth Kit, heard:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on the Synth Kit, heard on the machine (its thread "
          "rendering into the stream's ring, %d beats half a second apart, "
          "the engine playing when asked)" % (checks, len(beats)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
