#!/usr/bin/env python3
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
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

QEMU = "qemu-system-aarch64"

# Has to agree with kernel/console.c: RESERVED_ROWS rows of GLYPH_H,
# at the bottom, which the kernel keeps out of the scroll region and
# `monitor` draws its one line of text into.
GLYPH_W = 8
GLYPH_H = 16
RESERVED_PX = 2 * GLYPH_H


# Has to agree with the Makefile's line, except for the display: a window is
# not wanted here, and `-display none` still gives ramfb a surface to scan
# out and the monitor something to dump.
QEMU_ARGS = [
    "-M", "virt,gic-version=3",
    "-cpu", "cortex-a72",
    "-m", "512M",
    "-display", "none",
    "-device", "ramfb",
    # force-legacy=false is not optional: QEMU's virtio-mmio transports
    # report the legacy interface unless told otherwise, and the driver
    # refuses those.
    "-global", "virtio-mmio.force-legacy=false",
    "-device", "virtio-keyboard-device",
    "-device", "virtio-tablet-device",
]

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
    + "return 'drawn'"
)


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
        self.dir = tempfile.TemporaryDirectory()
        self.sockpath = os.path.join(self.dir.name, "monitor")
        self.qmppath = os.path.join(self.dir.name, "qmp")
        self.seen = ""

        try:
            self.proc = subprocess.Popen(
                [QEMU, *QEMU_ARGS,
                 "-monitor", f"unix:{self.sockpath},server,nowait",
                 "-qmp", f"unix:{self.qmppath},server,nowait",
                 "-serial", "stdio",
                 "-kernel", image],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, bufsize=0,
            )
        except FileNotFoundError:
            raise Failure(f"{QEMU} not found. See docs/setup.md.")

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

        self.monitor = None
        self.qmp = None

    def _read_available(self):
        """Drain whatever the serial line has produced, without blocking."""
        fd = self.proc.stdout.fileno()

        while True:
            ready, _, _ = select.select([fd], [], [], 0.1)
            if not ready:
                return

            try:
                chunk = os.read(fd, 65536)
            except BlockingIOError:
                return

            if not chunk:
                return

            self.seen += chunk.decode("utf-8", errors="replace")

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
        self.monitor.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.25)

    def screendump(self):
        self._connect_monitor()
        path = os.path.join(self.dir.name, f"shot{time.monotonic_ns()}.ppm")

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
        self.dir.cleanup()


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


def check_bars(data):
    """Phase two: what Lua drew, through gfx.screen().

    This is the one no in-guest test can do. A surface that steps by
    `width * 4` instead of by the pitch is perfectly self-consistent to
    everything inside the guest, and produces bars that walk sideways by
    sixteen pixels a row out here.
    """
    width, height, at = pixel_reader(data)
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


# The list selection in the gallery, 0x1f6feb, which is the accent colour.
SELECTED = (0x1f, 0x6f, 0xeb)


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
    guest.type("wm gallery")
    time.sleep(6)

    width, height, px = parse_ppm(guest.screendump())
    before = find_colour_anywhere(width, height, px, SELECTED)

    if before is None:
        raise Failure(
            "the gallery's list has no selection bar on screen, so either "
            "the window never opened or the kit did not draw it."
        )

    def send(data, wait=0.4):
        guest.proc.stdin.write(data)
        guest.proc.stdin.flush()
        time.sleep(wait)

    send(b"\t\t")            # past both buttons, to the checkbox
    send(b" ")                # tick it
    send(b"\t")               # to the field
    send(b" edited")
    send(b"\t")               # to the list
    send(b"\x1b[B\x1b[B")     # down twice
    send(b"\r", 1.5)

    width, height, px = parse_ppm(guest.screendump())
    after = find_colour_anywhere(width, height, px, SELECTED)

    if after is None:
        raise Failure("the list's selection bar vanished while it was used.")

    rows = (after[1] - before[1]) / 16.0

    if abs(rows - 2) > 0.5:
        raise Failure(
            f"two down arrows moved the list's selection {rows:.1f} rows "
            f"rather than 2 ({before} then {after}). Either Tab is not "
            "reaching the application - the window manager used to swallow "
            "it - or the arrows are not."
        )

    # Hand the screen back, or the next phase types its command at a shell
    # that is still blocked waiting for this window manager to finish.
    mark = len(guest.seen)
    send(b"\x03", 2.0)

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure(
            "Control-C did not get the screen back from the window manager, "
            "so the shell is still waiting for it."
        )

    return 2


# The focused window's tab, 0xffc700.
TAB = (0xff, 0xc7, 0x00)


def tab_width(width, height, px):
    """How wide the widest run of tab colour on screen is, in pixels.

    The tab is as wide as its title and no wider - the one BeOS decision
    copied exactly - so its width *is* the title's length, which makes it
    the thing to measure when checking that a rename reached the window.
    """
    widest = 0

    for y in range(0, height, 2):
        run = 0

        for x in range(width):
            at = (y * width + x) * 3

            if (px[at], px[at + 1], px[at + 2]) == TAB:
                run += 1

                if run > widest:
                    widest = run
            else:
                run = 0

    return widest


def check_scripting(guest):
    """An application scripted by another, with no scripting code in either.

    roadmap.md M7's third definition of done. `gallery.lua` contains not one
    line about properties: it calls `ui.window`, and `ui.window` registers
    the window with /app and answers for its properties. `setprop` is a
    general four-line program that writes a path. Neither knows about the
    other.

    `wm gallery,setprop:/app/gallery/title=renamed by another process`
    starts both, each in its own address space, each handed /dev/wm and
    /app and nothing else.

    Foreground, and that is not incidental. A window manager reads the
    keyboard, and so does the shell's line editor, so running one detached
    means two processes draining one input queue and whichever asks first
    wins. Doing this from the prompt worked and then did not, depending on
    timing. The real answer is the Terminal app, where the shell is a window
    and there is one reader; until then, the honest arrangement is that the
    window manager has the keyboard while it runs.

    Checked by consequence rather than by reply. The tab is exactly as wide
    as its title, so a longer title has to make a wider tab. A property
    store that accepted the write and told nobody would pass a read-back and
    fail this.
    """
    guest.type("wm gallery")
    time.sleep(6)

    width, height, px = parse_ppm(guest.screendump())
    before = tab_width(width, height, px)

    if before == 0:
        raise Failure(
            "no window tab on screen after `wm gallery`, so there is "
            "nothing here to rename."
        )

    # Back to the shell, then start both together.
    guest.proc.stdin.write(b"\x03")
    guest.proc.stdin.flush()
    time.sleep(2)

    guest.type("wm gallery,setprop:/app/gallery/title=renamed by another one")
    time.sleep(9)

    width, height, px = parse_ppm(guest.screendump())
    after = tab_width(width, height, px)

    if after == 0:
        raise Failure("no tab on screen at all after the second start.")

    if after <= before:
        raise Failure(
            f"the tab is {after}px wide and was {before}px before anything "
            "renamed it. A longer title has to make a wider tab: either the "
            "registry did not hand over the window's endpoint, or the "
            "property was stored somewhere that is not the window."
        )

    # And hand the screen back for the phase after this one.
    mark = len(guest.seen)
    guest.proc.stdin.write(b"\x03")
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
    no pointer yet, so `clock` offers the replicant through /data and
    `tracker` picks it up. The mechanism is the whole of it either way; the
    pointer is the part that is missing.

    `wm clock,tracker` starts two applications in two address spaces. The
    first publishes a description - source, state, and a `needs` list - and
    shows the clock. The second has never heard of clocks: it reads the
    description and instantiates it. Both must then be ticking, with
    different state, which is what makes it a replicant rather than a
    picture of one.

    Two things are checked, both by what reaches the screen:

      * two green clocks, in two different windows, and both changing.
        `tracker` re-runs the same source with its own state, so the two
        read differently and neither is a copy of the other's pixels.
      * `tracker` also prints what the replicant's restricted namespace
        actually answers - it tries /dev/cpu, which was declared, and /data,
        which was not - so the sandbox line on screen is a measurement and
        not a claim. That line is green only when the refusal happened.
    """
    guest.type("wm clock,tracker")
    time.sleep(12)

    width, height, before = parse_ppm(guest.screendump())
    bands = green_bands(width, height, before)

    if len(bands) < 3:
        raise Failure(
            f"expected three bands of replicant green - a clock in each of "
            f"two windows and the sandbox result - and found {len(bands)}. "
            "Either the replicant did not load in one of them, or the "
            "restricted namespace let /data through, which turns that line "
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
    guest.proc.stdin.write(b"\x03")
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
TAB_IDLE = (0xb8, 0xb8, 0xb8)


def count_windows(width, height, px):
    """How many windows are on screen, counted by their tabs.

    A tab is as wide as its title and every window has exactly one, so
    counting bands of tab colour counts windows - and it does not depend on
    knowing where any of them was put.
    """
    bands = 0
    inside = False

    for y in range(0, height, 2):
        found = False

        for x in range(0, width, 2):
            at = (y * width + x) * 3
            pixel = (px[at], px[at + 1], px[at + 2])

            if pixel == TAB or pixel == TAB_IDLE:
                found = True
                break

        if found and not inside:
            bands += 1

        inside = found

    return bands


def check_deskbar(guest):
    """A desktop you can start things from.

    `wm` with nothing asked for starts the Deskbar, top right, which lists
    every window on the desktop and every program that declared itself an
    application. Clicking one of those launches it.

    Checked by counting tabs rather than by reading the lists: every window
    has exactly one tab, as wide as its title, so the count is the number of
    windows and does not depend on knowing where anything was placed.

    The Deskbar asks the window manager what is on screen rather than asking
    /app what registered. The difference is real: a program that opens a
    window by talking to the desktop directly - which the two oldest
    demonstrations here do - has a window and no registration, and would be
    missing from a list built the other way.
    """
    guest.type("wm")
    time.sleep(8)

    width, height, px = parse_ppm(guest.screendump())
    before = count_windows(width, height, px)

    if before != 1:
        raise Failure(
            f"expected the Deskbar and nothing else after a bare `wm`, and "
            f"counted {before} windows."
        )

    # The Deskbar is placed at the right edge by deskbar.lua, and its
    # Applications list starts at (10, 170) inside it.
    dx, dy = width - 210 - 12, 34

    guest.mouse_to(*_to_tablet(dx + 70, dy + 170 + 2 + 8, width, height))
    time.sleep(0.4)
    guest.mouse_button(True)
    time.sleep(0.3)
    guest.mouse_button(False)
    time.sleep(6)

    width, height, px = parse_ppm(guest.screendump())
    after = count_windows(width, height, px)

    if after <= before:
        raise Failure(
            f"clicking an application in the Deskbar started nothing: "
            f"{before} window(s) before and {after} after. Either the launch "
            "request is not reaching the window manager, or the program it "
            "named is not marked `-- kosmos: application` and so is not in "
            "the list at all."
        )

    mark = len(guest.seen)
    guest.proc.stdin.write(b"\x03")
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back from the desktop.")

    return 2


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
    guest.type("wm gallery")
    time.sleep(7)

    width, height, px = parse_ppm(guest.screendump())

    # The window is opened at x=60, y=90 by gallery.lua, and its controls
    # are placed at fixed offsets inside it.
    wx, wy = 60, 90
    status = (wx + 12, wy + 296, 380, 16)
    park = (wx + 300, wy + 280)          # somewhere with nothing on it

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
    click(wx + 150 + 30, wy + 60 + 13)
    after_click = _strip(screen_now(), width, *status)

    if after_click == before:
        raise Failure(
            "clicking a button changed nothing. Either the window manager is "
            "not forwarding the press, or the kit is not routing it to the "
            "view under it."
        )

    # 2. A list row, checked by where the selection bar lands.
    _, _, px = parse_ppm(guest.screendump())
    bar_before = find_colour_anywhere(width, height, px, SELECTED)

    click(wx + 16 + 60, wy + 172 + 2 + 16 * 3 + 8)

    _, _, px = parse_ppm(guest.screendump())
    bar_after = find_colour_anywhere(width, height, px, SELECTED)

    if bar_after is None or bar_after[1] <= bar_before[1]:
        raise Failure(
            f"clicking the fourth row of the list did not move the selection "
            f"({bar_before} then {bar_after})."
        )

    # 3. Pressed, slid off, released: nothing may happen.
    settled = _strip(screen_now(), width, *status)

    click(wx + 16 + 50, wy + 60 + 13, release_at=(wx + 380, wy + 60))
    escaped = _strip(screen_now(), width, *status)

    if escaped != settled:
        raise Failure(
            "a button fired after the pointer was dragged off it before the "
            "release. It must fire on the release and only while the pointer "
            "is still on it."
        )

    mark = len(guest.seen)
    guest.proc.stdin.write(b"\x03")
    guest.proc.stdin.flush()

    deadline = time.monotonic() + 15

    while time.monotonic() < deadline:
        guest._read_available()

        if PROMPT in guest.seen[mark:]:
            break

        time.sleep(0.3)
    else:
        raise Failure("Control-C did not get the screen back after clicking.")

    return 3


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
    guest.proc.stdin.write(b"\x03")
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
    time.sleep(5)

    width, height, px = parse_ppm(guest.screendump())
    before = find_colour_anywhere(width, height, px, HUNG_TITLE)

    if before is None:
        raise Failure(
            "the hung application's window is not on screen at all, so "
            "there is nothing here to drag. Either `wm` did not start, or "
            "it could not hand /dev/wm to the applications it started."
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
        )

    return 3


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
    guest.type("edit /data/sum.lua")
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
    guest.type("run /data/sum.lua")

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
    args = ap.parse_args()

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
        guest.wait_for("drawn", "finished drawing")

        drawn = guest.screendump()
        bar_checks = check_bars(drawn)

        key_checks = check_keyboard(guest)

        latency_checks = check_latency(guest)
        stop_checks = check_interrupt(guest)
        bar_updates = check_status_bar(guest)
        editor_checks = check_editor(guest)
        widget_checks = check_widgets(guest)
        script_checks = check_scripting(guest)
        deskbar_checks = check_deskbar(guest)
        click_checks = check_clicks(guest)
        graphical_checks = check_graphical_mode(guest)
        replicant_checks = check_replicants(guest)
        wm_checks = check_window_manager(guest)

        if args.png:
            write_png(args.png, drawn)
            print(f"Wrote {args.png}.")

    except Failure as e:
        print(f"\nFAIL: {e}", file=sys.stderr)
        return 1
    finally:
        if guest is not None:
            guest.close()

    total = (splash_checks + bar_checks + key_checks + bar_updates
             + stop_checks + wm_checks + latency_checks + editor_checks
             + widget_checks + script_checks + replicant_checks
             + graphical_checks + click_checks + deskbar_checks)
    print(f"guest: the display is {reported}")
    print(f"\nPASS: {total} display checks "
          f"({splash_checks} on the kernel's boot screen, {bar_checks} on what "
          f"Lua drew through gfx, {key_checks} on the keyboard, "
          f"{bar_updates} on a detached program still drawing, "
          f"{stop_checks} on Control-C stopping it, "
          f"{wm_checks} on dragging a hung application's window, "
          f"{latency_checks} on scheduling latency, "
          f"{editor_checks} on the machine writing and running its own "
          f"program, "
          f"{widget_checks} on the widget kit, "
          f"{script_checks} on scripting a running application, "
          f"{replicant_checks} on a replicant moved between processes, "
          f"{graphical_checks} on the console staying off the screen while "
          f"something else owns it, "
          f"{click_checks} on the widgets under the pointer, "
          f"{deskbar_checks} on starting an application from the Deskbar).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
