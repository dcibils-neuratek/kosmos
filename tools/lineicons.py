#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The line icons, rendered from the mockups' own vectors.

    python3 tools/lineicons.py            # writes assets/icons/line/

`docs/preferences.html` draws a sidebar whose categories each have a small
line icon - grey, and the accent for the one that is chosen - and a header
with a search, a menu and a close. Diego, 24 September 2026: "pixel perfect
as the html mockups" (`roadmap.md` 5zp).

**The vectors are the source, and the pictures are what the build carries.**
Each icon here is the exact SVG the page draws, and each is rendered by a
real browser at every size the desktop's scale can ask for - 15 at 100 per
cent, 19 at 125, 23 at 150, 30 at 200 - rather than rendered once and
resampled, because a resampled one-pixel line is a grey smear. The kit
paints a look's colour through each one's coverage (`gfx.c`'s `tint`), so
one picture per size serves five looks.

**Run by hand when an icon changes, not by the build**, because it needs a
browser and the build must not. What it writes is committed, as a font's
BDF is: an input the build converts, and whose origin is written down here.

Each PNG is white with the icon's coverage as its alpha - the colour is
never in the file, which is the point.
"""

import os
import struct
import subprocess
import sys
import zlib

import scratch

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "icons", "line")
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

# The sizes the desktop's scale turns 15 points into: `scale.px(15, pct)`
# at 100, 125, 150 and 200 per cent.
SIZES = (15, 19, 23, 30)

# name -> (stroke width in the 16-unit box, the SVG body), exactly as
# `docs/preferences.html` has them. Categories are stroked at 1.4 with round
# joins; the header's icons at 1.6.
CAT = ('1.4', 'stroke-linejoin="round"')
HEAD = ('1.6', '')
PLACE = ('1.5', '')

ICONS = {
    "appearance": CAT + ('<path d="M8 2a6 6 0 100 12c1 0 1.5-.6 1.5-1.3 0-.8'
                         '-.7-1.1-.7-1.8 0-.5.4-.9 1-.9H11a3 3 0 003-3c0-2.8'
                         '-2.7-5-6-5z"/>',),
    "display":    CAT + ('<path d="M2 3h12v9H2zM6 14h4"/>',),
    "sound":      CAT + ('<path d="M3 6v4h2.5L9 13V3L5.5 6zM11.5 6a3 3 0 010 4"/>',),
    "power":      CAT + ('<path d="M8 2v6M5 4a5 5 0 106 0"/>',),
    "network":    CAT + ('<path d="M2 6a9 9 0 0112 0M4.5 8.5a5.5 5.5 0 017 0'
                         'M8 12h.01"/>',),
    "keyboard":   CAT + ('<path d="M2 4h12v8H2zM4.5 6.5h.01M7 6.5h.01'
                         'M9.5 6.5h.01M12 6.5h.01M5 9.5h6"/>',),
    "startup":    CAT + ('<path d="M8 2l5 3v6l-5 3-5-3V5z"/>',),
    "datetime":   CAT + ('<path d="M8 2a6 6 0 100 12A6 6 0 008 2zM8 5v3.2'
                         'l2 1.2"/>',),
    "system":     CAT + ('<path d="M5 5h6v6H5zM8 2v2M8 12v2M2 8h2M12 8h2"/>',),
    "search":     HEAD + ('<circle cx="7" cy="7" r="4.5"/>'
                          '<path d="M10.5 10.5 14 14"/>',),
    "menu":       HEAD + ('<path d="M2 4h12M2 8h12M2 12h12"/>',),
    "close":      HEAD + ('<path d="M4 4l8 8M12 4l-8 8"/>',),

    # `docs/tracker2.html`: the header's arrows at 1.7, the places and the
    # new folder at 1.5, and the three dots filled rather than stroked.
    "back":       ('1.7', '') + ('<path d="M10 3L5 8l5 5"/>',),
    "forward":    ('1.7', '') + ('<path d="M6 3l5 5-5 5"/>',),
    "more":       ('0', '') + ('<g fill="#000" stroke="none"><circle cx="8" '
                               'cy="3" r="1.3"/><circle cx="8" cy="8" r="1.3"/>'
                               '<circle cx="8" cy="13" r="1.3"/></g>',),
    "home":       PLACE + ('<path d="M2 7l6-5 6 5v7H2z"/>',),
    "newfolder":  PLACE + ('<path d="M2 4h4l1.5 2H14v7H2z"/>'
                           '<path d="M8 8.5v3M6.5 10h3"/>',),
    # The browser's reload: a circle nearly closed, and the arrow's head.
    "reload":     HEAD + ('<path d="M13.2 8.6A5.3 5.3 0 1 1 11.8 4.1"/>'
                          '<path d="M12.6 1.8v3h-3"/>',),
    # The new folder's folder without its plus: Tracker's place button,
    # anywhere that is not Home, the Trash or a drive.
    "folder":     PLACE + ('<path d="M2 4h4l1.5 2H14v7H2z"/>',),
    "recent":     PLACE + ('<circle cx="8" cy="8" r="6"/>'
                           '<path d="M8 5v3.2l2 1.2"/>',),
    "trash":      PLACE + ('<path d="M3 5h10v9H3zM6 5V3h4v2"/>',),
    "document":   PLACE + ('<path d="M3 2h7l3 3v9H3z"/>',),
    "music":      PLACE + ('<path d="M6 12V4l7-1v8"/><circle cx="4" cy="12" '
                           'r="2"/><circle cx="11" cy="11" r="2"/>',),
    "pictures":   PLACE + ('<path d="M2 3h12v10H2z"/>'
                           '<path d="M2 11l4-4 3 3 2-2 3 3"/>',),
    "drive":      PLACE + ('<path d="M2 4h12v8H2z"/><path d="M4 7h3"/>',),

    # The kit's checkbox, ticked: white on the accent, so heavier than the
    # rest and with round ends - at 15 pixels a 1.5 stroke on a filled box
    # reads as a scratch rather than a mark.
    "check":      ('2', 'stroke-linecap="round" stroke-linejoin="round"')
                  + ('<path d="M4 8.5l2.6 2.6L12 5.2"/>',),
}

STEP = 40          # one cell per icon on the sheet, wider than the largest


def sheet(size):
    """One page with every icon at `size`, in a row, black on nothing."""
    cells = []

    for i, (name, (width, extra, body)) in enumerate(sorted(ICONS.items())):
        cells.append(
            '<svg style="position:absolute;left:%dpx;top:0" width="%d" '
            'height="%d" viewBox="0 0 16 16" fill="none" stroke="#000" '
            'stroke-width="%s" %s>%s</svg>'
            % (i * STEP, size, size, width, extra, body))

    return ('<!doctype html><html><head><style>html,body{margin:0;'
            'background:transparent}</style></head><body>%s</body></html>'
            % "".join(cells))


def rgba_rows(path):
    """A PNG's rows as RGBA bytearrays, undoing the filters."""
    data = open(path, "rb").read()
    pos, idat = 8, b""
    width = height = 0

    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        tag, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]

        if tag == b"IHDR":
            width, height, depth, kind = struct.unpack(">IIBB", body[:10])

            if depth != 8 or kind != 6:
                sys.exit("lineicons: the browser gave a PNG that is not "
                         "8-bit RGBA (%d, %d)" % (depth, kind))
        elif tag == b"IDAT":
            idat += body

        pos += 12 + n

    raw = zlib.decompress(idat)
    stride = width * 4
    prev = bytearray(stride)
    rows = []

    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])

        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0

            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc
                                      else b if pb <= pc else c)) & 255

        prev = line
        rows.append(line)

    return width, height, rows


def write_mask(path, size, rows, x0):
    """White, with the icon's coverage as alpha."""
    raw = bytearray()

    for y in range(size):
        raw.append(0)

        for x in range(size):
            raw += bytes((255, 255, 255, rows[y][(x0 + x) * 4 + 3]))

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body)))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size,
                                             8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
                + chunk(b"IEND", b""))


def main():
    if not os.path.exists(CHROME):
        sys.exit("lineicons: needs Google Chrome at %s, to render the "
                 "vectors the way the mockups are rendered" % CHROME)

    os.makedirs(OUT, exist_ok=True)
    names = sorted(ICONS)
    written = 0

    # Through `scratch`, as every tool here makes its temporary files: it is
    # the one place that removes them when the tool is done.
    tmp = scratch.directory("lineicons")

    for size in SIZES:
        page = os.path.join(tmp, "sheet%d.html" % size)
        shot = os.path.join(tmp, "sheet%d.png" % size)

        with open(page, "w") as f:
            f.write(sheet(size))

        subprocess.run([CHROME, "--headless=new", "--disable-gpu",
                        "--hide-scrollbars",
                        "--force-device-scale-factor=1",
                        "--default-background-color=00000000",
                        "--window-size=%d,%d" % (len(names) * STEP, size),
                        "--screenshot=" + shot, "file://" + page],
                       stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=True)

        _, _, rows = rgba_rows(shot)

        for i, name in enumerate(names):
            write_mask(os.path.join(OUT, "%s-%d.png" % (name, size)),
                       size, rows, i * STEP)
            written += 1

    print("lineicons: %d icons at %s pixels into %s"
          % (len(names), ", ".join(map(str, SIZES)),
             os.path.relpath(OUT, ROOT)))
    return 0 if written else 1


if __name__ == "__main__":
    sys.exit(main())
