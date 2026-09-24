#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
Host-side display check, in two phases.

`make test` proves the framebuffer and the surfaces from inside the guest:
that they exist, are the right shape, clip instead of overrunning, and that
the alpha maths is exact. There is one property it structurally cannot reach.

**A pitch bug is invisible from inside.** If `row_of` in gfx.c stepped by
`width * 4` instead of by the pitch, every read and every write would agree
with each other and the whole suite would pass - the surface would simply
have an unused gap at the end of each row. It was tried: replacing the pitch
with `width * 4` passes 103 of 103 tests. The only observer who disagrees is
the one on the other side of the framebuffer, where the stride is 4160 and a
row written 4096 bytes along lands sixteen pixels to the left of where it
belongs.

So there are two phases here, and the second is the one that matters:

  1. **The boot screen**, drawn by the kernel: its ten narrated stages and
     the progress bar under them. Geometry against what the guest reported
     over serial, the banner and the body text present, and the bar full -
     a bar stuck short of the end means BOOT_STAGES and the number of stages
     actually announced disagree. The bar's green also covers the channel
     order, since a wrong fourcc turns it blue.

  3. **Real key events**, injected through QEMU's input subsystem into the
     virtio-input device - a path that shares nothing with the serial line
     everything else here types over.

  4. **A detached program still drawing.** `monitor 30 &` redraws the
     reserved rows once a second from a process of its own, and whether it
     does is invisible over serial - it draws on the framebuffer and says
     nothing. Two pictures 3.3 seconds apart, and the reserved rows have to
     differ by more than one glyph cell. The 0.3 is not decoration: the
     cursor blinks twice a second, so a whole number of seconds catches it in
     the same phase and reports a static screen on a moving one.

  5. **Control-C from the keyboard, stopping that program.** The same rows,
     which must go still. Two things this proves and serial cannot: that the
     driver turns Control plus C into byte 3 - the terminal does that on the
     other side of a cable, so Control-C worked over serial long before it
     did anything in the window - and that a running program is told.

  6. **A hung application's window, dragged.** This milestone's definition
     of done, which is BeOS's: `wm hello-win,stuck` puts two applications on
     screen, one of which never replies again, and the hung one's window has
     to keep moving. Last, because the window manager takes the whole screen.

  7. **Scheduling latency**, which is not about the display at all and is
     here because this is the only harness that boots the shipping image and
     drives its shell. `make test` cannot ask it: during the suite the
     kernel's first thread is running the suite, so it never reaches the
     idle loop whose behaviour is the whole question.

  8. **The machine writing its own program.** Four lines typed into `edit`,
     saved, and run from the shell. It depends on so many separate things at
     once that it is the closest thing here to a statement that the system
     works.

  9. **An application scripted from the shell**, with no scripting code in
     it: `fs.write("/app/gallery/title", ...)` has to widen the window's tab,
     which is as wide as its title. Checked by consequence, not by reply - a
     property store that accepted the write and told nobody would pass a
     read-back and fail this.

 10. **A replicant moved between processes.** One application publishes a
     view as source, state and a `needs` list; another, which has never
     heard of clocks, adopts it and runs it. Both clocks must be ticking and
     reading differently, and the line reporting what the replicant's
     restricted namespace actually answered must be the green one.

 11. **The console staying off the screen** while a compositor owns it. A
     program prints a paragraph while the window manager is running; it has
     to reach the serial line and not the display. One printed line used to
     scroll every window up sixteen pixels.

 12. **The widgets, clicked.** A button, a list row, and the one that
     matters: pressing a button and sliding off before letting go must do
     nothing. A button that fires on the press passes the first two and
     fails this.

 13. **The Deskbar.** A bare `wm` starts it and nothing else; clicking an
     application in its list has to put a second window on screen. Counted
     by tabs, since every window has exactly one.

 14. **An idle desktop is idle.** The processor meter has to be green and
     nearly empty. Everything used to poll, and one thread that never blocks
     keeps a core at a hundred per cent - the meter was right, which is why
     this reads the meter rather than trusting it.

 15. **Programs reached by typing their name**, at the bare prompt: nothing
     in `/bin` hidden by a name in the shell's environment, every kit the
     image lists a table, and `snes --scale 3`, `doom /nowhere.wad` and
     `quake /nowhere.pak` each answered by its program. Three kits that set
     globals once made `snes` print a table.

  2. **A pattern drawn from Lua**, through gfx.screen() at the shell prompt.
     Vertical bars, which is the shape a pitch error destroys: each row would
     shift by (4160 - 4096) / 4 = sixteen pixels, turning every vertical line
     into a diagonal. The check reads each bar's x position at several
     heights and requires them all to be the same.

Usage: run_screenshot.py <image.elf> [--png OUT] [--timeout SECONDS]
"""

import argparse
import fcntl
import json
import re
import os
import select
import shutil
import socket
import struct
import subprocess
import sys
import threading
import scratch
import time
import zlib

QEMU = "qemu-system-aarch64"

# Has to agree with kernel/console.c: RESERVED_ROWS rows of GLYPH_H,
# at the bottom, which the kernel keeps out of the scroll region and
# `monitor` draws its one line of text into.
GLYPH_W = 8
GLYPH_H = 16

# **The kit's fixed row** (`theme.metrics.row`, `roadmap.md` 5x): a list, a
# tree or a menu row is 32 pixels whatever the face - the drawings' row
# since 0.10.149 (`roadmap.md` 5zp); it was 24. Checks that found a row by
# the face's height - 16 in the harness's pinned bitmap - measured the
# layout that followed the face, which is gone.
LAYOUT_ROW = 32
RESERVED_PX = 2 * GLYPH_H


# Has to agree with the Makefile's line, except for the display: a window is
# not wanted here, and `-display none` still gives ramfb a surface to scan
# out and the monitor something to dump.
# **No disk, deliberately.** `run_tests.py` attaches one and this does not,
# so between them both machines are exercised: one that has a drive and one
# that does not. That is not a spare detail - adding the disk server broke
# booting without a drive, init exited, and this harness is what found it.
# If a disk is ever added here, that coverage has to move somewhere else.
#
# ...and a way to attach one anyway, without changing what the suite runs.
#
# `KOSMOS_DISK=build/kosmos.img` adds the drive. The default stays diskless
# so the coverage above is not quietly lost, and anything that needs a real
# file - a PDF, a song, a WAD - can ask for it in one word instead of
# rebuilding the argument list by hand, which is what every ad-hoc script
# had been doing.
#
_DISK = os.environ.get("KOSMOS_DISK")

#
# And a sound device, the same way, when a harness wants to hear the guest.
#
# `KOSMOS_AUDIO_WAV=path` adds virtio-sound with QEMU's WAV writer behind it,
# so what the guest played is a file afterwards - the way `run_x86.py`
# listens to HDA. The default stays silent, for the reason it stays diskless.
#
_WAV = os.environ.get("KOSMOS_AUDIO_WAV")

#
# Which machine, chosen from the image's own path.
#
# **Every harness that boots a guest goes through `Guest`**, so making this
# one function architecture-aware is what lets `run_web.py`,
# `run_browser.py` and `run_gallery.py` point at either board without
# knowing there are two. The alternative was a copy of each per machine,
# which is how two test suites drift apart.
#
# The path is the switch because every caller already passes one and they
# differ: the ARM image is `build/kosmos.elf` and the other is
# `build/x86_64/kosmos.elf`. Nothing has to be told twice.
#
def machine(image):
    return "x86_64" if "x86_64" in image else "aarch64"


def device(image, kind):
    """What QEMU calls a virtio device on this machine.

    The same hardware under two names: `virtio-net-device` is the MMIO
    transport the ARM board finds in a window the device tree describes, and
    `virtio-net-pci` is the same card behind a PCI capability. A harness
    that hardcoded one of them attached nothing on the other board and the
    guest reported, quite correctly, that there was no network card.
    """
    tail = "pci" if machine(image) == "x86_64" else "device"

    return "virtio-%s-%s" % (kind, tail)


def extra_args(image, more):
    """Append device arguments to whichever board's list is in force."""
    if machine(image) == "x86_64":
        global X86_ARGS
        X86_ARGS = X86_ARGS + more
    else:
        global QEMU_ARGS
        QEMU_ARGS = QEMU_ARGS + more


#
# The devices, per board, and they are the same devices.
#
# QEMU gives both machines ramfb, virtio-input and virtio-blk; what differs
# is how a guest finds them, which is `hal/qemu-virt/virtio.c` against
# `hal/pc/virtio.c` and none of this harness's business. What *is* its
# business is `-vga none`: q35 adds a VGA adapter unless told not to and
# QEMU dumps the first display device, so without it every screenshot of
# the x86 machine is 640x480 of black while ramfb draws perfectly into
# memory nobody looks at.
#
X86_ARGS = [
    "-M", "q35",
    "-m", "512M",
    "-display", "none",
    "-vga", "none",
    "-device", "ramfb",
    #
    # No virtio keyboard: q35's i8042 is this board's, so `sendkey` has to
    # reach the PS/2 controller for the check to mean anything. The tablet
    # stays because these checks put the pointer at exact coordinates, which
    # an absolute device does in one step and a relative one only by
    # counting. The i8042's auxiliary port delivers under QEMU now, and
    # `tools/run_x86.py` clicks through it with no tablet at all.
    #
    "-device", "virtio-tablet-pci",
] + ([
    "-drive", "file=%s,format=raw,if=none,id=disk" % _DISK,
    "-device", "virtio-blk-pci,drive=disk",
] if _DISK else [])

QEMU_ARGS = [
    "-M", "virt,gic-version=3",
    "-cpu", "cortex-a72",
    "-m", "512M",
    #
    # Four, which is what `make qemu` boots and what `run_tests.py` boots.
    #
    # This harness ran on one for as long as one was all the kernel could
    # use, and that stopped being true at `docs/smp.md` step three: three
    # secondaries start, claim a `struct percpu` and park in `wfi`. A
    # display test on a one-core machine is a display test of a machine
    # nobody runs - and it showed up immediately, because `cores` and
    # `sysmon` draw a row per processor and drew one.
    #
    # And all four take new threads: a plain boot spreads work across every
    # processor that arrived, so this harness measures the machine as it is
    # used.
    #
    "-smp", "4",
    #
    # And, when asked, the boot option that narrows that.
    # `KOSMOS_SMPWORK=1 make screenshot` homes every new thread on core zero
    # again, which is how a failure here is told apart from one about
    # placement.
    #
    # An environment variable rather than a flag because the harness is
    # invoked from the Makefile in five places and a flag would have to be
    # threaded through all of them to be used in one.
    #
] + ([
    "-fw_cfg", "name=opt/kosmos/smp,string=" + os.environ["KOSMOS_SMPWORK"],
] if os.environ.get("KOSMOS_SMPWORK") else []) + [
    "-display", "none",
    "-device", "ramfb",
    # force-legacy=false is not optional: QEMU's virtio-mmio transports
    # report the legacy interface unless told otherwise, and the driver
    # refuses those.
    "-global", "virtio-mmio.force-legacy=false",
    "-device", "virtio-keyboard-device",
    "-device", "virtio-tablet-device",

] + ([
    "-drive", "file=%s,format=raw,if=none,id=disk" % _DISK,
    "-device", "virtio-blk-device,drive=disk",
] if _DISK else []) + ([
    "-audiodev", "wav,id=snd0,path=%s" % _WAV,
    "-device", "virtio-sound-device,audiodev=snd0",
] if _WAV else [])


def use_audiodev(spec):
    """Swap the audiodev for the next guest this process boots.

    **The wav writer is paced by QEMU's own timer, and a real device is
    not.** That is the difference this exists for: the writer waits for the
    guest, so a guest that hands over periods too slowly - or a backend that
    retires them too fast - still measures perfect, because both ends are on
    the same clock. A device on a real clock drains whether or not anybody
    is ready.

    Found on 16 September: Diego heard Music play at twice speed under
    `make qemu`, where the backend is coreaudio, while the ThinkPad was
    correct and the whole suite was green. Six runs of a three-clock probe
    put it at 2.09x under coreaudio and exactly 1.00x under the wav writer,
    `none`, and `none` forced to either rate - so no test in this tree had
    ever exercised a device that keeps its own time.

    `none` is that device and makes no sound, which is why the suite can
    boot against it without a person having to listen.
    """
    global QEMU_ARGS

    args = list(QEMU_ARGS)
    args[args.index("-audiodev") + 1] = spec
    QEMU_ARGS = args


PROMPT = "kosmos>"          # printed once the shell is serving

# The kernel's display stage prints its geometry as part of narrating the
# boot. Matched rather than a dedicated marker line, because a marker that
# exists only for a test is a line somebody deletes while tidying and nobody
# notices until the test fails for an unrelated-looking reason.
#
# Kept loose for the same reason it has already broken twice: it matched
# "video 1024x768" until the boot log was rewritten, then "32-bit colour"
# until that line said XRGB instead. The geometry is the part that is really
# being asked for; everything after it is prose and prose changes.
GEOMETRY = re.compile(r"(\d+)x(\d+), 32-bit")

# The pattern phase two draws. Vertical bars on a black field, at x positions
# a pitch error would smear: a wrong stride shifts each row sixteen pixels, so
# a bar two pixels wide stops being a bar by the third row.
BARS = [(200, 0x00ff4040), (500, 0x0040ff40), (800, 0x004040ff)]
BAR_WIDTH = 6

DRAW = (
    "local s = gfx.screen() "
    "local w, h = s:size() "
    "s:fill(0, 0, w, h, 0xff000000) "
    + " ".join(f"s:fill({x}, 0, {BAR_WIDTH}, h, 0xff{c:06x}) " for x, c in BARS)
)

#
# **It used to end `return 'drawn'`, and that marker is what broke it.**
#
# The shell echoes what a snippet returns, so the marker was a *print* -
# which scrolls the console, and a console that scrolls now repaints its
# whole grid rather than shifting the framebuffer. The bars were painted
# over by the very line that announced they had been painted.
#
# Under the old console the same print shifted the picture up sixteen pixels
# instead of erasing it, and these bars are vertical and full height, so a
# vertical shift left them looking identical. The phase passed for years on
# a property nobody chose.
#
# So there is no marker. Nothing is printed after the fills, nothing
# scrolls, and the phase waits for the *picture* to show bars rather than
# for a word to appear on the serial line - which is what `settle` exists
# for and what its own docstring recommends over waiting a fixed time.
#


def bars_drawn(width, height, px):
    """Are all three bars on the screen yet? The predicate `settle` waits on."""
    def at(x, y):
        o = (y * width + x) * 3
        return tuple(px[o:o + 3])

    for x, colour in BARS:
        want = ((colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff)

        if at(x + BAR_WIDTH // 2, 1) != want:
            return None

    return width, height, px


class Failure(Exception):
    """Something about the picture was wrong. The message is the report."""


class Guest:
    """A booted image, with its serial on pipes and its monitor on a socket.

    Two channels because both are needed: the serial line to read what the
    guest says and to type at its shell, and the monitor to ask QEMU for the
    picture. They cannot share stdio.
    """

    def __init__(self, image, timeout):
        self.timeout = timeout
        self.dir = scratch.directory("g")
        self.sockpath = os.path.join(self.dir, "monitor")
        self.qmppath = os.path.join(self.dir, "qmp")
        self.seen = ""
        self._lock = threading.Lock()

        arch = machine(image)
        binary = "qemu-system-x86_64" if arch == "x86_64" else QEMU
        args = X86_ARGS if arch == "x86_64" else QEMU_ARGS

        try:
            self.proc = subprocess.Popen(
                [binary, *args,
                 "-monitor", f"unix:{self.sockpath},server,nowait",
                 "-qmp", f"unix:{self.qmppath},server,nowait",
                 "-serial", "stdio",
                 "-kernel", image],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, bufsize=0,
            )
        except FileNotFoundError:
            raise Failure(f"{binary} not found. See docs/setup.md.")

        # Bytes and non-blocking, not lines.
        #
        # The thing most worth waiting for is the shell prompt, and the shell
        # prints "kosmos> " with no newline after it - so readline() blocks
        # for ever on a guest that is sitting there perfectly happily waiting
        # to be typed at. Reading whatever has arrived is the only way to see
        # a prompt.
        fd = self.proc.stdout.fileno()
        fcntl.fcntl(fd, fcntl.F_SETFL,
                    fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_NONBLOCK)

        # Started here rather than in the lines above, because it reads
        # `self.proc` and there was no process until a moment ago.
        self._reader = threading.Thread(target=self._drain, daemon=True)
        self._reader.start()

        self.monitor = None
        self.qmp = None

    def _drain(self):
        """Read the serial line for as long as the guest lives.

        **A thread, because the guest blocks when nobody is reading.** This
        used to be drained only from `wait_for` - so during a phase that
        types nothing and waits on the *picture* instead, nothing emptied the
        pipe for seconds at a time. A pipe holds 64 KB; past that the write
        blocks, and the write blocking is in `kputc`, which holds the console
        lock, which stops the machine dead.

        It was invisible while the guest was quiet. The window manager now
        narrates its startup into the log, about five kilobytes per instance
        and twenty instances across a full run, and the last phase - which is
        the one that drags a window through three seconds of sleeps - started
        failing with the window not moving at all. That reads exactly like a
        compositor waiting on an application, which is what its message says,
        and it was the harness holding the machine still.

        So the fix is here rather than a quieter guest: a test that cannot
        survive its subject talking is a test that will fail again the next
        time anything useful is printed.
        """
        fd = self.proc.stdout.fileno()

        while True:
            try:
                ready, _, _ = select.select([fd], [], [], 0.2)

                if not ready:
                    if self.proc.poll() is not None:
                        return
                    continue

                chunk = os.read(fd, 65536)

                if not chunk:
                    return

                with self._lock:
                    self.seen += chunk.decode("utf-8", errors="replace")
            except (BlockingIOError, OSError, ValueError):
                return

    def _read_available(self):
        """Nothing to do: `_drain` has been reading all along.

        Kept so the call sites read the same as they did, and because
        "drain now" is still the right thing to say at a point where the
        answer matters.
        """
        return

    def wait_for(self, text, what):
        deadline = time.monotonic() + self.timeout

        while time.monotonic() < deadline:
            self._read_available()
            if text in self.seen:
                return
            if self.proc.poll() is not None:
                raise Failure(f"QEMU exited before {what}.")

        raise Failure(
            f"the guest never {what} within {self.timeout}s.\n"
            f"--- what it did say ---\n{self.seen}"
        )

    def wait_for_line(self, text, what, since=0):
        """The rest of the line that `text` begins, once it has all arrived.

        `wait_for` returns the moment `text` is on the serial line, and a
        check that then reads to the end of its line can take half of it:
        the Appearance panel's `668x590, 5 roles, dark` was read as
        `668x590,` on 22 September, when one more line at the desktop's
        start moved where the serial port's reads fell. So this waits for
        the newline too. `since` is where in `seen` to start looking.
        """
        deadline = time.monotonic() + self.timeout

        while time.monotonic() < deadline:
            self._read_available()
            at = self.seen.find(text, since)

            if at >= 0 and "\n" in self.seen[at + len(text):]:
                return self.seen[at + len(text):].split("\n", 1)[0].strip()

            if self.proc.poll() is not None:
                raise Failure(f"QEMU exited before {what}.")

            time.sleep(0.05)

        raise Failure(
            f"the guest never {what} within {self.timeout}s.\n"
            f"--- what it did say ---\n{self.seen[since:][-1500:]}"
        )

    def line_starting(self, prefix):
        for text in self.seen.splitlines():
            if text.startswith(prefix):
                return text
        return None

    def type(self, text):
        self.proc.stdin.write((text + "\n").encode())
        self.proc.stdin.flush()

    def _connect_monitor(self):
        if self.monitor is None:
            self.monitor = socket.socket(socket.AF_UNIX)
            self.monitor.settimeout(self.timeout)
            self.monitor.connect(self.sockpath)
            time.sleep(0.3)
            try:
                self.monitor.recv(65536)        # the greeting
            except socket.timeout:
                pass

    def _connect_qmp(self):
        """The other monitor.

        Key presses go through the human monitor's `sendkey`, which is enough
        for a keyboard. A pointer is not: `mouse_move` there sends *relative*
        deltas to whichever device QEMU thinks is current, and a tablet
        reports absolute position - so the two never meet and nothing moves.

        `input-send-event` on the QMP socket is the one that speaks the same
        language as the device: an absolute value on a named axis, in the
        same 0..32767 range the tablet itself reports.
        """
        if self.qmp is not None:
            return

        self.qmp = socket.socket(socket.AF_UNIX)
        self.qmp.settimeout(self.timeout)
        self.qmp.connect(self.qmppath)
        self.qmp.recv(65536)                        # the greeting

        self.qmp.sendall(b'{"execute":"qmp_capabilities"}\n')
        self.qmp.recv(65536)                        # its answer

    def _qmp(self, command, arguments):
        self._connect_qmp()
        self.qmp.sendall(
            (json.dumps({"execute": command, "arguments": arguments}) + "\n")
            .encode())

        # The reply, plus whatever asynchronous events arrive with it. Not
        # parsed: this is a test harness and the only interesting failure -
        # QEMU refusing the command - shows up as the guest not moving.
        time.sleep(0.15)

        try:
            self.qmp.recv(65536)
        except socket.timeout:
            pass

    def mouse_to(self, x, y):
        """Absolute position, in the tablet's own 0..32767 range."""
        self._qmp("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y)}},
        ]})

    def mouse_button(self, down, button="left"):
        self._qmp("input-send-event", {"events": [
            {"type": "btn", "data": {"down": bool(down), "button": button}},
        ]})

    def sendkey(self, key):
        """One key press and release, through QEMU's own input plumbing.

        This is the only way to test the keyboard from here: `type()` writes
        to the serial line, which is the path that already worked. `sendkey`
        goes into the QEMU input subsystem and out through the virtio-input
        device, which is the path the driver reads - so what it proves is the
        virtqueue, the feature negotiation and the keymap, none of which the
        serial line touches.
        """
        self._connect_monitor()
        self._drain_monitor()
        self.monitor.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.35)
        self._drain_monitor()

    def _drain_monitor(self):
        """Empties whatever the monitor has said and nobody read.

        `sendkey` and `screendump` share one socket, and neither of them
        reads the prompt and echo that come back. The unread bytes pile up,
        and a `sendkey` issued after a few hundred screendumps can be lost
        in the backlog - which looks exactly like a key that did not arrive,
        intermittently, only in long runs.

        Non-blocking: there is usually nothing, and waiting for something
        that is not coming would stall every key press.
        """
        self.monitor.setblocking(False)

        try:
            while True:
                if not self.monitor.recv(65536):
                    break
        except (BlockingIOError, socket.timeout, OSError):
            pass
        finally:
            self.monitor.setblocking(True)
            self.monitor.settimeout(self.timeout)

    def screendump(self):
        self._connect_monitor()
        self._drain_monitor()
        path = os.path.join(self.dir, f"shot{time.monotonic_ns()}.ppm")

        self.monitor.sendall(f"screendump {path}\n".encode())

        for _ in range(int(self.timeout * 10)):
            if os.path.exists(path) and os.path.getsize(path) > 0:
                time.sleep(0.3)                 # let the write finish
                return open(path, "rb").read()
            time.sleep(0.1)

        raise Failure("the monitor never produced a screendump.")

    def close(self):
        try:
            if self.qmp is not None:
                self.qmp.close()
                self.qmp = None

            if self.monitor is not None:
                self.monitor.sendall(b"quit\n")
                self.monitor.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
        shutil.rmtree(self.dir, ignore_errors=True)


def parse_ppm(data):
    """P6, binary, maxval 255. Returns (width, height, pixels)."""
    parts = data.split(b"\n", 3)

    if len(parts) < 4 or parts[0] != b"P6":
        raise Failure("the screendump is not a binary PPM.")

    width, height = (int(v) for v in parts[1].split())

    if parts[2] != b"255":
        raise Failure(f"unexpected PPM maxval {parts[2]!r}.")

    return width, height, parts[3]


def pixel_reader(data):
    width, height, px = parse_ppm(data)

    def at(x, y):
        o = (y * width + x) * 3
        return tuple(px[o:o + 3])

    return width, height, at


def find_colour(at, want, x0, y0, x1, y1):
    """Whether `want` appears anywhere in a region. Returns a sample point."""
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            if at(x, y) == want:
                return (x, y)
    return None


#
# **What the harness pins, in one place.**
#
# The default faces are IBM Plex since 19 September (`docs/styleguide.html`),
# and nearly every check below finds a row, a baseline or a column by the 8
# by 16 bitmap it was written for. So the harness writes its own faces into
# `/home/.appearance` - and *every* phase that rewrites that file has to
# write them too, because a write with the palette alone drops them and the
# next desktop to start comes up in Plex. That is exactly what happened: the
# wallpapers phase rewrote the file, and the Deskbar check three phases later
# found its buttons a few pixels from where it expected them.
#
PINNED_FONTS = ('fonts = { ui = { font = "spleen", px = 16 }, '
                'title = { font = "spleen", px = 16 }, '
                'text = { font = "spleen", px = 16 }, '
                'mono = { font = "spleen", px = 16 } }')


def appearance(extra=""):
    """The `/home/.appearance` the harness runs with: the dark palette its
    colours were written against, the bitmap faces its rows were measured
    with, and whatever a phase adds."""
    return ('fs.write("/home/.appearance", { palette = "dark", %s%s })'
            % (PINNED_FONTS, (", " + extra) if extra else ""))


def settle(guest, predicate, what, seconds=20):
    """Waits for the screen to satisfy `predicate`, and returns the picture.

    `predicate(width, height, pixels)` returns the thing being waited for, or
    None. The last picture taken is passed to the failure message so it can
    say what it did see.

    This exists because the obvious thing - do something, sleep, look - is
    wrong here in a way that only shows up in a full run. An application
    blocks for up to a second between events, so how long a click takes to
    reach the screen depends on where in that second it landed; a fixed
    sleep is a bet on that, and a suite of bets fails somewhere different
    every time. Two consecutive full runs failed in two different phases,
    both of which passed alone, which is the shape of a racing harness
    rather than a broken system.

    Waiting for the result instead makes the phases say what they mean: not
    "half a second later the bar had moved" but "the bar moved".
    """
    deadline = time.monotonic() + seconds
    width = height = 0
    px = b""

    while time.monotonic() < deadline:
        width, height, px = parse_ppm(guest.screendump())
        found = predicate(width, height, px)

        if found is not None:
            return found

        time.sleep(0.3)

    raise Failure(f"waited {seconds}s and {what}")


def started(guest, windows=1, seconds=25):
    """Waits until `windows` window(s) are on screen, and returns the picture.

    Replaces the `guest.type("wm ...")` then `time.sleep(12)` that every
    phase used to open with. `settle`'s own docstring says why that was
    wrong - an application blocks for up to a second between events, so how
    long a start takes to reach the screen depends on where in that second
    it landed, and a suite of such bets fails somewhere different each run.
    The sleeps were also sized for the slowest machine anybody had run this
    on, which is what made a green run take twenty minutes.

    Waiting for the window to *be there* is both faster and steadier: it
    returns as soon as it is true, and it says what it was waiting for when
    it never becomes true.
    """
    return settle(
        guest,
        lambda w, h, px: (lambda n: n if n >= windows else None)(
            count_windows(w, h, px)),
        f"waited {seconds}s and {windows} window(s) never appeared.",
        seconds=seconds)


def check_boot_screen(geometry, data):
    """Phase one: what the kernel drew while booting.

    The kernel narrates its ten stages onto the framebuffer through the same
    console that writes to the serial port, and fills a progress bar at the
    bottom as they complete. Both are checked, and between them they cover
    the two things only an outside observer can see: that the geometry the
    guest reported is the geometry QEMU is scanning out, and that the channel
    order is right - a wrong fourcc turns the bar's green into blue.
    """
    width, height, at = pixel_reader(data)

    if geometry != f"{width}x{height}":
        raise Failure(
            f"the guest reported {geometry} and QEMU is scanning out "
            f"{width}x{height}. The ramfb config and the framebuffer "
            "disagree about the geometry."
        )

    checks = 1

    # The console's own colours, from kernel/console.c and kernel/boot.c.
    background = (0x0d, 0x11, 0x17)
    title      = (0x58, 0xa6, 0xff)
    bar_fill   = (0x3f, 0xb9, 0x50)

    got = at(width - 4, height // 2)
    if got != background:
        why = ("that is the same three bytes reversed, so the fourcc or the "
               "channel order is wrong"
               if tuple(reversed(got)) == background
               else "the console did not clear the screen")
        raise Failure(
            f"the background is {got} and should be {background}: {why}."
        )
    checks += 1

    # Blue text somewhere in the log: the banner if it is still on screen,
    # and the [n/12] on every stage line regardless.
    #
    # Not "on the first row" any more. The boot narration outgrew the screen
    # when every stage started explaining what it is for, so it scrolls and
    # the banner is gone by the time the prompt appears - which is correct
    # behaviour and used to fail this check.
    if find_colour(at, title, 0, 0, 200, 400) is None:
        raise Failure(
            "no blue pixels anywhere in the boot log. Either the console "
            "never attached to the screen or the channel order is wrong."
        )
    checks += 1

    # Body text, in the rows below it. Any pixel lighter than the background
    # will do: this is "something was written", not "what was written".
    lit = 0
    for y in range(32, 300, 3):
        for x in range(0, width, 3):
            if at(x, y) != background:
                lit += 1
    if lit < 200:
        raise Failure(
            f"only {lit} lit pixels where the boot log should be. The kernel "
            "narrated its stages to the serial port and not to the screen."
        )
    checks += 1

    # The progress bar, full. It is in the rows the text never scrolls
    # through, and by the time the shell is up every stage has completed - a
    # bar stuck short of the end means BOOT_STAGES and the number of
    # boot_stage calls disagree.
    bar = find_colour(at, bar_fill, 0, height - 48, width, height)
    if bar is None:
        raise Failure(
            f"the progress bar's fill colour {bar_fill} is nowhere in the "
            "bottom of the screen. Either it was never drawn, or the "
            "channel order is wrong and it came out blue."
        )
    checks += 1

    _, bar_y = bar
    if at(width - 20, bar_y) != bar_fill:
        raise Failure(
            f"the progress bar is not full at the right-hand end: "
            f"{at(width - 20, bar_y)} rather than {bar_fill}. BOOT_STAGES "
            "and the number of stages actually announced disagree."
        )
    checks += 1

    return checks


#
# **What ends the desktop, as two bytes on the serial line.**
#
# It was `\x03` - Control-C - and that was the first line of the window
# manager's key handler, before anything else could look at the byte. Which
# meant copy closed the desktop, so Control-C became `Control-W Q`: a prefix
# and a letter, two deliberate presses for the most destructive thing this
# keyboard can do.
#
# A name rather than the bytes at eighteen call sites, because eighteen
# copies of `b"\x17q"` is eighteen places to be wrong on the day it changes
# again - and it has now changed once.
#
STOP_DESKTOP = b"\x17q"


def check_bars(data):
    """Phase two: what Lua drew, through gfx.screen().

    This is the one no in-guest test can do. A surface that steps by
    `width * 4` instead of by the pitch is perfectly self-consistent to
    everything inside the guest, and produces bars that walk sideways by
    sixteen pixels a row out here.
    """
    width, height, px = data

    def at(x, y):
        o = (y * width + x) * 3
        return tuple(px[o:o + 3])

    rows = [1, 5, 50, 200, 400, 600, height - 2]
    checked = 0

    for x, colour in BARS:
        want = ((colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff)

        for y in rows:
            got = at(x + BAR_WIDTH // 2, y)

            if got != want:
                # Say where it actually went; a shear has a signature.
                found = [
                    sx for sx in range(0, width)
                    if at(sx, y) == want
                ]
                where = (f"found at x={found[0]}..{found[-1]}"
                         if found else "not on this row at all")

                raise Failure(
                    f"the bar drawn at x={x} is {got} at (x={x}, y={y}), "
                    f"expected {want}; {where}.\n"
                    "A bar that moves sideways as y increases means a "
                    "surface stepped by width * 4 instead of by the pitch. "
                    f"This display's stride is padded, so the drift is "
                    f"sixteen pixels per row."
                )

            checked += 1

        # And that it really is a bar rather than a fill: black beside it.
        if at(x - 4, height // 2) != (0, 0, 0):
            raise Failure(f"the area left of the bar at x={x} is not black.")
        checked += 1

    return checked


def check_keyboard(guest):
    """Phase three: real key events, through the virtio keyboard.

    Everything typed at the shell until now arrived over the serial line.
    This types `2+2` and a shifted `H` as key *events*, which go through
    QEMU's input subsystem into the virtio-input device and come back out of
    a virtqueue - a path that shares nothing with the UART.

    Two things are checked and the second is the interesting one: that the
    answer appears at all, and that shift works. A keymap indexed wrongly
    still produces characters; producing the *right* character under shift
    is what says the two tables and the modifier tracking agree.
    """
    before = len(guest.seen)

    for key in ("2", "shift-equal", "2", "ret"):
        guest.sendkey(key)

    guest.wait_for("2+2", "echoed a key press")

    # The shell answers 4. Waiting for the echo above is not enough: that
    # only proves the characters arrived, not that the line was submitted -
    # which is what the Enter key is for and what a wrong keycode for it
    # would break.
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        guest._read_available()
        if "\n4\n" in guest.seen[before:] or "4\r\n" in guest.seen[before:]:
            break
        time.sleep(0.2)
    else:
        raise Failure(
            "typing 2+2 and Enter on the keyboard did not produce 4.\n"
            f"--- what arrived ---\n{guest.seen[before:]}"
        )

    # Shift. `H` is a different table from `h`, and an unshifted keymap
    # would give a lower-case one that Lua reports differently.
    mark = len(guest.seen)
    for key in ("shift-h", "shift-i", "ret"):
        guest.sendkey(key)

    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        guest._read_available()
        if "HI" in guest.seen[mark:]:
            break
        time.sleep(0.2)
    else:
        raise Failure(
            "shift-h shift-i did not echo as HI, so the shift keymap or the "
            "modifier tracking is wrong.\n"
            f"--- what arrived ---\n{guest.seen[mark:]}"
        )

    return 2


# Typed at the prompt, and whose refusal each must print. Each argument is
# one the program turns down before it opens a window or maps a byte, so the
# refusal is quick and is the program's own sentence. `snes --scale 3` is the
# line that found the bug; `quake` answers "not built with QUAKE=1" in an
# image without it, which is the same proof - the program ran.
PROGRAMS_BY_NAME = (
    ("snes --scale 3", "snes"),
    ("doom /nowhere.wad", "doom"),
    ("quake /nowhere.pak", "quake"),
)


def check_programs_by_name(guest):
    """A program in /bin reached by typing its name, and nothing hiding one.

    The shell sends a word to Lua when the word already names something in
    its environment - `print` stays the function - and to `/bin` otherwise.
    So a *global* with a program's name makes the program unreachable, and
    that is what the Doom, Quake and Super Nintendo kits did: each set a
    global named after its program in every Lua state, the shell's included.
    `snes --scale 3` printed `table: 0x00000081002300` and ran nothing, since
    `--scale 3` became a comment. They are `use("/kits/snes")` now.

    Three checks, and the first is the one that holds the class. The shell
    walks `/bin` against its own environment and names every program it
    hides - for any name, not only these three, so the next thing to leak a
    global is caught whatever it is called. The second asks the image for
    its kits and requires each to be a table. The third types the three
    lines and requires each program's own refusal, which is what a person
    sees - and not "not built with" from a program whose kit is listed,
    which is what the second exists to make checkable.
    """
    mark = len(guest.seen)

    # The count is printed so an empty `/bin` cannot pass by hiding nothing.
    # Assembled at run time so waiting for it cannot match the echo.
    guest.type('local n = 0 '
               'for _, e in ipairs(fs.list("/bin") or {}) do '
               'local w = tostring(e):match("^(.-)%.lua$") '
               'if w then n = n + 1 '
               'if _ENV[w] ~= nil then print("hidden: " .. w) end end end '
               'print("bin" .. "-scanned " .. n)')
    guest.wait_for("bin-scanned ", "walked /bin against its environment")

    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        said = guest.seen[mark:].replace("\r", "")
        counted = re.search(r"^bin-scanned (\d+)$", said, re.M)
        if counted:
            break
        time.sleep(0.2)
    else:
        raise Failure("the /bin walk never printed its count.\n"
                      f"--- what arrived ---\n{guest.seen[mark:]}")

    hidden = re.findall(r"^hidden: (\S+)$", said, re.M)
    scanned = int(counted.group(1))

    if scanned < 20:
        raise Failure(
            f"the /bin walk saw {scanned} program(s), so it checked nothing "
            "worth trusting - fs.list is not answering for /bin.\n"
            f"--- what arrived ---\n{said}")

    if hidden:
        raise Failure(
            f"{len(hidden)} of {scanned} program(s) in /bin cannot be run by "
            f"typing their name, because the shell's environment already "
            f"holds that name: {', '.join(hidden)}. A kit that sets a global "
            "does this; a kit is reached with use(\"/kits/<name>\").")

    #
    # Which kits this image has, and that each one is a table.
    #
    # Without this the lines below cannot tell a missing build from a broken
    # kit, because both end in the same refusal. The move that made these
    # three kits left one `lua_setglobal` behind: `sys.kit("snes")` then
    # returned its own argument, `snes.lua` saw a string, and said the image
    # was not built with SNES=1 - in an image that was. This phase passed it.
    #
    mark = len(guest.seen)
    guest.type('for _, k in ipairs(sys.kit_names()) do '
               'print("kit: " .. k .. " " .. type(sys.kit(k))) end '
               'print("kits" .. "-listed")')
    guest.wait_for("kits-listed", "listed its kits")

    said = guest.seen[mark:].replace("\r", "")
    kits = dict(re.findall(r"^kit: (\S+) (\S+)$", said, re.M))
    broken = sorted(k for k, kind in kits.items() if kind != "table")

    if not kits:
        raise Failure("sys.kit_names() listed no kits at all.\n"
                      f"--- what arrived ---\n{said}")

    if broken:
        raise Failure(
            "sys.kit answered with something other than a table for "
            + ", ".join(f"/kits/{k} (a {kits[k]})" for k in broken)
            + ". A kit's build function has to leave its table on top of "
            "the stack; one that also sets a global leaves the caller's "
            "argument there instead.")

    for line, name in PROGRAMS_BY_NAME:
        reaches_program(guest, line, name, built=name in kits)

    return 2 + len(PROGRAMS_BY_NAME)


def reaches_program(guest, line, name, built=None):
    """Types `line` at the prompt and fails unless `/bin/<name>.lua` answered.

    `built` is whether the image lists `/kits/<name>`. When it does, "this
    image was not built with" is the wrong answer even though it is the
    program's own.

    Its own function so it can be run without the walk above, which fails
    first on the same bug and would otherwise be the only half ever seen
    failing.
    """
    mark = len(guest.seen)
    guest.type(line)

    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if PROMPT in guest.seen[mark:]:
            break
        time.sleep(0.2)
    else:
        raise Failure(f"`{line}` at the prompt never came back to it.\n"
                      f"--- what arrived ---\n{guest.seen[mark:]}")

    said = guest.seen[mark:].replace("\r", "")

    if not re.search(rf"^{name}: ", said, re.M):
        raise Failure(
            f"`{line}` at the prompt did not reach /bin/{name}.lua - "
            f"nothing it printed starts `{name}: `. A `table: 0x...`, or "
            "an `error:` from Lua, means the shell took the line as Lua "
            "because a global of that name exists.\n"
            f"--- what arrived ---\n{said}")

    if built and re.search(rf"^{name}: this image was not built with",
                           said, re.M):
        raise Failure(
            f"`{line}` reached /bin/{name}.lua, and it said the image was "
            f"not built with it - but this image lists /kits/{name}, so the "
            "program did not get the kit it asked for.\n"
            f"--- what arrived ---\n{said}")


def _band_changed(width, height, a, b):
    """Did anything in the reserved rows differ between two pictures?

    Only those rows, and that is what makes both phases below sound. The
    kernel console keeps its cursor inside the scroll region - `rows` is
    already RESERVED_ROWS short - so nothing else in the system draws there.
    """
    for y in range(height - RESERVED_PX, height):
        row = y * width * 3

        if a[row:row + width * 3] != b[row:row + width * 3]:
            return True

    return False


def check_status_bar(guest):
    """A detached program still runs, and its drawing still reaches the screen.

    `monitor` redraws the reserved rows once a second from a process of its
    own. Every earlier version of it did not: one drew a single time, one
    redrew only after each typed command, and one counted yields rather than
    seconds - and all three look identical from the serial line, because they
    draw on the framebuffer and say nothing. So it is checked from outside.

    Only the reserved rows are looked at, and that is what makes the check
    sound. The kernel console keeps its cursor inside the scroll region -
    `rows` is already RESERVED_ROWS short - so nothing else in the system
    draws below that line. Any pixel that changes there changed because a
    detached process drew it.

    Scanning the whole screen instead would not work, and the reason is worth
    recording: the cursor blinks twice a second, so on a whole number of
    seconds it is caught in the same phase and contributes nothing, while on
    any other interval it looks exactly like a program drawing. Two pictures
    3.0 seconds apart reported a completely static screen on a system where
    two separate things were moving. 3.3 here for the same reason - it is not
    aligned to the blink or to the monitor's own second.
    """
    guest.type("monitor 30 &")
    time.sleep(2.5)

    width, height, before = parse_ppm(guest.screendump())
    time.sleep(3.3)
    _, _, after = parse_ppm(guest.screendump())

    if not _band_changed(width, height, before, after):
        raise Failure(
            "nothing in the bottom {0} rows changed in 3.3 seconds with "
            "`monitor 30 &` running. Nothing but a detached program draws "
            "there, so either the trailing & is not detaching, or the "
            "program is not redrawing on a clock of its own.".format(
                RESERVED_PX // GLYPH_H)
        )

    return 1


def check_interrupt(guest):
    """Control-C, from the keyboard, stopping a program in the foreground.

    `monitor 30` without a trailing `&` holds the shell for thirty seconds
    while redrawing the reserved rows once a second. Control-C has to end it
    early, and both halves are checked: the rows go still, and the prompt
    comes back.

    Foreground on purpose. Who receives the interrupt is decided by who is
    reading the keyboard, and that turns out to be the right rule by itself:

      * a program in the foreground - the shell is parked in `wait`, not
        reading, so the console hands byte 3 to whoever asks for it, which
        is the program.
      * nothing running - the console is inside `read` for the shell, takes
        the byte itself, and abandons the line, which is what Control-C does
        at a prompt.
      * a program in the background - the shell is at the prompt, so the
        line editor takes it and the background program carries on. Which
        is also correct, and is why this phase does not use `&`.

    Three separate things have to work, and none is visible over serial:
    the driver has to turn a Control press and a C press into byte 3 (the
    terminal does that on the other side of a cable, so Control-C worked
    over serial long before it did anything in the window), the console
    server has to hand it to a program that asks, and the program has to act.
    """
    guest.type("monitor 30")
    time.sleep(2.5)

    width, height, before = parse_ppm(guest.screendump())
    time.sleep(1.6)
    _, _, during = parse_ppm(guest.screendump())

    if not _band_changed(width, height, before, during):
        raise Failure(
            "a foreground `monitor 30` was not drawing at all, so there is "
            "nothing here for Control-C to stop. Check the status bar phase "
            "first - this one cannot mean anything until that passes."
        )

    mark = len(guest.seen)
    guest.sendkey("ctrl-c")
    time.sleep(2.5)                  # the poll is once a second, plus the wipe

    _, _, after_stop = parse_ppm(guest.screendump())
    time.sleep(3.3)
    _, _, later = parse_ppm(guest.screendump())

    if _band_changed(width, height, after_stop, later):
        raise Failure(
            "the reserved rows were still changing 2.5 seconds after "
            "ctrl-c, so `monitor` did not stop. Either the keyboard is not "
            "producing byte 3 for Control-C, or the console server is not "
            "reporting it, or the program is not asking."
        )

    # And the shell has to be usable again. A program that stopped drawing
    # but never returned would pass the check above and leave the system
    # exactly as stuck as before.
    guest.type("")
    deadline = time.monotonic() + 10

    while time.monotonic() < deadline:
        guest._read_available()
        if PROMPT in guest.seen[mark:]:
            break
        time.sleep(0.2)
    else:
        raise Failure(
            "the reserved rows went still after ctrl-c but the prompt never "
            "came back, so the shell is still waiting for a program that "
            "stopped drawing.\n"
            f"--- since the interrupt ---\n{guest.seen[mark:]}"
        )

    return 2


# The hung application's title bar, 0xda3633. Looked for by colour rather
# than by position, because the whole point of the phase below is that the
# position changes.
HUNG_TITLE = (0xda, 0x36, 0x33)


def find_colour_anywhere(width, height, px, want):
    """Where the first pixel of this colour is, scanning top to bottom.

    Every other pixel, both ways: the thing being looked for is a title bar
    three hundred pixels wide and twenty-eight tall, so a step of two cannot
    miss it and the scan is four times cheaper.
    """
    for y in range(0, height, 2):
        for x in range(0, width, 2):
            at = (y * width + x) * 3

            if (px[at], px[at + 1], px[at + 2]) == want:
                return x, y

    return None


def find_colour_in(width, px, box, want):
    """Where the first pixel of this colour is inside `box` (x, y, w, h),
    scanning rows top to bottom and every pixel.

    **For the gallery's list, which is not the only accent on the screen any
    more**: its verb is a button filled with the accent, and so is a switch
    that is on - so a scan of the whole screen for the selection found the
    button first, and an arrow that moved the list moved nothing it could
    see."""
    x0, y0, w, h = box

    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            at = (y * width + x) * 3

            if (px[at], px[at + 1], px[at + 2]) == want:
                return x, y

    return None


# The list selection in the gallery, 0x1f6feb, which is the accent colour.
SELECTED = (0x1f, 0x6f, 0xeb)

# A window's frame down its sides and along its bottom, `BORDER` in
# `wm.lua`: 4 since 24 September - Diego asked for "some extra chrome to the
# other borders", it was 6 for an afternoon and then "a couple of pixels"
# less; it had been 2.
FRAME = 4

# The Deskbar's height. It is the strip across the top, it is
# tab-coloured, and it is not a window - so anything counting windows
# by their title bars starts below it. 32 and fixed since 22 September
# (`theme.metrics.deskbar`, `roadmap.md` 5v); it was 36.
STRIP_H = 32

# And its icons, `ICON` in deskbar.lua: 24 in the 32-pixel bar, where they
# were 32 in the 36-pixel one.
DESKBAR_ICON = 24

# A menu row, `theme.metrics.row` - the one fixed layout the faces fit
# (`roadmap.md` 5x). `menu_metrics` in `ui.lua` puts two pixels of edge above
# the first row, so row `i` of a menu opened at `y` starts at
# `y + 2 + (i - 1) * MENU_ROW`. The row is `LAYOUT_ROW`, and was a copy of it
# at 24 that stayed 24 when the row became 32 - which is how the direct
# menu phase came to press row 2 for row 3.
MENU_ROW = LAYOUT_ROW


def check_registry(guest):
    """A window manager started a second time is found by name.

    `wm` registers itself in /app, and a program started with `run` and
    handed nothing else reaches `/app/wm` by asking the registry for that
    name - which is how `desktop` starts the Tracker. Stopped with
    Control-C, a window manager destroys its endpoint and does not
    unregister, and the registry used to keep the name: the next one was
    filed as `wm2`, a lookup of `wm` was handed an endpoint that had ended,
    and the Tracker died at once with "no such path: /app/wm" under a
    desktop that was running.

    So this starts a window manager, stops it, and starts another. Each
    time, the program `wm` launches starts a second one with `run` and no
    shares, and that one looks up /app/wm, calls it, and prints what /app
    holds.

    **The first start is the control, which is why this phase comes before
    any other that starts a window manager.** Nothing in the registry can be
    stale yet, so the probe has to be answered there; a failure on the
    second start is then the restart's and not the probe's.

    Checked by what the program says rather than by the screen, because
    nothing here opens a window: a lookup that succeeds and an endpoint that
    answers are the whole of the question.
    """
    # The marker is split in the source, so the echo of the line that writes
    # the program cannot be mistaken for the program printing it.
    probe = (
        "local answer, why = fs.send('/app/wm', { type = 'windows' }) "
        "local said = answer and (answer.ok and 'answered' "
        "or tostring(answer.error)) or tostring(why) "
        "print('registry' .. '-probe: ' .. said .. ' | ' "
        ".. table.concat(fs.list('/app') or {}, ' ') .. ' |')"
    )
    launcher = (
        "local ok, why = run('/ramfs/probe.lua', '', false) "
        "if not ok then print('registry' .. '-probe: ' .. tostring(why) "
        ".. ' | |') end"
    )

    guest.type("fs.write('/ramfs/probe.lua', %r)" % probe)
    time.sleep(2)
    guest.type("fs.write('/ramfs/viarun.lua', %r)" % launcher)
    time.sleep(2)

    def start_and_probe(which):
        mark = len(guest.seen)
        guest.type("wm /ramfs/viarun.lua")

        deadline = time.monotonic() + 40
        found = None

        while found is None and time.monotonic() < deadline:
            time.sleep(0.3)
            guest._read_available()
            found = re.search(r"registry-probe: ([^\n|]*)\|([^\n|]*)\|",
                              guest.seen[mark:])

        if found is None:
            raise Failure(
                f"the {which} window manager started, and the program run "
                "under it never said what it found at /app/wm.\n"
                "--- what the guest said ---\n" + guest.seen[mark:][-800:])

        # Back to the shell, the way every phase here ends.
        stop = len(guest.seen)
        guest.proc.stdin.write(STOP_DESKTOP)
        guest.proc.stdin.flush()

        deadline = time.monotonic() + 15

        while time.monotonic() < deadline:
            guest._read_available()

            if PROMPT in guest.seen[stop:]:
                break

            time.sleep(0.3)
        else:
            raise Failure(f"Control-C did not get the screen back from the "
                          f"{which} window manager.")

        return found.group(1).strip(), found.group(2).split()

    said, names = start_and_probe("first")

    if said != "answered":
        raise Failure(
            "under the first window manager of this boot, a program started "
            f"with `run` could not reach it through /app/wm: {said}. /app "
            f"held {names}. Nothing in the registry can be stale yet, so the "
            "lookup or the probe is what broke, not a restart.")

    said, names = start_and_probe("second")

    if said != "answered":
        raise Failure(
            "under a second window manager, a program started with `run` "
            f"could not reach it through /app/wm: {said}. /app held {names}. "
            "The registry is handing out a name whose holder has ended.")

    if names != ["wm"]:
        raise Failure(
            f"/app held {names} under the second window manager, where `wm` "
            "alone belongs. The first one's name was kept after it ended, so "
            "the second was registered under another.")

    return 3


def check_context(guest):
    """The right button reaches an application, and presses nothing.

    Both pointer drivers have always reported the right button - `hal.h` says
    bit 0 left, bit 1 right - and everything above the HAL threw it away: the
    window manager masked `buttons & 1` and the `mouse` event it posted
    carried no button at all.

    **The danger in delivering it is that a program which has never heard of
    one reads it as a left press and acts on it.** Paint would draw with it,
    Quake would fire, Lite XL would move its cursor. So a right press goes
    only to a window that said it understands `button`, `ui.lua` says so for
    every window it opens, and the kit drops what no view claimed - see
    `handlers.open` in `wm.lua`.

    The probe is one window with both halves, because the interesting claim
    is about the two together: a button to press, and an `on_context`. A
    right press on the button has to reach `on_context` and leave the button
    alone, and a left press on the same pixel has to press it. Checked by
    what the program prints rather than by the screen, because "the button
    was not pressed" is not a picture.
    """
    # Split in the source so the echo of the line that writes the program
    # cannot be mistaken for the program printing it.
    probe = (
        'local ui = use("/lib/ui.lua") '
        'local win = ui.window{ title = "Context", w = 300, h = 160, '
        'x = 200, y = 200 } '
        'win:add(ui.button{ x = 20, y = 40, w = 120, text = "Press", '
        'on_click = function() print("ctx" .. "-probe: pressed") end }) '
        'function win:on_context(x, y) '
        'print("ctx" .. "-probe: context at " .. x .. "," .. y) '
        'return true end '
        'print("ctx" .. "-probe: at " .. win.origin_x .. "," .. win.origin_y) '
        'win:run()'
    )

    guest.type("fs.write('/ramfs/ctxprobe.lua', %r)" % probe)
    time.sleep(2)

    mark = len(guest.seen)
    guest.type("wm /ramfs/ctxprobe.lua")

    deadline = time.monotonic() + 40
    where = None

    while where is None and time.monotonic() < deadline:
        time.sleep(0.3)
        guest._read_available()
        where = re.search(r"ctx-probe: at (\d+),(\d+)", guest.seen[mark:])

    if where is None:
        raise Failure(
            "the window manager started and the probe never said where its "
            "window went.\n--- what the guest said ---\n"
            + guest.seen[mark:][-800:])

    ox, oy = int(where.group(1)), int(where.group(2))
    width, height, _ = parse_ppm(guest.screendump())

    # The button is at 20,40 inside the window and is 120 wide; `ui.button`
    # gives it the font's height plus ten. Its middle, in screen pixels.
    bx, by = ox + 20 + 60, oy + 40 + 13

    def press(button):
        guest.mouse_to(*_to_tablet(bx, by, width, height))
        time.sleep(0.4)
        guest.mouse_button(True, button)
        time.sleep(0.4)
        guest.mouse_button(False, button)
        time.sleep(0.8)
        guest._read_available()

    checks = 0

    # 1 and 2. A right press does not press, and reaches on_context.
    #
    # **Pressed first, and the order is not arbitrary.** Both are wrong when
    # a right press is routed like a left one, and "it pressed the button" is
    # the sentence that says what happened; "on_context never fired" is a
    # symptom of it and sends the reader to the wrong half of the system.
    # Asking the other way round is what this did first, and the negative
    # control below caught it reporting the wrong one - the guest had printed
    # `ctx-probe: pressed` and the failure never mentioned it.
    at_right = len(guest.seen)
    press("right")
    said = guest.seen[at_right:]

    if "ctx-probe: pressed" in said:
        raise Failure(
            "a right press on the probe's button *pressed* it. That is the "
            "whole failure this is here to catch: a right press routed like "
            "a left one draws in Paint and fires in Quake. `dispatch_mouse` "
            "in `ui.lua` has to take `button == \"right\"` out of the normal "
            "path before any widget sees it.\n"
            "--- what the guest said ---\n" + said[-800:])

    checks += 1

    if not re.search(r"ctx-probe: context at \d+,\d+", said):
        raise Failure(
            "a right press on the probe's button never reached its "
            "`on_context`, and did not press it either - so nothing arrived "
            "at all. The window manager sends one only to a window that says "
            "it understands `button`, and `ui.lua` says so for every window "
            "it opens, so either the button never came from the driver or "
            "`wm.lua` dropped it.\n"
            "--- what the guest said ---\n" + said[-800:])

    checks += 1

    # 3. And the left button still does what it always did.
    at_left = len(guest.seen)
    press("left")

    if "ctx-probe: pressed" not in guest.seen[at_left:]:
        raise Failure(
            "a left press on the probe's button no longer presses it, so "
            "adding the right button broke the one that worked.\n"
            "--- what the guest said ---\n" + guest.seen[at_left:][-800:])

    checks += 1

    # Back to the shell, the way every phase here ends.
    stop = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[stop:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back from the probe.")

    return checks



def check_preferences(guest):
    """Preferences: the sidebar picks a page, and the page changes.

    One window drawn from `user/lib/settings.lua` - every row on every page
    comes out of that list, so this phase is really asking whether the list
    reaches the screen at all.

    **What it checks is that the content changes**, not that the highlight
    moves. A sidebar whose selection bar slides down while the right-hand
    side stays put is exactly the bug this application can have and nothing
    else would: the list works, the page is rebuilt, and the rebuild draws
    the same thing because the category never reached it. Comparing the two
    pictures over the content area alone catches that; comparing the
    selection would not.

    Driven from the keyboard, like every other phase here. The sidebar is
    the first focusable thing in the window, so Down moves it.
    """
    guest.type("wm preferences")

    # Through `settle` rather than a bare `screendump`, which is the
    # harness's own idiom: the monitor answers a dump and a key on one
    # socket, and a dump asked for immediately after another can come back
    # as nothing at all. `settle` retries until the picture parses.
    def picture(what):
        """A parsed screendump, retried.

        The monitor answers keys and pictures on one socket and a dump can
        come back empty; `settle` does not catch that, because every other
        phase asks for one only when the screen has already been still for a
        while. This one asks right after a burst of keys.
        """
        for _ in range(12):
            try:
                return parse_ppm(guest.screendump())
            except Failure:
                time.sleep(0.5)

        raise Failure("no picture of Preferences: " + what)

    #
    # **Its own wait, rather than `started`.** `started` and `settle` both
    # call `parse_ppm` without catching it, so a dump that comes back empty
    # - which the monitor does, sharing one socket with the keys - ends the
    # phase instead of being retried. Every other phase asks for a picture
    # only when the screen has been still for a while; this one asks right
    # after typing, and again right after a burst of keys.
    #
    deadline = time.monotonic() + 30
    width = height = 0
    px = b""

    while time.monotonic() < deadline:
        width, height, px = picture("when it opened")

        if count_windows(width, height, px) >= 1:
            break

        time.sleep(0.4)
    else:
        raise Failure("Preferences never put a window on the screen.")


    # Where the window is: its title bar is the only tab-coloured run, and
    # the application asked for 150,100 so this is a check as much as a
    # measurement - a window that opened somewhere else would read nothing.
    found = find_colour_anywhere(width, height, px, TAB)

    check = []

    if found is None:
        raise Failure("Preferences opened no window with a title bar on it.")

    wx, wy = found

    # The content area: right of the sidebar, below the title bar. Numbers
    # from the application's own SIDE and the window's size, kept here so a
    # layout change has to be agreed with rather than silently tracked.
    x0, y0 = wx + 230, wy + 60
    x1, y1 = wx + 760, wy + 400

    def content(px_):
        """A histogram of the content area, which is enough to say whether
        the page is the same page - and cheap, where comparing pixel by
        pixel would fail on a cursor or a one-pixel scroll.

        Indexed straight into the pixels: `pixel_reader` takes a whole
        screendump and parses it again, and this already has the parsed
        bytes."""
        seen = {}

        for y in range(y0, min(y1, height), 3):
            base = y * width * 3

            for x in range(x0, min(x1, width), 3):
                at = base + x * 3
                c = bytes(px_[at:at + 3])
                seen[c] = seen.get(c, 0) + 1

        return seen

    before = content(px)

    # Down the sidebar. The list has the focus when the window opens, so no
    # Tab is needed; five presses clears the gap rows and lands well away
    # from where it started.
    # A pause between them, as every other phase here does: the monitor
    # answers keys and pictures on one socket, and a burst with no gap both
    # loses presses and collides with the dump that follows.
    for _ in range(5):
        guest.sendkey("down")
        time.sleep(0.3)

    time.sleep(1.5)

    width2, height2, px2 = picture("after moving down the sidebar")
    after = content(px2)

    same = sum(min(before.get(c, 0), after.get(c, 0))
               for c in set(before) | set(after))
    total = max(sum(before.values()), 1)

    check.append((
        same < total * 0.92,
        "the content beside the sidebar is %d%% the same after moving down "
        "five categories, so the page did not change. The selection can move "
        "while the page does not: the list works, the rebuild runs, and the "
        "category never reaches it." % (100 * same // total)))

    #
    # **And that choosing a look changes the machine.**
    #
    # Diego, on the ThinkCentre M700 running 0.10.146: *"the preferences
    # pane that is usable but does nothing to the system"*. He was right,
    # and the phase above could not have seen it: it asks whether the page
    # changes when the category does, which was always true. Every *choice*
    # in the window stored a value and returned - `control_for`'s dropdown
    # never called `live` at all - so the look was written to
    # `/home/.appearance` and the desktop went on wearing the old one until
    # the next boot.
    #
    # The look is the right one to drive, because it is the loudest: a
    # palette reaches every window and the desktop behind them, so "did
    # anything happen" is a question about the whole screen rather than
    # about one control's pixels.
    #
    # Driven by the keyboard alone: the swatches take the arrows, which is
    # what a person trying looks would reach for.
    #
    # **Back to Appearance first**, because the check above left the sidebar
    # five categories down and the page with it. The first version of this
    # did not, tabbed into whatever control that page happened to start
    # with, and opened a four-item menu 72 pixels wide - the Scale, on
    # Displays. A control answering is not evidence that the right one did.
    #
    for _ in range(6):
        guest.sendkey("up")
        time.sleep(0.25)

    time.sleep(1.0)
    mark = len(guest.seen)

    # One Tab from the sidebar is the Theme row's swatches: the sidebar holds
    # the focus when the window opens, and the swatches are the first control
    # the Appearance page adds. Right chooses the next look along, as the
    # sidebar's arrows choose a category - `docs/preferences.html` draws the
    # look as swatches, and it was a dropdown for one version.
    def whole(px_, w_, h_):
        """A histogram of the whole screen. A look reaches the desk, every
        window's tab and every window's inside, so this is the measurement
        that matches the claim - where one sampled pixel would be asking
        whether one thing happened to change."""
        seen = {}

        for y in range(0, h_, 6):
            base = y * w_ * 3

            for x in range(0, w_, 6):
                at = base + x * 3
                c = bytes(px_[at:at + 3])
                seen[c] = seen.get(c, 0) + 1

        return seen

    guest.sendkey("tab")
    time.sleep(0.4)

    w0, h0, px0 = picture("before the look was changed")
    looked = whole(px0, w0, h0)

    guest.sendkey("right")

    def changed(w_, h_, px_):
        now = whole(px_, w_, h_)
        same = sum(min(looked.get(c, 0), now.get(c, 0))
                   for c in set(looked) | set(now))
        total = max(sum(looked.values()), 1)

        return True if same < total * 0.90 else None

    try:
        settle(guest, changed,
               "the screen is the same after the next look was chosen in "
               "Preferences, so the choice was stored and never applied. A "
               "`theme` request reaches the desk, every window's tab and "
               "every window's inside; the swatches have to send one as well "
               "as write the file.", seconds=15)
        check.append((True, ""))
    except Failure as e:
        check.append((False, str(e)))

    # And the manager said so, which is what a person reads on a machine with
    # no harness (`log`).
    guest._read_available()
    check.append((
        "wm: theme applied" in guest.seen[mark:],
        "the window manager applied a palette and said nothing about "
        "it. `handlers.theme` answers a person's press, and a look that "
        "was chosen has to read differently in the log from one that "
        "was never sent - which is exactly what could not be told apart "
        "on the M700."))

    stop_desktop(guest)

    #
    # **The look this phase chose is this phase's, not the next one's.**
    #
    # It writes `/home/.appearance` through Preferences, exactly as a person
    # would, so the desktop that starts after this one wears Endeavour - and
    # every phase after it counts windows by the tab colour it was written
    # against and finds none. Two suites failed that way on the first full
    # run, in phases that have nothing to do with Preferences.
    #
    # The same tidy-up Log View does after choosing a text size, and for the
    # same reason: a phase that changes a setting owns putting it back.
    #
    guest.type(appearance() + ' print("preferences" .. "-restored")')
    guest.wait_for("preferences-restored",
                   "put the harness's own palette and faces back")

    failed = [why for ok, why in check if not ok]

    for why in failed:
        print("FAIL: " + why)

    return len(check) - len(failed) if not failed else -len(failed)


def gallery_layout(guest, mark, seconds=30):
    """What `gallery.lua` says about itself: its size, where its list is and
    the centres of its two buttons and its field, in points inside the
    window.

    **Read rather than copied.** The phases that drive the gallery each held
    its layout as numbers - a list at 16,172, rows 16 apart, a status line
    at 296 - and when the gallery was redrawn to `docs/apps.html` those
    numbers were four copies of a window that no longer existed.
    """
    deadline = time.monotonic() + seconds
    pattern = (r"gallery: (\d+)x(\d+), list at (\d+),(\d+) (\d+)x(\d+), "
               r"buttons at (\d+),(\d+) (\d+),(\d+), field at (\d+),(\d+)")

    while time.monotonic() < deadline:
        guest._read_available()
        m = re.search(pattern, guest.seen[mark:])

        if m:
            v = [int(g) for g in m.groups()]
            return {"size": (v[0], v[1]), "list": tuple(v[2:6]),
                    "press": (v[6], v[7]), "verb": (v[8], v[9]),
                    "field": (v[10], v[11])}

        time.sleep(0.3)

    raise Failure("the gallery never said where its controls are:\n"
                  + guest.seen[mark:][-800:])


def check_widgets(guest):
    """The UI kit, driven from the keyboard.

    Opens the gallery under the window manager and works every control:
    presses a button, ticks a checkbox, types into a field, moves a list
    with the arrows and selects. Then reads the status label back off the
    screen - by finding the list's selection bar, which only lands where it
    lands if the arrows arrived.

    This is the phase that caught the design error worth having. The window
    manager originally took Tab for "next window" and the arrows for "move
    the window", and with the gallery on screen it was immediately obvious
    that a manager holding those keys has decided no application may have a
    second control. There is one reserved key now and it introduces a
    command.

    Before the window-drag phase, which needs the desktop to itself.
    """
    mark = len(guest.seen)
    guest.type("wm gallery")
    started(guest)
    laid = gallery_layout(guest, mark)
    lx, ly, lw, lh = laid["list"]

    width, height, px = parse_ppm(guest.screendump())
    box = (60 + lx, 90 + ly, lw, lh)
    before = find_colour_in(width, px, box, SELECTED)

    if before is None:
        raise Failure(
            "the gallery's list has no selection bar on screen, so either "
            "the window never opened or the kit did not draw it."
        )

    #
    # **The list's thumb is the scrollbar's own grey, ridged** - not the
    # tab's colour. It wore the tab's from 22 September (`roadmap.md` 5y)
    # until Diego took it back on 24 September: "We should go back to
    # scrollbars and handle with the same color". The gallery's list is five
    # items in three rows, so it has a bar; in the harness's `dark` look the
    # thumb is `raised`, #21262d, with four ridges each an `edge_light` line
    # directly over an `edge_dark` one. Counted in the strip the bar
    # occupies - the list's last eighteen columns - where the tab's yellow
    # has no business at all: the control for a thumb in the tab's colour is
    # the build before this, which fails the first count.
    #
    list_x, list_y = 60 + lx, 90 + ly
    RAISED, LIT, DARK = (0x21, 0x26, 0x2d), (0x42, 0x4a, 0x55), (0x05, 0x08, 0x0c)
    yellow = face = ridges = 0

    for y in range(list_y, list_y + lh - 1):
        for x in range(list_x + lw - 18, list_x + lw):
            at = (y * width + x) * 3
            below = at + width * 3
            p = (px[at], px[at + 1], px[at + 2])

            if p == TAB:
                yellow += 1
            elif p == RAISED:
                face += 1
            elif p == LIT and (px[below], px[below + 1], px[below + 2]) == DARK:
                ridges += 1

    if yellow or face < 150 or ridges < 16:
        raise Failure(
            f"the gallery's scrollbar has {yellow} pixels of the tab's yellow, "
            f"{face} of the controls' face and {ridges} of ridge - wanted a "
            "thumb in the scrollbar's own grey with a grip across it, and "
            "none of the title bar's colour."
        )

    def send(data, wait=0.4):
        guest.proc.stdin.write(data)
        guest.proc.stdin.flush()
        time.sleep(wait)

    #
    # The card's order, from the first button: the verb, the switch, the
    # tick, the choice, the level and the field. A space on the choice
    # would open its menu, so it is passed over rather than pressed.
    #
    send(b"\t\t")            # past both buttons, to the switch
    send(b" ")                # flip it
    send(b"\t")               # to the tick
    send(b" ")                # tick it
    send(b"\t\t\t")          # past the choice and the level, to the field
    send(b" edited")

    #
    # The list is focused by clicking it, not by tabbing to it.
    #
    # Tabbing there worked and was occasionally a row short in a long run,
    # because it depends on this phase's idea of the focus order matching
    # the gallery's - five controls, in the order they were added, with the
    # focus starting on the first. Any of those changing makes this phase
    # fail somewhere unrelated to what it is about.
    #
    # A click puts the focus where the click is. Tab is still being tested,
    # by everything above this line.
    #
    guest.mouse_to(*_to_tablet(list_x + 60, list_y + 8, width, height))
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)
    guest.mouse_button(False)
    time.sleep(0.6)

    width, height, px = parse_ppm(guest.screendump())
    bar_before = find_colour_in(width, px, box, SELECTED)

    #
    # One arrow at a time, and *waited for* rather than slept after.
    #
    # A fixed pause here was flaky in a full run and never standalone, which
    # is the signature of a harness that is racing rather than a system that
    # is broken: an application blocks for up to a second between events, so
    # how long a keystroke takes to show up depends on where in that second
    # it arrived. Watching for the result removes the guess.
    #
    #
    # Real key presses, through QEMU's input plumbing into the virtio
    # keyboard - not escape sequences down the serial line.
    #
    # This is the difference that hid a real bug. An arrow over a cable is
    # three bytes because that is what a terminal sends; on a keyboard it is
    # a single keycode with no character at all, and the driver's keymap has
    # one byte per code, so `keymap_plain[108]` was zero and arrows produced
    # nothing. They worked over serial and did nothing in the window, and
    # every check here typed over serial.
    #
    for step in (1, 2):
        guest.sendkey("down")
        time.sleep(0.2)

        #
        # Twenty-five seconds, which sounds absurd for a keystroke and is
        # not: another process may be polling the console for Control-C
        # between this one and the desktop, and the desktop itself sleeps a
        # tick a pass. Ten seconds was enough almost always, which is the
        # worst amount to be enough.
        #
        deadline = time.monotonic() + 25

        while time.monotonic() < deadline:
            width, height, px = parse_ppm(guest.screendump())
            after = find_colour_in(width, px, box, SELECTED)

            if after is not None and after[1] - bar_before[1] >= step * LAYOUT_ROW:
                break

            time.sleep(0.3)
        else:
            raise Failure(
                f"down arrow {step} never moved the list's selection "
                f"({bar_before} then {after}). The list had been clicked, so "
                "it had the focus; the key is not arriving."
            )

    send(b"\r", 1.0)

    rows = (after[1] - bar_before[1]) / float(LAYOUT_ROW)

    if abs(rows - 2) > 0.5:
        raise Failure(
            f"two down arrows moved the list's selection {rows:.1f} rows "
            f"rather than 2 ({bar_before} then {after})."
        )

    # Hand the screen back, or the next phase types its command at a shell
    # that is still blocked waiting for this window manager to finish.
    mark = len(guest.seen)
    send(STOP_DESKTOP, 2.0)

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        #
        # With what the guest said, because this sentence alone is every
        # cause at once: a window manager that never saw the byte, one that
        # saw it and did not finish, and a shell that came back and printed
        # nothing. It failed once in a full run and passed on its own, and
        # the next time it fails the transcript is the only way to tell.
        #
        raise Failure(
            "Control-C did not get the screen back from the window manager, "
            "so the shell is still waiting for it.\n"
            "--- what the guest said after Control-C ---\n"
            + (guest.seen[mark:][-3000:] or "(nothing at all)")
            + "\n--- and just before it ---\n"
            + guest.seen[max(0, mark - 1500):mark]
        )

    return 2


# The focused window's tab, 0xffc700.
TAB = (0xff, 0xc7, 0x00)

# How far a pixel may sit from a tab colour and still be that tab.
#
# **Zero until the chrome stopped being flat.** These three functions each
# tested a pixel with `== TAB`, which was exactly right while a title bar was
# one colour, and became wrong the moment `theme.chrome` gave it a gradient:
# the bar now runs from the base colour lifted 13 counts to the base dropped
# 9, so the row that is *exactly* TAB is one row out of twenty and may not
# exist at all once the ends clamp. `count_windows` needs a run of 40 such
# pixels and found none, so a screen with three windows on it counted zero.
#
# 20 rather than 13 because the ramp's ends clamp per channel and the
# arithmetic rounds, and well under the distance to any other colour that
# appears in a run this long.
TAB_TOL = 20


def is_tab(px, at, base=TAB):
    """Whether the pixel at byte offset `at` belongs to a tab of `base`."""
    return (abs(px[at]     - base[0]) <= TAB_TOL
            and abs(px[at + 1] - base[1]) <= TAB_TOL
            and abs(px[at + 2] - base[2]) <= TAB_TOL)


def tab_width(width, height, px):
    """How wide the widest run of tab colour on screen is, in pixels.

    It used to be that the tab was as wide as its title and no wider, so
    this measured the title's length. It does not any more - the decoration
    is one colour across the whole frame - so this now measures the widest
    *window*, which is still what several checks want.
    """
    widest = 0

    for y in range(0, height, 2):
        run = 0

        for x in range(width):
            at = (y * width + x) * 3

            if is_tab(px, at):
                run += 1

                if run > widest:
                    widest = run
            else:
                run = 0

    return widest


def tab_top(width, height, px):
    """The first row holding the focused window's decoration, or None.

    The top of the tab, which is where a title is drawn a few rows below.
    Searched from the top of the screen so that the frontmost window wins
    when several overlap.
    """
    for y in range(height):
        for x in range(0, width, 2):
            at = (y * width + x) * 3

            if is_tab(px, at):
                return y

    return None


def check_default_look(guest, ask_wm):
    """**What a machine nobody has told looks like**, which since 19
    September is IBM Plex (`docs/styleguide.html`, roadmap 5).

    Run before the harness pins the bitmap faces its own rows were measured
    against - so this is the only place the default can be checked, and
    without it the pin would hide a face that had stopped loading.

    **The first version of this check could not fail, and the desktop it
    passed on was visibly broken.** It read `theme.fonts` - the constants in
    the file it had just loaded - and compared them with themselves, then
    measured three strings in a console process where nothing had ever
    loaded a face, so every width came back from the bitmap and the "it
    drew" check was `48 > 0`. It passed on the build whose screenshot shows
    menu titles overlapping and every button clipped.

    So three things, and the first two are the ones that bite:

    - **What the window manager holds**, which it is asked for rather than
      told: its `theme` reply carries `held`, the faces it actually loaded,
      beside `fonts`, the ones it hands to applications. That process draws
      the text of every ordinary window, so a desktop holding the bitmap
      while advertising Plex lays out in one face and paints in the other -
      which is exactly what `load_appearance` did, applying a saved font
      table when there was none to apply.
    - **That the face is proportional once loaded**, measured as the
      difference between ten narrow letters and ten wide ones. A face that
      failed to parse leaves the bitmap, where both are eighty pixels.
    - That the roles in the file say what the style guide says.
    """
    checks = 0
    program = (
        "local theme = use('/lib/theme.lua') "
        "local f = theme.fonts "
        "local ok = {} "
        "for _, r in ipairs { 'ui', 'title', 'text', 'mono' } do "
        "ok[#ok + 1] = gfx.use_font(f[r].font, f[r].px, r) and 'y' or 'n' end "
        "local thin = gfx.measure('iiiiiiiiii', 'ui') "
        "local wide = gfx.measure('MMMMMMMMMM', 'ui') "
        "local r = (%s) and fs.send('/app/wm', { type = 'theme' }) or {} "
        "local held = {} "
        "for _, role in ipairs { 'ui', 'title', 'text', 'mono' } do "
        "local h = (r.held or {})[role] "
        "held[#held + 1] = role .. '=' .. "
        "(h and (h.font .. '/' .. h.px) or 'none') end "
        "print('look' .. ': ' .. f.ui.font .. ' ' .. f.ui.px .. ' ' .. "
        "f.title.font .. ' ' .. f.title.px .. ' ' .. f.mono.font .. ' ' .. "
        "f.mono.px .. ' ' .. table.concat(ok) .. ' ' .. thin .. ' ' .. wide "
        ".. ' ' .. table.concat(held, ' ') .. ' why=' .. tostring(r.held_why)) "
        "print('look' .. '-said')"
    ) % ("true" if ask_wm else "false")

    mark = len(guest.seen)
    guest.type("fs.write('/ramfs/look.lua', %r)" % program)

    # **Under `wm`, because half of what is asked is a question only the
    # window manager can answer** - and it has to be a desktop with nothing
    # saved, which is this phase and no other: the pin below gives every
    # later desktop a font table to load, and a window manager that loads
    # what it was given is exactly the bug this cannot see.
    guest.type("wm /ramfs/look.lua" if ask_wm else "./ramfs/look.lua")
    # Waited for the marker *after* the line, not for the line itself: the
    # line arrives from QEMU in pieces, and reading it the instant its first
    # characters land gives "ibmplexsans 14 ibm". `run_media.py` has the same
    # lesson written down; this is the second time it has been learned.
    guest.wait_for("look-said", "report the default faces")

    line = guest.seen[mark:].split("look: ", 1)[1].split("\n")[0].strip()
    parts = line.split()

    if len(parts) < 14:
        raise Failure("the default faces came back as %r" % line)

    ui, ui_px, title, title_px, mono, mono_px = parts[0:6]
    loaded, thin, wide = parts[6], int(parts[7]), int(parts[8])

    # 18 since 24 September: Diego, with a menu open, "push the regular
    # font up a point or two as toy see items in menus look small compared
    # to the height of the selection" (`roadmap.md` 5zz). It was 16 from 22
    # September, "the default font size for regular and widgets is 16".
    if ui != "ibmplexsans" or ui_px != "18":
        raise Failure("the widgets' face is %s %s, not ibmplexsans 18" % (ui, ui_px))

    checks += 1

    # The title at the mockups' 14 semibold, which is 18 here (`roadmap.md`
    # 5zp). It was Plex Sans Condensed at 14 until 0.10.146 - 10.8 as CSS,
    # below everything around it. Diego: "the window tab font is really
    # small if you compare it with the mockups".
    if title != "ibmplexsans-semibold" or title_px != "18":
        raise Failure("a title's face is %s %s, not ibmplexsans-semibold 18"
                      % (title, title_px))

    checks += 1

    if mono != "ibmplexmono" or mono_px != "16":
        raise Failure("the terminal's face is %s %s, not ibmplexmono 16"
                      % (mono, mono_px))

    checks += 1

    if loaded != "yyyy":
        raise Failure("a default face would not load: %s for ui/title/text/mono"
                      % loaded)

    checks += 1

    if wide <= thin:
        raise Failure("ten M's measure %d and ten i's %d, so the widgets' face "
                      "is not proportional - it fell back to the bitmap"
                      % (wide, thin))

    checks += 1

    # What the desktop itself holds, which is what its windows are drawn in.
    if not ask_wm:
        return checks

    held = dict(part.split("=", 1) for part in parts[9:13])
    why = parts[13].split("=", 1)[1]

    for role, want in (("ui", "ibmplexsans/18"),
                       ("title", "ibmplexsans-semibold/18"),
                       ("text", "ibmplexsans/18"),
                       ("mono", "ibmplexmono/16")):
        if held.get(role) != want:
            raise Failure(
                "the window manager draws %s in %s, not %s%s - so every "
                "window it draws for is measured in one face and painted in "
                "another"
                % (role, held.get(role), want,
                   "" if why == "nil" else " (" + why + ")"))

        checks += 1

    return checks


def check_text_size(guest):
    """A window with a heading larger than the desktop's own text.

    `ui.md` said outright that a window drawing through commands "cannot have
    a heading at 28 pixels and a paragraph at 16 on the same screen", because
    a text command carried a role - one of four sizes chosen in Appearance -
    and nothing else. Music's design wants a large title, and Diego chose a
    size the kit carries over a window drawing its own pixels, so that every
    application restyled after Music gets it for nothing.

    **A TrueType face first, and that is not incidental.** The default face
    for every role is the bitmap, which exists at one size; asking it for 28
    pixels can only fall back, and both lines would come out identical - a
    check that measures nothing and passes. So this chooses a scalable face,
    the way the log view phase does, and only then asks for the size.

    What is measured is the consequence on the screen rather than a reply: the
    rows of ink each line puts down. The line asked for at 28 has to be taller
    than the line drawn at the role's own size. A compositor that ignored the
    size, or resolved it against a face index that means nothing in its own
    process - which is what sending a face number rather than a size would
    have done - draws both the same and fails here.
    """
    guest.type('fs.write("/home/.appearance", { fonts = { '
               'ui = { font = "ibmplexmono", px = 20 }, '
               'mono = { font = "ibmplexmono", px = 20 } } }) '
               'print("sized-font" .. "-ready")')
    guest.wait_for("sized-font-ready", "chose a scalable face for the widgets")

    program = (
        "local ui = use('/lib/ui.lua') "
        "local w = ui.window{ title = 'Sized', w = 320, h = 150, "
        "x = 760, y = 130 } "
        "if not w then return end "
        "local v = ui.view{ x = 0, y = 0, w = 320, h = 150 } "
        "function v:draw(gc) "
        "gc:fill(0, 0, 320, 150, 0xff000000) "
        "gc:text(12, 24, 'HHHH', 0xffffffff, nil, 'ui') "
        "gc:text(12, 96, 'HHHH', 0xffffffff, nil, 'ui', 28) "
        "end "
        "w:add(v) w:run()"
    )

    guest.type("fs.write('/ramfs/sized.lua', %r)" % program)
    guest.type("wm sized,/ramfs/sized.lua")

    mark = len(guest.seen)
    placed, deadline = None, time.monotonic() + 40

    while placed is None and time.monotonic() < deadline:
        found = re.search(r"wm: window Sized at (\d+),(\d+) (\d+)x(\d+)",
                          guest.seen)
        if found:
            placed = tuple(int(v) for v in found.groups())
        time.sleep(0.3)

    if placed is None:
        raise Failure("the window that draws two sizes never opened:\n"
                      + guest.seen[mark:][-900:])

    wx, wy, ww, wh = placed
    time.sleep(2.0)
    width, height, px = parse_ppm(guest.screendump())

    def ink_runs(top, bottom):
        """Each block of consecutive rows that has ink in it: (first, height).

        **Blocks rather than a count per band**, and the difference is what
        made the first version of this check worthless. Counting inked rows
        in a band above and a band below passed with the size resolution
        removed: the window has a few rows of chrome near its bottom edge,
        and three of those plus a small line out-counted the small line
        alone. A block is the line itself, so its height is the letters'
        height and nothing else can pad it.

        Specks of one or two rows are left out for the same reason.
        """
        runs, start = [], None

        for dy in range(top, bottom):
            y = wy + dy
            lit = 0

            if 0 <= y < height:
                for x in range(wx, min(wx + ww, width)):
                    o = (y * width + x) * 3

                    if px[o] > 200 and px[o + 1] > 200 and px[o + 2] > 200:
                        lit += 1

                        if lit >= 2:
                            break

            if lit >= 2:
                if start is None:
                    start = dy
            elif start is not None:
                runs.append((start, dy - start))
                start = None

        if start is not None:
            runs.append((start, bottom - start))

        return [r for r in runs if r[1] >= 3]

    runs = ink_runs(0, min(wh, 130))

    if len(runs) < 2:
        raise Failure(
            f"the window drew {len(runs)} block(s) of text where two were "
            f"asked for, at {runs}. One of the lines is not on screen."
        )

    small, large = runs[0][1], runs[1][1]

    if large <= small + 2:
        raise Failure(
            f"the line asked for at 28 pixels is {large} rows tall and the "
            f"line at the role's size is {small}, so the size the command "
            f"carried changed nothing that shows. The blocks found: {runs}."
        )

    # **The screen back first, and the order is the whole of it.** The shell
    # is blocked running this window manager, so anything typed now is queued
    # and runs only once it exits - which is how the restore below waited
    # thirty seconds for a marker that could not be printed yet.
    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()
    time.sleep(2.0)

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure(
            "Control-C did not get the screen back after the text size "
            "phase.\n--- what the guest said ---\n"
            + (guest.seen[mark:][-2000:] or "(nothing at all)"))

    # **And the face back to the desktop's own**, which matters more than it
    # looks. The Deskbar sizes its Kosmos button with `gfx.measure("Kosmos")`
    # in whatever face is loaded, while the focus phase below computes that
    # width as `len("Kosmos") * GLYPH_W` - the bitmap's 8-pixel cell. Leave a
    # 20-pixel TrueType face behind and every button sits further right than
    # that phase assumes, so it samples a bevel instead of a button face and
    # calls the focus colours wrong. Found by breaking exactly that.
    # **The palette goes back with it.** Writing an empty table put the face
    # right and took the palette away, and the setup above chose `dark` on
    # purpose: every colour this file looks for is a field of that palette.
    # The focus phase then read its buttons three greens off - the right
    # pixels, the wrong palette - which is a subtler failure than the wrong
    # pixels and took one more run to see.
    guest.type(appearance() + ' '
               'print("sized-font" .. "-back")')
    guest.wait_for("sized-font-back",
                   "put the desktop's own face and palette back")

    return 2


def check_window_resize(guest):
    """A window asking for its own size, and getting it on the screen.

    The window manager has served `resize` since 2 September and refuses only
    a window that draws its own pixels. **Nothing in the userland ever asked**
    - the only matches were the kit's own layout and the event it receives -
    so Music's mini player, which is that window with its list folded away,
    had no way to fold.

    Measured on the screen rather than from the reply, and the difference
    matters: the compositor throws the old surface away and allocates a new
    one, so a window whose *numbers* changed and whose surface did not would
    answer correctly and still be the old size on screen. The window draws one
    block of colour over its whole area; the block has to shrink with it.

    The reply is checked too, because the second half of this fix is that the
    size comes back in it: the `resize` event lays the tree out again but used
    to leave the window's own width and height as they were.
    """
    BLOCK = (0x30, 0x80, 0xd0)
    program = (
        "local ui = use('/lib/ui.lua') "
        "local w = ui.window{ title = 'Folds', w = 300, h = 200, "
        "x = 700, y = 200 } "
        "if not w then return end "
        "local v = ui.view{ x = 0, y = 0, w = 300, h = 200 } "
        "function v:draw(gc) gc:fill(0, 0, w.w or 300, w.h or 200, 0xff3080d0) end "
        "w:add(v) "
        "function v:mouse(action) "
        "if action == 'release' then "
        "local ok, nw, nh = w:resize(160, 90) "
        "print('folds: ' .. tostring(ok) .. ' ' .. tostring(nw) .. 'x' .. tostring(nh) "
        ".. ' kit ' .. tostring(w.w) .. 'x' .. tostring(w.h)) "
        "end return true end "
        "w:run()"
    )

    guest.type("fs.write('/ramfs/folds.lua', %r)" % program)
    guest.type("wm folds,/ramfs/folds.lua")

    mark = len(guest.seen)
    placed, deadline = None, time.monotonic() + 40

    while placed is None and time.monotonic() < deadline:
        found = re.search(r"wm: window Folds at (\d+),(\d+) (\d+)x(\d+)",
                          guest.seen)
        if found:
            placed = tuple(int(v) for v in found.groups())
        time.sleep(0.3)

    if placed is None:
        raise Failure("the window that resizes itself never opened:\n"
                      + guest.seen[mark:][-900:])

    wx, wy, ww, wh = placed
    time.sleep(1.5)

    def block(px, width, height):
        """How many rows and columns of the window's colour are on screen."""
        rows, cols = 0, 0

        for y in range(wy, min(wy + wh + 40, height)):
            o = (y * width + wx + 4) * 3

            if (px[o], px[o + 1], px[o + 2]) == BLOCK:
                rows += 1

        for x in range(wx, min(wx + ww + 40, width)):
            o = ((wy + 4) * width + x) * 3

            if (px[o], px[o + 1], px[o + 2]) == BLOCK:
                cols += 1

        return rows, cols

    width, height, px = parse_ppm(guest.screendump())
    was = block(px, width, height)

    if was[0] < 150 or was[1] < 250:
        raise Failure(
            f"the window did not draw its block at the size it opened with: "
            f"{was[1]} columns and {was[0]} rows of colour, where 300x200 was "
            "asked for.")

    # The click is what asks: a window cannot resize itself before it is up
    # without racing its own first paint.
    guest.mouse_to(*_to_tablet(wx + 40, wy + 40, width, height))
    time.sleep(0.3)
    guest.mouse_button(True)
    time.sleep(0.2)
    guest.mouse_button(False)
    time.sleep(2.5)

    said = re.search(r"folds: (\w+) (\d+)x(\d+) kit (\d+)x(\d+)",
                     guest.seen[mark:])

    if not said:
        raise Failure("the window never said what its resize answered:\n"
                      + guest.seen[mark:][-900:])

    if said.group(1) != "true" or said.group(2) != "160" or said.group(3) != "90":
        raise Failure(
            f"asking for 160x90 answered {said.group(0)!r}, so the window "
            "manager refused a window it can resize.")

    if said.group(4) != "160" or said.group(5) != "90":
        raise Failure(
            f"the window manager resized to {said.group(2)}x{said.group(3)} "
            f"and the kit still thinks it is {said.group(4)}x{said.group(5)}: "
            "the size has to come back in the reply, because the event that "
            "follows does not carry it into those fields.")

    width, height, px = parse_ppm(guest.screendump())
    now = block(px, width, height)

    if now[0] >= was[0] or now[1] >= was[1]:
        raise Failure(
            f"the window's colour still covers {now[1]} columns and {now[0]} "
            f"rows, where it covered {was[1]} and {was[0]} before: the reply "
            "said 160x90 but the surface on screen did not change.")

    #
    # **And an application that lays itself out again fills the new size.**
    #
    # Diego dragged Music wider on 16 September and the design stayed as it
    # was, with the rest of the window `0xff202020` - the grey a freshly
    # allocated surface is filled with. The window above shrinks; this one
    # grows, which is the direction that leaves room to not draw, and the
    # check is that none of the new room is that grey.
    #
    #
    # **A fresh mark before waiting**, or this looks at output from earlier in
    # the phase, decides the prompt is already back, and types the next
    # command into a window manager that still has the screen - which is how
    # a stray `q` ended up at a prompt and this phase failed on its own
    # interrupt rather than on anything it was testing.
    #
    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()
    time.sleep(2.0)

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure(
            "Control-C did not get the screen back before the grown window.\n"
            + (guest.seen[mark:][-1500:] or "(nothing at all)"))

    grower = (
        "local ui = use('/lib/ui.lua') "
        "local w = ui.window{ title = 'Grew', w = 200, h = 140, "
        "x = 700, y = 200, background = false } "
        "if not w then return end "
        "local v = ui.view{ x = 0, y = 0, w = 200, h = 140 } "
        "function v:draw(gc) gc:fill(0, 0, self.w, self.h, 0xff20a060) end "
        "w:add(v) "
        "function w:on_resize(nw, nh) v.w, v.h = nw, nh end "
        "function v:mouse(action) "
        "if action == 'release' then w:resize(340, 260) end return true end "
        "w:run()"
    )

    guest.type("fs.write('/ramfs/grew.lua', %r)" % grower)
    guest.type("wm grew,/ramfs/grew.lua")

    mark = len(guest.seen)
    grown, deadline = None, time.monotonic() + 40

    while grown is None and time.monotonic() < deadline:
        found = re.search(r"wm: window Grew at (\d+),(\d+) (\d+)x(\d+)",
                          guest.seen)
        if found:
            grown = tuple(int(v) for v in found.groups())
        time.sleep(0.3)

    if grown is None:
        raise Failure("the window that grows itself never opened:\n"
                      + guest.seen[mark:][-800:])

    gx, gy = grown[0], grown[1]
    width, height, px = parse_ppm(guest.screendump())

    guest.mouse_to(*_to_tablet(gx + 40, gy + 40, width, height))
    time.sleep(0.3)
    guest.mouse_button(True)
    time.sleep(0.2)
    guest.mouse_button(False)
    time.sleep(2.5)

    width, height, px = parse_ppm(guest.screendump())
    undrawn = 0

    for dy in range(4, 256, 4):
        for dx in range(4, 336, 4):
            o = ((gy + dy) * width + gx + dx) * 3

            if (px[o], px[o + 1], px[o + 2]) == (0x20, 0x20, 0x20):
                undrawn += 1

    if undrawn:
        raise Failure(
            f"{undrawn} places in the grown window are the grey a new surface "
            "is filled with, so the application laid out for its old size and "
            "the new room was never drawn.")

    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()
    time.sleep(2.0)

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure(
            "Control-C did not get the screen back after growing.\n"
            + (guest.seen[mark:][-2000:] or "(nothing at all)"))

    return 4


def check_triangle(guest):
    """A triangle drawn by a window that does not own its pixels.

    An application drawing through commands had rectangles, text and pictures.
    The triangle primitive has been in `gfx.c` since it was written and was
    reachable only by a window that draws its own pixels, so Music's play
    arrow would have been a staircase of thin fills - visibly stepped at the
    18 pixels the design draws it at. Diego chose the command over generating
    seven pictures, because the restyle after Music wants the same shape for
    menus, sliders and disclosure arrows.

    **A triangle is told from a box by what is missing.** The window draws one
    filling the lower-left half of a square of known colour. A point well
    inside the shape is that colour; the opposite corner, outside the
    hypotenuse, is not. A compositor that drew the bounding box instead - or a
    kit that quietly sent a fill - paints both, and fails here.
    """
    INK = (0xf0, 0x90, 0x20)
    program = (
        "local ui = use('/lib/ui.lua') "
        "local w = ui.window{ title = 'Tri', w = 200, h = 160, "
        "x = 820, y = 240 } "
        "if not w then return end "
        "local v = ui.view{ x = 0, y = 0, w = 200, h = 160 } "
        "function v:draw(gc) "
        "gc:fill(0, 0, 200, 160, 0xff101010) "
        "gc:triangle(20, 20, 20, 120, 120, 120, 0xfff09020) "
        "end "
        "w:add(v) w:run()"
    )

    guest.type("fs.write('/ramfs/tri.lua', %r)" % program)
    guest.type("wm tri,/ramfs/tri.lua")

    mark = len(guest.seen)
    placed, deadline = None, time.monotonic() + 40

    while placed is None and time.monotonic() < deadline:
        found = re.search(r"wm: window Tri at (\d+),(\d+) (\d+)x(\d+)",
                          guest.seen)
        if found:
            placed = tuple(int(v) for v in found.groups())
        time.sleep(0.3)

    if placed is None:
        raise Failure("the window that draws a triangle never opened:\n"
                      + guest.seen[mark:][-900:])

    wx, wy = placed[0], placed[1]
    time.sleep(2.0)
    width, height, px = parse_ppm(guest.screendump())

    def at(dx, dy):
        o = ((wy + dy) * width + wx + dx) * 3
        return (px[o], px[o + 1], px[o + 2])

    inside = at(35, 105)          # low and left, well inside the shape
    outside = at(110, 30)         # the corner the hypotenuse cuts off

    if inside != INK:
        raise Failure(
            f"a point inside the triangle is {inside}, not {INK}: the shape "
            "was not drawn at all.")

    if outside == INK:
        raise Failure(
            f"the corner beyond the triangle's long edge is {outside} as well, "
            "so what was drawn is the bounding box and not a triangle.")

    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()
    time.sleep(2.0)

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure(
            "Control-C did not get the screen back after the triangle phase.\n"
            + (guest.seen[mark:][-2000:] or "(nothing at all)"))

    return 2


def check_scripting(guest):
    """An application scripted by another, with no scripting code in either.

    roadmap.md M7's third definition of done. `gallery.lua` contains not one
    line about properties: it calls `ui.window`, and `ui.window` registers
    the window with /app and answers for its properties. `setprop` is a
    general four-line program that writes a path. Neither knows about the
    other.

    `wm gallery,setprop:/app/gallery/title=renamed by another process`
    starts both, each in its own address space, each handed /app/wm and
    /app and nothing else.

    Foreground, and that is not incidental. A window manager reads the
    keyboard, and so does the shell's line editor, so running one detached
    means two processes draining one input queue and whichever asks first
    wins. Doing this from the prompt worked and then did not, depending on
    timing. The real answer is the Terminal app, where the shell is a window
    and there is one reader; until then, the honest arrangement is that the
    window manager has the keyboard while it runs.

    Checked by consequence rather than by reply: a property store that
    accepted the write and told nobody would pass a read-back and fail this.

    **The consequence used to be the tab getting wider**, because a tab was
    exactly as wide as its title. Kosmos departed from that - the decoration
    is now one colour across the whole frame, see `ui.md` - so the width
    says nothing any more and the check would pass whatever the title said.

    What it measures instead is the *title text itself*: the pixels along
    the tab where the title is drawn, before and after. A rename changes
    them and nothing else on that row does. That is a weaker signal than a
    width in one way - it cannot say the new title is longer - and a
    stronger one in another, since a store that wrote the right length and
    the wrong text would have passed the old check.
    """

    def title_row(width, height, px):
        """The pixels of the tab's title row, as bytes.

        One row through the middle of the tab, from past the close box to
        the width the title could reach. Compared for difference, not
        matched against an expected picture: what has to be true is that
        renaming changed what is drawn there.
        """
        top = tab_top(width, height, px)

        if top is None:
            return None

        y = top + 10                       # inside the tab, on the glyphs
        row = []

        for x in range(0, min(width, 700)):
            at = (y * width + x) * 3
            row.append(px[at:at + 3])

        return b"".join(bytes(v) for v in row)
    guest.type("wm gallery")
    started(guest)

    width, height, px = parse_ppm(guest.screendump())
    before = title_row(width, height, px)

    if before is None:
        raise Failure(
            "no window tab on screen after `wm gallery`, so there is "
            "nothing here to rename."
        )

    # Back to the shell, then start both together.
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()
    time.sleep(2)

    guest.type("wm gallery,setprop:/app/gallery/title=renamed by another one")
    started(guest)

    width, height, px = parse_ppm(guest.screendump())
    after = title_row(width, height, px)

    if after is None:
        raise Failure("no tab on screen at all after the second start.")

    if after == before:
        raise Failure(
            "the title drawn on the tab is identical before and after "
            "something renamed the window. Either the registry did not hand "
            "over the window's endpoint, or the property was stored "
            "somewhere that is not the window."
        )

    # And hand the screen back for the phase after this one.
    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back after scripting.")

    return 2


# A replicant's text, theme.good.
GREEN = (0x3f, 0xb9, 0x50)


def green_bands(width, height, px):
    """The distinct horizontal bands containing replicant-green pixels."""
    bands = []

    for y in range(0, height, 2):
        found = False

        for x in range(0, width, 2):
            at = (y * width + x) * 3

            if (px[at], px[at + 1], px[at + 2]) == GREEN:
                found = True
                break

        if found:
            if bands and y - bands[-1][1] <= 6:
                bands[-1][1] = y
            else:
                bands.append([y, y])

    return bands


def check_replicants(guest):
    """A view moved between processes, still running, with what it declared.

    roadmap.md M7's second definition of done, minus the dragging - there is
    no pointer yet, so `clock` offers the replicant through /ramfs and
    `adopt` picks it up. The mechanism is the whole of it either way; the
    pointer is the part that is missing.

    `wm clock,adopt` starts two applications in two address spaces. The
    first publishes a description - source, state, and a `needs` list - and
    shows the clock. The second has never heard of clocks: it reads the
    description and instantiates it. Both must then be ticking, with
    different state, which is what makes it a replicant rather than a
    picture of one.

    Two things are checked, both by what reaches the screen:

      * two green clocks, in two different windows, and both changing.
        `adopt` re-runs the same source with its own state, so the two
        read differently and neither is a copy of the other's pixels.
      * `adopt` also prints what the replicant's restricted namespace
        actually answers - it tries /dev/cpu, which was declared, and /ramfs,
        which was not - so the sandbox line on screen is a measurement and
        not a claim. That line is green only when the refusal happened.
    """
    guest.type("wm clock,adopt")
    started(guest)

    width, height, before = parse_ppm(guest.screendump())
    bands = green_bands(width, height, before)

    if len(bands) < 3:
        raise Failure(
            f"expected three bands of replicant green - a clock in each of "
            f"two windows and the sandbox result - and found {len(bands)}. "
            "Either the replicant did not load in one of them, or the "
            "restricted namespace let /ramfs through, which turns that line "
            "red."
        )

    time.sleep(3)
    _, _, after = parse_ppm(guest.screendump())

    ticking = 0

    for top, bottom in bands:
        changed = False

        for y in range(top, min(bottom + 2, height)):
            row = y * width * 3

            if before[row:row + width * 3] != after[row:row + width * 3]:
                changed = True
                break

        if changed:
            ticking += 1

    if ticking < 2:
        raise Failure(
            f"only {ticking} of the replicants changed in three seconds. A "
            "replicant that was adopted but is not running is a picture of "
            "a clock."
        )

    # Hand the screen back.
    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back after the clocks.")

    return 3


def _to_tablet(x, y, width, height):
    """Screen pixels to the tablet's own units.

    The device reports 0..32767 on both axes whatever the display is, which
    is why the range travels with the position all the way from the HAL: the
    only place that knows how big the screen is, is the place doing this.
    """
    return x * 32767 // (width - 1), y * 32767 // (height - 1)


def _strip(px, width, x0, y0, w, h):
    """A rectangle of the screen, as bytes, for comparing against itself."""
    out = bytearray()

    for y in range(y0, y0 + h):
        at = (y * width + x0) * 3
        out += px[at:at + w * 3]

    return bytes(out)


# The unfocused tab, 0xb8b8b8. With the focused one that is every window.
# An unfocused window's decoration. **Must match `theme.tab_idle` in the
# default palette** - the harness runs in the dark one. When the theme moved
# into a palette table this silently became a different grey, every
# unfocused window stopped being counted, and the Deskbar check failed
# saying nothing had launched. It had launched; it could not be seen.
TAB_IDLE = (0xb8, 0xb8, 0xb8)


def count_windows(width, height, px, from_y=0):
    """How many windows are on screen, counted by their title bars.

    `from_y` skips rows at the top, and it exists for the Deskbar. The
    bar across the top is tab-coloured, the full width of the screen, and
    it **redraws every second** as its clock ticks - so its gradient rows
    drift in and out of the tab-colour test and the whole-screen count
    wobbles between readings. That made the Deskbar phase flaky rather
    than wrong: it passed or failed depending on which frame it caught.

    The bar is chrome rather than a window, so the answer is to stop
    counting it.

    **Not by rows containing tab colour**, which is what this did until the
    decoration became one colour across the whole frame. A window's *border*
    is now that colour too, for the window's whole height, so any two windows
    that overlap vertically produce one unbroken band of rows and the count
    said 1 when there were plainly 2. That is what made the Deskbar check
    fail while the click it was testing worked perfectly.

    So: a title bar is a *long horizontal run* of tab colour and a border is
    a two-pixel one. Runs shorter than MIN are ignored, and a run that
    overlaps one on the row above is the same window continuing rather than
    a new one.
    """
    MIN = 40                    # wider than a border, narrower than any tab

    def runs_in(y):
        out = []
        run_from = None

        for x in range(width):
            at = (y * width + x) * 3
            hit = is_tab(px, at) or is_tab(px, at, TAB_IDLE)

            if hit and run_from is None:
                run_from = x
            elif not hit and run_from is not None:
                if x - run_from >= MIN:
                    out.append((run_from, x))
                run_from = None

        if run_from is not None and width - run_from >= MIN:
            out.append((run_from, width))

        return out

    # A title bar is not a solid block: the close box and the title's own
    # glyphs are drawn *on* it in another colour, so on those rows the run is
    # chopped into pieces too short to count. Requiring a run on the
    # immediately preceding row therefore made one window look like several
    # starting over and over.
    #
    # So a run continues a window it overlaps that was seen recently, where
    # recently is a little more than a tab is tall. Below the tab there is
    # nothing but a two-pixel border, which never reaches MIN, so a window's
    # entry expires and the next window down is counted as its own.
    MEMORY = 30

    # And a title bar is *tall*. The bottom border of a window is a
    # full-width horizontal run of the same colour, so counting every long
    # run counted each window twice - once for its tab and once for the
    # line underneath it. A tab is TAB_H (20px) deep and a border is two, so
    # requiring a cluster to survive a few sampled rows tells them apart.
    DEEP = 3

    clusters = []                       # [x0, x1, last_y, rows]

    for y in range(from_y, height, 2):
        for a0, a1 in runs_in(y):
            for c in clusters:
                if a0 < c[1] and c[0] < a1 and y - c[2] <= MEMORY:
                    c[0], c[1] = min(a0, c[0]), max(a1, c[1])
                    c[2], c[3] = y, c[3] + 1
                    break
            else:
                clusters.append([a0, a1, y, 1])

    windows = sum(1 for c in clusters if c[3] >= DEEP)

    return windows


def check_idle(guest):
    """An idle desktop is idle.

    Starts the monitor under the window manager, leaves it alone, and looks
    at the processor meter: `sysmon` fills the bar red above eighty per cent
    and green below, so a busy machine has a red run three hundred pixels
    long and an idle one has no red at all.

    Measured as the longest run of red rather than by counting coloured
    pixels. Counting was the first version and it was wrong in the direction
    that matters: at one per cent the green fill is three pixels wide, a scan
    that steps by two can miss it entirely, and the phase then fails on a
    machine that is behaving perfectly.

    **The run tolerates gaps of two pixels, because the bar has them now.**
    A processor meter is drawn in segments - six lit, two dark, BeOS Pulse's
    look, `/lib/pulse.lua` - so the longest unbroken run of red in a meter
    that is completely full is six. Left alone, this check would have gone
    on passing and stopped meaning anything, which is worse than failing:
    the threshold below is forty, and six can never reach it. Bridging the
    dark pixel between segments keeps the question the same one it was -
    *is there a long red bar on this screen* - against a bar that is no
    longer continuous.

    This is the regression check for the thing that made it true, which is
    that nothing polls any more. The desktop used to ask the console for
    keys, get none, yield and ask again; every window did the same to the
    desktop; and a machine with four windows open had five threads that were
    permanently runnable. The meter read ninety-six per cent on an empty
    desktop and was telling the truth.
    """
    guest.type("wm sysmon")
    started(guest)

    width, height, px = parse_ppm(guest.screendump())

    if count_windows(width, height, px) < 1:
        raise Failure(
            "the monitor did not open a window, so there is no meter to "
            "read here."
        )

    longest = 0

    # Two, which is the dark gap between two lit segments. Three would
    # start bridging things that are not one bar.
    GAP = 2

    for y in range(height):
        run = 0
        gap = 0

        for x in range(width):
            at = (y * width + x) * 3

            if (px[at], px[at + 1], px[at + 2]) == (0xda, 0x36, 0x33):
                run += 1 + gap          # the gap was inside the bar
                gap = 0

                if run > longest:
                    longest = run
            else:
                gap += 1

                if gap > GAP:
                    run = 0
                    gap = 0

    # The bar is a little over three hundred pixels wide, so a busy meter is
    # a run of that order. Forty is far above any red incidental to the
    # window and far below a meter that is filling up.
    if longest > 40:
        raise Failure(
            f"the processor meter has a red run {longest} pixels long on an "
            "idle desktop. Something is polling: the meter is not wrong, a "
            "thread that never blocks keeps the core busy and the idle "
            "thread never gets a turn."
        )

    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back from the desktop.")

    return 1


def check_direct(guest):
    """An application drawing its own pixels, into memory both sides map.

    `gfx.md` 19.4, working. `plasma` opens a window with `direct = true`,
    which allocates a region big enough for two copies of the surface, sends
    the *capability* to it in the open message, and from then on writes
    pixels rather than sending commands. `commit` swaps which buffer is
    shown and says what changed.

    Two things are checked, and the second is the point:

      * the window has colour in it, so the compositor really is reading
        the application's memory - nothing was copied and no drawing
        command was sent;
      * it *changes* between two pictures, so the buffers are being swapped
        rather than one being shown for ever.

    A single frame would pass the first check on its own, which is why the
    second exists: the failure this guards against is a commit that damages
    the right rectangle and never actually swaps.
    """
    guest.type("wm plasma")
    started(guest)

    width, height, before = parse_ppm(guest.screendump())

    # The window is opened at 140,120 and is 420x300.
    x0, y0, w, h = 150, 140, 400, 260

    def sample(px):
        out = []

        for y in range(y0, y0 + h, 8):
            for x in range(x0, x0 + w, 8):
                at = (y * width + x) * 3
                out.append(px[at:at + 3])

        return out

    first = sample(before)
    distinct = len(set(first))

    if distinct < 8:
        raise Failure(
            f"the plasma window has only {distinct} distinct colours in it, "
            "so the compositor is not reading the application's memory. "
            "Either the shared region never reached it - the capability "
            "travels in the open message and is easy to drop - or the "
            "commit is not swapping."
        )

    time.sleep(2)
    _, _, after = parse_ppm(guest.screendump())

    if sample(after) == first:
        raise Failure(
            "the plasma window is not changing. The application is drawing "
            "and committing, so the buffers are not being swapped: the "
            "compositor is showing one of the two for ever."
        )

    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back from plasma.")

    return 2


def check_3d(guest):
    """A solid object, rendered in software, into a shared surface.

    The check that says the renderer works and not merely that it runs.
    Three things, and the middle one is the one that would be missed by
    looking at a screenshot and nodding:

      * the window holds face colours from the cube's palette, so triangles
        were actually filled;
      * **no more than three of the six faces appear at once**, because a
        cube cannot show a fourth. Back-face culling is one comparison - the
        sign of a cross product - and getting it backwards draws the far
        faces instead of the near ones, which still looks like a cube, still
        rotates, and is wrong. The face count is what tells the two apart;
      * it changes between two pictures, so it is rotating rather than
        showing one pose.

    The colours come from `g3d.cube` in user/lib/g3d.lua. If that palette
    changes this has to change with it, which is the price of checking
    something specific instead of "there are several colours".
    """
    FACES = {
        (0x3a, 0x5f, 0x8f), (0x5a, 0x7f, 0xbf), (0x2a, 0x4f, 0x7f),
        (0x6a, 0x8f, 0xcf), (0x4a, 0x6f, 0x9f), (0x7a, 0x9f, 0xdf),
    }

    guest.type("wm cube3d")
    started(guest)

    # The window is opened at 180,100 and is 400x320.
    x0, y0, x1, y1 = 190, 140, 570, 400

    def faces_on_screen():
        width, height, px = parse_ppm(guest.screendump())
        seen = set()

        for y in range(y0, y1, 3):
            for x in range(x0, x1, 3):
                at = (y * width + x) * 3
                seen.add(tuple(px[at:at + 3]))

        return seen & FACES, px, width

    first, before, width = faces_on_screen()

    if not first:
        raise Failure(
            "the cube window has none of the cube's face colours in it, so "
            "no triangle was filled. Either the shared surface never "
            "arrived or surface:triangle drew nothing."
        )

    if len(first) > 3:
        raise Failure(
            f"{len(first)} of the cube's six faces are visible at once, and "
            "a cube can show three. The back-face test has the wrong sign, "
            "so the far faces are being drawn instead of the near ones - "
            "which still looks like a rotating cube from across the room."
        )

    time.sleep(2)
    _, after, _ = faces_on_screen()

    if after == before:
        raise Failure("the cube is not rotating; every frame is the same.")

    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back from cube3d.")

    return 3


# `theme.console`, 0x0b0b0b. While the Terminal is the only window drawn in
# it, the bounding box of that colour is the terminal's character grid - which
# is the one thing that has to change size when the window does.
#
# **Only while.** Log View draws on it too since 0.10.47, and the first gate
# with both on the screen measured a box round the two of them: the Terminal
# grew to full size and the check said it had not. A phase with Log View open
# measures the Terminal from its own corner instead, with `_terminal_grid`.
CONSOLE = (0x0b, 0x0b, 0x0b)


def _terminal_grid(width, height, px, x, y):
    """The Terminal's grid, measured from its own corner. None if it is not there.

    `x, y` is the window as the window manager logged it. The console runs
    from under the header to the window's edges (`docs/apps.html`, 0.10.149;
    it was a framed box 8 in until then), and its first rows and columns
    never hold text - lines start 9 down and 12 in - so the console colour
    runs unbroken from nine in along both to the far edges of the grid.
    Another window's edge stops the walk, so a Log View beside the Terminal,
    or over it, cannot make the grid look bigger than it is.

    **The top is found rather than assumed.** It was `y + 9`, which was the
    grid's first row until the Terminal grew a menu bar (`roadmap.md` 5zc)
    and the grid moved a row down. Looked for in the first sixty rows of the
    window, so this holds whether or not there is a bar.
    """
    x0 = x + 9

    def console(cx, cy):
        at = (cy * width + cx) * 3

        return (px[at], px[at + 1], px[at + 2]) == CONSOLE

    if not (0 <= x0 < width):
        return None

    y0 = None

    for cy in range(max(0, y + 9), min(height, y + 70)):
        if console(x0, cy):
            y0 = cy
            break

    if y0 is None:
        return None

    x1 = x0

    while x1 + 1 < width and console(x1 + 1, y0):
        x1 += 1

    y1 = y0

    while y1 + 1 < height and console(x0, y1 + 1):
        y1 += 1

    return x0, y0, x1, y1


def _console_box(width, height, px):
    """Where the terminal's grid is, and how big. None if there is not one.

    The bounding box of the console colour on the whole screen, so it is the
    Terminal's grid only when nothing else on the screen is that colour.
    """
    x0, y0, x1, y1 = width, height, -1, -1

    for y in range(height):
        row = y * width

        for x in range(width):
            at = (row + x) * 3

            if (px[at], px[at + 1], px[at + 2]) == CONSOLE:
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y

    if x1 < 0:
        return None

    return x0, y0, x1, y1


def check_budget(guest):
    """The window manager can decode a full-screen picture and maximise a
    window at the same time, at 1920x1080.

    On the ThinkPad it could not. Its log said `no room for a 1916x1016
    surface: the kernel refused 1905 pages` on every step of a Terminal being
    dragged to full size, and a JPEG wallpaper would not load. The window
    manager was allowed 48 MB of mappings like every other process - a number
    chosen as "four full screens at 1080p" - and it holds more than that: its
    backbuffer, the desktop's backdrop, every window, a second surface for a
    window while it resizes, and a picture's decode buffer beside the surface
    it lands in.

    **Checked as a property rather than a number.** The allowance is now
    derived from the framebuffer the process holds, so the test asserts what a
    person needs - the Terminal grows and nothing is refused - and does not
    care how many pages that took. It fails on the flat budget: dragging a
    Terminal to full size beside a decoded `test-screen.jpg` was refused three
    times and the grid stayed put, and with the derived budget it filled the
    screen with nothing refused.

    The picture is carried in the image because this harness boots without a
    disk; `assets/images/README.md` has why it is flat colour. The Terminal is
    raised before its grip is touched, because the windows the Deskbar starts
    at login cascade over its bottom-right corner and a press there lands on
    whichever is on top - which is what made the first attempt at this test
    pass without testing anything.

    **And the second attempt passed without the fix too**, which is why the
    picture is 4:4:4 and Log View is open. It was a 4:2:0 JPEG and three
    windows, and against the flat budget nothing was refused. The ThinkPad's
    wallpapers are 4:4:4 - a full-resolution plane for each colour, about
    three megabytes more of the decoder's scratch than 4:2:0 - and that
    scratch comes from `malloc`, whose arenas are never given back, so it is
    still counted when the drag begins. That and one more window was the
    margin; with both, the flat budget refuses exactly what the ThinkPad
    refused, `1920x1044 surface (7830 KB)` three times.
    """
    width, height, _ = parse_ppm(guest.screendump())

    if (width, height) != (1920, 1080):
        raise Failure(
            f"the compositor budget is checked at 1920x1080, where the ThinkPad "
            f"refused its surfaces, and this display is {width}x{height}."
        )

    mark = len(guest.seen)
    guest.type("wm desktop,deskbar,terminal,logview,photo:test-screen.jpg")
    started(guest)
    time.sleep(25)
    guest._read_available()

    placed = re.findall(r"wm: window Terminal at (\d+),(\d+) (\d+)x(\d+)",
                        guest.seen[mark:])

    if not placed:
        raise Failure("the Terminal never opened, so there is nothing to resize.")

    x, y, w, h = (int(v) for v in placed[-1])
    width, height, px = parse_ppm(guest.screendump())
    # From the Terminal's corner, because Log View is on this screen too and
    # draws on the same black.
    before = _terminal_grid(width, height, px, x, y)

    if before is None:
        raise Failure("the Terminal opened and its grid is not on the screen.")

    # Raised first, by a press inside its banner.
    guest.mouse_to(*_to_tablet(x + 60, y + 60, width, height))
    time.sleep(0.3)
    guest.mouse_button(True)
    time.sleep(0.2)
    guest.mouse_button(False)
    time.sleep(1.5)

    gx, gy = x + w - 3, y + h - 3
    guest.mouse_to(*_to_tablet(gx, gy, width, height))
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)

    for i in range(1, 13):
        guest.mouse_to(*_to_tablet(gx + (width - 8 - gx) * i // 12,
                                   gy + (height - 8 - gy) * i // 12,
                                   width, height))
        time.sleep(0.15)

    guest.mouse_button(False)
    time.sleep(5)
    guest._read_available()

    refused = [l.strip() for l in guest.seen[mark:].replace("\r", "").split("\n")
               if "no room for a" in l]

    if refused:
        raise Failure(
            f"the window manager was refused {len(refused)} surface(s) at "
            f"1920x1080 with a full-screen picture decoded - first: "
            f"{refused[0]!r}. Its mapping allowance is meant to scale with the "
            "screen it holds; a flat one runs out exactly here."
        )

    width, height, px = parse_ppm(guest.screendump())
    after = _terminal_grid(width, height, px, x, y)

    #
    # **Filling the screen, not growing by a number.**
    #
    # This asked for 800 pixels of growth, and growth is the wrong thing to
    # measure: the grid is read from the window's own corner and the window is
    # placed by the cascade, so how much room it has to grow into is whatever
    # the cascade left. On 16 September it began at x=969 of 1920 - 951 pixels
    # to its right - and the check wanted 1421. The drag worked, the grid went
    # from 621 wide to 936, which is every pixel available, and the phase
    # failed on arithmetic that could not have succeeded.
    #
    # What the phase is for is in its own first line: the Terminal grows and
    # nothing is refused. So it asserts the grid reaches the screen's edges,
    # which is what filling the screen means wherever the window started.
    #
    # The drag ends 8 pixels from each edge and the grid sits inside the
    # frame, so a filled Terminal stops a little short: 1905 of 1920 and 1061
    # of 1080 on that run. 48 is clear of that, and nowhere near loose enough
    # to pass an undragged window - the same run's grid ended at 1590, which
    # is 330 short.
    #
    EDGE = 48

    if (after is None or after[2] < width - EDGE or after[3] < height - EDGE
            or (after[2] - after[0]) <= (before[2] - before[0])):
        raise Failure(
            f"nothing was refused and the Terminal's grid did not fill the "
            f"screen: {before} before the drag, {after} after, on a "
            f"{width}x{height} display. Its right edge should reach "
            f"{width - EDGE} and its bottom {height - EDGE}. The window was "
            f"placed at {x},{y} {w}x{h} - which is the first thing to look "
            f"at, because where the cascade put it decides how much room it "
            f"had to grow into."
        )

    stop = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[stop:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-W Q did not get the screen back.")

    return 2


def check_faces(guest):
    """Every outline face the image carries loads, measures and draws.

    `gfx.fonts()` lists what `fonts_table` embeds, and nothing checked that
    each of them actually works: a file stb_truetype cannot read would load
    as nothing and draw as blank text, and the first anybody would know is a
    choice in Appearance that shows no letters. Space Grotesk, five weights
    added on 18 September at Diego's asking, is the reason this exists.

    **A program, run once per eight faces.** A process holds eight outline
    faces beside its four roles (`FACES_MAX` in `gfx.c`) and never lets one
    go, so asking one process for all sixteen would fail at the ninth for a
    reason that has nothing to do with the fonts. Each run is its own
    process and starts with every slot free. It prints, for each face, the
    width `gfx.measure` gives and how many pixels drawing it lit.
    """
    program = (
        "local names = gfx.fonts() "
        "local from, to = args:match('(%d+)%s+(%d+)') "
        "from, to = tonumber(from), tonumber(to) "
        "print('faces' .. ': ' .. (#names - 1) .. ' outline') "
        "for i = from, math.min(to, #names) do "
        "local n = names[i] "
        "if n ~= 'spleen' then "
        "local id, why = gfx.face(n, 24) "
        "if not id then print('face' .. ': ' .. n .. ' refused ' .. tostring(why)) "
        "else "
        "local w = gfx.measure('Kosmos Space', id) "
        "local s = gfx.surface{ w = 320, h = 48 } "
        "s:fill(0, 0, 320, 48, 0xff000000) "
        "s:text(4, 8, 'Kosmos Space', 0xffffffff, nil, id) "
        "local lit = 0 "
        "for y = 0, 47 do for x = 0, 319 do "
        "if s:get(x, y) ~= 0xff000000 then lit = lit + 1 end end end "
        "print('face' .. ': ' .. n .. ' width ' .. w .. ' lit ' .. lit) "
        "end end end "
        "print('faces' .. ': batch done')"
    )
    guest.type("fs.write('/ramfs/faces.lua', %r)" % program)
    time.sleep(1.0)

    drawn, refused, total = {}, {}, None
    first = 2                       # 1 is spleen, the bitmap

    while total is None or first <= total + 1:
        mark = len(guest.seen)
        guest.type("/ramfs/faces.lua %d %d" % (first, first + 7))

        deadline = time.monotonic() + 40

        while "faces: batch done" not in guest.seen[mark:] \
                and time.monotonic() < deadline:
            time.sleep(0.3)
            guest._read_available()

        said = guest.seen[mark:]

        # **A broken font does not refuse; it kills.** stb_truetype believes
        # the offsets inside the file, so forty kilobytes of noise named as a
        # font sent the program reading an address nothing maps - the
        # control that proved this phase (`testing.md` 18.98). So the death
        # is named, with the last face that drew before it.
        died = re.search(r'process "faces" died: ([^\n]*)', said)

        if died:
            before = re.findall(r"face: (\S+) width", said)
            raise Failure("the faces program died loading a face after %s - "
                          "%s. A face in assets/fonts is not a font stb_truetype "
                          "can read:\n%s" % (before[-1] if before else
                                             "none at all", died.group(1),
                                             said[-800:]))

        if "faces: batch done" not in said:
            raise Failure("the faces program never finished its batch from "
                          "%d:\n%s" % (first, said[-800:]))

        counted = re.search(r"faces: (\d+) outline", said)

        if counted:
            total = int(counted.group(1))

        for name, w, lit in re.findall(r"face: (\S+) width (\d+) lit (\d+)",
                                       said):
            drawn[name] = (int(w), int(lit))

        for name, why in re.findall(r"face: (\S+) refused (.*)", said):
            refused[name] = why.strip()

        first += 8

    if refused:
        raise Failure("these faces would not load: %s"
                      % ", ".join("%s (%s)" % kv for kv in refused.items()))

    blank = [n for n, (w, lit) in drawn.items() if w <= 0 or lit < 50]

    if total is None or len(drawn) != total or blank:
        raise Failure("of %s outline faces, %d drew; these measured or drew "
                      "nothing: %s" % (total, len(drawn), ", ".join(blank)
                                       or "none"))

    grotesk = {"spacegrotesk-light", "spacegrotesk", "spacegrotesk-medium",
               "spacegrotesk-semibold", "spacegrotesk-bold"}

    if not grotesk <= set(drawn):
        raise Failure("Space Grotesk is not all there: %s of its five weights "
                      "drew - %s" % (len(grotesk & set(drawn)),
                                     ", ".join(sorted(grotesk - set(drawn)))))

    return 2


def check_wallpapers(guest):
    """The desktop's wallpapers are in the image, and one reaches the screen.

    Twenty-four photographs from Unsplash, added on 18 September at Diego's
    asking, carried in a `FULL=1` image as `wallpaper/<file>`
    (`assets/wallpapers/README.md`) - the image this harness boots. A program
    at the prompt counts them, decodes the first and says three of its
    pixels, and names it the wallpaper in `/home/.appearance`, which is where
    Appearance saves a choice. Then the desktop starts, and the screen at
    those three points has to be exactly the decoded pixels: the window
    manager draws a picture the size of the screen one to one, with the same
    decoder, so any difference is a fault rather than a rounding. The points
    are clear of the desktop's icons, the stamp, and the middle of the screen,
    where the pointer starts - the first choice put one there, and read the
    pointer's outline.

    It took the userland image past sixteen megabytes, where the heap began,
    and the link said so; the heap and the stack moved up sixteen
    (`kernel/process.h`). So this is also the check that a process still
    starts with its image at thirty-two.
    """
    probe = (
        "local names = {} "
        "for _, n in ipairs(sys.asset()) do "
        "if n:match('^wallpaper/') then names[#names + 1] = n end end "
        "table.sort(names) "
        "print('walls' .. ': ' .. #names .. ' ' .. tostring(names[1])) "
        "if names[1] then "
        "local p = gfx.jpeg(sys.asset(names[1])) "
        "local w, h = p:size() "
        "print('walls' .. ': size ' .. w .. 'x' .. h) "
        "for _, xy in ipairs{ {300, 600}, {1400, 300}, {700, 850} } do "
        "print(string.format('walls' .. ': at %d %d %08x', xy[1], xy[2], "
        "p:get(xy[1], xy[2]))) end "
        + appearance("wallpaper = names[1]") + " " 
        "end "
        "print('walls' .. ': done')"
    )
    guest.type("fs.write('/ramfs/walls.lua', %r)" % probe)
    time.sleep(1.0)
    mark = len(guest.seen)
    guest.type("/ramfs/walls.lua")

    deadline = time.monotonic() + 40

    while "walls: done" not in guest.seen[mark:] and time.monotonic() < deadline:
        time.sleep(0.3)
        guest._read_available()

    said = guest.seen[mark:]
    counted = re.search(r"walls: (\d+) (\S+)", said)

    if not counted or int(counted.group(1)) != 24:
        raise Failure("the image does not carry the desktop's 24 wallpapers:\n"
                      + said[-800:])

    if "walls: size 1920x1080" not in said:
        raise Failure("the first wallpaper did not decode at 1920x1080:\n"
                      + said[-800:])

    points = [(int(x), int(y), int(v, 16)) for x, y, v in
              re.findall(r"walls: at (\d+) (\d+) ([0-9a-f]{8})", said)]

    if len(points) != 3:
        raise Failure("the program did not say three pixels of the picture:\n"
                      + said[-800:])

    def shown(w, h, px):
        for x, y, v in points:
            at = (y * w + x) * 3

            if tuple(px[at:at + 3]) != ((v >> 16) & 255, (v >> 8) & 255,
                                         v & 255):
                return None

        return True

    guest.type("wm desktop")

    try:
        settle(guest, shown,
               "the desktop started and the screen never showed %s at the "
               "three points its decode gave" % counted.group(2), seconds=30)

        # And the window manager says which wallpaper it restored. It used
        # to say nothing either way, so a saved wallpaper that would not
        # load - Diego's plain-colour PNGs, on 21 September - left nothing
        # to search for; the refusal names its reason on the same line.
        restored = "wm: wallpaper " + counted.group(2)
        told = guest.seen[mark:]

        if restored not in told or " would not load: " in told:
            raise Failure("the desktop did not say it restored %s:\n%s"
                          % (counted.group(2), told[-800:]))
    finally:
        back = len(guest.seen)
        guest.proc.stdin.write(STOP_DESKTOP)
        guest.proc.stdin.flush()
        deadline = time.monotonic() + 15

        while time.monotonic() < deadline:
            guest._read_available()

            if PROMPT in guest.seen[back:]:
                break

            time.sleep(0.3)

        guest.type(appearance() + ' '
                   'print("walls" .. "-reset")')
        guest.wait_for("walls-reset", "put the flat desktop back")

    return 4


def check_direct_menu(guest):
    """A window that draws its own pixels has a menu bar, drawn above them.

    Diego's choice on 18 September, for the Super Nintendo's File menu: the
    window manager draws the strip over a direct window, as the kit draws
    `ui.menubar`, and the application's buffer is the area below it; a
    press on a title is a `menubar` event, and the application opens an
    ordinary kit menu (`strips` in `wm.lua`, `window:direct_event`).

    Not the Super Nintendo itself, which needs a ROM and this harness has
    none - game data is never in the repository. A program of its own opens
    a direct window with a File menu, Say hello and Quit, and fills its
    pixels blue. Then:

      the strip is drawn at the top and the blue starts under it, so the
      window grew by the strip rather than the strip covering the buffer;
      File, then Say hello, reaches the program's `on_choose`;
      File, then Quit, closes it - the program says so, and ends.
    """
    program = (
        "local ui = use('/lib/ui.lua') "
        "local wmproto = use('/lib/wmproto.lua') "
        "local win "
        "win = ui.window{ title = 'Strip', w = 240, h = 120, x = 400, y = 300, "
        "direct = true, menubar = { { title = 'File', items = { "
        "{ text = 'Say hello', on_choose = function() print('strip' .. ': said hello') end }, "
        "{ separator = true }, "
        "{ text = 'Quit', on_choose = function() print('strip' .. ': quit') win:close() end } "
        "} } } } "
        "for _ = 1, 2 do win:surface():fill(0, 0, 240, 120, 0xff3060c0) "
        "win:commit{ x = 0, y = 0, w = 240, h = 120 } end "
        "while win.running do "
        "local reply = wmproto.poll(win.handle, 10) "
        "if not reply then break end "
        "for _, ev in ipairs(reply.events or {}) do "
        "if ev.type == 'menubar' then "
        "print('strip' .. ': menubar ' .. ev.title .. ' at ' .. ev.x .. ' ' .. ev.y) end "
        "if not win:direct_event(ev) and ev.type == 'close' then win:close() end "
        "end end "
        "print('strip' .. ': gone')"
    )
    guest.type("fs.write('/ramfs/strip.lua', %r)" % program)
    time.sleep(1.0)
    mark = len(guest.seen)
    guest.type("wm /ramfs/strip.lua")

    def said(text, seconds=20):
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            guest._read_available()

            if text in guest.seen[mark:]:
                return True

            time.sleep(0.3)

        return False

    if not said("wm: window Strip at"):
        raise Failure("the direct window with a menu bar never opened:\n"
                      + guest.seen[mark:][-800:])

    placed = re.search(r"wm: window Strip at (\d+),(\d+) (\d+)x(\d+)",
                       guest.seen[mark:])
    x, y, w, h = (int(v) for v in placed.groups())
    strip = h - 120
    blue = (0x30, 0x60, 0xc0)
    width, height, _ = parse_ppm(guest.screendump())

    def at(px, xx, yy):
        o = (yy * width + xx) * 3
        return tuple(px[o:o + 3])

    def drawn(w_, h_, px):
        below = at(px, x + w - 20, y + strip + 4)
        above = at(px, x + w - 20, y + strip - 6)

        return px if (below == blue and above != blue) else None

    if not 16 <= strip <= 48:
        raise Failure("the window is %d tall for a 120-row buffer, so no menu "
                      "bar was added above it" % h)

    settle(guest, drawn, "the menu bar is not drawn above the window's own "
           "pixels: the blue does not start %d rows down, under a strip that "
           "is not blue" % strip)

    def click(cx, cy):
        guest.mouse_to(*_to_tablet(cx, cy, width, height))
        time.sleep(0.3)
        guest.mouse_button(True)
        time.sleep(0.2)
        guest.mouse_button(False)
        time.sleep(0.6)

    def choose(row, marker):
        before = len(guest.seen)
        click(x + 4 + 10, y + strip // 2)

        if not said("strip: menubar File at", 15) \
                or "strip: menubar File at" not in guest.seen[before:]:
            raise Failure("a press on File in the strip did not reach the "
                          "program as a menubar event:\n"
                          + guest.seen[before:][-600:])

        where = re.findall(r"strip: menubar File at (\d+) (\d+)",
                           guest.seen[before:])[-1]
        mx, my = int(where[0]), int(where[1])
        time.sleep(1.0)

        # The kit's rows are `MENU_ROW`, the fixed layout's (`menu_metrics`),
        # after a two-row border; `row` counts from one, as `menu_mouse`
        # does. This said 22 - "a glyph and six" - a size the rows had not
        # been since the layout was fixed, and it worked while row 3 at 22
        # still landed inside row 3 at 24.
        click(mx + 20, my + 2 + (row - 1) * MENU_ROW + MENU_ROW // 2)

        deadline = time.monotonic() + 15

        while marker not in guest.seen[before:] and time.monotonic() < deadline:
            time.sleep(0.3)
            guest._read_available()

        if marker not in guest.seen[before:]:
            raise Failure("File, then row %d, did not reach the program's "
                          "on_choose:\n%s" % (row, guest.seen[before:][-600:]))

    choose(1, "strip: said hello")
    choose(3, "strip: gone")

    back = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()
    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[back:]:
            break

        time.sleep(0.3)

    return 3


def check_appearance(guest):
    """**The look, chosen in Preferences, at the size it was drawn.**

    This was the Appearance panel's phase, and the panel is gone: on 24
    September its look, wallpaper and scale folded into Preferences'
    Appearance page, which `docs/preferences.html` draws and Diego asked to
    be "pixel perfect as the html mockups" (`roadmap.md` 5zp). So the
    window is held to its size - the drawing's column, at 740 by 680 since
    Diego found the drawing's 840 by 920 too big - which it says as it
    opens, and it offers exactly the looks `themes.lua` ships.
    """
    mark = len(guest.seen)

    guest.type("wm preferences")
    line = guest.wait_for_line("preferences: ",
                               "Preferences to lay itself out", mark)
    said = re.match(r"(\d+)x(\d+), (\d+) looks, (\S+)", line)

    if not said:
        raise Failure("Preferences said %r, which is not a layout" % line)

    width, height, looks = (int(said.group(1)), int(said.group(2)),
                            int(said.group(3)))
    checks = 1

    #
    # **740 by 680, which is Diego's call over the drawing's 840 by 920**:
    # "the entire preferences app looks too big with a lot of whitespace
    # unused" (24 September). The column is the drawing's; the window is
    # the column with its margins and as tall as the tallest page.
    #
    if (width, height) != (740, 680):
        raise Failure("Preferences is %dx%d; it is 740x680 - the drawing's "
                      "column with its margins, as tall as Appearance"
                      % (width, height))

    checks += 1

    #
    # **Five, and the number is Diego's to move.** This asked for four and
    # was right to: "Let's just make 3 or 4 good design options in colors and
    # fonts and stick to those", 22 September. Endeavour is the fifth and he
    # asked for it by name on 23 September, with two screenshots of a GNOME
    # desktop beside him (`roadmap.md` 5zk).
    #
    # The check stays a number rather than becoming `len(themes.order)`,
    # because what it is guarding is that nobody adds a sixth without a
    # conversation - and a check that counts whatever is there guards
    # nothing.
    #
    if looks != 5:
        raise Failure("Preferences offers %d looks; there are five - Plex, "
                      "Plex Night, Classic, Studio and Endeavour" % looks)

    checks += 1

    #
    # **And the desktop is stopped again**, which is the phase's own
    # housekeeping and not a detail: every phase in this part shares one
    # machine, and a desktop that is left running owns the console - so the
    # next phase types `wm snes:--scale 3` at a window manager instead of a
    # shell and hears nothing back. That is exactly how this phase failed
    # the suite the first time it ran beside others.
    #
    # **Waited for from the stop, not from the phase's start.** `mark` was
    # taken before `wm preferences` was typed, and the prompt that command
    # was typed at is after it - so this wait was over before it began, and
    # the next command raced the window manager's exit: twice on x86 on 22
    # September, a stray `q` at the prompt and the next look never chosen
    # (`testing.md` 18.145).
    #
    back = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    end = time.monotonic() + 15

    while time.monotonic() < end:
        guest._read_available()

        if PROMPT in guest.seen[back:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-W Q did not get the screen back after the "
                      "Appearance panel")

    checks += check_theme_plex(guest)
    checks += check_theme_events(guest)
    checks += check_deskbar_fixed(guest)

    return checks


def check_deskbar_fixed(guest):
    """**The Deskbar is 32 pixels, whatever was saved** (`roadmap.md` 5v).

    For an afternoon it was 36, 44 or 52, chosen in Appearance and kept in
    `/home/.appearance`; then Diego, on the ThinkPad on 22 September: "Taskbar
    size should not be changeable let's make it fixed at 32". A `/home`
    written by 0.10.111 still says `bar_h = 52`, so that is what is saved
    here, and a desktop started over it has to paint the bar to its 32nd
    row and leave the 35th to the desk.

    **In Plex, whose Deskbar is its tab's yellow** (`roadmap.md` 5y). Diego:
    "the deskbar tab color should be yellow or at least the same color of
    the acccent color of the theme". Plex's Deskbar was stone, `#e7e7e3`,
    until then; its tab is `#f2c230`. The bar is a gradient, so its last
    row is near the tab's colour rather than on it, and stone is far from
    it in blue. The later of two `palette` keys in the table is the one
    Lua keeps.
    """
    guest.type(appearance('bar_h = 52, palette = "plex"')
               + ' print("height" .. "-saved")')
    guest.wait_for("height-saved", "save a Deskbar height of 52, as 0.10.111 "
                   "did")
    guest.type("wm deskbar")

    def fixed_bar(w, h, px):
        inside = (31 * w + w // 2) * 3
        below = (34 * w + w // 2) * 3

        # Plex's tab yellow at the bar's last row, and its desk below.
        def yellow(r_, g_, b_):
            return (abs(r_ - 0xf2) <= 12 and abs(g_ - 0xc2) <= 12
                    and abs(b_ - 0x30) <= 16)

        if yellow(*px[inside:inside + 3]) and not yellow(*px[below:below + 3]):
            return True

        return None

    try:
        settle(guest, fixed_bar, "a desktop started in Plex over a saved "
               "height of 52 never drew a Deskbar 32 pixels tall in Plex's "
               "tab yellow", seconds=30)
    finally:
        stop_desktop(guest)
        guest.type(appearance() + ' print("height" .. "-reset")')
        guest.wait_for("height-reset", "put the harness's appearance back")

    return 2


def stop_desktop(guest):
    """Control-W Q, and the prompt back - the phases above spell it out."""
    back = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()
    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[back:]:
            break

        time.sleep(0.3)


def check_theme_events(guest):
    """**Four theme events reach a window that was not polling.**

    A theme event is a whole palette and five faces, several hundred bytes,
    and the window manager filled a poll's reply by *count* - twelve events -
    so three of them already made a reply that would not fit in a message.
    On the ThinkPad on 22 September that was `wm: reply for poll failed:
    value does not fit in a message`, and the window's events were gone.

    A program opens a window, sends the window manager four theme messages
    that change nothing - the palette and faces already in force - so four
    theme events queue for it, and then polls. All four have to arrive,
    across however many replies it takes.
    """
    program = (
        "local ui = use('/lib/ui.lua') "
        "local wmproto = use('/lib/wmproto.lua') "
        "local w = ui.window{ title = 'Events', w = 200, h = 80, "
        "x = 100, y = 600 } "
        "for _ = 1, 4 do fs.send('/app/wm', { type = 'theme', "
        "palette = ui.theme.current(), fonts = ui.theme.fonts }) end "
        "local got, polls = 0, 0 "
        "while got < 4 and polls < 20 do "
        "local r = wmproto.poll(w.handle, 20) "
        "polls = polls + 1 "
        "if not r then break end "
        "for _, ev in ipairs(r.events or {}) do "
        "if ev.type == 'theme' then got = got + 1 end end end "
        "print('theme' .. '-events: ' .. got .. ' in ' .. polls)"
    )
    guest.type("fs.write('/ramfs/events.lua', %r)" % program)
    time.sleep(1.0)
    mark = len(guest.seen)
    guest.type("wm events,/ramfs/events.lua")

    try:
        said = guest.wait_for_line("theme-events: ",
                                   "a window to count its theme events", mark)
    finally:
        back = len(guest.seen)
        guest.proc.stdin.write(STOP_DESKTOP)
        guest.proc.stdin.flush()
        deadline = time.monotonic() + 15

        while time.monotonic() < deadline:
            guest._read_available()

            if PROMPT in guest.seen[back:]:
                break

            time.sleep(0.3)

    if not said.startswith("4 in "):
        raise Failure("a window sent four theme events received %r - a "
                      "reply that did not fit took the rest with it" % said)

    return 1


# Plex's five faces, as `docs/plex.html` has them and Diego chose them on
# 22 September - the same table `tools/test_theme.lua` holds the file to.
PLEX_HELD = ("ui=ibmplexsans/18 title=ibmplexsans-semibold/18 "
             "text=ibmplexsans/18 mono=ibmplexmono/16 "
             "heading=ibmplexsans-semibold/18 label=ibmplexsans-medium/18")


def check_theme_plex(guest):
    """**A theme chooses its faces** (`roadmap.md` 5s).

    Diego, 21 September: "a theme is a complete color scheme + font
    selection". The theme file is held on the host by `test_theme.lua`;
    what only a machine can show is the rest of the path - the panel taking
    the theme's five faces, the window manager loading every one of them,
    and the choice written down. So `wm preferences:--theme plex` chooses it
    the way a press on its swatch does, and prints what the window manager
    says it *holds*, which is what was loaded rather than what was asked
    for: a face the image lacks would show there as the previous one.

    Then `/home/.appearance` is read back for the palette's name and a face;
    a fresh desktop is asked what it wears, since a theme that does not
    survive a restart is not a setting; and the harness's own appearance is
    put back, since every phase after this one measured its rows in the
    faces that file pins.
    """
    mark = len(guest.seen)

    guest.type("wm preferences:--theme plex")

    try:
        line = guest.wait_for_line("preferences: theme plex",
                                   "Preferences to choose Plex", mark)

        if not line.startswith("applied, held "):
            raise Failure("choosing Plex was not applied: %r" % line)

        held = line[len("applied, held "):]

        # As a set: the panel lists its roles in its own order, which is
        # not the point.
        if sorted(held.split()) != sorted(PLEX_HELD.split()):
            raise Failure("choosing Plex left the window manager holding %r, "
                          "not Plex's faces %r" % (held, PLEX_HELD))
    finally:
        back = len(guest.seen)
        guest.proc.stdin.write(STOP_DESKTOP)
        guest.proc.stdin.flush()
        deadline = time.monotonic() + 15

        while time.monotonic() < deadline:
            guest._read_available()

            if PROMPT in guest.seen[back:]:
                break

            time.sleep(0.3)

    mark = len(guest.seen)
    guest.type('local a = fs.read("/home/.appearance") '
                'print("saved" .. ": " .. tostring(a and a.palette) .. " " '
                '.. tostring(a and a.fonts ~= nil))')
    saved = guest.wait_for_line("saved: ",
                                "the saved appearance to be read back", mark)

    #
    # **And still Plex after a restart**, which is what a theme is for. The
    # window manager applied a saved theme by name and knew only the two
    # palettes compiled into `theme.lua`, so Photon, BeOS, Platinum, IRIX
    # and Plex were written down faithfully and came back as `dark` - with
    # nothing said. A fresh desktop is asked which theme it wears and which
    # heading face it holds.
    #
    #
    # And the fixed layout reaching an application (`roadmap.md` 5x): a
    # list's row is 24 whatever the face, and a button given no size is its
    # words and 16 either side, 28 tall.
    #
    program = ("local ui = use('/lib/ui.lua') "
               "local w = ui.window{ title = 'Spacing', w = 200, h = 90, "
               "x = 900, y = 500 } "
               "local l = ui.list{ x = 0, y = 0, w = 100, h = 60, "
               "items = { 'a' } } "
               "local b = ui.button{ text = 'Probe' } "
               "local r = fs.send('/app/wm', { type = 'theme' }) "
               "local h = r and r.held and r.held.heading "
               "print('theme' .. '-now: ' .. tostring(r and r.palette) .. ' ' "
               ".. tostring(h and (h.font .. '/' .. h.px))) "
               "print('theme' .. '-space: ' .. l:row_height() "
               ".. ' ' .. (b.w - gfx.measure('Probe')) .. ' ' .. b.h)")
    guest.type("fs.write('/ramfs/themenow.lua', %r)" % program)
    time.sleep(1.0)
    mark = len(guest.seen)
    guest.type("wm themenow,/ramfs/themenow.lua")

    try:
        now = guest.wait_for_line("theme-now: ",
                                  "the restarted desktop to say its theme",
                                  mark)
        space = guest.wait_for_line("theme-space: ",
                                    "an application to measure Plex's "
                                    "spacing", mark)
    finally:
        back = len(guest.seen)
        guest.proc.stdin.write(STOP_DESKTOP)
        guest.proc.stdin.flush()
        deadline = time.monotonic() + 15

        while time.monotonic() < deadline:
            guest._read_available()

            if PROMPT in guest.seen[back:]:
                break

            time.sleep(0.3)

        guest.type(appearance() + ' print("plex" .. "-reset")')
        guest.wait_for("plex-reset", "put the harness's appearance back")

    # The look and nothing of its parts: faces come with the look, so
    # none are written down to outlive it.
    if saved != "plex false":
        raise Failure("/home/.appearance holds %r after choosing Plex - "
                      "wanted the look's name and no faces of its own" % saved)

    if now != "plex ibmplexsans-semibold/18":
        raise Failure("a desktop started with Plex saved wears %r - the "
                      "theme was written down and did not come back" % now)

    # The drawings' fixed layout since 0.10.149: a row 32, a button its
    # words and 26 (12 either side and the rule), 31 tall. It was 24, 32, 28.
    if space != "32 26 31":
        raise Failure("under Plex a list's row is %s, a button its words and "
                      "%s, and %s tall - the fixed layout is 32, 26 and 31"
                      % tuple((space.split() + ["?", "?", "?"])[:3]))

    return 5


def check_tabs(guest):
    """The title bar: across the whole window, and a maximise box greyed.

    Diego, 22 September: "i want to switch back the tabs from be os style
    to full width", and "when a window cant be maximixed we shouldnt remove
    the button we should just gray it out and disable it". Two windows of
    one program that draw their own pixels - so neither can be maximised -
    Behind (blue) and Front (green) on top of it, placed so the row of
    Front's title bar lies across Behind's body; and a `/home/.appearance`
    that still says `tabs = "beos"`, as the old panel wrote it:

      the point where a BeOS tab would have ended, in Front's title row, is
      the tab's yellow - the bar is across, whatever was saved;
      the window manager says Front's bar is its whole frame wide;
      Front's maximise box is there, its glyph in `text_dim`;
      and a press on it that drags does nothing - before, with no box
      there, the same press took the window by its title and moved it.

    Front is only a third over Behind: more than half and the window
    manager would open it somewhere it is not buried.
    """
    program = (
        "local ui = use('/lib/ui.lua') "
        "local wmproto = use('/lib/wmproto.lua') "
        "local back = ui.window{ title = 'Behind', w = 700, h = 400, x = 300, "
        "y = 250, direct = true } "
        "local front = ui.window{ title = 'Front', w = 500, h = 150, x = 350, "
        "y = 600, direct = true } "
        "for _ = 1, 2 do back:surface():fill(0, 0, 700, 400, 0xff3060c0) "
        "back:commit{ x = 0, y = 0, w = 700, h = 400 } "
        "front:surface():fill(0, 0, 500, 150, 0xff30a040) "
        "front:commit{ x = 0, y = 0, w = 500, h = 150 } end "
        "print('tabs' .. ': ready') "
        "while back.running and front.running do "
        "local r = wmproto.poll(front.handle, 1) if not r then break end "
        "for _, ev in ipairs(r.events or {}) do "
        "if ev.type == 'mouse' and ev.action == 'press' then "
        "print('tabs' .. ': front pressed') back:close() front:close() end end "
        "r = wmproto.poll(back.handle, 0) if not r then break end "
        "end print('tabs' .. ': gone')"
    )
    # In pieces: a line typed at the prompt is cut at about a kilobyte.
    guest.type("TABS_SRC = ''")

    for at in range(0, len(program), 600):
        guest.type("TABS_SRC = TABS_SRC .. %r" % program[at:at + 600])
        time.sleep(0.3)

    guest.type("fs.write('/ramfs/tabs.lua', TABS_SRC) print('tabs' .. '-written')")
    guest.type(appearance('tabs = "beos"') + ' print("tabs" .. "-saved")')
    guest.wait_for("tabs-saved", "save the old panel's tab shape")
    time.sleep(1.0)
    mark = len(guest.seen)
    guest.type("wm /ramfs/tabs.lua")

    def said(text, seconds=25):
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            guest._read_available()

            if text in guest.seen[mark:]:
                return True

            time.sleep(0.3)

        return False

    try:
        if not said("tabs: ready") or not said("wm: window Front at"):
            raise Failure("the two windows for the title bar never opened:\n"
                          + guest.seen[mark:][-800:])

        front = re.search(r"wm: window Front at (\d+),(\d+) (\d+)x(\d+), a "
                          r"tab (\d+) wide", guest.seen[mark:])
        behind = re.search(r"wm: window Behind at (\d+),(\d+) (\d+)x(\d+)",
                           guest.seen[mark:])

        if not front or not behind:
            raise Failure("the window manager did not say where Front and "
                          "Behind are:\n" + guest.seen[mark:][-800:])

        fx, fy, fw, fh, bar = (int(v) for v in front.groups())
        bx, by, bw, bh = (int(v) for v in behind.groups())

        if bar != fw + 2 * FRAME:
            raise Failure("Front's title bar is %d wide and its frame %d - a "
                          "bar across is the frame's width, whatever "
                          "/home/.appearance says" % (bar, fw + 2 * FRAME))

        # Where a BeOS tab on "Front" would have ended long before: over
        # Behind's body, in Front's title row, left of the boxes.
        px, py = fx + fw * 6 // 10, fy - 13

        if not (bx <= px < bx + bw and by <= py < by + bh):
            raise Failure("Front's title row does not lie over Behind, so "
                          "the check has nothing to look through")

        width, height, _ = parse_ppm(guest.screendump())

        def pixel(x, y):
            _, _, px_ = parse_ppm(guest.screendump())
            o = (y * width + x) * 3
            return tuple(px_[o:o + 3])

        time.sleep(1.5)
        across = pixel(px, py)

        # Within `TAB_TOL`: the bar is a gradient over the tab's colour.
        if any(abs(a - b) > TAB_TOL for a, b in zip(across, TAB)):
            raise Failure("Front's title row is %r where a BeOS tab would "
                          "have ended - wanted the tab's yellow %r across the "
                          "whole window" % (across, TAB))

        #
        # The maximise box, greyed. It is the middle of the three at the
        # right: the run ends `MARGIN` - 10 since 0.10.149, it was 4 - in
        # from the frame, each box a `BOX_W` slot of 22 and the last one
        # `BOX`, 18, so maximise starts 50 in from the frame's right edge
        # (`boxes_x`), which is `FRAME` outside the content's. 22 above the
        # top.
        #
        # **Since 0.10.149 the three are coloured circles** (`roadmap.md`
        # 5zq), and a maximise that cannot be used is a grey one with no
        # glyph where a working one is green. So the check is the disc's
        # middle: grey - its three channels together - and not the green,
        # which holds in any look without knowing the look's grey.
        #
        zx, zy = fx + fw + FRAME - 50, fy - 22
        glyph = pixel(zx + 9, zy + 9)
        green = (0x28, 0xc8, 0x40)

        if glyph == green or max(glyph) - min(glyph) > 24:
            raise Failure("Front cannot be maximised and its maximise "
                          "circle's middle is %r - wanted it grey, not the "
                          "green %r of one that works, nor anything else "
                          % (glyph, green))

        # Pressed and dragged, it does nothing: Front's top-left stays green.
        corner = pixel(fx + 6, fy + 6)
        guest.mouse_to(*_to_tablet(zx + 9, zy + 9, width, height))
        time.sleep(0.3)
        guest.mouse_button(True)
        time.sleep(0.2)

        for step in range(1, 7):
            guest.mouse_to(*_to_tablet(zx + 9 - 12 * step, zy + 9 + 12 * step,
                                       width, height))
            time.sleep(0.1)

        guest.mouse_button(False)
        time.sleep(1.5)
        after = pixel(fx + 6, fy + 6)

        if corner != (0x30, 0xa0, 0x40) or after != corner:
            raise Failure("a press on Front's greyed maximise box, dragged, "
                          "moved the window: its corner was %r and is %r"
                          % (corner, after))

        guest.mouse_to(*_to_tablet(fx + fw // 2, fy + fh // 2, width, height))
        time.sleep(0.3)
        guest.mouse_button(True)
        time.sleep(0.2)
        guest.mouse_button(False)
        said("tabs: gone", 10)
    finally:
        stop_desktop(guest)
        guest.type(appearance() + ' print("tabs" .. "-reset")')
        guest.wait_for("tabs-reset", "put the harness's appearance back")

    return 4


def check_corners(guest):
    """**A rounded window's corners show what is behind them, after a move
    onto its own old place** (`roadmap.md` 5zr, `ui.md` 16.22).

    Diego, 24 September, with a photograph of Photo's bottom-right corner:
    "There seems to be an issue with rounded corners in that app". Its own
    dark page showed outside the curve. The compositor's culling cut each
    opaque window's whole frame rectangle out of what it painted behind, so
    under a rounded corner nothing was painted and the corner was put back
    from whatever the backbuffer last held - which was that window's own
    page when `tile` had moved it up and to the left onto itself.

    So: a window filled red, moved thirty up and thirty left once it has
    drawn, and the pixel just inside its frame's new bottom-right corner -
    outside the curve - has to be the desk, not red. The control is the
    build before `OUT.uncover`, where that pixel is red.

    **And a menu's corners.** A menu is composed in a loop of its own and
    was never rounded there, while the kit drew a rounded line inside it -
    so once a flat control stopped filling its square, a menu's corners
    were its surface's dark ground (seen with the ui face at 18, 24
    September). The window opens a menu over the desk and its four corner
    pixels have to be the desk. The control is the build before the menu
    loop kept and put back its corners, where they are the menu's own.
    """
    program = (
        "local ui = use('/lib/ui.lua') "
        "local w = ui.window{ title = 'Corner', w = 240, h = 140, "
        "x = 400, y = 300 } "
        "if not w then return end "
        "local v = ui.view{ x = 0, y = 0, w = 240, h = 140 } "
        "function v:draw(g) g:fill(0, 0, self.w, self.h, 0xffe03030) end "
        "w:add(v) "
        "local n = 0 "
        "function w:on_frame() n = n + 1 "
        "if n == 20 then w:move(370, 270) print('corner' .. ': moved') end "
        "if n == 30 then local m = w:open_menu(700, 300, "
        "{ { text = 'One' }, { text = 'Two' } }) "
        "if m then print(('corner' .. '-menu: %d %d %d %d')"
        ":format(m.x, m.y, m.w, m.h)) end end "
        "return false end "
        "w:run()"
    )
    guest.type("fs.write('/ramfs/corner.lua', %r)" % program)
    mark = len(guest.seen)
    guest.type("wm /ramfs/corner.lua")

    deadline = time.monotonic() + 40

    while time.monotonic() < deadline:
        guest._read_available()

        if "corner: moved" in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        stop_desktop(guest)
        raise Failure("the corner window never moved itself:\n"
                      + guest.seen[mark:][-800:])

    try:
        time.sleep(2.0)
        width, height, px = parse_ppm(guest.screendump())

        def at(x, y):
            o = (y * width + x) * 3
            return tuple(px[o:o + 3])

        # The frame's new bottom-right corner, `FRAME` outside the content,
        # and one pixel in from it: outside any curve a corner has.
        cx, cy = 370 + 240 + FRAME - 1, 270 + 140 + FRAME - 1
        desk = at(10, 700)
        corner = at(cx, cy)

        if corner == (0xe0, 0x30, 0x30) or corner != desk:
            raise Failure(
                "outside the moved window's bottom-right curve is %r where "
                "the desk is %r - what showed there is what the backbuffer "
                "held before, because nothing behind a rounded corner was "
                "painted" % (corner, desk))

        line = guest.wait_for_line("corner-menu: ", "the window to open a "
                                   "menu over the desk", mark)
        mx, my, mw, mh = (int(v) for v in line.split()[:4])
        time.sleep(1.5)
        width, height, px = parse_ppm(guest.screendump())

        for x, y in ((mx, my), (mx + mw - 1, my), (mx, my + mh - 1),
                     (mx + mw - 1, my + mh - 1)):
            if at(x, y) != desk:
                raise Failure(
                    "the corner of a menu at %d,%d %dx%d is %r at %d,%d "
                    "where the desk behind it is %r - a menu's corners are "
                    "not rounded" % (mx, my, mw, mh, at(x, y), x, y, desk))
    finally:
        stop_desktop(guest)

    return 2


def check_shadow(guest):
    """**A window casts a shadow, and takes it with it when it moves.**

    Shadows were made fast on 24 September by clipping them to the rectangle
    being composed (`testing.md` 18.165) - and the window manager hands each
    window only its frame's visible part, so the shadow, which lies outside
    the frame, was clipped to nothing. Diego: "the new drop shadow does not
    work". Nothing here had ever looked for one.

    So: shadows on, a window that moves itself 250 right and 30 down once
    it has drawn - far enough that its new shadow cannot reach where the old
    one was - and then the pixels just under its frame where it is now have
    to be darker than the desk in every channel, and where its shadow was
    before the move have to be the desk again. The control
    is the build that clipped the shadow to the frame, which shows none.
    """
    program = (
        "local ui = use('/lib/ui.lua') "
        "local w = ui.window{ title = 'Shade', w = 240, h = 140, "
        "x = 400, y = 300 } "
        "if not w then return end "
        "local n = 0 "
        "function w:on_frame() n = n + 1 "
        "if n == 20 then w:move(650, 330) print('shade' .. ': moved') end "
        "return false end "
        "w:run()"
    )
    guest.type("fs.write('/ramfs/shade.lua', %r)" % program)
    guest.type(appearance("shadow = true") + ' print("shadow" .. "-on")')
    guest.wait_for("shadow-on", "switch shadows on")
    mark = len(guest.seen)
    guest.type("wm /ramfs/shade.lua")

    deadline = time.monotonic() + 40

    while time.monotonic() < deadline:
        guest._read_available()

        if "shade: moved" in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        stop_desktop(guest)
        raise Failure("the shadowed window never moved itself:\n"
                      + guest.seen[mark:][-800:])

    try:
        time.sleep(2.0)
        width, height, px = parse_ppm(guest.screendump())

        def at(x, y):
            o = (y * width + x) * 3
            return tuple(px[o:o + 3])

        desk = at(10, 700)

        # Six under the frame's bottom edge, in the middle of it: inside a
        # shadow's band whatever its spread, and clear of the rounding.
        now = at(650 + 120, 330 + 140 + FRAME + 6)
        was = at(400 + 20, 300 + 140 + FRAME + 6)

        if not all(now[i] < desk[i] for i in range(3)):
            raise Failure("under the window's frame is %r where the desk is "
                          "%r - no shadow is being cast" % (now, desk))

        if was != desk:
            raise Failure("where the window's shadow was before it moved is "
                          "%r, not the desk's %r - the shadow was left "
                          "behind" % (was, desk))
    finally:
        stop_desktop(guest)
        guest.type(appearance() + ' print("shadow" .. "-off")')
        guest.wait_for("shadow-off", "put the harness's appearance back")

    return 2


def check_wheel(guest):
    """**The scroll wheel scrolls what is under the pointer.**

    Diego, 24 September: "add scrollwheel mouse suppor to tracker and apps
    so i can scroll a list of files in tracker without going to the
    scrollbars all the time" (`roadmap.md` 5zv). A notch crosses every
    layer - the virtio tablet's wheel, the pointer the kernel reports, the
    window manager posting it to the window under the pointer, and the kit
    handing it to the deepest view that scrolls - and a break in any of them
    is a wheel that does nothing, with nothing to say why.

    The gallery's list shows three of its five rows with the first one
    selected. The pointer goes over the list *without a click*, because the
    wheel follows the pointer and not the focus, and one notch down scrolls
    it by `ui.WHEEL_ROWS`: the selection bar has to leave the list. A notch
    up has to bring it back to the row it was on.

    **Its first run was its control, and found the bug it exists for.** The
    wheel reached the kernel's pointer and stopped at the console: its reply
    to the window manager's `wait` (`conproto.h`) had no field for it, so
    every layer below and above was right and the bar did not move.
    """
    mark = len(guest.seen)
    guest.type("wm gallery")
    started(guest)
    laid = gallery_layout(guest, mark)
    lx, ly, lw, lh = laid["list"]
    box = (60 + lx, 90 + ly, lw, lh)

    try:
        width, height, px = parse_ppm(guest.screendump())
        before = find_colour_in(width, px, box, SELECTED)

        if before is None:
            raise Failure("the gallery's list has no selection bar to scroll "
                          "out of sight")

        guest.mouse_to(*_to_tablet(box[0] + lw // 2, box[1] + lh // 2,
                                   width, height))
        time.sleep(0.4)

        def notch(button, gone):
            guest.mouse_button(True, button)
            time.sleep(0.05)
            guest.mouse_button(False, button)
            deadline = time.monotonic() + 20

            while time.monotonic() < deadline:
                _, _, px_ = parse_ppm(guest.screendump())
                bar = find_colour_in(width, px_, box, SELECTED)

                if (bar is None) == gone and (gone or bar == before):
                    return bar

                time.sleep(0.3)

            raise Failure(
                "a notch of the wheel (%s) over the gallery's list left its "
                "selection bar at %r - it was at %r and should %s"
                % (button, bar, before,
                   "have scrolled out of the list" if gone
                   else "be back where it was"))

        notch("wheel-down", True)
        notch("wheel-up", False)
    finally:
        stop_desktop(guest)

    return 2


def check_scale(guest):
    """**Everything at 150 per cent** (`roadmap.md` 5z, `ui.md` 16.18).

    Diego, 22 September: "a factor multiplier of all the things in the UI".
    With `scale = 150` in `/home/.appearance`, the gallery - a kit window
    asking for the size it says - and a window drawing its own pixels, 200
    by 100 of green:

      the window manager says it is at 150;
      the gallery is half again its size on the screen, and its title bar
      39 tall;
      its list's selection bar is 48 rows, the fixed layout's 32 at 150;
      a click on the list's third row selects the third row - the click
      divided back into points lands where the drawing, multiplied, put it;
      dragged by its title bar 120 by 60, it moves 120 by 60, under the
      pointer;
      and the own-pixel window is 300 by 150, its surface stretched to its
      bottom-right corner.
    """
    program = (
        "local ui = use('/lib/ui.lua') "
        "local wmproto = use('/lib/wmproto.lua') "
        "local w = ui.window{ title = 'Direct', w = 200, h = 100, x = 800, "
        "y = 150, direct = true } "
        "for _ = 1, 2 do w:surface():fill(0, 0, 200, 100, 0xff30a040) "
        "w:commit{ x = 0, y = 0, w = 200, h = 100 } end "
        "print('direct' .. ': ready') "
        "while w.running do local r = wmproto.poll(w.handle, 1) "
        "if not r then break end end"
    )
    guest.type("fs.write('/ramfs/direct.lua', %r)" % program)
    guest.type(appearance("scale = 150") + ' print("scale" .. "-saved")')
    guest.wait_for("scale-saved", "save a scale of 150")
    time.sleep(0.5)
    mark = len(guest.seen)
    guest.type("wm gallery,/ramfs/direct.lua")

    def said(pattern, seconds=30):
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            guest._read_available()
            m = re.search(pattern, guest.seen[mark:])

            if m:
                return m

            time.sleep(0.3)

        return None

    try:
        if not said(r"wm: scale 150"):
            raise Failure("a window manager started over `scale = 150` never "
                          "said it was at 150:\n" + guest.seen[mark:][-800:])

        gallery = said(r"wm: window gallery at (\d+),(\d+) (\d+)x(\d+)")
        direct = said(r"wm: window Direct at (\d+),(\d+) (\d+)x(\d+)")

        if not gallery or not direct or not said(r"direct: ready"):
            raise Failure("the gallery and the own-pixel window did not both "
                          "open:\n" + guest.seen[mark:][-800:])

        gx, gy, gw, gh = (int(v) for v in gallery.groups())
        dx, dy, dw, dh = (int(v) for v in direct.groups())
        laid = gallery_layout(guest, mark)
        asked = laid["size"]
        lx, ly, lw, lh = laid["list"]

        if (gw, gh) != (asked[0] * 3 // 2, asked[1] * 3 // 2):
            raise Failure("the gallery asked for %dx%d and is %dx%d on the "
                          "screen - at 150 it is %dx%d"
                          % (asked + (gw, gh)
                             + (asked[0] * 3 // 2, asked[1] * 3 // 2)))

        if (dw, dh) != (300, 150):
            raise Failure("the own-pixel window asked for 200x100 and is "
                          "%dx%d - at 150 it is 300x150" % (dw, dh))

        time.sleep(2.0)
        shot = guest.screendump()
        width, height, px = parse_ppm(shot)

        # Kept, so a person can look at the desktop at 150 as the check saw
        # it - through a file of its own, since both boards run this at once.
        kept = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                            "build", "scale-150.ppm")

        with open("%s.%d" % (kept, os.getpid()), "wb") as f:
            f.write(shot)

        os.replace("%s.%d" % (kept, os.getpid()), kept)

        def at(x, y):
            o = (y * width + x) * 3
            return tuple(px[o:o + 3])

        #
        # The title bar: the rows straight above the own-pixel window that
        # are neither the desk nor its shadow. Not by the tab's colour,
        # since which of the two windows opened last - and so is focused -
        # is a race.
        #
        # **The shadow is why "not the desk" is not enough** (`roadmap.md`
        # 5zj, 23 September). A window casts one now, so between its tab and
        # the desk there are a dozen rows that are the desk *darkened* - and
        # this counted them as title bar and read 49 where the tab is 39.
        #
        # A shadow is darker than the desk in every channel and a tab is
        # not: every look's tab is lighter than its desktop, because a title
        # bar that recedes into the background is a title bar nobody finds.
        # So "darker than the desk everywhere" separates the two without
        # knowing either colour.
        #
        desk = at(10, 700)
        rows = 0

        while rows < 80 and dy - 1 - rows >= 0:
            here = at(dx + dw // 2, dy - 1 - rows)

            if here == desk or all(here[i] <= desk[i] for i in range(3)):
                break

            rows += 1

        if abs(rows - 39) > 2:
            raise Failure("the own-pixel window's title bar is %d rows tall - "
                          "the tab's 26 at 150 is 39" % rows)

        # The own-pixel window's surface reaches its bottom-right corner -
        # sampled twelve in from it, since the page is rounded inside the
        # frame (0.10.149) and the last few pixels of its corner are frame.
        corner = at(dx + dw - 12, dy + dh - 12)

        if corner != (0x30, 0xa0, 0x40):
            raise Failure("near the own-pixel window's bottom-right corner is "
                          "%r, not its green - its 200x100 surface was not "
                          "stretched to its 300x150 place" % (corner,))

        # The list's selection bar: its height, in the list's column.
        def selection():
            _, _, px_ = parse_ppm(guest.screendump())
            x = gx + (lx + lw // 3) * 3 // 2
            top = run = None

            for y in range(gy, gy + gh):
                o = (y * width + x) * 3

                if tuple(px_[o:o + 3]) == SELECTED:
                    if top is None:
                        top, run = y, 0

                    run += 1
                elif top is not None:
                    break

            return top, run

        top, run = selection()

        if top is None or abs(run - LAYOUT_ROW * 3 // 2) > 1:
            raise Failure("the gallery's selection bar is %r rows tall at %r "
                          "- a row of %d at 150 is %d"
                          % (run, top, LAYOUT_ROW, LAYOUT_ROW * 3 // 2))

        # A click on the third row: the list's rows start two in, so the
        # third row's middle is two and a half rows below that.
        third = ly + 2 + 2 * LAYOUT_ROW + LAYOUT_ROW // 2
        guest.mouse_to(*_to_tablet(gx + (lx + lw // 3) * 3 // 2,
                                   gy + third * 3 // 2, width, height))
        time.sleep(0.3)
        guest.mouse_button(True)
        time.sleep(0.2)
        guest.mouse_button(False)

        want = gy + (ly + 2 + 2 * LAYOUT_ROW) * 3 // 2
        deadline = time.monotonic() + 15
        now = (top, run)

        while time.monotonic() < deadline:
            now = selection()

            if now[0] is not None and abs(now[0] - want) <= 2:
                break

            time.sleep(0.4)
        else:
            raise Failure("a click on the third row left the selection bar at "
                          "%r, not near %d - the click and the drawing do not "
                          "agree on where the row is" % (now, want))

        #
        # **Dragged by its title bar, it stays under the pointer.** Diego, on
        # the ThinkPad at 110: "if you grab a window by the titlebar ... the
        # mouse is off by a margin", "even worse with more scale". The drag
        # handed screen pixels to the handler that converts an application's
        # points, so the window went half again as far as the pointer at 150.
        # 120 across and 60 down is what it has to move - the selection bar,
        # the one thing of its colour, measured before and after.
        #
        def bar_corner():
            _, _, px_ = parse_ppm(guest.screendump())

            for y in range(gy, min(height, gy + gh + 120)):
                for x in range(gx, min(width, gx + gw + 200)):
                    o = (y * width + x) * 3

                    if tuple(px_[o:o + 3]) == SELECTED:
                        return x, y

            return None

        before = bar_corner()
        grab_x, grab_y = gx + 300, gy - 20
        guest.mouse_to(*_to_tablet(grab_x, grab_y, width, height))
        time.sleep(0.3)
        guest.mouse_button(True)
        time.sleep(0.2)

        for step in range(1, 7):
            guest.mouse_to(*_to_tablet(grab_x + 20 * step, grab_y + 10 * step,
                                       width, height))
            time.sleep(0.1)

        guest.mouse_button(False)
        time.sleep(1.5)
        after = bar_corner()

        if (before is None or after is None
                or abs(after[0] - before[0] - 120) > 2
                or abs(after[1] - before[1] - 60) > 2):
            raise Failure("dragged by its title bar 120 across and 60 down, "
                          "the gallery went from %r to %r - at 150 the drag "
                          "moved it further than the pointer" % (before, after))
    finally:
        stop_desktop(guest)
        guest.type(appearance() + ' print("scale" .. "-reset")')
        guest.wait_for("scale-reset", "put the harness's appearance back")

    return 7


def check_scale_live(guest):
    """**The scale changed with windows open** (`roadmap.md` 5z).

    Preferences' scale chosen at 150 - `wm gallery,preferences:--scale 150`
    - has to rebuild the gallery, already open at the size it says, at half
    again that, say so, and write 150 down; and let go at 100 again, the
    gallery comes back to its size exactly, not a point smaller for the
    trip.
    """
    guest.type(appearance() + ' print("live" .. "-reset")')
    guest.wait_for("live-reset", "start the live scale at 100")
    mark = len(guest.seen)
    guest.type("wm gallery,preferences:--scale 150")

    def said(pattern, since, seconds=30):
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            guest._read_available()
            m = re.search(pattern, guest.seen[since:])

            if m:
                return m

            time.sleep(0.3)

        return None

    try:
        if not said(r"preferences: scale 150 applied", mark):
            raise Failure("Preferences never applied a scale of 150:\n"
                          + guest.seen[mark:][-800:])

        asked = gallery_layout(guest, mark)["size"]
        up = said(r"wm: rescaled gallery to (\d+)x(\d+)", mark)
        grown = (asked[0] * 3 // 2, asked[1] * 3 // 2)

        if not up or (int(up.group(1)), int(up.group(2))) != grown:
            raise Failure("the open gallery was rescaled to %s - it asked for "
                          "%dx%d, and at 150 it is %dx%d"
                          % ((up and up.group(0),) + asked + grown))
    finally:
        stop_desktop(guest)

    mark = len(guest.seen)
    guest.type('local a = fs.read("/home/.appearance") '
               'print("saved" .. "-scale " .. tostring(a and a.scale))')
    kept = said(r"saved-scale (\S+)", mark)

    if not kept or kept.group(1) != "150":
        raise Failure("/home/.appearance holds scale %r after 150 was chosen"
                      % (kept and kept.group(1)))

    mark = len(guest.seen)
    guest.type("wm gallery,preferences:--scale 100")

    try:
        back = said(r"wm: rescaled gallery to (\d+)x(\d+)", mark)

        if not back or (int(back.group(1)), int(back.group(2))) != asked:
            raise Failure("back at 100 the gallery is %s - it asked for "
                          "%dx%d" % ((back and back.group(0),) + asked))
    finally:
        stop_desktop(guest)
        guest.type(appearance() + ' print("live" .. "-done")')
        guest.wait_for("live-done", "put the harness's appearance back")

    return 3


def check_drives_app(guest):
    """The Drives app opens, says what it found, and draws (USB step 6e).

    The harness has no stick and no disk, so what it finds is nothing - and
    "No drives" drawn in its first list, not an empty window and not a Lua
    error, is the case this machine can show. The model under it is held to
    a real stick's two volumes by `run_x86.py`'s `usb_drives`.
    """
    mark = len(guest.seen)
    guest.type("wm drives")
    deadline = time.monotonic() + 30

    while time.monotonic() < deadline:
        guest._read_available()

        if re.search(r"drives: \d+ drive", guest.seen[mark:]) \
                and "wm: window Drives at" in guest.seen[mark:]:
            break

        time.sleep(0.3)

    said = guest.seen[mark:]

    if "wm: window Drives at" not in said:
        raise Failure("the Drives app never opened a window:\n" + said[-800:])

    if not re.search(r"drives: \d+ drive", said):
        raise Failure("the Drives app never said what it found:\n"
                      + said[-800:])

    time.sleep(2.0)
    guest._read_available()
    bad = [l for l in guest.seen[mark:].splitlines()
           if "drives" in l and ("attempt to" in l or "error" in l)]

    if bad:
        raise Failure("the Drives app failed while drawing: " + bad[0])

    back = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()
    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[back:]:
            break

        time.sleep(0.3)

    return 3


def check_snes_scale(guest):
    """`--scale` reaches the Super Nintendo, and never reaches a ROM's name.

    The emulator takes one option before the ROM - `--scale 2`, a window twice
    the size, which a launcher stores with its arguments - and ROMs are named
    the way No-Intro names them, spaces and all, so the rest of the line is the
    name. Both ways that goes wrong are asked here without a ROM, which this
    harness does not carry: a scale it cannot draw is refused by name, and the
    option comes off the front of the line rather than becoming part of the
    file it looks for.

    Started through the window manager, as the Deskbar and a launcher start
    it. The first version typed `snes --scale 3` at the prompt and got
    `table: 0x...` back: the core registers itself as a global named `snes`
    in every Lua state, the shell's included, so there the name is the core
    and `--scale 3` a Lua comment.

    The pixels are `tools/test_snesblit.c`'s, on the host. Whether a real game
    fills a 1024 by 960 window is `make snes-check ROM=...`'s to say.
    """
    def ask(line, *texts):
        """Starts `line`, waits for the first of `texts`, and gives the screen
        back. Answers which text was heard, or None, and everything said."""
        mark = len(guest.seen)
        guest.type(line)
        deadline = time.monotonic() + 30
        heard = None

        while heard is None and time.monotonic() < deadline:
            guest._read_available()
            heard = next((t for t in texts if t in guest.seen[mark:]), None)

            if heard is None:
                time.sleep(0.3)

        answer = guest.seen[mark:]
        stop = len(guest.seen)
        guest.proc.stdin.write(STOP_DESKTOP)
        guest.proc.stdin.flush()
        end = time.monotonic() + 15

        while time.monotonic() < end:
            guest._read_available()

            if PROMPT in guest.seen[stop:]:
                break

            time.sleep(0.3)
        else:
            raise Failure(f"Control-W Q did not get the screen back after `{line}`.")

        return heard, answer

    unbuilt = "not built with SNES=1"
    refused = "snes: --scale is 1 or 2, and 3 is neither"
    heard, answer = ask("wm snes:--scale 3", refused, unbuilt)

    if heard == unbuilt:
        return 0                        # an image without the core: nothing to ask

    if heard != refused:
        raise Failure(
            "`wm snes:--scale 3` was not refused by name; the program said: "
            + repr(answer[-300:])
        )

    looked = "snes: no /home/roms/snes/nosuch.sfc"
    heard, answer = ask("wm snes:--scale 2 nosuch.sfc", looked)

    if heard != looked:
        raise Failure(
            "`wm snes:--scale 2 nosuch.sfc` did not look for exactly nosuch.sfc - "
            "the option has to come off the front of the ROM's name; the "
            "program said: " + repr(answer[-300:])
        )

    return 2


def check_volume_keys(guest):
    """The volume keys reach the window manager, on both boards, and are no
    longer keys the keyboard driver does not know.

    On x86 they come through the i8042 as e0 20, e0 2e and e0 30 and the
    driver's table maps them; on the ARM board the keyboard is virtio and
    sends evdev's codes as they are. Either way the window manager takes
    them before any window, and says what it did - which here, with no
    sound device in this harness, is that there was nothing to answer: this
    phase is the key path, and `run_media.py` is whether the machine goes
    quiet.
    """
    program = (
        "local ui = use('/lib/ui.lua') "
        "local w = ui.window{ title = 'Keys', w = 200, h = 120, x = 400, y = 300 } "
        "if w then w:run() end"
    )
    guest.type("fs.write('/ramfs/keys.lua', %r)" % program)
    time.sleep(1.0)

    mark = len(guest.seen)
    guest.type("wm /ramfs/keys.lua")

    deadline = time.monotonic() + 40
    while "wm: window Keys at" not in guest.seen[mark:] \
            and time.monotonic() < deadline:
        time.sleep(0.3)
        guest._read_available()

    if "wm: window Keys at" not in guest.seen[mark:]:
        raise Failure("the window for the volume keys never opened:\n"
                      + guest.seen[mark:][-600:])

    time.sleep(1.5)
    pressed = len(guest.seen)

    for key in ("volumeup", "volumedown", "audiomute"):
        guest.sendkey(key)
        time.sleep(0.8)

    time.sleep(1.5)
    guest._read_available()
    said = guest.seen[pressed:]

    back = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()
    time.sleep(2.0)

    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        guest._read_available()
        if PROMPT in guest.seen[back:]:
            break
        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back after the "
                      "volume keys.\n" + guest.seen[back:][-600:])

    # One pattern per key, and only mute's accepts "muted": a shared one let
    # the mute line stand in for a volume-up that never arrived.
    wanted = {"up": r"wm: volume up\b", "down": r"wm: volume down\b",
              "mute": r"wm: volume (mute\b|muted)"}

    for which, pattern in wanted.items():
        if not re.search(pattern, said):
            raise Failure(
                "the volume key '%s' never reached the window manager:\n%s"
                % (which, said[-600:]))

    if "has no entry for, e0" in said:
        raise Failure(
            "a volume key was still a key the keyboard driver does not know:"
            "\n" + said[-600:])

    return 2


def check_power_setting(guest):
    """**The power button does what Preferences says** (`roadmap.md` 5zp).

    Power's row - Shut down, Open the menu, Do nothing - wrote
    `/home/.power` and nothing read it: the window manager shut the machine
    down on every press. Diego, 24 September: "go thrpugh all the settings
    options and make sure they do something useful".

    So: "Do nothing" written where Preferences writes it, a desktop
    started, the button pressed through QEMU (`system_powerdown`, which the
    kernel hears as the ACPI button and the window manager as a key), and
    the manager has to say it did nothing - and the machine has to be
    running afterwards, which is the half that matters. The control is the
    manager before `OUT.keys`, which shuts down and takes QEMU with it.

    x86-64 only: on the ARM board the button reaches its driver, which
    reports it and nothing else.
    """
    guest.type('fs.write("/home/.power", { button = "nothing" }) '
               'print("power" .. "-set")')
    guest.wait_for("power-set", "write the power button's setting")
    mark = len(guest.seen)
    guest.type("wm gallery")
    started(guest)
    time.sleep(2.0)

    try:
        guest._qmp("system_powerdown", {})

        deadline = time.monotonic() + 15

        while time.monotonic() < deadline:
            guest._read_available()

            if "wm: the power button" in guest.seen[mark:]:
                break

            time.sleep(0.3)

        said = guest.seen[mark:]

        if "wm: the power button - set to do nothing" not in said:
            raise Failure("the power button was set to do nothing and the "
                          "window manager did not say it did nothing:\n"
                          + said[-600:])

        time.sleep(2.0)

        if guest.proc.poll() is not None:
            raise Failure("the power button was set to do nothing and the "
                          "machine shut down anyway")
    finally:
        # Only a machine still running can be put back; one that shut down
        # has said why above, and a write to its pipe would bury that.
        if guest.proc.poll() is None:
            stop_desktop(guest)
            guest.type('fs.write("/home/.power", {}) '
                       'print("power" .. "-reset")')
            guest.wait_for("power-reset", "put the power button back")

    return 1


def check_unknown_keys(guest):
    """A key the keyboard driver has no entry for is named once, not dropped
    in silence. x86 only: the ARM board's keyboard is virtio, and this is
    the i8042's.

    `hal/pc/i8042.c` dropped any key behind an 0xe0 prefix that its table
    did not know, which is where the ThinkPad's volume and brightness keys
    went - Diego, 18 September, "its too dim now and i cant control it". So
    each is said in the kernel's log the first time it goes down, and never
    again, because a held key repeats and `diagnose` carries that log off
    the stick.

    **The calculator key, because nothing will ever map it.** The volume
    keys are about to have entries, and a test built on them would stop
    testing this the day they did. QEMU sends it as e0 21. Pressed twice,
    and Home once between: named once with its byte, and Home, which the
    driver knows, never.
    """
    time.sleep(1.0)
    mark = len(guest.seen)

    for key in ("calculator", "home", "calculator"):
        guest.sendkey(key)
        time.sleep(0.6)

    time.sleep(2.0)
    guest._read_available()

    said = [line.strip() for line in guest.seen[mark:].splitlines()
            if "i8042: a key this driver has no entry for" in line]

    if not said:
        raise Failure(
            "a key the keyboard driver has no entry for was dropped in "
            "silence - the calculator key, pressed twice, was never named:\n"
            + guest.seen[mark:][-600:])

    if not all(" e0 21 " in line for line in said):
        raise Failure(
            "the keyboard driver named a key, and not the one pressed - "
            "wanted e0 21, the calculator: " + " / ".join(said))

    if len(said) != 1:
        raise Failure(
            "the calculator key, pressed twice, was named %d times - once is "
            "the point, because a held key repeats: %s"
            % (len(said), " / ".join(said)))

    return 3


def check_power_button(guest):
    """The power button reaches a driver that is not in the kernel.

    `user/servers/powerbutton.c` is the first driver in this system outside
    the kernel, and it exists to prove `docs/drivers.md`'s three primitives on
    a device simple enough that a failure points at them. It is told where the
    PL061 is, maps its registers, claims its interrupt and blocks in
    `SYS_IRQ_WAIT`; QEMU's `system_powerdown` pulses the power key; the driver
    wakes, clears the controller, acks, and says so.

    **Twice, and the second press is the test that matters.** The first press
    proves the blocking wait ends on a real interrupt - the one piece of the
    primitives no suite could reach, because only a device can end that wait.
    The second proves the acknowledgement: the kernel masked the line on
    delivery, so if `SYS_IRQ_ACK` did not unmask it the second press would
    arrive at a masked line and never be reported. A driver that forgot to ack
    passes a one-press test.

    aarch64 only: the PC's power button is an ACPI event, not a GPIO line.
    """
    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if "powerbutton: waiting on line" in guest.seen:
            break

        time.sleep(0.3)
    else:
        raise Failure(
            "the power button driver never said it was waiting. It is started "
            "by init with device authority and the console server's endpoint; "
            "look for `no power button driver` in the boot log, or for a line "
            "from the driver saying which of find, map or claim refused it."
        )

    mark = len(guest.seen)

    for press in (1, 2):
        guest._qmp("system_powerdown", {})

        deadline = time.monotonic() + 10

        while time.monotonic() < deadline:
            guest._read_available()

            if guest.seen[mark:].count("powerbutton: pressed") >= press:
                break

            time.sleep(0.2)
        else:
            if press == 1:
                raise Failure(
                    "the power key was pressed and the driver never woke. "
                    "Either the interrupt did not reach `irq_deliver`, or the "
                    "wait did not end on it - which is the half of the "
                    "primitives this is the only test of."
                )

            raise Failure(
                "the first press was reported and the second was not. The "
                "kernel masks a line when it delivers, and only `SYS_IRQ_ACK` "
                "unmasks it - so a missing second press is a missing or "
                "ineffective ack, or a controller cleared after the ack "
                "rather than before it."
            )

        # The key is held for 100 ms of the guest's clock. Well past it, so
        # the second press is a new edge rather than the tail of the first.
        time.sleep(1.0)

    return 2


def check_repaints(guest):
    """A window with nothing to do draws nothing.

    The window manager narrates a *finished* frame under `wm trace` - one
    line when an application sends the last batch of a repaint - and this
    counts them with the desktop sitting still. The answer has to be none.

    **This is here because the bug it catches is invisible every other way.**
    The Terminal repainted itself once a second, for ever, with nothing to
    redraw: a 0x0 view called `pump` whose `tick` had been emptied when its
    work moved into `on_frame`, and `window:add` treats *having* a `tick` as
    "this changes on its own, like a clock". A repaint costs far too little
    to move the processor meter the idle phase reads, and it puts the same
    pixels back, so the screen cannot show it either.

    What it did show was a flicker. The window manager writes a window's
    surface on every batch and holds the damage to the last, so a frame is
    never composited half-drawn by its own damage - but the surface is live
    memory, and anything else damaging the screen mid-frame scans out a
    terminal that has been cleared and not yet re-texted. The Terminal sends
    the largest frame on the machine, so it is the widest opening there is.

    Measured rather than reasoned about, and the measurement was checked
    against the bug: with the dead hook back it reads about one frame a
    second, and without it, none.
    """
    guest.type("wm trace,terminal")
    started(guest)

    # The banner, and whatever the desktop does when it opens. Frames during
    # this are the point of the window rather than a fault in it.
    time.sleep(8)
    guest._read_available()
    mark = len(guest.seen)

    IDLE = 8
    time.sleep(IDLE)
    guest._read_available()

    drew = re.findall(r"wm: draw (\S+)", guest.seen[mark:])

    counts = {}

    for name in drew:
        counts[name] = counts.get(name, 0) + 1

    #
    # The clock is allowed one. The Deskbar redraws when the minute changes,
    # which is a real change and may fall inside the window this watches.
    # Anything repeating is not that.
    #
    busy = {name: n for name, n in counts.items() if n > 1}

    if busy:
        worst = ", ".join(f"{name} drew {n} times"
                          for name, n in sorted(busy.items(),
                                                key=lambda kv: -kv[1]))

        raise Failure(
            f"in {IDLE} seconds of an idle desktop, {worst}. A window that "
            "repaints when nothing has happened is sending its whole "
            "contents to the compositor for nothing - and because the "
            "surface is written batch by batch while only the damage waits, "
            "it is also the thing that makes a window flicker while it sits "
            "still. Look for a widget with a `tick` that does nothing: the "
            "kit cannot tell an empty hook from a full one."
        )

    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back.")

    return 1


def check_terminal(guest):
    """A program runs inside a window, and prints into it.

    `wm terminal`, click into it, type `hello`, and the program's output has
    to appear *in the window*. It also must not appear on the machine's
    console, which is checked by the graphical-mode phase separately.

    What this proves is the design rather than a feature. The terminal is a
    console server: it speaks the same `write` and `read` a `/dev/console`
    speaks, and hands itself to its children under that name. No program
    knows or can ask what is behind `/dev/console` - a name resolves to a
    capability and nothing has a global meaning - so a terminal is a
    process that answers three verbs and passes itself on.

    Checked by ink rather than by reading text: `hello` prints a paragraph,
    so a window that ran it has far more drawn in it than one that did not.
    """
    guest.type("wm terminal")
    started(guest)

    width, height, px = parse_ppm(guest.screendump())

    # The window is opened at 90,40 by terminal.lua. Count the pixels in it
    # that are neither its background nor its frame.
    x0, y0, w, h = 100, 100, 600, 500

    def ink(pixels):
        n = 0
        base = pixels[((y0 + h - 4) * width + x0 + 4) * 3:
                      ((y0 + h - 4) * width + x0 + 4) * 3 + 3]

        for y in range(y0, y0 + h, 2):
            for x in range(x0, x0 + w, 2):
                at = (y * width + x) * 3

                if pixels[at:at + 3] != base:
                    n += 1

        return n

    guest.mouse_to(*_to_tablet(300, 200, width, height))
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)
    guest.mouse_button(False)
    time.sleep(0.6)

    #
    # **`clear` first, and this check was wrong without it.**
    #
    # A terminal opens with the `neofetch` banner in it, so the box sampled
    # above starts full of banner. `before` was taken the moment the window
    # appeared - while the banner was still being painted - so ink in that
    # box went on climbing by hundreds whether or not anything else ran, and
    # the assertion below was satisfied by the banner arriving rather than
    # by `hello` printing. It passed for the wrong reason for as long as the
    # terminal was slow enough, and stopped the day it got fast.
    #
    # Emptying the window first is what makes the growth attributable: after
    # this, everything drawn in there was drawn by the program.
    #
    for ch in "clear\n":
        guest.proc.stdin.write(ch.encode())
        guest.proc.stdin.flush()
        time.sleep(0.08)

    time.sleep(1.5)

    width, height, px = parse_ppm(guest.screendump())
    before = ink(px)

    for ch in "hello\n":
        guest.proc.stdin.write(ch.encode())
        guest.proc.stdin.flush()
        time.sleep(0.08)

    settle(guest,
           lambda w_, h_, px_: True if ink(px_) > before + 200 else None,
           "typing `hello` into the terminal put nothing in its window. "
           "Either the program did not start, or its `write` is not "
           "reaching this terminal - which would mean the child got the "
           "machine's console instead of the one it was handed.",
           seconds=25)

    #
    # **And the grid is the window's size, not the size it opened at.**
    #
    # The view holding the characters was built with the width and height the
    # window opens with, and the kit only resizes a child that *said* it
    # follows an edge - the default is left and top, "something that sits
    # where it was put". So a resized Terminal kept the grid it started with
    # and showed the window's own colour around it, while `draw` went on
    # correctly dividing a width that never changed by the character cell.
    #
    # Measured by the grid rather than by the window frame, because the frame
    # is the window manager's and would move whether or not the application
    # noticed. What has to grow is the black.
    #
    width, height, px = parse_ppm(guest.screendump())
    before = _console_box(width, height, px)

    if before is None:
        raise Failure(
            "there is no console-coloured area on the screen, so the "
            "terminal either did not open or is not drawing its grid."
        )

    bx0, by0, bx1, by1 = before

    # The window's own bottom-right corner, which is the console's: it runs
    # from the header to the window's edges (`docs/apps.html`), so the grip
    # is a few pixels in from where the console colour ends.
    grip_x, grip_y = bx1 - 4, by1 - 4

    # Only into the room that exists. This screen is not large and the
    # terminal already fills most of it, so a drag past the edge would be a
    # test that fails on a smaller display for a reason that is not the bug.
    room_x = (width - 6) - (grip_x + 3)
    room_y = (height - 6) - (grip_y + 3)

    if room_x < 60 or room_y < 40:
        raise Failure(
            f"no room to resize the terminal: {room_x}x{room_y} left on a "
            f"{width}x{height} screen."
        )

    guest.mouse_to(*_to_tablet(grip_x, grip_y, width, height))
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)

    # In steps, the way a hand does it. One jump can outrun a drag that is
    # tracking the pointer rather than teleporting with it.
    for i in range(1, 9):
        guest.mouse_to(*_to_tablet(grip_x + room_x * i // 8,
                                   grip_y + room_y * i // 8,
                                   width, height))
        time.sleep(0.12)

    guest.mouse_button(False)

    def grew(w_, h_, px_):
        now = _console_box(w_, h_, px_)

        if now is None:
            return None

        wider = (now[2] - now[0]) - (bx1 - bx0)
        taller = (now[3] - now[1]) - (by1 - by0)

        return True if wider > 60 and taller > 40 else None

    settle(guest, grew,
           "the terminal's window was made bigger and its character grid "
           "stayed the size it opened at, which is a view that never said "
           "it follows the right and bottom edges. The window frame moves "
           "either way, so this looks like a border of window colour around "
           "a console that will not grow.",
           seconds=20)

    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back from the terminal.")

    return 2


def check_programs_by_file(guest):
    """A program run by its file in the Terminal, and one opened as Tracker
    opens it.

    **In the Terminal**: `cd /ramfs`, then `./term.lua` - a file made at the
    prompt before the desktop starts - and its output has to be drawn in the
    window, counted by ink as `check_terminal` counts `hello`'s.

    **As Tracker opens it**: Tracker asks `/lib/filetypes.lua` how to open a
    file and sends what it answers to the window manager - for a Lua program
    that is not an application, a Terminal with the program's path as its
    argument. `opener.lua` does exactly that, from inside the first Terminal.
    So the window manager has to say it launched a Terminal, and the kernel
    has to say `term` ended after that: the second window ran the file in
    place of its banner. Tracker's own double-click is not driven here; the
    decision it asks is `test_filetypes.lua`'s, and the rest of the way is
    this.
    """
    guest.type('fs.write("/ramfs/term.lua", '
               '[[for i = 1, 30 do print("term-" .. i) end]]) '
               'fs.write("/ramfs/opener.lua", '
               '[[local t = use("/lib/filetypes.lua") '
               'local how = t.how_to_open("/ramfs/term.lua", nil, '
               'fs.read("/ramfs/term.lua")) '
               'fs.send("/app/wm", { type = "launch", program = how.program, '
               'args = how.args })]]) '
               'print("programs-by" .. "-file")')
    guest.wait_for("programs-by-file",
                   "wrote the two programs the Terminal runs")

    guest.type("wm terminal")
    started(guest)

    width, height, px = parse_ppm(guest.screendump())
    x0, y0, w, h = 100, 100, 600, 500

    def ink(pixels):
        n = 0
        base = pixels[((y0 + h - 4) * width + x0 + 4) * 3:
                      ((y0 + h - 4) * width + x0 + 4) * 3 + 3]

        for y in range(y0, y0 + h, 2):
            for x in range(x0, x0 + w, 2):
                at = (y * width + x) * 3

                if pixels[at:at + 3] != base:
                    n += 1

        return n

    def typed(text):
        for ch in text:
            guest.proc.stdin.write(ch.encode())
            guest.proc.stdin.flush()
            time.sleep(0.08)

    guest.mouse_to(*_to_tablet(300, 200, width, height))
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)
    guest.mouse_button(False)
    time.sleep(0.6)

    typed("cd /ramfs\n")
    typed("clear\n")
    time.sleep(1.5)

    width, height, px = parse_ppm(guest.screendump())
    before = ink(px)

    typed("./term.lua\n")

    settle(guest,
           lambda w_, h_, px_: True if ink(px_) > before + 200 else None,
           "typing `./term.lua` into the Terminal in /ramfs put nothing in its "
           "window: a file is not being run from where the window is.",
           seconds=25)

    #
    # And the way Tracker opens it. The mark is taken after the first run, so
    # a `term` ending before it cannot stand in for the second.
    #
    time.sleep(1.0)
    guest._read_available()
    mark = len(guest.seen)

    typed("./opener.lua\n")

    deadline = time.monotonic() + 30
    launched = ended = False

    while time.monotonic() < deadline and not (launched and ended):
        guest._read_available()
        after = guest.seen[mark:]
        at = after.find("wm: launched terminal -> true")
        launched = at >= 0
        ended = launched and re.search(r"process \d+ \(term\) ended, code 0",
                                       after[at:]) is not None
        time.sleep(0.3)

    if not launched:
        raise Failure(
            "a Lua program opened the way Tracker opens one did not get a "
            "Terminal from the window manager:\n"
            + guest.seen[mark:][-1200:])

    if not ended:
        raise Failure(
            "the Terminal the window manager started for `/ramfs/term.lua` "
            "never ran it - no `term` ended after the launch:\n"
            + guest.seen[mark:][-1200:])

    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-W Q did not get the screen back from the "
                      "Terminals.")

    return 3


def _log_view_area(width, height, px, win):
    """Where Log View draws its rows, or None if it has no console.

    `win` is the window as the window manager logged it, `(x, y, w, h)`. The
    console box is the bounding rectangle of `theme.console` inside it, which
    is the inside of the view's one-pixel frame; the rows stop short of the
    scroll bar the kit draws down the right-hand side, 16 wide and 2 in.
    """
    x, y, w, h = win
    count = 0
    rows, columns = {}, {}

    for yy in range(max(0, y), min(height, y + h)):
        row = yy * width

        for xx in range(max(0, x), min(width, x + w)):
            at = (row + xx) * 3

            if (px[at], px[at + 1], px[at + 2]) == CONSOLE:
                count += 1
                rows[yy] = rows.get(yy, 0) + 1
                columns[xx] = columns.get(xx, 0) + 1

    #
    # **The rows and columns that are mostly console**, not every pixel that
    # happens to be its colour. This took the bounding box of all of them,
    # and the day Log View grew a menu bar (`roadmap.md` 5zc) one
    # anti-aliased pixel of the word "View" - black text on grey lands on
    # exactly `#0b0b0b` now and then - put the top of the "area" twenty
    # pixels above the view, inside the bar. The corner where the check
    # looks for "new lines below" then held the bar's grey, which is ink,
    # and a phase that was about following the log failed about a menu.
    #
    wanted = max(1, (min(width, x + w) - max(0, x)) // 4)
    solid_rows = sorted(r for r, n in rows.items() if n >= wanted)

    if not solid_rows:
        return None

    tall = max(1, len(solid_rows) // 4)
    solid_columns = sorted(c for c, n in columns.items() if n >= tall)

    if not solid_columns:
        return None

    y0, y1 = solid_rows[0], solid_rows[-1]
    x0, x1 = solid_columns[0], solid_columns[-1]

    # A quarter of the window at least. Black text anti-aliased onto a grey
    # window has pixels that land on exactly this colour, and the first run
    # against the old window drew a box round a handful of them and reported
    # "0 of 0 pixels" instead of saying the window was grey.
    if count < (w * h) // 4:
        return None

    return x0 + 2, y0 + 2, x1 - 20, y1 - 1


def _commonest(width, px, win):
    """The most common colour in a window, sampled every third pixel."""
    x, y, w, h = win
    seen = {}

    for yy in range(y, y + h, 3):
        for xx in range(x, x + w, 3):
            at = (yy * width + xx) * 3
            c = (px[at], px[at + 1], px[at + 2])
            seen[c] = seen.get(c, 0) + 1

    return max(seen, key=seen.get)


def _text_rows(width, px, area, right=None):
    """The rows of text in an area, as bands of pixel rows with ink in them.

    Each band is `[top, bottom, red, green]`, the last two saying whether any
    of it is Log View's fault colour or its stage colour - the console's
    `bad` and `good`, 0xda3633 and 0x3fb950, which this finds by channel
    rather than exactly so a TrueType face's softened edges still count.

    Ink is anything brighter than the console by a margin, so the yellow note
    counts as ink and as neither colour. `right` stops short of the area's
    right edge, which is how a check looks past that note.
    """
    ax0, ay0, ax1, ay1 = area
    stop = ax1 if right is None else right
    bands = []

    for y in range(ay0, ay1):
        row = y * width
        ink = red = green = False

        for x in range(ax0, stop):
            at = (row + x) * 3
            r, g, b = px[at], px[at + 1], px[at + 2]

            if r > 90 or g > 90 or b > 90:
                ink = True

                if r > 150 and g < 110 and b < 110:
                    red = True
                elif g > 140 and r < 120 and b < 130:
                    green = True

        if not ink:
            continue

        if bands and bands[-1][1] == y - 1:
            bands[-1][1] = y
            bands[-1][2] = bands[-1][2] or red
            bands[-1][3] = bands[-1][3] or green
        else:
            bands.append([y, y, red, green])

    return bands


def _settled_rows(guest, area, seconds=15):
    """Log View's rows once two looks a second apart agree.

    For after something that logs a line of its own - a click, which the
    window manager narrates - while the view is still to catch up with it.
    A second is twice the window's refresh, so two agreeing looks cannot both
    have come before a refresh that was on its way.
    """
    deadline = time.monotonic() + seconds
    last = None

    while time.monotonic() < deadline:
        width, height, px = parse_ppm(guest.screendump())
        rows = _text_rows(width, px, area)

        if rows == last:
            return rows

        last = rows
        time.sleep(1.0)

    raise Failure(f"Log View's rows were still changing {seconds}s after a "
                  "click.")


def check_log_view(guest):
    """Log View reads like the Terminal, and follows what is logged.

    Diego, on the ThinkPad: *the log viewer should look like the terminal, on
    a black background, scrolling automatically as new items are added; right
    now it has an overlapping title and the content is not readable because
    it is on a grey background.* Reproduced in QEMU before it was touched, and
    all three were real:

    - The rows were `ui.text`'s body colour, `text_dim`, on the window's
      panel grey: #808080 on #d8d8d8 in the BeOS palette.
    - `ui.text` and its heading label laid text out on the bitmap font's 8x16
      cell while the compositor drew it in the interface face. With a
      20-pixel TrueType face the heading ran into the first row and rows sat
      16 pixels apart. At the default font the two cells agree, so no
      harness screen could show it.
    - It never followed. It set `scroll` past the end and the widget clamped
      that against the height the *previous* draw measured - nothing, the
      first time - so it opened at the top of the log and stayed there.

    **So the conditions are the ThinkPad's, not the harness's.** This phase
    runs Classic, BeOS's palette, because in the dark one the window colour is
    already dark and the old window would pass the first check; and a
    TrueType face at 20 pixels, because at spleen's 16 the second bug cannot
    be seen. Both are put back afterwards.

    What is logged is under this phase's control. `/ramfs/logger.lua` opens a
    small window and prints when it is clicked: forty plain lines, then one
    the log colours as a fault; the second click, forty more and one it
    colours as a stage. Finding them by colour rather than by shape is what
    survives the kernel's stamp, which makes every row start with the same
    nine characters. It prints on the *release*, because the window manager
    logs every click itself and logs the release before it delivers it - so
    the harness's lines come after its own.

    Four checks:

    1. **The rows are on black**: most of the text area is `theme.console`
       and light text is drawn on it.
    2. **Nothing overlaps**: the rows are separate bands of ink, each
       shorter than the pitch between them, and that pitch is no smaller
       than the face. A heading drawn into the first row merges bands; rows
       laid out on a cell smaller than the face may not, which is why the
       pitch is checked as well.
    3. **It follows**: the fault line appears in the lower half of the view,
       under the plain lines printed before it, with nothing touched.
    4. **It holds while scrolled back, and follows again at the bottom**:
       one row up, the second batch arrives, the rows on screen stay as they
       were - the fault line among them - and a note says there are new
       lines; one row down and the stage line is in view.

    **One row, and it was three.** Three rows up, the held view was nothing
    but plain lines, and plain lines look the same whichever batch printed
    them - so a window that followed regardless showed forty identical rows
    and passed. One row up keeps the fault line on screen, and a window that
    jumps takes it away.
    """
    # The face's size, which check 2 holds the row pitch to.
    FACE_PX = 20

    program = (
        "local ui = use('/lib/ui.lua') "
        "local w = ui.window{ title = 'Logger', w = 240, h = 90, "
        "x = 900, y = 110 } "
        "if not w then return end "
        "local n = 0 "
        "local v = ui.view{ x = 0, y = 0, w = 240, h = 90 } "
        "function v:mouse(action) "
        "if action == 'release' then "
        "n = n + 1 "
        "for i = 1, 40 do print('filler') end "
        "print(n == 1 and 'error: deliberate' or '[2/2] harness') "
        "end "
        "return true "
        "end "
        "w:add(v) w:run()"
    )

    guest.type("fs.write('/ramfs/logger.lua', %r)" % program)
    #
    # **Classic, named rather than defaulted to.** This wrote no palette and
    # took whatever a machine nobody has set up wears - BeOS, which is what
    # it was written against - and the day the default became Plex (18.139)
    # its console was `#1c1c1e` where the check looks for `#0b0b0b`, and the
    # phase failed on both boards. A phase that depends on the default
    # breaks whenever the default is changed; Classic is BeOS's palette.
    #
    guest.type('fs.write("/home/.appearance", { palette = "classic", '
               'fonts = { '
               'ui = { font = "ibmplexmono", px = %d }, '
               'mono = { font = "ibmplexmono", px = %d } } }) '
               'print("log-view" .. "-ready")' % (FACE_PX, FACE_PX))
    guest.wait_for("log-view-ready",
                   "wrote the logger and chose BeOS with a TrueType face")

    mark = len(guest.seen)
    guest.type("wm logview,/ramfs/logger.lua")

    placed = {}
    deadline = time.monotonic() + 40

    while time.monotonic() < deadline and len(placed) < 2:
        for title, *geometry in re.findall(
                r"wm: window (Log|Logger) at (\d+),(\d+) (\d+)x(\d+)",
                guest.seen[mark:]):
            placed[title] = tuple(int(v) for v in geometry)

        time.sleep(0.3)

    if len(placed) < 2:
        raise Failure(
            f"Log View and the logger did not both open within 40s: "
            f"{sorted(placed)} did."
        )

    log, logger = placed["Log"], placed["Logger"]

    # A second for the first refresh and the window's first paint.
    time.sleep(3)

    width, height, px = parse_ppm(guest.screendump())

    def click(x, y):
        guest.mouse_to(*_to_tablet(x, y, width, height))
        time.sleep(0.3)
        guest.mouse_button(True)
        time.sleep(0.2)
        guest.mouse_button(False)
        time.sleep(0.4)

    # The pointer is drawn on the screen too, and a pointer resting over the
    # rows is a row of ink that moves when it does.
    def park():
        guest.mouse_to(*_to_tablet(width - 200, height - 200, width, height))
        time.sleep(0.4)

    park()
    failures = []

    # 1. On black.
    width, height, px = parse_ppm(guest.screendump())
    area = _log_view_area(width, height, px, log)

    if area is None:
        common = _commonest(width, px, log)
        failures.append(
            "Log View's rows are not on a dark ground: almost none of its "
            "window is the console colour, and the commonest colour there is "
            "#%02x%02x%02x. The other three checks find the rows inside that "
            "console, so they were not run." % common
        )
    else:
        ax0, ay0, ax1, ay1 = area
        dark = ink = 0

        for y in range(ay0, ay1, 2):
            for x in range(ax0, ax1, 2):
                at = (y * width + x) * 3
                c = (px[at], px[at + 1], px[at + 2])

                if c == CONSOLE:
                    dark += 1
                elif max(c) > 150:
                    ink += 1

        total = len(range(ay0, ay1, 2)) * len(range(ax0, ax1, 2))

        if dark < total // 2 or ink < 100:
            failures.append(
                f"Log View has a console box but is not text on it: {dark} of "
                f"{total} sampled pixels are the console colour and {ink} are "
                "light."
            )

    lx, ly, lw, lh = logger

    if area is not None:
        ax0, ay0, ax1, ay1 = area
        middle = (ay0 + ay1) // 2
        pitch = None

        # 3 first, because 2 is measured on the rows it puts on screen.
        click(lx + lw // 2, ly + lh // 2)
        park()

        def followed(w_, h_, px_):
            bands = _text_rows(w_, px_, area)
            reds = [i for i, b in enumerate(bands) if b[2]]

            if not reds or bands[reds[-1]][0] < middle:
                return None

            above = bands[max(0, reds[-1] - 3):reds[-1]]

            if len(above) < 3 or any(b[2] or b[3] for b in above):
                return None

            return bands

        try:
            bands = settle(
                guest, followed,
                "a line logged while Log View was open never came into the "
                "lower half of its view, under the plain lines printed before "
                "it. It is not following the log: a reader would have to "
                "scroll by hand to see what just happened.",
                seconds=20)
        except Failure as e:
            failures.append(str(e))
            bands = None

        # 2. Nothing overlaps, measured on those rows.
        if bands is not None:
            tops = [b[0] for b in bands]
            steps = sorted(b - a for a, b in zip(tops, tops[1:]))
            pitch = steps[len(steps) // 2] if steps else None

            inside = [b for b in bands if b[0] > ay0 and b[1] < ay1 - 1]
            tall = [b for b in inside
                    if pitch is None or b[1] - b[0] + 1 >= pitch]

            #
            # **Two conditions, because the first alone passed the bug.**
            # Rows 16 pixels apart in a 20-pixel face do not always touch:
            # Plex Mono's bracket is short enough to leave a pixel between
            # them, so a window laid out on the bitmap cell passed "no band
            # as tall as the pitch". A pitch smaller than the face is the
            # cause itself, seen whether or not these glyphs collide.
            #
            if len(bands) < 4 or pitch is None or pitch < FACE_PX or tall:
                failures.append(
                    f"Log View's rows overlap: {len(bands)} rows of text "
                    f"{pitch} pixels apart in a {FACE_PX}-pixel face, and "
                    f"{len(tall)} of them as tall as that. Text laid out on "
                    "one font's cell and drawn in another runs into the row "
                    "below - which is also what a heading over the first row "
                    "looks like."
                )

        # 4. Held while scrolled back, and following again at the bottom.
        if bands is not None and pitch:
            click(ax0 + 60, ay0 + 40)
            park()

            #
            # Where the fault line is *after* that click, not before it. The
            # window manager logs a click itself, so the click that gives the
            # view the focus also lifts every row by two as its lines arrive -
            # and a position taken before it made one row down look like one
            # row up.
            #
            try:
                reds = [b[0] for b in _settled_rows(guest, area) if b[2]]
            except Failure as e:
                failures.append(str(e))
                reds = []

            if not reds:
                failures.append(
                    "the fault line left Log View's rows when the view was "
                    "clicked, so there was nothing to scroll back over."
                )

            before = reds[-1] if reds else ay1

            guest.sendkey("up")

            def moved(w_, h_, px_):
                now = [b[0] for b in _text_rows(w_, px_, area) if b[2]]

                return (True if not now or now[-1] >= before + pitch // 2
                        else None)

            left = ax0 + (ax1 - ax0) * 55 // 100
            note = (ax0 + (ax1 - ax0) * 6 // 10, ay0, ax1, ay0 + pitch + 2)

            try:
                settle(guest, moved,
                       "the up arrow did not move Log View's rows.",
                       seconds=10)

                width, height, px = parse_ppm(guest.screendump())
                held = _text_rows(width, px, area, right=left)

                said = len(guest.seen)
                click(lx + lw // 2, ly + lh // 2)
                park()

                #
                # Until the logger has finished, which the serial line says,
                # and then for the note - separately. This waited for the
                # note alone, and a window that followed regardless never
                # shows one, so it failed on the note and never reached the
                # question of whether the rows had moved.
                #
                deadline = time.monotonic() + 15

                while "[2/2] harness" not in guest.seen[said:]:
                    if time.monotonic() > deadline:
                        raise Failure("the logger's second batch never "
                                      "reached the serial line.")
                    time.sleep(0.3)

                try:
                    width, height, px = settle(
                        guest,
                        lambda w_, h_, px_: (w_, h_, px_)
                        if _text_rows(w_, px_, note) else None,
                        "more was logged while Log View was scrolled back "
                        "and nothing in its corner said so.", seconds=10)
                except Failure as e:
                    failures.append(str(e))
                    width, height, px = parse_ppm(guest.screendump())

                now = _text_rows(width, px, area, right=left)

                if now != held or any(b[3] for b in _text_rows(width, px, area)):
                    failures.append(
                        "Log View jumped to the newest lines while it was "
                        "scrolled back: the rows on screen changed under a "
                        "reader who had left the bottom to read them."
                    )

                click(ax0 + 60, ay0 + 40)
                park()

                guest.sendkey("down")

                def following(w_, h_, px_):
                    bands_ = _text_rows(w_, px_, area)
                    greens = [b for b in bands_ if b[3]]

                    if not greens or greens[-1][0] < middle:
                        return None

                    return None if _text_rows(w_, px_, note) else True

                settle(guest, following,
                       "back at the bottom, Log View did not follow again: "
                       "the line logged while it was held never came into "
                       "view, or the note that there were new lines stayed.",
                       seconds=15)
            except Failure as e:
                failures.append(str(e))

        #
        # 5. **Larger text, from the `...` menu** (`/lib/textsize.lua`).
        # Diego, 22 September: "a way to increase font size in the menu of
        # the log viewer and terminal". The face here is 20, so a step up is
        # 24 and the rows have to stand that much apart afterwards. The menu
        # is opened by clicking the button and the item by where the window
        # manager says the menu went, rather than by arithmetic on padding.
        #
        # **The button moved with the look** (0.10.145): Log View's menu bar
        # became the header every other window has, so the press is at the
        # right-hand end of that header rather than at its left - the kit's
        # dots, 26 square and 10 in, centred in the header's 46 (0.10.149).
        # Measured from the window's own width, which `wm` reports, so a
        # window that opens at another size is still clicked in the right
        # place.
        #
        if pitch:
            opened = len(guest.seen)
            click(log[0] + log[2] - 23, log[1] + 22)

            where = None
            deadline = time.monotonic() + 10

            while time.monotonic() < deadline:
                guest._read_available()
                m = re.search(r"wm: menu of Log at (\d+),(\d+) (\d+)x(\d+)",
                              guest.seen[opened:])

                if m:
                    where = tuple(int(v) for v in m.groups())
                    break

                time.sleep(0.3)

            if where is None:
                failures.append("Log View's `...` menu did not open: "
                                "nothing in its header answered a click.")
            else:
                click(where[0] + 30, where[1] + 2 + LAYOUT_ROW // 2)

                def bigger(w_, h_, px_):
                    bands_ = _text_rows(w_, px_, area)
                    tops = [b[0] for b in bands_]
                    steps_ = sorted(b - a for a, b in zip(tops, tops[1:]))

                    if len(steps_) < 3:
                        return None

                    now_ = steps_[len(steps_) // 2]

                    return True if now_ >= FACE_PX + 4 else None

                try:
                    settle(guest, bigger,
                           "Larger text in Log View's View menu did not make "
                           "its rows taller: a %d-pixel face steps up to 24."
                           % FACE_PX, seconds=15)
                except Failure as e:
                    failures.append(str(e))

    stop = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[stop:]:
            break

        time.sleep(0.3)
    else:
        failures.append("Control-W Q did not get the screen back.")

    # The size chosen above is this phase's, not the next one's.
    guest.type('fs.write("/home/.logview", {}) print("log-view" .. "-done")')
    guest.wait_for("log-view-done", "put Log View's text size back")

    # The palette and faces every other phase was written against.
    guest.type(appearance() + ' '
               'print("log-view" .. "-restored")')
    guest.wait_for("log-view-restored", "put the dark palette back")

    if failures:
        raise Failure("\n  - ".join(["Log View:"] + failures))

    return 5


# The focus ring and the selection, 0x58a6ff. `ui.editor` draws a selected
# run on this ground for the reason the block caret uses it: a selection is
# a widened cursor, so it is the same colour by construction.
HIGHLIGHT = (0x58, 0xa6, 0xff)


def _colour_area(width, height, px, want):
    """How many pixels of the screen are this colour.

    Every other pixel both ways, which is four times cheaper and cannot miss
    anything the size of a meter bar.
    """
    total = 0

    for y in range(0, height, 2):
        row = y * width * 3

        for x in range(0, width, 2):
            at = row + x * 3

            if (px[at], px[at + 1], px[at + 2]) == want:
                total += 1

    return total


def _highlight_area(width, height, px):
    """How many pixels of the screen are selection.

    A count rather than a run, because a focus ring is also drawn in this
    colour and contributes two full-width rows to any longest-run measure.
    Its *area* is a one-pixel outline and is swamped by any real selection,
    which is what makes the count the honest measure of the two.
    """
    total = 0

    for y in range(0, height, 2):
        row = y * width * 3

        for x in range(0, width, 2):
            at = row + x * 3

            if (px[at], px[at + 1], px[at + 2]) == HIGHLIGHT:
                total += 1

    return total


#
# What a filled meter looks like, and it is two colours.
#
# `theme.good` below 80% and `theme.bad` above it - which is the whole
# reason both are here. Written against the green alone this found nothing
# and said the meter had not moved, because one spinner on one core settles
# at *100%* and the bar had already turned red. A test that knows only the
# calm colour fails exactly when the machine is busiest, which is the case
# it exists to check.
#
METER_CALM = (0x3f, 0xb9, 0x50)     # theme.good
METER_BUSY = (0xda, 0x36, 0x33)     # theme.bad


def _meter_area(width, height, px):
    return (_colour_area(width, height, px, METER_CALM)
            + _colour_area(width, height, px, METER_BUSY))


def check_cores(guest):
    """A meter that moves when the machine is given something to do.

    **`cores` is an instrument built before the thing it measures.** There
    is one processor; `docs/smp.md` is the plan for more and step one of it
    is done. What this checks is the whole path from the kernel's per-CPU
    counters to a bar on the screen - `struct percpu`, `sysinfo`'s `cpu[]`
    array, `sys.cpuload`, and the subtraction every meter in this system
    makes - because until that path works there is nothing to watch SMP
    arrive on.

    It found a bug the day it was written, which is the argument for it: the
    loop filling `sysinfo.cpu[]` was bounded by `info.cpus`, a field
    assigned seventy lines further down, so the array stayed as `memset`
    left it and every core read 0% with two spinners running. Nothing else
    in the suite would have noticed - the *total* was right, and the total
    is what four other programs read.

    A worker is `/bin/spin.lua`, which exists for exactly this and
    deliberately does not yield. So this is also a check on the priority
    bands: the window is DISPLAY and the spinner is NORMAL, and if the
    button stopped answering while a core was pinned the screenshot below
    would not change at all.
    """
    guest.type("wm cores")
    started(guest)
    time.sleep(2.0)

    width, height, px = parse_ppm(guest.screendump())
    before = _meter_area(width, height, px)

    # "Add a worker", by the keyboard: it is the first control in the window,
    # so it holds the focus when the window opens and Enter presses it.
    #
    # **It was a click at (188, 126)**, which worked while the buttons sat
    # at a fixed offset above the panel - and they were put there because a
    # test could not aim at them under it, where their y depended on how
    # many processors the machine has. Since 0.10.149 they are the header's
    # (`docs/apps.html`), placed from the right edge by the widths of their
    # words, which depend on the face. The keyboard aims at nothing, which
    # is the property the first move was after.
    guest.sendkey("ret")

    #
    # Waited *for*, not waited out.
    #
    # This was `sleep(5)` and one reading, which is a fixed count standing in
    # for a duration - the same shape as the twelve `thread_yield()` calls
    # that made a kernel test flake on both boards. It held for as long as
    # the bar was a solid fill: a spinner takes a second or two to move the
    # average, and a filled rectangle crosses a hundred sampled pixels the
    # moment it starts.
    #
    # A segmented bar does not. Six pixels lit and two dark is three
    # quarters of the pixels, the reading climbs a segment at a time, and at
    # five seconds it was at a tenth of the width - real, visible on screen,
    # and under the threshold. The check then reported that the meter had
    # not moved, which was false, and sent an afternoon after a bug that was
    # not there.
    #
    # So: read until it has moved, and give up on the clock rather than on a
    # count. Twenty seconds is far longer than it needs and costs nothing
    # when it passes on the second read.
    #
    after = before
    deadline = time.monotonic() + 20.0

    while time.monotonic() < deadline:
        time.sleep(1.0)
        width, height, px = parse_ppm(guest.screendump())
        after = _meter_area(width, height, px)

        if after > before + 100:
            break

    if after <= before + 100:
        raise Failure(
            f"the processor meter did not move when a worker was started: "
            f"{before} filled pixels before and {after} after. Either "
            "`sys.cpuload` is not reporting, or the click never reached the "
            "button - which on a machine with one core pinned by a spinner "
            "would mean the priority bands are not holding."
        )

    checks = 1

    #
    # And take it off again, which is a check and a tidy-up at once.
    #
    # **The tidy-up is not optional.** A worker is `spin 600` - ten minutes,
    # because a person looking at this window should not have it evaporate -
    # and it is *detached*, so Control-C to the window manager does not
    # touch it. Leaving one running poisons every phase after this one:
    # written without this, the next phase's window never appeared and the
    # harness blamed the window rather than the spinner still burning the
    # core behind it.
    #
    # It is also the better assertion. Watching the meter fall proves the
    # kill reached a process this program started, which is the other half
    # of what the buttons claim.
    #
    # "Take one off": the next control along.
    guest.sendkey("tab")
    time.sleep(0.4)
    guest.sendkey("ret")

    #
    # Waited for, on the way down as well as on the way up.
    #
    # This half was still `sleep(4)` and one reading after the other half
    # was fixed, and it failed on the very next run: 716 pixels with the
    # worker running and 716 four seconds after killing it. A meter that
    # climbs a segment at a time falls a segment at a time, and the average
    # it is showing has to decay before any of them go out.
    #
    # Worth writing down because the first fix looked complete. The reading
    # rises slowly *and* falls slowly - one property, two places - and
    # fixing the first without the second is the shape of bug that gets
    # called flakiness for a week.
    #
    ended = after
    deadline = time.monotonic() + 20.0

    while time.monotonic() < deadline:
        time.sleep(1.0)
        width, height, px = parse_ppm(guest.screendump())
        ended = _meter_area(width, height, px)

        if ended < after - 100:
            break

    if ended >= after - 100:
        raise Failure(
            f"the worker was not taken off: {after} filled pixels with it "
            f"running and {ended} after asking for it to stop. A spinner "
            "left behind here runs for ten minutes, and every phase after "
            "this one would share a core with it."
        )

    checks += 1

    #
    # **Two workers, taken off one at a time - and the count is the check.**
    #
    # The pair above passed for months while the button was broken, because
    # one add and one remove is the single sequence that cannot meet the
    # bug. A killed process keeps its row in `sys.processes()` until it is
    # reaped - name, id and all - and `cores` counted by name, taking
    # `ids[#ids]` as the one to end.
    #
    # With one worker that is the live one and it works. With two, the first
    # removal ends the *last* started, and its corpse stays at the end of
    # the list - so every removal after that aims at the same dead process.
    # `sys.kill` answers *true* for one of those, the kernel's "already
    # gone; nothing to do", so the button reports success for ever while the
    # remaining worker keeps its core.
    #
    # Measured on the machine with five workers: the first click took one
    # off, and clicks two through five all aimed at id 17, which had died on
    # click one. The count stuck at four and the meter never came down -
    # which is what a person reports as not being able to take workers off.
    #
    # Three seconds apart, not one. Two presses in quick succession on the
    # same control are a double-click to anything that looks for one, and
    # this needs two separate presses to be seen as two.
    #
    # **By the keyboard, as the pair above is**, since the buttons moved into
    # the header (0.10.149): these were clicks at 188,126 and 308,126, where
    # the buttons had been a row above the panel, and pressed the panel. The
    # focus is on Take one off after the pair; Tab wraps to Add a worker.
    guest.sendkey("tab")
    time.sleep(0.5)

    for _ in range(2):
        guest.sendkey("ret")
        time.sleep(3.0)

    two = ended
    deadline = time.monotonic() + 20.0

    while time.monotonic() < deadline:
        time.sleep(1.0)
        width, height, px = parse_ppm(guest.screendump())
        two = _meter_area(width, height, px)

        if two > ended + 100:
            break

    if two <= ended + 100:
        raise Failure(
            f"two workers did not start: {ended} filled pixels before and "
            f"{two} after. The first add/remove pair worked, so this is "
            "about the state they left behind rather than about the button."
        )

    # And Take one off, the next along.
    guest.sendkey("tab")
    time.sleep(0.5)

    for _ in range(2):
        guest.sendkey("ret")
        time.sleep(3.0)

    settled = two
    deadline = time.monotonic() + 40.0

    while time.monotonic() < deadline:
        time.sleep(1.0)
        width, height, px = parse_ppm(guest.screendump())
        settled = _meter_area(width, height, px)

        if settled < ended + 100:
            break

    if settled >= ended + 100:
        raise Failure(
            f"the second worker could not be taken off: {two} filled pixels "
            f"with two running, {settled} after asking for both to stop, "
            f"against {ended} with none. One came off and one did not - "
            "which is `cores` aiming its second kill at the corpse of the "
            "first, because an exited process keeps its row in "
            "`sys.processes()` and it was counting by name."
        )

    checks += 1

    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back from `cores`.")

    return checks


def check_clipboard(guest):
    """Text selected in one application, and pasted into another.

    Three things, and they are separable on purpose because each of them
    failed differently while it was being built.

    **A drag selects.** `ui.editor` holds an anchor and a cursor and nothing
    else - there is no shift key on this machine to extend with, because a
    key arrives here as a byte and shift-plus-arrow is the same four bytes
    as an arrow. So the pointer is what makes a range, the press is the
    anchor, and the drag is the cursor.

    **Control-W c and Control-W v carry it across a process.** The
    clipboard lives in the window manager, which is the one process both
    applications already talk to, and the keys are behind the prefix for
    the same reason the window-moving keys are: Control-C is not available
    here - it is what stops a program, in nine checks in this file - so the
    letters everyone knows have to live behind something.

    **A copy larger than a message shrinks to what fits, visibly.** A
    message is 2048 bytes, so selecting a four-kilobyte report and copying
    it cannot take all of it. What must not happen is taking part of it in
    silence, so the highlight comes back to exactly the run that left, and
    that is what the third measurement below is looking at: the selection
    getting *smaller* when you copy it.

    **And This Machine's report follows its window when it is made bigger**,
    checked here because this is the window already open: the window manager
    always put a grip on it, and the editor inside was pinned to its left
    and top alone, so what grew was a border of window colour.
    """
    #
    # **Where both windows are is read from the window manager, not assumed.**
    #
    # The clicks below used to be numbers: the report at 100,60, and the
    # gallery "about 460x350 near the top left". The report is always where
    # it asks to be. The gallery is not. It asks for 60,90, which is under
    # the report, and the window manager moves a window that would be more
    # than a third buried into a free quarter (`taken_at` in `wm.lua`). So
    # when `machine` gets its window first the gallery opens bottom right,
    # every click aimed at it lands in the report, and Control-W v pastes
    # into a read-only editor - "changed nothing", for a reason that has
    # nothing to do with the clipboard. Which of the two gets its window
    # first is a race, and it went the wrong way four runs in a row.
    #
    # So each click is an offset into the window it is meant for, taken from
    # the line the window manager prints when it places one, and checked to
    # be clear of the other window's frame.
    #
    mark = len(guest.seen)

    def placed(title, seconds=25):
        """Where `title`'s window was put: x, y, w, h of what it draws in."""
        pattern = re.compile(r"wm: window " + re.escape(title)
                             + r" at (\d+),(\d+) (\d+)x(\d+)")
        deadline = time.monotonic() + seconds

        while True:
            guest._read_available()
            found = pattern.search(guest.seen, mark)

            if found:
                return tuple(int(v) for v in found.groups())

            if time.monotonic() > deadline:
                raise Failure(f"the window manager never placed {title!r}:\n"
                              + guest.seen[mark:][-1500:])

            time.sleep(0.25)

    # A window's frame around what it draws in: `BORDER` on three sides and
    # `TAB_H` above, in `wm.lua`. The tab is counted as the whole width, which
    # is more than a tab covers and errs towards "not clear".
    def covers(win, x, y):
        wx, wy, ww, wh = win
        return (wx - FRAME <= x < wx + ww + FRAME
                and wy - 26 <= y < wy + wh + FRAME)

    def clear_point(win, other, offsets):
        """The first offset into `win` that is clear of `other`'s frame."""
        for dx, dy in offsets:
            x, y = win[0] + dx, win[1] + dy

            if not covers(other, x, y):
                return x, y

        raise Failure(f"no point tried on the window at {win} is clear of "
                      f"the one at {other}")

    guest.type("wm machine,gallery")
    started(guest)

    report = placed("This Machine")
    gallery = placed("gallery")
    time.sleep(2.0)

    width, height, px = parse_ppm(guest.screendump())
    quiet = _highlight_area(width, height, px)

    #
    # Raise the report first, and do not assume it is already on top.
    #
    # **Which of the two is in front is the same race.** `wm machine,gallery`
    # spawns them in that order and `started` waits for the *first* window,
    # so the one that finishes opening last is the one raised - and that is
    # whichever took longer to build itself, not whichever was named last.
    # When the gallery is last, the drag below lands on it and selects
    # nothing, which is what this said before this click existed.
    #
    # A point in the report and clear of the gallery, wherever that landed.
    # Not the title bar: the left end of a tab is the close box.
    #
    x, y = clear_point(report, gallery, [(500, 640), (300, 600), (600, 300)])
    guest.mouse_to(*_to_tablet(x, y, width, height))
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)
    guest.mouse_button(False)
    time.sleep(1.0)

    #
    # A drag inside the report, whose editor is eight pixels in, so this
    # starts a few characters into a line and ends four lines down. The
    # report is on top now, so the gallery cannot be in the way.
    #
    guest.mouse_to(*_to_tablet(report[0] + 30, report[1] + 90, width, height))
    time.sleep(0.3)
    guest.mouse_button(True)
    time.sleep(0.3)
    guest.mouse_to(*_to_tablet(report[0] + 460, report[1] + 154, width, height))
    time.sleep(0.5)
    guest.mouse_button(False)
    time.sleep(0.8)

    width, height, px = parse_ppm(guest.screendump())
    dragged = _highlight_area(width, height, px)

    if dragged <= quiet + 200:
        raise Failure(
            f"dragging across the report selected nothing: {quiet} "
            f"highlighted pixels before and {dragged} after. Either the "
            "press is not setting an anchor, or `ui.editor` is not drawing "
            "the run between the anchor and the cursor."
        )

    def send(data, wait=0.6):
        guest.proc.stdin.write(data)
        guest.proc.stdin.flush()
        time.sleep(wait)

    send(b"\x03", 1.0)                       # Control-C, copy

    #
    # The gallery, raised from its bottom label rather than its title bar,
    # because the left of a tab is the close box and clicking it here closes
    # the window this phase is about to use. A point the report does not
    # cover: when the gallery opened under the report, the strip of it left
    # of the report is all there is to click.
    #
    x, y = clear_point(gallery, report, [(15, 306), (15, 14), (440, 306)])
    guest.mouse_to(*_to_tablet(x, y, width, height))
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)
    guest.mouse_button(False)
    time.sleep(1.0)

    # Its text field, which is `ui.field` and takes a paste at the caret,
    # where the gallery says it is. The gallery is on top now, so nothing
    # covers it.
    field = gallery_layout(guest, mark)["field"]
    guest.mouse_to(*_to_tablet(gallery[0] + field[0], gallery[1] + field[1],
                               width, height))
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)
    guest.mouse_button(False)
    time.sleep(0.8)

    before = guest.screendump()

    send(b"\x16", 1.2)                       # Control-V, paste

    after = guest.screendump()

    if before == after:
        raise Failure(
            "Control-V changed nothing in the gallery's text field. The "
            "copy was made in a different application, so either the "
            "window manager is not holding the clipboard between the two "
            "or `ui.field` is not answering a paste."
        )

    #
    # And the cap. Back to the report, everything selected, then copied -
    # which is more than a message holds, so the selection has to come back
    # to the part that fits.
    #
    # Clear of the gallery, which is on top again and may have opened over
    # the part of the report this used to click.
    x, y = clear_point(report, gallery, [(300, 600), (500, 640), (600, 300)])
    guest.mouse_to(*_to_tablet(x, y, width, height))
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)
    guest.mouse_button(False)
    time.sleep(1.0)

    guest.mouse_to(*_to_tablet(report[0] + 200, report[1] + 340,
                               width, height))
    time.sleep(0.3)
    guest.mouse_button(True)
    time.sleep(0.2)
    guest.mouse_button(False)
    time.sleep(0.8)

    send(b"\x01", 1.2)                       # Control-A, select everything

    width, height, px = parse_ppm(guest.screendump())
    everything = _highlight_area(width, height, px)

    send(b"\x03", 1.5)                       # copy, which cannot take it all

    width, height, px = parse_ppm(guest.screendump())
    capped = _highlight_area(width, height, px)

    if capped >= everything:
        raise Failure(
            f"copying a selection bigger than a message left it the same "
            f"size on screen: {everything} highlighted pixels before the "
            f"copy and {capped} after. A clipboard holds "
            "less than 2048 bytes, so the highlight should have come back "
            "to the run that actually left - and if it did not, the copy "
            "was truncated without saying so."
        )

    #
    # **And the report follows its window.** The window manager has always
    # put a grip on this window, and the editor was built at the size the
    # window opened with and pinned to its left and top alone - so made
    # bigger, the window showed a border of its own colour round a report
    # the size it started. Diego, on the ThinkPad: "make sure is resizable
    # as well".
    #
    # Measured by the editor's own background, the commonest colour in a
    # band of it with nothing selected, against the window's colour in the
    # margin beside it: the two have to differ, or a report that did not grow
    # would read as one that did.
    #
    rx, ry, rw, rh = report

    #
    # **Room to grow, made when the window manager did not leave it.** Where
    # the report lands depends on what opened beside it: once the gallery
    # became a page of cards 658 tall, the report's own corner was taken and
    # it went to a slot at the bottom right, flush with the screen - and a
    # window with no room below cannot be made taller. So it is lifted by
    # its title bar first, as a person would, as far as there is room above.
    #
    below = (height - 6) - (ry + rh)

    if below < 80:
        lift = min(160 - below, ry - (STRIP_H + 26 + 10))

        if lift > 0:
            gx, gy = rx + rw // 2, ry - 13
            guest.mouse_to(*_to_tablet(gx, gy, width, height))
            time.sleep(0.3)
            guest.mouse_button(True)
            time.sleep(0.2)

            for step in range(1, 9):
                guest.mouse_to(*_to_tablet(gx, gy - lift * step // 8,
                                           width, height))
                time.sleep(0.1)

            guest.mouse_button(False)
            time.sleep(1.0)
            ry -= lift

    guest.mouse_to(*_to_tablet(rx + 200, ry + 200, width, height))
    time.sleep(0.3)
    guest.mouse_button(True)
    time.sleep(0.2)
    guest.mouse_button(False)
    time.sleep(0.8)

    width, height, px = parse_ppm(guest.screendump())
    paper = _commonest(width, px, (rx + 40, ry + rh - 200, rw - 120, 100))
    margin = _commonest(width, px, (rx + rw - 12, ry + rh // 3, 6, rh // 3))

    if paper == margin:
        raise Failure(
            f"This Machine's report and the window round it are both "
            f"{paper}, so whether the report grows with its window cannot be "
            "seen."
        )

    before = _colour_area(width, height, px, paper)
    grip_x, grip_y = rx + rw - 3, ry + rh - 3
    room_x = (width - 6) - (grip_x + 3)
    room_y = (height - 6) - (grip_y + 3)

    if room_x < 60 or room_y < 40:
        raise Failure(
            f"no room to make This Machine bigger: {room_x}x{room_y} left on "
            f"a {width}x{height} screen."
        )

    guest.mouse_to(*_to_tablet(grip_x, grip_y, width, height))
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)

    # In steps, the way the Terminal's is dragged, and for its reason.
    for i in range(1, 9):
        guest.mouse_to(*_to_tablet(grip_x + room_x * i // 8,
                                   grip_y + room_y * i // 8,
                                   width, height))
        time.sleep(0.12)

    guest.mouse_button(False)

    # What the editor gains if it follows both edges, on the quarter of the
    # pixels `_colour_area` counts - and half of that, so text drawn into the
    # new room cannot fail a report that grew.
    gained = (room_x * (rh - 80) + room_y * (rw - 40)) // 4 // 2

    def followed(w_, h_, px_):
        grown = _colour_area(w_, h_, px_, paper) - before

        return True if grown > gained else None

    settle(guest, followed,
           "This Machine's window was made bigger and its report stayed the "
           "size it opened at: an editor that never said it follows the right "
           "and bottom edges, in a window the manager resized.",
           seconds=20)

    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back after the "
                      "clipboard.")

    return 4


def check_deskbar(guest):
    """A desktop you can start things from.

    `wm` with nothing asked for starts a desktop: the Tracker that draws the
    backdrop, and the Deskbar top right, which lists every window on the
    desktop and every program that declared itself an application. Clicking
    one of those launches it.

    Checked by counting tabs rather than by reading the lists: every window
    has exactly one tab, as wide as its title, so the count is the number of
    windows and does not depend on knowing where anything was placed.

    **The backdrop is not one of them**, and that is why this count is still
    one. A backdrop is undecorated - `wm.lua` gives no tab to a menu, a
    backdrop or the strip - so the thing that draws the desktop does not
    appear in a census of windows any more than the desktop itself would.

    The Deskbar asks the window manager what is on screen rather than asking
    /app what registered. The difference is real: a program that opens a
    window by talking to the desktop directly - which the two oldest
    demonstrations here do - has a window and no registration, and would be
    missing from a list built the other way.
    """
    guest.type("wm")
    started(guest)

    #
    # The Deskbar is a *strip* now, so it has no tab and does not appear in
    # a census of windows at all.
    #
    # This used to read "expected the Deskbar and nothing else, and counted
    # 1" - the 1 being the Deskbar's own tab. When the Deskbar became the
    # bar across the top it became chrome, like the desktop, and `wm.lua`
    # gives chrome no tab. So the thing to assert is that the strip opened,
    # which the window manager says outright, and the count becomes a
    # baseline rather than a claim.
    #
    # The baseline is what the real check below is made of: after choosing
    # something from the menu there has to be *more* than there was.
    #
    deadline = time.monotonic() + 30

    while "wm: window Deskbar at 0,0 " not in guest.seen:
        if time.monotonic() > deadline:
            raise Failure(
                "a bare `wm` did not open the Deskbar across the top.\n"
                "--- what the guest said ---\n" + guest.seen[-1200:])

        time.sleep(0.3)
        guest._read_available()

    #
    # **The baseline is taken once the machine has stopped opening windows.**
    #
    # The Deskbar starts what `startup.lua` lists - four applications - and
    # they arrive over a second or so. Sampling the count straight after
    # `wm` and then waiting for it to *rise* meant the rise came from those,
    # not from the menu: the check passed without the menu being involved at
    # all, which is the worst kind of green.
    #
    # So: wait for two readings the same, and use that.
    #
    width, height, px = parse_ppm(guest.screendump())
    before = count_windows(width, height, px, STRIP_H)

    settled = time.monotonic() + 25

    while time.monotonic() < settled:
        time.sleep(1.5)
        width, height, px = parse_ppm(guest.screendump())
        again = count_windows(width, height, px, STRIP_H)

        if again == before:
            break

        before = again

    #
    # Through the menu, which is where the applications are now.
    #
    # The Deskbar used to carry every application in a list of its own and
    # this clicked a row in it. It carries a *menu* now - three sections,
    # the way BeOS's Be menu had three folders - so the path is: press the
    # button, slide onto Applications, slide onto the first item, release.
    #
    # That is three more steps and they are the point: this check exists to
    # prove an application can be started from the Deskbar by a person, and
    # a person now has to open a menu and walk a submenu. A check that still
    # clicked where the old list was would pass on a Deskbar nobody could
    # use.
    #
    # deskbar.lua: the strip at 0,0, `STRIP_H` tall, with the Kosmos button
    # at its left end - twelve in, a 24-pixel icon, eight, then the word. Its
    # menu opens under the bar, at the window's own origin.
    #
    # It used to be a window in the top-right corner and these numbers were
    # measured from there, which is why they had to change: the bar is the
    # whole width now and the middle of it is somebody's window button.
    #
    # Every item in the Deskbar's menus has a picture, and a menu with
    # pictures gives every row the picture's height, 32 and four, from two
    # pixels in.
    #
    row_h = max(LAYOUT_ROW, 32 + 4)
    menu_x, menu_y = 0, STRIP_H

    # And its width, because the submenu opens beside it: the widest name,
    # `Applications`, then the padding, the arrow and the picture - the sum
    # `menu_metrics` in ui.lua makes.
    top_w = len("Applications") * GLYPH_W + 8 * 2 + 12 + 12 + 32 + 6

    # Everything after this point follows a click on the bar.
    opened_at = len(guest.seen)

    guest.mouse_to(*_to_tablet(40, 18, width, height))
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)
    guest.mouse_button(False)
    time.sleep(1.2)

    # Applications is the first row; hovering it opens its submenu, which
    # the window manager forwards because a menu is open.
    guest.mouse_to(*_to_tablet(menu_x + 40, menu_y + 2 + row_h // 2,
                               width, height))
    time.sleep(1.4)

    # And the first item in that submenu, which sits beside the parent row.
    guest.mouse_to(*_to_tablet(menu_x + top_w + 30, menu_y + 2 + row_h // 2 + 4,
                               width, height))
    time.sleep(0.8)

    # From here on, any window the window manager opens is this choice's.
    chose = len(guest.seen)

    guest.mouse_button(True)
    time.sleep(0.3)
    guest.mouse_button(False)

    #
    # **Asked of the window manager rather than counted off the screen.**
    #
    # This counted title bars before and after, and the count is a pixel
    # heuristic: a title bar is a long horizontal run of tab colour, and a
    # run that overlaps one seen a few rows above is taken to be the same
    # window continuing. With five windows on a 1920x1080 desktop the new
    # one lands overlapping an old one often enough that the count does not
    # move, so the check failed while the launch it was testing worked
    # perfectly - which is the third time that heuristic has cost a day.
    #
    # The window manager says when it opens a window and what it is called.
    # That is the thing that decides it, so that is what to ask - the same
    # rule the rest of this file follows for everything that could look
    # right and be wrong.
    #
    deadline = time.monotonic() + 25
    opened = None

    while opened is None and time.monotonic() < deadline:
        guest._read_available()
        # A title has spaces in it - "About Kosmos" - so the name is
        # taken up to the position, not up to the first space.
        opened = re.search(r"wm: window (.+?) at \d+,\d+ ",
                           guest.seen[chose:])

        if opened is None:
            time.sleep(0.4)

    if opened is None:
        raise Failure(
            "choosing the first item of the Kosmos menu's Applications "
            "section opened no window. Either the menu did not open, or its "
            "submenu did not (the window manager forwards pointer movement "
            "only while a menu is open - see `pointer_pass`), or the item "
            "names a program that is not there.\n"
            "--- what the window manager said ---\n"
            + "\n".join(l.strip() for l in guest.seen[chose:].splitlines()
                         if "menu of" in l or "launch" in l
                         or "wm: window" in l)[-900:])

    after = before + 1

    #
    # **And clicking the bar must not resize the desktop.**
    #
    # `pointer_pass` raises whatever is under the pointer before it asks
    # what kind of window it is, so a click on the Deskbar raises the strip
    # itself. `raise` used to take the window out of `windows`, count the
    # strips in that incomplete list - finding none - and resize the
    # backdrop to the whole screen, then put it back and resize it again.
    #
    # Every click on the bar therefore reallocated and repainted an 8 MB
    # desktop surface twice. On a ThinkPad that is a visible flicker across
    # the screen; under QEMU it hid in the noise and the gate stayed green,
    # which is why this check exists at all.
    #
    # The window manager says the backdrop's size every time it sets it, so
    # counting those lines is the whole test: one at startup, and no more.
    #
    resized = len(re.findall(r"wm: the desktop is below the strip", 
                             guest.seen[opened_at:]))

    if resized > 0:
        raise Failure(
            f"clicking the Deskbar resized the desktop {resized} time(s). "
            "`raise` must count the strips over a *complete* window list - "
            "it raises the strip itself, so counting while the strip is "
            "removed finds none and gives the backdrop the whole screen.\n"
            + "\n".join(l.strip() for l in guest.seen[opened_at:].splitlines()
                         if "below the strip" in l)[-500:])

    if False:
        raise Failure(
            f"clicking an application in the Deskbar started nothing: "
            f"{before} window(s) before and {after} after. Either the launch "
            "request is not reaching the window manager, or the program it "
            "named is not marked `-- kosmos: application` and so is not in "
            "the list at all."
        )

    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back from the desktop.")

    return 3


def check_focus_shown(guest):
    """The Deskbar shows where the focus went, at once.

    Diego, on the ThinkPad: when he moved the focus to another application,
    its button on the Deskbar took "like half a second" to show as selected.
    The bar learned where the focus was by asking the window manager on its
    tick, which is once a second, so a focus that moved anywhere but on the
    bar reached it up to a second late. Measured here before anything
    changed: 290 to 1029 ms from the focus moving to the bar's next frame,
    about 600 on average. The window manager now posts the bar a `windows`
    event when its list changes (`tell_watchers` in `wm.lua`).

    **Timed on the guest's own counter**, by two notes the window manager
    writes under `trace`: `focus <title> at <us>` when the top of its stack
    changes, and `draw <title> at <us>` when a window finishes a frame. The
    difference is how long the bar took to finish a frame after the focus
    moved, on the one clock both happened on. A screendump takes most of a
    second here and could not tell 50 ms from 500.

    **The picture says what that frame showed.** After each change the
    button of the window with the focus has to be drawn pressed and the
    other not, so a frame that arrived quickly with the old focus in it does
    not pass.

    Three ways the focus moves, three rounds of each, and **every** sample
    has to land under the bound. The press on the bar's own button was never
    slow - it paints what it asked for - so it is here to stay that way; the
    tab and the key are what a one-second tick fails. The wait before each
    change is a little longer than the one before, so the samples land at
    different points of such a tick rather than all at one: against the old
    bar, before that wait was stepped, every Control-W Tab read about 280 ms
    and would have passed on its own.

    And one that is not about time. A window put away by its own minimise
    box stays at the top of the window manager's stack, so it is still
    reported `focused` - and the bar drew it pressed, as the window you are
    in. Its button must not be.
    """
    #
    # **The bound.** The old bar's frames came 1140 ms apart. The floor is the
    # bar's own paint, which is what a press on its button has always cost -
    # about 100 ms here, with `trace` printing every stage of every pass -
    # and every path costs that now. 400 leaves room for a busy host and is
    # still well inside the tick.
    #
    BOUND_MS = 400

    mark = len(guest.seen)
    guest.type("wm trace,deskbar,clock,calc")

    #
    # Where each window landed, and the order they opened in: the bar sorts
    # its buttons by handle, and handles are given out in that order.
    #
    placed, order, tab_wide = {}, [], {}
    deadline = time.monotonic() + 40

    while time.monotonic() < deadline:
        for m in re.finditer(r"wm: window (.+?) at (\d+),(\d+) (\d+)x(\d+)"
                             r"(?:, a tab (\d+) wide)?", guest.seen[mark:]):
            if m.group(1) not in placed:
                placed[m.group(1)] = tuple(int(v) for v in m.groups()[1:5])
                tab_wide[m.group(1)] = int(m.group(6) or 0)
                order.append(m.group(1))

        if {"Deskbar", "clock", "Calculator"} <= set(placed):
            break

        time.sleep(0.3)
    else:
        raise Failure(
            "`wm trace,deskbar,clock,calc` did not open the bar and both "
            f"windows; the window manager reported {order}.")

    apps = [t for t in order if t != "Deskbar"]

    while "wm: draw Deskbar at" not in guest.seen[mark:]:
        if time.monotonic() > deadline:
            raise Failure("the Deskbar never finished a frame.")
        time.sleep(0.3)

    time.sleep(2)
    width, height, _ = parse_ppm(guest.screendump())

    # wm.lua: BORDER is `FRAME` and TAB_H is 26, and the minimise box is the first
    # of the pair `boxes_x` puts 44 pixels in from the *tab's* right edge -
    # the whole frame's with a bar across it, the title's end with a BeOS
    # tab, whose width the window manager says as it places the window.
    def frame(title):
        x, y, w, h = placed[title]
        return x - FRAME, y - 26, w + 2 * FRAME, h + 26 + FRAME

    def tab_point(title):
        """A point on the tab that the other window does not cover."""
        fx, fy, fw, _ = frame(title)
        ox, oy, ow, oh = frame([t for t in apps if t != title][0])
        y = fy + 13

        # Clear of the three at the right, which take the last 72.
        for x in range(fx + 40, fx + fw - 80, 8):
            if not (ox <= x < ox + ow and oy <= y < oy + oh):
                return x, y

        raise Failure(f"no part of {title}'s tab is clear of the other "
                      "window, so it cannot be pressed.")

    # deskbar.lua: the Kosmos end is 12, an icon, 8, the word and 12; then a
    # button per window, `TASK_W` 190 wide with a `GAP` of 4.
    kosmos_w = 12 + DESKBAR_ICON + 8 + len("Kosmos") * GLYPH_W + 12

    def button_x(i):
        return kosmos_w + 4 + i * (190 + 4)

    # deskbar.lua's `lit`, and its ladder: a button is `FACE` per cent toward
    # white from the strip, and pressed is 20 per cent darker than that.
    def lit(c, k):
        if k >= 0:
            return tuple(v + ((255 - v) * k) // 100 for v in c)
        return tuple(v + (v * k) // 100 for v in c)

    face = lit(TAB, 24)
    pressed = lit(face, -20)

    def buttons_now():
        w, _, px = parse_ppm(guest.screendump())
        # Two in from the button's left edge: inside its fill, left of its
        # icon, and on a row the rounded corners do not reach.
        return [tuple(px[(18 * w + button_x(i) + 2) * 3:
                         (18 * w + button_x(i) + 2) * 3 + 3])
                for i in range(len(apps))]

    def click(x, y):
        guest.mouse_to(*_to_tablet(x, y, width, height))
        time.sleep(0.2)
        guest.mouse_button(True)
        guest.mouse_button(False)

    def shown(since, how):
        """Where the focus went, and how long the bar took to finish a frame
        after it did."""
        limit = time.monotonic() + 6

        while time.monotonic() < limit:
            said = guest.seen[since:]

            # The first move onto an application. A press on the bar raises
            # the strip before the bar asks for anything, and that is noted
            # as a move too.
            for f in re.finditer(r"wm: focus (.+?) at (\d+)us", said):
                if f.group(1) in apps:
                    d = re.search(r"wm: draw Deskbar at (\d+)us",
                                  said[f.end():])

                    if d:
                        return (f.group(1),
                                (int(d.group(1)) - int(f.group(2))) / 1000.0)
                    break

            time.sleep(0.05)

        raise Failure(
            f"after {how}, the window manager noted no move of the focus "
            "onto an application followed by a Deskbar frame, in 6 s.\n"
            + "\n".join(l.strip() for l in guest.seen[since:].splitlines()
                        if "wm: focus" in l or "draw Deskbar" in l)[-800:])

    samples = []
    current = None

    def change(how, act):
        nonlocal current

        #
        # **A different wait before each one, stepping through a second.**
        #
        # Everything between two changes - a screendump, the sleeps - takes
        # about the same time, so without this every sample landed at the
        # same point of the old bar's one-second tick: against the old bar
        # every Control-W Tab read about 280 ms and every tab press about a
        # second, and a small shift in the harness's timing could have put
        # them all under the bound together. Stepped by 170 ms, the samples
        # walk right round a tick that size instead of sitting at one point
        # of it.
        #
        time.sleep(0.2 + (len(samples) * 0.17) % 1.2)

        since = len(guest.seen)
        act()
        title, ms = shown(since, how)

        if title == current:
            raise Failure(f"{how} did not move the focus off {title}.")

        current = title
        samples.append((how, title, ms))

        want = [pressed if t == title else face for t in apps]
        got = buttons_now()

        if got != want:
            raise Failure(
                f"after {how} moved the focus to {title}, the Deskbar's "
                f"buttons for {apps} are {got}; pressed is {pressed} and a "
                f"button that is not is {face}. A frame that arrives soon "
                "after the move and shows the focus where it was is one "
                "painted from a list asked for before the move - which is "
                "what a bar that asks on a tick sends when the tick falls "
                "just before the press.")

        time.sleep(0.4)

    def other():
        return [t for t in apps if t != current][0]

    def next_window():
        guest.proc.stdin.write(b"\x17\t")          # Control-W, Tab
        guest.proc.stdin.flush()

    # The first window opened is never the last, so this always moves it.
    change("a press on a window's tab", lambda: click(*tab_point(apps[0])))

    for _ in range(3):
        change("a press on the other window's tab",
               lambda: click(*tab_point(other())))
        change("Control-W Tab", next_window)
        change("a press on the other window's Deskbar button",
               lambda: click(button_x(apps.index(other())) + 60, 18))

    report = ", ".join(f"{ms:.0f}" for _, _, ms in samples)
    print(f"deskbar focus: the bar finished a frame {report} ms after the "
          "focus moved")

    slow = [(how, title, ms) for how, title, ms in samples if ms > BOUND_MS]

    if slow:
        raise Failure(
            f"the Deskbar showed a focus change more than {BOUND_MS} ms after "
            "it happened: "
            + "; ".join(f"{how} to {title} took {ms:.0f} ms"
                        for how, title, ms in slow)
            + f". All of them: {report} ms. A bar that learns the focus by "
            "asking on its one-second tick reads like this - the window "
            "manager has to post it `windows` when the list changes "
            "(`tell_watchers`).")

    #
    # Minimised by its own box, and not drawn as the window you are in.
    #
    #
    # The minimise box, which is the *first* of the three at the right.
    # `boxes_x` in wm.lua ends the run `MARGIN` from the frame and each box
    # takes a `BOX_W` slot: three slots and the margin, 62 and 10 since
    # 0.10.149 (the margin was 4, and 66 was the sum).
    #
    since = len(guest.seen)
    fx, fy, fw, _ = frame(current)
    click(fx + (tab_wide.get(current) or fw) - 72 + 9, fy + 13)

    limit = time.monotonic() + 6

    while not re.search(r"wm: draw Deskbar at", guest.seen[since:]):
        if time.monotonic() > limit:
            raise Failure("minimising a window drew nothing on the Deskbar "
                          "in 6 s.")
        time.sleep(0.05)

    if buttons_now()[apps.index(current)] == pressed:
        raise Failure(
            f"{current}, minimised by its own box, is drawn pressed on the "
            "Deskbar - as the window you are in. It stays at the top of the "
            "window manager's stack and so is reported `focused`; the bar "
            "has to read that as selected only while it is not hidden.")

    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while PROMPT not in guest.seen[mark:]:
        if time.monotonic() > deadline:
            raise Failure("Control-C did not get the screen back from the "
                          "desktop.")
        time.sleep(0.3)

    return 3


def check_panel(guest):
    """The Open and Save window every application shares, in use: a filter
    that hides, one click that only selects, and a second that hands the
    application the whole path.

    **Arranged so the path says which of three things happened.** The folder
    holds a folder, `a.txt` and `b.sfc`, and the filter keeps `.sfc`. Folders
    come first and then names, so the list's second row is `a.txt` without
    the filter and `b.sfc` with it: the path the application is handed is
    the filter's evidence. One click on that row must hand over nothing - the
    window used to choose a file the moment it was clicked - and a second one
    must hand over `/home/picktest/b.sfc`, whole.

    Two seconds between the lone click and the pair, because a second click
    is anything within a second of the counter, and under emulation the
    counter runs ahead of the guest (`ui.md` 16.8c): too short a pause would
    make the lone click and the pair's first one a double, and the check that
    one click chooses nothing would pass without proving it.
    """
    guest.type(appearance())
    guest.type('fs.send("/home/picktest", { type = "mkdir" })')
    guest.type('fs.send("/home/picktest/sub", { type = "mkdir" })')
    guest.type('fs.write("/home/picktest/a.txt", "a")')
    guest.type('fs.write("/home/picktest/b.sfc", "b")')

    # No comments inside: fs.write puts it on one line. The marker is joined
    # by Lua so it never appears in the echo of the line that writes it.
    program = (
        "local panel = use('/lib/panel.lua') "
        "local w = panel.open{ title = 'Pick', start = '/home/picktest', "
        "x = 300, y = 200, "
        "filter = function(n) return n:match('%.sfc$') ~= nil end, "
        "on_choose = function(p) print('pick' .. 'ed ' .. p) end } "
        "if w then w:run() end"
    )
    guest.type("fs.write('/ramfs/pick.lua', %r)" % program)
    time.sleep(1.0)

    # A path alone: `wm` starts every comma-separated entry as a program, so
    # `wm pick,/ramfs/pick.lua` would try `/bin/pick.lua` first and say it
    # could not - which the triangle and resize phases do, harmlessly.
    mark = len(guest.seen)
    guest.type("wm /ramfs/pick.lua")

    placed, deadline = None, time.monotonic() + 40
    while placed is None and time.monotonic() < deadline:
        found = re.search(r"wm: window Pick at (\d+),(\d+) (\d+)x(\d+)",
                          guest.seen[mark:])
        if found:
            placed = tuple(int(v) for v in found.groups())
        time.sleep(0.3)

    if placed is None:
        raise Failure("the Open window never opened:\n"
                      + guest.seen[mark:][-900:])

    wx, wy = placed[0], placed[1]
    time.sleep(2.5)
    width, height, _ = parse_ppm(guest.screendump())

    # panel.lua: the list starts at x 12 + 190 + 6 and at y 34 plus a header
    # a face high and four; its rows are the fixed layout's, from 2 in. The
    # middle of row two.
    row_x, row_y = 208 + 60, 34 + 20 + 2 + LAYOUT_ROW + LAYOUT_ROW // 2

    def click():
        guest.mouse_to(*_to_tablet(wx + row_x, wy + row_y, width, height))
        time.sleep(0.05)
        guest.mouse_button(True)
        time.sleep(0.05)
        guest.mouse_button(False)

    click()
    time.sleep(2.0)
    guest._read_available()
    after_one = "picked " in guest.seen[mark:]

    click()
    time.sleep(0.1)
    click()
    time.sleep(2.0)
    guest._read_available()

    chose = re.search(r"picked (\S+)", guest.seen[mark:])

    back = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()
    time.sleep(2.0)

    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        guest._read_available()
        if PROMPT in guest.seen[back:]:
            break
        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back after the "
                      "panel phase.\n" + guest.seen[back:][-600:])

    if after_one:
        raise Failure(
            "one click on a file in the Open window chose it - one click "
            "selects, and a second click or Enter opens:\n"
            + guest.seen[mark:][-600:])

    if not chose:
        raise Failure(
            "a second click on a file in the Open window handed the "
            "application nothing:\n" + guest.seen[mark:][-600:])

    if chose.group(1) == "/home/picktest/a.txt":
        raise Failure(
            "the Open window's filter did not hide a.txt - the second row was "
            "a.txt, which the filter keeps out: " + chose.group(0))

    if chose.group(1) != "/home/picktest/b.sfc":
        raise Failure(
            "the Open window handed over the wrong path, wanted "
            "/home/picktest/b.sfc: " + chose.group(0))

    return 3


def check_places(guest):
    """Tracker's shortcut places: made by a drop, opened by a click, and taken
    out by a right-click - the three paths no picture can show running.

    `tools/test_places.lua` proves the rule, and a photograph on x86 proved
    the sidebar draws a place and finds its drive. Neither runs a drop, the
    name box, or a right-click, and that is where a handler goes wrong in a
    way nothing parses: the drop handler first called `focus_on`, declared
    as a local a thousand lines further down, which Lua binds as a global -
    and the first drop would have stopped Tracker. Caught by reading, which
    is exactly the kind of catch a test is for next time.

    **Checked on the files, not the pixels, wherever a file can say it**,
    because the look can change and `/home/Places` cannot lie. The harness
    is diskless, so `/home` starts empty every boot and nothing here can
    pass on a place a previous run left behind.

      - dropped and named, it is a place - found afterwards in the Trash with
        `kind = "place"` and the path it points at, which proves the drop
        and the name box wrote it and the right-click moved it rather than
        destroying it;
      - clicking it goes there: the list, which held one folder, is empty;
      - right-clicked, it is no longer in Places.
    """
    NAME = "PlaceProbe"
    ROW = LAYOUT_ROW                    # a tree's row, the fixed layout's

    guest.type(appearance())
    guest.type('fs.send("/home/placetest", { type = "mkdir" })')
    guest.type('fs.send("/home/placetest/%s", { type = "mkdir" })' % NAME)
    time.sleep(1.0)

    mark = len(guest.seen)
    guest.type("wm tracker:/home/placetest")

    placed, deadline = None, time.monotonic() + 60
    while placed is None and time.monotonic() < deadline:
        found = re.search(r"wm: window Tracker at (\d+),(\d+) (\d+)x(\d+)",
                          guest.seen[mark:])
        if found:
            placed = tuple(int(v) for v in found.groups())
        time.sleep(0.3)

    if placed is None:
        raise Failure("Tracker never opened on /home/placetest:\n"
                      + guest.seen[mark:][-900:])

    wx, wy = placed[0], placed[1]
    time.sleep(3.0)
    width, height, _ = parse_ppm(guest.screendump())

    def to(x, y):
        guest.mouse_to(*_to_tablet(wx + x, wy + y, width, height))

    #
    # The one folder is the list's first row; the sidebar is x 0 to 200,
    # its places from under its own head, and Tracker says where a new one
    # lands (below).
    #
    # **`CONTENT_Y` is Tracker's to say**, and it says it: `tracker: content
    # at N`. It was 86 until 23 September, when the menu bar, toolbar and
    # trail became one band, and 41 until the kit's header (0.10.149) - and
    # each time this phase held a number that the window had moved away
    # from.
    #
    said, deadline = None, time.monotonic() + 15

    while said is None and time.monotonic() < deadline:
        guest._read_available()
        said = re.search(r"tracker: content at (\d+)", guest.seen[mark:])
        time.sleep(0.25)

    if not said:
        raise Failure("Tracker did not say where its content starts:\n"
                      + guest.seen[mark:][-900:])

    content_y = int(said.group(1))
    # The list's heading is a row of the fixed layout with a hairline under
    # it (0.10.149), so the first file's middle is a row and a half down.
    first_row_y = content_y + LAYOUT_ROW + 1 + LAYOUT_ROW // 2

    to(260, first_row_y)
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)
    to(272, first_row_y + 12)
    time.sleep(0.4)
    to(100, 300)                        # the sidebar's empty lower part
    time.sleep(0.6)
    guest.mouse_button(False)
    time.sleep(1.5)

    guest.sendkey("ret")                # the offered name, as it is
    time.sleep(2.0)

    def differing(px, x0, y0, w, h):
        counts = {}

        for y in range(wy + y0, wy + y0 + h):
            for x in range(wx + x0, wx + x0 + w):
                o = (y * width + x) * 3
                c = (px[o], px[o + 1], px[o + 2])
                counts[c] = counts.get(c, 0) + 1

        return w * h - max(counts.values())

    _, _, before = parse_ppm(guest.screendump())
    held = differing(before, 236, first_row_y - 7, 180, 14)

    #
    # **Where the new place went, as Tracker says**: after the built-in
    # three and a hairline since 0.10.150 (`docs/tracker2.html`), where it
    # had been the fourth row of a tree.
    #
    placed_at = None
    deadline = time.monotonic() + 10

    while placed_at is None and time.monotonic() < deadline:
        guest._read_available()
        placed_at = re.search(r"tracker: place (\S+) at (\d+)",
                              guest.seen[mark:])
        time.sleep(0.25)

    if placed_at is None:
        raise Failure("Tracker never said where the new place went:\n"
                      + guest.seen[mark:][-900:])

    place_row_y = int(placed_at.group(2))

    to(60, place_row_y)                 # the new place: click it
    time.sleep(0.3)
    guest.mouse_button(True)
    time.sleep(0.1)
    guest.mouse_button(False)
    time.sleep(2.0)

    _, _, after = parse_ppm(guest.screendump())
    emptied = differing(after, 236, first_row_y - 7, 180, 14)

    to(60, place_row_y)                 # and take it out again
    time.sleep(0.3)
    guest.mouse_button(True, "right")
    time.sleep(0.1)
    guest.mouse_button(False, "right")
    time.sleep(2.0)

    back = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()
    time.sleep(2.0)

    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        guest._read_available()
        if PROMPT in guest.seen[back:]:
            break
        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back after the "
                      "places phase.\n" + guest.seen[back:][-600:])

    #
    # **The marker is joined by Lua, so it is not in what was typed.** The
    # serial line echoes the command, and the first version of this matched
    # its own echo - `placecheck " .. tostring(p` - and failed a Tracker it had
    # not heard from. `run_x86.py` prints `"drives" .. ": ids"` for exactly
    # this reason, and this is that.
    #
    asked = len(guest.seen)
    guest.type('local p = fs.getattr("/home/Places/%s") '
               'local t = fs.getattr("/home/Desktop/Trash/%s") or {} '
               'print("place" .. "check " .. tostring(p ~= nil) .. " " '
               '.. tostring(t.kind) .. " " .. tostring(t.path))'
               % (NAME, NAME))
    time.sleep(2.0)
    guest._read_available()

    said = re.search(r"placecheck (\S+) (\S+) (\S+)", guest.seen[asked:])

    if not said:
        raise Failure("the places phase could not read /home/Places back:\n"
                      + guest.seen[asked:][-600:])

    still_there, kind, path = said.groups()

    if kind != "place" and still_there != "true":
        raise Failure(
            "a folder dropped on the sidebar and named did not become a "
            "place - nothing in /home/Places and nothing in the Trash, so the "
            "drop or the name box never wrote it: " + said.group(0))

    if still_there == "true":
        raise Failure(
            "a right-click on a place did not take it out of Places - "
            "/home/Places/%s is still there: %s" % (NAME, said.group(0)))

    if kind != "place" or path != "/home/placetest/" + NAME:
        raise Failure(
            "the place in the Trash is not what was dropped - wanted a place "
            "pointing at /home/placetest/%s: %s" % (NAME, said.group(0)))

    if not (held > 15 and emptied < 5):
        raise Failure(
            "clicking the place did not open it - the list's first row had "
            "%d pixels of a row before the click and %d after, where an "
            "empty folder leaves none" % (held, emptied))

    return 3


def check_icon_sizes(guest):
    """**How big the icons are, chosen on the desktop and kept** (`roadmap.md`
    5za, `iconsize.lua`).

    Diego, 22 September 2026: "with the new icon sizes we should also be able
    to select icon size on desktop, tracker icon view and else", and of the
    sizes: "16,32,64 are the correct ones".

    A desktop has no menu bar, so the choice is a right press on the
    background - which is what a right press on a desktop's background has
    meant since there were two buttons. This drives that menu and then asks
    two different things whether it worked, because either alone would pass
    for the wrong reason: the screen, and the settings file.

    **The screen, measured by where the last icon's name ends.** The
    desktop's items stack down the first column, so the bottom of the last
    label is

        2 + (N - 1) * CELL_H + px + 6 + lines * GH,   CELL_H = px + 8 + 2 * GH

    and every term but `px` is the same before and after. Differentiate it:
    **choosing a size `d` points larger moves that row down by exactly
    `N * d`**, whatever the icons look like, however many lines the last name
    takes and whatever the font is. So the phase measures the lowest row of
    ink in that column, counts the items at the prompt afterwards, and holds
    the move to the point.

    That formula is the reason this is measured at the *bottom* rather than
    at an icon. Every icon-shaped test - is this box mostly ink, how wide is
    the picture - depends on what Haiku drew inside a transparent square, and
    on a desktop of stacked cells each icon's neighbours are a few pixels
    away. The last row of ink in a column is none of that.

    **Two desktops, and the second one is the point.** The size is chosen in
    the first, which is then quit; the second has to come up already large,
    which is the claim a settings file is for and the one a phase that never
    restarted anything could not make. `/home/.tracker` is read between them
    and has to say 64.

    **Put back to Medium in the second**, and the lowest row has to come back
    to where it started - the same claim in the other direction. Medium is
    the default and a default is kept as *nothing*, so the file has to end
    with no size for the desktop at all, and every other phase finds the
    desktop as it expects it.
    """
    checks = 0

    DESK = (0x1c, 0x25, 0x30)          # the dark palette's `desktop`
    strip = STRIP_H
    STEP = 64 - 32                     # Medium to Large, in points

    # The first column is read over the narrowest cell there is, 84 at 16 and
    # 32 (`CELL_W` in `tracker.lua`), so the band holds whichever size is in
    # force. A wider cell only puts more of the same icon and name inside it.
    COLUMN = 84

    # And the menu is pressed clear of the widest, 116 at 64.
    #
    # How wide a cell is, this does not try to measure. It would have to be
    # read off where an icon's ink begins and ends, and Haiku's 64 and 32 do
    # not fill their squares to the same fraction: the middle of the first
    # block moved 14 pixels where the cell moved 16. `iconsize.cell` holds
    # the arithmetic and `tools/test_iconsize.lua` holds it exactly.
    WIDEST = 116

    # The palette these colours were written against and the bitmap faces the
    # arithmetic above assumes - `GH` is 16 because the pinned face is - and
    # at no scale, whatever the phase before this one left behind.
    guest.type(appearance())
    time.sleep(2)

    # Nothing opened at login, so the first column of the desktop is the
    # desktop's own icons and nothing that landed on top of them.
    guest.type('fs.write("/home/.startup", { items = {} })')
    time.sleep(2)

    # And no size chosen by an earlier boot: this one starts at the default
    # and says so.
    guest.type('fs.write("/home/.tracker", {})')
    time.sleep(1)

    def lowest_ink(w, h, px):
        """The last row holding anything but desktop, in the first column."""
        for yy in range(h - 1, strip, -1):
            for xx in range(2, min(COLUMN - 2, w)):
                o = (yy * w + xx) * 3

                if tuple(px[o:o + 3]) != DESK:
                    return yy

        return None

    def column_at(want, what):
        """Wait for the first column's last row of ink to be `want`."""
        def look(w, h, px):
            low = lowest_ink(w, h, px)

            return (w, h, px) if low is not None and abs(low - want) <= 2 \
                else None

        return look, what

    def start_desktop():
        guest.type("wm desktop,deskbar")

        #
        # Most of the screen its own colour *and* a short first column.
        #
        # Both, because either alone passes on the picture that is already
        # there. The phases above leave the screen black with three coloured
        # stripes down it, and a black column reads as a column of icons that
        # reaches the bottom of the screen - which is how the first version of
        # this measured the boot picture and called it a desktop.
        #
        def drawn(w, h, px):
            seen = 0

            for yy in range(strip + 4, h - 4, 16):
                for xx in range(4, w - 4, 16):
                    o = (yy * w + xx) * 3

                    if tuple(px[o:o + 3]) == DESK:
                        seen += 1

            if seen <= (w // 16) * (h // 16) // 3:
                return None

            low = lowest_ink(w, h, px)

            return (w, h, px) if low and strip + 40 < low < strip + 600 \
                else None

        return settle(
            guest, drawn,
            "the desktop never drew its first column of icons. "
            "`wm desktop,deskbar` starts Tracker in backdrop mode, and if it "
            "died instead the lines above say why.")

    #
    # The first desktop: at the default, and made large.
    #
    width, height, px = start_desktop()
    before = lowest_ink(width, height, px)
    checks += 1


    #
    # Somewhere bare to press, with room for the menu under it and clear of
    # the right edge so the window manager does not pull the menu back on.
    #
    # Searched for rather than named, which the desktop phase learned the
    # hard way: a fixed point is a bet on what else is on the screen, and it
    # was what made that phase fail twice for a reason that had nothing to do
    # with what it was testing.
    #
    MENU_W, MENU_H = 200, 3 * MENU_ROW + 8

    def bare(x0, y0):
        # The menu's room, and the strip above the pointer the check below
        # wants still empty when the menu is up.
        for yy in list(range(y0 - 22, y0, 2)) + list(range(y0, y0 + MENU_H, 4)):
            for xx in range(x0, x0 + MENU_W, 4):
                o = (yy * width + xx) * 3

                if tuple(px[o:o + 3]) != DESK:
                    return False

        return True

    spot = next(((x, y)
                 for y in range(strip + 30, height - MENU_H - 8, 24)
                 for x in range(WIDEST + 8, width - MENU_W - 8, 48)
                 if bare(x, y)), None)

    if spot is None:
        raise Failure("there was no bare piece of desktop to press on, on a "
                      f"screen of {width}x{height}. Something is covering it, "
                      "and a right press has to land on the background for "
                      "the icon sizes to come up.")

    at_x, at_y = spot

    def press(button, x, y):
        guest.mouse_to(*_to_tablet(x, y, width, height))
        time.sleep(0.4)
        guest.mouse_button(True, button)
        time.sleep(0.3)
        guest.mouse_button(False, button)
        time.sleep(0.6)

    def choose(row):
        """Right-press the bare desktop, then release on a row of the menu.

        A menu window opens at the press, with two pixels of edge above its
        first row and `MENU_ROW` for each - `menu_metrics` in `ui.lua`.
        """
        press("right", at_x, at_y)
        press("left", at_x + 24,
              at_y + 2 + (row - 1) * MENU_ROW + MENU_ROW // 2)

    #
    # The menu itself first: a press on the background has to put something
    # over the desktop where there was nothing. Without this, a phase whose
    # menu never opened would fail several steps later saying the icons did
    # not change size - which is true and is not the reason.
    #
    guest.mouse_to(*_to_tablet(at_x, at_y, width, height))
    time.sleep(0.4)
    guest.mouse_button(True, "right")
    time.sleep(0.3)
    guest.mouse_button(False, "right")

    def counted(pixels, w, y0, y1):
        ink = 0

        for yy in range(y0, y1):
            for xx in range(at_x + 4, at_x + 60):
                o = (yy * w + xx) * 3

                if tuple(pixels[o:o + 3]) != DESK:
                    ink += 1

        return ink

    #
    # **Under the pointer, and not above it.** Both halves, because a menu
    # is placed on the *screen* from the window's origin and the desktop's
    # origin is the one thing on this machine that is not where the window
    # is: `fit_backdrop` moves it below the strip, and until 22 September it
    # said so with a `resize` and no `moved`. The menu came up 32 pixels
    # high, over the Deskbar, and a check that only counted ink below the
    # pointer saw the same menu and passed.
    #
    def menu_over(w, h, pixels):
        if counted(pixels, w, at_y + 4, at_y + 3 * MENU_ROW) <= 200:
            return None

        return (w, h, pixels) if counted(pixels, w, at_y - 20, at_y - 4) == 0 \
            else None

    settle(guest, menu_over,
           "a right press on the bare desktop opened no menu under the "
           f"pointer at {at_x},{at_y}. The desktop has no menu bar, so "
           "`rows:on_context` in `tracker.lua` is the only way to the icon "
           "sizes - and a menu that is there but above the pointer is a "
           "window that does not know where it is: see `fit_backdrop` in "
           "`wm.lua`, which has to post `moved` as well as `resize`.")
    checks += 1

    #
    # **Which row is marked, which is which size is in force.**
    #
    # The mark is a diamond in a column of its own on the left of every row -
    # `MENU_MARK` in `ui.lua`, 12 wide, the shape drawn from `MENU_PAD + 2`.
    # What is between it and the text is the menu's own background, and that
    # is what "ink" means here: the menu is not the desktop's colour, so the
    # test the rest of this phase uses would count every pixel of it.
    #
    def marked_rows(w, pixels, ox, oy, rows, skip=()):
        #
        # `skip` names the menu's separators. A groove is drawn from
        # `MENU_PAD` to the far side, which crosses the mark's column - so a
        # separator reads as a marked row, and saying which rows are
        # separators is clearer here than teaching this to tell a diamond
        # from a line eight pixels wide.
        #
        o = ((oy + 2 + MENU_ROW // 2) * w + ox + 20) * 3
        background = tuple(pixels[o:o + 3])
        out = []

        for row in range(1, rows + 1):
            if row in skip:
                continue

            top = oy + 2 + (row - 1) * MENU_ROW
            ink = 0

            for yy in range(top + 6, top + 18):
                for xx in range(ox + 10, ox + 18):
                    at = (yy * w + xx) * 3

                    if tuple(pixels[at:at + 3]) != background:
                        ink += 1

            if ink > 8:
                out.append(row)

        return out

    def only_marked(row):
        def look(w, h, pixels):
            return (w, h, pixels) \
                if marked_rows(w, pixels, at_x, at_y, 3) == [row] else None

        return look

    _, _, px = settle(
        guest, only_marked(2),
        "the desktop's menu should mark Medium icons, the second of its three "
        "rows, as the size in force - and marks "
        + str(marked_rows(width, px, at_x, at_y, 3) or "none")
        + ". A menu of choices that "
        "does not say which one you are looking at is a menu you have to "
        "guess at: `mark` in `ui.lua`.")
    checks += 1

    # And away again without choosing: a press outside a menu closes it.
    press("left", min(at_x + 400, width - 40), at_y + 200)

    choose(3)                          # Large icons

    def grew(w, h, pixels):
        low = lowest_ink(w, h, pixels)

        return (w, h, pixels) if low is not None and low > before + 8 else None

    _, _, px = settle(
        guest, grew,
        "Large icons was chosen from the desktop's menu and the first column "
        f"did not get taller - its last row of ink is still about {before}.")

    large = lowest_ink(width, height, px)
    checks += 1


    stop_desktop(guest)

    #
    # What it was measuring, and what was written down.
    #
    #
    # Waited for by a marker on the *next* line, not by the one being read.
    # `wait_for` returns the moment its text appears, and the rest of that
    # line may still be on its way - which is how this read "ICON-KEPT" with
    # nothing after it once and said the size had not been written down.
    #
    guest.type('local t = fs.read("/home/.tracker") or {} '
               'print("ICON" .. "-KEPT", #(fs.list("/home/Desktop") or {}), '
               't.desktop_icon_px, t.window_icon_px) print("ICON" .. "-READ-1")')
    guest.wait_for("ICON-READ-1", "the desktop's listing and the size it kept")

    said = [ln for ln in guest.seen.splitlines() if "ICON-KEPT" in ln][-1]
    fields = said.split()

    try:
        items = int(fields[1])
    except (IndexError, ValueError):
        raise Failure("could not count what is on the desktop:\n" + said)

    if items < 2:
        raise Failure(f"the desktop holds {items} things, and this phase needs "
                      "at least two to measure a grid. Tracker puts the Trash, "
                      "Drive and the cheat sheet there when they are missing.\n"
                      + said)

    want = before + items * STEP

    if abs(large - want) > 2:
        raise Failure(
            f"the desktop's {items} icons were made large and the last name in "
            f"the first column moved to row {large}, where it should be "
            f"{want}. It was at {before} at 32 points, and a size `d` larger "
            f"moves it down by exactly the number of icons times `d` - "
            f"{items} x {STEP} here - because every cell in the column grows "
            "by `d` and so does the last icon itself. See `CELL_H` in "
            "`tracker.lua`. One other thing would move this row: a wider cell "
            "fits more of a name on a line, so a last name that took two "
            "lines at 32 and takes one at 64 is a line fewer and this is what "
            "would notice.")

    checks += 1

    if fields[2:3] != ["64"]:
        raise Failure("the desktop's icon size was chosen and /home/.tracker "
                      "does not say 64, so it would not survive a restart. "
                      "`iconsize.lua` writes the key the place names:\n" + said)

    checks += 1

    #
    # The second desktop, which has to come up large without being told.
    #
    start_desktop()

    look, _ = column_at(want, "large again")
    settle(guest, look,
           "the desktop was started again after Large icons was chosen and "
           f"came up with its first column ending at some other row than "
           f"{want}. A size is kept per place in /home/.tracker and read when "
           "the place opens.")
    checks += 1

    #
    # And the mark has moved to Large, which is the other half of a menu
    # whose items are worked out when it opens: this menu belongs to a
    # desktop that started *after* the choice was made, so a list built when
    # the window was made would say the same thing either way, and a mark
    # that never moved would be a mark that means nothing.
    #
    guest.mouse_to(*_to_tablet(at_x, at_y, width, height))
    time.sleep(0.4)
    guest.mouse_button(True, "right")
    time.sleep(0.3)
    guest.mouse_button(False, "right")

    settle(guest, only_marked(3),
           "Large icons is in force and the desktop's menu does not mark its "
           "third row. `ui.menu_items` works a menu's items out when it "
           "opens, so that a mark says what is true now.")
    checks += 1

    press("left", at_x + 24, at_y + 2 + MENU_ROW + MENU_ROW // 2)  # Medium

    look, _ = column_at(before, "back where it was")
    settle(guest, look,
           "the icons were put back to Medium and the first column's last row "
           f"did not come back to {before}.")
    checks += 1

    stop_desktop(guest)

    guest.type('local t = fs.read("/home/.tracker") or {} '
               'print("ICON" .. "-DEFAULT", t.desktop_icon_px == nil) '
               'print("ICON" .. "-READ-2")')
    guest.wait_for("ICON-READ-2", "what Tracker kept for the default size")

    line = [ln for ln in guest.seen.splitlines() if "ICON-DEFAULT" in ln][-1]

    if "true" not in line:
        raise Failure("Medium was chosen again and /home/.tracker still holds "
                      "a size for the desktop. The default is kept as nothing, "
                      "so a place that never chose follows a default that "
                      "changes rather than freezing one.\n" + line)

    checks += 1

    #
    # **And a Tracker window's View menu**, which is the other place that
    # draws a grid of icons and the only one with a menu bar.
    #
    # A window opens as a list, so View marks its second row and offers no
    # sizes at all - a choice that would change nothing is worse than no
    # choice. Chosen as icons, the same menu has to come up marking its
    # first row *and* the three sizes below, with Medium marked among them.
    #
    # That is what `ui.menu_items` is for: a menu bar's items may be a
    # function, worked out when the menu opens. Built once with the window,
    # every row of this would say how things were when Tracker started.
    #
    mark = len(guest.seen)
    guest.type("wm tracker")

    pattern = re.compile(r"wm: window Tracker at (\d+),(\d+) (\d+)x(\d+)")
    deadline = time.monotonic() + 30
    where = None

    while time.monotonic() < deadline:
        guest._read_available()
        found = pattern.search(guest.seen, mark)

        if found:
            where = tuple(int(v) for v in found.groups())
            break

        time.sleep(0.25)

    if where is None:
        raise Failure("`wm tracker` never opened a window:\n"
                      + guest.seen[mark:][-1500:])

    #
    # **The View button in the header**, which is where it went when Tracker
    # lost its menu bar (`roadmap.md` 5zg, 23 September): back, forward and
    # the place on the left, and search, a new folder, View and the dots on
    # the right - icons since 0.10.149, in the kit's 46-pixel header.
    #
    # Where View is, Tracker says (`tracker: ... view at x,y`, its middle in
    # the window), and its menu opens under the header at the button's left.
    # This phase held the header's numbers written out until the header
    # moved twice in two days.
    #
    said = None
    deadline = time.monotonic() + 15

    while said is None and time.monotonic() < deadline:
        guest._read_available()
        said = re.search(r"tracker: content at \d+, view at (\d+),(\d+)",
                         guest.seen[mark:])
        time.sleep(0.25)

    if said is None:
        raise Failure("Tracker did not say where its View button is:\n"
                      + guest.seen[mark:][-1500:])

    win_x, win_y = where[0], where[1]
    view_x = win_x + int(said.group(1))
    view_y = win_y + int(said.group(2))
    menu_x, menu_y = view_x - 13, win_y + 46

    def view_menu(rows, want, skip):
        guest.mouse_to(*_to_tablet(view_x, view_y, width, height))
        time.sleep(0.4)
        guest.mouse_button(True)
        time.sleep(0.3)
        guest.mouse_button(False)

        def look(w, h, pixels):
            return (w, h, pixels) \
                if marked_rows(w, pixels, menu_x, menu_y, rows, skip) == want \
                else None

        return look

    #
    # Two marks, because a View menu holds two sets of choices: the layout
    # and the column it is sorted on. A window opens as a list sorted by
    # name, so rows 2 and 4, and no sizes at all below them.
    #
    settle(guest, view_menu(6, [2, 4], (3,)),
           "a Tracker window opens as a list sorted by name, and its View "
           "menu should mark both - the second and fourth of its six rows, "
           "with no icon sizes under them, since a list has no icons and a "
           "choice that changes nothing is worse than no choice. See "
           "`view_menu` in `tracker.lua`.")
    checks += 1

    # "as icons", the first row.
    guest.mouse_to(*_to_tablet(menu_x + 24, menu_y + 2 + MENU_ROW // 2,
                               width, height))
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)
    guest.mouse_button(False)
    time.sleep(0.8)

    settle(guest, view_menu(10, [1, 4, 9], (3, 7)),
           "a Tracker window was put into icon view, and its View menu should "
           "now mark that row and offer the three sizes below it with Medium "
           "marked - ten rows, with the first, fourth and ninth marked. "
           "`ui.menu_items` works a menu's items out when it opens, so a menu "
           "says what is true now rather than what was true when the window "
           "was made.")
    checks += 1

    stop_desktop(guest)

    return checks


def check_desktop(guest):
    """The desktop: below the strip, holding what it always holds, and an
    icon that stays where it is dragged.

    Each of these could look right and be wrong, so each is asked of the
    thing that decides it rather than read off the picture alone.

    **Below the strip.** `wm desktop,deskbar` starts both, in whichever
    order they arrive, and the window manager has to say the backdrop is at
    the strip's height - either when it opens, or when it is moved there
    because the strip came second. A desktop at 0,0 draws its first row of
    icons under the bar, which is the bug this exists for.

    The strip is the Deskbar itself now. It used to be `topbar`, a second
    bar with five hard-coded shortcuts; the Deskbar is that strip, so there
    is one piece of chrome across the top rather than two.

    **Dragged, it stays.** Drive is pressed where an empty desktop puts it,
    in the cell under the Trash's, and let go on bare desktop further down.
    The icon has to be drawn there, and the place has to be written to the
    file as `desktop_x` and `desktop_y` - read back at the prompt, which is
    the claim that survives a restart.

    **Thrown away.** Then Drive is dragged onto the Trash, which is how
    anything is thrown away from a desktop with no menu bar: the backdrop is
    never raised and the window manager gives keys to the top window, so a
    Delete key would never arrive. The Trash's picture has to change, and
    Drive has to be inside it afterwards with its place still on it.

    **What it always has.** The same listing says the Trash and the cheat
    sheet are in `/home/Desktop`, and Drive's attributes say it is a
    launcher for Tracker at `/`.
    """
    checks = 0
    mark = len(guest.seen)

    #
    # Nothing opened at login, so what is on the screen is the desktop.
    #
    # This used to start `topbar`, which opened nothing of its own. The
    # Deskbar is the strip now, and the Deskbar starts what `startup.lua`
    # lists - four applications, which cover enough of a 1920x1080 screen
    # that "most of what I sample is desktop colour" stops being true.
    #
    # An empty list rather than a smaller sampling threshold: the phase is
    # about the desktop being *drawn below the strip*, and a threshold tuned
    # around four windows would pass on a desktop that was not there at all
    # the moment somebody changed what opens at login. `startup.lua` treats
    # an absent file and an empty list as different things on purpose, and
    # this is the empty one.
    #
    guest.type('fs.write("/home/.startup", { items = {} })')
    time.sleep(2)

    guest.type("wm desktop,deskbar")

    def said_any(texts, seconds=40):
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            guest._read_available()

            if any(t in guest.seen[mark:] for t in texts):
                return True

            time.sleep(0.3)

        return False

    # The Deskbar's own height, fixed (`STRIP_H`).
    strip = STRIP_H
    below = (f"wm: window Tracker at 0,{strip} ",
             f"wm: the desktop is below the strip, at 0,{strip} ")

    if not said_any(["wm: window Deskbar at 0,0 "]):
        raise Failure("`wm desktop,deskbar` never opened the strip.\n"
                      + guest.seen[mark:][-1200:])

    if not said_any(below):
        placed = [line for line in guest.seen[mark:].splitlines()
                  if "wm: window Tracker" in line or "below the strip" in line]
        raise Failure(
            f"the desktop is not below the {strip}-pixel strip. The window "
            "manager places the backdrop at the height the strip claimed, or "
            "moves it there when the strip opens second - and said:\n"
            + "\n".join(placed))

    checks += 1

    DESK = (0x1c, 0x25, 0x30)          # the dark palette's `desktop`
    CELL_W, CELL_H = 84, 56 + GLYPH_H  # tracker.lua's cell

    #
    # And on the screen, before anything is looked for on it.
    #
    # Everything below tests pixels *against* the desktop's colour, so a
    # screen with no desktop on it passes those tests for the wrong reason
    # and fails several steps later saying something unrelated - which is
    # what a negative control did say, about a sabotage that had nothing to
    # do with it.
    #
    def desktop_drawn(w, h, px):
        seen = 0

        for yy in range(strip + 4, h - 4, 16):
            for xx in range(4, w - 4, 16):
                o = (yy * w + xx) * 3

                if tuple(px[o:o + 3]) == DESK:
                    seen += 1

        return (w, h, px) if seen > (w // 16) * (h // 16) // 3 else None

    settle(guest, desktop_drawn,
           "the desktop never drew. `wm desktop,deskbar` starts Tracker in "
           "backdrop mode through the window manager, and if Tracker died "
           "instead the lines above say why.")

    #
    # And what the compositor paints *under* the desktop is still painted.
    #
    # The wallpaper, the flat colour and the version stamp in the
    # bottom-right corner are one layer, drawn only where no window covers.
    # A desktop window that counted as opaque took the whole screen out of
    # that, so a wallpaper was never drawn at all - and the stamp is the part
    # of that layer a harness can check without a picture on the disk. It has
    # to be legible through the desktop.
    #
    def stamp_shows(w, h, px):
        ink = 0

        for yy in range(h - 26, h - 4):
            for xx in range(w - 600, w - 10):
                o = (yy * w + xx) * 3

                if tuple(px[o:o + 3]) != DESK:
                    ink += 1

        return (w, h, px) if ink > 60 else None

    settle(guest, stamp_shows,
           "the window manager's stamp in the bottom-right corner is not "
           "visible through the desktop, so neither would a wallpaper be. "
           "The desktop is transparent between its icons and must not "
           "occlude the layer under it - see `compose_rect`.")
    checks += 1

    #
    # **And the bottom of the screen is the compositor's**, which is a
    # different claim from anything above it: every check so far looks at the
    # top-left, where the icons are, and would pass on a machine painting
    # three quarters of its display.
    #
    # That is what a 3440x1440 screen did on the ThinkCentre M700. A
    # process's window for the framebuffer was sixteen megabytes - a gap
    # between two addresses that nothing checked - and 3440x1440 is 18.9, so
    # the mapping ran into the window above it and the compositor's first
    # surface was mapped over the bottom of the screen. The desktop drew its
    # top and the kernel's console showed through the rest.
    #
    # Mostly, rather than every pixel: the version stamp is down there and is
    # meant to be.
    #
    def bottom_is_desktop(w, h, px):
        seen, looked = 0, 0

        for yy in range(h - 48, h - 2):
            for xx in range(4, w - 4, 8):
                o = (yy * w + xx) * 3
                looked += 1

                if tuple(px[o:o + 3]) == DESK:
                    seen += 1

        return (w, h, px) if looked > 0 and seen > looked * 3 // 4 else None

    settle(guest, bottom_is_desktop,
           "the last rows of the screen are not the desktop's colour, so the "
           "compositor is painting some of the display and not all of it. "
           "The framebuffer is mapped into the process holding the screen at "
           "`USER_SCREEN_VA` and bounded by `USER_SCREEN_MAX`; a display "
           "larger than that window used to be mapped past it, into the "
           "addresses the next surface is given.")
    checks += 1

    def drawn(x, y):
        """An icon's 32 pixels at x, y are not all desktop."""
        def look(w, h, px):
            n = 0

            for yy in range(y, y + 32, 2):
                for xx in range(x, x + 32, 2):
                    o = (yy * w + xx) * 3

                    if tuple(px[o:o + 3]) != DESK:
                        n += 1

            return (w, h, px) if n > 40 else None

        return look

    # Directories first, then by name: the Trash takes the first cell, and
    # Drive - which sorts before cheatsheet.html - the one under it.
    left = 2 + (CELL_W - 4 - 32) // 2
    trash_y = strip + 2 + 2
    cell_y = 2 + CELL_H
    width, height, px = settle(
        guest, drawn(left, strip + cell_y + 2),
        "Drive never appeared in the second cell under the strip. Either the "
        "desktop did not put the Trash and Drive in /home/Desktop, or it is "
        "not drawing from the desktop's own top-left corner.")

    def bare(x0, y0, size=96):
        for yy in range(y0, y0 + size, 8):
            for xx in range(x0, x0 + size, 8):
                o = (yy * width + xx) * 3

                if tuple(px[o:o + 3]) != DESK:
                    return False

        return True

    #
    # Somewhere to let go: any 96-pixel square of bare desktop, looked for
    # from the bottom upward. The application the Deskbar phase started is
    # placed by the window manager and lands somewhere different from run to
    # run, and a search over one band of the screen made *that* the reason
    # this phase failed - twice, in a negative control that was supposed to
    # be failing about something else entirely.
    #
    spot = next(((x, y) for y in range(height - 140, strip + CELL_H, -32)
                 for x in range(2, width - 120, 32) if bare(x, y)), None)

    if spot is None:
        raise Failure("there was no bare 96-pixel square of desktop to let an "
                      f"icon go on, on a screen of {width}x{height}.")

    press_x, press_y = left + 16, strip + cell_y + 2 + 16
    drop_x, drop_y = spot[0] + 48, spot[1] + 48
    trash_was = _strip(px, width, 2, trash_y, CELL_W - 4, 40)

    guest.mouse_to(*_to_tablet(press_x, press_y, width, height))
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)
    guest.mouse_to(*_to_tablet(press_x + 12, press_y + 12, width, height))
    time.sleep(0.4)
    guest.mouse_to(*_to_tablet(drop_x, drop_y, width, height))
    time.sleep(0.6)
    guest.mouse_button(False)

    # Where the cell moved to, in the desktop's own coordinates and on the
    # screen: by exactly how far the pointer went.
    want_x = 2 + (drop_x - press_x)
    want_y = cell_y + (drop_y - press_y)

    settle(guest, drawn(want_x + (CELL_W - 4 - 32) // 2, strip + want_y + 2),
           f"Drive was let go at {drop_x},{drop_y} and is not drawn there. "
           "A drop from the desktop onto the desktop moves the icon.")
    checks += 1

    #
    # And onto the Trash, which is how a thing is thrown away from here.
    #
    guest.mouse_to(*_to_tablet(want_x + (CELL_W - 4 - 32) // 2 + 16,
                               strip + want_y + 18, width, height))
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)
    guest.mouse_to(*_to_tablet(want_x + 40, strip + want_y + 34,
                               width, height))
    time.sleep(0.4)
    guest.mouse_to(*_to_tablet(left + 16, trash_y + 16, width, height))
    time.sleep(0.6)
    guest.mouse_button(False)

    def trash_changed(w, h, pixels):
        now = _strip(pixels, w, 2, trash_y, CELL_W - 4, 40)

        return (w, h, pixels) if now != trash_was else None

    settle(guest, trash_changed,
           "the Trash's picture did not change when Drive was dragged onto "
           "it. `files.icon` draws Trash_Full while anything is in it.")
    checks += 1

    back = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[back:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back from the desktop.")

    guest.type('local a = fs.getattr("/home/Desktop/Trash/Drive") or {} '
               'print("DESK" .. "-AT", a.desktop_x, a.desktop_y, a.kind, '
               'a.program, a.args) '
               'print("DESK" .. "-HAS", table.concat(fs.list("/home/Desktop") '
               'or {}, ",")) '
               'print("DESK" .. "-TRASH", '
               'table.concat(fs.list("/home/Desktop/Trash") or {}, ","))')
    guest.wait_for("DESK-TRASH", "the desktop and Trash listings")

    at = [line for line in guest.seen.splitlines() if "DESK-AT" in line][-1]
    has = [line for line in guest.seen.splitlines() if "DESK-HAS" in line][-1]
    trash = [line for line in guest.seen.splitlines()
             if "DESK-TRASH" in line][-1]
    fields = at.split()

    if "Drive" not in trash:
        raise Failure("Drive was dragged onto the Trash and is not in it.\n"
                      + trash + "\n" + has)

    checks += 1

    try:
        got_x, got_y = int(float(fields[1])), int(float(fields[2]))
    except (IndexError, ValueError):
        raise Failure("Drive was dragged and nothing wrote where it went: "
                      "`desktop_x` and `desktop_y` should be attributes of "
                      f"the file.\n{at}")

    if abs(got_x - want_x) > 3 or abs(got_y - want_y) > 3:
        raise Failure(f"Drive's place was written as {got_x},{got_y} and it "
                      f"was dropped at {want_x},{want_y}.\n{at}")

    checks += 1

    #
    # The whole path, not the short name.
    #
    # `handlers.launch` accepts either and completes a bare `tracker` to
    # `/bin/tracker.lua`, which is right for somebody typing. What a *file*
    # records should say what it runs without the reader knowing that rule,
    # so everything that writes a launcher writes the path - and this check
    # is where that decision is held to.
    #
    if fields[3:6] != ["launcher", "/bin/tracker.lua", "/"]:
        raise Failure("Drive should be a launcher for Tracker at /: "
                      "kind=launcher, program=/bin/tracker.lua, args=/. A "
                      "launcher records the whole path rather than a short "
                      "name the window manager would complete.\n" + at)

    if "Trash" not in has or "cheatsheet.html" not in has:
        raise Failure("the desktop folder should always hold the Trash and "
                      "the cheat sheet, and holds:\n" + has)

    checks += 1

    return checks


def check_clicks(guest):
    """The widgets, driven with the pointer.

    The gallery opens at a known place, so its controls are at known places.
    This clicks a button, then a list row, and checks each by what changed on
    screen rather than by anything the program said.

    The third check is the one worth having. A button fires on the *release*
    and only if the pointer is still on it, so pressing one and sliding away
    before letting go does nothing - which every graphical system since the
    Macintosh has allowed and which a button that fires on the press takes
    away. It is also the reason the window manager forwards movement while a
    button is held, and the only part of this that a naive implementation
    gets wrong while looking perfectly correct.
    """
    mark = len(guest.seen)
    guest.type("wm gallery")
    started(guest)
    laid = gallery_layout(guest, mark)

    width, height, px = parse_ppm(guest.screendump())

    #
    # The window is opened at x=60, y=90 by gallery.lua, which says where
    # its controls are. What a control did is said beside the header's
    # title, so that is the strip that changes: from where the subject's
    # words start to where the dots are.
    #
    wx, wy = 60, 90
    gw, gh = laid["size"]
    lx, ly, lw, lh = laid["list"]
    press, verb = laid["press"], laid["verb"]
    status = (wx + 90, wy + 10, gw - 90 - 50, 26)
    park = (wx + gw - 40, wy + gh - 12)  # the page's foot, with nothing on it

    def click(x, y, release_at=None):
        guest.mouse_to(*_to_tablet(x, y, width, height))
        time.sleep(0.35)
        guest.mouse_button(True)
        time.sleep(0.35)

        if release_at is not None:
            guest.mouse_to(*_to_tablet(release_at[0], release_at[1],
                                       width, height))
            time.sleep(0.35)

        guest.mouse_button(False)
        time.sleep(0.6)

    def screen_now():
        guest.mouse_to(*_to_tablet(park[0], park[1], width, height))
        time.sleep(0.6)
        return parse_ppm(guest.screendump())[2]

    before = _strip(screen_now(), width, *status)

    # 1. The second button, which sets a different message.
    click(wx + verb[0], wy + verb[1])

    settle(guest,
           lambda w, h, px: True if _strip(px, w, *status) != before else None,
           "clicking a button changed nothing on screen. Either the window "
           "manager is not forwarding the press, or the kit is not routing "
           "it to the view under it.")

    # 2. A list row, checked by where the selection bar lands.
    _, _, px = parse_ppm(guest.screendump())
    box = (wx + lx, wy + ly, lw, lh)
    bar_before = find_colour_in(width, px, box, SELECTED)

    click(wx + lx + 60, wy + ly + 2 + 2 * LAYOUT_ROW + LAYOUT_ROW // 2)

    settle(guest,
           lambda w, h, px: (lambda at: at if at is not None
                             and at[1] > bar_before[1] else None)(
                                 find_colour_in(w, px, box, SELECTED)),
           f"clicking the third row of the list did not move the selection "
           f"from {bar_before}.")

    # 3. Pressed, slid off, released: nothing may happen.
    settled = _strip(screen_now(), width, *status)

    click(wx + press[0], wy + press[1],
          release_at=(wx + lx + 60, wy + press[1]))
    escaped = _strip(screen_now(), width, *status)

    if escaped != settled:
        raise Failure(
            "a button fired after the pointer was dragged off it before the "
            "release. It must fire on the release and only while the pointer "
            "is still on it."
        )

    #
    # 4. A click nobody could have sampled, which is the one that was lost.
    #
    # Every click above is a press, a sleep, and a release - three separate
    # QMP calls - so the window manager samples the pointer while the
    # button is down and sees it. This one sends the press and the release
    # as **one event batch**: the board processes both before anybody
    # looks, so a reader that only ever asks "what is held now" observes
    # nothing at all and the click never existed.
    #
    # That is the bug Diego found on the ThinkPad on 21 September - "the
    # mouse buttons be unrespiosnive under certaun scenarios" - and the
    # reason it needed real hardware is that only a *busy* desktop takes
    # long enough between samples for an ordinary click to fall through.
    # Sending both in one batch reproduces on an idle machine in QEMU what
    # a loaded machine did by itself.
    #
    # `hal/pointer_edges.c` keeps the transitions now and the manager
    # replays them, so this must behave exactly as check 1 did. The
    # control that proves it bites: with the replay removed from `wm.lua`,
    # the window manager reports no button at all.
    #
    before_fast = _strip(screen_now(), width, *status)

    guest.mouse_to(*_to_tablet(wx + verb[0], wy + verb[1], width, height))
    time.sleep(0.5)
    guest._qmp("input-send-event", {"events": [
        {"type": "btn", "data": {"down": True,  "button": "left"}},
        {"type": "btn", "data": {"down": False, "button": "left"}},
    ]})

    settle(guest,
           lambda w, h, px: (True if _strip(px, w, *status) != before_fast
                             else None),
           "a press and release delivered together changed nothing: the "
           "click was dropped because the pointer was sampled rather than "
           "its transitions read. See hal/pointer_edges.c.")

    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back after clicking.")

    return 4


def check_graphical_mode(guest):
    """While something owns the screen, the console must not print on it.

    `wm gallery,say:...` starts a window manager with two programs: one opens
    a window, and the other waits and then prints six lines to the console.
    Those lines must reach the serial line and must not reach the display.

    They cannot share. The console's scroll moves every pixel there is,
    because as far as it knows every pixel is text - so one printed line
    dragged every window up sixteen rows and left a copy of its title bar
    behind. It read as a compositor bug and was not.

    The console is not silenced, only kept off the framebuffer. Everything
    still goes down the cable, which is where a machine running a window
    manager is debugged from - and a panic takes the screen back regardless
    of who holds it, because the reason the machine stopped matters more
    than what was being drawn.

    `say` waits before printing, and that is the whole reason it exists.
    This check has to photograph the screen before the output and again
    after it, and every other program here prints the moment it starts - so
    the first photograph already contained the answer. Written first with
    `hello`, it passed with the bug deliberately put back, twice, in two
    different shapes.
    """
    mark = len(guest.seen)
    guest.type("wm gallery,say:6 while the window manager owns the screen")

    #
    # The baseline is taken once a window tab is on screen, not after a
    # fixed wait.
    #
    # A fixed wait was wrong in a way that only showed up in a full run: the
    # window manager has to load two programs out of /bin before it composes
    # anything, and until it does the console is still printing the command
    # that started it. The baseline caught that half-drawn, the comparison
    # found the difference, and the phase failed with the code perfectly
    # correct. Standalone it passed every time.
    #
    deadline = time.monotonic() + 30

    while time.monotonic() < deadline:
        width, height, before = parse_ppm(guest.screendump())

        if tab_width(width, height, before) > 0:
            break

        time.sleep(0.5)
    else:
        raise Failure(
            "no window appeared, so the window manager never took the "
            "screen and there is nothing here to check."
        )

    # And a moment past that, so the first composite is finished.
    time.sleep(1.5)
    width, height, before = parse_ppm(guest.screendump())

    deadline = time.monotonic() + 40

    while time.monotonic() < deadline:
        guest._read_available()

        if "6: while the window manager" in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure(
            "the program that prints never printed, so this phase proves "
            "nothing about where its output went.\n"
            f"--- what arrived ---\n{guest.seen[mark:]}"
        )

    time.sleep(2)
    _, _, after = parse_ppm(guest.screendump())

    for y in range(height):
        row = y * width * 3

        if before[row:row + width * 3] != after[row:row + width * 3]:
            raise Failure(
                f"row {y} of the screen changed while a program printed six "
                "lines and the window manager owned the display. The console "
                "is still drawing on the framebuffer - check that every path "
                "through kernel/console.c asks can_draw() and not just "
                "`attached`."
            )

    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back.")

    return 2


def check_window_manager(guest):
    """This milestone's definition of done: BeOS's test.

    `wm hello-win,stuck` starts two applications in windows. One of them
    answers and one of them is an infinite loop that never replies again.
    Dragging the hung one's window by its title bar has to work anyway.

    That is not a question about speed, it is a question about who owns the
    pixels. An application here owns none: it sends a list of drawing
    commands and the window manager renders them into a surface it keeps. So
    a hung application changes nothing - its contents were never in its
    address space, and the manager takes every request with a non-blocking
    receive, so there is nowhere for it to wait.

    The window that gets moved is deliberately the hung one. Moving the
    other would prove nothing: it answers.

    This phase runs last because the window manager takes the whole screen.
    """
    guest.type("wm hello-win,stuck")
    started(guest)

    width, height, px = parse_ppm(guest.screendump())
    before = find_colour_anywhere(width, height, px, HUNG_TITLE)

    if before is None:
        raise Failure(
            "the hung application's window is not on screen at all, so "
            "there is nothing here to drag. Either `wm` did not start, or "
            "it could not hand /app/wm to the applications it started."
        )

    # Dragged by its title bar, with the mouse, which is what the milestone
    # actually asks for. The keyboard path still exists - Control-W then an
    # arrow - and is checked by the widget phase using the same window
    # manager; this is the one that matters.
    #
    # The events go through QMP's `input-send-event` rather than the human
    # monitor's `mouse_move`, because that one sends *relative* deltas and
    # this device reports absolute position. They never meet, and the
    # symptom is a cursor that never moves.
    tab_x, tab_y = before[0] + 40, before[1] - 10

    guest.mouse_to(*_to_tablet(tab_x, tab_y, width, height))
    time.sleep(0.5)
    guest.mouse_button(True)
    time.sleep(0.4)

    for step in range(1, 9):
        guest.mouse_to(*_to_tablet(tab_x - step * 30, tab_y + step * 12,
                                   width, height))
        time.sleep(0.25)

    guest.mouse_button(False)
    time.sleep(1.5)

    width, height, px = parse_ppm(guest.screendump())
    after = find_colour_anywhere(width, height, px, HUNG_TITLE)

    if after is None:
        raise Failure(
            f"the hung window was at {before} and is now nowhere on screen. "
            "Moving it did not move it, it lost it."
        )

    if after[0] >= before[0] or after[1] <= before[1]:
        raise Failure(
            f"the hung application's window did not follow the pointer: "
            f"{before} then {after}, and the drag went left and down. "
            "Either the compositor is waiting for an application - the one "
            "thing it must never do - or the button press never reached it."
        
            + "\n--- the last of what the machine said ---\n"
            + guest.seen[-1800:]
        )

    return 3


def check_wm_latency(guest):
    """An application's request wakes the window manager.

    Every window asks the window manager something each frame and waits for
    the answer. The manager used to sleep in the console server's input wait
    and collect requests only when that ended, so a request on an idle
    desktop cost 11.5 ms - 2.86 scheduler ticks - and a game making two a
    frame could not pass 43 frames a second on the ThinkPad however little it
    had to draw. The console server now ends its sleep when a caller arrives
    on the manager's endpoint.

    `wmlatency` judges itself against half a scheduler tick, and this reads
    its verdict. It was run first against the manager as it was and failed
    at 2.86 ticks, three times.
    """
    mark = len(guest.seen)
    guest.type("wm wmlatency")
    deadline = time.monotonic() + 90

    while time.monotonic() < deadline:
        guest._read_available()
        tail = guest.seen[mark:]

        if "PASS:" in tail or "FAIL:" in tail or "went away" in tail:
            time.sleep(0.5)
            guest._read_available()
            break

        time.sleep(0.3)

    said = [l.strip() for l in guest.seen[mark:].replace("\r", "").split("\n")
            if "wmlatency" in l or l.strip().startswith(("PASS:", "FAIL:"))]

    stop = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()
    end = time.monotonic() + 15

    while time.monotonic() < end:
        guest._read_available()

        if PROMPT in guest.seen[stop:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-W Q did not get the screen back after wmlatency.")

    if not any(l.startswith("PASS:") for l in said):
        raise Failure("an application's request does not wake the window "
                      "manager:\n  " + "\n  ".join(said or ["(wmlatency said nothing)"]))

    return 1


def check_latency(guest):
    """A yield is not paced by the timer, and neither is an IPC round trip.

    Not a display check, and here anyway, because this is the only harness
    that boots the *shipping* image and drives its shell. `make test` cannot
    ask this question: during the test suite the kernel's first thread is
    running the suite and never reaches the idle loop, so a yield with an
    empty runqueue returns instantly and the answer is always yes.

    A test was written in luatest.lua that did exactly that. It passed with
    the bug deliberately put back, which is worse than having no test, and
    this replaced it.

    The program judges itself; this reads the verdict. `latency.lua` carries
    the explanation of what it is watching for and why nothing else saw it.
    """
    mark = len(guest.seen)
    guest.type("latency")

    deadline = time.monotonic() + 30

    while time.monotonic() < deadline:
        guest._read_available()

        if "PASS:" in guest.seen[mark:] or "FAIL:" in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure(
            "`latency` never reported.\n"
            f"--- what it did say ---\n{guest.seen[mark:]}"
        )

    said = guest.seen[mark:]

    if "FAIL:" in said:
        detail = said[said.index("FAIL:"):].split("\n")[0]
        raise Failure(f"scheduling latency: {detail}")

    return 1


def check_reaped(guest):
    """An application that dies says why, and loses its window.

    **Both halves of this were silent, and the silence cost a day.** A
    detached program - which is every graphical application, because the
    window manager launches them that way - had its error thrown away by a
    `pcall(chunk)` whose result nothing read. And a window whose process had
    gone stayed on the screen for ever, fully drawn, because the compositor
    owns the pixels and nothing told the desktop otherwise.

    Together those two make a dead application and a busy one *identical*
    from the outside. An afternoon went into a window that had died on its
    first pass, read as a hang, then as a lost mouse event, then as a
    message-size limit; the one thing that would have said otherwise in ten
    seconds is the line this now checks for.

    So this writes a program that opens a window and then raises, runs it,
    and asks for both: the reason on the serial line, and the window gone
    from the screen.
    """
    #
    # Written by the machine rather than carried in /bin, because a program
    # whose whole purpose is to crash is not one to ship - and `edit` has
    # already proved the machine can write its own.
    #
    program = (
        "local ui = use('/lib/ui.lua') "
        "local w = ui.window{ title = 'Dying', w = 220, h = 90, "
        "x = 300, y = 300 } "
        "if not w then return end "
        "local v = ui.view{ x = 0, y = 0, w = 0, h = 0 } "
        "function v:tick() error('deliberate, for the harness') end "
        "w:add(v) w:run()"
    )

    guest.type("fs.write('/ramfs/dying.lua', %r)" % program)
    time.sleep(2)

    mark = len(guest.seen)
    guest.type("wm /ramfs/dying.lua")

    # It has to appear before it can be missed. If it never opens, the check
    # below would pass for the wrong reason.
    started(guest)
    checks = 1

    #
    # Five seconds of silence plus a pass, which is what the desktop waits
    # before it asks whether a quiet window still has a process behind it.
    #
    deadline = time.monotonic() + 25.0
    gone = False

    while time.monotonic() < deadline:
        time.sleep(1.0)
        width, height, px = parse_ppm(guest.screendump())

        if count_windows(width, height, px) == 0:
            gone = True
            break

    if not gone:
        raise Failure(
            "an application raised on its first tick and its window is "
            "still on the screen. A dead window and a busy one look the "
            "same from outside, which is exactly the confusion this "
            "checks against."
        )

    checks += 1

    guest._read_available()
    said = guest.seen[mark:]

    if "deliberate, for the harness" not in said:
        raise Failure(
            "the application died without saying why. A detached program's "
            "error is reported by the runner in `init.lua`; if that is "
            "silent again, every application crash on this desktop is "
            "invisible.\n" + said[-600:]
        )

    checks += 1

    # Control-C back to the shell, the way every phase here ends.
    mark = len(guest.seen)
    guest.proc.stdin.write(STOP_DESKTOP)
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back after the "
                      "reaping check.")

    return checks


def check_editor(guest):
    """The machine writes and runs its own program.

    Types four lines into `edit`, saves with Control-S, quits with
    Control-Q, and runs the file from the shell. The answer has to be 15.

    The strongest end-to-end statement this harness makes, because of how
    many separate things it depends on: the console draining raw keys to a
    program that has taken the screen, the editor's line handling, `fs.write`
    into a server, the file surviving the editor exiting, and the shell
    running a path outside /bin. Any one of them broken and there is no 15.

    Before the window manager phase, which takes the screen for good.
    """
    guest.type("edit /ramfs/sum.lua")
    time.sleep(3)

    program = (
        "-- written on the machine itself\n"
        "local n = 0\n"
        "for i = 1, 5 do n = n + i end\n"
        "print(\"the sum is \" .. n)\n"
    )

    for ch in program:
        guest.proc.stdin.write(ch.encode())
        guest.proc.stdin.flush()
        time.sleep(0.02)

    time.sleep(1)

    width, height, px = parse_ppm(guest.screendump())
    blank = all(px[i] == px[0] for i in range(0, width * 3 * 40, 3))

    if blank:
        raise Failure(
            "nothing was drawn in the editor's first forty rows. The keys "
            "are not reaching it, or it is not redrawing when they do."
        )

    guest.proc.stdin.write(b"\x13")     # Control-S
    guest.proc.stdin.flush()
    time.sleep(1.5)

    guest.proc.stdin.write(b"\x11")     # Control-Q
    guest.proc.stdin.flush()
    time.sleep(1.5)

    mark = len(guest.seen)
    guest.type("run /ramfs/sum.lua")

    deadline = time.monotonic() + 20

    while time.monotonic() < deadline:
        guest._read_available()

        if "the sum is" in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure(
            "the program typed into the editor did not run.\n"
            f"--- what came back ---\n{guest.seen[mark:]}"
        )

    said = guest.seen[mark:]

    if "the sum is 15" not in said:
        line = said[said.index("the sum is"):].split("\n")[0]
        raise Failure(
            f"the editor saved something, and it was not what was typed: "
            f"it printed {line!r} rather than 'the sum is 15'."
        )

    return 2


def write_png(path, data):
    """The screendump, as something a person can open."""
    width, height, px = parse_ppm(data)

    raw = bytearray()
    for y in range(height):
        raw.append(0)                       # filter type 0, none
        raw += px[y * width * 3:(y + 1) * width * 3]

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))

    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", help="the image to boot")
    ap.add_argument("--png", help="also write the Lua-drawn screen here")
    ap.add_argument("--timeout", type=float, default=30.0,
                    help="seconds to wait for the guest (default: 30)")
    ap.add_argument("--phases", default="",
                    help="run only these phases, by name, separated by "
                         "commas - how `tools/gate.py` runs the harness as "
                         "several machines side by side")
    args = ap.parse_args()
    only = [p for p in args.phases.split(",") if p]

    guest = None

    try:
        guest = Guest(args.image, args.timeout)

        # The prompt, not the display stage: the progress bar is only full
        # once every stage has run, and the point of checking it is that it
        # reaches the end.
        guest.wait_for(PROMPT, "reached the shell prompt")

        found = GEOMETRY.search(guest.seen)
        if found is None:
            raise Failure(
                "the guest never reported a display geometry. ramfb was on "
                "the QEMU line, so either fw_cfg is not finding the etc/ramfb "
                "item or the display stage stopped printing its size.\n"
                f"--- what it did say ---\n{guest.seen}"
            )

        reported = f"{found.group(1)}x{found.group(2)}"
        splash_checks = check_boot_screen(reported, guest.screendump())

        guest.type(DRAW)

        bar_checks = check_bars(settle(
            guest, bars_drawn,
            "the three bars never appeared. The snippet draws them straight "
            "into gfx.screen(), so either the shell was not given the screen "
            "or the fill did not reach it."))

        # The picture `--png` saves, in the raw form `write_png` wants.
        # Taken here rather than kept from the wait above, because that now
        # hands back an already-parsed picture - and taken before the line
        # below prints anything, since a print scrolls the console and the
        # console repaints over what was drawn.
        drawn = guest.screendump()

        #
        # **The login set, emptied, before any phase starts a desktop.**
        #
        # `/lib/startup.lua` opens the top bar, Tracker, Monitor, Processes
        # and the log on a machine nobody has told otherwise, which is right
        # for a person and wrong for a harness: every phase below counts
        # windows, and a bare `wm` would arrive with six of them for reasons
        # that have nothing to do with what the phase is testing.
        #
        # An empty list rather than deleting the file, because those mean
        # different things and the difference is the feature: absent is
        # "nobody has chosen" and opens the default, empty is "somebody
        # unticked everything" and opens nothing. Writing it here asserts
        # the second, which no other check covers.
        #
        # Waited for, not merely typed. `type` writes into a pipe and returns;
        # the next phase sends key *events* through the monitor, and the two
        # streams land in the same console queue - so an unconsumed line here
        # came back spliced through the middle of `2+2`. The marker is
        # assembled at run time so that waiting for it cannot match the echo
        # of the line that asks for it.
        #
        # **And the palette its colours were written against.** A desktop
        # with nothing saved is BeOS now, and nearly every colour this file
        # looks for - `SELECTED`, `GREEN`, `HUNG_TITLE`, `HIGHLIGHT`,
        # `TAB_IDLE`, the meters - is a field of the kit's own dark palette.
        # Moving them to BeOS is not a change of numbers: its idle tab is the
        # same #e8e8e8 as every raised button, so a check that finds tabs by
        # that colour would find buttons. So this names the dark palette,
        # which `theme.apply` resolves by name. BeOS is what `make shot`
        # photographs and what every desktop boot in `run_x86.py` runs.
        #
        # The default look, before the pin below hides it - and before the
        # phase helper exists, which is the whole point: this is the one
        # check that has to run on a machine nobody has told anything.
        phase_times = []
        default_look_checks = 0

        #
        # **A desktop cannot be quit, so asking one costs a machine.**
        #
        # Half of this check is a question only the window manager can
        # answer, and the window manager it has to ask is one on a machine
        # nobody has told anything - which is this moment and no other,
        # because the pin below hands every later desktop a font table.
        # Starting it here takes the console for the rest of the boot, and
        # there is no way to stop a desktop, so the phases below would have
        # nobody to type to.
        #
        # So `gate.py` gives this phase a machine of its own, and only there
        # is the window manager asked. In a whole run - `make screenshot` -
        # the three checks that need no desktop still run.
        #
        if not only or "default look" in only:
            began = time.monotonic()
            default_look_checks = check_default_look(
                guest, ask_wm=(only == ["default look"]))
            phase_times.append((time.monotonic() - began, "default look"))

            if only == ["default look"]:
                print("%d checks on what a machine nobody has told looks "
                      "like: IBM Plex in its roles, each face drawing, and "
                      "the window manager holding the faces it hands to "
                      "applications." % default_look_checks)
                return 0

        # **And the faces its rows were measured against.** The default is
        # IBM Plex since 19 September (`docs/styleguide.html`), and nearly
        # every check below finds a row, a baseline or a column by the 8 by
        # 16 bitmap they were written for. Pinned here rather than chased
        # through forty phases - and the default itself is checked in
        # `default_look`, on a machine with nothing saved, which is the only
        # place it can be.
        guest.type('fs.write("/home/.startup", { items = {} }) '
                   + appearance() + ' '
                   'print("harness-set" .. "-up")')
        guest.wait_for("harness-set-up",
                       "emptied the login set, chose the dark palette and "
                       "pinned the bitmap faces")

        # Each phase timed, because "the harness is slow" is not something
        # to guess about. The number that matters is which phase, not the
        # total: a phase that waits a fixed twelve seconds and a phase that
        # takes twelve seconds of screendumps need different fixes.

        # **A phase not asked for counts nothing and runs nothing** - the
        # gate runs the harness as several machines, each with its share of
        # the phases in their original order, and adds up what they say.
        # The boot screen and the bars above run in every part: seconds,
        # and a machine that did not draw them is not one to test further.
        def phase(name, fn):
            if only and name not in only:
                return 0

            began = time.monotonic()
            result = fn(guest)
            phase_times.append((time.monotonic() - began, name))
            return result

        key_checks = phase("keyboard", check_keyboard)
        # At the bare prompt, before anything else has started a program.
        name_checks = phase("programs by name", check_programs_by_name)

        latency_checks = phase("latency", check_latency)
        wm_latency_checks = phase("window manager latency", check_wm_latency)
        stop_checks = phase("interrupt", check_interrupt)
        bar_updates = phase("status_bar", check_status_bar)
        editor_checks = phase("editor", check_editor)
        # Before `widgets`, the first phase that starts a window manager:
        # this one's first start is its control, and needs a clean registry.
        registry_checks = phase("registry", check_registry)
        context_checks = phase("context", check_context)
        widget_checks = phase("widgets", check_widgets)
        prefs_checks = phase("preferences", check_preferences)
        script_checks = phase("scripting", check_scripting)
        idle_checks = phase("idle", check_idle)
        direct_checks = phase("direct", check_direct)
        three_d_checks = phase("3d", check_3d)
        terminal_checks = phase("terminal", check_terminal)
        file_checks = phase("programs by file", check_programs_by_file)
        log_view_checks = phase("log view", check_log_view)
        sized_checks = phase("text size", check_text_size)
        fold_checks = phase("window resize", check_window_resize)
        tri_checks = phase("triangle", check_triangle)
        repaint_checks = phase("repaints", check_repaints)
        power_checks = (phase("power button", check_power_button)
                        if machine(args.image) == "aarch64" else 0)
        power_setting_checks = (phase("power setting", check_power_setting)
                                if machine(args.image) == "x86_64" else 0)
        unknown_key_checks = (phase("unknown keys", check_unknown_keys)
                              if machine(args.image) == "x86_64" else 0)
        volume_key_checks = phase("volume keys", check_volume_keys)
        budget_checks = phase("compositor budget", check_budget)
        face_checks = phase("faces", check_faces)
        wallpaper_checks = phase("wallpapers", check_wallpapers)
        direct_menu_checks = phase("direct menu", check_direct_menu)
        tab_checks = phase("tabs", check_tabs)
        corner_checks = phase("corners", check_corners)
        shadow_checks = phase("shadow", check_shadow)
        wheel_checks = phase("wheel", check_wheel)
        scale_checks = phase("scale", check_scale)
        scale_live_checks = phase("scale changed", check_scale_live)
        appearance_checks = phase("appearance", check_appearance)
        drives_app_checks = phase("drives app", check_drives_app)
        snes_checks = phase("Super Nintendo --scale", check_snes_scale)
        deskbar_checks = phase("deskbar", check_deskbar)
        focus_checks = phase("deskbar focus", check_focus_shown)
        icon_size_checks = phase("icon sizes", check_icon_sizes)
        desktop_checks = phase("desktop", check_desktop)
        places_checks = phase("places", check_places)
        panel_checks = phase("panel", check_panel)
        clip_checks = phase("clipboard", check_clipboard)
        cores_checks = phase("cores", check_cores)
        reaped_checks = phase("reaped", check_reaped)
        click_checks = phase("clicks", check_clicks)
        graphical_checks = phase("graphical", check_graphical_mode)
        replicant_checks = phase("replicants", check_replicants)
        wm_checks = phase("window_manager", check_window_manager)

        if args.png:
            write_png(args.png, drawn)
            print(f"Wrote {args.png}.")

    except Failure as e:
        print(f"\nFAIL: {e}", file=sys.stderr)

        #
        # **The whole of what the guest said, kept.** A failure used to
        # leave the last few lines and nothing before them, and three
        # failures of one shape - a file server answering as though what is
        # there were not: `/bin` listed as one name twice, a file written two
        # seconds earlier "no such path" once - each left too little to
        # explain (`roadmap.md`, Known and unexplained). `guest.seen` has
        # always held all of it; now a failure writes it down.
        #
        if guest is not None:
            where = "build/harness-failure-%s%s.txt" % (
                machine(args.image),
                ("-" + only[0].replace(" ", "-")) if only else "")

            try:
                guest._read_available()

                with open(where, "w") as kept:
                    kept.write(guest.seen)

                print(f"the guest's whole log is in {where}", file=sys.stderr)
            except OSError as why:
                print(f"the guest's log could not be kept: {why}",
                      file=sys.stderr)

        return 1
    finally:
        if guest is not None:
            guest.close()

    total = (splash_checks + bar_checks + key_checks + bar_updates
             + stop_checks + wm_checks + latency_checks + editor_checks
             + wm_latency_checks
             + widget_checks + prefs_checks + script_checks
             + replicant_checks
             + graphical_checks + click_checks + deskbar_checks
             + focus_checks + desktop_checks + places_checks
             + panel_checks
             + clip_checks + cores_checks + reaped_checks
             + idle_checks + terminal_checks + log_view_checks
             + icon_size_checks
             + sized_checks + fold_checks + tri_checks
             + direct_checks
             + three_d_checks + registry_checks + context_checks
             + repaint_checks + power_checks + budget_checks + snes_checks
             + unknown_key_checks + power_setting_checks + volume_key_checks + face_checks + wallpaper_checks + direct_menu_checks
             + default_look_checks
             + tab_checks + corner_checks + shadow_checks + wheel_checks
             + drives_app_checks
             + name_checks + file_checks)
    missing = [n for n in only if n not in {name for _, name in phase_times}]

    if missing:
        print("FAIL: no phase ran by these names: " + ", ".join(missing))
        return 1

    print("\nwhere the time went:")
    for seconds, name in sorted(phase_times, reverse=True):
        print(f"  {seconds:6.1f}s  {name}")
    print(f"  {sum(t for t, _ in phase_times):6.1f}s  in phases")

    print(f"guest: the display is {reported}")
    print(f"\nPASS: {total} display checks "
          f"({splash_checks} on the kernel's boot screen, {bar_checks} on what "
          f"Lua drew through gfx, {key_checks} on the keyboard, "
          f"{name_checks} on programs reached by typing their name, "
          f"{sized_checks} on a heading larger than the desktop's text, "
          f"{fold_checks} on a window asking for its own size, "
          f"{tri_checks} on a triangle drawn through a command, "
          f"{bar_updates} on a detached program still drawing, "
          f"{stop_checks} on Control-C stopping it, "
          f"{wm_checks} on dragging a hung application's window, "
          f"{latency_checks} on scheduling latency, "
          f"{wm_latency_checks} on a request waking the window manager, "
          f"{editor_checks} on the machine writing and running its own "
          f"program, "
          f"{registry_checks} on a window manager found by name after a "
          f"restart, "
          f"{context_checks} on the right button reaching an application "
          f"and pressing nothing, "
          f"{widget_checks} on the widget kit, "
          f"{prefs_checks} on Preferences changing its page when the "
          f"sidebar changes category, "
          f"{script_checks} on scripting a running application, "
          f"{replicant_checks} on a replicant moved between processes, "
          f"{graphical_checks} on the console staying off the screen while "
          f"something else owns it, "
          f"{click_checks} on the widgets under the pointer, "
          f"{deskbar_checks} on starting an application from the Deskbar, "
          f"{focus_checks} on the Deskbar showing where the focus went at "
          f"once, "
          f"{desktop_checks} on the desktop below the strip and an icon "
          f"staying where it is dragged, "
          f"{icon_size_checks} on the icon size chosen on the desktop and "
          f"kept, "
          f"{places_checks} on a place made by a drop, opened by a click "
          f"and taken out by a right-click, "
          f"{panel_checks} on the Open window's filter, its one click that "
          f"only selects and its second that hands over the path, "
          f"{clip_checks} on copying text from one application into "
          f"another and on This Machine's report following its window, "
          f"{reaped_checks} on an application that dies saying why and "
          "losing its window, "
          f"{cores_checks} on a processor meter moving when the machine is "
          f"given work, "
          f"{idle_checks} on an idle desktop being idle, "
          f"{terminal_checks} on a terminal window (a program printing into "
          f"one, and its character grid following the window when it is "
          f"resized), "
          f"{file_checks} on a program run by its file, in a Terminal and "
          f"as Tracker opens one, "
          f"{log_view_checks} on Log View (rows on black that do not "
          f"overlap, following what is logged, holding still while "
          f"scrolled back, and larger text from its View menu), "
          f"{repaint_checks} on an idle window drawing nothing at all, "
          f"{power_checks} on the power button reaching a driver outside the kernel, "
          f"{power_setting_checks} on the power button doing what Preferences "
          f"says, "
          f"{unknown_key_checks} on a key the keyboard driver has no entry for "
          f"being named once rather than dropped in silence, "
          f"{volume_key_checks} on the volume keys reaching the window "
          f"manager rather than a window, "
          f"{budget_checks} on a full-screen picture and a maximised window fitting "
          f"in the compositor at 1920x1080, "
          f"{default_look_checks} on what a machine nobody has told looks "
          f"like - IBM Plex in its roles, and each face drawing, "
          f"{face_checks} on every outline face loading and drawing, Space "
          f"Grotesk's five weights among them, "
          f"{wallpaper_checks} on the desktop's wallpapers carried in the "
          f"image and one reaching the screen pixel for pixel, "
          f"{direct_menu_checks} on a menu bar above a window that draws its "
          f"own pixels, and its menu reaching the program, "
          f"{drives_app_checks} on the Drives app opening and drawing, "
          f"{appearance_checks} on the Appearance panel laying itself out "
          f"from the faces in force rather than from a constant, and Plex "
          f"chosen - its five faces loaded, written down, and still worn "
          f"after a restart, and its spacing inside a widget; and four "
          f"theme events reaching a window across as many replies as fit, "
          f"and the Deskbar held at 32 over a saved height, "
          f"{scale_checks} on everything at 150 per cent - a kit window, "
          f"its title bar, its rows and a click, and a window drawing its own "
          f"pixels stretched, {scale_live_checks} on the scale changed with "
          f"windows open and back again, "
          f"{tab_checks} on the title bar - across the whole window over a "
          f"saved BeOS tab, and a maximise box greyed and doing nothing "
          f"where a window cannot be maximised, "
          f"{corner_checks} on a rounded window's corner showing the desk "
          f"after it moved onto its own old place, and a menu's corners "
          f"showing it too, "
          f"{shadow_checks} on a window casting a shadow and taking it with "
          f"it when it moves, "
          f"{wheel_checks} on the scroll wheel scrolling the list under the "
          f"pointer, both ways, "
          f"{snes_checks} on the Super Nintendo's --scale reaching the window "
          f"and not the ROM's name, "
          f"{direct_checks} on an application drawing its own pixels, "
          f"{three_d_checks} on a software-rendered solid).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
