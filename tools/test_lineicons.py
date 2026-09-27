#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The line icons, held to their list and to what the code asks for.

    python3 tools/test_lineicons.py

`tools/lineicons.py` names every small grey icon Kosmos draws - Kosmos's
name beside Lucide's - and renders each at the four sizes the desktop's
scale asks for into `assets/icons/line/`. It is run by hand, because it needs
a browser, so nothing in the build would notice an icon named in the code
and never rendered: `gc:line_icon` would draw nothing, which reads as a gap
rather than as a mistake. So this checks, on this machine:

- every icon in the list has its Lucide file, which is where it came from;
- every icon has its four pictures, each an 8-bit white RGBA square of its
  size with something in it - coverage alone, which is what lets the kit
  paint it any colour;
- no picture is there that the list does not name, left from an old icon;
- and every icon name written in Kosmos's Lua - a literal to `line_icon`, or
  an `icon = "..."` in lower case, which is a line icon's by the kit's
  convention, since an application's icon is `App_...` - is in the list.
"""

import glob
import importlib.util
import os
import re
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

spec = importlib.util.spec_from_file_location(
    "lineicons", os.path.join(ROOT, "tools", "lineicons.py"))
lineicons = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lineicons)

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)


def picture(path):
    """(width, height, colour type, depth, pixels) of a PNG, unfiltered
    only as far as the checks need: the alpha and the colour of each
    pixel, from rows written with filter 0, which is how `lineicons.py`
    writes them."""
    data = open(path, "rb").read()

    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None

    pos, idat, head = 8, b"", None

    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        tag, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]

        if tag == b"IHDR":
            head = struct.unpack(">IIBB", body[:10])
        elif tag == b"IDAT":
            idat += body

        pos += 12 + n

    if head is None:
        return None

    w, h, depth, kind = head
    raw = zlib.decompress(idat)
    stride = w * 4
    rows = [raw[y * (stride + 1):(y + 1) * (stride + 1)] for y in range(h)]
    return w, h, depth, kind, rows


def main():
    icons = lineicons.ICONS
    out = lineicons.OUT

    for name, (lucide, _) in sorted(icons.items()):
        check(os.path.exists(os.path.join(lineicons.LUCIDE, lucide + ".svg")),
              "%s: Lucide has no %s.svg in assets/icons/lucide/" % (name, lucide))

        for size in lineicons.SIZES:
            path = os.path.join(out, "%s-%d.png" % (name, size))

            if not os.path.exists(path):
                check(False, "%s: no %d-pixel picture - run tools/lineicons.py"
                      % (name, size))
                continue

            got = picture(path)

            if got is None:
                check(False, "%s-%d.png is not a PNG" % (name, size))
                continue

            w, h, depth, kind, rows = got
            check((w, h, depth, kind) == (size, size, 8, 6),
                  "%s-%d.png is %dx%d, depth %d, colour type %d - not an "
                  "8-bit RGBA square of its size" % (name, size, w, h, depth, kind))

            covered, white = 0, True

            for row in rows:
                if row[0] != 0:
                    white = False       # a filter this checker does not undo
                    break

                for x in range(w):
                    r, g, b, a = row[1 + x * 4:5 + x * 4]
                    covered += a > 0
                    white = white and (r, g, b) == (255, 255, 255)

            check(white, "%s-%d.png is not white with its coverage as alpha"
                  % (name, size))
            check(covered > size, "%s-%d.png has nothing in it (%d pixels)"
                  % (name, size, covered))

    named = {"%s-%d.png" % (n, s) for n in icons for s in lineicons.SIZES}

    for path in sorted(glob.glob(os.path.join(out, "*.png"))):
        check(os.path.basename(path) in named,
              "%s is in assets/icons/line/ and the list does not name it"
              % os.path.basename(path))

    used = set()

    for path in glob.glob(os.path.join(ROOT, "user", "**", "*.lua"), recursive=True):
        text = open(path, encoding="utf-8", errors="replace").read()
        used |= set(re.findall(r'line_icon\([^)\n]*?"([a-z_]+)"', text))
        used |= set(re.findall(r'\bicon\s*=\s*"([a-z][a-z_]*)"', text))

    for name in sorted(used):
        check(name in icons, "the Lua names a line icon %r that tools/lineicons.py "
              "does not render" % name)

    if failures:
        print("FAIL: %d of %d checks on the line icons:" % (len(failures), checks))
        for f in failures:
            print("  " + f)
        return 1

    print("PASS: %d checks on the line icons (%d icons from Lucide, each at %s "
          "pixels, white with its coverage, nothing stray, and all %d names the "
          "Lua uses among them)" % (checks, len(icons),
                                   ", ".join(map(str, lineicons.SIZES)), len(used)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
