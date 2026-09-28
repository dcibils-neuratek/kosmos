#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""`/Devices/midi`, end to end (`roadmap.md` 6zg, step b; `usb.md` §12).

QEMU has no MIDI device, so the USB driver offers a virtual keyboard under
`opt/kosmos/midi=virtual` - one port each way, whose port gives back what is
sent to it - and this plays it through the `midi` program, which reaches
`/Devices/midi` through `midi.lua` because it declares `needs midi`:

- **the list**: the virtual keyboard, its id, and its ports by name;
- **there and back**: a note on and off, a controller, a bend and System
  Exclusive sent, and six events heard - the System Exclusive in its two
  packets - in the order sent, each exactly what was sent, their counter
  times rising;
- **refusals, each in its own words**: a cable the device has no port on, a
  device that is not there, and bytes that are not whole messages;
- **and nothing for a program that did not ask**: one run from `/Temporary`,
  with no `needs midi`, finds no `/Devices/midi` at all.

Then, on the ARM machine, **Groove plays from it** (step c): Groove opens on
its techno demo, stopped, and listens to every device; `midi play`, started
with it, waits until the virtual keyboard has a listener - `/Devices/midi`
says how many - and plays the Launchkey's first drum pad, note 36 on channel
10. QEMU writes what the sound device played to a WAV, which has to hold one
kick and nothing else: the note heard by the driver, put in Groove's page,
taken by its pass, turned into a voice by the Synth Kit and played. Groove
says what it heard.

**And how long it took** (`roadmap.md` 4i, step a): Groove says the note's
way from its key to the ear - to the window's pass, to the kit's thread, and
the frames queued ahead of it in the ring and in the device. The parts have
to add up, the key's own time has to have come through from the driver - a
window's part of exactly nothing means the note was timed from when Groove
posted it - and the ring's part has to be whole periods within its depth.
The milliseconds before the kit are QEMU's emulation and are not checked;
the queue is structure, and is what step b makes shorter.

On the ARM machine, which has no USB controller, the driver answers from the
wait it keeps when there is none; on x86 it is given one, so the answers come
from its whole wait - five endpoints since `/Devices/midi`, which the kernel
took four of until this (`kernel/thread.h`).

Usage: run_midi.py IMAGE
"""

import os
import re
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
X86 = "x86_64" in IMAGE
WAV = os.path.join(scratch.directory("midi"), "heard.wav")
RATE = 44100


SENT = [
    ("on channel 1 note 60 velocity 100"),
    ("off channel 1 note 60 velocity 0"),
    ("cc channel 1 controller 21 value 64"),
    ("bend channel 1 value 4096"),
    ("sysex F0 7E 7F"),
    ("sysex 06 01 F7"),
]


def left_of(path):
    with open(path, "rb") as f:
        data = f.read()[44:]
    frames = len(data) // 4
    return struct.unpack("<%dh" % (frames * 2), data[:frames * 4])[0::2]


def onsets(left, level=2000, gap=RATE // 4):
    """Frames where the sound first rises past `level` after a quarter of a
    second under it."""
    found, last = [], -gap

    for i, v in enumerate(left):
        if abs(v) > level:
            if i - last >= gap:
                found.append(i)
            last = i

    return found


def main():
    # The sound device, on the ARM machine, written to a WAV.
    if not X86:
        os.environ["KOSMOS_AUDIO_WAV"] = WAV

    import run_screenshot as R

    x86 = R.machine(IMAGE) == "x86_64"

    if x86:
        R.extra_args(IMAGE, ["-device", "qemu-xhci,id=usb0"])

    guest = R.Guest(IMAGE, 120)
    failed, checks = [], 0
    heard_way = []

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    def run(line, until, seconds=30):
        mark = len(guest.seen)
        guest.type(line)
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline and until not in guest.seen[mark:]:
            time.sleep(0.2)
            guest._read_available()

        return guest.seen[mark:]

    try:
        guest.wait_for("kosmos> ", "reached a prompt")

        said = run("midi", "ended")
        listed = re.search(r"midi: device (\d+), Virtual keyboard, over virtual: "
                           r"1 in \(Keys\), 1 out \(Keys\), 0 listening", said)
        check(listed, "the virtual keyboard is not listed with its ports:\n" + said[-600:])
        vid = listed.group(1) if listed else "1"

        said = run("midi try " + vid, "heard")
        events = re.findall(r"midi:\s+([\d.]+) ms  device (\d+) cable (\d+)  (.+)", said)
        check(len(events) == len(SENT) and "sent 5 messages, heard 6 events" in said,
              "not every message came back once:\n" + said[-900:])
        check([e[3].strip() for e in events] == SENT,
              "what came back is not what was sent, in order: %r" % [e[3] for e in events])
        check(all(e[1] == vid and e[2] == "0" for e in events),
              "the events do not say the virtual keyboard's id and its cable 0")
        times = [float(e[0]) for e in events]
        check(times == sorted(times) and len(times) > 1 and times[-1] > times[0],
              "the events' counter times do not rise: %r" % times)

        said = run("midi send %s 1 90 3C 64" % vid, "ended")
        check("the device has no port on that cable" in said,
              "a cable with no port was not refused:\n" + said[-400:])
        said = run("midi send 99 0 90 3C 64", "ended")
        check("there is no MIDI device with that number" in said,
              "a device that is not there was not refused:\n" + said[-400:])
        said = run("midi send %s 0 3C 64" % vid, "ended")
        check("those bytes are not whole MIDI messages" in said,
              "bytes that are not messages were not refused:\n" + said[-400:])

        if x86:
            check("refused the wait" not in guest.seen,
                  "the kernel refused the driver's wait on its five endpoints")

        # A program that did not declare `needs midi` has no `/Devices/midi`.
        run('fs.write("/Temporary/nomidi.lua", [[local m = use("/Kosmos/Libraries/midi.lua") '
            'local all, why = m.all() print("nomidi: " .. #all .. " " .. tostring(why))]]) '
            'print("no" .. "midi-written")', "nomidi-written")
        said = run("run /Temporary/nomidi.lua", "nomidi: ")
        check("nomidi: 0 " in said,
              "a program that did not ask for MIDI reached it:\n" + said[-400:])

        # Groove, played from the keyboard. Last, because the window
        # manager keeps the prompt.
        if not x86:
            mark = len(guest.seen)
            run("wm groove,midi:play %s 36 10" % vid, "midi: ", 120)
            time.sleep(1.5)
            guest._read_available()
            said = guest.seen[mark:]
            check("groove: MIDI in from Virtual keyboard Keys" in said,
                  "Groove did not open the virtual keyboard:\n" + said[-900:])
            check("midi: played note 36 on channel 10 of device %s, 1 listening" % vid in said,
                  "midi play did not find Groove listening:\n" + said[-900:])
            check("groove: MIDI heard, on ch10 36 100" in said,
                  "Groove did not say it heard the pad:\n" + said[-900:])
            way = re.search(r"groove: key to ear ([\d.]+) ms - ([\d.-]+) to the window's pass, "
                            r"([\d.-]+) to the kit, ([\d.]+) in the ring \((\d+) frames, "
                            r"(\d+) periods kept\), ([\d.]+) in the device \((\d+) frames\)", said)
            check(way, "Groove did not say the note's way to the ear:\n" + said[-900:])

            if way:
                heard_way.append(way.group(0)[len("groove: "):])
                total, window, kit, ring, device = (float(way.group(i)) for i in (1, 2, 3, 4, 7))
                ring_frames, kept = int(way.group(5)), int(way.group(6))
                check(abs(window + kit + ring + device - total) <= 0.3 and kit >= 0,
                      "the note's way does not add up: %s" % way.group(0))
                check(window > 0,
                      "the pad's own time did not reach the kit: %s" % way.group(0))
                check(ring_frames % 256 == 0 and 2 <= kept <= 8 and ring_frames <= kept * 256,
                      "the ring's part is not whole periods within those the kit kept: %s"
                      % way.group(0))
    finally:
        guest.close()

    if not x86:
        left = left_of(WAV) if os.path.exists(WAV) else []
        found = onsets(left)
        check(len(found) == 1,
              "Groove's sound is not one kick from the pad: onsets at %r of %d frames"
              % (found[:6], len(left)))

    if failed:
        print("FAIL: %d of %d checks on /Devices/midi:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on /Devices/midi, on %s (the virtual keyboard listed with "
          "its ports, six events back in order with rising times, three refusals "
          "in their own words, and nothing for a program that did not ask%s)"
          % (checks, "x86-64 with an xHCI" if x86 else "the ARM machine, no USB",
             "" if x86 else "; and Groove playing one kick from the keyboard's pad, "
                            "and saying how long it took: %s" % "".join(heard_way)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
