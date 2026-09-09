#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
A BDF bitmap font, as a C array.

BDF is the interchange format every bitmap font is distributed in: plain
text, one hex byte per pixel row, and simple enough that parsing it is
cheaper than depending on a library to. The font is vendored unmodified
under assets/fonts/ for the same reason `lua/upstream/` is - the thing in
the tree should be what the author shipped, and anything we do to it should
be a build step that can be read.

The output is the layout the hardware wants and every 8x16 font uses:

    one byte per row, sixteen rows per glyph, MSB is the leftmost pixel

so glyph `c` starts at (c - first) * height and row `r` of it is one byte
whose bit 0x80 is the pixel at x = 0. That is the VGA ROM layout, and it is
what makes the rasteriser a shift and a test rather than a lookup.

Printable ASCII, then the Block Elements, and nothing else. The font has a
thousand glyphs and carrying all of them would be sixteen kilobytes.

The two ranges are chosen rather than convenient. ASCII is what anyone can
type. U+2580 to U+259F is what ASCII art is drawn with - the full and
fractional blocks and the three shades - and it is a whole Unicode block, so
"which of them" is not a judgement anybody has to make again. Together they
are 127 glyphs, about two kilobytes.

A second array, `<symbol>_extra`, gives the codepoint of each glyph past the
ASCII run, in order, because those are not contiguous with it. The console
searches it; there are 32 entries and only a non-ASCII character ever looks.

A glyph is appended between the two for anything in neither: a hollow box, so
a character the font does not have is visibly a character the font does not
have rather than a space.

Usage: bdf2c.py <font.bdf> <symbol> <out.c>
"""

import re
import sys


def parse(path):
    """Every glyph in the file, as {codepoint: [row bytes]}.

    BDF puts the width in FONTBOUNDINGBOX and per-glyph in BBX, and pads each
    row to a whole number of bytes. Only 8-pixel-wide fonts are accepted, so
    a row is exactly one byte and there is no padding to reason about - a
    wider font would need the rows unpacked, and silently taking the first
    byte of each would produce a font that is subtly wrong rather than one
    that fails.
    """
    glyphs = {}
    codepoint = None
    rows = None

    with open(path, errors="replace") as f:
        for line in f:
            line = line.strip()

            if line.startswith("FONTBOUNDINGBOX"):
                width = int(line.split()[1])
                if width != 8:
                    raise SystemExit(
                        f"{path}: this converter only handles 8-pixel-wide "
                        f"fonts, and this one is {width}. A wider one packs "
                        f"each row into more than a byte and needs unpacking."
                    )

            elif line.startswith("ENCODING "):
                codepoint = int(line.split()[1])

            elif line == "BITMAP":
                rows = []

            elif line == "ENDCHAR":
                if codepoint is not None and rows is not None:
                    glyphs[codepoint] = rows
                codepoint, rows = None, None

            elif rows is not None and re.fullmatch(r"[0-9A-Fa-f]+", line):
                rows.append(int(line[:2], 16))

    return glyphs


def provenance(path):
    """The COMMENT block at the top, which is where the font says who it is."""
    out = []

    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("COMMENT"):
                text = line[len("COMMENT"):].strip()
                if text and text not in ("/*", "*/"):
                    out.append(text.lstrip("* ").rstrip())
            elif line.startswith("FONT ") or out and not line.startswith("COMMENT"):
                if line.startswith("FONT "):
                    out.append(line.strip())
                break

    return [l for l in out if l]


FIRST, LAST, HEIGHT = 0x20, 0x7E, 16

# The Block Elements, whole. See the note at the top for why the whole block
# rather than the two the first caller wanted.
EXTRA = list(range(0x2580, 0x25A0))

# A hollow box, for anything the font does not have. Deliberately not a
# space: a missing character should look missing.
UNKNOWN = [0x00, 0x00, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x42,
           0x42, 0x42, 0x42, 0x7E, 0x00, 0x00, 0x00, 0x00]


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__.strip().splitlines()[-1])

    source, symbol, out = sys.argv[1], sys.argv[2], sys.argv[3]
    glyphs = parse(source)

    missing = [c for c in list(range(FIRST, LAST + 1)) + EXTRA
               if c not in glyphs]
    if missing:
        raise SystemExit(
            f"{source}: no glyph for " +
            ", ".join(f"U+{c:04X}" for c in missing[:8]) +
            ("..." if len(missing) > 8 else "")
        )

    lines = [
        "/*",
        f" * Generated by tools/bdf2c.py from {source}. Do not edit.",
        " *",
    ]
    lines += [f" * {l}" for l in provenance(source)]
    lines += [
        " *",
        f" * Glyphs U+{FIRST:04X} to U+{LAST:04X}, one for anything else, then"
        f" U+{EXTRA[0]:04X} to U+{EXTRA[-1]:04X}.",
        f" * {HEIGHT} bytes each, one byte per row, MSB is the leftmost pixel.",
        " */",
        "",
        f"const unsigned char {symbol}[] = {{",
    ]

    for c in list(range(FIRST, LAST + 1)) + [None] + EXTRA:
        rows = UNKNOWN if c is None else glyphs[c]

        if len(rows) != HEIGHT:
            raise SystemExit(
                f"{source}: U+{c:04X} has {len(rows)} rows, expected {HEIGHT}."
            )

        name = "the unknown glyph" if c is None else (
            f"U+{c:04X} {chr(c)!r}" if c != 0x27 else "U+0027 apostrophe"
        )

        # Each byte next to the row it draws.
        #
        # It costs nothing in the image - comments do not survive the
        # compiler - and it makes a wrong conversion something a person can
        # see by reading this file, pairwise, instead of only by looking at a
        # screen and wondering. A shifted bit, a reversed row order or an
        # off-by-one in the range are all obvious here and all subtle there.
        lines.append(f"    /* {name} */")
        for r in rows:
            art = "".join("#" if r & (0x80 >> b) else "." for b in range(8))
            lines.append(f"    0x{r:02x},  /* {art} */")

    lines += [
        "};",
        "",
        f"const unsigned long {symbol}_len = sizeof({symbol});",
        "",
        "/*",
        " * What each glyph past the unknown box is, so a lookup can find it.",
        " * Ascending, and the console relies on nothing but that it matches",
        " * the order above.",
        " */",
        f"const unsigned short {symbol}_extra[] = {{",
    ]

    for c in EXTRA:
        lines.append(f"    0x{c:04x},  /* {chr(c)!r} */")

    lines += [
        "};",
        "",
        f"const unsigned long {symbol}_extra_len ="
        f" sizeof({symbol}_extra) / sizeof({symbol}_extra[0]);",
        "",
    ]

    with open(out, "w") as f:
        f.write("\n".join(lines))

    count = LAST - FIRST + 2 + len(EXTRA)
    print(f"{out}: {count} glyphs, {count * HEIGHT} bytes")


if __name__ == "__main__":
    main()
