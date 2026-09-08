#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""A PDF with a font inside it, made from a text file.

`pdfview`, `pdfinfo` and `pdfbench` all default to `/home/odyssey.pdf` and
that file has never been in this repository - it was always somebody's own
document, which meant the PDF path could not be exercised by anybody who
did not happen to have one. This makes one.

**It has to embed a TrueType face, and that is the whole reason this is
three hundred lines instead of thirty.** `pdfpage.rasteriser` returns nil
when `font.file` is nil, so a PDF using one of the base-14 names - plain
`/Helvetica`, which is four lines to write - parses perfectly, reports its
pages, its text and its fonts, and then draws *nothing at all*. A blank
white page with a correct page count is the most confusing possible
failure, and it is what the short version produces.

So: a Type0 font, Identity-H, with a CIDFontType2 descendant whose
FontDescriptor carries the actual `.ttf` as a FontFile2 stream. Which means
reading the font's `cmap` to turn characters into glyph ids, and its
`hmtx` to turn glyph ids into widths, because Identity-H addresses glyphs
and knows nothing about characters.

The stream is stored uncompressed. `program_of` in `pdfpage.lua` takes that
branch directly and inflates only when the filter says FlateDecode, so
uncompressed is both simpler here and one less thing between the file and
the first glyph.

Usage:
    mkpdf.py <text file> <out.pdf> [--font assets/fonts/IBMPlexSans-Regular.ttf]
             [--title "The Odyssey"]
"""

import argparse
import struct
import sys


# --------------------------------------------------------------------------
# The parts of a TrueType file this needs, and nothing else.
#
# Four tables: `head` for the em square, `maxp` for how many glyphs there
# are, `hhea`/`hmtx` for their advances, and `cmap` for which glyph a
# character is. A PDF viewer needs the whole file - it is embedded intact -
# but the *generator* needs only enough to write correct widths and correct
# glyph ids, and getting either wrong produces a page that renders with the
# spacing of a different font.
# --------------------------------------------------------------------------

class TrueType:
    def __init__(self, data):
        self.data = data

        if data[:4] not in (b"\x00\x01\x00\x00", b"true"):
            raise SystemExit("not a TrueType file (bad sfnt version)")

        count = struct.unpack(">H", data[4:6])[0]
        self.tables = {}

        for i in range(count):
            at = 12 + i * 16
            tag, _sum, off, length = struct.unpack(">4sIII", data[at:at + 16])
            self.tables[tag.decode("latin-1")] = (off, length)

        head_at = self.tables["head"][0]
        self.units_per_em = struct.unpack(">H", data[head_at + 18:head_at + 20])[0]

        maxp_at = self.tables["maxp"][0]
        self.num_glyphs = struct.unpack(">H", data[maxp_at + 4:maxp_at + 6])[0]

        self._read_hmtx()
        self._read_cmap()
        self._read_names()

    def _read_hmtx(self):
        hhea_at = self.tables["hhea"][0]
        pairs = struct.unpack(">H", self.data[hhea_at + 34:hhea_at + 36])[0]
        hmtx_at = self.tables["hmtx"][0]

        self.advances = []
        last = 0

        for g in range(self.num_glyphs):
            if g < pairs:
                last = struct.unpack(">H", self.data[hmtx_at + g * 4:
                                                     hmtx_at + g * 4 + 2])[0]
            self.advances.append(last)

    def _read_cmap(self):
        """Unicode -> glyph id, from a format 4 subtable."""
        cmap_at = self.tables["cmap"][0]
        n = struct.unpack(">H", self.data[cmap_at + 2:cmap_at + 4])[0]

        best = None

        for i in range(n):
            at = cmap_at + 4 + i * 8
            plat, enc, off = struct.unpack(">HHI", self.data[at:at + 8])

            # (3,1) is the Windows BMP table every text font has. (0,x) is
            # Unicode. Either will do; prefer 3,1 because it is the one
            # guaranteed to be format 4.
            if (plat, enc) == (3, 1):
                best = cmap_at + off
                break
            if plat == 0 and best is None:
                best = cmap_at + off

        if best is None:
            raise SystemExit("the font has no Unicode cmap")

        fmt = struct.unpack(">H", self.data[best:best + 2])[0]

        if fmt != 4:
            raise SystemExit("cmap subtable is format %d; only 4 is handled" % fmt)

        seg2 = struct.unpack(">H", self.data[best + 6:best + 8])[0]
        segs = seg2 // 2

        def arr(offset):
            return struct.unpack(">%dH" % segs,
                                 self.data[offset:offset + seg2])

        end_at = best + 14
        start_at = end_at + seg2 + 2
        delta_at = start_at + seg2
        range_at = delta_at + seg2

        ends = arr(end_at)
        starts = arr(start_at)
        deltas = arr(delta_at)
        ranges = arr(range_at)

        self.cmap = {}

        for s in range(segs):
            if starts[s] > ends[s]:
                continue

            for code in range(starts[s], min(ends[s], 0xFFFF) + 1):
                if ranges[s] == 0:
                    gid = (code + deltas[s]) & 0xFFFF
                else:
                    #
                    # The idRangeOffset indirection, which is the one part of
                    # format 4 that cannot be guessed: the offset is counted
                    # in bytes from the *position of the entry itself*.
                    #
                    at = range_at + s * 2 + ranges[s] + (code - starts[s]) * 2

                    if at + 2 > len(self.data):
                        continue

                    gid = struct.unpack(">H", self.data[at:at + 2])[0]

                    if gid != 0:
                        gid = (gid + deltas[s]) & 0xFFFF

                if gid != 0:
                    self.cmap[code] = gid

    def _read_names(self):
        """The PostScript name, for /BaseFont."""
        self.ps_name = "EmbeddedFont"

        if "name" not in self.tables:
            return

        at, _ = self.tables["name"]
        count, string_at = struct.unpack(">HH", self.data[at + 2:at + 6])

        for i in range(count):
            rec = at + 6 + i * 12
            plat, enc, lang, nid, length, off = struct.unpack(
                ">HHHHHH", self.data[rec:rec + 12])

            if nid != 6:
                continue

            raw = self.data[at + string_at + off:at + string_at + off + length]

            try:
                name = raw.decode("utf-16-be" if plat == 3 else "latin-1")
            except UnicodeDecodeError:
                continue

            name = "".join(c for c in name if 33 <= ord(c) <= 126)

            if name:
                self.ps_name = name
                return

    def width(self, gid):
        """The advance, in PDF's 1000-per-em space."""
        if gid >= len(self.advances):
            return 500
        return round(self.advances[gid] * 1000 / self.units_per_em)


# --------------------------------------------------------------------------

def layout(text, columns=88):
    """Wrap the source text into pages of lines.

    Deliberately dumb: this is a document to look at and to scan, not a
    typesetter. Blank lines are kept, because they are what separates one
    paragraph from the next.
    """
    lines = []

    for para in text.split("\n"):
        para = para.rstrip()

        if not para:
            lines.append("")
            continue

        words = para.split()
        row = ""

        for w in words:
            if row and len(row) + 1 + len(w) > columns:
                lines.append(row)
                row = w
            else:
                row = (row + " " + w) if row else w

        if row:
            lines.append(row)

    per_page = 46
    return [lines[i:i + per_page] for i in range(0, len(lines), per_page)] or [[""]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("out")
    ap.add_argument("--font", default="assets/fonts/IBMPlexSans-Regular.ttf")
    ap.add_argument("--title", default=None)
    ap.add_argument("--size", type=float, default=11.0)
    args = ap.parse_args()

    with open(args.font, "rb") as f:
        ttf_bytes = f.read()

    ttf = TrueType(ttf_bytes)

    with open(args.source, encoding="utf-8") as f:
        text = f.read()

    pages = layout(text)

    #
    # Every glyph the document uses, so the /W array describes exactly the
    # glyphs that appear and nothing else. A full-coverage W array for a
    # 3000-glyph face would be most of the file.
    #
    used = set()

    def gids(s):
        out = []

        for ch in s:
            gid = ttf.cmap.get(ord(ch))

            if gid is None:
                gid = ttf.cmap.get(ord("?"), 0)

            used.add(gid)
            out.append(gid)

        return out

    def hexstr(s):
        return "<" + "".join("%04X" % g for g in gids(s)) + ">"

    objs = {}
    FONT, CIDFONT, DESCR, FILE = 3, 4, 5, 6
    first_page = 7

    page_ids = [first_page + i * 2 for i in range(len(pages))]

    objs[1] = b"<< /Type /Catalog /Pages 2 0 R >>"
    objs[2] = ("<< /Type /Pages /Kids [%s] /Count %d >>"
               % (" ".join("%d 0 R" % p for p in page_ids), len(page_ids))).encode()

    objs[FONT] = ("<< /Type /Font /Subtype /Type0 /BaseFont /%s "
                  "/Encoding /Identity-H /DescendantFonts [%d 0 R] >>"
                  % (ttf.ps_name, CIDFONT)).encode()

    widths = " ".join("%d [%d]" % (g, ttf.width(g)) for g in sorted(used))

    objs[CIDFONT] = ("<< /Type /Font /Subtype /CIDFontType2 /BaseFont /%s "
                     "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) "
                     "/Supplement 0 >> /FontDescriptor %d 0 R "
                     "/CIDToGIDMap /Identity /DW 500 /W [%s] >>"
                     % (ttf.ps_name, DESCR, widths)).encode()

    objs[DESCR] = ("<< /Type /FontDescriptor /FontName /%s /Flags 32 "
                   "/FontBBox [-1000 -400 2000 1100] /ItalicAngle 0 "
                   "/Ascent 900 /Descent -200 /CapHeight 700 /StemV 80 "
                   "/FontFile2 %d 0 R >>" % (ttf.ps_name, FILE)).encode()

    objs[FILE] = (b"<< /Length %d /Length1 %d >>\nstream\n" %
                  (len(ttf_bytes), len(ttf_bytes)) + ttf_bytes + b"\nendstream")

    title = args.title
    leading = args.size * 1.55

    for i, lines in enumerate(pages):
        pid = page_ids[i]
        cid = pid + 1

        s = []
        y = 742.0

        if title and i == 0:
            s += ["BT", "/F1 22 Tf", "72 %.1f Td" % y, "%s Tj" % hexstr(title), "ET"]
            y -= 40

        s += ["BT", "/F1 %.1f Tf" % args.size, "72 %.1f Td" % y,
              "%.2f TL" % leading]

        for ln in lines:
            s.append("%s Tj T*" % hexstr(ln) if ln else "T*")

        s.append("ET")

        # A page number, so it is obvious which page is on screen.
        s += ["BT", "/F1 9 Tf", "300 40 Td",
              "%s Tj" % hexstr("%d" % (i + 1)), "ET"]

        body = "\n".join(s).encode("latin-1")

        objs[pid] = ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                     "/Resources << /Font << /F1 %d 0 R >> >> "
                     "/Contents %d 0 R >>" % (FONT, cid)).encode()

        objs[cid] = (b"<< /Length %d >>\nstream\n" % len(body)
                     + body + b"\nendstream")

    out = bytearray(b"%PDF-1.4\n")
    offsets = {}

    for n in sorted(objs):
        offsets[n] = len(out)
        out += b"%d 0 obj\n" % n + objs[n] + b"\nendobj\n"

    xref_at = len(out)
    top = max(objs) + 1

    out += b"xref\n0 %d\n" % top + b"0000000000 65535 f \n"

    for n in range(1, top):
        out += b"%010d 00000 n \n" % offsets.get(n, 0)

    out += (b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
            % (top, xref_at))

    with open(args.out, "wb") as f:
        f.write(bytes(out))

    print("%s: %d pages, %d glyphs, %s embedded, %d bytes"
          % (args.out, len(pages), len(used), ttf.ps_name, len(out)))


if __name__ == "__main__":
    sys.exit(main())
