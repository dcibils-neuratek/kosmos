#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""`make test`: every suite, side by side, in five to ten minutes.

Diego, 18 September 2026, after a gate that ran for forty: "i dont want 40
minutes tests any more, 5 to 10 minutes max from now on so make sure the
tests are built accordingly". **The same checks as ever, in less time -
never fewer checks to make the number** (`CLAUDE.md`).

Where the forty went was not the checks. Every suite ran after the one
before it, and each is its own QEMU with its own disk and its own sockets,
sharing nothing; the Mac has ten cores and the gate used about one. So:

  1. every image the suites boot is built first, with `-j`
     (`make gate-images`) - so no two suites build the same thing at once;
  2. the host checks and the QEMU suites start together, `--at-once` of them
     at a time, the longest first by what each took last time, each writing
     its own log in `build/gate/`;
  3. the two suites that were most of the forty - `run_x86.py`, some thirty
     machines one after another, and the display harness, some forty phases
     in one machine for each board - run in parts, each part a machine of
     its own (`--parts`, `--phases`).

A suite marked `alone` runs after the rest on a quiet machine, for a check
about timing that cannot share. One does: x86's HDA sessions, whose ring
underran once with five other machines running and passed alone. Sound on
the ARM board, scheduling latency, the idle desktop and the compositor's
budget have all passed shared; a run where one does not is the evidence for
marking it.

The first run of this took 18:28 with the display harness whole; the second,
with the parts, 4:37 - the same 1,000-odd checks (`testing.md` 18.97).

Each line says a suite passed or failed and how long it took, as it
happens; the end says which were slowest, so the next thing to make faster
is a number rather than an opinion. A failed suite's log is kept whole, and
its last lines are printed with the path to the rest.

Usage: gate.py [--jobs N] [--at-once N] [--only NAME,...]
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import threading
import time

import scratch

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGS = os.path.join(ROOT, "build", "gate")

ARM = "build/kosmos.elf"
ARM_TEST = "build/test/kosmos.elf"
X86 = "build/x86_64/kosmos.elf"
X86_TEST = "build/x86_64-test/kosmos.elf"


class Suite:
    def __init__(self, name, cmd, alone=False, x86=False):
        self.name = name
        self.cmd = cmd
        self.alone = alone          # measures time: run on a quiet machine
        self.x86 = x86              # needs the x86-64 cross compiler
        self.took = None
        self.code = None
        self.log = os.path.join(LOGS, name + ".log")


SUITES = [
    # Everything that boots nothing: the filesystem's format, the decoders,
    # the licences and every syscall's arguments, in seconds.
    Suite("host", ["make", "--no-print-directory", "host-check"]),

    # The kernel from inside: the largest test asset here, and the only thing
    # that exercises the kernel from inside it.
    Suite("arm-kernel", ["python3", "tools/run_tests.py", ARM_TEST]),

    # And the same machine with nothing plugged into it. A second boot, but
    # of the ordinary image rather than the test one: what it checks is init
    # and the shell, which the test image replaces.
    Suite("arm-headless", ["python3", "tools/run_headless.py", ARM]),

    # Files on and off the image from this computer, which is what a
    # filesystem that is not FAT32 has to answer for.
    Suite("arm-interchange", ["python3", "tools/run_interchange.py", ARM]),

    # Attributes and the queries over them. M7's definition of done was a
    # live query and nothing here ever checked one: `qbench` measures how
    # fast a query is and would not notice it returning the wrong paths,
    # which is what it did on the disk for as long as the disk could answer.
    Suite("arm-queries", ["python3", "tools/run_queries.py", ARM]),

    # And the prompt as a place to work rather than a place to look. The
    # verbs check each other rather than a constant written in the harness:
    # `wc` says five lines, so `head` and `tail` have to name the first and
    # last two of exactly those.
    Suite("arm-shell", ["python3", "tools/run_shell.py", ARM]),

    # And Disk Benchmark, the instrument storage is being made fast with:
    # held to saying what it measured and what it could not, rather than to a
    # speed, since under QEMU the speed is QEMU's.
    Suite("arm-diskbench", ["python3", "tools/run_diskbench.py", ARM]),

    # A frame off the card and onto the wire, read back out of QEMU's own
    # capture - because nothing inside the guest can establish that one
    # left. And a second boot with no card, which is the branch every device
    # grant in init.lua carries a comment about getting wrong.
    Suite("arm-network", ["python3", "tools/run_network.py", ARM]),

    # And the second architecture, which until it had these proved nothing
    # that stayed proved: a staging kmain printed what it found and a person
    # read it. Skipped rather than failed where the cross compiler is not
    # installed - and said out loud, because a suite that quietly runs fewer
    # checks on one machine than another is worse than one that does not run
    # them at all.
    #
    # The choice is about *what is board-specific* rather than about coverage
    # for its own sake. `run_x86.py` boots it; `run_headless.py` asks whether
    # a machine with no display still reaches a prompt, which is the branch
    # every device grant carries a comment about; `run_disk.py` and
    # `run_network.py` are the two that go through a driver this board finds
    # over PCI rather than in a device-tree window. `run_interchange.py` and
    # `run_queries.py` are deliberately not here: their guest half is the
    # same filesystem `run_disk.py` exercises on this board, and their host
    # half boots nothing.
    Suite("x86-bin", ["build/host/test_efiboot", "build/x86_64/kosmos.bin"],
          x86=True),
    #
    # `run_x86.py` is some thirty machines, one after another - three and a
    # quarter minutes alone - so it runs as four groups of its parts, each a
    # machine of its own, side by side (`--parts`).
    Suite("x86-core", ["python3", "tools/run_x86.py", X86, "--parts",
                       "core,power_button,battery"], x86=True),
    Suite("x86-storage", ["python3", "tools/run_x86.py", X86, "--parts",
                          "storage,memdisk,identity,firmware,machine_report"],
          x86=True),

    # **The HDA sessions on a quiet machine**: `audiolag` measures the ring
    # against the wall, and on 18 September it underran with five other
    # machines running - "worst write 33216 us" - and passed alone, twice.
    # That run is the evidence the note above says to wait for.
    Suite("x86-sound", ["python3", "tools/run_x86.py", X86, "--parts",
                        "sound,sound_slow_codec,sound_eapd"],
          alone=True, x86=True),
    Suite("x86-usb-1", ["python3", "tools/run_x86.py", X86, "--parts",
                        "usb,usb_blocks,usb_diskbench,usb_home,"
                        "usb_second_stick,usb_home_late"], x86=True),
    Suite("x86-usb-2", ["python3", "tools/run_x86.py", X86, "--parts",
                        "usb_home_named,usb_home_large,usb_drives,"
                        "usb_flush_refused,"
                        "cmdline_long,usb_hotplug,usb_mouse,pointer"],
          x86=True),
    Suite("x86-headless", ["python3", "tools/run_headless.py", X86], x86=True),
    Suite("x86-disk", ["python3", "tools/run_disk.py", X86], x86=True),
    Suite("x86-network", ["python3", "tools/run_network.py", X86], x86=True),

    # **`run_uefi.py` is the one that boots the way a machine will**, and it
    # is here because everything above it goes through QEMU's `-kernel`,
    # which is not a loader. It reads the multiboot header, copies the image
    # in and jumps - and it does not answer the video request, so the path a
    # laptop depends on entirely had never run while fourteen host checks on
    # the decision all passed. Three faults were hiding behind that and none
    # was a driver.
    #
    # It also checks the *screen* rather than the serial line, because a
    # machine whose framebuffer works stops talking to the serial line at
    # stage six. It boots a stick `mkusb_image.py` made with Kosmos's own
    # loader, which is the image `make usb` writes, with a 4 MB disk
    # `kfs.lua` made, so the loader's second look at a disk is checked as well
    # as its look at the kernel. And a second stick, whose kernel is zeros,
    # is booted to be refused: the loader's lines must be on the screen while
    # it waits for a key, drawn by the loader itself, because the ThinkPad's
    # firmware console showed nothing. And the first stick once more, with its
    # screen at 0x4000000000 where the ThinkPad's firmware puts it: the boot
    # log has to be on it by stage four, which on that machine it was not for
    # months. Skipped where OVMF is not installed, out loud.
    Suite("x86-uefi", ["python3", "tools/run_uefi.py",
                       "build/x86_64/kosmos-uefi.img",
                       "build/x86_64/kosmos-refusal.img",
                       "build/x86_64/kosmos-uefi-home.img"], x86=True),

    # `test_stickcheck.py` streams that stick, with faults put where mtools
    # says they are, into the check `mkusb.sh` runs on every stick it writes:
    # damage named where it is, and a mount's bookkeeping told apart from it.
    Suite("x86-stickcheck", ["python3", "tools/test_stickcheck.py",
                             "build/x86_64/kosmos-uefi.img", X86,
                             "build/x86_64/kosmos-uefi-home.img"], x86=True),

    # And `run_tests.py` on this board too, which ran on one board for as
    # long as there were two: the 0.9.0 review found it and could not fix it
    # in a line - the suite was written in AArch64 assembly in thirty-five
    # places, exited through ARM semihosting, and ran two hand-written
    # AArch64 blobs at EL0. The few that do not run here are about AArch64
    # itself rather than the kernel: stepping ELR past a faulting
    # instruction, execution resuming after one, SPSel, and the lazy-FP
    # mechanism being disarmed until something wants it. Nothing is skipped
    # for being inconvenient.
    Suite("x86-kernel", ["python3", "tools/run_tests.py", X86_TEST,
                         "--timeout", "90"], x86=True),

    # The media engine under Music, held to what it sounds like: a tone
    # played, sought and finished, counted in the WAV QEMU wrote - and the
    # sound keeping real time. It shares the machine with the others: a run
    # it fails because of them is the evidence for giving it a quiet one.
    Suite("arm-media", ["python3", "tools/run_media.py", ARM]),

    # The Game Kit's rasterizer against the portable one it was ported
    # from: every primitive, both ways, and all 368,640 pixels compared
    # with no tolerance. Nine hundred lines of numeric C are worth six to
    # eight times the speed only if they draw the same picture.
    #
    # **On both boards, and that is not duplication.** The rasterizer is
    # floating point all the way through - `atan2` and `acos` per pixel in
    # the sphere, a `pow` in the sun's limb darkening - and the two
    # machines reach that arithmetic differently: musl's libm compiled for
    # AArch64 against the same source compiled for x86-64, with different
    # register widths and a different compiler backend under it. Agreement
    # on one board says the port is right; agreement on both says the
    # arithmetic is portable, which is the claim a released binary makes.
    # Five seconds each.
    Suite("arm-game", ["python3", "tools/run_game.py", ARM]),
    Suite("x86-game", ["python3", "tools/run_game.py", X86], x86=True),
]

#
# **And the display harness on both boards**: what was drawn, the keyboard
# and the pointer, the desktop and its windows. One machine ran some forty
# phases one after another, 402 seconds on each board, which was most of
# the gate. So each board's phases run as four machines side by side
# (`--phases`), cut where the measured times come to about a hundred seconds
# apiece and in the harness's own order - so `programs by name` still comes
# at a bare prompt and `registry` still before the first window manager, as
# they say they must. The boot screen and the bars are checked in every part.
#
DISPLAY_PARTS = [
    ["keyboard", "programs by name", "latency", "window manager latency",
     "interrupt", "status_bar", "editor", "registry", "context", "widgets",
     "scripting", "idle", "direct", "3d", "terminal", "programs by file"],
    ["log view", "text size", "window resize", "triangle", "repaints",
     "@@BOARD@@", "volume keys", "compositor budget"],
    ["faces", "wallpapers", "direct menu", "tabs", "appearance",
     "Super Nintendo --scale", "deskbar", "deskbar focus", "desktop",
     "places", "panel"],
    ["clipboard", "cores", "reaped", "clicks", "graphical", "replicants",
     "window_manager", "drives app"],

    # **Alone, and that is the point.** It is the only check that wants a
    # desktop on a machine nobody has told anything, and every other phase
    # here runs after the harness has pinned the faces its rows were
    # measured against. A desktop cannot be quit either, so the phase takes
    # the console with it - `run_screenshot.py` says the rest.
    ["default look"],
]

for board, image, own in (("arm", ARM, "power button"),
                          ("x86", X86, "unknown keys")):
    for n, phases in enumerate(DISPLAY_PARTS, 1):
        cmd = ["python3", "tools/run_screenshot.py", image, "--phases",
               ",".join(own if ph == "@@BOARD@@" else ph for ph in phases)]

        if board == "arm" and n == 1:
            cmd += ["--png", "build/screenshot.png"]

        SUITES.append(Suite("%s-display-%d" % (board, n), cmd,
                            x86=(board == "x86")))


def run(suite, lock, started):
    begin = time.monotonic()

    with open(suite.log, "wb") as out:
        done = subprocess.run(suite.cmd, cwd=ROOT, stdout=out,
                              stderr=subprocess.STDOUT)

    suite.took = time.monotonic() - begin
    suite.code = done.returncode

    with lock:
        at = time.monotonic() - started
        print("%6.0fs  %s  %-16s %5.0fs" % (at, "PASS" if suite.code == 0
                                             else "FAIL", suite.name,
                                             suite.took), flush=True)


def summary_of(suite):
    """The suite's own last PASS or FAIL line, which says what it checked."""
    try:
        with open(suite.log, errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return ""

    said = [l for l in lines if l.startswith(("PASS", "FAIL", "SKIP"))]
    return said[-1][:150] if said else ""


def uncovered_x86_parts():
    """`run_x86.py`'s parts that no suite here names.

    **The suites name their parts by hand**, so a part added to `run_x86.py`
    runs only when somebody asks for it by name. `power_button` was that part
    on 19 September, found by reading this file rather than by anything
    failing - and a test that never runs passes for ever. So the parts are
    held to `run_x86.PARTS`, read from the file itself.
    """
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        "run_x86", os.path.join(ROOT, "tools", "run_x86.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    named = set()

    for suite in SUITES:
        if "tools/run_x86.py" in suite.cmd and "--parts" in suite.cmd:
            at = suite.cmd.index("--parts") + 1
            named.update(p for p in suite.cmd[at].split(",") if p)

    return [p for p in module.PARTS if p not in named]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4,
                        help="compiles at once, for the images")
    parser.add_argument("--at-once", type=int, default=6,
                        help="suites at once, before the quiet ones")
    parser.add_argument("--only", default="",
                        help="suites to run, by name, separated by commas")
    args = parser.parse_args()

    missing = uncovered_x86_parts()

    if missing:
        print("FAIL: run_x86.py has parts no suite runs: %s - name each in "
              "one of the x86 suites in tools/gate.py" % ", ".join(missing),
              flush=True)
        return 1

    os.makedirs(LOGS, exist_ok=True)
    started = time.monotonic()
    # What is in the temporary directory before any suite runs, so that what
    # is there after is what this run left (`scratch.py`).
    before = scratch.leftovers()
    have_x86 = shutil.which("x86_64-elf-gcc") is not None

    chosen = [s for s in SUITES if not args.only
              or s.name in args.only.split(",")]

    if not have_x86:
        print("SKIP: every x86-64 suite, because x86_64-elf-gcc is not "
              "installed.", flush=True)
        chosen = [s for s in chosen if not s.x86]

    # 1. The images, before any suite starts.
    images = os.path.join(LOGS, "images.log")

    with open(images, "wb") as out:
        built = subprocess.run(["make", "--no-print-directory",
                                "J=%d" % args.jobs, "gate-images"],
                               cwd=ROOT, stdout=out, stderr=subprocess.STDOUT)

    print("%6.0fs  %s  %-16s" % (time.monotonic() - started,
                                 "PASS" if built.returncode == 0 else "FAIL",
                                 "images"), flush=True)

    if built.returncode != 0:
        with open(images, errors="replace") as f:
            print("".join(f.readlines()[-40:]))
        print("FAIL: the images did not build - %s has all of it" % images)
        return 1

    # 2. Everything that shares the machine happily, side by side.
    lock = threading.Lock()
    shared = [s for s in chosen if not s.alone]
    quiet = [s for s in chosen if s.alone]
    # **The longest first**, by what each took last time - kept in
    # `times.json` - so the last suite to finish is not a long one that
    # started late. A suite never timed goes first.
    times_file = os.path.join(LOGS, "times.json")

    try:
        with open(times_file) as f:
            last = json.load(f)
    except (OSError, ValueError):
        last = {}

    waiting = sorted(shared, key=lambda s: -last.get(s.name, 1e9))
    running = []

    while waiting or running:
        running = [t for t in running if t.is_alive()]

        while waiting and len(running) < args.at_once:
            t = threading.Thread(target=run,
                                 args=(waiting.pop(0), lock, started))
            t.start()
            running.append(t)

        time.sleep(0.2)

    # 3. And the ones that measure time, one at a time on a quiet machine.
    for suite in quiet:
        run(suite, lock, started)

    total = time.monotonic() - started
    failed = [s for s in chosen if s.code != 0]
    left = sorted(scratch.leftovers() - before)

    last.update({s.name: round(s.took, 1) for s in chosen
                 if s.took is not None})

    with open(times_file, "w") as f:
        json.dump(last, f, indent=1, sort_keys=True)

    print()
    print("slowest: " + ", ".join("%s %.0fs" % (s.name, s.took) for s in
                                  sorted(chosen, key=lambda s: -s.took)[:6]))

    for s in failed:
        print()
        print("--- %s, the end of %s" % (s.name, s.log))

        with open(s.log, errors="replace") as f:
            print("".join(f.readlines()[-30:]).rstrip())

    print()

    #
    # **A run leaves nothing in the temporary directory.** Nineteen gigabytes
    # of it filled this Mac's disk on 19 September, a few hundred megabytes a
    # run, and the first sign was a suite dying of a full disk in a place
    # that had nothing to do with it. Checked here, after every suite, rather
    # than trusted to each: a tool that makes a temporary file any way but
    # `scratch.py` is refused by `test_scratch.py`, and one that is killed
    # before it can tidy up is named here, by what made it.
    #
    for name in left:
        print("LEFT BEHIND: %s, by %s"
              % (name, scratch.made_by(name) or "nothing that says"))

    if left:
        print()

    if failed or left:
        print("FAIL: %d of %d suites in %.0f s - %s%s"
              % (len(failed), len(chosen), total,
                 ", ".join(s.name for s in failed) or "every suite passed",
                 ("; and %d left in the temporary directory" % len(left))
                 if left else ""))
        return 1

    print("PASS: %d suites in %.0f s (%d:%02d)" % (len(chosen), total,
                                                   total // 60, total % 60))

    for s in chosen:
        line = summary_of(s)

        if line:
            print("  %-16s %s" % (s.name, line))

    return 0


if __name__ == "__main__":
    sys.exit(main())
