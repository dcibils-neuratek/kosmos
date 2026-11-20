#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The pictures `tools/test_picture_decode.c` decodes, and what each must
decode to - made with nothing but Python's standard library and `sips`.

`python3 tools/picture_fixtures.py OUTDIR` writes into OUTDIR:

  - PNGs written here, every pixel known before the file exists: each colour
    type gfx reads (grey, RGB, a palette with transparency, RGBA), every
    one of the five filters, widths either side of the vector paths'
    sixteen bytes, the image data split over several IDAT chunks, and a
    terrarium elevation tile as Flight's terrain fetches them;
  - the PNGs in `assets/`, with their pixels worked out by a decoder in
    this file - a second implementation, written from the specification
    and sharing nothing with `picture_decode.c`;
  - the JPEGs in `assets/`, with their pixels as macOS's own decoder gives
    them (`sips` to a BMP), which a different IDCT and chroma upsampling
    make close rather than equal;
  - files gfx must refuse, each with the code and the words it must say;
  - `manifest.txt`, one line a check, which the C test reads.

Each `.argb` is the pixels as the decoder must return them: 0xAARRGGBB
words, little-endian, rows of exactly the width.
"""

import os
import random
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# --- writing a PNG ---------------------------------------------------------

def chunk(tag, body):
    return (struct.pack(">I", len(body)) + tag + body
            + struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff))


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def filter_row(kind, line, prev, bpp):
    """The row as an encoder writes it under filter `kind` (PNG 9.2)."""
    out = bytearray(len(line))
    for i, x in enumerate(line):
        a = line[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        pred = (0, a, b, (a + b) // 2, paeth(a, b, c))[kind]
        out[i] = (x - pred) & 255
    return bytes(out)


def best_filter(line, prev, bpp):
    """The filter an encoder's usual heuristic picks: the least sum of the
    filtered bytes taken as signed. What a real tile's rows look like."""
    def cost(kind):
        return sum(v if v < 128 else 256 - v
                   for v in filter_row(kind, line, prev, bpp))
    return min(range(5), key=cost)


def write_png(path, width, height, colour, rows, filters, plte=None,
              trns=None, split=1, depth=8, interlace=0, raw_override=None):
    """`rows` are the unfiltered bytes; `filters(y, line, prev)` names each
    row's filter. `raw_override` replaces the inflated data, for a file
    that must lie about its size."""
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    stride = width * bpp
    raw = bytearray()
    prev = bytes(stride)
    for y, line in enumerate(rows):
        kind = filters(y, line, prev)
        raw.append(kind)
        raw += filter_row(kind, line, prev, bpp)
        prev = line
    if raw_override is not None:
        raw = raw_override
    z = zlib.compress(bytes(raw), 9)
    data = b"\x89PNG\r\n\x1a\n"
    data += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth,
                                       colour, 0, 0, interlace))
    if plte is not None:
        data += chunk(b"PLTE", plte)
    if trns is not None:
        data += chunk(b"tRNS", trns)
    step = max(1, (len(z) + split - 1) // split)
    for at in range(0, len(z), step):
        data += chunk(b"IDAT", z[at:at + step])
    data += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


def write_argb(path, words):
    with open(path, "wb") as f:
        f.write(struct.pack("<%dI" % len(words), *words))


# --- reading a PNG, independently ------------------------------------------

def read_png(path):
    """(width, height, words) for an eight-bit, non-interlaced PNG of
    colour type 0, 2, 3 or 6 - the same set gfx reads, worked out here a
    byte at a time from the specification."""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat, plte, trns = 8, b"", b"", b""
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        tag, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        if tag == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and body[12] == 0, path
        elif tag == b"PLTE":
            plte = body
        elif tag == b"tRNS":
            trns = body
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
        pos += 12 + n
    bpp = {0: 1, 2: 3, 3: 1, 6: 4}[colour]
    stride = width * bpp
    raw = zlib.decompress(idat)
    prev = bytearray(stride)
    words = []
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            line[i] = (line[i] + (0, a, b, (a + b) // 2,
                                  paeth(a, b, c))[kind]) & 255
        prev = line
        for x in range(width):
            p = line[x * bpp:(x + 1) * bpp]
            if colour == 0:
                r = g = b_ = p[0]
                al = 255
            elif colour == 3:
                i = p[0]
                if i * 3 + 2 < len(plte):
                    r, g, b_ = plte[i * 3:i * 3 + 3]
                    al = trns[i] if i < len(trns) else 255
                else:
                    r = g = b_ = 0
                    al = 255
            else:
                r, g, b_ = p[0], p[1], p[2]
                al = p[3] if bpp == 4 else 255
            words.append(al << 24 | r << 16 | g << 8 | b_)
    return width, height, words


# --- reading a BMP that sips wrote -----------------------------------------

def read_bmp(path):
    data = open(path, "rb").read()
    offset = struct.unpack("<I", data[10:14])[0]
    width, height = struct.unpack("<ii", data[18:26])
    bits = struct.unpack("<H", data[28:30])[0]
    assert bits in (24, 32), (path, bits)
    bpp = bits // 8
    pitch = (width * bpp + 3) & ~3
    top_down = height < 0
    height = abs(height)
    words = []
    for y in range(height):
        row = y if top_down else height - 1 - y
        at = offset + row * pitch
        for x in range(width):
            b_, g, r = data[at + x * bpp:at + x * bpp + 3]
            words.append(0xff000000 | r << 16 | g << 8 | b_)
    return width, height, words


# --- the fixtures ----------------------------------------------------------

def main(out):
    os.makedirs(out, exist_ok=True)
    rng = random.Random(20261120)
    lines = []

    def png_case(name, width, height, colour, filters, plte=None, trns=None,
                 split=1, pixel=None):
        """A PNG of known pixels: `pixel(x, y)` gives the unfiltered bytes
        of a pixel and the word it must decode to."""
        rows, words = [], []
        for y in range(height):
            line = bytearray()
            for x in range(width):
                raw, word = pixel(x, y)
                line += raw
                words.append(word)
            rows.append(bytes(line))
        path = os.path.join(out, name + ".png")
        write_png(path, width, height, colour, rows, filters, plte, trns,
                  split)
        write_argb(os.path.join(out, name + ".argb"), words)
        lines.append("png %s.png %s.argb" % (name, name))

    def noise_rgba(x, y):
        r, g, b, a = (rng.randrange(256) for _ in range(4))
        return bytes((r, g, b, a)), a << 24 | r << 16 | g << 8 | b

    def noise_rgb(x, y):
        r, g, b = (rng.randrange(256) for _ in range(3))
        return bytes((r, g, b)), 0xff000000 | r << 16 | g << 8 | b

    def noise_grey(x, y):
        v = rng.randrange(256)
        return bytes((v,)), 0xff000000 | v * 0x010101

    plte = bytes(rng.randrange(256) for _ in range(200 * 3))
    trns = bytes(rng.randrange(256) for _ in range(77))

    def noise_index(x, y):
        # Some indices past the palette: black and opaque, as gfx says.
        i = rng.randrange(210)
        if i >= 200:
            return bytes((i,)), 0xff000000
        a = trns[i] if i < len(trns) else 255
        r, g, b = plte[i * 3:i * 3 + 3]
        return bytes((i,)), a << 24 | r << 16 | g << 8 | b

    # Every filter, a row each in turn, so each one meets the first row
    # and each one meets a row above - and every filter alone over a whole
    # picture, so the vector paths run their full width.
    def cycle(y, line, prev):
        return y % 5

    for width in (1, 3, 4, 5, 16, 17, 64, 255):
        png_case("rgba-cycle-%d" % width, width, 11, 6, cycle,
                 pixel=noise_rgba)
    for kind in range(5):
        png_case("rgba-only%d" % kind, 67, 9, 6,
                 lambda y, l, p, k=kind: k, pixel=noise_rgba)
        png_case("rgb-only%d" % kind, 33, 7, 2,
                 lambda y, l, p, k=kind: k, pixel=noise_rgb)
        png_case("grey-only%d" % kind, 29, 6, 0,
                 lambda y, l, p, k=kind: k, pixel=noise_grey)
        png_case("palette-only%d" % kind, 31, 6, 3,
                 lambda y, l, p, k=kind: k, plte=plte, trns=trns,
                 pixel=noise_index)
    png_case("rgb-split", 40, 30, 2, cycle, split=7, pixel=noise_rgb)
    png_case("one-pixel", 1, 1, 6, cycle, pixel=noise_rgba)

    # A terrarium tile (`docs/flight.md`): height in metres as
    # (R * 256 + G + B / 256) - 32768, a smooth ground over 256 by 256,
    # filtered the way an encoder filters one.
    def terrarium(x, y):
        import math
        h = (1200 + 900 * math.sin(x / 23.0) * math.cos(y / 31.0)
             + 0.37 * x * y / 7.0)
        v = int(round((h + 32768) * 256))
        r, g, b = (v >> 16) & 255, (v >> 8) & 255, v & 255
        return bytes((r, g, b)), 0xff000000 | r << 16 | g << 8 | b

    png_case("terrarium", 256, 256, 2,
             lambda y, line, prev: best_filter(line, prev, 3),
             pixel=terrarium)

    # The pictures the system carries.
    for rel in ("assets/images/test-pattern.png", "assets/images/image00.png",
                "assets/icons/App_About.png", "assets/icons/App_Clock.png"):
        width, height, words = read_png(os.path.join(ROOT, rel))
        name = "asset-" + os.path.basename(rel)[:-4]
        write_argb(os.path.join(out, name + ".argb"), words)
        lines.append("png %s %s.argb" % (os.path.join(ROOT, rel), name))

    for rel in ("assets/images/test-quads.jpg",
                "assets/images/test-screen.jpg",
                "assets/wallpapers/cloud-pigment.jpg",
                "assets/wallpapers/white-turbine.jpg"):
        name = "asset-" + os.path.basename(rel)[:-4]
        bmp = os.path.join(out, name + ".bmp")
        subprocess.run(["sips", "-s", "format", "bmp",
                        os.path.join(ROOT, rel), "--out", bmp],
                       check=True, stdout=subprocess.DEVNULL)
        width, height, words = read_bmp(bmp)
        write_argb(os.path.join(out, name + ".argb"), words)
        lines.append("jpeg %s %s.argb %d %d"
                     % (os.path.join(ROOT, rel), name, width, height))

    # And what must be refused, with the code and the words.
    def refuse(name, fmt, code, words, **kw):
        path = os.path.join(out, name)
        write_png(path, **kw)
        lines.append("refuse %s %s %d %s" % (fmt, name, code, words))

    tiny = [bytes(4 * 3)] * 2
    zero = lambda y, l, p: 0
    refuse("sixteen.png", "png", -1, "only eight bits a channel",
           width=3, height=2, colour=6, rows=tiny, filters=zero, depth=16)
    refuse("interlaced.png", "png", -1, "interlaced",
           width=3, height=2, colour=6, rows=tiny, filters=zero, interlace=1)
    refuse("grey-alpha.png", "png", -1, "grey with alpha",
           width=3, height=2, colour=4, rows=[bytes(6)] * 2, filters=zero)
    refuse("no-palette.png", "png", -1, "no palette",
           width=3, height=2, colour=3, rows=[bytes(3)] * 2, filters=zero)
    refuse("bad-filter.png", "png", -1, "not one of the five",
           width=3, height=2, colour=6, rows=tiny, filters=zero,
           raw_override=bytes([0]) + bytes(12) + bytes([5]) + bytes(12))
    refuse("short-data.png", "png", -1, "different size",
           width=3, height=2, colour=6, rows=tiny, filters=zero,
           raw_override=bytes(13))
    refuse("long-data.png", "png", -1, "different size",
           width=3, height=2, colour=6, rows=tiny, filters=zero,
           raw_override=bytes(40))
    refuse("too-wide.png", "png", -1, "larger than",
           width=9000, height=1, colour=0, rows=[bytes(9000)], filters=zero)

    # A good PNG cut short inside its image data, as a download that
    # stopped is.
    with open(os.path.join(out, "terrarium.png"), "rb") as f:
        whole = f.read()
    with open(os.path.join(out, "truncated.png"), "wb") as f:
        f.write(whole[:100])
    lines.append("refuse png truncated.png -1 runs past the end")
    with open(os.path.join(out, "not-png.bin"), "wb") as f:
        f.write(b"GIF89a this is not one")
    lines.append("refuse png not-png.bin -2 not a PNG")
    lines.append("refuse jpeg not-png.bin -2 not a JPEG")
    lines.append("refuse jpeg %s -2 not a JPEG"
                 % os.path.join(ROOT, "assets/images/test-pattern.png"))
    with open(os.path.join(ROOT, "assets/images/test-screen.jpg"), "rb") as f:
        jpeg = f.read()
    # A JPEG that stops before its frame header, which stb refuses. One
    # cut after the header is not here: stb decodes what it has and fills
    # the rest, as it always did, and that is not a refusal.
    with open(os.path.join(out, "jpeg-header-only.jpg"), "wb") as f:
        f.write(jpeg[:20])
    lines.append("refuse jpeg jpeg-header-only.jpg -1 -")

    with open(os.path.join(out, "manifest.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("%d checks in %s" % (len(lines), out))


if __name__ == "__main__":
    main(sys.argv[1])
