#!/usr/bin/env python3
"""
The test pattern in assets/images/, generated.

Deliberately awkward, because a decoder is only tested by the things it has
to cope with:

  * **RGBA, not RGB.** Four channels, so a decoder that assumed three
    produces a smear rather than a picture.
  * **All five row filters**, one per scanline in rotation. PNG lets each
    row choose, and real encoders use all of them; a decoder that only
    implements "none" reads a file like this as noise, immediately, rather
    than reading somebody's photograph wrongly a month later.
  * **A translucent corner**, so alpha is carried through rather than
    assumed opaque.

Run it when the pattern needs changing. The output is committed, because a
test image that is regenerated on every build is a test image nobody
notices changing.

Usage: mkpattern.py [out.png]
"""

import math
import struct
import sys
import zlib

WIDTH = 240
HEIGHT = 160


def chunk(tag, data):
    body = tag + data
    return (struct.pack(">I", len(data)) + body
            + struct.pack(">I", zlib.crc32(body) & 0xffffffff))


def pixel(x, y):
    """A blue disc on a checkerboard, with one translucent corner."""
    if math.hypot(x - WIDTH * 0.35, y - HEIGHT * 0.5) < 46:
        rgb = (0x1f, 0x6f, 0xeb)
    elif (x // 16 + y // 16) % 2 == 0:
        rgb = (0x16, 0x1b, 0x22)
    else:
        rgb = (0xff, 0xc7, 0x00)

    alpha = 90 if (x > WIDTH - 40 and y < 40) else 255
    return rgb + (alpha,)


def filtered(row, previous, kind):
    """One scanline, encoded with PNG filter `kind`."""
    out = bytearray()

    for i, current in enumerate(row):
        left = row[i - 4] if i >= 4 else 0
        up = previous[i]
        upleft = previous[i - 4] if i >= 4 else 0

        if kind == 0:
            value = current
        elif kind == 1:
            value = current - left
        elif kind == 2:
            value = current - up
        elif kind == 3:
            value = current - ((left + up) // 2)
        else:
            estimate = left + up - upleft
            dl = abs(estimate - left)
            du = abs(estimate - up)
            dul = abs(estimate - upleft)
            best = left if (dl <= du and dl <= dul) else (up if du <= dul
                                                          else upleft)
            value = current - best

        out.append(value & 0xff)

    return out


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "assets/images/test-pattern.png"

    body = bytearray()
    previous = bytes(WIDTH * 4)

    for y in range(HEIGHT):
        row = bytearray()

        for x in range(WIDTH):
            row += bytes(pixel(x, y))

        kind = y % 5                    # every filter, in rotation
        body += bytes([kind]) + filtered(row, previous, kind)
        previous = bytes(row)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT,
                                        8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(body), 9))
           + chunk(b"IEND", b""))

    with open(out_path, "wb") as f:
        f.write(png)

    print(f"{out_path}: {WIDTH}x{HEIGHT} RGBA, {len(png)} bytes, "
          f"all five row filters")


if __name__ == "__main__":
    main()
