#!/usr/bin/env python3
"""
Host-side display check.

`make test` proves the framebuffer from inside the guest: that it exists, is
page aligned, is writable to the last row, and that the padded stride really
moves rows. None of that proves a single pixel ever reaches a screen. The
guest cannot prove that - it can only prove what it wrote into its own
memory, and a wrong fourcc, a wrong stride in the ramfb config or a wrong
address would all leave those tests passing and the display black or garbled.

So this asks QEMU instead. It boots the image, waits for the guest to say it
brought the display up, and takes a screendump through the monitor - which is
QEMU's own view of the scanout, on the far side of everything this kernel
controls. Then it checks the picture.

What it checks, and why each one is the thing it is:

  - **The geometry matches what the guest reported over serial.** Two
    independent statements about the same thing, from opposite sides.
  - **A square white border.** If anything computes an offset as width * 4
    instead of using the pitch, the top and bottom edges still look right
    and the left and right ones shear into diagonals.
  - **Red, green and blue bars, in that order.** A wrong fourcc or channel
    order reverses them, and reversed primaries are unmistakable where a
    slightly-off colour is not.
  - **A gradient that increases downward.** Rows landing where the pitch
    says they should, along the whole height rather than at one point.

Usage: run_screenshot.py <image.elf> [--png OUT] [--timeout SECONDS]
"""

import argparse
import os
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

# What the kernel prints once hal_fb_init has succeeded. Waiting for it is
# what stops the screendump racing the boot: without it the dump lands on
# ramfb's own 640x480 placeholder and reports a black screen, which looks
# exactly like a broken driver.
READY = "video "


class Failure(Exception):
    """Something about the picture was wrong. The message is the report."""


def boot_and_capture(image, timeout):
    """Boot, wait for the display, and return (reported_line, ppm_bytes)."""
    with tempfile.TemporaryDirectory() as tmp:
        serial = os.path.join(tmp, "serial.txt")
        shot = os.path.join(tmp, "shot.ppm")
        open(serial, "w").close()

        try:
            proc = subprocess.Popen(
                [QEMU, *QEMU_ARGS,
                 "-serial", f"file:{serial}",
                 "-monitor", "stdio",
                 "-kernel", image],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, bufsize=1,
            )
        except FileNotFoundError:
            raise Failure(f"{QEMU} not found. See docs/setup.md.")

        try:
            deadline = time.monotonic() + timeout
            line = None

            while time.monotonic() < deadline:
                for text in open(serial).read().splitlines():
                    if text.startswith(READY):
                        line = text
                        break
                if line is not None:
                    break
                if proc.poll() is not None:
                    raise Failure("QEMU exited before the guest reported a display.")
                time.sleep(0.2)

            if line is None:
                raise Failure(
                    f"the guest never printed a \"{READY}\" line within {timeout}s. "
                    "It either did not boot or hal_fb_init returned false."
                )

            if "none" in line:
                raise Failure(
                    f"the guest reported no display: {line!r}. "
                    "ramfb was on the QEMU line, so fw_cfg or the etc/ramfb "
                    "item is not being found."
                )

            proc.stdin.write(f"screendump {shot}\n")
            proc.stdin.flush()

            # The monitor is synchronous, but the file appears when the write
            # completes rather than when the command is accepted.
            for _ in range(50):
                if os.path.exists(shot) and os.path.getsize(shot) > 0:
                    break
                time.sleep(0.1)
            else:
                raise Failure("the monitor never produced a screendump.")

            time.sleep(0.2)
            data = open(shot, "rb").read()
        finally:
            try:
                proc.stdin.write("quit\n")
                proc.stdin.flush()
            except Exception:
                pass
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

        return line, data


def parse_ppm(data):
    """P6, binary, maxval 255. Returns (width, height, pixels)."""
    parts = data.split(b"\n", 3)

    if len(parts) < 4 or parts[0] != b"P6":
        raise Failure("the screendump is not a binary PPM.")

    width, height = (int(v) for v in parts[1].split())

    if parts[2] != b"255":
        raise Failure(f"unexpected PPM maxval {parts[2]!r}.")

    return width, height, parts[3]


def check(reported, data):
    width, height, px = parse_ppm(data)

    def at(x, y):
        o = (y * width + x) * 3
        return tuple(px[o:o + 3])

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
        ("the top-left corner",            at(0, 0),                    white),
        ("the top-right corner",           at(width - 1, 0),            white),
        ("the bottom-left corner",         at(0, height - 1),           white),
        ("the bottom-right corner",        at(width - 1, height - 1),   white),
        ("the left edge at mid-height",    at(0, height // 2),          white),
        ("the right edge at mid-height",   at(width - 1, height // 2),  white),
        ("the first bar (red)",            at(third // 2, height // 2), (0xc0, 0x30, 0x30)),
        ("the second bar (green)",         at(width // 2, height // 2), (0x30, 0xc0, 0x30)),
        ("the third bar (blue)",           at(width - third // 2, height // 2), (0x30, 0x30, 0xc0)),
    ]

    for name, got, want in checks:
        if got != want:
            raise Failure(
                f"{name} is {got} and should be {want}.\n"
                "A sheared border means an offset computed as width * 4 "
                "instead of the pitch; bars in the wrong order mean the "
                "fourcc or the channel order is wrong."
            )

    ramp = [at(width // 2, y)[2] for y in (10, 100, 300, 700)]
    if not all(a < b for a, b in zip(ramp, ramp[1:])):
        raise Failure(
            f"the gradient does not increase down the screen: {ramp}. "
            "Rows are not landing where the pitch says they should."
        )

    return width, height, len(checks) + 1


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
    ap.add_argument("--png", help="also write the screenshot here, as a PNG")
    ap.add_argument("--timeout", type=float, default=30.0,
                    help="seconds to wait for the guest (default: 30)")
    args = ap.parse_args()

    try:
        reported, data = boot_and_capture(args.image, args.timeout)
        width, height, count = check(reported, data)
    except Failure as e:
        print(f"\nFAIL: {e}", file=sys.stderr)
        return 1

    if args.png:
        write_png(args.png, data)
        print(f"Wrote {args.png}.")

    print(f"guest: {reported}")
    print(f"\nPASS: {count}/{count} display checks at {width}x{height}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
