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

  2. **A pattern drawn from Lua**, through gfx.screen() at the shell prompt.
     Vertical bars, which is the shape a pitch error destroys: each row would
     shift by (4160 - 4096) / 4 = sixteen pixels, turning every vertical line
     into a diagonal. The check reads each bar's x position at several
     heights and requires them all to be the same.

Usage: run_screenshot.py <image.elf> [--png OUT] [--timeout SECONDS]
"""

import argparse
import fcntl
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
        self.seen = ""

        try:
            self.proc = subprocess.Popen(
                [QEMU, *QEMU_ARGS,
                 "-monitor", f"unix:{self.sockpath},server,nowait",
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

        if args.png:
            write_png(args.png, drawn)
            print(f"Wrote {args.png}.")

    except Failure as e:
        print(f"\nFAIL: {e}", file=sys.stderr)
        return 1
    finally:
        if guest is not None:
            guest.close()

    total = splash_checks + bar_checks + key_checks
    print(f"guest: the display is {reported}")
    print(f"\nPASS: {total} display checks "
          f"({splash_checks} on the kernel's boot screen, {bar_checks} on what "
          f"Lua drew through gfx, {key_checks} on the keyboard).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
