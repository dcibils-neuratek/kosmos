#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""A PNG's pixels on the host, with nothing but the standard library.

`pixels(path)` -> (width, height, rows): each row a bytes of red, green and
blue, three bytes a pixel, whatever the file held - eight-bit RGB or RGBA,
the alpha dropped. Enough for a suite to hold a picture the guest made to
one somebody else made: what both say about each pixel, as a value.

Written once, for W6c's pictures (`tools/run_webcore.py`); `lineicons.py`
and `run_cafesa3d.py` read PNGs their own way, and are to come here
(roadmap).
"""

import struct
import zlib

CHANNELS = {2: 3, 6: 4}         # PNG's colour types: RGB, RGBA


def pixels(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s is not a PNG" % path)

    pos, idat = 8, b""
    width = height = channels = 0
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        tag, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        if tag == b"IHDR":
            width, height, depth, kind = struct.unpack(">IIBB", body[:10])
            interlace = body[12]
            if depth != 8 or kind not in CHANNELS or interlace:
                raise ValueError("%s: depth %d, colour type %d, interlace %d - "
                                 "only eight-bit RGB and RGBA, not interlaced"
                                 % (path, depth, kind, interlace))
            channels = CHANNELS[kind]
        elif tag == b"IDAT":
            idat += body
        pos += 12 + n

    raw = zlib.decompress(idat)
    stride = width * channels
    prev = bytearray(stride)
    rows = []
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        # The five filters, undone a byte at a time against the byte a
        # pixel to the left and the one above (PNG's specification, 9.2).
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc
                                      else b if pb <= pc else c)) & 255
        prev = line
        if channels == 3:
            rows.append(bytes(line))
        else:
            rows.append(bytes(b for i, b in enumerate(line) if i % 4 != 3))
    return width, height, rows
