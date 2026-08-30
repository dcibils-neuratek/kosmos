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

  1. **The boot splash**, drawn by the kernel. Geometry against what the
     guest reported over serial, a square white border, red/green/blue bars
     in that order, and a gradient increasing downward.

  2. **A pattern drawn from Lua**, through gfx.screen() at the shell prompt.
     Vertical bars, which is the shape a pitch error destroys: each row would
     shift by (4160 - 4096) / 4 = sixteen pixels, turning every vertical line
     into a diagonal. The check reads each bar's x position at several
     heights and requires them all to be the same.

Usage: run_screenshot.py <image.elf> [--png OUT] [--timeout SECONDS]
"""

import argparse
import fcntl
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
]

READY = "video "            # printed once hal_fb_init has succeeded
PROMPT = "kosmos>"          # printed once the shell is serving

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


def check_splash(reported, data):
    """Phase one: what the kernel drew at boot."""
    width, height, at = pixel_reader(data)

    # The guest's line reads: "video 1024x768, pitch 4160 bytes, at 0x..."
    geometry = reported.split()[1].rstrip(",")
    if geometry != f"{width}x{height}":
        raise Failure(
            f"the guest reported {geometry} and QEMU is scanning out "
            f"{width}x{height}. The ramfb config and the framebuffer "
            "disagree about the geometry."
        )

    white = (255, 255, 255)
    third = width // 3

    checks = [
        ("the top-left corner",          at(0, 0),                    white),
        ("the top-right corner",         at(width - 1, 0),            white),
        ("the bottom-left corner",       at(0, height - 1),           white),
        ("the bottom-right corner",      at(width - 1, height - 1),   white),
        ("the left edge at mid-height",  at(0, height // 2),          white),
        ("the right edge at mid-height", at(width - 1, height // 2),  white),
        ("the first bar (red)",          at(third // 2, height // 2), (0xc0, 0x30, 0x30)),
        ("the second bar (green)",       at(width // 2, height // 2), (0x30, 0xc0, 0x30)),
        ("the third bar (blue)",         at(width - third // 2, height // 2), (0x30, 0x30, 0xc0)),
    ]

    for name, got, want in checks:
        if got != want:
            raise Failure(
                f"splash: {name} is {got} and should be {want}.\n"
                "A sheared border means an offset computed as width * 4 "
                "instead of the pitch; bars in the wrong order mean the "
                "fourcc or the channel order is wrong."
            )

    ramp = [at(width // 2, y)[2] for y in (10, 100, 300, 700)]
    if not all(a < b for a, b in zip(ramp, ramp[1:])):
        raise Failure(
            f"splash: the gradient does not increase down the screen: {ramp}. "
            "Rows are not landing where the pitch says they should."
        )

    return len(checks) + 1


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

        guest.wait_for(READY, "reported a display")
        reported = guest.line_starting(READY)

        if "none" in reported:
            raise Failure(
                f"the guest reported no display: {reported!r}. "
                "ramfb was on the QEMU line, so fw_cfg or the etc/ramfb "
                "item is not being found."
            )

        splash_checks = check_splash(reported, guest.screendump())

        guest.wait_for(PROMPT, "reached the shell prompt")
        guest.type(DRAW)
        guest.wait_for("drawn", "finished drawing")

        drawn = guest.screendump()
        bar_checks = check_bars(drawn)

        if args.png:
            write_png(args.png, drawn)
            print(f"Wrote {args.png}.")

    except Failure as e:
        print(f"\nFAIL: {e}", file=sys.stderr)
        return 1
    finally:
        if guest is not None:
            guest.close()

    total = splash_checks + bar_checks
    print(f"guest: {reported}")
    print(f"\nPASS: {total} display checks "
          f"({splash_checks} on the kernel's splash, {bar_checks} on what "
          f"Lua drew through gfx).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
